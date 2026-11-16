#include "Vdp1ComputeMath.h"
#include "../vdp1texrow.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace vdp1c {

namespace {

// CMDPMOD bit 10 (0x0400) enables user clipping; bit 9 (0x0200) selects mode
// (0 = inside-only, 1 = outside-only). Translates to Vdp1Cmd::clipMode values
// matching the shader's check (Vdp1ComputeCommands.h:106): 0 disabled,
// 1 inside, 2 outside. Previously every encoder hard-coded clipMode = 0
// ("Plan 1: always 0") so any cmd with CMDPMOD bit 10 set ignored the user
// clip rectangle and rendered everywhere.
inline uint32_t deriveClipMode(uint32_t pmod) {
    if ((pmod & 0x0400u) == 0u) return 0u;
    return ((pmod & 0x0200u) != 0u) ? 2u : 1u;
}

}  // namespace

glm::ivec4 computeBBox(glm::ivec2 v0, glm::ivec2 v1, glm::ivec2 v2, glm::ivec2 v3) {
    int minX = std::min({v0.x, v1.x, v2.x, v3.x});
    int minY = std::min({v0.y, v1.y, v2.y, v3.y});
    int maxX = std::max({v0.x, v1.x, v2.x, v3.x});
    int maxY = std::max({v0.y, v1.y, v2.y, v3.y});
    return glm::ivec4(minX, minY, maxX, maxY);
}

float edgeFunction(glm::vec2 p, glm::vec2 v0, glm::vec2 v1) {
    return (v1.x - v0.x) * (p.y - v0.y) - (v1.y - v0.y) * (p.x - v0.x);
}

bool insideQuad(glm::vec2 p, glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d) {
    // Conservative rasterization: expand the quad outward by 0.5 pixels so that
    // adjacent quads with slight vertex rounding errors do not leave 1px gaps.
    // Signed perpendicular distance = edge / length, so the test
    // "edge >= -BIAS * length" determines whether a pixel is within the dilated
    // region. We square both sides to avoid sqrt: ef^2 <= BIAS^2 * lenSq when
    // ef < 0.
    auto inEdge = [](glm::vec2 p, glm::vec2 v0, glm::vec2 v1) {
        constexpr float BIAS_SQ = 0.25f;  // (0.5 px)^2
        float ef = edgeFunction(p, v0, v1);
        if (ef >= 0.0f) return true;
        glm::vec2 e = v1 - v0;
        float lenSq = e.x * e.x + e.y * e.y;
        return ef * ef <= BIAS_SQ * lenSq;
    };
    return inEdge(p, a, b) && inEdge(p, b, c) && inEdge(p, c, d) && inEdge(p, d, a);
}

bool isConcave(glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec2 d) {
    // Sign of (next_edge x prev_edge.next) at each vertex. For a convex quad
    // every consecutive edge pair turns the same way; mixed positive +
    // negative means at least one interior angle exceeds 180 deg (concave) or
    // the quad is self-intersecting / twisted. Strict zero (collinear edges)
    // is ignored -- a degenerate parallelogram is still treated as convex.
    auto cross = [](glm::vec2 u, glm::vec2 v) { return u.x * v.y - u.y * v.x; };
    const float c1 = cross(b - a, c - b);
    const float c2 = cross(c - b, d - c);
    const float c3 = cross(d - c, a - d);
    const float c4 = cross(a - d, b - a);
    constexpr float kEps = 1e-4f;
    const bool hasPos = (c1 > kEps) || (c2 > kEps) || (c3 > kEps) || (c4 > kEps);
    const bool hasNeg = (c1 < -kEps) || (c2 < -kEps) || (c3 < -kEps) || (c4 < -kEps);
    return hasPos && hasNeg;
}

bool normalizeWinding(glm::vec2& a, glm::vec2& b, glm::vec2& c, glm::vec2& d) {
    // CCW (counter-clockwise) iff edgeFunction(c, a, b) > 0.
    float area = edgeFunction(c, a, b);
    if (area < 0.0f) {
        // CW input: swap B and D to convert to CCW.
        std::swap(b, d);
        return false;
    }
    return true;
}

// The closed forms live in vdp1texrow.h so the C OpenGL core and the
// graphics tessellation path share them.
int vdp1TexRow(int k, int n, int t0, int t1) { return vdp1_tex_row(k, n, t0, t1); }

namespace {
inline int cheb13(glm::ivec2 a, glm::ivec2 b) { return vdp1_cheb13(a.x, a.y, b.x, b.y); }
}  // namespace

int vdp1CmdDmax(glm::ivec2 a, glm::ivec2 b, glm::ivec2 c, glm::ivec2 d) {
    const int v[8] = {a.x, a.y, b.x, b.y, c.x, c.y, d.x, d.y};
    return vdp1_cmd_dmax(v);
}

int vdp1LineLen(glm::ivec2 a, glm::ivec2 b) { return cheb13(a, b); }

std::array<glm::ivec2, 4> normalSpriteQuad(int x, int y, int w, int h) {
    const int x1 = x + std::max(w, 1) - 1;
    const int y1 = y + std::max(h, 1) - 1;
    return {{glm::ivec2(x, y), glm::ivec2(x1, y), glm::ivec2(x1, y1), glm::ivec2(x, y1)}};
}

uint32_t packSpriteSteps(const std::array<glm::ivec2, 4>& v) {
    const uint32_t dmax = static_cast<uint32_t>(vdp1CmdDmax(v[0], v[1], v[2], v[3]));
    const uint32_t len = static_cast<uint32_t>(std::min(vdp1LineLen(v[0], v[1]), 0xFFFF));
    return dmax | (len << 16);
}

int vdp1OneLineTexU(glm::vec2 p, glm::ivec2 a, glm::ivec2 b, glm::vec2 scale,
                    int lenAB, int w, bool flipH) {
    const glm::vec2 e = glm::vec2(b - a);
    const float ee = glm::dot(e, e);
    float t = 0.0f;
    if (ee > 0.0f) {
        const glm::vec2 q = p - 0.5f * scale - glm::vec2(a);
        t = std::min(1.0f, std::max(0.0f, glm::dot(q, e) / ee));
    }
    // roundEven, as in GLSL.
    const int j = static_cast<int>(std::nearbyint(t * static_cast<float>(lenAB)));
    return vdp1TexRow(j, lenAB + 1, flipH ? w - 1 : 0, flipH ? 0 : w - 1);
}

Vdp1Cmd encodePolygon(
    std::array<glm::ivec2, 4> vertices,
    glm::ivec2 localCoord,
    uint32_t color,
    uint32_t pmod,
    uint32_t gouraudAddr
) {
    Vdp1Cmd cmd{};
    cmd.cmdType = VDP1C_TYPE_POLYGON;
    cmd.pmod    = pmod;
    cmd.color   = color;
    cmd.srca    = 0;
    cmd.charSize = glm::uvec2(0);
    // Gouraud is enabled by CMDPMOD bit 2; the table address comes from
    // CMDGRDA<<3. Gate on both: callers may pass a stale address with the
    // bit cleared when prior cmds used Gouraud, and we must NOT enable.
    cmd.gouraudAddr = ((pmod & 0x4u) != 0u && gouraudAddr != 0u)
        ? glm::uvec2(gouraudAddr, 1u)
        : glm::uvec2(0u);
    // Preserve the Saturn standard vertex order (v0=A=TL, v1=B=TR, v2=C=BR,
    // v3=D=BL). The scanline rasterizer (vidsoft-faithful) walks TL->BL and
    // TR->BR edges and assumes this order, so we do NOT apply CCW
    // normalization (B<->D swap). Different vertex orderings render as texture
    // rotations, which matches Saturn behaviour.
    cmd.v0 = vertices[0];
    cmd.v1 = vertices[1];
    cmd.v2 = vertices[2];
    cmd.v3 = vertices[3];
    cmd.clipMode   = deriveClipMode(pmod);

    // Polygon: vertices are raw CMDXA..CMDXD (Saturn-cell-inclusive).
    Vdp1State s; s.localCoord = localCoord;
    applyState(cmd, s, /*inclusiveVerts=*/true);
    return cmd;
}

void resetState(Vdp1State& s) {
    // Preserve scaleMax, localCoord, systemClip, and userClip across frames.
    // - scaleMax is set per resolution change, not per frame.
    // - localCoord / systemClip / userClip are persistent VDP1 state: games
    //   typically issue these state-only cmds (CMDCTRL types 8/9/10) at the
    //   start of a frame's command list but rely on the values carrying over
    //   when omitted on a later frame (matches Saturn HW + graphics path,
    //   where Vdp1Regs->localX/localY are persistent registers that survive
    //   beginFrame). Clearing here used to make polygons whose CMDXA-D were
    //   centered around (0,0) (assuming localCoord = screen center from a
    //   prior frame) end up half off-screen on frames that omitted the
    //   LocalCoord cmd.
    const float      keepScale       = s.scaleMax;
    const glm::ivec2 keepLocalCoord  = s.localCoord;
    const glm::ivec2 keepSystemClip  = s.systemClip;
    const glm::ivec4 keepUserClip    = s.userClip;
    const uint32_t   keepSpriteType  = s.spriteType;
    s = Vdp1State{};
    s.scaleMax   = keepScale;
    s.localCoord = keepLocalCoord;
    s.systemClip = keepSystemClip;
    s.userClip   = keepUserClip;
    s.spriteType = keepSpriteType;
}

bool isThinPolygonGeometric(const Vdp1Cmd& cmd, float scaleMax) {
    // FLAG_THIN purpose: real Saturn HW connects 1-px thin slabs / lines
    // via its Bresenham edge walker. After HD upscale those degenerate
    // into trapezoids/triangles, and strict-inside coverage drops rows
    // or columns along the long axis, leaving visible gaps. FLAG_THIN
    // compensates for that case.
    //
    // Gaps form only when all three hold: (i) thin, (ii) long, and
    // (iii) belt-shaped (does not fill its bbox). Short small-chunk
    // polygons (bbox a few px on a side) and zero-area degenerates
    // (e.g. 3 coincident verts) lack the length to produce gaps and
    // are NOT thin.
    //
    // Axis-aligned slivers can be detected from the bbox short side
    // alone, but diagonal 1-px slivers (bbox is nearly square yet the
    // polygon occupies a tiny fraction) need a fill-ratio test as a
    // separate path.
    const glm::vec2 V0(cmd.v0), V1(cmd.v1), V2(cmd.v2), V3(cmd.v3);

    const float xMin = std::min({V0.x, V1.x, V2.x, V3.x});
    const float xMax = std::max({V0.x, V1.x, V2.x, V3.x});
    const float yMin = std::min({V0.y, V1.y, V2.y, V3.y});
    const float yMax = std::max({V0.y, V1.y, V2.y, V3.y});
    const float bw = xMax - xMin;
    const float bh = yMax - yMin;
    const float bboxShort = std::min(bw, bh);
    const float bboxLong  = std::max(bw, bh);

    // Shoelace area (twisted-quad robust via abs).
    const float twiceSigned =
        (V0.x * V1.y - V1.x * V0.y) +
        (V1.x * V2.y - V2.x * V1.y) +
        (V2.x * V3.y - V3.x * V2.y) +
        (V3.x * V0.y - V0.x * V3.y);
    const float area = std::fabs(twiceSigned) * 0.5f;

    // Zero-area degenerates (4 verts collapsed to a line or point)
    // produce essentially no render output on real HW (the Saturn
    // polygon rasterizer does not draw zero-area polygons), so they
    // are excluded from supercover.
    if (area < 0.5f) return false;

    const float widthThr = std::ceil(scaleMax * 2.0f) + 1.0f;

    // Rule (a) axis-aligned sliver: thin bbox short side AND
    // long/short ratio >= 4. Catches bbox 6x1 (aspect=6) at the
    // boundary, excludes bbox 3x1 (aspect=3).
    if (bboxShort > 1e-6f && bboxShort <= widthThr) {
        const float bboxAspect = bboxLong / bboxShort;
        if (bboxAspect >= 4.0f) return true;
    }

    // Rule (b) diagonal sliver: bbox short side passes the threshold
    // but the polygon only occupies a tiny fraction of the bbox
    // (= belt-shaped). Decide via fill ratio. Regardless of the short
    // side, we still require a minimum long-axis length (widthThr*2)
    // for gaps to form.
    const float bboxArea = bw * bh;
    if (bboxArea > 1e-6f && bboxLong > widthThr * 2.0f) {
        const float fillRatio = area / bboxArea;
        if (fillRatio < 0.10f) return true;
    }

    return false;
}

void applyState(Vdp1Cmd& cmd, const Vdp1State& s, bool inclusiveVerts) {
    // Preserve DILATE_BR_X/Y across re-application: the encoder sets these
    // bits when building the cmd, then appendCommand calls applyState
    // again without re-passing the param. OR-ing the existing bits keeps
    // the flags round-trip-safe.
    const bool callerInclusive =
        inclusiveVerts ||
        ((cmd.flags & (VDP1C_FLAG_DILATE_BR_X | VDP1C_FLAG_DILATE_BR_Y)) != 0u);

    // localCoord / systemClip / userClip are stored RAW (Saturn coords),
    // resolution-independent like the graphics path's Vdp1Regs->localX. Scale
    // them here by the per-axis effective HD factor (s.scaleX/scaleY = the
    // SAME vdp1wratio*computeFbScaleX the host hooks apply to vertices) so the
    // offset matches the already-scaled cmd.v0..v3. Storing them raw means a
    // mid-frame resolution change re-scales correctly on the next frame even
    // when the game omits the state-only command (issue #106: the previous
    // pre-scaled storage left a stale offset after a resolution change,
    // shifting every vertex toward the bottom-right). With s.scaleX defaulting
    // to 1.0, this is a no-op for host gtests that pass unscaled state.
    const glm::ivec2 scaledLocal(
        static_cast<int>(static_cast<float>(s.localCoord.x) * s.scaleX),
        static_cast<int>(static_cast<float>(s.localCoord.y) * s.scaleY));
    cmd.v0 += scaledLocal;
    cmd.v1 += scaledLocal;
    cmd.v2 += scaledLocal;
    cmd.v3 += scaledLocal;
    cmd.bbox = computeBBox(cmd.v0, cmd.v1, cmd.v2, cmd.v3);

    // DISTORTED_SPRITE: vertices are raw Saturn-cell-inclusive corners.
    // The forward shader extends the BR vertices by 1 Saturn pixel
    // (= scaleMax fb px) along each edge unit vector. Mirror that here so
    // tile-binning bbox encloses the actual rendered region. Without this,
    // shrunk distorted sprites (e.g., 16-cell texture into 4 Saturn rows)
    // bin into too few tiles and miss the BR Saturn cell at HD upscale.
    if (cmd.cmdType == VDP1C_TYPE_DISTORTED_SPRITE) {
        const glm::vec2 v0f(cmd.v0), v1f(cmd.v1), v2f(cmd.v2), v3f(cmd.v3);
        const bool axisAligned =
            (cmd.v0.y == cmd.v1.y) && (cmd.v3.y == cmd.v2.y) &&
            (cmd.v0.x == cmd.v3.x) && (cmd.v1.x == cmd.v2.x);
        float minX, minY, maxX, maxY;
        if (axisAligned) {
            // Match the tile_shade / forward shaders' axisAligned branch:
            // BR-cell coverage extends in the screen-space +X / +Y direction
            // (max-X / max-Y vertices each get +scaleMax). Mirror that here
            // so tile binning encloses the actual rendered region for
            // flipped 2-point scaled sprites (CMDXC<CMDXA / CMDYC<CMDYA) too
            // -- the previous edge-direction extension extended toward the
            // wrong side of the screen and either over-bound the LEFT/TOP
            // tiles or under-bound the RIGHT/BOTTOM ones.
            minX = std::min({v0f.x, v1f.x, v2f.x, v3f.x});
            minY = std::min({v0f.y, v1f.y, v2f.y, v3f.y});
            maxX = std::max({v0f.x, v1f.x, v2f.x, v3f.x}) + s.scaleMax;
            maxY = std::max({v0f.y, v1f.y, v2f.y, v3f.y}) + s.scaleMax;
        } else {
            // Non-axis-aligned (true distorted sprite): the shader's
            // non-axisAligned branch extends along per-texel edge steps, so
            // mirror that here by walking each BR-facing edge by scaleMax.
            const glm::vec2 e_top   = v1f - v0f;
            const glm::vec2 e_bot   = v2f - v3f;
            const glm::vec2 e_left  = v3f - v0f;
            const glm::vec2 e_right = v2f - v1f;
            const float lenU0 = glm::length(e_top);
            const float lenU1 = glm::length(e_bot);
            const float lenV0 = glm::length(e_left);
            const float lenV1 = glm::length(e_right);
            const glm::vec2 du_top   = (lenU0 > 1e-3f) ? e_top   * (s.scaleMax / lenU0) : glm::vec2(0.0f);
            const glm::vec2 du_bot   = (lenU1 > 1e-3f) ? e_bot   * (s.scaleMax / lenU1) : glm::vec2(0.0f);
            const glm::vec2 dv_left  = (lenV0 > 1e-3f) ? e_left  * (s.scaleMax / lenV0) : glm::vec2(0.0f);
            const glm::vec2 dv_right = (lenV1 > 1e-3f) ? e_right * (s.scaleMax / lenV1) : glm::vec2(0.0f);
            const glm::vec2 v1e = v1f + du_top;
            const glm::vec2 v2e = v2f + du_bot + dv_right;
            const glm::vec2 v3e = v3f + dv_left;
            minX = std::min({v0f.x, v1e.x, v2e.x, v3e.x});
            minY = std::min({v0f.y, v1e.y, v2e.y, v3e.y});
            maxX = std::max({v0f.x, v1e.x, v2e.x, v3e.x});
            maxY = std::max({v0f.y, v1e.y, v2e.y, v3e.y});
        }
        // Inclusive-bbox convention: shader sweep uses floor(cmin)/ceil(cmax)
        // with exclusive `< y1`, so last-row inclusive index = ceil(maxV)-1.
        cmd.bbox.x = static_cast<int>(std::floor(minX));
        cmd.bbox.y = static_cast<int>(std::floor(minY));
        cmd.bbox.z = std::max(cmd.bbox.x, static_cast<int>(std::ceil(maxX)) - 1);
        cmd.bbox.w = std::max(cmd.bbox.y, static_cast<int>(std::ceil(maxY)) - 1);
    }

    // Per-axis small-polygon guard: dilation extends BR edges by ~scaleMax
    // fb px (= 1 Saturn cell). For polygons whose bbox extent on a given
    // axis is on that order, the extension would visibly distort the shape
    // (e.g., a 1x1 cell sprite at RES_4x doubling in size). Decide each
    // axis independently so wide-thin polygons (100x1 cells) still get
    // horizontal gap-fix while their short vertical axis is left alone.
    // Threshold = scaleMax * MIN_DILATION_BBOX_CELLS (4 Saturn cells).
    // DISTORTED_SPRITE bbox is already extended above, so we skip this
    // path for that type to avoid double-extension.
    constexpr float MIN_DILATION_BBOX_CELLS = 4.0f;
    bool dilateX = false;
    bool dilateY = false;
    if (callerInclusive && cmd.cmdType != VDP1C_TYPE_DISTORTED_SPRITE) {
        const int bboxW = cmd.bbox.z - cmd.bbox.x;
        const int bboxH = cmd.bbox.w - cmd.bbox.y;
        const float minDimFb = s.scaleMax * MIN_DILATION_BBOX_CELLS;
        dilateX = (static_cast<float>(bboxW) >= minDimFb);
        dilateY = (static_cast<float>(bboxH) >= minDimFb);
    }

    // Bbox extension: only along the axes that pass the size guard. The
    // shader's brEdgeDilation also gates per-axis on the same flags, so
    // BR-edge coverage stays inside the extended bbox.
    if (dilateX || dilateY) {
        const int dilationPix = std::max(0, static_cast<int>(std::ceil(s.scaleMax)) - 1);
        if (dilateX) cmd.bbox.z += dilationPix;
        if (dilateY) cmd.bbox.w += dilationPix;
    }
    // systemClip / userClip are also stored raw (see localCoord note above);
    // scale to fb pixels so the shader's pix-vs-clip comparison is correct
    // across resolution changes.
    cmd.systemClip = glm::ivec4(
        static_cast<int>(static_cast<float>(s.systemClip.x) * s.scaleX),
        static_cast<int>(static_cast<float>(s.systemClip.y) * s.scaleY), 0, 0);
    cmd.userClip = glm::ivec4(
        static_cast<int>(static_cast<float>(s.userClip.x) * s.scaleX),
        static_cast<int>(static_cast<float>(s.userClip.y) * s.scaleY),
        static_cast<int>(static_cast<float>(s.userClip.z) * s.scaleX),
        static_cast<int>(static_cast<float>(s.userClip.w) * s.scaleY));
    // F18: VDP2 attrs -- encode functions zero-initialize the struct, callers
    // (Vdp1Renderer host hook) overwrite priority/colorcl/vdp2Attrs AFTER the
    // encode call. applyState must NOT clobber them on repeated invocations
    // (encodePolygon -> appendCommand -> applyState would lose them otherwise).
    // Default state when never explicitly set is "all zero" which still routes
    // to slot 0 -- matches the behavior tests expect.

    // F13: gate the 2-triangle shader path on concave / twisted quads. Only
    // sprites and polygons trace coverage as a 4-vertex region; polylines and
    // lines walk segments so the flag is irrelevant for them.
    cmd.flags = 0u;
    if (dilateX) cmd.flags |= VDP1C_FLAG_DILATE_BR_X;
    if (dilateY) cmd.flags |= VDP1C_FLAG_DILATE_BR_Y;
    if (cmd.cmdType == VDP1C_TYPE_POLYGON ||
        cmd.cmdType == VDP1C_TYPE_NORMAL_SPRITE ||
        cmd.cmdType == VDP1C_TYPE_SCALED_SPRITE ||
        cmd.cmdType == VDP1C_TYPE_DISTORTED_SPRITE) {
        // Quads with a collapsed vertex (any consecutive pair identical) are
        // really triangles; insideStrict's 4-edge test gets confused by the
        // zero-length edge (degenerate edge fn returns 0 for all pixels and
        // the polygon's winding is undefined). Force the FLAG_TRI_SPLIT
        // path so the shader uses 2-triangle barycentric coverage -- the
        // collapsed-edge triangle is empty (insideTriangle returns false)
        // and the other triangle (v0,v2,v3 or v0,v1,v3 etc.) renders
        // correctly. cmd 276 (CMDXA==CMDXB, CMDYA==CMDYB) hit this:
        // failed insideStrict so the polygon wasn't drawn at all.
        const bool isDegenerateQuad =
            cmd.v0 == cmd.v1 || cmd.v1 == cmd.v2 ||
            cmd.v2 == cmd.v3 || cmd.v3 == cmd.v0;
        if (isDegenerateQuad ||
            isConcave(glm::vec2(cmd.v0), glm::vec2(cmd.v1),
                      glm::vec2(cmd.v2), glm::vec2(cmd.v3))) {
            cmd.flags |= VDP1C_FLAG_TRI_SPLIT;
        }
        // THIN: edge-supercover gate. Geometric (not bbox) thickness so
        // diagonal 1-px stripes also get flagged. Per-axis brEdgeDilation
        // alone is NOT sufficient for thin diagonal slivers (verified in
        // Sega Rally) -- the supercover band closes the remaining inter-poly
        // gaps where two thin polygons share an edge that rounds to the
        // same screen pixel.
        // One-line sprite: both side edges have length 0 (A == D and
        // B == C), so VDP1 draws dmax + 1 = 1 line from A to B even though
        // the quad has zero area. isThinPolygonGeometric rejects zero-area
        // quads, and the shaders' one-cell extension only rebuilds a drawable
        // quad when the line is horizontal (their axis-aligned case), so a
        // vertical or diagonal one-line sprite drew nothing. Route it through
        // the FLAG_THIN supercover band, which covers the A-B segment.
        const bool oneLineSprite =
            cmd.cmdType == VDP1C_TYPE_DISTORTED_SPRITE &&
            cmd.v0 == cmd.v3 && cmd.v1 == cmd.v2 && cmd.v0.y != cmd.v1.y;
#if 1
        if (oneLineSprite || isThinPolygonGeometric(cmd, s.scaleMax)) {
            cmd.flags |= VDP1C_FLAG_THIN;
            // The shader's FLAG_THIN supercover band (forward
            // kForwardShaderSrc / vdp1_compute_forward.comp) extends
            // halfHD = THIN_SUPERCOVER_HALF_SAT * scaleMax fb px outside
            // the polygon. Dilate the bbox (the tile-binning input) by
            // the same amount so the band-touching tiles are not dropped
            // from assignment, which otherwise shows up as gaps along
            // the band at HD resolution. THIN_SUPERCOVER_HALF_SAT /
            // MIN_SUPERCOVER_HD must match the shader values
            // (0.75 / 0.555).
            constexpr float kThinSupercoverHalfSat = 0.50f;
            constexpr float kMinSupercoverHD       = 0.555f;
            const float halfHD = std::max(kMinSupercoverHD,
                                          kThinSupercoverHalfSat * s.scaleMax);
            const int extPix = static_cast<int>(std::ceil(halfHD));
            cmd.bbox.x -= extPix;
            cmd.bbox.y -= extPix;
            cmd.bbox.z += extPix;
            cmd.bbox.w += extPix;
        }
#endif
    }
}

namespace {
inline float cross2(const glm::vec2& a, const glm::vec2& b) { return a.x*b.y - a.y*b.x; }
}  // namespace

glm::vec2 inverseBilinear(glm::vec2 p, glm::vec2 A, glm::vec2 B, glm::vec2 C, glm::vec2 D) {
    glm::vec2 e0 = B - A;
    glm::vec2 e1 = D - A;
    glm::vec2 e2 = (A - B + C - D);
    glm::vec2 q  = p - A;

    // Coefficients of the quadratic a*t^2 + b*t + c = 0.
    // Derivation: q = s*(e0 + t*e2) + t*e1 cross with (e0+t*e2) cancels s
    // (Inigo Quilez).
    float a = cross2(e2, e1);
    float b = cross2(e0, e1) - cross2(e2, q);
    float c = cross2(q, e0);

    float t;
    // Relative threshold: treat a as zero when much smaller than b.
    // Axis-aligned quads should have e2 = (0, 0) and a = 0, but FP
    // rounding in (A - B + C - D) leaves a small non-zero residue when
    // vertex coords span large positive and negative values combined
    // with non-integer scaleX/scaleY (e.g. flipped scaled sprites at
    // non-integer RES_NATIVE scale). The absolute 1e-6 threshold missed
    // that residue and routed to the quadratic branch, where
    // sqrt(b*b - 4ac) with 4ac << b*b suffers catastrophic cancellation
    // and t collapses to 0 or 1. Mirror the shader fix.
    if (std::fabs(a) * 1e6f < std::fabs(b)) {
        // Parallelogram - linear case.
        if (std::fabs(b) < 1e-6f) return glm::vec2(0);
        t = -c / b;
    } else {
        float disc = std::max(0.0f, b * b - 4.0f * a * c);
        float sq = std::sqrt(disc);
        float t0 = (-b - sq) / (2.0f * a);
        float t1 = (-b + sq) / (2.0f * a);
        // Tie-break:
        //   1. If both roots are in [0, 1], prefer the strictly interior
        //      root (closer to 0.5). Required for degenerate quads (e.g.
        //      v2==v3) where the bilinear has TWO valid roots: the interior
        //      root v/u and the boundary root t==1 corresponding to the
        //      collapsed vertex. The boundary root makes sden = 0 in the s
        //      solve and falls back to s=0, mapping the WHOLE collapsed
        //      texture row to a single screen point -- interior pixels then
        //      get wrong UV. The "closest to [0, 1]" rule treats both roots
        //      as equally good (d0 == d1 == 0) and picks t0, which happens
        //      to be the interior root for CW windings but the collapsed
        //      root for CCW. Picking the strictly interior root works for
        //      both windings.
        //   2. Otherwise, pick the root closest to [0, 1]. Inside-the-quad
        //      points have exactly one root in-range. BR-dilation band
        //      points have BOTH roots slightly out-of-range; the closer
        //      one keeps UV continuity across the polygon edge so
        //      clamp(uv, 0, 1) snaps cleanly to the edge texel. Picking
        //      arbitrarily (old code: always t0) flipped outside-band UV
        //      to the WRONG side on tilted polygons, causing texture
        //      wraparound in the dilated region.
        auto distFromUnit = [](float v) {
            return std::max(0.0f, std::max(-v, v - 1.0f));
        };
        float d0 = distFromUnit(t0);
        float d1 = distFromUnit(t1);
        if (d0 == 0.0f && d1 == 0.0f) {
            t = (std::fabs(t0 - 0.5f) <= std::fabs(t1 - 0.5f)) ? t0 : t1;
        } else {
            t = (d0 <= d1) ? t0 : t1;
        }
    }
    glm::vec2 sden = e0 + e2 * t;
    float s;
    if (std::fabs(sden.x) > std::fabs(sden.y)) {
        s = (std::fabs(sden.x) < 1e-6f) ? 0.0f : (q.x - e1.x * t) / sden.x;
    } else {
        s = (std::fabs(sden.y) < 1e-6f) ? 0.0f : (q.y - e1.y * t) / sden.y;
    }
    return glm::vec2(s, t);
}

Vdp1Cmd encodeDistortedSprite(
    std::array<glm::ivec2, 4> vertices,
    uint32_t srca,
    glm::uvec2 charSize,
    uint32_t color,
    uint32_t pmod,
    glm::ivec2 localCoord,
    uint32_t flip,
    uint32_t gouraudAddr,
    bool inclusiveVerts)
{
    Vdp1Cmd cmd{};
    cmd.cmdType  = VDP1C_TYPE_DISTORTED_SPRITE;
    cmd.pmod     = pmod;
    cmd.color    = color;
    cmd.srca     = srca;
    cmd.charSize = charSize;
    cmd.gouraudAddr = ((pmod & 0x4u) != 0u && gouraudAddr != 0u)
        ? glm::uvec2(gouraudAddr, 1u)
        : glm::uvec2(0u);
    // Preserve the Saturn standard vertex order (no CCW normalization). The
    // scanline rasterizer walks v0->v3 (TL->BL) and v1->v2 (TR->BR), so a
    // different input ordering renders as texture rotation, matching Saturn.
    cmd.v0 = vertices[0];
    cmd.v1 = vertices[1];
    cmd.v2 = vertices[2];
    cmd.v3 = vertices[3];

    cmd.clipMode = deriveClipMode(pmod);
    cmd.flip = flip & 3u;  // bit 0 = CMDCTRL Hflip, bit 1 = CMDCTRL Vflip

    // localCoord application, bbox computation, and clip baking are handled
    // by applyState(). inclusiveVerts is forwarded so the host can control
    // whether the BR edge gets shader-side dilation (only consulted by the
    // tile_shade fallback path; the forward-mapping shader handles
    // cell-inclusive coverage natively via its lerp denominator).
    Vdp1State s; s.localCoord = localCoord;
    applyState(cmd, s, inclusiveVerts);
    return cmd;
}

Vdp1Cmd encodeNormalSprite(
    int x, int y, int w, int h,
    uint32_t srca,
    glm::uvec2 charSize,
    uint32_t color,
    uint32_t pmod,
    glm::ivec2 localCoord,
    uint32_t flip,
    uint32_t gouraudAddr)
{
    // Build the 4 axis-aligned rect corners in Saturn standard order
    // (TL, TR, BR, BL) and route through the DISTORTED_SPRITE pipeline. The
    // existing inverse-bilinear UV solver returns the trivially correct
    // (s, t) on a non-skewed parallelogram.
    std::array<glm::ivec2, 4> verts = {{
        glm::ivec2(x,         y        ),
        glm::ivec2(x + w,     y        ),
        glm::ivec2(x + w,     y + h    ),
        glm::ivec2(x,         y + h    )
    }};
    return encodeDistortedSprite(verts, srca, charSize, color, pmod, localCoord, flip, gouraudAddr);
}

Vdp1Cmd encodeScaledSprite(
    std::array<glm::ivec2, 4> vertices,
    uint32_t srca,
    glm::uvec2 charSize,
    uint32_t color,
    uint32_t pmod,
    glm::ivec2 localCoord,
    uint32_t flip,
    uint32_t gouraudAddr)
{
    // ScaledSprite differs from DistortedSprite only in how the host
    // resolves CMDCTRL zoom-point bits into 4 vertices. Once that is done
    // the shader path is identical, so we simply forward.
    return encodeDistortedSprite(vertices, srca, charSize, color, pmod, localCoord, flip, gouraudAddr);
}

Vdp1Cmd encodePolyline(
    std::array<glm::ivec2, 4> vertices,
    uint32_t color,
    uint32_t pmod,
    glm::ivec2 localCoord,
    uint32_t gouraudAddr)
{
    Vdp1Cmd cmd{};
    cmd.cmdType  = VDP1C_TYPE_POLYLINE;
    cmd.pmod     = pmod;
    cmd.color    = color;
    cmd.srca     = 0;
    cmd.charSize = glm::uvec2(0);
    cmd.gouraudAddr = ((pmod & 0x4u) != 0u && gouraudAddr != 0u)
        ? glm::uvec2(gouraudAddr, 1u)
        : glm::uvec2(0u);
    cmd.v0 = vertices[0];
    cmd.v1 = vertices[1];
    cmd.v2 = vertices[2];
    cmd.v3 = vertices[3];
    cmd.clipMode = deriveClipMode(pmod);
    cmd.flip     = 0;

    Vdp1State s; s.localCoord = localCoord;
    // inclusiveVerts=true: matches encodePolygon. The shader's polyline path
    // shifts BR-edge vertices by dilationPix so the outline aligns with the
    // matching polygon's filled area at HD upscale (cmd 92 polygon vs cmd 94
    // polyline both with CMDXA-D=80,26-128,37 must overlay perfectly).
    applyState(cmd, s, /*inclusiveVerts=*/true);
    return cmd;
}

Vdp1Cmd encodeLine(
    glm::ivec2 v0,
    glm::ivec2 v1,
    uint32_t color,
    uint32_t pmod,
    glm::ivec2 localCoord,
    uint32_t gouraudAddr)
{
    Vdp1Cmd cmd{};
    cmd.cmdType  = VDP1C_TYPE_LINE;
    cmd.pmod     = pmod;
    cmd.color    = color;
    cmd.srca     = 0;
    cmd.charSize = glm::uvec2(0);
    cmd.gouraudAddr = ((pmod & 0x4u) != 0u && gouraudAddr != 0u)
        ? glm::uvec2(gouraudAddr, 1u)
        : glm::uvec2(0u);
    // Fill v2/v3 with the segment endpoints so computeBBox returns a valid
    // (collapsed) rectangle around the line. The shader only reads v0/v1
    // when cmdType == VDP1C_TYPE_LINE, but the bbox needs to encompass both
    // endpoints for the bin shader's tile-overlap test.
    cmd.v0 = v0;
    cmd.v1 = v1;
    cmd.v2 = v1;
    cmd.v3 = v0;
    cmd.clipMode = deriveClipMode(pmod);
    cmd.flip     = 0;

    Vdp1State s; s.localCoord = localCoord;
    // Same rationale as encodePolyline: cell-inclusive vertex semantics.
    applyState(cmd, s, /*inclusiveVerts=*/true);
    return cmd;
}

std::vector<uint32_t> collectDispatchIndices(
    const std::vector<Vdp1Cmd>& cmds, uint32_t lastCmdIndex)
{
    std::vector<uint32_t> out;
    out.reserve(cmds.size());
    // Treat UINT32_MAX as "no cap" to avoid uint overflow on lastCmdIndex+1.
    const uint32_t cap = (lastCmdIndex == UINT32_MAX)
        ? static_cast<uint32_t>(cmds.size())
        : (lastCmdIndex + 1u);
    const uint32_t upper = std::min<uint32_t>(static_cast<uint32_t>(cmds.size()), cap);
    for (uint32_t i = 0; i < upper; ++i) {
        const auto t = cmds[i].cmdType;
        // POLYGON / DISTORTED_SPRITE feed the scanline shader (1 thread per
        // scanline). NORMAL/SCALED were normalized to DISTORTED at encode
        // time so they take the same path. POLYLINE / LINE require the
        // tile_shade shader (1 thread per pixel) -- the scanline shader
        // returns early for those types, but we still include them here so
        // the tile-binning Bin shader sees the right command range.
        if (t != VDP1C_TYPE_NOOP) {
            out.push_back(i);
        }
    }
    return out;
}

namespace {
// AABB overlap test for Vdp1Cmd::bbox = (minX, minY, maxX, maxY). Boundary
// touch (a.maxX == b.minX) counts as overlap because both rasterize the
// shared pixel column.
inline bool bboxOverlap(const glm::ivec4& a, const glm::ivec4& b) {
    return !(a.z < b.x || b.z < a.x || a.w < b.y || b.w < a.y);
}
}  // namespace

uint32_t pipelineVariantForCmd(const Vdp1Cmd& cmd) {
    if (cmd.cmdType == VDP1C_TYPE_POLYGON) {
        return VDP1C_VARIANT_POLYGON;
    }
    if (cmd.cmdType == VDP1C_TYPE_DISTORTED_SPRITE) {
        const uint32_t cm = (cmd.pmod >> 3u) & 7u;
        if (cm <= 5u) {
            return VDP1C_VARIANT_SPRITE_CM0 + cm;  // 1..6
        }
        return VDP1C_VARIANT_INVALID;  // cm=6/7 reserved by Saturn spec
    }
    return VDP1C_VARIANT_INVALID;
}

std::vector<uint8_t> computeBarrierMask(
    const std::vector<Vdp1Cmd>& cmds,
    const std::vector<uint32_t>& indices)
{
    std::vector<uint8_t> mask(indices.size(), 0u);
    if (indices.empty()) return mask;

    // The first dispatch never needs an inter-command barrier (the initial
    // image-layout transition barrier handles synchronization with whatever
    // came before).
    size_t batchStart = 0;
    for (size_t k = 1; k < indices.size(); ++k) {
        const glm::ivec4& cur = cmds[indices[k]].bbox;
        bool overlap = false;
        for (size_t m = batchStart; m < k; ++m) {
            if (bboxOverlap(cur, cmds[indices[m]].bbox)) {
                overlap = true;
                break;
            }
        }
        if (overlap) {
            mask[k] = 1u;
            batchStart = k;  // start fresh in-flight set from this index
        }
    }
    return mask;
}

}  // namespace vdp1c
