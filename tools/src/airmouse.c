/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * airmouse.c - the userspace airmouse engine (portable).  See airmouse.h.
 *
 * The input channel is the remote's own pointer output (POI on the
 * MR21N), not a raw gyro rate: its rest value parks at hundreds of
 * counts after handling and recenters to zero only over tens of
 * seconds.  Feeding that through a static bias subtraction leaves a
 * constant residual that crawls the pointer forever.  The engine
 * therefore works on per-frame deltas: the first sample is the
 * baseline, every following frame moves the pointer only by its change
 * from the previous frame.  Constant offsets (parked POI, a stale
 * calibration bias, the slow recentering ramp) cancel in the
 * difference, and the existing per-axis LPF still smooths the delta
 * stream.  The movement formulas replicate cmd_imu.c:39-47 and 263-265
 * from v1 exactly - including the (int) cast truncation - so `imu
 * --mouse` semantics stay byte-identical apart from the delta input.
 *
 * The spring-back gate (see airmouse.h) absorbs closed frames outright:
 * the POI returning to rest is recentering, not motion, and must not
 * erase the net displacement of the gesture that preceded it.
 */
#include "airmouse.h"

void airmouse_init(struct airmouse *a, double alpha, double scale)
{
	a->alpha = alpha;
	a->scale = scale;
	a->prev[0] = a->prev[1] = a->prev[2] = 0.0;
	a->have_raw = 0;
	a->gate_on = 1;
	a->gate_lo = 60.0;
	a->gate_hi = 400.0;
	a->gate_a = 0.02;
	a->gate_latch = 10;
	a->latch_left = 0;
	a->gate_open = 1;
	a->a_absent = 1;
	a->a_base[0] = a->a_base[1] = a->a_base[2] = 0.0;
}

void airmouse_gate_cfg(struct airmouse *a, int on, double lo, double hi)
{
	a->gate_on = on;
	a->gate_lo = lo;
	a->gate_hi = hi;
}

/* dev = max|acc - a_base| in raw accel counts, computed against the
 * PREVIOUS baseline; a_base then follows with the slow EMA.  Returns
 * 1 when the frame should move the pointer. */
static int gate_decision(struct airmouse *a, const double acc[3])
{
	double dev = 0.0;
	int i;

	for (i = 0; i < 3; i++) {
		double e = acc[i] - a->a_base[i];

		if (e < 0.0)
			e = -e;
		if (e > dev)
			dev = e;
	}
	if (dev > a->gate_hi)
		a->latch_left = a->gate_latch;
	else if (a->latch_left > 0)
		a->latch_left--;
	if (a->gate_open) {
		/* Hysteresis: stay open while the latch runs, close only
		 * below lo/2 - the wiggle between lo/2 and lo is motion
		 * noise, not a decision. */
		if (dev < a->gate_lo * 0.5 && a->latch_left == 0)
			a->gate_open = 0;
	} else if (dev > a->gate_lo) {
		a->gate_open = 1;
	}
	for (i = 0; i < 3; i++)
		a->a_base[i] += a->gate_a * (acc[i] - a->a_base[i]);
	return a->gate_open;
}

void airmouse_process(struct airmouse *a, const double g[3],
		      const double acc[3], double out[3],
		      int *dx, int *dy)
{
	/* An all-zero accelerometer means the source reports no accel at
	 * all (a gyro-only device or a synthetic test frame): the gate has
	 * no discriminator there and stays off.  The first non-zero accel
	 * frame arms it with that reading as the baseline. */
	if (a->gate_on && a->a_absent &&
	    (acc[0] != 0.0 || acc[1] != 0.0 || acc[2] != 0.0)) {
		a->a_base[0] = acc[0];
		a->a_base[1] = acc[1];
		a->a_base[2] = acc[2];
		a->a_absent = 0;
	}

	if (!a->have_raw) {
		/* First sample is the baseline: constant offsets carry no
		 * motion, so nothing moves until a change is observed. */
		a->raw_prev[0] = g[0];
		a->raw_prev[1] = g[1];
		a->raw_prev[2] = g[2];
		a->have_raw = 1;
		out[0] = out[1] = out[2] = 0.0;
	} else {
		double d[3];
		int closed = 0;

		d[0] = g[0] - a->raw_prev[0];
		d[1] = g[1] - a->raw_prev[1];
		d[2] = g[2] - a->raw_prev[2];
		/* The baseline advances even when the gate closes: the
		 * spring-back deltas are absorbed, never deferred. */
		a->raw_prev[0] = g[0];
		a->raw_prev[1] = g[1];
		a->raw_prev[2] = g[2];

		if (a->gate_on && !a->a_absent)
			closed = !gate_decision(a, acc);

		if (closed) {
			/* Absorb the frame: no delta enters the filter, the
			 * existing state decays towards zero. */
			out[0] = (1.0 - a->alpha) * a->prev[0];
			out[1] = (1.0 - a->alpha) * a->prev[1];
			out[2] = (1.0 - a->alpha) * a->prev[2];
		} else {
			out[0] = a->alpha * d[0] +
				 (1.0 - a->alpha) * a->prev[0];
			out[1] = a->alpha * d[1] +
				 (1.0 - a->alpha) * a->prev[1];
			out[2] = a->alpha * d[2] +
				 (1.0 - a->alpha) * a->prev[2];
		}
	}
	a->prev[0] = out[0];
	a->prev[1] = out[1];
	a->prev[2] = out[2];
	*dx = (int)(-out[2] * a->scale);
	*dy = (int)(-out[1] * a->scale);
}
