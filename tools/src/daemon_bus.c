/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * daemon_bus.c - lgmagicd sd-bus interface + polkit (Linux only).
 *
 * One object (org.lgmagic / /org/lgmagic/Manager / org.lgmagic.Manager)
 * with the read methods unprivileged and the write methods gated by
 * polkitd CheckAuthorization (ratbagd pattern):
 *
 *   set-profile  SetProfile, SetScrollSpeed, SetSensitivity
 *   set-config   MapButton, ResetButtons, SetCalibPath
 *   reload       Reload
 *
 * Polkit fails closed: no polkitd / a failed call -> NotAuthorized.
 * uid 0 is authorized unconditionally (the e2e and the rescue path
 * rely on it).  Every mutation persists first (atomic write) and only
 * then applies to the live pipeline; on a write error the in-memory
 * config is rolled back from disk.
 *
 * The daemon drives the bus manually from its poll() loop; the bus is
 * optional - daemon_bus_open returns NULL and the daemon runs busless
 * when the system bus is unavailable (the busless e2e mode).
 */
#include "daemon_bus.h"

#include "keymap.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* reply helpers                                                       */
/* ------------------------------------------------------------------ */

/* Send an error reply with a formatted message.  Returns 1: a vtable
 * handler that has sent a reply itself must return nonzero, otherwise
 * sd-bus (bus_process_object's tail, v255 bus-objects.c:1456) emits an
 * extra UnknownMethod error after ours. */
static int fail(sd_bus_message *m, const char *errname, const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	(void)sd_bus_reply_method_errorf(m, errname, "%s", buf);
	return 1;
}

/* Send an empty success reply; returns 1 (see fail()). */
static int ok(sd_bus_message *m)
{
	(void)sd_bus_reply_method_return(m, "");
	return 1;
}

static struct daemon_remote *find_remote(struct daemon_devices *dd,
					 const char *mac)
{
	size_t i;

	for (i = 0; i < dd->nremotes; i++)
		if (strcmp(dd->remotes[i].identity, mac) == 0)
			return &dd->remotes[i];
	return NULL;
}

/* Re-apply the remote's configuration after a mutation (or rollback);
 * pipeline failures are logged, not fatal - the bus reply describes the
 * persistence step only. */
static void remote_apply(struct daemon_remote *r, struct daemon_devices *dd)
{
	char calib[4096], err[256];

	daemon_config_calib_path(dd->config, &r->dc, r->identity, calib,
				 sizeof(calib));
	if (pipeline_configure(&r->pl, &r->dc, calib,
			       dd->config->global,
			       r->kbd_uinput, err, sizeof(err)) < 0)
		fprintf(stderr, "lgmagicd: %s: %s\n", r->identity, err);
}

/* Roll the mutated in-memory config back to what is on disk. */
static void remote_rollback(struct daemon_remote *r, struct daemon_devices *dd)
{
	char err[256];
	struct device_config fresh;

	device_config_init(&fresh);
	if (daemon_config_load_remote(dd->config, r->identity, &fresh,
				      err, sizeof(err)) < 0) {
		/* keep the defaults - better than a half-mutated state */
		fprintf(stderr, "lgmagicd: %s: rollback failed: %s\n",
			r->identity, err);
	}
	device_config_free(&r->dc);
	r->dc = fresh;
	remote_apply(r, dd);
}

/* Mutate the remote's config, persist it, and on persistence failure
 * roll back.  mutate(dc, ctx) returns 0 / -1; its error is not
 * forwarded (the caller replies Failed with the write error). */
typedef int (*mutate_fn)(struct device_config *dc, void *ctx);

static int mutate_and_save(struct daemon_remote *r, struct daemon_devices *dd,
			   mutate_fn mutate, void *ctx, char *err, size_t errsz)
{
	if (mutate(&r->dc, ctx) < 0) {
		snprintf(err, errsz, "out of memory");
		return -1;
	}
	if (daemon_config_save_device(dd->config, &r->dc, r->identity,
				      err, errsz) < 0) {
		remote_rollback(r, dd);
		return -1;
	}
	remote_apply(r, dd);
	return 0;
}

/* ------------------------------------------------------------------ */
/* polkit                                                              */
/* ------------------------------------------------------------------ */

/* Field 22 of /proc/PID/stat: the start time in clock ticks, in the
 * kernel's own units - exactly what polkit's unix-process subject
 * wants.  libsystemd dropped SD_BUS_CREDS_PID_STARTTIME, so read it
 * from procfs instead of the creds. */
static int read_pid_starttime(pid_t pid, uint64_t *starttime)
{
	char buf[1024], path[64], *p, *end;
	ssize_t n;
	int fd, i;

	snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	p = strrchr(buf, ')');	/* the comm may contain spaces */
	if (!p || p[1] != ' ')
		return -1;
	p += 2;			/* field 3 is "state", a letter */
	p += 2;			/* skip it and the space after it */
	for (i = 0; i < 19; i++) {	/* fields 4..22 are numeric, the last
					 * one (starttime) is what we want */
		*starttime = (uint64_t)strtoull(p, &end, 10);
		if (end == p)
			return -1;
		p = end;
	}
	return 0;
}

/* CheckAuthorization for the caller of m.  Returns 1 = authorized,
 * 0 = denied by polkit, -1 = cannot decide (fail closed). */
static int polkit_authorized(sd_bus_message *m, const char *action)
{
	sd_bus *bus = sd_bus_message_get_bus(m);
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	const char *sender = sd_bus_message_get_sender(m);
	uint32_t uid = 0, pid = 0;
	uint64_t starttime = 0;
	int have_uid = 0, have_pid = 0, have_start = 0;
	int is_auth = 0, is_challenge = 0;
	int r;

	/* sd_bus_query_sender_creds() on v255 does not fetch the UID
	 * (sd_bus_creds_get_uid() -> -ENODATA, only
	 * GetConnectionUnixProcessID is sent), so ask the broker
	 * explicitly - GetConnectionCredentials always carries
	 * UnixUserID, ProcessID and ProcessStartTime. */
	r = sd_bus_call_method(bus,
			       "org.freedesktop.DBus", "/org/freedesktop/DBus",
			       "org.freedesktop.DBus",
			       "GetConnectionCredentials", &error, &reply,
			       "s", sender);
	if (r < 0) {
		fprintf(stderr,
			"lgmagicd: polkit: GetConnectionCredentials(%s): %s\n",
			sender, error.message);
		sd_bus_error_free(&error);
		return -1;
	}
	sd_bus_error_free(&error);

	r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{sv}");
	if (r < 0)
		goto parse_fail;
	while ((r = sd_bus_message_enter_container(reply,
						  SD_BUS_TYPE_DICT_ENTRY,
						  "sv")) > 0) {
		const char *key;

		r = sd_bus_message_read(reply, "s", &key);
		if (r < 0)
			break;
		if (strcmp(key, "UnixUserID") == 0) {
			r = sd_bus_message_read(reply, "v", "u", &uid);
			have_uid = r >= 0;
		} else if (strcmp(key, "ProcessID") == 0) {
			r = sd_bus_message_read(reply, "v", "u", &pid);
			have_pid = r >= 0;
		} else if (strcmp(key, "ProcessStartTime") == 0) {
			r = sd_bus_message_read(reply, "v", "t", &starttime);
			have_start = r >= 0;
		} else
			r = sd_bus_message_skip(reply, "v");
		if (r < 0)
			break;
		r = sd_bus_message_exit_container(reply);
		if (r < 0)
			break;
	}
	if (r < 0)
		goto parse_fail;
	sd_bus_message_unref(reply);

	if (!have_uid || !have_pid) {
		fprintf(stderr, "lgmagicd: polkit: no uid/pid for %s\n",
			sender);
		return -1;
	}
	if (uid == 0)
		return 1;	/* root bypasses polkit (the e2e relies on it) */
	/* only the polkit subject for a non-root caller needs the start
	 * time (dbus-daemon 1.14 does not report ProcessStartTime, so it
	 * usually comes from procfs here) */
	if (!have_start && read_pid_starttime((pid_t)pid, &starttime) < 0) {
		fprintf(stderr, "lgmagicd: polkit: no start time for pid %u\n",
			(unsigned)pid);
		return -1;
	}

	/* details is a{ss} in the CheckAuthorization signature (the
	 * reply carries a{sv}); a{sv} here makes the broker reject the
	 * message - the daemon would fail closed for everyone. */
	r = sd_bus_call_method(bus, "org.freedesktop.PolicyKit1",
			       "/org/freedesktop/PolicyKit1/Authority",
			       "org.freedesktop.PolicyKit1.Authority",
			       "CheckAuthorization", &error, &reply,
			       "(sa{sv})sa{ss}us",
			       "unix-process", 3,
			       "pid", "u", pid,
			       "start-time", "t", starttime,
			       "uid", "u", uid,
			       action, 0, 0, "");
	if (r < 0) {
		fprintf(stderr, "lgmagicd: polkit: CheckAuthorization(%s): %s\n",
			action, error.message);
		sd_bus_error_free(&error);
		return -1;	/* polkitd missing or the call failed */
	}
	sd_bus_error_free(&error);
	r = sd_bus_message_read(reply, "bb", &is_auth, &is_challenge);
	sd_bus_message_unref(reply);
	if (r < 0)
		return -1;
	return (is_auth && !is_challenge) ? 1 : 0;

parse_fail:
	sd_bus_message_unref(reply);
	return -1;
}

/* Gate a write method: returns 0 when authorized, otherwise sends the
 * NotAuthorized reply and returns 1 (the handler must return 1 then,
 * see fail()). */
static int gate(sd_bus_message *m, const char *action)
{
	switch (polkit_authorized(m, action)) {
	case 1:
		return 0;
	case 0:
		fail(m, LG_ERROR_NOT_AUTHORIZED, "permission denied by polkit");
		return 1;
	default:
		fail(m, LG_ERROR_NOT_AUTHORIZED,
		     "polkitd not running (refusing to authorize)");
		return 1;
	}
}

/* ------------------------------------------------------------------ */
/* methods                                                             */
/* ------------------------------------------------------------------ */

static int method_list_devices(sd_bus_message *m, void *userdata,
			       sd_bus_error *ret_error)
{
	struct daemon_devices *dd = userdata;
	sd_bus_message *reply;
	size_t i;
	int r;

	(void)ret_error;
	r = sd_bus_message_new_method_return(m, &reply);
	if (r < 0)
		return r;
	r = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "s");
	if (r >= 0) {
		for (i = 0; i < dd->nremotes && r >= 0; i++)
			r = sd_bus_message_append(reply, "s",
						  dd->remotes[i].identity);
		if (r >= 0)
			r = sd_bus_message_close_container(reply);
	}
	if (r < 0) {
		sd_bus_message_unref(reply);
		return r;
	}
	r = sd_bus_send(NULL, reply, NULL);
	sd_bus_message_unref(reply);
	return r;
}

static int method_get_status(sd_bus_message *m, void *userdata,
			     sd_bus_error *ret_error)
{
	struct daemon_devices *dd = userdata;
	const char *mac;
	struct daemon_remote *r;
	const struct pipeline *pl;
	char buf[1024], calib[4096];
	int n;

	(void)ret_error;
	if (sd_bus_message_read(m, "s", &mac) < 0)
		return -EINVAL;
	if (!daemon_identity_valid(mac))
		return fail(m, LG_ERROR_INVALID_ARGUMENTS,
			    "invalid device identity '%s'", mac);
	r = find_remote(dd, mac);
	if (!r)
		return fail(m, LG_ERROR_NOT_FOUND, "no device '%s'", mac);
	pl = &r->pl;
	daemon_config_calib_path(dd->config, &r->dc, r->identity, calib,
				 sizeof(calib));
	n = snprintf(buf, sizeof(buf),
		     "identity: %s\n"
		     "profile: %s\n"
		     "airmouse: %s\n"
		     "calibration: %s\n"
		     "calib_loaded: %s\n"
		     "keyboard: %s\n"
		     "imu: %s\n"
		     "scroll_speed: %.6g\n"
		     "sensitivity: %.6g\n"
		     "lpf_alpha: %.6g\n"
		     "buttons: %zu\n",
		     r->identity, r->dc.profile,
		     pl->airmouse_on ? "on" : "off",
		     calib, pl->have_cal ? "yes" : "no",
		     r->kbd_path, r->imu_path[0] ? r->imu_path : "-",
		     pl->active.scroll_speed, pl->active.sensitivity,
		     pl->active.has_lpf ? pl->active.lpf_alpha :
					 dd->config->global->lpf_alpha,
		     pl->active.nmap);
	if (n < 0 || (size_t)n >= sizeof(buf))
		return -ENOMEM;
	if (sd_bus_reply_method_return(m, "s", buf) < 0)
		return -errno;
	return 1;
}

static int method_set_profile(sd_bus_message *m, void *userdata,
			      sd_bus_error *ret_error)
{
	struct daemon_devices *dd = userdata;
	const char *mac, *profile;
	struct daemon_remote *r;
	char err[256];

	(void)ret_error;
	if (sd_bus_message_read(m, "ss", &mac, &profile) < 0)
		return -EINVAL;
	if (gate(m, "org.lgmagic.profile-set"))
		return 1;
	if (!daemon_identity_valid(mac))
		return fail(m, LG_ERROR_INVALID_ARGUMENTS,
			    "invalid device identity '%s'", mac);
	r = find_remote(dd, mac);
	if (!r)
		return fail(m, LG_ERROR_NOT_FOUND, "no device '%s'", mac);
	/* "default" is always usable (built-in); any other name must be
	 * defined in the device file. */
	if (strcmp(profile, "default") != 0 && !profile_find(&r->dc, profile))
		return fail(m, LG_ERROR_INVALID_ARGUMENTS,
			    "profile '%s' is not defined for '%s'", profile, mac);
	if (daemon_config_save_profile(dd->config, mac, profile,
				       err, sizeof(err)) < 0)
		return fail(m, LG_ERROR_FAILED, "%s", err);
	/* The dc carries the active profile resolved from state.toml at
	 * load time - reload it so the pipeline follows the new profile
	 * immediately (no daemon restart). */
	device_config_free(&r->dc);
	device_config_init(&r->dc);
	if (daemon_config_load_remote(dd->config, r->identity, &r->dc,
				      err, sizeof(err)) < 0)
		fprintf(stderr, "lgmagicd: %s: profile reload failed: %s\n",
			r->identity, err);
	remote_apply(r, dd);
	return ok(m);
}

struct map_ctx {
	int from, to;
};

static int mutate_map(struct device_config *dc, void *ctx)
{
	struct map_ctx *mc = ctx;
	struct profile *p = device_config_profile_ensure(dc, dc->profile);

	if (!p)
		return -1;
	return profile_set_button(p, mc->from, mc->to);
}

static int method_map_button(sd_bus_message *m, void *userdata,
			     sd_bus_error *ret_error)
{
	struct daemon_devices *dd = userdata;
	const char *mac, *from, *to;
	struct daemon_remote *r;
	struct map_ctx mc;
	char err[256];

	(void)ret_error;
	if (sd_bus_message_read(m, "sss", &mac, &from, &to) < 0)
		return -EINVAL;
	mc.from = keymap_name_to_code(from);
	mc.to = keymap_name_to_code(to);
	if (mc.from < 0 || mc.to < 0)
		return fail(m, LG_ERROR_INVALID_ARGUMENTS,
			    "unknown keycode ('%s' or '%s')", from, to);
	if (gate(m, "org.lgmagic.modify-input"))
		return 1;
	if (!daemon_identity_valid(mac))
		return fail(m, LG_ERROR_INVALID_ARGUMENTS,
			    "invalid device identity '%s'", mac);
	r = find_remote(dd, mac);
	if (!r)
		return fail(m, LG_ERROR_NOT_FOUND, "no device '%s'", mac);
	if (mutate_and_save(r, dd, mutate_map, &mc, err, sizeof(err)) < 0)
		return fail(m, LG_ERROR_FAILED, "%s", err);
	return ok(m);
}

static int mutate_reset(struct device_config *dc, void *ctx)
{
	const struct profile *found;
	struct profile *p;

	(void)ctx;
	/* Only a profile actually present in the file has something to
	 * reset (an implicit built-in "default" has nothing persisted). */
	found = profile_find(dc, dc->profile);
	if (!found)
		return 0;
	p = device_config_profile_ensure(dc, dc->profile);
	if (!p)
		return -1;
	profile_clear_buttons(p);
	return 0;
}

static int method_reset_buttons(sd_bus_message *m, void *userdata,
				sd_bus_error *ret_error)
{
	struct daemon_devices *dd = userdata;
	const char *mac;
	struct daemon_remote *r;
	char err[256];

	(void)ret_error;
	if (sd_bus_message_read(m, "s", &mac) < 0)
		return -EINVAL;
	if (gate(m, "org.lgmagic.modify-input"))
		return 1;
	if (!daemon_identity_valid(mac))
		return fail(m, LG_ERROR_INVALID_ARGUMENTS,
			    "invalid device identity '%s'", mac);
	r = find_remote(dd, mac);
	if (!r)
		return fail(m, LG_ERROR_NOT_FOUND, "no device '%s'", mac);
	if (mutate_and_save(r, dd, mutate_reset, NULL, err, sizeof(err)) < 0)
		return fail(m, LG_ERROR_FAILED, "%s", err);
	return ok(m);
}

struct num_ctx {
	int is_scroll;
	double v;
};

static int mutate_number(struct device_config *dc, void *ctx)
{
	struct num_ctx *nc = ctx;
	struct profile *p = device_config_profile_ensure(dc, dc->profile);

	if (!p)
		return -1;
	if (nc->is_scroll) {
		p->scroll_speed = nc->v;
		p->has_scroll = 1;
	} else {
		p->sensitivity = nc->v;
		p->has_sens = 1;
	}
	return 0;
}

static int method_set_number(sd_bus_message *m, void *userdata, int is_scroll,
			     double lo, double hi, const char *what)
{
	struct daemon_devices *dd = userdata;
	const char *mac;
	struct daemon_remote *r;
	struct num_ctx nc;
	double v;
	char err[256];

	if (sd_bus_message_read(m, "sd", &mac, &v) < 0)
		return -EINVAL;
	if (gate(m, "org.lgmagic.profile-set"))
		return 1;
	if (!daemon_identity_valid(mac))
		return fail(m, LG_ERROR_INVALID_ARGUMENTS,
			    "invalid device identity '%s'", mac);
	r = find_remote(dd, mac);
	if (!r)
		return fail(m, LG_ERROR_NOT_FOUND, "no device '%s'", mac);
	if (v < lo || v > hi)
		return fail(m, LG_ERROR_INVALID_ARGUMENTS,
			    "%s out of range (%.6g..%.6g)", what, lo, hi);
	nc.is_scroll = is_scroll;
	nc.v = v;
	if (mutate_and_save(r, dd, mutate_number, &nc, err, sizeof(err)) < 0)
		return fail(m, LG_ERROR_FAILED, "%s", err);
	return ok(m);
}

static int method_set_scroll_speed(sd_bus_message *m, void *userdata,
				   sd_bus_error *ret_error)
{
	(void)ret_error;
	return method_set_number(m, userdata, 1, 0.0, 100.0, "scroll_speed");
}

static int method_set_sensitivity(sd_bus_message *m, void *userdata,
				  sd_bus_error *ret_error)
{
	(void)ret_error;
	return method_set_number(m, userdata, 0, 0.0, 10000.0, "sensitivity");
}

static int mutate_calib(struct device_config *dc, void *ctx)
{
	const char *path = ctx;
	char *s = NULL;

	if (path[0]) {
		s = strdup(path);
		if (!s)
			return -1;
	}
	free(dc->calib);
	dc->calib = s;		/* NULL restores the per-device default */
	return 0;
}

static int method_set_calib_path(sd_bus_message *m, void *userdata,
				 sd_bus_error *ret_error)
{
	struct daemon_devices *dd = userdata;
	const char *mac, *path;
	struct daemon_remote *r;
	char err[256];

	(void)ret_error;
	if (sd_bus_message_read(m, "ss", &mac, &path) < 0)
		return -EINVAL;
	if (gate(m, "org.lgmagic.modify-input"))
		return 1;
	if (!daemon_identity_valid(mac))
		return fail(m, LG_ERROR_INVALID_ARGUMENTS,
			    "invalid device identity '%s'", mac);
	r = find_remote(dd, mac);
	if (!r)
		return fail(m, LG_ERROR_NOT_FOUND, "no device '%s'", mac);
	if (mutate_and_save(r, dd, mutate_calib, (void *)path, err, sizeof(err)) < 0)
		return fail(m, LG_ERROR_FAILED, "%s", err);
	return ok(m);
}

static int method_reload(sd_bus_message *m, void *userdata,
			 sd_bus_error *ret_error)
{
	struct daemon_devices *dd = userdata;
	char err[256];

	(void)ret_error;
	if (gate(m, "org.lgmagic.modify-input"))
		return 1;
	if (daemon_devices_reload(dd, err, sizeof(err)) < 0)
		return fail(m, LG_ERROR_FAILED, "%s", err);
	return ok(m);
}

/* ------------------------------------------------------------------ */
/* properties                                                          */
/* ------------------------------------------------------------------ */

static int property_get_state(sd_bus *bus, const char *path,
			      const char *interface, const char *property,
			      sd_bus_message *reply, void *userdata,
			      sd_bus_error *ret_error)
{
	(void)bus; (void)path; (void)interface; (void)property;
	(void)userdata; (void)ret_error;
	return sd_bus_message_append(reply, "s", "running");
}

static int property_get_api_version(sd_bus *bus, const char *path,
				    const char *interface,
				    const char *property,
				    sd_bus_message *reply, void *userdata,
				    sd_bus_error *ret_error)
{
	(void)bus; (void)path; (void)interface; (void)property;
	(void)userdata; (void)ret_error;
	return sd_bus_message_append(reply, "s", LG_API_VERSION);
}

static int property_get_devices(sd_bus *bus, const char *path,
				const char *interface, const char *property,
				sd_bus_message *reply, void *userdata,
				sd_bus_error *ret_error)
{
	struct daemon_devices *dd = userdata;
	size_t i;
	int r;

	(void)bus; (void)path; (void)interface; (void)property; (void)ret_error;
	r = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "s");
	if (r < 0)
		return r;
	for (i = 0; i < dd->nremotes && r >= 0; i++)
		r = sd_bus_message_append(reply, "s", dd->remotes[i].identity);
	if (r >= 0)
		r = sd_bus_message_close_container(reply);
	return r;
}

static const sd_bus_vtable lgmagic_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("ListDevices", "", "as", method_list_devices,
		      SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("GetStatus", "s", "s", method_get_status,
		      SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("SetProfile", "ss", "", method_set_profile,
		      SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("MapButton", "sss", "", method_map_button,
		      SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("ResetButtons", "s", "", method_reset_buttons,
		      SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("SetScrollSpeed", "sd", "", method_set_scroll_speed,
		      SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("SetSensitivity", "sd", "", method_set_sensitivity,
		      SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("SetCalibPath", "ss", "", method_set_calib_path,
		      SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Reload", "", "", method_reload,
		      SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("State", "s", property_get_state, 0,
			SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("Devices", "as", property_get_devices, 0,
			SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("ApiVersion", "s", property_get_api_version, 0,
			SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END
};

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

sd_bus *daemon_bus_open(struct daemon_devices *dd, char *err, size_t errsz)
{
	sd_bus *bus;
	int r;

	r = sd_bus_open_system(&bus);
	if (r < 0) {
		snprintf(err, errsz, "cannot open the system bus: %s",
			 strerror(-r));
		return NULL;
	}
	r = sd_bus_add_object_vtable(bus, NULL, LG_BUS_PATH, LG_BUS_IFACE,
				     lgmagic_vtable, dd);
	if (r < 0)
		goto fail;
	r = sd_bus_request_name(bus, LG_BUS_NAME, 0);
	if (r < 0)
		goto fail;
	return bus;

fail:
	snprintf(err, errsz, "bus setup failed: %s", strerror(-r));
	sd_bus_unref(bus);
	return NULL;
}

int daemon_bus_fd(sd_bus *bus)
{
	int fd = sd_bus_get_fd(bus);

	if (fd < 0)
		return -1;
	return fd;
}

int daemon_bus_events(sd_bus *bus)
{
	int events = sd_bus_get_events(bus);

	if (events < 0)
		return POLLIN;	/* a dead bus is detected by process */
	/* systemd v255 reports 0 on a running bus with empty queues; the
	 * socket must still be watched for incoming calls (POLLOUT stays
	 * conditional - unconditional level-triggered POLLOUT would make
	 * poll() return immediately forever) */
	if (events == 0)
		events = POLLIN;
	return events;
}

int daemon_bus_process(sd_bus *bus, int revents)
{
	int r = 0;

	if (revents & POLLOUT) {
		r = sd_bus_flush(bus);
		if (r < 0)
			return -1;
	}
	if (revents & (POLLIN | POLLHUP | POLLERR)) {
		r = sd_bus_process(bus, NULL);
		if (r < 0)
			return -1;
	}
	return 0;
}

void daemon_bus_close(sd_bus *bus)
{
	if (bus)
		sd_bus_unref(bus);
}
