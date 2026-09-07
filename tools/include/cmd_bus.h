/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cmd_bus.h - shared plumbing for the CLI subcommands (portable).
 *
 * The state-changing commands (device/profile/button/scroll) talk to
 * lg-magicd over the system bus with the zero-dependency dbus_client;
 * the read-only ones may load the world-readable device files
 * directly.  This header holds the common connect/call/resolve/load
 * helpers so that error reporting ("not authorized", "daemon not
 * running") stays identical across the commands.
 */
#ifndef LG_TOOLS_CMD_BUS_H
#define LG_TOOLS_CMD_BUS_H

#include "dbus_client.h"
#include "profiles.h"

/* Connect to the system bus.  On failure prints the friendly
 * daemon-not-running hint and returns -1. */
int cmd_bus_connect(struct dbus_client *c);

/* One call.  On failure prints the error (NotAuthorized -> "permission
 * denied (polkit)") and returns -1; the caller exits 1.  On success
 * *err_name and *err_msg are NULL and *out holds the reply (the caller
 * frees it with dbus_value_free). */
int cmd_bus_call(struct dbus_client *c, const char *method, const char *sig,
		 const struct dbus_arg *args, char **err_name, char **err_msg,
		 struct dbus_value **out);

/* Resolve an optional MAC argument: a non-NULL `given` wins; otherwise
 * the daemon's ListDevices must return exactly one device.  Fills buf;
 * returns 0 / -1 (message printed). */
int cmd_bus_resolve_mac(struct dbus_client *c, const char *given,
			char *buf, size_t bufsz);

/* The production config roots, overridable for tests via
 * LG_MAGIC_CONFIG_ROOT / LG_MAGIC_STATE_DIR. */
const char *cmd_config_root(void);
const char *cmd_state_dir(void);

/* Load the resolved device config for mac: devices.d/<mac>.toml on top
 * of the defaults, then the active-profile overlay from state.toml.
 * Returns 0 / -1 with err (dc is always initialised). */
int cmd_load_device(const char *mac, struct device_config *dc,
		    char *err, size_t errsz);

#endif /* LG_TOOLS_CMD_BUS_H */
