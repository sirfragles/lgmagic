/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cube.c - ANSI terminal wireframe cube (donut.c style, no GPU libraries).
 *
 * The current orientation quaternion rotates the 8 cube vertices; the 12
 * edges are drawn as sampled 3D segments with a per-cell z-buffer and
 * depth-based luminance shading. The whole frame is written with a single
 * fwrite ("\033[H" + header + grid lines) to minimize flicker; the
 * terminal size is polled every frame (TIOCGWINSZ), so resizing works
 * without a SIGWINCH handler. The cursor is hidden for the lifetime of the
 * cube and restored by cube_shutdown(), which cmd_imu guarantees to call
 * from its SIGINT/SIGTERM handler.
 */
#include "cube.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/ioctl.h>
#endif

#define CUBE_LUM ".,-~:;=!*#$@"
#define CUBE_NLUM 12
#define CUBE_F 2.2		/* perspective focal factor */
#define CUBE_D 5.0		/* camera distance (pyqtgraph: 5) */
#define CUBE_HALF 1.8		/* depth range around CUBE_D for shading */
#define CUBE_STEPS 60		/* samples per edge */

static struct {
	int rows, cols;		/* terminal size; rows includes the text line */
	char *grid;		/* (rows-1) * cols */
	double *zbuf;		/* (rows-1) * cols */
	char *out;		/* frame buffer */
	size_t out_cap;
	int inited;
} C;

static const double VERTICES[8][3] = {
	{ -1.0, -1.0, -1.0 }, { 1.0, -1.0, -1.0 },
	{ 1.0, -1.0, 1.0 }, { -1.0, -1.0, 1.0 },
	{ -1.0, 1.0, -1.0 }, { 1.0, 1.0, -1.0 },
	{ 1.0, 1.0, 1.0 }, { -1.0, 1.0, 1.0 },
};

static const int EDGES[12][2] = {
	{ 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },	/* bottom square */
	{ 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },	/* top square */
	{ 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },	/* verticals */
};

int cube_available(void)
{
	return isatty(STDOUT_FILENO);
}

static void get_size(void)
{
	int rows = 24, cols = 80;
#ifdef __linux__
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
	    ws.ws_row >= 8 && ws.ws_col >= 20) {
		rows = ws.ws_row;
		cols = ws.ws_col;
	}
#endif
	if (rows == C.rows && cols == C.cols)
		return;
	C.rows = rows;
	C.cols = cols;
	free(C.grid);
	free(C.zbuf);
	C.grid = malloc((size_t)(rows - 1) * (size_t)cols);
	C.zbuf = malloc((size_t)(rows - 1) * (size_t)cols * sizeof(double));
}

void cube_init(void)
{
	get_size();
	C.inited = 1;
	fputs("\033[?25l", stdout);
	fflush(stdout);
}

void cube_shutdown(void)
{
	if (!C.inited)
		return;
	fputs("\033[?25h\n", stdout);
	fflush(stdout);
	C.inited = 0;
}

void cube_project(const quat *q, float vx, float vy, float vz,
		  int rows, int cols, float *px, float *py, float *pz)
{
	mat3 r = quat_to_mat3(*q);
	vec3 v = mat3_mul_vec3(&r, (vec3){ vx, vy, vz });
	double zc = v.z + CUBE_D;
	double s = 0.2 * cols;

	*px = (float)((cols / 2.0) + (v.x * CUBE_F / zc) * s);
	/* 2:1 cell aspect: vertical offsets are scaled by 0.5 */
	*py = (float)((rows / 2.0) - (v.y * CUBE_F / zc) * s * 0.5);
	*pz = (float)zc;
}

static void plot(int r, int c, double zc)
{
	double t;
	int idx;
	char ch;

	if (r < 1 || r >= C.rows || c < 0 || c >= C.cols)
		return;
	r -= 1;		/* row 0 is the text line */
	{
		double *z = &C.zbuf[(size_t)r * C.cols + c];

		if (zc >= *z)
			return;		/* occluded */
		*z = zc;
	}
	t = (zc - (CUBE_D - CUBE_HALF)) / (2.0 * CUBE_HALF);
	if (t < 0.0)
		t = 0.0;
	if (t > 1.0)
		t = 1.0;
	idx = (int)(t * (CUBE_NLUM - 1) + 0.5);
	ch = CUBE_LUM[CUBE_NLUM - 1 - idx];	/* near = bright */
	C.grid[(size_t)r * C.cols + c] = ch;
}

void cube_render(const quat *q)
{
	size_t used;
	double roll, pitch, yaw;
	mat3 r;
	double vv[8][3];
	int e, s, row;

	if (!C.inited)
		cube_init();
	get_size();
	if (!C.grid || !C.zbuf)	/* allocation failed - no rendering */
		return;

	/* header: same format as display_imu.py */
	quat_to_euler(*q, &roll, &pitch, &yaw);
	roll *= 180.0 / M_PI;
	pitch *= 180.0 / M_PI;
	yaw *= 180.0 / M_PI;

	r = quat_to_mat3(*q);
	for (e = 0; e < 8; e++) {
		vec3 v = mat3_mul_vec3(&r,
			(vec3){ VERTICES[e][0], VERTICES[e][1],
				VERTICES[e][2] });

		vv[e][0] = v.x;
		vv[e][1] = v.y;
		vv[e][2] = v.z;
	}

	memset(C.grid, ' ', (size_t)(C.rows - 1) * (size_t)C.cols);
	{
		size_t n = (size_t)(C.rows - 1) * (size_t)C.cols;

		for (row = 0; row < C.rows - 1; row++) {
			int c;

			for (c = 0; c < C.cols; c++)
				C.zbuf[(size_t)row * C.cols + c] = 1e30;
		}
		(void)n;
	}

	for (e = 0; e < 12; e++) {
		const double *a = vv[EDGES[e][0]];
		const double *b = vv[EDGES[e][1]];

		for (s = 0; s <= CUBE_STEPS; s++) {
			double t = (double)s / CUBE_STEPS;
			vec3 p = { a[0] + (b[0] - a[0]) * t,
				   a[1] + (b[1] - a[1]) * t,
				   a[2] + (b[2] - a[2]) * t };
			double zc = p.z + CUBE_D;
			double sc = 0.2 * C.cols;
			double x = (C.cols / 2.0) + (p.x * CUBE_F / zc) * sc;
			double y = (C.rows / 2.0) - (p.y * CUBE_F / zc) * sc * 0.5;

			plot((int)(y + 0.5), (int)(x + 0.5), zc);
		}
	}

	/* assemble the frame: "\033[H", header, grid lines (trailing spaces
	 * trimmed), one fwrite */
	{
		size_t need = (size_t)(C.rows - 1) * (size_t)(C.cols + 2) + 128;
		size_t pos = 0;

		if (!C.out || need > C.out_cap) {
			char *no = realloc(C.out, need);

			if (!no)
				return;
			C.out = no;
			C.out_cap = need;
		}
		memcpy(C.out + pos, "\033[H", 3);
		pos += 3;
		pos += (size_t)snprintf(C.out + pos, C.out_cap - pos,
			"Roll=%+.2f  Pitch=%+.2f  Yaw=%+.2f\n",
			roll, pitch, yaw);
		for (row = 0; row < C.rows - 1; row++) {
			const char *line = C.grid + (size_t)row * C.cols;
			int last = C.cols - 1;

			while (last >= 0 && line[last] == ' ')
				last--;
			if (last >= 0) {
				memcpy(C.out + pos, line, (size_t)last + 1);
				pos += (size_t)last + 1;
			}
			C.out[pos++] = '\n';
		}
		used = pos;
	}
	fwrite(C.out, 1, used, stdout);
	fflush(stdout);
}
