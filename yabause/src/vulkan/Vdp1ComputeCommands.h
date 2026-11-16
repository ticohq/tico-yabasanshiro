// Copyright 2026 devMiyax (smiyaxdev@gmail.com)
// VDP1 Compute Rasterizer: shared CPU/GPU data layout.
// std430-aligned (16-byte boundary aware).
#pragma once

// This header is included from GLSL as well, so guard the C++ side.
#ifndef __cplusplus
  // GLSL: vec/uvec/ivec are built-in.
#else
  #include <cstdint>
  #include <glm/glm.hpp>
  using uint = uint32_t;
  using vec2 = glm::vec2;
  using uvec2 = glm::uvec2;
  using ivec2 = glm::ivec2;
  using ivec4 = glm::ivec4;
#endif

// Compute rasterizer constants.
#define VDP1C_MAX_CMDS              16384
#define VDP1C_TILE_SIZE             16          // 16x16 px / tile
#define VDP1C_MAX_CMDS_PER_TILE     64

// Command kind (NOTE: internal representation; does NOT match CMDCTRL bit3-0).
#define VDP1C_TYPE_NOOP             0u
#define VDP1C_TYPE_NORMAL_SPRITE    1u
#define VDP1C_TYPE_SCALED_SPRITE    2u
#define VDP1C_TYPE_DISTORTED_SPRITE 3u
#define VDP1C_TYPE_POLYGON          4u
#define VDP1C_TYPE_POLYLINE         5u
#define VDP1C_TYPE_LINE             6u

// Vdp1Cmd::flags bit definitions. Set by host encode functions, read by the
// raster shaders. Stored at offset 120 of Vdp1Cmd.
#define VDP1C_FLAG_TRI_SPLIT        (1u << 0)  // concave/twisted quad: 2-triangle path
// THIN: polygon's actual geometric thickness <= ~2 Saturn px. Computed from
// quad edge lengths (not bbox) so diagonal narrow polygons are caught.
// Shader gates edge-supercover on this bit so only thin polygons get the
// dilation band; normal-thickness polygons render strict-inside-only.
#define VDP1C_FLAG_THIN             (1u << 1)
// DILATE_BR_X / DILATE_BR_Y: per-axis BR-cell dilation gates for Saturn-
// cell-inclusive vertex semantics. Vertex (cx, cy) represents Saturn cell
// (cx, cy) which at HD upscale spans fb pixels [cx*sX, (cx+1)*sX-1] x
// [cy*sY, (cy+1)*sY-1]; the shader's brEdgeDilation extends BR-facing
// edges by ~scaleMax fb px to cover the last fb pixel of that cell, and
// bbox.z/.w are extended in applyState so the bbox clamp doesn't reject
// pixels in the dilated band.
//
// Set independently by the host based on bbox dimensions:
//   * bbox width  >= scaleMax * MIN_DILATION_BBOX_CELLS -> DILATE_BR_X
//   * bbox height >= scaleMax * MIN_DILATION_BBOX_CELLS -> DILATE_BR_Y
// Per-axis so wide-thin polygons (e.g., 100x1 cells) get horizontal
// extension without distorting their short vertical axis. Cleared for
// fence-post paths (NormalSprite / ScaledSprite, where +1 is already
// baked into the host vertex code) and for small polygons that would be
// visibly distorted by a +1 cell extension.
#define VDP1C_FLAG_DILATE_BR_X      (1u << 2)
#define VDP1C_FLAG_DILATE_BR_Y      (1u << 3)

// Vdp1Cmd::attrFlags bits -- VDP2 framebuffer encoding extras (F18).
//   MSB_SHADOW    : CMDPMOD bit 15 set; force shadow pixel encoding
//   SPRITE_WINDOW : VDP2 SPCTL gates this cmd into the sprite-window mask
#define VDP1C_ATTR_MSB_SHADOW       (1u << 0)
#define VDP1C_ATTR_SPRITE_WINDOW    (1u << 1)

// Per-command metadata. std430 layout matches between C++ and GLSL.
// Plan 1 only uses Polygon in practice; other fields are zero-initialized.
//
// Measured layout (g++ -std=c++17, glm::uvec2/ivec2/ivec4):
//   offset  0: cmdType     (uint,  4B)
//   offset  4: pmod        (uint,  4B)
//   offset  8: color       (uint,  4B)
//   offset 12: srca        (uint,  4B)
//   offset 16: charSize    (uvec2, 8B)  align=4
//   offset 24: gouraudAddr (uvec2, 8B)  align=4
//   offset 32: v0          (ivec2, 8B)  align=4
//   offset 40: v1          (ivec2, 8B)
//   offset 48: v2          (ivec2, 8B)
//   offset 56: v3          (ivec2, 8B)
//   offset 64: bbox        (ivec4,16B)  align=4
//   offset 80: systemClip  (ivec4,16B)
//   offset 96: userClip    (ivec4,16B)
//   offset112: clipMode    (uint,  4B)
//   offset116: flip        (uint,  4B)  bit0=Hflip, bit1=Vflip (CMDCTRL bits 4-5)
//   offset120: flags       (uint,  4B)  VDP1C_FLAG_* bitfield (THIN, TRI_SPLIT)
//   offset124: vdp2Attrs   (uint,  4B)  F18: bits 0:15 normalShadow / 16:31 attrFlags
//   offset128: priority    (uint,  4B)  F18: VDP2 sprite priority slot index 0-7
//   offset132: colorcl     (uint,  4B)  F18: VDP2 color-calc slot index 0-7
//   offset136: lineSteps   (uint,  4B)  sprites: bits 0..15 dmax, bits 16..31 A->B length
//   offset140: pad7        (uint,  4B)  pads std430 array stride to a 16B multiple
//   total = 144 bytes (16-byte aligned, std430 array stride 144B)
struct Vdp1Cmd {
    uint  cmdType;       // VDP1C_TYPE_*
    uint  pmod;          // CMDPMOD raw
    uint  color;         // CMDCOLR (Polygon: 15-bit RGB or palette)
    uint  srca;          // CMDSRCA (sprite only)
    uvec2 charSize;      // CMDSIZE width/height (sprite only)
    uvec2 gouraudAddr;   // (address_div8, enable_flag)
    ivec2 v0;            // 4 verts (local coord applied, screen-pixel coords)
    ivec2 v1;
    ivec2 v2;
    ivec2 v3;
    ivec4 bbox;          // (minX, minY, maxX, maxY)
    ivec4 systemClip;    // (xc, yc, _, _)  Plan 1: pinned to full area
    ivec4 userClip;      // (xa, ya, xc, yc) Plan 1: pinned to full area
    uint  clipMode;      // 0=disabled, 1=inside, 2=outside  Plan 1: always 0
    uint  flip;          // bit0=Hflip, bit1=Vflip (CMDCTRL bits 4-5)
    uint  flags;         // VDP1C_FLAG_* bitfield (THIN, TRI_SPLIT)
    // F18: VDP2 framebuffer encoding extras packed into one uint:
    //   bits 0:15  = normalShadow colorindex (per-cmd Saturn "normal shadow"
    //                detection value; 0 disables match)
    //   bits 16:31 = attrFlags (VDP1C_ATTR_MSB_SHADOW | VDP1C_ATTR_SPRITE_WINDOW)
    uint  vdp2Attrs;
    uint  priority;     // F18: VDP2 sprite priority slot index 0-7
    uint  colorcl;      // F18: VDP2 color-calc slot index 0-7
    // Sprites only: bits 0..15 = dmax (the command is drawn as dmax + 1
    // textured lines), bits 16..31 = Chebyshev length of A->B. Both come
    // from the raw Saturn vertices (vdp1c::packSpriteSteps); the shaders
    // read them instead of deriving them from the scaled fb vertices.
    uint  lineSteps;
    uint  pad7;         // pads std430 array stride to a 16B multiple (= 144)
};

// Per-tile command list (output of the Bin shader).
struct BinningTileCount {
    uint count;          // target of atomicAdd
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

#ifdef __cplusplus
// Measured: 144 bytes (the thin-poly snap fields uvTL/TR/BR/BL/thinFlag were removed).
static_assert(sizeof(Vdp1Cmd) == 144, "Vdp1Cmd std430 layout must be 144 bytes");
static_assert(sizeof(BinningTileCount) == 16, "BinningTileCount must be 16 bytes (std430 align)");
#endif
