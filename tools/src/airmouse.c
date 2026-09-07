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
 */
#include "airmouse.h"

void airmouse_init(struct airmouse *a, double alpha, double scale)
{
	a->alpha = alpha;
	a->scale = scale;
	a->prev[0] = a->prev[1] = a->prev[2] = 0.0;
	a->have_raw = 0;
}

void airmouse_process(struct airmouse *a, const double g[3], double out[3],
		      int *dx, int *dy)
{
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

		d[0] = g[0] - a->raw_prev[0];
		d[1] = g[1] - a->raw_prev[1];
		d[2] = g[2] - a->raw_prev[2];
		a->raw_prev[0] = g[0];
		a->raw_prev[1] = g[1];
		a->raw_prev[2] = g[2];
		out[0] = a->alpha * d[0] + (1.0 - a->alpha) * a->prev[0];
		out[1] = a->alpha * d[1] + (1.0 - a->alpha) * a->prev[1];
		out[2] = a->alpha * d[2] + (1.0 - a->alpha) * a->prev[2];
	}
	a->prev[0] = out[0];
	a->prev[1] = out[1];
	a->prev[2] = out[2];
	*dx = (int)(-out[2] * a->scale);
	*dy = (int)(-out[1] * a->scale);
}
