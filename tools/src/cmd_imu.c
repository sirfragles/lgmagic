/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cmd_imu.c - `lgmagic imu` subcommand (scripts/display_imu.py).
 *
 * Modes match the Python script:
 *   (default)      raw dt/Accel/Gyro prints
 *   --csv FILE     record raw samples (no calibration, like Python)
 *   --calib FILE   apply bias/matrix, R_align, gyro deg->rad
 *   --mouse        LPF + uinput virtual mouse (needs --calib, like Python)
 *   --ahrs         Madgwick filter + Roll/Pitch/Yaw
 *   --cube         ANSI wireframe cube (implies --ahrs - a documented fix
 *                  for the Python bug where --cube alone showed a static
 *                  cube because the update lived in the --ahrs branch)
 *
 * Extensions over Python: --device, --duration S, --print-calib.
 * No threads: read -> process -> print, Ctrl+C via a flag + EINTR.
 */
#include "airmouse.h"
#include "calib.h"
#include "config.h"
#include "csv.h"
#include "cube.h"
#include "evdev.h"
#include "madgwick.h"
#include "matrix.h"
#include "uinput.h"

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void usage(FILE *out)
{
	fputs("Usage: lgmagic imu [options]\n"
	      "\n"
	      "Reads the IMU via evdev (auto-detected, or --device / config).\n"
	      "\n"
	      "  --csv FILE       record raw samples to FILE (no calibration)\n"
	      "  --calib FILE     calibration JSON (default: config default_calib)\n"
	      "  --mouse          airmouse: LPF + virtual mouse via uinput\n"
	      "  --ahrs           Madgwick filter, print Roll/Pitch/Yaw\n"
	      "  --cube           ANSI wireframe cube (implies --ahrs)\n"
	      "  --device PATH    evdev device (default: config imu_device)\n"
	      "  --duration S     stop after S seconds\n"
	      "  --print-calib    print the loaded calibration and exit\n", out);
}

int cmd_imu(int argc, char **argv)
{
	const char *csv_path = NULL, *calib_path = NULL, *device_path = NULL;
	int do_mouse = 0, do_ahrs = 0, do_cube = 0, do_print_calib = 0;
	double duration = 0.0;
	struct evdev_imu dev;
	struct calib cal;
	struct imu_sample *samples = NULL;
	size_t nsamples = 0, cap = 0;
	struct madgwick_state mad;
	struct airmouse am;
	int q_init = 0, ufd = -1;
	struct sigaction sa;
	struct timespec t0;
	int have_t0 = 0;
	char err[256];
	int i, ret = 0;

	if (argc >= 2 && (strcmp(argv[1], "--help") == 0 ||
			  strcmp(argv[1], "-h") == 0)) {
		usage(stdout);
		return 0;
	}
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc)
			csv_path = argv[++i];
		else if (strncmp(argv[i], "--csv=", 6) == 0)
			csv_path = argv[i] + 6;
		else if (strcmp(argv[i], "--calib") == 0 && i + 1 < argc)
			calib_path = argv[++i];
		else if (strncmp(argv[i], "--calib=", 8) == 0)
			calib_path = argv[i] + 8;
		else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc)
			device_path = argv[++i];
		else if (strncmp(argv[i], "--device=", 9) == 0)
			device_path = argv[i] + 9;
		else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
			duration = strtod(argv[++i], NULL);
		else if (strncmp(argv[i], "--duration=", 11) == 0)
			duration = strtod(argv[i] + 11, NULL);
		else if (strcmp(argv[i], "--mouse") == 0)
			do_mouse = 1;
		else if (strcmp(argv[i], "--ahrs") == 0)
			do_ahrs = 1;
		else if (strcmp(argv[i], "--cube") == 0)
			do_cube = 1;
		else if (strcmp(argv[i], "--print-calib") == 0)
			do_print_calib = 1;
		else {
			fprintf(stderr, "lgmagic imu: unexpected argument "
				"'%s'\n", argv[i]);
			usage(stderr);
			return 1;
		}
	}
	/* Documented fix: --cube implies --ahrs (Python showed a static
	 * cube without it - the update lived in the --ahrs branch). */
	if (do_cube)
		do_ahrs = 1;

	/* Calibration: --calib flag > config default_calib. */
	if (!calib_path)
		calib_path = g_cfg->default_calib;
	if (calib_path) {
		if (calib_load(calib_path, &cal, err, sizeof(err)) < 0) {
			fprintf(stderr, "lgmagic: %s\n", err);
			return 1;
		}
	}
	if (do_print_calib) {
		if (!calib_path) {
			fprintf(stderr, "lgmagic: no calibration loaded "
				"(use --calib FILE)\n");
			return 1;
		}
		printf("accel bias: [%g %g %g]\n", cal.accel_bias[0],
		       cal.accel_bias[1], cal.accel_bias[2]);
		for (i = 0; i < 3; i++)
			printf("accel matrix[%d]: [%g %g %g]\n", i,
			       cal.accel_matrix[i][0], cal.accel_matrix[i][1],
			       cal.accel_matrix[i][2]);
		printf("gyro bias: [%g %g %g]\n", cal.gyro_bias[0],
		       cal.gyro_bias[1], cal.gyro_bias[2]);
		printf("gyro scale: [%g %g %g]\n", cal.gyro_scale[0],
		       cal.gyro_scale[1], cal.gyro_scale[2]);
		return 0;
	}

	/* Cube needs a real terminal; fall back to --ahrs text mode. */
	if (do_cube && !cube_available()) {
		fprintf(stderr, "lgmagic: --cube needs a terminal; "
			"falling back to --ahrs text mode\n");
		do_cube = 0;
	}

	/* Device: --device flag > config imu_device > auto-detect. */
	if (!device_path)
		device_path = g_cfg->imu_device;
	if (evdev_find_imu(&dev, device_path, err, sizeof(err)) < 0) {
		fprintf(stderr, "lgmagic: %s\n", err);
		return 1;
	}
	printf("Using device: %s (%s)\n", dev.name, dev.path);
	printf("Initial state: [%d, %d, %d] [%d, %d, %d]\n", dev.accel[0],
	       dev.accel[1], dev.accel[2], dev.gyro[0], dev.gyro[1],
	       dev.gyro[2]);

	if (do_mouse) {
		ufd = uinput_open(err, sizeof(err));
		if (ufd < 0) {
			fprintf(stderr, "lgmagic: %s\n", err);
			evdev_close(&dev);
			return 1;
		}
	}
	airmouse_init(&am, g_cfg->lpf_alpha, g_cfg->mouse_scale);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	/* No SA_RESTART: a pending read returns EINTR so the loop exits. */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	if (do_cube)
		cube_init();

	printf("Press Ctrl+C to stop...\n");
	for (;;) {
		unsigned counter;
		double dt, a_corr[3], g_corr[3];
		int accel[3], gyro[3], rv;

		/* --duration must fire even when the device goes quiet:
		 * evdev_read_frame() blocks, so give the read a poll()
		 * deadline instead of checking the clock only between
		 * frames.  Without this, a single injected frame (the e2e)
		 * is recorded in memory and the loop sits in read() until
		 * killed - the CSV file is only written after the loop
		 * exits, so the test would read back nothing. */
		if (duration > 0.0) {
			struct pollfd pfd;
			double wait_s = duration;
			int pr;

			if (have_t0) {
				struct timespec now;
				double elapsed;

				clock_gettime(CLOCK_MONOTONIC, &now);
				elapsed = (now.tv_sec - t0.tv_sec) +
					(now.tv_nsec - t0.tv_nsec) * 1e-9;
				wait_s = duration - elapsed;
			}
			if (wait_s <= 0.0)
				break;
			pfd.fd = dev.fd;
			pfd.events = POLLIN;
			pr = poll(&pfd, 1, (int)(wait_s * 1000.0) + 1);
			if (pr < 0) {
				if (errno == EINTR && g_stop)
					break;
			} else if (pr == 0) {
				/* the deadline fired, no data pending -
				 * break even if a signal raced the timeout */
				break;
			}
		}

		rv = evdev_read_frame(&dev, &counter, &dt, accel, gyro,
				      err, sizeof(err));
		if (rv < 0) {
			if (g_stop)
				break;
			fprintf(stderr, "lgmagic: %s\n", err);
			ret = 1;
			break;
		}
		if (rv == 0)
			break;	/* EOF */
		if (!have_t0) {
			clock_gettime(CLOCK_MONOTONIC, &t0);
			have_t0 = 1;
		}
		if (duration > 0.0) {
			struct timespec now;
			double elapsed;

			clock_gettime(CLOCK_MONOTONIC, &now);
			elapsed = (now.tv_sec - t0.tv_sec) +
				(now.tv_nsec - t0.tv_nsec) * 1e-9;
			if (elapsed >= duration)
				break;
		}

		if (csv_path) {
			if (nsamples == cap) {
				cap = cap ? cap * 2 : 1024;
				samples = realloc(samples, cap * sizeof(*samples));
				if (!samples) {
					fprintf(stderr, "lgmagic: out of "
						"memory\n");
					ret = 1;
					break;
				}
			}
			samples[nsamples].counter = counter;
			samples[nsamples].dt = dt;
			samples[nsamples].accel[0] = accel[0];
			samples[nsamples].accel[1] = accel[1];
			samples[nsamples].accel[2] = accel[2];
			samples[nsamples].gyro[0] = gyro[0];
			samples[nsamples].gyro[1] = gyro[1];
			samples[nsamples].gyro[2] = gyro[2];
			nsamples++;
			continue;
		}

		if (calib_path) {
			double filt_out[3];

			calib_apply(&cal, (double[3]){ accel[0], accel[1], accel[2] },
				    (double[3]){ gyro[0], gyro[1], gyro[2] },
				    a_corr, g_corr);
			if (do_mouse) {
				int dx, dy;

				airmouse_process(&am, g_corr, filt_out,
						 &dx, &dy);
				printf("dt=%.5fs | gyro_filt =[% .6g % .6g "
				       "% .6g]\n", dt ? dt : 0.0, filt_out[0],
				       filt_out[1], filt_out[2]);
				if (uinput_move(ufd, dx, dy) < 0)
					fprintf(stderr, "lgmagic: uinput "
						"write failed\n");
				printf("REL_X : %d REL_Y: %d\n", dx, dy);
			}
			if (do_ahrs && !isnan(dt)) {
				vec3 a = { a_corr[0], a_corr[1], a_corr[2] };
				double roll, pitch, yaw;

				if (!q_init) {
					quat q0 = accel_to_quat(a);

					madgwick_init(&mad, g_cfg->madgwick_beta,
						      50.0, q0);
					q_init = 1;
				}
				madgwick_update_imu(&mad, g_corr[0], g_corr[1],
						    g_corr[2], a_corr[0],
						    a_corr[1], a_corr[2]);
				if (do_cube)
					cube_render(&mad.q);
				quat_to_euler(mad.q, &roll, &pitch, &yaw);
				printf("Roll=%+.2f  Pitch=%+.2f  Yaw=%+.2f\n",
				       roll * 180.0 / M_PI,
				       pitch * 180.0 / M_PI,
				       yaw * 180.0 / M_PI);
				continue;
			}
			printf("dt=%.5fs | Accel=[% .6g % .6g % .6g] | "
			       "Gyro=[% .6g % .6g % .6g]\n", dt ? dt : 0.0,
			       a_corr[0], a_corr[1], a_corr[2],
			       g_corr[0], g_corr[1], g_corr[2]);
			continue;
		}

		printf("dt=%.5fs | Accel=[%d, %d, %d] | Gyro=[%d, %d, %d]\n",
		       dt ? dt : 0.0, accel[0], accel[1], accel[2],
		       gyro[0], gyro[1], gyro[2]);
	}

	if (csv_path && nsamples > 0) {
		if (csv_write(csv_path, samples, nsamples, err, sizeof(err)) < 0) {
			fprintf(stderr, "lgmagic: %s\n", err);
			ret = 1;
		} else {
			printf("Wrote %zu samples to %s\n", nsamples, csv_path);
		}
	}
	free(samples);
	if (do_cube)
		cube_shutdown();
	uinput_close(ufd);
	evdev_close(&dev);
	return ret;
}
