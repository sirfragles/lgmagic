/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * csv.c - IMU CSV reader/writer, byte-compatible with scripts/display_imu.py.
 *
 * The Python writer used csv.writer (default dialect): CRLF line endings,
 * header "counter,dt,ax,ay,az,gx,gy,gz", the first data row has an empty
 * dt field (the Python code wrote None), float dt via %.9g which matches
 * Python's repr() for the values that occur, raw values as integers.
 *
 * The reader is deliberately stricter than the Python one: rows with
 * fewer than 8 fields or non-numeric payload are skipped (in Python a
 * 7-field row would crash with IndexError - a hidden bug we fix).
 */
#include "csv.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CSV_MAX_FIELDS 64

long csv_read(const char *path, struct imu_sample **out, char *err, size_t errsz)
{
	FILE *f;
	long sz;
	char *buf, *line, *save = NULL;
	struct imu_sample *samples = NULL;
	long n = 0, cap = 0;

	f = fopen(path, "rb");
	if (!f) {
		snprintf(err, errsz, "cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	if (fseek(f, 0, SEEK_END) < 0 || (sz = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET) < 0) {
		snprintf(err, errsz, "cannot read %s", path);
		fclose(f);
		return -1;
	}
	buf = malloc((size_t)sz + 1);
	if (!buf) {
		snprintf(err, errsz, "out of memory");
		fclose(f);
		return -1;
	}
	if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		snprintf(err, errsz, "cannot read %s", path);
		free(buf);
		fclose(f);
		return -1;
	}
	fclose(f);
	buf[sz] = '\0';

	for (line = strtok_r(buf, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		const char *fields[CSV_MAX_FIELDS];
		int nfields = 0;
		char *p = line, *q;
		unsigned long counter;
		char *end;
		struct imu_sample s;

		/* strip trailing CR */
		{
			size_t l = strlen(line);

			if (l > 0 && line[l - 1] == '\r')
				line[l - 1] = '\0';
		}
		if (*line == '\0')
			continue;

		/* split on ',' (no quoted fields in this format) */
		for (q = p; nfields < CSV_MAX_FIELDS; q++) {
			fields[nfields++] = p;
			if (!*q)
				break;
			if (*q == ',') {
				*q = '\0';
				p = q + 1;
				continue;
			}
			/* find next ',' */
			while (*q && *q != ',')
				q++;
			if (*q == ',') {
				*q = '\0';
				p = q + 1;
				continue;
			}
			break;
		}
		if (nfields == CSV_MAX_FIELDS || *q != '\0' || nfields < 8)
			continue;	/* skip malformed row */

		/* counter */
		errno = 0;
		counter = strtoul(fields[0], &end, 10);
		if (errno != 0 || end == fields[0] || *end != '\0')
			continue;
		s.counter = (unsigned)counter;

		/* dt (empty on the first frame) */
		if (fields[1][0] == '\0') {
			s.dt = NAN;
		} else {
			errno = 0;
			s.dt = strtod(fields[1], &end);
			if (errno != 0 || end == fields[1] || *end != '\0')
				continue;
		}

		/* ax, ay, az, gx, gy, gz */
		{
			int i;
			double *dst[6] = { &s.accel[0], &s.accel[1], &s.accel[2],
					   &s.gyro[0], &s.gyro[1], &s.gyro[2] };
			int bad = 0;

			for (i = 0; i < 6; i++) {
				errno = 0;
				*dst[i] = strtod(fields[2 + i], &end);
				if (errno != 0 || end == fields[2 + i] ||
				    *end != '\0') {
					bad = 1;
					break;
				}
			}
			if (bad)
				continue;
		}

		if (n == cap) {
			struct imu_sample *ns;

			cap = cap ? cap * 2 : 64;
			ns = realloc(samples, (size_t)cap * sizeof(*samples));
			if (!ns) {
				snprintf(err, errsz, "out of memory");
				free(samples);
				free(buf);
				return -1;
			}
			samples = ns;
		}
		samples[n++] = s;
	}
	free(buf);
	*out = samples;
	return n;
}

int csv_write(const char *path, const struct imu_sample *samples, size_t n,
	      char *err, size_t errsz)
{
	FILE *f;
	size_t i;

	f = fopen(path, "wb");
	if (!f) {
		snprintf(err, errsz, "cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	fprintf(f, "counter,dt,ax,ay,az,gx,gy,gz\r\n");
	for (i = 0; i < n; i++) {
		char dtbuf[32] = "";

		if (!isnan(samples[i].dt))
			snprintf(dtbuf, sizeof(dtbuf), "%.9g", samples[i].dt);
		fprintf(f, "%u,%s,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f\r\n",
			samples[i].counter, dtbuf,
			samples[i].accel[0], samples[i].accel[1],
			samples[i].accel[2], samples[i].gyro[0],
			samples[i].gyro[1], samples[i].gyro[2]);
	}
	if (ferror(f) || fclose(f) != 0) {
		snprintf(err, errsz, "write error on %s: %s", path,
			 strerror(errno));
		return -1;
	}
	return 0;
}
