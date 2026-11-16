/*  Copyright 2026 devMiyax(smiyaxdev@gmail.com)

    This file is part of YabaSanshiro.

    YabaSanshiro is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    YabaSanshiro is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with YabaSanshiro; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

/* CRT mode for the OpenGL / OpenGL ES renderer (see crt_gl.c). */
#ifndef YAB_CRT_GL_H
#define YAB_CRT_GL_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Draw the rendered Saturn picture through the CRT shader of filter (one of
 * the AA_CRT_* modes) into target_fbo.
 *   src_tex          the picture, GL's way up (line 0 in the top row), at the
 *                    Saturn's resolution: src_w x src_h
 *   field            the field shown this frame for a double-density
 *                    interlaced picture (more than 256 lines): 0 even, 1 odd
 *   rotate           1: turned 90 degrees as rotate_screen does elsewhere
 *   win_*            the drawable area of target_fbo; it is cleared to black
 *                    and the picture goes in the middle (crt_layout.h),
 *                    or at the top when top is 1 (CrtLayoutAlignTop)
 * Returns 0, or -1 when the shader cannot be used (nothing is drawn then).
 */
int YglCrtPresent(int filter, unsigned int src_tex, int src_w, int src_h, int field, int rotate,
                  unsigned int target_fbo, int win_x, int win_y, int win_w, int win_h,
                  int top);

/* Delete every GL object of the CRT mode and forget it; call while the
   renderer's context is still current (YglDeInit). */
void YglCrtDeInit(void);

/* Forget the CRT mode's GL objects without deleting them, for a new context
   (YglInit): names from an old context must never be used or deleted. */
void YglCrtForget(void);

/* 1 when the shader of filter failed to build in this context, so the
   renderer must not render for it (VIDOGLApplyEffectiveResolution). */
int YglCrtUnavailable(int filter);

#ifdef __cplusplus
}
#endif

#endif
