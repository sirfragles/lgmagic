/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * uinput.c - virtual input devices via /dev/uinput.
 *
 * v2: spec-driven uinput_create() for the daemon's virtual keyboard and
 * mouse, with uinput_key()/uinput_scroll()/uinput_move() emitters.  The
 * v1 `imu --mouse` device (uinput_open) is a thin wrapper with the same
 * name, ids and event set as before.
 */
#include "uinput.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* open one of the uinput device nodes; error message matches v1 */
static int uinput_open_node(char *err, size_t errsz)
{
	static const char *paths[] = { "/dev/uinput", "/dev/input/uinput" };
	int fd = -1;
	int last_errno = 0;
	size_t i;

	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		fd = open(paths[i], O_WRONLY | O_NONBLOCK);
		if (fd >= 0)
			break;
		last_errno = errno;
	}
	if (fd < 0) {
		if (last_errno == ENOENT)
			snprintf(err, errsz, "no /dev/uinput device - "
				 "run 'modprobe uinput' first");
		else if (last_errno == EACCES)
			snprintf(err, errsz, "%s: permission denied - "
				 "run as root or join the input group",
				 paths[0]);
		else
			snprintf(err, errsz, "cannot open %s: %s", paths[0],
				 strerror(last_errno));
	}
	return fd;
}

void uinput_spec_init(struct uinput_spec *s, const char *name)
{
	memset(s, 0, sizeof(*s));
	s->name = name;
	s->bustype = BUS_USB;
}

void uinput_spec_key(struct uinput_spec *s, unsigned code)
{
	if (code < KEY_CNT)
		s->keybits[code / 8] |= (unsigned char)(1u << (code % 8));
}

void uinput_spec_rel(struct uinput_spec *s, unsigned code)
{
	if (code < REL_CNT)
		s->relbits[code / 8] |= (unsigned char)(1u << (code % 8));
}

void uinput_spec_abs(struct uinput_spec *s, unsigned code)
{
	if (code < ABS_CNT)
		s->absbits[code / 8] |= (unsigned char)(1u << (code % 8));
}

/* does the bit array have any bit set? */
static int any_bits(const unsigned char *bits, size_t nbytes)
{
	size_t i;

	for (i = 0; i < nbytes; i++)
		if (bits[i])
			return 1;
	return 0;
}

int uinput_create(const struct uinput_spec *spec, char *err, size_t errsz)
{
	struct uinput_setup usetup;
	unsigned code;
	int fd;

	fd = uinput_open_node(err, errsz);
	if (fd < 0)
		return -1;

	if (any_bits(spec->keybits, sizeof(spec->keybits)) &&
	    ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0)
		goto fail;
	if (any_bits(spec->relbits, sizeof(spec->relbits)) &&
	    ioctl(fd, UI_SET_EVBIT, EV_REL) < 0)
		goto fail;
	if (any_bits(spec->absbits, sizeof(spec->absbits)) &&
	    ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0)
		goto fail;

	for (code = 0; code < KEY_CNT; code++)
		if (spec->keybits[code / 8] & (1u << (code % 8)))
			if (ioctl(fd, UI_SET_KEYBIT, code) < 0)
				goto fail;
	for (code = 0; code < REL_CNT; code++)
		if (spec->relbits[code / 8] & (1u << (code % 8)))
			if (ioctl(fd, UI_SET_RELBIT, code) < 0)
				goto fail;
	for (code = 0; code < ABS_CNT; code++)
		if (spec->absbits[code / 8] & (1u << (code % 8)))
			if (ioctl(fd, UI_SET_ABSBIT, code) < 0)
				goto fail;

	memset(&usetup, 0, sizeof(usetup));
	snprintf(usetup.name, UINPUT_MAX_NAME_SIZE, "%s", spec->name);
	usetup.id.bustype = spec->bustype;
	usetup.id.vendor = (__u16)spec->vendor;
	usetup.id.product = (__u16)spec->product;
	if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0 ||
	    ioctl(fd, UI_DEV_CREATE) < 0)
		goto fail;
	return fd;

fail:
	snprintf(err, errsz, "uinput setup for '%s' failed: %s", spec->name,
		 strerror(errno));
	close(fd);
	return -1;
}

void uinput_close(int fd)
{
	if (fd < 0)
		return;
	ioctl(fd, UI_DEV_DESTROY);
	close(fd);
}

static int uinput_write(int fd, const struct input_event *ev, size_t n)
{
	if (write(fd, ev, n * sizeof(*ev)) == (ssize_t)(n * sizeof(*ev)))
		return 0;
	if (!errno)
		errno = EIO;	/* short write - a partial event stream */
	return -1;
}

int uinput_key(int fd, int code, int value)
{
	struct input_event ev[2];

	memset(ev, 0, sizeof(ev));
	ev[0].type = EV_KEY;
	ev[0].code = (__u16)code;
	ev[0].value = value;
	ev[1].type = EV_SYN;
	ev[1].code = SYN_REPORT;
	return uinput_write(fd, ev, 2);
}

int uinput_scroll(int fd, int wheel, int hi_res)
{
	struct input_event ev[3];
	size_t n = 0;

	memset(ev, 0, sizeof(ev));
	if (wheel) {
		ev[n].type = EV_REL;
		ev[n].code = REL_WHEEL;
		ev[n].value = wheel;
		n++;
	}
	if (hi_res) {
		ev[n].type = EV_REL;
		ev[n].code = REL_WHEEL_HI_RES;
		ev[n].value = hi_res;
		n++;
	}
	ev[n].type = EV_SYN;
	ev[n].code = SYN_REPORT;
	n++;
	return uinput_write(fd, ev, n);
}

int uinput_move(int fd, int dx, int dy)
{
	struct input_event ev[3];

	memset(ev, 0, sizeof(ev));
	ev[0].type = EV_REL;
	ev[0].code = REL_X;
	ev[0].value = dx;
	ev[1].type = EV_REL;
	ev[1].code = REL_Y;
	ev[1].value = dy;
	ev[2].type = EV_SYN;
	ev[2].code = SYN_REPORT;
	ev[2].value = 0;
	return uinput_write(fd, ev, 3);
}

/* ------------------------------------------------------------------ */
/* v1 compatibility                                                    */
/* ------------------------------------------------------------------ */

int uinput_open(char *err, size_t errsz)
{
	struct uinput_spec s;

	uinput_spec_init(&s, "lg-magic mouse");
	s.vendor = 0x1;		/* the exact v1 UI_DEV_SETUP values */
	s.product = 0x1;
	uinput_spec_rel(&s, REL_X);
	uinput_spec_rel(&s, REL_Y);
	uinput_spec_key(&s, BTN_LEFT);
	uinput_spec_key(&s, BTN_RIGHT);
	return uinput_create(&s, err, errsz);
}
