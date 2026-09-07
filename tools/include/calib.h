/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * calib.h - calibration JSON load/save, sample correction, blob conversion.
 *
 * The JSON shape matches scripts/calibrate.py:
 *   {"accel": {"bias": [3], "matrix": [[3x3]]}, "gyro": {"bias": [3], "scale": [3]}}
 */
#ifndef LG_TOOLS_CALIB_H
#define LG_TOOLS_CALIB_H

#include <stddef.h>
#include <lg_magic_calib.h>	/* kernel/userspace-shared blob struct */

struct calib {
	double accel_bias[3];
	double accel_matrix[3][3];
	double gyro_bias[3];
	double gyro_scale[3];
};

/* Defaults: identity accel correction, zero gyro bias, unit gyro scale.
 * (Fixes a Python bug: a --gyro-only calibration wrote empty accel arrays,
 * which broke --ahrs. Identity here is a no-op for --mouse.) */
void calib_init_identity(struct calib *c);

/* Load a calibration JSON. gyro.scale defaults to [1,1,1] when missing.
 * Returns 0 on success, -1 on error (err filled). */
int calib_load(const char *path, struct calib *c, char *err, size_t errsz);

/* Apply the correction, in the same order as display_imu.py:
 *   a = R_align * M(a - b);  g = R_align * ((g - bias) * scale) * pi/180
 * i.e. the gyro output is already in rad/s. */
void calib_apply(const struct calib *c, const double a_raw[3], const double g_raw[3],
		 double a_out[3], double g_out[3]);

/* Write the calibration JSON (Python json.dump(indent=4) shape).
 * Returns 0 on success, -1 on error. */
int calib_save_json(const struct calib *c, const char *path, char *err, size_t errsz);

/* Convert to the 32-byte kernel firmware blob (alpha/mouse_k supplied). */
void calib_to_blob(const struct calib *c, float alpha, float mouse_k,
		   struct lg_magic_airmouse_calib *blob);
/* Same range checks as the kernel's lgmagic_validate_calib(); returns 0 if
 * valid, -1 otherwise (reason printed to stderr). */
int calib_validate_blob(const struct lg_magic_airmouse_calib *blob);

#endif /* LG_TOOLS_CALIB_H */
