/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * daemon_pipeline.c - lgmagicd frame pipeline.
 * See daemon_pipeline.h.
 */
#include "daemon_pipeline.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "uinput.h"

void pipeline_init(struct pipeline *p)
{
	memset(p, 0, sizeof(*p));
	p->gate_logged = -1;
	calib_init_identity(&p->cal);
	profile_init(&p->active, "resolved");
}

void pipeline_free(struct pipeline *p)
{
	profile_free(&p->active);
}

int pipeline_configure(struct pipeline *p, const struct device_config *dc,
		       const char *calib_path, const struct config *gcfg,
		       int kbd_uinput, char *err, size_t errsz)
{
	const struct profile *src;
	struct calib cal;
	double alpha;

	/* Release held keys before the map changes: a remap while a key
	 * is down must not leave the previously pressed virtual key
	 * stuck forever (the later physical release repeats the ORIGINAL
	 * code, so nothing would release the old one). */
	if (kbd_uinput >= 0) {
		for (; p->nheld > 0; p->nheld--)
			uinput_key(kbd_uinput, p->held[p->nheld - 1].virt, 0);
	}

	/* Active profile: state/device name -> "default" -> built-ins.
	 * Reset first so a re-configure does not leak values that were
	 * removed from the config files. */
	src = profile_find(dc, dc->profile);
	if (!src)
		src = profile_find(dc, "default");
	profile_free(&p->active);
	profile_init(&p->active, "resolved");
	if (src && profile_merge(&p->active, src) < 0) {
		snprintf(err, errsz, "out of memory");
		return -1;
	}

	/* Calibration: reload on every (re)configure.  A broken file is an
	 * error the caller logs, but the PREVIOUS calibration stays live
	 * (the e2e asserts exactly this: bad calib rejected, old one
	 * kept); only an explicit "" path resets to identity. */
	if (calib_path && calib_path[0]) {
		if (calib_load(calib_path, &cal, err, errsz) < 0)
			return -1;
		p->cal = cal;
		p->have_cal = 1;
	} else {
		p->have_cal = 0;
	}

	/* Air mouse: profile lpf_alpha when set, else the global config. */
	alpha = p->active.has_lpf ? p->active.lpf_alpha : gcfg->lpf_alpha;
	airmouse_init(&p->am, alpha, p->active.sensitivity);
	airmouse_gate_cfg(&p->am, gcfg->accel_gate, gcfg->accel_gate_lo,
			  gcfg->accel_gate_hi);
	p->airmouse_on = dc->airmouse;
	return 0;
}

int pipeline_keyboard(struct pipeline *p, const struct evdev_frame *f,
		      int kbd_fd, int mouse_fd)
{
	int i, rc = 0;

	for (i = 0; i < f->nkeys; i++) {
		int phys = f->keys[i].code, val = f->keys[i].value;
		int to = phys, held_idx = -1;
		int k;

		for (k = 0; k < p->nheld; k++)
			if (p->held[k].phys == phys)
				held_idx = (int)k;
		if (held_idx >= 0) {
			/* Repeat AND release: the code the press was
			 * mapped AS - the map may have changed while
			 * held, and neither may start a different
			 * virtual key. */
			to = p->held[held_idx].virt;
			if (val == 0) {
				p->held[held_idx] = p->held[p->nheld - 1];
				p->nheld--;
			}
		} else if (val != 0) {
			/* Fresh press: resolve the current map and pin
			 * it for the future repeat/release. */
			for (k = 0; k < (int)p->active.nmap; k++) {
				if (p->active.map[k].from == phys) {
					to = p->active.map[k].to;
					break;
				}
			}
			if (p->nheld <
			    (int)(sizeof(p->held) / sizeof(p->held[0]))) {
				p->held[p->nheld].phys = phys;
				p->held[p->nheld].virt = to;
				p->nheld++;
			}
		} else {
			/* Release of an untracked key (pressed before the
			 * daemon took over): emit the current mapping -
			 * harmless when the virtual key is already up. */
			for (k = 0; k < (int)p->active.nmap; k++) {
				if (p->active.map[k].from == phys) {
					to = p->active.map[k].to;
					break;
				}
			}
		}
		if (uinput_key(kbd_fd, to, val) < 0)
			rc = -1;
	}

	/* Wheel: fractional accumulator, integer clicks out (C cast
	 * truncates towards zero, like the v1 movement formula). */
	if (f->wheel) {
		int clicks;

		p->wheel_accum += (double)f->wheel * p->active.scroll_speed;
		clicks = (int)p->wheel_accum;
		p->wheel_accum -= clicks;
		if (clicks && uinput_scroll(mouse_fd, clicks, clicks * 120) < 0)
			rc = -1;
	}
	return rc;
}

int pipeline_imu(struct pipeline *p, const struct evdev_frame *f,
		 int mouse_fd)
{
	double a_raw[3], g_raw[3], a_out[3], g_out[3], filt[3];
	int dx, dy;

	if (!p->airmouse_on)
		return 0;
	a_raw[0] = f->accel[0];
	a_raw[1] = f->accel[1];
	a_raw[2] = f->accel[2];
	g_raw[0] = f->gyro[0];
	g_raw[1] = f->gyro[1];
	g_raw[2] = f->gyro[2];
	if (p->have_cal)
		calib_apply(&p->cal, a_raw, g_raw, a_out, g_out);
	else {
		memcpy(g_out, g_raw, sizeof(g_out));
		a_out[0] = a_raw[0];
		a_out[1] = a_raw[1];
		a_out[2] = a_raw[2];
	}
	/* The gate works on the RAW accelerometer: lo/hi are raw counts
	 * (calibration scale would change their meaning). */
	airmouse_process(&p->am, g_out, a_raw, filt, &dx, &dy);
	if (dx || dy)
		return uinput_move(mouse_fd, dx, dy) < 0 ? -1 : 0;
	return 0;
}
