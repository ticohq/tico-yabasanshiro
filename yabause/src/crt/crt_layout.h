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

/*
 * CRT mode: where the Saturn picture goes in the window. The picture is
 * 4:3 and as large as the window allows, so a scan line may cover a
 * fractional number of window pixels; it is centred, or at the
 * top where the normal output is (CrtLayoutAlignTop). Shared by
 * the OpenGL (crt_gl.c) and Vulkan renderers.
 */
#ifndef YAB_CRT_LAYOUT_H
#define YAB_CRT_LAYOUT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int x, y, w, h;    /* the picture in the window, GL's way (from the bottom left) */
  int lines;         /* scan lines in a field */
  int line_px;       /* whole window pixels per scan line, rounded down */
  int field_shift;   /* how far down the odd field goes */
} CrtLayout;

/*
 * Where a picture frame_h lines high goes in a win_w x win_h window.
 * Interlace (more than 256 lines) counts the lines of one field, so a
 * 448-line picture gets the scan lines of a 224-line one; field 1 is the
 * odd one and goes half a scan line down.
 */
void CrtLayoutCompute(int frame_h, int win_w, int win_h, int field, CrtLayout* out);

/*
 * Put the picture at the top of the window instead of the middle, for a
 * renderer whose normal output is aligned to the top (full screen with the
 * picture limited by the window's width, i.e. portrait). win_w / win_h are
 * the ones given to CrtLayoutCompute. rotated: the picture is turned for
 * rotate_screen (its top line on the left of the window), so the window's
 * top is the picture's right end and the picture moves along its own x.
 */
void CrtLayoutAlignTop(CrtLayout* l, int win_w, int win_h, int rotated);

#ifdef __cplusplus
}
#endif

#endif
