/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * lm.c - Levenberg-Marquardt fit of the 12-parameter accelerometer
 * calibration model, the C equivalent of scripts/calibrate.py:
 *
 *   x = [b0, b1, b2, m00..m22] (row-major),  r_i(x) = ||M(a_i - b)|| - g
 *
 * with g = 9.80665. Started from x0 = [mean(samples), identity] like the
 * Python code, solved with LM using the analytic Jacobian:
 *
 *   v = M(a - b), nrm = ||v||
 *   dr/db_j   = -(v . M_col_j) / nrm
 *   dr/dM_kj  = (a - b)_j * v_k / nrm
 *
 * guarded by nrm < 1e-9 (sample skipped). The normal equations
 * (J^T J + lambda*diag(J^T J)) d = -J^T r are solved by Gauss elimination
 * with partial pivoting. Lambda adapts x10 on rejection, /10 on
 * acceptance; stops when the cost change drops below 1e-10 or after
 * 100 iterations (scipy's least_squares defaults to TRF, not LM, so the
 * converged parameters are compared within ~1e-4, not bit-identical).
 */
#include "lm.h"

#include <math.h>
#include <stdlib.h>

#define LM_G 9.80665
#define LM_N 12
#define LM_MAX_ITER 100
#define LM_MIN_IMPROV 1e-10
#define LM_NRM_GUARD 1e-9

/* v = M(a - b); d = a - b (input a is one sample, x the parameters) */
static void calc_v(const double *a, const double x[LM_N], double *d, double *v)
{
	int j, k;

	for (j = 0; j < 3; j++)
		d[j] = a[j] - x[j];
	for (k = 0; k < 3; k++) {
		v[k] = 0.0;
		for (j = 0; j < 3; j++)
			v[k] += x[3 + 3 * k + j] * d[j];
	}
}

static double resid_one(const double *a, const double x[LM_N])
{
	double d[3], v[3], nrm;

	calc_v(a, x, d, v);
	nrm = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	return nrm - LM_G;
}

void lm_residuals(const double (*samples)[3], size_t n, const double x[LM_N],
		  double *r)
{
	size_t i;

	for (i = 0; i < n; i++)
		r[i] = resid_one(samples[i], x);
}

void lm_jacobian(const double (*samples)[3], size_t n, const double x[LM_N],
		 double J[][LM_N])
{
	size_t i;
	int j, k;

	for (i = 0; i < n; i++) {
		const double *a = samples[i];
		double d[3], v[3], nrm;

		calc_v(a, x, d, v);
		nrm = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (nrm < LM_NRM_GUARD) {
			for (j = 0; j < LM_N; j++)
				J[i][j] = 0.0;
			continue;
		}
		/* dr/db_j = -(v . M_col_j) / nrm */
		for (j = 0; j < 3; j++) {
			double dot = 0.0;

			for (k = 0; k < 3; k++)
				dot += v[k] * x[3 + 3 * k + j];
			J[i][j] = -dot / nrm;
		}
		/* dr/dM_kj = (a - b)_j * v_k / nrm */
		for (k = 0; k < 3; k++)
			for (j = 0; j < 3; j++)
				J[i][3 + 3 * k + j] = d[j] * v[k] / nrm;
	}
}

/* cost = mean of squared residuals */
static double mean_cost(const double *r, size_t n)
{
	double s = 0.0;
	size_t i;

	for (i = 0; i < n; i++)
		s += r[i] * r[i];
	return s / (double)n;
}

/* Solve A x = b with Gauss elimination and partial pivoting.
 * Returns 0 on success, -1 on a (near-)singular pivot. */
static int gauss_solve(double A[LM_N][LM_N], const double b[LM_N],
		       double x[LM_N])
{
	double M[LM_N][LM_N + 1];
	int i, j, k;

	for (i = 0; i < LM_N; i++) {
		for (j = 0; j < LM_N; j++)
			M[i][j] = A[i][j];
		M[i][LM_N] = b[i];
	}
	for (k = 0; k < LM_N; k++) {
		int piv = k;
		double t, maxv = fabs(M[k][k]);

		for (i = k + 1; i < LM_N; i++) {
			if (fabs(M[i][k]) > maxv) {
				maxv = fabs(M[i][k]);
				piv = i;
			}
		}
		if (maxv < 1e-15)
			return -1;
		if (piv != k) {
			for (j = k; j <= LM_N; j++) {
				t = M[k][j];
				M[k][j] = M[piv][j];
				M[piv][j] = t;
			}
		}
		for (i = k + 1; i < LM_N; i++) {
			double f = M[i][k] / M[k][k];

			M[i][k] = 0.0;
			for (j = k + 1; j <= LM_N; j++)
				M[i][j] -= f * M[k][j];
		}
	}
	for (i = LM_N - 1; i >= 0; i--) {
		double s = M[i][LM_N];

		for (j = i + 1; j < LM_N; j++)
			s -= M[i][j] * x[j];
		x[i] = s / M[i][i];
	}
	return 0;
}

int lm_fit_accel(const double (*samples)[3], size_t n,
		 double bias[3], double m[3][3], double *cost)
{
	double x[LM_N];
	double *r, *r_new;
	double (*J)[LM_N];
	size_t i;
	int iter, j, k, converged = 0;

	if (n < 6)
		return -1;

	/* x0 = [mean(samples), identity] */
	for (j = 0; j < 3; j++) {
		double s = 0.0;

		for (i = 0; i < n; i++)
			s += samples[i][j];
		x[j] = s / (double)n;
	}
	for (k = 0; k < 3; k++)
		for (j = 0; j < 3; j++)
			x[3 + 3 * k + j] = (k == j) ? 1.0 : 0.0;

	r = malloc(n * sizeof(*r));
	r_new = malloc(n * sizeof(*r_new));
	J = malloc(n * sizeof(*J));
	if (!r || !r_new || !J) {
		free(r);
		free(r_new);
		free(J);
		return -1;
	}

	for (iter = 0; iter < LM_MAX_ITER && !converged; iter++) {
		double A[LM_N][LM_N], g[LM_N], d[LM_N], x_new[LM_N];
		double lambda = 1e-3, cost_old, cost_new;
		int improved = 0, tries;

		lm_residuals(samples, n, x, r);
		cost_old = mean_cost(r, n);

		/* A = J^T J, g = J^T r */
		lm_jacobian(samples, n, x, J);
		for (j = 0; j < LM_N; j++) {
			g[j] = 0.0;
			for (i = 0; i < n; i++)
				g[j] += J[i][j] * r[i];
			for (k = 0; k < LM_N; k++) {
				A[j][k] = 0.0;
				for (i = 0; i < n; i++)
					A[j][k] += J[i][j] * J[i][k];
			}
		}
		{
			double ng[LM_N];

			for (j = 0; j < LM_N; j++)
				ng[j] = -g[j];
			for (tries = 0; tries < 10; tries++) {
				double Ap[LM_N][LM_N];

				for (j = 0; j < LM_N; j++)
					for (k = 0; k < LM_N; k++)
						Ap[j][k] = A[j][k];
				for (j = 0; j < LM_N; j++)
					Ap[j][j] += lambda *
						(A[j][j] > 0.0 ? A[j][j] : 1.0);
				/* solve Ap d = -g */
				if (gauss_solve(Ap, ng, d) < 0) {
					lambda *= 10.0;
					continue;
				}
				for (j = 0; j < LM_N; j++)
					x_new[j] = x[j] + d[j];
				lm_residuals(samples, n, x_new, r_new);
				cost_new = mean_cost(r_new, n);
				if (cost_new < cost_old) {
					for (j = 0; j < LM_N; j++)
						x[j] = x_new[j];
					lambda /= 10.0;
					improved = 1;
					converged =
						fabs(cost_old - cost_new) <
						LM_MIN_IMPROV;
					break;
				}
				lambda *= 10.0;
			}
		}
		if (!improved)
			converged = 1;	/* cannot make progress - stop */
	}
	for (j = 0; j < 3; j++)
		bias[j] = x[j];
	for (k = 0; k < 3; k++)
		for (j = 0; j < 3; j++)
			m[k][j] = x[3 + 3 * k + j];
	lm_residuals(samples, n, x, r);
	*cost = mean_cost(r, n);
	free(r);
	free(r_new);
	free(J);
	return 0;
}
