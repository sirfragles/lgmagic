/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * airmouse.h - the userspace airmouse engine (portable).
 *
 * Extracted from cmd_imu.c (v1 `imu --mouse`): a per-axis low-pass
 * filter followed by the exact v1 movement formula
 *
 *     dx = (int)(-filt[2] * scale);
 *     dy = (int)(-filt[1] * scale);
 *
 * with C `(int)` truncation (towards zero) preserved bit for bit.
 * Used by `imu --mouse`, the setup wizard's airmouse test and the
 * lg-magicd daemon - one engine, one behaviour.
 */
#ifndef LG_TOOLS_AIRMOUSE_H
#define LG_TOOLS_AIRMOUSE_H

struct airmouse {
	double alpha;		/* LPF gain (config lpf_alpha, 0.2) */
	double prev[3];		/* filter state */
	double scale;		/* config mouse_scale / profile sensitivity */
};

/* Reset the state; alpha and scale are fixed for the lifetime of the
 * filter (as in v1, where they were read from the config once). */
void airmouse_init(struct airmouse *a, double alpha, double scale);

/* Filter the calibrated gyro vector g (rad/s or counts - the engine is
 * unit-agnostic), return the filtered vector in out and the resulting
 * pointer movement in dx and dy with the exact v1 int truncation. */
void airmouse_process(struct airmouse *a, const double g[3], double out[3],
		      int *dx, int *dy);

#endif /* LG_TOOLS_AIRMOUSE_H */
