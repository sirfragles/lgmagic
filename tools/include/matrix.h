/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * matrix.h - vec3 / mat3 / quaternion helpers for the lgmagic tools.
 *
 * Quaternion convention is [w, x, y, z], matching the Python `ahrs`
 * library used by the original scripts (scripts/display_imu.py).
 *
 * This file is part of lgmagic (the C port of the Python scripts).
 */
#ifndef LG_TOOLS_MATRIX_H
#define LG_TOOLS_MATRIX_H

/* Double precision throughout: the Python scripts computed everything in
 * float64 (numpy), and the golden-trace parity tests compare against them. */

typedef struct {
	double x, y, z;
} vec3;

typedef struct {
	double m[3][3];		/* m[row][col] */
} mat3;

typedef struct {
	double w, x, y, z;	/* scalar first */
} quat;

vec3 vec3_add(vec3 a, vec3 b);
vec3 vec3_sub(vec3 a, vec3 b);
vec3 vec3_scale(vec3 a, double s);
double vec3_dot(vec3 a, vec3 b);
double vec3_norm(vec3 a);
vec3 vec3_normalized(vec3 a);

mat3 mat3_identity(void);
vec3 mat3_mul_vec3(const mat3 *m, vec3 v);
mat3 mat3_mul_mat3(const mat3 *a, const mat3 *b);

quat quat_normalized(quat q);
mat3 quat_to_mat3(quat q);
/* Roll/Pitch/Yaw in radians, same convention as
 * ahrs.common.orientation.q2euler from the Python `ahrs` library. */
void quat_to_euler(quat q, double *roll, double *pitch, double *yaw);
/* Initial orientation from the accelerometer only (yaw = 0).
 * Exact port of accel_to_quat() from scripts/display_imu.py. */
quat accel_to_quat(vec3 a);

#endif /* LG_TOOLS_MATRIX_H */
