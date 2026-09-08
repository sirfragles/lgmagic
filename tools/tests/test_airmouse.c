/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * test_airmouse.c - golden tests for the extracted airmouse engine
 * (airmouse.h).
 *
 * The engine works on per-frame deltas (the MR21N "gyro" channel is the
 * remote's own pointer output, whose rest value parks off zero after
 * handling): the first sample is the baseline, and every following
 * frame contributes only its change from the previous frame.  The
 * dx/dy values below are the exact v1 `imu --mouse` semantics applied
 * to that delta stream: per-axis LPF followed by
 * dx = (int)(-filt[2] * scale), dy = (int)(-filt[1] * scale) with C
 * truncation towards zero.  The integers are the contract (checked
 * exactly); the filter outputs are checked within 1e-12.
 */
#include <stdio.h>

#include "test_util.h"
#include "airmouse.h"

static void test_baseline_and_step(void)
{
	struct airmouse am;
	double g[3] = { 0.0, 0.0, 0.0 }, out[3];
	int dx = 0, dy = 0;

	airmouse_init(&am, 0.2, 30.0);
	airmouse_process(&am, g, out, &dx, &dy);
	CHECK_INT_EQ(dx, 0, "baseline frame: no movement, dx == 0");
	CHECK_INT_EQ(dy, 0, "baseline frame: no movement, dy == 0");

	g[1] = 1.0;	/* pitch up */
	g[2] = -0.5;	/* yaw left */
	airmouse_process(&am, g, out, &dx, &dy);
	CHECK_FLOAT_NEAR(out[1], 0.2, 1e-12, "first delta filters y with alpha");
	CHECK_FLOAT_NEAR(out[2], -0.1, 1e-12, "first delta filters z with alpha");
	CHECK_INT_EQ(dx, 3, "dx = (int)(-(-0.1) * 30) == 3");
	CHECK_INT_EQ(dy, -6, "dy = (int)(-0.2 * 30) == -6");
}

static void test_parked_constant(void)
{
	struct airmouse am;
	double g[3] = { 5.0, 483.0, 1954.0 }, out[3];
	int dx, dy;
	int i;

	/* The drift bug: after handling, the remote's pointer output parks
	 * at hundreds of counts for tens of seconds.  Any CONSTANT input
	 * must produce no movement - not on the baseline frame, and not on
	 * any later frame. */
	airmouse_init(&am, 0.2, 30.0);
	for (i = 0; i < 5; i++) {
		airmouse_process(&am, g, out, &dx, &dy);
		CHECK_INT_EQ(dx, 0, "parked constant: dx == 0");
		CHECK_INT_EQ(dy, 0, "parked constant: dy == 0");
	}
}

static void test_baseline_anywhere(void)
{
	struct airmouse am;
	double g[3], out[3];
	int dx, dy;

	/* The baseline may sit anywhere (mid-decay, stale bias): only the
	 * CHANGE from it moves the pointer. */
	airmouse_init(&am, 0.2, 30.0);
	g[0] = 0.0; g[1] = 10.0; g[2] = 20.0;
	airmouse_process(&am, g, out, &dx, &dy);
	CHECK_INT_EQ(dx, 0, "nonzero baseline: dx == 0");
	CHECK_INT_EQ(dy, 0, "nonzero baseline: dy == 0");

	g[2] = 30.0;	/* step of +10 on z */
	airmouse_process(&am, g, out, &dx, &dy);
	CHECK_FLOAT_NEAR(out[2], 2.0, 1e-12, "step delta = 10, filtered by alpha");
	CHECK_INT_EQ(dx, -60, "dx = (int)(-2.0 * 30) == -60");
	CHECK_INT_EQ(dy, 0, "y unchanged: dy == 0");
}

static void test_truncation(void)
{
	struct airmouse am;
	double g[3] = { 0.0, 0.0, 0.0 }, out[3];
	int dx, dy;

	/* (int)(-0.02 * 30) = (int)(-0.6) = 0: truncation towards zero,
	 * not rounding. */
	airmouse_init(&am, 0.2, 30.0);
	airmouse_process(&am, g, out, &dx, &dy);	/* baseline */
	g[1] = 0.1;
	airmouse_process(&am, g, out, &dx, &dy);
	CHECK_INT_EQ(dy, 0, "truncation towards zero: (int)(-0.6) == 0");

	g[1] = -0.1;
	airmouse_process(&am, g, out, &dx, &dy);
	CHECK_INT_EQ(dy, 0, "negative side: (int)(0.6) == 0");

	/* (int)(-0.15 * 10) = (int)(-1.5) = -1. */
	airmouse_init(&am, 0.5, 10.0);
	g[0] = 0.0; g[1] = 0.0; g[2] = 0.0;
	airmouse_process(&am, g, out, &dx, &dy);	/* baseline */
	g[1] = 0.3;
	g[2] = 0.4;
	airmouse_process(&am, g, out, &dx, &dy);
	CHECK_INT_EQ(dy, -1, "(int)(-1.5) == -1 (towards zero)");
	CHECK_INT_EQ(dx, -2, "dx = (int)(-0.2 * 10) == -2");

	airmouse_init(&am, 1.0, 2.0);
	g[0] = 0.0; g[1] = 0.0; g[2] = 0.0;
	airmouse_process(&am, g, out, &dx, &dy);	/* baseline */
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
	airmouse_process(&am, g, out, &dx, &dy);	/* baseline */
	g[1] = 1.0;
	g[2] = -0.5;
	airmouse_process(&am, g, out, &dx, &dy);

	/* a repeated identical frame: delta 0, the filter decays */
	airmouse_process(&am, g, out, &dx, &dy);
	CHECK_INT_EQ(dx, 2, "decay frame: dx = (int)(0.8*0.1*30) == 2");
	CHECK_INT_EQ(dy, -4, "decay frame: dy = (int)(-0.8*0.2*30) == -4");

	/* re-init resets the state: the next frame is a baseline again */
	airmouse_init(&am, 0.2, 30.0);
	airmouse_process(&am, g, out, &dx, &dy);
	CHECK_INT_EQ(dx, 0, "re-init: first frame is the baseline, dx == 0");
	CHECK_INT_EQ(dy, 0, "re-init: first frame is the baseline, dy == 0");
}

static void test_axis_isolation(void)
{
	struct airmouse am;
	double g[3] = { 0.0, 0.0, 0.0 }, out[3];
	int dx, dy;

	airmouse_init(&am, 0.5, 10.0);
	airmouse_process(&am, g, out, &dx, &dy);	/* baseline */
	g[0] = 1.0;
	airmouse_process(&am, g, out, &dx, &dy);
	CHECK_FLOAT_NEAR(out[0], 0.5, 1e-12, "x axis filtered");
	CHECK_INT_EQ(dx, 0, "x axis does not move the pointer horizontally");
	CHECK_INT_EQ(dy, 0, "x axis does not move the pointer vertically");
}

int main(void)
{
	TEST_BEGIN();
	test_baseline_and_step();
	test_parked_constant();
	test_baseline_anywhere();
	test_truncation();
	test_state_and_reset();
	test_axis_isolation();
	TEST_SUMMARY("test_airmouse");
	return tu_fail_count ? 1 : 0;
}
