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
 * VDP1 texture row stepping, shared by the C OpenGL core, the Vulkan
 * renderer and the compute rasterizer host code (Vdp1ComputeMath.cpp).
 *
 * A textured VDP1 sprite is drawn as dmax + 1 lines, dmax being the larger
 * Chebyshev length of the side edges A->D and B->C. Line k takes texture row
 * vdp1_tex_row(k, dmax + 1, t0, t1): the closed form of the hardware's
 * error-term walk over the rows t0..t1 (the reference software renderer,
 * ss/vdp1_common.h VileTex::Setup and the per-line PreStep in
 * ss/vdp1_sprite.cpp). The GLSL copies in the shaders must stay identical.
 */

#ifndef VDP1TEXROW_H
#define VDP1TEXROW_H

#include <math.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sign-extend the low 13 bits (VDP1 coordinates are 13-bit). */
static inline int vdp1_sext13(int v) { return ((v & 0x1FFF) ^ 0x1000) - 0x1000; }

static inline int vdp1_cheb13(int ax, int ay, int bx, int by) {
  const int dx = abs(vdp1_sext13(bx - ax));
  const int dy = abs(vdp1_sext13(by - ay));
  return dx > dy ? dx : dy;
}

/* v = { A.x, A.y, B.x, B.y, C.x, C.y, D.x, D.y } in raw Saturn coordinates. */
static inline int vdp1_cmd_dmax(const int v[8]) {
  const int ad = vdp1_cheb13(v[0], v[1], v[6], v[7]);
  const int bc = vdp1_cheb13(v[2], v[3], v[4], v[5]);
  return (ad > bc ? ad : bc) & 0xFFF;
}

/* Row taken by line k of n lines stepping from row t0 to row t1. */
static inline int vdp1_tex_row(int k, int n, int t0, int t1) {
  const int dt = t1 - t0;
  const int adt = dt < 0 ? -dt : dt;
  const int rev = (dt < 0) ? 1 : 0;
  long long inc, adj, e, m;
  if (n <= adt) {
    inc = 2 * (long long)(adt + 1);
    adj = 2 * (long long)n;
    e = adt + 1 - (2 * (long long)n + rev);
  } else {
    inc = 2 * (long long)adt;
    adj = 2 * (long long)(n - 1);
    e = n - (2 * (long long)n - rev);
  }
  e += (long long)k * inc;
  m = (e >= 0 && adj != 0) ? e / adj + 1 : 0;
  return (int)(rev ? t0 - m : t0 + m);
}

/*
 * Line parameter for the graphics (GPU tessellation) path. The quad is drawn
 * with the parameter interpolated linearly from k0 on the A-B edge to k1 on
 * the D-C edge; a fragment rounds it to its line number.
 *
 * expanded != 0: the drawn quad covers whole cells (its D-C edge lies one
 * cell past the last line), so a pixel centre of line i sits at i + 0.5 of
 * dmax + 1 lines: k0 = -0.5, k1 = dmax + 0.5.
 *
 * expanded == 0: the drawn quad uses the raw vertices (cell origins). The
 * hardware evaluates a cell at its origin, half a cell up-left of the pixel
 * centre, so the parameter u * dmax is offset by half a cell along the
 * gradient of u, taken at the quad centre.
 */
static inline void vdp1_tess_line_params(const int v[8], int expanded, int *dmax_out,
                                         float *k0, float *k1) {
  const int dmax = vdp1_cmd_dmax(v);
  float c = 0.5f;
  *dmax_out = dmax;
  if (expanded) {
    *k0 = -0.5f;
    *k1 = (float)dmax + 0.5f;
    return;
  }
  {
    /* dP/du (A->D direction) and dP/dv (A->B direction) at u = v = 0.5. */
    const float pux = 0.5f * (float)((v[6] - v[0]) + (v[4] - v[2]));
    const float puy = 0.5f * (float)((v[7] - v[1]) + (v[5] - v[3]));
    const float pvx = 0.5f * (float)((v[2] - v[0]) + (v[4] - v[6]));
    const float pvy = 0.5f * (float)((v[3] - v[1]) + (v[5] - v[7]));
    const float det = pux * pvy - puy * pvx;
    if (fabsf(det) > 1e-6f) {
      /* du/dx = pvy / det, du/dy = -pvx / det. */
      c = 0.5f * (float)dmax * (pvy - pvx) / det;
      if (c > 1.0f) c = 1.0f;
      if (c < -1.0f) c = -1.0f;
    } else {
      c = 0.0f;
    }
  }
  *k0 = -c;
  *k1 = (float)dmax - c;
}

/*
 * Vertex word packing the per-sprite constants for the shaders:
 * -(1 + dmax + 4096 * (adt + 256 * rev)), always <= -1 and exact in a float.
 * adt = h - 1 (texture rows minus one), rev = vertical flip. A value >= 0
 * (the q = 1 the tessellation path otherwise stores) means "rule off".
 */
static inline float vdp1_tess_pack(int dmax, int h, int flipV) {
  int adt = h - 1;
  if (adt < 0) adt = 0;
  if (adt > 255) adt = 255;
  return -(float)(1 + (dmax & 0xFFF) + 4096 * (adt + 256 * (flipV ? 1 : 0)));
}

#ifdef __cplusplus
}
#endif

#endif /* VDP1TEXROW_H */
