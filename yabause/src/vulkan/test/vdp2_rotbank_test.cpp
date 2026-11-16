// Copyright 2026 devMiyax(smiyaxdev@gmail.com)
// TDD test for the rotation scroll VRAM bank access rule (RAMCTL RDBS). ASCII-only.
#include "gtest/gtest.h"
#include "vdp2_rotbank.h"

// RAMCTL 0x13B4: A0=0 A1=coef B0=char B1=name, both split
TEST(Vdp2RotBank, TwnE006) {
  u32 nt, cg; Vdp2RotBankMasks(0x13B4, 0x0010, 0, &nt, &cg);
  EXPECT_EQ(nt, 0x8u); EXPECT_EQ(cg, 0x4u);
}

// VRAMD=0: A1 follows A0 (=char)
TEST(Vdp2RotBank, UnsplitUsesLowerField) {
  u32 nt, cg; Vdp2RotBankMasks(0x0007, 0x0010, 0, &nt, &cg);
  EXPECT_EQ(cg, 0x3u); EXPECT_EQ(nt, 0x0u);
}

// BGON bit5: RBG0 loses B0/B1, RBG1 is fixed to B1 names / B0 chars
TEST(Vdp2RotBank, Rbg1Active) {
  u32 nt, cg; Vdp2RotBankMasks(0x03FF, 0x0030, 0, &nt, &cg);
  EXPECT_EQ(nt & 0xCu, 0u); EXPECT_EQ(cg & 0xCu, 0u);
  Vdp2RotBankMasks(0x03FF, 0x0030, 1, &nt, &cg);
  EXPECT_EQ(nt, 0x8u); EXPECT_EQ(cg, 0x4u);
}
