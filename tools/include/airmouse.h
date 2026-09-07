/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * airmouse.h - the userspace airmouse engine (portable).
 *
 * The engine works on PER-FRAME DELTAS of the input vector, not on the
 * vector itself: the first sample is the baseline, and every following
 * frame contributes only its change from the previous frame (through
 * the per-axis low-pass filter).  The MR21N "gyro" channel is the
 * remote's own pointer output (POI) - a position-like signal whose rest
 * value parks at hundreds of counts after handling and recenters to
 * zero only over tens of seconds.  A static bias can never neutralise
 * that, but a difference can: any constant offset (parked POI, stale
 * calibration bias, slow recentering) produces zero delta and therefore
 * no movement, while real motion passes through unchanged.
 *
 * The movement formula is the exact v1 formula
 *
 *     dx = (int)(-filt[2] * scale);
 *     dy = (int)(-filt[1] * scale);
 *
 * with C `(int)` truncation (towards zero) preserved bit for bit.
 * Used by `imu --mouse`, the setup wizard's airmouse test and the
 * lgmagicd daemon - one engine, one behaviour.
 */
#ifndef LG_TOOLS_AIRMOUSE_H
#define LG_TOOLS_AIRMOUSE_H

struct airmouse {
	double alpha;		/* LPF gain (config lpf_alpha, 0.2) */
	double prev[3];		/* filter state (delta stream) */
	double raw_prev[3];	/* previous input frame (delta baseline) */
	int have_raw;		/* first frame sets the baseline, no delta */
	double scale;		/* config mouse_scale / profile sensitivity */
};

/* Reset the state; alpha and scale are fixed for the lifetime of the
 * filter (as in v1, where they were read from the config once). */
void airmouse_init(struct airmouse *a, double alpha, double scale);

/* Filter the frame-to-frame change of the input vector g (rad or
 * counts - the engine is unit-agnostic), return the filtered delta
 * vector in out and the resulting pointer movement in dx and dy with
 * the exact v1 int truncation.  The first call after init/re-init only
 * records the baseline and moves nothing. */
void airmouse_process(struct airmouse *a, const double g[3], double out[3],
		      int *dx, int *dy);

#endif /* LG_TOOLS_AIRMOUSE_H */
