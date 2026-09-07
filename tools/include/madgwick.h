/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * madgwick.h - AHRS orientation filter (Madgwick, 6-DoF IMU variant).
 *
 * Port of the public-domain implementation by Sebastian Madgwick
 * (https://x-io.co.uk/open-source-imu-and-ahrs-algorithms/) to a plain C
 * state struct, matching the behaviour of the Python `ahrs` library as
 * used by scripts/display_imu.py (Madgwick(sampleperiod=0.02, beta=0.1)).
 *
 * This port is GPL-2.0-or-later; the algorithm itself is public domain.
 */
#ifndef LG_TOOLS_MADGWICK_H
#define LG_TOOLS_MADGWICK_H

#include "matrix.h"

struct madgwick_state {
	double beta;		/* filter gain, 0.1 */
	double sample_freq;	/* fixed 50 Hz (Python had Dt commented out) */
	double qw, qx, qy, qz;	/* internal state in double - the Python `ahrs`
				 * implementation computes in float64, and the
				 * golden-trace test compares to 1e-6 */
	quat q;			/* last result, for consumers (cube, euler print) */
};

void madgwick_init(struct madgwick_state *st, double beta, double sample_freq, quat q0);

/* gyro in rad/s, accel in m/s^2 (normalized internally, like the ahrs lib). */
void madgwick_update_imu(struct madgwick_state *st,
			 double gx, double gy, double gz,
			 double ax, double ay, double az);

#endif /* LG_TOOLS_MADGWICK_H */
