// Copyright 2026 devMiyax
#include "Vdp1ComputeRasterizer.h"
#include "VIDVulkan.h"
#include "VdpPipeline.h"
#include "VulkanTools.h"
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <chrono>
#include "shaderc/shaderc.hpp"

namespace {

// ---------------------------------------------------------------------------
// Embedded shader sources.
// Android APK cannot bundle loose .comp files so the GLSL is embedded as a
// compile-time string constant. Canonical source-of-truth files live under
// yabause/src/vulkan/shaders/; keep them in sync whenever either side is
// modified. The Phase 2A rewrite removed the legacy scanline shader; the
// remaining embedded sources (binning / tile_shade / forward) are defined
// further below.
// ---------------------------------------------------------------------------

struct BinPushConstants {
    uint32_t numCmds;
    uint32_t numTilesX;
    uint32_t numTilesY;
    uint32_t tileSize;
};
static_assert(sizeof(BinPushConstants) == 16, "BinPushConstants must be 16 bytes");

struct ShadePushConstants {
    uint32_t numTilesX;
    uint32_t numTilesY;
    uint32_t fbWidth;
    uint32_t fbHeight;
    // HD upscale factor (= max(vdp1wratio, vdp1hratio)). Lets the shader
    // scale the edge-supercover threshold to approximately 1 Saturn cell
    // wide regardless of HD resolution.
    float    scaleMax;
    // VDP2 SPCTL & 0xF -- sprite type (0..15). Required for 4bpp LUT mode
    // because the LUT word itself encodes sprite-type-dependent
    // priority/colorcl/normalshadow bits (graphics path runs
    // Vdp1ProcessSpritePixel per pixel; without it the compute path would
    // leave those bits inside colorindex and use the cmd-level colorcl /
    // priority that readPriority extracted from CMDCOLR rather than from
    // the LUT word, producing wrong VDP2 framebuffer pixels).
    uint32_t spriteType;
    // Per-axis HD upscale factor (Sprint 7.3, 2026-05-10). Used by
    // DISTORTED_SPRITE BR-vertex extension in tile_shade.comp to extend
    // axis-aligned distorted sprite verts by exactly 1 Saturn screen cell
    // per axis, mirroring forward shader's logic. scaleMax alone is not
    // sufficient for non-uniform aspect ratios.
    float    scaleX;
    float    scaleY;
};
static_assert(sizeof(ShadePushConstants) == 32, "ShadePushConstants must be 32 bytes");

// Phase 1A -- per-texel forward mapping push constants. Layout matches the
// `Push` block in vdp1_compute_forward.comp.
//
// cmdIndex (Issue #1 fix): the shader receives the target cmd index via
// push constant so per-cmd dispatch + barrier preserves VDP1 cmd submission
// order (LIFO blend). Previously gl_WorkGroupID.x was reused as the cmd
// index, but firing numCmds workgroups in a single dispatch left the
// imageLoad -> blend -> imageStore RMW order undefined across cmds on the
// GPU, breaking shadow / half-trans / mesh.
struct ForwardPushConstants {
    uint32_t fbWidth;
    uint32_t fbHeight;
    float    scaleMax;
    uint32_t spriteType;
    uint32_t cmdIndex;        // per-cmd / per-batch dispatch only
    uint32_t numTilesX;       // tile-binning forward only
    uint32_t tileSize;        // tile-binning forward only
    uint32_t maxCmdsPerTile;  // tile-binning forward only
    float    scaleX;          // axis-aligned BR extension X delta
    float    scaleY;          // axis-aligned BR extension Y delta
};
static_assert(sizeof(ForwardPushConstants) == 40, "ForwardPushConstants must be 40 bytes");

// =============================================================================
// Phase B1: Tile-binning Bin shader (Pass 1).
// Canonical source: yabause/src/vulkan/shaders/vdp1_compute_binning.comp.
// Each thread = one input cmd. atomicAdd into TileCount, write into TileCmdList.
// =============================================================================
constexpr const char* kBinShaderSrc = R"GLSL(
#version 450
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#define MAX_CMDS_PER_TILE 64
#define TYPE_NOOP             0u
#define TYPE_NORMAL_SPRITE    1u
#define TYPE_SCALED_SPRITE    2u
#define TYPE_DISTORTED_SPRITE 3u
#define TYPE_POLYGON          4u
#define TYPE_POLYLINE         5u
#define TYPE_LINE             6u

struct Vdp1Cmd {
    uint  cmdType;
    uint  pmod;
    uint  color;
    uint  srca;
    uvec2 charSize;
    uvec2 gouraudAddr;
    ivec2 v0, v1, v2, v3;
    ivec4 bbox;
    ivec4 systemClip;
    ivec4 userClip;
    uint  clipMode;
    uint  flip;
    uint  flags;        // VDP1C_FLAG_* bitfield (THIN, TRI_SPLIT)
    uint  vdp2Attrs;    // F18
    uint  priority;     // F18
    uint  colorcl;      // F18
    uint  lineSteps;     // bits 0..15 dmax, bits 16..31 A->B length (raw Saturn)
    uint  pad7;
};

struct TileCount {
    uint count;
    uint _pad0, _pad1, _pad2;
};

layout(std430, binding = 0) restrict readonly buffer CmdSSBO  { Vdp1Cmd cmds[]; };
layout(std430, binding = 1) coherent buffer TileCountSSBO { TileCount tiles[]; };
layout(std430, binding = 2) writeonly buffer TileListSSBO { uint cmdIdx[]; };
// F17: single uint counter incremented every time a cmd is dropped because
// its tile already reached MAX_CMDS_PER_TILE. Std430 padding keeps the
// buffer >= 16B (matches host OVERFLOW_SSBO_SIZE) but only counter is read.
layout(std430, binding = 3) coherent buffer OverflowSSBO {
    uint overflowCount;
    uint _pad0, _pad1, _pad2;
} overflow;

layout(push_constant) uniform Push {
    uint numCmds;
    uint numTilesX;
    uint numTilesY;
    uint tileSize;
} pc;

void main() {
    uint cid = gl_GlobalInvocationID.x;
    if (cid >= pc.numCmds) return;
    Vdp1Cmd c = cmds[cid];
    // All non-NOOP cmd types bin. Per-type rasterization happens in the
    // tile_shade shader; binning is purely a bbox->tile scatter.
    if (c.cmdType == TYPE_NOOP) return;

    ivec4 bbox = c.bbox;
    bbox.x = max(bbox.x, 0);
    bbox.y = max(bbox.y, 0);
    bbox.z = min(bbox.z, c.systemClip.x);
    bbox.w = min(bbox.w, c.systemClip.y);
    if (bbox.z < bbox.x || bbox.w < bbox.y) return;

    int tx0 = clamp(bbox.x / int(pc.tileSize), 0, int(pc.numTilesX) - 1);
    int ty0 = clamp(bbox.y / int(pc.tileSize), 0, int(pc.numTilesY) - 1);
    int tx1 = clamp(bbox.z / int(pc.tileSize), 0, int(pc.numTilesX) - 1);
    int ty1 = clamp(bbox.w / int(pc.tileSize), 0, int(pc.numTilesY) - 1);

    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            uint tileIdx = uint(ty) * pc.numTilesX + uint(tx);
            uint slot = atomicAdd(tiles[tileIdx].count, 1u);
            if (slot < uint(MAX_CMDS_PER_TILE)) {
                cmdIdx[tileIdx * uint(MAX_CMDS_PER_TILE) + slot] = cid;
            } else {
                // F17: tile already at capacity -- record the drop so the host
                // can flag overcrowded scenes in dev builds.
                atomicAdd(overflow.overflowCount, 1u);
            }
        }
    }
}
)GLSL";

// =============================================================================
// Phase B1: Tile-binning Shade shader (Pass 2).
// Canonical source: yabause/src/vulkan/shaders/vdp1_compute_tile_shade.comp.
// One workgroup = one 16x16 tile, one thread = one pixel. Cooperatively load
// + sort tile cmds, then walk them per-pixel doing inside-quad coverage +
// UV + texture sample + Replace blend in thread-local state. No atomics, no
// inter-cmd barriers.
// =============================================================================
constexpr const char* kShadeShaderSrc = R"GLSL(
#version 450
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

#define TILE_SIZE         16
#define MAX_CMDS_PER_TILE 64
#define TYPE_NOOP             0u
#define TYPE_NORMAL_SPRITE    1u
#define TYPE_SCALED_SPRITE    2u
#define TYPE_DISTORTED_SPRITE 3u
#define TYPE_POLYGON          4u
#define TYPE_POLYLINE         5u
#define TYPE_LINE             6u

// F13: concave / twisted quad -> 2-triangle path. See tile_shade applyCmd.
#define FLAG_TRI_SPLIT             1u
// THIN: polygon's geometric thickness <= ~2 Saturn px (set by host
// applyState). Gates edge-supercover so only thin polys get the
// dilation band; thicker polys render strict-inside-only.
#define FLAG_THIN                  2u
// DILATE_BR_X / DILATE_BR_Y: per-axis BR-cell dilation gates for Saturn-
// cell-inclusive vertex semantics. Host applyState sets each independently
// when bbox width / height >= scaleMax * MIN_DILATION_BBOX_CELLS -- so a
// long-thin polygon (100x1 cells) gets horizontal extension without
// distorting its short vertical axis. Cleared for fence-post paths
// (NormalSprite / ScaledSprite) and small polygons.
#define VDP1C_FLAG_DILATE_BR_X     4u
#define VDP1C_FLAG_DILATE_BR_Y     8u

// F18: VDP2 framebuffer encoding extras packed into Vdp1Cmd::vdp2Attrs.
//   bits 0:15  = normalShadow (per-cmd colorindex value, 0 disables)
//   bits 16:31 = attrFlags
#define ATTR_MSB_SHADOW       1u
#define ATTR_SPRITE_WINDOW    2u

struct Vdp1Cmd {
    uint  cmdType;
    uint  pmod;
    uint  color;
    uint  srca;
    uvec2 charSize;
    uvec2 gouraudAddr;
    ivec2 v0, v1, v2, v3;
    ivec4 bbox;
    ivec4 systemClip;
    ivec4 userClip;
    uint  clipMode;
    uint  flip;
    uint  flags;        // VDP1C_FLAG_* bitfield (THIN, TRI_SPLIT)
    uint  vdp2Attrs;    // F18: bits 0:15 normalShadow, 16:31 attrFlags
    uint  priority;     // F18
    uint  colorcl;      // F18
    uint  lineSteps;     // bits 0..15 dmax, bits 16..31 A->B length (raw Saturn)
    uint  pad7;
};

struct TileCount {
    uint count;
    uint _pad0, _pad1, _pad2;
};

layout(rgba8, binding = 0) uniform image2D fb;
layout(std430, binding = 1) restrict readonly buffer CmdSSBO       { Vdp1Cmd cmds[]; };
layout(std430, binding = 2) restrict readonly buffer TileCountSSBO { TileCount tiles[]; };
layout(std430, binding = 3) restrict readonly buffer TileListSSBO  { uint cmdIdx[]; };
layout(std430, binding = 4) restrict readonly buffer VramSSBO      { uint vram[]; };
layout(std430, binding = 5) restrict readonly buffer CramSSBO      { uint cram[]; };

// Spec consts (see Vdp1ComputeRasterizer.h for cache key layout).
layout(constant_id = 0) const uint SPEC_SPRITE_TYPE = 0u;
layout(constant_id = 1) const uint SPEC_USE_USERCLIP = 1u;
layout(constant_id = 2) const uint SPEC_USE_GOURAUD = 1u;
layout(constant_id = 3) const uint SPEC_USE_MESH = 1u;
layout(constant_id = 4) const uint SPEC_USE_MSB_SHADOW = 1u;

layout(push_constant) uniform Push {
    uint numTilesX;
    uint numTilesY;
    uint fbWidth;
    uint fbHeight;
    // HD upscale factor; converts Saturn-scale supercover thickness into
    // HD-pixel space so a "1 Saturn cell wide" edge band stays visually
    // consistent across RES_NATIVE / RES_2X / RES_4X.
    float scaleMax;
    // VDP2 SPCTL & 0xF -- sprite type 0..15. Only consumed by the 4bpp LUT
    // path: extracts priority/colorcl/normalshadow from the LUT word per
    // sprite-type bit layout, mirroring graphics path's per-pixel
    // Vdp1ProcessSpritePixel call. Other color modes use cmd.priority/
    // cmd.colorcl already baked from CMDCOLR by readPriority.
    uint spriteType;
    // Per-axis HD upscale factor (Sprint 7.3, 2026-05-10). DISTORTED_SPRITE
    // BR-vertex extension uses scaleX/scaleY for axis-aligned cases so a
    // non-uniform aspect ratio extends each side by exactly 1 Saturn screen
    // cell on its own axis. Mirrors forward shader's BR extension formula.
    float scaleX;
    float scaleY;
} pc;

shared uint sortedCmds[MAX_CMDS_PER_TILE];
shared uint tileCmdCount;

uint vramByte(uint addr) {
    uint w = vram[addr >> 2];
    return (w >> ((addr & 3u) * 8u)) & 0xFFu;
}
uint vramWord(uint addr) {
    return (vramByte(addr) << 8) | vramByte(addr + 1u);
}
// F18: VDP1COLOR layout in framebuffer pixel:
//   alpha byte: bit 7 = S, bit 6 = C, bits 5:3 = colorcl, bits 2:0 = priority
//   B byte:     bit 7 = shadow, bit 6 = sprite_window
uint encodeAlphaByte(uint c, uint colorcl, uint priority) {
    return 0x80u | ((c & 1u) << 6) | ((colorcl & 7u) << 3) | (priority & 7u);
}
uint encodeBByte(uint shadow, Vdp1Cmd cmd) {
    uint sw = (((cmd.vdp2Attrs >> 16) & ATTR_SPRITE_WINDOW) != 0u) ? 0x40u : 0u;
    return ((shadow & 1u) << 7) | sw;
}
vec4 packDirect(uint rgb15, Vdp1Cmd cmd) {
    float r = float((rgb15      ) & 0x1Fu) * 8.0 / 255.0;
    float g = float((rgb15 >>  5) & 0x1Fu) * 8.0 / 255.0;
    float b = float((rgb15 >> 10) & 0x1Fu) * 8.0 / 255.0;
    uint a = encodeAlphaByte(0u, cmd.colorcl, cmd.priority);
    return vec4(r, g, b, float(a) / 255.0);
}
vec4 packPalette(uint colorindex, Vdp1Cmd cmd) {
    float r = float(colorindex & 0xFFu) / 255.0;
    float g = float((colorindex >> 8) & 0xFFu) / 255.0;
    float b = float(encodeBByte(0u, cmd)) / 255.0;
    uint a = encodeAlphaByte(1u, cmd.colorcl, cmd.priority);
    return vec4(r, g, b, float(a) / 255.0);
}
vec4 packShadow(Vdp1Cmd cmd) {
    float b = float(encodeBByte(1u, cmd)) / 255.0;
    uint a = encodeAlphaByte(1u, 0u, cmd.priority);
    return vec4(0.0, 0.0, b, float(a) / 255.0);
}
bool isShadowFor(uint colorindex, Vdp1Cmd cmd) {
    uint normalShadow = cmd.vdp2Attrs & 0xFFFFu;
    bool msb = ((cmd.vdp2Attrs >> 16) & ATTR_MSB_SHADOW) != 0u;
    return msb || (normalShadow != 0u && colorindex == normalShadow);
}
// 4bpp LUT mode only: sprite-type-dependent priority/colorcl/normalshadow
// extraction from a non-RGB LUT word (mirrors vidshared.h::Vdp1GetSpritePixelInfo
// / Vdp1Renderer.cpp case 1 line ~3953).
void getLutSpriteInfo(uint pixel, uint type,
                      out uint outPriority, out uint outColorcl,
                      out uint outMaskedColor, out bool outNormalShadow) {
    uint pri = 0u;
    uint cc = 0u;
    uint masked = pixel;
    bool ns = false;
    if (type == 0x0u) {
        pri = pixel >> 14;
        cc  = (pixel >> 11) & 0x7u;
        masked = pixel & 0x7FFu;
        ns = (masked == 0x7FEu);
    } else if (type == 0x1u) {
        pri = pixel >> 13;
        cc  = (pixel >> 11) & 0x3u;
        masked = pixel & 0x7FFu;
        ns = (masked == 0x7FEu);
    } else if (type == 0x2u) {
        pri = (pixel >> 14) & 0x1u;
        cc  = (pixel >> 11) & 0x7u;
        masked = pixel & 0x7FFu;
        ns = (masked == 0x7FEu);
    } else if (type == 0x3u) {
        pri = (pixel >> 13) & 0x3u;
        cc  = (pixel >> 11) & 0x3u;
        masked = pixel & 0x7FFu;
        ns = (masked == 0x7FEu);
    } else if (type == 0x4u) {
        pri = (pixel >> 13) & 0x3u;
        cc  = (pixel >> 10) & 0x7u;
        masked = pixel & 0x3FFu;
        ns = (masked == 0x3FEu);
    } else if (type == 0x5u) {
        pri = (pixel >> 12) & 0x7u;
        cc  = (pixel >> 11) & 0x1u;
        masked = pixel & 0x7FFu;
        ns = (masked == 0x7FEu);
    } else if (type == 0x6u) {
        pri = (pixel >> 12) & 0x7u;
        cc  = (pixel >> 10) & 0x3u;
        masked = pixel & 0x3FFu;
        ns = (masked == 0x3FEu);
    } else if (type == 0x7u) {
        pri = (pixel >> 12) & 0x7u;
        cc  = (pixel >> 9)  & 0x7u;
        masked = pixel & 0x1FFu;
        ns = (masked == 0x1FEu);
    } else if (type == 0x8u) {
        pri = (pixel >> 7) & 0x1u;
        masked = pixel & 0x7Fu;
        ns = (masked == 0x7Eu);
    } else if (type == 0x9u) {
        pri = (pixel >> 7) & 0x1u;
        cc  = (pixel >> 6) & 0x1u;
        masked = pixel & 0x3Fu;
        ns = (masked == 0x3Eu);
    } else if (type == 0xAu) {
        pri = (pixel >> 6) & 0x3u;
        masked = pixel & 0x3Fu;
        ns = (masked == 0x3Eu);
    } else if (type == 0xBu) {
        cc = (pixel >> 6) & 0x3u;
        masked = pixel & 0x3Fu;
        ns = (masked == 0x3Eu);
    } else if (type == 0xCu) {
        pri = (pixel >> 7) & 0x1u;
        masked = pixel & 0xFFu;
        ns = (masked == 0xFEu);
    } else if (type == 0xDu) {
        pri = (pixel >> 7) & 0x1u;
        cc  = (pixel >> 6) & 0x1u;
        masked = pixel & 0xFFu;
        ns = (masked == 0xFEu);
    } else if (type == 0xEu) {
        pri = (pixel >> 6) & 0x3u;
        masked = pixel & 0xFFu;
        ns = (masked == 0xFEu);
    } else if (type == 0xFu) {
        cc = (pixel >> 6) & 0x3u;
        masked = pixel & 0xFFu;
        ns = (masked == 0xFEu);
    }
    outPriority     = pri;
    outColorcl      = cc;
    outMaskedColor  = masked;
    outNormalShadow = ns;
}
vec4 packPaletteWithAttrs(uint colorindex, uint priority, uint colorcl, Vdp1Cmd cmd) {
    float r = float(colorindex & 0xFFu) / 255.0;
    float g = float((colorindex >> 8) & 0xFFu) / 255.0;
    float b = float(encodeBByte(0u, cmd)) / 255.0;
    uint a = encodeAlphaByte(1u, colorcl, priority);
    return vec4(r, g, b, float(a) / 255.0);
}
vec4 packShadowWithPri(uint priority, Vdp1Cmd cmd) {
    float b = float(encodeBByte(1u, cmd)) / 255.0;
    uint a = encodeAlphaByte(1u, 0u, priority);
    return vec4(0.0, 0.0, b, float(a) / 255.0);
}
// End-code (F9): when CMDPMOD bit 7 (ECD) is CLEAR, "all bits 1" within the
// cm bit width terminates the row in real Saturn hardware (2nd occurrence).
// MVP approximation: treat any end-code occurrence as transparent -- per-row
// state would break per-pixel parallelism. See vidsoft.c:2566 for cm-by-cm
// reference values; cm=3 here uses 0x7F (the actual 7-bit mask) so the check
// can ever fire (vidsoft's 0xFF was dead code due to Pattern128 masking).
vec4 sample16bppRGB(Vdp1Cmd cmd, vec2 uv, uint v, bool spd, bool endcEnabled) {
    uvec2 charSize = cmd.charSize;
    uint u = uint(clamp(uv.x * float(charSize.x), 0.0, float(charSize.x) - 1.0));
    if (endcEnabled) {
        uint endcnt = 0u;
        // Word-cached scan: 2 consecutive 16bpp texels share a 32-bit VRAM word.
        // Cache it -> ~2x fewer SSBO loads on the O(u) end-code scan. The two
        // bytes of a 16bpp texel are always within one word (texel is 2-byte
        // aligned), so the big-endian assembly matches vramWord() exactly.
        uint ecW = 0u; uint ecWi = 0xFFFFFFFFu;
        for (uint i = 0u; i < u; ++i) {
            uint a = cmd.srca * 8u + (v * charSize.x + i) * 2u;
            uint wi = a >> 2;
            if (wi != ecWi) { ecW = vram[wi]; ecWi = wi; }
            uint b0 = (ecW >> ((a & 3u) * 8u)) & 0xFFu;
            uint b1 = (ecW >> (((a + 1u) & 3u) * 8u)) & 0xFFu;
            uint wd = (b0 << 8) | b1;
            if (wd == 0x7FFFu) { endcnt++; if (endcnt >= 2u) return vec4(0); }
        }
    }
    uint addr = cmd.srca * 8u + (v * charSize.x + u) * 2u;
    uint word = vramWord(addr);
    // Graphics path (Vdp1Renderer.cpp case 5 line 4132 onward):
    //   bit 15 clear AND SPD clear      -> transparent
    //   dot == 0x7FFF AND ECD clear     -> end-code (transparent)
    //   bit 15 set AND SPCTL bit 5 set  -> C=0 RGB direct
    //   otherwise                       -> C=1 palette with 16-bit dot as
    //                                     colorindex (line 4144)
    // Screen-clear sprites (SPD=1, CMDCOLR=0, low-bit data) hit the
    // "otherwise" branch and need C=1 palette, not C=0 direct.
    if ((word & 0x8000u) == 0u && !spd) return vec4(0);
    if (endcEnabled && word == 0x7FFFu) return vec4(0);
    bool spctlRgb = (SPEC_SPRITE_TYPE & 0x20u) != 0u;
    if ((word & 0x8000u) != 0u && spctlRgb) {
        if (isShadowFor(word, cmd)) return packShadow(cmd);
        return packDirect(word, cmd);
    }
    // 16bpp palette pixel: per-pixel Vdp1ProcessSpritePixel (getLutSpriteInfo),
    // mirroring the 4bpp LUT path / vidogl.c readTexture. packPalette(word) kept
    // the command-level priority/colorcl and the full unmasked 16-bit color
    // index, so palette sprite types > 0 (Mr. Bones forest skeleton = type 7)
    // lost the per-pixel priority slot (dot>>12)&7 and were discarded when that
    // PRISA slot held 0.
    uint pri; uint cc; uint masked; bool ns;
    getLutSpriteInfo(word, SPEC_SPRITE_TYPE & 0xFu, pri, cc, masked, ns);
    bool msb = ((cmd.vdp2Attrs >> 16) & ATTR_MSB_SHADOW) != 0u;
    if (msb || ns) return packShadowWithPri(pri, cmd);
    return packPaletteWithAttrs(masked, pri, cc, cmd);
}
)GLSL"
R"GLSL(
vec4 sample4bppBank(Vdp1Cmd cmd, vec2 uv, uint v, uint colorBank, bool spd, bool endcEnabled) {
    uvec2 charSize = cmd.charSize;
    uint u = uint(clamp(uv.x * float(charSize.x), 0.0, float(charSize.x) - 1.0));
    if (endcEnabled) {
        uint endcnt = 0u;
        // Word-cached scan: the per-texel vramByte() re-fetches the same 32-bit
        // VRAM word for 8 consecutive 4bpp texels. Cache it and only reload when
        // the word index changes -> ~8x fewer SSBO loads on this O(u) per-pixel
        // end-code scan (the dominant texel-fetch cost at HD upscale). Output is
        // bit-identical to the byte-at-a-time path.
        uint ecW = 0u; uint ecWi = 0xFFFFFFFFu;
        for (uint i = 0u; i < u; ++i) {
            uint a = cmd.srca * 8u + ((v * charSize.x + i) >> 1);
            uint wi = a >> 2;
            if (wi != ecWi) { ecW = vram[wi]; ecWi = wi; }
            uint bi = (ecW >> ((a & 3u) * 8u)) & 0xFFu;
            uint di = ((i & 1u) == 0u) ? ((bi >> 4) & 0xFu) : (bi & 0xFu);
            if (di == 0xFu) { endcnt++; if (endcnt >= 2u) return vec4(0); }
        }
    }
    uint byteIdx = (v * charSize.x + u) >> 1;
    uint b = vramByte(cmd.srca * 8u + byteIdx);
    uint dot = ((u & 1u) == 0u) ? ((b >> 4) & 0xFu) : (b & 0xFu);
    if (endcEnabled && dot == 0xFu) return vec4(0);
    if (dot == 0u && !spd) return vec4(0);
    uint colorindex = dot | colorBank;
    if (isShadowFor(colorindex, cmd)) return packShadow(cmd);
    // SPCTL bit 5 (sprite-RGB-enable) + colorBank bit 0x8000 -> direct RGB
    // (vidogl.c:653-657, 4bpp Bank case 0).
    bool spctlRgb_4b = (SPEC_SPRITE_TYPE & 0x20u) != 0u;
    if ((colorindex & 0x8000u) != 0u && spctlRgb_4b) return packDirect(colorindex, cmd);
    return packPalette(colorindex, cmd);
}
vec4 sample4bppLut(Vdp1Cmd cmd, vec2 uv, uint v, uint colorLut, bool spd, bool endcEnabled) {
    uvec2 charSize = cmd.charSize;
    uint u = uint(clamp(uv.x * float(charSize.x), 0.0, float(charSize.x) - 1.0));
    // End-code terminator scan (F9): when ECD bit is CLEAR, dot=0xF acts as
    // a row terminator -- 1st end-code emits transparent, 2nd end-code makes
    // ALL subsequent pixels in the row transparent. Walk [0,u) and count
    // end-codes; if count >= 2 the current pixel is past the terminator.
    if (endcEnabled) {
        uint endcnt = 0u;
        // Word-cached scan: the per-texel vramByte() re-fetches the same 32-bit
        // VRAM word for 8 consecutive 4bpp texels. Cache it and only reload when
        // the word index changes -> ~8x fewer SSBO loads on this O(u) per-pixel
        // end-code scan (the dominant texel-fetch cost at HD upscale). Output is
        // bit-identical to the byte-at-a-time path.
        uint ecW = 0u; uint ecWi = 0xFFFFFFFFu;
        for (uint i = 0u; i < u; ++i) {
            uint a = cmd.srca * 8u + ((v * charSize.x + i) >> 1);
            uint wi = a >> 2;
            if (wi != ecWi) { ecW = vram[wi]; ecWi = wi; }
            uint bi = (ecW >> ((a & 3u) * 8u)) & 0xFFu;
            uint di = ((i & 1u) == 0u) ? ((bi >> 4) & 0xFu) : (bi & 0xFu);
            if (di == 0xFu) { endcnt++; if (endcnt >= 2u) return vec4(0); }
        }
    }
    uint byteIdx = (v * charSize.x + u) >> 1;
    uint b = vramByte(cmd.srca * 8u + byteIdx);
    uint dot = ((u & 1u) == 0u) ? ((b >> 4) & 0xFu) : (b & 0xFu);
    if (endcEnabled && dot == 0xFu) return vec4(0);
    if (dot == 0u && !spd) return vec4(0);
    uint w = vramWord((dot * 2u + colorLut) & 0x7FFFFu);
    if ((w & 0x8000u) != 0u) {
        // RGB direct LUT entry -- graphics path forces priority=0 here
        // (Vdp1Renderer.cpp:3950).
        if (isShadowFor(w, cmd)) return packShadow(cmd);
        vec4 c = packDirect(w, cmd);
        uint a = encodeAlphaByte(0u, cmd.colorcl, 0u);
        c.a = float(a) / 255.0;
        return c;
    }
    // Palette LUT entry: extract priority/colorcl/normalshadow from the LUT
    // word per VDP2 sprite type (graphics path runs Vdp1ProcessSpritePixel
    // here). Without this, compute leaves attribute bits inside colorindex
    // and uses cmd-level (CMDCOLR-derived) priority/colorcl, producing wrong
    // VDP2 framebuffer pixels for any 4bpp LUT sprite.
    uint pri; uint cc; uint masked; bool ns;
)GLSL"
R"GLSL(
    getLutSpriteInfo(w, SPEC_SPRITE_TYPE & 0xFu, pri, cc, masked, ns);
    bool msb = ((cmd.vdp2Attrs >> 16) & ATTR_MSB_SHADOW) != 0u;
    if (msb || ns) return packShadowWithPri(pri, cmd);
    return packPaletteWithAttrs(masked, pri, cc, cmd);
}
vec4 sample8bppBank(Vdp1Cmd cmd, vec2 uv, uint v, uint colorBank, uint palMask, bool spd, bool endcEnabled) {
    uvec2 charSize = cmd.charSize;
    uint u = uint(clamp(uv.x * float(charSize.x), 0.0, float(charSize.x) - 1.0));
    if (endcEnabled) {
        uint endcnt = 0u;
        // Word-cached scan: 4 consecutive 8bpp texels share a 32-bit VRAM word.
        // Cache it -> ~4x fewer SSBO loads on the O(u) end-code scan. Bit-identical.
        uint ecW = 0u; uint ecWi = 0xFFFFFFFFu;
        for (uint i = 0u; i < u; ++i) {
            uint a = cmd.srca * 8u + v * charSize.x + i;
            uint wi = a >> 2;
            if (wi != ecWi) { ecW = vram[wi]; ecWi = wi; }
            uint di = (ecW >> ((a & 3u) * 8u)) & 0xFFu;
            if ((di & palMask) == palMask) { endcnt++; if (endcnt >= 2u) return vec4(0); }
        }
    }
    uint dot = vramByte(cmd.srca * 8u + v * charSize.x + u);
    if (endcEnabled && (dot & palMask) == palMask) return vec4(0);
    if (dot == 0u && !spd) return vec4(0);
    uint colorindex = (dot & palMask) | colorBank;
    if (isShadowFor(colorindex, cmd)) return packShadow(cmd);
    // SPCTL bit 5 (sprite-RGB-enable) + colorBank bit 0x8000 -> direct RGB
    // (mirror of 4bpp Bank handling for 8bpp Bank cases 2/3/4).
    bool spctlRgb_8b = (SPEC_SPRITE_TYPE & 0x20u) != 0u;
    if ((colorindex & 0x8000u) != 0u && spctlRgb_8b) return packDirect(colorindex, cmd);
    return packPalette(colorindex, cmd);
}
)GLSL"
R"GLSL(

// Coverage = strict inside  OR  edge supercover. Each of the 4 edges is treated
// as a finite segment; pixels within sqrt(thrSq) HD px of any segment are
// filled. Because we use distance-to-segment (not distance-to-infinite-
// line), the filled set is bounded by the polygon's convex hull + half-
// disc caps at vertices -- never extends past the geometric extent in the
// way a uniform inside-test bias does.
//
// Threshold scales with HD resolution: SUPERCOVER_HALF_SAT (in Saturn px)
// is the half-thickness of the edge band, multiplied by scaleMax to convert
// to HD px. Tuned empirically (0.38 -> ~0.76 Saturn cell band) so cmd 278
// style degenerate triangles fill ~95% of bbox at HD scale 4 without the
// edge band becoming too aggressive.
//
// MIN_SUPERCOVER_HD ensures sub-pixel rounding is always handled even at
// scaleMax = 1 (~half HD cell diagonal floor).
//
// NOTE: applyCmd ALSO bbox-clamps every pixel before this test, so the
// perpendicular overflow that segment supercover would otherwise produce
// at edges sitting on the bbox boundary is bounded to zero. The threshold
// here only controls fill rate within bbox -- it does not affect overflow.
const float SUPERCOVER_HALF_SAT = 0.58;
const float MIN_SUPERCOVER_HD   = 0.555;

float edgeFn(vec2 p, vec2 v0, vec2 v1) {
    return (v1.x - v0.x) * (p.y - v0.y) - (v1.y - v0.y) * (p.x - v0.x);
}

// Squared distance from point p to the line SEGMENT a->b. Bounded by
// segment endpoints (clamping t to [0,1]) so the supercover cannot extend
// past the segment's geometric end.
float distSqToSegment(vec2 p, vec2 a, vec2 b) {
    vec2 ab = b - a;
    float lenSq = dot(ab, ab);
    if (lenSq < 1e-6) {
        vec2 dp = p - a;
        return dot(dp, dp);
    }
    float t = clamp(dot(p - a, ab) / lenSq, 0.0, 1.0);
    vec2 d = p - (a + t * ab);
    return dot(d, d);
}

// Saturn-cell-inclusive vertex semantics: vertex (cx, cy) covers Saturn
// cell (cx, cy) which at HD upscale spans fb [cx*sX, (cx+1)*sX-1] x
// [cy*sY, (cy+1)*sY-1]. We do NOT shift encoder vertices (that breaks
// triangles / lines / twisted quads where A is not a TL anchor). Instead,
// each edge whose outward normal points "+x or +y" (= bottom-right facing)
// is dilated by ~scaleMax fb pixels so the polygon covers the last Saturn
// cell on the BR side.
//
// Per-axis gating: DILATE_BR_X enables right-facing extension, DILATE_BR_Y
// enables bottom-facing extension. Host applyState sets each independently
// based on bbox width / height >= scaleMax * MIN_DILATION_BBOX_CELLS so
// wide-thin polygons (e.g., 100x1 cells) only extend along the long axis.
// For tilted edges with both outward components positive, dilation
// applies if EITHER flag is set (conservative: extending once is better
// than not extending at all on a diagonal BR-facing edge).
//
// outward = sgn * vec2(e.y, -e.x) where e = b-a. sgn auto-detects polygon
// winding (CW vs CCW). Zero-length edges (degenerate triangles, lines)
// yield length(b-a)=0 so the dilation term collapses to 0 -- safe for all
// polygon types.
//
// Perpendicular distance from p to edge a->b equals |edgeFn(p,a,b)| / |b-a|
// so requiring dilation d means edgeFn(p,a,b)*sgn >= -d * |b-a|.
float brEdgeDilation(vec2 a, vec2 b, float sgn, uint flags) {
    vec2 e = b - a;
    vec2 outward = sgn * vec2(e.y, -e.x);
    bool wantX = (outward.x > 0.0) && (flags & VDP1C_FLAG_DILATE_BR_X) != 0u;
    bool wantY = (outward.y > 0.0) && (flags & VDP1C_FLAG_DILATE_BR_Y) != 0u;
    if (!wantX && !wantY) return 0.0;
    // scaleMax-0.5 because pixel center at fb (last_pix + 0.5) needs to land
    // inside the dilated edge; min 0.5 keeps scale=1 inclusive (1px polygon
    // edge at y=N still includes pixel y=N).
    return max(pc.scaleMax - 0.5, 0.5);
}

bool insideStrict(vec2 p, vec2 a, vec2 b, vec2 c, vec2 d, uint flags) {
    float sgn = edgeFn(c, a, b) >= 0.0 ? 1.0 : -1.0;
    return (edgeFn(p, a, b) * sgn >= -brEdgeDilation(a, b, sgn, flags) * length(b - a)) &&
           (edgeFn(p, b, c) * sgn >= -brEdgeDilation(b, c, sgn, flags) * length(c - b)) &&
           (edgeFn(p, c, d) * sgn >= -brEdgeDilation(c, d, sgn, flags) * length(d - c)) &&
           (edgeFn(p, d, a) * sgn >= -brEdgeDilation(d, a, sgn, flags) * length(a - d));
}

bool insideOrOnEdge(vec2 p, vec2 a, vec2 b, vec2 c, vec2 d, float thrSq, uint flags) {
    // 1. Strict inside test (BR-edge dilation handles Saturn-inclusive cells).
    if (insideStrict(p, a, b, c, d, flags)) return true;

    // 2. Edge supercover: 4 finite segments, capped at vertices.
    return distSqToSegment(p, a, b) <= thrSq ||
           distSqToSegment(p, b, c) <= thrSq ||
           distSqToSegment(p, c, d) <= thrSq ||
           distSqToSegment(p, d, a) <= thrSq;
}

float cross2(vec2 a, vec2 b) { return a.x * b.y - a.y * b.x; }
vec2 inverseBilinear(vec2 p, vec2 A, vec2 B, vec2 C, vec2 D) {
    vec2 e0 = B - A;
    vec2 e1 = D - A;
    vec2 e2 = (A - B + C - D);
    vec2 q  = p - A;
    float a = cross2(e2, e1);
    float b = cross2(e0, e1) - cross2(e2, q);
    float c = cross2(q, e0);
    float t;
    // Relative threshold: treat a as zero when it is much smaller than b.
    // Axis-aligned quads should have e2 = (0, 0) and a = 0, but FP rounding
    // in (A - B + C - D) leaves a small non-zero residue when vertex coords
    // span large positive and negative values combined with non-integer
    // scaleX/scaleY (e.g. flipped scaled sprites at non-integer RES_NATIVE
    // scale). The absolute 1e-6 threshold missed that residue and routed
    // to the quadratic branch, where sqrt(b*b - 4*a*c) with 4ac << b*b
    // suffers catastrophic cancellation -- t collapses to 0 or to a huge
    // value, clamped to [0, 1] as 0 or 1, producing vertical stripes
    // (V stuck on one texture row). Relative scale keeps quadratic for
    // genuine curved quads (a ~ b) while routing FP-tiny a to linear.
    if (abs(a) * 1e6 < abs(b)) {
        if (abs(b) < 1e-6) return vec2(0);
        t = -c / b;
    } else {
        float disc = max(0.0, b * b - 4.0 * a * c);
        float sq = sqrt(disc);
        float t0 = (-b - sq) / (2.0 * a);
        float t1 = (-b + sq) / (2.0 * a);
        // Tie-break (mirrors Vdp1ComputeMath.cpp::inverseBilinear):
        //   1. Both roots in [0, 1]: pick the strictly interior one (closer
        //      to 0.5). Required for degenerate quads (v2==v3 etc.) where
        //      the bilinear has TWO valid roots -- the interior root v/u
        //      and the boundary root t==1 corresponding to the collapsed
        //      vertex. Picking the boundary root sets sden=0 and falls back
        //      to s=0, mapping the entire collapsed texture row to one
        //      screen point.
        //   2. Otherwise, closest to [0, 1] (BR-dilation band continuity).
        float d0 = max(0.0, max(-t0, t0 - 1.0));
        float d1 = max(0.0, max(-t1, t1 - 1.0));
        if (d0 == 0.0 && d1 == 0.0) {
            t = (abs(t0 - 0.5) <= abs(t1 - 0.5)) ? t0 : t1;
        } else {
            t = (d0 <= d1) ? t0 : t1;
        }
    }
    vec2 sden = e0 + e2 * t;
    float s;
    if (abs(sden.x) > abs(sden.y)) {
        s = abs(sden.x) < 1e-6 ? 0.0 : (q.x - e1.x * t) / sden.x;
    } else {
        s = abs(sden.y) < 1e-6 ? 0.0 : (q.y - e1.y * t) / sden.y;
    }
    return vec2(s, t);
}
vec2 computeUV(vec2 P, Vdp1Cmd c) {
    return clamp(inverseBilinear(P, vec2(c.v0), vec2(c.v1), vec2(c.v2), vec2(c.v3)), 0.0, 1.0);
}
)GLSL"
R"GLSL(
// =============================================================================
// Texture row selection (Saturn line stepping). VDP1 draws a textured
// command as dmax + 1 lines between edges A->D and B->C, where dmax is the
// longest Chebyshev length of those two edges, and picks each line's texture
// row with an error term (the reference software renderer, ss/vdp1_common.h
// VileTex::Setup and the per-line PreStep in ss/vdp1_sprite.cpp). Along a
// line the texture column is stepped the same way per dot (ss/vdp1.cpp line
// setup, ss/vdp1_common.h DrawLine). vdp1TexRow is the closed form of that
// stepping; it must match Vdp1ComputeMath.cpp::vdp1TexRow (checked against
// the stepping for every size in vdp1_compute_math_test).
// =============================================================================
int vdp1TexRow(int k, int n, int t0, int t1) {
    int dt = t1 - t0;
    int adt = abs(dt);
    int rev = dt < 0 ? 1 : 0;
    int inc; int adj; int e;
    if (n <= adt) { inc = 2 * (adt + 1); adj = 2 * n;       e = adt + 1 - (2 * n + rev); }
    else          { inc = 2 * adt;       adj = 2 * (n - 1); e = n - (2 * n - rev); }
    e += k * inc;
    int m = (e >= 0 && adj != 0) ? e / adj + 1 : 0;
    return rev != 0 ? t0 - m : t0 + m;
}
// dmax and the A->B dot count minus one, computed on the host from the raw
// Saturn vertices (Vdp1ComputeMath.cpp::packSpriteSteps) and carried in
// Vdp1Cmd::lineSteps; the fb-pixel vertices are truncated after scaling and
// cannot give them back exactly at non-integer scales.
int cmdDmax(Vdp1Cmd c) { return int(c.lineSteps & 0xFFFFu); }
int cmdLenAB(Vdp1Cmd c) { return int(c.lineSteps >> 16); }
// One-line sprite: both side edges have length 0 (A == D, B == C), so the
// hardware draws the single line A->B. The quad has no interior, so the
// inverse bilinear solve degenerates (0/0) and cannot give the column.
bool isOneLineSprite(Vdp1Cmd c) {
    return c.cmdType == TYPE_DISTORTED_SPRITE && c.v0 == c.v3 && c.v1 == c.v2;
}
// Parameter of fb point P along A->B of a one-line sprite, measured at the
// Saturn cell centre (P minus half a cell, see spriteTexRow).
float oneLineParam(vec2 P, Vdp1Cmd c) {
    vec2 e = vec2(c.v1 - c.v0);
    float ee = dot(e, e);
    if (ee <= 0.0) return 0.0;
    vec2 q = P - 0.5 * vec2(pc.scaleX, pc.scaleY) - vec2(c.v0);
    return clamp(dot(q, e) / ee, 0.0, 1.0);
}
// Texture column (flip applied) of the dot of a one-line sprite nearest to P.
uint oneLineTexU(vec2 P, Vdp1Cmd c) {
    int len = cmdLenAB(c);
    int j = int(roundEven(oneLineParam(P, c) * float(len)));
    int w = int(c.charSize.x);
    bool flipH = (c.flip & 1u) != 0u;
    return uint(vdp1TexRow(j, len + 1, flipH ? w - 1 : 0, flipH ? 0 : w - 1));
}
// Texture row (flip applied) of the Saturn line that covers fb point P.
// Vertex (x, y) names Saturn cell (x, y), whose centre is half a cell past
// the vertex in fb space, so the line parameter is solved at P minus half a
// cell; line k then owns parameters within half a line of k / dmax.
uint spriteTexRow(vec2 P, Vdp1Cmd c) {
    int dmax = cmdDmax(c);
    int k = 0;
    if (dmax > 0) {
        vec2 q = P - 0.5 * vec2(pc.scaleX, pc.scaleY);
        float t;
        if (c.v0 == c.v1 && c.v3 == c.v2) {
            // Zero-width quad (A == B, D == C): every line is one dot on
            // the A->D edge, so the line parameter is the projection on it.
            vec2 e = vec2(c.v3 - c.v0);
            t = dot(q - vec2(c.v0), e) / dot(e, e);
        } else {
            t = inverseBilinear(q, vec2(c.v0), vec2(c.v1), vec2(c.v2), vec2(c.v3)).y;
        }
        k = int(roundEven(clamp(t, 0.0, 1.0) * float(dmax)));
    }
    int h = int(c.charSize.y);
    bool flipV = (c.flip & 2u) != 0u;
    return uint(vdp1TexRow(k, dmax + 1, flipV ? h - 1 : 0, flipV ? 0 : h - 1));
}
)GLSL"
R"GLSL(
// F13: 2-triangle coverage / UV for concave or twisted quads. The 4-edge
// inside-test is undefined on those quads (sign of the area sign flips
// depending on which "inside" is meant), so we split into (v0,v1,v2) +
// (v0,v2,v3) and use barycentric UV with the canonical sprite mapping
// A=(0,0), B=(1,0), C=(1,1), D=(0,1).
bool insideTriangle(vec2 p, vec2 a, vec2 b, vec2 c, uint flags) {
    // Degenerate triangle: brEdgeDilation slack across two opposite
    // half-planes through a collapsed vertex (e.g. v2==v3 -> triangle
    // v0,v2,v3) collapses to a 2*scaleMax-wide band along line V0-V2
    // instead of empty coverage, painting a diagonal strip across bbox.
    // edgeFn returns 2x signed area in fb px^2; 1e-4 catches only exact
    // FP-zero, which is the case integer-coincident vertices produce.
    float area2 = edgeFn(c, a, b);
    if (abs(area2) < 1e-4) return false;
    float sgn = area2 >= 0.0 ? 1.0 : -1.0;
    return (edgeFn(p, a, b) * sgn >= -brEdgeDilation(a, b, sgn, flags) * length(b - a)) &&
           (edgeFn(p, b, c) * sgn >= -brEdgeDilation(b, c, sgn, flags) * length(c - b)) &&
           (edgeFn(p, c, a) * sgn >= -brEdgeDilation(c, a, sgn, flags) * length(a - c));
}
vec2 triBary(vec2 p,
             vec2 V1, vec2 V2, vec2 V3,
             vec2 UV1, vec2 UV2, vec2 UV3) {
    float det = (V2.y - V3.y) * (V1.x - V3.x)
              + (V3.x - V2.x) * (V1.y - V3.y);
    if (abs(det) < 1e-6) return UV1;
    float b1 = ((V2.y - V3.y) * (p.x - V3.x)
              + (V3.x - V2.x) * (p.y - V3.y)) / det;
    float b2 = ((V3.y - V1.y) * (p.x - V3.x)
              + (V1.x - V3.x) * (p.y - V3.y)) / det;
    float b3 = 1.0 - b1 - b2;
    return b1 * UV1 + b2 * UV2 + b3 * UV3;
}

)GLSL"
// MSVC raw string literals max out around ~16k bytes; split the embedded
// shader into two segments which the preprocessor concatenates.
R"GLSL(
// Saturn VDP1 Gouraud shading (Sega VDP1 manual section 5.2 / Table 5.3).
// Reads 4 RGB555 vertex colors from VRAM at c.gouraudAddr.x (= CMDGRDA<<3),
// each channel is a 5-bit "table value" T in [0x00, 0x1F]; the correction
// applied to the source pixel's matching channel is exactly (T - 0x10):
//   T=0x00 -> -0x10, T=0x10 -> 0, T=0x1F -> +0x0F  (Manual Table 5.3)
// then the result is clamped in 5-bit space to [0x00, 0x1F].
//
// We do the math in normalized [0, 1] space so it composes with packDirect /
// sample16bppRGB whose 5->8 mapping is value*8/255. The offset scale is the
// same so 5-bit space clamp [0, 31] becomes [0, 248/255]. clamp(_, 0, 1)
// here is conservative (difference is sub-LSB at 8-bit framebuffer
// precision) and tracks how the offscreen rgba8 image is stored.
//
// Vertex word order: A=TL=v0 (word 0), B=TR=v1 (word 1), C=BR=v2 (word 2),
// D=BL=v3 (word 3). bit layout (Manual Fig 5.4):
//   bits 14:10 = B, 9:5 = G, 4:0 = R; bit 15 = X (ignored).
//
// Spec note: "Valid only for RGB color codes. Color is not guaranteed when
// Gouraud shading is specified for the color code of the color bank code."
// We apply the offset uniformly to both packDirect (RGB) and packPalette
// (palette index) outputs; for the palette path the result is unspecified
// per Saturn manual but matches what the existing graphics path does.
vec3 applyGouraud(vec3 srcRgb, vec2 uv, Vdp1Cmd c) {
    if (SPEC_USE_GOURAUD == 0u) return srcRgb;
    if (c.gouraudAddr.y == 0u) return srcRgb;
    uint addr = c.gouraudAddr.x;
    vec3 g[4];
    for (int i = 0; i < 4; ++i) {
        uint w = vramWord(addr + uint(i) * 2u);
        g[i] = (vec3(float(w & 0x1Fu),
                     float((w >> 5u) & 0x1Fu),
                     float((w >> 10u) & 0x1Fu)) - 16.0) * (8.0 / 255.0);
    }
    vec3 top  = mix(g[0], g[1], uv.x);
    vec3 bot  = mix(g[3], g[2], uv.x);
    vec3 offs = mix(top, bot, uv.y);
    return clamp(srcRgb + offs, 0.0, 1.0);
}

// Saturn VDP1 CMDPMOD blending. Operates entirely in linear RGB space so
// half-luminance / half-transparent are arithmetic (>> 1 / average) rather
// than gamma-correct.
//   bits 2:0 (Color Calc):
//     0 = replace, 1 = shadow, 2 = half-luminance, 3 = half-transparent,
//     4..7 = gouraud variants (Color Calc + Gouraud shading; the gouraud
//            offset itself is applied earlier in applyCmd via applyGouraud,
//            so 4..7 here behave like 0..3 with the offset already baked in).
//   bit 8       (Mesh): drop pixel on (x XOR y) odd checkerboard.
//   bit 15      (MSB shadow): mark dst alpha so VDP2 can treat the pixel
//                             as a shadow target downstream.
//
// Saturn fed CMDPMOD bits 6:0 = SP/MS so the CC nibble is bits 2:0 only;
// the alpha-channel handling preserves dst.a for shadow (since shadow does
// not write a new color), and src.a otherwise.
vec4 applyBlend(vec4 src, vec4 dst, uint pmod, ivec2 pix) {
    if (SPEC_USE_MESH != 0u) {
        if ((pmod & 0x100u) != 0u && ((pix.x ^ pix.y) & 1) == 0) return dst;
    }

    // Bit 2 of CMDPMOD selects Gouraud shading (handled in applyGouraud);
    // the actual blend mode is the LOW 2 bits, matching vidsoft IS_REPLACE /
    // IS_HALF_LUMINANCE / etc. macros that pair (cc & 0x3) values 0/4, 1/5,
    // 2/6, 3/7. Without masking to 2 bits, cc=5..7 would all fall into the
    // Replace branch and silently drop Shadow / Half-lum / Half-trans for
    // Gouraud-shaded sprites.
    uint cc = pmod & 0x3u;
    vec4 result;
    if (cc == 1u) {
        // Shadow: only darken sprite-drawn RGB-direct pixels (S=1, C=0).
        // Matches tessellation VDP1Shadow fragment shader's
        // `(dst.a & 0xC0) == 0x80` gate. Skipping the gate corrupts palette
        // pixels because dst.r/g carry the colorindex bytes (not displayable
        // RGB), so multiplying them halves the colorindex and the VDP2 FB
        // reader resolves the wrong CRAM entry.
        uint dstA = uint(dst.a * 255.0);
        if ((dstA & 0xC0u) != 0x80u) return dst;
        result = vec4(dst.rgb * 0.5, dst.a);
    }
    else if (cc == 2u) result = vec4(src.rgb * 0.5, src.a);             // Half-luminance
    else if (cc == 3u) {
        // Half-transparent: only blend when destination is RGB-direct
        // (encodeAlphaByte's 'c' bit 6 is clear). When dst is palette-indexed
        // (bit 6 set) the dst.r/g channels store a palette index, not RGB,
        // so RGB averaging would corrupt them -- fall back to replace, which
        // is what VDP1GlowShadingAndHalfTransOperation does in the graphics
        // pipeline (VdpPipeline.cpp `(additional & 0x40) == 0` branch).
        uint dstA = uint(dst.a * 255.0);
        if ((dstA & 0x40u) == 0u) {
            result = vec4((src.rgb + dst.rgb) * 0.5, src.a);
        } else {
            result = src;
        }
    }
    else               result = src;                                     // Replace

    if (SPEC_USE_MSB_SHADOW != 0u) {
        if ((pmod & 0x8000u) != 0u) result.a = 1.0;
    }
    return result;
}

// Distance-squared based "on this segment" test reused by Polyline / Line
// rasterization. Same supercover threshold as the quad path so a single
// horizontal Polyline edge looks identical to a 1px-thick polygon strip.
bool isOnSegment(vec2 P, vec2 a, vec2 b, float thrSq) {
    return distSqToSegment(P, a, b) <= thrSq;
}

vec4 applyCmd(ivec2 pix, Vdp1Cmd c, vec4 dst) {
    // Cheapest cull first: bbox (4 comparisons, no branch chain). Most cmds
    // in a tile only touch a small fraction of the tile's 256 pixels so this
    // catches the bulk of negative cases before more elaborate clip tests.
    // Reordered from the original "systemClip -> userClip -> bbox" path
    // (2026-05-17) -- shade pass was 79ms / 80ms total at 12fps on SD865,
    // bbox-first ordering buys some divergence-time back even though the
    // outer-loop pre-check below should already short-circuit most non-
    // covering cmds.
    if (pix.x < c.bbox.x || pix.x > c.bbox.z ||
        pix.y < c.bbox.y || pix.y > c.bbox.w) return dst;
    if (pix.x > c.systemClip.x || pix.y > c.systemClip.y) return dst;
    // User clip (CMDPMOD bit 10): clipMode 1 = inside-only, 2 = outside-only.
    // applyState bakes the current state.userClip into each cmd at submission
    // time, so mid-frame UserClip cmds take effect on subsequent draws.
    if (SPEC_USE_USERCLIP != 0u) {
        if (c.clipMode == 1u) {
            if (pix.x < c.userClip.x || pix.x > c.userClip.z ||
                pix.y < c.userClip.y || pix.y > c.userClip.w) return dst;
        } else if (c.clipMode == 2u) {
            if (pix.x >= c.userClip.x && pix.x <= c.userClip.z &&
                pix.y >= c.userClip.y && pix.y <= c.userClip.w) return dst;
        }
    }
    // Pixel-center test point. Saturn-cell-inclusive vertex semantics are
    // handled GEOMETRICALLY here via brEdgeDilation() in insideStrict /
    // insideTriangle: BR-facing edges get dilated by ~scaleMax fb px so the
    // polygon covers the last Saturn cell on the BR side. The encoder does
    // NOT shift vertices (that broke triangles / lines / twisted quads where
    // A is not the TL anchor). bbox.z/.w are extended in applyState so the
    // bbox clamp does not reject pixels in the dilated region.
    vec2 P = vec2(pix) + vec2(0.5);
    float halfHD = max(MIN_SUPERCOVER_HD, SUPERCOVER_HALF_SAT * pc.scaleMax);
    float thrSq  = halfHD * halfHD;
    // Polyline / Line use a full Saturn-cell half-thickness instead of the
    // polygon-tuned 0.58 cell. bbox clamp + BR-shift make the supercover band
    // one-sided (inward only), so to cover the full Saturn cell that the edge
    // represents the inward thickness must equal scaleMax fb px. With 0.58 *
    // scaleMax adjacent polylines on neighbouring Saturn rows leave a visible
    // gap at HD upscale (concentric outline pairs render as two thin lines
    // instead of one thick border).
    float halfHDLine = max(MIN_SUPERCOVER_HD, pc.scaleMax);
    float thrSqLine  = halfHDLine * halfHDLine;

    vec4 src = vec4(0);
    if (c.cmdType == TYPE_POLYLINE) {
        // Closed 4-segment polyline: hit any of v0->v1, v1->v2, v2->v3, v3->v0.
        // Cell-inclusive vertex semantics: BR-edge vertices get shifted to
        // bbox.z/.w so the outline aligns with the matching polygon's filled
        // area at HD upscale (see canonical tile_shade.comp for full
        // rationale).
        vec2 V0 = vec2(c.v0), V1 = vec2(c.v1), V2 = vec2(c.v2), V3 = vec2(c.v3);
        if ((c.flags & VDP1C_FLAG_DILATE_BR_X) != 0u) {
            int origMaxX = max(max(c.v0.x, c.v1.x), max(c.v2.x, c.v3.x));
            if (c.v0.x == origMaxX) V0.x = float(c.bbox.z);
            if (c.v1.x == origMaxX) V1.x = float(c.bbox.z);
            if (c.v2.x == origMaxX) V2.x = float(c.bbox.z);
            if (c.v3.x == origMaxX) V3.x = float(c.bbox.z);
        }
)GLSL"
R"GLSL(
        if ((c.flags & VDP1C_FLAG_DILATE_BR_Y) != 0u) {
            int origMaxY = max(max(c.v0.y, c.v1.y), max(c.v2.y, c.v3.y));
            if (c.v0.y == origMaxY) V0.y = float(c.bbox.w);
            if (c.v1.y == origMaxY) V1.y = float(c.bbox.w);
            if (c.v2.y == origMaxY) V2.y = float(c.bbox.w);
            if (c.v3.y == origMaxY) V3.y = float(c.bbox.w);
        }
        if (!(isOnSegment(P, V0, V1, thrSqLine) ||
              isOnSegment(P, V1, V2, thrSqLine) ||
              isOnSegment(P, V2, V3, thrSqLine) ||
              isOnSegment(P, V3, V0, thrSqLine))) return dst;
        if ((c.color & 0x8000u) != 0u) {
            src = isShadowFor(c.color, c) ? packShadow(c) : packDirect(c.color, c);
        } else {
            src = isShadowFor(c.color, c) ? packShadow(c) : packPalette(c.color, c);
        }
        // Gouraud for closed polyline: bilinear over the 4 corner vertices via
        // screen-space inverseBilinear (same as POLYGON). On segment v0->v1
        // the uv collapses to (t, 0) -> mix(g[0], g[1], t); v1->v2 to (1, t) ->
        // mix(g[1], g[2], t); v2->v3 to (1-t, 1) -> mix(g[3], g[2], 1-t);
        // v3->v0 to (0, 1-t) -> mix(g[3], g[0], 1-t). Matches Saturn manual
        // section 5.2 (4 vertex colors A/B/C/D = v0/v1/v2/v3).
        src.rgb = applyGouraud(src.rgb, computeUV(P, c), c);
    } else if (c.cmdType == TYPE_LINE) {
        // Single segment v0->v1. v2/v3 are filled with v1/v0 by encodeLine
        // so bbox is correct, but we must NOT loop the closed polyline
        // edges or we'd double-rasterize the segment.
        // Same BR-edge dilation as polyline so single LINE commands also
        // cover the cell-inclusive endpoint at HD upscale.
        vec2 V0 = vec2(c.v0), V1 = vec2(c.v1);
        if ((c.flags & VDP1C_FLAG_DILATE_BR_X) != 0u) {
            int origMaxX = max(c.v0.x, c.v1.x);
            if (c.v0.x == origMaxX) V0.x = float(c.bbox.z);
            if (c.v1.x == origMaxX) V1.x = float(c.bbox.z);
        }
        if ((c.flags & VDP1C_FLAG_DILATE_BR_Y) != 0u) {
            int origMaxY = max(c.v0.y, c.v1.y);
            if (c.v0.y == origMaxY) V0.y = float(c.bbox.w);
            if (c.v1.y == origMaxY) V1.y = float(c.bbox.w);
        }
        if (!isOnSegment(P, V0, V1, thrSqLine)) return dst;
        if ((c.color & 0x8000u) != 0u) {
            src = isShadowFor(c.color, c) ? packShadow(c) : packDirect(c.color, c);
        } else {
            src = isShadowFor(c.color, c) ? packShadow(c) : packPalette(c.color, c);
        }
        // Gouraud for single line: project P onto v0->v1, use t in uv.x with
        // uv.y=0 so applyGouraud reduces to mix(g[0], g[1], t). Cannot rely
        // on inverseBilinear here -- encodeLine emits a degenerate quad
        // (v2=v1, v3=v0) and the Gouraud table's g[2]/g[3] entries are
        // garbage for LINE commands.
        vec2 ab = vec2(c.v1) - vec2(c.v0);
        float denom = max(dot(ab, ab), 1e-6);
        float t = clamp(dot(P - vec2(c.v0), ab) / denom, 0.0, 1.0);
        src.rgb = applyGouraud(src.rgb, vec2(t, 0.0), c);
)GLSL"
R"GLSL(
    } else {
        // POLYGON / NORMAL_SPRITE / SCALED_SPRITE / DISTORTED_SPRITE.
        vec2 uv;
        // Sprint 7.3 (2026-05-10): DISTORTED_SPRITE BR-vertex extension,
        // ported from vdp1_compute_forward.comp. Saturn raw distorted-sprite
        // verts encode v0..v3 as INCLUSIVE corner positions. Without
        // extension, adjacent meshes (e.g. flag DISTORTED_SPRITE grids)
        // leave a 1-fb-px black gap on every shared edge. brEdgeDilation
        // alone cannot fix this -- applyState explicitly does not set
        // VDP1C_FLAG_DILATE_BR_X/Y for DISTORTED_SPRITE. Extend BR verts
        // in-shader for COVERAGE only; UV mapping uses ORIGINAL c.v0..v3
        // via computeUV(P, c) so the texture is not stretched.
        vec2 v0e = vec2(c.v0);
        vec2 v1e = vec2(c.v1);
        vec2 v2e = vec2(c.v2);
        vec2 v3e = vec2(c.v3);
        if (c.cmdType == TYPE_DISTORTED_SPRITE) {
            bool axisAligned = (c.v0.y == c.v1.y) && (c.v3.y == c.v2.y)
                            && (c.v0.x == c.v3.x) && (c.v1.x == c.v2.x);
            if (axisAligned) {
                // Saturn-cell-inclusive: extend +X / +Y in screen space.
                // sign(v1.x-v0.x) inverted for flipped 2-point scaled
                // sprites (CMDXC<CMDXA) and pushed coverage onto the
                // screen-LEFT edge, cloning texels there.
                // Per-axis degenerate fallback: NormalSprite w==1 / h==1
                // collapses all 4 corners onto the same coord on that
                // axis under cell-inclusive vertex convention. The
                // max-based check would flag all 4 corners as the +X /
                // +Y edge and extend them together, leaving the quad
                // degenerate (TRI_SPLIT path then writes nothing because
                // both triangles have zero area). Fall back to the
                // TL/TR/BR/BL vertex-order convention (v1,v2 are the
                // +X corners; v2,v3 are the +Y corners) on the degenerate
                // axis only.
                int minXi = min(min(c.v0.x, c.v1.x), min(c.v2.x, c.v3.x));
                int maxXi = max(max(c.v0.x, c.v1.x), max(c.v2.x, c.v3.x));
                int minYi = min(min(c.v0.y, c.v1.y), min(c.v2.y, c.v3.y));
                int maxYi = max(max(c.v0.y, c.v1.y), max(c.v2.y, c.v3.y));
                if (minXi < maxXi) {
                    if (c.v0.x == maxXi) v0e.x = float(c.v0.x) + pc.scaleX;
                    if (c.v1.x == maxXi) v1e.x = float(c.v1.x) + pc.scaleX;
                    if (c.v2.x == maxXi) v2e.x = float(c.v2.x) + pc.scaleX;
                    if (c.v3.x == maxXi) v3e.x = float(c.v3.x) + pc.scaleX;
                } else {
                    v1e.x = float(c.v1.x) + pc.scaleX;
                    v2e.x = float(c.v2.x) + pc.scaleX;
                }
                if (minYi < maxYi) {
                    if (c.v0.y == maxYi) v0e.y = float(c.v0.y) + pc.scaleY;
                    if (c.v1.y == maxYi) v1e.y = float(c.v1.y) + pc.scaleY;
                    if (c.v2.y == maxYi) v2e.y = float(c.v2.y) + pc.scaleY;
                    if (c.v3.y == maxYi) v3e.y = float(c.v3.y) + pc.scaleY;
                } else {
                    v2e.y = float(c.v2.y) + pc.scaleY;
                    v3e.y = float(c.v3.y) + pc.scaleY;
                }
            } else if (c.charSize.x > 1u && c.charSize.y > 1u) {
                // Non-axis-aligned: per-texel edge step needs divX/divY > 0.
                // charSize 1xN / Nx1 rotated distorted sprite would be a
                // mathematical strip; rare in Saturn content and skipped.
                float divX = float(c.charSize.x - 1u);
                float divY = float(c.charSize.y - 1u);
                vec2 step_top   = (vec2(c.v1) - vec2(c.v0)) / divX;
                vec2 step_bot   = (vec2(c.v2) - vec2(c.v3)) / divX;
                vec2 step_left  = (vec2(c.v3) - vec2(c.v0)) / divY;
                vec2 step_right = (vec2(c.v2) - vec2(c.v1)) / divY;
                // Raw per-texel step (no inflation). Inflating to scaleMax
                // distorted small polygons (cmd 46: 16x47 texels into a ~5x12
                // fb-px quad) because v2e = v2 + step_bot + step_right sums
                // two inflated vectors, pushing v2 by ~2 Saturn cells past
                // its raw position. Adjacent-poly seam closure relies on the
                // EDGE_BAND_HD = 0.5 fb-px band check below instead.
                v1e = vec2(c.v1) + step_top;
                v3e = vec2(c.v3) + step_left;
                v2e = vec2(c.v2) + step_bot + step_right;
                if (c.v2.x == c.v3.x && c.v2.y == c.v3.y) {
                    v2e = vec2(c.v2);
                    v3e = vec2(c.v3);
                }
                if (c.v1.x == c.v2.x && c.v1.y == c.v2.y) {
                    v1e = vec2(c.v1);
                    v2e = vec2(c.v2);
                }
            }
        }
        // FLAG_THIN takes priority over FLAG_TRI_SPLIT. Saturn-style thin
        // sprite meshes are often degenerate-twisted (3 verts collinear +
        // 1 offset) which trips isConcave, but the polygon's actual shape
        // is a sliver fully covered by bbox + supercover. Routing thin
        // twisted polys through the F13 2-triangle path would lose the
        // supercover edge-band that closes inter-poly gaps; routing them
        // through insideOrOnEdge handles them correctly because the
        // degenerate quad is bounded by its bbox anyway.
        if ((c.flags & FLAG_THIN) != 0u) {
            // FLAG_THIN polygon (<=2 Saturn px geometric thickness):
            // strict-inside  OR  edge-supercover so NiGHTS-style diagonal
            // sprite meshes close their tiny inter-poly gaps even when
            // both edges round to the same screen pixel.
            if (!insideOrOnEdge(P, v0e, v1e, v2e, v3e, thrSq, c.flags)) return dst;
            uv = isOneLineSprite(c) ? vec2(oneLineParam(P, c), 0.0)
                                    : computeUV(P, c);
        } else if ((c.flags & FLAG_TRI_SPLIT) != 0u) {
            // F13: concave / twisted / degenerate quad split into triangles
            // (v0,v1,v2) and (v0,v2,v3). Coverage = union of both triangles;
            // for degenerate quads (vertex collapsed) one triangle has zero
            // area and insideTriangle's area2 < 1e-4 guard rejects it.
            bool t1in = insideTriangle(P, v0e, v1e, v2e, c.flags);
            bool t2in = !t1in && insideTriangle(P, v0e, v2e, v3e, c.flags);
            if (!t1in && !t2in) return dst;
            // UV: degenerate quad (any consecutive vertex pair coincident)
            // must use bilinear inverse. triBary with TL/TR/BR + TL/BR/BL
            // sprite-corner UVs maps each triangle to HALF the texture; for
            // a degenerate quad the second triangle is empty so only HALF
            // the texture is sampled, and screen content shifts vs. forward
            // mapping. Bilinear inverse instead warps the WHOLE texture into
            // the surviving triangle with the texture row collapsed at the
            // degenerate vertex -- matches the per-texel forward path.
            // Non-degenerate concave/twisted quads keep triBary because the
            // 2-triangle union does cover the whole texture there.
            bool degenerateQuad =
                (c.v0 == c.v1) || (c.v1 == c.v2) ||
                (c.v2 == c.v3) || (c.v3 == c.v0);
            if (degenerateQuad) {
                // Two flavors of degenerate quad reach here:
                //   1. Saturn triangle (single collapsed edge, e.g. v2==v3):
                //      original c.v0..v3 still spans an axis-aligned bbox
                //      with non-zero area on both axes. inverseBilinear on
                //      c.v0..v3 returns proper (s, t) and texel mapping is
                //      not stretched by the BR extension.
                //   2. NormalSprite w==1 / h==1 cell-inclusive collapse
                //      (BOTH v1==v2 AND v3==v0, all 4 verts collinear on
                //      one axis): original c.v0..v3 has zero area on that
                //      axis, inverseBilinear's discriminant is 0 and it
                //      returns (0, 0) -- every pixel would read texel
                //      (0, 0). Use the BR-extended v0e..v3e instead;
                //      they form a proper rectangle after the per-axis
                //      degenerate fallback added in the BR extension.
                int origMinX = min(min(c.v0.x, c.v1.x), min(c.v2.x, c.v3.x));
                int origMaxX = max(max(c.v0.x, c.v1.x), max(c.v2.x, c.v3.x));
                int origMinY = min(min(c.v0.y, c.v1.y), min(c.v2.y, c.v3.y));
                int origMaxY = max(max(c.v0.y, c.v1.y), max(c.v2.y, c.v3.y));
                bool axisFullyCollapsed =
                    (origMinX == origMaxX) || (origMinY == origMaxY);
                if (axisFullyCollapsed) {
)GLSL"
R"GLSL(
                    uv = clamp(inverseBilinear(P, v0e, v1e, v2e, v3e),
                               0.0, 1.0);
                } else {
                    uv = computeUV(P, c);
                }
            } else {
                // UV uses ORIGINAL verts so texel mapping is not stretched
                // by the BR extension.
                vec2 V0 = vec2(c.v0), V1 = vec2(c.v1);
                vec2 V2 = vec2(c.v2), V3 = vec2(c.v3);
                uv = t1in
                    ? triBary(P, V0, V1, V2, vec2(0,0), vec2(1,0), vec2(1,1))
                    : triBary(P, V0, V2, V3, vec2(0,0), vec2(1,1), vec2(0,1));
                uv = clamp(uv, 0.0, 1.0);
            }
        } else {
            // Normal-thickness polygon: strict-inside ONLY. Supercover on
            // a thick polygon would dilate ~1 Saturn px of edge-texel bleed
            // along every edge (inverseBilinear extrapolates past [0,1]
            // then clamps to the polygon's border row/col).
            // Sprint 7.3: DISTORTED_SPRITE additionally accepts pixels
            // within EDGE_BAND_HD (0.5 fb px) of any extended edge. This
            // mirrors forward shader's Bresenham band and closes residual
            // 1-px corner gaps where 4 adjacent mesh polygons meet (e.g.
            // curved flag mesh seams). POLYGON keeps strict-only since it
            // has no texture edge texels to bleed.
            bool covered = insideStrict(P, v0e, v1e, v2e, v3e, c.flags);
            if (!covered && c.cmdType == TYPE_DISTORTED_SPRITE) {
                const float EDGE_BAND_HD = 0.5;
                float bandSq = EDGE_BAND_HD * EDGE_BAND_HD;
                covered = distSqToSegment(P, v0e, v1e) <= bandSq ||
                          distSqToSegment(P, v1e, v2e) <= bandSq ||
                          distSqToSegment(P, v2e, v3e) <= bandSq ||
                          distSqToSegment(P, v3e, v0e) <= bandSq;
            }
            if (!covered) return dst;
            // UV uses EXTENDED v0e..v3e (not original c.v0..v3) so the BR-
            // extension cell maps uniformly into UV space. Otherwise the last
            // texel clamps to UV=1 across the whole extension cell, rendering
            // the last texel row/column at 1.5-2x the height of interior
            // rows on shrunk sprites (cmd 50 NiGHTS title flag).
            uv = clamp(inverseBilinear(P, v0e, v1e, v2e, v3e), 0.0, 1.0);
        }
        if (c.cmdType == TYPE_POLYGON) {
            if ((c.color & 0x8000u) != 0u) {
                src = isShadowFor(c.color, c) ? packShadow(c) : packDirect(c.color, c);
            } else {
                src = isShadowFor(c.color, c) ? packShadow(c) : packPalette(c.color, c);
            }
        } else {
            // Texture lookup uses post-flip uv; Gouraud must NOT see the
            // flip because the gouraud table indexes vertex (A,B,C,D) =
            // (v0,v1,v2,v3) in SCREEN space (manual section 5.2). Keep separate.
            vec2 texUv = uv;
            if ((c.flip & 1u) != 0u) texUv.x = 1.0 - texUv.x;
            // Texture row: the Saturn line stepping (flip included),
            // not a texture-space V. See spriteTexRow.
            uint texV = min(spriteTexRow(P, c), c.charSize.y - 1u);
            texUv = clamp(texUv, 0.0, 1.0);
            // One-line sprite: the column is the per-dot stepping along
            // A->B (flip included), placed at the texel centre so the
            // samplers' floor(uv.x * w) returns it unchanged.
            if (isOneLineSprite(c)) {
                texUv.x = (float(oneLineTexU(P, c)) + 0.5) / float(c.charSize.x);
            }
            uint cm = (c.pmod >> 3) & 7u;
            bool spd = (c.pmod & 0x40u) != 0u;
            // ECD (CMDPMOD bit 7): CLEAR = end-code processing enabled. Only
            // applies to sprite cmds with character pattern; this branch is
            // sprite-only so we always pass it. For polygon/polyline/line the
            // Saturn manual instructs games to set this bit, and our branch
            // above doesn't sample textures, so end-code never fires there.
            bool endc = (c.pmod & 0x80u) == 0u;
            if      (cm == 0u) src = sample4bppBank(c, texUv, texV, c.color & 0xFFF0u, spd, endc);
            else if (cm == 1u) src = sample4bppLut (c, texUv, texV, c.color * 8u,      spd, endc);
            else if (cm == 2u) src = sample8bppBank(c, texUv, texV, c.color & 0xFFC0u, 0x3Fu, spd, endc);
            else if (cm == 3u) src = sample8bppBank(c, texUv, texV, c.color & 0xFF80u, 0x7Fu, spd, endc);
            else if (cm == 4u) src = sample8bppBank(c, texUv, texV, c.color & 0xFF00u, 0xFFu, spd, endc);
            else if (cm == 5u) src = sample16bppRGB(c, texUv, texV, spd, endc);
            else return dst;
            if (src.a < 0.5) return dst;
        }
        // Gouraud offset (table - 0x10 in 5-bit space) applied BEFORE blend
        // so cc=5/6/7 (Gouraud + Shadow/Half-lum/Half-trans) compose. uv is
        // screen-space (pre-flip) so vertices A/B/C/D match TL/TR/BR/BL.
        src.rgb = applyGouraud(src.rgb, uv, c);
    }

    return applyBlend(src, dst, c.pmod, pix);
}
)GLSL"
R"GLSL(
void main() {
    uvec2 tile = gl_WorkGroupID.xy;
    uint  tileIdx = tile.y * pc.numTilesX + tile.x;
    uint  lid = gl_LocalInvocationIndex;

    if (lid == 0u) {
        tileCmdCount = min(tiles[tileIdx].count, uint(MAX_CMDS_PER_TILE));
    }
    barrier();
    uint n = tileCmdCount;
    if (lid < n) {
        sortedCmds[lid] = cmdIdx[tileIdx * uint(MAX_CMDS_PER_TILE) + lid];
    }
    barrier();
    if (lid == 0u) {
        for (uint i = 1u; i < n; ++i) {
            uint key = sortedCmds[i];
            uint j = i;
            while (j > 0u && sortedCmds[j - 1u] > key) {
                sortedCmds[j] = sortedCmds[j - 1u];
                j--;
            }
            sortedCmds[j] = key;
        }
    }
    barrier();

    ivec2 pix = ivec2(tile * uint(TILE_SIZE)) + ivec2(lid & 15u, lid >> 4u);
    if (pix.x < 0 || pix.y < 0 ||
        pix.x >= int(pc.fbWidth) || pix.y >= int(pc.fbHeight)) return;

    // Match scanline-shader convention: image y=0 at top; pix.y is in Saturn
    // coord (y down), so flip vertically when accessing the image.
    ivec2 imgPix = ivec2(pix.x, int(pc.fbHeight) - 1 - pix.y);

    vec4 dst = imageLoad(fb, imgPix);
    for (uint i = 0u; i < n; ++i) {
        uint ci = sortedCmds[i];
        // Hoisted bbox cull: read only the 16-byte bbox before pulling the
        // full ~144-byte cmd struct. The vast majority of cmds binned into a
        // tile do not actually touch this thread's pixel (a sprite covering
        // a corner of the tile has only a few pixels in common with the rest
        // of the workgroup). Skipping the full struct fetch in that case
        // halves the SSBO traffic for the cull-heavy case observed on SD865
        // (shade pass dominated at 79ms / 80ms total in a 282-cmd distorted
        // sprite scene).
        ivec4 bb = cmds[ci].bbox;
        if (pix.x < bb.x || pix.x > bb.z ||
            pix.y < bb.y || pix.y > bb.w) continue;
        Vdp1Cmd c = cmds[ci];
        dst = applyCmd(pix, c, dst);
    }
    imageStore(fb, imgPix, dst);
}
)GLSL";

// =============================================================================
// Phase 1A: Per-texel forward mapping shader (Saturn-faithful).
// Canonical source: yabause/src/vulkan/shaders/vdp1_compute_forward.comp.
// 1 workgroup per cmd, local_size 8x8, 1 thread per texel. Each thread
// computes the texel's screen coverage rect via lerp on the 4 cmd vertices
// and imageStores the texel color across that HD pixel block. No vertex
// gimmicks (no +1 shift, no DILATE flags) -- cell-inclusive emerges naturally
// from `cellsW = max - min + 1` baked into the lerp parameters.
// Phase 1A scope: NORMAL/SCALED/DISTORTED sprite + 4bpp Bank only.
// =============================================================================
constexpr const char* kForwardShaderSrc = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#define TYPE_NOOP             0u
#define TYPE_NORMAL_SPRITE    1u
#define TYPE_SCALED_SPRITE    2u
#define TYPE_DISTORTED_SPRITE 3u
#define TYPE_POLYGON          4u
#define TYPE_POLYLINE         5u
#define TYPE_LINE             6u

// Vdp1Cmd::flags bits - same values as kShadeShaderSrc (FLAG_TRI_SPLIT=1,
// FLAG_THIN=2, VDP1C_FLAG_DILATE_BR_X=4, BR_Y=8). The forward shader only
// references FLAG_THIN (a gate that widens the cover_rect / bbox of a thin
// polygon outward by halfHD).
#define FLAG_THIN             2u

// F18: VDP2 framebuffer encoding extras packed into Vdp1Cmd::vdp2Attrs.
//   bits 0:15  = normalShadow (per-cmd colorindex value, 0 disables)
//   bits 16:31 = attrFlags
#define ATTR_MSB_SHADOW       1u
#define ATTR_SPRITE_WINDOW    2u

struct Vdp1Cmd {
    uint  cmdType;
    uint  pmod;
    uint  color;
    uint  srca;
    uvec2 charSize;
    uvec2 gouraudAddr;
    ivec2 v0, v1, v2, v3;
    ivec4 bbox;
    ivec4 systemClip;
    ivec4 userClip;
    uint  clipMode;
    uint  flip;
    uint  flags;
    uint  vdp2Attrs;
    uint  priority;
    uint  colorcl;
    uint  lineSteps;     // bits 0..15 dmax, bits 16..31 A->B length (raw Saturn)
    uint  pad7;
};

layout(rgba8, binding = 0) uniform image2D fb;
layout(std430, binding = 1) restrict readonly buffer CmdSSBO  { Vdp1Cmd cmds[]; };
layout(std430, binding = 2) restrict readonly buffer VramSSBO { uint vram[]; };

#ifdef TILE_MODE
#define MAX_CMDS_PER_TILE 64
struct TileCount { uint count; uint _pad0; uint _pad1; uint _pad2; };
layout(std430, binding = 3) restrict readonly buffer TileCountSSBO { TileCount tiles[]; };
layout(std430, binding = 4) restrict readonly buffer TileListSSBO  { uint cmdIdx[]; };

// Saturn LIFO sort. Bin shader's atomicAdd races assign slots in GPU
// schedule order, not cmd id order -- without sorting here, two cmds in
// the same tile may run in reverse, producing wrong draw order on
// overlapping sprites. Mirrors tile_shade.comp shared+insertion-sort.
shared uint sortedCmds[MAX_CMDS_PER_TILE];
shared uint tileCmdCount_shared;
#endif

layout(push_constant) uniform Push {
    uint  fbWidth;
    uint  fbHeight;
    float scaleMax;
    uint  spriteType;
    uint  cmdIndex;
    uint  numTilesX;
    uint  tileSize;
    uint  maxCmdsPerTile;
    // Per-axis HD upscale factor; see canonical Push declaration in
    // shaders/vdp1_compute_forward.comp.
    float scaleX;
    float scaleY;
} pc;

uint vramByte(uint addr) {
    uint w = vram[addr >> 2];
    return (w >> ((addr & 3u) * 8u)) & 0xFFu;
}
uint vramWord(uint addr) {
    return (vramByte(addr) << 8) | vramByte(addr + 1u);
}

uint encodeAlphaByte(uint c, uint colorcl, uint priority) {
    return 0x80u | ((c & 1u) << 6) | ((colorcl & 7u) << 3) | (priority & 7u);
}
uint encodeBByte(uint shadow, Vdp1Cmd cmd) {
    uint sw = (((cmd.vdp2Attrs >> 16) & ATTR_SPRITE_WINDOW) != 0u) ? 0x40u : 0u;
    return ((shadow & 1u) << 7) | sw;
}
vec4 packPalette(uint colorindex, Vdp1Cmd cmd) {
    float r = float(colorindex & 0xFFu) / 255.0;
    float g = float((colorindex >> 8) & 0xFFu) / 255.0;
    float b = float(encodeBByte(0u, cmd)) / 255.0;
    uint  a = encodeAlphaByte(1u, cmd.colorcl, cmd.priority);
    return vec4(r, g, b, float(a) / 255.0);
}
vec4 packDirect(uint rgb15, Vdp1Cmd cmd) {
    float r = float((rgb15      ) & 0x1Fu) * 8.0 / 255.0;
    float g = float((rgb15 >>  5) & 0x1Fu) * 8.0 / 255.0;
    float b = float((rgb15 >> 10) & 0x1Fu) * 8.0 / 255.0;
    uint a = encodeAlphaByte(0u, cmd.colorcl, cmd.priority);
    return vec4(r, g, b, float(a) / 255.0);
}
vec4 packShadow(Vdp1Cmd cmd) {
    float b = float(encodeBByte(1u, cmd)) / 255.0;
    uint a = encodeAlphaByte(1u, 0u, cmd.priority);
    return vec4(0.0, 0.0, b, float(a) / 255.0);
}
bool isShadowFor(uint colorindex, Vdp1Cmd cmd) {
    uint normalShadow = cmd.vdp2Attrs & 0xFFFFu;
    bool msb = ((cmd.vdp2Attrs >> 16) & ATTR_MSB_SHADOW) != 0u;
    return msb || (normalShadow != 0u && colorindex == normalShadow);
}
// 4bpp LUT mode only: extract sprite-type-dependent priority/colorcl/
// normalshadow from a non-RGB LUT word and return the masked color index.
// Mirrors tile_shade.comp::getLutSpriteInfo so forward shader's FB
// encoding matches graphics path / tile_shade per-pixel attrs.
void getLutSpriteInfo(uint pixel, uint type,
                      out uint outPriority, out uint outColorcl,
                      out uint outMaskedColor, out bool outNormalShadow) {
    uint pri = 0u;
    uint cc = 0u;
    uint masked = pixel;
    bool ns = false;
    if (type == 0x0u) {
        pri = pixel >> 14;
        cc  = (pixel >> 11) & 0x7u;
        masked = pixel & 0x7FFu;
        ns = (masked == 0x7FEu);
    } else if (type == 0x1u) {
        pri = pixel >> 13;
        cc  = (pixel >> 11) & 0x3u;
        masked = pixel & 0x7FFu;
        ns = (masked == 0x7FEu);
    } else if (type == 0x2u) {
        pri = (pixel >> 14) & 0x1u;
        cc  = (pixel >> 11) & 0x7u;
        masked = pixel & 0x7FFu;
        ns = (masked == 0x7FEu);
    } else if (type == 0x3u) {
        pri = (pixel >> 13) & 0x3u;
        cc  = (pixel >> 11) & 0x3u;
        masked = pixel & 0x7FFu;
        ns = (masked == 0x7FEu);
    } else if (type == 0x4u) {
        pri = (pixel >> 13) & 0x3u;
        cc  = (pixel >> 10) & 0x7u;
        masked = pixel & 0x3FFu;
        ns = (masked == 0x3FEu);
    } else if (type == 0x5u) {
        pri = (pixel >> 12) & 0x7u;
        cc  = (pixel >> 11) & 0x1u;
        masked = pixel & 0x7FFu;
        ns = (masked == 0x7FEu);
    } else if (type == 0x6u) {
        pri = (pixel >> 12) & 0x7u;
        cc  = (pixel >> 10) & 0x3u;
        masked = pixel & 0x3FFu;
        ns = (masked == 0x3FEu);
    } else if (type == 0x7u) {
        pri = (pixel >> 12) & 0x7u;
        cc  = (pixel >> 9)  & 0x7u;
        masked = pixel & 0x1FFu;
        ns = (masked == 0x1FEu);
    } else if (type == 0x8u) {
        pri = (pixel >> 7) & 0x1u;
        masked = pixel & 0x7Fu;
        ns = (masked == 0x7Eu);
    } else if (type == 0x9u) {
        pri = (pixel >> 7) & 0x1u;
        cc  = (pixel >> 6) & 0x1u;
        masked = pixel & 0x3Fu;
        ns = (masked == 0x3Eu);
    } else if (type == 0xAu) {
        pri = (pixel >> 6) & 0x3u;
        masked = pixel & 0x3Fu;
        ns = (masked == 0x3Eu);
    } else if (type == 0xBu) {
        cc = (pixel >> 6) & 0x3u;
        masked = pixel & 0x3Fu;
        ns = (masked == 0x3Eu);
    } else if (type == 0xCu) {
        pri = (pixel >> 7) & 0x1u;
        masked = pixel & 0xFFu;
        ns = (masked == 0xFEu);
    } else if (type == 0xDu) {
        pri = (pixel >> 7) & 0x1u;
        cc  = (pixel >> 6) & 0x1u;
        masked = pixel & 0xFFu;
        ns = (masked == 0xFEu);
    } else if (type == 0xEu) {
        pri = (pixel >> 6) & 0x3u;
        masked = pixel & 0xFFu;
        ns = (masked == 0xFEu);
    } else if (type == 0xFu) {
        cc = (pixel >> 6) & 0x3u;
        masked = pixel & 0xFFu;
        ns = (masked == 0xFEu);
    }
    outPriority     = pri;
    outColorcl      = cc;
    outMaskedColor  = masked;
    outNormalShadow = ns;
}
vec4 packPaletteWithAttrs(uint colorindex, uint priority, uint colorcl, Vdp1Cmd cmd) {
    float r = float(colorindex & 0xFFu) / 255.0;
    float g = float((colorindex >> 8) & 0xFFu) / 255.0;
    float b = float(encodeBByte(0u, cmd)) / 255.0;
    uint a = encodeAlphaByte(1u, colorcl, priority);
    return vec4(r, g, b, float(a) / 255.0);
}
vec4 packShadowWithPri(uint priority, Vdp1Cmd cmd) {
    float b = float(encodeBByte(1u, cmd)) / 255.0;
    uint a = encodeAlphaByte(1u, 0u, priority);
    return vec4(0.0, 0.0, b, float(a) / 255.0);
}
)GLSL"
R"GLSL(
// Polygon inside-test helpers (POLYGON-only). Forward shader uses a 1x1
// virtual texture for POLYGON, so cover_rect = bbox of v0..v3, which over-
// fills triangles (v3 == v0) and non-rectangular quads. 2-triangle
// decomposition with auto-winding edge function gates the bbox sweep.
float edgeFn(vec2 p, vec2 a, vec2 b) {
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}
// Edge tolerance INSIDE_TRI_EPS: pixels exactly on a polygon edge should
// pass the inclusive >=0 test (edge fn = 0 mathematically), but lerp /
// edgeFn computed in fp32 produces values like -1e-3 due to rounding,
// causing on-edge pixels to be rejected. For the per-texel quad coverage
// path this manifests as 1-pixel holes along polygon edges. 2026-05-09:
// cmd 48 (8x8 16bpp RGB distorted sprite, scaleMax=2) showed 2 such holes
// at (137,207) and (134,208); eps=1e-3 closes both without admitting
// clearly-outside pixels (their edge fn is ~2 px scale, 1/2000 ratio).
const float INSIDE_TRI_EPS = 1e-3;
bool insideTriangleAuto(vec2 p, vec2 a, vec2 b, vec2 c) {
    float sgn = edgeFn(c, a, b) >= 0.0 ? 1.0 : -1.0;
    float e0 = edgeFn(p, a, b);
    float e1 = edgeFn(p, b, c);
    float e2 = edgeFn(p, c, a);
    return (e0 * sgn >= -INSIDE_TRI_EPS)
        && (e1 * sgn >= -INSIDE_TRI_EPS)
        && (e2 * sgn >= -INSIDE_TRI_EPS);
}
bool insidePolygonQuad(vec2 p, vec2 v0, vec2 v1, vec2 v2, vec2 v3) {
    return insideTriangleAuto(p, v0, v1, v2) ||
           insideTriangleAuto(p, v0, v2, v3);
}

// Polyline / Line per-pixel coverage helpers (Phase 2C). Copied verbatim
// from tile_shade.comp so a Saturn-cell-wide line renders identically across
// the two paths.
const float SUPERCOVER_HALF_SAT      = 0.58;
const float MIN_SUPERCOVER_HD        = 0.555;
// FLAG_THIN-only band width (Saturn cell units). 0.58 still leaves a row
// gap at HD, while 1.0 made thin sprites bloat too much, so we run with
// 0.75 (3 fb px at RES_4x). non-thin paths keep SUPERCOVER_HALF_SAT.
const float THIN_SUPERCOVER_HALF_SAT = 0.50;

float distSqToSegment(vec2 p, vec2 a, vec2 b) {
    vec2 ab = b - a;
    float lenSq = dot(ab, ab);
    if (lenSq < 1e-6) {
        vec2 dp = p - a;
        return dot(dp, dp);
    }
    float t = clamp(dot(p - a, ab) / lenSq, 0.0, 1.0);
    vec2 d = p - (a + t * ab);
    return dot(d, d);
}

bool isOnSegment(vec2 p, vec2 a, vec2 b, float thrSq) {
    return distSqToSegment(p, a, b) <= thrSq;
}

// POLYGON inside test with edge supercover band (matches tile_shade's
// insideOrOnEdge for thin polys). Strict-inside alone drops pixels in
// narrow rows for sliver triangles where the polygon x range at a given
// y is < 1 pixel wide. Saturn HW Bresenham fills every row; we
// approximate by accepting pixels within thrSq of any of the 4 edges.
bool insidePolygonOrOnEdge(vec2 p, vec2 v0, vec2 v1, vec2 v2, vec2 v3, float thrSq) {
    if (insidePolygonQuad(p, v0, v1, v2, v3)) return true;
    return isOnSegment(p, v0, v1, thrSq) ||
           isOnSegment(p, v1, v2, thrSq) ||
           isOnSegment(p, v2, v3, thrSq) ||
           isOnSegment(p, v3, v0, thrSq);
}

// Inverse bilinear (T1B.11): solve P(s,t) = mix(mix(A,B,s), mix(D,C,s), t)
// for (s, t). Used by POLYGON per-pixel Gouraud so the screen pixel position
// projects back to the (s, t) the gouraud table is parameterized over.
// Copied verbatim from tile_shade.comp (sensitive math; do not edit body).
float cross2(vec2 a, vec2 b) { return a.x * b.y - a.y * b.x; }

vec2 inverseBilinear(vec2 p, vec2 A, vec2 B, vec2 C, vec2 D) {
    vec2 e0 = B - A;
    vec2 e1 = D - A;
    vec2 e2 = (A - B + C - D);
    vec2 q  = p - A;

    float a = cross2(e2, e1);
    float b = cross2(e0, e1) - cross2(e2, q);
    float c = cross2(q, e0);

    float t;
    // Relative threshold: treat a as zero when it is much smaller than b.
    // Axis-aligned quads should have e2 = (0, 0) and a = 0, but FP rounding
    // in (A - B + C - D) leaves a small non-zero residue when vertex coords
    // span large positive and negative values combined with non-integer
    // scaleX/scaleY (e.g. flipped scaled sprites at non-integer RES_NATIVE
    // scale). The absolute 1e-6 threshold missed that residue and routed
    // to the quadratic branch, where sqrt(b*b - 4*a*c) with 4ac << b*b
    // suffers catastrophic cancellation -- t collapses to 0 or to a huge
    // value, clamped to [0, 1] as 0 or 1, producing vertical stripes
    // (V stuck on one texture row). Relative scale keeps quadratic for
    // genuine curved quads (a ~ b) while routing FP-tiny a to linear.
    if (abs(a) * 1e6 < abs(b)) {
        if (abs(b) < 1e-6) return vec2(0);
        t = -c / b;
    } else {
        float disc = max(0.0, b * b - 4.0 * a * c);
        float sq = sqrt(disc);
        float t0 = (-b - sq) / (2.0 * a);
        float t1 = (-b + sq) / (2.0 * a);
        // Tie-break (mirrors Vdp1ComputeMath.cpp::inverseBilinear):
        //   1. Both roots in [0, 1]: pick the strictly interior one (closer
        //      to 0.5). Required for degenerate quads (v2==v3 etc.) where
        //      the bilinear has TWO valid roots -- the interior root v/u
        //      and the boundary root t==1 corresponding to the collapsed
        //      vertex. Picking the boundary root sets sden=0 and falls back
        //      to s=0, mapping the entire collapsed texture row to one
        //      screen point.
        //   2. Otherwise, closest to [0, 1] (BR-dilation band continuity).
        float d0 = max(0.0, max(-t0, t0 - 1.0));
        float d1 = max(0.0, max(-t1, t1 - 1.0));
        if (d0 == 0.0 && d1 == 0.0) {
            t = (abs(t0 - 0.5) <= abs(t1 - 0.5)) ? t0 : t1;
        } else {
            t = (d0 <= d1) ? t0 : t1;
        }
    }

    vec2 sden = e0 + e2 * t;
    float s;
    if (abs(sden.x) > abs(sden.y)) {
        s = abs(sden.x) < 1e-6 ? 0.0 : (q.x - e1.x * t) / sden.x;
    } else {
        s = abs(sden.y) < 1e-6 ? 0.0 : (q.y - e1.y * t) / sden.y;
    }
    return vec2(s, t);
}

vec2 computeUV(vec2 P, Vdp1Cmd c) {
    return clamp(inverseBilinear(P, vec2(c.v0), vec2(c.v1), vec2(c.v2), vec2(c.v3)), 0.0, 1.0);
}
)GLSL"
R"GLSL(
// =============================================================================
// Texture row selection (Saturn line stepping). VDP1 draws a textured
// command as dmax + 1 lines between edges A->D and B->C, where dmax is the
// longest Chebyshev length of those two edges, and picks each line's texture
// row with an error term (the reference software renderer, ss/vdp1_common.h
// VileTex::Setup and the per-line PreStep in ss/vdp1_sprite.cpp). Along a
// line the texture column is stepped the same way per dot (ss/vdp1.cpp line
// setup, ss/vdp1_common.h DrawLine). vdp1TexRow is the closed form of that
// stepping; it must match Vdp1ComputeMath.cpp::vdp1TexRow (checked against
// the stepping for every size in vdp1_compute_math_test).
// =============================================================================
int vdp1TexRow(int k, int n, int t0, int t1) {
    int dt = t1 - t0;
    int adt = abs(dt);
    int rev = dt < 0 ? 1 : 0;
    int inc; int adj; int e;
    if (n <= adt) { inc = 2 * (adt + 1); adj = 2 * n;       e = adt + 1 - (2 * n + rev); }
    else          { inc = 2 * adt;       adj = 2 * (n - 1); e = n - (2 * n - rev); }
    e += k * inc;
    int m = (e >= 0 && adj != 0) ? e / adj + 1 : 0;
    return rev != 0 ? t0 - m : t0 + m;
}
// dmax and the A->B dot count minus one, computed on the host from the raw
// Saturn vertices (Vdp1ComputeMath.cpp::packSpriteSteps) and carried in
// Vdp1Cmd::lineSteps; the fb-pixel vertices are truncated after scaling and
// cannot give them back exactly at non-integer scales.
int cmdDmax(Vdp1Cmd c) { return int(c.lineSteps & 0xFFFFu); }
int cmdLenAB(Vdp1Cmd c) { return int(c.lineSteps >> 16); }
// One-line sprite: both side edges have length 0 (A == D, B == C), so the
// hardware draws the single line A->B. The quad has no interior, so the
// inverse bilinear solve degenerates (0/0) and cannot give the column.
bool isOneLineSprite(Vdp1Cmd c) {
    return c.cmdType == TYPE_DISTORTED_SPRITE && c.v0 == c.v3 && c.v1 == c.v2;
}
// Parameter of fb point P along A->B of a one-line sprite, measured at the
// Saturn cell centre (P minus half a cell, see spriteTexRow).
float oneLineParam(vec2 P, Vdp1Cmd c) {
    vec2 e = vec2(c.v1 - c.v0);
    float ee = dot(e, e);
    if (ee <= 0.0) return 0.0;
    vec2 q = P - 0.5 * vec2(pc.scaleX, pc.scaleY) - vec2(c.v0);
    return clamp(dot(q, e) / ee, 0.0, 1.0);
}
// Texture column (flip applied) of the dot of a one-line sprite nearest to P.
uint oneLineTexU(vec2 P, Vdp1Cmd c) {
    int len = cmdLenAB(c);
    int j = int(roundEven(oneLineParam(P, c) * float(len)));
    int w = int(c.charSize.x);
    bool flipH = (c.flip & 1u) != 0u;
    return uint(vdp1TexRow(j, len + 1, flipH ? w - 1 : 0, flipH ? 0 : w - 1));
}
// Texture row (flip applied) of the Saturn line that covers fb point P.
// Vertex (x, y) names Saturn cell (x, y), whose centre is half a cell past
// the vertex in fb space, so the line parameter is solved at P minus half a
// cell; line k then owns parameters within half a line of k / dmax.
uint spriteTexRow(vec2 P, Vdp1Cmd c) {
    int dmax = cmdDmax(c);
    int k = 0;
    if (dmax > 0) {
        vec2 q = P - 0.5 * vec2(pc.scaleX, pc.scaleY);
        float t;
        if (c.v0 == c.v1 && c.v3 == c.v2) {
            // Zero-width quad (A == B, D == C): every line is one dot on
            // the A->D edge, so the line parameter is the projection on it.
            vec2 e = vec2(c.v3 - c.v0);
            t = dot(q - vec2(c.v0), e) / dot(e, e);
        } else {
            t = inverseBilinear(q, vec2(c.v0), vec2(c.v1), vec2(c.v2), vec2(c.v3)).y;
        }
        k = int(roundEven(clamp(t, 0.0, 1.0) * float(dmax)));
    }
    int h = int(c.charSize.y);
    bool flipV = (c.flip & 2u) != 0u;
    return uint(vdp1TexRow(k, dmax + 1, flipV ? h - 1 : 0, flipV ? 0 : h - 1));
}
)GLSL"
R"GLSL(
// Gouraud shading (Phase 1B.6) - VDP1 manual sec 5.2 / Table 5.3. CMDPMOD
// bit 2 enables; cmd.gouraudAddr.x is VRAM offset of a 4 entry RGB555
// vertex color table. Bias: actual_offset = table - 0x10 in 5 bit space.
// Per pixel RGB = clamp(sample + bilinear(offset, screen_uv), 0, 0x1F).
// Vertex word order: 0=A=v0=TL, 1=B=v1=TR, 2=C=v2=BR, 3=D=v3=BL.
// Texture flip does NOT affect gouraud (gradient is screen-space).
vec3 applyGouraud(vec3 srcRgb, vec2 uv, Vdp1Cmd c) {
    if (c.gouraudAddr.y == 0u) return srcRgb;
    uint addr = c.gouraudAddr.x;
    vec3 g[4];
    for (int i = 0; i < 4; ++i) {
        uint w = vramWord(addr + uint(i) * 2u);
        g[i] = (vec3(float(w & 0x1Fu),
                     float((w >> 5u) & 0x1Fu),
                     float((w >> 10u) & 0x1Fu)) - 16.0) * (8.0 / 255.0);
    }
    vec3 top = mix(g[0], g[1], uv.x);
    vec3 bot = mix(g[3], g[2], uv.x);
    vec3 offs = mix(top, bot, uv.y);
    return clamp(srcRgb + offs, 0.0, 1.0);
}

// CMDPMOD blend (Phase 1B.7). bits 1:0 = cc (0=replace/1=shadow/2=half-lum/
// 3=half-trans); bit 2 = Gouraud (already applied); bit 8 = Mesh
// (drop pixel on (x^y) even checkerboard); bit 15 = MSB shadow (force
// dst.a=1.0). Mirrors tile_shade::applyBlend 1:1.
// Sprint 6 S2: mediump signatures (Adreno doc: 2x ALU speedup over highp
// for blend math). 8-bit-per-channel rgba8 framebuffer values fit in
// mediump precision exactly. Implicit highp -> mediump conversion at the
// caller is free on Adreno.
mediump vec4 applyBlend(mediump vec4 src, mediump vec4 dst, uint pmod, ivec2 pix) {
    if ((pmod & 0x100u) != 0u && ((pix.x ^ pix.y) & 1) == 0) return dst;
    uint cc = pmod & 0x3u;
    mediump vec4 result;
    if (cc == 1u) {
        // Shadow: gate on (dst.a & 0xC0) == 0x80 to darken only sprite-drawn
        // RGB-direct pixels. Palette dst stores colorindex in r/g; darkening
        // would shift the CRAM index. Mirrors tile_shade::applyBlend.
        uint dstA = uint(dst.a * 255.0);
        if ((dstA & 0xC0u) != 0x80u) return dst;
        result = vec4(dst.rgb * 0.5, dst.a);
    }
    else if (cc == 2u) result = vec4(src.rgb * 0.5, src.a);
    else if (cc == 3u) {
        // Half-trans: dst alpha bit 6 (C bit) == 0 -> blend, else replace.
        uint dstA = uint(dst.a * 255.0);
        if ((dstA & 0x40u) == 0u) result = vec4((src.rgb + dst.rgb) * 0.5, src.a);
        else                      result = src;
    }
    else               result = src;
    if ((pmod & 0x8000u) != 0u) result.a = 1.0;
    return result;
}

// Pmod-aware blend (perf opt 2026-05-08). Saves the imageLoad when cc==0
// (Replace) or cc==2 (Half-luminance) -- neither path reads dst. Mesh
// returns false to signal "skip imageStore" (functionally equivalent to
// applyBlend's "return dst" since dst would be re-stored unchanged).
// 'result' kept highp so callers do not need precision-matching locals.
bool applyBlendNoDst(out vec4 result, mediump vec4 src, uint pmod, ivec2 pix) {
    if ((pmod & 0x100u) != 0u && ((pix.x ^ pix.y) & 1) == 0) return false;
    uint cc = pmod & 0x3u;
    mediump vec4 r = (cc == 2u) ? vec4(src.rgb * 0.5, src.a) : src;
    if ((pmod & 0x8000u) != 0u) r.a = 1.0;
    result = r;
    return true;
}

// Per-texel sample functions (forward shader takes uvec2 texel index, not
// vec2 uv like tile_shade). Phase 1B.1 scope: bare color decode + spd
// transparency + 4bpp LUT direct entry. Phase 1B.3 added SPCTL bit 5 direct
// RGB branch in Bank (cm=0/2/3/4) and 16bpp RGB (cm=5) samplers. Phase 1B.4
// added Saturn end-code (F9) terminator scan: when CMDPMOD bit 7 (ECD) is
// CLEAR, dot==0xF / (dot & palMask)==palMask / 0x7FFF acts as a row
// terminator. 1st end-code transparent, 2nd end-code -> all subsequent
// pixels in row transparent (Vdp1Renderer.cpp:3940-3943). Shadow check and
// LUT sprite-type attr extract still deferred to future / 1B.7.
vec4 sampleTexel4bppBank(Vdp1Cmd c, uvec2 t, uint colorBank, bool spd, bool endcEnabled) {
    uvec2 sz = c.charSize;
    if (endcEnabled) {
        uint endcnt = 0u;
        for (uint i = 0u; i < t.x; ++i) {
            uint bi = vramByte(c.srca * 8u + ((t.y * sz.x + i) >> 1));
            uint di = ((i & 1u) == 0u) ? ((bi >> 4) & 0xFu) : (bi & 0xFu);
            if (di == 0xFu) {
                endcnt++;
                if (endcnt >= 2u) return vec4(0);
            }
        }
    }
    uint byteIdx = (t.y * sz.x + t.x) >> 1;
    uint b = vramByte(c.srca * 8u + byteIdx);
    uint dot = ((t.x & 1u) == 0u) ? ((b >> 4) & 0xFu) : (b & 0xFu);
    if (endcEnabled && dot == 0xFu) return vec4(0);
    if (dot == 0u && !spd) return vec4(0);
    uint colorindex = dot | colorBank;
    if (isShadowFor(colorindex, c)) return packShadow(c);
    // SPCTL bit 5 (sprite-RGB-enable) + colorindex bit 0x8000 -> direct RGB
    // (vidogl.c:653-657, 4bpp Bank case 0). Mirrors tile_shade sample4bppBank.
    bool spctlRgb = (pc.spriteType & 0x20u) != 0u;
    if ((colorindex & 0x8000u) != 0u && spctlRgb) return packDirect(colorindex, c);
    return packPalette(colorindex, c);
}
vec4 sampleTexel4bppLut(Vdp1Cmd c, uvec2 t, uint colorLut, bool spd, bool endcEnabled) {
    uvec2 sz = c.charSize;
    if (endcEnabled) {
        uint endcnt = 0u;
        for (uint i = 0u; i < t.x; ++i) {
            uint bi = vramByte(c.srca * 8u + ((t.y * sz.x + i) >> 1));
            uint di = ((i & 1u) == 0u) ? ((bi >> 4) & 0xFu) : (bi & 0xFu);
            if (di == 0xFu) {
                endcnt++;
                if (endcnt >= 2u) return vec4(0);
            }
        }
    }
    uint byteIdx = (t.y * sz.x + t.x) >> 1;
    uint b = vramByte(c.srca * 8u + byteIdx);
    uint dot = ((t.x & 1u) == 0u) ? ((b >> 4) & 0xFu) : (b & 0xFu);
    if (endcEnabled && dot == 0xFu) return vec4(0);
    if (dot == 0u && !spd) return vec4(0);
    uint w = vramWord((dot * 2u + colorLut) & 0x7FFFFu);
    if ((w & 0x8000u) != 0u) {
        // RGB direct LUT entry: graphics path forces priority=0
        // (Vdp1Renderer.cpp:3950).
        if (isShadowFor(w, c)) return packShadow(c);
        vec4 c_direct = packDirect(w, c);
        uint a = encodeAlphaByte(0u, c.colorcl, 0u);
        c_direct.a = float(a) / 255.0;
        return c_direct;
    }
    // Palette LUT entry: graphics path runs Vdp1ProcessSpritePixel on the
    // LUT word per pixel (Vdp1Renderer.cpp:3953), extracting
    // priority/colorcl/normalshadow per VDP2 sprite type and masking the
    // colorindex. Without this, compute leaves the upper bits inside the
    // colorindex and uses cmd-level priority/colorcl instead of LUT-derived.
    uint pri; uint cc_; uint masked; bool ns;
    getLutSpriteInfo(w, pc.spriteType & 0xFu, pri, cc_, masked, ns);
    bool msb = ((c.vdp2Attrs >> 16) & ATTR_MSB_SHADOW) != 0u;
    if (msb || ns) return packShadowWithPri(pri, c);
    return packPaletteWithAttrs(masked, pri, cc_, c);
}
vec4 sampleTexel8bppBank(Vdp1Cmd c, uvec2 t, uint colorBank, uint palMask, bool spd, bool endcEnabled) {
    uvec2 sz = c.charSize;
    if (endcEnabled) {
        uint endcnt = 0u;
        for (uint i = 0u; i < t.x; ++i) {
            uint di = vramByte(c.srca * 8u + t.y * sz.x + i);
            if ((di & palMask) == palMask) {
                endcnt++;
                if (endcnt >= 2u) return vec4(0);
            }
        }
    }
    uint dot = vramByte(c.srca * 8u + t.y * sz.x + t.x);
    if (endcEnabled && (dot & palMask) == palMask) return vec4(0);
    if (dot == 0u && !spd) return vec4(0);
    uint colorindex = (dot & palMask) | colorBank;
    if (isShadowFor(colorindex, c)) return packShadow(c);
    // SPCTL bit 5 + colorindex bit 0x8000 -> direct RGB (8bpp Bank cm=2/3/4
    // mirror the 4bpp Bank case 0 path). Same rationale as sample4bppBank.
    bool spctlRgb = (pc.spriteType & 0x20u) != 0u;
    if ((colorindex & 0x8000u) != 0u && spctlRgb) return packDirect(colorindex, c);
    return packPalette(colorindex, c);
}
vec4 sampleTexel16bppRGB(Vdp1Cmd c, uvec2 t, bool spd, bool endcEnabled) {
    uvec2 sz = c.charSize;
    if (endcEnabled) {
        uint endcnt = 0u;
        for (uint i = 0u; i < t.x; ++i) {
            uint wi = vramWord(c.srca * 8u + (t.y * sz.x + i) * 2u);
            if (wi == 0x7FFFu) {
                endcnt++;
                if (endcnt >= 2u) return vec4(0);
            }
        }
    }
    uint addr = c.srca * 8u + (t.y * sz.x + t.x) * 2u;
    uint word = vramWord(addr);
    // bit 15 clear AND SPD clear -> transparent
    // bit 15 set + SPCTL bit 5   -> direct RGB (1B.3: gated to match tile_shade)
    // otherwise                  -> palette (16-bit colorindex)
    if ((word & 0x8000u) == 0u && !spd) return vec4(0);
    if (endcEnabled && word == 0x7FFFu) return vec4(0);
    bool spctlRgb = (pc.spriteType & 0x20u) != 0u;
    if ((word & 0x8000u) != 0u && spctlRgb) {
        if (isShadowFor(word, c)) return packShadow(c);
        return packDirect(word, c);
    }
    // 16bpp palette pixel: per-pixel Vdp1ProcessSpritePixel (getLutSpriteInfo),
    // mirroring the 4bpp LUT path / vidogl.c readTexture. packPalette(word) kept
    // the command-level priority/colorcl and the full unmasked 16-bit color
    // index, so palette sprite types > 0 (Mr. Bones forest skeleton = type 7)
    // lost the per-pixel priority slot (dot>>12)&7 and were discarded when that
    // PRISA slot held 0.
    uint pri; uint cc_; uint masked; bool ns;
    getLutSpriteInfo(word, pc.spriteType & 0xFu, pri, cc_, masked, ns);
    bool msb = ((c.vdp2Attrs >> 16) & ATTR_MSB_SHADOW) != 0u;
    if (msb || ns) return packShadowWithPri(pri, c);
    return packPaletteWithAttrs(masked, pri, cc_, c);
}

// Sample one sprite texel for color mode cm; vec4(0) (transparent) for the
// reserved modes 6/7.
vec4 sampleSpriteTexel(Vdp1Cmd c, uvec2 t, uint cm, bool spd, bool endcEnabled) {
    if (cm == 0u) return sampleTexel4bppBank(c, t, c.color & 0xFFF0u, spd, endcEnabled);
    if (cm == 1u) return sampleTexel4bppLut (c, t, c.color * 8u,      spd, endcEnabled);
    if (cm == 2u) return sampleTexel8bppBank(c, t, c.color & 0xFFC0u, 0x3Fu, spd, endcEnabled);
    if (cm == 3u) return sampleTexel8bppBank(c, t, c.color & 0xFF80u, 0x7Fu, spd, endcEnabled);
    if (cm == 4u) return sampleTexel8bppBank(c, t, c.color & 0xFF00u, 0xFFu, spd, endcEnabled);
    if (cm == 5u) return sampleTexel16bppRGB(c, t, spd, endcEnabled);
    return vec4(0);
}
)GLSL"
R"GLSL(

void processCmd(Vdp1Cmd c, ivec4 clipRect) {
    // Phase 2C: sprite + POLYGON + POLYLINE + LINE.
    bool isPolygon  = (c.cmdType == TYPE_POLYGON);
    bool isPolyline = (c.cmdType == TYPE_POLYLINE);
    bool isLine     = (c.cmdType == TYPE_LINE);
    bool isLineLike = isPolyline || isLine;
    if (c.cmdType != TYPE_DISTORTED_SPRITE &&
        c.cmdType != TYPE_NORMAL_SPRITE &&
        c.cmdType != TYPE_SCALED_SPRITE &&
        !isPolygon && !isLineLike) return;

    // pmod-aware imageLoad gating (perf opt 2026-05-08). cc==0 (Replace)
    // and cc==2 (Half-luminance) do not read dst, so they go through the
    // applyBlendNoDst path that skips imageLoad. Workgroup-uniform branch
    // (= no warp divergence).
    uint pmodCc = c.pmod & 0x3u;
    bool needsDst = (pmodCc == 1u || pmodCc == 3u);

    // Polyline / Line per-pixel pull within the cmd's bbox. No texture / no
    // forward push concept; lines are inherently per-pixel on Saturn HW.
    // Bbox pixels distributed across the 8x8 workgroup with stride loop.
    if (isLineLike) {
        vec4 src;
        if (isShadowFor(c.color, c)) {
            src = packShadow(c);
        } else if ((c.color & 0x8000u) != 0u) {
            src = packDirect(c.color, c);
        } else {
            src = packPalette(c.color, c);
        }

        float halfHD = max(MIN_SUPERCOVER_HD, SUPERCOVER_HALF_SAT * pc.scaleMax);
        float thrSq  = halfHD * halfHD;

        // clipRect intersects with cmd bbox so lines that miss this tile
        // early-out without walking off-tile pixels (per-cmd mode passes
        // full screen so this collapses to the original cmd-bbox sweep).
        int sweepMinX = max(c.bbox.x, clipRect.x);
        int sweepMinY = max(c.bbox.y, clipRect.y);
        int sweepMaxX = min(c.bbox.z + 1, clipRect.z);
        int sweepMaxY = min(c.bbox.w + 1, clipRect.w);
        int x0 = sweepMinX + int(gl_LocalInvocationID.x);
        int y0 = sweepMinY + int(gl_LocalInvocationID.y);
        int x1 = sweepMaxX;
        int y1 = sweepMaxY;

        for (int y = y0; y < y1; y += int(gl_WorkGroupSize.y)) {
            for (int x = x0; x < x1; x += int(gl_WorkGroupSize.x)) {
                if (x < 0 || y < 0) continue;
                if (x > c.systemClip.x || y > c.systemClip.y) continue;
                if (x >= int(pc.fbWidth) || y >= int(pc.fbHeight)) continue;
                if (c.clipMode == 1u) {
                    if (x < c.userClip.x || x > c.userClip.z ||
                        y < c.userClip.y || y > c.userClip.w) continue;
                } else if (c.clipMode == 2u) {
                    if (x >= c.userClip.x && x <= c.userClip.z &&
                        y >= c.userClip.y && y <= c.userClip.w) continue;
                }

                vec2 p = vec2(float(x) + 0.5, float(y) + 0.5);
                bool hit;
                if (isPolyline) {
                    hit = isOnSegment(p, vec2(c.v0), vec2(c.v1), thrSq) ||
                          isOnSegment(p, vec2(c.v1), vec2(c.v2), thrSq) ||
                          isOnSegment(p, vec2(c.v2), vec2(c.v3), thrSq) ||
                          isOnSegment(p, vec2(c.v3), vec2(c.v0), thrSq);
                } else {
                    // Line: only v0->v1; encodeLine sets v2=v1, v3=v0 for
                    // bbox coverage only -- skip those duplicate segments.
                    hit = isOnSegment(p, vec2(c.v0), vec2(c.v1), thrSq);
                }
                if (!hit) continue;

                vec4 finalSrc = src;
                if (c.gouraudAddr.y != 0u) {
                    vec2 lineUv;
                    if (isPolyline) {
                        lineUv = computeUV(p, c);
                    } else {
                        vec2 ab = vec2(c.v1) - vec2(c.v0);
                        float denom = max(dot(ab, ab), 1e-6);
                        float t = clamp(dot(p - vec2(c.v0), ab) / denom, 0.0, 1.0);
                        lineUv = vec2(t, 0.0);
                    }
                    finalSrc.rgb = applyGouraud(finalSrc.rgb, lineUv, c);
                }

                ivec2 imgPix = ivec2(x, int(pc.fbHeight) - 1 - y);
                if (needsDst) {
                    vec4 dst = imageLoad(fb, imgPix);
                    vec4 result = applyBlend(finalSrc, dst, c.pmod, ivec2(x, y));
                    imageStore(fb, imgPix, result);
                } else {
                    vec4 result;
                    if (applyBlendNoDst(result, finalSrc, c.pmod, ivec2(x, y))) {
                        imageStore(fb, imgPix, result);
                    }
                }
            }
        }
        return;
    }
)GLSL"
R"GLSL(

    // POLYGON has no texture (cmd.charSize == (0,0)). Treat as 1x1 virtual
    // texture: one texel covers the whole polygon, color comes directly from
    // cmd.color (no VRAM read).
    uvec2 effCharSize = isPolygon ? uvec2(1u, 1u) : c.charSize;

    if (effCharSize.x == 0u || effCharSize.y == 0u) return;

    // Sprint 4 S3 polygon fast path: distribute pixel scan across the 8x8
    // thread block instead of running a single thread sequentially over the
    // entire polygon. Mirrors the .comp canonical source.
    if (isPolygon && (c.flags & FLAG_THIN) == 0u) {
        vec4 src;
        if (isShadowFor(c.color, c)) {
            src = packShadow(c);
        } else if ((c.color & 0x8000u) != 0u) {
            src = packDirect(c.color, c);
        } else {
            src = packPalette(c.color, c);
        }
        if (src.a < 0.5) return;

        vec2 polyMin = min(min(vec2(c.v0), vec2(c.v1)),
                           min(vec2(c.v2), vec2(c.v3)));
        vec2 polyMax = max(max(vec2(c.v0), vec2(c.v1)),
                           max(vec2(c.v2), vec2(c.v3)));
        polyMax += vec2(max(1.0, pc.scaleMax));

        int x0 = max(int(floor(polyMin.x)), clipRect.x);
        int y0 = max(int(floor(polyMin.y)), clipRect.y);
        int x1 = min(int(ceil (polyMax.x)), clipRect.z);
        int y1 = min(int(ceil (polyMax.y)), clipRect.w);

        float halfHD_p = max(MIN_SUPERCOVER_HD,
                             SUPERCOVER_HALF_SAT * pc.scaleMax);
        float thrSq_p = halfHD_p * halfHD_p;

        for (int y = y0 + int(gl_LocalInvocationID.y);
             y < y1; y += int(gl_WorkGroupSize.y)) {
            for (int x = x0 + int(gl_LocalInvocationID.x);
                 x < x1; x += int(gl_WorkGroupSize.x)) {
                if (x < 0 || y < 0) continue;
                if (x > c.systemClip.x || y > c.systemClip.y) continue;
                if (x >= int(pc.fbWidth) || y >= int(pc.fbHeight)) continue;
                if (c.clipMode == 1u) {
                    if (x < c.userClip.x || x > c.userClip.z ||
                        y < c.userClip.y || y > c.userClip.w) continue;
                } else if (c.clipMode == 2u) {
                    if (x >= c.userClip.x && x <= c.userClip.z &&
                        y >= c.userClip.y && y <= c.userClip.w) continue;
                }

                vec2 pTest = vec2(float(x) + 0.5, float(y) + 0.5);
                if (!insidePolygonOrOnEdge(pTest,
                                           vec2(c.v0), vec2(c.v1),
                                           vec2(c.v2), vec2(c.v3),
                                           thrSq_p)) continue;

                ivec2 imgPix = ivec2(x, int(pc.fbHeight) - 1 - y);
                if (needsDst) {
                    vec4 dst = imageLoad(fb, imgPix);
                    vec4 result = applyBlend(src, dst, c.pmod, ivec2(x, y));
                    imageStore(fb, imgPix, result);
                } else {
                    vec4 result;
                    if (applyBlendNoDst(result, src, c.pmod, ivec2(x, y))) {
                        imageStore(fb, imgPix, result);
                    }
                }
            }
        }
        return;
    }

    // Phase 1B.1: full color mode cascade (cm 0..5). 1B.4 added end-code
    // (F9) terminator. Shadow / LUT sprite-type attrs deferred to later
    // 1B sub-tasks.
    uint cm = (c.pmod >> 3) & 7u;
    bool spd = (c.pmod & 0x40u) != 0u;
    // CMDPMOD bit 7 (ECD): CLEAR = end-code processing ENABLED.
    bool endcEnabled = (c.pmod & 0x80u) == 0u;

    // Axis-aligned, integer-aligned rectangle detection. Used to
    // pick both the BR extension formula and whether the SPRITE
    // supercover band applies. Saturn HW Bresenham doesn't fatten
    // axis-aligned edges, and the step lattice fits the dest
    // exactly when the extension is a whole Saturn cell, so band
    // is skipped on these sprites to avoid 1-px overdraw on each
    // side.
    bool axisAligned = (c.v0.y == c.v1.y) && (c.v3.y == c.v2.y)
                    && (c.v0.x == c.v3.x) && (c.v1.x == c.v2.x);

    // Cell-inclusive BR vertex extension for DISTORTED_SPRITE.
    //
    // Saturn raw distorted-sprite verts encode v0..v3 as INCLUSIVE
    // endpoints -- v1/v2/v3 sit at the START of the last Saturn
    // screen cell, not the exclusive end. To cover the last cell,
    // BR verts must extrapolate.
    //
    // Two extension formulas, picked by axisAligned:
    //
    // 1. axis-aligned: extend by exactly `sign(edge) * scaleMax`
    //    per axis = 1 Saturn screen cell. Always covers the
    //    terminal Saturn cell regardless of texture / dest size
    //    mismatch. Required for shrunk axis-aligned sprites (cmd
    //    3: 64x32 texture mapped to 28 fb-px tall, lattice step
    //    = 0.9 fb-px would only extend to fb y=800.9, leaving
    //    Saturn cell at y=200 fb 801..803 unpainted, creating a
    //    3-fb-px gap before cmd 4 at fb y=804). For non-shrunk
    //    axis-aligned (cmd 145: 128x32 at scaleMax=4) sign*
    //    scaleMax = 4 = edge/(charSize-1) so the formulas agree.
    //
    // 2. non-axis-aligned (rotated / sheared / sub-pixel): extend
    //    by `edge / (charSize - 1)` = "1 step on the sprite's own
    //    lattice". Scales with the sprite's orientation. cmd 46
    //    (16x47 rotated): step ~ 0.4 fb-px, the SPRITE supercover
    //    band (still active on non-axis-aligned) catches Saturn
    //    HW Bresenham edge fattening. Using sign*scaleMax here
    //    would overdraw rotated sprites past their intended
    //    footprint (cmd 46 painted x=473+ at scaleMax=4 with the
    //    old fixed formula).
    //
    // NORMAL/SCALED_SPRITE: host pre-extends v1.x = v0.x + W*scale
    // so v1 is the EXCLUSIVE endpoint; no extension applied.
    // POLYGON: 1x1 virtual texel, separate cmax dilation per-texel.
    vec2 v0e = vec2(c.v0);
    vec2 v1e = vec2(c.v1);
    vec2 v2e = vec2(c.v2);
    vec2 v3e = vec2(c.v3);
    if (c.cmdType == TYPE_DISTORTED_SPRITE) {
        if (axisAligned) {
            // Saturn-cell-inclusive: extend +X / +Y in screen space.
            // sign(v1.x-v0.x) inverted for flipped 2-point scaled
            // sprites (CMDXC<CMDXA) and cloned texels onto the LEFT edge.
            // Per-axis degenerate fallback: NormalSprite w==1 / h==1
            // collapses all 4 corners onto the same coord on that axis
            // under cell-inclusive vertex convention. The max-based
            // check would flag all 4 corners as the +X / +Y edge and
            // extend them together, leaving the quad degenerate
            // (TRI_SPLIT path then writes nothing because both
            // triangles have zero area). Fall back to the TL/TR/BR/BL
            // vertex-order convention (v1,v2 are +X; v2,v3 are +Y) on
            // the degenerate axis only.
            int minXi = min(min(c.v0.x, c.v1.x), min(c.v2.x, c.v3.x));
            int maxXi = max(max(c.v0.x, c.v1.x), max(c.v2.x, c.v3.x));
            int minYi = min(min(c.v0.y, c.v1.y), min(c.v2.y, c.v3.y));
            int maxYi = max(max(c.v0.y, c.v1.y), max(c.v2.y, c.v3.y));
            if (minXi < maxXi) {
                if (c.v0.x == maxXi) v0e.x = float(c.v0.x) + pc.scaleX;
                if (c.v1.x == maxXi) v1e.x = float(c.v1.x) + pc.scaleX;
                if (c.v2.x == maxXi) v2e.x = float(c.v2.x) + pc.scaleX;
                if (c.v3.x == maxXi) v3e.x = float(c.v3.x) + pc.scaleX;
            } else {
                v1e.x = float(c.v1.x) + pc.scaleX;
                v2e.x = float(c.v2.x) + pc.scaleX;
            }
            if (minYi < maxYi) {
                if (c.v0.y == maxYi) v0e.y = float(c.v0.y) + pc.scaleY;
                if (c.v1.y == maxYi) v1e.y = float(c.v1.y) + pc.scaleY;
                if (c.v2.y == maxYi) v2e.y = float(c.v2.y) + pc.scaleY;
                if (c.v3.y == maxYi) v3e.y = float(c.v3.y) + pc.scaleY;
            } else {
                v2e.y = float(c.v2.y) + pc.scaleY;
                v3e.y = float(c.v3.y) + pc.scaleY;
            }
        } else if (effCharSize.x > 1u && effCharSize.y > 1u) {
            // Non-axis-aligned: per-texel edge step needs divX/divY > 0.
            // charSize 1xN / Nx1 rotated distorted sprite would be a
            // mathematical strip; rare in Saturn content and skipped.
            float divX = float(effCharSize.x - 1u);
            // Side edges step one Saturn line (dmax lines span A->D),
            // matching the dmax + 1 line grid below.
            float divY = float(max(cmdDmax(c), 1));
            vec2 step_top   = (vec2(c.v1) - vec2(c.v0)) / divX;
            vec2 step_bot   = (vec2(c.v2) - vec2(c.v3)) / divX;
            vec2 step_left  = (vec2(c.v3) - vec2(c.v0)) / divY;
            vec2 step_right = (vec2(c.v2) - vec2(c.v1)) / divY;
            v1e = vec2(c.v1) + step_top;
            v3e = vec2(c.v3) + step_left;
            v2e = vec2(c.v2) + step_bot + step_right;
            // Raw vertex collapse preservation for non-axis-aligned
            // triangle-as-quad encoding (axis-aligned with a
            // collapse would degenerate to a line; not handled
            // here). Saturn distorted-sprite triangles set v2==v3
            // or v1==v2; the matching step is 0 but adjacent steps
            // still separate the BR-corner v?e by ~step magnitude.
            // Strict per-texel quad coverage then draws sliver
            // pixels past the collapsed apex (cmd 56 v2==v3 ->
            // bottom-row texels paint around the (D=C) corner).
            // Snap collapsed pairs back to raw.
            // (v0==v1 / v0==v3 cases self-preserve since step_top
            // / step_left are 0 when the originating edge is
            // zero-length, so v0e/v1e and v0e/v3e are coincident
            // automatically.)
            if (c.v2.x == c.v3.x && c.v2.y == c.v3.y) {
                v2e = vec2(c.v2);
                v3e = vec2(c.v3);
            }
            if (c.v1.x == c.v2.x && c.v1.y == c.v2.y) {
                v1e = vec2(c.v1);
                v2e = vec2(c.v2);
            }
        }
    }
)GLSL"
R"GLSL(
    uint u = 0u;
    uint v = 0u;
    vec2 p00 = vec2(0.0);
    vec2 p10 = vec2(0.0);
    vec2 p01 = vec2(0.0);
    vec2 p11 = vec2(0.0);
    vec2 cmin = vec2(0.0);
    vec2 cmax = vec2(0.0);
    vec2 thinMin = min(min(vec2(c.v0), v1e), min(v2e, v3e));
    vec2 thinMax = max(max(vec2(c.v0), v1e), max(v2e, v3e));
    float thinHalfHD = max(MIN_SUPERCOVER_HD,
                           THIN_SUPERCOVER_HALF_SAT * pc.scaleMax);

    if ((c.flags & FLAG_THIN) != 0u && !isPolygon) {
        int x0 = max(int(floor(thinMin.x - thinHalfHD)), clipRect.x);
        int y0 = max(int(floor(thinMin.y - thinHalfHD)), clipRect.y);
        int x1 = min(int(ceil (thinMax.x + thinHalfHD)), clipRect.z);
        int y1 = min(int(ceil (thinMax.y + thinHalfHD)), clipRect.w);

        for (int y = y0 + int(gl_LocalInvocationID.y);
             y < y1; y += int(gl_WorkGroupSize.y)) {
            for (int x = x0 + int(gl_LocalInvocationID.x);
                 x < x1; x += int(gl_WorkGroupSize.x)) {
                if (x < 0 || y < 0) continue;
                if (x > c.systemClip.x || y > c.systemClip.y) continue;
                if (x >= int(pc.fbWidth) || y >= int(pc.fbHeight)) continue;
                if (c.clipMode == 1u) {
                    if (x < c.userClip.x || x > c.userClip.z ||
                        y < c.userClip.y || y > c.userClip.w) continue;
                } else if (c.clipMode == 2u) {
                    if (x >= c.userClip.x && x <= c.userClip.z &&
                        y >= c.userClip.y && y <= c.userClip.w) continue;
                }

                vec2 pTest = vec2(float(x) + 0.5, float(y) + 0.5);
                float thrSq_t = thinHalfHD * thinHalfHD;
                if (!insidePolygonOrOnEdge(pTest, v0e, v1e, v2e, v3e, thrSq_t)) continue;

                bool oneLine = isOneLineSprite(c);
                vec2 pixelUv = oneLine ? vec2(oneLineParam(pTest, c), 0.0)
                                       : computeUV(pTest, c);
                uvec2 t = uvec2(
                    uint(clamp(int(floor(pixelUv.x * float(effCharSize.x))),
                               0, int(effCharSize.x) - 1)),
                    min(spriteTexRow(pTest, c), effCharSize.y - 1u)
                );
                if ((c.flip & 1u) != 0u) t.x = effCharSize.x - 1u - t.x;
                if (oneLine) t.x = min(oneLineTexU(pTest, c), effCharSize.x - 1u);

                vec4 src;
                if (cm == 0u) src = sampleTexel4bppBank(c, t, c.color & 0xFFF0u, spd, endcEnabled);
                else if (cm == 1u) src = sampleTexel4bppLut (c, t, c.color * 8u,      spd, endcEnabled);
                else if (cm == 2u) src = sampleTexel8bppBank(c, t, c.color & 0xFFC0u, 0x3Fu, spd, endcEnabled);
                else if (cm == 3u) src = sampleTexel8bppBank(c, t, c.color & 0xFF80u, 0x7Fu, spd, endcEnabled);
                else if (cm == 4u) src = sampleTexel8bppBank(c, t, c.color & 0xFF00u, 0xFFu, spd, endcEnabled);
                else if (cm == 5u) src = sampleTexel16bppRGB(c, t, spd, endcEnabled);
                else continue;
                if (src.a < 0.5) continue;

                vec4 finalSrc = src;
                if (c.gouraudAddr.y != 0u) {
                    finalSrc.rgb = applyGouraud(finalSrc.rgb, pixelUv, c);
                }

                ivec2 imgPix = ivec2(x, int(pc.fbHeight) - 1 - y);
                if (needsDst) {
                    vec4 dst = imageLoad(fb, imgPix);
                    vec4 result = applyBlend(finalSrc, dst, c.pmod, ivec2(x, y));
                    imageStore(fb, imgPix, result);
                } else {
                    vec4 result;
                    if (applyBlendNoDst(result, finalSrc, c.pmod, ivec2(x, y))) {
                        imageStore(fb, imgPix, result);
                    }
                }
            }
        }
        return;
    }

    // Sprites walk the dmax + 1 Saturn lines instead of the texture rows:
    // grid row v is line v, drawn with the row picked by the hardware
    // line stepping (vdp1TexRow). POLYGON keeps its 1x1 virtual texel.
    uint lineCount = isPolygon ? effCharSize.y : uint(cmdDmax(c)) + 1u;
    for (uint vBase = 0u; vBase < lineCount; vBase += gl_WorkGroupSize.y) {
        for (uint uBase = 0u; uBase < effCharSize.x; uBase += gl_WorkGroupSize.x) {
            u = uBase + gl_LocalInvocationID.x;
            v = vBase + gl_LocalInvocationID.y;
            if (u >= effCharSize.x || v >= lineCount) continue;

            float ty0 = float(v) / float(lineCount);
            float ty1 = float(v + 1u) / float(lineCount);
            float tx0 = float(u) / float(effCharSize.x);
            float tx1 = float(u + 1u) / float(effCharSize.x);

            vec2 left0  = mix(v0e, v3e, ty0);
            vec2 right0 = mix(v1e, v2e, ty0);
            vec2 left1  = mix(v0e, v3e, ty1);
            vec2 right1 = mix(v1e, v2e, ty1);

            p00 = mix(left0, right0, tx0);
            p10 = mix(left0, right0, tx1);
            p01 = mix(left1, right1, tx0);
            p11 = mix(left1, right1, tx1);

            cmin = min(min(p00, p10), min(p01, p11));
            cmax = max(max(p00, p10), max(p01, p11));

            // POLYGON cell-inclusive bbox extension (Issue #5).
            if (isPolygon) {
                cmax += vec2(max(1.0, pc.scaleMax));
            }
            // FLAG_THIN cover_rect dilation - shared by POLYGON / SPRITE.
            //   POLYGON: pull the edge supercover band into the pixel scan.
            //   SPRITE: only widens the scan range. The actual texel drawn
            //     is uniquely determined by the inverse mapping of the
            //     pixel center, so neighboring-texel overdraw cannot
            //     corrupt the image.
            if ((c.flags & FLAG_THIN) != 0u) {
                cmin -= vec2(thinHalfHD);
                cmax += vec2(thinHalfHD);
            }

            // Saturn Bresenham edge-walker fattening for SPRITE (Plan B).
            // For rotated / distorted sprites the polygon outer edge can
            // land on a sub-pixel position; per-texel quad coverage then
            // drops near-edge pixels and the result shows up as a 1-px
            // hole band (e.g. cmd 42 at RES_2x, pixel (224,197) sitting
            // 0.094 fb-px outside the edge). Real Saturn HW guarantees a
            // Bresenham edge-walker draws at least 1 px per scanline, so
            // we apply an EDGE_BAND_HD = 0.5 fb-px supercover band to the
            // outer perimeter only for edge texels (u/v on the boundary)
            // and force-fill the near-edge pixels. Interior texels are
            // not affected.
            const float EDGE_BAND_HD = 0.5;
            bool isLeftEdgeTexel  = (u == 0u);
            bool isRightEdgeTexel = (u == effCharSize.x - 1u);
            bool isTopEdgeTexel   = (v == 0u);
            bool isBotEdgeTexel   = (v == lineCount - 1u);
            bool isEdgeTexel = isLeftEdgeTexel || isRightEdgeTexel
                            || isTopEdgeTexel  || isBotEdgeTexel;
            if (isEdgeTexel && !isPolygon && (c.flags & FLAG_THIN) == 0u && !axisAligned) {
                cmin -= vec2(EDGE_BAND_HD);
                cmax += vec2(EDGE_BAND_HD);
            }

            uvec2 t = uvec2(u, v);
            // Texture flip applies to texel-side reading only (CMDCTRL bits 4-5).
            // Saturn spec: "reverse character-pattern reading direction" - flip
            // the texture, not the polygon shape. The screen position via lerp
            // on v0..v3 stays unchanged; only the texel index for the sample
            // is mirrored. For POLYGON (effCharSize=(1,1)) this is a no-op.
            if ((c.flip & 1u) != 0u) t.x = effCharSize.x - 1u - t.x;
            if (!isPolygon) {
                bool flipV = (c.flip & 2u) != 0u;
                int h = int(effCharSize.y);
                t.y = uint(vdp1TexRow(int(v), int(lineCount),
                                      flipV ? h - 1 : 0, flipV ? 0 : h - 1));
            }
            vec4 src;
            if (isPolygon) {
                // POLYGON: direct color from cmd.color (no texture, no spd, no
                // end-code). CMDCOLR bit 15 set -> direct RGB; clear -> palette.
                // No SPCTL gate -- Saturn POLYGON spec differs from sprite path.
                if (isShadowFor(c.color, c)) {
                    src = packShadow(c);
                } else if ((c.color & 0x8000u) != 0u) {
                    src = packDirect(c.color, c);
                } else {
                    src = packPalette(c.color, c);
                }
            } else if (cm == 0u) src = sampleTexel4bppBank(c, t, c.color & 0xFFF0u, spd, endcEnabled);
            else if (cm == 1u) src = sampleTexel4bppLut (c, t, c.color * 8u,      spd, endcEnabled);
            else if (cm == 2u) src = sampleTexel8bppBank(c, t, c.color & 0xFFC0u, 0x3Fu, spd, endcEnabled);
            else if (cm == 3u) src = sampleTexel8bppBank(c, t, c.color & 0xFF80u, 0x7Fu, spd, endcEnabled);
            else if (cm == 4u) src = sampleTexel8bppBank(c, t, c.color & 0xFF00u, 0xFFu, spd, endcEnabled);
            else if (cm == 5u) src = sampleTexel16bppRGB(c, t, spd, endcEnabled);
            else continue;
            if (src.a < 0.5 && isPolygon) continue;
)GLSL"
R"GLSL(
            // Phase 1B.6: Gouraud shading. Screen-space bilinear offset over
            // (s, t); texel center is the natural per-texel sample point.
            // Pre-flip uv (u, v) is correct since gouraud is screen-space and
            // texture flip only affects the texel index. POLYGON skipped:
            // 1x1 virtual texel collapses to a single uv = (0.5, 0.5) which
            // would only produce a uniform tint instead of a gradient. Per-
            // pixel POLYGON gouraud is deferred to a later sub-task.
            if (!isPolygon) {
                vec2 gouraudUv = vec2(
                    (float(u) + 0.5) / float(effCharSize.x),
                    (float(v) + 0.5) / float(lineCount)
                );
                src.rgb = applyGouraud(src.rgb, gouraudUv, c);
            }

            // clipRect intersects with the texel's screen footprint so
            // tile-bound dispatches skip pixels in other tiles up front.
            int x0 = max(int(floor(cmin.x)), clipRect.x);
            int y0 = max(int(floor(cmin.y)), clipRect.y);
            int x1 = min(int(ceil(cmax.x)),  clipRect.z);
            int y1 = min(int(ceil(cmax.y)),  clipRect.w);
            for (int y = y0; y < y1; y++) {
                for (int x = x0; x < x1; x++) {
                    if (x < 0 || y < 0) continue;
                    if (x > c.systemClip.x || y > c.systemClip.y) continue;
                    if (x >= int(pc.fbWidth) || y >= int(pc.fbHeight)) continue;
                    if (c.clipMode == 1u) {
                        if (x < c.userClip.x || x > c.userClip.z ||
                            y < c.userClip.y || y > c.userClip.w) continue;
                    } else if (c.clipMode == 2u) {
                        if (x >= c.userClip.x && x <= c.userClip.z &&
                            y >= c.userClip.y && y <= c.userClip.w) continue;
                    }
                    // POLYGON-only: cover_rect = bbox of v0..v3 over-fills
                    // triangles / non-rectangular quads. 2-triangle inside
                    // test culls outside the actual polygon shape. SPRITE
                    // path skipped (per-texel cover_rect is in-polygon by
                    // lerp construction).
                    // Inside-test:
                    //   POLYGON: always applied.
                    //   SPRITE: per-texel coverage is checked in the
                    //     branch below.
                    // Uses extended verts v0e..v3e (equivalent to raw for
                    // POLYGON, matches the BR-extended polygon for
                    // DISTORTED_SPRITE).
                    bool wantInside = isPolygon;
                    if (wantInside) {
                        vec2 pTest = vec2(float(x) + 0.5, float(y) + 0.5);
                        float halfHD_p = max(MIN_SUPERCOVER_HD,
                                             SUPERCOVER_HALF_SAT * pc.scaleMax);
                        float thrSq_p = halfHD_p * halfHD_p;
                        if (!insidePolygonOrOnEdge(pTest,
                                                   v0e, v1e, v2e, v3e,
                                                   thrSq_p)) continue;
                    } else if (!isPolygon) {
                        // Per-texel quad coverage (Plan A smoothing).
                        // cmin/cmax is the AXIS-ALIGNED bbox of the rotated
                        // texel quad (p00,p10,p11,p01). For heavily rotated
                        // / distorted sprites the bbox over-approximates and
                        // each texel paints a screen-axis-aligned block of
                        // HD px -> visible "chunky" blocks that misalign
                        // with the polygon's perspective.
                        //
                        // Gating per-pixel emit on the actual rotated quad
                        // shrinks each texel's footprint to a parallelogram
                        // aligned with the polygon's orientation. Adjacent
                        // texels' quads share edges exactly by construction
                        // (same lerp on v0e..v3e), so coverage tiles without
                        // gaps (insidePolygonQuad uses inclusive >=0 edges,
                        // so shared-edge pixels pass on both sides -> tile
                        // fully).
                        //
                        // Edge-texel band (Plan B): pixels within EDGE_BAND_HD
                        // fb-px of the polygon outer perimeter are also
                        // accepted, mirroring Saturn Bresenham edge-walker
                        // 1-px-per-scanline fattening for sub-pixel-rotated
                        // edges. Multi-claim avoidance: only the texel whose
                        // own outer edge is closest claims the pixel (band
                        // = 0.5 fb-px is < texel half-width for typical sprite
                        // sizes, so adjacent texel's same-side outer edge is
                        // out of band).
                        vec2 pTest = vec2(float(x) + 0.5, float(y) + 0.5);
                        bool insideTex = insidePolygonQuad(pTest, p00, p10, p11, p01);
                        if (!insideTex && isEdgeTexel && !axisAligned) {
                            float thrSq_o = EDGE_BAND_HD * EDGE_BAND_HD;
                            if (isLeftEdgeTexel  && isOnSegment(pTest, vec2(c.v0), v3e, thrSq_o)) insideTex = true;
                            if (!insideTex && isRightEdgeTexel && isOnSegment(pTest, v1e, v2e, thrSq_o)) insideTex = true;
                            if (!insideTex && isTopEdgeTexel   && isOnSegment(pTest, vec2(c.v0), v1e, thrSq_o)) insideTex = true;
                            if (!insideTex && isBotEdgeTexel   && isOnSegment(pTest, v3e, v2e, thrSq_o)) insideTex = true;
                        }
                        if (!insideTex) continue;
                    }
                    // Sprite row per pixel: the grid row v is line v, but on a
                    // rotated or distorted sprite the line that actually passes
                    // through this pixel (measured at the Saturn cell centre,
                    // see spriteTexRow) can be a neighbour. Use that line's row
                    // so forward picks the same row as tile_shade.
                    vec4 pixSrc = src;
                    if (!isPolygon) {
                        uint rowP = min(spriteTexRow(vec2(float(x) + 0.5, float(y) + 0.5), c),
                                        effCharSize.y - 1u);
                        if (rowP != t.y) {
                            pixSrc = sampleSpriteTexel(c, uvec2(t.x, rowP), cm, spd, endcEnabled);
                            pixSrc.rgb = applyGouraud(pixSrc.rgb, vec2(
                                (float(u) + 0.5) / float(effCharSize.x),
                                (float(v) + 0.5) / float(lineCount)), c);
                        }
                        if (pixSrc.a < 0.5) continue;
                    }
                    // T1B.11: per-pixel POLYGON Gouraud. The 1x1 virtual
                    // texel collapses sprite-path per-texel gouraudUv to a
                    // uniform (0.5, 0.5), so for POLYGON we solve inverse-
                    // bilinear at the actual pixel center to get the real
                    // (s, t) the 4-vertex gouraud table is parameterized
                    // over. SPRITE keeps its existing per-texel path.
                    vec4 finalSrc = pixSrc;
                    if (isPolygon && c.gouraudAddr.y != 0u) {
                        vec2 polyUv = computeUV(
                            vec2(float(x) + 0.5, float(y) + 0.5), c);
                        finalSrc.rgb = applyGouraud(finalSrc.rgb, polyUv, c);
                    }
                    ivec2 imgPix = ivec2(x, int(pc.fbHeight) - 1 - y);
                    // Phase 1B.7: RMW for CMDPMOD blend (Replace / Shadow /
                    // Half-lum / Half-trans) + Mesh + MSB shadow. Saturn-
                    // space (x, y) feeds applyBlend mesh checker.
                    //
                    // pmod-aware: when needsDst=false (cc==0/2) we skip
                    // imageLoad. For mesh, applyBlendNoDst returns false
                    // so imageStore is also skipped (functionally
                    // equivalent).
                    if (needsDst) {
                        vec4 dst = imageLoad(fb, imgPix);
                        vec4 result = applyBlend(finalSrc, dst, c.pmod, ivec2(x, y));
                        imageStore(fb, imgPix, result);
                    } else {
                        vec4 result;
                        if (applyBlendNoDst(result, finalSrc, c.pmod, ivec2(x, y))) {
                            imageStore(fb, imgPix, result);
                        }
                    }
                }
            }
        }
    }
}

#ifdef TILE_MODE
void main() {
    uvec2 tileXY = gl_WorkGroupID.xy;
    int tileMinX = int(tileXY.x) * int(pc.tileSize);
    int tileMinY = int(tileXY.y) * int(pc.tileSize);
    int tileMaxX = min(tileMinX + int(pc.tileSize), int(pc.fbWidth));
    int tileMaxY = min(tileMinY + int(pc.tileSize), int(pc.fbHeight));
    ivec4 clipRect = ivec4(tileMinX, tileMinY, tileMaxX, tileMaxY);


    uint tileIdx = tileXY.y * pc.numTilesX + tileXY.x;
    uint lid     = gl_LocalInvocationIndex;

    // Co-load + sort tile cmds (mirrors tile_shade.comp pattern).
    if (lid == 0u) {
        tileCmdCount_shared = min(tiles[tileIdx].count, uint(MAX_CMDS_PER_TILE));
    }
    barrier();
    uint n = tileCmdCount_shared;
    if (lid < n) {
        sortedCmds[lid] = cmdIdx[tileIdx * uint(MAX_CMDS_PER_TILE) + lid];
    }
    barrier();
    if (lid == 0u) {
        for (uint i = 1u; i < n; ++i) {
            uint key = sortedCmds[i];
            uint j = i;
            while (j > 0u && sortedCmds[j - 1u] > key) {
                sortedCmds[j] = sortedCmds[j - 1u];
                j--;
            }
            sortedCmds[j] = key;
        }
    }
    barrier();

    // Iterate sorted cmds. Cross-cmd LIFO sync within workgroup only --
    // tiles are pixel-disjoint so workgroup-scope flush is sufficient.
    // Avoid memoryBarrierImage() (device scope) which Adreno expands to
    // a UCHE/L1 image cache flush; per-tile per-cmd cost adds up fast.
    for (uint ci = 0u; ci < n; ++ci) {
        Vdp1Cmd c = cmds[sortedCmds[ci]];
        processCmd(c, clipRect);
        groupMemoryBarrier();
        barrier();
    }
}
#else
void main() {
    // Per-cmd dispatch (existing batch dispatch path).
    uint cmdIdx = pc.cmdIndex + gl_WorkGroupID.x;
    Vdp1Cmd c = cmds[cmdIdx];
    ivec4 fullScreen = ivec4(0, 0, int(pc.fbWidth), int(pc.fbHeight));
    processCmd(c, fullScreen);
}
#endif
)GLSL";

void createBufferRaw(VIDVulkan* vulkan, VkDevice device,
                     VkDeviceSize size, VkBufferUsageFlags usage,
                     VkMemoryPropertyFlags props,
                     VkBuffer& outBuf, VkDeviceMemory& outMem) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bi, nullptr, &outBuf) != VK_SUCCESS) {
        throw std::runtime_error("Vdp1ComputeRasterizer: vkCreateBuffer failed");
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, outBuf, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = vulkan->findMemoryType(req.memoryTypeBits, props);
    if (vkAllocateMemory(device, &ai, nullptr, &outMem) != VK_SUCCESS) {
        vkDestroyBuffer(device, outBuf, nullptr);
        outBuf = VK_NULL_HANDLE;
        throw std::runtime_error("Vdp1ComputeRasterizer: vkAllocateMemory failed");
    }
    vkBindBufferMemory(device, outBuf, outMem, 0);
}

}  // anonymous namespace

Vdp1ComputeRasterizer::Vdp1ComputeRasterizer(VIDVulkan* vulkan, int fbWidth, int fbHeight)
    : vulkan(vulkan), fbWidth(fbWidth), fbHeight(fbHeight)
{
    cpuCmds.reserve(VDP1C_MAX_CMDS);
    dispatchableIndices.reserve(VDP1C_MAX_CMDS);
    createBuffers();
    recreateTileBuffers();
    createDescriptorLayouts();
    createPipelines();
}

Vdp1ComputeRasterizer::~Vdp1ComputeRasterizer() {
    destroyPipelines();
    destroyTileBuffers();
    destroyBuffers();
}

void Vdp1ComputeRasterizer::beginFrame() {
    // Fire snapshot hook (if registered) before tearing down the previous frame's
    // command list. Debug UI uses this to capture the just-completed frame.
    if (preBeginFrameHook) preBeginFrameHook();
    cpuCmds.clear();
    dispatchableIndices.clear();
    vdp1c::resetState(state);
    // Reset per-frame feature flags used to pick the shade pipeline variant.
    // Each appendCommand sets these on demand; absent -> spec const eliminates
    // the corresponding shader code path at compile time.
    frameUsesUserClip  = false;
    frameUsesGouraud   = false;
    frameUsesMesh      = false;
    frameUsesMsbShadow = false;
}

void Vdp1ComputeRasterizer::appendCommand(const Vdp1Cmd& cmd) {
    if (cpuCmds.size() >= VDP1C_MAX_CMDS) {
        std::cerr << "[Vdp1ComputeRasterizer] cmd buffer overflow at "
                  << VDP1C_MAX_CMDS << ", dropping command" << std::endl;
        return;
    }
    Vdp1Cmd c = cmd;
    vdp1c::applyState(c, state);
    if (c.clipMode != 0u)        frameUsesUserClip  = true;
    if (c.gouraudAddr.y != 0u)   frameUsesGouraud   = true;
    if ((c.pmod & 0x100u) != 0u) frameUsesMesh      = true;
    if ((c.pmod & 0x8000u) != 0u) frameUsesMsbShadow = true;
    const uint32_t idx = static_cast<uint32_t>(cpuCmds.size());
    cpuCmds.push_back(c);
    // Mirror collectDispatchIndices() filter -- every non-NOOP type since F12
    // (POLYGON / DISTORTED_SPRITE / NORMAL_SPRITE / SCALED_SPRITE / POLYLINE
    // / LINE). Plan 1 only pushed POLYGON / DISTORTED here, which silently
    // dropped Normal/Scaled/Polyline/Line entries that F12 added -- making
    // dispatchableIndices.size() < cpuCmds.size() and confusing the
    // numCmdsToBin sizing in dispatchUpTo() (it iterates cmds[0..N-1] by
    // index, so an undercount drops the tail of cpuCmds).
    if (c.cmdType != VDP1C_TYPE_NOOP) {
        dispatchableIndices.push_back(idx);
    }
}

void Vdp1ComputeRasterizer::dispatch(VkCommandBuffer cb,
                                      VkImage targetImage, VkImageView targetView,
                                      VkImageLayout currentLayout, int frameIndex,
                                      const StageMarkerFn& markStage) {
    // Default: dispatch every command, no override.
    dispatchUpTo(cb, targetImage, targetView, currentLayout, UINT32_MAX, frameIndex, nullptr, markStage);
}

void Vdp1ComputeRasterizer::dispatchUpTo(VkCommandBuffer cb,
                                          VkImage targetImage, VkImageView targetView,
                                          VkImageLayout currentLayout,
                                          uint32_t lastCmdIndex,
                                          int frameIndex,
                                          const std::vector<Vdp1Cmd>* cmdsOverride,
                                          const StageMarkerFn& markStage)
{
    // Clamp frameIndex to valid slot range.
    if (frameIndex < 0 || frameIndex >= 2) frameIndex = 0;
    const std::vector<Vdp1Cmd>& cmds = cmdsOverride ? *cmdsOverride : cpuCmds;

    // Frame debugger: reset per-call counters. Paused step replays will see
    // this single dispatchUpTo()'s numbers, which matches what the user
    // intuitively means by "this frame's dispatches".
    lastDispatchStats = {};

    if (cmds.empty()) return;

    // Map currentLayout to (srcAccessMask, srcStageMask) for the
    // -> GENERAL transition. erase() leaves the offscreen image in
    // COLOR_ATTACHMENT_OPTIMAL with COLOR_ATTACHMENT_WRITE pending; using
    // a hard-coded SHADER_READ_ONLY oldLayout caused Adreno to discard our
    // compute writes (Windows desktop drivers were lenient and hid this).
    VkAccessFlags        layoutSrcAccess = 0u;
    VkPipelineStageFlags layoutSrcStage  = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    switch (currentLayout) {
      case VK_IMAGE_LAYOUT_UNDEFINED:
        layoutSrcAccess = 0u;
        layoutSrcStage  = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        break;
      case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        layoutSrcAccess = VK_ACCESS_SHADER_READ_BIT;
        layoutSrcStage  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        break;
      case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        layoutSrcAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        layoutSrcStage  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        break;
      case VK_IMAGE_LAYOUT_GENERAL:
        layoutSrcAccess = VK_ACCESS_SHADER_WRITE_BIT;
        layoutSrcStage  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        break;
      case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        // Vdp1Renderer's compute path now blits pending Saturn CPU
        // framebuffer writes into the offscreen image immediately before
        // recording the compute dispatch (matches the per-pipeline blit
        // in the graphics path). After that blit the image is left in
        // TRANSFER_DST_OPTIMAL with a pending TRANSFER_WRITE, so we must
        // chain srcAccess=TRANSFER_WRITE_BIT / srcStage=TRANSFER_BIT here
        // to keep the blitted bytes visible to the compute shader. The
        // TOP_OF_PIPE fallback below would discard them on Adreno
        // (same class of bug noted in the COLOR_ATTACHMENT_OPTIMAL
        // comment above).
        layoutSrcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
        layoutSrcStage  = VK_PIPELINE_STAGE_TRANSFER_BIT;
        break;
      default:
        // Fall back to TOP_OF_PIPE-discard if caller passes an unexpected
        // layout. Not strictly correct but avoids an outright validation
        // error; in practice only the four cases above ever appear.
        layoutSrcAccess = 0u;
        layoutSrcStage  = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        break;
    }

    // 0) Upload command SSBO (HOST_VISIBLE persistent map).
    if (cmdsOverride) {
        // Step UI feeds an alternative command list (e.g. truncated copy);
        // copy directly without touching cpuCmds.
        memcpy(cmdSSBOMapped, cmdsOverride->data(), sizeof(Vdp1Cmd) * cmdsOverride->size());
    } else {
        uploadCommands();
    }

    // -------------------------------------------------------------------------
    // Phase 1A/1B: Per-texel forward mapping (Saturn-faithful).
    //
    // When useForwardMapping is enabled, sprites (Normal/Scaled/Distorted)
    // and POLYGON (1B.5) render via the forward shader. Polyline/Line are
    // NOT rendered in this mode yet -- they remain on tile_shade until a
    // later 1B sub-task migrates them. Phase 1C will make forward the
    // default and demote tile_shade. See:
    // docs/superpowers/specs/2026-05-08-vdp1-compute-forward-mapping-design.md
    // -------------------------------------------------------------------------
    if (useForwardMapping) {
        lastDispatchStats.usedForwardPath = true;

        // Sprint 5 (2026-05-10): unified on tile-forward. The frame-adaptive
        // heuristic from Sprint 3-4 was removed for these reasons:
        //   - cmds-count / bbox-size thresholds are scene-dependent and the
        //     resulting behavior is unpredictable.
        //   - per-batch-forward has O(N) overhead in cmd-heavy scenes
        //     (observed 124 ms at 746 cmds vs ~60 ms for tile-forward).
        //   - On Windows desktop GPUs, tile-forward is fastest across every
        //     scene we measured (and keeps the compute-only invariant).
        //
        // Open issue: on Adreno, tile-forward suffers from redundant
        // rasterization of full-screen cmds. That will be addressed *inside*
        // tile-forward (per-texel range pre-cull, sprite fast path, mediump,
        // SSBO -> texelBuffer migration), not by switching paths.
        //
        // When useTileForward = false (DebugUI toggle), we fall through to
        // per-batch-forward, but no auto-select happens. The only automatic
        // fallback is when the tile SSBOs have not been initialized yet.
        bool useTileForwardThisFrame = useTileForward
            && tileCountSSBO != VK_NULL_HANDLE
            && tileCmdListSSBO != VK_NULL_HANDLE;
        const uint32_t totalCmds = static_cast<uint32_t>(cmds.size());
        const uint32_t scanLimit =
            (lastCmdIndex == UINT32_MAX)
                ? totalCmds
                : std::min<uint32_t>(lastCmdIndex + 1u, totalCmds);

        // Diagnostic log: cmd type distribution + tile occupancy + chosen
        // mode. Runs regardless of which mode the heuristic picked, so we
        // can see *why* a frame went per-batch instead of tile-forward.
        // 60 dispatches per log line keeps overhead negligible.
        {
            static uint32_t s_tileLogCounter = 0;
            if ((s_tileLogCounter++ % 60u) == 0u
                && numTilesX > 0 && numTilesY > 0) {
                const uint32_t totalTiles =
                    static_cast<uint32_t>(numTilesX) *
                    static_cast<uint32_t>(numTilesY);
                std::vector<uint16_t> tileCount(totalTiles, 0);
                uint32_t totalCmdTilePairs = 0;
                uint32_t typeCount[7] = {0, 0, 0, 0, 0, 0, 0};
                uint32_t largeBboxCount = 0;
                const int64_t halfArea =
                    static_cast<int64_t>(fbWidth) *
                    static_cast<int64_t>(fbHeight) / 2;
                for (uint32_t i = 0; i < scanLimit; ++i) {
                    const Vdp1Cmd& cmd = cmds[i];
                    if (cmd.cmdType == VDP1C_TYPE_NOOP) continue;
                    if (cmd.cmdType < 7u) typeCount[cmd.cmdType]++;
                    int bx0 = std::max(cmd.bbox[0], 0);
                    int by0 = std::max(cmd.bbox[1], 0);
                    int bx1 = std::min(cmd.bbox[2], cmd.systemClip[0]);
                    int by1 = std::min(cmd.bbox[3], cmd.systemClip[1]);
                    if (bx1 < bx0 || by1 < by0) continue;
                    int64_t bw = static_cast<int64_t>(cmd.bbox[2]) -
                                 static_cast<int64_t>(cmd.bbox[0]);
                    int64_t bh = static_cast<int64_t>(cmd.bbox[3]) -
                                 static_cast<int64_t>(cmd.bbox[1]);
                    if (bw * bh >= halfArea) ++largeBboxCount;
                    int tx0 = std::max(0, bx0 / TILE_SIZE_PX);
                    int ty0 = std::max(0, by0 / TILE_SIZE_PX);
                    int tx1 = std::min(numTilesX - 1, bx1 / TILE_SIZE_PX);
                    int ty1 = std::min(numTilesY - 1, by1 / TILE_SIZE_PX);
                    for (int ty = ty0; ty <= ty1; ++ty) {
                        for (int tx = tx0; tx <= tx1; ++tx) {
                            uint32_t idx =
                                static_cast<uint32_t>(ty) *
                                static_cast<uint32_t>(numTilesX) +
                                static_cast<uint32_t>(tx);
                            if (tileCount[idx] < TILE_MAX_CMDS) {
                                tileCount[idx]++;
                            }
                            totalCmdTilePairs++;
                        }
                    }
                }
                uint32_t nonEmpty = 0;
                uint16_t maxPerTile = 0;
                for (uint16_t v : tileCount) {
                    if (v > 0) ++nonEmpty;
                    if (v > maxPerTile) maxPerTile = v;
                }
                LOGI("[VKPROF-TILE] mode=%s cmds=%u tilesXY=%dx%d=%u "
                     "nonEmpty=%u (%.1f%%) maxPerTile=%u "
                     "avgCmdsPerNonEmpty=%.2f totalPairs=%u "
                     "types: NS=%u SS=%u DS=%u POLY=%u PLINE=%u LINE=%u "
                     "largeBbox=%u\n",
                     useTileForwardThisFrame ? "tile-forward"
                                             : "per-batch-forward",
                     scanLimit, numTilesX, numTilesY, totalTiles,
                     nonEmpty,
                     totalTiles ? 100.0 * nonEmpty / totalTiles : 0.0,
                     static_cast<uint32_t>(maxPerTile),
                     nonEmpty ? static_cast<double>(totalCmdTilePairs) /
                                static_cast<double>(nonEmpty) : 0.0,
                     totalCmdTilePairs,
                     typeCount[1], typeCount[2], typeCount[3],
                     typeCount[4], typeCount[5], typeCount[6],
                     largeBboxCount);
            }
        }

        // Tile-binning forward (perf opt 2026-05-08). When enabled, run
        // the existing Bin pass to scatter cmds into per-tile lists, then
        // dispatch a single tile-forward shade pass (one workgroup per
        // 16x16 tile). LIFO order is preserved by intra-workgroup
        // memoryBarrierImage()/barrier() between cmds in the shader.
        // Total: 2 vkCmdDispatch + 3 vkCmdPipelineBarrier per frame,
        // independent of cmd count.
        if (useTileForwardThisFrame) {
            lastDispatchStats.usedTileForwardPath = true;

            // Step UI replay caps cmd range. Bin shader filters by cmdType
            // so feeding extra cmds is safe but wasteful.
            const bool fastPath = (lastCmdIndex == UINT32_MAX) && (cmdsOverride == nullptr);
            uint32_t numCmdsToBin;
            if (fastPath) {
                numCmdsToBin = static_cast<uint32_t>(dispatchableIndices.size());
            } else {
                uint32_t cap = static_cast<uint32_t>(cmds.size());
                if (lastCmdIndex != UINT32_MAX) {
                    const uint32_t limit = lastCmdIndex + 1u;
                    if (limit < cap) cap = limit;
                }
                numCmdsToBin = cap;
            }
            if (numCmdsToBin == 0u) return;

            // pmod-aware stats (mirror per-batch path so the DebugUI shows
            // the same skip/need ratio regardless of which dispatch mode is
            // active).
            for (uint32_t i = 0; i < numCmdsToBin; ++i) {
                const Vdp1Cmd& cmd = cmds[i];
                if (cmd.cmdType == VDP1C_TYPE_NOOP) continue;
                uint32_t cc = cmd.pmod & 0x3u;
                if (cc == 1u || cc == 3u) {
                    lastDispatchStats.cmdsNeedImageLoad++;
                } else {
                    lastDispatchStats.cmdsSkipImageLoad++;
                }
                lastDispatchStats.totalCmdsDispatched++;
            }

            updateTileDescriptorSets(targetView, frameIndex);  // bin descriptor set re-bind
            updateForwardDescriptorSets(targetView, frameIndex);
            resetTileCount(cb);

            // Barrier: TileCount fill (transfer) -> Bin shader read.
            {
                VkMemoryBarrier mb{};
                mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                vkCmdPipelineBarrier(cb,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0, 1, &mb, 0, nullptr, 0, nullptr);
                lastDispatchStats.pipelineBarrierCount++;
            }

            // Bin pass.
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, binPipeline);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                binPipeLayout, 0, 1, &binDescSet[frameIndex], 0, nullptr);
            BinPushConstants binPc{};
            binPc.numCmds   = numCmdsToBin;
            binPc.numTilesX = static_cast<uint32_t>(numTilesX);
            binPc.numTilesY = static_cast<uint32_t>(numTilesY);
            binPc.tileSize  = static_cast<uint32_t>(TILE_SIZE_PX);
            vkCmdPushConstants(cb, binPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(binPc), &binPc);
            vkCmdDispatch(cb, (numCmdsToBin + 63u) / 64u, 1, 1);
            lastDispatchStats.dispatchCount++;

            // Stage marker: end of Bin pass (inside the same compute queue).
            // Subsequent timing up to the next marker covers the inter-pass
            // barrier + image transition + the Forward dispatch.
            if (markStage) markStage("bin");

            // Barrier: Bin SSBO writes -> tile-forward shader reads.
            {
                VkMemoryBarrier mb{};
                mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cb,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0, 1, &mb, 0, nullptr, 0, nullptr);
                lastDispatchStats.pipelineBarrierCount++;
            }

            // Image transition: currentLayout -> GENERAL for tile-forward fb
            // writes. Source access/stage derived from currentLayout (see top
            // of dispatchUpTo) so we honor erase()'s COLOR_ATTACHMENT layout
            // correctly on Adreno.
            {
                VkImageMemoryBarrier ib{};
                ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                ib.oldLayout = currentLayout;
                ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                ib.image = targetImage;
                ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                ib.srcAccessMask = layoutSrcAccess;
                ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                vkCmdPipelineBarrier(cb,
                    layoutSrcStage,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &ib);
                lastDispatchStats.pipelineBarrierCount++;
                firstDispatch = false;
            }

            // Tile-forward shade pass. One workgroup per 16x16 tile;
            // entire frame in a single dispatch.
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, tileForwardPipeline);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                forwardPipeLayout, 0, 1, &forwardDescSet[frameIndex], 0, nullptr);
            ForwardPushConstants forwardPc{};
            forwardPc.fbWidth        = static_cast<uint32_t>(fbWidth);
            forwardPc.fbHeight       = static_cast<uint32_t>(fbHeight);
            forwardPc.scaleMax       = state.scaleMax;
            forwardPc.spriteType     = state.spriteType;
            forwardPc.cmdIndex       = 0;  // unused in tile mode
            forwardPc.numTilesX      = static_cast<uint32_t>(numTilesX);
            forwardPc.tileSize       = static_cast<uint32_t>(TILE_SIZE_PX);
            forwardPc.maxCmdsPerTile = static_cast<uint32_t>(TILE_MAX_CMDS);
            forwardPc.scaleX         = state.scaleX;
            forwardPc.scaleY         = state.scaleY;
            vkCmdPushConstants(cb, forwardPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(forwardPc), &forwardPc);
            vkCmdDispatch(cb,
                static_cast<uint32_t>(numTilesX),
                static_cast<uint32_t>(numTilesY),
                1);
            lastDispatchStats.dispatchCount++;

            // Stage marker: end of tile-forward shade pass. Anything after
            // this until endSample (the GENERAL -> SHADER_READ_ONLY image
            // transition) is reported as the "tail" delta.
            if (markStage) markStage("forward");

            // Image transition: GENERAL -> SHADER_READ_ONLY for VDP2 sampler.
            {
                VkImageMemoryBarrier ib{};
                ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                ib.image = targetImage;
                ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                ib.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cb,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &ib);
                lastDispatchStats.pipelineBarrierCount++;
            }
            return;
        }

        // Image transition: ANY -> GENERAL for forward dispatch fb writes.
        {
            VkImageMemoryBarrier ib{};
            ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            ib.oldLayout = currentLayout;
            ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ib.image = targetImage;
            ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            ib.srcAccessMask = layoutSrcAccess;
            ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cb,
                layoutSrcStage,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &ib);
            lastDispatchStats.pipelineBarrierCount++;
            firstDispatch = false;
        }

        updateForwardDescriptorSets(targetView, frameIndex);

        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, forwardPipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
            forwardPipeLayout, 0, 1, &forwardDescSet[frameIndex], 0, nullptr);
        ForwardPushConstants forwardPc{};
        forwardPc.fbWidth    = static_cast<uint32_t>(fbWidth);
        forwardPc.fbHeight   = static_cast<uint32_t>(fbHeight);
        forwardPc.scaleMax   = state.scaleMax;
        forwardPc.spriteType = state.spriteType;
        forwardPc.scaleX     = state.scaleX;
        forwardPc.scaleY     = state.scaleY;

        // Step UI replay: lastCmdIndex caps the dispatch range to
        // [0, lastCmdIndex] so only commands up to the selected step are
        // rendered (matches the tile-binning path's cap above). Without
        // this, stepping forward to cmd N still draws every frame command.
        uint32_t numCmds = static_cast<uint32_t>(cmds.size());
        if (lastCmdIndex != UINT32_MAX) {
            const uint32_t limit = lastCmdIndex + 1u;
            if (limit < numCmds) numCmds = limit;
        }

        // Issue #1 + perf opt (2026-05-08): batch dispatch of bbox-disjoint cmds.
        //
        // The old per-cmd dispatch issued one dispatch + one memory barrier
        // per cmd to preserve VDP1 cmd submission order (LIFO blend). At
        // numCmds=1000 that becomes 1000 dispatches + 1000 barriers, heavy
        // on both CPU and GPU (real devices reported a serious fps drop
        // before this optimization).
        //
        // Observation: cmd N+1 can only affect pixels that cmd N wrote
        // when N+1 shares pixels with N (Half-trans / Shadow imageLoad,
        // or Replace overwrite order). cmds with disjoint bboxes are
        // pixel-disjoint and run safely in parallel without changing the
        // visible result.
        //
        // Strategy: greedy-scan the cmd list; if a cmd's bbox does not
        // overlap the current batch's union bbox, append it to the batch.
        // Otherwise flush the batch (= 1 vkCmdDispatch with
        // workgroupCount=batchSize + 1 memory barrier) and start a new
        // batch. Inside the shader cmdIdx = pc.cmdIndex (batch start) +
        // gl_WorkGroupID.x (offset within batch).
        //
        // Complexity: O(N) batching, O(1) overlap test per cmd. Average
        // batch size is scene dependent: 50+ for tilemaps, 5-10 for
        // sprite-heavy scenes, 1-3 for HUD-heavy overlap. Worst case
        // matches the old per-cmd dispatch count.
        auto bboxOverlaps = [](const glm::ivec4& a, const glm::ivec4& b) -> bool {
            // bbox = (xmin, ymin, xmax, ymax) inclusive. Overlap = "cannot
            // separate": !(a fully right of b || left of b || below b ||
            // above b).
            return !(a.x > b.z || a.z < b.x || a.y > b.w || a.w < b.y);
        };

        uint32_t batchStart = 0;
        glm::ivec4 unionBbox{INT_MAX, INT_MAX, INT_MIN, INT_MIN};
        bool unionValid = false;

        auto flushBatch = [&](uint32_t end) {
            if (end <= batchStart) return;
            const uint32_t batchSize = end - batchStart;
            forwardPc.cmdIndex = batchStart;
            vkCmdPushConstants(cb, forwardPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(forwardPc), &forwardPc);
            vkCmdDispatch(cb, batchSize, 1, 1);

            // Frame debugger: track dispatch + batch breakdown.
            lastDispatchStats.dispatchCount++;
            lastDispatchStats.batchCount++;
            lastDispatchStats.totalCmdsDispatched += batchSize;
            if (lastDispatchStats.minBatchSize == 0u
                || batchSize < lastDispatchStats.minBatchSize) {
                lastDispatchStats.minBatchSize = batchSize;
            }
            if (batchSize > lastDispatchStats.maxBatchSize) {
                lastDispatchStats.maxBatchSize = batchSize;
            }

            // Memory barrier: serialize so the imageStore inside this
            // batch is visible before the imageLoad / imageStore of the
            // next batch.
            VkMemoryBarrier mb{};
            mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cb,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 1, &mb, 0, nullptr, 0, nullptr);
            lastDispatchStats.pipelineBarrierCount++;

            batchStart = end;
            unionBbox = {INT_MAX, INT_MAX, INT_MIN, INT_MIN};
            unionValid = false;
        };

        for (uint32_t i = 0; i < numCmds; ++i) {
            const Vdp1Cmd& cmd = cmds[i];
            const glm::ivec4 b = cmd.bbox;
            // pmod-aware: only count drawing cmds (NOOP / state-only cmds
            // early-return in the shader). cc==0 Replace / cc==2 Half-lum
            // do not read dst and skip imageLoad. cc==1 Shadow /
            // cc==3 Half-trans require imageLoad.
            if (cmd.cmdType != VDP1C_TYPE_NOOP) {
                uint32_t cc = cmd.pmod & 0x3u;
                if (cc == 1u || cc == 3u) {
                    lastDispatchStats.cmdsNeedImageLoad++;
                } else {
                    lastDispatchStats.cmdsSkipImageLoad++;
                }
            }
            // Degenerate bbox check (xmax < xmin etc.). NOOP / state-only
            // cmds may lack a valid drawing range after applyState, but
            // the shader's cmdType filter early-returns them, so here we
            // assume well-formed bboxes for drawing cmds.
            if (unionValid && bboxOverlaps(unionBbox, b)) {
                flushBatch(i);
            }
            if (!unionValid) {
                unionBbox = b;
                unionValid = true;
            } else {
                unionBbox.x = std::min(unionBbox.x, b.x);
                unionBbox.y = std::min(unionBbox.y, b.y);
                unionBbox.z = std::max(unionBbox.z, b.z);
                unionBbox.w = std::max(unionBbox.w, b.w);
            }
        }
        flushBatch(numCmds);

        // Stage marker: end of per-batch-forward dispatch sequence. With
        // tile-forward we report bin/forward; here there is no Bin pass, so
        // the stage names are "batch_setup" (everything up to first dispatch
        // = trivially small, included in the forward measurement) and
        // "forward" (sum of batchCount dispatches + their inter-batch
        // barriers). Logged as `forward=` so it stays comparable to the
        // tile-forward reading.
        if (markStage) markStage("forward");

        // Image transition: GENERAL -> SHADER_READ_ONLY for VDP2 sampler.
        {
            VkImageMemoryBarrier ib{};
            ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ib.image = targetImage;
            ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            ib.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cb,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &ib);
            lastDispatchStats.pipelineBarrierCount++;
        }
        return;  // forward path complete; skip tile-binning below
    }

    // -------------------------------------------------------------------------
    // Phase B1: Tile-binning (B-lite) 2-pass dispatch.
    //
    // Pass 1 (Bin):
    //   - Reset TileCount to 0 via vkCmdFillBuffer
    //   - Barrier (TRANSFER_WRITE -> SHADER_READ on TileCount)
    //   - Dispatch Bin shader: 1 thread per cmd, atomicAdd into TileCount,
    //     write into TileCmdList
    // Barrier (SHADER_WRITE -> SHADER_READ on Tile* SSBOs).
    // Pass 2 (Shade):
    //   - Image transition (ANY -> GENERAL) on fb
    //   - Dispatch Shade shader: 1 WG per tile (16x16 px), 1 thread per pixel,
    //     atomic-free RMW within thread for Replace blend
    //   - Image transition (GENERAL -> SHADER_READ_ONLY) for VDP2 sampler
    //
    // This collapses the per-command dispatch + barrier chain (Phase A) into
    // exactly 2 dispatches per frame, regardless of how many cmds.
    // -------------------------------------------------------------------------
    if (useTileBinning && tileCountSSBO != VK_NULL_HANDLE && tileCmdListSSBO != VK_NULL_HANDLE) {
        lastDispatchStats.usedTilePath = true;
        // Source of dispatchable count for the Bin shader:
        //   fast path  -> dispatchableIndices.size(); appendCommand pushes every
        //                non-NOOP cmd here, so this equals cpuCmds.size() and
        //                lets the bin shader iterate cmds[0..N-1] over every
        //                cpuCmd. (Plan 1 only pushed POLYGON/DISTORTED here;
        //                F12 widened the filter once Normal/Scaled/Polyline/
        //                Line started landing in cpuCmds -- stale POLYGON-only
        //                filter caused the tail of cpuCmds to be silently
        //                dropped, which is what the NiGHTS missing-character
        //                regression turned out to be.)
        //   debug path -> cmds.size() (Bin shader filters by cmdType anyway),
        //                capped by lastCmdIndex+1 so the step UI replay only
        //                bins commands [0, lastCmdIndex]. Without the cap,
        //                step N always shows the full frame because every
        //                command in the override list ends up binned and
        //                shaded.
        const bool fastPath = (lastCmdIndex == UINT32_MAX) && (cmdsOverride == nullptr);
        uint32_t numCmdsToBin;
        if (fastPath) {
            numCmdsToBin = static_cast<uint32_t>(dispatchableIndices.size());
        } else {
            uint32_t cap = static_cast<uint32_t>(cmds.size());
            if (lastCmdIndex != UINT32_MAX) {
                const uint32_t limit = lastCmdIndex + 1u;
                if (limit < cap) cap = limit;
            }
            numCmdsToBin = cap;
        }
        if (numCmdsToBin == 0u) return;

        // Sprint 7: tile_shade (inverse mapping) path entry marker. Mirrors
        // the [VKPROF-TILE] cadence so the user can confirm in logcat that
        // the Android default is exercising tile_shade and not forward.
        {
            static uint32_t s_shadeLogCounter = 0;
            if ((s_shadeLogCounter++ % 60u) == 0u) {
                LOGI("[VKPROF-TILE] mode=tile-shade cmds=%u tilesXY=%dx%d=%u\n",
                     numCmdsToBin, numTilesX, numTilesY,
                     static_cast<uint32_t>(numTilesX) *
                     static_cast<uint32_t>(numTilesY));
            }
        }

        // Bind cached desc set + reset TileCount.
        updateTileDescriptorSets(targetView, frameIndex);
        resetTileCount(cb);

        // Barrier: vkCmdFillBuffer is a transfer; Bin shader reads as storage.
        {
            VkMemoryBarrier mb{};
            mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cb,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 1, &mb, 0, nullptr, 0, nullptr);
            lastDispatchStats.pipelineBarrierCount++;
        }

        // Pass 1 -- Bin dispatch.
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, binPipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
            binPipeLayout, 0, 1, &binDescSet[frameIndex], 0, nullptr);
        BinPushConstants binPc{};
        binPc.numCmds   = numCmdsToBin;
        binPc.numTilesX = static_cast<uint32_t>(numTilesX);
        binPc.numTilesY = static_cast<uint32_t>(numTilesY);
        binPc.tileSize  = static_cast<uint32_t>(TILE_SIZE_PX);
        vkCmdPushConstants(cb, binPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(binPc), &binPc);
        vkCmdDispatch(cb, (numCmdsToBin + 63u) / 64u, 1, 1);
        lastDispatchStats.dispatchCount++;

        // Stage marker: end of Bin pass for tile_shade path.
        if (markStage) markStage("bin");

        // Barrier: Bin SSBO writes -> Shade SSBO reads.
        {
            VkMemoryBarrier mb{};
            mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cb,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 1, &mb, 0, nullptr, 0, nullptr);
            lastDispatchStats.pipelineBarrierCount++;
        }

        // Image transition: currentLayout -> GENERAL for shade dispatch fb writes.
        {
            VkImageMemoryBarrier ib{};
            ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            ib.oldLayout = currentLayout;
            ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ib.image = targetImage;
            ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            ib.srcAccessMask = layoutSrcAccess;
            ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cb,
                layoutSrcStage,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &ib);
            lastDispatchStats.pipelineBarrierCount++;
            firstDispatch = false;
        }

        // Pass 2 -- Shade dispatch. Bind the specialized pipeline for the
        // current SPCTL + per-frame feature flags. Cache misses build
        // inline; steady-state frames hit a warm pipeline.
        uint32_t shadeKey = (state.spriteType & 0x3Fu)
                          | (frameUsesUserClip  ? 0x40u  : 0u)
                          | (frameUsesGouraud   ? 0x80u  : 0u)
                          | (frameUsesMesh      ? 0x100u : 0u)
                          | (frameUsesMsbShadow ? 0x200u : 0u);
        VkPipeline shadePipe = getOrBuildShadePipeline(shadeKey);
        if (shadePipe == VK_NULL_HANDLE) shadePipe = shadePipeline; // fallback to default
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, shadePipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
            shadePipeLayout, 0, 1, &shadeDescSet[frameIndex], 0, nullptr);
        ShadePushConstants shadePc{};
        shadePc.numTilesX  = static_cast<uint32_t>(numTilesX);
        shadePc.numTilesY  = static_cast<uint32_t>(numTilesY);
        shadePc.fbWidth    = static_cast<uint32_t>(fbWidth);
        shadePc.fbHeight   = static_cast<uint32_t>(fbHeight);
        shadePc.scaleMax   = state.scaleMax;
        shadePc.spriteType = state.spriteType;
        shadePc.scaleX     = state.scaleX;
        shadePc.scaleY     = state.scaleY;
        vkCmdPushConstants(cb, shadePipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(shadePc), &shadePc);
        vkCmdDispatch(cb,
            static_cast<uint32_t>(numTilesX),
            static_cast<uint32_t>(numTilesY),
            1);
        lastDispatchStats.dispatchCount++;

        // Stage marker: end of tile_shade dispatch.
        if (markStage) markStage("shade");

        // Image transition: GENERAL -> SHADER_READ_ONLY for VDP2 sampler.
        {
            VkImageMemoryBarrier ib{};
            ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ib.image = targetImage;
            ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            ib.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cb,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &ib);
            lastDispatchStats.pipelineBarrierCount++;
        }
        return;  // tile-binning path complete
    }
}

void Vdp1ComputeRasterizer::resize(int w, int h) {
    fbWidth = w;
    fbHeight = h;
    // The tile-binning pipeline (Phase B1) sizes its TileCount / TileCmdList
    // SSBOs by tile count, derived from fb dims, so re-allocate them when
    // resolution changes. recreateTileBuffers() is a no-op when dims have
    // not changed.
    recreateTileBuffers();
    // Force descriptor set re-update on next dispatch since the SSBOs may
    // have new VkBuffer handles. Invalidate both per-frame slots.
    lastShadeTargetView[0] = VK_NULL_HANDLE;
    lastShadeTargetView[1] = VK_NULL_HANDLE;
    lastForwardTargetView[0] = VK_NULL_HANDLE;
    lastForwardTargetView[1] = VK_NULL_HANDLE;
}

void Vdp1ComputeRasterizer::createBuffers() {
    VkDevice device = vulkan->getDevice();

    // CmdSSBO: HOST_VISIBLE | COHERENT, persistently mapped (~2 MB).
    cmdSSBOSize = sizeof(Vdp1Cmd) * VDP1C_MAX_CMDS;
    createBufferRaw(vulkan, device, cmdSSBOSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        cmdSSBO, cmdSSBOMem);
    vkMapMemory(device, cmdSSBOMem, 0, cmdSSBOSize, 0, &cmdSSBOMapped);

    // VRAM SSBO (HOST_VISIBLE+COHERENT, persistently mapped).
    createBufferRaw(vulkan, device, VDP1_VRAM_SIZE,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        vramSSBO, vramSSBOMem);
    if (vkMapMemory(device, vramSSBOMem, 0, VDP1_VRAM_SIZE, 0, &vramSSBOMapped) != VK_SUCCESS) {
        throw std::runtime_error("Vdp1ComputeRasterizer: vkMapMemory(vram) failed");
    }

    // CRAM SSBO (HOST_VISIBLE+COHERENT, persistently mapped).
    createBufferRaw(vulkan, device, VDP2_CRAM_SIZE,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        cramSSBO, cramSSBOMem);
    if (vkMapMemory(device, cramSSBOMem, 0, VDP2_CRAM_SIZE, 0, &cramSSBOMapped) != VK_SUCCESS) {
        throw std::runtime_error("Vdp1ComputeRasterizer: vkMapMemory(cram) failed");
    }

    // Overflow SSBO (F17). Single uint counter, host-mapped so we can read
    // and zero it without GPU transfer commands. Use HOST_COHERENT to skip
    // explicit flushes and let the F14 fence drain handle visibility.
    createBufferRaw(vulkan, device, OVERFLOW_SSBO_SIZE,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        overflowSSBO, overflowSSBOMem);
    if (vkMapMemory(device, overflowSSBOMem, 0, OVERFLOW_SSBO_SIZE, 0, &overflowSSBOMapped) != VK_SUCCESS) {
        throw std::runtime_error("Vdp1ComputeRasterizer: vkMapMemory(overflow) failed");
    }
    memset(overflowSSBOMapped, 0, OVERFLOW_SSBO_SIZE);

    resourcesReady = true;
}

void Vdp1ComputeRasterizer::destroyBuffers() {
    VkDevice device = vulkan->getDevice();
    if (cmdSSBOMapped) {
        vkUnmapMemory(device, cmdSSBOMem);
        cmdSSBOMapped = nullptr;
    }
    if (cmdSSBO)       { vkDestroyBuffer(device, cmdSSBO, nullptr);       cmdSSBO       = VK_NULL_HANDLE; }
    if (cmdSSBOMem)    { vkFreeMemory(device, cmdSSBOMem, nullptr);       cmdSSBOMem    = VK_NULL_HANDLE; }
    if (vramSSBOMem)    { if (vramSSBOMapped) vkUnmapMemory(device, vramSSBOMem); vramSSBOMapped = nullptr; }
    if (vramSSBO)       { vkDestroyBuffer(device, vramSSBO, nullptr);   vramSSBO    = VK_NULL_HANDLE; }
    if (vramSSBOMem)    { vkFreeMemory(device, vramSSBOMem, nullptr);   vramSSBOMem = VK_NULL_HANDLE; }
    if (cramSSBOMem)    { if (cramSSBOMapped) vkUnmapMemory(device, cramSSBOMem); cramSSBOMapped = nullptr; }
    if (cramSSBO)       { vkDestroyBuffer(device, cramSSBO, nullptr);   cramSSBO    = VK_NULL_HANDLE; }
    if (cramSSBOMem)    { vkFreeMemory(device, cramSSBOMem, nullptr);   cramSSBOMem = VK_NULL_HANDLE; }
    if (overflowSSBOMem){ if (overflowSSBOMapped) vkUnmapMemory(device, overflowSSBOMem); overflowSSBOMapped = nullptr; }
    if (overflowSSBO)   { vkDestroyBuffer(device, overflowSSBO, nullptr);   overflowSSBO    = VK_NULL_HANDLE; }
    if (overflowSSBOMem){ vkFreeMemory(device, overflowSSBOMem, nullptr);   overflowSSBOMem = VK_NULL_HANDLE; }
    resourcesReady = false;
}

void Vdp1ComputeRasterizer::recreateTileBuffers() {
    // Allocate (or re-allocate after fb resize) the Phase B1 tile binning
    // SSBOs. Sizing is based on fbWidth/fbHeight; if dims haven't changed
    // since the last call we no-op so we don't burn vkCreateBuffer cycles.
    if (fbWidth <= 0 || fbHeight <= 0) return;
    const int newTilesX = (fbWidth  + TILE_SIZE_PX - 1) / TILE_SIZE_PX;
    const int newTilesY = (fbHeight + TILE_SIZE_PX - 1) / TILE_SIZE_PX;
    if (newTilesX == numTilesX && newTilesY == numTilesY &&
        tileBuffersFbW == fbWidth && tileBuffersFbH == fbHeight &&
        tileCountSSBO != VK_NULL_HANDLE && tileCmdListSSBO != VK_NULL_HANDLE) {
        return;  // already sized correctly
    }
    destroyTileBuffers();
    numTilesX = newTilesX;
    numTilesY = newTilesY;
    tileBuffersFbW = fbWidth;
    tileBuffersFbH = fbHeight;
    const size_t tileN = static_cast<size_t>(numTilesX) * static_cast<size_t>(numTilesY);

    // TileCount: one TileCount struct (16B std430 stride) per tile.
    tileCountSSBOSize = static_cast<VkDeviceSize>(tileN * TILE_COUNT_STRIDE);
    createBufferRaw(vulkan, vulkan->getDevice(), tileCountSSBOSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        tileCountSSBO, tileCountSSBOMem);

    // TileCmdList: TILE_MAX_CMDS uint slots per tile.
    tileCmdListSSBOSize = static_cast<VkDeviceSize>(tileN * TILE_MAX_CMDS * sizeof(uint32_t));
    createBufferRaw(vulkan, vulkan->getDevice(), tileCmdListSSBOSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        tileCmdListSSBO, tileCmdListSSBOMem);
}

void Vdp1ComputeRasterizer::destroyTileBuffers() {
    VkDevice device = vulkan->getDevice();
    if (tileCountSSBO)    { vkDestroyBuffer(device, tileCountSSBO, nullptr);    tileCountSSBO    = VK_NULL_HANDLE; }
    if (tileCountSSBOMem) { vkFreeMemory(device, tileCountSSBOMem, nullptr);    tileCountSSBOMem = VK_NULL_HANDLE; }
    if (tileCmdListSSBO)  { vkDestroyBuffer(device, tileCmdListSSBO, nullptr);  tileCmdListSSBO  = VK_NULL_HANDLE; }
    if (tileCmdListSSBOMem){ vkFreeMemory(device, tileCmdListSSBOMem, nullptr); tileCmdListSSBOMem = VK_NULL_HANDLE; }
    tileCountSSBOSize = 0;
    tileCmdListSSBOSize = 0;
}

uint32_t Vdp1ComputeRasterizer::readAndResetTileOverflow() {
    if (overflowSSBOMapped == nullptr) return 0u;
    uint32_t value;
    memcpy(&value, overflowSSBOMapped, sizeof(uint32_t));
    if (value != 0u) {
        memset(overflowSSBOMapped, 0, OVERFLOW_SSBO_SIZE);
    }
    return value;
}

void Vdp1ComputeRasterizer::logFrameDiagnostic(uint32_t tileOverflow) {
    // Rolling 60-frame window stats. Reset at each window boundary so a
    // long-running session does not just sit at the global max forever.
    static uint32_t s_frame = 0;
    static uint32_t s_winMaxCmds = 0;
    static uint32_t s_winMaxOverflow = 0;
    static uint32_t s_winSpikeCount = 0;
    static uint32_t s_winMeshGouraudFrames = 0;
    static uint32_t s_winUserClipFrames = 0;
    // Cmd-type sum across the 60-frame window (then divided by 60 in log).
    static uint32_t s_winSumNormal    = 0;
    static uint32_t s_winSumScaled    = 0;
    static uint32_t s_winSumDistorted = 0;
    static uint32_t s_winSumPolygon   = 0;
    static uint32_t s_winSumPolyline  = 0;
    static uint32_t s_winSumLine      = 0;
    static uint32_t s_winSumClipCmd   = 0;  // explicit clip-mode cmds (clipMode != 0)

    ++s_frame;
    const uint32_t cmdCount = static_cast<uint32_t>(cpuCmds.size());
    const uint32_t shadeKey = (state.spriteType & 0x3Fu)
                            | (frameUsesUserClip  ? 0x40u  : 0u)
                            | (frameUsesGouraud   ? 0x80u  : 0u)
                            | (frameUsesMesh      ? 0x100u : 0u)
                            | (frameUsesMsbShadow ? 0x200u : 0u);
    const bool meshGouraud = frameUsesMesh && frameUsesGouraud;

    // Per-frame cmd-type breakdown.
    uint32_t cNormal = 0, cScaled = 0, cDistorted = 0;
    uint32_t cPolygon = 0, cPolyline = 0, cLine = 0;
    uint32_t cClipCmd = 0;
    for (const auto& c : cpuCmds) {
        switch (c.cmdType) {
            case VDP1C_TYPE_NORMAL_SPRITE:    ++cNormal;    break;
            case VDP1C_TYPE_SCALED_SPRITE:    ++cScaled;    break;
            case VDP1C_TYPE_DISTORTED_SPRITE: ++cDistorted; break;
            case VDP1C_TYPE_POLYGON:          ++cPolygon;   break;
            case VDP1C_TYPE_POLYLINE:         ++cPolyline;  break;
            case VDP1C_TYPE_LINE:             ++cLine;      break;
            default: break;
        }
        if (c.clipMode != 0u) ++cClipCmd;
    }
    s_winSumNormal    += cNormal;
    s_winSumScaled    += cScaled;
    s_winSumDistorted += cDistorted;
    s_winSumPolygon   += cPolygon;
    s_winSumPolyline  += cPolyline;
    s_winSumLine      += cLine;
    s_winSumClipCmd   += cClipCmd;

    if (cmdCount     > s_winMaxCmds)     s_winMaxCmds     = cmdCount;
    if (tileOverflow > s_winMaxOverflow) s_winMaxOverflow = tileOverflow;
    if (meshGouraud)              ++s_winMeshGouraudFrames;
    if (frameUsesUserClip)        ++s_winUserClipFrames;

    // Lowered spike thresholds: previous 800 cmd cutoff missed sustained
    // 450-cmd scenes that empirically run at ~10 fps on SD865. Now any
    // frame >= 300 cmds OR explicit cmd-type extremes triggers a spike log.
    bool        spike       = false;
    const char* spikeReason = "";
    if (tileOverflow != 0u) {
        spike = true; spikeReason = "tile_overflow";
    } else if (cmdCount >= 300u) {
        spike = true; spikeReason = "cmds_ge_300";
    } else if (cPolyline + cLine >= 100u) {
        spike = true; spikeReason = "polyline_line";
    } else if (cDistorted >= 200u) {
        spike = true; spikeReason = "distorted_heavy";
    }
    if (spike) {
        ++s_winSpikeCount;
        // Throttle spike prints: at 60fps a sustained scene would flood
        // logcat. Print every 30 frames to keep enough samples for trend.
        if ((s_winSpikeCount % 30u) == 1u) {
            printf("[Vdp1Compute] SPIKE frame=%u reason=%s cmds=%u "
                   "N=%u Sc=%u D=%u Pg=%u Pl=%u L=%u clipCmd=%u "
                   "overflow=%u key=0x%03x UCLIP=%u GOUR=%u MESH=%u "
                   "tiles=%dx%d\n",
                s_frame, spikeReason, cmdCount,
                cNormal, cScaled, cDistorted, cPolygon, cPolyline, cLine,
                cClipCmd, tileOverflow, shadeKey,
                (shadeKey & 0x40u)  ? 1u : 0u,
                (shadeKey & 0x80u)  ? 1u : 0u,
                (shadeKey & 0x100u) ? 1u : 0u,
                numTilesX, numTilesY);
        }
    }
    if ((s_frame % 60u) == 0u) {
        // Avg per frame over the 60-frame window.
        const uint32_t avgN = s_winSumNormal    / 60u;
        const uint32_t avgS = s_winSumScaled    / 60u;
        const uint32_t avgD = s_winSumDistorted / 60u;
        const uint32_t avgPg = s_winSumPolygon  / 60u;
        const uint32_t avgPl = s_winSumPolyline / 60u;
        const uint32_t avgL  = s_winSumLine     / 60u;
        const uint32_t avgC  = s_winSumClipCmd  / 60u;
        printf("[Vdp1Compute] window60 frame=%u maxCmds=%u maxOverflow=%u "
               "meshGouraudFrames=%u userClipFrames=%u spikes=%u "
               "avgN=%u Sc=%u D=%u Pg=%u Pl=%u L=%u clipCmd=%u\n",
            s_frame, s_winMaxCmds, s_winMaxOverflow,
            s_winMeshGouraudFrames, s_winUserClipFrames, s_winSpikeCount,
            avgN, avgS, avgD, avgPg, avgPl, avgL, avgC);
        s_winMaxCmds           = 0;
        s_winMaxOverflow       = 0;
        s_winMeshGouraudFrames = 0;
        s_winUserClipFrames    = 0;
        s_winSpikeCount        = 0;
        s_winSumNormal         = 0;
        s_winSumScaled         = 0;
        s_winSumDistorted      = 0;
        s_winSumPolygon        = 0;
        s_winSumPolyline       = 0;
        s_winSumLine           = 0;
        s_winSumClipCmd        = 0;
    }
}

void Vdp1ComputeRasterizer::resetTileCount(VkCommandBuffer cb) {
    // Zero the TileCount SSBO so each frame starts with empty bins. Use
    // vkCmdFillBuffer rather than a uniform init from host so we do not
    // need a HOST_VISIBLE mapping on what is otherwise a DEVICE_LOCAL
    // resource. The resulting transfer must be fenced before Pass 1 reads.
    if (tileCountSSBO == VK_NULL_HANDLE || tileCountSSBOSize == 0) return;
    vkCmdFillBuffer(cb, tileCountSSBO, 0, tileCountSSBOSize, 0u);
}

void Vdp1ComputeRasterizer::uploadCommands() {
    if (cpuCmds.empty()) return;
    memcpy(cmdSSBOMapped, cpuCmds.data(), sizeof(Vdp1Cmd) * cpuCmds.size());
    // HOST_COHERENT - no explicit flush required.
}

void Vdp1ComputeRasterizer::uploadVramCram(const void* vram, const void* cram) {
    // Snapshot the VRAM/CRAM bytes BEFORE the GPU upload memcpy. This must
    // happen even if vramSSBOMapped is null (e.g. compute disabled mid-frame),
    // so that the debug UI sees the same VRAM state that produced cpuCmds in
    // the just-completed render pass -- not a later state mutated by the game
    // thread before the next beginFrame hook fires.
    if (vram) {
        const auto* p = static_cast<const uint8_t*>(vram);
        cachedVram.assign(p, p + VDP1_VRAM_SIZE);
    }
    if (cram) {
        const auto* p = static_cast<const uint8_t*>(cram);
        cachedCram.assign(p, p + VDP2_CRAM_SIZE);
    }
    if (vramSSBOMapped && vram) {
        memcpy(vramSSBOMapped, vram, VDP1_VRAM_SIZE);
    }
    if (cramSSBOMapped && cram) {
        memcpy(cramSSBOMapped, cram, VDP2_CRAM_SIZE);
    }
}

VkShaderModule Vdp1ComputeRasterizer::compileComputeShader(
    const std::string& source, const std::string& name) {
    using namespace shaderc;
    Compiler compiler;
    CompileOptions options;
    options.SetOptimizationLevel(shaderc_optimization_level_performance);

    SpvCompilationResult result = compiler.CompileGlslToSpv(
        source, shaderc_compute_shader, name.c_str(), options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        std::string msg = "[Vdp1ComputeRasterizer] shader compile failed (" +
                          name + "): " + result.GetErrorMessage();
        std::cerr << msg << std::endl;
        throw std::runtime_error(msg);
    }
    std::vector<uint32_t> spirv(result.cbegin(), result.cend());

    VkShaderModuleCreateInfo mi{};
    mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = spirv.size() * sizeof(uint32_t);
    mi.pCode    = spirv.data();
    VkShaderModule mod;
    if (vkCreateShaderModule(vulkan->getDevice(), &mi, nullptr, &mod) != VK_SUCCESS) {
        throw std::runtime_error("Vdp1ComputeRasterizer: vkCreateShaderModule failed");
    }
    return mod;
}

void Vdp1ComputeRasterizer::createDescriptorLayouts() {
    VkDevice device = vulkan->getDevice();

    // Phase B1 - Bin pipeline: 0=Cmd SSBO, 1=TileCount SSBO, 2=TileCmdList SSBO,
    // 3=Overflow SSBO (F17 -- single uint atomic counter for dropped cmds).
    {
        VkDescriptorSetLayoutBinding b[4] = {};
        for (int i = 0; i < 4; i++) {
            b[i].binding = i;
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = 4; ci.pBindings = b;
        if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &binDescLayout) != VK_SUCCESS) {
            throw std::runtime_error("createDescriptorSetLayout(bin) failed");
        }
    }

    // Phase B1 -- Shade pipeline: 0=fb image, 1=Cmd, 2=TileCount, 3=TileCmdList,
    // 4=VRAM, 5=CRAM.
    {
        VkDescriptorSetLayoutBinding b[6] = {};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        for (int i = 1; i < 6; i++) {
            b[i].binding = i;
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        for (int i = 0; i < 6; i++) {
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = 6; ci.pBindings = b;
        if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &shadeDescLayout) != VK_SUCCESS) {
            throw std::runtime_error("createDescriptorSetLayout(shade) failed");
        }
    }

    // Phase 1A -- Forward mapping pipeline.
    //   0 = fb image
    //   1 = Cmd SSBO
    //   2 = VRAM SSBO
    //   3 = TileCount  (consumed only by tile-binning forward shader)
    //   4 = TileCmdList (consumed only by tile-binning forward shader)
    // Per-cmd shader (TILE_MODE undefined) ignores bindings 3/4 -- the layout
    // is shared so we only manage one descriptor set / pipeline layout.
    {
        VkDescriptorSetLayoutBinding b[5] = {};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[1].binding = 1; b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[2].binding = 2; b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[3].binding = 3; b[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[4].binding = 4; b[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        for (int i = 0; i < 5; i++) {
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ci.bindingCount = 5; ci.pBindings = b;
        if (vkCreateDescriptorSetLayout(device, &ci, nullptr, &forwardDescLayout) != VK_SUCCESS) {
            throw std::runtime_error("createDescriptorSetLayout(forward) failed");
        }
    }
}

VkPipeline Vdp1ComputeRasterizer::getOrBuildShadePipeline(uint32_t key) {
    // Pipeline cache key layout (see Vdp1ComputeRasterizer.h):
    //   bits 0..5 = SPCTL (constant_id 0)
    //   bit  6    = USE_USERCLIP (constant_id 1)
    //   bit  7    = USE_GOURAUD  (constant_id 2)
    //   bit  8    = USE_MESH     (constant_id 3)
    //   bit  9    = USE_MSB_SHADOW (constant_id 4)
    auto it = shadePipelineCache.find(key);
    if (it != shadePipelineCache.end()) return it->second;

    VkDevice device = vulkan->getDevice();
    struct SpecData {
        uint32_t spriteType;
        uint32_t useUserClip;
        uint32_t useGouraud;
        uint32_t useMesh;
        uint32_t useMsbShadow;
    } sd;
    sd.spriteType   = key & 0x3Fu;
    sd.useUserClip  = (key & 0x40u)  ? 1u : 0u;
    sd.useGouraud   = (key & 0x80u)  ? 1u : 0u;
    sd.useMesh      = (key & 0x100u) ? 1u : 0u;
    sd.useMsbShadow = (key & 0x200u) ? 1u : 0u;
    VkSpecializationMapEntry mapEntries[5] = {};
    mapEntries[0].constantID = 0;
    mapEntries[0].offset     = offsetof(SpecData, spriteType);
    mapEntries[0].size       = sizeof(uint32_t);
    mapEntries[1].constantID = 1;
    mapEntries[1].offset     = offsetof(SpecData, useUserClip);
    mapEntries[1].size       = sizeof(uint32_t);
    mapEntries[2].constantID = 2;
    mapEntries[2].offset     = offsetof(SpecData, useGouraud);
    mapEntries[2].size       = sizeof(uint32_t);
    mapEntries[3].constantID = 3;
    mapEntries[3].offset     = offsetof(SpecData, useMesh);
    mapEntries[3].size       = sizeof(uint32_t);
    mapEntries[4].constantID = 4;
    mapEntries[4].offset     = offsetof(SpecData, useMsbShadow);
    mapEntries[4].size       = sizeof(uint32_t);
    VkSpecializationInfo specInfo{};
    specInfo.mapEntryCount = 5;
    specInfo.pMapEntries   = mapEntries;
    specInfo.dataSize      = sizeof(sd);
    specInfo.pData         = &sd;

    VkPipelineShaderStageCreateInfo ss{};
    ss.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ss.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    ss.module = shadeModule;
    ss.pName  = "main";
    ss.pSpecializationInfo = &specInfo;
    VkComputePipelineCreateInfo scpi{};
    scpi.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    scpi.stage  = ss;
    scpi.layout = shadePipeLayout;

    auto t0 = std::chrono::steady_clock::now();
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(device, VdpPipeline::threadPipelineCache, 1, &scpi, nullptr, &pipe) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    auto t1 = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    // Log every NEW variant built so we can collect the observed-set per game
    // and seed a hardcoded prebuild list. Grep: "ShadeVariant".
    printf("[Vdp1Compute] ShadeVariant built key=0x%03x SPCTL=0x%02x UCLIP=%u GOUR=%u MESH=%u MSB=%u (%lld us)\n",
         key, key & 0x3Fu,
         (key & 0x40u)  ? 1u : 0u,
         (key & 0x80u)  ? 1u : 0u,
         (key & 0x100u) ? 1u : 0u,
         (key & 0x200u) ? 1u : 0u,
         (long long)us);
    shadePipelineCache.emplace(key, pipe);
    return pipe;
}

void Vdp1ComputeRasterizer::prebuildAllShadeVariants() {
    // Observed shade variant set collected from representative games on
    // SD865 / Adreno 650 (2026-05-17). SPCTL values: 0x00, 0x0c, 0x20, 0x22,
    // 0x23, 0x24. Flag combinations: USERCLIP / GOURAUD / MESH (no MSB
    // shadow yet). Building these up front eliminates 100ms+ stutter that
    // happens when a new variant first appears in-game.
    //
    // Extension: ShadeVariant builds still log via printf; new key seen at
    // runtime should be appended here, OR add the SPCTL+flag axis tag to
    // observation list.
    static constexpr uint32_t kPrebuildKeys[] = {
        0x000, 0x00c, 0x020, 0x022, 0x023, 0x024,
        0x063, 0x064, 0x0a0, 0x0a2, 0x0a3, 0x0e0,
        0x0e2, 0x0e3, 0x0e4, 0x10c, 0x1a0, 0x1a3,
        0x1e0, 0x1e3,
    };
    auto t0 = std::chrono::steady_clock::now();
    uint32_t built = 0;
    uint32_t failed = 0;
    for (uint32_t key : kPrebuildKeys) {
        if (getOrBuildShadePipeline(key) != VK_NULL_HANDLE) ++built;
        else ++failed;
    }
    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    const size_t TOTAL = sizeof(kPrebuildKeys) / sizeof(kPrebuildKeys[0]);
    printf("[Vdp1Compute] Prebuilt %u/%zu observed shade variants (failed=%u) in %lld ms\n",
         built, TOTAL, failed, (long long)ms);
}

void Vdp1ComputeRasterizer::createPipelines() {
    VkDevice device = vulkan->getDevice();

    printf("[Vdp1Compute] createPipelines start\n");

    // Phase B1 - Bin pipeline (Pass 1). Single pipeline, no specialization.
    {
        VkPushConstantRange binPc{};
        binPc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        binPc.offset = 0;
        binPc.size   = sizeof(BinPushConstants);
        VkPipelineLayoutCreateInfo bli{};
        bli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        bli.setLayoutCount = 1; bli.pSetLayouts = &binDescLayout;
        bli.pushConstantRangeCount = 1; bli.pPushConstantRanges = &binPc;
        if (vkCreatePipelineLayout(device, &bli, nullptr, &binPipeLayout) != VK_SUCCESS)
            throw std::runtime_error("createPipelineLayout(bin) failed");

        std::string binSrc(kBinShaderSrc);
        VkShaderModule binMod = compileComputeShader(binSrc, "vdp1_compute_binning");
        VkPipelineShaderStageCreateInfo bs{};
        bs.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        bs.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        bs.module = binMod; bs.pName = "main";
        VkComputePipelineCreateInfo bcpi{};
        bcpi.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        bcpi.stage  = bs; bcpi.layout = binPipeLayout;
        if (vkCreateComputePipelines(device, VdpPipeline::threadPipelineCache, 1, &bcpi, nullptr, &binPipeline) != VK_SUCCESS) {
            vkDestroyShaderModule(device, binMod, nullptr);
            throw std::runtime_error("createComputePipelines(bin) failed");
        }
        vkDestroyShaderModule(device, binMod, nullptr);
    }

    // Phase B1 -- Shade pipeline (Pass 2).
    {
        VkPushConstantRange shadePc{};
        shadePc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        shadePc.offset = 0;
        shadePc.size   = sizeof(ShadePushConstants);
        VkPipelineLayoutCreateInfo sli{};
        sli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        sli.setLayoutCount = 1; sli.pSetLayouts = &shadeDescLayout;
        sli.pushConstantRangeCount = 1; sli.pPushConstantRanges = &shadePc;
        if (vkCreatePipelineLayout(device, &sli, nullptr, &shadePipeLayout) != VK_SUCCESS)
            throw std::runtime_error("createPipelineLayout(shade) failed");

        // Compile once, reuse module for every SPCTL-specialized pipeline.
        std::string shadeSrc(kShadeShaderSrc);
        shadeModule = compileComputeShader(shadeSrc, "vdp1_compute_tile_shade");
        // Pre-build the SPCTL=0 default so cold-frame dispatch never blocks
        // on pipeline compilation; getOrBuildShadePipeline() reuses this.
        shadePipeline = getOrBuildShadePipeline(0u);
        if (shadePipeline == VK_NULL_HANDLE) {
            throw std::runtime_error("createComputePipelines(shade) failed");
        }
    }

    // Phase 1A + tile-binning forward (perf opt 2026-05-08). Two pipelines
    // share one pipeline layout / descriptor layout. The shader source is
    // compiled twice -- once with TILE_MODE undefined for per-cmd dispatch
    // and once with `#define TILE_MODE 1` injected immediately after the
    // #version directive for the tile path. Per-cmd shader does not
    // reference the TileCount / TileCmdList SSBOs (bindings 3/4) so the
    // shared layout is harmless in that mode.
    {
        VkPushConstantRange forwardPc{};
        forwardPc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        forwardPc.offset = 0;
        forwardPc.size   = sizeof(ForwardPushConstants);
        VkPipelineLayoutCreateInfo fli{};
        fli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        fli.setLayoutCount = 1; fli.pSetLayouts = &forwardDescLayout;
        fli.pushConstantRangeCount = 1; fli.pPushConstantRanges = &forwardPc;
        if (vkCreatePipelineLayout(device, &fli, nullptr, &forwardPipeLayout) != VK_SUCCESS)
            throw std::runtime_error("createPipelineLayout(forward) failed");

        // Build per-cmd source as-is, then prepare a tile-mode source by
        // inserting "#define TILE_MODE 1\n" right after "#version 450\n".
        std::string forwardSrc(kForwardShaderSrc);
        std::string tileForwardSrc(kForwardShaderSrc);
        {
            const std::string marker = "#version 450\n";
            size_t pos = tileForwardSrc.find(marker);
            if (pos == std::string::npos) {
                throw std::runtime_error("vdp1_compute_forward: #version directive not found for TILE_MODE injection");
            }
            tileForwardSrc.insert(pos + marker.size(), "#define TILE_MODE 1\n");
        }

        // Per-cmd pipeline.
        {
            VkShaderModule forwardMod = compileComputeShader(forwardSrc, "vdp1_compute_forward");
            VkPipelineShaderStageCreateInfo fs{};
            fs.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            fs.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
            fs.module = forwardMod; fs.pName = "main";
            VkComputePipelineCreateInfo fcpi{};
            fcpi.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            fcpi.stage  = fs; fcpi.layout = forwardPipeLayout;
            if (vkCreateComputePipelines(device, VdpPipeline::threadPipelineCache, 1, &fcpi, nullptr, &forwardPipeline) != VK_SUCCESS) {
                vkDestroyShaderModule(device, forwardMod, nullptr);
                throw std::runtime_error("createComputePipelines(forward) failed");
            }
            vkDestroyShaderModule(device, forwardMod, nullptr);
        }
        // Tile-binning forward pipeline.
        {
            VkShaderModule mod = compileComputeShader(tileForwardSrc, "vdp1_compute_tile_forward");
            VkPipelineShaderStageCreateInfo fs{};
            fs.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            fs.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
            fs.module = mod; fs.pName = "main";
            VkComputePipelineCreateInfo fcpi{};
            fcpi.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            fcpi.stage  = fs; fcpi.layout = forwardPipeLayout;
            if (vkCreateComputePipelines(device, VdpPipeline::threadPipelineCache, 1, &fcpi, nullptr, &tileForwardPipeline) != VK_SUCCESS) {
                vkDestroyShaderModule(device, mod, nullptr);
                throw std::runtime_error("createComputePipelines(tile_forward) failed");
            }
            vkDestroyShaderModule(device, mod, nullptr);
        }
    }

    // Prebuild observed shade variants up front. List collected from
    // representative games on SD865 (see prebuildAllShadeVariants). Eliminates
    // the 100ms+ in-game stutter when a new variant first appears. New
    // variants still log via ShadeVariant printf -> append to the list.
    prebuildAllShadeVariants();

    // Descriptor pool + sets. Two sets per layout (one per offscreen frame
    // index, drawframe = 0/1) -- Adreno validation rejects vkUpdateDescriptorSets
    // on a set still bound by an in-flight cb, so cycling sets per frame avoids
    // the violation when the offscreen image view alternates each frame.
    VkDescriptorPoolSize ps[2] = {};
    ps[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; ps[0].descriptorCount = (4 + 5 + 4) * 2; // (bin 4 + shade 5 + forward 4) x 2
    ps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;  ps[1].descriptorCount = (1 + 1) * 2;     // (shade 1 + forward 1) x 2
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 3 * 2; pi.poolSizeCount = 2; pi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(device, &pi, nullptr, &descPool) != VK_SUCCESS)
        throw std::runtime_error("createDescriptorPool failed");

    // Allocate bin descriptor set x2 + write all 4 buffer bindings (F17 added
    // overflow SSBO at binding 3). Bin set has no image binding so per-frame
    // cycling here is purely for in-flight safety; SSBO bindings are static.
    for (int f = 0; f < 2; ++f) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = descPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &binDescLayout;
        if (vkAllocateDescriptorSets(device, &ai, &binDescSet[f]) != VK_SUCCESS)
            throw std::runtime_error("allocateDescriptorSets(bin) failed");

        VkDescriptorBufferInfo bi[4];
        bi[0] = {cmdSSBO,         0, VK_WHOLE_SIZE};
        bi[1] = {tileCountSSBO,   0, VK_WHOLE_SIZE};
        bi[2] = {tileCmdListSSBO, 0, VK_WHOLE_SIZE};
        bi[3] = {overflowSSBO,    0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[4] = {};
        for (int i = 0; i < 4; i++) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = binDescSet[f];
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[i].pBufferInfo = &bi[i];
        }
        vkUpdateDescriptorSets(device, 4, w, 0, nullptr);
    }

    // Allocate shade descriptor set x2 + write 5 buffer bindings (image
    // binding is updated lazily in updateTileDescriptorSets per frame).
    for (int f = 0; f < 2; ++f) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = descPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &shadeDescLayout;
        if (vkAllocateDescriptorSets(device, &ai, &shadeDescSet[f]) != VK_SUCCESS)
            throw std::runtime_error("allocateDescriptorSets(shade) failed");

        VkDescriptorBufferInfo bi[5];
        bi[0] = {cmdSSBO,         0, VK_WHOLE_SIZE};
        bi[1] = {tileCountSSBO,   0, VK_WHOLE_SIZE};
        bi[2] = {tileCmdListSSBO, 0, VK_WHOLE_SIZE};
        bi[3] = {vramSSBO,        0, VK_WHOLE_SIZE};
        bi[4] = {cramSSBO,        0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[5] = {};
        for (int i = 0; i < 5; i++) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = shadeDescSet[f];
            w[i].dstBinding = i + 1;  // 1..5; binding 0 is the fb image
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[i].pBufferInfo = &bi[i];
        }
        vkUpdateDescriptorSets(device, 5, w, 0, nullptr);
    }

    // Phase 1A + tile-binning -- Allocate forward descriptor set x2 + write
    // Cmd / VRAM / TileCount / TileCmdList bindings. Image binding (slot 0)
    // is updated lazily in updateForwardDescriptorSets() when the offscreen
    // image view changes for that frame slot.
    for (int f = 0; f < 2; ++f) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = descPool; ai.descriptorSetCount = 1; ai.pSetLayouts = &forwardDescLayout;
        if (vkAllocateDescriptorSets(device, &ai, &forwardDescSet[f]) != VK_SUCCESS)
            throw std::runtime_error("allocateDescriptorSets(forward) failed");

        VkDescriptorBufferInfo bi[4];
        bi[0] = {cmdSSBO,         0, VK_WHOLE_SIZE};
        bi[1] = {vramSSBO,        0, VK_WHOLE_SIZE};
        bi[2] = {tileCountSSBO,   0, VK_WHOLE_SIZE};
        bi[3] = {tileCmdListSSBO, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[4] = {};
        for (int i = 0; i < 4; i++) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = forwardDescSet[f];
            w[i].dstBinding = i + 1;  // 1=Cmd, 2=VRAM, 3=TileCount, 4=TileCmdList
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[i].pBufferInfo = &bi[i];
        }
        vkUpdateDescriptorSets(device, 4, w, 0, nullptr);
    }
}

void Vdp1ComputeRasterizer::destroyPipelines() {
    VkDevice device = vulkan->getDevice();
    if (descPool)           { vkDestroyDescriptorPool(device, descPool, nullptr);                 descPool           = VK_NULL_HANDLE; }
    if (binPipeline)        { vkDestroyPipeline(device, binPipeline, nullptr);                    binPipeline        = VK_NULL_HANDLE; }
    if (binPipeLayout)      { vkDestroyPipelineLayout(device, binPipeLayout, nullptr);            binPipeLayout      = VK_NULL_HANDLE; }
    if (binDescLayout)      { vkDestroyDescriptorSetLayout(device, binDescLayout, nullptr);       binDescLayout      = VK_NULL_HANDLE; }
    // Destroy every SPCTL-specialized shade pipeline. shadePipeline above
    // is just an alias to the SPCTL=0 entry in the cache (no double-free
    // because we VK_NULL_HANDLE the alias before the cache wipe).
    shadePipeline = VK_NULL_HANDLE;
    for (auto& kv : shadePipelineCache) {
        if (kv.second) vkDestroyPipeline(device, kv.second, nullptr);
    }
    shadePipelineCache.clear();
    if (shadeModule)        { vkDestroyShaderModule(device, shadeModule, nullptr);                shadeModule        = VK_NULL_HANDLE; }
    if (shadePipeLayout)    { vkDestroyPipelineLayout(device, shadePipeLayout, nullptr);          shadePipeLayout    = VK_NULL_HANDLE; }
    if (shadeDescLayout)    { vkDestroyDescriptorSetLayout(device, shadeDescLayout, nullptr);     shadeDescLayout    = VK_NULL_HANDLE; }
    if (forwardPipeline)     { vkDestroyPipeline(device, forwardPipeline, nullptr);                forwardPipeline     = VK_NULL_HANDLE; }
    if (tileForwardPipeline) { vkDestroyPipeline(device, tileForwardPipeline, nullptr);            tileForwardPipeline = VK_NULL_HANDLE; }
    if (forwardPipeLayout)   { vkDestroyPipelineLayout(device, forwardPipeLayout, nullptr);        forwardPipeLayout   = VK_NULL_HANDLE; }
    if (forwardDescLayout)   { vkDestroyDescriptorSetLayout(device, forwardDescLayout, nullptr);   forwardDescLayout   = VK_NULL_HANDLE; }
}

void Vdp1ComputeRasterizer::updateTileDescriptorSets(VkImageView targetView, int frameIndex) {
    // Per-frame slot. Re-bind shade descriptor set when fb image view changes
    // (each frame swap on the offscreen image, or after resize). Also re-binds
    // the tile SSBOs since recreateTileBuffers() may have replaced their
    // handles. The previous frame's set is guaranteed not to be in flight by
    // drainComputeInFlight() in Vdp1Renderer.
    if (targetView == lastShadeTargetView[frameIndex]) return;
    lastShadeTargetView[frameIndex] = targetView;

    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageView   = targetView;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorBufferInfo bi[5];
    bi[0] = {cmdSSBO,         0, VK_WHOLE_SIZE};
    bi[1] = {tileCountSSBO,   0, VK_WHOLE_SIZE};
    bi[2] = {tileCmdListSSBO, 0, VK_WHOLE_SIZE};
    bi[3] = {vramSSBO,        0, VK_WHOLE_SIZE};
    bi[4] = {cramSSBO,        0, VK_WHOLE_SIZE};

    VkWriteDescriptorSet w[6] = {};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = shadeDescSet[frameIndex];
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[0].pImageInfo = &imgInfo;
    for (int i = 0; i < 5; ++i) {
        w[i + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i + 1].dstSet = shadeDescSet[frameIndex];
        w[i + 1].dstBinding = static_cast<uint32_t>(i + 1);
        w[i + 1].descriptorCount = 1;
        w[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i + 1].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(vulkan->getDevice(), 6, w, 0, nullptr);

    // Bin descriptor set: re-bind cmd / tile / overflow buffers in case any
    // got replaced (e.g. tile buffers after resize).
    VkDescriptorBufferInfo binBi[4];
    binBi[0] = {cmdSSBO,         0, VK_WHOLE_SIZE};
    binBi[1] = {tileCountSSBO,   0, VK_WHOLE_SIZE};
    binBi[2] = {tileCmdListSSBO, 0, VK_WHOLE_SIZE};
    binBi[3] = {overflowSSBO,    0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet binW[4] = {};
    for (int i = 0; i < 4; ++i) {
        binW[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        binW[i].dstSet = binDescSet[frameIndex];
        binW[i].dstBinding = static_cast<uint32_t>(i);
        binW[i].descriptorCount = 1;
        binW[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binW[i].pBufferInfo = &binBi[i];
    }
    vkUpdateDescriptorSets(vulkan->getDevice(), 4, binW, 0, nullptr);
}

void Vdp1ComputeRasterizer::updateForwardDescriptorSets(VkImageView targetView, int frameIndex) {
    // Per-frame slot. Re-bind forward descriptor set when fb image view
    // changes. Slot 0 = fb image, 1 = Cmd, 2 = VRAM, 3 = TileCount,
    // 4 = TileCmdList. Tile* SSBOs may be replaced by recreateTileBuffers()
    // on resolution change so we re-bind unconditionally each time the
    // image view differs (covers both cases since they share the trigger).
    if (targetView == lastForwardTargetView[frameIndex]) return;
    lastForwardTargetView[frameIndex] = targetView;

    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageView   = targetView;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorBufferInfo bi[4];
    bi[0] = {cmdSSBO,         0, VK_WHOLE_SIZE};
    bi[1] = {vramSSBO,        0, VK_WHOLE_SIZE};
    bi[2] = {tileCountSSBO,   0, VK_WHOLE_SIZE};
    bi[3] = {tileCmdListSSBO, 0, VK_WHOLE_SIZE};

    VkWriteDescriptorSet w[5] = {};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = forwardDescSet[frameIndex];
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[0].pImageInfo = &imgInfo;
    for (int i = 0; i < 4; ++i) {
        w[i + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i + 1].dstSet = forwardDescSet[frameIndex];
        w[i + 1].dstBinding = static_cast<uint32_t>(i + 1);
        w[i + 1].descriptorCount = 1;
        w[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i + 1].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(vulkan->getDevice(), 5, w, 0, nullptr);
}
