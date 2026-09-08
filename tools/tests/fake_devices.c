/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * fake_devices.c - e2e test harness: a fake LG Magic Remote (Linux only).
 *
 * Modes:
 *   create  create a "LG Magic Remote" keyboard (EV_KEY + REL_WHEEL) and
 *           a "LG Magic Remote IMU" (EV_ABS + EV_MSC) via uinput, print
 *           their /dev/input/event* paths (mknod'ing them when needed),
 *           then block until killed (the fds keep the devices alive).
 *   emit    write input_event structs straight into an evdev node
 *           (this is how the uinput driver itself delivers events):
 *             --kbd PATH --key KEY_UP     press + release
 *             --kbd PATH --press KEY_UP   press only (held-key e2e)
 *             --kbd PATH --release KEY_UP release only
 *             --kbd PATH --wheel N        REL_WHEEL N
 *             --imu PATH --gyro X,Y,Z     ABS_RX/RY/RZ + MSC counter
 *                                         (repeatable: one frame per --gyro,
 *                                          emitted in order - the airmouse
 *                                          engine works on frame-to-frame
 *                                          deltas, so tests drive it with a
 *                                          baseline frame followed by motion)
 *             --imu PATH --accel X,Y,Z [--gyro X,Y,Z ...]
 *                                         ABS_X/Y/Z (accel) + ABS_RX/RY/RZ
 *                                         per frame; --accel sets the accel
 *                                         for all following --gyro frames
 *                                         (default 0,0,0 = no accelerometer,
 *                                         which disables the spring-back
 *                                         gate in the engine)
 *   watch   read and print frames from an evdev node (e.g. the daemon's
 *           per-remote "lgmagicd keyboard <identity>" outputs).
 *
 * The devices carry vendor 0x000f/product 0x3412 and the exact kernel
 * names, so the daemon's pairing (pairing_is_keyboard/_imu) sees them
 * as a real remote.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>
#include <linux/uinput.h>

#include "keymap.h"

#define KBD_NAME "LG Magic Remote"
#define IMU_NAME "LG Magic Remote IMU"

static volatile sig_atomic_t g_stop_local;

static void on_signal(int sig)
{
	(void)sig;
	g_stop_local = 1;
}

static void usage(FILE *out)
{
	fputs("Usage: fake_devices create\n"
	      "       fake_devices emit --kbd PATH --key NAME\n"
	      "       fake_devices emit --kbd PATH --press NAME\n"
	      "       fake_devices emit --kbd PATH --release NAME\n"
	      "       fake_devices emit --kbd PATH --wheel N\n"
	      "       fake_devices emit --imu PATH --gyro X,Y,Z [--gyro ...]\n"
	      "       fake_devices emit --imu PATH --accel X,Y,Z [--gyro ...]\n"
	      "       fake_devices watch PATH [--ms MS]\n", out);
}

/* ------------------------------------------------------------------ */
/* uinput creation (raw ioctls: the IMU needs UI_ABS_SETUP)            */
/* ------------------------------------------------------------------ */

static int open_uinput(void)
{
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);

	if (fd < 0)
		fd = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
	return fd;
}

static int setup_fake(const char *name, unsigned vendor, unsigned product,
		      int with_abs)
{
	struct uinput_setup usetup;
	static const int keys[] = {
		KEY_ENTER, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
		KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_HOME, KEY_BACK,
	};
	static const int abs_axes[] = { ABS_X, ABS_Y, ABS_Z,
					ABS_RX, ABS_RY, ABS_RZ };
	size_t i;
	int fd = open_uinput();

	if (fd < 0) {
		fprintf(stderr, "fake_devices: cannot open /dev/uinput: %s\n",
			strerror(errno));
		return -1;
	}
	if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0)
		goto fail;
	for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		if (ioctl(fd, UI_SET_KEYBIT, keys[i]) < 0)
			goto fail;
	if (with_abs) {
		if (ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0 ||
		    ioctl(fd, UI_SET_EVBIT, EV_MSC) < 0)
			goto fail;
		for (i = 0; i < sizeof(abs_axes) / sizeof(abs_axes[0]); i++) {
			struct uinput_abs_setup ab;

			memset(&ab, 0, sizeof(ab));
			ab.code = (__u16)abs_axes[i];
			ab.absinfo.minimum = -32768;
			ab.absinfo.maximum = 32767;
			if (ioctl(fd, UI_ABS_SETUP, &ab) < 0 ||
			    ioctl(fd, UI_SET_ABSBIT, abs_axes[i]) < 0)
				goto fail;
		}
		if (ioctl(fd, UI_SET_MSCBIT, MSC_SERIAL) < 0)
			goto fail;
	} else {
		if (ioctl(fd, UI_SET_EVBIT, EV_REL) < 0 ||
		    ioctl(fd, UI_SET_RELBIT, REL_WHEEL) < 0)
			goto fail;
	}

	memset(&usetup, 0, sizeof(usetup));
	snprintf(usetup.name, UINPUT_MAX_NAME_SIZE, "%s", name);
	usetup.id.bustype = BUS_BLUETOOTH;
	usetup.id.vendor = (__u16)vendor;
	usetup.id.product = (__u16)product;
	if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0 ||
	    ioctl(fd, UI_DEV_CREATE) < 0)
		goto fail;
	return fd;

fail:
	fprintf(stderr, "fake_devices: uinput setup for '%s' failed: %s\n",
		name, strerror(errno));
	close(fd);
	return -1;
}

/* Find the event node of the input device with the given name, create
 * /dev/input/eventN when missing (containers without udev).  Fills
 * path or "" when not found yet. */
static void find_event_node(const char *name, char *path, size_t pathsz)
{
	DIR *dir;
	struct dirent *de;

	path[0] = '\0';
	dir = opendir("/sys/class/input");
	if (!dir)
		return;
	while ((de = readdir(dir)) != NULL) {
		/* NAME_MAX (255) plus the sysfs prefixes */
		char npath[1024], buf[256];
		FILE *f;

		if (strncmp(de->d_name, "input", 5) != 0)
			continue;
		snprintf(npath, sizeof(npath), "/sys/class/input/%s/name",
			 de->d_name);
		f = fopen(npath, "r");
		if (!f)
			continue;
		if (!fgets(buf, sizeof(buf), f)) {
			fclose(f);
			continue;
		}
		fclose(f);
		buf[strcspn(buf, "\n")] = '\0';
		if (strcmp(buf, name) != 0)
			continue;
		{
			/* the eventN child holds the dev number */
			DIR *d2;
			struct dirent *de2;

			snprintf(npath, sizeof(npath), "/sys/class/input/%s",
				 de->d_name);
			d2 = opendir(npath);
			if (!d2)
				break;
			while ((de2 = readdir(d2)) != NULL) {
				char devpath[1024], devbuf[64];
				unsigned maj, min;

				if (strncmp(de2->d_name, "event", 5) != 0)
					continue;
				snprintf(devpath, sizeof(devpath),
					 "/sys/class/input/%s/%s/dev",
					 de->d_name, de2->d_name);
				f = fopen(devpath, "r");
				if (!f || !fgets(devbuf, sizeof(devbuf), f)) {
					if (f)
						fclose(f);
					continue;
				}
				fclose(f);
				if (sscanf(devbuf, "%u:%u", &maj, &min) != 2)
					continue;
				snprintf(path, pathsz, "/dev/input/%s",
					 de2->d_name);
				if (access(path, F_OK) != 0) {
					mkdir("/dev/input", 0755);
					/* EEXIST: created by a race - fine */
					if (mknod(path, S_IFCHR | 0600,
						  makedev(maj, min)) < 0 &&
					    errno != EEXIST)
						path[0] = '\0';
				}
				closedir(d2);
				closedir(dir);
				return;
			}
			closedir(d2);
		}
	}
	closedir(dir);
}

/* ------------------------------------------------------------------ */
/* create                                                              */
/* ------------------------------------------------------------------ */

static int cmd_create(void)
{
	struct sigaction sa;
	char kbd_node[1024], imu_node[1024];
	int kfd, ifd, tries;

	kfd = setup_fake(KBD_NAME, 0x000f, 0x3412, 0);
	if (kfd < 0)
		return 1;
	ifd = setup_fake(IMU_NAME, 0x000f, 0x3412, 1);
	if (ifd < 0) {
		ioctl(kfd, UI_DEV_DESTROY);
		close(kfd);
		return 1;
	}

	/* The sysfs nodes appear right after UI_DEV_CREATE. */
	for (tries = 0; tries < 50; tries++) {
		find_event_node(KBD_NAME, kbd_node, sizeof(kbd_node));
		find_event_node(IMU_NAME, imu_node, sizeof(imu_node));
		if (kbd_node[0] && imu_node[0])
			break;
		usleep(20000);
	}
	if (!kbd_node[0] || !imu_node[0]) {
		fprintf(stderr, "fake_devices: event nodes did not appear\n");
		return 1;
	}
	printf("keyboard=%s\n", kbd_node);
	printf("imu=%s\n", imu_node);
	fflush(stdout);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	while (!g_stop_local)
		pause();
	/* Closing the fds destroys the devices. */
	ioctl(kfd, UI_DEV_DESTROY);
	ioctl(ifd, UI_DEV_DESTROY);
	close(kfd);
	close(ifd);
	return 0;
}

/* ------------------------------------------------------------------ */
/* emit                                                               */
/* ------------------------------------------------------------------ */

static int emit_ev(int fd, __u16 type, __u16 code, int value)
{
	struct input_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.code = code;
	ev.value = value;
	return write(fd, &ev, sizeof(ev)) == sizeof(ev) ? 0 : -1;
}

/* Press or release only - the held-key e2e needs a key that stays down
 * across a remap (--key emits press + release in one go). */
static int emit_key_edge(const char *path, const char *name, int value)
{
	int code = keymap_name_to_code(name);
	int fd;

	if (code < 0) {
		fprintf(stderr, "fake_devices: unknown key name '%s'\n", name);
		return 1;
	}
	fd = open(path, O_WRONLY);
	if (fd < 0) {
		fprintf(stderr, "fake_devices: cannot open %s: %s\n", path,
			strerror(errno));
		return 1;
	}
	fprintf(stderr, "fake_devices: emit %s %s (code %d) on %s\n", name,
		value ? "press" : "release", code, path);
	if (emit_ev(fd, EV_KEY, (__u16)code, value) < 0 ||
	    emit_ev(fd, EV_SYN, SYN_REPORT, 0) < 0) {
		fprintf(stderr, "fake_devices: write to %s failed: %s\n", path,
			strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);
	return 0;
}

static int emit_key(const char *path, const char *name)
{
	int code = keymap_name_to_code(name);
	int fd;

	if (code < 0) {
		fprintf(stderr, "fake_devices: unknown key name '%s'\n", name);
		return 1;
	}
	fd = open(path, O_WRONLY);
	if (fd < 0) {
		fprintf(stderr, "fake_devices: cannot open %s: %s\n", path,
			strerror(errno));
		return 1;
	}
	fprintf(stderr, "fake_devices: emit %s (code %d) on %s\n", name, code,
		path);
	if (emit_ev(fd, EV_KEY, (__u16)code, 1) < 0 ||
	    emit_ev(fd, EV_SYN, SYN_REPORT, 0) < 0 ||
	    emit_ev(fd, EV_KEY, (__u16)code, 0) < 0 ||
	    emit_ev(fd, EV_SYN, SYN_REPORT, 0) < 0) {
		fprintf(stderr, "fake_devices: write to %s failed: %s\n", path,
			strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);
	return 0;
}

static int emit_wheel(const char *path, int clicks)
{
	int fd = open(path, O_WRONLY);

	if (fd < 0) {
		fprintf(stderr, "fake_devices: cannot open %s: %s\n", path,
			strerror(errno));
		return 1;
	}
	if (emit_ev(fd, EV_REL, REL_WHEEL, clicks) < 0 ||
	    emit_ev(fd, EV_SYN, SYN_REPORT, 0) < 0) {
		fprintf(stderr, "fake_devices: write to %s failed: %s\n", path,
			strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);
	return 0;
}

static int emit_gyro(const char *path, int ax, int ay, int az,
		     int gx, int gy, int gz)
{
	int fd = open(path, O_WRONLY);

	if (fd < 0) {
		fprintf(stderr, "fake_devices: cannot open %s: %s\n", path,
			strerror(errno));
		return 1;
	}
	/* Accel first, then the gyro/POI values - one SYN_REPORT closes the
	 * frame, so the reader sees both triplets together. */
	if (emit_ev(fd, EV_ABS, ABS_X, ax) < 0 ||
	    emit_ev(fd, EV_ABS, ABS_Y, ay) < 0 ||
	    emit_ev(fd, EV_ABS, ABS_Z, az) < 0 ||
	    emit_ev(fd, EV_ABS, ABS_RX, gx) < 0 ||
	    emit_ev(fd, EV_ABS, ABS_RY, gy) < 0 ||
	    emit_ev(fd, EV_ABS, ABS_RZ, gz) < 0 ||
	    emit_ev(fd, EV_MSC, MSC_SERIAL, 1) < 0 ||
	    emit_ev(fd, EV_SYN, SYN_REPORT, 0) < 0) {
		fprintf(stderr, "fake_devices: write to %s failed: %s\n", path,
			strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);
	return 0;
}

/* Emit each gyro frame in order - one input frame per --gyro flag. */
static int emit_gyro_frames(const char *path, int frames[][6], int n)
{
	int i;

	for (i = 0; i < n; i++)
		if (emit_gyro(path, frames[i][0], frames[i][1], frames[i][2],
			      frames[i][3], frames[i][4], frames[i][5]) < 0)
			return 1;
	return 0;
}

#define MAX_GYRO_FRAMES 8

static int cmd_emit(int argc, char **argv)
{
	const char *kbd = NULL, *imu = NULL;
	const char *key = NULL, *press = NULL, *release = NULL;
	int wheel = 0, have_wheel = 0;
	int accel[3] = { 0, 0, 0 };	/* --accel, applies to following --gyro */
	int frames[MAX_GYRO_FRAMES][6];	/* accel[3] + gyro[3] per frame */
	int n_frames = 0;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--kbd") == 0 && i + 1 < argc)
			kbd = argv[++i];
		else if (strcmp(argv[i], "--imu") == 0 && i + 1 < argc)
			imu = argv[++i];
		else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc)
			key = argv[++i];
		else if (strcmp(argv[i], "--press") == 0 && i + 1 < argc)
			press = argv[++i];
		else if (strcmp(argv[i], "--release") == 0 && i + 1 < argc)
			release = argv[++i];
		else if (strcmp(argv[i], "--wheel") == 0 && i + 1 < argc) {
			wheel = atoi(argv[++i]);
			have_wheel = 1;
		} else if (strcmp(argv[i], "--accel") == 0 && i + 1 < argc) {
			if (sscanf(argv[++i], "%d,%d,%d", &accel[0],
				   &accel[1], &accel[2]) != 3) {
				fprintf(stderr, "fake_devices: --accel needs "
					"X,Y,Z\n");
				return 1;
			}
		} else if (strcmp(argv[i], "--gyro") == 0 && i + 1 < argc) {
			if (n_frames >= MAX_GYRO_FRAMES) {
				fprintf(stderr, "fake_devices: too many --gyro "
					"frames\n");
				return 1;
			}
			if (sscanf(argv[++i], "%d,%d,%d",
				   &frames[n_frames][3],
				   &frames[n_frames][4],
				   &frames[n_frames][5]) != 3) {
				fprintf(stderr, "fake_devices: --gyro needs "
					"X,Y,Z\n");
				return 1;
			}
			frames[n_frames][0] = accel[0];
			frames[n_frames][1] = accel[1];
			frames[n_frames][2] = accel[2];
			n_frames++;
		} else {
			usage(stderr);
			return 1;
		}
	}
	if (key && kbd)
		return emit_key(kbd, key);
	if (press && kbd)
		return emit_key_edge(kbd, press, 1);
	if (release && kbd)
		return emit_key_edge(kbd, release, 0);
	if (have_wheel && kbd)
		return emit_wheel(kbd, wheel);
	if (n_frames && imu)
		return emit_gyro_frames(imu, frames, n_frames);
	usage(stderr);
	return 1;
}

/* ------------------------------------------------------------------ */
/* watch                                                               */
/* ------------------------------------------------------------------ */

static const char *code_name(__u16 type, __u16 code)
{
	/* REL codes numerically collide with KEY_* codes (REL_X = 0,
	 * REL_WHEEL = 8 = KEY_7, REL_WHEEL_HI_RES = 11 = KEY_0 ...) -
	 * name them by type, never through the keymap. */
	if (type == EV_REL) {
		static const char *rel[] = {
			"REL_X", "REL_Y", "REL_Z", "REL_RX", "REL_RY", "REL_RZ",
			"REL_HWHEEL", "REL_DIAL", "REL_WHEEL", "REL_MISC",
			"REL_RESERVED", "REL_WHEEL_HI_RES", "REL_HWHEEL_HI_RES",
		};

		return code < sizeof(rel) / sizeof(rel[0]) ? rel[code] : "?";
	}
	if (type == EV_KEY) {
		const char *n = keymap_code_to_name(code);

		return n ? n : "?";
	}
	return "?";
}

static void print_event(const struct input_event *ev)
{
	if (ev->type == EV_SYN)
		printf("---\n");
	else
		printf("%s %s %d\n",
		       ev->type == EV_KEY ? "KEY" :
		       ev->type == EV_REL ? "REL" :
		       ev->type == EV_ABS ? "ABS" : "EV",
		       code_name(ev->type, ev->code), ev->value);
}

static int cmd_watch(int argc, char **argv)
{
	const char *path = NULL;
	long long limit_ms = 0, t0 = 0;
	struct input_event evs[16];
	ssize_t rv;
	int fd, i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--ms") == 0 && i + 1 < argc)
			limit_ms = atoll(argv[++i]);
		else if (!path)
			path = argv[i];
		else {
			usage(stderr);
			return 1;
		}
	}
	if (!path) {
		usage(stderr);
		return 1;
	}
	/* O_NONBLOCK: the --ms deadline must end the loop even when the
	 * device goes quiet.  A blocking read would sit forever past the
	 * limit, and the test's kill would throw the stdio buffer (and
	 * the lines with it) away - the reason the very first e2e checks
	 * read back empty. */
	fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "fake_devices: cannot open %s: %s\n", path,
			strerror(errno));
		return 1;
	}
	for (;;) {
		struct timespec ts;
		long long now;

		rv = read(fd, evs, sizeof(evs));
		if (rv > 0) {
			/* A read can carry several whole events - print them
			 * all (rv is always a multiple of sizeof(evs[0])). */
			for (i = 0; i < rv / (ssize_t)sizeof(evs[0]); i++)
				print_event(&evs[i]);
			fflush(stdout);	/* survives a kill right after this */
			continue;
		}
		if (rv == 0)
			break;	/* EOF - the device was destroyed */
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			break;
		if (limit_ms > 0) {
			clock_gettime(CLOCK_MONOTONIC, &ts);
			now = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
			if (t0 == 0)
				t0 = now;
			else if (now - t0 >= limit_ms)
				break;
		}
		usleep(5000);
	}
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage(stderr);
		return 1;
	}
	if (strcmp(argv[1], "create") == 0)
		return cmd_create();
	if (strcmp(argv[1], "emit") == 0)
		return cmd_emit(argc - 1, argv + 1);
	if (strcmp(argv[1], "watch") == 0)
		return cmd_watch(argc - 1, argv + 1);
	usage(stderr);
	return 1;
}
