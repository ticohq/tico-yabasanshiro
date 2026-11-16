/* Tests for the SCSP high-quality renderer (sound_hq/scsp_hq.h).
 *
 * Each tone case plays one slot from synthetic taps: a looped sine in sound
 * RAM (one period repeated so the loop is long), stepped the way the chip
 * steps a slot (14-bit phase, normal loop wrap that keeps the overshoot), for
 * 2 seconds. With hq the slot is a WAVE tap and the renderer interpolates it;
 * without, the tap carries the chip's own 2-point linear interpolation (6-bit
 * fraction), which is what the exact output plays. SINAD is measured against
 * a least-squares fit of the expected sine. */
#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "scsp_hq.h"

namespace {

const double kPi = 3.14159265358979323846;
const unsigned kRate = 44100;
const uint32_t kSa = 0x10000;  // waveform start, bytes

struct Tone {
  std::vector<float> l;
  double freq;  // expected tone frequency, Hz
};

void put_word(std::vector<uint8_t>& ram, uint32_t addr, int16_t v) {
  const uint16_t w = (uint16_t)v;
  std::memcpy(&ram[addr], &w, 2);  // host order, as T2WriteWord stores it
}

int16_t get_word(const std::vector<uint8_t>& ram, uint32_t addr) {
  uint16_t w;
  std::memcpy(&w, &ram[addr], 2);
  return (int16_t)w;
}

ScspHqTaps base_taps() {
  ScspHqTaps taps;
  std::memset(&taps, 0, sizeof taps);
  for (int n = 0; n < 32; n++) {
    taps.v[n].kind = ScspHqVoiceTap::SAMPLE;
    taps.v[n].att = 0x3FF;
  }
  taps.master = 0x8000;
  return taps;
}

Tone renderTone(bool hq, unsigned period, double speed, double seconds = 2.0) {
  std::vector<uint8_t> ram(0x80000, 0);
  const unsigned reps = 4096 / period > 0 ? 4096 / period : 1;
  const unsigned len = period * reps;
  for (unsigned i = 0; i < len + 1; i++)
    put_word(ram, kSa + 2 * i, (int16_t)std::lrint(std::sin(2 * kPi * i / period) * 0.9 * 32767));

  ScspHqRenderer r;
  ScspHqTaps taps = base_taps();
  ScspHqVoiceTap& t = taps.v[0];
  t.loop = true;
  t.lsa = 0;
  t.lea = (uint16_t)len;
  t.sa = kSa;
  t.att = 0;
  t.direct_vol[0] = t.direct_vol[1] = 16384;
  t.step = (uint32_t)std::lrint(speed * 16384);

  uint32_t pos = 0, phase = 0;
  Tone out;
  out.freq = kRate * (t.step / 16384.0) / period;
  const unsigned n = (unsigned)(kRate * seconds);
  for (unsigned i = 0; i < n; i++) {
    t.key_on = i == 0;
    t.pos = (uint16_t)pos;
    t.phase = (uint16_t)phase;
    if (hq) {
      t.kind = ScspHqVoiceTap::WAVE;
    } else {
      // the chip: two neighbours, 6-bit fraction of the 14-bit phase
      const int32_t frac = (int32_t)((phase >> 8) & 0x3F);
      const int32_t s0 = get_word(ram, kSa + 2 * pos);
      const int32_t s1 = get_word(ram, kSa + 2 * (pos + 1));
      t.kind = ScspHqVoiceTap::SAMPLE;
      t.sample = (int16_t)((s0 * (0x40 - frac) + s1 * frac) >> 6);
    }
    float f[2];
    r.render(taps, ram.data(), f);
    out.l.push_back(f[0]);
    phase += t.step;
    pos += phase >> 14;
    phase &= 0x3FFF;
    if (pos >= t.lea) pos -= (uint32_t)(t.lea - t.lsa);
  }
  return out;
}

// signal-to-(noise + distortion) in dB, against the best-fitting sine at the
// expected frequency; the first 0.1 s is skipped
double sinad(const Tone& tone, bool quantized = false) {
  const size_t skip = kRate / 10, n = tone.l.size() - skip;
  std::vector<float> y16;
  if (quantized) {
    ScspHqQuantizer q;
    for (float v : tone.l) y16.push_back((float)q(v));
  }
  const std::vector<float>& ys = quantized ? y16 : tone.l;
  const double w = 2 * kPi * tone.freq / kRate;
  // least squares for a*sin + b*cos + c
  double m[3][4] = {{0}};
  for (size_t i = 0; i < n; i++) {
    const double x[3] = {std::sin(w * i), std::cos(w * i), 1.0};
    const double y = ys[skip + i];
    for (int r = 0; r < 3; r++) {
      for (int c = 0; c < 3; c++) m[r][c] += x[r] * x[c];
      m[r][3] += x[r] * y;
    }
  }
  for (int p = 0; p < 3; p++)
    for (int r = 0; r < 3; r++) {
      if (r == p) continue;
      const double k = m[r][p] / m[p][p];
      for (int c = 0; c < 4; c++) m[r][c] -= k * m[p][c];
    }
  const double a = m[0][3] / m[0][0], b = m[1][3] / m[1][1], c = m[2][3] / m[2][2];
  double sig = 0, err = 0;
  for (size_t i = 0; i < n; i++) {
    const double fit = a * std::sin(w * i) + b * std::cos(w * i) + c;
    const double y = ys[skip + i];
    sig += fit * fit;
    err += (y - fit) * (y - fit);
  }
  return 10 * std::log10(sig / err);
}

double rms(const Tone& tone) {
  double s = 0;
  for (float v : tone.l) s += (double)v * v;
  return std::sqrt(s / tone.l.size());
}

// one second of every slot keyed on a silent waveform, through the 16-bit
// output stage
std::vector<int16_t> renderSilence(bool hq) {
  std::vector<uint8_t> ram(0x80000, 0);
  ScspHqRenderer r;
  ScspHqQuantizer q;
  ScspHqTaps taps = base_taps();
  for (int n = 0; n < 32; n++) {
    ScspHqVoiceTap& t = taps.v[n];
    t.kind = hq ? ScspHqVoiceTap::WAVE : ScspHqVoiceTap::SAMPLE;
    t.loop = (n & 1) != 0;
    t.pcm8 = (n & 2) != 0;
    t.lsa = 16;
    t.lea = 1000;
    t.sa = 0x2000 * n;
    t.att = 0;
    t.step = 16384 + 977 * n;  // a range of pitches, some well above 1x
    t.direct_vol[0] = t.direct_vol[1] = 16384;
    t.effect_vol[0] = t.effect_vol[1] = 16384;
  }
  std::vector<int16_t> out;
  uint32_t pos = 0;
  for (unsigned i = 0; i < kRate; i++) {
    for (int n = 0; n < 32; n++) {
      taps.v[n].key_on = i == 0;
      taps.v[n].pos = (uint16_t)(pos % 900);
    }
    float f[2];
    r.render(taps, ram.data(), f);
    out.push_back(q(f[0]));
    out.push_back(q(f[1]));
    pos++;
  }
  return out;
}

int peak(const std::vector<int16_t>& v) {
  int p = 0;
  for (int16_t s : v) p = std::max(p, std::abs((int)s));
  return p;
}

}  // namespace

// The chip's pitch: (1024 + FNS) / 1024 * 2^OCT source samples per output
// sample. An exact 3x (OCT 1, FNS 512) never interpolates (the fraction stays
// 0), so the 3x cases use OCT 1, FNS 520 = 3.016x, as in the renderer's first
// measurement (period 32: 56.8 dB linear, 80.0 dB hq after 16-bit output).
double pitch(int oct, unsigned fns) { return (1024 + fns) / 1024.0 * std::pow(2.0, oct); }

TEST(ScspHq, SincBeatsLinearAt3x) {
  const double hq = sinad(renderTone(/*hq=*/true, 32, pitch(1, 520)));
  const double lin = sinad(renderTone(/*hq=*/false, 32, pitch(1, 520)));  // 2-point linear like the chip
  const double hq16 = sinad(renderTone(true, 32, pitch(1, 520)), true);
  printf("period 32, 3.016x: SINAD hq %.1f dB (16-bit out %.1f dB), linear %.1f dB\n", hq, hq16, lin);
  EXPECT_GT(hq, 75.0);
  EXPECT_GT(hq16, 75.0);
  EXPECT_LT(lin, 60.0);
}

TEST(ScspHq, SincBeatsLinearAt3xPeriod64) {
  const double hq = sinad(renderTone(true, 64, pitch(1, 520)));
  const double lin = sinad(renderTone(false, 64, pitch(1, 520)));
  printf("period 64, 3.016x: SINAD hq %.1f dB, linear %.1f dB\n", hq, lin);
  EXPECT_GT(hq, 75.0);
  EXPECT_GT(hq, lin + 5.0);
}

// far up, pitched down and at the slot's own rate the renderer must not lose quality
TEST(ScspHq, OtherPitches) {
  const struct { unsigned period; int oct; unsigned fns; } cases[] = {
      {16, 2, 600}, {64, -2, 300}, {100, 0, 0}, {64, 0, 77}};
  for (const auto& c : cases) {
    const double hq = sinad(renderTone(true, c.period, pitch(c.oct, c.fns)));
    const double lin = sinad(renderTone(false, c.period, pitch(c.oct, c.fns)));
    printf("period %u, %.3fx: SINAD hq %.1f dB, linear %.1f dB\n", c.period, pitch(c.oct, c.fns), hq, lin);
    EXPECT_GE(hq, lin - 0.5);
  }
}

// the renderer keeps the chip's loudness
TEST(ScspHq, LevelMatchesChip) {
  const double ratio = rms(renderTone(true, 64, 1.37)) / rms(renderTone(false, 64, 1.37));
  printf("rms ratio hq / linear: %.4f\n", ratio);
  EXPECT_NEAR(ratio, 1.0, 0.01);
}

TEST(ScspHq, SilenceStaysSilent) {
  EXPECT_EQ(peak(renderSilence(true)), 0);
  EXPECT_EQ(peak(renderSilence(false)), 0);
}

// A looped instrument whose attack (before LSA) differs from its loop: a
// 0.75 kHz-ish sine attack, then a loop of another period.
struct AttackLoopVoice {
  std::vector<uint8_t> ram;
  ScspHqTaps taps;
  uint32_t pos, phase;
  static const unsigned kLsa = 300, kLoopPeriod = 40, kLoopLen = 40 * 20;
  AttackLoopVoice() : ram(0x80000, 0), taps(base_taps()), pos(0), phase(0) {
    for (unsigned i = 0; i < kLsa; i++)
      put_word(ram, kSa + 2 * i, (int16_t)std::lrint(std::sin(2 * kPi * i / 59.0) * 0.8 * 32767));
    for (unsigned i = 0; i <= kLoopLen; i++)
      put_word(ram, kSa + 2 * (kLsa + i), (int16_t)std::lrint(std::sin(2 * kPi * i / kLoopPeriod) * 0.8 * 32767));
    ScspHqVoiceTap& t = taps.v[29];  // one of the slots mixed a call late on the chip
    t.kind = ScspHqVoiceTap::WAVE;
    t.loop = true;
    t.lsa = kLsa;
    t.lea = kLsa + kLoopLen;
    t.sa = kSa;
    t.att = 0;
    t.direct_vol[0] = t.direct_vol[1] = 16384;
    t.step = (uint32_t)std::lrint(1.31 * 16384);
  }
  float step(ScspHqRenderer& r, bool key_on) {
    ScspHqVoiceTap& t = taps.v[29];
    t.key_on = key_on;
    t.pos = (uint16_t)pos;
    t.phase = (uint16_t)phase;
    float f[2];
    r.render(taps, ram.data(), f);
    phase += t.step;
    pos += phase >> 14;
    phase &= 0x3FFF;
    if (pos >= t.lea) pos -= (uint32_t)(t.lea - t.lsa);
    return f[0];
  }
  void restart() { pos = 0; phase = 0; }
};

// A second note on a voice that has looped must start from its attack again:
// with the key-on flag the output equals the first note sample for sample.
// Without it (what the taps carried for slots 26-30 before the key-on flag)
// the samples before LSA are taken from the loop instead.
TEST(ScspHq, KeyOnRestartsFromAttack) {
  AttackLoopVoice v;
  ScspHqRenderer r;
  std::vector<float> first;
  for (unsigned i = 0; i < 2000; i++) first.push_back(v.step(r, i == 0));  // wraps several times
  v.restart();
  double max_err = 0;
  for (unsigned i = 0; i < 400; i++) max_err = std::max(max_err, (double)std::fabs(v.step(r, i == 0) - first[i]));
  printf("second note vs first, with key on: max diff %.4f\n", max_err);
  EXPECT_LT(max_err, 1e-3);

  AttackLoopVoice w;
  ScspHqRenderer r2;
  for (unsigned i = 0; i < 2000; i++) w.step(r2, i == 0);
  w.restart();
  double max_err_missed = 0;
  for (unsigned i = 0; i < 400; i++)
    max_err_missed = std::max(max_err_missed, (double)std::fabs(w.step(r2, false) - first[i]));
  printf("second note vs first, key on missed: max diff %.1f\n", max_err_missed);
  EXPECT_GT(max_err_missed, 1000.0);  // the attack is wrong when the flag is lost
}

// After reset() (state load, enabling the mode) a looped voice met past LSA
// is primed as wrapped, so near LSA it renders exactly as without the reset.
TEST(ScspHq, ResetMidLoopPrimesWrap) {
  AttackLoopVoice a, b;
  ScspHqRenderer ra, rb;
  double max_err = 0;
  bool reset_done = false;
  for (unsigned i = 0; i < 4000; i++) {
    // reset b just after one of its wraps, while pos is within the kernel span of LSA
    if (!reset_done && i > 1500 && b.pos >= AttackLoopVoice::kLsa && b.pos < AttackLoopVoice::kLsa + 3) {
      rb.reset();
      reset_done = true;
    }
    const float x = a.step(ra, i == 0), y = b.step(rb, i == 0);
    if (reset_done) max_err = std::max(max_err, (double)std::fabs(x - y));
  }
  printf("reset next to LSA: max diff vs no reset %.4f\n", max_err);
  ASSERT_TRUE(reset_done);
  EXPECT_LT(max_err, 1e-3);
}

// after reset() (state load, chip reset) the history is rebuilt from sound
// RAM, so the output continues without a jump
TEST(ScspHq, ResetDoesNotClick) {
  std::vector<uint8_t> ram(0x80000, 0);
  const unsigned period = 64, len = period * 64;
  for (unsigned i = 0; i < len + 1; i++)
    put_word(ram, kSa + 2 * i, (int16_t)std::lrint(std::sin(2 * kPi * i / period) * 0.9 * 32767));
  ScspHqRenderer r;
  ScspHqTaps taps = base_taps();
  ScspHqVoiceTap& t = taps.v[0];
  t.kind = ScspHqVoiceTap::WAVE;
  t.loop = true;
  t.lea = (uint16_t)len;
  t.sa = kSa;
  t.att = 0;
  t.direct_vol[0] = t.direct_vol[1] = 16384;
  t.step = (uint32_t)std::lrint(2.3 * 16384);
  uint32_t pos = 0, phase = 0;
  float prev = 0, max_step = 0, step_at_reset = 0;
  for (unsigned i = 0; i < kRate; i++) {
    if (i == kRate / 2) r.reset();
    t.pos = (uint16_t)pos;
    t.phase = (uint16_t)phase;
    float f[2];
    r.render(taps, ram.data(), f);
    if (i > 100) {
      const float d = std::fabs(f[0] - prev);
      if (i == kRate / 2) step_at_reset = d;
      else max_step = std::max(max_step, d);
    }
    prev = f[0];
    phase += t.step;
    pos += phase >> 14;
    phase &= 0x3FFF;
    if (pos >= t.lea) pos -= (uint32_t)(t.lea - t.lsa);
  }
  printf("sample step at reset %.1f, max elsewhere %.1f\n", step_at_reset, max_step);
  EXPECT_LE(step_at_reset, max_step * 1.01f);
}
