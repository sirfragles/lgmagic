/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * daemon_devices.c - lgmagicd device discovery and hotplug.
 * See daemon_devices.h.
 */
#include "daemon_devices.h"

#include "pairing.h"
#include "uinput.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>

#define RESCAN_INTERVAL_MS 2000		/* polling fallback */
#define RESCAN_DEBOUNCE_MS 200		/* inotify debounce */

/* ------------------------------------------------------------------ */
/* Logging (stderr - journald captures it in Phase 4)                  */
/* ------------------------------------------------------------------ */

static void log_info(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "lgmagicd: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void log_debug(const struct daemon_devices *dd, const char *fmt, ...)
{
	va_list ap;

	if (!dd->debug)
		return;
	fprintf(stderr, "lgmagicd: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------------ */
/* Scan                                                                */
/* ------------------------------------------------------------------ */

/* One probed /dev/input node; pd points into the fields. */
struct scan_entry {
	char path[256];
	char name[256];
	char uniq[64];
	unsigned vendor;
	unsigned product;
	struct pairing_input_dev pd;
};

static int scan_add(struct scan_entry **entries, size_t *n, size_t *cap,
		    const char *path)
{
	struct scan_entry *e;

	if (*n == *cap) {
		size_t ncap = *cap ? *cap * 2 : 8;
		struct scan_entry *p = realloc(*entries, ncap * sizeof(*p));

		if (!p)
			return -1;
		*entries = p;
		*cap = ncap;
	}
	e = &(*entries)[*n];
	memset(e, 0, sizeof(*e));
	snprintf(e->path, sizeof(e->path), "%s", path);
	if (evdev_probe(path, e->name, sizeof(e->name), &e->vendor,
			&e->product) < 0)
		return 0;	/* unopenable node - skip silently */
	if (evdev_read_uniq(path, e->uniq, sizeof(e->uniq), NULL, 0) < 0)
		e->uniq[0] = '\0';
	e->pd.path = e->path;
	e->pd.name = e->name;
	e->pd.uniq = e->uniq;
	e->pd.vendor = e->vendor;
	e->pd.product = e->product;
	(*n)++;
	return 0;
}

static int scan_inputs(struct scan_entry **entries, size_t *n,
		       const char *kbd_override)
{
	struct dirent *de;
	DIR *dir;
	size_t cap = 0, i;

	*entries = NULL;
	*n = 0;
	dir = opendir("/dev/input");
	if (!dir)
		return kbd_override ? 0 : -1;
	while ((de = readdir(dir)) != NULL) {
		char path[sizeof(de->d_name) + 16];

		if (strncmp(de->d_name, "event", 5) != 0)
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		scan_add(entries, n, &cap, path);
	}
	closedir(dir);

	/* Test mode: the pinned keyboard must be part of the pairing even
	 * when its node is not visible in /dev/input. */
	if (kbd_override) {
		for (i = 0; i < *n; i++)
			if (strcmp((*entries)[i].path, kbd_override) == 0)
				return 0;
		if (scan_add(entries, n, &cap, kbd_override) < 0)
			return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Remote open/close                                                   */
/* ------------------------------------------------------------------ */

int daemon_identity_valid(const char *s)
{
	size_t i;

	if (!s)
		return 0;
	if (strcmp(s, "unknown") == 0)
		return 1;
	if (strlen(s) != 17)
		return 0;
	for (i = 0; i < 17; i++) {
		if (i % 3 == 2) {
			if (s[i] != ':')
				return 0;
		} else if (!isxdigit((unsigned char)s[i])) {
			return 0;
		}
	}
	return 1;
}

static void remote_free(struct daemon_remote *r)
{
	evdev_close(&r->kbd);
	evdev_close(&r->imu);
	uinput_close(r->kbd_uinput);
	uinput_close(r->mouse_uinput);
	device_config_free(&r->dc);
	pipeline_free(&r->pl);
}

/* The per-remote virtual pair: the keyboard carries the full EV_KEY
 * set (mapped keys + mouse buttons are all keycodes), the mouse the
 * relative axes.  The names carry the identity so desktops can tell
 * remotes apart.  Returns 0 or -1 (nothing half-open is kept). */
static int remote_uinput_open(struct daemon_remote *r,
			      const struct daemon_devices *dd)
{
	struct uinput_spec kbd, mouse;
	/* identity is 64 bytes; room for it plus the fixed prefix */
	char kname[sizeof(r->identity) + sizeof("lgmagicd keyboard ")];
	char mname[sizeof(r->identity) + sizeof("lgmagicd mouse ")];
	char err[256];
	int i;

	if (dd->no_uinput)
		return 0;
	snprintf(kname, sizeof(kname), "lgmagicd keyboard %s", r->identity);
	snprintf(mname, sizeof(mname), "lgmagicd mouse %s", r->identity);
	uinput_spec_init(&kbd, kname);
	for (i = 0; i < KEY_CNT; i++)
		uinput_spec_key(&kbd, (unsigned)i);
	uinput_spec_init(&mouse, mname);
	uinput_spec_rel(&mouse, REL_X);
	uinput_spec_rel(&mouse, REL_Y);
	uinput_spec_rel(&mouse, REL_WHEEL);
	uinput_spec_rel(&mouse, REL_WHEEL_HI_RES);
	uinput_spec_rel(&mouse, REL_HWHEEL);
	uinput_spec_key(&mouse, BTN_LEFT);
	uinput_spec_key(&mouse, BTN_RIGHT);
	uinput_spec_key(&mouse, BTN_MIDDLE);

	r->kbd_uinput = uinput_create(&kbd, err, sizeof(err));
	if (r->kbd_uinput < 0) {
		log_info("virtual devices for %s: %s (not grabbing, retry "
			 "on the next rescan)", r->identity, err);
		return -1;
	}
	r->mouse_uinput = uinput_create(&mouse, err, sizeof(err));
	if (r->mouse_uinput < 0) {
		uinput_close(r->kbd_uinput);
		r->kbd_uinput = -1;
		log_info("virtual devices for %s: %s (not grabbing, retry "
			 "on the next rescan)", r->identity, err);
		return -1;
	}
	log_info("virtual devices ready for %s", r->identity);
	return 0;
}

/* Open one remote from its paths, load its config, configure the
 * pipeline.  Returns 0 or -1 (nothing half-open is kept). */
static int remote_open(struct daemon_remote *r, const char *identity,
		       const char *kbd_path, const char *imu_path,
		       const struct daemon_devices *dd)
{
	char err[256];
	char calib_path[4096];

	memset(r, 0, sizeof(*r));
	r->kbd.fd = -1;
	r->imu.fd = -1;
	r->kbd_uinput = -1;
	r->mouse_uinput = -1;
	snprintf(r->identity, sizeof(r->identity), "%s", identity);
	snprintf(r->kbd_path, sizeof(r->kbd_path), "%s", kbd_path);
	snprintf(r->imu_path, sizeof(r->imu_path), "%s", imu_path);
	pipeline_init(&r->pl);

	/* The virtual pair BEFORE the grab (plan safety order): a remote
	 * must never be taken over without an output target. */
	if (remote_uinput_open(r, dd) < 0)
		goto fail;

	if (evdev_find_keyboard(&r->kbd, kbd_path, err, sizeof(err)) < 0) {
		log_debug(dd, "keyboard %s: %s", kbd_path, err);
		goto fail;
	}
	/* EVIOCGRAB after this remote's uinput pair exists.  Best effort -
	 * warn and continue without it. */
	if (evdev_grab(r->kbd.fd, 1) < 0)
		log_info("cannot grab %s: %s (continuing without exclusive "
			 "grab)", kbd_path, strerror(errno));
	else
		log_debug(dd, "grabbed %s", kbd_path);

	/* IMU: NO grab - `lgmagic imu --csv/--mouse` runs in parallel. */
	if (imu_path[0] &&
	    evdev_find_imu(&r->imu, imu_path, err, sizeof(err)) < 0) {
		log_debug(dd, "IMU %s: %s", imu_path, err);
		r->imu.fd = -1;
		r->imu_path[0] = '\0';
	}

	device_config_init(&r->dc);
	if (daemon_config_load_remote(dd->config, identity, &r->dc,
				      err, sizeof(err)) < 0)
		log_info("config for %s: %s (using defaults)", identity, err);
	daemon_config_calib_path(dd->config, &r->dc, identity, calib_path,
				 sizeof(calib_path));
	if (pipeline_configure(&r->pl, &r->dc, calib_path,
			       dd->config->global->lpf_alpha,
			       r->kbd_uinput, err, sizeof(err)) < 0)
		log_info("calibration for %s: %s (airmouse without "
			 "calibration)", identity, err);

	log_info("remote %s: keyboard %s (%s)%s%s%s", identity, r->kbd.name,
		 kbd_path, imu_path[0] ? ", IMU " : "", imu_path,
		 imu_path[0] ? "" : ", no IMU");
	return 0;

fail:
	remote_free(r);
	return -1;
}

/* ------------------------------------------------------------------ */
/* Rescan                                                              */
/* ------------------------------------------------------------------ */

int daemon_devices_rescan(struct daemon_devices *dd, char *err, size_t errsz)
{
	struct scan_entry *entries = NULL;
	size_t nentries = 0, i;
	struct pairing_input_dev *devs = NULL;
	struct paired_remote *paired = NULL;
	size_t npaired = 0;
	struct daemon_remote *newr = NULL;
	size_t nnew = 0;
	int ret = -1;

	dd->last_rescan_ms = now_ms();
	dd->have_rescan = 0;

	/* Retry the inotify watch (e.g. /dev/input appeared later). */
	if (dd->inotify_fd >= 0 && dd->inotify_wd < 0)
		dd->inotify_wd = inotify_add_watch(dd->inotify_fd, "/dev/input",
						    IN_CREATE | IN_DELETE |
						    IN_MOVED_TO | IN_MOVED_FROM |
						    IN_ATTRIB);

	if (scan_inputs(&entries, &nentries, dd->kbd_override) < 0) {
		snprintf(err, errsz, "cannot scan /dev/input");
		goto out;
	}
	if (nentries > 0) {
		devs = malloc(nentries * sizeof(*devs));
		paired = malloc((nentries ? nentries : 1) * sizeof(*paired));
		newr = malloc((nentries + 1) * sizeof(*newr));
		if (!devs || !paired || !newr) {
			snprintf(err, errsz, "out of memory");
			goto out;
		}
		for (i = 0; i < nentries; i++)
			devs[i] = entries[i].pd;
		if (pairing_match(devs, nentries, paired, nentries, &npaired,
				  err, errsz) < 0) {
			/* Ambiguous set: refuse to guess, keep what we have. */
			goto out;
		}
	} else {
		newr = malloc(sizeof(*newr));
		if (!newr) {
			snprintf(err, errsz, "out of memory");
			goto out;
		}
	}

	/* Rebuild: steal unchanged remotes, open the rest. */
	for (i = 0; i < npaired; i++) {
		const struct paired_remote *pr = &paired[i];
		const char *kbd_path = pr->keyboard->path;
		const char *imu_path = pr->imu ? pr->imu->path : "";
		char identity[64];
		struct daemon_remote *dst = &newr[nnew];
		size_t j;
		int found = 0;

		/* Test mode: only the pinned keyboard is taken over. */
		if (dd->kbd_override && strcmp(kbd_path, dd->kbd_override) != 0)
			continue;
		/* The identity reaches file paths below - accept only a MAC
		 * or the "unknown" fallback (see daemon_identity_valid). */
		snprintf(identity, sizeof(identity), "%s",
			 daemon_identity_valid(pr->uniq) ? pr->uniq : "unknown");
		if (pr->uniq[0] && !daemon_identity_valid(pr->uniq))
			log_debug(dd, "non-MAC uniq '%s' - using the "
				  "'unknown' identity", pr->uniq);

		for (j = 0; j < dd->nremotes; j++) {
			struct daemon_remote *e = &dd->remotes[j];

			if (strcmp(e->identity, identity) != 0)
				continue;
			if (strcmp(e->kbd_path, kbd_path) != 0 ||
			    strcmp(e->imu_path, imu_path) != 0)
				continue;
			/* Unchanged: steal the open remote (fds + pipeline
			 * state survive the rescan). */
			*dst = *e;
			memset(e, 0, sizeof(*e));
			e->kbd.fd = -1;
			e->imu.fd = -1;
			e->kbd_uinput = -1;
			e->mouse_uinput = -1;
			nnew++;
			found = 1;
			break;
		}
		if (found)
			continue;
		if (remote_open(dst, identity, kbd_path, imu_path, dd) == 0)
			nnew++;
		else
			remote_free(dst);	/* retried on the next rescan */
	}

	for (i = 0; i < dd->nremotes; i++) {
		struct daemon_remote *e = &dd->remotes[i];

		if (e->identity[0])
			remote_free(e);
	}
	free(dd->remotes);
	dd->remotes = newr;
	dd->nremotes = nnew;
	newr = NULL;
	ret = 0;

out:
	free(newr);
	free(paired);
	free(devs);
	free(entries);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Poll + handling                                                     */
/* ------------------------------------------------------------------ */

int daemon_devices_pollfds(struct daemon_devices *dd, struct pollfd *fds,
			   size_t nfds)
{
	size_t n = 0, i;

	if (dd->inotify_fd >= 0 && n < nfds) {
		fds[n].fd = dd->inotify_fd;
		fds[n].events = POLLIN;
		fds[n].revents = 0;
		n++;
	}
	for (i = 0; i < dd->nremotes; i++) {
		struct daemon_remote *r = &dd->remotes[i];

		if (r->kbd.fd >= 0 && n < nfds) {
			fds[n].fd = r->kbd.fd;
			fds[n].events = POLLIN;
			fds[n].revents = 0;
			n++;
		}
		if (r->imu.fd >= 0 && n < nfds) {
			fds[n].fd = r->imu.fd;
			fds[n].events = POLLIN;
			fds[n].revents = 0;
			n++;
		}
	}
	return (int)n;
}

/* Schedule a rescan after the debounce (0 = immediately). */
static void schedule_rescan(struct daemon_devices *dd, long long delay_ms)
{
	dd->rescan_due_ms = now_ms() + delay_ms;
	dd->have_rescan = 1;
}

static void drain_inotify(struct daemon_devices *dd)
{
	char buf[4096];

	while (read(dd->inotify_fd, buf, sizeof(buf)) > 0)
		;
	/* The watch may have died with the directory. */
	if (errno != EAGAIN && errno != EWOULDBLOCK)
		dd->inotify_wd = -1;
	schedule_rescan(dd, RESCAN_DEBOUNCE_MS);
}

static int handle_kbd(struct daemon_remote *r, struct daemon_devices *dd)
{
	struct evdev_frame f;
	char err[256];
	int rv;

	rv = evdev_read_frame_ext(&r->kbd, &f, err, sizeof(err));
	if (rv == 1) {
		if (f.nkeys || f.wheel)
			log_debug(dd, "keyboard frame from %s: %d keys, wheel %d",
				  r->identity, f.nkeys, f.wheel);
		if (pipeline_keyboard(&r->pl, &f, r->kbd_uinput,
				      r->mouse_uinput) < 0)
			log_info("uinput write for %s failed: %s", r->identity,
				 strerror(errno));
		return 1;
	}
	if (g_stop)
		return 0;	/* shutting down */
	log_info("keyboard %s (%s) gone: %s", r->identity, r->kbd_path,
		 rv == 0 ? "EOF" : err);
	evdev_close(&r->kbd);
	schedule_rescan(dd, 0);
	return -1;
}

static int handle_imu(struct daemon_remote *r, struct daemon_devices *dd)
{
	struct evdev_frame f;
	char err[256];
	int rv;

	rv = evdev_read_frame_ext(&r->imu, &f, err, sizeof(err));
	if (rv == 1) {
		if (pipeline_imu(&r->pl, &f, r->mouse_uinput) < 0)
			log_info("uinput write for %s (mouse) failed: %s",
				 r->identity, strerror(errno));
		return 1;
	}
	if (g_stop)
		return 0;
	log_info("IMU %s (%s) gone: %s", r->identity, r->imu_path,
		 rv == 0 ? "EOF" : err);
	evdev_close(&r->imu);
	schedule_rescan(dd, 0);
	return -1;
}

int daemon_devices_handle(struct daemon_devices *dd, size_t idx)
{
	size_t n = 0, i;

	if (dd->inotify_fd >= 0) {
		if (idx == 0) {
			drain_inotify(dd);
			return 0;
		}
		n = 1;
	}
	for (i = 0; i < dd->nremotes; i++) {
		struct daemon_remote *r = &dd->remotes[i];

		if (r->kbd.fd >= 0) {
			if (idx == n)
				return handle_kbd(r, dd);
			n++;
		}
		if (r->imu.fd >= 0) {
			if (idx == n)
				return handle_imu(r, dd);
			n++;
		}
	}
	return 0;
}

long long daemon_devices_rescan_delay_ms(const struct daemon_devices *dd,
					 long long now)
{
	long long due = -1, fallback;

	if (dd->have_rescan) {
		due = dd->rescan_due_ms - now;
		if (due < 0)
			due = 0;
	}
	fallback = dd->last_rescan_ms + RESCAN_INTERVAL_MS - now;
	if (fallback < 0)
		fallback = 0;
	return due < 0 || fallback < due ? fallback : due;
}

/* ------------------------------------------------------------------ */
/* Reload (SIGHUP)                                                     */
/* ------------------------------------------------------------------ */

int daemon_devices_reload(struct daemon_devices *dd, char *err, size_t errsz)
{
	struct config *newg;
	size_t i;

	newg = config_load_daemon(dd->config->config_root);
	if (!newg) {
		snprintf(err, errsz, "out of memory");
		return -1;
	}
	config_free(dd->config->global);
	dd->config->global = newg;

	for (i = 0; i < dd->nremotes; i++) {
		struct daemon_remote *r = &dd->remotes[i];
		struct device_config ndc;
		char calib_path[4096];

		if (daemon_config_load_remote(dd->config, r->identity, &ndc,
					      err, errsz) < 0) {
			log_info("config reload for %s: %s (keeping the "
				 "previous config)", r->identity, err);
			continue;
		}
		daemon_config_calib_path(dd->config, &ndc, r->identity,
					 calib_path, sizeof(calib_path));
		if (pipeline_configure(&r->pl, &ndc, calib_path,
				       newg->lpf_alpha, r->kbd_uinput,
				       err, errsz) < 0)
			log_info("calibration reload for %s: %s", r->identity,
				 err);
		device_config_free(&r->dc);
		r->dc = ndc;
	}
	log_info("config reloaded");
	return 0;
}

/* ------------------------------------------------------------------ */

int daemon_devices_init(struct daemon_devices *dd, struct daemon_config *config,
			const char *kbd_override, int no_uinput, int debug,
			char *err, size_t errsz)
{
	(void)err;
	(void)errsz;
	memset(dd, 0, sizeof(*dd));
	dd->config = config;
	dd->kbd_override = kbd_override;
	dd->no_uinput = no_uinput;
	dd->debug = debug;
	dd->inotify_fd = -1;
	dd->inotify_wd = -1;

	dd->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (dd->inotify_fd >= 0) {
		dd->inotify_wd = inotify_add_watch(dd->inotify_fd, "/dev/input",
						    IN_CREATE | IN_DELETE |
						    IN_MOVED_TO | IN_MOVED_FROM |
						    IN_ATTRIB);
		if (dd->inotify_wd < 0) {
			close(dd->inotify_fd);
			dd->inotify_fd = -1;
			log_info("inotify unavailable - polling fallback "
				 "only");
		}
	}
	return 0;
}

void daemon_devices_free(struct daemon_devices *dd)
{
	size_t i;

	for (i = 0; i < dd->nremotes; i++)
		remote_free(&dd->remotes[i]);
	free(dd->remotes);
	if (dd->inotify_fd >= 0)
		close(dd->inotify_fd);
	memset(dd, 0, sizeof(*dd));
	dd->inotify_fd = -1;
	dd->inotify_wd = -1;
}
