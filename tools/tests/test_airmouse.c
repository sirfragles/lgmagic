/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * test_airmouse.c - golden tests for the extracted airmouse engine
 * (airmouse.h).
 *
 * The dx/dy values below are the exact v1 `imu --mouse` semantics:
 * per-axis LPF followed by dx = (int)(-filt[2] * scale),
 * dy = (int)(-filt[1] * scale) with C truncation towards zero.  The
 * integers are the contract (checked exactly); the filter outputs are
 * checked within 1e-12.
 */
#include <stdio.h>

#include "test_util.h"
#include "airmouse.h"

static void test_first_frame(void)
{
	struct airmouse am;
	double g[3] = { 0.0, 0.0, 0.0 }, out[3];
	int dx = 0, dy = 0;

	airmouse_init(&am, 0.2, 30.0);
	airmouse_process(&am, g, out, &dx, &dy);
	CHECK_INT_EQ(dx, 0, "zero input at rest: dx == 0");
	CHECK_INT_EQ(dy, 0, "zero input at rest: dy == 0");

	g[1] = 1.0;	/* pitch up */
	g[2] = -0.5;	/* yaw left */
	airmouse_process(&am, g, out, &dx, &dy);
	CHECK_FLOAT_NEAR(out[1], 0.2, 1e-12, "first frame filters y with alpha");
	CHECK_FLOAT_NEAR(out[2], -0.1, 1e-12, "first frame filters z with alpha");
	CHECK_INT_EQ(dx, 3, "dx = (int)(-(-0.1) * 30) == 3");
	CHECK_INT_EQ(dy, -6, "dy = (int)(-0.2 * 30) == -6");
}

static void test_truncation(void)
{
	struct airmouse am;
	double g[3] = { 0.0, 0.1, 0.0 }, out[3];
	int dx, dy;

	/* (int)(-0.02 * 30) = (int)(-0.6) = 0: truncation towards zero,
	 * not rounding. */
	airmouse_init(&am, 0.2, 30.0);
	airmouse_process(&am, g, out, &dx, &dy);
	CHECK_INT_EQ(dy, 0, "truncation towards zero: (int)(-0.6) == 0");

	g[1] = -0.1;
	airmouse_process(&am, g, out, &dx, &dy);
	CHECK_INT_EQ(dy, 0, "negative side: (int)(0.6) == 0");

	/* (int)(-0.15 * 10) = (int)(-1.5) = -1. */
	airmouse_init(&am, 0.5, 10.0);
	g[1] = 0.3;
	g[2] = 0.4;
	airmouse_process(&am, g, out, &dx, &dy);
	CHECK_INT_EQ(dy, -1, "(int)(-1.5) == -1 (towards zero)");
	CHECK_INT_EQ(dx, -2, "dx = (int)(-0.2 * 10) == -2");

	airmouse_init(&am, 1.0, 2.0);
	g[1] = 0.0;
	g[2] = 0.5;
	airmouse_process(&am, g, out, &dx, &dy);
	CHECK_INT_EQ(dx, -1, "dx = (int)(-0.5 * 2) == -1");
}

static void test_state_and_reset(void)
{
	struct airmouse am;
	double g[3] = { 0.0, 0.0, 0.0 }, out[3];
	int dx, dy;

	airmouse_init(&am, 0.2, 30.0);
	g[1] = 1.0;
	g[2] = -0.5;
	airmouse_process(&am, g, out, &dx, &dy);

	/* second frame, zero input: the filter decays towards zero */
	g[1] = 0.0;
	g[2] = 0.0;
	airmouse_process(&am, g, out, &dx, &dy);
	CHECK_INT_EQ(dx, 2, "decay frame: dx = (int)(0.8*0.1*30) == 2");
	CHECK_INT_EQ(dy, -4, "decay frame: dy = (int)(-0.8*0.2*30) == -4");

	/* re-init resets the state: same input, same output as frame 1 */
	airmouse_init(&am, 0.2, 30.0);
	g[1] = 1.0;
	g[2] = -0.5;
	airmouse_process(&am, g, out, &dx, &dy);
	CHECK_INT_EQ(dx, 3, "re-init: dx == 3 again");
	CHECK_INT_EQ(dy, -6, "re-init: dy == -6 again");
}

static void test_axis_isolation(void)
{
	struct airmouse am;
	double g[3] = { 1.0, 0.0, 0.0 }, out[3];
	int dx, dy;

	airmouse_init(&am, 0.5, 10.0);
	airmouse_process(&am, g, out, &dx, &dy);
	CHECK_FLOAT_NEAR(out[0], 0.5, 1e-12, "x axis filtered");
	CHECK_INT_EQ(dx, 0, "x axis does not move the pointer horizontally");
	CHECK_INT_EQ(dy, 0, "x axis does not move the pointer vertically");
}

int main(void)
{
	TEST_BEGIN();
	test_first_frame();
	test_truncation();
	test_state_and_reset();
	test_axis_isolation();
	TEST_SUMMARY("test_airmouse");
	return tu_fail_count ? 1 : 0;
}
