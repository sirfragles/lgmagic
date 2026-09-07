/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * madgwick.c - 6-DoF IMU orientation filter (Madgwick).
 *
 * Port of the public-domain algorithm by Sebastian Madgwick
 * (https://x-io.co.uk/open-source-imu-and-ahrs-algorithms/) in the exact
 * form used by the Python `ahrs` library's updateIMU() - the ground truth
 * for the original scripts - including its quirks:
 *
 *   - if the gyro sample is exactly zero, the quaternion is returned
 *     unchanged (the original x-io code always integrates),
 *   - only a normalized copy of the quaternion is used for the
 *     accelerometer objective function and Jacobian; the integration
 *     always uses the original q,
 *   - all arithmetic is float64 (numpy), hence double here.
 *
 * This port is GPL-2.0-or-later; the algorithm itself is public domain.
 */
#include "madgwick.h"

#include <math.h>

void madgwick_init(struct madgwick_state *st, double beta, double sample_freq,
		   quat q0)
{
	st->beta = beta;
	st->sample_freq = sample_freq;
	st->qw = q0.w;
	st->qx = q0.x;
	st->qy = q0.y;
	st->qz = q0.z;
	st->q = q0;
}

void madgwick_update_imu(struct madgwick_state *st,
			 double gx, double gy, double gz,
			 double ax, double ay, double az)
{
	double qw = st->qw, qx = st->qx, qy = st->qy, qz = st->qz;
	double qdw, qdx, qdy, qdz;
	double dt = 1.0 / st->sample_freq;

	/* ahrs: if np.linalg.norm(gyr) == 0 -> return q unchanged */
	if (gx == 0.0 && gy == 0.0 && gz == 0.0)
		goto done;

	/* qDot = 0.5 * q (x) [0, gx, gy, gz], Hamilton product */
	qdw = 0.5 * (-qx * gx - qy * gy - qz * gz);
	qdx = 0.5 * ( qw * gx + qy * gz - qz * gy);
	qdy = 0.5 * ( qw * gy - qx * gz + qz * gx);
	qdz = 0.5 * ( qw * gz + qx * gy - qy * gx);

	{
		double an = sqrt(ax * ax + ay * ay + az * az);

		if (an > 0.0) {
			/* Normalised accelerometer and (copy of the) quaternion,
			 * used only for f and J below. */
			double a0 = ax / an, a1 = ay / an, a2 = az / an;
			double qn = sqrt(qw * qw + qx * qx + qy * qy + qz * qz);
			double nw = qw / qn, nx = qx / qn, ny = qy / qn, nz = qz / qn;
			/* Objective function (eq. 25) */
			double f0 = 2.0 * (nx * nz - nw * ny) - a0;
			double f1 = 2.0 * (nw * nx + ny * nz) - a1;
			double f2 = 2.0 * (0.5 - nx * nx - ny * ny) - a2;
			double fn = sqrt(f0 * f0 + f1 * f1 + f2 * f2);

			if (fn > 0.0) {
				/* gradient = J^T f (eq. 34), J from (eq. 26) */
				double g0 = (-2.0 * ny) * f0 + ( 2.0 * nx) * f1;
				double g1 = ( 2.0 * nz) * f0 + ( 2.0 * nw) * f1
						+ (-4.0 * nx) * f2;
				double g2 = (-2.0 * nw) * f0 + ( 2.0 * nz) * f1
						+ (-4.0 * ny) * f2;
				double g3 = ( 2.0 * nx) * f0 + ( 2.0 * ny) * f1;
				double gn = sqrt(g0 * g0 + g1 * g1 + g2 * g2 + g3 * g3);

				g0 /= gn;
				g1 /= gn;
				g2 /= gn;
				g3 /= gn;
				qdw -= st->beta * g0;
				qdx -= st->beta * g1;
				qdy -= st->beta * g2;
				qdz -= st->beta * g3;
			}
		}
	}

	/* Integrate (eq. 13) and normalise */
	qw += qdw * dt;
	qx += qdx * dt;
	qy += qdy * dt;
	qz += qdz * dt;
	{
		double n = sqrt(qw * qw + qx * qx + qy * qy + qz * qz);

		qw /= n;
		qx /= n;
		qy /= n;
		qz /= n;
	}

done:
	st->qw = qw;
	st->qx = qx;
	st->qy = qy;
	st->qz = qz;
	st->q = (quat){ qw, qx, qy, qz };
}
