/*  Copyright 2004 Stephane Dallongeville
    Copyright 2004-2006 Theo Berkau

    This file is part of Yabause.

    Yabause is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Yabause is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Yabause; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/
/*
        Copyright 2019 devMiyax(smiyaxdev@gmail.com)

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

// Quick hack so nobody else needs to know we're using a different header file
#ifdef USE_SCSP2
#include "scsp2.h"  // defines SCSP_H
#endif

#ifndef SCSP_H
#define SCSP_H

#include "core.h"

#if defined (__cplusplus)
extern "C"{
#endif

#define SNDCORE_DEFAULT -1
#define SNDCORE_DUMMY   0
#define SNDCORE_WAV     10 // should really be 1, but I'll probably break people's stuff

#define SCSP_MUTE_SYSTEM    1
#define SCSP_MUTE_USER      2

typedef struct
{
   int id;
   const char *Name;
   int (*Init)(void);
   void (*DeInit)(void);
   int (*Reset)(void);
   int (*ChangeVideoFormat)(int vertfreq);
   void (*UpdateAudio)(u32 *leftchanbuffer, u32 *rightchanbuffer, u32 num_samples);
   u32 (*GetAudioSpace)(void);
   void (*MuteAudio)(void);
   void (*UnMuteAudio)(void);
   void (*SetVolume)(int volume);
#ifdef USE_SCSPMIDI
	int (*MidiChangePorts)(int inport, int outport);
	u8 (*MidiIn)(int *isdata);
	int (*MidiOut)(u8 data);
#endif
} SoundInterface_struct;

typedef struct
{
   u32 D[8];
   u32 A[8];
   u32 SR;
   u32 PC;
} m68kregs_struct;

typedef struct
{
  u32 addr;
} m68kcodebreakpoint_struct;

#define MAX_BREAKPOINTS 10

#if !defined(IOS) // iPhone is too fast 
#define ASYNC_SCSP
#endif  

typedef struct
{
  u32 scsptiming1;
  u32 scsptiming2;  // 16.16 fixed point
  m68kcodebreakpoint_struct codebreakpoint[MAX_BREAKPOINTS];
  int numcodebreakpoints;
  void (*BreakpointCallBack)(u32);
  int inbreakpoint;
} ScspInternal;

extern SoundInterface_struct SNDDummy;
extern SoundInterface_struct SNDWave;
extern u8 *SoundRam;
extern int use_new_scsp;

u8 FASTCALL SoundRamReadByte(u32 addr);
u16 FASTCALL SoundRamReadWord(u32 addr);
u32 FASTCALL SoundRamReadLong(u32 addr);
void FASTCALL SoundRamWriteByte(u32 addr, u8 val);
void FASTCALL SoundRamWriteWord(u32 addr, u16 val);
void FASTCALL SoundRamWriteLong(u32 addr, u32 val);

int ScspInit(int coreid, int scsp_sync_count_per_frame, int scsp_main_mode);
int ScspChangeSoundCore(int coreid);
void ScspDeInit(void);
void M68KStart(void);
void M68KStop(void);
int M68KIsRunning(void);
void ScspReset(void);
int ScspChangeVideoFormat(int type);
void M68KExec(s32 cycles);
void ScspExec(void);
void ScspConvert32uto16s(s32 *srcL, s32 *srcR, s16 *dst, u32 len);
void ScspReceiveCDDA(const u8 *sector);
int SoundSaveState(FILE *fp);
int SoundLoadState(FILE *fp, int version, int size);
void ScspSlotDebugStats(u8 slotnum, char *outstring);
void ScspCommonControlRegisterDebugStats(char *outstring);
int ScspSlotDebugSaveRegisters(u8 slotnum, const char *filename);
u32 ScspSlotDebugAudio (u32 *workbuf, s16 *buf, u32 len);
void ScspSlotResetDebug(u8 slotnum);
int ScspSlotDebugAudioSaveWav(u8 slotnum, const char *filename);
void ScspMuteAudio(int flags);
void ScspUnMuteAudio(int flags);
void ScspSetVolume(int volume);
void ScspAsynMain(void * p);
void ScspExecAsync();
void FASTCALL scsp_w_b(u32, u8);
void FASTCALL scsp_w_w(u32, u16);
void FASTCALL scsp_w_d(u32, u32);
u8 FASTCALL scsp_r_b(u32);
u16 FASTCALL scsp_r_w(u32);
u32 FASTCALL scsp_r_d(u32);

void scsp_init(u8 *scsp_ram, void (*sint_hand)(u32), void (*mint_hand)(void));
void scsp_shutdown(void);
void scsp_reset(void);

void scsp_midi_in_send(u8 data);
void scsp_midi_out_send(u8 data);
u8 scsp_midi_in_read(void);
u8 scsp_midi_out_read(void);
void scsp_update(s32 *bufL, s32 *bufR, u32 len);
void scsp_update_monitor(void);
void scsp_update_timer(u32 len);

u32 FASTCALL c68k_word_read(const u32 adr);

void M68KStep(void);
void M68KSync(void);
void M68KWriteNotify(u32 address, u32 size);
void M68KGetRegisters(m68kregs_struct *regs);
void M68KSetRegisters(m68kregs_struct *regs);
void M68KSetBreakpointCallBack(void (*func)(u32));
int M68KAddCodeBreakpoint(u32 addr);
void M68KSortCodeBreakpoints(void);
int M68KDelCodeBreakpoint(u32 addr);
m68kcodebreakpoint_struct *M68KGetBreakpointList(void);
void M68KClearCodeBreakpoints(void);

void scsp_debug_instrument_get_data(int i, u32 * sa, int * is_muted);
void scsp_debug_instrument_set_mute(u32 sa, int mute);
void scsp_debug_instrument_clear();
void scsp_debug_get_envelope(int chan, int * env, int * state);
void scsp_debug_get_tl(int chan, int * tl, int * dldisdl, int * imxl, int * efsdl );
void scsp_debug_set_mode(int mode);
void scsp_set_use_new(int which);
void new_scsp_exec(s32 cycles);
int scsp_debug_get_mvol();

// Extended debug API for SCSP debugging (Issue #73)
#define SCSP_ENV_ATTACK   0
#define SCSP_ENV_DECAY1   1
#define SCSP_ENV_DECAY2   2
#define SCSP_ENV_RELEASE  3

typedef struct {
   // Key state
   u8 key_on;           // kx: key execute
   u8 key_bit;          // kb: key bit
   u8 is_active;        // calculated: slot is producing sound

   // Source settings
   u8 source_bit_ctrl;  // sbctl
   u8 source_select;    // ssctl: 0=external DRAM, 1=noise, 2=zero
   u8 loop_ctrl;        // lpctl: 0=none, 1=normal, 2=reverse, 3=alternating
   u8 pcm_8bit;         // pcm8b: 1=8bit, 0=16bit

   // Address
   u32 start_addr;      // sa: sample start address
   u16 loop_start;      // lsa: loop start address
   u16 loop_end;        // lea: loop end address
   u32 current_addr;    // current address pointer

   // Envelope (ADSR)
   u8 attack_rate;      // ar
   u8 decay1_rate;      // d1r
   u8 decay2_rate;      // d2r
   u8 release_rate;     // rr
   u8 decay_level;      // dl
   u8 key_rate_scale;   // krs
   u8 envelope_state;   // current state: ATTACK/DECAY1/DECAY2/RELEASE
   u16 envelope_level;  // current attenuation (0=max vol, 0x3FF=silent)

   // Pitch
   u8 octave;           // oct
   u16 frequency_num;   // fns
   s32 phase_value;     // current waveform phase

   // LFO
   u8 lfo_frequency;    // lfof
   u8 pitch_lfo_wave;   // plfows
   u8 pitch_lfo_shift;  // plfos
   u8 amp_lfo_wave;     // alfows
   u8 amp_lfo_shift;    // alfos
   u32 lfo_position;    // current LFO position

   // Level
   u16 total_level;     // tl
   u8 direct_level;     // disdl
   u8 direct_pan;       // dipan
   u8 effect_level;     // efsdl
   u8 effect_pan;       // efpan

   // Modulation
   u8 mod_level;        // mdl
   u8 mod_x_select;     // mdxsl
   u8 mod_y_select;     // mdysl

   // Output
   s16 current_output;  // current sample output
   u8 is_muted;         // mute flag
} ScspSlotDebugInfo;

// Get comprehensive slot state
void scsp_debug_get_slot_state(int slot, ScspSlotDebugInfo *info);

// Check if slot is currently active (producing sound)
int scsp_debug_is_slot_active(int slot);

// Get output levels for slot (for level meters)
void scsp_debug_get_output_level(int slot, s16 *level_l, s16 *level_r);

// Get sample buffer for waveform display (returns actual sample count)
int scsp_debug_get_sample_buffer(int slot, s16 *buffer, int max_samples);

void ScspLockThread();
void ScspUnLockThread();
void setM68kCounter(u64 counter);
void setM68kDoneCounter(u64 counter);
void ScspCommitSh2SoundRamWrites(void);

extern int use_new_scsp;

// High-quality output of the new SCSP (sound_hq/scsp_hq.h): band-limited
// waveform interpolation and a floating-point mix, rendered from what the
// chip played. Off by default; the chip itself is unchanged either way. Has
// no effect with the old SCSP (use_new_scsp == 0). Any thread.
void ScspSetHighQuality(int on);
int ScspGetHighQuality(void);

// Developer hook: while the high-quality output is on, receives for each
// sample the chip's own output and the high-quality output that replaces it
// (SCSP thread).
extern void (*scsp_hq_chip_output_hook)(s16 chip_l, s16 chip_r, s16 hq_l, s16 hq_r);

// Float effect DSP of the high-quality output (sound_hq/dsp_float.h): the
// chip's DSP program runs a second time in floating point, fed with the
// high-quality voices, and its outputs replace the chip's. 0 off (default),
// 1 on, 2 on but fed with the chip's own sends (developer verification of the
// DSP arithmetic). Only acts while ScspSetHighQuality(1). Any thread.
void ScspSetHighQualityFloatDsp(int mode);
int ScspGetHighQualityFloatDsp(void);
// Developer statistics: samples where the float DSP took its state from the
// chip instead of running (first sample, program or parameter changes).
u32 ScspHqGetFloatDspSeedCount(void);

// Developer hook: while the float DSP is on, receives for each sample the
// effect returns (EFREG through EFSDL/EFPAN and the master volume, on the
// output's scale) of the chip's DSP and of the float DSP (SCSP thread).
extern void (*scsp_hq_fx_output_hook)(const float chip[2], const float flt[2]);

// Developer statistics, counted only while the high-quality output is on:
// per slot, key ons executed by keyon(), key ons the taps carried, and op2
// captures that saw sample_counter == 0 (the earlier detection).
void ScspHqGetKeyOnStats(u32 keyon_calls[32], u32 tap_keyons[32], u32 counter_zero[32]);

#if defined (__cplusplus)
}
#endif


#endif
