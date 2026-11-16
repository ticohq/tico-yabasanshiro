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

/* See dsp_float.h. The step below follows ScspDspExec() in scspdsp.c, in
 * the same order; the comments name the chip's variables. */
#include "dsp_float.h"

#include <cmath>
#include <cstring>

namespace {

const float SCALE = 8388608.0f;          /* 2^23: one datapath unit */
const float INV_SCALE = 1.0f / 8388608.0f;
/* the shifter saturates at 0x7FFFFF / -0x800000 */
const float SAT_MAX = 8388607.0f / 8388608.0f;

inline int32_t sext(uint32_t v, unsigned bits) {
  return (int32_t)(v << (32 - bits)) >> (32 - bits);
}

/* a float on the datapath's scale as the 24-bit word the chip would hold */
inline uint32_t to24(float f) {
  double v = std::floor((double)f * SCALE);
  if (v > 8388607.0) v = 8388607.0;
  if (v < -8388608.0) v = -8388608.0;
  return (uint32_t)(int32_t)v & 0xFFFFFF;
}

/* wraps into [-limit, limit), as two's complement arithmetic does */
inline float wrap(float v, float limit) {
  if (v >= -limit && v < limit) return v;
  const float span = 2 * limit;
  v = std::fmod(v + limit, span);
  if (v < 0) v += span;
  return v - limit;
}

/* the ring's 16-bit float format, decoded as float_to_int() in scspdsp.c
 * (sign-extended to 32 bits) */
inline int32_t decode_ring_float(uint16_t f) {
  const uint32_t sign = (f >> 15) & 1;
  uint32_t exponent = (f >> 11) & 0xF;
  const uint32_t mantissa = f & 0x7FF;
  uint32_t v = sign << 31;
  if (exponent > 11) {
    exponent = 11;
    v |= sign << 30;
  } else {
    v |= (sign ^ 1) << 30;
  }
  v |= mantissa << 19;
  return (int32_t)v >> (exponent + 8);
}

}  // namespace

ScspHqFloatDsp::ScspHqFloatDsp()
    : rbp_(0), rbl_(0), last_step_(0), steps_run_(0), efreg_mask_(0), seed_request_(true),
      seed_count_(0),
      inputs_(0), acc_(0), y_(0), frc_(0), adrs_(0), rw_addr_(0), read_pending_(0),
      write_pending_(false), write_from_chip_(false), write_mode_(1), write_value_(0),
      read_value_(0), ring_(RAM_WORDS, 0.0f), written_(RAM_WORDS, 0) {
  std::memset(mpro_, 0, sizeof mpro_);
  std::memset(coef_, 0, sizeof coef_);
  std::memset(madrs_, 0, sizeof madrs_);
  std::memset(steps_, 0, sizeof steps_);
  std::memset(temp_, 0, sizeof temp_);
  std::memset(mems_, 0, sizeof mems_);
  std::memset(efreg_, 0, sizeof efreg_);
}

void ScspHqFloatDsp::discard(uint32_t first, uint32_t count) {
  if (first >= RAM_WORDS) return;
  if (count > RAM_WORDS - first) count = RAM_WORDS - first;
  std::memset(&written_[first], 0, count);
}

/* Field positions as union ScspDspInstruction in scspdsp.h. The step count
 * is the chip's own last_step, not one recounted from MPRO: the chip recounts
 * it only when it sees MPRO change, so the two can differ. */
void ScspHqFloatDsp::decode(int last_step) {
  steps_run_ = (unsigned)(last_step < 0 ? 0 : (last_step > 128 ? 128 : last_step));
  efreg_mask_ = 0;
  for (unsigned i = 0; i < 128; i++) {
    const uint64_t w = mpro_[i];
    Step& s = steps_[i];
#define YAB_DSP_BITS(pos, n) ((unsigned)((w >> (pos)) & ((1u << (n)) - 1)))
    s.nxadr = YAB_DSP_BITS(0, 1) != 0;
    s.adreb = YAB_DSP_BITS(1, 1) != 0;
    s.masa = (uint8_t)YAB_DSP_BITS(2, 5);
    s.cra = (uint8_t)YAB_DSP_BITS(9, 6);
    s.nofl = YAB_DSP_BITS(15, 1) != 0;
    s.bsel = YAB_DSP_BITS(16, 1) != 0;
    s.zero = YAB_DSP_BITS(17, 1) != 0;
    s.negb = YAB_DSP_BITS(18, 1) != 0;
    s.yrl = YAB_DSP_BITS(19, 1) != 0;
    s.shft0 = YAB_DSP_BITS(20, 1) != 0;
    s.shft1 = YAB_DSP_BITS(21, 1) != 0;
    s.frcl = YAB_DSP_BITS(22, 1) != 0;
    s.adrl = YAB_DSP_BITS(23, 1) != 0;
    s.ewa = (uint8_t)YAB_DSP_BITS(24, 4);
    s.ewt = YAB_DSP_BITS(28, 1) != 0;
    s.mrd = YAB_DSP_BITS(29, 1) != 0;
    s.mwt = YAB_DSP_BITS(30, 1) != 0;
    s.table = YAB_DSP_BITS(31, 1) != 0;
    s.iwa = (uint8_t)YAB_DSP_BITS(32, 5);
    s.iwt = YAB_DSP_BITS(37, 1) != 0;
    s.ira = (uint8_t)YAB_DSP_BITS(38, 6);
    s.ysel = (uint8_t)YAB_DSP_BITS(45, 2);
    s.xsel = YAB_DSP_BITS(47, 1) != 0;
    s.twa = (uint8_t)YAB_DSP_BITS(48, 7);
    s.twt = YAB_DSP_BITS(55, 1) != 0;
    s.tra = (uint8_t)YAB_DSP_BITS(56, 7);
#undef YAB_DSP_BITS
    if (s.ewt && i < steps_run_) efreg_mask_ |= (uint16_t)(1u << s.ewa);
  }
}

bool ScspHqFloatDsp::program_changed(const ScspHqDspView& c) const {
  return c.rbp != rbp_ || c.rbl != rbl_ || c.last_step != last_step_ ||
         std::memcmp(mpro_, c.mpro, sizeof mpro_) != 0 ||
         std::memcmp(coef_, c.coef, sizeof coef_) != 0 ||
         std::memcmp(madrs_, c.madrs, sizeof madrs_) != 0;
}

void ScspHqFloatDsp::seed(const ScspHqDspView& c) {
  int i;
  std::memcpy(mpro_, c.mpro, sizeof mpro_);
  std::memcpy(coef_, c.coef, sizeof coef_);
  std::memcpy(madrs_, c.madrs, sizeof madrs_);
  rbp_ = c.rbp;
  rbl_ = c.rbl;
  last_step_ = c.last_step;
  decode(last_step_);
  for (i = 0; i < 128; i++) temp_[i] = (float)sext((uint32_t)c.temp[i], 24) * INV_SCALE;
  for (i = 0; i < 32; i++) mems_[i] = (float)sext((uint32_t)c.mems[i], 24) * INV_SCALE;
  for (i = 0; i < 16; i++) efreg_[i] = (float)c.efreg[i] * (1.0f / 32768);
  acc_ = (float)sext(c.shift_reg, 26) * INV_SCALE;
  inputs_ = (float)sext((uint32_t)c.inputs, 24) * INV_SCALE;
  y_ = (float)sext((uint32_t)c.y_reg, 24) * INV_SCALE;
  frc_ = c.frc_reg;
  adrs_ = c.adrs_reg;
  read_value_ = (float)sext(c.read_value, 24) * INV_SCALE;
  read_pending_ = (uint8_t)c.read_pending;
  write_pending_ = c.write_pending != 0;
  /* the chip has the value of a pending write; it lands in RAM at the same
   * step and address as ours, so ours only has to forget the word */
  write_from_chip_ = true;
  write_mode_ = 1;
  write_value_ = 0;
  rw_addr_ = c.io_addr & 0x3FFFF;
  std::memset(&written_[0], 0, written_.size());
  seed_request_ = false;
  seed_count_.fetch_add(1, std::memory_order_relaxed);
}

const float* ScspHqFloatDsp::run_sample(const ScspHqDspView& c, const uint8_t* sound_ram,
                                        const float mixs[16], const float exts[2]) {
  if (seed_request_ || program_changed(c)) {
    seed(c);
    return efreg_;
  }
  const uint16_t* ram = (const uint16_t*)sound_ram;
  const uint32_t mdec = c.mdec_ct;
  const uint16_t ring_mask = (uint16_t)((0x2000u << rbl_) - 1);

  for (unsigned i = 0; i < steps_run_; i++) {
    const Step& s = steps_[i];

    /* input (IRA 0x32-0x3F keep the previous INPUTS) */
    if (s.ira < 0x20)
      inputs_ = mems_[s.ira];
    else if (s.ira < 0x30)
      inputs_ = wrap(mixs[s.ira & 0xF], 1.0f);
    else if (s.ira < 0x32)
      inputs_ = exts[s.ira & 1];

    const float temp = temp_[(s.tra + mdec) & 0x7F];

    /* Y from the registers before this step */
    float y;
    switch (s.ysel) {
      case 0: y = (float)sext(frc_, 13) * (1.0f / 4096); break;
      case 1: y = (float)sext(coef_[s.cra], 13) * (1.0f / 4096); break;
      case 2: y = y_; break;
      default: y = (float)((to24(y_) >> 4) & 0xFFF) * (1.0f / 4096); break;
    }
    if (s.yrl) y_ = inputs_;

    /* shifter: the accumulator from the previous step */
    float shifted = acc_ * ((s.shft0 != s.shft1) ? 2.0f : 1.0f);
    if (!s.shft1) {
      if (shifted > SAT_MAX)
        shifted = SAT_MAX;
      else if (shifted < -1.0f)
        shifted = -1.0f;
    } else {
      shifted = wrap(shifted, 1.0f);
    }

    if (s.ewt) efreg_[s.ewa] = shifted;
    if (s.twt) temp_[(s.twa + mdec) & 0x7F] = shifted;
    if (s.frcl) {
      const uint32_t v = to24(shifted);
      frc_ = (s.shft0 && s.shft1) ? (uint16_t)(v & 0xFFF) : (uint16_t)(v >> 11);
    }

    /* multiply-add into the 26-bit adder */
    const float x = s.xsel ? inputs_ : temp;
    float b = s.bsel ? acc_ : temp;
    if (s.negb) b = -b;
    if (s.zero) b = 0;
    acc_ = wrap(y * x + b, 4.0f);

    if (s.iwt) mems_[s.iwa] = read_value_;

    /* the memory access the previous step asked for: a read goes first */
    if (read_pending_) {
      const uint8_t mode = read_pending_;
      if (written_[rw_addr_] == mode) {
        read_value_ = ring_[rw_addr_];
      } else {
        const uint16_t raw = ram[rw_addr_];
        const int32_t v = mode == 2 ? sext((uint32_t)raw << 8, 24) : decode_ring_float(raw);
        read_value_ = (float)v * INV_SCALE;
      }
      read_pending_ = 0;
    } else if (write_pending_) {
      if (write_from_chip_) {
        written_[rw_addr_] = 0;
      } else {
        ring_[rw_addr_] = write_value_;
        written_[rw_addr_] = write_mode_;
      }
      write_pending_ = false;
    }

    /* this step's address and access */
    uint16_t a = madrs_[s.masa];
    a = (uint16_t)(a + (s.nxadr ? 1 : 0));
    if (s.adreb) a = (uint16_t)(a + sext(adrs_, 12));
    if (!s.table) {
      a = (uint16_t)(a + mdec);
      a &= ring_mask;
    }
    rw_addr_ = (a + ((uint32_t)rbp_ << 12)) & 0x3FFFF;
    if (s.mrd) read_pending_ = s.nofl ? 2 : 1;
    if (s.mwt) {
      write_pending_ = true;
      write_from_chip_ = false;
      write_mode_ = s.nofl ? 2 : 1;
      /* both formats keep the full value (the chip stores 16 bits) */
      write_value_ = shifted;
    }
    if (s.adrl) {
      adrs_ = (s.shft0 && s.shft1)
                  ? (uint16_t)(to24(shifted) >> 12)
                  : (uint16_t)(((uint32_t)(sext(to24(inputs_), 24) >> 16)) & 0xFFF);
    }
  }
  /* an EFREG the program does not write keeps what the CPU wrote there */
  for (int k = 0; k < 16; k++)
    if (!(efreg_mask_ & (1u << k))) efreg_[k] = (float)c.efreg[k] * (1.0f / 32768);
  return efreg_;
}
