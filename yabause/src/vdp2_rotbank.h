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

#ifndef VDP2_ROTBANK_H
#define VDP2_ROTBANK_H

#include "core.h"

/*
 * VRAM bank access rule for the rotation scroll screens (RBG0 / RBG1).
 *
 * VDP2 VRAM is four 128 KB banks: A0, A1, B0, B1 (bank = byte address >> 17).
 * A rotation screen does not use the cycle pattern registers; it reads only
 * the banks RAMCTL's RDBS fields hand to it:
 *   RDBS = 0 unused, 1 coefficient table, 2 pattern name, 3 character/bitmap.
 * When a bank pair is not split (RAMCTL bit 8 VRAMD for A, bit 9 VRBMD for B)
 * the upper bank (A1 / B1) follows the field of the lower bank (A0 / B0).
 * When RBG1 is on (BGON bit 5), RBG0 cannot use B0/B1, and RBG1 is fixed to
 * pattern names in B1 and characters in B0.
 * A read from a bank the screen may not access returns 0.
 *
 * Source: the reference software renderer, ss/vdp2_render.cpp (nt_ok/cg_ok).
 *
 * Output: bit b of *nt_mask / *cg_mask is set when bank b may be read for
 * pattern names / character (and bitmap) data.
 */
static INLINE void Vdp2RotBankMasks(u16 ramctl, u16 bgon, int is_rbg1, u32 *nt_mask, u32 *cg_mask)
{
   u32 nt = 0;
   u32 cg = 0;
   unsigned bank;
   const int rbg1_on = (bgon & 0x20) != 0;

   if (is_rbg1 && rbg1_on)
   {
      *nt_mask = 0x8;  /* B1 */
      *cg_mask = 0x4;  /* B0 */
      return;
   }

   for (bank = 0; bank < 4; bank++)
   {
      /* Split bit for this pair: VRAMD (bit 8) for A, VRBMD (bit 9) for B. */
      const unsigned split = (ramctl >> (8 + (bank >> 1))) & 1;
      /* Effective field: unsplit pairs use the lower bank's field. */
      const unsigned esb = bank & (2 | split);
      const unsigned rdbs = (ramctl >> (esb << 1)) & 0x3;

      if (rbg1_on && bank >= 2)
         continue;

      if (rdbs == 2)
         nt |= 1u << bank;
      else if (rdbs == 3)
         cg |= 1u << bank;
   }

   *nt_mask = nt;
   *cg_mask = cg;
}

#endif
