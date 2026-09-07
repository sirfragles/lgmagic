/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * daemon_bus.h - lgmagicd sd-bus interface (Linux only, links libsystemd).
 *
 * One object: org.lgmagic / /org/lgmagic/Manager / org.lgmagic.Manager.
 * Read methods (ListDevices, GetStatus, the properties) are unprivileged;
 * the write methods are gated by polkit:
 *   org.lgmagic.profile-set  SetProfile, SetScrollSpeed, SetSensitivity
 *   org.lgmagic.modify-input  MapButton, ResetButtons, SetCalibPath, Reload
 * Polkit fails closed (missing polkitd -> NotAuthorized); uid 0 is
 * authorized unconditionally (the e2e relies on it).
 *
 * Every method that takes a device identity validates it first
 * (daemon_identity_valid: "unknown" or a 17-char BT MAC) - the identity
 * reaches file paths, so arbitrary CLI input is InvalidArguments.  The
 * ApiVersion property lets clients check compatibility without probing.
 *
 * The daemon drives the bus manually from its poll() loop (the device
 * manager owns pollfds): daemon_bus_fd/daemon_bus_process.  A bus that
 * cannot be opened degrades to a busless daemon (the busless e2e mode).
 */
#ifndef LG_TOOLS_DAEMON_BUS_H
#define LG_TOOLS_DAEMON_BUS_H

#include <systemd/sd-bus.h>

#include "daemon_devices.h"

/* The bus name / object path / interface of the daemon. */
#define LG_BUS_NAME "org.lgmagic"
#define LG_BUS_PATH "/org/lgmagic/Manager"
#define LG_BUS_IFACE "org.lgmagic.Manager"

/* The API version advertised on the ApiVersion property (clients can
 * check it instead of guessing method availability). */
#define LG_API_VERSION "0.0"

/* D-Bus error names (org.lgmagic.Error.*). */
#define LG_ERROR_NOT_AUTHORIZED "org.lgmagic.Error.NotAuthorized"
#define LG_ERROR_NOT_FOUND "org.lgmagic.Error.NotFound"
#define LG_ERROR_INVALID_ARGUMENTS "org.lgmagic.Error.InvalidArguments"
#define LG_ERROR_FAILED "org.lgmagic.Error.Failed"

/* Open the system bus, install the object and request the name.  On
 * failure returns NULL with err set (the daemon continues busless). */
sd_bus *daemon_bus_open(struct daemon_devices *dd, char *err, size_t errsz);

/* The poll fd of the bus (-1 when the bus needs none). */
int daemon_bus_fd(sd_bus *bus);

/* The events to poll the bus fd with (sd_bus_get_events: POLLIN always,
 * POLLOUT only while the queue is non-empty - polling level-triggered
 * POLLOUT unconditionally would make poll() return immediately forever). */
int daemon_bus_events(sd_bus *bus);

/* Drive the bus after poll() reported revents: process incoming
 * messages (POLLIN) and flush the queue (POLLOUT).  Returns 0 or -1 on
 * a fatal bus error (the caller drops the bus). */
int daemon_bus_process(sd_bus *bus, int revents);

void daemon_bus_close(sd_bus *bus);

#endif /* LG_TOOLS_DAEMON_BUS_H */
