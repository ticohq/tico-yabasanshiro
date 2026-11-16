/* Tests for the float effect DSP (sound_hq/dsp_float.h) against the chip's
 * DSP (scspdsp.c, compiled in).
 *
 * The harness runs both side by side the way generate_sample() in scsp.cpp
 * does: the chip runs its program on the sends, then the ring offset steps;
 * the float DSP runs the same sample from the chip's program, parameters and
 * ring offset, on the same sends, sharing the same sound RAM (the chip
 * writes its ring there, the float DSP only reads it).
 *
 * - Exact cases: programs whose arithmetic is exact in both (products by -1,
 *   NOFL words, integer inputs) must give the chip's EFREG bit for bit. They
 *   cover the input selection, the shifter's saturation and SHFT1 wrap, the
 *   accumulator's wrap, TEMP rotation, the ring pipeline, NOFL, TABLE,
 *   NXADR, ADREB/ADRL (including negative inputs) and YRL.
 * - The shadow memory: a CPU write (discard()), a format mismatch and a
 *   program change all make the float DSP read what the chip reads.
 * - Seeding: a float DSP started on a running chip continues it.
 * - Effect scenarios (as the scenarios of the Azel port's DSP test): an
 *   echo / lowpass / modulated-tap program over noise, sines, silence over a
 *   filled ring, and full-scale squares. The float DSP's EFREG must stay
 *   >= 50 dB below the effect signal away from the chip's.
 * - The renderer feeds the float DSP with its float sends. */
#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "dsp_float.h"
#include "ram_watch.h"
#include "scsp_hq.h"
#include "scspdsp.h"

/* scspdsp.c's globals and imports, as yabause.c / the core provide them */
ScspDsp scsp_dsp;
extern "C" FILE* fopen_utf8(const char* name, const char* mode) { return fopen(name, mode); }

namespace {

const double kPi = 3.14159265358979323846;

/* one microprogram word, through the chip's own bit-field layout */
struct Insn {
  ScspDspInstruction u;
  Insn() { u.all = 0; }
  Insn& ira(int v) { u.part.ira = v; return *this; }
  Insn& iwt(int a) { u.part.iwt = 1; u.part.iwa = a; return *this; }
  Insn& xsel(int v) { u.part.xsel = v; return *this; }
  Insn& ysel(int v) { u.part.ysel = v; return *this; }
  Insn& coef(int c) { u.part.ysel = 1; u.part.coef = c; return *this; }
  Insn& tra(int v) { u.part.tra = v; return *this; }
  Insn& twt(int a) { u.part.twt = 1; u.part.twa = a; return *this; }
  Insn& bsel() { u.part.bsel = 1; return *this; }
  Insn& negb() { u.part.negb = 1; return *this; }
  Insn& zero() { u.part.zero = 1; return *this; }
  Insn& yrl() { u.part.yrl = 1; return *this; }
  Insn& shift(int s) { u.part.shift0 = s & 1; u.part.shift1 = (s >> 1) & 1; return *this; }
  Insn& ewt(int a) { u.part.ewt = 1; u.part.ewa = a; return *this; }
  Insn& mrd(int masa) { u.part.mrd = 1; u.part.masa = masa; return *this; }
  Insn& mwt(int masa) { u.part.mwt = 1; u.part.masa = masa; return *this; }
  Insn& nofl() { u.part.nofl = 1; return *this; }
  Insn& table() { u.part.table = 1; return *this; }
  Insn& nxadr() { u.part.nxadr = 1; return *this; }
  Insn& adreb() { u.part.adreb = 1; return *this; }
  Insn& adrl() { u.part.adrl = 1; return *this; }
  Insn& frcl() { u.part.frcl = 1; return *this; }
};

/* a coefficient as the chip stores it (13-bit two's complement, -4096..4095) */
uint16_t coef13(int c) { return (uint16_t)(c & 0x1FFF); }

class Bench {
public:
  explicit Bench(int rbp = 0x30, int rbl = 0) : ram(0x80000, 0), rbp(rbp), rbl(rbl) {
    std::memset(&scsp_dsp, 0, sizeof scsp_dsp);
    scsp_dsp.updated = 1;
  }

  void load(const std::vector<Insn>& prog, const std::vector<int>& coefs,
            const std::vector<uint16_t>& madrs) {
    for (int i = 0; i < 128; i++) scsp_dsp.mpro[i] = i < (int)prog.size() ? prog[i].u.all : 0;
    for (int i = 0; i < 64; i++) scsp_dsp.coef[i] = i < (int)coefs.size() ? coef13(coefs[i]) : 0;
    for (int i = 0; i < 32; i++) scsp_dsp.madrs[i] = i < (int)madrs.size() ? madrs[i] : 0;
    scsp_dsp.updated = 1;
  }

  /* fill the ring with the driver's "cleared" value (float format zero) */
  void clear_ring() {
    uint16_t* w = (uint16_t*)&ram[0];
    for (uint32_t a = 0; a < (0x2000u << rbl); a++) w[((uint32_t)rbp << 12) + a] = 0x6000;
  }

  /* one sample of the chip, as generate_sample(); mixs on the 20-bit scale */
  void chip(const int32_t mixs[16], const int16_t exts[2]) {
    scsp_dsp.rbp = rbp;
    scsp_dsp.rbl = rbl;
    scsp_dsp.exts[0] = exts[0];
    scsp_dsp.exts[1] = exts[1];
    for (int i = 0; i < 16; i++) scsp_dsp.mixs[i] = mixs[i];
    mdec_used = scsp_dsp.mdec_ct;
    if (scsp_dsp.updated) {
      int i;
      for (i = 127; i >= 0; --i)
        if (scsp_dsp.mpro[i] != 0) break;
      scsp_dsp.last_step = i + 1;
      scsp_dsp.updated = 0;
    }
    for (int i = 0; i < scsp_dsp.last_step; i++) ScspDspExec(&scsp_dsp, i, &ram[0]);
    if (!scsp_dsp.mdec_ct) scsp_dsp.mdec_ct = (0x2000 << rbl);
    scsp_dsp.mdec_ct--;
  }

  /* as scsp_hq_fill_dsp_view() */
  ScspHqDspView view() const {
    ScspHqDspView v;
    v.mpro = (const uint64_t*)scsp_dsp.mpro;
    v.coef = (const uint16_t*)scsp_dsp.coef;
    v.madrs = (const uint16_t*)scsp_dsp.madrs;
    v.mdec_ct = mdec_used;
    v.rbp = rbp;
    v.rbl = rbl;
    v.last_step = scsp_dsp.last_step;
    v.temp = (const int32_t*)scsp_dsp.temp;
    v.mems = (const int32_t*)scsp_dsp.mems;
    v.efreg = (const int16_t*)scsp_dsp.efreg;
    v.shift_reg = scsp_dsp.shift_reg;
    v.inputs = scsp_dsp.inputs;
    v.y_reg = scsp_dsp.y_reg;
    v.frc_reg = scsp_dsp.frc_reg;
    v.adrs_reg = scsp_dsp.adrs_reg;
    v.read_value = scsp_dsp.read_value;
    v.read_pending = scsp_dsp.read_pending;
    v.write_pending = scsp_dsp.write_pending;
    v.io_addr = scsp_dsp.io_addr;
    return v;
  }

  /* the chip, then the float DSP, on the same sends */
  const float* both(ScspHqFloatDsp& f, const int32_t mixs[16], const int16_t exts[2]) {
    chip(mixs, exts);
    float fm[16], fe[2];
    for (int i = 0; i < 16; i++) fm[i] = (float)mixs[i] * (1.0f / 524288);
    fe[0] = exts[0] * (1.0f / 32768);
    fe[1] = exts[1] * (1.0f / 32768);
    return f.run_sample(view(), &ram[0], fm, fe);
  }

  uint16_t& word(uint32_t a) { return ((uint16_t*)&ram[0])[a & 0x3FFFF]; }

  std::vector<uint8_t> ram;
  int rbp, rbl;
  uint32_t mdec_used = 0;
};

/* the chip's EFREG from a float one: the shifter output >> 8, floored */
int efreg_floor(float v) { return (int)std::floor((double)v * 32768.0); }

/* ---------------------------------------------------------------- exact */

/* Saturation, SHFT1 wrap, accumulator wrap, TEMP rotation, IRA sources. */
TEST(FloatDsp, ShifterAndAccumulatorMatchChip) {
  const int M1 = -4096;  // Y = -1: exact products
  std::vector<Insn> p;
  p.push_back(Insn().ira(0x20).xsel(1).coef(0).zero().ewt(7));  // acc = -in; EFREG7 = the last step's acc
  p.push_back(Insn().ira(0x20).xsel(1).coef(0).bsel());        // -2in
  p.push_back(Insn().ira(0x20).xsel(1).coef(0).bsel());        // -3in
  p.push_back(Insn().ira(0x20).xsel(1).coef(0).bsel().ewt(0)); // EFREG0 = sat(-3in); -4in
  p.push_back(Insn().ira(0x20).xsel(1).coef(0).bsel().ewt(1).shift(1));  // x2, saturate
  p.push_back(Insn().ira(0x20).xsel(1).coef(0).bsel().ewt(2).shift(2));  // x2, wrap
  p.push_back(Insn().ira(0x30).xsel(1).coef(0).bsel().ewt(3).shift(3).twt(5));  // wrap
  p.push_back(Insn().ira(0x31).xsel(0).tra(6).coef(0).negb().ewt(4));  // -TEMP(prev sample) - TEMP
  p.push_back(Insn().ira(0x3A).xsel(1).coef(0).zero().ewt(5));         // IRA 0x3A keeps INPUTS
  p.push_back(Insn().ewt(6));
  // the last word: its accumulator reaches the next sample's step 0 only if no
  // step runs after it (the chip stops after the last non-zero word)
  p.push_back(Insn().ira(0x31).xsel(1).coef(0).zero());
  Bench b;
  b.load(p, {M1}, {});
  ScspHqFloatDsp f;
  int mism = 0, nonzero = 0;
  for (int n = 0; n < 6000; n++) {
    int32_t mixs[16] = {0};
    // a ramp over the whole 20-bit range and beyond (MIXS wraps)
    mixs[0] = (int32_t)(((n * 997) % 1600000) - 800000);
    const int16_t exts[2] = {(int16_t)((n * 37) % 65536 - 32768), (int16_t)(32767 - (n * 53) % 65536)};
    const float* e = b.both(f, mixs, exts);
    if (n == 0) continue;  // seeded from the chip
    for (int i = 0; i < 8; i++) {
      if (efreg_floor(e[i]) != scsp_dsp.efreg[i]) mism++;
      if (scsp_dsp.efreg[i] != 0) nonzero++;
    }
  }
  EXPECT_EQ(mism, 0);
  EXPECT_GT(nonzero, 30000);
  EXPECT_EQ(f.seed_count(), 1u);
}

/* The ring: NOFL delay, TABLE, NXADR, ADREB with ADRL from positive and
 * negative inputs, YRL; mdec rotation over two ring lengths. */
TEST(FloatDsp, RingAddressingMatchesChip) {
  const int M1 = -4096;
  std::vector<Insn> p;
  p.push_back(Insn().ira(0x20).xsel(1).coef(0).zero().yrl());  // acc = -in
  p.push_back(Insn().mwt(0).nofl());                           // line[mdec] = -in (NOFL)
  p.push_back(Insn().mrd(1).nofl());                           // line[mdec + D]
  p.push_back(Insn().ira(0x21).adrl());                        // ADRS = in1 >> 16
  p.push_back(Insn().iwt(0).mrd(2).nofl().adreb().nxadr());    // line[mdec + E + ADRS + 1]
  p.push_back(Insn().mrd(3).nofl().table());                   // RBP + F, no mdec
  p.push_back(Insn().iwt(1));
  p.push_back(Insn().iwt(2));
  p.push_back(Insn().ira(0).xsel(1).coef(0).zero());
  p.push_back(Insn().ira(1).xsel(1).coef(0).zero().ewt(0));
  p.push_back(Insn().ira(2).xsel(1).coef(0).zero().ewt(1));
  p.push_back(Insn().ira(0x20).xsel(1).ysel(2).zero().ewt(2));  // in * Y_REG (exact: in is a multiple of 2^11)
  p.push_back(Insn().ewt(3));
  for (int rbl = 0; rbl < 2; rbl++) {
    Bench b(0x38, rbl);
    b.load(p, {M1}, {700, 1234, 3000, 3000});
    // TABLE source: a fixed pattern
    b.word(((uint32_t)0x38 << 12) + 3000) = 0x1234;
    ScspHqFloatDsp f;
    int mism = 0, nonzero = 0;
    const int N = (0x2000 << rbl) * 2 + 500;
    for (int n = 0; n < N; n++) {
      int32_t mixs[16] = {0};
      // multiples of 2^11 on the 24-bit scale (16 bits exact for NOFL, 13 for Y)
      mixs[0] = (int32_t)((((n * 7919) % 511) - 255) * 128);
      mixs[1] = (int32_t)((((n * 31) % 4096) - 2048) * 256);  // ADRS sweeps -128..127
      const int16_t exts[2] = {0, 0};
      const float* e = b.both(f, mixs, exts);
      if (n == 0) continue;
      for (int i = 0; i < 4; i++) {
        if (efreg_floor(e[i]) != scsp_dsp.efreg[i]) mism++;
        if (scsp_dsp.efreg[i] != 0) nonzero++;
      }
    }
    EXPECT_EQ(mism, 0) << "rbl " << rbl;
    EXPECT_GT(nonzero, N) << "rbl " << rbl;
  }
}

/* The 16-bit float format of the ring: the chip keeps about 11 bits, the
 * float DSP keeps the value. */
TEST(FloatDsp, FloatRingKeepsPrecision) {
  const int M1 = -4096;
  std::vector<Insn> p;
  p.push_back(Insn().ira(0x20).xsel(1).coef(0).zero());
  p.push_back(Insn().mwt(0));      // float format
  p.push_back(Insn().mrd(1));
  p.push_back(Insn());
  p.push_back(Insn().iwt(0));
  p.push_back(Insn().ira(0).xsel(1).coef(0).zero());
  p.push_back(Insn().ewt(0));
  Bench b;
  b.load(p, {M1}, {0, 100});
  b.clear_ring();
  ScspHqFloatDsp f;
  std::vector<int32_t> in;
  double err_chip = 0, err_float = 0, sig = 0;
  for (int n = 0; n < 4000; n++) {
    int32_t mixs[16] = {0};
    mixs[0] = (int32_t)std::lrint(std::sin(n * 0.01) * 300000.0);
    in.push_back(mixs[0]);
    const int16_t exts[2] = {0, 0};
    const float* e = b.both(f, mixs, exts);
    if (n < 200) continue;
    // EFREG = the input of 100 samples ago (through -1 twice), >> 8 of 24 bits
    const double want = in[n - 100] * 16.0 / 256.0;
    sig += want * want;
    err_chip += (scsp_dsp.efreg[0] - want) * (scsp_dsp.efreg[0] - want);
    err_float += (e[0] * 32768.0 - want) * (e[0] * 32768.0 - want);
  }
  const double chip_db = 10 * std::log10(sig / err_chip);
  const double float_db = 10 * std::log10(sig / (err_float + 1e-30));
  printf("float ring: chip SNR %.1f dB, float SNR %.1f dB\n", chip_db, float_db);
  EXPECT_LT(chip_db, 80.0);
  EXPECT_GT(float_db, 120.0);
}

/* The float DSP runs the chip's own step count (last_step), which the chip
 * recounts only when it sees MPRO change (scsp_dsp.updated). Words written
 * to MPRO without that (the chip's byte write path) are not run by the chip,
 * so they must not be run by the float DSP either. */
std::vector<Insn> delay_program();

TEST(FloatDsp, StepCountIsTheChips) {
  std::vector<Insn> p;
  p.push_back(Insn().ira(0x20).xsel(1).coef(0).zero());
  p.push_back(Insn().ewt(0));
  Bench b;
  b.load(p, {-4096}, {});
  ScspHqFloatDsp f;
  int mism = 0, nonzero = 0;
  for (int n = 0; n < 3000; n++) {
    if (n == 1000) {
      // a word after the chip's last step, written without `updated`
      scsp_dsp.mpro[5] = Insn().ira(0x20).xsel(1).coef(0).zero().ewt(1).u.all;
      scsp_dsp.mpro[6] = Insn().ewt(2).u.all;
    }
    if (n == 2000) {
      // the same, on a chip that never counted its program (last_step 0)
      std::memset(&scsp_dsp.efreg, 0, sizeof scsp_dsp.efreg);
      scsp_dsp.last_step = 0;
    }
    int32_t mixs[16] = {0};
    mixs[0] = (int32_t)((n * 911) % 400000) - 200000;
    const int16_t exts[2] = {0, 0};
    const float* e = b.both(f, mixs, exts);
    for (int i = 0; i < 3; i++) {
      if (efreg_floor(e[i]) != scsp_dsp.efreg[i]) mism++;
      if (scsp_dsp.efreg[i] != 0) nonzero++;
    }
  }
  EXPECT_EQ(mism, 0);
  EXPECT_GT(nonzero, 900);
  EXPECT_EQ(scsp_dsp.efreg[2], 0);  // the chip never ran the new words
}

/* An EFREG the program does not write keeps the value the CPU wrote. */
TEST(FloatDsp, CpuEfregWritePassesThrough) {
  std::vector<Insn> p;
  p.push_back(Insn().ira(0x20).xsel(1).coef(0).zero());
  p.push_back(Insn().ewt(0));
  Bench b;
  b.load(p, {-4096}, {});
  ScspHqFloatDsp f;
  int mism = 0;
  for (int n = 0; n < 500; n++) {
    if (n == 100) scsp_dsp.efreg[5] = 1234;
    if (n == 300) scsp_dsp.efreg[15] = -77;
    int32_t mixs[16] = {0};
    mixs[0] = (int32_t)(n * 1000) - 250000;
    const int16_t exts[2] = {0, 0};
    const float* e = b.both(f, mixs, exts);
    for (int i = 0; i < 16; i++)
      if (efreg_floor(e[i]) != scsp_dsp.efreg[i]) mism++;
  }
  EXPECT_EQ(mism, 0);
  EXPECT_EQ(scsp_dsp.efreg[5], 1234);
}

/* The page arithmetic of the CPU write watch (ram_watch.h): a write marks
 * the 1 KB pages its bytes touch, including two for a write across a page
 * edge and across the end of RAM, and taking a page hands its 512 words to
 * discard() once. */
TEST(FloatDsp, RamWatchPages) {
  ScspHqRamWatch w;
  std::vector<std::pair<uint32_t, uint32_t> > got;
  auto take = [&]() {
    got.clear();
    w.take([&](uint32_t first, uint32_t count) { got.push_back(std::make_pair(first, count)); });
  };
  take();
  EXPECT_TRUE(got.empty());
  w.mark(0x3FE, 4);  // bytes 0x3FE-0x401: pages 0 and 1
  take();
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0], std::make_pair(0u, 512u));
  EXPECT_EQ(got[1], std::make_pair(512u, 512u));
  take();
  EXPECT_TRUE(got.empty());  // taken once
  w.mark(0x400, 2);        // page 1 only
  w.mark(0x7FC, 4);        // page 1 again
  w.mark(0x40000, 1);      // page 256 (64-bit word 4)
  w.mark(0x7FFFE, 4);      // across the end of RAM: pages 511 and 0
  take();
  ASSERT_EQ(got.size(), 4u);
  EXPECT_EQ(got[0].first, 0u);
  EXPECT_EQ(got[1].first, 512u);
  EXPECT_EQ(got[2].first, 256u * 512u);
  EXPECT_EQ(got[3].first, 511u * 512u);

  // with the float DSP: a CPU write marked at a page edge drops exactly the
  // words of the pages it touched
  Bench b;
  b.load(delay_program(), {-4096}, {0, 300});
  b.clear_ring();
  ScspHqFloatDsp f;
  for (int n = 0; n < 8300; n++) {  // every ring word written once
    int32_t mixs[16] = {0};
    mixs[0] = 0x10000;  // exact in the ring's float format
    const int16_t exts[2] = {0, 0};
    b.both(f, mixs, exts);
  }
  // the CPU clears byte 0x60000 + 0x3FE .. 0x401 of the ring (words 0x301FF,
  // 0x30200), and the whole page before it, as plain RAM writes
  for (uint32_t a = 0x30000; a < 0x30201; a++) b.word(a) = 0x6000;
  w.mark(0x60000, 0x3FE);
  w.mark(0x603FE, 4);
  w.take([&](uint32_t first, uint32_t count) { f.discard(first, count); });
  // the ring is read back through the chip's own read path: every cleared
  // word now reads 0 in both
  int mism = 0;
  for (int n = 0; n < 9000; n++) {
    int32_t mixs[16] = {0};
    const int16_t exts[2] = {0, 0};
    const float* e = b.both(f, mixs, exts);
    if (efreg_floor(e[0]) != scsp_dsp.efreg[0]) mism++;
  }
  EXPECT_EQ(mism, 0);
}

/* --------------------------------------------------------------- shadow */

std::vector<Insn> delay_program() {
  std::vector<Insn> p;
  p.push_back(Insn().ira(0x20).xsel(1).coef(0).zero());
  p.push_back(Insn().mwt(0));
  p.push_back(Insn().mrd(1));
  p.push_back(Insn());
  p.push_back(Insn().iwt(0));
  p.push_back(Insn().ira(0).xsel(1).coef(0).zero());
  p.push_back(Insn().ewt(0));
  return p;
}

/* The CPU clears the ring: with discard() the float DSP reads the cleared
 * words as the chip does; without it, it would keep playing its copy. */
TEST(FloatDsp, CpuWriteDiscardsShadow) {
  Bench b;
  b.load(delay_program(), {-4096}, {0, 300});
  b.clear_ring();
  ScspHqFloatDsp with, without;
  // `without` runs on its own copy of the RAM and chip state
  int mism_with = 0, stale_without = 0;
  for (int n = 0; n < 2000; n++) {
    int32_t mixs[16] = {0};
    if (n < 1000) mixs[0] = (int32_t)std::lrint(std::sin(n * 0.05) * 200000.0);
    const int16_t exts[2] = {0, 0};
    if (n == 1000) {
      // the driver clears the ring (word writes), as for a new effect
      for (uint32_t a = 0; a < 0x2000; a++) b.word(((uint32_t)b.rbp << 12) + a) = 0x6000;
      with.discard((uint32_t)b.rbp << 12, 0x2000);
    }
    b.chip(mixs, exts);
    float fm[16] = {0}, fe[2] = {0, 0};
    fm[0] = mixs[0] * (1.0f / 524288);
    const float* e1 = with.run_sample(b.view(), &b.ram[0], fm, fe);
    const float* e2 = without.run_sample(b.view(), &b.ram[0], fm, fe);
    if (n > 1000 && n < 1300) {
      if (scsp_dsp.efreg[0] != 0) mism_with += 1000;  // the chip must be silent here
      if (efreg_floor(e1[0]) != 0) mism_with++;
      if (efreg_floor(e2[0]) != 0) stale_without++;
    }
  }
  EXPECT_EQ(mism_with, 0);
  EXPECT_GT(stale_without, 100);  // the test does detect a missing discard
}

/* A word written in float format and read as NOFL (and the other way) is
 * read from RAM, as the chip reads it. */
TEST(FloatDsp, FormatMismatchReadsRam) {
  std::vector<Insn> p;
  p.push_back(Insn().ira(0x20).xsel(1).coef(0).zero());
  p.push_back(Insn().mwt(0));         // float format
  p.push_back(Insn().mrd(1).nofl());  // read back as NOFL
  p.push_back(Insn().mwt(2).nofl());  // NOFL
  p.push_back(Insn().iwt(0).mrd(3));  // read back as float
  p.push_back(Insn());
  p.push_back(Insn().iwt(1));
  p.push_back(Insn().ira(0).xsel(1).coef(0).zero());
  p.push_back(Insn().ira(1).xsel(1).coef(0).zero().ewt(0));
  p.push_back(Insn().ewt(1));
  Bench b;
  b.load(p, {-4096}, {0, 50, 2000, 2050});
  b.clear_ring();
  ScspHqFloatDsp f;
  int mism = 0, nonzero = 0;
  for (int n = 0; n < 3000; n++) {
    int32_t mixs[16] = {0};
    mixs[0] = (int32_t)std::lrint(std::sin(n * 0.03) * 250000.0);
    const int16_t exts[2] = {0, 0};
    const float* e = b.both(f, mixs, exts);
    if (n == 0) continue;
    for (int i = 0; i < 2; i++) {
      if (efreg_floor(e[i]) != scsp_dsp.efreg[i]) mism++;
      if (scsp_dsp.efreg[i] != 0) nonzero++;
    }
  }
  EXPECT_EQ(mism, 0);
  EXPECT_GT(nonzero, 4000);
}

/* ------------------------------------------------- an effect program */

/* Echo with feedback, a TEMP lowpass, an FRC-scaled modulated tap (ADRL,
 * ADREB, YSEL 0 and 3, YRL), a NOFL TABLE tap and the shifter modes. */
void load_effect(Bench& b) {
  std::vector<Insn> p;
  p.push_back(Insn().mrd(1));                                   // 0: line[D1]
  p.push_back(Insn().ira(0x30).yrl());                          // 1: Y_REG = EXTS0
  p.push_back(Insn().iwt(0));                                   // 2: MEMS0 = delayed
  p.push_back(Insn().ira(0).xsel(1).coef(1).zero());            // 3: d * fb
  p.push_back(Insn().ira(0x20).xsel(1).coef(0).bsel());         // 4: + in * g
  p.push_back(Insn().mwt(0).ira(0).xsel(1).coef(2).zero());     // 5: line = ..; d * wet
  p.push_back(Insn().ewt(0).xsel(0).tra(1).coef(4).zero());     // 6: EFREG0; TEMP' * 0.6
  p.push_back(Insn().ira(0).xsel(1).coef(3).bsel());            // 7: + d * 0.4
  p.push_back(Insn().twt(0).ewt(1).frcl());                     // 8: lowpass state, FRC
  p.push_back(Insn().ira(0x21).adrl());                         // 9: ADRS from MIXS1
  p.push_back(Insn().mrd(2).adreb().nxadr());                   // 10: modulated tap
  p.push_back(Insn().ira(0x30).xsel(1).ysel(3).zero());         // 11: EXTS * Y_REG low bits
  p.push_back(Insn().iwt(1).ewt(3));                            // 12
  p.push_back(Insn().ira(1).xsel(1).ysel(0).zero());            // 13: tap * FRC
  p.push_back(Insn().ira(1).xsel(1).coef(5).bsel());            // 14: + tap * 0.3
  p.push_back(Insn().ewt(2).shift(1).ira(0x20).xsel(1).coef(0).zero());  // 15: doubled, saturated
  p.push_back(Insn().mwt(3).nofl());                            // 16: NOFL line = in * g
  p.push_back(Insn().mrd(5).nofl());                            // 17: NOFL line, 100 later
  p.push_back(Insn().mrd(4).nofl().table());                    // 18: TABLE tap
  p.push_back(Insn().iwt(2));                                   // 19
  p.push_back(Insn().iwt(3).ira(2).xsel(1).coef(2).zero());     // 20
  p.push_back(Insn().ira(3).xsel(1).coef(5).bsel());            // 21
  p.push_back(Insn().ewt(4).shift(2));                          // 22: doubled, wrapped
  b.load(p, {2048, 2253, 2867, 1638, 2458, 1229}, {0, 3000, 1500, 6000, 5000, 6100});
}

struct Scenario {
  const char* name;
  int kind;  // 0 noise, 1 sines, 2 silence over a filled ring, 3 full-scale squares
};

double run_effect(const Scenario& sc, bool print) {
  Bench b(0x30, 1);  // a 16K-word ring at word 0x30000
  load_effect(b);
  b.clear_ring();
  ScspHqFloatDsp f;
  uint32_t seed = 12345;
  double sig = 0, err = 0, ch_sig[5] = {0}, ch_err[5] = {0};
  const int N = 44100 * 2;
  for (int n = 0; n < N; n++) {
    int32_t mixs[16] = {0};
    int16_t exts[2] = {0, 0};
    seed = seed * 1664525u + 1013904223u;
    const double noise = ((seed >> 8) & 0xFFFF) / 32768.0 - 1.0;
    const double t = n / 44100.0;
    double in = 0, ex = 0;
    switch (sc.kind) {
      case 0: in = 0.4 * noise; ex = 0.2 * noise; break;
      case 1: in = 0.5 * std::sin(2 * kPi * 440 * t) + 0.2 * std::sin(2 * kPi * 1234 * t);
              ex = 0.3 * std::sin(2 * kPi * 220 * t); break;
      case 2: if (n < 22050) { in = 0.5 * noise; ex = 0.3 * noise; } break;
      default: in = std::sin(2 * kPi * 110 * t) >= 0 ? 0.999 : -1.0;
               ex = std::sin(2 * kPi * 73 * t) >= 0 ? 0.999 : -1.0; break;
    }
    mixs[0] = (int32_t)std::lrint(in * 524287.0);
    mixs[1] = (int32_t)std::lrint(std::sin(2 * kPi * 0.7 * t) * 200000.0);  // ADRS sweep
    exts[0] = (int16_t)std::lrint(ex * 32767.0);
    const float* e = b.both(f, mixs, exts);
    if (sc.kind == 2 && n < 22050) continue;  // measure the decay only
    if (n < 4410) continue;                   // after the ring has filled
    for (int i = 0; i < 5; i++) {
      const double c = scsp_dsp.efreg[i] + 0.5;  // the chip floors: compare mid-step
      const double d = e[i] * 32768.0 - c;
      sig += c * c;
      err += d * d;
      ch_sig[i] += c * c;
      ch_err[i] += d * d;
    }
  }
  const double db = 10 * std::log10(sig / (err + 1e-30));
  if (print) {
    printf("effect %-26s chip vs float: %.1f dB below the effect signal; per EFREG (rms / dB):",
           sc.name, db);
    for (int i = 0; i < 5; i++)
      printf(" %.0f/%.1f", std::sqrt(ch_sig[i] / N), 10 * std::log10(ch_sig[i] / (ch_err[i] + 1e-30)));
    printf("\n");
  }
  return db;
}

TEST(FloatDsp, EffectMatchesChipNoise) {
  EXPECT_GT(run_effect({"noise", 0}, true), 50.0);
}
TEST(FloatDsp, EffectMatchesChipSines) {
  EXPECT_GT(run_effect({"sines", 1}, true), 50.0);
}
TEST(FloatDsp, EffectMatchesChipSilenceAfterSignal) {
  EXPECT_GT(run_effect({"silence over a filled ring", 2}, true), 50.0);
}
TEST(FloatDsp, EffectFullScaleSquares) {
  /* saturation and the SHFT1 wrap at full scale */
  EXPECT_GT(run_effect({"full-scale squares", 3}, true), 50.0);
}

/* A float DSP started on a chip that has been running continues it: the
 * seeded sample is the chip's, and the next ones stay close. */
TEST(FloatDsp, SeedContinuesRunningChip) {
  Bench b(0x30, 1);  // a 16K-word ring at word 0x30000
  load_effect(b);
  b.clear_ring();
  ScspHqFloatDsp f;
  double sig = 0, err = 0;
  int seeded_mism = 0;
  for (int n = 0; n < 30000; n++) {
    int32_t mixs[16] = {0};
    int16_t exts[2] = {0, 0};
    mixs[0] = (int32_t)std::lrint(std::sin(n * 0.031) * 200000.0);
    mixs[1] = (int32_t)std::lrint(std::sin(n * 0.0001) * 200000.0);
    exts[0] = (int16_t)std::lrint(std::sin(n * 0.017) * 9000.0);
    if (n < 20000) {
      b.chip(mixs, exts);  // the float DSP is not running yet
      continue;
    }
    const float* e = b.both(f, mixs, exts);
    for (int i = 0; i < 5; i++) {
      if (n == 20000) {
        if ((int)std::lrint(e[i] * 32768.0) != scsp_dsp.efreg[i]) seeded_mism++;
        continue;
      }
      const double c = scsp_dsp.efreg[i] + 0.5;
      sig += c * c;
      err += (e[i] * 32768.0 - c) * (e[i] * 32768.0 - c);
    }
  }
  EXPECT_EQ(seeded_mism, 0);
  const double db = 10 * std::log10(sig / err);
  printf("seeded mid-run: %.1f dB below the effect signal\n", db);
  EXPECT_GT(db, 50.0);
  EXPECT_EQ(f.seed_count(), 1u);
}

/* A change of COEF / MADRS / MPRO / RBL re-seeds from the chip. */
TEST(FloatDsp, ParameterChangeReseeds) {
  Bench b(0x30, 1);  // a 16K-word ring at word 0x30000
  load_effect(b);
  b.clear_ring();
  ScspHqFloatDsp f;
  int seeded_mism = 0;
  for (int n = 0; n < 8000; n++) {
    int32_t mixs[16] = {0};
    int16_t exts[2] = {0, 0};
    mixs[0] = (int32_t)std::lrint(std::sin(n * 0.031) * 200000.0);
    if (n == 2000) scsp_dsp.coef[1] = coef13(1000);
    if (n == 4000) scsp_dsp.madrs[1] = 2500;
    if (n == 6000) {
      scsp_dsp.mpro[3] = Insn().ira(0).xsel(1).coef(1).zero().negb().u.all;
      scsp_dsp.updated = 1;
    }
    if (n == 7000) b.rbl = 2;
    const float* e = b.both(f, mixs, exts);
    if (n == 2000 || n == 4000 || n == 6000 || n == 7000)
      for (int i = 0; i < 5; i++)
        if ((int)std::lrint(e[i] * 32768.0) != scsp_dsp.efreg[i]) seeded_mism++;
  }
  EXPECT_EQ(f.seed_count(), 5u);  // the first sample and the four changes
  EXPECT_EQ(seeded_mism, 0);
}

/* ------------------------------------------------------------- renderer */

/* The renderer feeds the float DSP with its voices' float sends (previous
 * sample) and mixes its EFREG instead of the chip's. A pass-through program
 * (EFREG0 = MIXS0) returns the voice's send one sample later. */
TEST(FloatDsp, RendererFeedsFloatSends) {
  std::vector<Insn> p;
  p.push_back(Insn().ira(0x20).xsel(1).coef(0).zero());  // acc = -in (exact)
  p.push_back(Insn().coef(1).bsel().negb());              // acc = 0 * TEMP - acc = in
  p.push_back(Insn().ewt(0));
  Bench b;
  b.load(p, {-4096, 0}, {});
  ScspHqRenderer r;
  r.float_dsp = true;
  ScspHqTaps taps;
  std::memset(&taps, 0, sizeof taps);
  for (int n = 0; n < 32; n++) {
    taps.v[n].kind = ScspHqVoiceTap::SAMPLE;
    taps.v[n].att = 0xFFFF;  // no level
  }
  taps.master = 0x8000;
  ScspHqDspView v = b.view();
  taps.dsp = &v;
  // slot 3 sends to MIXS0 at IMXL 6 (half), no direct output; slot 0 returns
  // EFREG0 at full level, centre-left only
  taps.v[3].isel = 0;
  taps.v[3].imxl = 6;
  taps.v[0].effect_vol[0] = 16384;
  float prev_sample = 0;
  double max_err = 0;
  for (int n = 0; n < 200; n++) {
    const float s = (float)std::lrint(std::sin(n * 0.2) * 20000.0);
    taps.v[3].sample = (int16_t)s;
    int32_t mixs[16] = {0};
    const int16_t exts[2] = {0, 0};
    b.chip(mixs, exts);  // the chip's ring offset and state (its sends are unused)
    v = b.view();
    float out[2];
    r.render(taps, &b.ram[0], out);
    // -(-x) of the previous sample's send (half level): EFREG in 1/32768
    // units is the send * 32768. The first sample is seeded (chip: 0).
    if (n >= 2) max_err = std::max(max_err, (double)std::fabs(out[0] - prev_sample * 0.5f));
    prev_sample = s;
  }
  EXPECT_LT(max_err, 1e-3);
}

}  // namespace
