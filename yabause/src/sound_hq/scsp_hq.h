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

/* High-quality rendering of the SCSP's output (enhancement option, off by
 * default).
 *
 * The chip (the new SCSP in scsp.cpp) keeps running exactly as the hardware
 * does: its envelopes, LFOs, timers, loop points and DSP decide everything
 * the game and the sound driver can observe. This renderer takes what each
 * slot played in each sample (ScspHqVoiceTap) and renders it again without
 * the hardware's shortcuts:
 *  - band-limited interpolation of the waveforms (windowed sinc, Kaiser,
 *    8 zero crossings each side, with the cutoff lowered when a sample is
 *    played faster than its own rate) in place of the hardware's 2-point
 *    linear interpolation, which aliases when a waveform is pitched up and
 *    leaves images when it is pitched down;
 *  - the level, pan and mix in floating point, without the hardware's
 *    truncation at every stage.
 * Voices the renderer does not model (noise, FM, reverse and alternate
 * loops, short waves) use the chip's own sample, still mixed in floating
 * point. The external (CD) input is the chip's.
 *
 * The effects: with float_dsp off the chip's DSP outputs are mixed as they
 * are. With float_dsp on, the chip's DSP program runs a second time in
 * floating point (dsp_float.h), fed with these voices' float sends, and its
 * outputs replace the chip's.
 *
 * The output stays at 44.1 kHz. Levels and timing are the chip's, so the
 * result has the same loudness, balance and envelopes as the exact output.
 *
 * C++11, no dependencies. */
#ifndef YAB_SOUND_HQ_SCSP_HQ_H
#define YAB_SOUND_HQ_SCSP_HQ_H

#include <stdint.h>
#include <vector>

#include "dsp_float.h"

/* What one slot played in one sample. */
struct ScspHqVoiceTap {
  enum {
    SAMPLE = 0, /* use `sample` (noise, silence, FM, reverse loops, short waves) */
    WAVE = 1    /* waveform in sound RAM played forwards, normal loop or none */
  };
  uint8_t kind;
  bool key_on;            /* first sample after a key on */
  bool pcm8;
  bool loop;              /* normal loop (else none) */
  uint16_t lsa, lea;      /* loop start / end, in samples */
  uint16_t sb_xor;        /* SBCTL as an XOR mask */
  uint32_t sa;            /* start address, bytes */
  uint16_t pos;           /* read position, in samples ... */
  uint16_t phase;         /* ... and its 14-bit fraction */
  uint32_t step;          /* advance per output sample, 14-bit fraction */
  int16_t sample;         /* the chip's interpolated sample before the level (SAMPLE) */
  int16_t fx;             /* the DSP output (slots 0-15) or external input (16, 17) this slot mixes */
  uint16_t att;           /* EG + TL + ALFO, 0..0x3FF; 0xFFFF: SDIR, no level at all */
  int16_t direct_vol[2];  /* 1.14, DISDL/DIPAN */
  int16_t effect_vol[2];  /* 1.14, EFSDL/EFPAN */
  uint8_t isel, imxl;     /* DSP send: input, level (0 = none) */
};

struct ScspHqTaps {
  ScspHqVoiceTap v[32];
  uint16_t master;        /* 1.15 (0x8000 = MVOL 15) */
  int16_t exts[2];        /* the external inputs this sample */
  /* the chip's DSP this sample; read only with float_dsp */
  const ScspHqDspView* dsp;
  int32_t chip_mixs[16];  /* the sends the chip's program ran with (20-bit) */
};

/* Windowed-sinc lowpass with cutoff 1 (Nyquist), evaluated at any x. */
class ScspHqSincKernel {
public:
  ScspHqSincKernel(int half_width, double kaiser_beta);
  int half_width() const { return width_; }
  float operator()(float x) const {
    x = x < 0 ? -x : x;
    if (x >= (float)width_) return 0.0f;
    const float f = x * RES;
    const int i = (int)f;
    return table_[i] + (table_[i + 1] - table_[i]) * (f - (float)i);
  }

private:
  enum { RES = 512 };
  int width_;
  std::vector<float> table_;
};

class ScspHqRenderer {
public:
  ScspHqRenderer();

  /* Forget the per-voice history (state load, reset, enabling the mode).
   * A looped voice met again past LSA is then taken to have wrapped already:
   * true for any voice that has played its loop once, which is what a voice
   * still sounding at a state load or an enable usually has done. */
  void reset();

  /* One 44.1 kHz stereo sample on the exact output's scale (32768 = full
   * scale, before the DAC's clipping). `sound_ram` is the 512 KB sound RAM
   * in the emulator's in-memory layout (16-bit words in host order). */
  void render(const ScspHqTaps& taps, const uint8_t* sound_ram, float out[2]);

  /* Run the effect DSP in floating point (taps.dsp must be set). Turning it
   * on, and reset(), take the float DSP's state from the chip. */
  bool float_dsp;
  /* Developer verification: feed the float DSP with the chip's own sends
   * (taps.chip_mixs) instead of the float ones, so that its outputs differ
   * from the chip's only by the DSP's arithmetic. */
  bool float_dsp_chip_sends;

  /* The CPU wrote sound RAM words [first, first + count). */
  void discard_ram(uint32_t first, uint32_t count) { fx_.discard(first, count); }

  /* With float_dsp, the last sample's effect returns (EFREG through EFSDL,
   * EFPAN and the master volume, on the output's scale): the chip's, and the
   * float DSP's that replaced them. */
  void last_effect_mix(float chip[2], float flt[2]) const {
    chip[0] = fx_mix_[0][0];
    chip[1] = fx_mix_[0][1];
    flt[0] = fx_mix_[1][0];
    flt[1] = fx_mix_[1][1];
  }
  uint32_t float_dsp_seed_count() const { return fx_.seed_count(); }

private:
  float voice(const ScspHqVoiceTap& t, const uint8_t* ram, bool wrapped) const;

  struct VoiceState {
    uint16_t last_pos;
    bool wrapped; /* has looped at least once: the history before LSA is the loop's end */
    bool fresh;   /* no tap seen since reset(): `wrapped` is primed from the first one */
  } state_[32];
  ScspHqSincKernel kernel_;
  ScspHqFloatDsp fx_;
  bool fx_running_;     /* float_dsp was on in the previous sample */
  float sends_[16];     /* this sample's float DSP sends, for the next sample */
  float fx_mix_[2][2];  /* [chip, float][L, R] */
};

/* The DAC's clipping and TPDF dither to 16 bits. A value that rounds to 0
 * (|v| < 0.5, which includes digital silence and voices decayed below one
 * LSB) gives exactly 0, as the chip does, instead of dither noise. */
class ScspHqQuantizer {
public:
  ScspHqQuantizer() : seed_(1) {}
  int16_t operator()(float v);

private:
  uint32_t seed_;
};

#endif
