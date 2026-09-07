/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cube.h - ANSI terminal wireframe cube (portable, no GPU libraries).
 *
 * Renders the current orientation as an ASCII wireframe cube with a
 * z-buffer, in the spirit of donut.c. One fwrite per frame, cursor hidden
 * while rendering, guaranteed restore on shutdown / signals.
 */
#ifndef LG_TOOLS_CUBE_H
#define LG_TOOLS_CUBE_H

#include "matrix.h"

/* stdout is a TTY (the cube needs a real terminal). */
int cube_available(void);

void cube_init(void);			/* read terminal size, hide cursor */
void cube_shutdown(void);		/* show cursor, leave a clean line */

/* Render one frame for orientation q; includes a Roll/Pitch/Yaw header line. */
void cube_render(const quat *q);

/* Project one vertex (cube coords +-1) to screen coords + depth, for tests. */
void cube_project(const quat *q, float vx, float vy, float vz,
		  int rows, int cols, float *px, float *py, float *pz);

#endif /* LG_TOOLS_CUBE_H */
