/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * daemon_config.h - lgmagicd configuration (Linux only).
 *
 * Two roots, both overridable for tests:
 *   config_root (/etc/lgmagic):     config.toml + devices.d/<MAC>.toml
 *   state_dir   (/var/lib/lgmagic): state.toml + <MAC>/calibration.json
 *
 * Per-remote precedence: built-in defaults < devices.d/<MAC>.toml < the
 * active-profile override in state.toml.  The daemon never reads
 * ~/.config - it runs as root and must not read user files.
 */
#ifndef LG_TOOLS_DAEMON_CONFIG_H
#define LG_TOOLS_DAEMON_CONFIG_H

#include <stddef.h>

#include "config.h"
#include "profiles.h"

struct daemon_config {
	struct config *global;		/* config_root/config.toml */
	char config_root[4096];
	char state_dir[4096];
};

/* Load the daemon config. Returns 0 / -1 with err (OOM only - a missing
 * or broken config.toml falls back to the defaults with a warning). */
int daemon_config_init(struct daemon_config *dc, const char *config_root,
		       const char *state_dir, char *err, size_t errsz);

void daemon_config_free(struct daemon_config *dc);

/* Resolve the per-remote configuration for identity ("AA:BB:.." or
 * "unknown"): devices.d/<identity>.toml on top of the defaults, then the
 * active-profile override from state.toml.  out must be fresh; the caller
 * frees it with device_config_free().  Returns 0 / -1 with err. */
int daemon_config_load_remote(const struct daemon_config *dc,
			      const char *identity, struct device_config *out,
			      char *err, size_t errsz);

/* Persist the active profile for identity in state.toml.  Atomic
 * (tmp + fsync + rename); other device entries are preserved.
 * Returns 0 / -1 with err. */
int daemon_config_save_profile(const struct daemon_config *dc,
			       const char *identity, const char *profile,
			       char *err, size_t errsz);

/* Persist the whole device config for identity as
 * config_root/devices.d/<identity>.toml (directory created on demand).
 * The file is re-serialized from d (comments are not preserved).
 * Atomic (tmp + fsync + rename).  Returns 0 / -1 with err. */
int daemon_config_save_device(const struct daemon_config *dc,
			      const struct device_config *d,
			      const char *identity, char *err, size_t errsz);

/* The calibration JSON path for a remote: d->calib when set, else
 * <state_dir>/<identity>/calibration.json. */
void daemon_config_calib_path(const struct daemon_config *dc,
			      const struct device_config *d,
			      const char *identity, char *out, size_t outsz);

#endif /* LG_TOOLS_DAEMON_CONFIG_H */
