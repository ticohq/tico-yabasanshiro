#pragma once
#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <vector>
#include "Vdp1ComputeCommands.h"

namespace vdp1c {

// Axis-aligned bounding box from 4 polygon vertices.
glm::ivec4 computeBBox(glm::ivec2 v0, glm::ivec2 v1, glm::ivec2 v2, glm::ivec2 v3);

// Edge function (twice the signed triangle area).
// Positive when p is on the left side of v0->v1 (CCW convention), negative on
// the right.
float edgeFunction(glm::vec2 p, glm::vec2 v0, glm::vec2 v1);

// Inside test for a convex 4-vertex quad (CCW input expected).
bool insideQuad(glm::vec2 p, glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d);

// Normalize the vertex order to CCW (counter-clockwise, i.e. left-rotating in
// screen coords). When the input is CW, swap B and D.
// Returns true if the input was already CCW (false means it was flipped).
//
// Precondition: the quad must be convex. Concave / self-intersecting quads
// produce undefined results (the Saturn VDP1 spec itself leaves concave quads
// undefined).
bool normalizeWinding(glm::vec2& a, glm::vec2& b, glm::vec2& c, glm::vec2& d);

// Concave-quad detection (F13). Returns true when ABCD does not form a convex
// quad -- i.e. the 4 cross products of consecutive edges have mixed sign. A
// strictly-zero cross (collinear edge) is treated as "consistent" so a
// degenerate parallelogram is not flagged as concave.
//
// Used by encode functions to set Vdp1Cmd::flags |= FLAG_TRI_SPLIT, which
// switches the shader from quad inverse-bilinear coverage to a 2-triangle
// barycentric path that is well-defined on concave quads.
bool isConcave(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d);

// Texture row read by line k of a textured VDP1 command drawn as n lines,
// with the texture rows running from t0 to t1 (t1 < t0 when vertically
// flipped). VDP1 steps the row with an error term set up for n lines over
// |t1 - t0| + 1 rows (the reference software renderer, ss/vdp1_common.h
// VileTex::Setup and the per-line PreStep in ss/vdp1_sprite.cpp); this is
// the closed form of that stepping. Rows are left out evenly when there are
// fewer lines than rows, repeated when there are more, and a single line
// (n == 1) reads the middle row. Same name and body in the compute shaders.
int vdp1TexRow(int k, int n, int t0, int t1);

// Number of lines minus one that VDP1 draws for a quad A B C D (raw Saturn
// coordinates): the longest Chebyshev length of the side edges A->D and
// B->C. Each edge difference is sign-extended from 13 bits and the result
// is masked to 12 bits, as the hardware does (the reference software
// renderer, ss/vdp1_sprite.cpp dmax). A textured command is drawn as
// vdp1CmdDmax(...) + 1 lines.
int vdp1CmdDmax(glm::ivec2 a, glm::ivec2 b, glm::ivec2 c, glm::ivec2 d);

// Chebyshev length of the segment a->b with 13-bit sign-extended
// differences: the number of dots minus one VDP1 plots along a line.
int vdp1LineLen(glm::ivec2 a, glm::ivec2 b);

// Saturn vertices of a Normal Sprite at (x, y) with a w x h character, as the
// hardware builds them: B = A + (max(w,1) - 1, 0), C/D = + (max(h,1) - 1).
std::array<glm::ivec2, 4> normalSpriteQuad(int x, int y, int w, int h);

// Value for Vdp1Cmd::lineSteps from the raw Saturn vertices A B C D of a
// sprite: bits 0..15 = vdp1CmdDmax, bits 16..31 = vdp1LineLen(A, B). The
// host hooks assign it after encoding, because the encoders only see the
// scaled and truncated fb vertices.
uint32_t packSpriteSteps(const std::array<glm::ivec2, 4>& saturnVerts);

// Texture column (flip applied) of a one-line sprite (A == D, B == C) at fb
// point p: the dot of A->B nearest to p (measured at the Saturn cell centre,
// p - scale / 2), stepped with vdp1TexRow over lenAB + 1 dots. a/b are the
// fb vertices. Host mirror of the shaders' oneLineTexU.
int vdp1OneLineTexU(glm::vec2 p, glm::ivec2 a, glm::ivec2 b, glm::vec2 scale,
                    int lenAB, int w, bool flipH);

// Pack a VDP1 Polygon command (0100) into a Vdp1Cmd.
// vertices: A,B,C,D = TL,TR,BR,BL (Saturn standard order). No CCW
//   normalization is applied: the scanline rasterizer walks v0->v3 and v1->v2
//   in vidsoft-faithful style.
// localCoord: appendCommand applies it via the state machine, so callers
//   should always pass (0, 0).
// color: CMDCOLR (15-bit RGB or palette index).
// pmod:  CMDPMOD.
// gouraudAddr: VRAM byte address of the 4 RGB555 Gouraud table (= CMDGRDA<<3).
//   Pass 0 when Gouraud is not used; the encode function automatically gates
//   on (pmod & 4) so callers can always pass the table address.
Vdp1Cmd encodePolygon(
    std::array<glm::ivec2, 4> vertices,
    glm::ivec2 localCoord,
    uint32_t color,
    uint32_t pmod,
    uint32_t gouraudAddr = 0
);

struct Vdp1State {
    glm::ivec2 systemClip{2047, 2047};
    glm::ivec4 userClip{0, 0, 2047, 2047};
    glm::ivec2 localCoord{0, 0};
    // Per-axis HD upscale factor (vdp1wratio * computeFbScaleX,
    // vdp1hratio * computeFbScaleY). scaleX/scaleY are needed for
    // strictly correct "1 Saturn screen cell" extension on axis-aligned
    // distorted sprites at non-uniform aspect ratios; scaleMax = max of
    // the two preserves existing band/threshold scaling that doesn't
    // care about per-axis distinction.
    float scaleX{1.0f};
    float scaleY{1.0f};
    float scaleMax{1.0f};
    // VDP2 SPCTL & 0x3F -- packs both sprite type (bits 0..3, 0..15) and
    // SPCTL bit 5 (sprite-RGB-enable). Forwarded to the shade shader via
    // push constant. Consumers in the shader:
    //   - 4bpp LUT palette path: getLutSpriteInfo(pc.spriteType & 0xFu)
    //     extracts priority/colorcl/normalshadow from the LUT word per
    //     sprite-type bit layout (Vdp1GetSpritePixelInfo equivalent).
    //   - 16bpp RGB path: (pc.spriteType & 0x20u) gates direct vs palette
    //     encoding -- graphics path Vdp1Renderer.cpp:4140 emits C=0
    //     direct color only when (dot & 0x8000) AND (SPCTL & 0x20),
    //     otherwise C=1 palette with the 16-bit dot as colorindex.
    // Initialised to 0 (sprite type 0, RGB-enable clear); host updates
    // before each frame from fixVdp2Regs->SPCTL.
    uint32_t spriteType{0};
};

void resetState(Vdp1State& s);

// Bake state into a Vdp1Cmd: add localCoord to v0..v3, update systemClip /
// userClip fields, recompute bbox, and set FLAG_TRI_SPLIT / FLAG_THIN bits
// in cmd.flags based on the polygon's geometry and the current scaleMax.
//
// inclusiveVerts: if true, mark the cmd as Saturn-cell-inclusive
// (VDP1C_FLAG_INCLUSIVE_VERTS) and extend bbox.z/.w by ceil(scaleMax)-1 so
// the shader's brEdgeDilation can extend coverage to the last fb pixel of
// the BR Saturn cell. Pass false for fence-post paths (NormalSprite /
// ScaledSprite host code already adds +1 to BR vertices).
void applyState(Vdp1Cmd& cmd, const Vdp1State& s, bool inclusiveVerts = false);

// Geometric thinness check used to gate FLAG_THIN. A polygon is "thin" iff
// it could produce HD-scaled rasterization gaps along its long axis. Returns
// true for:
//   (a) bbox short side <= widthThr AND long/short ratio >= 4 (axis-aligned sliver)
//   (b) bbox long side > widthThr*2 AND polygon bbox fill ratio < 10% (diagonal sliver)
// Zero-area degenerates (3+ coincident vertices) are treated as
// "not rasterized" and therefore NOT thin.
//   widthThr = ceil(scaleMax * 2) + 1
bool isThinPolygonGeometric(const Vdp1Cmd& cmd, float scaleMax);

// Compute interpolation coefficients (s, t) of point P inside a 4-vertex quad
// where A=(0,0), B=(1,0), C=(1,1), D=(0,1) is the canonical CCW mapping.
// Forward: P(s, t) = mix(mix(A, B, s), mix(D, C, s), t).
// The inverse solves a quadratic. Handles degenerate (zero-area) and
// parallelogram cases safely.
glm::vec2 inverseBilinear(glm::vec2 p, glm::vec2 A, glm::vec2 B, glm::vec2 C, glm::vec2 D);

// Pack a VDP1 Distorted Sprite (0010) command into a Vdp1Cmd.
// vertices: A,B,C,D = TL,TR,BR,BL (Saturn standard order, no CCW
//   normalization).
// charSize: texture size in pixels.
// flip:     CMDCTRL bits 4-5 (bit 0 = Hflip, bit 1 = Vflip).
// localCoord: applied via applyState (state machine), so callers normally
//   pass (0, 0) and let appendCommand bake in the current state.
// inclusiveVerts: when true, vertices follow Saturn-cell-inclusive
// convention (raw CMDXA..CMDXD; the BR vertex IS the last cell). Set the
// VDP1C_FLAG_INCLUSIVE_VERTS bit so the shader extends BR-facing edges by
// scaleMax fb px to cover the last cell. This flag is only consulted by
// the tile_shade fallback path; the forward-mapping shader (default in
// Phase 1C) handles cell-inclusive coverage natively via its lerp
// denominator and ignores inclusiveVerts.
Vdp1Cmd encodeDistortedSprite(
    std::array<glm::ivec2, 4> vertices,
    uint32_t srca,
    glm::uvec2 charSize,
    uint32_t color,
    uint32_t pmod,
    glm::ivec2 localCoord,
    uint32_t flip = 0,
    uint32_t gouraudAddr = 0,
    bool inclusiveVerts = false);

// Pack a VDP1 Normal Sprite (0000) command. Internally normalized to the
// DISTORTED_SPRITE shader path: the axis-aligned rect [x,y]-[x+w,y+h] is
// emitted as a 4-vertex quad in Saturn standard order so the existing
// inverseBilinear UV / coverage logic Just Works (a parallelogram with zero
// shear is the trivial case for inverse bilinear).
Vdp1Cmd encodeNormalSprite(
    int x, int y, int w, int h,
    uint32_t srca,
    glm::uvec2 charSize,
    uint32_t color,
    uint32_t pmod,
    glm::ivec2 localCoord,
    uint32_t flip = 0,
    uint32_t gouraudAddr = 0);

// Pack a VDP1 Scaled Sprite (0001) command. The renderer is expected to have
// resolved CMDCTRL zoom-point bits into 4 explicit screen-space vertices
// (Saturn standard order); we treat the result as DISTORTED_SPRITE for the
// shader path same as encodeNormalSprite.
Vdp1Cmd encodeScaledSprite(
    std::array<glm::ivec2, 4> vertices,
    uint32_t srca,
    glm::uvec2 charSize,
    uint32_t color,
    uint32_t pmod,
    glm::ivec2 localCoord,
    uint32_t flip = 0,
    uint32_t gouraudAddr = 0);

// Pack a VDP1 Polyline (0101) command. v0->v1->v2->v3->v0 forms a closed
// 4-segment polyline. Color is CMDCOLR (15-bit RGB or palette) and pmod is
// CMDPMOD raw.
Vdp1Cmd encodePolyline(
    std::array<glm::ivec2, 4> vertices,
    uint32_t color,
    uint32_t pmod,
    glm::ivec2 localCoord,
    uint32_t gouraudAddr = 0);

// Pack a VDP1 Line (0110) command. v0->v1 single segment. Other v* fields
// are filled with v0/v1 so computeBBox / pipelines that expect 4 vertices
// still produce a valid (degenerate) bbox without special-casing.
Vdp1Cmd encodeLine(
    glm::ivec2 v0,
    glm::ivec2 v1,
    uint32_t color,
    uint32_t pmod,
    glm::ivec2 localCoord,
    uint32_t gouraudAddr = 0);

// dispatchUpTo internal logic extracted as a pure function for host testing.
// Returns the indices of cmds that should be dispatched (POLYGON or
// DISTORTED_SPRITE only). Other command types are skipped.
// lastCmdIndex caps the iteration upper bound (inclusive); pass UINT32_MAX
// to dispatch all. Out-of-range lastCmdIndex is clamped to cmds.size().
std::vector<uint32_t> collectDispatchIndices(
    const std::vector<Vdp1Cmd>& cmds, uint32_t lastCmdIndex);

// Pipeline variant indices for shader specialization (Phase A3).
// One compute pipeline is compiled per variant via VkSpecializationConstant
// so the cmdType + cm branch cascade in emitPixel collapses to a single
// constant-folded path. Pipelines are indexed 0..6 inclusive.
constexpr uint32_t VDP1C_VARIANT_POLYGON    = 0u;
constexpr uint32_t VDP1C_VARIANT_SPRITE_CM0 = 1u;  // 4bpp color bank
constexpr uint32_t VDP1C_VARIANT_SPRITE_CM1 = 2u;  // 4bpp color LUT
constexpr uint32_t VDP1C_VARIANT_SPRITE_CM2 = 3u;  // 8bpp bank palMask=0x3F
constexpr uint32_t VDP1C_VARIANT_SPRITE_CM3 = 4u;  // 8bpp bank palMask=0x7F
constexpr uint32_t VDP1C_VARIANT_SPRITE_CM4 = 5u;  // 8bpp bank palMask=0xFF
constexpr uint32_t VDP1C_VARIANT_SPRITE_CM5 = 6u;  // 16bpp RGB direct
constexpr uint32_t VDP1C_VARIANT_COUNT      = 7u;
constexpr uint32_t VDP1C_VARIANT_INVALID    = UINT32_MAX;

// Returns the pipeline variant index 0..6 for a Vdp1Cmd, or
// VDP1C_VARIANT_INVALID when the cmd should be skipped (cm 6/7 reserved by
// Saturn spec, or non-rasterizable cmd type).
uint32_t pipelineVariantForCmd(const Vdp1Cmd& cmd);

// Compute the per-dispatch barrier mask for Replace-blend ordering.
//
// Returns a vector of size == indices.size(). mask[k] == 1 means a
// VkMemoryBarrier should be emitted *before* dispatching indices[k] so that
// it observes earlier (overlapping) draws. mask[k] == 0 means the dispatch
// has no in-flight overlap and can be issued in parallel with the current
// batch.
//
// Algorithm: keep an "in-flight" set of bboxes since the last barrier.
//   - if cmds[indices[k]].bbox overlaps ANY in-flight bbox -> mask[k] = 1,
//     emit barrier, reset in-flight set to {cmds[indices[k]].bbox}.
//   - else -> mask[k] = 0, append the bbox to the in-flight set.
//
// Replace-blend semantics are preserved: any pair of overlapping draws is
// separated by at least one intervening barrier. Non-overlapping draws are
// allowed to run in parallel, which is the actual GPU performance win for
// SEGA Rally / Burning Rangers style scenes where most polygons cover
// disjoint screen regions.
//
// Returned as std::vector<uint8_t> rather than vector<bool> for predictable
// memory layout in the hot dispatch loop and easier debugger inspection.
std::vector<uint8_t> computeBarrierMask(
    const std::vector<Vdp1Cmd>& cmds,
    const std::vector<uint32_t>& indices);

}  // namespace vdp1c
