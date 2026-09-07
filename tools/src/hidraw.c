/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hidraw.c - hidraw access to the LG Magic Remote (scripts/lg_magic.py).
 *
 * The output of hidraw_run() replicates lg_magic.py line by line,
 * including its quirks (the "Button:" line printed twice - once with the
 * code in decimal, once in hex; button code parsed big-endian; the six
 * points of interest labelled POI1..3/ACCEL_X..Z). Auto-detection by
 * VID/PID replaces the hardcoded /dev/hidraw7.
 */
#include "hidraw.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/hidraw.h>

/* Expected payload sizes (excluding the report ID) - lg_magic.py. */
static int expected_size(unsigned char report_id)
{
	switch (report_id) {
	case 0xFD: return 20;
	case 0xF9: return 29;
	case 0x01: return 29;
	default:   return 15;	/* fallback guess */
	}
}

/* BUTTON_CODES from lg_magic.py, verbatim. */
static const struct {
	unsigned code;
	const char *name;
} button_codes[] = {
	{ 0x0000, "None" },
	{ 0x8000, "POWER" },
	{ 0x8099, "POWER2" },
	{ 0x8010, "0" },
	{ 0x8011, "1" },
	{ 0x8012, "2" },
	{ 0x8013, "3" },
	{ 0x8014, "4" },
	{ 0x8015, "5" },
	{ 0x8016, "6" },
	{ 0x8017, "7" },
	{ 0x8018, "8" },
	{ 0x8019, "9" },
	{ 0x8044, "WHEEL_PRESS" },
	{ 0x8053, "LIST" },
	{ 0x8045, "..." },
	{ 0x8002, "VOL+" },
	{ 0x8003, "VOL-" },
	{ 0x8009, "MUTE" },
	{ 0x808B, "MIC" },
	{ 0x807C, "HOME" },
	{ 0x8043, "SETTINGS" },
	{ 0x8028, "BACK" },
	{ 0x80AB, "GUIDE" },
	{ 0x805D, "STREAMING" },
	{ 0x800B, "INPUT" },
	{ 0x8098, "STB MENU" },
	{ 0x8001, "CH-" },
	{ 0x8072, "RED" },
	{ 0x8071, "GREEN" },
	{ 0x8063, "YELLOW" },
	{ 0x8061, "BLUE" },
	{ 0x8081, "MOVIES" },
	{ 0x80B0, "PLAY" },
	{ 0x80BA, "PAUSE" },
	{ 0x8040, "UP" },
	{ 0x8041, "DOWN" },
	{ 0x8006, "RIGHT" },
	{ 0x8007, "LEFT" },
};

static const char *button_name(unsigned code)
{
	static char buf[32];
	size_t i;

	for (i = 0; i < sizeof(button_codes) / sizeof(button_codes[0]); i++)
		if (button_codes[i].code == code)
			return button_codes[i].name;
	snprintf(buf, sizeof(buf), "UNKNOWN_%04X", code);
	return buf;
}

static void hexstr(const unsigned char *data, size_t n, char *out)
{
	size_t i;

	for (i = 0; i < n; i++)
		sprintf(out + 2 * i, "%02x", data[i]);
	out[2 * n] = '\0';
}

static int le16(const unsigned char *p)
{
	return (int)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

static int be16(const unsigned char *p)
{
	/* int.from_bytes(..., 'big') - unsigned (button codes). */
	return (int)(((unsigned)p[0] << 8) | (unsigned)p[1]);
}

static int be16s(const unsigned char *p)
{
	/* int.from_bytes(..., 'big', signed=True) - sign-extended (points). */
	return (int)(signed short)(((unsigned)p[0] << 8) | (unsigned)p[1]);
}

static void parse_fd(const unsigned char *d, size_t n, int *wheel_pos)
{
	char hex[128];
	int btn_code, wheel, counter, const_fd00, points[6];
	const char *name;
	int i;

	if (n < 20) {
		hexstr(d, n, hex);
		printf("[0xFD] short payload: %s\n", hex);
		return;
	}
	/* Button + wheel parsing: last two bytes before the wheel byte. */
	btn_code = be16(d + 17);	/* int.from_bytes(btn_bytes) = big endian */
	wheel = (signed char)d[19];
	*wheel_pos += wheel;
	name = button_name(btn_code);
	printf("Button: %s (%u)  Wheel=%d\n", name, btn_code, wheel);
	hexstr(d, n, hex);
	printf("[0xFD] Buttons raw: %s\n", hex);

	counter = le16(d + 1);
	const_fd00 = le16(d + 3);
	for (i = 0; i < 6; i++)
		points[i] = be16s(d + 5 + 2 * i);
	/* Python: hex(const_fd00) - lowercase, right-justified in 6. */
	snprintf(hex, sizeof(hex), "0x%x", const_fd00);

	printf("%8s  %6s  %6s  %6s  %6s  %6s  %6s  %6s\n",
	       "Counter", "Const", "POI1", "POI2", "POI3",
	       "ACCEL_X", "ACCEL_Y", "ACCEL_Z");
	printf("%8d  %6s  %6d  %6d  %6d  %6d  %6d  %6d\n",
	       counter, hex, points[0], points[1], points[2],
	       points[3], points[4], points[5]);
	printf("Button: %s (%04X)  Wheel=%d Wheel Pos = %d\n",
	       name, btn_code, wheel, *wheel_pos);
}

static void parse_f9(const unsigned char *d, size_t n)
{
	char hex[128];

	hexstr(d, n, hex);
	printf("[0xF9] Extra control raw: %s\n", hex);
}

static void parse_01(const unsigned char *d, size_t n)
{
	char hex[128];

	if (n % 2 == 0) {
		size_t i;

		printf("[0x01] Gyro values: (");
		for (i = 0; i < n; i += 2)
			printf("%s%d", i ? ", " : "", (int)(signed short)le16(d + i));
		printf(")\n");
	} else {
		hexstr(d, n, hex);
		printf("[0x01] Odd-size gyro packet: %s\n", hex);
	}
}

int hidraw_run(const char *path)
{
	unsigned char buf[64];
	int fd, wheel_pos = 0;
	ssize_t rv;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "lg-magic: cannot open %s: %s\n", path,
			strerror(errno));
		return -1;
	}
	for (;;) {
		unsigned char report_id;
		int size, got;

		rv = read(fd, &report_id, 1);
		if (rv < 0) {
			if (errno == EINTR)
				break;	/* Ctrl+C: clean end */
			fprintf(stderr, "lg-magic: read error on %s: %s\n",
				path, strerror(errno));
			close(fd);
			return -1;
		}
		if (rv == 0)
			break;	/* EOF */
		size = expected_size(report_id);
		/* One blocking read, like Python's f.read(size). */
		got = 0;
		do {
			rv = read(fd, buf + got, (size_t)(size - got));
			if (rv < 0) {
				if (errno == EINTR)
					break;
				fprintf(stderr, "lg-magic: read error on "
					"%s: %s\n", path, strerror(errno));
				close(fd);
				return -1;
			}
			if (rv == 0)
				break;
			got += (int)rv;
		} while (got < size);

		switch (report_id) {
		case 0xFD: parse_fd(buf, (size_t)got, &wheel_pos); break;
		case 0xF9: parse_f9(buf, (size_t)got); break;
		case 0x01: parse_01(buf, (size_t)got); break;
		default:
			{
				char hex[128];

				hexstr(buf, (size_t)got, hex);
				printf("[0x%02X] Unknown len=%d: %s\n",
				       report_id, got, hex);
			}
			break;
		}
		if (got < size)
			break;	/* short final read */
	}
	close(fd);
	return 0;
}

static int scan_remotes(int (*cb)(const char *path, void *ctx), void *ctx)
{
	struct dirent *de;
	DIR *dir;
	int found = 0;

	dir = opendir("/dev");
	if (!dir)
		return 0;
	while ((de = readdir(dir)) != NULL) {
		char path[sizeof(de->d_name) + 16];
		struct hidraw_devinfo info;
		int fd;

		if (strncmp(de->d_name, "hidraw", 6) != 0)
			continue;
		snprintf(path, sizeof(path), "/dev/%s", de->d_name);
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		if (ioctl(fd, HIDIOCGRAWINFO, &info) == 0 &&
		    (unsigned)info.vendor == LG_MAGIC_VID &&
		    (unsigned)info.product == LG_MAGIC_PID) {
			found++;
			if (cb && cb(path, ctx) != 0) {
				close(fd);
				closedir(dir);
				return found;
			}
		}
		close(fd);
	}
	closedir(dir);
	return found;
}

static int pick_first(const char *path, void *ctx)
{
	snprintf((char *)ctx, 256, "%s", path);
	return 1;
}

int hidraw_find_remote(char *path, size_t pathsz, char *err, size_t errsz)
{
	char buf[256];

	buf[0] = '\0';
	if (scan_remotes(pick_first, buf) == 0) {
		snprintf(err, errsz, "no LG Magic Remote hidraw device found "
			 "(VID %04x PID %04x)", LG_MAGIC_VID, LG_MAGIC_PID);
		return -1;
	}
	snprintf(path, pathsz, "%s", buf);
	return 0;
}

int hidraw_get_uniq(const char *path, char *buf, size_t bufsz)
{
	/* Mainline Linux has no uniq ioctl on hidraw. The HID device's
	 * unique ID (the Bluetooth MAC for this remote) is exposed through
	 * sysfs instead. */
	const char *dev;
	char syspath[256];
	FILE *f;
	size_t n;

	if (bufsz == 0)
		return -1;
	buf[0] = '\0';
	dev = strrchr(path, '/');
	dev = dev ? dev + 1 : path;
	if (snprintf(syspath, sizeof(syspath),
		     "/sys/class/hidraw/%s/device/uniq", dev) >=
	    (int)sizeof(syspath))
		return -1;
	f = fopen(syspath, "r");
	if (!f)
		return -1;
	n = fread(buf, 1, bufsz - 1, f);
	fclose(f);
	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = '\0';
	return 0;
}

static int print_one(const char *path, void *ctx)
{
	(void)ctx;
	printf("%s\n", path);
	return 0;
}

void hidraw_list_remotes(void)
{
	if (scan_remotes(print_one, NULL) == 0)
		printf("no LG Magic Remote hidraw devices found\n");
}
