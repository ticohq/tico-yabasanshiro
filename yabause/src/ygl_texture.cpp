/*
        Copyright 2019 devMiyax(smiyaxdev@gmail.com)

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
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

extern "C"
{
#include "ygl.h"
#include "yui.h"
#include "vidshared.h"

  extern vdp2rotationparameter_struct paraA;
  extern vdp2rotationparameter_struct paraB;
  extern Vdp2 *fixVdp2Regs;
}

#include <math.h>
#include "vdp2_rotbank.h"

// The VRAM banks a rotation screen may read depend on the layer, not on the
// rotation parameter: Vdp2ReadRotationTable() fills the RBG0 masks, and the
// RBG1 draw (rgb_type 0x04, which uses parameter B) replaces them with the
// fixed RBG1 banks. Both upload paths below go through here.
static void rbgApplyLayerBankMasks(int rgb_type, vdp2rotationparameter_struct *a, vdp2rotationparameter_struct *b)
{
  if (rgb_type != 0x04) return;
  Vdp2RotBankMasks(fixVdp2Regs->RAMCTL, fixVdp2Regs->BGON, 1, &a->nt_bank_mask, &a->cg_bank_mask);
  b->nt_bank_mask = a->nt_bank_mask;
  b->cg_bank_mask = a->cg_bank_mask;
}

#if defined(HAVE_VULKAN)
#include "vulkan/VIDVulkan.h"
#include "vulkan/VulkanTools.h"
#include "vulkan/vulkan.hpp"
#endif

// Debug logging for compute shader crash investigation
#ifndef __SWITCH__
#include <stdio.h>
static inline void CS_DEBUG_LOG(const char* fmt, ...) {
    static FILE* cs_log = nullptr;
    if (!cs_log) {
        cs_log = fopen("sdmc:/tico/debug/compute_shader.txt", "a");
    }
    if (cs_log) {
        va_list args;
        va_start(args, fmt);
        vfprintf(cs_log, fmt, args);
        va_end(args);
        fprintf(cs_log, "\n");
        fflush(cs_log);
    }
}
#else
#define CS_DEBUG_LOG(...) do {} while(0)
#endif

#define YGLDEBUG LOG

// debug Color
// imageStore(outSurface,texel,vec4( float(32u&0xFFu)/255.0, float((32u>>8) &0xFFu)/255.0, float((32u>>16) &0xFFu)/255.0, 1.0)); return;

const char prg_generate_rbg_base[] =
#if defined(_OGLES3_)
    "#version 310 es \n"
    "precision highp float; \n"
    "precision highp int;\n"
    "precision highp image2D;\n"
#else
    "#version 430 \n"
#endif
"layout(local_size_x = %d, local_size_y = %d, local_size_z = 1) in;\n"
"layout(rgba8, binding = 0)  uniform writeonly image2D outSurface;\n"
"layout(std430, binding = 1) readonly buffer VDP2 { uint vram[]; };\n"
"struct vdp2rotationparameter_struct{ \n"
" uint PlaneAddrv[16];\n"
" float Xst;\n"
" float Yst;\n"
" float Zst;\n"
" float deltaXst;\n"
" float deltaYst;\n"
" float deltaX;\n"
" float deltaY;\n"
" float A;\n"
" float B;\n"
" float C;\n"
" float D;\n"
" float E;\n"
" float F;\n"
" float Px;\n"
" float Py;\n"
" float Pz;\n"
" float Cx;\n"
" float Cy;\n"
" float Cz;\n"
" float Mx;\n"
" float My;\n"
" float kx;\n"
" float ky;\n"
" float KAst;\n"
" float deltaKAst;\n"
" float deltaKAx;\n"
" uint coeftbladdr;\n"
" int coefenab;\n"
" int coefmode;\n"
" int coefdatasize;\n"
" int use_coef_for_linecolor;\n"
" float Xp;\n"
" float Yp;\n"
" float dX;\n"
" float dY;\n"
" int screenover;\n"
" int msb;\n"
" uint charaddr;\n"
" int planew, planew_bits, planeh, planeh_bits;\n"
" int MaxH, MaxV;\n"
" float Xsp;\n"
" float Ysp;\n"
" float dx;\n"
" float dy;\n"
" float lkx;\n"
" float lky;\n"
" int KtablV;\n"
" int ShiftPaneX;\n"
" int ShiftPaneY;\n"
" int MskH;\n"
" int MskV;\n"
" uint lineaddr;\n"
" int k_mem_type;\n"
" uint over_pattern_name;\n"
" int linecoefenab;\n"
" uint nt_bank_mask;\n"
" uint cg_bank_mask;\n"
"};\n"
"layout(std430, binding = 2) readonly buffer vdp2Param { \n"
"  vdp2rotationparameter_struct para[2];\n"
"};\n"
"layout(std140, binding = 3) uniform  RBGDrawInfo { \n"
"  float hres_scale; \n"
"  float vres_scale; \n"
"  int cellw_; \n"
"  int cellh_; \n"
"  uint paladdr_; \n"
"  int pagesize; \n"
"  int patternshift; \n"
"  int planew; \n"
"  int pagewh; \n"
"  int patterndatasize; \n"
"  uint supplementdata; \n"
"  int auxmode; \n"
"  int patternwh;\n"
"  uint coloroffset;\n"
"  int transparencyenable;\n"
"  int specialcolormode;\n"
"  int specialcolorfunction;\n"
"  uint specialcode;\n"
"  int colornumber;\n"
"  int window_area_mode;"
"  float alpha_;"
"  int cram_shift;"
"  int hires_shift;"
"  int specialprimode;"
"  uint priority;"
"};\n"
" struct vdp2WindowInfo\n"
"{\n"
"  int WinShowLine;\n"
"  int WinHStart;\n"
"  int WinHEnd;\n"
"};\n"
"layout(std430, binding = 4) readonly buffer windowinfo { \n"
"  vdp2WindowInfo pWinInfo[];\n"
"};\n"
"layout(std430, binding = 5) readonly buffer VDP2C { uint cram[]; };\n"
// The coefficient table entry of whole Saturn pixel (px, py). lineaddr is
// written on every path (0 when the entry carries no line colour code).
" int GetKValueAt( int paramid, int px, int py, out float ky, out uint lineaddr ){ \n"
"  uint kdata;\n"
"  lineaddr = 0u;\n"
"  int kindex = int( ceil(para[paramid].deltaKAst*float(py)+(para[paramid].deltaKAx*float(px))) ); \n"
"  if (para[paramid].coefdatasize == 2) { \n"
"    uint addr = ( uint( int(para[paramid].coeftbladdr) + (kindex<<1)) &0x7FFFFu); "
"    if( para[paramid].k_mem_type == 0) { \n"
"	     kdata = vram[ addr>>2 ]; \n"
"      if( (addr & 0x02u) != 0u ) { kdata >>= 16; } \n"
"      kdata = (((kdata>>8)&0xFFu) | ((kdata << 8) & 0xFFFFFF00u))&0xFFFFu;\n"
"    }else{\n"
"      kdata = cram[ ((0x800u + (addr&0x7FFu))>>2)  ]; \n"
"      if( (addr & 0x02u) != 0u ) { kdata >>= 16; } \n"
"    }\n"
"    if ( (kdata & 0x8000u) != 0u) { return -1; }\n"
"	   if((kdata&0x4000u)!=0u) ky=float( int(kdata&0x7FFFu)| int(0xFFFF8000u) )/1024.0; else ky=float(kdata&0x7FFFu)/1024.0;\n"
"  }else{\n"
"    uint addr = ( uint( int(para[paramid].coeftbladdr) + (kindex<<2))&0x7FFFFu);"
"    if( para[paramid].k_mem_type == 0) { \n"
"	     kdata = vram[ addr>>2 ]; \n"
"      kdata = ((kdata&0xFF000000u) >> 24 | ((kdata >> 8) & 0x0000FF00u) | ((kdata<<8) & 0x00FF0000u) | (kdata&0x000000FFu) << 24);\n"
"    }else{\n"
"      kdata = cram[ ((0x800u + (addr&0x7FFu) )>>2) ]; \n"
"      kdata = ((kdata&0xFFFF0000u)>>16|(kdata&0x0000FFFFu)<<16);\n"
"    }\n"
"	 if( para[paramid].linecoefenab != 0) lineaddr = (kdata >> 24) & 0x7Fu; else lineaddr = 0u;\n"
"	 if((kdata&0x80000000u)!=0u){ return -1;}\n"
"	 if((kdata&0x00800000u)!=0u) ky=float( int(kdata&0x00FFFFFFu)| int(0xFF800000u) )/65536.0; else ky=float(kdata&0x00FFFFFFu)/65536.0;\n"
"  }\n"
"  return 0;"
" }\n"

" bool isWindowInside(int posx, int posy) {\n"
" posy <<= hires_shift;"
"	if (window_area_mode == 0) {\n"
"		if (pWinInfo[posy].WinShowLine == 0) {\n"
"			return true;\n"
"		}\n"
"		else {\n"
"			if (posx < pWinInfo[posy].WinHStart || posx >= pWinInfo[posy].WinHEnd) {\n"
"				return true;\n"
"			}\n"
"			else {\n"
"				return false;\n"
"			}\n"
"		}\n"
"	}\n"
"	else {\n"
"		if (pWinInfo[posy].WinShowLine == 0) {\n"
"			return false;\n"
"		}\n"
"		else {\n"
"			if (posx < pWinInfo[posy].WinHStart || posx >= pWinInfo[posy].WinHEnd) {\n"
"				return false;\n"
"			}\n"
"			else {\n"
"				return true;\n"
"			}\n"
"		}\n"
"	}\n"
"	return true;\n"
"}\n"

// The Saturn pixel that contains the dot sampled at (posx, posy). posx/posy
// are corner based (texel * scale), so this is the floor; the small margin
// keeps a scale such as 1/6 (texel * scale = 0.99999994) from picking the
// previous pixel. At the original resolution the positions are integers and
// the margin changes nothing.
"ivec2 rbg_dot_pixel(float posx, float posy) { return ivec2(floor(vec2(posx, posy) + 1.0e-4)); }\n"
// Rotation parameter mode 3: set by prg_get_param_mode03 before it reads the
// coefficient, so GetKValue knows the window picks the parameter set per dot.
"bool rbg_param_mode3 = false;\n"
// Mode 3: the parameter set VDP2 draws whole pixel p with (the same window
// test prg_get_param_mode03 uses), p clamped to the Saturn screen.
"int rbg_param_at(ivec2 p) {\n"
"  ivec2 lim = ivec2(vec2(imageSize(outSurface)) * vec2(hres_scale, vres_scale) + 0.5) - 1;\n"
"  p = clamp(p, ivec2(0), max(lim, ivec2(0)));\n"
"  return isWindowInside(p.x, p.y) ? 0 : 1;\n"
"}\n"
// The coefficient at Saturn position (posx, posy). VDP2 reads the table at
// whole pixels only, so at the original resolution every dot takes exactly
// the entry of its pixel. Above it, a sample between pixels interpolates the
// four entries around it: as 1/k when they share a sign (1/k is linear along a
// plane in perspective), linearly otherwise. It holds the entry of the pixel
// that contains the dot when one of the four is transparent or, in mode 3,
// drawn by the other parameter set. Transparency and the line colour code
// always come from that pixel.
" int GetKValue( int paramid, float posx, float posy, out float ky, out uint lineaddr ){ \n"
"  if (hres_scale == 1.0 && vres_scale == 1.0) return GetKValueAt(paramid, int(posx), int(posy), ky, lineaddr);\n"
"  vec2 s = vec2(posx, posy);\n"
"  ivec2 i0 = rbg_dot_pixel(posx, posy);\n"
"  vec2 f = clamp(s - vec2(i0), 0.0, 1.0);\n"
"  ivec2 n = i0;\n"
"  if (GetKValueAt(paramid, n.x, n.y, ky, lineaddr) == -1) return -1;\n"
"  if (f.x == 0.0 && f.y == 0.0) return 0;\n"
"  if (rbg_param_mode3 && (rbg_param_at(i0) != paramid || rbg_param_at(i0 + ivec2(1, 0)) != paramid ||\n"
"      rbg_param_at(i0 + ivec2(0, 1)) != paramid || rbg_param_at(i0 + ivec2(1, 1)) != paramid)) return 0;\n"
"  vec4 q; uint la;\n"
"  if (GetKValueAt(paramid, i0.x, i0.y, q.x, la) == -1) return 0;\n"
"  if (GetKValueAt(paramid, i0.x + 1, i0.y, q.y, la) == -1) return 0;\n"
"  if (GetKValueAt(paramid, i0.x, i0.y + 1, q.z, la) == -1) return 0;\n"
"  if (GetKValueAt(paramid, i0.x + 1, i0.y + 1, q.w, la) == -1) return 0;\n"
"  if (all(greaterThan(q, vec4(0.0))) || all(lessThan(q, vec4(0.0)))) {\n"
"    vec4 iq = 1.0 / q;\n"
"    ky = 1.0 / mix(mix(iq.x, iq.y, f.x), mix(iq.z, iq.w, f.x), f.y);\n"
"  } else {\n"
"    ky = mix(mix(q.x, q.y, f.x), mix(q.z, q.w, f.x), f.y);\n"
"  }\n"
"  return 0;\n"
" }\n"

// RAMCTL RDBS bank access rule (see vdp2_rotbank.h): a rotation screen reads
// pattern names / character data only from the banks assigned to it; a read
// from any other bank returns 0.
"bool rbg_nt_bank_ok(int paramid, uint a) { return ((para[paramid].nt_bank_mask >> ((a >> 17) & 3u)) & 1u) != 0u; }\n"
"bool rbg_cg_bank_ok(int paramid, uint a) { return ((para[paramid].cg_bank_mask >> ((a >> 17) & 3u)) & 1u) != 0u; }\n"

"uint get_cram_msb(uint colorindex) { \n"
"	uint colorval = 0u; \n"
"	colorindex = (colorindex << cram_shift) & 0xFFFu; \n"
"	colorval = cram[colorindex >> 2]; \n"
"	if ((colorindex & 0x02u) != 0u) { colorval >>= 16; } \n"
"	return (colorval & 0x8000u); \n"
"}\n"


"int PixelIsSpecialPriority( uint specialcode, uint dot ) { \n"
"  dot &= 0xfu; \n"
"  if ( (specialcode & 0x01u) != 0u && (dot == 0u || dot == 1u) ){ return 1;} \n"
"  if ( (specialcode & 0x02u) != 0u && (dot == 2u || dot == 3u) ){ return 1;} \n"
"  if ( (specialcode & 0x04u) != 0u && (dot == 4u || dot == 5u) ){ return 1;} \n"
"  if ( (specialcode & 0x08u) != 0u && (dot == 6u || dot == 7u) ){ return 1;} \n"
"  if ( (specialcode & 0x10u) != 0u && (dot == 8u || dot == 9u) ){ return 1;} \n"
"  if ( (specialcode & 0x20u) != 0u && (dot == 0xau || dot == 0xbu) ){ return 1;} \n"
"  if ( (specialcode & 0x40u) != 0u && (dot == 0xcu || dot == 0xdu) ){ return 1;} \n"
"  if ( (specialcode & 0x80u) != 0u && (dot == 0xeu || dot == 0xfu) ){ return 1;} \n"
"  return 0; \n"
"} \n"

//----------------------------------------------------------------------
// Main
//----------------------------------------------------------------------
"void main(){ \n"
"  int x, y;\n"
"  int paramid = 0;\n"
"  int cellw;\n"
"  uint paladdr; \n"
"  uint charaddr; \n"
"  uint lineaddr = 0u; \n"
"  float ky; \n"
"  uint kdata;\n"
"  uint patternname = 0xFFFFFFFFu;\n"
"  uint specialfunction_in = 0u;\n"
"  uint specialcolorfunction_in = 0u;\n"
"  ivec2 texel = ivec2(gl_GlobalInvocationID.xy);\n"
"  ivec2 size = imageSize(outSurface);\n"
"  uint flipfunction = 0u;\n"
"  uint addr = 0u;\n"
"  if (texel.x >= size.x || texel.y >= size.y ) return;\n"
"  float posx = float(texel.x) * hres_scale;\n"
"  float posy = float(texel.y) * vres_scale;\n"
"  specialfunction_in = (supplementdata >> 9) & 0x1u; \n"
"  specialcolorfunction_in = (supplementdata >> 8) & 0x1u; \n";

char prg_generate_rbg[ sizeof(prg_generate_rbg_base) + 64 ] = {};

const char prg_rbg_rpmd0_2w[] =
    "  paramid = 0; \n"
    "  ky = para[paramid].ky; \n"
    "  lineaddr = para[paramid].lineaddr; \n"
    "  if( para[paramid].coefenab != 0 ){ \n"
    "   if( GetKValue(paramid,posx,posy,ky,lineaddr ) == -1 ) { imageStore(outSurface,texel,vec4(0.0)); return;} \n"
    "  }\n";

const char prg_rbg_rpmd1_2w[] =
    "  paramid = 1; \n"
    "  ky = para[paramid].ky; \n"
    "  lineaddr = para[paramid].lineaddr; \n"
    "  if( para[paramid].coefenab != 0 ){ \n"
    "   if( GetKValue(paramid,posx,posy,ky,lineaddr ) == -1 ) { imageStore(outSurface,texel,vec4(0.0)); return;} \n"
    "  }\n";

// RPMD=2: the coefficient line colour code always comes from parameter A's
// coefficient (gated by A's KLCE), even when the dot switches to parameter B.
// B's coefficient is read into a scratch variable so lineaddr stays A's.
const char prg_rbg_rpmd2_2w[] =
"  paramid = 0; \n"
"  ky = para[paramid].ky; \n"
"  lineaddr = para[paramid].lineaddr; \n"
"  if( para[paramid].coefenab != 0 ){ \n"
"    if( GetKValue(paramid,posx,posy,ky,lineaddr ) == -1 ) { \n"
"      paramid=1;\n"
"      uint lineaddr_b = 0u; \n"
"      if( para[paramid].coefenab != 0 ){ \n"
"        if( GetKValue(paramid,posx,posy,ky,lineaddr_b ) == -1 ) { imageStore(outSurface,texel,vec4(0.0)); return;} \n"
"      }else{ \n"
"        ky = para[paramid].ky; \n"
"      }\n"
"    }\n"
"  }\n";


// Rotation parameter mode 3 chooses between A and B with the rotation
// parameter window and nothing else. In this mode the MSB of BOTH coefficient
// tables is a plain transparency bit (Sega VDP2 manual table 6.4), so a dot
// whose selected parameter reports a transparent coefficient is transparent.
// It must not be re-fetched with the other parameter: the fallback that used
// to be here filled the half of the screen the window handed to B with A's
// plane, which made mode 3 render exactly like mode 0 - the switch was
// invisible. vidsoft has always done it this way.
const char prg_get_param_mode03[] =
"  rbg_param_mode3 = true; \n"
"  paramid = rbg_param_at(rbg_dot_pixel(posx, posy)); \n"
"  ky = para[paramid].ky; \n"
"  lineaddr = para[paramid].lineaddr; \n"
    "        if( para[paramid].coefenab != 0 ){ \n"
    "          if( GetKValue(paramid,posx,posy,ky,lineaddr ) == -1 ) { imageStore(outSurface,texel,vec4(0.0)); return;} \n"
    " }\n";

const char prg_rbg_xy[] =
    "  float Xsp = para[paramid].A * ((para[paramid].Xst + para[paramid].deltaXst * posy) - para[paramid].Px) +\n"
    "  para[paramid].B * ((para[paramid].Yst + para[paramid].deltaYst * posy) - para[paramid].Py) +\n"
    "  para[paramid].C * (para[paramid].Zst - para[paramid].Pz);\n"
    "  float Ysp = para[paramid].D * ((para[paramid].Xst + para[paramid].deltaXst *posy) - para[paramid].Px) +\n"
    "  para[paramid].E * ((para[paramid].Yst + para[paramid].deltaYst * posy) - para[paramid].Py) +\n"
    "  para[paramid].F * (para[paramid].Zst - para[paramid].Pz);\n"
    "  float fh = (ky * (Xsp + para[paramid].dx * posx) + para[paramid].Xp);\n"
    "  float fv = (ky * (Ysp + para[paramid].dy * posx) + para[paramid].Yp);\n";

const char prg_rbg_get_bitmap[] =
    "  cellw = cellw_;\n"
    "  charaddr = para[paramid].charaddr;\n"
    "  paladdr = paladdr_;\n"
    "  switch( para[paramid].screenover){ \n "
    "  case 0: // OVERMODE_REPEAT \n"
    "  case 1: // OVERMODE_SELPATNAME \n"
    "    x = int(fh) & (cellw-1);\n"
    "    y = int(fv) & (cellh_-1);\n"
    "    break;\n"
    "  case 2: // OVERMODE_TRANSE \n"
    "    if ((fh < 0.0) || (fh > float(cellw_)) || (fv < 0.0) || (fv > float(cellh_)) ) {\n"
    "      imageStore(outSurface,texel,vec4(0.0));\n"
    "      return; \n"
    "    }\n"
    "    x = int(fh);\n"
    "    y = int(fv);\n"
    "    break;\n"
    "  case 3: // OVERMODE_512 \n"
    "    if ((fh < 0.0) || (fh > 512.0) || (fv < 0.0) || (fv > 512.0)) {\n"
    "      imageStore(outSurface,texel,vec4(0.0));\n"
    "      return; \n"
    "    }\n"
    "    x = int(fh);\n"
    "    y = int(fv);\n"
    "    break;\n"
    "  }\n";

const char prg_rbg_overmode_repeat[] =
    "  switch( para[paramid].screenover){ \n "
    "  case 0: // OVERMODE_REPEAT \n"
    "    x = int(fh) & (para[paramid].MaxH-1);\n"
    "    y = int(fv) & (para[paramid].MaxV-1);\n"
    "    break;\n"
    "  case 1: // OVERMODE_SELPATNAME \n"
    "    if ((fh < 0.0) || (fh > float(para[paramid].MaxH)) || (fv < 0.0) || (fv > float(para[paramid].MaxV)) ) {\n"
    "        patternname = para[paramid].over_pattern_name;\n"
    "    }\n"
    "    x = int(fh);\n"
    "    y = int(fv);\n"
    "    break;\n"
    "  case 2: // OVERMODE_TRANSE \n"
    "    if ((fh < 0.0) || (fh > float(para[paramid].MaxH) ) || (fv < 0.0) || (fv > float(para[paramid].MaxV)) ) {\n"
    "      imageStore(outSurface,texel,vec4(0.0));\n"
    "      return; \n"
    "    }\n"
    "    x = int(fh);\n"
    "    y = int(fv);\n"
    "    break;\n"
    "  case 3: // OVERMODE_512 \n"
    "    if ((fh < 0.0) || (fh > 512.0) || (fv < 0.0) || (fv > 512.0)) {\n"
    "      imageStore(outSurface,texel,vec4(0.0));\n"
    "      return; \n"
    "    }\n"
    "    x = int(fh);\n"
    "    y = int(fv);\n"
    "    break;\n"
    "   }\n";

const char prg_rbg_get_patternaddr[] =
    "  int planenum = (x >> para[paramid].ShiftPaneX) + ((y >> para[paramid].ShiftPaneY) << 2);\n"
    "  x &= (para[paramid].MskH);\n"
    "  y &= (para[paramid].MskV);\n"
    "  addr = para[paramid].PlaneAddrv[planenum];\n"
    "  addr += uint( (((y >> 9) * pagesize * planew) + \n"
    "  ((x >> 9) * pagesize) + \n"
    "  (((y & 511) >> patternshift) * pagewh) + \n"
    "  ((x & 511) >> patternshift)) << patterndatasize ); \n"
    "  addr &= 0x7FFFFu;\n";

const char prg_rbg_get_pattern_data_1w[] =
"  if( patternname == 0xFFFFFFFFu){\n"
"    patternname = vram[addr>>2]; \n" // WORD mode( patterndatasize == 1 )
"    if( !rbg_nt_bank_ok(paramid, addr) ) patternname = 0u; \n"
"    if( (addr & 0x02u) != 0u ) { patternname >>= 16; } \n"
"    patternname = (((patternname>>8)&0xFFu) | ((patternname << 8) & 0xFFFFFF00u)) & 0xFFFFu;\n"
"  }\n"
"  if(colornumber==0) paladdr = ((patternname & 0xF000u) >> 12) | ((supplementdata & 0xE0u) >> 1); else paladdr = (patternname & 0x7000u) >> 8;\n" // not in 16 colors
"  switch (auxmode)\n"
"  {\n"
"  case 0: \n"
"    flipfunction = (patternname & 0xC00u) >> 10;\n"
"    switch (patternwh)\n"
"    {\n"
"    case 1:\n"
"      charaddr = (patternname & 0x3FFu) | ((supplementdata & 0x1Fu) << 10);\n"
"      break;\n"
"    case 2:\n"
"      charaddr = ((patternname & 0x3FFu) << 2) | (supplementdata & 0x3u) | ((supplementdata & 0x1Cu) << 10);\n"
"      break;\n"
"    }\n"
"    break;\n"
"  case 1:\n"
"    flipfunction = 0u;\n"
"    switch (patternwh)\n"
"    {\n"
"    case 1:\n"
"      charaddr = (patternname & 0xFFFu) | ((supplementdata & 0x1Cu) << 10);\n"
"      break;\n"
"    case 2:\n"
"      charaddr = ((patternname & 0xFFFu) << 2) | (supplementdata & 0x3u) | ((supplementdata & 0x10u) << 10);\n"
"      break;\n"
"    }\n"
"    break;\n"
"  }\n"
"  charaddr &= 0x3FFFu;\n"
"  charaddr *= 0x20u;\n";

const char prg_rbg_get_pattern_data_2w[] =
"  patternname = vram[addr>>2]; \n"
"  if( !rbg_nt_bank_ok(paramid, addr) ) patternname = 0u; \n"
"  uint tmp1 = patternname & 0x7FFFu; \n"
"  charaddr = patternname >> 16; \n"
"  charaddr = (((charaddr>>8)&0xFFu) | ((charaddr << 8) & 0xFFFFFF00u)) & 0xFFFFu;\n"
"  tmp1 = ((tmp1>>8)&0xFFu) | ((tmp1 << 8) & 0xFFFFFF00u);\n"
"  flipfunction = (tmp1 & 0xC000u) >> 14;\n"
"  if(colornumber==0) paladdr = tmp1 & 0x7Fu; else paladdr = tmp1 & 0x70u;\n" // not in 16 colors
"  specialfunction_in = (tmp1 & 0x2000u) >> 13;\n"
"  specialcolorfunction_in = (tmp1 & 0x1000u) >> 12;\n"
"  charaddr &= 0x3FFFu;\n"
"  charaddr *= 0x20u;\n";

const char prg_rbg_get_charaddr[] =
    "  cellw = 8; \n"
    "  if (patternwh == 1) { \n" // Figure out which pixel in the tile we want
    "    x &= 0x07;\n"
    "    y &= 0x07;\n"
    "    if ( (flipfunction & 0x2u) != 0u ) y = 7 - y;\n"
    "    if ( (flipfunction & 0x1u) != 0u ) x = 7 - x;\n"
    "  }else{\n"
    "    if (flipfunction != 0u) { \n"
    "      y &= 16 - 1;\n"
    "      if ( (flipfunction & 0x2u) != 0u ) {\n"
    "        if ( (y & 8) == 0 ) {\n"
    "          y = 8 - 1 - y + 16;\n"
    "        }else{ \n"
    "          y = 16 - 1 - y;\n"
    "        }\n"
    "      } else if ( (y & 8) != 0 ) { \n"
    "        y += 8; \n"
    "      }\n"
    "      if ((flipfunction & 0x1u) != 0u ) {\n"
    "        if ( (x & 8) == 0 ) y += 8;\n"
    "        x &= 8 - 1;\n"
    "        x = 8 - 1 - x;\n"
    "      } else if ( (x & 8) != 0 ) {\n"
    "        y += 8;\n"
    "        x &= 8 - 1;\n"
    "      } else {\n"
    "        x &= 8 - 1;\n"
    "      }\n"
    "    }else{\n"
    "      y &= 16 - 1;\n"
    "      if ( (y & 8) != 0 ) y += 8;\n"
    "      if ( (x & 8) != 0 ) y += 8;\n"
    "      x &= 8 - 1;\n"
    "    }\n"
    "  }\n";

// 4 BPP
const char prg_rbg_getcolor_4bpp[] =
"  uint dot = 0u;\n"
"  uint cramindex = 0u;\n"
"  float alpha = alpha_;\n"
"  uint dotaddr = ((charaddr + uint(((y * cellw) + x) >> 1)) & 0x7FFFFu);\n"
"  dot = vram[ dotaddr >> 2];\n"
"  if( !rbg_cg_bank_ok(paramid, dotaddr) ) dot = 0u;\n"
"  if( (dotaddr & 0x3u) == 0u ) dot >>= 0;\n"
"  else if( (dotaddr & 0x3u) == 1u ) dot >>= 8;\n"
"  else if( (dotaddr & 0x3u) == 2u ) dot >>= 16;\n"
"  else if( (dotaddr & 0x3u) == 3u ) dot >>= 24;\n"
"  if ( (x & 0x1) == 0 ) dot >>= 4;\n"
"  if ( (dot & 0xFu) == 0u && transparencyenable != 0 ) { \n"
"    cramindex = 0u; \n"
"    alpha = 0.0;\n"
"  } else {\n"
"    cramindex = (coloroffset + ((paladdr << 4) | (dot & 0xFu)));\n"
"    if (specialprimode == 2) { \n"
"      uint spriority = priority & 0xEu; \n"
"      if ( (specialfunction_in & 0x01u) != 0u ) { \n"
"        if( PixelIsSpecialPriority(specialcode, dot ) == 1 ){ \n"
"          spriority |= 0x1u; \n"
"        }\n"
"      }\n"
"      cramindex |= spriority << 16; \n"
// SFPRMD mode 1 (per-character special priority): the priority LSB is the
// character's special function bit, constant across the character's texels.
// Bake it into cramindex bits 16-18 (-> texture B channel) exactly like the
// CPU decoder (VIDVulkan setSpecialPriority). The new-composite GBuffer
// companion reads per-pixel priority from B whenever SFPRMD != 0; without
// this bake every RBG0 pixel reported priority 0 and the compositor dropped
// the whole layer (Sega Rally: black rotation plane).
"    } else if (specialprimode == 1) { \n"
"      uint spriority = (priority & 0xEu) | (specialfunction_in & 0x1u); \n"
"      cramindex |= spriority << 16; \n"
    "    }\n"
    "    switch (specialcolormode)\n"
    "    {\n"
    "    case 1:\n"
    "      if (specialcolorfunction == 0) { alpha = 1.0; } break;\n"
    "    case 2:\n"
    "      if (specialcolorfunction == 0) { alpha = 1.0; }\n"
    "      else { if ((specialcode & (1u << ((dot & 0xFu) >> 1))) == 0u) { alpha = 1.0; } } \n"
    "      break; \n"
    "    case 3:\n"
    "	   if (get_cram_msb(cramindex) == 0u) { alpha = 1.0; }\n"
    "	   break;\n"
    "    }\n"
    "  }\n";

// 8BPP
const char prg_rbg_getcolor_8bpp[] =
"  uint dot = 0u;\n"
"  uint cramindex = 0u;\n"
"  float alpha = alpha_;\n"
"  uint dotaddr = charaddr + uint((y*cellw)+x);\n"
"  dot = vram[ dotaddr >> 2];\n"
"  if( !rbg_cg_bank_ok(paramid, dotaddr) ) dot = 0u;\n"
"  if( (dotaddr & 0x3u) == 0u ) dot >>= 0;\n"
"  else if( (dotaddr & 0x3u) == 1u ) dot >>= 8;\n"
"  else if( (dotaddr & 0x3u) == 2u ) dot >>= 16;\n"
"  else if( (dotaddr & 0x3u) == 3u ) dot >>= 24;\n"
"  dot = dot & 0xFFu; \n"
"  if ( dot == 0u && transparencyenable != 0 ) { \n"
"    cramindex = 0u; \n"
"    alpha = 0.0;\n"
"  } else {\n"
"    cramindex = (coloroffset + ((paladdr << 4) | dot));\n"
"    if (specialprimode == 2) { \n"
"      uint spriority = priority & 0xEu; \n"
"      if ( (specialfunction_in & 0x01u) != 0u) { \n"
"        if( PixelIsSpecialPriority(specialcode, dot ) == 1 ){ \n"
"          spriority |= 0x1u; \n"
"        }\n"
"      }\n"
"      cramindex |= spriority << 16; \n"
// SFPRMD mode 1: bake the per-character priority into B, mirroring the CPU
// decoder (setSpecialPriority). See the 4bpp variant above for details.
"    } else if (specialprimode == 1) { \n"
"      uint spriority = (priority & 0xEu) | (specialfunction_in & 0x1u); \n"
"      cramindex |= spriority << 16; \n"
    "    }\n"
    "    switch (specialcolormode)\n"
    "    {\n"
    "    case 1:\n"
    "      if (specialcolorfunction == 0) { alpha = 1.0; } break;\n"
    "    case 2:\n"
    "      if (specialcolorfunction == 0) { alpha = 1.0; }\n"
    "      else { if ((specialcode & (1u << ((dot & 0xFu) >> 1))) == 0u) { alpha = 1.0; } } \n"
    "      break; \n"
    "    case 3:\n"
    "	   if (get_cram_msb(cramindex) == 0u) { alpha = 1.0; }\n"
    "	   break;\n"
    "    }\n"
    "  }\n";

const char prg_rbg_getcolor_16bpp_palette[] =
"  uint dot = 0u;\n"
"  uint cramindex = 0u;\n"
"  float alpha = alpha_;\n"
"  uint dotaddr = charaddr + uint((y*cellw)+x) * 2u;\n"
"  dot = vram[dotaddr>>2]; \n"
"  if( !rbg_cg_bank_ok(paramid, dotaddr) ) dot = 0u;\n"
"  if( (dotaddr & 0x02u) != 0u ) { dot >>= 16; } \n"
"  dot = (((dot>>8)&0xFFu) | ((dot << 8) & 0xFFFFFF00u))&0xFFFFu;\n"
"  //dot = (((dot) >> 8 & 0xFF) | ((dot) & 0xFF) << 8);\n"
"  if ( dot == 0 && transparencyenable != 0 ) { \n"
"    cramindex = 0u; \n"
"    alpha = 0.0;\n"
"  } else {\n"
"    cramindex = (coloroffset + dot);\n"
"  }\n";

const char prg_rbg_getcolor_16bpp_rbg[] =
"  uint dot = 0u;\n"
"  uint cramindex = 0u;\n"
"  float alpha = alpha_;\n"
"  uint dotaddr = charaddr + uint((y*cellw)+x) * 2u;\n"
"  dot = vram[dotaddr>>2]; \n"
"  if( !rbg_cg_bank_ok(paramid, dotaddr) ) dot = 0u;\n"
"  if( (dotaddr & 0x02u) != 0u ) { dot >>= 16; } \n"
"  dot = (((dot>>8)&0xFFu) | ((dot << 8) & 0xFFFFFF00u))&0xFFFFu;\n"
"  //dot = (((dot >> 8) & 0xFFu) | ((dot) & 0xFFu) << 8);\n"
"  if ( (dot&0x8000u) == 0u && transparencyenable != 0 ) { \n"
"    cramindex = 0u; \n"
"    alpha = 0.0;\n"
"  } else {\n"
"    cramindex = (dot & 0x1Fu) << 3 | (dot & 0x3E0u) << 6 | (dot & 0x7C00u) << 9;\n"
"  }\n";


const char prg_rbg_getcolor_32bpp_rbg[] =
"  uint dot = 0u;\n"
"  uint cramindex = 0u;\n"
"  float alpha = alpha_;\n"
"  uint dotaddr = charaddr + uint((y*cellw)+x) * 4u;\n"
"  dot = vram[dotaddr>>2]; \n"
"  if( !rbg_cg_bank_ok(paramid, dotaddr) ) dot = 0u;\n"
"  dot = ((dot&0xFF000000u) >> 24 | ((dot >> 8) & 0x0000FF00u) | ((dot<<8) & 0x00FF0000u) | (dot&0x000000FFu) << 24);\n"
"  if ( (dot&0x80000000u) == 0u && transparencyenable != 0 ) { \n"
"    cramindex = 0u; \n"
"    alpha = 0.0;\n"
"  } else {\n"
"    cramindex = dot & 0x00FFFFFFu;\n"
"  }\n";


const char prg_generate_rbg_end[] =
    "  imageStore(outSurface,texel,vec4( float(cramindex&0xFFu)/255.0, float((cramindex>>8) &0xFFu)/255.0, float((cramindex>>16) &0xFFu)/255.0, alpha));\n"
    "}\n";

const char prg_generate_rbg_line_end[] =
    "  cramindex |= 0x8000u;\n"
    "  uint line_color = 0u;\n"
    "  if( lineaddr != 0xFFFFFFFFu && lineaddr != 0u ) line_color = ((lineaddr & 0x7Fu) | 0x80u);"
    "  imageStore(outSurface,texel,vec4( float(cramindex&0xFFu)/255.0, float((cramindex>>8)&0xFFu)/255.0, float((line_color)&0xFFu)/255.0, alpha));\n"
    "}\n";

const GLchar *a_prg_rbg_0_2w_bitmap[] = {
    prg_generate_rbg,
    prg_rbg_rpmd0_2w,
    prg_rbg_xy,
    prg_rbg_get_bitmap,
    prg_rbg_getcolor_8bpp,
    prg_generate_rbg_end};

const GLchar *a_prg_rbg_0_2w_p1_4bpp[] = {
    prg_generate_rbg,
    prg_rbg_rpmd0_2w,
    prg_rbg_xy,
    prg_rbg_overmode_repeat,
    prg_rbg_get_patternaddr,
    prg_rbg_get_pattern_data_1w,
    prg_rbg_get_charaddr,
    prg_rbg_getcolor_4bpp,
    prg_generate_rbg_end};

const GLchar *a_prg_rbg_0_2w_p2_4bpp[] = {
    prg_generate_rbg,
    prg_rbg_rpmd0_2w,
    prg_rbg_xy,
    prg_rbg_overmode_repeat,
    prg_rbg_get_patternaddr,
    prg_rbg_get_pattern_data_2w,
    prg_rbg_get_charaddr,
    prg_rbg_getcolor_4bpp,
    prg_generate_rbg_end};

const GLchar *a_prg_rbg_0_2w_p1_8bpp[] = {
    prg_generate_rbg,
    prg_rbg_rpmd0_2w,
    prg_rbg_xy,
    prg_rbg_overmode_repeat,
    prg_rbg_get_patternaddr,
    prg_rbg_get_pattern_data_1w,
    prg_rbg_get_charaddr,
    prg_rbg_getcolor_8bpp,
    prg_generate_rbg_end};

const GLchar *a_prg_rbg_0_2w_p2_8bpp[] = {
    prg_generate_rbg,
    prg_rbg_rpmd0_2w,
    prg_rbg_xy,
    prg_rbg_overmode_repeat,
    prg_rbg_get_patternaddr,
    prg_rbg_get_pattern_data_2w,
    prg_rbg_get_charaddr,
    prg_rbg_getcolor_8bpp,
    prg_generate_rbg_end};

//--------------------------------------------------
// RPMD 1
const GLchar *a_prg_rbg_1_2w_bitmap[] = {
    prg_generate_rbg,
    prg_rbg_rpmd1_2w,
    prg_rbg_xy,
    prg_rbg_get_bitmap,
    prg_rbg_getcolor_8bpp,
    prg_generate_rbg_end};

const GLchar *a_prg_rbg_1_2w_p1_4bpp[] = {
    prg_generate_rbg,
    prg_rbg_rpmd1_2w,
    prg_rbg_xy,
    prg_rbg_overmode_repeat,
    prg_rbg_get_patternaddr,
    prg_rbg_get_pattern_data_1w,
    prg_rbg_get_charaddr,
    prg_rbg_getcolor_4bpp,
    prg_generate_rbg_end};

const GLchar *a_prg_rbg_1_2w_p2_4bpp[] = {
    prg_generate_rbg,
    prg_rbg_rpmd1_2w,
    prg_rbg_xy,
    prg_rbg_overmode_repeat,
    prg_rbg_get_patternaddr,
    prg_rbg_get_pattern_data_2w,
    prg_rbg_get_charaddr,
    prg_rbg_getcolor_4bpp,
    prg_generate_rbg_end};

const GLchar *a_prg_rbg_1_2w_p1_8bpp[] = {
    prg_generate_rbg,
    prg_rbg_rpmd1_2w,
    prg_rbg_xy,
    prg_rbg_overmode_repeat,
    prg_rbg_get_patternaddr,
    prg_rbg_get_pattern_data_1w,
    prg_rbg_get_charaddr,
    prg_rbg_getcolor_8bpp,
    prg_generate_rbg_end};

const GLchar *a_prg_rbg_1_2w_p2_8bpp[] = {
    prg_generate_rbg,
    prg_rbg_rpmd1_2w,
    prg_rbg_xy,
    prg_rbg_overmode_repeat,
    prg_rbg_get_patternaddr,
    prg_rbg_get_pattern_data_2w,
    prg_rbg_get_charaddr,
    prg_rbg_getcolor_8bpp,
    prg_generate_rbg_end};

//--------------------------------------------------
// RPMD 2
const GLchar *a_prg_rbg_2_2w_bitmap[] = {
    prg_generate_rbg,
    prg_rbg_rpmd2_2w,
    prg_rbg_xy,
    prg_rbg_get_bitmap,
    prg_rbg_getcolor_8bpp,
    prg_generate_rbg_end};

const GLchar *a_prg_rbg_2_2w_p1_4bpp[] = {
    prg_generate_rbg,
    prg_rbg_rpmd2_2w,
    prg_rbg_xy,
    prg_rbg_overmode_repeat,
    prg_rbg_get_patternaddr,
    prg_rbg_get_pattern_data_1w,
    prg_rbg_get_charaddr,
    prg_rbg_getcolor_4bpp,
    prg_generate_rbg_end};

const GLchar *a_prg_rbg_2_2w_p2_4bpp[] = {
    prg_generate_rbg,
    prg_rbg_rpmd2_2w,
    prg_rbg_xy,
    prg_rbg_overmode_repeat,
    prg_rbg_get_patternaddr,
    prg_rbg_get_pattern_data_2w,
    prg_rbg_get_charaddr,
    prg_rbg_getcolor_4bpp,
    prg_generate_rbg_end};

const GLchar *a_prg_rbg_2_2w_p1_8bpp[] = {
    prg_generate_rbg,
    prg_rbg_rpmd2_2w,
    prg_rbg_xy,
    prg_rbg_overmode_repeat,
    prg_rbg_get_patternaddr,
    prg_rbg_get_pattern_data_1w,
    prg_rbg_get_charaddr,
    prg_rbg_getcolor_8bpp,
    prg_generate_rbg_end};

const GLchar *a_prg_rbg_2_2w_p2_8bpp[] = {
    prg_generate_rbg,
    prg_rbg_rpmd2_2w,
    prg_rbg_xy,
    prg_rbg_overmode_repeat,
    prg_rbg_get_patternaddr,
    prg_rbg_get_pattern_data_2w,
    prg_rbg_get_charaddr,
    prg_rbg_getcolor_8bpp,
    prg_generate_rbg_end};

//--------------------------------------------------
// RPMD 3
const GLchar *a_prg_rbg_3_2w_bitmap[] = {
    prg_generate_rbg,
    prg_get_param_mode03,
    prg_rbg_xy,
    prg_rbg_get_bitmap,
    prg_rbg_getcolor_8bpp,
    prg_generate_rbg_end};

const GLchar *a_prg_rbg_3_2w_p1_4bpp[] = {
    prg_generate_rbg,
    prg_get_param_mode03,
    prg_rbg_xy,
    prg_rbg_overmode_repeat,
    prg_rbg_get_patternaddr,
    prg_rbg_get_pattern_data_1w,
    prg_rbg_get_charaddr,
    prg_rbg_getcolor_4bpp,
    prg_generate_rbg_end};

const GLchar *a_prg_rbg_3_2w_p2_4bpp[] = {
    prg_generate_rbg,
    prg_get_param_mode03,
    prg_rbg_xy,
    prg_rbg_overmode_repeat,
    prg_rbg_get_patternaddr,
    prg_rbg_get_pattern_data_2w,
    prg_rbg_get_charaddr,
    prg_rbg_getcolor_4bpp,
    prg_generate_rbg_end};

const GLchar *a_prg_rbg_3_2w_p1_8bpp[] = {
    prg_generate_rbg,
    prg_get_param_mode03,
    prg_rbg_xy,
    prg_rbg_overmode_repeat,
    prg_rbg_get_patternaddr,
    prg_rbg_get_pattern_data_1w,
    prg_rbg_get_charaddr,
    prg_rbg_getcolor_8bpp,
    prg_generate_rbg_end};

const GLchar *a_prg_rbg_3_2w_p2_8bpp[] = {
    prg_generate_rbg,
    prg_get_param_mode03,
    prg_rbg_xy,
    prg_rbg_overmode_repeat,
    prg_rbg_get_patternaddr,
    prg_rbg_get_pattern_data_2w,
    prg_rbg_get_charaddr,
    prg_rbg_getcolor_8bpp,
    prg_generate_rbg_end};

struct RBGUniform
{
  RBGUniform()
  {
    pagesize = 0;
    patternshift = 0;
    planew = 0;
    pagewh = 0;
    patterndatasize = 0;
    supplementdata = 0;
    auxmode = 0;
    patternwh = 0;
    coloroffset = 0;
    transparencyenable = 0;
    specialcolormode = 0;
    specialcolorfunction = 0;
    specialcode = 0;
    window_area_mode = 0;
    alpha_ = 0.0;
    cram_shift = 1;
    hires_shift = 0;
    specialprimode = 0;
    priority = 0;
  }
  float hres_scale;
  float vres_scale;
  int cellw;
  int cellh;
  int paladdr_;
  int pagesize;
  int patternshift;
  int planew;
  int pagewh;
  int patterndatasize;
  int supplementdata;
  int auxmode;
  int patternwh;
  unsigned int coloroffset;
  int transparencyenable;
  int specialcolormode;
  int specialcolorfunction;
  unsigned int specialcode;
  int colornumber;
  int window_area_mode;
  float alpha_;
  int cram_shift;
  int hires_shift;
  int specialprimode;
  unsigned int priority;
};

// GL compute-shader RBG generator — only for builds that link OpenGL.
// (Vulkan-only builds use RBGGeneratorVulkan below; the shared shader
// sources and RBGUniform above stay available to both.)
#if defined(HAVE_LIBGL) || defined(_OGLES3_)
class RBGGenerator
{
  GLuint prg_rbg_0_2w_bitmap_4bpp_ = 0;
  GLuint prg_rbg_0_2w_bitmap_8bpp_ = 0;
  GLuint prg_rbg_0_2w_bitmap_16bpp_p_ = 0;
  GLuint prg_rbg_0_2w_bitmap_16bpp_ = 0;
  GLuint prg_rbg_0_2w_bitmap_32bpp_ = 0;
  GLuint prg_rbg_0_2w_p1_4bpp_ = 0;
  GLuint prg_rbg_0_2w_p2_4bpp_ = 0;
  GLuint prg_rbg_0_2w_p1_8bpp_ = 0;
  GLuint prg_rbg_0_2w_p2_8bpp_ = 0;
  GLuint prg_rbg_0_2w_p1_16bpp_p_ = 0;
  GLuint prg_rbg_0_2w_p2_16bpp_p_ = 0;
  GLuint prg_rbg_0_2w_p1_16bpp_ = 0;
  GLuint prg_rbg_0_2w_p2_16bpp_ = 0;
  GLuint prg_rbg_0_2w_p1_32bpp_ = 0;
  GLuint prg_rbg_0_2w_p2_32bpp_ = 0;

  GLuint prg_rbg_1_2w_bitmap_4bpp_ = 0;
  GLuint prg_rbg_1_2w_bitmap_8bpp_ = 0;
  GLuint prg_rbg_1_2w_bitmap_16bpp_p_ = 0;
  GLuint prg_rbg_1_2w_bitmap_16bpp_ = 0;
  GLuint prg_rbg_1_2w_bitmap_32bpp_ = 0;
  GLuint prg_rbg_1_2w_p1_4bpp_ = 0;
  GLuint prg_rbg_1_2w_p2_4bpp_ = 0;
  GLuint prg_rbg_1_2w_p1_8bpp_ = 0;
  GLuint prg_rbg_1_2w_p2_8bpp_ = 0;
  GLuint prg_rbg_1_2w_p1_16bpp_p_ = 0;
  GLuint prg_rbg_1_2w_p2_16bpp_p_ = 0;
  GLuint prg_rbg_1_2w_p1_16bpp_ = 0;
  GLuint prg_rbg_1_2w_p2_16bpp_ = 0;
  GLuint prg_rbg_1_2w_p1_32bpp_ = 0;
  GLuint prg_rbg_1_2w_p2_32bpp_ = 0;

  GLuint prg_rbg_2_2w_bitmap_4bpp_ = 0;
  GLuint prg_rbg_2_2w_bitmap_8bpp_ = 0;
  GLuint prg_rbg_2_2w_bitmap_16bpp_p_ = 0;
  GLuint prg_rbg_2_2w_bitmap_16bpp_ = 0;
  GLuint prg_rbg_2_2w_bitmap_32bpp_ = 0;
  GLuint prg_rbg_2_2w_p1_4bpp_ = 0;
  GLuint prg_rbg_2_2w_p2_4bpp_ = 0;
  GLuint prg_rbg_2_2w_p1_8bpp_ = 0;
  GLuint prg_rbg_2_2w_p2_8bpp_ = 0;
  GLuint prg_rbg_2_2w_p1_16bpp_p_ = 0;
  GLuint prg_rbg_2_2w_p2_16bpp_p_ = 0;
  GLuint prg_rbg_2_2w_p1_16bpp_ = 0;
  GLuint prg_rbg_2_2w_p2_16bpp_ = 0;
  GLuint prg_rbg_2_2w_p1_32bpp_ = 0;
  GLuint prg_rbg_2_2w_p2_32bpp_ = 0;

  GLuint prg_rbg_3_2w_bitmap_4bpp_ = 0;
  GLuint prg_rbg_3_2w_bitmap_8bpp_ = 0;
  GLuint prg_rbg_3_2w_bitmap_16bpp_p_ = 0;
  GLuint prg_rbg_3_2w_bitmap_16bpp_ = 0;
  GLuint prg_rbg_3_2w_bitmap_32bpp_ = 0;
  GLuint prg_rbg_3_2w_p1_4bpp_ = 0;
  GLuint prg_rbg_3_2w_p2_4bpp_ = 0;
  GLuint prg_rbg_3_2w_p1_8bpp_ = 0;
  GLuint prg_rbg_3_2w_p2_8bpp_ = 0;
  GLuint prg_rbg_3_2w_p1_16bpp_p_ = 0;
  GLuint prg_rbg_3_2w_p2_16bpp_p_ = 0;
  GLuint prg_rbg_3_2w_p1_16bpp_ = 0;
  GLuint prg_rbg_3_2w_p2_16bpp_ = 0;
  GLuint prg_rbg_3_2w_p1_32bpp_ = 0;
  GLuint prg_rbg_3_2w_p2_32bpp_ = 0;

  GLuint prg_rbg_0_2w_bitmap_4bpp_line_ = 0;
  GLuint prg_rbg_0_2w_bitmap_8bpp_line_ = 0;
  GLuint prg_rbg_0_2w_bitmap_16bpp_p_line_ = 0;
  GLuint prg_rbg_0_2w_bitmap_16bpp_line_ = 0;
  GLuint prg_rbg_0_2w_bitmap_32bpp_line_ = 0;
  GLuint prg_rbg_0_2w_p1_4bpp_line_ = 0;
  GLuint prg_rbg_0_2w_p2_4bpp_line_ = 0;
  GLuint prg_rbg_0_2w_p1_8bpp_line_ = 0;
  GLuint prg_rbg_0_2w_p2_8bpp_line_ = 0;
  GLuint prg_rbg_0_2w_p1_16bpp_p_line_ = 0;
  GLuint prg_rbg_0_2w_p2_16bpp_p_line_ = 0;
  GLuint prg_rbg_0_2w_p1_16bpp_line_ = 0;
  GLuint prg_rbg_0_2w_p2_16bpp_line_ = 0;
  GLuint prg_rbg_0_2w_p1_32bpp_line_ = 0;
  GLuint prg_rbg_0_2w_p2_32bpp_line_ = 0;

  GLuint prg_rbg_1_2w_bitmap_4bpp_line_ = 0;
  GLuint prg_rbg_1_2w_bitmap_8bpp_line_ = 0;
  GLuint prg_rbg_1_2w_bitmap_16bpp_p_line_ = 0;
  GLuint prg_rbg_1_2w_bitmap_16bpp_line_ = 0;
  GLuint prg_rbg_1_2w_bitmap_32bpp_line_ = 0;
  GLuint prg_rbg_1_2w_p1_4bpp_line_ = 0;
  GLuint prg_rbg_1_2w_p2_4bpp_line_ = 0;
  GLuint prg_rbg_1_2w_p1_8bpp_line_ = 0;
  GLuint prg_rbg_1_2w_p2_8bpp_line_ = 0;
  GLuint prg_rbg_1_2w_p1_16bpp_p_line_ = 0;
  GLuint prg_rbg_1_2w_p2_16bpp_p_line_ = 0;
  GLuint prg_rbg_1_2w_p1_16bpp_line_ = 0;
  GLuint prg_rbg_1_2w_p2_16bpp_line_ = 0;
  GLuint prg_rbg_1_2w_p1_32bpp_line_ = 0;
  GLuint prg_rbg_1_2w_p2_32bpp_line_ = 0;

  GLuint prg_rbg_2_2w_bitmap_4bpp_line_ = 0;
  GLuint prg_rbg_2_2w_bitmap_8bpp_line_ = 0;
  GLuint prg_rbg_2_2w_bitmap_16bpp_p_line_ = 0;
  GLuint prg_rbg_2_2w_bitmap_16bpp_line_ = 0;
  GLuint prg_rbg_2_2w_bitmap_32bpp_line_ = 0;
  GLuint prg_rbg_2_2w_p1_4bpp_line_ = 0;
  GLuint prg_rbg_2_2w_p2_4bpp_line_ = 0;
  GLuint prg_rbg_2_2w_p1_8bpp_line_ = 0;
  GLuint prg_rbg_2_2w_p2_8bpp_line_ = 0;
  GLuint prg_rbg_2_2w_p1_16bpp_p_line_ = 0;
  GLuint prg_rbg_2_2w_p2_16bpp_p_line_ = 0;
  GLuint prg_rbg_2_2w_p1_16bpp_line_ = 0;
  GLuint prg_rbg_2_2w_p2_16bpp_line_ = 0;
  GLuint prg_rbg_2_2w_p1_32bpp_line_ = 0;
  GLuint prg_rbg_2_2w_p2_32bpp_line_ = 0;

  GLuint prg_rbg_3_2w_bitmap_4bpp_line_ = 0;
  GLuint prg_rbg_3_2w_bitmap_8bpp_line_ = 0;
  GLuint prg_rbg_3_2w_bitmap_16bpp_p_line_ = 0;
  GLuint prg_rbg_3_2w_bitmap_16bpp_line_ = 0;
  GLuint prg_rbg_3_2w_bitmap_32bpp_line_ = 0;
  GLuint prg_rbg_3_2w_p1_4bpp_line_ = 0;
  GLuint prg_rbg_3_2w_p2_4bpp_line_ = 0;
  GLuint prg_rbg_3_2w_p1_8bpp_line_ = 0;
  GLuint prg_rbg_3_2w_p2_8bpp_line_ = 0;
  GLuint prg_rbg_3_2w_p1_16bpp_p_line_ = 0;
  GLuint prg_rbg_3_2w_p2_16bpp_p_line_ = 0;
  GLuint prg_rbg_3_2w_p1_16bpp_line_ = 0;
  GLuint prg_rbg_3_2w_p2_16bpp_line_ = 0;
  GLuint prg_rbg_3_2w_p1_32bpp_line_ = 0;
  GLuint prg_rbg_3_2w_p2_32bpp_line_ = 0;

  GLuint tex_surface_ = 0;
  GLuint tex_surface_1 = 0;
  GLuint ssbo_vram_ = 0;
  GLuint ssbo_cram_ = 0;
  GLuint ssbo_window_ = 0;
  GLuint ssbo_paraA_ = 0;
  int tex_width_ = 0;
  int tex_height_ = 0;
  static RBGGenerator *instance_;
  GLuint scene_uniform = 0;
  RBGUniform uniform;
  int struct_size_;

  void *mapped_vram = nullptr;

  int local_size_x = 10;
  int local_size_y = 10;

protected:
  RBGGenerator()
  {
    tex_surface_ = 0;
    tex_width_ = 0;
    tex_height_ = 0;
    struct_size_ = sizeof(vdp2rotationparameter_struct);
    int am = sizeof(vdp2rotationparameter_struct) % 16;
    if (am != 0)
    {
      struct_size_ += 16 - am;
    }
  }

public:
  static RBGGenerator *getInstance()
  {
    if (instance_ == nullptr)
    {
      instance_ = new RBGGenerator();
    }
    return instance_;
  }

  // Destroy the singleton, releasing all GPU resources.
  static void destroyInstance()
  {
    if (instance_ == nullptr)
      return;
    instance_->deinit();
    delete instance_;
    instance_ = nullptr;
  }

  // Destroy all GPU resources held by this instance for clean shutdown.
  // This prevents leaked nouveau BOs / nvMap allocations on NX.
  void deinit()
  {
    int count = 0;

    // Delete textures
    if (tex_surface_ != 0)
    {
      glDeleteTextures(1, &tex_surface_);
      tex_surface_ = 0;
      count++;
    }
    if (tex_surface_1 != 0)
    {
      glDeleteTextures(1, &tex_surface_1);
      tex_surface_1 = 0;
      count++;
    }

    // Delete SSBOs and uniform buffer
    if (ssbo_vram_ != 0)
    {
      glDeleteBuffers(1, &ssbo_vram_);
      ssbo_vram_ = 0;
      count++;
    }
    if (ssbo_cram_ != 0)
    {
      glDeleteBuffers(1, &ssbo_cram_);
      ssbo_cram_ = 0;
      count++;
    }
    if (ssbo_window_ != 0)
    {
      glDeleteBuffers(1, &ssbo_window_);
      ssbo_window_ = 0;
      count++;
    }
    if (ssbo_paraA_ != 0)
    {
      glDeleteBuffers(1, &ssbo_paraA_);
      ssbo_paraA_ = 0;
      count++;
    }
    if (scene_uniform != 0)
    {
      glDeleteBuffers(1, &scene_uniform);
      scene_uniform = 0;
      count++;
    }

    // Delete all compute shader programs — iterate all prg_ fields.
    // The class layout starts with consecutive GLuint prg_ fields (0_2w through 3_2w line variants).
    // We iterate from the first prg field to (but not including) tex_surface_.
    GLuint *prg_start = &prg_rbg_0_2w_bitmap_4bpp_;
    GLuint *prg_end = &tex_surface_;
    for (GLuint *p = prg_start; p < prg_end; p++)
    {
      if (*p != 0)
      {
        glDeleteProgram(*p);
        *p = 0;
        count++;
      }
    }

    tex_width_ = 0;
    tex_height_ = 0;
    mapped_vram = nullptr;

    printf("RBGGenerator::deinit: deleted %d GL resources\n", count);
  }

  void resize(int width, int height)
  {
    if (tex_width_ == width && tex_height_ == height)
      return;

    YGLDEBUG("resize %d, %d\n", width, height);

    glGetError();

    if (tex_surface_ != 0)
    {
      glDeleteTextures(1, &tex_surface_);
      tex_surface_ = 0;
    }

    glGenTextures(1, &tex_surface_);
    ErrorHandle("glGenTextures");

    tex_width_ = width;
    tex_height_ = height;

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_surface_);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    ErrorHandle("glBindTexture");
    // glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tex_width_, tex_height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, tex_width_, tex_height_);
    ErrorHandle("glTexStorage2D");
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    ErrorHandle("glTexParameteri");

    if (tex_surface_1 != 0)
    {
      glDeleteTextures(1, &tex_surface_1);
      glGenTextures(1, &tex_surface_1);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, tex_surface_1);
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
      ErrorHandle("glBindTexture");
      // glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tex_width_, tex_height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, tex_width_, tex_height_);
      ErrorHandle("glTexStorage2D");
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      ErrorHandle("glTexParameteri");
    }

    YGLDEBUG("resize tex_surface_=%d, tex_surface_1=%d\n", tex_surface_, tex_surface_1);
  }

  GLuint createProgram(int count, const GLchar **prg_strs)
  {
    // FILE *fp = fopen("sdmc:/tiicu/debug/debug_core.txt", "a");
    // if (fp) {
    //     fprintf(fp, "[CS] createProgram Call. glCreateShader=%p\n", glCreateShader);
    //     fclose(fp);
    // }
    GLuint result = glCreateShader(GL_COMPUTE_SHADER);

    // fp = fopen("sdmc:/tiicu/debug/debug_core.txt", "a");
    // if (fp) {
    //     fprintf(fp, "[CS] glCreateShader returned %d\n", result);
    //     fclose(fp);
    // }

    glShaderSource(result, count, prg_strs, NULL);

    // fp = fopen("sdmc:/tiicu/debug/debug_core.txt", "a");
    // if (fp) {
    //     fprintf(fp, "[CS] glShaderSource called\n");
    //     fclose(fp);
    // }

    glCompileShader(result);

    // fp = fopen("sdmc:/tiicu/debug/debug_core.txt", "a");
    // if (fp) {
    //     fprintf(fp, "[CS] glCompileShader called\n");
    //     fclose(fp);
    // }

    // fp = fopen("sdmc:/tiicu/debug/debug_core.txt", "a");
    // if (fp) {
    //     fprintf(fp, "[CS] glCompileShader called. glGetShaderiv=%p\n", glGetShaderiv);
    //     fclose(fp);
    // }

    GLint status = GL_TRUE;
    glGetShaderiv(result, GL_COMPILE_STATUS, &status);

    // fp = fopen("sdmc:/tiicu/debug/debug_core.txt", "a");
    // if (fp) {
    //     fprintf(fp, "[CS] glGetShaderiv status=%d\n", status);
    //     fclose(fp);
    // }

    if (status == GL_FALSE)
    {
      GLint length = 0;
      glGetShaderiv(result, GL_INFO_LOG_LENGTH, &length);

      char info[4096];
      info[0] = 0;
      if (length > 0 && length < 4096)
      {
        glGetShaderInfoLog(result, length, NULL, info);
      }
      else if (length >= 4096)
      {
        glGetShaderInfoLog(result, 4095, NULL, info);
      }

      // fp = fopen("sdmc:/tiicu/debug/debug_core.txt", "a");
      // if (fp) {
      //     fprintf(fp, "[CS] Shader Compile Error:\n%s\n", info);
      //     fclose(fp);
      // }
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, result);
    glLinkProgram(program);
    glDetachShader(program, result);
    glDeleteShader(result); // Release shader object GPU memory (critical for NX nouveau)
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status == GL_FALSE)
    {
      GLint length;
      glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
      GLchar *info = new GLchar[length];
      glGetProgramInfoLog(program, length, NULL, info);
      YGLDEBUG("[LINK] %s\n", info);
      YuiErrorMsg(info);
      FILE *fp = fopen_utf8("tmp.cpp", "w");
      if (fp)
      {
        for (int i = 0; i < count; i++)
        {
          fprintf(fp, "%s", prg_strs[i]);
        }
        fclose(fp);
      }

      YabThreadUSleep(1000000);
      abort();
      delete[] info;
    }
    return program;
  }

  //-----------------------------------------------
  void init(int width, int height)
  {

    resize(width, height);
    if (ssbo_vram_ != 0)
      return; // always inisialized!

    // SSBO limits
    GLint maxSSBOBindings = 0;                    // binding points summed over all stages
    GLint maxCompSSBO     = 0;                    // SSBOs usable by the compute stage
    glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &maxSSBOBindings);
    glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS,   &maxCompSSBO);

    yprintf("maxSSBOBindings=%d, maxCompSSBO=%d",maxSSBOBindings,maxCompSSBO);


    glGenBuffers(1, &ssbo_vram_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_vram_);
    // Pass Vdp2Ram if available, NULL otherwise (buffer will be filled in update())
    glBufferData(GL_SHADER_STORAGE_BUFFER, 0x80000, Vdp2Ram, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ssbo_paraA_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_paraA_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, struct_size_ * 2, NULL, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &scene_uniform);
    glBindBuffer(GL_UNIFORM_BUFFER, scene_uniform);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(RBGUniform), &uniform, GL_STATIC_DRAW);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    glGenBuffers(1, &ssbo_window_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_window_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(vdp2WindowInfo) * 512, NULL, GL_DYNAMIC_DRAW);

    GLint workGroupCount[3], workGroupSize[3];
    GLint maxInvocations;
    glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &maxInvocations);
    glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0, &workGroupCount[0]);
    glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 1, &workGroupCount[1]);
    glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 2, &workGroupCount[2]);
    glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 0, &workGroupSize[0]);
    glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 1, &workGroupSize[1]);
    glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_SIZE, 2, &workGroupSize[2]);

    local_size_x = sqrtf(maxInvocations);
    local_size_y = sqrtf(maxInvocations);

    int length = sizeof(prg_generate_rbg_base) + 64;
    snprintf(prg_generate_rbg, length, prg_generate_rbg_base, local_size_x, local_size_y);

    // prg_rbg_0_2w_bitmap_8bpp_ = createProgram(sizeof(a_prg_rbg_0_2w_bitmap) / sizeof(char*), (const GLchar**)a_prg_rbg_0_2w_bitmap);
    // prg_rbg_0_2w_p1_4bpp_ = createProgram(sizeof(a_prg_rbg_0_2w_p1_4bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_0_2w_p1_4bpp);
    // prg_rbg_0_2w_p2_4bpp_ = createProgram(sizeof(a_prg_rbg_0_2w_p2_4bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_0_2w_p2_4bpp);
    // prg_rbg_0_2w_p1_8bpp_ = createProgram(sizeof(a_prg_rbg_0_2w_p1_8bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_0_2w_p1_8bpp);
    // prg_rbg_0_2w_p2_8bpp_ = createProgram(sizeof(a_prg_rbg_0_2w_p2_8bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_0_2w_p2_8bpp);

    // prg_rbg_1_2w_bitmap_8bpp_ = createProgram(sizeof(a_prg_rbg_1_2w_bitmap) / sizeof(char*), (const GLchar**)a_prg_rbg_1_2w_bitmap);
    // prg_rbg_1_2w_p1_4bpp_ = createProgram(sizeof(a_prg_rbg_1_2w_p1_4bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_1_2w_p1_4bpp);
    // prg_rbg_1_2w_p2_4bpp_ = createProgram(sizeof(a_prg_rbg_1_2w_p2_4bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_1_2w_p2_4bpp);
    // prg_rbg_1_2w_p1_8bpp_ = createProgram(sizeof(a_prg_rbg_1_2w_p1_8bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_1_2w_p1_8bpp);
    // prg_rbg_1_2w_p2_8bpp_ = createProgram(sizeof(a_prg_rbg_1_2w_p2_8bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_1_2w_p2_8bpp);

    // prg_rbg_2_2w_bitmap_8bpp_ = createProgram(sizeof(a_prg_rbg_2_2w_bitmap) / sizeof(char*), (const GLchar**)a_prg_rbg_2_2w_bitmap);
    // prg_rbg_2_2w_p1_4bpp_ = createProgram(sizeof(a_prg_rbg_2_2w_p1_4bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_2_2w_p1_4bpp);
    // prg_rbg_2_2w_p2_4bpp_ = createProgram(sizeof(a_prg_rbg_2_2w_p2_4bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_2_2w_p2_4bpp);
    // prg_rbg_2_2w_p1_8bpp_ = createProgram(sizeof(a_prg_rbg_2_2w_p1_8bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_2_2w_p1_8bpp);
    // prg_rbg_2_2w_p2_8bpp_ = createProgram( sizeof(a_prg_rbg_2_2w_p2_8bpp)/sizeof(char*) , (const GLchar**)a_prg_rbg_2_2w_p2_8bpp);

    // prg_rbg_3_2w_bitmap_8bpp_ = createProgram(sizeof(a_prg_rbg_3_2w_bitmap) / sizeof(char*), (const GLchar**)a_prg_rbg_3_2w_bitmap);
    // prg_rbg_3_2w_p1_4bpp_ = createProgram(sizeof(a_prg_rbg_3_2w_p1_4bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_3_2w_p1_4bpp);
    // prg_rbg_3_2w_p2_4bpp_ = createProgram(sizeof(a_prg_rbg_3_2w_p2_4bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_3_2w_p2_4bpp);
    // prg_rbg_3_2w_p1_8bpp_ = createProgram(sizeof(a_prg_rbg_3_2w_p1_8bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_3_2w_p1_8bpp);
    // prg_rbg_3_2w_p2_8bpp_ = createProgram(sizeof(a_prg_rbg_3_2w_p2_8bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_3_2w_p2_8bpp);

    // a_prg_rbg_0_2w_bitmap[sizeof(a_prg_rbg_0_2w_bitmap) / sizeof(char*) -1] = prg_generate_rbg_line_end;
    // prg_rbg_0_2w_bitmap_8bpp_line_ = createProgram(sizeof(a_prg_rbg_0_2w_bitmap) / sizeof(char*), (const GLchar**)a_prg_rbg_0_2w_bitmap);

    // a_prg_rbg_0_2w_p1_4bpp[sizeof(a_prg_rbg_0_2w_p1_4bpp) / sizeof(char*) - 1] = prg_generate_rbg_line_end;
    // prg_rbg_0_2w_p1_4bpp_line_ = createProgram(sizeof(a_prg_rbg_0_2w_p1_4bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_0_2w_p1_4bpp);

    // a_prg_rbg_0_2w_p2_4bpp[sizeof(a_prg_rbg_0_2w_p2_4bpp) / sizeof(char*) -1] = prg_generate_rbg_line_end;
    // prg_rbg_0_2w_p2_4bpp_line_ = createProgram(sizeof(a_prg_rbg_0_2w_p2_4bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_0_2w_p2_4bpp);

    // a_prg_rbg_0_2w_p1_8bpp[sizeof(a_prg_rbg_0_2w_p1_8bpp) / sizeof(char*) -1] = prg_generate_rbg_line_end;
    // prg_rbg_0_2w_p1_8bpp_line_ = createProgram(sizeof(a_prg_rbg_0_2w_p1_8bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_0_2w_p1_8bpp);

    // a_prg_rbg_0_2w_p2_8bpp[sizeof(a_prg_rbg_0_2w_p2_8bpp) / sizeof(char*) -1] = prg_generate_rbg_line_end;
    // prg_rbg_0_2w_p2_8bpp_line_ = createProgram(sizeof(a_prg_rbg_0_2w_p2_8bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_0_2w_p2_8bpp);

    // a_prg_rbg_1_2w_bitmap[sizeof(a_prg_rbg_1_2w_bitmap) / sizeof(char*) - 1] = prg_generate_rbg_line_end;
    // prg_rbg_1_2w_bitmap_8bpp_line_ = createProgram(sizeof(a_prg_rbg_1_2w_bitmap) / sizeof(char*), (const GLchar**)a_prg_rbg_1_2w_bitmap);

    // a_prg_rbg_1_2w_p1_4bpp[sizeof(a_prg_rbg_1_2w_p1_4bpp) / sizeof(char*) - 1] = prg_generate_rbg_line_end;
    // prg_rbg_1_2w_p1_4bpp_line_ = createProgram(sizeof(a_prg_rbg_1_2w_p1_4bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_1_2w_p1_4bpp);

    // a_prg_rbg_1_2w_p2_4bpp[sizeof(a_prg_rbg_1_2w_p2_4bpp) / sizeof(char*) - 1] = prg_generate_rbg_line_end;
    // prg_rbg_1_2w_p2_4bpp_line_ = createProgram(sizeof(a_prg_rbg_1_2w_p2_4bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_1_2w_p2_4bpp);

    // a_prg_rbg_1_2w_p1_8bpp[sizeof(a_prg_rbg_1_2w_p1_8bpp) / sizeof(char*) - 1] = prg_generate_rbg_line_end;
    // prg_rbg_1_2w_p1_8bpp_line_ = createProgram(sizeof(a_prg_rbg_1_2w_p1_8bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_1_2w_p1_8bpp);

    // a_prg_rbg_1_2w_p2_8bpp[sizeof(a_prg_rbg_1_2w_p2_8bpp) / sizeof(char*) - 1] = prg_generate_rbg_line_end;
    // prg_rbg_1_2w_p2_8bpp_line_ = createProgram(sizeof(a_prg_rbg_1_2w_p2_8bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_1_2w_p2_8bpp);

    // a_prg_rbg_2_2w_bitmap[sizeof(a_prg_rbg_2_2w_bitmap) / sizeof(char*) - 1] = prg_generate_rbg_line_end;
    // prg_rbg_2_2w_bitmap_8bpp_line_ = createProgram(sizeof(a_prg_rbg_2_2w_bitmap) / sizeof(char*), (const GLchar**)a_prg_rbg_2_2w_bitmap);

    // a_prg_rbg_2_2w_p1_4bpp[sizeof(a_prg_rbg_2_2w_p1_4bpp) / sizeof(char*) - 1] = prg_generate_rbg_line_end;
    // prg_rbg_2_2w_p1_4bpp_line_ = createProgram(sizeof(a_prg_rbg_2_2w_p1_4bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_2_2w_p1_4bpp);

    // a_prg_rbg_2_2w_p2_4bpp[sizeof(a_prg_rbg_2_2w_p2_4bpp) / sizeof(char*) - 1] = prg_generate_rbg_line_end;
    // prg_rbg_2_2w_p2_4bpp_line_ = createProgram(sizeof(a_prg_rbg_2_2w_p2_4bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_2_2w_p2_4bpp);

    // a_prg_rbg_2_2w_p1_8bpp[sizeof(a_prg_rbg_2_2w_p1_8bpp) / sizeof(char*) - 1] = prg_generate_rbg_line_end;
    // prg_rbg_2_2w_p1_8bpp_line_ = createProgram(sizeof(a_prg_rbg_2_2w_p1_8bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_2_2w_p1_8bpp);

    // a_prg_rbg_2_2w_p2_8bpp[sizeof(a_prg_rbg_2_2w_p2_8bpp) / sizeof(char*) - 1] = prg_generate_rbg_line_end;
    // prg_rbg_2_2w_p2_8bpp_line_ = createProgram(sizeof(a_prg_rbg_2_2w_p2_8bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_2_2w_p2_8bpp);

    // a_prg_rbg_3_2w_bitmap[sizeof(a_prg_rbg_3_2w_bitmap) / sizeof(char*) - 1] = prg_generate_rbg_line_end;
    // prg_rbg_3_2w_bitmap_8bpp_line_ = createProgram(sizeof(a_prg_rbg_3_2w_bitmap) / sizeof(char*), (const GLchar**)a_prg_rbg_3_2w_bitmap);

    // a_prg_rbg_3_2w_p1_4bpp[sizeof(a_prg_rbg_3_2w_p1_4bpp) / sizeof(char*) - 1] = prg_generate_rbg_line_end;
    // prg_rbg_3_2w_p1_4bpp_line_ = createProgram(sizeof(a_prg_rbg_3_2w_p1_4bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_3_2w_p1_4bpp);

    // a_prg_rbg_3_2w_p2_4bpp[sizeof(a_prg_rbg_3_2w_p2_4bpp) / sizeof(char*) - 1] = prg_generate_rbg_line_end;
    // prg_rbg_3_2w_p2_4bpp_line_ = createProgram(sizeof(a_prg_rbg_3_2w_p2_4bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_3_2w_p2_4bpp);

    // a_prg_rbg_3_2w_p1_8bpp[sizeof(a_prg_rbg_3_2w_p1_8bpp) / sizeof(char*) - 1] = prg_generate_rbg_line_end;
    // prg_rbg_3_2w_p1_8bpp_line_ = createProgram(sizeof(a_prg_rbg_3_2w_p1_8bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_3_2w_p1_8bpp);

    // a_prg_rbg_3_2w_p2_8bpp[sizeof(a_prg_rbg_3_2w_p2_8bpp) / sizeof(char*) - 1] = prg_generate_rbg_line_end;
    // prg_rbg_3_2w_p2_8bpp_line_ = createProgram(sizeof(a_prg_rbg_3_2w_p2_8bpp) / sizeof(char*), (const GLchar**)a_prg_rbg_3_2w_p2_8bpp);
  }

  bool ErrorHandle(const char *name)
  {
    GLenum error_code = glGetError();
    if (error_code == GL_NO_ERROR)
    {
      return true;
    }
    do
    {
      const char *msg = "";
      switch (error_code)
      {
      case GL_INVALID_ENUM:
        msg = "INVALID_ENUM";
        break;
      case GL_INVALID_VALUE:
        msg = "INVALID_VALUE";
        break;
      case GL_INVALID_OPERATION:
        msg = "INVALID_OPERATION";
        break;
      case GL_OUT_OF_MEMORY:
        msg = "OUT_OF_MEMORY";
        break;
      case GL_INVALID_FRAMEBUFFER_OPERATION:
        msg = "INVALID_FRAMEBUFFER_OPERATION";
        break;
      default:
        msg = "Unknown";
        break;
      }
      char errorbuf[256];
      snprintf(errorbuf, 256, "GLErrorLayer:ERROR:%04x'%s' %s\n", error_code, msg, name);
      YuiErrorMsg(errorbuf);
      abort();
      error_code = glGetError();
    } while (error_code != GL_NO_ERROR);
    return false;
  }

  template <typename T>
  T Add(T a, T b)
  {
    return a + b;
  }

  // #define COMPILE_COLOR_DOT( BASE, COLOR , DOT ) ({ GLuint PRG; BASE[sizeof(BASE)/sizeof(char*) - 2] = COLOR; BASE[sizeof(BASE)/sizeof(char*) - 1] = DOT; PRG=createProgram(sizeof(BASE)/sizeof(char*), (const GLchar**)BASE);})

#define COMPILE_COLOR_DOT(BASE, COLOR, DOT)
#define S(A) A, sizeof(A) / sizeof(char *)

  GLuint compile_color_dot(const char *base[], int size, const char *color, const char *dot)
  {
    // FILE *fp = fopen("sdmc:/tiicu/debug/debug_core.txt", "a");
    // if (fp) {
    //     fprintf(fp, "[CS] compile_color_dot Call (Safe)\\n");
    //     fclose(fp);
    // }

    const char **safe_base = new const char *[size];
    for (int i = 0; i < size; i++)
      safe_base[i] = base[i];

    safe_base[size - 2] = color;
    safe_base[size - 1] = dot;
    GLuint ret = createProgram(size, (const GLchar **)safe_base);
    delete[] safe_base;
    return ret;
  }

  //-----------------------------------------------
  void update(RBGDrawInfo *rbg)
  {
    CS_DEBUG_LOG("[CS] update() called");
    
    // === SAFETY CHECKS: Prevent crashes during mode transitions (movie playback, etc.) ===
    // These pointers can be NULL when VDP2 is being reinitialized
    if (rbg == nullptr) {
      CS_DEBUG_LOG("[CS] SKIP: rbg is NULL");
      return; // Invalid draw info
    }
    if (fixVdp2Regs == nullptr) {
      CS_DEBUG_LOG("[CS] SKIP: fixVdp2Regs is NULL");
      return; // VDP2 registers not available
    }
    if (Vdp2Ram == nullptr) {
      CS_DEBUG_LOG("[CS] SKIP: Vdp2Ram is NULL");
      return; // VDP2 RAM not available
    }
    if (tex_width_ == 0 || tex_height_ == 0) {
      return; // Texture not initialized
    }
    // === END SAFETY CHECKS ===

    GLuint error;

    int work_groups_x = 1 + (tex_width_ - 1) / local_size_x;
    int work_groups_y = 1 + (tex_height_ - 1) / local_size_y;

    error = glGetError();
    // Line color insersion
    // The "_line_" variants pack a CRAM index into R/G and the line colour into B,
    // so they only work for palette colour modes.  RGB direct modes (colornumber
    // 3 = 32K, 4 = 16.7M) put a 24-bit colour in cramindex; running them through
    // the line path drops the blue byte and forces bit 15 on, which turned every
    // direct colour dot green.  Fall back to the plain pipeline for those.
    if (rbg->info.LineColorBase != 0 && VDP2_CC_NONE != (rbg->info.blendmode & 0x03)
        && rbg->info.colornumber < 3) {
      if (fixVdp2Regs->RPMD == 0 || (fixVdp2Regs->RPMD == 3 && (fixVdp2Regs->WCTLD & 0xA) == 0)) {
        if (rbg->info.isbitmap) {
          if (prg_rbg_0_2w_bitmap_8bpp_line_ == 0) {
            prg_rbg_0_2w_bitmap_8bpp_line_ = compile_color_dot(
                S(a_prg_rbg_0_2w_bitmap),
                prg_rbg_getcolor_8bpp,
                prg_generate_rbg_line_end);
          }
          glUseProgram(prg_rbg_0_2w_bitmap_8bpp_line_);
        }
        else
        {
          if (rbg->info.patterndatasize == 1)
          {
            switch (rbg->info.colornumber)
            {
            case 0:
            { // Decathalete ToDo: Line Color Bug
              if (prg_rbg_0_2w_p1_4bpp_line_ == 0)
              {
                prg_rbg_0_2w_p1_4bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_0_2w_p1_4bpp),
                    prg_rbg_getcolor_4bpp,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_0_2w_p1_4bpp_line_);
              break;
            }
            case 1:
            { // Sakatuku 2 Ground, GUNDAM Side Story 2, SonicR ToDo: 2Player
              if (prg_rbg_0_2w_p1_8bpp_line_ == 0)
              {
                prg_rbg_0_2w_p1_8bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_0_2w_p1_8bpp),
                    prg_rbg_getcolor_8bpp,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_0_2w_p1_8bpp_line_);
              break;
            }
            case 2:
            {
              if (prg_rbg_0_2w_p1_16bpp_p_line_ == 0)
              {
                prg_rbg_0_2w_p1_16bpp_p_line_ = compile_color_dot(
                    S(a_prg_rbg_0_2w_p1_4bpp),
                    prg_rbg_getcolor_16bpp_palette,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_0_2w_p1_16bpp_p_line_);
              break;
            }
            case 3:
            {
              if (prg_rbg_0_2w_p1_16bpp_line_ == 0)
              {
                prg_rbg_0_2w_p1_16bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_0_2w_p1_4bpp),
                    prg_rbg_getcolor_16bpp_rbg,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_0_2w_p1_16bpp_line_);
              break;
            }
            case 4:
            {
              if (prg_rbg_0_2w_p1_32bpp_line_ == 0)
              {
                prg_rbg_0_2w_p1_32bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_0_2w_p1_4bpp),
                    prg_rbg_getcolor_32bpp_rbg,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_0_2w_p1_32bpp_line_);
              break;
            }
            }
          }
          else
          {
            switch (rbg->info.colornumber)
            {
            case 0:
            {
              if (prg_rbg_0_2w_p2_4bpp_line_ == 0)
              {
                prg_rbg_0_2w_p2_4bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_0_2w_p2_4bpp),
                    prg_rbg_getcolor_4bpp,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_0_2w_p2_4bpp_line_);
              break;
            }
            case 1:
            { // Thunder Force V
              if (prg_rbg_0_2w_p2_8bpp_line_ == 0)
              {
                prg_rbg_0_2w_p2_8bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_0_2w_p2_8bpp),
                    prg_rbg_getcolor_8bpp,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_0_2w_p2_8bpp_line_);
              break;
            }
            case 2:
            {
              if (prg_rbg_0_2w_p2_16bpp_p_line_ == 0)
              {
                prg_rbg_0_2w_p2_16bpp_p_line_ = compile_color_dot(
                    S(a_prg_rbg_0_2w_p2_4bpp),
                    prg_rbg_getcolor_16bpp_palette,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_0_2w_p2_16bpp_p_line_);
              break;
            }
            case 3:
            {
              if (prg_rbg_0_2w_p2_16bpp_line_ == 0)
              {
                prg_rbg_0_2w_p2_16bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_0_2w_p2_4bpp),
                    prg_rbg_getcolor_16bpp_rbg,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_0_2w_p2_16bpp_line_);
              break;
            }
            case 4:
            {
              if (prg_rbg_0_2w_p2_32bpp_line_ == 0)
              {
                prg_rbg_0_2w_p2_32bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_0_2w_p2_4bpp),
                    prg_rbg_getcolor_32bpp_rbg,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_0_2w_p2_32bpp_line_);
              break;
            }
            }
          }
        }
      }
      else if (fixVdp2Regs->RPMD == 1)
      {
        if (rbg->info.isbitmap)
        {
          if (prg_rbg_1_2w_bitmap_8bpp_line_ == 0)
          {
            prg_rbg_1_2w_bitmap_8bpp_line_ = compile_color_dot(
                S(a_prg_rbg_1_2w_bitmap),
                prg_rbg_getcolor_8bpp,
                prg_generate_rbg_line_end);
          }
          glUseProgram(prg_rbg_1_2w_bitmap_8bpp_line_);
        }
        else
        {
          if (rbg->info.patterndatasize == 1)
          {
            switch (rbg->info.colornumber)
            {
            case 0:
            {
              if (prg_rbg_1_2w_p1_4bpp_line_ == 0)
              {
                prg_rbg_1_2w_p1_4bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_1_2w_p1_4bpp),
                    prg_rbg_getcolor_4bpp,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_1_2w_p1_4bpp_line_);
              break;
            }
            case 1:
            {
              if (prg_rbg_1_2w_p1_8bpp_line_ == 0)
              {
                prg_rbg_1_2w_p1_8bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_1_2w_p1_8bpp),
                    prg_rbg_getcolor_8bpp,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_1_2w_p1_8bpp_line_);
              break;
            }
            case 2:
            {
              if (prg_rbg_1_2w_p1_16bpp_p_line_ == 0)
              {
                prg_rbg_1_2w_p1_16bpp_p_line_ = compile_color_dot(
                    S(a_prg_rbg_1_2w_p1_4bpp),
                    prg_rbg_getcolor_16bpp_palette,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_1_2w_p1_16bpp_p_line_);
              break;
            }
            case 3:
            {
              if (prg_rbg_1_2w_p1_16bpp_line_ == 0)
              {
                prg_rbg_1_2w_p1_16bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_1_2w_p1_4bpp),
                    prg_rbg_getcolor_16bpp_rbg,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_1_2w_p1_16bpp_line_);
              break;
            }
            case 4:
            {
              if (prg_rbg_1_2w_p1_32bpp_line_ == 0)
              {
                prg_rbg_1_2w_p1_32bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_0_2w_p1_4bpp),
                    prg_rbg_getcolor_32bpp_rbg,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_1_2w_p1_32bpp_line_);
              break;
            }
            }
          }
          else
          {
            switch (rbg->info.colornumber)
            {
            case 0:
            {
              if (prg_rbg_1_2w_p2_4bpp_line_ == 0)
              {
                prg_rbg_1_2w_p2_4bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_1_2w_p2_4bpp),
                    prg_rbg_getcolor_4bpp,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_1_2w_p2_4bpp_line_);
              break;
            }
            case 1:
            {
              if (prg_rbg_1_2w_p2_8bpp_line_ == 0)
              {
                prg_rbg_1_2w_p2_8bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_1_2w_p2_8bpp),
                    prg_rbg_getcolor_8bpp,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_1_2w_p2_8bpp_line_);
              break;
            }
            case 2:
            {
              if (prg_rbg_1_2w_p2_16bpp_p_line_ == 0)
              {
                prg_rbg_1_2w_p2_16bpp_p_line_ = compile_color_dot(
                    S(a_prg_rbg_1_2w_p2_4bpp),
                    prg_rbg_getcolor_16bpp_palette,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_1_2w_p2_16bpp_p_line_);
              break;
            }
            case 3:
            {
              if (prg_rbg_1_2w_p2_16bpp_line_ == 0)
              {
                prg_rbg_1_2w_p2_16bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_1_2w_p2_4bpp),
                    prg_rbg_getcolor_16bpp_rbg,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_1_2w_p2_16bpp_line_);
              break;
            }
            case 4:
            {
              if (prg_rbg_1_2w_p2_32bpp_line_ == 0)
              {
                prg_rbg_1_2w_p2_32bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_1_2w_p2_4bpp),
                    prg_rbg_getcolor_32bpp_rbg,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_1_2w_p2_32bpp_line_);
              break;
            }
            }
          }
        }
      }
      else if (fixVdp2Regs->RPMD == 2)
      {
        if (rbg->info.isbitmap)
        {
          if (prg_rbg_2_2w_bitmap_8bpp_line_ == 0)
          {
            prg_rbg_2_2w_bitmap_8bpp_line_ = compile_color_dot(
                S(a_prg_rbg_2_2w_bitmap),
                prg_rbg_getcolor_8bpp,
                prg_generate_rbg_line_end);
          }
          glUseProgram(prg_rbg_2_2w_bitmap_8bpp_line_);
        }
        else
        {
          if (rbg->info.patterndatasize == 1)
          {
            switch (rbg->info.colornumber)
            {
            case 0:
            {
              if (prg_rbg_2_2w_p1_4bpp_line_ == 0)
              {
                prg_rbg_2_2w_p1_4bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_2_2w_p1_4bpp),
                    prg_rbg_getcolor_4bpp,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_2_2w_p1_4bpp_line_);
              break;
            }
            case 1:
            { // Panzer Dragoon 1
              if (prg_rbg_2_2w_p1_8bpp_line_ == 0)
              {
                prg_rbg_2_2w_p1_8bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_2_2w_p1_8bpp),
                    prg_rbg_getcolor_8bpp,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_2_2w_p1_8bpp_line_);
              break;
            }
            case 2:
            {
              if (prg_rbg_2_2w_p1_16bpp_p_line_ == 0)
              {
                prg_rbg_2_2w_p1_16bpp_p_line_ = compile_color_dot(
                    S(a_prg_rbg_2_2w_p1_4bpp),
                    prg_rbg_getcolor_16bpp_palette,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_2_2w_p1_16bpp_p_line_);
              break;
            }
            case 3:
            {
              if (prg_rbg_2_2w_p1_16bpp_line_ == 0)
              {
                prg_rbg_2_2w_p1_16bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_2_2w_p1_4bpp),
                    prg_rbg_getcolor_16bpp_rbg,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_2_2w_p1_16bpp_line_);
              break;
            }
            case 4:
            {
              if (prg_rbg_2_2w_p1_32bpp_line_ == 0)
              {
                prg_rbg_2_2w_p1_32bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_2_2w_p1_4bpp),
                    prg_rbg_getcolor_32bpp_rbg,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_2_2w_p1_32bpp_line_);
              break;
            }
            }
          }
          else
          {
            switch (rbg->info.colornumber)
            {
            case 0:
            {
              if (prg_rbg_2_2w_p2_4bpp_line_ == 0)
              {
                prg_rbg_2_2w_p2_4bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_2_2w_p2_4bpp),
                    prg_rbg_getcolor_4bpp,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_2_2w_p2_4bpp_line_);
              break;
            }
            case 1:
            {
              if (prg_rbg_2_2w_p2_8bpp_line_ == 0)
              {
                prg_rbg_2_2w_p2_8bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_2_2w_p2_8bpp),
                    prg_rbg_getcolor_8bpp,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_2_2w_p2_8bpp_line_);
              break;
            }
            case 2:
            {
              if (prg_rbg_2_2w_p2_16bpp_p_line_ == 0)
              {
                prg_rbg_2_2w_p2_16bpp_p_line_ = compile_color_dot(
                    S(a_prg_rbg_2_2w_p2_4bpp),
                    prg_rbg_getcolor_16bpp_palette,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_2_2w_p2_16bpp_p_line_);
              break;
            }
            case 3:
            {
              if (prg_rbg_2_2w_p2_16bpp_line_ == 0)
              {
                prg_rbg_2_2w_p2_16bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_2_2w_p2_4bpp),
                    prg_rbg_getcolor_16bpp_rbg,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_2_2w_p2_16bpp_line_);
              break;
            }
            case 4:
            {
              if (prg_rbg_2_2w_p2_32bpp_line_ == 0)
              {
                prg_rbg_2_2w_p2_32bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_2_2w_p2_4bpp),
                    prg_rbg_getcolor_32bpp_rbg,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_2_2w_p2_32bpp_line_);
              break;
            }
            }
          }
        }
      }
      else if (fixVdp2Regs->RPMD == 3)
      {

        if (rbg->info.isbitmap)
        {
          if (prg_rbg_3_2w_bitmap_8bpp_line_ == 0)
          {
            prg_rbg_3_2w_bitmap_8bpp_line_ = compile_color_dot(
                S(a_prg_rbg_3_2w_bitmap),
                prg_rbg_getcolor_8bpp,
                prg_generate_rbg_line_end);
          }
          glUseProgram(prg_rbg_3_2w_bitmap_8bpp_line_);
        }
        else
        {
          if (rbg->info.patterndatasize == 1)
          {
            switch (rbg->info.colornumber)
            {
            case 0:
            {
              if (prg_rbg_3_2w_p1_4bpp_line_ == 0)
              {
                prg_rbg_3_2w_p1_4bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_3_2w_p1_4bpp),
                    prg_rbg_getcolor_4bpp,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_3_2w_p1_4bpp_line_);
              break;
            }
            case 1:
            {
              if (prg_rbg_3_2w_p1_8bpp_line_ == 0)
              {
                prg_rbg_3_2w_p1_8bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_3_2w_p1_8bpp),
                    prg_rbg_getcolor_8bpp,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_3_2w_p1_8bpp_line_);
              break;
            }
            case 2:
            {
              if (prg_rbg_3_2w_p1_16bpp_p_line_ == 0)
              {
                prg_rbg_3_2w_p1_16bpp_p_line_ = compile_color_dot(
                    S(a_prg_rbg_3_2w_p1_4bpp),
                    prg_rbg_getcolor_16bpp_palette,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_3_2w_p1_16bpp_p_line_);
              break;
            }
            case 3:
            {
              if (prg_rbg_3_2w_p1_16bpp_line_ == 0)
              {
                prg_rbg_3_2w_p1_16bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_3_2w_p1_4bpp),
                    prg_rbg_getcolor_16bpp_rbg,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_3_2w_p1_16bpp_line_);
              break;
            }
            case 4:
            {
              if (prg_rbg_3_2w_p1_32bpp_line_ == 0)
              {
                prg_rbg_3_2w_p1_32bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_3_2w_p1_4bpp),
                    prg_rbg_getcolor_32bpp_rbg,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_3_2w_p1_32bpp_line_);
              break;
            }
            }
          }
          else
          {
            switch (rbg->info.colornumber)
            {
            case 0:
            {
              if (prg_rbg_3_2w_p2_4bpp_line_ == 0)
              {
                prg_rbg_3_2w_p2_4bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_3_2w_p2_4bpp),
                    prg_rbg_getcolor_4bpp,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_3_2w_p2_4bpp_line_);
              break;
            }
            case 1:
            {
              if (prg_rbg_3_2w_p2_8bpp_line_ == 0)
              {
                prg_rbg_3_2w_p2_8bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_3_2w_p2_8bpp),
                    prg_rbg_getcolor_8bpp,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_3_2w_p2_8bpp_line_);
              break;
            }
            case 2:
            {
              if (prg_rbg_3_2w_p2_16bpp_p_line_ == 0)
              {
                prg_rbg_3_2w_p2_16bpp_p_line_ = compile_color_dot(
                    S(a_prg_rbg_3_2w_p2_4bpp),
                    prg_rbg_getcolor_16bpp_palette,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_3_2w_p2_16bpp_p_line_);
              break;
            }
            case 3:
            {
              if (prg_rbg_3_2w_p2_16bpp_line_ == 0)
              {
                prg_rbg_3_2w_p2_16bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_3_2w_p2_4bpp),
                    prg_rbg_getcolor_16bpp_rbg,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_3_2w_p2_16bpp_line_);
              break;
            }
            case 4:
            {
              if (prg_rbg_3_2w_p2_32bpp_line_ == 0)
              {
                prg_rbg_3_2w_p2_32bpp_line_ = compile_color_dot(
                    S(a_prg_rbg_3_2w_p2_4bpp),
                    prg_rbg_getcolor_32bpp_rbg,
                    prg_generate_rbg_line_end);
              }
              glUseProgram(prg_rbg_3_2w_p2_32bpp_line_);
              break;
            }
            }
          }
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_window_);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(vdp2WindowInfo) * (int(rbg->vres / rbg->rotate_mval_v) << rbg->info.hres_shift), (void *)rbg->info.pWinInfo);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, ssbo_window_);
      }
    }

    // no line color insersion
    else
    {
      if (rbg->rgb_type == 0 && (fixVdp2Regs->RPMD == 0 || (fixVdp2Regs->RPMD == 3 && (fixVdp2Regs->WCTLD & 0xA) == 0)))
      {
        if (rbg->info.isbitmap)
        {
          switch (rbg->info.colornumber)
          {
          case 0:
          {
            if (prg_rbg_0_2w_bitmap_4bpp_ == 0)
            {
              prg_rbg_0_2w_bitmap_4bpp_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_bitmap),
                  prg_rbg_getcolor_4bpp,
                  prg_generate_rbg_end);
            }
            glUseProgram(prg_rbg_0_2w_bitmap_4bpp_);
            break;
          }
          case 1:
          { // SF3S1( Initial )
            if (prg_rbg_0_2w_bitmap_8bpp_ == 0)
            {
              prg_rbg_0_2w_bitmap_8bpp_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_bitmap),
                  prg_rbg_getcolor_8bpp,
                  prg_generate_rbg_end);
            }
            glUseProgram(prg_rbg_0_2w_bitmap_8bpp_);
            break;
          }
          case 2:
          {
            if (prg_rbg_0_2w_bitmap_16bpp_p_ == 0)
            {
              prg_rbg_0_2w_bitmap_16bpp_p_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_bitmap),
                  prg_rbg_getcolor_16bpp_palette,
                  prg_generate_rbg_end);
            }
            glUseProgram(prg_rbg_0_2w_bitmap_16bpp_p_);
            break;
          }
          case 3:
          { // NHL 97 Title, GRANDIA Title
            if (prg_rbg_0_2w_bitmap_16bpp_ == 0)
            {
              prg_rbg_0_2w_bitmap_16bpp_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_bitmap),
                  prg_rbg_getcolor_16bpp_rbg,
                  prg_generate_rbg_end);
            }
            glUseProgram(prg_rbg_0_2w_bitmap_16bpp_);
            break;
          }
          case 4:
          {
            if (prg_rbg_0_2w_bitmap_32bpp_ == 0)
            {
              prg_rbg_0_2w_bitmap_32bpp_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_bitmap),
                  prg_rbg_getcolor_32bpp_rbg,
                  prg_generate_rbg_end);
            }
            glUseProgram(prg_rbg_0_2w_bitmap_32bpp_);
            break;
          }
          }
        }
        else
        {
          if (rbg->info.patterndatasize == 1)
          {
            switch (rbg->info.colornumber)
            {
            case 0:
            { // Dead or Alive, Rediant Silver Gun, Diehard
              if (prg_rbg_0_2w_p1_4bpp_ == 0)
              {
                prg_rbg_0_2w_p1_4bpp_ = compile_color_dot(
                    S(a_prg_rbg_0_2w_p1_4bpp),
                    prg_rbg_getcolor_4bpp,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_0_2w_p1_4bpp_);
              break;
            }
            case 1:
            { // Sakatuku 2 ( Initial Setting ), Virtua Fighter 2, Virtual-on
              if (prg_rbg_0_2w_p1_8bpp_ == 0)
              {
                prg_rbg_0_2w_p1_8bpp_ = compile_color_dot(
                    S(a_prg_rbg_0_2w_p1_8bpp),
                    prg_rbg_getcolor_8bpp,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_0_2w_p1_8bpp_);
              break;
            }
            case 2:
            {
              if (prg_rbg_0_2w_p1_16bpp_p_ == 0)
              {
                prg_rbg_0_2w_p1_16bpp_p_ = compile_color_dot(
                    S(a_prg_rbg_0_2w_p1_4bpp),
                    prg_rbg_getcolor_16bpp_palette,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_0_2w_p1_16bpp_p_);
              break;
            }
            case 3:
            {
              if (prg_rbg_0_2w_p1_16bpp_ == 0)
              {
                prg_rbg_0_2w_p1_16bpp_ = compile_color_dot(
                    S(a_prg_rbg_0_2w_p1_4bpp),
                    prg_rbg_getcolor_16bpp_rbg,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_0_2w_p1_16bpp_);
              break;
            }
            case 4:
            {
              if (prg_rbg_0_2w_p1_32bpp_ == 0)
              {
                prg_rbg_0_2w_p1_32bpp_ = compile_color_dot(
                    S(a_prg_rbg_0_2w_p1_4bpp),
                    prg_rbg_getcolor_32bpp_rbg,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_0_2w_p1_32bpp_);
              break;
            }
            }
          }
          else
          {
            switch (rbg->info.colornumber)
            {
            case 0:
            {
              if (prg_rbg_0_2w_p2_4bpp_ == 0)
              {
                prg_rbg_0_2w_p2_4bpp_ = compile_color_dot(
                    S(a_prg_rbg_0_2w_p2_4bpp),
                    prg_rbg_getcolor_4bpp,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_0_2w_p2_4bpp_);
              break;
            }
            case 1:
            { // NHL97(In Game), BIOS
              if (prg_rbg_0_2w_p2_8bpp_ == 0)
              {
                prg_rbg_0_2w_p2_8bpp_ = compile_color_dot(
                    S(a_prg_rbg_0_2w_p2_8bpp),
                    prg_rbg_getcolor_8bpp,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_0_2w_p2_8bpp_);
              break;
            }
            case 2:
            {
              if (prg_rbg_0_2w_p2_16bpp_p_ == 0)
              {
                prg_rbg_0_2w_p2_16bpp_p_ = compile_color_dot(
                    S(a_prg_rbg_0_2w_p2_4bpp),
                    prg_rbg_getcolor_16bpp_palette,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_0_2w_p2_16bpp_p_);
              break;
            }
            case 3:
            {
              if (prg_rbg_0_2w_p2_16bpp_ == 0)
              {
                prg_rbg_0_2w_p2_16bpp_ = compile_color_dot(
                    S(a_prg_rbg_0_2w_p2_4bpp),
                    prg_rbg_getcolor_16bpp_rbg,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_0_2w_p2_16bpp_);
              break;
            }
            case 4:
            {
              if (prg_rbg_0_2w_p2_32bpp_ == 0)
              {
                prg_rbg_0_2w_p2_32bpp_ = compile_color_dot(
                    S(a_prg_rbg_0_2w_p2_4bpp),
                    prg_rbg_getcolor_32bpp_rbg,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_0_2w_p2_32bpp_);
              break;
            }
            }
          }
        }
      }
      else if ((fixVdp2Regs->RPMD == 1 && rbg->rgb_type == 0) || rbg->rgb_type == 0x04)
      {
        if (rbg->info.isbitmap)
        {
          switch (rbg->info.colornumber)
          {
          case 0:
          {
            if (prg_rbg_1_2w_bitmap_4bpp_ == 0)
            {
              prg_rbg_1_2w_bitmap_4bpp_ = compile_color_dot(
                  S(a_prg_rbg_1_2w_bitmap),
                  prg_rbg_getcolor_4bpp,
                  prg_generate_rbg_end);
            }
            glUseProgram(prg_rbg_1_2w_bitmap_4bpp_);
            break;
          }
          case 1:
          {
            if (prg_rbg_1_2w_bitmap_8bpp_ == 0)
            {
              prg_rbg_1_2w_bitmap_8bpp_ = compile_color_dot(
                  S(a_prg_rbg_1_2w_bitmap),
                  prg_rbg_getcolor_8bpp,
                  prg_generate_rbg_end);
            }
            glUseProgram(prg_rbg_1_2w_bitmap_8bpp_);
            break;
          }
          case 2:
          {
            if (prg_rbg_1_2w_bitmap_16bpp_p_ == 0)
            {
              prg_rbg_1_2w_bitmap_16bpp_p_ = compile_color_dot(
                  S(a_prg_rbg_1_2w_bitmap),
                  prg_rbg_getcolor_16bpp_palette,
                  prg_generate_rbg_end);
            }
            glUseProgram(prg_rbg_1_2w_bitmap_16bpp_p_);
            break;
          }
          case 3:
          {
            if (prg_rbg_1_2w_bitmap_16bpp_ == 0)
            {
              prg_rbg_1_2w_bitmap_16bpp_ = compile_color_dot(
                  S(a_prg_rbg_1_2w_bitmap),
                  prg_rbg_getcolor_16bpp_rbg,
                  prg_generate_rbg_end);
            }
            glUseProgram(prg_rbg_1_2w_bitmap_16bpp_);
            break;
          }
          case 4:
          {
            if (prg_rbg_1_2w_bitmap_32bpp_ == 0)
            {
              prg_rbg_1_2w_bitmap_32bpp_ = compile_color_dot(
                  S(a_prg_rbg_1_2w_bitmap),
                  prg_rbg_getcolor_32bpp_rbg,
                  prg_generate_rbg_end);
            }
            glUseProgram(prg_rbg_1_2w_bitmap_32bpp_);
            break;
          }
          }
        }
        else
        {
          if (rbg->info.patterndatasize == 1)
          {
            switch (rbg->info.colornumber)
            {
            case 0:
            {
              if (prg_rbg_1_2w_p1_4bpp_ == 0)
              {
                prg_rbg_1_2w_p1_4bpp_ = compile_color_dot(
                    S(a_prg_rbg_1_2w_p1_4bpp),
                    prg_rbg_getcolor_4bpp,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_1_2w_p1_4bpp_);
              break;
            }
            case 1:
            {
              if (prg_rbg_1_2w_p1_8bpp_ == 0)
              {
                prg_rbg_1_2w_p1_8bpp_ = compile_color_dot(
                    S(a_prg_rbg_1_2w_p1_8bpp),
                    prg_rbg_getcolor_8bpp,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_1_2w_p1_8bpp_);
              break;
            }
            case 2:
            {
              if (prg_rbg_1_2w_p1_16bpp_p_ == 0)
              {
                prg_rbg_1_2w_p1_16bpp_p_ = compile_color_dot(
                    S(a_prg_rbg_1_2w_p1_4bpp),
                    prg_rbg_getcolor_16bpp_palette,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_1_2w_p1_16bpp_p_);
              break;
            }
            case 3:
            {
              if (prg_rbg_1_2w_p1_16bpp_ == 0)
              {
                prg_rbg_1_2w_p1_16bpp_ = compile_color_dot(
                    S(a_prg_rbg_1_2w_p1_4bpp),
                    prg_rbg_getcolor_16bpp_rbg,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_1_2w_p1_16bpp_);
              break;
            }
            case 4:
            {
              if (prg_rbg_1_2w_p1_32bpp_ == 0)
              {
                prg_rbg_1_2w_p1_32bpp_ = compile_color_dot(
                    S(a_prg_rbg_0_2w_p1_4bpp),
                    prg_rbg_getcolor_32bpp_rbg,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_1_2w_p1_32bpp_);
              break;
            }
            }
          }
          else
          {
            switch (rbg->info.colornumber)
            {
            case 0:
            {
              if (prg_rbg_1_2w_p2_4bpp_ == 0)
              {
                prg_rbg_1_2w_p2_4bpp_ = compile_color_dot(
                    S(a_prg_rbg_1_2w_p2_4bpp),
                    prg_rbg_getcolor_4bpp,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_1_2w_p2_4bpp_);
              break;
            }
            case 1:
            {
              if (prg_rbg_1_2w_p2_8bpp_ == 0)
              {
                prg_rbg_1_2w_p2_8bpp_ = compile_color_dot(
                    S(a_prg_rbg_1_2w_p2_8bpp),
                    prg_rbg_getcolor_8bpp,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_1_2w_p2_8bpp_);
              break;
            }
            case 2:
            {
              if (prg_rbg_1_2w_p2_16bpp_p_ == 0)
              {
                prg_rbg_1_2w_p2_16bpp_p_ = compile_color_dot(
                    S(a_prg_rbg_1_2w_p2_4bpp),
                    prg_rbg_getcolor_16bpp_palette,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_1_2w_p2_16bpp_p_);
              break;
            }
            case 3:
            {
              if (prg_rbg_1_2w_p2_16bpp_ == 0)
              {
                prg_rbg_1_2w_p2_16bpp_ = compile_color_dot(
                    S(a_prg_rbg_1_2w_p2_4bpp),
                    prg_rbg_getcolor_16bpp_rbg,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_1_2w_p2_16bpp_);
              break;
            }
            case 4:
            {
              if (prg_rbg_1_2w_p2_32bpp_ == 0)
              {
                prg_rbg_1_2w_p2_32bpp_ = compile_color_dot(
                    S(a_prg_rbg_1_2w_p2_4bpp),
                    prg_rbg_getcolor_32bpp_rbg,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_1_2w_p2_32bpp_);
              break;
            }
            }
          }
        }
      }
      else if (fixVdp2Regs->RPMD == 2)
      {
        if (rbg->info.isbitmap)
        {
          switch (rbg->info.colornumber)
          {
          case 0:
          {
            if (prg_rbg_2_2w_bitmap_4bpp_ == 0)
            {
              prg_rbg_2_2w_bitmap_4bpp_ = compile_color_dot(
                  S(a_prg_rbg_2_2w_bitmap),
                  prg_rbg_getcolor_4bpp,
                  prg_generate_rbg_end);
            }
            glUseProgram(prg_rbg_2_2w_bitmap_4bpp_);
            break;
          }
          case 1:
          {
            if (prg_rbg_2_2w_bitmap_8bpp_ == 0)
            {
              prg_rbg_2_2w_bitmap_8bpp_ = compile_color_dot(
                  S(a_prg_rbg_2_2w_bitmap),
                  prg_rbg_getcolor_8bpp,
                  prg_generate_rbg_end);
            }
            glUseProgram(prg_rbg_2_2w_bitmap_8bpp_);
            break;
          }
          case 2:
          {
            if (prg_rbg_2_2w_bitmap_16bpp_p_ == 0)
            {
              prg_rbg_2_2w_bitmap_16bpp_p_ = compile_color_dot(
                  S(a_prg_rbg_2_2w_bitmap),
                  prg_rbg_getcolor_16bpp_palette,
                  prg_generate_rbg_end);
            }
            glUseProgram(prg_rbg_2_2w_bitmap_16bpp_p_);
            break;
          }
          case 3:
          {
            if (prg_rbg_2_2w_bitmap_16bpp_ == 0)
            {
              prg_rbg_2_2w_bitmap_16bpp_ = compile_color_dot(
                  S(a_prg_rbg_2_2w_bitmap),
                  prg_rbg_getcolor_16bpp_rbg,
                  prg_generate_rbg_end);
            }
            glUseProgram(prg_rbg_2_2w_bitmap_16bpp_);
            break;
          }
          case 4:
          {
            if (prg_rbg_2_2w_bitmap_32bpp_ == 0)
            {
              prg_rbg_2_2w_bitmap_32bpp_ = compile_color_dot(
                  S(a_prg_rbg_2_2w_bitmap),
                  prg_rbg_getcolor_32bpp_rbg,
                  prg_generate_rbg_end);
            }
            glUseProgram(prg_rbg_2_2w_bitmap_32bpp_);
            break;
          }
          }
        }
        else
        {
          if (rbg->info.patterndatasize == 1)
          {
            switch (rbg->info.colornumber)
            {
            case 0:
            { // BlukSlash
              if (prg_rbg_2_2w_p1_4bpp_ == 0)
              {
                prg_rbg_2_2w_p1_4bpp_ = compile_color_dot(
                    S(a_prg_rbg_2_2w_p1_4bpp),
                    prg_rbg_getcolor_4bpp,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_2_2w_p1_4bpp_);
              break;
            }
            case 1:
            { // Panzer Dragoon Zwei, Toshiden(Title) ToDo: Sky bug
              if (prg_rbg_2_2w_p1_8bpp_ == 0)
              {
                prg_rbg_2_2w_p1_8bpp_ = compile_color_dot(
                    S(a_prg_rbg_2_2w_p1_8bpp),
                    prg_rbg_getcolor_8bpp,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_2_2w_p1_8bpp_);
              break;
            }
            case 2:
            {
              if (prg_rbg_2_2w_p1_16bpp_p_ == 0)
              {
                prg_rbg_2_2w_p1_16bpp_p_ = compile_color_dot(
                    S(a_prg_rbg_2_2w_p1_4bpp),
                    prg_rbg_getcolor_16bpp_palette,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_2_2w_p1_16bpp_p_);
              break;
            }
            case 3:
            {
              if (prg_rbg_2_2w_p1_16bpp_ == 0)
              {
                prg_rbg_2_2w_p1_16bpp_ = compile_color_dot(
                    S(a_prg_rbg_2_2w_p1_4bpp),
                    prg_rbg_getcolor_16bpp_rbg,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_2_2w_p1_16bpp_);
              break;
            }
            case 4:
            {
              if (prg_rbg_2_2w_p1_32bpp_ == 0)
              {
                prg_rbg_2_2w_p1_32bpp_ = compile_color_dot(
                    S(a_prg_rbg_2_2w_p1_4bpp),
                    prg_rbg_getcolor_32bpp_rbg,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_2_2w_p1_32bpp_);
              break;
            }
            }
          }
          else
          {
            switch (rbg->info.colornumber)
            {
            case 0:
            {
              if (prg_rbg_2_2w_p2_4bpp_ == 0)
              {
                prg_rbg_2_2w_p2_4bpp_ = compile_color_dot(
                    S(a_prg_rbg_2_2w_p2_4bpp),
                    prg_rbg_getcolor_4bpp,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_2_2w_p2_4bpp_);
              break;
            }
            case 1:
            {
              if (prg_rbg_2_2w_p2_8bpp_ == 0)
              {
                prg_rbg_2_2w_p2_8bpp_ = compile_color_dot(
                    S(a_prg_rbg_2_2w_p2_8bpp),
                    prg_rbg_getcolor_8bpp,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_2_2w_p2_8bpp_);
              break;
            }
            case 2:
            {
              if (prg_rbg_2_2w_p2_16bpp_p_ == 0)
              {
                prg_rbg_2_2w_p2_16bpp_p_ = compile_color_dot(
                    S(a_prg_rbg_2_2w_p2_4bpp),
                    prg_rbg_getcolor_16bpp_palette,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_2_2w_p2_16bpp_p_);
              break;
            }
            case 3:
            {
              if (prg_rbg_2_2w_p2_16bpp_ == 0)
              {
                prg_rbg_2_2w_p2_16bpp_ = compile_color_dot(
                    S(a_prg_rbg_2_2w_p2_4bpp),
                    prg_rbg_getcolor_16bpp_rbg,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_2_2w_p2_16bpp_);
              break;
            }
            case 4:
            {
              if (prg_rbg_2_2w_p2_32bpp_ == 0)
              {
                prg_rbg_2_2w_p2_32bpp_ = compile_color_dot(
                    S(a_prg_rbg_2_2w_p2_4bpp),
                    prg_rbg_getcolor_32bpp_rbg,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_2_2w_p2_32bpp_);
              break;
            }
            }
          }
        }
      }
      else if (fixVdp2Regs->RPMD == 3)
      {

        if (rbg->info.isbitmap)
        {
          switch (rbg->info.colornumber)
          {
          case 0:
          {
            if (prg_rbg_3_2w_bitmap_4bpp_ == 0)
            {
              prg_rbg_3_2w_bitmap_4bpp_ = compile_color_dot(
                  S(a_prg_rbg_3_2w_bitmap),
                  prg_rbg_getcolor_4bpp,
                  prg_generate_rbg_end);
            }
            glUseProgram(prg_rbg_3_2w_bitmap_4bpp_);
            break;
          }
          case 1:
          {
            if (prg_rbg_3_2w_bitmap_8bpp_ == 0)
            {
              prg_rbg_3_2w_bitmap_8bpp_ = compile_color_dot(
                  S(a_prg_rbg_3_2w_bitmap),
                  prg_rbg_getcolor_8bpp,
                  prg_generate_rbg_end);
            }
            glUseProgram(prg_rbg_3_2w_bitmap_8bpp_);
            break;
          }
          case 2:
          {
            if (prg_rbg_3_2w_bitmap_16bpp_p_ == 0)
            {
              prg_rbg_3_2w_bitmap_16bpp_p_ = compile_color_dot(
                  S(a_prg_rbg_3_2w_bitmap),
                  prg_rbg_getcolor_16bpp_palette,
                  prg_generate_rbg_end);
            }
            glUseProgram(prg_rbg_3_2w_bitmap_16bpp_p_);
            break;
          }
          case 3:
          {
            if (prg_rbg_3_2w_bitmap_16bpp_ == 0)
            {
              prg_rbg_3_2w_bitmap_16bpp_ = compile_color_dot(
                  S(a_prg_rbg_3_2w_bitmap),
                  prg_rbg_getcolor_16bpp_rbg,
                  prg_generate_rbg_end);
            }
            glUseProgram(prg_rbg_3_2w_bitmap_16bpp_);
            break;
          }
          case 4:
          {
            if (prg_rbg_3_2w_bitmap_32bpp_ == 0)
            {
              prg_rbg_3_2w_bitmap_32bpp_ = compile_color_dot(
                  S(a_prg_rbg_3_2w_bitmap),
                  prg_rbg_getcolor_32bpp_rbg,
                  prg_generate_rbg_end);
            }
            glUseProgram(prg_rbg_3_2w_bitmap_32bpp_);
            break;
          }
          }
        }
        else
        {
          if (rbg->info.patterndatasize == 1)
          {
            switch (rbg->info.colornumber)
            {
            case 0:
            {
              if (prg_rbg_3_2w_p1_4bpp_ == 0)
              {
                prg_rbg_3_2w_p1_4bpp_ = compile_color_dot(
                    S(a_prg_rbg_3_2w_p1_4bpp),
                    prg_rbg_getcolor_4bpp,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_3_2w_p1_4bpp_);
              break;
            }
            case 1:
            { // Final Fight Revenge, Grandia main
              if (prg_rbg_3_2w_p1_8bpp_ == 0)
              {
                prg_rbg_3_2w_p1_8bpp_ = compile_color_dot(
                    S(a_prg_rbg_3_2w_p1_8bpp),
                    prg_rbg_getcolor_8bpp,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_3_2w_p1_8bpp_);
              break;
            }
            case 2:
            {
              if (prg_rbg_3_2w_p1_16bpp_p_ == 0)
              {
                prg_rbg_3_2w_p1_16bpp_p_ = compile_color_dot(
                    S(a_prg_rbg_3_2w_p1_4bpp),
                    prg_rbg_getcolor_16bpp_palette,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_3_2w_p1_16bpp_p_);
              break;
            }
            case 3:
            { // Power Drift
              if (prg_rbg_3_2w_p1_16bpp_ == 0)
              {
                prg_rbg_3_2w_p1_16bpp_ = compile_color_dot(
                    S(a_prg_rbg_3_2w_p1_4bpp),
                    prg_rbg_getcolor_16bpp_rbg,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_3_2w_p1_16bpp_);
              break;
            }
            case 4:
            {
              if (prg_rbg_3_2w_p1_32bpp_ == 0)
              {
                prg_rbg_3_2w_p1_32bpp_ = compile_color_dot(
                    S(a_prg_rbg_3_2w_p1_4bpp),
                    prg_rbg_getcolor_32bpp_rbg,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_3_2w_p1_32bpp_);
              break;
            }
            }
          }
          else
          {
            switch (rbg->info.colornumber)
            {
            case 0:
            {
              if (prg_rbg_3_2w_p2_4bpp_ == 0)
              {
                prg_rbg_3_2w_p2_4bpp_ = compile_color_dot(
                    S(a_prg_rbg_3_2w_p2_4bpp),
                    prg_rbg_getcolor_4bpp,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_3_2w_p2_4bpp_);
              break;
            }
            case 1:
            {
              if (prg_rbg_3_2w_p2_8bpp_ == 0)
              {
                prg_rbg_3_2w_p2_8bpp_ = compile_color_dot(
                    S(a_prg_rbg_3_2w_p2_8bpp),
                    prg_rbg_getcolor_8bpp,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_3_2w_p2_8bpp_);
              break;
            }
            case 2:
            {
              if (prg_rbg_3_2w_p2_16bpp_p_ == 0)
              {
                prg_rbg_3_2w_p2_16bpp_p_ = compile_color_dot(
                    S(a_prg_rbg_3_2w_p2_4bpp),
                    prg_rbg_getcolor_16bpp_palette,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_3_2w_p2_16bpp_p_);
              break;
            }
            case 3:
            {
              if (prg_rbg_3_2w_p2_16bpp_ == 0)
              {
                prg_rbg_3_2w_p2_16bpp_ = compile_color_dot(
                    S(a_prg_rbg_3_2w_p2_4bpp),
                    prg_rbg_getcolor_16bpp_rbg,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_3_2w_p2_16bpp_);
              break;
            }
            case 4:
            {
              if (prg_rbg_3_2w_p2_32bpp_ == 0)
              {
                prg_rbg_3_2w_p2_32bpp_ = compile_color_dot(
                    S(a_prg_rbg_3_2w_p2_4bpp),
                    prg_rbg_getcolor_32bpp_rbg,
                    prg_generate_rbg_end);
              }
              glUseProgram(prg_rbg_3_2w_p2_32bpp_);
              break;
            }
            }
          }
        }
      }
    }

    ErrorHandle("glUseProgram");
    CS_DEBUG_LOG("[CS] glUseProgram OK");

    // Safety check: Vdp2Ram can be NULL during mode transitions (movie playback, etc.)
    if (Vdp2Ram == nullptr) {
      CS_DEBUG_LOG("[CS] SKIP: Vdp2Ram is NULL (mid-update)");
      return; // Cannot render RBG without VRAM data
    }

    CS_DEBUG_LOG("[CS] Binding VRAM buffer (ssbo_vram_=%u)", ssbo_vram_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_vram_);
    // glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, 0x80000, (void*)Vdp2Ram);
    if (mapped_vram == nullptr)
      mapped_vram = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, 0x80000, GL_MAP_WRITE_BIT);
    // Safety check: glMapBufferRange can return NULL on driver errors
    if (mapped_vram != nullptr) {
      CS_DEBUG_LOG("[CS] memcpy to mapped_vram");
      memcpy(mapped_vram, Vdp2Ram, 0x80000);
    } else {
      CS_DEBUG_LOG("[CS] WARNING: glMapBufferRange returned NULL!");
    }
    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    mapped_vram = nullptr;
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, ssbo_vram_);
    ErrorHandle("glBindBufferBase 1 (vram)");
    CS_DEBUG_LOG("[CS] VRAM buffer bound OK");

    //if (rbg->info.specialcolormode == 3 || paraA.k_mem_type != 0 || paraB.k_mem_type != 0) {
      // Safety check: Vdp2ColorRam can be NULL during mode transitions
      if (Vdp2ColorRam == nullptr) {
        return;
      }
      if (ssbo_cram_ == 0) {
        glGenBuffers(1, &ssbo_cram_);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_cram_);
        glBufferData(GL_SHADER_STORAGE_BUFFER, 0x1000, NULL, GL_DYNAMIC_DRAW);
        //yprintf("glGenBuffers %d",ssbo_cram_);
      }
      glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_cram_);
      glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, 0x1000, (void *)Vdp2ColorRam);
      glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, ssbo_cram_);
      //yprintf("glBindBufferBase %d",ssbo_cram_);
    //}

    CS_DEBUG_LOG("[CS] Binding paraA/paraB buffer");
    vdp2rotationparameter_struct upA = paraA;
    vdp2rotationparameter_struct upB = paraB;
    rbgApplyLayerBankMasks(rbg->rgb_type, &upA, &upB);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_paraA_);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(vdp2rotationparameter_struct), (void*)&upA);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, sizeof(vdp2rotationparameter_struct), sizeof(vdp2rotationparameter_struct), (void*)&upB);
    ErrorHandle("glBufferSubData");
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, ssbo_paraA_);
    ErrorHandle("glBindBufferBase 2 (paraA)");
    CS_DEBUG_LOG("[CS] paraA/paraB bound OK");

    if (ssbo_window_ != 0 && rbg->info.pWinInfo != nullptr)
    {
      glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_window_);
      glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(vdp2WindowInfo) * (int(rbg->vres / rbg->rotate_mval_v) << rbg->info.hres_shift), (void *)rbg->info.pWinInfo);
      glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, ssbo_window_);
      ErrorHandle("glBindBufferBase 4");
    }
    else if (ssbo_window_ != 0)
    {
      // Always bind to avoid undefined behavior if shader accesses index 4
      glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, ssbo_window_);
      ErrorHandle("glBindBufferBase 4 (Empty)");
    }

    uniform.hres_scale = 1.0 / rbg->rotate_mval_h;
    uniform.vres_scale = 1.0 / rbg->rotate_mval_v;
    uniform.cellw = rbg->info.cellw;
    uniform.cellh = rbg->info.cellh;
    uniform.paladdr_ = rbg->info.paladdr;
    uniform.pagesize = rbg->pagesize;
    uniform.patternshift = rbg->patternshift;
    uniform.planew = rbg->info.planew;
    uniform.pagewh = rbg->info.pagewh;
    uniform.patterndatasize = rbg->info.patterndatasize;
    uniform.supplementdata = rbg->info.supplementdata;
    uniform.auxmode = rbg->info.auxmode;
    uniform.patternwh = rbg->info.patternwh;
    uniform.coloroffset = rbg->info.coloroffset;
    uniform.transparencyenable = rbg->info.transparencyenable;
    uniform.specialcolormode = rbg->info.specialcolormode;
    uniform.specialcolorfunction = rbg->info.specialcolorfunction;
    uniform.specialcode = rbg->info.specialcode;
    uniform.colornumber = rbg->info.colornumber;
    uniform.window_area_mode = rbg->info.WindwAreaMode;
    uniform.alpha_ = (float)rbg->info.alpha / 255.0f;
    uniform.specialprimode = rbg->info.specialprimode;
    uniform.priority = rbg->info.priority;

    if (Vdp2Internal.ColorMode < 2)
    {
      uniform.cram_shift = 1;
    }
    else
    {
      uniform.cram_shift = 2;
    }
    uniform.hires_shift = rbg->info.hres_shift;

    glBindBuffer(GL_UNIFORM_BUFFER, scene_uniform);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(RBGUniform), (void *)&uniform);
    ErrorHandle("glBufferSubData");
    glBindBufferBase(GL_UNIFORM_BUFFER, 3, scene_uniform);
    ErrorHandle("glBindBufferBase 3 (uniform)");

    GLuint target_tex = tex_surface_;
    if (rbg->rgb_type == 0x04 && tex_surface_1 != 0)
    {
      target_tex = tex_surface_1;
    }

    if (rbg->rgb_type == 0x04)
    {
      if (tex_surface_1 == 0)
      {
        glActiveTexture(GL_TEXTURE0);
        glGenTextures(1, &tex_surface_1);
        glBindTexture(GL_TEXTURE_2D, tex_surface_1);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        ErrorHandle("glBindTexture");
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, tex_width_, tex_height_);
        ErrorHandle("glTexStorage2D");
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        ErrorHandle("glTexParameteri");
        target_tex = tex_surface_1;
      }
    }

    glBindImageTexture(0, target_tex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    ErrorHandle("glBindImageTexture 0 (outSurface)");
    CS_DEBUG_LOG("[CS] Image texture bound, target_tex=%u", target_tex);

    CS_DEBUG_LOG("[CS] PRE glDispatchCompute(%d, %d, 1)", work_groups_x, work_groups_y);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    glDispatchCompute(work_groups_x, work_groups_y, 1);
    ErrorHandle("glDispatchCompute");
    CS_DEBUG_LOG("[CS] POST glDispatchCompute OK");

    glBindBuffer(GL_UNIFORM_BUFFER, 0);
  }

  //-----------------------------------------------
  GLuint getTexture(int id)
  {
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    if (id == 1)
    {
      return tex_surface_;
    }
    return tex_surface_1;
  }

  //-----------------------------------------------
  void onFinish()
  {
    if (ssbo_vram_ != 0 && mapped_vram == nullptr)
    {
      glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_vram_);
      mapped_vram = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, 0x80000, GL_MAP_WRITE_BIT);
      glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }
  }
};

#endif // HAVE_LIBGL || _OGLES3_
#if defined(HAVE_VULKAN)

// Shader-source list helper, normally defined inside the GL RBGGenerator
// above — redefine for Vulkan-only builds where that region is compiled out.
#ifndef S
#define S(A) A, sizeof(A) / sizeof(char *)
#endif

#include "vulkan/RBGGeneratorVulkan.h"
#include <iostream>
/*
#define VK_CHECK_RESULT(f)																				\
{																										\
  vk::Result res = (f);																					\
  if (res != vk::Result::eSuccess)																				\
  {																									\
    std::cout << "Fatal : VkResult is \"" << vk::to_string(res) << "\" in " << __FILE__ << " at line " << __LINE__ << "\n"; \
    assert(res == vk::Result::eSuccess);																		\
  }																									\
}
*/

#include "shaderc/shaderc.hpp"
using shaderc::CompileOptions;
using shaderc::Compiler;
using shaderc::SpvCompilationResult;

RBGGeneratorVulkan::RBGGeneratorVulkan()
{

  rbgUniformParam = new RBGUniform();
}

RBGGeneratorVulkan::~RBGGeneratorVulkan()
{
  delete rbgUniformParam;
  if (vulkan != nullptr)
  {
    VkDevice d = vulkan->getDevice();
    vk::Device device(d);

    // Destroy images and views
    for (auto &surface : tex_surface)
    {
      if (surface.view)
        device.destroyImageView(surface.view);
      if (surface.image)
        device.destroyImage(surface.image);
      if (surface.mem)
        device.freeMemory(surface.mem);
    }

    // Destroy buffers
    if (ssbo_vram_.buf)
    {
      device.destroyBuffer(ssbo_vram_.buf);
      device.freeMemory(ssbo_vram_.mem);
    }
    if (ssbo_cram_.buf)
    {
      device.destroyBuffer(ssbo_cram_.buf);
      device.freeMemory(ssbo_cram_.mem);
    }
    if (ssbo_window_.buf)
    {
      device.destroyBuffer(ssbo_window_.buf);
      device.freeMemory(ssbo_window_.mem);
    }
    if (ssbo_paraA_.buf)
    {
      device.destroyBuffer(ssbo_paraA_.buf);
      device.freeMemory(ssbo_paraA_.mem);
    }

    // Destroy uniform buffers
    for (auto &uniform : rbgUniform)
    {
      if (uniform.buf)
      {
        device.destroyBuffer(uniform.buf);
        device.freeMemory(uniform.mem);
      }
    }

    // Destroy pipeline resources
    if (pipelineLayout)
      device.destroyPipelineLayout(pipelineLayout);
    if (descriptorPool)
      device.destroyDescriptorPool(descriptorPool);
    if (descriptorSetLayout)
      device.destroyDescriptorSetLayout(descriptorSetLayout);
    if (sampler)
      device.destroySampler(sampler);

    // Destroy command resources
    if (commandPool) {
      if (command[0]) {  // at least one command buffer was allocated
        device.freeCommandBuffers(commandPool, MAX_RBG_RENDER, command);
      }
      device.destroyCommandPool(commandPool);
    }

    // Destroy fences
    for (auto &fence : commandfence)
    {
      if (fence)
        device.destroyFence(fence);
    }

    // Destroy semaphores
    for (auto &sem : semaphores)
    {
      if (sem.ready)
        device.destroySemaphore(sem.ready);
      if (sem.complete)
        device.destroySemaphore(sem.complete);
    }

    // Destroy all pipelines
    // RBG 0
    if (prg_rbg_0_2w_bitmap_4bpp_)
      device.destroyPipeline(prg_rbg_0_2w_bitmap_4bpp_);
    if (prg_rbg_0_2w_bitmap_8bpp_)
      device.destroyPipeline(prg_rbg_0_2w_bitmap_8bpp_);
    if (prg_rbg_0_2w_bitmap_16bpp_p_)
      device.destroyPipeline(prg_rbg_0_2w_bitmap_16bpp_p_);
    if (prg_rbg_0_2w_bitmap_16bpp_)
      device.destroyPipeline(prg_rbg_0_2w_bitmap_16bpp_);
    if (prg_rbg_0_2w_bitmap_32bpp_)
      device.destroyPipeline(prg_rbg_0_2w_bitmap_32bpp_);
    if (prg_rbg_0_2w_p1_4bpp_)
      device.destroyPipeline(prg_rbg_0_2w_p1_4bpp_);
    if (prg_rbg_0_2w_p2_4bpp_)
      device.destroyPipeline(prg_rbg_0_2w_p2_4bpp_);
    if (prg_rbg_0_2w_p1_8bpp_)
      device.destroyPipeline(prg_rbg_0_2w_p1_8bpp_);
    if (prg_rbg_0_2w_p2_8bpp_)
      device.destroyPipeline(prg_rbg_0_2w_p2_8bpp_);
    if (prg_rbg_0_2w_p1_16bpp_p_)
      device.destroyPipeline(prg_rbg_0_2w_p1_16bpp_p_);
    if (prg_rbg_0_2w_p2_16bpp_p_)
      device.destroyPipeline(prg_rbg_0_2w_p2_16bpp_p_);
    if (prg_rbg_0_2w_p1_16bpp_)
      device.destroyPipeline(prg_rbg_0_2w_p1_16bpp_);
    if (prg_rbg_0_2w_p2_16bpp_)
      device.destroyPipeline(prg_rbg_0_2w_p2_16bpp_);
    if (prg_rbg_0_2w_p1_32bpp_)
      device.destroyPipeline(prg_rbg_0_2w_p1_32bpp_);
    if (prg_rbg_0_2w_p2_32bpp_)
      device.destroyPipeline(prg_rbg_0_2w_p2_32bpp_);

    // RBG 1
    if (prg_rbg_1_2w_bitmap_4bpp_)
      device.destroyPipeline(prg_rbg_1_2w_bitmap_4bpp_);
    if (prg_rbg_1_2w_bitmap_8bpp_)
      device.destroyPipeline(prg_rbg_1_2w_bitmap_8bpp_);
    if (prg_rbg_1_2w_bitmap_16bpp_p_)
      device.destroyPipeline(prg_rbg_1_2w_bitmap_16bpp_p_);
    if (prg_rbg_1_2w_bitmap_16bpp_)
      device.destroyPipeline(prg_rbg_1_2w_bitmap_16bpp_);
    if (prg_rbg_1_2w_bitmap_32bpp_)
      device.destroyPipeline(prg_rbg_1_2w_bitmap_32bpp_);
    if (prg_rbg_1_2w_p1_4bpp_)
      device.destroyPipeline(prg_rbg_1_2w_p1_4bpp_);
    if (prg_rbg_1_2w_p2_4bpp_)
      device.destroyPipeline(prg_rbg_1_2w_p2_4bpp_);
    if (prg_rbg_1_2w_p1_8bpp_)
      device.destroyPipeline(prg_rbg_1_2w_p1_8bpp_);
    if (prg_rbg_1_2w_p2_8bpp_)
      device.destroyPipeline(prg_rbg_1_2w_p2_8bpp_);
    if (prg_rbg_1_2w_p1_16bpp_p_)
      device.destroyPipeline(prg_rbg_1_2w_p1_16bpp_p_);
    if (prg_rbg_1_2w_p2_16bpp_p_)
      device.destroyPipeline(prg_rbg_1_2w_p2_16bpp_p_);
    if (prg_rbg_1_2w_p1_16bpp_)
      device.destroyPipeline(prg_rbg_1_2w_p1_16bpp_);
    if (prg_rbg_1_2w_p2_16bpp_)
      device.destroyPipeline(prg_rbg_1_2w_p2_16bpp_);
    if (prg_rbg_1_2w_p1_32bpp_)
      device.destroyPipeline(prg_rbg_1_2w_p1_32bpp_);
    if (prg_rbg_1_2w_p2_32bpp_)
      device.destroyPipeline(prg_rbg_1_2w_p2_32bpp_);

    // RBG 2
    if (prg_rbg_2_2w_bitmap_4bpp_)
      device.destroyPipeline(prg_rbg_2_2w_bitmap_4bpp_);
    if (prg_rbg_2_2w_bitmap_8bpp_)
      device.destroyPipeline(prg_rbg_2_2w_bitmap_8bpp_);
    if (prg_rbg_2_2w_bitmap_16bpp_p_)
      device.destroyPipeline(prg_rbg_2_2w_bitmap_16bpp_p_);
    if (prg_rbg_2_2w_bitmap_16bpp_)
      device.destroyPipeline(prg_rbg_2_2w_bitmap_16bpp_);
    if (prg_rbg_2_2w_bitmap_32bpp_)
      device.destroyPipeline(prg_rbg_2_2w_bitmap_32bpp_);
    if (prg_rbg_2_2w_p1_4bpp_)
      device.destroyPipeline(prg_rbg_2_2w_p1_4bpp_);
    if (prg_rbg_2_2w_p2_4bpp_)
      device.destroyPipeline(prg_rbg_2_2w_p2_4bpp_);
    if (prg_rbg_2_2w_p1_8bpp_)
      device.destroyPipeline(prg_rbg_2_2w_p1_8bpp_);
    if (prg_rbg_2_2w_p2_8bpp_)
      device.destroyPipeline(prg_rbg_2_2w_p2_8bpp_);
    if (prg_rbg_2_2w_p1_16bpp_p_)
      device.destroyPipeline(prg_rbg_2_2w_p1_16bpp_p_);
    if (prg_rbg_2_2w_p2_16bpp_p_)
      device.destroyPipeline(prg_rbg_2_2w_p2_16bpp_p_);
    if (prg_rbg_2_2w_p1_16bpp_)
      device.destroyPipeline(prg_rbg_2_2w_p1_16bpp_);
    if (prg_rbg_2_2w_p2_16bpp_)
      device.destroyPipeline(prg_rbg_2_2w_p2_16bpp_);
    if (prg_rbg_2_2w_p1_32bpp_)
      device.destroyPipeline(prg_rbg_2_2w_p1_32bpp_);
    if (prg_rbg_2_2w_p2_32bpp_)
      device.destroyPipeline(prg_rbg_2_2w_p2_32bpp_);

    // RBG 3
    if (prg_rbg_3_2w_bitmap_4bpp_)
      device.destroyPipeline(prg_rbg_3_2w_bitmap_4bpp_);
    if (prg_rbg_3_2w_bitmap_8bpp_)
      device.destroyPipeline(prg_rbg_3_2w_bitmap_8bpp_);
    if (prg_rbg_3_2w_bitmap_16bpp_p_)
      device.destroyPipeline(prg_rbg_3_2w_bitmap_16bpp_p_);
    if (prg_rbg_3_2w_bitmap_16bpp_)
      device.destroyPipeline(prg_rbg_3_2w_bitmap_16bpp_);
    if (prg_rbg_3_2w_bitmap_32bpp_)
      device.destroyPipeline(prg_rbg_3_2w_bitmap_32bpp_);
    if (prg_rbg_3_2w_p1_4bpp_)
      device.destroyPipeline(prg_rbg_3_2w_p1_4bpp_);
    if (prg_rbg_3_2w_p2_4bpp_)
      device.destroyPipeline(prg_rbg_3_2w_p2_4bpp_);
    if (prg_rbg_3_2w_p1_8bpp_)
      device.destroyPipeline(prg_rbg_3_2w_p1_8bpp_);
    if (prg_rbg_3_2w_p2_8bpp_)
      device.destroyPipeline(prg_rbg_3_2w_p2_8bpp_);
    if (prg_rbg_3_2w_p1_16bpp_p_)
      device.destroyPipeline(prg_rbg_3_2w_p1_16bpp_p_);
    if (prg_rbg_3_2w_p2_16bpp_p_)
      device.destroyPipeline(prg_rbg_3_2w_p2_16bpp_p_);
    if (prg_rbg_3_2w_p1_16bpp_)
      device.destroyPipeline(prg_rbg_3_2w_p1_16bpp_);
    if (prg_rbg_3_2w_p2_16bpp_)
      device.destroyPipeline(prg_rbg_3_2w_p2_16bpp_);
    if (prg_rbg_3_2w_p1_32bpp_)
      device.destroyPipeline(prg_rbg_3_2w_p1_32bpp_);
    if (prg_rbg_3_2w_p2_32bpp_)
      device.destroyPipeline(prg_rbg_3_2w_p2_32bpp_);

    // Line versions
    if (prg_rbg_0_2w_bitmap_4bpp_line_)
      device.destroyPipeline(prg_rbg_0_2w_bitmap_4bpp_line_);
    if (prg_rbg_0_2w_bitmap_8bpp_line_)
      device.destroyPipeline(prg_rbg_0_2w_bitmap_8bpp_line_);
    if (prg_rbg_0_2w_bitmap_16bpp_p_line_)
      device.destroyPipeline(prg_rbg_0_2w_bitmap_16bpp_p_line_);

    if (prg_rbg_0_2w_bitmap_16bpp_line_)
      device.destroyPipeline(prg_rbg_0_2w_bitmap_16bpp_line_);
    if (prg_rbg_0_2w_bitmap_32bpp_line_)
      device.destroyPipeline(prg_rbg_0_2w_bitmap_32bpp_line_);
    if (prg_rbg_0_2w_p1_4bpp_line_)
      device.destroyPipeline(prg_rbg_0_2w_p1_4bpp_line_);
    if (prg_rbg_0_2w_p2_4bpp_line_)
      device.destroyPipeline(prg_rbg_0_2w_p2_4bpp_line_);
    if (prg_rbg_0_2w_p1_8bpp_line_)
      device.destroyPipeline(prg_rbg_0_2w_p1_8bpp_line_);
    if (prg_rbg_0_2w_p2_8bpp_line_)
      device.destroyPipeline(prg_rbg_0_2w_p2_8bpp_line_);
    if (prg_rbg_0_2w_p1_16bpp_p_line_)
      device.destroyPipeline(prg_rbg_0_2w_p1_16bpp_p_line_);
    if (prg_rbg_0_2w_p2_16bpp_p_line_)
      device.destroyPipeline(prg_rbg_0_2w_p2_16bpp_p_line_);
    if (prg_rbg_0_2w_p1_16bpp_line_)
      device.destroyPipeline(prg_rbg_0_2w_p1_16bpp_line_);
    if (prg_rbg_0_2w_p2_16bpp_line_)
      device.destroyPipeline(prg_rbg_0_2w_p2_16bpp_line_);
    if (prg_rbg_0_2w_p1_32bpp_line_)
      device.destroyPipeline(prg_rbg_0_2w_p1_32bpp_line_);
    if (prg_rbg_0_2w_p2_32bpp_line_)
      device.destroyPipeline(prg_rbg_0_2w_p2_32bpp_line_);

    // Line versions RBG 1
    if (prg_rbg_1_2w_bitmap_4bpp_line_)
      device.destroyPipeline(prg_rbg_1_2w_bitmap_4bpp_line_);
    if (prg_rbg_1_2w_bitmap_8bpp_line_)
      device.destroyPipeline(prg_rbg_1_2w_bitmap_8bpp_line_);
    if (prg_rbg_1_2w_bitmap_16bpp_p_line_)
      device.destroyPipeline(prg_rbg_1_2w_bitmap_16bpp_p_line_);
    if (prg_rbg_1_2w_bitmap_16bpp_line_)
      device.destroyPipeline(prg_rbg_1_2w_bitmap_16bpp_line_);
    if (prg_rbg_1_2w_bitmap_32bpp_line_)
      device.destroyPipeline(prg_rbg_1_2w_bitmap_32bpp_line_);
    if (prg_rbg_1_2w_p1_4bpp_line_)
      device.destroyPipeline(prg_rbg_1_2w_p1_4bpp_line_);
    if (prg_rbg_1_2w_p2_4bpp_line_)
      device.destroyPipeline(prg_rbg_1_2w_p2_4bpp_line_);
    if (prg_rbg_1_2w_p1_8bpp_line_)
      device.destroyPipeline(prg_rbg_1_2w_p1_8bpp_line_);
    if (prg_rbg_1_2w_p2_8bpp_line_)
      device.destroyPipeline(prg_rbg_1_2w_p2_8bpp_line_);
    if (prg_rbg_1_2w_p1_16bpp_p_line_)
      device.destroyPipeline(prg_rbg_1_2w_p1_16bpp_p_line_);
    if (prg_rbg_1_2w_p2_16bpp_p_line_)
      device.destroyPipeline(prg_rbg_1_2w_p2_16bpp_p_line_);
    if (prg_rbg_1_2w_p1_16bpp_line_)
      device.destroyPipeline(prg_rbg_1_2w_p1_16bpp_line_);
    if (prg_rbg_1_2w_p2_16bpp_line_)
      device.destroyPipeline(prg_rbg_1_2w_p2_16bpp_line_);
    if (prg_rbg_1_2w_p1_32bpp_line_)
      device.destroyPipeline(prg_rbg_1_2w_p1_32bpp_line_);
    if (prg_rbg_1_2w_p2_32bpp_line_)
      device.destroyPipeline(prg_rbg_1_2w_p2_32bpp_line_);

    // Line versions RBG 2
    if (prg_rbg_2_2w_bitmap_4bpp_line_)
      device.destroyPipeline(prg_rbg_2_2w_bitmap_4bpp_line_);
    if (prg_rbg_2_2w_bitmap_8bpp_line_)
      device.destroyPipeline(prg_rbg_2_2w_bitmap_8bpp_line_);
    if (prg_rbg_2_2w_bitmap_16bpp_p_line_)
      device.destroyPipeline(prg_rbg_2_2w_bitmap_16bpp_p_line_);
    if (prg_rbg_2_2w_bitmap_16bpp_line_)
      device.destroyPipeline(prg_rbg_2_2w_bitmap_16bpp_line_);
    if (prg_rbg_2_2w_bitmap_32bpp_line_)
      device.destroyPipeline(prg_rbg_2_2w_bitmap_32bpp_line_);
    if (prg_rbg_2_2w_p1_4bpp_line_)
      device.destroyPipeline(prg_rbg_2_2w_p1_4bpp_line_);
    if (prg_rbg_2_2w_p2_4bpp_line_)
      device.destroyPipeline(prg_rbg_2_2w_p2_4bpp_line_);
    if (prg_rbg_2_2w_p1_8bpp_line_)
      device.destroyPipeline(prg_rbg_2_2w_p1_8bpp_line_);
    if (prg_rbg_2_2w_p2_8bpp_line_)
      device.destroyPipeline(prg_rbg_2_2w_p2_8bpp_line_);
    if (prg_rbg_2_2w_p1_16bpp_p_line_)
      device.destroyPipeline(prg_rbg_2_2w_p1_16bpp_p_line_);
    if (prg_rbg_2_2w_p2_16bpp_p_line_)
      device.destroyPipeline(prg_rbg_2_2w_p2_16bpp_p_line_);
    if (prg_rbg_2_2w_p1_16bpp_line_)
      device.destroyPipeline(prg_rbg_2_2w_p1_16bpp_line_);
    if (prg_rbg_2_2w_p2_16bpp_line_)
      device.destroyPipeline(prg_rbg_2_2w_p2_16bpp_line_);
    if (prg_rbg_2_2w_p1_32bpp_line_)
      device.destroyPipeline(prg_rbg_2_2w_p1_32bpp_line_);
    if (prg_rbg_2_2w_p2_32bpp_line_)
      device.destroyPipeline(prg_rbg_2_2w_p2_32bpp_line_);

    // Line versions RBG 3
    if (prg_rbg_3_2w_bitmap_4bpp_line_)
      device.destroyPipeline(prg_rbg_3_2w_bitmap_4bpp_line_);
    if (prg_rbg_3_2w_bitmap_8bpp_line_)
      device.destroyPipeline(prg_rbg_3_2w_bitmap_8bpp_line_);
    if (prg_rbg_3_2w_bitmap_16bpp_p_line_)
      device.destroyPipeline(prg_rbg_3_2w_bitmap_16bpp_p_line_);
    if (prg_rbg_3_2w_bitmap_16bpp_line_)
      device.destroyPipeline(prg_rbg_3_2w_bitmap_16bpp_line_);
    if (prg_rbg_3_2w_bitmap_32bpp_line_)
      device.destroyPipeline(prg_rbg_3_2w_bitmap_32bpp_line_);
    if (prg_rbg_3_2w_p1_4bpp_line_)
      device.destroyPipeline(prg_rbg_3_2w_p1_4bpp_line_);
    if (prg_rbg_3_2w_p2_4bpp_line_)
      device.destroyPipeline(prg_rbg_3_2w_p2_4bpp_line_);
    if (prg_rbg_3_2w_p1_8bpp_line_)
      device.destroyPipeline(prg_rbg_3_2w_p1_8bpp_line_);
    if (prg_rbg_3_2w_p2_8bpp_line_)
      device.destroyPipeline(prg_rbg_3_2w_p2_8bpp_line_);
    if (prg_rbg_3_2w_p1_16bpp_p_line_)
      device.destroyPipeline(prg_rbg_3_2w_p1_16bpp_p_line_);
    if (prg_rbg_3_2w_p2_16bpp_p_line_)
      device.destroyPipeline(prg_rbg_3_2w_p2_16bpp_p_line_);
    if (prg_rbg_3_2w_p1_16bpp_line_)
      device.destroyPipeline(prg_rbg_3_2w_p1_16bpp_line_);
    if (prg_rbg_3_2w_p2_16bpp_line_)
      device.destroyPipeline(prg_rbg_3_2w_p2_16bpp_line_);
    if (prg_rbg_3_2w_p1_32bpp_line_)
      device.destroyPipeline(prg_rbg_3_2w_p1_32bpp_line_);
    if (prg_rbg_3_2w_p2_32bpp_line_)
      device.destroyPipeline(prg_rbg_3_2w_p2_32bpp_line_);
  }
}

int RBGGeneratorVulkan::init(VIDVulkan *vulkan, int width, int height)
{

  this->vulkan = vulkan;
  VkDevice device = vulkan->getDevice();
  vk::Device d(device);
  resize(width, height);
  if (queue)
    return 0;

  int length = sizeof(prg_generate_rbg_base) + 64;
  snprintf(prg_generate_rbg, length, prg_generate_rbg_base, 16, 16);

  // queue = vk::Queue(vulkan->getVulkanQueue()); //d.getQueue(vulkan->getVulkanComputeQueueFamilyIndex(), 0);
  // queue = d.getQueue(vulkan->getVulkanComputeQueueFamilyIndex(), vulkan->getVulkanComputeQueue());

  queue = vk::Queue(vulkan->getVulkanComputeQueue());

  for (int i = 0; i < MAX_RBG_RENDER; i++)
  {
    semaphores[i].ready = d.createSemaphore({});
    semaphores[i].complete = d.createSemaphore({});
    commandfence[i] = d.createFence(vk::FenceCreateInfo());
    commandfence[i] = d.createFence(vk::FenceCreateInfo());
  }

  // vk::SubmitInfo computeSubmitInfo;
  // computeSubmitInfo.signalSemaphoreCount = 1;
  // computeSubmitInfo.pSignalSemaphores = &semaphores.complete;
  // queue.submit(computeSubmitInfo, {});

  // commandPool = d.createCommandPool({ vk::CommandPoolCreateFlagBits::eResetCommandBuffer, vulkan->getVulkanGraphicsQueueFamilyIndex() /*vulkan->getVulkanComputeQueueFamilyIndex()*/ });
  commandPool = d.createCommandPool({vk::CommandPoolCreateFlagBits::eResetCommandBuffer, vulkan->getVulkanComputeQueueFamilyIndex()});

  for (int i = 0; i < MAX_RBG_RENDER; i++)
  {
    command[i] = d.allocateCommandBuffers({commandPool, vk::CommandBufferLevel::ePrimary, 1})[0];
  }

  // Create sampler
  vk::SamplerCreateInfo sampler;
  sampler.magFilter = vk::Filter::eLinear;
  sampler.minFilter = vk::Filter::eLinear;
  sampler.mipmapMode = vk::SamplerMipmapMode::eNearest;
  sampler.addressModeU = vk::SamplerAddressMode::eClampToBorder;
  sampler.addressModeV = sampler.addressModeU;
  sampler.addressModeW = sampler.addressModeU;
  sampler.mipLodBias = 0.0f;
  sampler.maxAnisotropy = 1.0f;
  sampler.compareOp = vk::CompareOp::eNever;
  sampler.minLod = 0.0f;
  sampler.maxLod = 1.0f;
  sampler.borderColor = vk::BorderColor::eFloatOpaqueWhite;
  this->sampler = d.createSampler(sampler);

  int size = sizeof(RBGUniform);
  auto allocatedSize = getAllainedSize(size);

  VkBuffer u;
  VkDeviceMemory m;
  for (int i = 0; i < MAX_RBG_RENDER; i++)
  {
    vulkan->createBuffer(allocatedSize,
                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, u, m);
    rbgUniform[i].buf = vk::Buffer(u);
    rbgUniform[i].mem = vk::DeviceMemory(m);
  }

  allocatedSize = getAllainedSize(0x80000);
  vulkan->createBuffer(allocatedSize,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, u, m);
  ssbo_vram_.buf = vk::Buffer(u);
  ssbo_vram_.mem = vk::DeviceMemory(m);

  struct_size_ = sizeof(vdp2rotationparameter_struct);

  allocatedSize = getAllainedSize(struct_size_) * 2;
  vulkan->createBuffer(allocatedSize,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, u, m);
  ssbo_paraA_.buf = vk::Buffer(u);
  ssbo_paraA_.mem = vk::DeviceMemory(m);

  allocatedSize = getAllainedSize(sizeof(vdp2WindowInfo) * 512);
  vulkan->createBuffer(allocatedSize,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, u, m);
  ssbo_window_.buf = vk::Buffer(u);
  ssbo_window_.mem = vk::DeviceMemory(m);

  allocatedSize = getAllainedSize(0x1000);
  vulkan->createBuffer(allocatedSize,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, u, m);
  ssbo_cram_.buf = vk::Buffer(u);
  ssbo_cram_.mem = vk::DeviceMemory(m);

  std::vector<vk::DescriptorPoolSize> poolSizes = {
      vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, MAX_RBG_RENDER},
      vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, MAX_RBG_RENDER},
      vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, MAX_RBG_RENDER},
      vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, MAX_RBG_RENDER},
      vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, MAX_RBG_RENDER},
      vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, MAX_RBG_RENDER},
  };

  descriptorPool = d.createDescriptorPool(vk::DescriptorPoolCreateInfo{{}, 6, (uint32_t)poolSizes.size(), poolSizes.data()});
  std::vector<vk::DescriptorSetLayoutBinding> setLayoutBindings = {
      vk::DescriptorSetLayoutBinding{0, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{3, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{4, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
      vk::DescriptorSetLayoutBinding{5, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute}};

  descriptorSetLayout = d.createDescriptorSetLayout({{}, (uint32_t)setLayoutBindings.size(), setLayoutBindings.data()});
  for (int i = 0; i < MAX_RBG_RENDER; i++)
  {
    descriptorSet[i] = d.allocateDescriptorSets({descriptorPool, 1, &descriptorSetLayout})[0];
  }
  updateDescriptorSets(0);

  std::string target;
  /*
    target ="#version 320 es \n"
       "precision highp float;\n"
       "precision highp int;\n"
       "precision highp image2D;\n"
       "layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;\n"
       "layout(rgba8, binding = 0)  uniform writeonly image2D outSurface;\n"
       "layout(std430, binding = 1) readonly buffer VDP2 { uint vram[]; };\n"
       "layout(std430, binding = 2) readonly buffer VDP2C { uint cram[]; };\n"
       "void main() {\n"
       "  ivec2 texel = ivec2(gl_GlobalInvocationID.xy);\n"
       "  uint kdata = cram[texel.x];\n"
       "  float ky = 0.0;\n"
       "  kdata = ((kdata>>8)&0xFFu) | ((kdata << 8) & 0xFFFFFF00u);\n"
       "  if((kdata&0x4000u)!=0u) { ky=float( int(kdata&0x7FFFu)| int(0xFFFF8000u) )/1024.0; }else{ ky=float(kdata&0x7FFFu)/1024.0;}\n"
       "  kdata = ((kdata&0xFFFF0000u)>>16|(kdata&0x0000FFFFu)<<16);\n"
       "	if((kdata&0x00800000u)!=0u) ky=float( int(kdata&0x00FFFFFFu)| int(0xFF800000u) )/65536.0; else ky=float(kdata&0x00FFFFFFu)/65536.0;\n"
  //   "  kdata = (((kdata>>8)&0xFFu) | (kdata<<8) ) & 0xFFFFu ;\n"
       "  if ((kdata&0x8000u)!=0x0u) { return; }\n"
       "  imageStore(outSurface,texel,vec4(ky));\n"
       "}";
  */
  for (int i = 0; i < sizeof(a_prg_rbg_0_2w_bitmap) / sizeof(char *); i++)
  {
    target += a_prg_rbg_0_2w_bitmap[i];
  }
  // for (int i = 0; i < sizeof(a_prg_rbg_0_2w_p2_4bpp)/sizeof(char*); i++) {
  //   target += a_prg_rbg_0_2w_p2_4bpp[i];
  // }

  Compiler compiler;
  CompileOptions options;
  options.SetOptimizationLevel(shaderc_optimization_level_performance);
  // options.SetTargetEnvironment(shaderc_target_env_vulkan,shaderc_env_version_vulkan_1_2);
  // options.SetTargetSpirv(shaderc_spirv_version_1_5);
  // options.SetGenerateDebugInfo();
  SpvCompilationResult result = compiler.CompileGlslToSpv(
      target,
      shaderc_compute_shader,
      "a_prg_rbg_0_2w_bitmap",
      options);

  printf("%s%d\n", " erros: ", (int)result.GetNumErrors());
  if (result.GetNumErrors() != 0)
  {
    char xerr[256];
    sprintf(xerr, "%s%s\n", "messages", result.GetErrorMessage().c_str());
    printf("%s", xerr);
    LOGE("failed to create shader module!");
    return -1;
  }
  std::vector<uint32_t> data = {result.cbegin(), result.cend()};

  // std::ofstream file("/storage/emulated/0/Android/data/org.devmiyax.yabasanshioro2.debug/files/bad.spv", std::ios::binary);
  // if (file.is_open()) {
  //    file.write(reinterpret_cast<const char*>(data.data()), data.size() * sizeof(uint32_t));
  //    file.close();
  // }

  VkShaderModuleCreateInfo createInfo = {};
  createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  createInfo.codeSize = data.size() * sizeof(uint32_t);
  createInfo.pCode = data.data();
  VkShaderModule shaderModule;
  if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS)
  {
    // throw std::runtime_error("failed to create shader module!");
    LOGE("failed to create shader module!");
    return -1;
  }

  pipelineLayout = d.createPipelineLayout({{}, 1, &descriptorSetLayout});

  try
  {
    vk::ComputePipelineCreateInfo computePipelineCreateInfo;
    computePipelineCreateInfo.layout = pipelineLayout;
    computePipelineCreateInfo.stage.module = vk::ShaderModule(shaderModule);
    computePipelineCreateInfo.stage.stage = vk::ShaderStageFlagBits::eCompute;
    computePipelineCreateInfo.stage.pName = "main";
    auto a = d.createComputePipelines(nullptr, computePipelineCreateInfo);
    pipeline = a.value[0];

    d.destroyShaderModule(vk::ShaderModule(shaderModule));
  }
  catch (vk::SystemError &err)
  {
    LOGE("vk::SystemError: %s", err.what());
    return -1;
  }
  catch (std::exception &err)
  {
    LOGE("std::exception: %s", err.what());
    return -1;
  }
  catch (...)
  {
    LOGE("unknown error\n");
    return -1;
  }
  return 0;
}

void RBGGeneratorVulkan::updateDescriptorSets(int texindex)
{

  VkDevice device = vulkan->getDevice();
  vk::Device d(device);

  if (descriptorSet[0] == (vk::DescriptorSet) nullptr)
    return;

  vk::DescriptorBufferInfo descriptor;
  descriptor.buffer = rbgUniform[currentIndex].buf;
  descriptor.range = VK_WHOLE_SIZE;
  descriptor.offset = 0;

  vk::DescriptorImageInfo texDescriptor{
      this->sampler, tex_surface[texindex].view, vk::ImageLayout::eGeneral};

  vk::DescriptorBufferInfo descriptor2;
  descriptor2.buffer = ssbo_vram_.buf;
  descriptor2.range = VK_WHOLE_SIZE;
  descriptor2.offset = 0;

  vk::DescriptorBufferInfo descriptor3;
  descriptor3.buffer = ssbo_paraA_.buf;
  descriptor3.range = VK_WHOLE_SIZE;
  descriptor3.offset = 0;

  vk::DescriptorBufferInfo descriptor4;
  descriptor4.buffer = ssbo_window_.buf;
  descriptor4.range = VK_WHOLE_SIZE;
  descriptor4.offset = 0;

  vk::DescriptorBufferInfo descriptor5;
  descriptor5.buffer = ssbo_cram_.buf;
  descriptor5.range = VK_WHOLE_SIZE;
  descriptor5.offset = 0;

  std::vector<vk::WriteDescriptorSet> computeWriteDescriptorSets{
      {descriptorSet[currentIndex], 0, 0, 1, vk::DescriptorType::eStorageImage, &texDescriptor},
      {descriptorSet[currentIndex], 1, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &descriptor2},
      {descriptorSet[currentIndex], 2, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &descriptor3},
      {descriptorSet[currentIndex], 3, 0, 1, vk::DescriptorType::eUniformBuffer, nullptr, &descriptor},
      {descriptorSet[currentIndex], 4, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &descriptor4},
      {descriptorSet[currentIndex], 5, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &descriptor5},
  };

  d.updateDescriptorSets(computeWriteDescriptorSets, {});
}

int RBGGeneratorVulkan::getAllainedSize(int size)
{

  VkDevice device = vulkan->getDevice();
  vk::Device d(device);
  auto p = vk::PhysicalDevice(vulkan->getPhysicalDevice());
  auto alignment = p.getProperties().limits.minUniformBufferOffsetAlignment;
  auto extra = size % alignment;
  auto count = 1;
  auto alignedSize = size + (alignment - extra);
  auto allocatedSize = count * alignedSize;

  return allocatedSize;
}

void RBGGeneratorVulkan::resize(int width, int height)
{
  if (tex_width_ == width && tex_height_ == height)
    return;

  VkDevice device = vulkan->getDevice();
  vk::Device d(device);

  YGLDEBUG("resize %d, %d\n", width, height);

  tex_width_ = width;
  tex_height_ = height;

  if (tex_surface[0].image)
    d.destroyImage(tex_surface[0].image);
  if (tex_surface[0].view)
    d.destroyImageView(tex_surface[0].view);
  if (tex_surface[0].mem)
    d.freeMemory(tex_surface[0].mem);
  if (tex_surface[1].image)
    d.destroyImage(tex_surface[1].image);
  if (tex_surface[1].view)
    d.destroyImageView(tex_surface[1].view);
  if (tex_surface[1].mem)
    d.freeMemory(tex_surface[1].mem);

  vk::ImageCreateInfo imageCreateInfo;
  imageCreateInfo.imageType = vk::ImageType::e2D;
  imageCreateInfo.format = vk::Format::eR8G8B8A8Unorm;
  imageCreateInfo.extent.width = width;
  imageCreateInfo.extent.height = height;
  imageCreateInfo.extent.depth = 1;
  imageCreateInfo.mipLevels = 1;
  imageCreateInfo.arrayLayers = 1;
  imageCreateInfo.tiling = vk::ImageTiling::eOptimal;
  imageCreateInfo.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage;
  // imageCreateInfo.sharingMode = vk::SharingMode::eConcurrent;
  // imageCreateInfo.queueFamilyIndexCount = 2;
  // uint32_t indexes[] = { vulkan->getVulkanGraphicsQueueFamilyIndex(),vulkan->getVulkanComputeQueueFamilyIndex() };
  // imageCreateInfo.pQueueFamilyIndices = indexes;
  imageCreateInfo.sharingMode = vk::SharingMode::eExclusive; // VK_SHARING_MODE_EXCLUSIVE;
  imageCreateInfo.pQueueFamilyIndices = nullptr;

  for (int i = 0; i < 2; i++)
  {
    VK_CHECK_RESULT(VkResult(d.createImage(&imageCreateInfo, nullptr, &tex_surface[i].image)));

    vk::MemoryRequirements memReqs = d.getImageMemoryRequirements(tex_surface[i].image);
    vk::MemoryAllocateInfo memAllocInfo;

    memAllocInfo.allocationSize = memReqs.size;
    memAllocInfo.memoryTypeIndex = vulkan->findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
    tex_surface[i].mem = d.allocateMemory(memAllocInfo);
    d.bindImageMemory(tex_surface[i].image, tex_surface[i].mem, 0);

    vk::ImageViewCreateInfo view;
    view.viewType = vk::ImageViewType::e2D;
    view.format = vk::Format::eR8G8B8A8Unorm;
    view.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    view.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    view.image = tex_surface[i].image;
    tex_surface[i].view = d.createImageView(view);

    vulkan->transitionImageLayout(VkImage(tex_surface[i].image), VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    vulkan->transitionImageLayout(VkImage(tex_surface[i].image), VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }

  updateDescriptorSets(0);
}

#include <functional>
#include <fstream>
#include <iostream>

vk::Pipeline RBGGeneratorVulkan::compile_color_dot(
    const char *base[], int size, const char *color, const char *dot)
{

  VkDevice device = vulkan->getDevice();
  vk::Device d(device);

  vk::Pipeline rtn = nullptr;

  std::string target;
  for (int i = 0; i < size - 2; i++)
  {
    target += base[i];
  }

  target += color;
  target += dot;

  std::vector<uint32_t> data;
  std::vector<char> buffer;
  SpvCompilationResult result;
  // #if !defined(_WINDOWS)
  std::size_t hash_value = std::hash<std::string>()(target);
  // Serach from file
  string mempath = YuiGetShaderCachePath();
  std::string hashval = std::to_string(hash_value);
  string file_path = mempath + hashval + ".spv";

  // read the binary file
  std::ifstream file(file_path, std::ios::binary);
  if (file)
  {

    // get the file size
    file.seekg(0, std::ios::end);
    std::size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    // read the file contents
    data.resize(file_size / sizeof(uint32_t));
    file.read(reinterpret_cast<char *>(data.data()), file_size);
    file.close();
  }
  else
  {
    // #endif
    Compiler compiler;
    CompileOptions options;
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
    // options.SetOptimizationLevel(shaderc_optimization_level_zero);
    result = compiler.CompileGlslToSpv(
        target,
        shaderc_compute_shader,
        "RBG",
        options);

    printf("%s%d\n", " erros: ", (int)result.GetNumErrors());
    if (result.GetNumErrors() != 0)
    {
      printf("%s", target.c_str());
      printf("%s%s\n", "messages", result.GetErrorMessage().c_str());
      throw std::runtime_error("failed to create shader module!");
    }
    data = {result.cbegin(), result.cend()};
    // #if !defined(_WINDOWS)
    std::ofstream file(file_path, std::ios::binary);
    if (!file)
    {
      std::cerr << "Error: Failed to open file." << std::endl;
      throw std::runtime_error("failed to create shader module!");
    }

    // write the data
    file.write(reinterpret_cast<char*>(data.data()), data.size() * sizeof(uint32_t));

    // close the file
    file.close();
  }
  // #endif

  VkShaderModuleCreateInfo createInfo = {};
  createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  createInfo.codeSize = data.size() * sizeof(uint32_t);
  createInfo.pCode = data.data();
  VkShaderModule shaderModule;
  if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS)
  {
    throw std::runtime_error("failed to create shader module!");
  }

  pipelineLayout = d.createPipelineLayout({{}, 1, &descriptorSetLayout});

  try
  {
    vk::ComputePipelineCreateInfo computePipelineCreateInfo;
    computePipelineCreateInfo.layout = pipelineLayout;
    computePipelineCreateInfo.stage.module = vk::ShaderModule(shaderModule);
    computePipelineCreateInfo.stage.stage = vk::ShaderStageFlagBits::eCompute;
    computePipelineCreateInfo.stage.pName = "main";
    auto a = d.createComputePipelines(nullptr, computePipelineCreateInfo);
    rtn = a.value[0];
    d.destroyShaderModule(vk::ShaderModule(shaderModule));
  }
  catch (vk::SystemError &err)
  {
    LOGE("vk::SystemError: %s", err.what());
  }
  catch (std::exception &err)
  {
    LOGE("std::exception: %s", err.what());
  }
  catch (...)
  {
    LOGE("unknown error\n");
  }
  return rtn;
}

void RBGGeneratorVulkan::update(VIDVulkan::RBGDrawInfo *rbg, const vdp2rotationparameter_struct &paraa, const vdp2rotationparameter_struct &parab)
{

  VkDevice device = vulkan->getDevice();
  vk::Device d(device);

  vk::Pipeline CurrentPipeline = nullptr;

  int texindex = 0;
  if (rbg->rgb_type != 0)
  {
    texindex = 1;
  }

  // Line color insersion
  // The "_line_" variants pack a CRAM index into R/G and the line colour into B,
  // so they only work for palette colour modes.  RGB direct modes (colornumber
  // 3 = 32K, 4 = 16.7M) put a 24-bit colour in cramindex; running them through the
  // line path drops the blue byte and forces bit 15 on, which turned every direct
  // colour dot green.  Fall back to the plain pipeline for those.
  if (rbg->info.LineColorBase != 0 && VDP2_CC_NONE != (rbg->info.blendmode & 0x03)
      && rbg->info.colornumber < 3) {
    if (fixVdp2Regs->RPMD == 0 || (fixVdp2Regs->RPMD == 3 && (fixVdp2Regs->WCTLD & 0xA) == 0)) {
      if (rbg->info.isbitmap) {
        if (!prg_rbg_0_2w_bitmap_8bpp_line_) {
          prg_rbg_0_2w_bitmap_8bpp_line_ = compile_color_dot(
              S(a_prg_rbg_0_2w_bitmap),
              prg_rbg_getcolor_8bpp,
              prg_generate_rbg_line_end);
        }
        CurrentPipeline = prg_rbg_0_2w_bitmap_8bpp_line_;
      }
      else
      {
        if (rbg->info.patterndatasize == 1)
        {
          switch (rbg->info.colornumber)
          {
          case 0:
          { // Decathalete ToDo: Line Color Bug
            if (!prg_rbg_0_2w_p1_4bpp_line_)
            {
              prg_rbg_0_2w_p1_4bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_p1_4bpp),
                  prg_rbg_getcolor_4bpp,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_0_2w_p1_4bpp_line_;
            break;
          }
          case 1:
          { // Sakatuku 2 Ground, GUNDAM Side Story 2, SonicR ToDo: 2Player
            if (!prg_rbg_0_2w_p1_8bpp_line_)
            {
              prg_rbg_0_2w_p1_8bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_p1_8bpp),
                  prg_rbg_getcolor_8bpp,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_0_2w_p1_8bpp_line_;
            break;
          }
          case 2:
          {
            if (!prg_rbg_0_2w_p1_16bpp_p_line_)
            {
              prg_rbg_0_2w_p1_16bpp_p_line_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_p1_4bpp),
                  prg_rbg_getcolor_16bpp_palette,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_0_2w_p1_16bpp_p_line_;
            break;
          }
          case 3:
          {
            if (!prg_rbg_0_2w_p1_16bpp_line_)
            {
              prg_rbg_0_2w_p1_16bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_p1_4bpp),
                  prg_rbg_getcolor_16bpp_rbg,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_0_2w_p1_16bpp_line_;
            break;
          }
          case 4:
          {
            if (!prg_rbg_0_2w_p1_32bpp_line_)
            {
              prg_rbg_0_2w_p1_32bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_p1_4bpp),
                  prg_rbg_getcolor_32bpp_rbg,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_0_2w_p1_32bpp_line_;
            break;
          }
          }
        }
        else
        {
          switch (rbg->info.colornumber)
          {
          case 0:
          {
            if (!prg_rbg_0_2w_p2_4bpp_line_)
            {
              prg_rbg_0_2w_p2_4bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_p2_4bpp),
                  prg_rbg_getcolor_4bpp,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_0_2w_p2_4bpp_line_;
            break;
          }
          case 1:
          { // Thunder Force V
            if (!prg_rbg_0_2w_p2_8bpp_line_)
            {
              prg_rbg_0_2w_p2_8bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_p2_8bpp),
                  prg_rbg_getcolor_8bpp,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_0_2w_p2_8bpp_line_;
            break;
          }
          case 2:
          {
            if (!prg_rbg_0_2w_p2_16bpp_p_line_)
            {
              prg_rbg_0_2w_p2_16bpp_p_line_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_p2_4bpp),
                  prg_rbg_getcolor_16bpp_palette,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_0_2w_p2_16bpp_p_line_;
            break;
          }
          case 3:
          {
            if (!prg_rbg_0_2w_p2_16bpp_line_)
            {
              prg_rbg_0_2w_p2_16bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_p2_4bpp),
                  prg_rbg_getcolor_16bpp_rbg,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_0_2w_p2_16bpp_line_;
            break;
          }
          case 4:
          {
            if (!prg_rbg_0_2w_p2_32bpp_line_)
            {
              prg_rbg_0_2w_p2_32bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_p2_4bpp),
                  prg_rbg_getcolor_32bpp_rbg,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_0_2w_p2_32bpp_line_;
            break;
          }
          }
        }
      }
    }
    else if (fixVdp2Regs->RPMD == 1)
    {
      if (rbg->info.isbitmap)
      {
        if (!prg_rbg_1_2w_bitmap_8bpp_line_)
        {
          prg_rbg_1_2w_bitmap_8bpp_line_ = compile_color_dot(
              S(a_prg_rbg_1_2w_bitmap),
              prg_rbg_getcolor_8bpp,
              prg_generate_rbg_line_end);
        }
        CurrentPipeline = prg_rbg_1_2w_bitmap_8bpp_line_;
      }
      else
      {
        if (rbg->info.patterndatasize == 1)
        {
          switch (rbg->info.colornumber)
          {
          case 0:
          {
            if (!prg_rbg_1_2w_p1_4bpp_line_)
            {
              prg_rbg_1_2w_p1_4bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_1_2w_p1_4bpp),
                  prg_rbg_getcolor_4bpp,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_1_2w_p1_4bpp_line_;
            break;
          }
          case 1:
          {
            if (!prg_rbg_1_2w_p1_8bpp_line_)
            {
              prg_rbg_1_2w_p1_8bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_1_2w_p1_8bpp),
                  prg_rbg_getcolor_8bpp,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_1_2w_p1_8bpp_line_;
            break;
          }
          case 2:
          {
            if (!prg_rbg_1_2w_p1_16bpp_p_line_)
            {
              prg_rbg_1_2w_p1_16bpp_p_line_ = compile_color_dot(
                  S(a_prg_rbg_1_2w_p1_4bpp),
                  prg_rbg_getcolor_16bpp_palette,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_1_2w_p1_16bpp_p_line_;
            break;
          }
          case 3:
          {
            if (!prg_rbg_1_2w_p1_16bpp_line_)
            {
              prg_rbg_1_2w_p1_16bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_1_2w_p1_4bpp),
                  prg_rbg_getcolor_16bpp_rbg,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_1_2w_p1_16bpp_line_;
            break;
          }
          case 4:
          {
            if (!prg_rbg_1_2w_p1_32bpp_line_)
            {
              prg_rbg_1_2w_p1_32bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_p1_4bpp),
                  prg_rbg_getcolor_32bpp_rbg,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_1_2w_p1_32bpp_line_;
            break;
          }
          }
        }
        else
        {
          switch (rbg->info.colornumber)
          {
          case 0:
          {
            if (!prg_rbg_1_2w_p2_4bpp_line_)
            {
              prg_rbg_1_2w_p2_4bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_1_2w_p2_4bpp),
                  prg_rbg_getcolor_4bpp,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_1_2w_p2_4bpp_line_;
            break;
          }
          case 1:
          {
            if (!prg_rbg_1_2w_p2_8bpp_line_)
            {
              prg_rbg_1_2w_p2_8bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_1_2w_p2_8bpp),
                  prg_rbg_getcolor_8bpp,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_1_2w_p2_8bpp_line_;
            break;
          }
          case 2:
          {
            if (!prg_rbg_1_2w_p2_16bpp_p_line_)
            {
              prg_rbg_1_2w_p2_16bpp_p_line_ = compile_color_dot(
                  S(a_prg_rbg_1_2w_p2_4bpp),
                  prg_rbg_getcolor_16bpp_palette,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_1_2w_p2_16bpp_p_line_;
            break;
          }
          case 3:
          {
            if (!prg_rbg_1_2w_p2_16bpp_line_)
            {
              prg_rbg_1_2w_p2_16bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_1_2w_p2_4bpp),
                  prg_rbg_getcolor_16bpp_rbg,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_1_2w_p2_16bpp_line_;
            break;
          }
          case 4:
          {
            if (!prg_rbg_1_2w_p2_32bpp_line_)
            {
              prg_rbg_1_2w_p2_32bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_1_2w_p2_4bpp),
                  prg_rbg_getcolor_32bpp_rbg,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_1_2w_p2_32bpp_line_;
            break;
          }
          }
        }
      }
    }
    else if (fixVdp2Regs->RPMD == 2)
    {
      if (rbg->info.isbitmap)
      {
        if (!prg_rbg_2_2w_bitmap_8bpp_line_)
        {
          prg_rbg_2_2w_bitmap_8bpp_line_ = compile_color_dot(
              S(a_prg_rbg_2_2w_bitmap),
              prg_rbg_getcolor_8bpp,
              prg_generate_rbg_line_end);
        }
        CurrentPipeline = prg_rbg_2_2w_bitmap_8bpp_line_;
      }
      else
      {
        if (rbg->info.patterndatasize == 1)
        {
          switch (rbg->info.colornumber)
          {
          case 0:
          {
            if (!prg_rbg_2_2w_p1_4bpp_line_)
            {
              prg_rbg_2_2w_p1_4bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_2_2w_p1_4bpp),
                  prg_rbg_getcolor_4bpp,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_2_2w_p1_4bpp_line_;
            break;
          }
          case 1:
          { // Panzer Dragoon 1
            if (!prg_rbg_2_2w_p1_8bpp_line_)
            {
              prg_rbg_2_2w_p1_8bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_2_2w_p1_8bpp),
                  prg_rbg_getcolor_8bpp,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_2_2w_p1_8bpp_line_;
            break;
          }
          case 2:
          {
            if (!prg_rbg_2_2w_p1_16bpp_p_line_)
            {
              prg_rbg_2_2w_p1_16bpp_p_line_ = compile_color_dot(
                  S(a_prg_rbg_2_2w_p1_4bpp),
                  prg_rbg_getcolor_16bpp_palette,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_2_2w_p1_16bpp_p_line_;
            break;
          }
          case 3:
          {
            if (!prg_rbg_2_2w_p1_16bpp_line_)
            {
              prg_rbg_2_2w_p1_16bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_2_2w_p1_4bpp),
                  prg_rbg_getcolor_16bpp_rbg,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_2_2w_p1_16bpp_line_;
            break;
          }
          case 4:
          {
            if (!prg_rbg_2_2w_p1_32bpp_line_)
            {
              prg_rbg_2_2w_p1_32bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_2_2w_p1_4bpp),
                  prg_rbg_getcolor_32bpp_rbg,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_2_2w_p1_32bpp_line_;
            break;
          }
          }
        }
        else
        {
          switch (rbg->info.colornumber)
          {
          case 0:
          {
            if (!prg_rbg_2_2w_p2_4bpp_line_)
            {
              prg_rbg_2_2w_p2_4bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_2_2w_p2_4bpp),
                  prg_rbg_getcolor_4bpp,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_2_2w_p2_4bpp_line_;
            break;
          }
          case 1:
          {
            if (!prg_rbg_2_2w_p2_8bpp_line_)
            {
              prg_rbg_2_2w_p2_8bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_2_2w_p2_8bpp),
                  prg_rbg_getcolor_8bpp,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_2_2w_p2_8bpp_line_;
            break;
          }
          case 2:
          {
            if (!prg_rbg_2_2w_p2_16bpp_p_line_)
            {
              prg_rbg_2_2w_p2_16bpp_p_line_ = compile_color_dot(
                  S(a_prg_rbg_2_2w_p2_4bpp),
                  prg_rbg_getcolor_16bpp_palette,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_2_2w_p2_16bpp_p_line_;
            break;
          }
          case 3:
          {
            if (!prg_rbg_2_2w_p2_16bpp_line_)
            {
              prg_rbg_2_2w_p2_16bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_2_2w_p2_4bpp),
                  prg_rbg_getcolor_16bpp_rbg,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_2_2w_p2_16bpp_line_;
            break;
          }
          case 4:
          {
            if (!prg_rbg_2_2w_p2_32bpp_line_)
            {
              prg_rbg_2_2w_p2_32bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_2_2w_p2_4bpp),
                  prg_rbg_getcolor_32bpp_rbg,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_2_2w_p2_32bpp_line_;
            break;
          }
          }
        }
      }
    }
    else if (fixVdp2Regs->RPMD == 3)
    {

      if (rbg->info.isbitmap)
      {
        if (!prg_rbg_3_2w_bitmap_8bpp_line_)
        {
          prg_rbg_3_2w_bitmap_8bpp_line_ = compile_color_dot(
              S(a_prg_rbg_3_2w_bitmap),
              prg_rbg_getcolor_8bpp,
              prg_generate_rbg_line_end);
        }
        CurrentPipeline = prg_rbg_3_2w_bitmap_8bpp_line_;
      }
      else
      {
        if (rbg->info.patterndatasize == 1)
        {
          switch (rbg->info.colornumber)
          {
          case 0:
          {
            if (!prg_rbg_3_2w_p1_4bpp_line_)
            {
              prg_rbg_3_2w_p1_4bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_3_2w_p1_4bpp),
                  prg_rbg_getcolor_4bpp,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_3_2w_p1_4bpp_line_;
            break;
          }
          case 1:
          {
            if (!prg_rbg_3_2w_p1_8bpp_line_)
            {
              prg_rbg_3_2w_p1_8bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_3_2w_p1_8bpp),
                  prg_rbg_getcolor_8bpp,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_3_2w_p1_8bpp_line_;
            break;
          }
          case 2:
          {
            if (!prg_rbg_3_2w_p1_16bpp_p_line_)
            {
              prg_rbg_3_2w_p1_16bpp_p_line_ = compile_color_dot(
                  S(a_prg_rbg_3_2w_p1_4bpp),
                  prg_rbg_getcolor_16bpp_palette,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_3_2w_p1_16bpp_p_line_;
            break;
          }
          case 3:
          {
            if (!prg_rbg_3_2w_p1_16bpp_line_)
            {
              prg_rbg_3_2w_p1_16bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_3_2w_p1_4bpp),
                  prg_rbg_getcolor_16bpp_rbg,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_3_2w_p1_16bpp_line_;
            break;
          }
          case 4:
          {
            if (!prg_rbg_3_2w_p1_32bpp_line_)
            {
              prg_rbg_3_2w_p1_32bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_3_2w_p1_4bpp),
                  prg_rbg_getcolor_32bpp_rbg,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_3_2w_p1_32bpp_line_;
            break;
          }
          }
        }
        else
        {
          switch (rbg->info.colornumber)
          {
          case 0:
          {
            if (!prg_rbg_3_2w_p2_4bpp_line_)
            {
              prg_rbg_3_2w_p2_4bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_3_2w_p2_4bpp),
                  prg_rbg_getcolor_4bpp,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_3_2w_p2_4bpp_line_;
            break;
          }
          case 1:
          {
            if (!prg_rbg_3_2w_p2_8bpp_line_)
            {
              prg_rbg_3_2w_p2_8bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_3_2w_p2_8bpp),
                  prg_rbg_getcolor_8bpp,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_3_2w_p2_8bpp_line_;
            break;
          }
          case 2:
          {
            if (!prg_rbg_3_2w_p2_16bpp_p_line_)
            {
              prg_rbg_3_2w_p2_16bpp_p_line_ = compile_color_dot(
                  S(a_prg_rbg_3_2w_p2_4bpp),
                  prg_rbg_getcolor_16bpp_palette,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_3_2w_p2_16bpp_p_line_;
            break;
          }
          case 3:
          {
            if (!prg_rbg_3_2w_p2_16bpp_line_)
            {
              prg_rbg_3_2w_p2_16bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_3_2w_p2_4bpp),
                  prg_rbg_getcolor_16bpp_rbg,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_3_2w_p2_16bpp_line_;
            break;
          }
          case 4:
          {
            if (!prg_rbg_3_2w_p2_32bpp_line_)
            {
              prg_rbg_3_2w_p2_32bpp_line_ = compile_color_dot(
                  S(a_prg_rbg_3_2w_p2_4bpp),
                  prg_rbg_getcolor_32bpp_rbg,
                  prg_generate_rbg_line_end);
            }
            CurrentPipeline = prg_rbg_3_2w_p2_32bpp_line_;
            break;
          }
          }
        }
      }
      // ToDo: Copy Window Info
      // glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_window_);
      // glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(vdp2WindowInfo)*(int(rbg->vres / rbg->rotate_mval_v) << rbg->info.hres_shift), (void*)rbg->info.pWinInfo);
      // glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, ssbo_window_);
    }
  }

  // no line color insersion
  else
  {
    if (rbg->rgb_type == 0 && (fixVdp2Regs->RPMD == 0 || (fixVdp2Regs->RPMD == 3 && (fixVdp2Regs->WCTLD & 0xA) == 0)))
    {
      if (rbg->info.isbitmap)
      {
        switch (rbg->info.colornumber)
        {
        case 0:
        {
          if (!prg_rbg_0_2w_bitmap_4bpp_)
          {
            prg_rbg_0_2w_bitmap_4bpp_ = compile_color_dot(
                S(a_prg_rbg_0_2w_bitmap),
                prg_rbg_getcolor_4bpp,
                prg_generate_rbg_end);
          }
          CurrentPipeline = prg_rbg_0_2w_bitmap_4bpp_;
          break;
        }
        case 1:
        { // SF3S1( Initial )
          if (!prg_rbg_0_2w_bitmap_8bpp_)
          {
            prg_rbg_0_2w_bitmap_8bpp_ = compile_color_dot(
                S(a_prg_rbg_0_2w_bitmap),
                prg_rbg_getcolor_8bpp,
                prg_generate_rbg_end);
          }
          CurrentPipeline = prg_rbg_0_2w_bitmap_8bpp_;
          break;
        }
        case 2:
        {
          if (!prg_rbg_0_2w_bitmap_16bpp_p_)
          {
            prg_rbg_0_2w_bitmap_16bpp_p_ = compile_color_dot(
                S(a_prg_rbg_0_2w_bitmap),
                prg_rbg_getcolor_16bpp_palette,
                prg_generate_rbg_end);
          }
          CurrentPipeline = prg_rbg_0_2w_bitmap_16bpp_p_;
          break;
        }
        case 3:
        { // NHL 97 Title, GRANDIA Title
          if (!prg_rbg_0_2w_bitmap_16bpp_)
          {
            prg_rbg_0_2w_bitmap_16bpp_ = compile_color_dot(
                S(a_prg_rbg_0_2w_bitmap),
                prg_rbg_getcolor_16bpp_rbg,
                prg_generate_rbg_end);
          }
          CurrentPipeline = prg_rbg_0_2w_bitmap_16bpp_;
          break;
        }
        case 4:
        {
          if (!prg_rbg_0_2w_bitmap_32bpp_)
          {
            prg_rbg_0_2w_bitmap_32bpp_ = compile_color_dot(
                S(a_prg_rbg_0_2w_bitmap),
                prg_rbg_getcolor_32bpp_rbg,
                prg_generate_rbg_end);
          }
          CurrentPipeline = prg_rbg_0_2w_bitmap_32bpp_;
          break;
        }
        }
      }
      else
      {
        if (rbg->info.patterndatasize == 1)
        {
          switch (rbg->info.colornumber)
          {
          case 0:
          { // Dead or Alive, Rediant Silver Gun, Diehard
            if (!prg_rbg_0_2w_p1_4bpp_)
            {
              prg_rbg_0_2w_p1_4bpp_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_p1_4bpp),
                  prg_rbg_getcolor_4bpp,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_0_2w_p1_4bpp_;
            break;
          }
          case 1:
          { // Sakatuku 2 ( Initial Setting ), Virtua Fighter 2, Virtual-on
            if (!prg_rbg_0_2w_p1_8bpp_)
            {
              prg_rbg_0_2w_p1_8bpp_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_p1_8bpp),
                  prg_rbg_getcolor_8bpp,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_0_2w_p1_8bpp_;
            break;
          }
          case 2:
          {
            if (!prg_rbg_0_2w_p1_16bpp_p_)
            {
              prg_rbg_0_2w_p1_16bpp_p_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_p1_4bpp),
                  prg_rbg_getcolor_16bpp_palette,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_0_2w_p1_16bpp_p_;
            break;
          }
          case 3:
          {
            if (!prg_rbg_0_2w_p1_16bpp_)
            {
              prg_rbg_0_2w_p1_16bpp_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_p1_4bpp),
                  prg_rbg_getcolor_16bpp_rbg,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_0_2w_p1_16bpp_;
            break;
          }
          case 4:
          {
            if (!prg_rbg_0_2w_p1_32bpp_)
            {
              prg_rbg_0_2w_p1_32bpp_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_p1_4bpp),
                  prg_rbg_getcolor_32bpp_rbg,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_0_2w_p1_32bpp_;
            break;
          }
          }
        }
        else
        {
          switch (rbg->info.colornumber)
          {
          case 0:
          {
            if (!prg_rbg_0_2w_p2_4bpp_)
            {
              prg_rbg_0_2w_p2_4bpp_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_p2_4bpp),
                  prg_rbg_getcolor_4bpp,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_0_2w_p2_4bpp_;
            break;
          }
          case 1:
          { // NHL97(In Game), BIOS
            if (!prg_rbg_0_2w_p2_8bpp_)
            {
              prg_rbg_0_2w_p2_8bpp_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_p2_8bpp),
                  prg_rbg_getcolor_8bpp,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_0_2w_p2_8bpp_;
            break;
          }
          case 2:
          {
            if (!prg_rbg_0_2w_p2_16bpp_p_)
            {
              prg_rbg_0_2w_p2_16bpp_p_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_p2_4bpp),
                  prg_rbg_getcolor_16bpp_palette,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_0_2w_p2_16bpp_p_;
            break;
          }
          case 3:
          {
            if (!prg_rbg_0_2w_p2_16bpp_)
            {
              prg_rbg_0_2w_p2_16bpp_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_p2_4bpp),
                  prg_rbg_getcolor_16bpp_rbg,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_0_2w_p2_16bpp_;
            break;
          }
          case 4:
          {
            if (!prg_rbg_0_2w_p2_32bpp_)
            {
              prg_rbg_0_2w_p2_32bpp_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_p2_4bpp),
                  prg_rbg_getcolor_32bpp_rbg,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_0_2w_p2_32bpp_;
            break;
          }
          }
        }
      }
    }
    else if ((fixVdp2Regs->RPMD == 1 && rbg->rgb_type == 0) || rbg->rgb_type == 0x04)
    {
      if (rbg->info.isbitmap)
      {
        switch (rbg->info.colornumber)
        {
        case 0:
        {
          if (!prg_rbg_1_2w_bitmap_4bpp_)
          {
            prg_rbg_1_2w_bitmap_4bpp_ = compile_color_dot(
                S(a_prg_rbg_1_2w_bitmap),
                prg_rbg_getcolor_4bpp,
                prg_generate_rbg_end);
          }
          CurrentPipeline = prg_rbg_1_2w_bitmap_4bpp_;
          break;
        }
        case 1:
        {
          if (!prg_rbg_1_2w_bitmap_8bpp_)
          {
            prg_rbg_1_2w_bitmap_8bpp_ = compile_color_dot(
                S(a_prg_rbg_1_2w_bitmap),
                prg_rbg_getcolor_8bpp,
                prg_generate_rbg_end);
          }
          CurrentPipeline = prg_rbg_1_2w_bitmap_8bpp_;
          break;
        }
        case 2:
        {
          if (!prg_rbg_1_2w_bitmap_16bpp_p_)
          {
            prg_rbg_1_2w_bitmap_16bpp_p_ = compile_color_dot(
                S(a_prg_rbg_1_2w_bitmap),
                prg_rbg_getcolor_16bpp_palette,
                prg_generate_rbg_end);
          }
          CurrentPipeline = prg_rbg_1_2w_bitmap_16bpp_p_;
          break;
        }
        case 3:
        {
          if (!prg_rbg_1_2w_bitmap_16bpp_)
          {
            prg_rbg_1_2w_bitmap_16bpp_ = compile_color_dot(
                S(a_prg_rbg_1_2w_bitmap),
                prg_rbg_getcolor_16bpp_rbg,
                prg_generate_rbg_end);
          }
          CurrentPipeline = prg_rbg_1_2w_bitmap_16bpp_;
          break;
        }
        case 4:
        {
          if (!prg_rbg_1_2w_bitmap_32bpp_)
          {
            prg_rbg_1_2w_bitmap_32bpp_ = compile_color_dot(
                S(a_prg_rbg_1_2w_bitmap),
                prg_rbg_getcolor_32bpp_rbg,
                prg_generate_rbg_end);
          }
          CurrentPipeline = prg_rbg_1_2w_bitmap_32bpp_;
          break;
        }
        }
      }
      else
      {
        if (rbg->info.patterndatasize == 1)
        {
          switch (rbg->info.colornumber)
          {
          case 0:
          {
            if (!prg_rbg_1_2w_p1_4bpp_)
            {
              prg_rbg_1_2w_p1_4bpp_ = compile_color_dot(
                  S(a_prg_rbg_1_2w_p1_4bpp),
                  prg_rbg_getcolor_4bpp,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_1_2w_p1_4bpp_;
            break;
          }
          case 1:
          {
            if (!prg_rbg_1_2w_p1_8bpp_)
            {
              prg_rbg_1_2w_p1_8bpp_ = compile_color_dot(
                  S(a_prg_rbg_1_2w_p1_8bpp),
                  prg_rbg_getcolor_8bpp,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_1_2w_p1_8bpp_;
            break;
          }
          case 2:
          {
            if (!prg_rbg_1_2w_p1_16bpp_p_)
            {
              prg_rbg_1_2w_p1_16bpp_p_ = compile_color_dot(
                  S(a_prg_rbg_1_2w_p1_4bpp),
                  prg_rbg_getcolor_16bpp_palette,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_1_2w_p1_16bpp_p_;
            break;
          }
          case 3:
          {
            if (!prg_rbg_1_2w_p1_16bpp_)
            {
              prg_rbg_1_2w_p1_16bpp_ = compile_color_dot(
                  S(a_prg_rbg_1_2w_p1_4bpp),
                  prg_rbg_getcolor_16bpp_rbg,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_1_2w_p1_16bpp_;
            break;
          }
          case 4:
          {
            if (!prg_rbg_1_2w_p1_32bpp_)
            {
              prg_rbg_1_2w_p1_32bpp_ = compile_color_dot(
                  S(a_prg_rbg_0_2w_p1_4bpp),
                  prg_rbg_getcolor_32bpp_rbg,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_1_2w_p1_32bpp_;
            break;
          }
          }
        }
        else
        {
          switch (rbg->info.colornumber)
          {
          case 0:
          {
            if (!prg_rbg_1_2w_p2_4bpp_)
            {
              prg_rbg_1_2w_p2_4bpp_ = compile_color_dot(
                  S(a_prg_rbg_1_2w_p2_4bpp),
                  prg_rbg_getcolor_4bpp,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_1_2w_p2_4bpp_;
            break;
          }
          case 1:
          {
            if (!prg_rbg_1_2w_p2_8bpp_)
            {
              prg_rbg_1_2w_p2_8bpp_ = compile_color_dot(
                  S(a_prg_rbg_1_2w_p2_8bpp),
                  prg_rbg_getcolor_8bpp,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_1_2w_p2_8bpp_;
            break;
          }
          case 2:
          {
            if (!prg_rbg_1_2w_p2_16bpp_p_)
            {
              prg_rbg_1_2w_p2_16bpp_p_ = compile_color_dot(
                  S(a_prg_rbg_1_2w_p2_4bpp),
                  prg_rbg_getcolor_16bpp_palette,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_1_2w_p2_16bpp_p_;
            break;
          }
          case 3:
          {
            if (!prg_rbg_1_2w_p2_16bpp_)
            {
              prg_rbg_1_2w_p2_16bpp_ = compile_color_dot(
                  S(a_prg_rbg_1_2w_p2_4bpp),
                  prg_rbg_getcolor_16bpp_rbg,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_1_2w_p2_16bpp_;
            break;
          }
          case 4:
          {
            if (!prg_rbg_1_2w_p2_32bpp_)
            {
              prg_rbg_1_2w_p2_32bpp_ = compile_color_dot(
                  S(a_prg_rbg_1_2w_p2_4bpp),
                  prg_rbg_getcolor_32bpp_rbg,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_1_2w_p2_32bpp_;
            break;
          }
          }
        }
      }
    }
    else if (fixVdp2Regs->RPMD == 2)
    {
      if (rbg->info.isbitmap)
      {
        switch (rbg->info.colornumber)
        {
        case 0:
        {
          if (!prg_rbg_2_2w_bitmap_4bpp_)
          {
            prg_rbg_2_2w_bitmap_4bpp_ = compile_color_dot(
                S(a_prg_rbg_2_2w_bitmap),
                prg_rbg_getcolor_4bpp,
                prg_generate_rbg_end);
          }
          CurrentPipeline = prg_rbg_2_2w_bitmap_4bpp_;
          break;
        }
        case 1:
        {
          if (!prg_rbg_2_2w_bitmap_8bpp_)
          {
            prg_rbg_2_2w_bitmap_8bpp_ = compile_color_dot(
                S(a_prg_rbg_2_2w_bitmap),
                prg_rbg_getcolor_8bpp,
                prg_generate_rbg_end);
          }
          CurrentPipeline = prg_rbg_2_2w_bitmap_8bpp_;
          break;
        }
        case 2:
        {
          if (!prg_rbg_2_2w_bitmap_16bpp_p_)
          {
            prg_rbg_2_2w_bitmap_16bpp_p_ = compile_color_dot(
                S(a_prg_rbg_2_2w_bitmap),
                prg_rbg_getcolor_16bpp_palette,
                prg_generate_rbg_end);
          }
          CurrentPipeline = prg_rbg_2_2w_bitmap_16bpp_p_;
          break;
        }
        case 3:
        {
          if (!prg_rbg_2_2w_bitmap_16bpp_)
          {
            prg_rbg_2_2w_bitmap_16bpp_ = compile_color_dot(
                S(a_prg_rbg_2_2w_bitmap),
                prg_rbg_getcolor_16bpp_rbg,
                prg_generate_rbg_end);
          }
          CurrentPipeline = prg_rbg_2_2w_bitmap_16bpp_;
          break;
        }
        case 4:
        {
          if (!prg_rbg_2_2w_bitmap_32bpp_)
          {
            prg_rbg_2_2w_bitmap_32bpp_ = compile_color_dot(
                S(a_prg_rbg_2_2w_bitmap),
                prg_rbg_getcolor_32bpp_rbg,
                prg_generate_rbg_end);
          }
          CurrentPipeline = prg_rbg_2_2w_bitmap_32bpp_;
          break;
        }
        }
      }
      else
      {
        if (rbg->info.patterndatasize == 1)
        {
          switch (rbg->info.colornumber)
          {
          case 0:
          { // BlukSlash
            if (!prg_rbg_2_2w_p1_4bpp_)
            {
              prg_rbg_2_2w_p1_4bpp_ = compile_color_dot(
                  S(a_prg_rbg_2_2w_p1_4bpp),
                  prg_rbg_getcolor_4bpp,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_2_2w_p1_4bpp_;
            break;
          }
          case 1:
          { // Panzer Dragoon Zwei, Toshiden(Title) ToDo: Sky bug
            if (!prg_rbg_2_2w_p1_8bpp_)
            {
              prg_rbg_2_2w_p1_8bpp_ = compile_color_dot(
                  S(a_prg_rbg_2_2w_p1_8bpp),
                  prg_rbg_getcolor_8bpp,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_2_2w_p1_8bpp_;
            break;
          }
          case 2:
          {
            if (!prg_rbg_2_2w_p1_16bpp_p_)
            {
              prg_rbg_2_2w_p1_16bpp_p_ = compile_color_dot(
                  S(a_prg_rbg_2_2w_p1_4bpp),
                  prg_rbg_getcolor_16bpp_palette,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_2_2w_p1_16bpp_p_;
            break;
          }
          case 3:
          {
            if (!prg_rbg_2_2w_p1_16bpp_)
            {
              prg_rbg_2_2w_p1_16bpp_ = compile_color_dot(
                  S(a_prg_rbg_2_2w_p1_4bpp),
                  prg_rbg_getcolor_16bpp_rbg,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_2_2w_p1_16bpp_;
            break;
          }
          case 4:
          {
            if (!prg_rbg_2_2w_p1_32bpp_)
            {
              prg_rbg_2_2w_p1_32bpp_ = compile_color_dot(
                  S(a_prg_rbg_2_2w_p1_4bpp),
                  prg_rbg_getcolor_32bpp_rbg,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_2_2w_p1_32bpp_;
            break;
          }
          }
        }
        else
        {
          switch (rbg->info.colornumber)
          {
          case 0:
          {
            if (!prg_rbg_2_2w_p2_4bpp_)
            {
              prg_rbg_2_2w_p2_4bpp_ = compile_color_dot(
                  S(a_prg_rbg_2_2w_p2_4bpp),
                  prg_rbg_getcolor_4bpp,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_2_2w_p2_4bpp_;
            break;
          }
          case 1:
          {
            if (!prg_rbg_2_2w_p2_8bpp_)
            {
              prg_rbg_2_2w_p2_8bpp_ = compile_color_dot(
                  S(a_prg_rbg_2_2w_p2_8bpp),
                  prg_rbg_getcolor_8bpp,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_2_2w_p2_8bpp_;
            break;
          }
          case 2:
          {
            if (!prg_rbg_2_2w_p2_16bpp_p_)
            {
              prg_rbg_2_2w_p2_16bpp_p_ = compile_color_dot(
                  S(a_prg_rbg_2_2w_p2_4bpp),
                  prg_rbg_getcolor_16bpp_palette,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_2_2w_p2_16bpp_p_;
            break;
          }
          case 3:
          {
            if (!prg_rbg_2_2w_p2_16bpp_)
            {
              prg_rbg_2_2w_p2_16bpp_ = compile_color_dot(
                  S(a_prg_rbg_2_2w_p2_4bpp),
                  prg_rbg_getcolor_16bpp_rbg,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_2_2w_p2_16bpp_;
            break;
          }
          case 4:
          {
            if (!prg_rbg_2_2w_p2_32bpp_)
            {
              prg_rbg_2_2w_p2_32bpp_ = compile_color_dot(
                  S(a_prg_rbg_2_2w_p2_4bpp),
                  prg_rbg_getcolor_32bpp_rbg,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_2_2w_p2_32bpp_;
            break;
          }
          }
        }
      }
    }
    else if (fixVdp2Regs->RPMD == 3)
    {

      if (rbg->info.isbitmap)
      {
        switch (rbg->info.colornumber)
        {
        case 0:
        {
          if (!prg_rbg_3_2w_bitmap_4bpp_)
          {
            prg_rbg_3_2w_bitmap_4bpp_ = compile_color_dot(
                S(a_prg_rbg_3_2w_bitmap),
                prg_rbg_getcolor_4bpp,
                prg_generate_rbg_end);
          }
          CurrentPipeline = prg_rbg_3_2w_bitmap_4bpp_;
          break;
        }
        case 1:
        {
          if (!prg_rbg_3_2w_bitmap_8bpp_)
          {
            prg_rbg_3_2w_bitmap_8bpp_ = compile_color_dot(
                S(a_prg_rbg_3_2w_bitmap),
                prg_rbg_getcolor_8bpp,
                prg_generate_rbg_end);
          }
          CurrentPipeline = prg_rbg_3_2w_bitmap_8bpp_;
          break;
        }
        case 2:
        {
          if (!prg_rbg_3_2w_bitmap_16bpp_p_)
          {
            prg_rbg_3_2w_bitmap_16bpp_p_ = compile_color_dot(
                S(a_prg_rbg_3_2w_bitmap),
                prg_rbg_getcolor_16bpp_palette,
                prg_generate_rbg_end);
          }
          CurrentPipeline = prg_rbg_3_2w_bitmap_16bpp_p_;
          break;
        }
        case 3:
        {
          if (!prg_rbg_3_2w_bitmap_16bpp_)
          {
            prg_rbg_3_2w_bitmap_16bpp_ = compile_color_dot(
                S(a_prg_rbg_3_2w_bitmap),
                prg_rbg_getcolor_16bpp_rbg,
                prg_generate_rbg_end);
          }
          CurrentPipeline = prg_rbg_3_2w_bitmap_16bpp_;
          break;
        }
        case 4:
        {
          if (!prg_rbg_3_2w_bitmap_32bpp_)
          {
            prg_rbg_3_2w_bitmap_32bpp_ = compile_color_dot(
                S(a_prg_rbg_3_2w_bitmap),
                prg_rbg_getcolor_32bpp_rbg,
                prg_generate_rbg_end);
          }
          CurrentPipeline = prg_rbg_3_2w_bitmap_32bpp_;
          break;
        }
        }
      }
      else
      {
        if (rbg->info.patterndatasize == 1)
        {
          switch (rbg->info.colornumber)
          {
          case 0:
          {
            if (!prg_rbg_3_2w_p1_4bpp_)
            {
              prg_rbg_3_2w_p1_4bpp_ = compile_color_dot(
                  S(a_prg_rbg_3_2w_p1_4bpp),
                  prg_rbg_getcolor_4bpp,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_3_2w_p1_4bpp_;
            break;
          }
          case 1:
          { // Final Fight Revenge, Grandia main
            if (!prg_rbg_3_2w_p1_8bpp_)
            {
              prg_rbg_3_2w_p1_8bpp_ = compile_color_dot(
                  S(a_prg_rbg_3_2w_p1_8bpp),
                  prg_rbg_getcolor_8bpp,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_3_2w_p1_8bpp_;
            break;
          }
          case 2:
          {
            if (!prg_rbg_3_2w_p1_16bpp_p_)
            {
              prg_rbg_3_2w_p1_16bpp_p_ = compile_color_dot(
                  S(a_prg_rbg_3_2w_p1_4bpp),
                  prg_rbg_getcolor_16bpp_palette,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_3_2w_p1_16bpp_p_;
            break;
          }
          case 3:
          { // Power Drift
            if (!prg_rbg_3_2w_p1_16bpp_)
            {
              prg_rbg_3_2w_p1_16bpp_ = compile_color_dot(
                  S(a_prg_rbg_3_2w_p1_4bpp),
                  prg_rbg_getcolor_16bpp_rbg,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_3_2w_p1_16bpp_;
            break;
          }
          case 4:
          {
            if (!prg_rbg_3_2w_p1_32bpp_)
            {
              prg_rbg_3_2w_p1_32bpp_ = compile_color_dot(
                  S(a_prg_rbg_3_2w_p1_4bpp),
                  prg_rbg_getcolor_32bpp_rbg,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_3_2w_p1_32bpp_;
            break;
          }
          }
        }
        else
        {
          switch (rbg->info.colornumber)
          {
          case 0:
          {
            if (!prg_rbg_3_2w_p2_4bpp_)
            {
              prg_rbg_3_2w_p2_4bpp_ = compile_color_dot(
                  S(a_prg_rbg_3_2w_p2_4bpp),
                  prg_rbg_getcolor_4bpp,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_3_2w_p2_4bpp_;
            break;
          }
          case 1:
          {
            if (!prg_rbg_3_2w_p2_8bpp_)
            {
              prg_rbg_3_2w_p2_8bpp_ = compile_color_dot(
                  S(a_prg_rbg_3_2w_p2_8bpp),
                  prg_rbg_getcolor_8bpp,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_3_2w_p2_8bpp_;
            break;
          }
          case 2:
          {
            if (!prg_rbg_3_2w_p2_16bpp_p_)
            {
              prg_rbg_3_2w_p2_16bpp_p_ = compile_color_dot(
                  S(a_prg_rbg_3_2w_p2_4bpp),
                  prg_rbg_getcolor_16bpp_palette,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_3_2w_p2_16bpp_p_;
            break;
          }
          case 3:
          {
            if (!prg_rbg_3_2w_p2_16bpp_)
            {
              prg_rbg_3_2w_p2_16bpp_ = compile_color_dot(
                  S(a_prg_rbg_3_2w_p2_4bpp),
                  prg_rbg_getcolor_16bpp_rbg,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_3_2w_p2_16bpp_;
            break;
          }
          case 4:
          {
            if (!prg_rbg_3_2w_p2_32bpp_)
            {
              prg_rbg_3_2w_p2_32bpp_ = compile_color_dot(
                  S(a_prg_rbg_3_2w_p2_4bpp),
                  prg_rbg_getcolor_32bpp_rbg,
                  prg_generate_rbg_end);
            }
            CurrentPipeline = prg_rbg_3_2w_p2_32bpp_;
            break;
          }
          }
        }
      }
      // ToDo: Copy Window Info
      // glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_window_);
      // glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(vdp2WindowInfo)*(int(rbg->vres / rbg->rotate_mval_v) << rbg->info.hres_shift), (void*)rbg->info.pWinInfo);
      // glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, ssbo_window_);
    }
  }

  void *data;
  if (rbg->info.pWinInfo != nullptr)
  {
    int wsize = sizeof(vdp2WindowInfo) * (int(rbg->vres / rbg->rotate_mval_v) << rbg->info.hres_shift);
    data = d.mapMemory(ssbo_window_.mem, 0, wsize);
    memcpy(data, (void *)rbg->info.pWinInfo, wsize);
    d.unmapMemory(ssbo_window_.mem);
  }

  data = d.mapMemory(ssbo_vram_.mem, 0, 0x80000);
  memcpy(data, Vdp2Ram, 0x80000);
  d.unmapMemory(ssbo_vram_.mem);

  if (rbg->info.specialcolormode == 3 || paraa.k_mem_type != 0 || parab.k_mem_type != 0)
  {
    data = d.mapMemory(ssbo_cram_.mem, 0, 0x1000);
    memcpy(data, Vdp2ColorRam, 0x1000);
    d.unmapMemory(ssbo_cram_.mem);
  }

  int asize = getAllainedSize(sizeof(vdp2rotationparameter_struct));
  data = d.mapMemory(ssbo_paraA_.mem, 0, asize*2);
  vdp2rotationparameter_struct upA = paraa;
  vdp2rotationparameter_struct upB = parab;
  rbgApplyLayerBankMasks(rbg->rgb_type, &upA, &upB);
  memcpy(data, &upA, sizeof(vdp2rotationparameter_struct) );
  memcpy(((char*)data + sizeof(vdp2rotationparameter_struct)), &upB, sizeof(vdp2rotationparameter_struct));
  d.unmapMemory(ssbo_paraA_.mem);

  rbgUniformParam->hres_scale = 1.0 / rbg->rotate_mval_h;
  rbgUniformParam->vres_scale = 1.0 / rbg->rotate_mval_v;
  rbgUniformParam->cellw = rbg->info.cellw;
  rbgUniformParam->cellh = rbg->info.cellh;
  rbgUniformParam->paladdr_ = rbg->info.paladdr;
  rbgUniformParam->pagesize = rbg->pagesize;
  rbgUniformParam->patternshift = rbg->patternshift;
  rbgUniformParam->planew = rbg->info.planew;
  rbgUniformParam->pagewh = rbg->info.pagewh;
  rbgUniformParam->patterndatasize = rbg->info.patterndatasize;
  rbgUniformParam->supplementdata = rbg->info.supplementdata;
  rbgUniformParam->auxmode = rbg->info.auxmode;
  rbgUniformParam->patternwh = rbg->info.patternwh;
  rbgUniformParam->coloroffset = rbg->info.coloroffset;
  rbgUniformParam->transparencyenable = rbg->info.transparencyenable;
  rbgUniformParam->specialcolormode = rbg->info.specialcolormode;
  rbgUniformParam->specialcolorfunction = rbg->info.specialcolorfunction;
  rbgUniformParam->specialcode = rbg->info.specialcode;
  rbgUniformParam->colornumber = rbg->info.colornumber;
  rbgUniformParam->window_area_mode = rbg->info.WindwAreaMode;
  rbgUniformParam->alpha_ = (float)rbg->info.alpha / 255.0f;
  rbgUniformParam->specialprimode = rbg->info.specialprimode;
  rbgUniformParam->priority = rbg->info.priority;

  if (Vdp2Internal.ColorMode < 2)
  {
    rbgUniformParam->cram_shift = 1;
  }
  else
  {
    rbgUniformParam->cram_shift = 2;
  }
  rbgUniformParam->hires_shift = rbg->info.hres_shift;

  currentIndex = drawCounter & (MAX_RBG_RENDER - 1);

  data = d.mapMemory(rbgUniform[currentIndex].mem, 0, sizeof(*rbgUniformParam));
  memcpy(data, rbgUniformParam, sizeof(*rbgUniformParam));
  d.unmapMemory(rbgUniform[currentIndex].mem);

  try
  {
    auto c = vk::CommandBuffer(command[currentIndex]);

    vk::Result result;
    if (drawCounter > 1)
    {
      result = d.waitForFences(commandfence[currentIndex], true, UINT64_MAX);
      if (result != vk::Result::eSuccess)
      {
        LOGE("Timeout!");
        // exit( -1 );
      }
      d.resetFences(commandfence[currentIndex]);
    }

    queue.waitIdle();
    updateDescriptorSets(texindex);
    c.reset();
    c.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

    vk::ImageMemoryBarrier barrierBegin;
    barrierBegin.oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    barrierBegin.newLayout = vk::ImageLayout::eGeneral;
    barrierBegin.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrierBegin.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrierBegin.image = tex_surface[texindex].image;
    barrierBegin.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    barrierBegin.subresourceRange.baseMipLevel = 0;
    barrierBegin.subresourceRange.levelCount = 1;
    barrierBegin.subresourceRange.baseArrayLayer = 0;
    barrierBegin.subresourceRange.layerCount = 1;
    barrierBegin.srcAccessMask = vk::AccessFlagBits::eShaderRead;
    barrierBegin.dstAccessMask = vk::AccessFlags();

    c.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eComputeShader,
        vk::DependencyFlags(),
        0, nullptr,
        0, nullptr,
        1, &barrierBegin);

    c.bindPipeline(vk::PipelineBindPoint::eCompute, CurrentPipeline);
    c.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipelineLayout, 0, descriptorSet[currentIndex], nullptr);
    c.dispatch(tex_width_ / 16, tex_height_ / 16, 1);

    vk::ImageMemoryBarrier barrier;
    barrier.oldLayout = vk::ImageLayout::eGeneral;
    barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = tex_surface[texindex].image;
    barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = vk::AccessFlags();
    barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;

    c.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eComputeShader,
        vk::DependencyFlags(),
        0, nullptr,
        0, nullptr,
        1, &barrier);

    c.end();

    static const std::vector<vk::PipelineStageFlags> waitStages{vk::PipelineStageFlagBits::eComputeShader};
    // Submit compute commands
    vk::SubmitInfo computeSubmitInfo;
    computeSubmitInfo.commandBufferCount = 1;
    computeSubmitInfo.pCommandBuffers = &c;
    // computeSubmitInfo.waitSemaphoreCount = 1;
    // computeSubmitInfo.pWaitSemaphores = &semaphores.ready;
    // computeSubmitInfo.pWaitDstStageMask = waitStages.data();
    computeSubmitInfo.signalSemaphoreCount = 1;
    computeSubmitInfo.pSignalSemaphores = &semaphores[currentIndex].complete;
    tex_surface[texindex].rendered = true;
    queue.submit(computeSubmitInfo, commandfence[currentIndex]);
  }
  catch (vk::SystemError &err)
  {
    LOGE("vk::SystemError: %s", err.what());
  }
  catch (std::exception &err)
  {
    LOGE("std::exception: %s", err.what());
  }
  catch (...)
  {
    LOGE("unknown error\n");
  }

  drawCounter++;

  // vulkan->transitionImageLayout(VkImage(tex_surface[texindex].image),
  //   VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

VkImageView RBGGeneratorVulkan::getTexture(int id)
{
  return static_cast<VkImageView>(tex_surface[id].view);
}

void RBGGeneratorVulkan::onFinish()
{
  /*
    if (tex_surface[0].rendered) {
      vulkan->transitionImageLayout(VkImage(tex_surface[0].image),
        VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
      tex_surface[0].rendered = false;
    }

    if (tex_surface[1].rendered) {
      vulkan->transitionImageLayout(VkImage(tex_surface[1].image),
        VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
      tex_surface[1].rendered = false;
    }
  */
}

#endif
#if defined(HAVE_LIBGL) || defined(_OGLES3_)

RBGGenerator *RBGGenerator::instance_ = nullptr;

extern "C"
{
  void RBGGenerator_init(int width, int height)
  {
    if (_Ygl->rbg_use_compute_shader == 0)
      return;

    // FILE *fp = fopen("sdmc:/tiicu/debug/debug_core.txt", "a");
    // if (fp) {
    //     fprintf(fp, "[CS] RBGGenerator_init Start %dx%d\n", width, height);
    //     fclose(fp);
    // }

    RBGGenerator *instance = RBGGenerator::getInstance();
    instance->init(width, height);

    // fp = fopen("sdmc:/tiicu/debug/debug_core.txt", "a");
    // if (fp) {
    //     fprintf(fp, "[CS] RBGGenerator_init End\n");
    //     fclose(fp);
    // }
  }
  void RBGGenerator_resize(int width, int height)
  {
    if (_Ygl->rbg_use_compute_shader == 0)
      return;

    RBGGenerator *instance = RBGGenerator::getInstance();
    instance->resize(width, height);
  }
  void RBGGenerator_update(RBGDrawInfo *rbg)
  {
    if (_Ygl->rbg_use_compute_shader == 0)
      return;

    // static int log_counter = 0;
    // if (log_counter < 10) {
    //     FILE *fp = fopen("sdmc:/tiicu/debug/debug_core.txt", "a");
    //     if (fp) {
    //         fprintf(fp, "[CS] RBGGenerator_update Call %d\n", log_counter);
    //         fclose(fp);
    //     }
    //     log_counter++;
    // }

    RBGGenerator *instance = RBGGenerator::getInstance();
    instance->update(rbg);
  }
  GLuint RBGGenerator_getTexture(int id)
  {
    if (_Ygl->rbg_use_compute_shader == 0)
      return 0;

    RBGGenerator *instance = RBGGenerator::getInstance();
    return instance->getTexture(id);
  }
  void RBGGenerator_onFinish()
  {

    if (_Ygl->rbg_use_compute_shader == 0)
      return;
    RBGGenerator *instance = RBGGenerator::getInstance();
    instance->onFinish();
  }

  void RBGGenerator_deinit()
  {
    RBGGenerator::destroyInstance();
  }
}

#endif // HAVE_LIBGL || _OGLES3_