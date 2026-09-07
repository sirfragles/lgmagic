/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * profiles.h - per-device profiles and button maps (portable).
 *
 * The daemon's per-remote configuration comes from
 * /etc/lgmagic/devices.d/<MAC>.toml:
 *
 *     profile = "default"                     # active profile
 *     calib = "/path/to/calibration.json"     # optional override
 *     airmouse = true                         # enable the airmouse
 *
 *     [profiles.default]
 *     scroll_speed = 1.0
 *     sensitivity = 30.0
 *     lpf_alpha = 0.2                         # optional; unset = global
 *
 *     [profiles.default.button_map]
 *     "KEY_ENTER" = "BTN_LEFT"                # keycode -> keycode
 *
 * Precedence (resolved by the daemon): built-in defaults < the device
 * file < the active-profile override in state.toml.
 */
#ifndef LG_TOOLS_PROFILES_H
#define LG_TOOLS_PROFILES_H

#include <stddef.h>

struct toml_value;

struct button_map_entry {
	int from;		/* keycode produced by the kernel */
	int to;			/* keycode emitted by the daemon */
};

struct profile {
	char *name;			/* owned */
	double scroll_speed;		/* wheel multiplier (1.0) */
	double sensitivity;		/* airmouse pointer speed (30.0) */
	double lpf_alpha;		/* -1 = unset (use the global config) */
	int has_scroll;			/* scroll_speed was set explicitly */
	int has_sens;			/* sensitivity was set explicitly */
	int has_lpf;			/* lpf_alpha was set explicitly */
	struct button_map_entry *map;
	size_t nmap;
};

struct device_config {
	char *profile;			/* active profile name (owned) */
	char *calib;			/* calibration path, NULL = default */
	int airmouse;			/* enable the airmouse */
	int has_airmouse;		/* the key was present in the file */
	struct profile *profiles;
	size_t nprofiles;
};

/* Fill a profile with the built-in defaults (lpf_alpha unset). */
int profile_init(struct profile *p, const char *name);
void profile_free(struct profile *p);

/* Add or replace a button mapping; returns 0 / -1 (OOM). */
int profile_set_button(struct profile *p, int from, int to);
void profile_clear_buttons(struct profile *p);

/* Validate one profile: ranges (scroll_speed 0..100, sensitivity
 * 0..10000, lpf_alpha 0..1 when set) and known keycodes in the map.
 * Returns 0 or -1 with err set. */
int profile_validate(const struct profile *p, char *err, size_t errsz);

/* Merge src into dst: numeric fields that src has set overwrite dst,
 * button entries are merged by `from`.  Returns 0 / -1 (OOM). */
int profile_merge(struct profile *dst, const struct profile *src);

/* Find a profile by name; NULL when absent. */
const struct profile *profile_find(const struct device_config *dc,
				   const char *name);

/* Fill dc with the built-in defaults (active profile "default",
 * airmouse enabled, no calib override).  Always succeeds. */
void device_config_init(struct device_config *dc);
void device_config_free(struct device_config *dc);

/* Parse a device TOML document into dc (which must be initialised);
 * unknown keys are ignored, malformed values produce an error.
 * Returns 0 / -1 with err set. */
int device_config_from_toml(const struct toml_value *root,
			    struct device_config *dc, char *err, size_t errsz);

/* Validate dc: the active profile exists and every profile is valid.
 * Returns 0 / -1 with err set. */
int device_config_validate(const struct device_config *dc, char *err,
			   size_t errsz);

/* Load a device TOML file into dc.  Returns 0 on success, 1 when the
 * file does not exist (dc keeps the defaults), -1 on parse/validation
 * errors (err set). */
int device_config_load_file(const char *path, struct device_config *dc,
			    char *err, size_t errsz);

/* Find the named profile in dc, creating it (with the built-in
 * defaults) when absent - the bus methods mutate the active profile,
 * which may not exist in the file yet.  Returns NULL on OOM. */
struct profile *device_config_profile_ensure(struct device_config *dc,
					     const char *name);

/* Serialize dc back to a TOML document (malloc'd tree, caller frees
 * with toml_free).  Only the values that were explicitly set are
 * written, so the daemon's save_device re-serializes the file it
 * manages (comments are not preserved).  NULL on OOM. */
struct toml_value *device_config_to_toml(const struct device_config *dc);

#endif /* LG_TOOLS_PROFILES_H */
