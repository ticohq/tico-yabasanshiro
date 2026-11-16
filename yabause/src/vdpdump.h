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
 * Developer tool: render a raw VDP1/VDP2 memory dump without running the
 * SH-2. See vdpdump.c for the file format.
 */

#ifndef VDPDUMP_H
#define VDPDUMP_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Load <prefix>_vdp2vram.bin, _vdp2cram.bin, _vdp2regs.bin, _vdp1vram.bin
 * and _vdp1regs.bin into the emulated VDP1/VDP2 through the regular write
 * functions. Returns 0 on success, -1 when a file is missing or short (in
 * that case nothing is written).
 */
int VdpDumpLoad(const char *prefix);

/*
 * Emulate one video frame without any CPU: load the dump at the top of the
 * frame, then walk every scanline through the same HBlank / VBlank entry
 * points YabauseEmulate() uses, so VDP1 drawing (frame change / plot
 * trigger from the dumped registers) and VDP2 composition go through the
 * production paths. Returns the VdpDumpLoad() result.
 */
int VdpDumpRunFrame(const char *prefix);

#ifdef __cplusplus
}
#endif

#endif /* VDPDUMP_H */
