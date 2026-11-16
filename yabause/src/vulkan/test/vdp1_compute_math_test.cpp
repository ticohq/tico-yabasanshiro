#include "gtest/gtest.h"
#include "../Vdp1ComputeMath.h"
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

using namespace vdp1c;
using glm::vec2;
using glm::ivec2;
using glm::ivec4;

TEST(Vdp1ComputeMath, BBoxCoversAllVertices) {
    auto bbox = computeBBox(
        ivec2(10, 20), ivec2(50, 30), ivec2(60, 80), ivec2(15, 70)
    );
    EXPECT_EQ(bbox.x, 10);
    EXPECT_EQ(bbox.y, 20);
    EXPECT_EQ(bbox.z, 60);
    EXPECT_EQ(bbox.w, 80);
}

TEST(Vdp1ComputeMath, EdgeFunctionLeftSideIsPositive) {
    EXPECT_GT(edgeFunction(vec2(5, 5), vec2(0, 0), vec2(10, 0)), 0.0f);
    EXPECT_LT(edgeFunction(vec2(5, -5), vec2(0, 0), vec2(10, 0)), 0.0f);
}

TEST(Vdp1ComputeMath, InsideQuadAxisAlignedRectangle) {
    vec2 a(0, 0), b(10, 0), c(10, 10), d(0, 10);
    ASSERT_GT(edgeFunction(c, a, b), 0.0f);
    EXPECT_TRUE(insideQuad(vec2(5, 5), a, b, c, d));
    EXPECT_FALSE(insideQuad(vec2(15, 5), a, b, c, d));
    EXPECT_FALSE(insideQuad(vec2(-1, 5), a, b, c, d));
    EXPECT_TRUE(insideQuad(vec2(0, 0), a, b, c, d));
}

TEST(Vdp1ComputeMath, InsideQuadDistortedSEGARallyCase) {
    // Thin polygon of the kind SEGA Rally produces (seam reproduction target).
    // These coordinates are already CCW, so normalizeWinding is a no-op.
    // A triangle split would err along the diagonal; a single quad test does not.
    vec2 a(100, 100), b(105, 102), c(108, 200), d(102, 198);
    bool wasCCW = normalizeWinding(a, b, c, d);
    EXPECT_TRUE(wasCCW);  // confirm CCW input
    EXPECT_TRUE(insideQuad(vec2(103, 150), a, b, c, d));
    EXPECT_TRUE(insideQuad(vec2(104, 151), a, b, c, d));
}

TEST(Vdp1ComputeMath, InsideQuadCWInputAfterNormalize) {
    // CW input (clockwise; with Y pointing down on screen = mathematical CW)
    // a=UL, b=LL, c=LR, d=UR -> CW
    vec2 a(0, 0), b(0, 10), c(10, 10), d(10, 0);
    ASSERT_LT(edgeFunction(c, a, b), 0.0f);  // confirm CW

    bool wasCCW = normalizeWinding(a, b, c, d);
    EXPECT_FALSE(wasCCW);
    // After normalization the quad is CCW and the inside test holds
    EXPECT_TRUE(insideQuad(vec2(5, 5), a, b, c, d));
}

TEST(Vdp1ComputeMath, IsConcaveDetectsConvexAxisAlignedQuad) {
    // CCW axis-aligned rectangle. All cross products should be the same sign.
    EXPECT_FALSE(isConcave(vec2(0, 0), vec2(10, 0), vec2(10, 10), vec2(0, 10)));
    // CW input -- also convex (mixed sign would flip but uniformly).
    EXPECT_FALSE(isConcave(vec2(0, 0), vec2(0, 10), vec2(10, 10), vec2(10, 0)));
}

TEST(Vdp1ComputeMath, IsConcaveDetectsTrueConcaveQuad) {
    // C is pulled strictly inside triangle ABD (the (5,5) midpoint of BD
    // would be a degenerate collinear case, so we use (5,3) which is below
    // the BD line). The interior angle at C exceeds 180 deg -> concave.
    EXPECT_TRUE(isConcave(vec2(0, 0), vec2(10, 0), vec2(5, 3), vec2(0, 10)));
}

TEST(Vdp1ComputeMath, IsConcaveDetectsTwistedQuad) {
    // Self-intersecting "bowtie" quad. Cross products will alternate sign.
    EXPECT_TRUE(isConcave(vec2(0, 0), vec2(10, 10), vec2(10, 0), vec2(0, 10)));
}

TEST(Vdp1ComputeMath, IsConcaveAcceptsParallelogram) {
    // Sheared but convex; should not flag as concave.
    EXPECT_FALSE(isConcave(vec2(0, 0), vec2(10, 2), vec2(12, 12), vec2(2, 10)));
}

TEST(Vdp1ComputeMath, IsConcaveTreatsCollinearEdgePairAsConvex) {
    // Three collinear points with the 4th forming a triangle. Strict-sign
    // gating with epsilon must NOT flag this -- degenerate but not concave.
    EXPECT_FALSE(isConcave(vec2(0, 0), vec2(5, 0), vec2(10, 0), vec2(5, 5)));
}

TEST(Vdp1ComputeMath, NormalizeWindingFlipsCWToCCW) {
    vec2 a(0, 0), b(0, 10), c(10, 10), d(10, 0);
    bool wasCCW = normalizeWinding(a, b, c, d);
    EXPECT_FALSE(wasCCW);
    EXPECT_GT(edgeFunction(c, a, b), 0.0f);
}

TEST(Vdp1ComputeMath, EncodePolygonFillsStruct) {
    auto cmd = encodePolygon(
        {{ ivec2(10, 20), ivec2(50, 30), ivec2(60, 80), ivec2(15, 70) }},
        ivec2(0, 0),
        0x7C00,
        0x00C0
    );
    EXPECT_EQ(cmd.cmdType, VDP1C_TYPE_POLYGON);
    EXPECT_EQ(cmd.color, 0x7C00u);
    EXPECT_EQ(cmd.pmod, 0x00C0u);
    EXPECT_EQ(cmd.v0.x, 10);
    EXPECT_EQ(cmd.bbox.x, 10);
    EXPECT_EQ(cmd.bbox.z, 60);
    EXPECT_EQ(cmd.clipMode, 0u);
}

TEST(Vdp1ComputeMath, EncodePolygonAppliesLocalCoord) {
    auto cmd = encodePolygon(
        {{ ivec2(0, 0), ivec2(10, 0), ivec2(10, 10), ivec2(0, 10) }},
        ivec2(160, 112),
        0,
        0
    );
    EXPECT_EQ(cmd.v0.x, 160);
    EXPECT_EQ(cmd.v0.y, 112);
    EXPECT_EQ(cmd.bbox.x, 160);
    EXPECT_EQ(cmd.bbox.z, 170);
}

TEST(Vdp1ComputeMath, EncodePolygonPreservesVertexOrder) {
    // The scanline rasterizer (vidsoft-faithful) walks TL->BL and TR->BR
    // assuming the Saturn standard order v0=TL, v1=TR, v2=BR, v3=BL.
    // Different vertex orderings render as texture rotations, so we do NOT
    // apply CCW normalization (B<->D swap).
    // Input: a=UL, b=LL, c=LR, d=UR (visually clockwise on screen).
    auto cmd = encodePolygon(
        {{ ivec2(0, 0), ivec2(0, 10), ivec2(10, 10), ivec2(10, 0) }},
        ivec2(0, 0),
        0,
        0
    );
    // The input order must be preserved verbatim.
    EXPECT_EQ(cmd.v0, ivec2(0, 0));
    EXPECT_EQ(cmd.v1, ivec2(0, 10));
    EXPECT_EQ(cmd.v2, ivec2(10, 10));
    EXPECT_EQ(cmd.v3, ivec2(10, 0));
    // bbox covers all 4 vertices.
    EXPECT_EQ(cmd.bbox.x, 0);
    EXPECT_EQ(cmd.bbox.y, 0);
    EXPECT_EQ(cmd.bbox.z, 10);
    EXPECT_EQ(cmd.bbox.w, 10);
}

TEST(Vdp1State, ResetReturnsDefaultValues) {
    vdp1c::Vdp1State s;
    vdp1c::resetState(s);
    EXPECT_EQ(s.systemClip, glm::ivec2(2047, 2047));
    EXPECT_EQ(s.userClip,   glm::ivec4(0, 0, 2047, 2047));
    EXPECT_EQ(s.localCoord, glm::ivec2(0, 0));
}

TEST(Vdp1State, ApplyStateOverwritesCmdFields) {
    Vdp1State s;
    s.systemClip = glm::ivec2(500, 400);
    s.userClip   = glm::ivec4(10, 20, 300, 250);
    s.localCoord = glm::ivec2(50, 60);

    Vdp1Cmd cmd{};
    cmd.cmdType = VDP1C_TYPE_POLYGON;
    cmd.v0 = glm::ivec2(0, 0);
    cmd.v1 = glm::ivec2(10, 0);
    cmd.v2 = glm::ivec2(10, 10);
    cmd.v3 = glm::ivec2(0, 10);

    applyState(cmd, s);
    EXPECT_EQ(cmd.systemClip, glm::ivec4(500, 400, 0, 0));
    EXPECT_EQ(cmd.userClip,   glm::ivec4(10, 20, 300, 250));
    EXPECT_EQ(cmd.v0, glm::ivec2(50, 60));
    EXPECT_EQ(cmd.v1, glm::ivec2(60, 60));
    EXPECT_EQ(cmd.v2, glm::ivec2(60, 70));
    EXPECT_EQ(cmd.v3, glm::ivec2(50, 70));
    EXPECT_EQ(cmd.bbox, glm::ivec4(50, 60, 60, 70));
}

TEST(Vdp1State, ApplyStateSetsFlagThinForNarrowPolygon) {
    // Axis-aligned 100x1 thin polygon at scaleMax=1.0 -> FLAG_THIN set.
    Vdp1State s;
    s.scaleMax = 1.0f;
    Vdp1Cmd cmd{};
    cmd.cmdType = VDP1C_TYPE_POLYGON;
    cmd.v0 = glm::ivec2(0, 0);
    cmd.v1 = glm::ivec2(100, 0);
    cmd.v2 = glm::ivec2(100, 1);
    cmd.v3 = glm::ivec2(0, 1);
    applyState(cmd, s);
    EXPECT_NE(cmd.flags & VDP1C_FLAG_THIN, 0u);
}

TEST(Vdp1State, ApplyStateClearsFlagThinForUnitDot) {
    // FLAG_THIN compensates for lines turning into trapezoids with rows of gaps at HD upscale.
    // A 1x1 dot has no long axis, so no gaps can form; it must not be THIN
    // (the old implementation always flagged dots as THIN).
    Vdp1State s;
    s.scaleMax = 1.0f;
    Vdp1Cmd cmd{};
    cmd.cmdType = VDP1C_TYPE_POLYGON;
    cmd.v0 = glm::ivec2(0, 0);
    cmd.v1 = glm::ivec2(1, 0);
    cmd.v2 = glm::ivec2(1, 1);
    cmd.v3 = glm::ivec2(0, 1);
    applyState(cmd, s);
    EXPECT_EQ(cmd.flags & VDP1C_FLAG_THIN, 0u);
}

TEST(Vdp1State, ApplyStateClearsFlagThinForShortNarrowPolygon) {
    // 1x3 short piece: thickness=1 is thin, but aspect=3 has no long axis for gaps.
    // Boundary test of the new gate: long/short ratio below 4 is not THIN.
    Vdp1State s;
    s.scaleMax = 1.0f;
    Vdp1Cmd cmd{};
    cmd.cmdType = VDP1C_TYPE_POLYGON;
    cmd.v0 = glm::ivec2(0, 0);
    cmd.v1 = glm::ivec2(3, 0);
    cmd.v2 = glm::ivec2(3, 1);
    cmd.v3 = glm::ivec2(0, 1);
    applyState(cmd, s);
    EXPECT_EQ(cmd.flags & VDP1C_FLAG_THIN, 0u);
}

TEST(Vdp1State, ApplyStateClearsFlagThinForNormalPolygon) {
    Vdp1State s;
    s.scaleMax = 1.0f;
    Vdp1Cmd cmd{};
    cmd.cmdType = VDP1C_TYPE_DISTORTED_SPRITE;
    cmd.v0 = glm::ivec2(0, 0);
    cmd.v1 = glm::ivec2(24, 0);
    cmd.v2 = glm::ivec2(24, 24);
    cmd.v3 = glm::ivec2(0, 24);
    applyState(cmd, s);
    EXPECT_EQ(cmd.flags & VDP1C_FLAG_THIN, 0u);
}

TEST(Vdp1State, ApplyStateSetsFlagThinForDiagonalNarrowPolygon) {
    // bbox-only would miss this case; geometric thickness=1 catches it.
    Vdp1State s;
    s.scaleMax = 1.0f;
    Vdp1Cmd cmd{};
    cmd.cmdType = VDP1C_TYPE_DISTORTED_SPRITE;
    cmd.v0 = glm::ivec2(0,  0);
    cmd.v1 = glm::ivec2(70, 70);
    cmd.v2 = glm::ivec2(69, 71);
    cmd.v3 = glm::ivec2(-1, 1);
    applyState(cmd, s);
    EXPECT_NE(cmd.flags & VDP1C_FLAG_THIN, 0u);
}

TEST(Vdp1ComputeMath, IsThinPolygonGeometricOnSliverDump) {
    // Sliver quads from a real-scene JSONL dump (tilted thin strips with dy=1,
    // a pattern that leaves gaps between adjacent polygons). Under the new aspect
    // gate (longest/thickness >= 4) every one of them must be judged THIN.
    // The FLAG_THIN gate in applyState is sometimes disabled temporarily during
    // gap investigations, so test the algorithm by calling isThinPolygonGeometric
    // directly (not through applyState).
    constexpr float scaleMax = 1.0f;

    struct SliverCase {
        const char* name;
        glm::ivec2 v0, v1, v2, v3;
    };
    const SliverCase cases[] = {
        {"#01", {  0,-1}, {-22,-2}, {-26,-1}, { -2,-1}}, // aspect ~ 44
        {"#02", {-18,-2}, {-38,-3}, {-44,-2}, {-22,-2}}, // aspect ~ 37
        {"#03 v0==v3 deg", { 22,-2}, {  3,-3}, {  1,-2}, { 22,-2}}, // aspect ~ 42
        {"#04", { 57,-2}, { 39,-3}, { 40,-2}, { 60,-2}}, // aspect ~ 47
        {"#05", {  7,-3}, {-10,-4}, {-13,-3}, {  5,-3}}, // aspect ~ 32
        {"#06", { 53,-3}, { 37,-4}, { 38,-3}, { 54,-3}}, // aspect ~ 34
        {"#07", { -3,-4}, {-12,-5}, {-16,-4}, { -5,-4}}, // aspect ~ 19
        {"#08", { 10,-4}, { -2,-5}, { -3,-4}, {  9,-4}}, // aspect ~ 22
        {"#09", { 12,-4}, {  1,-5}, { -2,-5}, { 10,-4}}, // aspect ~ 58
        {"#10", { 26,-4}, { 14,-5}, { 12,-4}, { 25,-4}}, // aspect ~ 24
        {"#11", { 28,-4}, { 17,-5}, { 14,-5}, { 26,-4}}, // aspect ~ 58
        {"#12", { 30,-4}, { 19,-5}, { 17,-5}, { 28,-4}}, // aspect ~ 61
        {"#13", { 42,-4}, { 32,-5}, { 30,-4}, { 41,-4}}, // aspect ~ 20
        {"#14", { 43,-4}, { 33,-5}, { 32,-5}, { 42,-4}}, // aspect ~ 101
        {"#15", { 44,-4}, { 35,-5}, { 33,-5}, { 43,-4}}, // aspect ~ 67
        {"#16", { 45,-4}, { 36,-5}, { 35,-5}, { 44,-4}}, // aspect ~ 82
        {"#17 shortest", { 25,-6}, { 29,-5}, { 31,-5}, { 28,-6}}, // aspect ~ 6.8 (near the boundary)
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        Vdp1Cmd cmd{};
        cmd.cmdType = VDP1C_TYPE_DISTORTED_SPRITE;
        cmd.v0 = c.v0;
        cmd.v1 = c.v1;
        cmd.v2 = c.v2;
        cmd.v3 = c.v3;
        EXPECT_TRUE(isThinPolygonGeometric(cmd, scaleMax))
            << "Sliver case " << c.name << " must be classified THIN "
            << "(needs adjacent-polygon gap compensation)";
    }
}

TEST(Vdp1ComputeMath, IsThinPolygonGeometricNotThinOnSmallChunkDump) {
    // Small-chunk polygons from a real-scene JSONL dump. Each one either has a
    // bbox of only a few px on both axes, is degenerate (3 coincident verts), or
    // is not belt-shaped, so none can cause gaps and all must be NOT thin.
    // The old aspect-only gate wrongly flagged 5 of 10 as THIN, so this is pinned
    // as a regression guard.
    constexpr float scaleMax = 1.0f;

    struct ChunkCase {
        const char* name;
        glm::ivec2 v0, v1, v2, v3;
    };
    const ChunkCase cases[] = {
        // #1 v1==v2==v3 -> a segment, area=0 (rejected by the degenerate guard)
        {"#1 deg-line", {-86,24}, {-81,23}, {-81,23}, {-81,23}},
        // #2 v2==v3 -> triangle, bbox 7x3, fill=0.19
        {"#2 tri",      {-74,26}, {-76,24}, {-81,23}, {-81,23}},
        // #3 twisted, bbox 12x4, bboxShort=4 > widthThr=3
        {"#3 twist",    {-85,38}, {-81,36}, {-91,38}, {-93,40}},
        // #4 bbox 11x4, bboxShort=4 > 3
        {"#4 chunk",    {-75,35}, {-77,38}, {-67,39}, {-66,38}},
        // #5 bbox 2x7, aspect=3.5 < 4
        {"#5 short",    {-82,41}, {-82,37}, {-84,34}, {-83,40}},
        // #6 bbox 3x7, aspect=2.33 < 4
        {"#6 short",    {-68,42}, {-67,48}, {-65,46}, {-67,41}},
        // #7 bbox 3x3, nearly square
        {"#7 square",   {-80,32}, {-77,31}, {-77,29}, {-80,30}},
        // #8 v2==v3, bbox 2x2 small chunk
        {"#8 small",    {-80,35}, {-81,34}, {-82,36}, {-82,36}},
        // #9 v2==v3, bbox 8x3, fill=0.29 (not belt-shaped)
        {"#9 tri-mid",  {-85,27}, {-80,30}, {-77,29}, {-77,29}},
        // #10 bbox 3x2 small chunk
        {"#10 small",   {-83,31}, {-80,32}, {-80,30}, {-83,30}},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        Vdp1Cmd cmd{};
        cmd.cmdType = VDP1C_TYPE_DISTORTED_SPRITE;
        cmd.v0 = c.v0;
        cmd.v1 = c.v1;
        cmd.v2 = c.v2;
        cmd.v3 = c.v3;
        EXPECT_FALSE(isThinPolygonGeometric(cmd, scaleMax))
            << "Chunk case " << c.name << " must NOT be classified THIN "
            << "(not a gap-producing belt / degenerate / short small chunk)";
    }
}

TEST(Vdp1State, ApplyStateFlagsThinAndTriSplitOnDegenerateThin) {
    // Real cmd 324 from JSONL: twisted-thin polygon with 3 verts on
    // y=471 + 1 vertex offset to y=467. isConcave detects sign flip
    // -> TRI_SPLIT also set, but FLAG_THIN must also be set so the
    // shader routes through supercover (THIN priority over TRI_SPLIT).
    Vdp1State s;
    s.scaleMax = 4.0f;  // RES_NATIVE-ish HD scale
    Vdp1Cmd cmd{};
    cmd.cmdType = VDP1C_TYPE_DISTORTED_SPRITE;
    cmd.v0 = glm::ivec2(1014, 471);
    cmd.v1 = glm::ivec2(925, 467);
    cmd.v2 = glm::ivec2(937, 471);
    cmd.v3 = glm::ivec2(1031, 471);
    applyState(cmd, s);
    EXPECT_NE(cmd.flags & VDP1C_FLAG_THIN, 0u)
        << "cmd 324 must be classified thin so the shader applies supercover";
}

TEST(InverseBilinear, ParallelogramReturnsLinearMapping) {
    // Axis-aligned 10x10 unit square, center point
    glm::vec2 A(0,0), B(10,0), C(10,10), D(0,10);
    auto st = inverseBilinear(glm::vec2(5, 5), A, B, C, D);
    EXPECT_NEAR(st.x, 0.5f, 1e-3f);
    EXPECT_NEAR(st.y, 0.5f, 1e-3f);
}

TEST(InverseBilinear, ParallelogramCornerA) {
    glm::vec2 A(0,0), B(10,0), C(10,10), D(0,10);
    auto st = inverseBilinear(A, A, B, C, D);
    EXPECT_NEAR(st.x, 0.0f, 1e-3f);
    EXPECT_NEAR(st.y, 0.0f, 1e-3f);
}

TEST(InverseBilinear, ParallelogramCornerC) {
    glm::vec2 A(0,0), B(10,0), C(10,10), D(0,10);
    auto st = inverseBilinear(C, A, B, C, D);
    EXPECT_NEAR(st.x, 1.0f, 1e-3f);
    EXPECT_NEAR(st.y, 1.0f, 1e-3f);
}

TEST(InverseBilinear, DistortedQuadInteriorWithinUnit) {
    // SEGA Rally style thin distorted quad; an interior point maps to (s,t) in [0,1]^2
    glm::vec2 A(100,100), B(105,102), C(108,200), D(102,198);
    auto st = inverseBilinear(glm::vec2(104, 150), A, B, C, D);
    EXPECT_GE(st.x, 0.0f); EXPECT_LE(st.x, 1.0f);
    EXPECT_GE(st.y, 0.0f); EXPECT_LE(st.y, 1.0f);
}

// Degenerate quad with v2==v3 (Saturn distorted-sprite triangle encoding).
// Forward mapping: P = mix(mix(v0, v1, s), v2, t) collapses bottom row at v2.
// Inverse must pick the strictly interior root, NOT the collapsed t=1 root,
// otherwise s falls back to 0 and texture content shifts.
//
// Real cmd from yabause user report (2026-05-12):
//   v0=(1610,343), v1=(1604,311), v2=v3=(1598,317).
// Centroid of triangle = (1604, 323.667). Solving forward equation gives
// (s, t) = (0.5, 1/3).
TEST(InverseBilinear, DegenerateQuadV2EqualsV3CWWindingCentroid) {
    glm::vec2 A(1610.f, 343.f);
    glm::vec2 B(1604.f, 311.f);
    glm::vec2 C(1598.f, 317.f);
    glm::vec2 D(1598.f, 317.f);  // v3 == v2
    glm::vec2 P(1604.f, 323.6667f);  // centroid of triangle ABC
    auto st = inverseBilinear(P, A, B, C, D);
    EXPECT_NEAR(st.x, 0.5f,       1e-2f);
    EXPECT_NEAR(st.y, 1.0f / 3.0f, 1e-2f);
}

// Same shape, mirrored to give CCW winding. The two roots t0/t1 swap order
// in the quadratic solver, so the tie-break must handle both windings.
TEST(InverseBilinear, DegenerateQuadV2EqualsV3CCWWindingCentroid) {
    // Mirror across y axis to flip winding from CW to CCW.
    glm::vec2 A(-1610.f, 343.f);
    glm::vec2 B(-1604.f, 311.f);
    glm::vec2 C(-1598.f, 317.f);
    glm::vec2 D(-1598.f, 317.f);
    glm::vec2 P(-1604.f, 323.6667f);
    auto st = inverseBilinear(P, A, B, C, D);
    EXPECT_NEAR(st.x, 0.5f,       1e-2f);
    EXPECT_NEAR(st.y, 1.0f / 3.0f, 1e-2f);
}

// Midpoint of v1-v2 edge: (s, t) = (1, 0.5) per forward bilinear math.
TEST(InverseBilinear, DegenerateQuadV2EqualsV3MidpointOfV1V2Edge) {
    glm::vec2 A(1610.f, 343.f);
    glm::vec2 B(1604.f, 311.f);
    glm::vec2 C(1598.f, 317.f);
    glm::vec2 D(1598.f, 317.f);
    glm::vec2 P = 0.5f * B + 0.5f * C;  // = (1601, 314)
    auto st = inverseBilinear(P, A, B, C, D);
    EXPECT_NEAR(st.x, 1.0f, 1e-2f);
    EXPECT_NEAR(st.y, 0.5f, 1e-2f);
}

// Midpoint of v0-v2 edge: (s, t) = (0, 0.5) per forward bilinear math.
TEST(InverseBilinear, DegenerateQuadV2EqualsV3MidpointOfV0V2Edge) {
    glm::vec2 A(1610.f, 343.f);
    glm::vec2 B(1604.f, 311.f);
    glm::vec2 C(1598.f, 317.f);
    glm::vec2 D(1598.f, 317.f);
    glm::vec2 P = 0.5f * A + 0.5f * C;  // = (1604, 330)
    auto st = inverseBilinear(P, A, B, C, D);
    EXPECT_NEAR(st.x, 0.0f, 1e-2f);
    EXPECT_NEAR(st.y, 0.5f, 1e-2f);
}

TEST(EncodeDistortedSprite, FieldsArePopulated) {
    std::array<glm::ivec2, 4> verts = {{
        glm::ivec2(10, 20), glm::ivec2(50, 25),
        glm::ivec2(55, 80), glm::ivec2(15, 70)
    }};
    auto cmd = encodeDistortedSprite(
        verts,
        /*srca*/ 0x1234,
        /*charSize*/ glm::uvec2(32, 24),
        /*color*/ 0x8000,
        /*pmod*/ 0x0028,  // cm=5 (16bpp RGB direct)
        /*localCoord*/ glm::ivec2(0, 0));
    EXPECT_EQ(cmd.cmdType, VDP1C_TYPE_DISTORTED_SPRITE);
    EXPECT_EQ(cmd.srca,    0x1234u);
    EXPECT_EQ(cmd.charSize, glm::uvec2(32, 24));
    EXPECT_EQ(cmd.pmod,    0x0028u);
    EXPECT_EQ(cmd.color,   0x8000u);
    EXPECT_EQ(cmd.bbox,    glm::ivec4(10, 20, 55, 80));
}

TEST(EncodeDistortedSprite, LocalCoordIsApplied) {
    std::array<glm::ivec2, 4> verts = {{
        glm::ivec2(0, 0), glm::ivec2(10, 0),
        glm::ivec2(10, 10), glm::ivec2(0, 10)
    }};
    auto cmd = encodeDistortedSprite(verts, 0, glm::uvec2(0), 0, 0, glm::ivec2(100, 200));
    EXPECT_EQ(cmd.v0, glm::ivec2(100, 200));
    EXPECT_EQ(cmd.v2, glm::ivec2(110, 210));
    EXPECT_EQ(cmd.bbox, glm::ivec4(100, 200, 110, 210));
}

// =============================================================================
// Thin-polygon classification (FLAG_THIN gate)
// =============================================================================

TEST(ThinPolygon, IsThinPolygonGeometricMatchesAxisAlignedThin) {
    // 100 x 1 axis-aligned rectangle: area=100, longest=100 -> thickness=1.
    Vdp1Cmd cmd{};
    cmd.cmdType = VDP1C_TYPE_POLYGON;
    cmd.v0 = glm::ivec2(0, 0);
    cmd.v1 = glm::ivec2(100, 0);
    cmd.v2 = glm::ivec2(100, 1);
    cmd.v3 = glm::ivec2(0, 1);
    EXPECT_TRUE(isThinPolygonGeometric(cmd, 1.0f));  // 1 <= 3
}

TEST(ThinPolygon, IsThinPolygonGeometricCatchesDiagonalThin) {
    // 100 px diagonal at 45 deg, 1 px perpendicular width. The bbox is ~71x71 so
    // the bbox-aspect axis cannot detect it, but the polygon area is only ~3% of
    // the bbox area, so the fill-ratio route (rule (b)) judges it THIN.
    Vdp1Cmd cmd{};
    cmd.cmdType = VDP1C_TYPE_DISTORTED_SPRITE;
    cmd.v0 = glm::ivec2(0,  0);
    cmd.v1 = glm::ivec2(70, 70);
    cmd.v2 = glm::ivec2(70 - 1, 70 + 1);
    cmd.v3 = glm::ivec2(0  - 1,  0 + 1);
    cmd.bbox = glm::ivec4(-1, 0, 70, 71);
    EXPECT_TRUE(isThinPolygonGeometric(cmd, 1.0f));
}

TEST(ThinPolygon, IsThinPolygonGeometricRejectsNormalSprite) {
    // 24x24 normal sprite: area=576, longest=24 -> thickness=24.
    Vdp1Cmd cmd{};
    cmd.cmdType = VDP1C_TYPE_DISTORTED_SPRITE;
    cmd.v0 = glm::ivec2(0,  0);
    cmd.v1 = glm::ivec2(24, 0);
    cmd.v2 = glm::ivec2(24, 24);
    cmd.v3 = glm::ivec2(0,  24);
    EXPECT_FALSE(isThinPolygonGeometric(cmd, 1.0f));  // 24 > 3
    EXPECT_FALSE(isThinPolygonGeometric(cmd, 4.0f));  // 24 > 9
}

TEST(ThinPolygon, IsThinPolygonGeometricHonorsScaleMaxThreshold) {
    // 50x6 rectangle: area=300, longest=50 -> thickness=6.
    //   scaleMax=4.0 -> thr=9: thin
    //   scaleMax=1.0 -> thr=3: not thin
    Vdp1Cmd cmd{};
    cmd.cmdType = VDP1C_TYPE_POLYGON;
    cmd.v0 = glm::ivec2(0, 0);
    cmd.v1 = glm::ivec2(50, 0);
    cmd.v2 = glm::ivec2(50, 6);
    cmd.v3 = glm::ivec2(0, 6);
    EXPECT_TRUE(isThinPolygonGeometric(cmd, 4.0f));
    EXPECT_FALSE(isThinPolygonGeometric(cmd, 1.0f));
}

TEST(ThinPolygon, IsThinPolygonGeometricCatchesTriangularSlivers) {
    // Real cmd 332 from JSONL: bbox 110x4, vertex layout is
    //   v0=(747,471) v1=(654,467) v2=(637,471) v3=(739,471)
    // = 3 verts on bottom edge (y=471) + 1 vertex offset upward (v1 at y=467).
    // At HD upscale (scaleMax=4 -> widthThr=9) bbox short side 4 <= 9 and aspect
    // 110/4=27.5 >= 4, so THIN. At Saturn 1x (scaleMax=1 -> widthThr=3)
    // bbox short side 4 > 3, so there is no risk of gap rows: NOT thin.
    Vdp1Cmd cmd{};
    cmd.cmdType = VDP1C_TYPE_DISTORTED_SPRITE;
    cmd.v0 = glm::ivec2(747, 471);
    cmd.v1 = glm::ivec2(654, 467);
    cmd.v2 = glm::ivec2(637, 471);
    cmd.v3 = glm::ivec2(739, 471);
    EXPECT_TRUE(isThinPolygonGeometric(cmd, 4.0f));   // HD: bbox short side 4 <= 9
    EXPECT_FALSE(isThinPolygonGeometric(cmd, 1.0f));  // 1x: bbox short side 4 > 3
}


TEST(ThinPolygon, ResetStatePreservesPersistentState) {
    // scaleMax / localCoord / systemClip / userClip are VDP1 persistent
    // state. resetState at the start of a frame (via beginFrame()) does not
    // reset them; the previous frame's values are kept until the game re-sends
    // the LocalCoord / SystemClip / UserClip cmds (matches Saturn HW and the
    // graphics path).
    Vdp1State s;
    s.scaleMax   = 3.77f;
    s.localCoord = glm::ivec2(176, 112);
    s.systemClip = glm::ivec2(319, 223);
    s.userClip   = glm::ivec4(10, 20, 300, 200);
    resetState(s);
    EXPECT_FLOAT_EQ(s.scaleMax, 3.77f);
    EXPECT_EQ(s.localCoord, glm::ivec2(176, 112));
    EXPECT_EQ(s.systemClip, glm::ivec2(319, 223));
    EXPECT_EQ(s.userClip,   glm::ivec4(10, 20, 300, 200));
}

// ---------------------------------------------------------------------------
// Texture row stepping (hardware line -> texture row).
//
// Oracle: sequential error-term stepping copied from the reference software
// renderer (ss/vdp1_common.h VileTex::Setup and ss/vdp1_sprite.cpp, which
// calls PreStep once per line before drawing it). VDP1 draws dmax + 1 lines;
// line k reads the texture row returned by the k-th PreStep.
// ---------------------------------------------------------------------------
namespace {
std::vector<int> steppedRows(int length, int tstart, int tend) {
    const int dt = tend - tstart;
    const int absDt = std::abs(dt);
    int t = tstart;
    const int tinc = (dt >= 0) ? 1 : -1;
    int64_t errorInc, errorAdj, error;
    if (length <= absDt) {
        errorInc = (int64_t)(absDt + 1) * 2;
        errorAdj = (int64_t)length * 2;
        error = absDt + 1 - ((int64_t)length * 2 + ((dt < 0) ? 1 : 0));
    } else {
        errorInc = (int64_t)absDt * 2;
        errorAdj = (int64_t)(length - 1) * 2;
        error = length - ((int64_t)length * 2 - ((dt < 0) ? 1 : 0));
    }
    std::vector<int> rows;
    rows.reserve(length);
    for (int k = 0; k < length; ++k) {
        // PreStep
        while (error >= 0) {
            t += tinc;
            error -= errorAdj;
        }
        error += errorInc;
        rows.push_back(t);
    }
    return rows;
}
}  // namespace

TEST(Vdp1TexRow, MatchesSteppedAllSizes) {
    for (int h = 1; h <= 128; ++h)
        for (int n = 1; n <= 128; ++n)
            for (int rev = 0; rev < 2; ++rev) {
                int t0 = rev ? h - 1 : 0, t1 = rev ? 0 : h - 1;
                auto ref = steppedRows(n, t0, t1);
                for (int k = 0; k < n; ++k)
                    ASSERT_EQ(vdp1TexRow(k, n, t0, t1), ref[k]) << h << " " << n << " " << k;
            }
}
TEST(Vdp1TexRow, OneLineTakesMiddleRow) { EXPECT_EQ(vdp1TexRow(0, 1, 0, 15), 8); }
TEST(Vdp1TexRow, SameCountIsIdentity) {
    for (int k = 0; k < 16; ++k) EXPECT_EQ(vdp1TexRow(k, 16, 0, 15), k);
}
TEST(Vdp1CmdDmax, Chebyshev) { EXPECT_EQ(vdp1CmdDmax({0,0},{10,0},{10,3},{0,7}), 7); }
TEST(Vdp1CmdDmax, UsesBothSideEdgesAndBothAxes) {
    // B->C x-extent dominates.
    EXPECT_EQ(vdp1CmdDmax({0,0},{10,0},{-2,4},{0,1}), 12);
    // Zero-length side edges (A == D, B == C): a single line.
    EXPECT_EQ(vdp1CmdDmax({3,5},{40,9},{40,9},{3,5}), 0);
}

// A sprite whose two side edges both have length 0 (A == D and B == C) has
// zero area but the hardware still draws it as one line (dmax = 0). The
// generic zero-area guard would leave it undrawn when the line is not
// horizontal, so the encoder must route it through the FLAG_THIN supercover
// path.
TEST(ZeroAreaSprite, DiagonalLineSpriteGetsThinFlag) {
    Vdp1Cmd cmd = encodeDistortedSprite(
        {{ivec2(10, 10), ivec2(50, 30), ivec2(50, 30), ivec2(10, 10)}},
        0u, glm::uvec2(16, 8), 0u, 0u, ivec2(0, 0));
    EXPECT_NE(cmd.flags & VDP1C_FLAG_THIN, 0u);
}
TEST(ZeroAreaSprite, VerticalLineSpriteGetsThinFlag) {
    Vdp1Cmd cmd = encodeDistortedSprite(
        {{ivec2(10, 10), ivec2(10, 30), ivec2(10, 30), ivec2(10, 10)}},
        0u, glm::uvec2(16, 8), 0u, 0u, ivec2(0, 0));
    EXPECT_NE(cmd.flags & VDP1C_FLAG_THIN, 0u);
}
TEST(ZeroAreaSprite, HorizontalLineSpriteKeepsAxisAlignedPath) {
    // A horizontal one-line sprite is already covered by the shaders'
    // axis-aligned one-cell extension; leave it on that path.
    Vdp1Cmd cmd = encodeDistortedSprite(
        {{ivec2(10, 10), ivec2(50, 10), ivec2(50, 10), ivec2(10, 10)}},
        0u, glm::uvec2(16, 8), 0u, 0u, ivec2(0, 0));
    EXPECT_EQ(cmd.flags & VDP1C_FLAG_THIN, 0u);
}

// ---------------------------------------------------------------------------
// dmax from raw Saturn vertices (fix round 1).
// ---------------------------------------------------------------------------
TEST(Vdp1CmdDmax, ThirteenBitWrap) {
    // Edge differences are sign-extended from 13 bits (coordinates are 13-bit
    // on the hardware), then dmax is masked to 12 bits.
    EXPECT_EQ(vdp1CmdDmax({0, 0}, {0, 0}, {0, 0}, {0, 0x1FFF}), 1);    // -1
    EXPECT_EQ(vdp1CmdDmax({0, 0}, {0, 0}, {0, 0}, {0x1000, 0}), 0);    // 4096 & 0xFFF
    EXPECT_EQ(vdp1CmdDmax({-5, 0}, {0, 0}, {0, 0}, {4100, 0}), 4087);  // 4105 -> -4087
}
TEST(Vdp1SpriteSteps, PacksDmaxAndLengthAB) {
    std::array<ivec2, 4> q = {{ivec2(0, 0), ivec2(10, 3), ivec2(12, 9), ivec2(1, 7)}};
    uint32_t s = packSpriteSteps(q);
    EXPECT_EQ(s & 0xFFFFu, 7u);    // max(|D-A|, |C-B|) = max(7, 6)
    EXPECT_EQ(s >> 16, 10u);       // |B-A| = max(10, 3)
}
TEST(Vdp1SpriteSteps, NormalSpriteQuadFollowsHardware) {
    // Normal Sprite: B = A + (w - 1, 0), C = A + (w - 1, h - 1).
    auto q = normalSpriteQuad(-7, -3, 24, 16);
    EXPECT_EQ(q[0], ivec2(-7, -3));
    EXPECT_EQ(q[1], ivec2(16, -3));
    EXPECT_EQ(q[2], ivec2(16, 12));
    EXPECT_EQ(q[3], ivec2(-7, 12));
    EXPECT_EQ(packSpriteSteps(q) & 0xFFFFu, 15u);
}
TEST(Vdp1SpriteSteps, RegressionScale125NormalSpriteKeepsAllRows) {
    // Round 0 derived dmax from the scaled, truncated fb vertices:
    // y = -3 .. 12 at scale 1.25 -> fb -3 .. 15, round(18 / 1.25) = 14, so
    // one of the 16 rows was dropped. The raw-vertex value is h - 1 = 15.
    const float scale = 1.25f;
    const int y0 = -3, h = 16;
    const int fb0 = (int)((float)y0 * scale);
    const int fb1 = (int)((float)(y0 + h - 1) * scale);
    EXPECT_EQ((int)std::lround((fb1 - fb0) / scale), 14);  // the old bug
    auto q = normalSpriteQuad(0, y0, 8, h);
    EXPECT_EQ(packSpriteSteps(q) & 0xFFFFu, 15u);
    // Every one of the 16 rows is drawn once (identity stepping).
    for (int k = 0; k < h; ++k) EXPECT_EQ(vdp1TexRow(k, 16, 0, h - 1), k);
}

// ---------------------------------------------------------------------------
// One-line sprite column (fix round 1): U is the per-dot stepping along A->B.
// ---------------------------------------------------------------------------
namespace {
void checkOneLineU(ivec2 satA, ivec2 satB, float scale, int w, bool flipH) {
    const vec2 s(scale, scale);
    const ivec2 a((int)(satA.x * scale), (int)(satA.y * scale));
    const ivec2 b((int)(satB.x * scale), (int)(satB.y * scale));
    const int len = (int)(packSpriteSteps({{satA, satB, satB, satA}}) >> 16);
    int prev = flipH ? w : -1;
    int first = -1, last = -1;
    const int steps = 400;
    for (int i = 0; i <= steps; ++i) {
        // Pixel centres along the fb segment (through the cell centres).
        vec2 p = glm::mix(vec2(a), vec2(b), (float)i / steps) + 0.5f * s;
        int u = vdp1OneLineTexU(p, a, b, s, len, w, flipH);
        ASSERT_GE(u, 0);
        ASSERT_LT(u, w);
        if (flipH) ASSERT_LE(u, prev) << i; else ASSERT_GE(u, prev) << i;
        prev = u;
        if (i == 0) first = u;
        last = u;
    }
    EXPECT_EQ(first, flipH ? w - 1 : 0);
    EXPECT_EQ(last, flipH ? 0 : w - 1);
}
}  // namespace
TEST(OneLineSprite, DiagonalColumnIsMonotonic) {
    checkOneLineU(ivec2(10, 10), ivec2(50, 30), 4.0f, 16, false);
    checkOneLineU(ivec2(10, 10), ivec2(50, 30), 4.0f, 16, true);
    checkOneLineU(ivec2(-20, 5), ivec2(30, -40), 1.25f, 64, false);
}
TEST(OneLineSprite, VerticalColumnIsMonotonic) {
    checkOneLineU(ivec2(10, 10), ivec2(10, 30), 4.0f, 8, false);
    checkOneLineU(ivec2(10, 10), ivec2(10, 30), 1.0f, 32, true);
}

// ---------------------------------------------------------------------------
// GPU tessellation path (vdp1texrow.h): the line parameter the graphics quad
// interpolates must round to the hardware line of each pixel.
// ---------------------------------------------------------------------------
#include "../../vdp1texrow.h"

namespace {
// Decode the packed q word exactly as the fragment shaders do.
void decodeTessPack(float q, int& dmax, int& adt, int& rev) {
    const int p = static_cast<int>(-q + 0.5f) - 1;
    dmax = p & 0xFFF;
    adt = (p >> 12) & 0xFF;
    rev = (p >> 20) & 1;
}
}  // namespace

TEST(TessLineParams, ExpandedQuadPixelCentresHitEveryLine) {
    // Normal sprite 16 rows: hardware vertices are inclusive, the drawn quad
    // spans 16 whole cells.
    const int raw[8] = {0, 0, 15, 0, 15, 15, 0, 15};
    int dmax = -1; float k0 = 0, k1 = 0;
    vdp1_tess_line_params(raw, 1, &dmax, &k0, &k1);
    EXPECT_EQ(dmax, 15);
    for (int i = 0; i < 16; ++i) {
        const float u = (i + 0.5f) / 16.0f;  // pixel centre of line i
        const float z = k0 + u * (k1 - k0);
        const int k = static_cast<int>(std::nearbyint(z));
        EXPECT_EQ(k, i);
        EXPECT_EQ(vdp1_tex_row(k, dmax + 1, 0, 15), i);
    }
}

TEST(TessLineParams, RawQuadSamplesCellOrigin) {
    // Distorted sprite drawn with raw vertices: the quad spans dmax cells and
    // a pixel centre is half a cell past the cell origin the hardware uses.
    const int raw[8] = {0, 0, 31, 0, 31, 15, 0, 15};
    int dmax = -1; float k0 = 0, k1 = 0;
    vdp1_tess_line_params(raw, 0, &dmax, &k0, &k1);
    EXPECT_EQ(dmax, 15);
    EXPECT_FLOAT_EQ(k0, -0.5f);
    EXPECT_FLOAT_EQ(k1, 14.5f);
    for (int i = 0; i < 15; ++i) {
        const float u = (i + 0.5f) / 15.0f;
        EXPECT_EQ(static_cast<int>(std::nearbyint(k0 + u * (k1 - k0))), i);
    }
}

TEST(TessLineParams, ParallelogramMatchesHalfCellShift) {
    // For an affine (parallelogram) quad the shifted linear parameter equals
    // dmax * u(P - (0.5, 0.5)) exactly, whatever the orientation.
    const int quads[][8] = {
        {0, 0, 20, 10, 12, 30, -8, 20},    // rotated
        {40, 5, 10, 5, 4, 25, 34, 25},     // mirrored in x (B left of A)
        {0, 30, 25, 30, 35, 0, 10, 0},     // upside down
        {5, 5, 30, 12, 30, 20, 5, 13},     // sheared
    };
    for (const auto& v : quads) {
        int dmax = -1; float k0 = 0, k1 = 0;
        vdp1_tess_line_params(v, 0, &dmax, &k0, &k1);
        ASSERT_GT(dmax, 0);
        const float ux = float(v[6] - v[0]), uy = float(v[7] - v[1]);  // A->D
        const float vx = float(v[2] - v[0]), vy = float(v[3] - v[1]);  // A->B
        const float det = ux * vy - uy * vx;
        auto uAt = [&](float px, float py) {
            const float qx = px - v[0], qy = py - v[1];
            return (qx * vy - qy * vx) / det;
        };
        const float c = -k0;
        if (std::fabs(c) >= 1.0f) continue;  // clamped case
        for (float py = -5.0f; py < 40.0f; py += 3.7f) {
            for (float px = -10.0f; px < 45.0f; px += 4.3f) {
                const float zLinear = k0 + uAt(px, py) * (k1 - k0);
                const float zShift = dmax * uAt(px - 0.5f, py - 0.5f);
                EXPECT_NEAR(zLinear, zShift, 1e-3f);
            }
        }
    }
}

TEST(TessLineParams, OneLineSpriteHasNoLines) {
    const int raw[8] = {3, 5, 40, 9, 40, 9, 3, 5};  // A == D, B == C
    int dmax = -1; float k0 = 1, k1 = 1;
    vdp1_tess_line_params(raw, 1, &dmax, &k0, &k1);
    EXPECT_EQ(dmax, 0);
    // Every fragment clamps to line 0, which takes the middle row.
    EXPECT_EQ(vdp1_tex_row(0, 1, 0, 15), 8);
}

TEST(TessPack, RoundTripsAllFields) {
    for (int dmax : {0, 1, 15, 255, 4095}) {
        for (int h : {1, 2, 16, 255}) {
            for (int flip = 0; flip < 2; ++flip) {
                const float q = vdp1_tess_pack(dmax, h, flip);
                ASSERT_LT(q, 0.0f);
                ASSERT_EQ(q, std::floor(q));  // exact integer in a float
                int d, a, r;
                decodeTessPack(q, d, a, r);
                EXPECT_EQ(d, dmax);
                EXPECT_EQ(a, h - 1);
                EXPECT_EQ(r, flip);
            }
        }
    }
}

TEST(TessPack, SharedRowMatchesComputeRow) {
    // The graphics path and the compute path call the same closed form.
    for (int n = 1; n <= 64; ++n)
        for (int k = 0; k < n; ++k)
            ASSERT_EQ(vdp1_tex_row(k, n, 7, 7 + 20), vdp1c::vdp1TexRow(k, n, 7, 27));
}
