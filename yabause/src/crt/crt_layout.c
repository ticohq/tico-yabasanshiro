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

/* CRT mode: the picture's place in the window (crt_layout.h). */
#include "crt_layout.h"

static int width_of(int h) { return (int)(h * 4.0 / 3.0 + 0.5); }

void CrtLayoutCompute(int frame_h, int win_w, int win_h, int field, CrtLayout* out)
{
  const int interlaced = frame_h > 256;
  const int lines = interlaced ? frame_h / 2 : frame_h;
  /* as large as the window allows at 4:3; a scan line may cover a
     fractional number of window pixels */
  out->h = win_h;
  out->w = width_of(win_h);
  if (out->w > win_w) {
    out->w = win_w;
    out->h = (int)(win_w * 3.0 / 4.0 + 0.5);
  }
  out->x = (win_w - out->w) / 2;
  out->y = (win_h - out->h) / 2;
  out->lines = lines;
  out->line_px = lines > 0 ? out->h / lines : 0;
  /* half a scan line, rounded to whole window pixels */
  out->field_shift = interlaced && field && lines > 0 ? (out->h + lines) / (2 * lines) : 0;
}

void CrtLayoutAlignTop(CrtLayout* l, int win_w, int win_h, int rotated)
{
  /* the layout counts y from the bottom (GL's way) */
  if (rotated) l->x = win_w - l->w;
  else l->y = win_h - l->h;
}
