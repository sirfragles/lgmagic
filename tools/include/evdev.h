/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * evdev.h - Linux evdev access for the IMU and keyboard devices
 * (Linux only).
 */
#ifndef LG_TOOLS_EVDEV_H
#define LG_TOOLS_EVDEV_H

#include <signal.h>
#include <stddef.h>

struct evdev_imu {
	int fd;
	char name[256];
	char path[256];
	unsigned last_counter;
	int have_frame;		/* seen at least one EV_SYN (for the dt quirk) */
	int accel[3];		/* ABS_X/Y/Z */
	int gyro[3];		/* ABS_RX/RY/RZ */
	unsigned vendor;	/* EVIOCGID id.vendor */
	unsigned product;	/* EVIOCGID id.product */
};

/* Set by the SIGINT/SIGTERM handler in cmd_imu.c; evdev_read_frame()
 * returns -1 (errno EINTR) instead of retrying once this is set. */
extern volatile sig_atomic_t g_stop;

/* One EV_KEY event collected since the last EV_SYN. */
struct evdev_key_event {
	int code;
	int value;
};

/* A full frame from the keyboard or the IMU device: the IMU accel/gyro
 * state plus the key events and wheel clicks that arrived since the last
 * EV_SYN.  For the IMU device keys/wheel stay empty; the counter/dt/
 * accel/gyro semantics are bit-identical to v1. */
struct evdev_frame {
	unsigned counter;
	double dt;
	int accel[3];		/* ABS_X/Y/Z */
	int gyro[3];		/* ABS_RX/RY/RZ */
	int nkeys;		/* pending key events, 0..16 */
	struct evdev_key_event keys[16];
	int wheel;		/* REL_WHEEL accumulated since the last SYN */
	int hwheel;		/* REL_HWHEEL accumulated since the last SYN */
};

/* Open /dev/input/eventN, read its name and input id, close it.  A probe
 * never blocks.  Returns 0 or -1. */
int evdev_probe(const char *path, char *name, size_t namesz,
		unsigned *vendor, unsigned *product);

/* Find the first /dev/input/event* whose EVIOCGNAME contains "IMU"
 * (same predicate as display_imu.py). wanted_path (non-NULL) overrides
 * detection. Returns 0 (device open, fields filled) or -1 with err. */
int evdev_find_imu(struct evdev_imu *dev, const char *wanted_path,
		   char *err, size_t errsz);

/* Find the first /dev/input/event* that looks like the LG keyboard
 * (exact name "LG Magic Remote", id 000f:3412 - the same predicate as
 * pairing_is_keyboard()). wanted_path (non-NULL) overrides detection
 * without checking the predicate. Returns 0 or -1 with err. */
int evdev_find_keyboard(struct evdev_imu *dev, const char *wanted_path,
			char *err, size_t errsz);

void evdev_close(struct evdev_imu *dev);

/* EVIOCGRAB: grab=1 takes exclusive ownership of the device (the
 * daemon), grab=0 releases it.  Returns 0 or -1. */
int evdev_grab(int fd, int grab);

/* Read the sysfs uniq (Bluetooth MAC) of the input device behind the
 * /dev/input/eventN node.  Fills out with "" when the sysfs node or the
 * value is absent (the normal case for uinput test devices).  Returns
 * 0 or -1 with err (unexpected path). */
int evdev_read_uniq(const char *path, char *out, size_t outsz,
		    char *err, size_t errsz);

/* Read events until the next EV_SYN. Returns 1 with a frame
 * (counter/dt/accel/gyro filled), 0 on EOF, -1 on error.
 * dt = ((counter - last) % 65536) / 256 * 0.02, NAN on the first frame.
 * evdev_read_frame_ext() additionally collects EV_KEY and
 * REL_WHEEL/REL_HWHEEL into the frame; the v1 signature is a thin
 * wrapper over it with identical output. */
int evdev_read_frame(struct evdev_imu *dev, unsigned *counter, double *dt,
		     int accel[3], int gyro[3], char *err, size_t errsz);
int evdev_read_frame_ext(struct evdev_imu *dev, struct evdev_frame *frame,
			 char *err, size_t errsz);

#endif /* LG_TOOLS_EVDEV_H */
