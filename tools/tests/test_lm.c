/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * test_lm.c - unit tests for the Levenberg-Marquardt accelerometer
 * calibration fit (lm.h / scripts/calibrate.py equivalent).
 *
 * The model: r_i(x) = ||M(a_i - b)|| - 9.80665, x = [b0..b2, m00..m22]
 * (m row-major).  M is only identifiable up to a left orthogonal rotation
 * (the gauge freedom of ||M(a-b)||), so the recovered M is compared
 * through the gauge-invariant product M^T M.
 *
 *  - noiseless synthetic ellipsoid data (N = 50, b* = [100, -50, 200],
 *    well-conditioned M*): bias recovered to 1e-3, cost < 1e-6,
 *    M^T M ~ M*^T M* to 1e-3;
 *  - the same data plus small noise (uniform +-5e-3, the declared unit is
 *    1e-3 of a count) still recovers the bias to within a few times the
 *    noise floor;
 *  - fewer than 6 samples returns -1;
 *  - lm_jacobian() agrees with a central-difference Jacobian of
 *    lm_residuals() to 1e-4 at both the solution and the LM start point.
 *
 * All samples come from a fixed-seed LCG + Box-Muller, so the test is
 * deterministic.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "test_util.h"
#include "lm.h"

#define LM_G 9.80665
#define N 50
#define EPS_NOISE 5e-3		/* +-5e-3 noise = +-5 counts of 1e-3 */

/* ------------------------------------------------------------------ */
/* Deterministic PRNG                                                  */
/* ------------------------------------------------------------------ */

static unsigned long long lcg_state = 0x9e3779b97f4a7c15ULL;

static double tu_rand01(void)
{
	/* 53-bit uniform in [0, 1) */
	lcg_state = lcg_state * 6364136223846793005ULL + 1442695040888963407ULL;
	return (double)(lcg_state >> 11) * (1.0 / 9007199254740992.0);
}

static double tu_randn(void)
{
	double u1, u2;

	do {
		u1 = tu_rand01();
	} while (u1 == 0.0);
	u2 = tu_rand01();
	return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

/* ------------------------------------------------------------------ */
/* Synthetic data:  a_i = b* + M*^-1 (g * u_i),  u_i unit directions   */
/* ------------------------------------------------------------------ */

static double inv3(const double m[3][3], double out[3][3])
{
	double d;

	d = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
	  - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
	  + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
	if (d == 0.0)
		return 0.0;
	out[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / d;
	out[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / d;
	out[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / d;
	out[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / d;
	out[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / d;
	out[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / d;
	out[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / d;
	out[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / d;
	out[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / d;
	return d;
}

static void gen_samples(double samples[N][3], const double b[3],
			const double m[3][3], double noise)
{
	double mi[3][3];
	int i, j;

	if (inv3(m, mi) == 0.0) {
		fprintf(stderr, "test_lm: singular model matrix\n");
		exit(2);
	}
	for (i = 0; i < N; i++) {
		double u[3], nrm;

		/* unit direction from 3 gaussians */
		for (j = 0; j < 3; j++)
			u[j] = tu_randn();
		nrm = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
		for (j = 0; j < 3; j++)
			u[j] /= nrm;
		/* a = b* + M*^-1 (g u)  =>  ||M*(a - b*)|| == g exactly */
		for (j = 0; j < 3; j++) {
			double v = 0.0;
			int k;

			for (k = 0; k < 3; k++)
				v += mi[j][k] * (LM_G * u[k]);
			samples[i][j] = b[j] + v + noise * (2.0 * tu_rand01() - 1.0);
		}
	}
}

static void mtm(const double m[3][3], double out[3][3])
{
	int i, j, k;

	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++) {
			out[i][j] = 0.0;
			for (k = 0; k < 3; k++)
				out[i][j] += m[k][i] * m[k][j];
		}
}

static void test_fit_noiseless(void)
{
	static const double b_star[3] = { 100.0, -50.0, 200.0 };
	static const double m_star[3][3] = {
		{ 1.1, 0.05, -0.02 },
		{ 0.0, 1.05, 0.03 },
		{ 0.0, 0.0, 0.95 }
	};
	double samples[N][3];
	double bias[3], m[3][3], cost = 0.0;
	double mtm_got[3][3], mtm_want[3][3];
	int i, j, rc, ok;

	gen_samples(samples, b_star, m_star, 0.0);
	rc = lm_fit_accel(samples, N, bias, m, &cost);
	CHECK(rc == 0, "noiseless fit succeeds");
	if (rc != 0)
		return;

	ok = 1;
	for (i = 0; i < 3; i++)
		if (fabs(bias[i] - b_star[i]) > 1e-3)
			ok = 0;
	CHECK(ok == 1, "bias recovered to 1e-3 on noiseless data");
	if (!ok)
		printf("  got bias (%.6f %.6f %.6f), want (%.0f %.0f %.0f)\n",
		       bias[0], bias[1], bias[2], b_star[0], b_star[1],
		       b_star[2]);

	CHECK(cost < 1e-6, "final cost < 1e-6 on noiseless data");
	if (!(cost < 1e-6))
		printf("  cost = %.3g\n", cost);

	/* M is identifiable only up to a left rotation: M^T M is invariant. */
	mtm(m, mtm_got);
	mtm(m_star, mtm_want);
	ok = 1;
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			if (fabs(mtm_got[i][j] - mtm_want[i][j]) > 1e-3)
				ok = 0;
	CHECK(ok == 1, "M^T M matches the model to 1e-3 (gauge-invariant)");
	if (!ok)
		printf("  got M^T M [[%.5f %.5f %.5f] [%.5f %.5f %.5f] "
		       "[%.5f %.5f %.5f]]\n  want     [[%.5f %.5f %.5f] "
		       "[%.5f %.5f %.5f] [%.5f %.5f %.5f]]\n",
		       mtm_got[0][0], mtm_got[0][1], mtm_got[0][2],
		       mtm_got[1][0], mtm_got[1][1], mtm_got[1][2],
		       mtm_got[2][0], mtm_got[2][1], mtm_got[2][2],
		       mtm_want[0][0], mtm_want[0][1], mtm_want[0][2],
		       mtm_want[1][0], mtm_want[1][1], mtm_want[1][2],
		       mtm_want[2][0], mtm_want[2][1], mtm_want[2][2]);
}

static void test_fit_noisy(void)
{
	static const double b_star[3] = { 100.0, -50.0, 200.0 };
	static const double m_star[3][3] = {
		{ 1.1, 0.05, -0.02 },
		{ 0.0, 1.05, 0.03 },
		{ 0.0, 0.0, 0.95 }
	};
	double samples[N][3];
	double bias[3], m[3][3], cost = 0.0;
	int i, rc, ok;

	/* noise floor for the center with N = 50 is well below 1e-3 in the
	 * declared units; allow a generous margin so the test is not brittle */
	gen_samples(samples, b_star, m_star, EPS_NOISE);
	rc = lm_fit_accel(samples, N, bias, m, &cost);
	CHECK(rc == 0, "fit with +-5e-3 noise succeeds");
	if (rc != 0)
		return;

	ok = 1;
	for (i = 0; i < 3; i++)
		if (fabs(bias[i] - b_star[i]) > 2e-3)
			ok = 0;
	CHECK(ok == 1, "bias recovered to 2e-3 under +-5e-3 noise");
	if (!ok)
		printf("  got bias (%.6f %.6f %.6f), want (%.0f %.0f %.0f), "
		       "cost %.3g\n",
		       bias[0], bias[1], bias[2], b_star[0], b_star[1],
		       b_star[2], cost);
}

static void test_too_few_samples(void)
{
	double samples[5][3] = {
		{ 0.0, 0.0, 0.0 }, { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 },
		{ 0.0, 0.0, 1.0 }, { 1.0, 1.0, 1.0 }
	};
	double bias[3] = { 0, 0, 0 }, m[3][3], cost = 123.0;
	int rc;

	rc = lm_fit_accel(samples, 5, bias, m, &cost);
	CHECK(rc == -1, "fewer than 6 samples returns -1");
}

static void test_jacobian_numeric(void)
{
	static const double b_star[3] = { 100.0, -50.0, 200.0 };
	static const double m_star[3][3] = {
		{ 1.1, 0.05, -0.02 },
		{ 0.0, 1.05, 0.03 },
		{ 0.0, 0.0, 0.95 }
	};
	/* two evaluation points: the true solution and the LM start
	 * (mean, identity) */
	double x_pts[2][12];
	double samples[N][3];
	double r[2][N];
	double J[2][N][12];
	int i, j, p;

	gen_samples(samples, b_star, m_star, EPS_NOISE);

	/* point 0: true parameters, flattened row-major */
	for (j = 0; j < 3; j++)
		x_pts[0][j] = b_star[j];
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			x_pts[0][3 + 3 * i + j] = m_star[i][j];

	/* point 1: [mean(samples), identity] */
	for (j = 0; j < 3; j++) {
		double s = 0.0;

		for (i = 0; i < N; i++)
			s += samples[i][j];
		x_pts[1][j] = s / N;
	}
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			x_pts[1][3 + 3 * i + j] = (i == j) ? 1.0 : 0.0;

	for (p = 0; p < 2; p++) {
		lm_residuals(samples, N, x_pts[p], r[p]);
		lm_jacobian(samples, N, x_pts[p], J[p]);
	}
	for (p = 0; p < 2; p++) {
		double maxerr = 0.0;
		double maxabs = 0.0;

		for (j = 0; j < 12; j++) {
			double xp[12], xm[12];
			double rp[N], rm[N];
			double h = 1e-6 * (fabs(x_pts[p][j]) > 1.0 ?
					   fabs(x_pts[p][j]) : 1.0);
			int k;

			for (k = 0; k < 12; k++) {
				xp[k] = x_pts[p][k];
				xm[k] = x_pts[p][k];
			}
			xp[j] += h;
			xm[j] -= h;
			lm_residuals(samples, N, xp, rp);
			lm_residuals(samples, N, xm, rm);
			for (i = 0; i < N; i++) {
				double num = (rp[i] - rm[i]) / (2.0 * h);
				double err = fabs(num - J[p][i][j]);

				if (err > maxerr)
					maxerr = err;
				if (fabs(J[p][i][j]) > maxabs)
					maxabs = fabs(J[p][i][j]);
			}
		}
		if (p == 0)
			CHECK(maxerr <= 1e-4,
			      "analytic Jacobian matches numeric at the "
			      "solution (1e-4)");
		else
			CHECK(maxerr <= 1e-4,
			      "analytic Jacobian matches numeric at the start "
			      "point (1e-4)");
		if (maxerr > 1e-4)
			printf("  max |J_num - J_an| = %.3g (max |J| %.3g)\n",
			       maxerr, maxabs);
	}
}

int main(void)
{
	TEST_BEGIN();
	test_fit_noiseless();
	test_fit_noisy();
	test_too_few_samples();
	test_jacobian_numeric();
	TEST_SUMMARY("test_lm");
	return tu_fail_count ? 1 : 0;
}
