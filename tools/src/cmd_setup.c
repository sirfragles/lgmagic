/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cmd_setup.c - `lgmagic setup`, the interactive configuration and
 * calibration wizard. After it finishes, the remote works end to end.
 *
 * Steps:  1. environment (root, module, devices, uinput)
 *         2. input mode (daemon - raw_only=1 + lgmagicd - vs the
 *            kernel airmouse - raw_only=0 airmouse=1, the v1 behaviour)
 *         3. module parameters (/etc/modprobe.d/lgmagic.conf + reload;
 *            daemon mode enables lgmagicd best-effort)
 *         4. accelerometer calibration (recording + Levenberg-Marquardt)
 *         5. gyroscope calibration (recording + mean bias)
 *         6. calibration JSON (gyro scale / alpha / mouse_k questions)
 *         7. firmware blob to /lib/firmware - LEGACY ONLY (raw_only=0;
 *            skipped in daemon mode: one calibration source per mode)
 *         8. per-device state (/var/lib/lgmagic/<MAC>/calibration.json,
 *            /etc/lgmagic/devices.d/<MAC>.toml) + daemon Reload()
 *         9. module reload + dmesg verification
 *        10. airmouse test (daemon status with a standalone fallback)
 *        11. user config (SUDO_USER/HOME aware)
 *        12. summary
 *
 * `--non-interactive` accepts every default (daemon mode). Run as root.
 * The wizard runs as root and writes the state files directly (polkit
 * gates unprivileged writes only; sudo already authenticated), then
 * asks the daemon to reload via the D-Bus client.
 *
 * Note on ordering: the IMU evdev node only exists while lgmagic runs
 * with imu_evdev=1 (the module default is 0), so the parameters are
 * written and the module is (re)loaded before the device is reopened
 * for the calibration recordings.
 */
#include "airmouse.h"
#include "calib.h"
#include "config.h"
#include "csv.h"
#include "dbus_client.h"
#include "evdev.h"
#include "hidraw.h"
#include "lm.h"
#include "modprobe_helpers.h"
#include "uinput.h"

#include <errno.h>
#include <math.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MODULE_NAME "lgmagic"
#define CALIB_DIR "/etc/lgmagic"
#define CALIB_JSON "/etc/lgmagic/calib.json"
#define FW_DIR "/lib/firmware"
#define FW_GENERIC "lgmagic_calib.bin"
/* The daemon state: per-device calibration + the daemon's own default
 * (daemon_config_calib_path). */
#define STATE_DIR "/var/lib/lgmagic"
#define DEVICES_D_DIR "/etc/lgmagic/devices.d"
/* gcc's -Wformat-truncation bounds a %s source by its declared array
 * size, so every buffer is sized beyond the capacities that flow into
 * it: state_dir holds STATE_DIR + "/" + MAC + NUL (35), calib_path adds
 * "/calibration.json" (another 17) - 128 covers both with room. */
#define STATE_PATH_MAX 128
#define ACCEL_SECONDS 20.0
#define GYRO_SECONDS 10.0
#define MOUSE_TEST_SECONDS 8.0
#define LM_G 9.80665

/* Quality gates for the accelerometer fit. The residual r is
 * ||M(a - b)|| - g, so the rms is in m/s^2 regardless of the raw
 * counts scale; the spread is scale-free (max axis range / max norm). */
#define GOOD_RMS 0.2		/* m/s^2 (2% of g) */
#define GOOD_SPREAD 0.15
#define GYRO_STD_WARN 100.0	/* counts; above this the remote moved */

static int non_interactive;
static int stdin_closed;	/* set when ask() hits EOF on stdin */

/* ------------------------------------------------------------------ */
/* Interactive helpers                                                 */
/* ------------------------------------------------------------------ */

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void sigsetup(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	/* No SA_RESTART: a pending read returns EINTR so loops can exit. */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
}

static void trimnl(char *s)
{
	size_t n = strlen(s);

	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
		s[--n] = '\0';
}

/* Prompt with a default; empty answer (or non-interactive mode, or EOF
 * on stdin) keeps the default. */
static void ask(const char *prompt, const char *def, char *buf, size_t bufsz)
{
	printf("%s [%s]: ", prompt, def);
	fflush(stdout);
	if (non_interactive || stdin_closed) {
		printf("%s (default)\n", def);
		snprintf(buf, bufsz, "%s", def);
		return;
	}
	if (!fgets(buf, (int)bufsz, stdin)) {
		stdin_closed = 1;
		snprintf(buf, bufsz, "%s", def);
		return;
	}
	trimnl(buf);
	if (!buf[0])
		snprintf(buf, bufsz, "%s", def);
}

static int ask_yn(const char *prompt, int def)
{
	char buf[64];

	ask(prompt, def ? "y" : "n", buf, sizeof(buf));
	return buf[0] == 'y' || buf[0] == 'Y';
}

static double ask_number(const char *prompt, double def, double lo, double hi)
{
	char buf[64], defbuf[64];
	double v;

	snprintf(defbuf, sizeof(defbuf), "%.4g", def);
	ask(prompt, defbuf, buf, sizeof(buf));
	v = strtod(buf, NULL);
	if (v < lo || v > hi) {
		if (!non_interactive && !stdin_closed)
			printf("Value out of range (%.4g..%.4g) - keeping the "
			       "default %.4g\n", lo, hi, def);
		return def;
	}
	return v;
}

/* ------------------------------------------------------------------ */
/* Recording                                                           */
/* ------------------------------------------------------------------ */

/* Record `seconds` of IMU frames into a growing sample array.
 * Returns the number of samples recorded, or -1 on error. */
static long record_samples(struct evdev_imu *dev, double seconds,
			   struct imu_sample **out, char *err, size_t errsz)
{
	struct imu_sample *s;
	size_t cap = 1024, n = 0;
	struct timespec t0, now;
	int have_t0 = 0;
	long i;

	s = malloc(cap * sizeof(*s));
	if (!s) {
		fprintf(stderr, "lgmagic: out of memory\n");
		return -1;
	}
	for (;;) {
		unsigned counter;
		double dt;
		int a[3], g[3], rv;

		rv = evdev_read_frame(dev, &counter, &dt, a, g, err, errsz);
		if (rv < 0) {
			if (g_stop)
				break;
			fprintf(stderr, "lgmagic: %s\n", err);
			free(s);
			return -1;
		}
		if (rv == 0)
			break;	/* EOF */
		if (!have_t0) {
			clock_gettime(CLOCK_MONOTONIC, &t0);
			have_t0 = 1;
		}
		if (n == cap) {
			cap *= 2;
			s = realloc(s, cap * sizeof(*s));
			if (!s) {
				fprintf(stderr, "lgmagic: out of memory\n");
				return -1;
			}
		}
		s[n].counter = counter;
		s[n].dt = dt;
		for (i = 0; i < 3; i++) {
			s[n].accel[i] = a[i];
			s[n].gyro[i] = g[i];
		}
		n++;
		clock_gettime(CLOCK_MONOTONIC, &now);
		printf("\rRecording... %.1f s (%zu frames) ",
		       (now.tv_sec - t0.tv_sec) +
		       (now.tv_nsec - t0.tv_nsec) * 1e-9, n);
		fflush(stdout);
		{
			double elapsed = (now.tv_sec - t0.tv_sec) +
				(now.tv_nsec - t0.tv_nsec) * 1e-9;

			if (elapsed >= seconds)
				break;
		}
	}
	printf("\n");
	*out = s;
	return (long)n;
}

/* ------------------------------------------------------------------ */
/* Step 1: environment                                                 */
/* ------------------------------------------------------------------ */

static int step_environment(struct evdev_imu *dev, char *hidraw_path,
			    size_t hidraw_sz)
{
	char err[256];

	printf("\n=== Step 1: environment ===\n");
	if (geteuid() != 0) {
		fprintf(stderr, "lgmagic: setup needs root - the module "
			"parameters, /etc/modprobe.d and /lib/firmware are "
			"system-wide.\nRun it as: sudo lgmagic setup\n");
		return -1;
	}
	if (!module_is_loaded(MODULE_NAME)) {
		printf("Loading the lgmagic module...\n");
		if (module_load(MODULE_NAME, err, sizeof(err)) < 0) {
			fprintf(stderr, "Warning: %s\n", err);
			fprintf(stderr, "Check the DKMS installation and Secure "
				"Boot (unsigned modules must be enrolled). The "
				"wizard continues, but the kernel part will not "
				"work until this is fixed.\n");
		}
	} else {
		printf("Kernel module lgmagic: loaded.\n");
	}

	/* The IMU evdev node only exists with imu_evdev=1 (the default is
	 * 0); step 2 fixes the parameters if it is missing. */
	if (evdev_find_imu(dev, g_cfg->imu_device, err, sizeof(err)) < 0) {
		fprintf(stderr, "lgmagic: %s\n", err);
		fprintf(stderr, "The lgmagic module must be loaded with "
			"imu_evdev=1 for the IMU to be exposed. After this "
			"wizard has written the parameter file:\n"
			"  unplug the receiver, run 'sudo rmmod lgmagic', "
			"plug it back in.\n");
		return -1;
	}
	printf("IMU device: %s (%s)\n", dev->name, dev->path);
	evdev_close(dev);	/* re-opened after the module reload */

	if (g_cfg->hidraw_device) {
		snprintf(hidraw_path, hidraw_sz, "%s", g_cfg->hidraw_device);
	} else if (hidraw_find_remote(hidraw_path, hidraw_sz, err,
				      sizeof(err)) < 0) {
		fprintf(stderr, "lgmagic: %s\n", err);
		fprintf(stderr, "Is the remote paired and switched on?\n");
		return -1;
	}
	printf("Remote: %s\n", hidraw_path);

	if (access("/dev/uinput", W_OK) == 0 ||
	    access("/dev/input/uinput", W_OK) == 0) {
		printf("uinput: available.\n");
	} else {
		printf("Warning: /dev/uinput is not writable. Install the udev "
		       "rule (51-lgimu.rules) and add your user to the "
		       "'input' group, or run the airmouse as root.\n");
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Step 2: input mode                                                  */
/* ------------------------------------------------------------------ */

/* ':' -> '_', the same sanitisation as lgmagic_sanitize_mac() in the
 * kernel (used for the firmware blob name and the daemon state paths). */
static int sanitize_mac(const char *uniq, char *out, size_t outsz)
{
	size_t i, n = strlen(uniq);

	if (n + 1 > outsz)
		return -1;
	for (i = 0; i < n; i++)
		out[i] = uniq[i] == ':' ? '_' : uniq[i];
	out[n] = '\0';
	return 0;
}

/* Returns 1 for the daemon mode (the default: raw_only=1, lgmagicd
 * takes over the input), 0 for the v1 kernel airmouse fallback. */
static int ask_mode(void)
{
	printf("\n=== Step 2: input mode ===\n");
	printf("v2 drives the remote through the lgmagicd daemon: one virtual\n"
	       "mouse and keyboard, profiles, per-device calibration and\n"
	       "button mapping. The kernel then only decodes the raw reports\n"
	       "(raw_only=1). The v1 kernel airmouse remains as a fallback.\n");
	if (non_interactive) {
		printf("Non-interactive: the daemon mode is used.\n");
		return 1;
	}
	return ask_yn("Use the daemon (recommended)", 1);
}

/* ------------------------------------------------------------------ */
/* Step 3: module parameters                                           */
/* ------------------------------------------------------------------ */

static void step_module_params(int daemon_mode)
{
	const char *params = daemon_mode ? "raw_only=1 imu_evdev=1"
					 : "raw_only=0 airmouse=1 imu_evdev=1";
	char err[256];

	printf("\n=== Step 3: module parameters ===\n");
	printf("Writing /etc/modprobe.d/lgmagic.conf (%s)...\n", params);
	if (module_write_conf(MODULE_NAME, params, err, sizeof(err)) < 0) {
		fprintf(stderr, "Warning: %s\n", err);
		return;
	}
	printf("Reloading the module to apply the parameters...\n");
	if (module_reload(MODULE_NAME, err, sizeof(err)) < 0) {
		fprintf(stderr, "Note: %s\n", err);
		fprintf(stderr, "The module is in use by the connected remote. "
			"The new parameters will apply after:\n"
			"  unplug the receiver, run 'sudo rmmod lgmagic', "
			"plug it back in (or reboot).\n");
	} else {
		printf("Module reloaded with the new parameters.\n");
	}
	if (daemon_mode) {
		/* Best-effort: the unit exists once the package is installed.
		 * On a v1 -> v2 upgrade the wizard may run before the daemon
		 * package - systemctl then fails and the message says so. */
		if (access("/usr/bin/systemctl", X_OK) == 0) {
			printf("Enabling lgmagicd...\n");
			if (system("systemctl enable --now lgmagicd "
				   ">/dev/null 2>&1") == 0)
				printf("lgmagicd enabled and started.\n");
			else
				fprintf(stderr, "Warning: 'systemctl enable --now "
					"lgmagicd' failed - start it manually "
					"after the install.\n");
		}
	}
}

/* ------------------------------------------------------------------ */
/* Steps 4-5: accelerometer / gyroscope calibration                    */
/* ------------------------------------------------------------------ */

/* Scale-free orientation spread: max axis range / max sample norm. */
static double accel_spread(const struct imu_sample *s, long n)
{
	double mn[3], mx[3], maxnorm = 0.0, best = 0.0;
	long i;
	int j;

	for (j = 0; j < 3; j++) {
		mn[j] = 1e30;
		mx[j] = -1e30;
	}
	for (i = 0; i < n; i++) {
		double nrm = 0.0;

		for (j = 0; j < 3; j++) {
			double v = s[i].accel[j];

			if (v < mn[j])
				mn[j] = v;
			if (v > mx[j])
				mx[j] = v;
			nrm += v * v;
		}
		nrm = sqrt(nrm);
		if (nrm > maxnorm)
			maxnorm = nrm;
	}
	for (j = 0; j < 3; j++)
		if (mx[j] - mn[j] > best)
			best = mx[j] - mn[j];
	return maxnorm > 1.0 ? best / maxnorm : 0.0;
}

static int step_accel_calib(struct evdev_imu *dev, struct calib *c,
			    const char *rec_dir)
{
	/* rec_dir is bounded by STATE_PATH_MAX (the caller builds it from
	 * calib_path); the +32 covers "/calib_accel.csv". */
	char csv_path[STATE_PATH_MAX + 32];
	char err[256];
	int attempt;

	snprintf(csv_path, sizeof(csv_path), "%s/calib_accel.csv", rec_dir);

	printf("\n=== Step 4: accelerometer calibration ===\n");
	printf("Slowly rotate the remote so that each axis points up and down\n"
	       "in turn. The fit needs at least 6 well-spread orientations.\n");
	for (attempt = 1; attempt <= 3; attempt++) {
		struct imu_sample *s = NULL;
		double (*a)[3];
		double cost, rms, spread;
		long n, i;

		ask_yn("Start the 20 s recording", 1);
		n = record_samples(dev, ACCEL_SECONDS, &s, err, sizeof(err));
		if (n < 0)
			return -1;
		if (g_stop) {
			free(s);
			fprintf(stderr, "Interrupted.\n");
			return -1;
		}
		if (n < 6) {
			printf("Only %ld samples recorded - need at least 6 in "
			       "different orientations.\n", n);
			free(s);
			if (stdin_closed || !ask_yn("Record again", 1))
				return -1;
			continue;
		}
		if (csv_write(csv_path, s, (size_t)n, err, sizeof(err)) == 0)
			printf("Saved the recording to %s\n", csv_path);
		a = malloc((size_t)n * sizeof(*a));
		if (!a) {
			free(s);
			fprintf(stderr, "lgmagic: out of memory\n");
			return -1;
		}
		for (i = 0; i < n; i++)
			memcpy(a[i], s[i].accel, sizeof(a[i]));
		lm_fit_accel(a, (size_t)n, c->accel_bias, c->accel_matrix,
			     &cost);
		free(a);
		rms = sqrt(cost);
		spread = accel_spread(s, n);
		free(s);
		printf("mean squared residual: %.6g (rms %.4g m/s^2, %.2f%% of "
		       "g)\n", cost, rms, rms / LM_G * 100.0);
		printf("orientation spread: %.2f of the signal\n", spread);
		if (rms <= GOOD_RMS && spread >= GOOD_SPREAD)
			return 0;
		printf("Poor fit - rotate the remote more, in all axes.\n");
		if (stdin_closed || !ask_yn("Record again", 1))
			break;
	}
	fprintf(stderr, "Warning: accepting a poor accelerometer calibration; "
		"re-run 'sudo lgmagic setup' to retry.\n");
	return 0;
}

static int step_gyro_calib(struct evdev_imu *dev, struct calib *c,
			   const char *rec_dir)
{
	struct imu_sample *s = NULL;
	char csv_path[STATE_PATH_MAX + 32];
	char err[256];
	double std[3];
	long n, i;
	int j;

	snprintf(csv_path, sizeof(csv_path), "%s/calib_gyro.csv", rec_dir);
	printf("\n=== Step 5: gyroscope calibration ===\n");
	printf("Lay the remote down on a flat surface and do not touch it.\n");
	ask_yn("Start the 10 s recording", 1);
	n = record_samples(dev, GYRO_SECONDS, &s, err, sizeof(err));
	if (n < 0)
		return -1;
	if (g_stop) {
		free(s);
		fprintf(stderr, "Interrupted.\n");
		return -1;
	}
	if (n == 0) {
		fprintf(stderr, "lgmagic: no samples recorded\n");
		free(s);
		return -1;
	}
	if (csv_write(csv_path, s, (size_t)n, err, sizeof(err)) == 0)
		printf("Saved the recording to %s\n", csv_path);
	for (j = 0; j < 3; j++) {
		c->gyro_bias[j] = 0.0;
		for (i = 0; i < n; i++)
			c->gyro_bias[j] += s[i].gyro[j];
		c->gyro_bias[j] /= (double)n;
		std[j] = 0.0;
		for (i = 0; i < n; i++)
			std[j] += (s[i].gyro[j] - c->gyro_bias[j]) *
				  (s[i].gyro[j] - c->gyro_bias[j]);
		std[j] = sqrt(std[j] / (double)n);
	}
	free(s);
	printf("gyro bias: [%.6g %.6g %.6g]\n", c->gyro_bias[0],
	       c->gyro_bias[1], c->gyro_bias[2]);
	printf("gyro noise (std): [%.3g %.3g %.3g]\n", std[0], std[1],
	       std[2]);
	for (j = 0; j < 3; j++)
		if (std[j] > GYRO_STD_WARN) {
			fprintf(stderr, "Warning: the gyro values were moving "
				"during the recording - repeat it if the "
				"airmouse drifts.\n");
			break;
		}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Steps 6-7: tuning questions, firmware blob                          */
/* ------------------------------------------------------------------ */

static void ask_tuning(struct calib *c, double *alpha, double *mouse_k)
{
	double s;

	printf("\n=== Step 6: calibration tuning ===\n");
	printf("The kernel multiplies (gyro - bias) by the gyro scale. The\n"
	       "README recommends about 0.07; raise it if the pointer feels\n"
	       "too slow.\n");
	s = ask_number("Gyroscope scale", g_cfg->gyro_scale_default,
		       0.001, 10.0);
	c->gyro_scale[0] = c->gyro_scale[1] = c->gyro_scale[2] = s;
	printf("Kernel low-pass alpha (0..1, smaller = smoother pointer):\n");
	*alpha = ask_number("alpha", *alpha, 0.0, 1.0);
	printf("Kernel airmouse sensitivity mouse_k (0..1):\n");
	*mouse_k = ask_number("mouse_k", *mouse_k, 0.0, 1.0);
}

static int write_blob_file(const char *name,
			   const struct lgmagic_airmouse_calib *blob,
			   char *err, size_t errsz)
{
	char path[256];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", FW_DIR, name);
	f = fopen(path, "wb");
	if (!f) {
		/* %.200s bounds the path so the message always fits in err */
		snprintf(err, errsz, "cannot write %.200s: %s", path,
			 strerror(errno));
		return -1;
	}
	if (fwrite(blob, sizeof(*blob), 1, f) != 1 || fclose(f) != 0) {
		snprintf(err, errsz, "write error on %.200s", path);
		return -1;
	}
	printf("Wrote %s\n", path);
	return 0;
}

static int step_blob(const struct calib *c, double alpha, double mouse_k,
		     const char *uniq)
{
	struct lgmagic_airmouse_calib blob;
	char fname[128], mac[18];
	char err[256];

	printf("\n=== Step 7: firmware blob ===\n");
	if (mkdir(FW_DIR, 0755) < 0 && errno != EEXIST) {
		fprintf(stderr, "lgmagic: cannot create %s: %s\n", FW_DIR,
			strerror(errno));
		return -1;
	}
	calib_to_blob(c, (float)alpha, (float)mouse_k, &blob);
	if (calib_validate_blob(&blob) < 0)
		fprintf(stderr, "Warning: the blob fails the kernel validation "
			"ranges; writing it anyway (the kernel disables the "
			"airmouse if it rejects it).\n");

	if (uniq[0] && strlen(uniq) == 17) {
		sanitize_mac(uniq, mac, sizeof(mac));
		snprintf(fname, sizeof(fname), "lgmagic_calib_%s.bin", mac);
		if (write_blob_file(fname, &blob, err, sizeof(err)) < 0) {
			fprintf(stderr, "lgmagic: %s\n", err);
			return -1;
		}
	} else if (uniq[0]) {
		printf("The hidraw uniq string ('%s') is not a MAC - writing "
		       "only the generic blob.\n", uniq);
	} else {
		printf("No uniq string available - writing only the generic "
		       "blob.\n");
	}
	if (write_blob_file(FW_GENERIC, &blob, err, sizeof(err)) < 0) {
		fprintf(stderr, "lgmagic: %s\n", err);
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Step 8: per-device state (daemon)                                   */
/* ------------------------------------------------------------------ */

/* The parent directory of the calibration path (the recordings are
 * written next to the authoritative file: /var/lib/lgmagic/<MAC>/ in
 * daemon mode, /etc/lgmagic/ in kernel mode - /etc must not hold
 * calibration data in daemon mode). */
static void dir_of(const char *path, char *out, size_t outsz)
{
	const char *slash = strrchr(path, '/');
	size_t n;

	if (!slash) {
		snprintf(out, outsz, ".");
		return;
	}
	if (slash == path) {
		snprintf(out, outsz, "/");
		return;
	}
	n = (size_t)(slash - path);
	if (n >= outsz)
		n = outsz - 1;
	memcpy(out, path, n);
	out[n] = '\0';
}

/* mkdir -p.  The wizard only ever creates under its own roots
 * (/var/lib/lgmagic/..., /etc/lgmagic) - a few components deep. */
static int mkdir_p(const char *dir)
{
	char path[STATE_PATH_MAX], *p;

	snprintf(path, sizeof(path), "%s", dir);
	for (p = path + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(path, 0755) < 0 && errno != EEXIST) {
			*p = '/';
			return -1;
		}
		*p = '/';
	}
	if (mkdir(path, 0755) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

/* The wizard runs as root and writes the daemon state files directly -
 * polkit gates unprivileged writes; sudo has already authenticated.
 * `calib_path`/`toml_path` are built by the caller (an empty toml_path
 * means no per-device entry, e.g. when the uniq is not a MAC). */
static int step_daemon_state(const struct calib *c, int daemon_mode,
			     const char *calib_path, const char *toml_path)
{
	char dir[STATE_PATH_MAX], err[256];
	char *slash;

	printf("\n=== Step 8: daemon state ===\n");
	/* mkdir -p the calibration directory (the <MAC>/"unknown" dir and
	 * the state dir above it; the latter is also created by tmpfiles,
	 * this keeps the wizard self-sufficient). */
	if (mkdir(STATE_DIR, 0755) < 0 && errno != EEXIST) {
		fprintf(stderr, "lgmagic: cannot create %s: %s\n", STATE_DIR,
			strerror(errno));
		return -1;
	}
	snprintf(dir, sizeof(dir), "%s", calib_path);
	slash = strrchr(dir, '/');
	if (slash)
		*slash = '\0';
	if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
		fprintf(stderr, "lgmagic: cannot create %s: %s\n", dir,
			strerror(errno));
		return -1;
	}
	if (calib_save_json(c, calib_path, err, sizeof(err)) < 0) {
		fprintf(stderr, "lgmagic: %s\n", err);
		return -1;
	}
	printf("Calibration saved to %s\n", calib_path);

	if (toml_path[0]) {
		FILE *f;

		if (mkdir(DEVICES_D_DIR, 0755) < 0 && errno != EEXIST) {
			fprintf(stderr, "lgmagic: cannot create %s: %s\n",
				DEVICES_D_DIR, strerror(errno));
			return -1;
		}
		f = fopen(toml_path, "w");
		if (!f) {
			fprintf(stderr, "lgmagic: cannot write %s: %s\n",
				toml_path, strerror(errno));
			return -1;
		}
		fprintf(f, "# Written by 'lgmagic setup'.\n"
			"profile = \"default\"\n"
			"calib = \"%s\"\n"
			"airmouse = %s\n", calib_path,
			daemon_mode ? "true" : "false");
		if (fclose(f) != 0) {
			fprintf(stderr, "lgmagic: write error on %s\n",
				toml_path);
			return -1;
		}
		printf("Wrote %s\n", toml_path);
	}

	if (daemon_mode) {
		/* Ask the daemon to pick up the new state. The daemon may
		 * not be installed/running yet (v1 -> v2 upgrade) - then
		 * this is only a note. */
		struct dbus_client cli;
		char *err_name = NULL, *err_msg = NULL;
		int rc;

		if (dbus_connect(&cli, NULL, err, sizeof(err)) < 0) {
			fprintf(stderr, "Note: %s\n", err);
		} else {
			rc = dbus_call(&cli, "Reload", "", NULL, &err_name,
				       &err_msg, NULL, err, sizeof(err));
			if (rc == 0)
				printf("The daemon reloaded the state.\n");
			else if (rc == 1)
				fprintf(stderr, "Warning: Reload failed: %s "
					"(%s)\n",
					err_msg ? err_msg : "(no message)",
					err_name ? err_name : "no error name");
			else
				fprintf(stderr, "Note: %s\n", err);
			dbus_disconnect(&cli);
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Step 9: reload + dmesg verification                                 */
/* ------------------------------------------------------------------ */

static void step_reload_verify(void)
{
	char err[256];
	int i, rc = -2;

	printf("\n=== Step 9: loading the calibration into the kernel ===\n");
	if (module_reload(MODULE_NAME, err, sizeof(err)) < 0) {
		fprintf(stderr, "Note: %s\n", err);
		fprintf(stderr, "The module is in use by the connected remote. "
			"The calibration blob will be picked up after:\n"
			"  unplug the receiver, run 'sudo rmmod lgmagic', "
			"plug it back in (or reboot).\n");
		return;
	}
	/* request_firmware runs at probe time; give it a moment. */
	for (i = 0; i < 20; i++) {
		usleep(50000);
		rc = dmesg_contains("Loading LG Magic calibration");
		if (rc == 1) {
			printf("The kernel loaded the calibration blob (found "
			       "in dmesg).\n");
			return;
		}
	}
	if (rc == -1)
		fprintf(stderr, "Cannot read dmesg - check manually: "
			"dmesg | grep 'LG Magic'\n");
	else
		fprintf(stderr, "Warning: no 'Loading LG Magic calibration' "
			"in dmesg; the blob may not have been loaded.\n");
}

/* ------------------------------------------------------------------ */
/* Step 10: airmouse test                                              */
/* ------------------------------------------------------------------ */

/* Daemon mode: ask the daemon (ListDevices) - if it is driving the
 * remote, the pointer already follows and there is nothing to test
 * standalone. Without a reachable daemon the v1 standalone uinput test
 * is the fallback (the IMU evdev is never grabbed, so it works while
 * lgmagicd runs). */
static void step_mouse_test(struct evdev_imu *dev, const struct calib *c,
			    int daemon_mode)
{
	struct airmouse am;
	struct timespec t0, now;
	char err[256];
	int ufd, have_t0 = 0;

	printf("\n=== Step 10: airmouse test ===\n");
	if (non_interactive) {
		printf("Skipped (--non-interactive). Try it later with "
		       "'lgmagic imu --mouse'.\n");
		return;
	}
	if (daemon_mode) {
		struct dbus_client cli;
		struct dbus_value *out = NULL;
		char *err_name = NULL, *err_msg = NULL;
		int rc;

		if (dbus_connect(&cli, NULL, err, sizeof(err)) == 0) {
			/* err_name/err_msg are borrowed from the client -
			 * use them before the disconnect frees them. */
			rc = dbus_call(&cli, "ListDevices", "", NULL,
				       &err_name, &err_msg, &out, err,
				       sizeof(err));
			if (rc == 0) {
				dbus_disconnect(&cli);
				dbus_value_free(out);
				printf("lgmagicd is running and driving the "
				       "remote - move it, the pointer should "
				       "follow. No need for Ctrl+C; the test "
				       "continues as you use the remote.\n");
				return;
			}
			if (rc == 1)
				fprintf(stderr, "Note: ListDevices failed: %s "
					"(%s)\n",
					err_msg ? err_msg : "(no message)",
					err_name ? err_name : "no error name");
			dbus_disconnect(&cli);
		}
		printf("lgmagicd is not reachable - falling back to the "
		       "standalone airmouse test.\n");
	}
	if (!ask_yn("Run a quick airmouse test", 1))
		return;
	/* The reload in step 2 may have recreated the evdev node. */
	evdev_close(dev);
	if (evdev_find_imu(dev, g_cfg->imu_device, err, sizeof(err)) < 0) {
		fprintf(stderr, "lgmagic: %s\n", err);
		return;
	}
	ufd = uinput_open(err, sizeof(err));
	if (ufd < 0) {
		fprintf(stderr, "lgmagic: %s\n", err);
		evdev_close(dev);
		return;
	}
	airmouse_init(&am, g_cfg->lpf_alpha, g_cfg->mouse_scale);
	g_stop = 0;
	printf("Move the remote - the pointer should follow. Ctrl+C ends the "
	       "test.\n");
	for (;;) {
		unsigned counter;
		double dt, a_corr[3], g_corr[3], filt_out[3];
		int accel[3], gyro[3], dx, dy, rv;

		rv = evdev_read_frame(dev, &counter, &dt, accel, gyro, err,
				      sizeof(err));
		if (rv < 0) {
			if (g_stop)
				break;
			fprintf(stderr, "lgmagic: %s\n", err);
			break;
		}
		if (rv == 0)
			break;
		if (!have_t0) {
			clock_gettime(CLOCK_MONOTONIC, &t0);
			have_t0 = 1;
		}
		clock_gettime(CLOCK_MONOTONIC, &now);
		if ((now.tv_sec - t0.tv_sec) +
		    (now.tv_nsec - t0.tv_nsec) * 1e-9 >=
		    MOUSE_TEST_SECONDS)
			break;
		calib_apply(c, (double[3]){ accel[0], accel[1], accel[2] },
			    (double[3]){ gyro[0], gyro[1], gyro[2] },
			    a_corr, g_corr);
		airmouse_process(&am, g_corr, filt_out, &dx, &dy);
		if (uinput_move(ufd, dx, dy) < 0)
			fprintf(stderr, "lgmagic: uinput write failed\n");
		printf("REL_X : %d REL_Y: %d\n", dx, dy);
	}
	uinput_close(ufd);
	printf("Airmouse test finished.\n");
}

/* ------------------------------------------------------------------ */
/* Step 11: user configuration                                         */
/* ------------------------------------------------------------------ */

static void step_save_user_config(double alpha, double mouse_k,
				  const char *calib_path)
{
	const char *sudo_user = getenv("SUDO_USER");
	const char *saved_home = getenv("HOME");
	struct passwd *pw = NULL;
	char path[4096] = { 0 };
	char err[256], buf[64];

	printf("\n=== Step 11: user configuration ===\n");
	if (config_set_key(g_cfg, "default_calib", calib_path, err,
			   sizeof(err)) < 0)
		fprintf(stderr, "Warning: %s\n", err);
	snprintf(buf, sizeof(buf), "%.4g", alpha);
	if (config_set_key(g_cfg, "alpha", buf, err, sizeof(err)) < 0)
		fprintf(stderr, "Warning: %s\n", err);
	snprintf(buf, sizeof(buf), "%.4g", mouse_k);
	if (config_set_key(g_cfg, "mouse_k", buf, err, sizeof(err)) < 0)
		fprintf(stderr, "Warning: %s\n", err);

	/* Under sudo, HOME belongs to root - save into the invoking
	 * user's home and fix the ownership. */
	if (sudo_user)
		pw = getpwnam(sudo_user);
	if (pw) {
		setenv("HOME", pw->pw_dir, 1);
		snprintf(path, sizeof(path), "%s/.config/lgmagic/config.toml",
			 pw->pw_dir);
	}
	if (config_save_user(g_cfg, err, sizeof(err)) < 0) {
		fprintf(stderr, "Warning: %s\n", err);
	} else {
		printf("Saved %s\n", path[0] ? path :
		       "~/.config/lgmagic/config.toml");
		if (pw) {
			char dir[4096];

			snprintf(dir, sizeof(dir), "%s/.config", pw->pw_dir);
			/* best effort: the files stay usable if this fails */
			if (chown(dir, pw->pw_uid, pw->pw_gid) < 0) {
			}
			snprintf(dir, sizeof(dir), "%s/.config/lgmagic",
				 pw->pw_dir);
			if (chown(dir, pw->pw_uid, pw->pw_gid) < 0) {
			}
			if (chown(path, pw->pw_uid, pw->pw_gid) < 0) {
			}
		}
	}
	if (saved_home)
		setenv("HOME", saved_home, 1);
	else
		unsetenv("HOME");
}

/* ------------------------------------------------------------------ */
/* Step 12: summary                                                    */
/* ------------------------------------------------------------------ */

static void step_summary(int daemon_mode, const char *calib_path)
{
	char rec_dir[STATE_PATH_MAX];

	dir_of(calib_path, rec_dir, sizeof(rec_dir));
	printf("\n=== Step 12: summary ===\n");
	printf("What was done:\n");
	printf("  - /etc/modprobe.d/lgmagic.conf: %s\n",
	       daemon_mode ? "raw_only=1 imu_evdev=1"
			   : "raw_only=0 airmouse=1 imu_evdev=1");
	printf("  - calibration (accel + gyro) in %s\n", calib_path);
	printf("  - %s/calib_accel.csv, calib_gyro.csv (recordings, for "
	       "'lgmagic calibrate')\n", rec_dir);
	if (!daemon_mode)
		printf("  - %s/%s* (per-device + generic kernel blob)\n",
		       FW_DIR, "lgmagic_calib");
	printf("  - user configuration (~/.config/lgmagic/config.toml)\n");
	printf("\nUsage:\n");
	if (daemon_mode) {
		printf("  lgmagic device list      paired remotes\n");
		printf("  lgmagic device status    daemon status\n");
		printf("  lgmagic profile set <MAC> <profile>\n");
		printf("  lgmagic button map <MAC> KEY_FROM KEY_TO\n");
		printf("  lgmagic diagnose         troubleshooting report\n");
	} else {
		printf("  lgmagic imu --mouse     airmouse (virtual mouse)\n");
		printf("  lgmagic imu --ahrs      orientation angles\n");
		printf("  lgmagic imu --cube      terminal cube\n");
	}
	printf("  lgmagic analyze         HID report decoder\n");
	printf("  lgmagic config          show / edit the configuration\n");
	printf("\nRe-run this wizard any time: sudo lgmagic setup\n");
	printf("If the module parameters or the blob have not been applied yet,\n"
	       "unplug the receiver, run 'sudo rmmod lgmagic', and plug it\n"
	       "back in (or reboot).\n");
}

/* ------------------------------------------------------------------ */

static void usage(FILE *out)
{
	fputs("Usage: lgmagic setup [--non-interactive]\n"
	      "\n"
	      "Interactive wizard: module parameters, accelerometer and\n"
	      "gyroscope calibration, firmware blob installation and user\n"
	      "configuration. Run as root: sudo lgmagic setup\n"
	      "\n"
	      "  --non-interactive   accept the defaults everywhere\n", out);
}

int cmd_setup(int argc, char **argv)
{
	struct evdev_imu dev;
	struct calib c;
	double alpha, mouse_k;
	char hidraw_path[256], uniq[64], err[256];
	char calib_path[STATE_PATH_MAX], toml_path[STATE_PATH_MAX];
	char rec_dir[STATE_PATH_MAX];
	int daemon_mode, i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--non-interactive") == 0)
			non_interactive = 1;
		else if (strcmp(argv[i], "--help") == 0 ||
			 strcmp(argv[i], "-h") == 0) {
			usage(stdout);
			return 0;
		} else {
			fprintf(stderr, "lgmagic setup: unexpected argument "
				"'%s'\n", argv[i]);
			usage(stderr);
			return 1;
		}
	}

	printf("LG Magic Remote setup wizard\n");
	sigsetup();

	if (step_environment(&dev, hidraw_path, sizeof(hidraw_path)) < 0)
		return 1;
	if (g_stop) {
		fprintf(stderr, "Interrupted.\n");
		return 1;
	}
	daemon_mode = ask_mode();
	if (g_stop) {
		fprintf(stderr, "Interrupted.\n");
		return 1;
	}
	step_module_params(daemon_mode);
	if (g_stop) {
		fprintf(stderr, "Interrupted.\n");
		return 1;
	}
	if (mkdir(CALIB_DIR, 0755) < 0 && errno != EEXIST) {
		fprintf(stderr, "lgmagic: cannot create %s: %s\n", CALIB_DIR,
			strerror(errno));
		return 1;
	}

	/* The reload in step 2 may have recreated the evdev nodes. */
	if (evdev_find_imu(&dev, g_cfg->imu_device, err, sizeof(err)) < 0) {
		fprintf(stderr, "lgmagic: %s\n", err);
		fprintf(stderr, "The module is not running with imu_evdev=1 "
			"yet - apply the parameters from step 2 and re-run.\n");
		return 1;
	}
	uniq[0] = '\0';
	if (hidraw_get_uniq(hidraw_path, uniq, sizeof(uniq)) == 0 && uniq[0])
		printf("Device MAC: %s\n", uniq);

	/* One calibration source per mode.  Daemon mode: the ONLY
	 * authoritative file is /var/lib/lgmagic/<MAC>/calibration.json
	 * (the daemon's default path); without a MAC the daemon resolves
	 * the "unknown" identity under the state dir, so the wizard
	 * writes exactly there.  The firmware blob is legacy-only
	 * (raw_only=0).  Kernel mode keeps the v1 layout: the blob plus
	 * /etc/lgmagic/calib.json.  /etc holds policy and profiles, not
	 * calibration data. */
	if (uniq[0] && strlen(uniq) == 17) {
		char mac[18], state_dir[64];

		sanitize_mac(uniq, mac, sizeof(mac));
		snprintf(state_dir, sizeof(state_dir), "%s/%s", STATE_DIR,
			 mac);
		snprintf(calib_path, sizeof(calib_path), "%s/calibration.json",
			 state_dir);
		snprintf(toml_path, sizeof(toml_path), "%s/%s.toml",
			 DEVICES_D_DIR, mac);
	} else {
		if (uniq[0])
			printf("The uniq string ('%s') is not a MAC.\n", uniq);
		if (daemon_mode) {
			snprintf(calib_path, sizeof(calib_path),
				 "%s/unknown/calibration.json", STATE_DIR);
			/* no MAC -> no per-device entry; the daemon picks
			 * the same file up for the "unknown" identity */
			toml_path[0] = '\0';
			printf("Daemon mode without a MAC: using %s\n",
			       calib_path);
		} else {
			snprintf(calib_path, sizeof(calib_path), "%s",
				 CALIB_JSON);
			toml_path[0] = '\0';
		}
	}

	/* The recordings live next to the authoritative file. */
	dir_of(calib_path, rec_dir, sizeof(rec_dir));
	if (mkdir_p(rec_dir) < 0) {
		fprintf(stderr, "lgmagic: cannot create %s: %s\n",
			rec_dir, strerror(errno));
		return 1;
	}

	calib_init_identity(&c);
	if (step_accel_calib(&dev, &c, rec_dir) < 0)
		return 1;
	if (g_stop) {
		evdev_close(&dev);
		fprintf(stderr, "Interrupted.\n");
		return 1;
	}
	if (step_gyro_calib(&dev, &c, rec_dir) < 0) {
		evdev_close(&dev);
		return 1;
	}
	if (g_stop) {
		evdev_close(&dev);
		fprintf(stderr, "Interrupted.\n");
		return 1;
	}

	alpha = g_cfg->alpha;
	mouse_k = g_cfg->mouse_k;
	ask_tuning(&c, &alpha, &mouse_k);

	/* The firmware blob is for the legacy kernel airmouse only -
	 * daemon mode calibrates from the JSON and must not write one
	 * (one calibration source per mode). */
	if (daemon_mode) {
		printf("\n=== Step 7: firmware blob (skipped) ===\n"
		       "Daemon mode: the kernel airmouse is off, so the "
		       "blob is not written; the calibration JSON is the "
		       "single source.\n");
	} else if (step_blob(&c, alpha, mouse_k, uniq) < 0) {
		evdev_close(&dev);
		return 1;
	}
	if (step_daemon_state(&c, daemon_mode, calib_path, toml_path) < 0) {
		evdev_close(&dev);
		return 1;
	}
	step_reload_verify();
	step_mouse_test(&dev, &c, daemon_mode);
	step_save_user_config(alpha, mouse_k, calib_path);
	step_summary(daemon_mode, calib_path);
	evdev_close(&dev);
	return 0;
}
