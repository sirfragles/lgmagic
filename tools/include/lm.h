/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * lm.h - Levenberg-Marquardt fit of the 12-parameter accelerometer
 * calibration model (scripts/calibrate.py equivalent):
 *
 *   x = [b0, b1, b2, m00..m22],  r_i(x) = ||M(a_i - b)|| - 9.80665
 *
 * Started from x0 = [mean(samples), identity] like the Python code, solved
 * with LM using the analytic Jacobian (see lm.c), Gauss elimination on the
 * 12x12 normal equations.
 */
#ifndef LG_TOOLS_LM_H
#define LG_TOOLS_LM_H

#include <stddef.h>

/* Fit; returns 0 on success (bias/m filled, cost = final mean squared
 * residual), -1 if fewer than 6 samples. */
int lm_fit_accel(const double (*samples)[3], size_t n,
		 double bias[3], double m[3][3], double *cost);

/* Residuals and analytic Jacobian, exposed for the numeric cross-check test. */
void lm_residuals(const double (*samples)[3], size_t n, const double x[12], double *r);
void lm_jacobian(const double (*samples)[3], size_t n, const double x[12], double J[][12]);

#endif /* LG_TOOLS_LM_H */
