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
 *
 * The spring-back gate works on the RAW accelerometer: an all-zero
 * accel vector means "no accelerometer" and disables the gate, which is
 * why every legacy test below passes the acc0 vector - its golden
 * numbers are the pure pass-through behaviour.  The gate tests feed
 * gravity (0,0,4007) at rest and a deviating accel during motion.
 */
#include <stdio.h>

#include "test_util.h"
#include "airmouse.h"

/* No accelerometer -> the gate stays off (pass-through). */
static const double acc0[3] = { 0.0, 0.0, 0.0 };
/* Flat-rest gravity (raw counts, z down) and a motion frame. */
static const double acc_rest[3] = { 0.0, 0.0, 4007.0 };
static const double acc_move[3] = { 0.0, 600.0, 4200.0 };

static void test_baseline_and_step(void)
{
	struct airmouse am;
	double g[3] = { 0.0, 0.0, 0.0 }, out[3];
	int dx = 0, dy = 0;

	airmouse_init(&am, 0.2, 30.0);
	airmouse_process(&am, g, acc0, out, &dx, &dy);
	CHECK_INT_EQ(dx, 0, "baseline frame: no movement, dx == 0");
	CHECK_INT_EQ(dy, 0, "baseline frame: no movement, dy == 0");

	g[1] = 1.0;	/* pitch up */
	g[2] = -0.5;	/* yaw left */
	airmouse_process(&am, g, acc0, out, &dx, &dy);
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
		airmouse_process(&am, g, acc0, out, &dx, &dy);
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
	airmouse_process(&am, g, acc0, out, &dx, &dy);
	CHECK_INT_EQ(dx, 0, "nonzero baseline: dx == 0");
	CHECK_INT_EQ(dy, 0, "nonzero baseline: dy == 0");

	g[2] = 30.0;	/* step of +10 on z */
	airmouse_process(&am, g, acc0, out, &dx, &dy);
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
	airmouse_process(&am, g, acc0, out, &dx, &dy);	/* baseline */
	g[1] = 0.1;
	airmouse_process(&am, g, acc0, out, &dx, &dy);
	CHECK_INT_EQ(dy, 0, "truncation towards zero: (int)(-0.6) == 0");

	g[1] = -0.1;
	airmouse_process(&am, g, acc0, out, &dx, &dy);
	CHECK_INT_EQ(dy, 0, "negative side: (int)(0.6) == 0");

	/* (int)(-0.15 * 10) = (int)(-1.5) = -1. */
	airmouse_init(&am, 0.5, 10.0);
	g[0] = 0.0; g[1] = 0.0; g[2] = 0.0;
	airmouse_process(&am, g, acc0, out, &dx, &dy);	/* baseline */
	g[1] = 0.3;
	g[2] = 0.4;
	airmouse_process(&am, g, acc0, out, &dx, &dy);
	CHECK_INT_EQ(dy, -1, "(int)(-1.5) == -1 (towards zero)");
	CHECK_INT_EQ(dx, -2, "dx = (int)(-0.2 * 10) == -2");

	airmouse_init(&am, 1.0, 2.0);
	g[0] = 0.0; g[1] = 0.0; g[2] = 0.0;
	airmouse_process(&am, g, acc0, out, &dx, &dy);	/* baseline */
	g[2] = 0.5;
	airmouse_process(&am, g, acc0, out, &dx, &dy);
	CHECK_INT_EQ(dx, -1, "dx = (int)(-0.5 * 2) == -1");
}

static void test_state_and_reset(void)
{
	struct airmouse am;
	double g[3] = { 0.0, 0.0, 0.0 }, out[3];
	int dx, dy;

	airmouse_init(&am, 0.2, 30.0);
	airmouse_process(&am, g, acc0, out, &dx, &dy);	/* baseline */
	g[1] = 1.0;
	g[2] = -0.5;
	airmouse_process(&am, g, acc0, out, &dx, &dy);

	/* a repeated identical frame: delta 0, the filter decays */
	airmouse_process(&am, g, acc0, out, &dx, &dy);
	CHECK_INT_EQ(dx, 2, "decay frame: dx = (int)(0.8*0.1*30) == 2");
	CHECK_INT_EQ(dy, -4, "decay frame: dy = (int)(-0.8*0.2*30) == -4");

	/* re-init resets the state: the next frame is a baseline again */
	airmouse_init(&am, 0.2, 30.0);
	airmouse_process(&am, g, acc0, out, &dx, &dy);
	CHECK_INT_EQ(dx, 0, "re-init: first frame is the baseline, dx == 0");
	CHECK_INT_EQ(dy, 0, "re-init: first frame is the baseline, dy == 0");
}

static void test_axis_isolation(void)
{
	struct airmouse am;
	double g[3] = { 0.0, 0.0, 0.0 }, out[3];
	int dx, dy;

	airmouse_init(&am, 0.5, 10.0);
	airmouse_process(&am, g, acc0, out, &dx, &dy);	/* baseline */
	g[0] = 1.0;
	airmouse_process(&am, g, acc0, out, &dx, &dy);
	CHECK_FLOAT_NEAR(out[0], 0.5, 1e-12, "x axis filtered");
	CHECK_INT_EQ(dx, 0, "x axis does not move the pointer horizontally");
	CHECK_INT_EQ(dy, 0, "x axis does not move the pointer vertically");
}

static void test_gate_passes_motion(void)
{
	struct airmouse am;
	double g[3] = { 0.0, 0.0, 0.0 }, out[3];
	int dx, dy;

	/* Motion with a deviating accelerometer moves the pointer - and the
	 * latch keeps the gate open for a few frames after the deviation,
	 * so the tail of the gesture is not cut off. */
	airmouse_init(&am, 0.2, 30.0);
	airmouse_process(&am, g, acc_rest, out, &dx, &dy);	/* arms + baseline */
	CHECK_INT_EQ(dx, 0, "gate baseline: dx == 0");
	CHECK_INT_EQ(dy, 0, "gate baseline: dy == 0");

	g[2] = 100.0;
	airmouse_process(&am, g, acc_move, out, &dx, &dy);
	CHECK_INT_EQ(dx, -600, "wave frame with accel deviation: dx == -600");
	CHECK_INT_EQ(dy, 0, "wave frame: dy == 0");

	/* accel back at rest, but the latch (dev > hi on the wave frame)
	 * still holds the gate open: the next step must pass. */
	g[2] = 200.0;
	airmouse_process(&am, g, acc_rest, out, &dx, &dy);
	CHECK_INT_EQ(dx, -1080, "latched frame: dx = (int)(-36*30) == -1080");
	CHECK_INT_EQ(dy, 0, "latched frame: dy == 0");
}

static void test_gate_blocks_spring(void)
{
	struct airmouse am;
	double g[3] = { 0.0, 0.0, 0.0 }, out[3];
	int dx, dy;
	int i;

	/* The spring-back: the POI returns towards its rest point with the
	 * accelerometer at rest (gravity +- noise).  Those deltas are
	 * recentering, not motion - closed frames absorb them EXACTLY. */
	airmouse_init(&am, 0.2, 30.0);
	airmouse_process(&am, g, acc_rest, out, &dx, &dy);	/* arms + baseline */

	g[2] = 100.0;
	airmouse_process(&am, g, acc_move, out, &dx, &dy);
	CHECK_INT_EQ(dx, -600, "wave: dx == -600 before the spring");

	/* Drain: constant gyro with at-rest accel.  The latch runs out
	 * after 10 frames, the gate closes, and the LPF decays below the
	 * (int) truncation threshold (out[2] = 20 * 0.8^40 ~ 2.7e-3). */
	for (i = 0; i < 40; i++)
		airmouse_process(&am, g, acc_rest, out, &dx, &dy);
	CHECK_INT_EQ(dx, 0, "drain: filter tail below truncation, dx == 0");
	CHECK_INT_EQ(dy, 0, "drain: dy == 0");

	/* The spring itself: a large gyro delta back towards baseline with
	 * the accelerometer at rest - the gate is closed, so it must move
	 * nothing at all. */
	g[2] = 0.0;
	airmouse_process(&am, g, acc_rest, out, &dx, &dy);
	CHECK_INT_EQ(dx, 0, "spring-back: absorbed, dx == 0 exactly");
	CHECK_INT_EQ(dy, 0, "spring-back: absorbed, dy == 0 exactly");

	/* The baseline advanced through the absorbed frame, so a repeated
	 * identical frame is delta-free anyway. */
	airmouse_process(&am, g, acc_rest, out, &dx, &dy);
	CHECK_INT_EQ(dx, 0, "post-spring: dx == 0");
	CHECK_INT_EQ(dy, 0, "post-spring: dy == 0");
}

static void test_gate_disabled(void)
{
	struct airmouse am;
	double g[3] = { 0.0, 0.0, 0.0 }, out[3];
	int dx, dy;

	/* accel_gate = false: the gate never interferes, even with a live
	 * accelerometer (the config kill-switch). */
	airmouse_init(&am, 0.2, 30.0);
	airmouse_gate_cfg(&am, 0, 60.0, 400.0);
	airmouse_process(&am, g, acc_rest, out, &dx, &dy);	/* baseline */
	g[2] = 100.0;
	airmouse_process(&am, g, acc_rest, out, &dx, &dy);
	CHECK_INT_EQ(dx, -600, "gate off: step passes with at-rest accel");
	CHECK_INT_EQ(dy, 0, "gate off: dy == 0");
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
	test_gate_passes_motion();
	test_gate_blocks_spring();
	test_gate_disabled();
	TEST_SUMMARY("test_airmouse");
	return tu_fail_count ? 1 : 0;
}
