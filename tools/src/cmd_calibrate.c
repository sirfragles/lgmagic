/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cmd_calibrate.c - `lg-magic calibrate` subcommand (scripts/calibrate.py).
 *
 *   lg-magic calibrate CSV OUTPUT_JSON --accel   fit bias + 3x3 matrix
 *   lg-magic calibrate CSV OUTPUT_JSON --gyro    mean gyro bias
 *
 * Deliberate fix vs Python: a --gyro-only run writes an identity accel
 * correction instead of empty arrays (the empty arrays broke --ahrs; the
 * identity is a no-op).
 */
#include "calib.h"
#include "config.h"
#include "csv.h"
#include "lm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out)
{
	fputs("Usage: lg-magic calibrate CSV OUTPUT_JSON (--accel | --gyro)\n"
	      "\n"
	      "  CSV         IMU recording from 'lg-magic imu --csv'\n"
	      "  OUTPUT_JSON calibration file (accel bias/matrix, gyro bias/scale)\n"
	      "\n"
	      "  --accel     fit accelerometer bias + 3x3 matrix (Levenberg-\n"
	      "              Marquardt, at least 6 samples in different\n"
	      "              orientations; like scripts/calibrate.py)\n"
	      "  --gyro      mean gyroscope bias from static samples\n", out);
}

int cmd_calibrate(int argc, char **argv)
{
	const char *csv_path, *out_path;
	const char *pos[2];
	int do_accel = 0, do_gyro = 0;
	struct imu_sample *samples = NULL;
	long n;
	char err[256];
	struct calib c;
	int i, npos = 0;

	/* argv[0] is the subcommand name; flags may come before or after
	 * the two positional arguments. */
	if (argc >= 2 && (strcmp(argv[1], "--help") == 0 ||
			  strcmp(argv[1], "-h") == 0)) {
		usage(stdout);
		return 0;
	}
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--accel") == 0)
			do_accel = 1;
		else if (strcmp(argv[i], "--gyro") == 0)
			do_gyro = 1;
		else if (npos < 2)
			pos[npos++] = argv[i];
		else {
			fprintf(stderr, "lg-magic calibrate: unexpected "
				"argument '%s'\n", argv[i]);
			usage(stderr);
			return 1;
		}
	}
	if (npos != 2 || (!do_accel && !do_gyro)) {
		if (!do_accel && !do_gyro && npos == 2)
			puts("You need to select --accel or --gyro");
		else
			usage(stderr);
		return 1;
	}
	csv_path = pos[0];
	out_path = pos[1];

	n = csv_read(csv_path, &samples, err, sizeof(err));
	if (n < 0) {
		fprintf(stderr, "Error reading CSV: %s\n", err);
		return 1;
	}

	calib_init_identity(&c);
	if (do_accel) {
		double (*a)[3];
		double cost;

		if (n < 6) {
			fprintf(stderr, "Error reading CSV: Need at least 6 "
				"samples in different orientations\n");
			free(samples);
			return 1;
		}
		a = malloc((size_t)n * sizeof(*a));
		if (!a) {
			fprintf(stderr, "lg-magic: out of memory\n");
			free(samples);
			return 1;
		}
		for (i = 0; i < n; i++)
			memcpy(a[i], samples[i].accel, sizeof(a[i]));
		lm_fit_accel(a, (size_t)n, c.accel_bias, c.accel_matrix, &cost);
		free(a);
		fprintf(stderr, "mean squared residual: %.6g\n", cost);
	} else {
		/* mean gyro bias (calibrate_gyro_bias) */
		for (i = 0; i < n; i++) {
			c.gyro_bias[0] += samples[i].gyro[0];
			c.gyro_bias[1] += samples[i].gyro[1];
			c.gyro_bias[2] += samples[i].gyro[2];
		}
		if (n > 0) {
			c.gyro_bias[0] /= n;
			c.gyro_bias[1] /= n;
			c.gyro_bias[2] /= n;
		}
	}
	free(samples);

	if (calib_save_json(&c, out_path, err, sizeof(err)) < 0) {
		fprintf(stderr, "lg-magic: %s\n", err);
		return 1;
	}
	printf("Calibration saved to %s\n", out_path);
	return 0;
}
