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
 * The POI however SPRINGS BACK to its rest point within a second or two
 * after motion stops (firmware recentering - no physical movement), and
 * those deltas are larger than the gesture that preceded them.  Fed
 * through the delta engine they drag the pointer back towards the
 * centre.  The raw accelerometer discriminates the two: gravity +-20
 * counts at rest and during the spring, a large deviation during real
 * motion.  When the gate is on, every frame measures dev = max|acc -
 * a_base| against a slow EMA of the accelerometer (a_base); dev above
 * gate_hi refreshes a latch, dev above gate_lo opens the gate, and when
 * the latch drains and dev falls below gate_lo/2 it closes.  Closed
 * frames ABSORB the delta entirely (the LPF decays towards zero), so
 * the spring contributes exactly zero movement.  An accelerometer that
 * never reports (all-zero vector) disables the gate - a gyro-only
 * source has no discriminator - which keeps pure-gyro setups and their
 * tests on the exact pass-through behaviour.
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
	/* Spring-back gate (see above). */
	int gate_on;		/* config accel_gate */
	double gate_lo, gate_hi; /* config accel_gate_lo / accel_gate_hi */
	double gate_a;		/* a_base EMA gain (0.02) */
	int gate_latch;		/* frames to hold the gate open after dev>hi */
	int latch_left;		/* latch countdown */
	int gate_open;		/* currently open (movement passes) */
	int a_absent;		/* accel has never reported -> gate disabled */
	double a_base[3];	/* slow EMA of the raw accelerometer */
};

/* Reset the state; alpha and scale are fixed for the lifetime of the
 * filter (as in v1, where they were read from the config once).  The
 * gate defaults to on with lo=60 / hi=400 raw accel counts. */
void airmouse_init(struct airmouse *a, double alpha, double scale);

/* Re-arm the gate with config values (call after airmouse_init). */
void airmouse_gate_cfg(struct airmouse *a, int on, double lo, double hi);

/* Filter the frame-to-frame change of the input vector g (rad or
 * counts - the engine is unit-agnostic), return the filtered delta
 * vector in out and the resulting pointer movement in dx and dy with
 * the exact v1 int truncation.  acc is the RAW accelerometer reading
 * (same units as gate_lo/gate_hi); an all-zero acc vector means "no
 * accelerometer data" and disables the gate.  The first call after
 * init/re-init only records the baseline and moves nothing. */
void airmouse_process(struct airmouse *a, const double g[3],
		      const double acc[3], double out[3],
		      int *dx, int *dy);

#endif /* LG_TOOLS_AIRMOUSE_H */
