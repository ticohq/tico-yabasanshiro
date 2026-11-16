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
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

/* See scsp_hq.h. */
#include "scsp_hq.h"

#include <algorithm>
#include <cmath>

namespace {

double bessel_i0(double x) {
  double sum = 1, term = 1;
  for (int k = 1; k < 40; k++) {
    term *= (x / (2 * k)) * (x / (2 * k));
    sum += term;
    if (term < sum * 1e-12) break;
  }
  return sum;
}

/* Waveform kernel: 8 zero crossings each side; the cutoff sits a little
 * below the slot's Nyquist so the transition band stays out of the audio. */
const int VOICE_WIDTH = 8;
const float VOICE_CUTOFF = 0.95f;
/* Taps each side at most (the cutoff goes no lower than VOICE_WIDTH / MAX_SPAN). */
const int MAX_SPAN = 48;

const double HQ_PI = 3.14159265358979323846;

const uint32_t SOUND_RAM_MASK = 0x7FFFF;

/* Sound RAM reads in the emulator's layout (T2ReadWord / T2ReadByte in memory.h). */
inline uint16_t ram_word(const uint8_t* ram, uint32_t addr) {
  return *(const uint16_t*)(ram + (addr & (SOUND_RAM_MASK & ~1u)));
}

inline uint8_t ram_byte(const uint8_t* ram, uint32_t addr) {
#ifdef WORDS_BIGENDIAN
  return ram[addr & SOUND_RAM_MASK];
#else
  return ram[(addr & SOUND_RAM_MASK) ^ 1];
#endif
}

}  // namespace

ScspHqSincKernel::ScspHqSincKernel(int half_width, double beta)
    : width_(half_width), table_((size_t)half_width * RES + 2) {
  const double norm = bessel_i0(beta);
  for (size_t i = 0; i < table_.size(); i++) {
    const double x = (double)i / RES;
    if (x >= half_width) {
      table_[i] = 0;
      continue;
    }
    const double sinc = x == 0 ? 1.0 : std::sin(HQ_PI * x) / (HQ_PI * x);
    const double r = x / half_width;
    table_[i] = (float)(sinc * bessel_i0(beta * std::sqrt(1 - r * r)) / norm);
  }
}

ScspHqRenderer::ScspHqRenderer()
    : float_dsp(false), float_dsp_chip_sends(false), kernel_(VOICE_WIDTH, 8.0),
      fx_running_(false) {
  reset();
}

void ScspHqRenderer::reset() {
  for (int i = 0; i < 32; i++) {
    state_[i].last_pos = 0;
    state_[i].wrapped = false;
    state_[i].fresh = true;
  }
  for (int i = 0; i < 16; i++) sends_[i] = 0;
  for (int i = 0; i < 2; i++) fx_mix_[i][0] = fx_mix_[i][1] = 0;
  fx_.request_seed();
}

/* The slot's waveform at read position p, band-limited. */
float ScspHqRenderer::voice(const ScspHqVoiceTap& t, const uint8_t* ram, bool wrapped) const {
  const double p = t.pos + t.phase / 16384.0;
  const double speed = t.step / 16384.0; /* source samples per output sample */
  float cutoff = speed > 1 ? VOICE_CUTOFF / (float)speed : VOICE_CUTOFF;
  cutoff = std::max(cutoff, (float)VOICE_WIDTH / MAX_SPAN);
  const double span = VOICE_WIDTH / cutoff;
  const int32_t first = (int32_t)std::floor(p - span) + 1;
  const int32_t last = (int32_t)std::floor(p + span);
  const int32_t loop_len = (int32_t)t.lea - (int32_t)t.lsa;

  float sum = 0;
  for (int32_t j = first; j <= last; j++) {
    /* where sample j of the voice's timeline is in its waveform */
    int32_t k = j;
    if (t.loop && loop_len > 0) {
      /* the loop plays LSA..LEA-1 (LEA itself is never the read position) */
      if (k >= t.lea)
        k = t.lsa + (k - t.lsa) % loop_len;
      else if (wrapped && k < t.lsa)
        k += ((t.lsa - k + loop_len - 1) / loop_len) * loop_len;
    } else if (k > t.lea) {
      continue; /* after the end */
    }
    if (k < 0) continue; /* before the start */
    int16_t s;
    if (t.pcm8)
      s = (int16_t)(ram_byte(ram, t.sa + (uint32_t)k) << 8);
    else
      s = (int16_t)ram_word(ram, t.sa + (uint32_t)k * 2);
    s = (int16_t)(s ^ t.sb_xor);
    sum += s * kernel_((float)(cutoff * (j - p)));
  }
  return sum * cutoff;
}

void ScspHqRenderer::render(const ScspHqTaps& taps, const uint8_t* ram, float out[2]) {
  /* The effects. As on the chip, this sample's program works on the
   * previous sample's sends. */
  const float* fx_out = NULL;
  const bool fx_on = float_dsp && taps.dsp != NULL;
  if (fx_on) {
    if (!fx_running_) {
      fx_.request_seed(); /* just turned on: start from the chip's state */
      for (int i = 0; i < 16; i++) sends_[i] = 0;
    }
    float chip_sends[16];
    const float* sends = sends_;
    if (float_dsp_chip_sends) {
      /* MIXS is 20-bit; the DSP shifts it by 4 onto its 24-bit scale */
      for (int i = 0; i < 16; i++) chip_sends[i] = (float)taps.chip_mixs[i] * (1.0f / 524288);
      sends = chip_sends;
    }
    const float exts[2] = {taps.exts[0] * (1.0f / 32768), taps.exts[1] * (1.0f / 32768)};
    fx_out = fx_.run_sample(*taps.dsp, ram, sends, exts);
    for (int i = 0; i < 16; i++) sends_[i] = 0;
  }
  fx_running_ = fx_on;

  float fx_acc[2][2] = {{0, 0}, {0, 0}};
  float acc[2] = {0, 0};
  for (int n = 0; n < 32; n++) {
    const ScspHqVoiceTap& t = taps.v[n];
    VoiceState& st = state_[n];
    if (t.key_on) {
      st.last_pos = 0;
      st.wrapped = false;
      st.fresh = false;
    }
    float v;
    if (t.kind == ScspHqVoiceTap::WAVE) {
      if (st.fresh) {
        /* first tap after reset(): the history is unknown, so prime it */
        st.wrapped = t.loop && t.pos >= t.lsa;
        st.fresh = false;
      } else if (t.pos < st.last_pos) {
        st.wrapped = true;
      }
      st.last_pos = t.pos;
      v = voice(t, ram, st.wrapped);
    } else {
      v = t.sample;
    }
    /* the level: 0x40 steps of attenuation halve it, linearly in between */
    if (t.att != 0xFFFF)
      v *= (float)((t.att & 0x3F) ^ 0x7F) / (float)(1u << ((t.att >> 6) + 7));
    if (fx_out == NULL) {
      const float fx = (float)t.fx;
      acc[0] += v * t.direct_vol[0] * (1.0f / 16384) + fx * t.effect_vol[0] * (1.0f / 16384);
      acc[1] += v * t.direct_vol[1] * (1.0f / 16384) + fx * t.effect_vol[1] * (1.0f / 16384);
      continue;
    }
    /* DSP send: IMXL 7 is the sample as it is, each step below halves it
     * (the chip shifts the levelled sample right by 7 - IMXL) */
    if (t.imxl) sends_[t.isel] += v * (1.0f / 32768) * (float)(1 << t.imxl) * (1.0f / 128);
    /* slots 0-15 return EFREG; 16 and 17 put out the external inputs */
    const float fx = n < 16 ? fx_out[n] * 32768.0f : (float)t.fx;
    acc[0] += v * t.direct_vol[0] * (1.0f / 16384) + fx * t.effect_vol[0] * (1.0f / 16384);
    acc[1] += v * t.direct_vol[1] * (1.0f / 16384) + fx * t.effect_vol[1] * (1.0f / 16384);
    if (n < 16) {
      fx_acc[0][0] += (float)t.fx * t.effect_vol[0] * (1.0f / 16384);
      fx_acc[0][1] += (float)t.fx * t.effect_vol[1] * (1.0f / 16384);
      fx_acc[1][0] += fx * t.effect_vol[0] * (1.0f / 16384);
      fx_acc[1][1] += fx * t.effect_vol[1] * (1.0f / 16384);
    }
  }
  const float master = taps.master * (1.0f / 32768);
  out[0] = acc[0] * master;
  out[1] = acc[1] * master;
  if (fx_out != NULL) {
    for (int i = 0; i < 2; i++) {
      fx_mix_[i][0] = fx_acc[i][0] * master;
      fx_mix_[i][1] = fx_acc[i][1] * master;
    }
  }
}

int16_t ScspHqQuantizer::operator()(float v) {
  if (v > -0.5f && v < 0.5f) return 0;
  v = std::min(32767.0f, std::max(-32768.0f, v));
  /* TPDF: the difference of two uniform values in [0, 1) */
  seed_ = seed_ * 1664525u + 1013904223u;
  const float r1 = (float)(seed_ >> 16) * (1.0f / 65536);
  seed_ = seed_ * 1664525u + 1013904223u;
  const float r2 = (float)(seed_ >> 16) * (1.0f / 65536);
  v += r1 - r2;
  return (int16_t)std::lrint(std::min(32767.0f, std::max(-32768.0f, v)));
}
