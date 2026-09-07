/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * daemon_pipeline.h - lg-magicd frame pipeline (Linux only).
 *
 * Keyboard frames: EV_KEY through the active profile's button map (an
 * unmapped key passes through unchanged), REL_WHEEL through the scroll
 * accumulator (wheel * scroll_speed, integer clicks out).
 * IMU frames: calibration, then the v1 airmouse engine (shared with
 * `imu --mouse`); REL_X/REL_Y on the virtual mouse.
 *
 * The pipeline is pure state + emitters - no I/O of its own besides the
 * uinput writes - so the fake-device e2e can verify it end to end.
 */
#ifndef LG_TOOLS_DAEMON_PIPELINE_H
#define LG_TOOLS_DAEMON_PIPELINE_H

#include "airmouse.h"
#include "calib.h"
#include "evdev.h"
#include "profiles.h"

struct pipeline {
	double wheel_accum;	/* fractional wheel clicks not yet emitted */
	struct profile active;	/* resolved active profile (owned copy) */
	struct calib cal;
	int have_cal;		/* a calibration JSON was loaded */
	int airmouse_on;
	struct airmouse am;
	/* Keys currently held down on the physical remote: the virtual
	 * code they were pressed AS.  A map change must release these
	 * (otherwise the old virtual key stays stuck), and a release
	 * must repeat the ORIGINAL virtual code - the mapping may have
	 * changed since the press. */
	struct {
		int phys;	/* physical code (the press side) */
		int virt;	/* virtual code emitted on press */
	} held[32];
	int nheld;
};

void pipeline_init(struct pipeline *p);
void pipeline_free(struct pipeline *p);

/* (Re)apply the resolved configuration: pick the active profile
 * (dc->profile, falling back to "default" and then the built-in
 * defaults), reload the calibration from calib_path ("" = none), set
 * the airmouse on/off.  A broken calibration file is an error (err set)
 * but the pipeline stays usable without it.  Held keys are released on
 * kbd_uinput first (a remap must not leave the old virtual key stuck).
 * Returns 0 / -1. */
int pipeline_configure(struct pipeline *p, const struct device_config *dc,
		       const char *calib_path, double global_lpf,
		       int kbd_uinput, char *err, size_t errsz);

/* One keyboard frame: map + emit keys on kbd_fd, accumulate the wheel
 * and emit whole clicks on mouse_fd.  Returns 0, or -1 when a uinput
 * write failed (errno set by the emitter). */
int pipeline_keyboard(struct pipeline *p, const struct evdev_frame *f,
		      int kbd_fd, int mouse_fd);

/* One IMU frame: calibrate + airmouse; moves the mouse when enabled.
 * Returns 0, or -1 when a uinput write failed (errno set). */
int pipeline_imu(struct pipeline *p, const struct evdev_frame *f,
		 int mouse_fd);

#endif /* LG_TOOLS_DAEMON_PIPELINE_H */
