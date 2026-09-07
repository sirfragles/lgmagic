/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * matrix.c - vec3 / mat3 / quaternion helpers for the lgmagic tools.
 *
 * Double precision throughout: the Python scripts computed everything in
 * float64 (numpy / ahrs), and the golden-trace parity tests compare the C
 * port against them to 1e-6.
 *
 * The quaternion convention is [w, x, y, z], matching the Python `ahrs`
 * library used by the original scripts.
 */
#include "matrix.h"

#include <math.h>

vec3 vec3_add(vec3 a, vec3 b)
{
	return (vec3){ a.x + b.x, a.y + b.y, a.z + b.z };
}

vec3 vec3_sub(vec3 a, vec3 b)
{
	return (vec3){ a.x - b.x, a.y - b.y, a.z - b.z };
}

vec3 vec3_scale(vec3 a, double s)
{
	return (vec3){ a.x * s, a.y * s, a.z * s };
}

double vec3_dot(vec3 a, vec3 b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

double vec3_norm(vec3 a)
{
	/* Same evaluation order as numpy.linalg.norm for length-3 vectors:
	 * sqrt(((x*x + y*y) + z*z)). */
	return sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}

vec3 vec3_normalized(vec3 a)
{
	double n = vec3_norm(a);

	if (n == 0.0)
		return (vec3){ 0.0, 0.0, 0.0 };
	return vec3_scale(a, 1.0 / n);
}

mat3 mat3_identity(void)
{
	mat3 r;

	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			r.m[i][j] = (i == j) ? 1.0 : 0.0;
	return r;
}

vec3 mat3_mul_vec3(const mat3 *m, vec3 v)
{
	vec3 r;

	r.x = m->m[0][0] * v.x + m->m[0][1] * v.y + m->m[0][2] * v.z;
	r.y = m->m[1][0] * v.x + m->m[1][1] * v.y + m->m[1][2] * v.z;
	r.z = m->m[2][0] * v.x + m->m[2][1] * v.y + m->m[2][2] * v.z;
	return r;
}

mat3 mat3_mul_mat3(const mat3 *a, const mat3 *b)
{
	mat3 r;

	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			r.m[i][j] = 0.0;
			for (int k = 0; k < 3; k++)
				r.m[i][j] += a->m[i][k] * b->m[k][j];
		}
	return r;
}

quat quat_normalized(quat q)
{
	double n = sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);

	if (n == 0.0)
		return (quat){ 1.0, 0.0, 0.0, 0.0 };
	return (quat){ q.w / n, q.x / n, q.y / n, q.z / n };
}

mat3 quat_to_mat3(quat q)
{
	/* Same formula as Quaternion.to_DCM() from the Python `ahrs` library. */
	double w = q.w, x = q.x, y = q.y, z = q.z;
	mat3 r;

	r.m[0][0] = 1.0 - 2.0 * (y * y + z * z);
	r.m[0][1] = 2.0 * (x * y - w * z);
	r.m[0][2] = 2.0 * (x * z + w * y);
	r.m[1][0] = 2.0 * (x * y + w * z);
	r.m[1][1] = 1.0 - 2.0 * (x * x + z * z);
	r.m[1][2] = 2.0 * (y * z - w * x);
	r.m[2][0] = 2.0 * (x * z - w * y);
	r.m[2][1] = 2.0 * (w * x + y * z);
	r.m[2][2] = 1.0 - 2.0 * (x * x + y * y);
	return r;
}

void quat_to_euler(quat q, double *roll, double *pitch, double *yaw)
{
	/*
	 * Exact port of ahrs.common.orientation.q2euler() from the Python
	 * `ahrs` library, including its identity shortcut: the check is
	 * sum([1,0,0,0] - q) == 0.0, i.e. it uses the *sum* of the elementwise
	 * differences, not per-component equality. We reproduce it verbatim
	 * (numpy sums a length-4 array pairwise: ((a+b)+(c+d))).
	 */
	if ((((1.0 - q.w) - q.x) + (-q.y - q.z)) == 0.0) {
		*roll = 0.0;
		*pitch = 0.0;
		*yaw = 0.0;
		return;
	}
	{
		double r00 = 2.0 * q.w * q.w - 1.0 + 2.0 * q.x * q.x;
		double r10 = 2.0 * (q.x * q.y - q.w * q.z);
		double r20 = 2.0 * (q.x * q.z + q.w * q.y);
		double r21 = 2.0 * (q.y * q.z - q.w * q.x);
		double r22 = 2.0 * q.w * q.w - 1.0 + 2.0 * q.z * q.z;
		double d = 1.0 - r20 * r20;

		if (d < 0.0)
			d = 0.0;	/* defensive; unit quaternions stay within range */
		*roll = atan2(r21, r22);
		*pitch = -atan(r20 / sqrt(d));
		*yaw = atan2(r10, r00);
	}
}

quat accel_to_quat(vec3 a)
{
	/* Exact port of accel_to_quat() from scripts/display_imu.py. */
	double n = vec3_norm(a);
	double ax = a.x / n, ay = a.y / n, az = a.z / n;
	double roll = atan2(ay, az);
	double pitch = atan2(-ax, sqrt(ay * ay + az * az));
	/* yaw = 0 */
	double cr = cos(roll / 2.0), sr = sin(roll / 2.0);
	double cp = cos(pitch / 2.0), sp = sin(pitch / 2.0);

	return (quat){ cr * cp, sr * cp, cr * sp, -sr * sp };
}
