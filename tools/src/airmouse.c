/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * airmouse.c - the userspace airmouse engine (portable).  See airmouse.h.
 *
 * The formulas below replicate cmd_imu.c:39-47 and 263-265 from v1
 * exactly - including the (int) cast truncation - so `imu --mouse`
 * output stays byte-identical after the extraction.
 */
#include "airmouse.h"

void airmouse_init(struct airmouse *a, double alpha, double scale)
{
	a->alpha = alpha;
	a->scale = scale;
	a->prev[0] = a->prev[1] = a->prev[2] = 0.0;
}

void airmouse_process(struct airmouse *a, const double g[3], double out[3],
		      int *dx, int *dy)
{
	out[0] = a->alpha * g[0] + (1.0 - a->alpha) * a->prev[0];
	out[1] = a->alpha * g[1] + (1.0 - a->alpha) * a->prev[1];
	out[2] = a->alpha * g[2] + (1.0 - a->alpha) * a->prev[2];
	a->prev[0] = out[0];
	a->prev[1] = out[1];
	a->prev[2] = out[2];
	*dx = (int)(-out[2] * a->scale);
	*dy = (int)(-out[1] * a->scale);
}
