/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * test_matrix.c - unit tests for matrix.h (vec3/mat3/quaternion math).
 *
 * The conventions under test are pinned to the Python `ahrs` library, which
 * the C port mirrors and which the original scripts (scripts/display_imu.py)
 * used:
 *
 *   - quat_to_mat3(q) == ahrs Quaternion.to_DCM(q)  (the standard active
 *     quaternion rotation matrix; v' = q v q*):
 *         R[0] = [1-2(y^2+z^2), 2(xy-wz),   2(xz+wy)]
 *         R[1] = [2(xy+wz),     1-2(x^2+z^2), 2(yz-wx)]
 *         R[2] = [2(xz-wy),     2(yz+wx),    1-2(x^2+y^2)]
 *   - quat_to_euler(q) == ahrs common.orientation.q2euler(q), including its
 *     identity shortcut, computed here literally:
 *         R_00 = 2w^2 - 1 + 2x^2,  R_10 = 2(xy - wz)
 *         R_20 = 2(xz + wy),        R_21 = 2(yz - wx)
 *         R_22 = 2w^2 - 1 + 2z^2
 *         roll = atan2(R_21, R_22)
 *         pitch = -atan(R_20 / sqrt(max(0, 1 - R_20^2)))
 *         yaw  = atan2(R_10, R_00)
 *   - accel_to_quat(a) == the display_imu.py construction
 *         roll = atan2(ay, az); pitch = atan2(-ax, sqrt(ay^2+az^2));
 *         q = qz(0) * qy(pitch) * qx(roll)   (Hamilton product).
 *
 * The reference implementations of all three are re-derived from those
 * sources inside this file, never via the library under test.
 */
#include <stdio.h>
#include <math.h>

#include "test_util.h"
#include "matrix.h"

/* ------------------------------------------------------------------ */
/* Reference implementations (independent of the code under test).    */
/* ------------------------------------------------------------------ */

/* ahrs Quaternion.to_DCM(). */
static mat3 ref_dcm(quat q)
{
	double w = q.w, x = q.x, y = q.y, z = q.z;
	mat3 m;

	m.m[0][0] = 1 - 2 * (y * y + z * z);
	m.m[0][1] = 2 * (x * y - w * z);
	m.m[0][2] = 2 * (x * z + w * y);
	m.m[1][0] = 2 * (x * y + w * z);
	m.m[1][1] = 1 - 2 * (x * x + z * z);
	m.m[1][2] = 2 * (y * z - w * x);
	m.m[2][0] = 2 * (x * z - w * y);
	m.m[2][1] = 2 * (y * z + w * x);
	m.m[2][2] = 1 - 2 * (x * x + y * y);
	return m;
}

/* ahrs common.orientation.q2euler(), formulas written literally. */
static void ref_euler(quat q, double *roll, double *pitch, double *yaw)
{
	double r00, r10, r20, r21, r22, denom;

	if (q.w == 1.0 && q.x == 0.0 && q.y == 0.0 && q.z == 0.0) {
		*roll = *pitch = *yaw = 0.0;
		return;
	}
	r00 = 2 * q.w * q.w - 1.0 + 2 * q.x * q.x;
	r10 = 2 * (q.x * q.y - q.w * q.z);
	r20 = 2 * (q.x * q.z + q.w * q.y);
	r21 = 2 * (q.y * q.z - q.w * q.x);
	r22 = 2 * q.w * q.w - 1.0 + 2 * q.z * q.z;
	denom = 1.0 - r20 * r20;
	if (denom < 0.0)	/* domain edge: clamp, per ahrs' formula */
		denom = 0.0;
	*roll = atan2(r21, r22);
	*pitch = -atan(r20 / sqrt(denom));
	*yaw = atan2(r10, r00);
}

/* display_imu.py accel_to_quat() - literal port of the Python code. */
static quat ref_accel_to_quat(vec3 a)
{
	double ax, ay, az;
	double roll, pitch;
	double cy, sy, cp, sp, cr, sr;
	quat q;
	double n = sqrt(a.x * a.x + a.y * a.y + a.z * a.z);

	ax = a.x / n;
	ay = a.y / n;
	az = a.z / n;
	roll = atan2(ay, az);
	pitch = atan2(-ax, sqrt(ay * ay + az * az));
	/* yaw unresolvable -> 0 */
	cy = cos(0.0 / 2.0);
	sy = sin(0.0 / 2.0);
	cp = cos(pitch / 2.0);
	sp = sin(pitch / 2.0);
	cr = cos(roll / 2.0);
	sr = sin(roll / 2.0);
	q.w = cr * cp * cy + sr * sp * sy;
	q.x = sr * cp * cy - cr * sp * sy;
	q.y = cr * sp * cy + sr * cp * sy;
	q.z = cr * cp * sy - sr * sp * cy;
	return q;
}

/* Hamilton product, composed in the test itself. */
static quat ref_qmul(quat a, quat b)
{
	quat r;

	r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
	r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
	r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
	r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
	return r;
}

static quat axis_quat(char axis, double ang)
{
	quat q;

	q.w = cos(ang / 2.0);
	q.x = q.y = q.z = 0.0;
	if (axis == 'x')
		q.x = sin(ang / 2.0);
	else if (axis == 'y')
		q.y = sin(ang / 2.0);
	else
		q.z = sin(ang / 2.0);
	return q;
}

static double wrap_pi(double a)
{
	while (a > M_PI)
		a -= 2 * M_PI;
	while (a < -M_PI)
		a += 2 * M_PI;
	return a;
}

/* Angular difference wrapped into [-pi, pi]. */
static double angle_diff(double a, double b)
{
	return wrap_pi(a - b);
}

static double mat3_maxdiff(const mat3 *a, const mat3 *b)
{
	double worst = 0.0;
	int i, j;

	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++) {
			double d = fabs(a->m[i][j] - b->m[i][j]);
			if (d > worst)
				worst = d;
		}
	return worst;
}

/* ------------------------------------------------------------------ */
/* Deterministic PRNG (LCG + Box-Muller) for the random-quat cases.   */
/* ------------------------------------------------------------------ */

static unsigned long long tu_seed = 0x9E3779B97F4A7C15ULL;

static double tu_rand01(void)
{
	tu_seed = tu_seed * 6364136223846793005ULL + 1442695040888963407ULL;
	return (double)((tu_seed >> 11) & 0x1FFFFFFFFFFFFFULL) /
	       (double)0x20000000000000ULL;	/* [0, 1) */
}

/* Random unit quaternion, uniform on S^3 (4 iid normals, normalized). */
static quat tu_rand_quat(void)
{
	double a = sqrt(-2.0 * log(tu_rand01() + 1e-300));
	double b = 2.0 * M_PI * tu_rand01();
	double c = sqrt(-2.0 * log(tu_rand01() + 1e-300));
	double d = 2.0 * M_PI * tu_rand01();
	quat q;
	double s;

	q.w = a * cos(b);
	q.x = a * sin(b);
	q.y = c * cos(d);
	q.z = c * sin(d);
	s = sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
	q.w /= s;
	q.x /= s;
	q.y /= s;
	q.z /= s;
	return q;
}

/* ------------------------------------------------------------------ */
/* Cases                                                              */
/* ------------------------------------------------------------------ */

static void test_quat_to_mat3_known_rotations(void)
{
	static const double lit_qx90[3][3] = {
		{ 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 }
	};
	static const double lit_qy90[3][3] = {
		{ 0, 0, 1 }, { 0, 1, 0 }, { -1, 0, 0 }
	};
	static const double lit_qz90[3][3] = {
		{ 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 }
	};
	static const double lit_qx180[3][3] = {
		{ 1, 0, 0 }, { 0, -1, 0 }, { 0, 0, -1 }
	};
	mat3 got, want;
	quat q;
	int i, j;
	double worst = 0.0;
	const double PI_2 = M_PI / 2.0;

	/* identity */
	q.w = 1.0; q.x = q.y = q.z = 0.0;
	got = quat_to_mat3(q);
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			want.m[i][j] = (i == j) ? 1.0 : 0.0;
	worst = mat3_maxdiff(&got, &want);
	CHECK(worst <= 1e-12, "quat_to_mat3(identity) == identity matrix");

	/* +90 deg about each axis, literal expected matrices */
	got = quat_to_mat3(axis_quat('x', PI_2));
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			want.m[i][j] = lit_qx90[i][j];
	CHECK(mat3_maxdiff(&got, &want) <= 1e-12,
	      "quat_to_mat3(qx(90deg)) == R_x(90deg)");
	got = quat_to_mat3(axis_quat('y', PI_2));
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			want.m[i][j] = lit_qy90[i][j];
	CHECK(mat3_maxdiff(&got, &want) <= 1e-12,
	      "quat_to_mat3(qy(90deg)) == R_y(90deg)");
	got = quat_to_mat3(axis_quat('z', PI_2));
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			want.m[i][j] = lit_qz90[i][j];
	CHECK(mat3_maxdiff(&got, &want) <= 1e-12,
	      "quat_to_mat3(qz(90deg)) == R_z(90deg)");

	/* 180 deg about X */
	got = quat_to_mat3(axis_quat('x', M_PI));
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			want.m[i][j] = lit_qx180[i][j];
	CHECK(mat3_maxdiff(&got, &want) <= 1e-12,
	      "quat_to_mat3(qx(180deg)) == diag(1,-1,-1)");

	/* random quats vs the ahrs to_DCM reference */
	{
		int iter;
		worst = 0.0;
		for (iter = 0; iter < 300; iter++) {
			q = tu_rand_quat();
			got = quat_to_mat3(q);
			want = ref_dcm(q);
			worst = fmax(worst, mat3_maxdiff(&got, &want));
		}
		printf("  (quat_to_mat3 vs to_DCM, 300 random quats: worst %.3g)\n",
		       worst);
		CHECK(worst <= 1e-12,
		      "quat_to_mat3 matches ahrs to_DCM on 300 random quats");
	}
}

static void test_quat_multiplication_order(void)
{
	/* to_DCM is multiplicative: M(q1*q2) == M(q1) * M(q2). */
	quat q1 = axis_quat('x', 50.0 * M_PI / 180.0);
	quat q2 = axis_quat('z', 20.0 * M_PI / 180.0);
	quat q3 = axis_quat('y', -35.0 * M_PI / 180.0);
	quat prod;
	mat3 m1 = ref_dcm(q1), m2 = ref_dcm(q2), m3 = ref_dcm(q3);
	mat3 got, want, tmp;
	double worst = 0.0;
	int i, j, k;

	prod = ref_qmul(ref_qmul(q1, q2), q3);
	got = quat_to_mat3(prod);
	/* compose the reference product M1*M2*M3 one step at a time, through a
	 * separate matrix - never reading and writing want in the same pass */
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++) {
			double s = 0.0;
			for (k = 0; k < 3; k++)
				s += m1.m[i][k] * m2.m[k][j];
			tmp.m[i][j] = s;
		}
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++) {
			double s = 0.0;
			for (k = 0; k < 3; k++)
				s += tmp.m[i][k] * m3.m[k][j];
			want.m[i][j] = s;
		}
	worst = mat3_maxdiff(&got, &want);
	printf("  (combined quat rotation matrix diff %.3g)\n", worst);
	CHECK(worst <= 1e-12,
	      "quat_to_mat3(qx*qz*qy) == mat3_mul_mat3 products in order");

	/* the same identity holds when mat3_mul_mat3 itself is used */
	{
		mat3 step;
		mat3 step2 = mat3_mul_mat3(&m1, &m2);
		step = mat3_mul_mat3(&step2, &m3);
		worst = mat3_maxdiff(&got, &step);
		CHECK(worst <= 1e-12,
		      "mat3_mul_mat3 matches hand-composed matrix product");
	}
	/* and in the reverse quaternion order (commuted pair) */
	{
		quat prod2 = ref_qmul(q2, q1);
		mat3 want2;
		got = quat_to_mat3(prod2);
		want2 = mat3_mul_mat3(&m2, &m1);
		worst = mat3_maxdiff(&got, &want2);
		CHECK(worst <= 1e-12,
		      "quat_to_mat3(qz*qx) == mat3_mul_mat3(Mz, Mx)");
	}
}

static void test_mat3_vec3_ops(void)
{
	quat q;
	mat3 m;
	vec3 v, got;
	int i;
	double worst;

	/* identity rotation leaves vectors alone */
	m = quat_to_mat3(axis_quat('x', 0.0));
	v.x = 1.0; v.y = -2.0; v.z = 3.5;
	got = mat3_mul_vec3(&m, v);
	CHECK(fabs(got.x - v.x) <= 1e-12 && fabs(got.y - v.y) <= 1e-12 &&
	      fabs(got.z - v.z) <= 1e-12, "mat3_mul_vec3 with identity matrix");

	/* Rx(90): +y -> +z, +z -> -y (the to_DCM convention) */
	m = quat_to_mat3(axis_quat('x', M_PI / 2.0));
	v.x = 0.0; v.y = 1.0; v.z = 0.0;
	got = mat3_mul_vec3(&m, v);
	CHECK(fabs(got.x) <= 1e-12 && fabs(got.y) <= 1e-12 &&
	      fabs(got.z - 1.0) <= 1e-12, "Rx(90) maps +y onto +z");
	v.x = 0.0; v.y = 0.0; v.z = 1.0;
	got = mat3_mul_vec3(&m, v);
	CHECK(fabs(got.x) <= 1e-12 && fabs(got.y + 1.0) <= 1e-12 &&
	      fabs(got.z) <= 1e-12, "Rx(90) maps +z onto -y");

	/* (A*B)*v == A*(B*v) */
	{
		mat3 a = ref_dcm(axis_quat('y', 0.7));
		mat3 b = ref_dcm(axis_quat('z', -1.1));
		mat3 ab = mat3_mul_mat3(&a, &b);
		vec3 w;
		w.x = 0.3; w.y = -1.7; w.z = 2.2;
		got = mat3_mul_vec3(&ab, w);
		{
			vec3 step = mat3_mul_vec3(&b, w);
			vec3 ref = mat3_mul_vec3(&a, step);
			worst = fmax(fabs(got.x - ref.x),
				     fmax(fabs(got.y - ref.y),
					  fabs(got.z - ref.z)));
		}
		CHECK(worst <= 1e-12, "(A*B)*v == A*(B*v)");
	}

	/* conjugation: M(conj(q)) == transpose(M(q)) */
	{
		double worst2 = 0.0;
		for (i = 0; i < 100; i++) {
			quat qr = tu_rand_quat();
			quat qc = qr;
			mat3 mr, mc;
			int ii, jj;
			qc.x = -qc.x; qc.y = -qc.y; qc.z = -qc.z;
			mr = quat_to_mat3(qr);
			mc = quat_to_mat3(qc);
			for (ii = 0; ii < 3; ii++)
				for (jj = 0; jj < 3; jj++)
					worst2 = fmax(worst2,
						      fabs(mr.m[ii][jj] -
							   mc.m[jj][ii]));
		}
		CHECK(worst2 <= 1e-12, "M(q*) == transpose(M(q)) on 100 quats");
	}

	/* orthonormality of quat_to_mat3 output */
	{
		mat3 mr;
		double worst2 = 0.0;
		int ii, jj;
		q = tu_rand_quat();
		mr = quat_to_mat3(q);
		for (ii = 0; ii < 3; ii++)
			for (jj = 0; jj < 3; jj++) {
				double d = 0.0;
				int k;
				for (k = 0; k < 3; k++)
					d += mr.m[k][ii] * mr.m[k][jj];
				worst2 = fmax(worst2,
					      fabs(d - (ii == jj ? 1.0 : 0.0)));
			}
		CHECK(worst2 <= 1e-12, "quat_to_mat3(q) rows orthonormal");
	}
}

static void test_vec3_helpers(void)
{
	vec3 a = { 1.0, 2.0, 2.0 };	/* norm 3 */
	vec3 b = { -4.0, 0.5, 1.0 };
	vec3 s;

	s = vec3_add(a, b);
	CHECK(fabs(s.x + 3.0) <= 1e-12 && fabs(s.y - 2.5) <= 1e-12 &&
	      fabs(s.z - 3.0) <= 1e-12, "vec3_add");
	s = vec3_sub(a, b);
	CHECK(fabs(s.x - 5.0) <= 1e-12 && fabs(s.y - 1.5) <= 1e-12 &&
	      fabs(s.z - 1.0) <= 1e-12, "vec3_sub");
	s = vec3_scale(a, 0.5);
	CHECK(fabs(s.x - 0.5) <= 1e-12 && fabs(s.y - 1.0) <= 1e-12 &&
	      fabs(s.z - 1.0) <= 1e-12, "vec3_scale");
	CHECK(fabs(vec3_dot(a, b) - (-4.0 + 1.0 + 2.0)) <= 1e-12, "vec3_dot");
	CHECK(fabs(vec3_norm(a) - 3.0) <= 1e-12, "vec3_norm");
	s = vec3_normalized(a);
	CHECK(fabs(vec3_norm(s) - 1.0) <= 1e-12 &&
	      fabs(s.x - 1.0 / 3.0) <= 1e-12 &&
	      fabs(s.y - 2.0 / 3.0) <= 1e-12 &&
	      fabs(s.z - 2.0 / 3.0) <= 1e-12, "vec3_normalized");
}

static void test_quat_normalized(void)
{
	quat q = { 2.0, 3.0, 4.0, 5.0 };
	double n = sqrt(4.0 + 9.0 + 16.0 + 25.0);
	quat r = quat_normalized(q);

	CHECK(fabs(r.w * r.w + r.x * r.x + r.y * r.y + r.z * r.z - 1.0)
	      <= 1e-12, "quat_normalized returns a unit quaternion");
	CHECK(fabs(r.w - 2.0 / n) <= 1e-12 && fabs(r.x - 3.0 / n) <= 1e-12 &&
	      fabs(r.y - 4.0 / n) <= 1e-12 && fabs(r.z - 5.0 / n) <= 1e-12,
	      "quat_normalized preserves direction");
}

static void test_quat_to_euler(void)
{
	double roll, pitch, yaw, rr, pp, yy;
	quat q;

	/* identity */
	q.w = 1.0; q.x = q.y = q.z = 0.0;
	quat_to_euler(q, &roll, &pitch, &yaw);
	CHECK(fabs(roll) <= 1e-12 && fabs(pitch) <= 1e-12 &&
	      fabs(yaw) <= 1e-12, "quat_to_euler(identity) == (0,0,0)");

	/* pure axis rotations at exact closed-form angles (ahrs semantics:
	 * the axis-angle quaternion qx(th) comes out as roll = -th) */
	{
		const double TH = 0.3;
		q = axis_quat('x', TH);
		quat_to_euler(q, &roll, &pitch, &yaw);
		CHECK(fabs(roll + TH) <= 1e-9 && fabs(pitch) <= 1e-9 &&
		      fabs(yaw) <= 1e-9, "euler(qx(0.3)) == (-0.3, 0, 0)");
		q = axis_quat('y', TH);
		quat_to_euler(q, &roll, &pitch, &yaw);
		CHECK(fabs(roll) <= 1e-9 && fabs(pitch + TH) <= 1e-9 &&
		      fabs(yaw) <= 1e-9, "euler(qy(0.3)) == (0, -0.3, 0)");
		q = axis_quat('z', TH);
		quat_to_euler(q, &roll, &pitch, &yaw);
		CHECK(fabs(roll) <= 1e-9 && fabs(pitch) <= 1e-9 &&
		      fabs(yaw + TH) <= 1e-9, "euler(qz(0.3)) == (0, 0, -0.3)");
	}
	/* 90 degrees about each axis */
	{
		const double H = M_PI / 2.0;
		q = axis_quat('x', H);
		quat_to_euler(q, &roll, &pitch, &yaw);
		CHECK(fabs(roll + H) <= 1e-9 && fabs(pitch) <= 1e-9 &&
		      fabs(yaw) <= 1e-9, "euler(qx(90deg)) == (-90, 0, 0) deg");
		q = axis_quat('y', H);
		quat_to_euler(q, &roll, &pitch, &yaw);
		CHECK(fabs(roll) <= 1e-9 && fabs(pitch + H) <= 1e-9 &&
		      fabs(yaw) <= 1e-9, "euler(qy(90deg)) == (0, -90, 0) deg");
		q = axis_quat('z', H);
		quat_to_euler(q, &roll, &pitch, &yaw);
		CHECK(fabs(roll) <= 1e-9 && fabs(pitch) <= 1e-9 &&
		      fabs(yaw + H) <= 1e-9, "euler(qz(90deg)) == (0, 0, -90) deg");
	}
	/* 180 deg about x: roll is +/- pi; compare wrapped */
	{
		q = axis_quat('x', M_PI);
		quat_to_euler(q, &roll, &pitch, &yaw);
		CHECK(fabs(angle_diff(roll, M_PI)) <= 1e-9 &&
		      fabs(angle_diff(pitch, 0.0)) <= 1e-9 &&
		      fabs(angle_diff(yaw, 0.0)) <= 1e-9,
		      "euler(qx(180deg)) == (+-180, 0, 0) deg");
	}
	/* random quats: match the ahrs q2euler formulas literally */
	{
		int iter;
		double worst = 0.0;
		for (iter = 0; iter < 500; iter++) {
			q = tu_rand_quat();
			ref_euler(q, &rr, &pp, &yy);
			quat_to_euler(q, &roll, &pitch, &yaw);
			worst = fmax(worst, fabs(angle_diff(roll, rr)));
			worst = fmax(worst, fabs(angle_diff(pitch, pp)));
			worst = fmax(worst, fabs(angle_diff(yaw, yy)));
			/* q and -q are the same orientation */
			{
				quat qn = q;
				double r2, p2, y2;
				qn.w = -qn.w; qn.x = -qn.x;
				qn.y = -qn.y; qn.z = -qn.z;
				quat_to_euler(qn, &r2, &p2, &y2);
				worst = fmax(worst, fabs(angle_diff(r2, roll)));
				worst = fmax(worst, fabs(angle_diff(p2, pitch)));
				worst = fmax(worst, fabs(angle_diff(y2, yaw)));
			}
		}
		printf("  (quat_to_euler vs q2euler formulas: worst %.3g rad)\n",
		       worst);
		CHECK(worst <= 1e-9,
		      "quat_to_euler matches ahrs q2euler on 500 random quats");
	}
	/* near the asin/atan domain edge (pitch near +-90 deg) */
	{
		double worst = 0.0;
		int i;
		static const double offs[] = { 1e-3, 1e-4, 1e-2 };
		for (i = 0; i < 3; i++) {
			q = axis_quat('y', M_PI / 2.0 - offs[i]);
			ref_euler(q, &rr, &pp, &yy);
			quat_to_euler(q, &roll, &pitch, &yaw);
			worst = fmax(worst, fabs(angle_diff(roll, rr)));
			worst = fmax(worst, fabs(angle_diff(pitch, pp)));
			worst = fmax(worst, fabs(angle_diff(yaw, yy)));
			q = axis_quat('y', -M_PI / 2.0 + offs[i]);
			ref_euler(q, &rr, &pp, &yy);
			quat_to_euler(q, &roll, &pitch, &yaw);
			worst = fmax(worst, fabs(angle_diff(roll, rr)));
			worst = fmax(worst, fabs(angle_diff(pitch, pp)));
			worst = fmax(worst, fabs(angle_diff(yaw, yy)));
		}
		printf("  (near-gimbal quat_to_euler: worst %.3g rad)\n", worst);
		CHECK(worst <= 1e-9,
		      "quat_to_euler near pitch = +-90 deg still matches");
	}
}

static void test_accel_to_quat(void)
{
	static const vec3 cases[] = {
		{ 0.0, 0.0, 9.80665 },
		{ 0.0, 9.8, 0.0 },
		{ 0.0, -9.8, 0.0 },
		{ 9.8, 0.0, 0.0 },
		{ -9.8, 0.0, 0.0 },
		{ 0.0, 0.0, -9.8 },
		{ 1.0, 2.0, 3.0 },
		{ 2.0, -3.0, 5.0 },
		{ -7.0, 1.0, -2.0 },
		{ 100.0, 1.0, 1.0 },
		{ 0.1, 0.2, -0.3 },
	};
	double worst_q = 0.0, worst_m = 0.0, worst_v = 0.0;
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		quat got = accel_to_quat(cases[i]);
		quat want = ref_accel_to_quat(cases[i]);
		mat3 gm, wm;
		vec3 n, img, imw;
		double nn;

		/* unit quaternion */
		nn = sqrt(got.w * got.w + got.x * got.x + got.y * got.y +
			  got.z * got.z);
		worst_q = fmax(worst_q, fabs(nn - 1.0));
		/* quaternion equality with the display_imu.py construction */
		worst_q = fmax(worst_q, fabs(got.w - want.w));
		worst_q = fmax(worst_q, fabs(got.x - want.x));
		worst_q = fmax(worst_q, fabs(got.y - want.y));
		worst_q = fmax(worst_q, fabs(got.z - want.z));

		/* its rotation matrix maps the acceleration direction onto the
		 * axis implied by roll=atan2(ay,az), pitch=atan2(-ax,...) */
		gm = quat_to_mat3(got);
		wm = ref_dcm(want);	/* == Rz(0)*Ry(pitch)*Rx(roll) */
		worst_m = fmax(worst_m, mat3_maxdiff(&gm, &wm));
		nn = sqrt(cases[i].x * cases[i].x + cases[i].y * cases[i].y +
			  cases[i].z * cases[i].z);
		n.x = cases[i].x / nn;
		n.y = cases[i].y / nn;
		n.z = cases[i].z / nn;
		img = mat3_mul_vec3(&gm, n);
		imw = mat3_mul_vec3(&wm, n);
		worst_v = fmax(worst_v, fabs(img.x - imw.x));
		worst_v = fmax(worst_v, fabs(img.y - imw.y));
		worst_v = fmax(worst_v, fabs(img.z - imw.z));
	}
	printf("  (accel_to_quat: worst quat diff %.3g, matrix diff %.3g, "
	       "mapped-vector diff %.3g)\n", worst_q, worst_m, worst_v);
	CHECK(worst_q <= 1e-12,
	      "accel_to_quat returns a unit quaternion equal to the "
	      "display_imu.py construction");
	CHECK(worst_m <= 1e-9,
	      "accel_to_quat rotation matrix matches the reference matrix "
	      "built from the roll/pitch formulas");
	CHECK(worst_v <= 1e-9,
	      "quat_to_mat3(accel_to_quat(a)) maps a onto the same axis as "
	      "the roll/pitch reference matrix");
}

int main(void)
{
	TEST_BEGIN();
	test_quat_to_mat3_known_rotations();
	test_quat_multiplication_order();
	test_mat3_vec3_ops();
	test_vec3_helpers();
	test_quat_normalized();
	test_quat_to_euler();
	test_accel_to_quat();
	TEST_SUMMARY("test_matrix");
	return tu_fail_count ? 1 : 0;
}
