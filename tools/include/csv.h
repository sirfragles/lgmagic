/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * csv.h - IMU sample CSV reader/writer, byte-compatible with
 * scripts/display_imu.py (header "counter,dt,ax,ay,az,gx,gy,gz",
 * CRLF line endings, empty dt field on the first frame).
 */
#ifndef LG_TOOLS_CSV_H
#define LG_TOOLS_CSV_H

#include <stddef.h>
#include <math.h>

struct imu_sample {
	unsigned counter;
	double dt;		/* seconds; NAN for the first frame (Python: None) */
	double accel[3];	/* ax, ay, az (raw counts) */
	double gyro[3];		/* gx, gy, gz (raw counts) */
};

/* Read a whole IMU CSV. Rows with fewer than 8 fields or non-numeric data
 * are skipped (the Python reader would crash on short rows - deliberate
 * fix). Returns the number of samples read, or -1 on error. */
long csv_read(const char *path, struct imu_sample **out, char *err, size_t errsz);

/* Write samples in display_imu.py format (CRLF, empty dt on the first
 * frame, dt via %.9g which matches Python's repr for these values).
 * Returns 0 on success, -1 on error. */
int csv_write(const char *path, const struct imu_sample *samples, size_t count,
	      char *err, size_t errsz);

#endif /* LG_TOOLS_CSV_H */
