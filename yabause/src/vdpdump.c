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
 * Developer tool: render a raw VDP1/VDP2 memory dump (for example the frame
 * dumps written by the Azel PC port) without executing any SH-2 code.
 *
 * Dump format: one file per memory region, named <prefix>_<region>.bin.
 *
 *   _vdp2vram.bin  0x80000 bytes  VDP2 VRAM        (0x25E00000)
 *   _vdp2cram.bin  0x01000 bytes  VDP2 color RAM   (0x25F00000)
 *   _vdp2regs.bin  0x00200 bytes  VDP2 registers   (0x25F80000)
 *   _vdp1vram.bin  0x80000 bytes  VDP1 VRAM        (0x25C00000)
 *   _vdp1regs.bin  0x00020 bytes  VDP1 registers   (0x25D00000)
 *
 * Byte order: the files are raw Saturn memory images, i.e. BIG-ENDIAN
 * 16-bit words, exactly as the SH-2 sees them. Verified on the e006 dump:
 * the first word of _vdp2regs.bin reads 0x8001 big-endian (TVMD with DISP,
 * bit 15, set) and the word at offset 0x0E reads 0x13B4 (RAMCTL). No
 * 16-bit swapping is needed; each word is read as (b[0] << 8) | b[1].
 *
 * Every word is written through the regular bus write functions
 * (Vdp2WriteWord, Vdp2ColorRamWriteWord, Vdp2RamWriteWord, Vdp1RamWriteWord,
 * Vdp1WriteWord) so all side effects, caches and renderer notifications are
 * updated exactly as when a game writes the same data.
 *
 * Other files that may sit next to the dump (_vdp1hd, _wramh, _wraml,
 * _sndram, ...) are ignored.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "yabause.h"
#include "vdp1.h"
#include "vdp2.h"
#include "vdpdump.h"

#define VDPDUMP_VDP2VRAM_SIZE 0x80000
#define VDPDUMP_VDP2CRAM_SIZE 0x1000
#define VDPDUMP_VDP2REGS_SIZE 0x200
#define VDPDUMP_VDP1VRAM_SIZE 0x80000
#define VDPDUMP_VDP1REGS_SIZE 0x20

/* VDP2 registers live at 0x000-0x11F; the rest of the 0x200 window is
   unused. */
#define VDPDUMP_VDP2REGS_USED 0x120

/* VDP1 registers that are written back: TVMR, FBCR, PTMR, EWDR, EWLR, EWRR.
   ENDR (0x0C) is skipped because writing it force-stops drawing; EDSR,
   LOPR, COPR and MODR (0x10-0x16) are read-only status registers. */
#define VDPDUMP_VDP1REGS_USED 0x0C

static u8 *vdpdump_read_file(const char *prefix, const char *suffix,
                             size_t size) {
  char path[1024];
  FILE *fp;
  u8 *buf;
  size_t got;

  snprintf(path, sizeof(path), "%s_%s.bin", prefix, suffix);
  fp = fopen(path, "rb");
  if (fp == NULL) {
    printf("[VDPDUMP] cannot open %s\n", path);
    return NULL;
  }
  buf = (u8 *)malloc(size);
  if (buf == NULL) {
    fclose(fp);
    return NULL;
  }
  got = fread(buf, 1, size, fp);
  fclose(fp);
  if (got != size) {
    printf("[VDPDUMP] %s is too short (%u of %u bytes)\n", path,
           (unsigned)got, (unsigned)size);
    free(buf);
    return NULL;
  }
  return buf;
}

static u16 vdpdump_be16(const u8 *p) {
  return (u16)((p[0] << 8) | p[1]);
}

int VdpDumpLoad(const char *prefix) {
  u8 *v2vram = vdpdump_read_file(prefix, "vdp2vram", VDPDUMP_VDP2VRAM_SIZE);
  u8 *v2cram = vdpdump_read_file(prefix, "vdp2cram", VDPDUMP_VDP2CRAM_SIZE);
  u8 *v2regs = vdpdump_read_file(prefix, "vdp2regs", VDPDUMP_VDP2REGS_SIZE);
  u8 *v1vram = vdpdump_read_file(prefix, "vdp1vram", VDPDUMP_VDP1VRAM_SIZE);
  u8 *v1regs = vdpdump_read_file(prefix, "vdp1regs", VDPDUMP_VDP1REGS_SIZE);
  int rtn = -1;
  u32 i;

  if (!v2vram || !v2cram || !v2regs || !v1vram || !v1regs) {
    goto done;
  }

  /* VDP2 registers first: RAMCTL selects the color RAM mode that
     Vdp2ColorRamWriteWord uses to mirror / notify the renderer. */
  for (i = 0; i < VDPDUMP_VDP2REGS_USED; i += 2) {
    Vdp2WriteWord(i, vdpdump_be16(v2regs + i));
  }

  for (i = 0; i < VDPDUMP_VDP2CRAM_SIZE; i += 2) {
    Vdp2ColorRamWriteWord(i, vdpdump_be16(v2cram + i));
  }

  for (i = 0; i < VDPDUMP_VDP2VRAM_SIZE; i += 2) {
    Vdp2RamWriteWord(i, vdpdump_be16(v2vram + i));
  }

  for (i = 0; i < VDPDUMP_VDP1VRAM_SIZE; i += 2) {
    Vdp1RamWriteWord(i, vdpdump_be16(v1vram + i));
  }

  /* VDP1 registers last, so a frame change / plot trigger armed by the
     FBCR / PTMR writes sees the complete command list in VRAM. */
  for (i = 0; i < VDPDUMP_VDP1REGS_USED; i += 2) {
    Vdp1WriteWord(i, vdpdump_be16(v1regs + i));
  }

  rtn = 0;

done:
  free(v2vram);
  free(v2cram);
  free(v2regs);
  free(v1vram);
  free(v1regs);
  return rtn;
}

int VdpDumpRunFrame(const char *prefix) {
  int rtn;

  /* Top of the frame (LineCount == 0, right after VBlank-OUT). In async
     rendering the render thread is idle here: the previous field was
     joined by the VBlank-IN rendez-vous. */
  rtn = VdpDumpLoad(prefix);
  if (rtn != 0) {
    return rtn;
  }

  yabsys.frame_count++;
  yabsys.LineCount = 0;
  yabsys.DecilineCount = 0;

  /* Same scanline sequence as YabauseEmulate(), minus every CPU / SCU /
     SCSP / SMPC / CD step. */
  for (;;) {
    Vdp2HBlankStatusIN();
    Vdp2HBlankIN();
    Vdp2HBlankOUT();
    yabsys.LineCount++;

    if (yabsys.LineCount == yabsys.VBlankLineCount) {
      Vdp2VBlankIN();
    }
    else if (yabsys.LineCount == yabsys.MaxLineCount) {
      Vdp2VBlankOUT();
      yabsys.LineCount = 0;
      break;
    }
  }
  return 0;
}
