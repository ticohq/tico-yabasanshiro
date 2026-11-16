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

/* The SCSP effect DSP in floating point (part of the high-quality output,
 * an enhancement that is off by default).
 *
 * Runs the program the chip's DSP (scspdsp.c) runs, with the same
 * coefficients (COEF), delay addresses (MADRS), ring buffer (RBP/RBL) and
 * the same step-by-step datapath, but keeps every value as a float: no
 * 16-bit "float" format in the delay ring (about 11 bits of precision on the
 * hardware) and no truncation of products, sends and outputs. Values use the
 * 24-bit datapath's scale (1.0 = 2^23). The hardware's rules are kept as the
 * chip implements them: the shifter saturates at +-1.0 unless SHFT1 is set,
 * where it wraps; the accumulator wraps at +-4.0 (26 bits); MIXS wraps at
 * +-1.0 (24 bits once shifted onto the datapath).
 *
 * Shadow memory. The delay ring is a float copy of sound RAM: a word the
 * float DSP wrote is read back from the copy, any other word is read from
 * sound RAM (decoded as the chip decodes it), so tables in RAM work. The
 * glue reports every CPU write to sound RAM (discard()); those words are
 * read from RAM again. A word written in one format (float or NOFL) and read
 * in the other is read from RAM, as the chip would read it.
 *
 * Seeding. On the first sample, after request_seed(), and whenever the
 * program or its parameters (MPRO, COEF, MADRS, RBP, RBL) change, the whole
 * state is taken from the chip instead of running: every datapath register,
 * TEMP, MEMS, the memory pipeline and the EFREG outputs are converted to
 * float and the ring copy is dropped. That sample's outputs are the chip's.
 *
 * The chip's state is only read, never written. C++11, no dependencies. */
#ifndef YAB_SOUND_HQ_DSP_FLOAT_H
#define YAB_SOUND_HQ_DSP_FLOAT_H

#include <stdint.h>
#include <atomic>
#include <vector>

/* What the float DSP reads from the chip's DSP (ScspDsp in scspdsp.h). The
 * glue fills it for each sample, after the chip has run its program. */
struct ScspHqDspView {
  const uint64_t* mpro;   /* 128 program words */
  const uint16_t* coef;   /* 64 coefficients, 13-bit two's complement */
  const uint16_t* madrs;  /* 32 delay addresses */
  uint32_t mdec_ct;       /* the ring offset this sample's program ran with */
  int rbp, rbl;           /* ring base (8 KB units) and length (0..3) */
  int last_step;          /* the number of steps the chip ran (its last_step) */
  /* the datapath after the chip ran this sample, used for seeding */
  const int32_t* temp;    /* 128, 24-bit */
  const int32_t* mems;    /* 32, 24-bit */
  const int16_t* efreg;   /* 16 */
  uint32_t shift_reg;     /* accumulator, 26-bit */
  int32_t inputs;         /* 24-bit */
  int32_t y_reg;          /* 24-bit */
  uint16_t frc_reg, adrs_reg;
  uint32_t read_value;    /* 24-bit */
  int read_pending;       /* 0, 1 float, 2 NOFL */
  int write_pending;
  uint32_t io_addr;       /* word address of the pending access */
};

class ScspHqFloatDsp {
public:
  enum { RAM_WORDS = 0x40000 };

  ScspHqFloatDsp();

  /* Take the whole state from the chip on the next sample. */
  void request_seed() { seed_request_ = true; }

  /* One sample. `mixs` are the previous sample's summed slot sends and
   * `exts` the external inputs, both +-1.0 full scale. `sound_ram` is the
   * 512 KB sound RAM in the emulator's layout (16-bit words in host order).
   * Returns the 16 EFREG outputs, +-1.0. */
  const float* run_sample(const ScspHqDspView& chip, const uint8_t* sound_ram,
                          const float mixs[16], const float exts[2]);

  /* The CPU wrote sound RAM words [first, first + count): read them from
   * RAM again. */
  void discard(uint32_t first, uint32_t count);

  /* Samples that were seeded instead of run (statistics). */
  uint32_t seed_count() const { return seed_count_.load(std::memory_order_relaxed); }

private:
  struct Step {
    uint8_t tra, twa, ysel, ira, iwa, ewa, cra, masa;
    bool twt, xsel, iwt, table, mwt, mrd, ewt, adrl, frcl, shft0, shft1, yrl, negb, zero, bsel,
        nofl, adreb, nxadr;
  };
  void decode(int last_step);
  bool program_changed(const ScspHqDspView& chip) const;
  void seed(const ScspHqDspView& chip);

  /* the program the steps were decoded from */
  uint64_t mpro_[128];
  uint16_t coef_[64];
  uint16_t madrs_[32];
  int rbp_, rbl_, last_step_;
  Step steps_[128];
  unsigned steps_run_;  /* the chip's last_step, 0..128 */
  uint16_t efreg_mask_; /* EFREG outputs the steps run write (EWT) */
  bool seed_request_;
  std::atomic<uint32_t> seed_count_; /* read from other threads (statistics) */

  float temp_[128], mems_[32], efreg_[16];
  float inputs_, acc_, y_;
  uint16_t frc_, adrs_;
  uint32_t rw_addr_;
  uint8_t read_pending_;  /* 1 float, 2 NOFL */
  bool write_pending_;
  bool write_from_chip_;  /* the pending write was seeded: the chip's own value is in RAM */
  uint8_t write_mode_;    /* 1 float, 2 NOFL */
  float write_value_, read_value_;

  std::vector<float> ring_;      /* by sound RAM word */
  std::vector<uint8_t> written_; /* 0: read RAM; 1: ring_ holds a float write; 2: a NOFL write */
};

#endif
