/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * test_cube_math.c - unit tests for the cube projection math (cube.h),
 * i.e. cube_project(), which is the only geometry the cube exposes.
 *
 * The projection (see cube.c) is a pinhole camera at distance CUBE_D = 5
 * looking along +z, screen center at (cols/2, rows/2), horizontal scale
 * 0.2*cols, and a 2:1 cell aspect ratio (vertical offsets halved).
 * Rather than pinning the private constants, the tests check properties
 * that follow from them:
 *
 *  - the cube origin projects to the exact grid center at depth 5;
 *  - a vertex and its opposite on the z = 0 plane are screen-mirrored
 *    about the center (same depth for both, so the sums are exact);
 *  - the 2:1 cell aspect: for v = (1, 1, 0) the horizontal offset from
 *    the center is exactly twice the vertical one;
 *  - rotations move vertices as the quaternion rotation says: the
 *    projection of Rz(90 deg) * (1,0,0) equals the projection of (0,1,0)
 *    at identity, and Rx(90 deg) * (0,0,1) equals (0,-1,0) at identity;
 *  - a vertex rotated toward the camera moves closer (pz decreases).
 *
 * Quaternions here are built from cos/sin, exactly like matrix.h users do.
 */
#include <stdio.h>
#include <math.h>

#include "test_util.h"
#include "cube.h"

#define ROWS 24
#define COLS 80

static quat axis_quat(char axis, double deg)
{
	double r = deg * M_PI / 180.0;

	switch (axis) {
	case 'x':
		return (quat){ cos(r / 2.0), sin(r / 2.0), 0.0, 0.0 };
	case 'y':
		return (quat){ cos(r / 2.0), 0.0, sin(r / 2.0), 0.0 };
	default:
		return (quat){ cos(r / 2.0), 0.0, 0.0, sin(r / 2.0) };
	}
}

int main(void)
{
	quat q0 = { 1.0, 0.0, 0.0, 0.0 };
	quat qy90 = axis_quat('y', 90.0);
	quat qz90 = axis_quat('z', 90.0);
	quat qx90 = axis_quat('x', 90.0);
	quat qz40 = axis_quat('z', 40.0);
	float px, py, pz;

	TEST_BEGIN();

	/* origin -> exact grid center, camera distance */
	cube_project(&q0, 0.0f, 0.0f, 0.0f, ROWS, COLS, &px, &py, &pz);
	CHECK_FLOAT_EQ(px, COLS / 2.0f, "origin projects to the horizontal "
		       "grid center");
	CHECK_FLOAT_EQ(py, ROWS / 2.0f, "origin projects to the vertical "
		       "grid center");
	CHECK_FLOAT_EQ(pz, 5.0f, "origin is at the camera distance (pz=5)");

	/* 2:1 cell aspect for v = (1,1,0): |dx| == 2*|dy| */
	cube_project(&q0, 1.0f, 1.0f, 0.0f, ROWS, COLS, &px, &py, &pz);
	{
		float dx = px - COLS / 2.0f;
		float dy = ROWS / 2.0f - py;

		CHECK(dx > 0.0f && dy > 0.0f,
		      "+x vertex is right of center, +y vertex is above");
		CHECK_FLOAT_NEAR(dx, 2.0f * dy, 1e-3f,
				 "horizontal offset is twice the vertical "
				 "(2:1 cell aspect)");
	}

	/* opposite vertices on the z = 0 plane mirror about the center */
	{
		float px2, py2, pz2;

		cube_project(&q0, -1.0f, -1.0f, 0.0f, ROWS, COLS, &px2, &py2,
			     &pz2);
		CHECK_FLOAT_NEAR(px + px2, (float)COLS, 1e-3f,
				 "opposite z=0 vertices mirror in x");
		CHECK_FLOAT_NEAR(py + py2, (float)ROWS, 1e-3f,
				 "opposite z=0 vertices mirror in y");
		CHECK_FLOAT_NEAR(pz, pz2, 1e-3f,
				 "opposite z=0 vertices share the depth");

		/* the same holds under any yaw rotation (z components stay
		 * zero, so both depths stay equal) */
		cube_project(&qz40, 1.0f, 1.0f, 0.0f, ROWS, COLS, &px, &py,
			     &pz);
		cube_project(&qz40, -1.0f, -1.0f, 0.0f, ROWS, COLS, &px2, &py2,
			     &pz2);
		CHECK_FLOAT_NEAR(px + px2, (float)COLS, 1e-3f,
				 "opposite-vertex symmetry holds under yaw");
		CHECK_FLOAT_NEAR(py + py2, (float)ROWS, 1e-3f,
				 "opposite-vertex symmetry holds under yaw "
				 "(y)");
	}

	/* rotation semantics: Rz(90)*x_axis == y_axis, Rx(90)*z_axis == -y */
	{
		float rx, ry, rz;	/* reference projections at identity */
		float tx, ty, tz;

		cube_project(&q0, 0.0f, 1.0f, 0.0f, ROWS, COLS, &rx, &ry, &rz);
		cube_project(&qz90, 1.0f, 0.0f, 0.0f, ROWS, COLS, &tx, &ty,
			     &tz);
		CHECK_FLOAT_NEAR(tx, rx, 1e-3f,
				 "qz(90)*(1,0,0) projects like (0,1,0) (x)");
		CHECK_FLOAT_NEAR(ty, ry, 1e-3f,
				 "qz(90)*(1,0,0) projects like (0,1,0) (y)");
		CHECK_FLOAT_NEAR(tz, rz, 1e-3f,
				 "qz(90)*(1,0,0) keeps the depth (z)");

		cube_project(&q0, 0.0f, -1.0f, 0.0f, ROWS, COLS, &rx, &ry,
			     &rz);
		cube_project(&qx90, 0.0f, 0.0f, 1.0f, ROWS, COLS, &tx, &ty,
			     &tz);
		CHECK_FLOAT_NEAR(tx, rx, 1e-3f,
				 "qx(90)*(0,0,1) projects like (0,-1,0) (x)");
		CHECK_FLOAT_NEAR(ty, ry, 1e-3f,
				 "qx(90)*(0,0,1) projects like (0,-1,0) (y)");
		CHECK_FLOAT_NEAR(tz, rz, 1e-3f,
				 "qx(90)*(0,0,1) keeps the depth (z)");
	}

	/* depth: qx(90)*(0,1,0) = (0,0,1) swings the vertex away from the
	 * camera (pz grows), and (0,-1,0) -> (0,0,-1) toward it */
	cube_project(&q0, 0.0f, 1.0f, 0.0f, ROWS, COLS, &px, &py, &pz);
	cube_project(&qx90, 0.0f, 1.0f, 0.0f, ROWS, COLS, &px, &py, &pz);
	CHECK_FLOAT_NEAR(pz, 6.0f, 1e-3f,
			 "qx(90) swings +y to +z: depth grows to 6");
	cube_project(&qx90, 0.0f, -1.0f, 0.0f, ROWS, COLS, &px, &py, &pz);
	CHECK_FLOAT_NEAR(pz, 4.0f, 1e-3f,
			 "qx(90) swings -y to -z: depth shrinks to 4");

	/* sanity: qy(90)*(1,0,0) = (0,0,-1) - checked via depth only */
	cube_project(&qy90, 1.0f, 0.0f, 0.0f, ROWS, COLS, &px, &py, &pz);
	CHECK_FLOAT_NEAR(pz, 4.0f, 1e-3f,
			 "qy(90)*(1,0,0) ends up nearest the camera (pz=4)");

	TEST_SUMMARY("test_cube_math");
	return tu_fail_count ? 1 : 0;
}
