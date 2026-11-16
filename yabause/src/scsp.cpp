/*
 * Copyright 2004 Stephane Dallongeville
 * Copyright 2004-2007 Theo Berkau
 * Copyright 2006 Guillaume Duhamel
 * Copyright 2012 Chris Lord
 *
 * This file is part of Yabause.
 *
 * Yabause is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Yabause is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Yabause; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
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

/*! \file scsp.c
    \brief SCSP emulation functions.
*/

////////////////////////////////////////////////////////////////
// Custom Sound Processor

// note: model2 scsp is mapped to 0x100000~0x100ee4 of the space, but seems to
//       have additional hw ports ($40a~$410)
// note: it seems that the user interrupt is used by the sound driver to reset
//       the subsystem

//--------------------------------------------------------------
//
// Common Control Register (CCR)
//
//      $+00      $+01
// $400 ---- --12 3333 4444 1:MEM4MB memory size 2:DAC18B dac for digital output 3:VER version number 4:MVOL
// $402 ---- ---1 1222 2222 1:RBL ring buffer length 2:RBP lead address
// $404 ---1 2345 6666 6666 1:MOFULL out fifo full 2:MOEMP empty 3:MIOVF overflow 4:MIFULL in 5:MIEMP 6:MIBUF
// $406 ---- ---- 1111 1111 1:MOBUF midi output data buffer
// $408 1111 1222 2334 4444 1:MSLC monitor slot 2:CA call address 3:SGC Slot phase 4:EG Slot envelope
// $40a ---- ---- ---- ----
// $40c ---- ---- ---- ----
// $40e ---- ---- ---- ----
// $410 ---- ---- ---- ----
// $412 1111 1111 1111 111- 1:DMEAL transfer start address (sound)
// $414 1111 2222 2222 222- 1:DMEAH transfer start address hi 2:DRGA start register address (dsp)
// $416 -123 4444 4444 444- 1:DGATE transfer gate 0 clear 2:DDIR direction 3:DEXE start 4:DTLG data count
// $418 ---- -111 2222 2222 1:TACTL timer a prescalar control 2:TIMA timer a count data
// $41a ---- -111 2222 2222 1:TBCTL timer b prescalar control 2:TIMB timer b count data
// $41c ---- -111 2222 2222 2:TCCTL timer c prescalar control 2:TIMC timer c count data
// $41e ---- -111 1111 1111 1:SCIEB allow sound cpu interrupt
// $420 ---- -111 1111 1111 1:SCIPD request sound cpu interrupt
// $422 ---- -111 1111 1111 1:SCIRE reset sound cpu interrupt
// $424 ---- ---- 1111 1111 1:SCILV0 sound cpu interrupt level bit0
// $426 ---- ---- 1111 1111 1:SCILV1 sound cpu interrupt level bit1
// $428 ---- ---- 1111 1111 1:SCILV2 sound cpu interrupt level bit2
// $42a ---- -111 1111 1111 1:MCIEB allow main cpu interrupt
// $42c ---- -111 1111 1111 1:MCIPD request main cpu interrupt
// $42e ---- -111 1111 1111 1:MCIRE reset main cpu interrupt
//
//--------------------------------------------------------------
//
// Individual Slot Register (ISR)
//
//     $+00      $+01
// $00 ---1 2334 4556 7777 1:KYONEX 2:KYONB 3:SBCTL 4:SSCTL 5:LPCTL 6:PCM8B 7:SA start address
// $02 1111 1111 1111 1111 1:SA start address
// $04 1111 1111 1111 1111 1:LSA loop start address
// $06 1111 1111 1111 1111 1:LEA loop end address
// $08 1111 1222 2234 4444 1:D2R decay 2 rate 2:D1R decay 1 rate 3:EGHOLD eg hold mode 4:AR attack rate
// $0a -122 2233 3334 4444 1:LPSLNK loop start link 2:KRS key rate scaling 3:DL decay level 4:RR release rate
// $0c ---- --12 3333 3333 1:STWINH stack write inhibit 2:SDIR sound direct 3:TL total level
// $0e 1111 2222 2233 3333 1:MDL modulation level 2:MDXSL modulation input x 3:MDYSL modulation input y
// $10 -111 1-22 2222 2222 1:OCT octave 2:FNS frequency number switch
// $12 1222 2233 4445 5666 1:LFORE 2:LFOF 3:PLFOWS 4:PLFOS 5:ALFOWS 6:ALFOS
// $14 ---- ---- -111 1222 1:ISEL input select 2:OMXL input mix level
// $16 1112 2222 3334 4444 1:DISDL 2:DIPAN 3:EFSDL 4:EFPAN
//
//--------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <limits.h>
// Before the min/max macros below: it pulls in <vector>.
#include "sound_hq/scsp_hq.h"
#include "sound_hq/ram_watch.h"
#ifdef _MSC_VER
#include <intrin.h>  // _BitScanForward (MSVC has no __builtin_ctz)
#endif

#include "c68k/c68k.h"
#include "cs2.h"
#include "debug.h"
#include "error.h"
#include "memory.h"
#include "m68kcore.h"
#include "scu.h"
#include "yabause.h"
#include "scsp.h"
#include "scspdsp.h"
#include "sh2cache.h"
#include "threads.h"
#include <atomic>
#include <mutex>

#ifndef max
#define max(a,b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif


#if 0
#include "windows/aviout.h"
#endif

#ifdef PSP
# include "psp/common.h"

/* Macro to write a variable's value through the cache to main memory */
# define WRITE_THROUGH(var)  (*(u32 *)((u32)&(var) | 0x40000000) = (var))

/* Macro to flush SCSP state so it can be read from the ME */
# define FLUSH_SCSP()  sceKernelDcacheWritebackRange(&scsp, sizeof(scsp))

#else  // !PSP
# define WRITE_THROUGH(var)  /*nothing*/
# define FLUSH_SCSP()  /*nothing*/
#endif

#if defined(ARCH_IS_LINUX)
#include "sys/resource.h"
#include <errno.h>
#include <pthread.h>
pthread_cond_t  sync_cnd = PTHREAD_COND_INITIALIZER;
pthread_mutex_t sync_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

int use_new_scsp = 0;
int new_scsp_outbuf_pos = 0;
s32 new_scsp_outbuf_l[900] = { 0 };
s32 new_scsp_outbuf_r[900] = { 0 };
int new_scsp_cycles = 0;
int g_scsp_lock = 0;
YabMutex * g_scsp_mtx = NULL;
static int g_scsp_sync_count_per_frame = 1;
static int g_scsp_main_mode = 0;

#include "sh2core.h"

std::atomic<u32> m68kcycle = 0;

// scsp_main_mode==0 (CPU-time synced) runs the M68K on the SCSP thread in
// 256-cycle chunks while the SH2 runs concurrently on the emulation thread.
// Without ordering, a M68K store issued at an EARLIER emulated time can land
// in host memory AFTER a LATER SH2 store to the same sound RAM address,
// corrupting mailbox handshakes (e.g. Techno Motor request flag at 0x4E0).
// m68k_done_upto publishes, in current-frame m68k cycles, how far the SCSP
// thread has FULLY executed the M68K. Before the SH2 touches sound RAM it
// waits until this reaches the current m68k counter, so host memory order
// matches emulated order. Both values reset each frame; the comparison is
// race-free because the waiting thread (emulation thread) is the only
// writer of the counter.
std::atomic<u64> m68k_done_upto(0);
// set while the mode-0 thread is parked at the frame-boundary handshake;
// the SH2 drain must not wait in that state (the SCSP thread cannot run
// until the emulation thread reaches SyncCPUtoSCSP)
std::atomic<int> m68k_at_frame_boundary(0);

extern "C" {
  extern volatile u64 saved_m68k_cycles;
  extern YabEventQueue * q_scsp_frame_start;
  extern YabEventQueue * q_scsp_finish;
  extern void setM68kCounter(u64 counter);
  extern u64 getM68KCounter();
  extern void SyncSh2And68k();
}


#define CLOCK_SYNC_SHIFT (4)

enum EnvelopeStates
{
   ATTACK,
   DECAY1,
   DECAY2,
   RELEASE
};

//0x30 to 0x3f only
const u8 attack_rate_table[][4] =
{
   { 4,4,4,4 },//0x30
   { 3,4,4,4 },
   { 3,4,3,4 },
   { 3,3,3,4 },

   { 3,3,3,3 },
   { 2,3,3,3 },
   { 2,3,2,3 },
   { 2,2,2,3 },

   { 2,2,2,2 },
   { 1,2,2,2 },
   { 1,2,1,2 },
   { 1,1,1,2 },

   { 1,1,1,1 },//0x3c
   { 1,1,1,1 },//0x3d
   { 1,1,1,1 },//0x3e
   { 1,1,1,1 },//0x3f
};

const u8 decay_rate_table[][4] =
{
   { 1,1,1,1 },//0x30
   { 2,1,1,1 },
   { 2,1,2,1 },
   { 2,2,2,1 },

   { 2,2,2,2 },
   { 4,2,2,2 },
   { 4,2,4,2 },
   { 4,4,4,2 },

   { 4,4,4,4 },
   { 8,4,4,4 },
   { 8,4,8,4 },
   { 8,8,8,4 },

   { 8,8,8,8 },//0x3c
   { 8,8,8,8 },//0x3d
   { 8,8,8,8 },//0x3e
   { 8,8,8,8 },//0x3f
};

#define EFFECTIVE_RATE_END 0xffff

//effective rate step table
//settings 0x2 to 0x5, then repeats with bigger shifts
//EFFECTIVE_RATE_END represents going back to the beginning of a row since
//the number of steps is not the same for each setting
#define MAKE_TABLE(SHIFT) \
   { 8192 >> SHIFT, 4096 >> SHIFT, 4096 >> SHIFT, EFFECTIVE_RATE_END,EFFECTIVE_RATE_END,EFFECTIVE_RATE_END,EFFECTIVE_RATE_END, EFFECTIVE_RATE_END }, \
   { 8192 >> SHIFT, 4096 >> SHIFT, 4096 >> SHIFT, 4096 >> SHIFT, 4096 >> SHIFT, 4096 >> SHIFT, 4096 >> SHIFT, EFFECTIVE_RATE_END }, \
   { 4096 >> SHIFT, EFFECTIVE_RATE_END,EFFECTIVE_RATE_END,EFFECTIVE_RATE_END,EFFECTIVE_RATE_END, EFFECTIVE_RATE_END,EFFECTIVE_RATE_END , EFFECTIVE_RATE_END }, \
   { 4096 >> SHIFT, 4096 >> SHIFT, 4096 >> SHIFT, 2048 >> SHIFT, 2048 >> SHIFT, EFFECTIVE_RATE_END, EFFECTIVE_RATE_END, EFFECTIVE_RATE_END },

const u16 envelope_table[][8] =
{
   MAKE_TABLE(0)
   MAKE_TABLE(1)
   MAKE_TABLE(2)
   MAKE_TABLE(3)
   MAKE_TABLE(4)
   MAKE_TABLE(5)
   MAKE_TABLE(6)
   MAKE_TABLE(7)
   MAKE_TABLE(8) 
   MAKE_TABLE(9)
   MAKE_TABLE(10)
   MAKE_TABLE(11)
   MAKE_TABLE(12)
};

//unknown stored bits are unknown1-5
struct SlotRegs
{
   u8 kx;
   u8 kb;
   u8 sbctl;
   u8 ssctl;
   u8 lpctl;
   u8 pcm8b;
   u32 sa;
   u16 lsa;
   u16 lea;
   u8 d2r;
   u8 d1r;
   u8 hold;
   u8 ar;
   u8 unknown1;
   u8 ls;
   u8 krs;
   u8 dl;
   u8 rr;
   u8 unknown2;
   u8 si;
   u8 sd;
   u16 tl;
   u8 mdl;
   u8 mdxsl;
   u8 mdysl;
   u8 unknown3;
   u8 oct;
   u8 unknown4;
   u16 fns;
   u8 shortwave; // register $08 bit15: short-wave wrap for small loops (scsp3 ShortWave)
   u8 re;
   u8 lfof;
   u8 plfows;
   u8 plfos;
   u8 alfows;
   u8 alfos;
   u8 unknown5;
   u8 isel;
   u8 imxl;
   u8 disdl;
   u8 dipan; 
   u8 efsdl;
   u8 efpan;
};

struct SlotState
{
   u16 wave;
   int backwards;
   enum EnvelopeStates envelope;
   s16 output;
   u16 attenuation;
   int step_count;
   u32 sample_counter;
   u32 envelope_steps_taken;
   s32 waveform_phase_value;
   s32 sample_offset;
   u32 address_pointer;
   u32 lfo_counter;
   u32 lfo_pos;          // doubles as the 8-bit LFOCounter for the scsp3-style LFO

   // 6-bit sub-sample fraction (sia) captured in op2 for linear interpolation in op3
   u32 sample_frac;

   // Down-counter that paces LFOCounter advance (scsp3 LFOTimeCounter)
   u32 lfo_time_counter;

   // Playback has reached the loop start (scsp3 InLoop); used for LPSLNK attack->decay link
   int in_loop;

   // Set on key-on so op4 skips the envelope step for the key-on sample: scsp3 runs RunEG before
   // executing the key-on, so the key-on sample itself never advances the attack (else the
   // envelope runs one step ahead of scsp3 from the very first sample of every note).
   int eg_keyon_skip;

   // Phase fraction captured BEFORE op1 adds this sample's increment. scsp3 reads the waveform with
   // the pre-increment phase, so op2 uses this (not the post-increment fraction) for interpolation.
   u32 read_phase_frac;

   // Raw sample index (sample_offset + FM offset) before ShortWaveMask, used by op3
   s32 wave_index;

   int num;
   int is_muted;
};

struct Slot
{
   //registers
   struct SlotRegs regs;

   //internal state
   struct SlotState state;
};

struct Scsp
{
   // scsp3 modulation ring (SoundStack[64]): op7 commits each slot's output here through the
   // 4-sample delay line below; op2 reads the FM modulator taps from it.
   u16 sound_stack[64];
   struct Slot slots[32];

   int debug_mode;

   // scsp3 SoundStackDelayer[4]: 4-sample delay line for the modulation ring.
   s16 sound_stack_delayer[4];
   // Free-running counter incremented once per op7, tracking scsp3's GlobalCounter
   // (GC == 32*sample + slot). op7 writes/inhibits via GC; op2 reads via (GC + 5) to absorb the
   // 5-stage op2->op7 pipeline gap.
   // NOTE: a residual parity-phase skew in this ring leaves FM cross-modulation with a Nyquist
   // buzz; resolving it (correcting the delayer's parity placement) is tracked as a follow-up.
   u32 global_counter;
   u32 frame;
}new_scsp;

// 17-bit Galois LFSR shared by all slots for noise source and noise LFO (scsp3 RunSample).
static u32 new_scsp_lfsr = 1;

//samples per step through a 256 entry lfo table
const int lfo_step_table[0x20] = {
   0x3fc,//0
   0x37c,//1
   0x2fc,//2
   0x27c,//3
   0x1fc,//4
   0x1bc,//5
   0x17c,//6
   0x13c,//7
   0x0fc,//8
   0x0bc,//9 
   0x0dc,//0xa 
   0x08c,//0xb
   0x07c,//0xc
   0x06c,//0xd
   0x05c,//0xe
   0x04c,//0xf

   0x03c,//0x10
   0x034,//0x11
   0x02c,//0x12
   0x024,//0x13
   0x01c,//0x14
   0x018,//0x15
   0x014,//0x16
   0x010,//0x17
   0x00c,//0x18
   0x00a,//0x19
   0x008,//0x1a
   0x006,//0x1b
   0x004,//0x1c
   0x003,//0x1d
   0x002,//0x1e
   0x001,//0x1f
};

struct PlfoTables
{
   s8 saw_table[256];
   s8 square_table[256];
   s8 tri_table[256];
   s8 noise_table[256];
};

struct PlfoTables plfo;

struct AlfoTables
{
   u8 saw_table[256];
   u8 square_table[256];
   u8 tri_table[256];
   u8 noise_table[256];
};

struct AlfoTables alfo;

void scsp_main_interrupt (u32 id);
void scsp_sound_interrupt (u32 id);

void fill_plfo_tables()
{
   int i;

   //saw
   for (i = 0; i < 256; i++)
   {
      if (i < 128)
         plfo.saw_table[i] = i;
      else
         plfo.saw_table[i] = -256 + i;
   }

   //square
   for (i = 0; i < 256; i++)
   {
      if (i < 128)
         plfo.square_table[i] = 127;
      else
         plfo.square_table[i] = -128;
   }

   //triangular
   for (i = 0; i < 256; i++)
   {
      if (i < 64)
         plfo.tri_table[i] = i * 2;
      else if (i < 192)
         plfo.tri_table[i] = 255 - (i * 2);
      else
         plfo.tri_table[i] = (i * 2) - 512;
   }

   //noise
   for (i = 0; i < 256; i++)
   {
      plfo.noise_table[i] = rand() & 0xff;
   }
}

void fill_alfo_tables()
{
   int i;

   //saw
   for (i = 0; i < 256; i++)
   {
      alfo.saw_table[i] = i;
   }

   //square
   for (i = 0; i < 256; i++)
   {
      if (i < 128)
         alfo.square_table[i] = 0;
      else
         alfo.square_table[i] = 0xff;
   }

   //triangular
   for (i = 0; i < 256; i++)
   {
      if (i < 128)
         alfo.tri_table[i] = i * 2;
      else
         alfo.tri_table[i] = 255 - (i * 2);
   }

   //noise
   for (i = 0; i < 256; i++)
   {
      alfo.noise_table[i] = rand() & 0xff;
   }
}

// Forward declaration for debug sample recording (Issue #73)
void scsp_debug_record_sample(int slot, s16 sample);

// Pitch LFO: waveform derived directly from the 8-bit LFOCounter, scaled relative to
// the slot frequency (scsp3 GetPLFO).
static int new_scsp_get_plfo(struct Slot * slot)
{
   u8 c = (u8)slot->state.lfo_pos;
   int ret;

   switch (slot->regs.plfows)
   {
   default:
   case 0: // saw
      ret = (s8)(c & ~1);
      break;
   case 1: // square
      ret = (s8)((c & 0x80) ? 0x80 : 0x7E);
      break;
   case 2: // triangle
      ret = (s8)(((c & 0x3F) ^ ((c & 0x40) ? 0x3F : 0x00) ^ ((c & 0x80) ? 0x7F : 0x00)) << 1);
      break;
   case 3: // noise
      ret = (s8)(new_scsp_lfsr & ~1);
      break;
   }

   ret >>= (7 - slot->regs.plfos);
   if (!slot->regs.plfos)
      ret = 0;

   ret = ((0x40 ^ (slot->regs.fns >> 4)) * ret) >> 6;
   return ret;
}

// Amplitude LFO: returns an attenuation increment (scsp3 GetALFO).
static u8 new_scsp_get_alfo(struct Slot * slot)
{
   u8 c = (u8)slot->state.lfo_pos;
   u8 ret;

   switch (slot->regs.alfows)
   {
   default:
   case 0: // saw
      ret = c & ~1;
      break;
   case 1: // square
      ret = ((s8)c >> 7) & ~1;
      break;
   case 2: // triangle
      ret = (u8)((unsigned)(c ^ ((s8)c >> 7)) << 1);
      break;
   case 3: // noise
      ret = new_scsp_lfsr & ~1;
      break;
   }

   ret >>= (7 - slot->regs.alfos);
   if (!slot->regs.alfos)
      ret = 0;

   return ret;
}

// Advance the LFOCounter at the rate set by LFOF, with optional reset (scsp3 RunLFO).
static void new_scsp_run_lfo(struct Slot * slot)
{
   slot->state.lfo_time_counter--;
   if (!slot->state.lfo_time_counter)
   {
      slot->state.lfo_pos = (slot->state.lfo_pos + 1) & 0xFF;
      slot->state.lfo_time_counter =
         (((8 - (slot->regs.lfof & 0x3)) << 7) >> (slot->regs.lfof >> 2)) - 4;
   }

   if (slot->regs.re)
      slot->state.lfo_pos = 0;
}

//pg, plfo
void op1(struct Slot * slot)
{
   u32 oct = slot->regs.oct ^ 8;
   s32 base;

   if (slot->state.attenuation >= 0x3bf)
      return;

   // Frequency increment with pitch LFO folded in (scsp3 RunSample). The new pipeline keeps
   // 18 fractional bits, which already absorbs scsp3's trailing >>4 on its 14-bit phase.
   base = (s32)(0x400 ^ slot->regs.fns) + new_scsp_get_plfo(slot);

   slot->state.waveform_phase_value &= (1 << 18) - 1;//18 fractional bits
   // Capture the pre-increment phase fraction for op2's interpolation (scsp3 reads before incrementing).
   slot->state.read_phase_frac = (slot->state.waveform_phase_value >> 12) & 0x3F;
   // scsp3 truncates the low 4 bits of the increment (it adds (incr >> 4) to a
   // 14-bit phase). The new pipeline keeps 18 fractional bits, so to stay
   // bit-aligned with scsp3 the same low 4 bits must be discarded here, else the
   // phase drifts ahead by the accumulated remainder.
   slot->state.waveform_phase_value += (base << oct) & ~0xF;

   // Advance the LFO between PLFO use (here) and ALFO use (op5).
   new_scsp_run_lfo(slot);
}

int get_slot(struct Slot * slot, int mdsl)
{
   return (mdsl + slot->state.num) & 0x1f;
}

// Count trailing zeros, portable across GCC/Clang and MSVC.
static inline unsigned new_scsp_ctz32(unsigned x)
{
#ifdef _MSC_VER
   unsigned long idx;
   _BitScanForward(&idx, x);
   return (unsigned)idx;
#else
   return (unsigned)__builtin_ctz(x);
#endif
}

// Short-wave wrap mask for small loops (scsp3 RecalcShortWaveMask). Returns all-ones (no wrap)
// unless the ShortWave bit is set and the loop end selects a short region.
static u32 new_scsp_shortwave_mask(struct Slot * slot)
{
   unsigned lev = slot->regs.lea & 0x780;

   if (slot->regs.shortwave && lev)
      return (1u << new_scsp_ctz32(lev)) - 1;

   return 0xFFFFFFFF;
}


//address pointer calculation
//modulation data read
void op2(struct Slot * slot, struct Scsp * s)
{
   s32 md_out = 0;
   s32 sample_delta = slot->state.waveform_phase_value >> 18;

   if (slot->state.attenuation >= 0x3bf)
      return;

   // Use the pre-increment phase fraction (captured in op1) for interpolation, matching scsp3.
   slot->state.sample_frac = slot->state.read_phase_frac;

   if (slot->regs.mdl)
   {
      // FM modulation (scsp3 RunSample): read the two modulator taps from SoundStack at
      // gc + ModInputX/Y. gc must equal scsp3's GlobalCounter at this slot's read; the harness
      // (tools/scsp3) showed the read index was +6 vs scsp3 with the old +5, so the correct
      // offset relative to the op7-advanced global_counter is -1.
      u32 gc = s->global_counter - 1;
      u32 px = (gc + slot->regs.mdxsl) & 0x3F;
      u32 py = (gc + slot->regs.mdysl) & 0x3F;
      s32 xd = (s16)s->sound_stack[px];
      s32 yd = (s16)s->sound_stack[py];
      s32 sum = xd + yd;
      s32 modalizer = ((sum << 6) >> (0x10 - slot->regs.mdl)) & ~1;
      s32 sia;

      if (slot->regs.mdl <= 4)
         modalizer = 0;

      sia = modalizer + (s32)slot->state.sample_frac;
      // Sign-extend the integer part to 11 bits and keep the low 6 bits as the interpolation fraction.
      md_out = ((s32)((u32)(sia >> 6) << 21)) >> 21;
      slot->state.sample_frac = sia & 0x3F;

   }

   //address pointer

   // scsp3 reads the waveform at the CURRENT address, then advances; the new pipeline advanced the
   // address before reading, leaving every voice one sample ahead of scsp3. Capture the pre-advance
   // address here and use it for wave_index below.
   s32 read_offset = slot->state.sample_offset;

   if (slot->regs.lpctl == 0)//no loop
   {
      slot->state.sample_offset += sample_delta;

      if (slot->state.sample_offset >= slot->regs.lea)
         slot->state.attenuation = 0x3ff;
   }
   else if (slot->regs.lpctl == 1)//normal loop
   {
      slot->state.sample_offset += sample_delta;

      // scsp3 wraps by subtracting the loop length (CurrentAddr += LoopStart - LoopEnd), preserving
      // the overshoot past LoopEnd, rather than snapping back to LoopStart and losing it.
      if (slot->state.sample_offset >= slot->regs.lea)
         slot->state.sample_offset -= (s32)(slot->regs.lea - slot->regs.lsa);
   }
   else if (slot->regs.lpctl == 2)//reverse
   {
      if (!slot->state.backwards)
         slot->state.sample_offset += sample_delta;
      else
         slot->state.sample_offset -= sample_delta;

      if (!slot->state.backwards)
      {
         if (slot->state.sample_offset >= slot->regs.lea)
         {
            slot->state.sample_offset = slot->regs.lea;
            slot->state.backwards = 1;
         }
      }
      else
      {
         //backwards
         if (slot->state.sample_offset <= slot->regs.lsa)
            slot->state.sample_offset = slot->regs.lea;
      }
   }
   else if (slot->regs.lpctl == 3)//ping pong
   {
      if(!slot->state.backwards)
         slot->state.sample_offset += sample_delta;
      else
         slot->state.sample_offset -= sample_delta;

      if (!slot->state.backwards)
      {
         if (slot->state.sample_offset >= slot->regs.lea)
         {
            slot->state.sample_offset = slot->regs.lea;
            slot->state.backwards = 1;
         }
      }
      else
      {
         if (slot->state.sample_offset <= slot->regs.lsa)
         {
            slot->state.sample_offset = slot->regs.lsa;
            slot->state.backwards = 0;
         }
      }
   }

   // Track loop-start crossing for the LPSLNK attack->decay link (scsp3 InLoop).
   if (slot->state.sample_offset >= (s32)slot->regs.lsa)
      slot->state.in_loop = 1;

   // Raw sample index; op3 applies the ShortWaveMask and computes the interpolation pair.
   // Use the pre-advance address (scsp3 reads before incrementing CurrentAddr).
   slot->state.wave_index = read_offset + md_out;

   {
      u32 mask = new_scsp_shortwave_mask(slot);
      u32 i0 = ((u32)slot->state.wave_index) & mask;

      if (!slot->regs.pcm8b)
         slot->state.address_pointer = (s32)slot->regs.sa + i0 * 2;
      else
         slot->state.address_pointer = (s32)slot->regs.sa + i0;
   }
}


//waveform dram read
void op3(struct Slot * slot)
{
   // SBCTL XOR masks: 0=none, 1=invert lower 15 bits, 2=invert sign, 3=invert all (scsp3 SB_XOR_Table).
   static const u16 sb_xor_table[4] = { 0x0000, 0x7FFF, 0x8000, 0xFFFF };
   u16 sbxor = sb_xor_table[slot->regs.sbctl & 3];
   s32 sample;

   if (slot->state.attenuation < 0x3bf)
   {
      if (slot->regs.ssctl == 0)
      {
         // Memory waveform with linear interpolation (scsp3 RunSample). The two sample indices
         // are wrapped independently by the ShortWaveMask so short loops stay in range.
         u32 mask = new_scsp_shortwave_mask(slot);
         u32 i0 = ((u32)slot->state.wave_index) & mask;
         u32 i1 = ((u32)(slot->state.wave_index + 1)) & mask;
         u32 sia = slot->state.sample_frac & 0x3F;
         u32 addr0, addr1;
         s32 s0, s1;

         if (!slot->regs.pcm8b)
         {
            addr0 = ((u32)slot->regs.sa + i0 * 2) & 0x7FFFF;
            addr1 = ((u32)slot->regs.sa + i1 * 2) & 0x7FFFF;
            s0 = (s16)(T2ReadWord(SoundRam, addr0) ^ sbxor);
            s1 = (s16)(T2ReadWord(SoundRam, addr1) ^ sbxor);
         }
         else
         {
            addr0 = ((u32)slot->regs.sa + i0) & 0x7FFFF;
            addr1 = ((u32)slot->regs.sa + i1) & 0x7FFFF;
            s0 = (s16)((T2ReadByte(SoundRam, addr0) << 8) ^ sbxor);
            s1 = (s16)((T2ReadByte(SoundRam, addr1) << 8) ^ sbxor);
         }

         sample = ((s0 * (0x40 - (s32)sia)) + (s1 * (s32)sia)) >> 6;
         slot->state.wave = (u16)s0;
      }
      else
      {
         // Source select 1 = LFSR noise, 2/3 = zero. SBXOR applies after.
         u16 raw = (slot->regs.ssctl == 1) ? (u16)(new_scsp_lfsr << 8) : 0;
         sample = (s16)(raw ^ sbxor);
         slot->state.wave = (u16)sample;
      }

      slot->state.output = (s16)sample;
   }

   // Clock the 17-bit LFSR ONCE per slot, unconditionally, exactly like scsp3 (which clocks it
   // every slot regardless of activity). Gating it behind the active-slot early return slowed the
   // LFSR and changed the noise-source timbre.
   new_scsp_lfsr = (new_scsp_lfsr >> 1) | (((new_scsp_lfsr >> 5) ^ new_scsp_lfsr) & 1) << 16;
}

void change_envelope_state(struct Slot * slot, enum EnvelopeStates new_state)
{
   slot->state.envelope = new_state;
   slot->state.step_count = 0;
}

int need_envelope_step(int effective_rate, u32 sample_counter, struct Slot* slot)
{
   if (sample_counter == 0)
      return 0;

   if (effective_rate == 0 || effective_rate == 1)
   {
      return 0;//never step
   }
   else if (effective_rate >= 0x30)
   {
      if ((sample_counter & 1) == 0)
      {
         slot->state.envelope_steps_taken++;
         return 1;
      }
      else
         return 0;
   }
   else
   {
      int pos = effective_rate - 2;

      int result = 0;

      int value = envelope_table[pos][slot->state.step_count];

      if (sample_counter % value == 0)
      {
         result = 1;

         slot->state.envelope_steps_taken++;
         slot->state.step_count++;

         if (envelope_table[pos][slot->state.step_count] == EFFECTIVE_RATE_END)
            slot->state.step_count = 0;//reached the end of the array
      }

      return result;
   }
   return 0;
}

s32 get_rate(struct Slot * slot, int rate)
{
   s32 result = 0;

   if (slot->regs.krs == 0xf)
      result = rate * 2;
   else
   {
      result = (slot->regs.krs * 2) + (rate * 2) + ((slot->regs.fns >> 9) & 1);
      result = (8 ^ slot->regs.oct) + (result - 8);
   }

   if (result <= 0)
      return 0;

   if (result >= 0x3c)
      return 0x3c;

   return result;
}

void do_decay(struct Slot * slot, int rate_in)
{
   int rate = get_rate(slot, rate_in);
   int sample_mod_4 = slot->state.envelope_steps_taken & 3;
   int decay_rate;

   if (rate <= 0x30)
      decay_rate = decay_rate_table[0][sample_mod_4];
   else
      decay_rate = decay_rate_table[rate - 0x30][sample_mod_4];

   if (need_envelope_step(rate, slot->state.sample_counter, slot))
   {
      if (slot->state.attenuation < 0x3bf)
         slot->state.attenuation += decay_rate;
   }
}

//interpolation
//eg (scsp3 RunEG: exponential attack via ~EnvLevel, SampleCounter/XC bit-trick clocking)
void op4(struct Slot * slot)
{
   if (slot->state.attenuation >= 0x3bf)
      return;

   // SampleCounter is the per-sample tick (scsp3 GlobalCounter>>5). scsp3 evaluates it at the START
   // of RunSample, before the counter advances for this sample; the new pipeline increments
   // new_scsp.frame at the top of generate_sample, so subtract 1 to align the EG clock phase (else
   // the envelope advances one sample ahead of scsp3, e.g. an extra attack step on the key-on sample).
   const u32 sc = new_scsp.frame - 1;
   const u32 scxc = (sc ^ (sc - 1)) & (sc ^ 1);
   const u32 env_level_cache = slot->state.attenuation;

   unsigned erate_noscale;
   switch (slot->state.envelope)
   {
   case ATTACK:  erate_noscale = slot->regs.ar;  break;
   case DECAY1:  erate_noscale = slot->regs.d1r; break;
   case DECAY2:  erate_noscale = slot->regs.d2r; break;
   default:      erate_noscale = slot->regs.rr;  break; // RELEASE
   }

   // key_eg_scale: KRS plus signed octave, clamped to 0..0xF (scsp3 RunSample).
   unsigned key_eg_scale;
   if (slot->regs.krs == 0xf)
      key_eg_scale = 0;
   else
   {
      int v = (int)slot->regs.krs + (int)(slot->regs.oct ^ 8) - 8;
      key_eg_scale = (v < 0) ? 0 : (v > 0xf ? 0xf : (unsigned)v);
   }

   const unsigned erate = (key_eg_scale + erate_noscale < 0x1f) ? (key_eg_scale + erate_noscale) : 0x1f;
   const unsigned erate_wbt = (0x18 - (erate < 0x18 ? erate : 0x18)) >> 1;
   // ShiftAdj = (ERate < 0x18) & ERate  -> 0/1 (low bit of ERate only while below 0x18).
   const unsigned shift_adj = (erate < 0x18) ? (erate & 1u) : 0u;
   int clock_eg = (int)(((scxc | (scxc >> shift_adj)) >> (erate_wbt + shift_adj)) & (erate_noscale ? 1u : 0u));

   // Maximum attack rate keeps the level stuck at the key-on value (scsp3 RunEG guard).
   if (slot->state.envelope == ATTACK && (erate_noscale + key_eg_scale) >= 0x20)
      clock_eg = 0;

   // scsp3 executes RunEG before the key-on, so the key-on sample never advances the envelope.
   if (slot->state.eg_keyon_skip)
   {
      slot->state.eg_keyon_skip = 0;
      clock_eg = 0;
   }

   if (clock_eg)
   {
      const s32 inc_base = (slot->state.envelope == ATTACK) ? ~(s32)env_level_cache : 16;
      unsigned ermaxo = erate;
      if (ermaxo < 0x18) ermaxo = 0x18;
      if (ermaxo > 0x1e) ermaxo = 0x1e;
      const u32 srac = ((0x20 - ermaxo) >> 1) + (ermaxo & 1u & (sc >> (erate_wbt + 1)));
      s32 new_env = (s32)env_level_cache + (inc_base >> srac);

      if (new_env > 0x3ff) new_env = 0x3ff;
      if (new_env < 0) new_env = 0;
      slot->state.attenuation = (u16)new_env;
   }

   // Phase transitions use the pre-update level (scsp3 RunEG, EnvLevelCache).
   if (slot->state.envelope == DECAY1)
   {
      if ((env_level_cache >> 5) == slot->regs.dl)
         change_envelope_state(slot, DECAY2);
   }
   else if (slot->state.envelope == ATTACK)
   {
      // LPSLNK holds attack at full volume until the loop start is reached; otherwise the
      // attack ends when the level reaches maximum volume (EnvLevelCache == 0).
      if ((slot->regs.ls && slot->state.in_loop) ||
          (!slot->regs.ls && env_level_cache == 0))
         change_envelope_state(slot, DECAY1);
   }
}

s16 apply_volume(u16 tl, u16 slot_att, const s16 s)
{
   s32 sample_att = 0;
   s32 v = 0;

   v += tl * 4;
   v += slot_att;
   if (v > 0x3ff)
     v = 0x3ff;

   sample_att = (s * ((v & 0x3F) ^ 0x7F)) >> ((v >> 6) + 7);

   return sample_att;
}

//level 1
void op5(struct Slot * slot)
{
   if (slot->state.attenuation >= 0x3bf)
   {
      slot->state.output = 0;
      return;
   }
   else
   {
      s16 sample = 0;

      // Direct Sound
      if( slot->regs.sd ){
        sample = slot->state.output;
      }else{
        // EGHOLD: during attack the slot is held at full volume (scsp3 AttackHold).
        u16 eff_att = (slot->state.envelope == ATTACK && slot->regs.hold) ? 0 : slot->state.attenuation;
        // Amplitude LFO adds to the envelope attenuation (scsp3 GetALFO).
        sample = apply_volume(slot->regs.tl, eff_att + new_scsp_get_alfo(slot), slot->state.output);
      }
      slot->state.output = sample;
   }
}

//level 2
void op6(struct Slot * slot)
{
   
}

//sound stack write
void op7(struct Slot * slot, struct Scsp*s)
{
   {
      // scsp3 GlobalCounter mechanics: commit the 4-deep delayed output to ring entry (GC-4),
      // gated by the LIVE StackWriteInhibit of the slot being written, then advance the counter.
      // op7 processes slot[step-6] while global_counter has advanced to 32*frame+step, so the
      // GlobalCounter that scsp3 would have for this slot is global_counter-6. delayer[3] holds
      // slot(step-10)'s output, which scsp3 stores at ring entry (GlobalCounter-6-4).
      u32 gc = s->global_counter - 6;

      if (!s->slots[(gc - 4) & 0x1F].regs.si)
         s->sound_stack[(gc - 4) & 0x3F] = (u16)s->sound_stack_delayer[3];


      s->sound_stack_delayer[3] = s->sound_stack_delayer[2];
      s->sound_stack_delayer[2] = s->sound_stack_delayer[1];
      s->sound_stack_delayer[1] = s->sound_stack_delayer[0];
      s->sound_stack_delayer[0] = slot->state.output;

      s->global_counter++;
   }

   // Record sample for debug waveform display (Issue #73)
   // Only record when debug mode is active to avoid performance overhead
   if (s->debug_mode) {
      scsp_debug_record_sample(slot->state.num, slot->state.output);
   }

   slot->state.sample_counter++;
   // lfo_counter is no longer used; the scsp3-style LFO is paced by lfo_time_counter in op1.
}

struct DebugInstrument
{
   u32 sa;
   int is_muted;
};

#define NUM_DEBUG_INSTRUMENTS 24

struct DebugInstrument debug_instruments[NUM_DEBUG_INSTRUMENTS] = { 0 };
int debug_instrument_pos = 0;

void scsp_debug_search_instruments(const u32 sa, int* found, int * offset)
{
   int i = 0;
   *found = 0;
   for (i = 0; i < NUM_DEBUG_INSTRUMENTS; i++)
   {
      if (debug_instruments[i].sa == sa)
      {
         *found = 1;
         break;
      }
   }

   *offset = i;
}

void scsp_debug_add_instrument(u32 sa)
{
   int i = 0, found = 0, offset = 0;

   if (debug_instrument_pos >= NUM_DEBUG_INSTRUMENTS)
      return;

   scsp_debug_search_instruments(sa, &found, &offset);

   //new instrument discovered
   if (!found)
      debug_instruments[debug_instrument_pos++].sa = sa;
}

void scsp_debug_instrument_set_mute(u32 sa, int mute)
{
   int found = 0, offset = 0;
   scsp_debug_search_instruments(sa, &found, &offset);

   if (offset >= NUM_DEBUG_INSTRUMENTS)
      return;

   if (found)
      debug_instruments[offset].is_muted = mute;
}

int scsp_debug_instrument_check_is_muted(u32 sa)
{
   int found = 0, offset = 0;
   scsp_debug_search_instruments(sa, &found, &offset);

   if (offset >= NUM_DEBUG_INSTRUMENTS)
      return 0;

   if (found && debug_instruments[offset].is_muted)
      return 1;

   return 0;
}

void scsp_debug_instrument_get_data(int i, u32 * sa, int * is_muted)
{
   if(i >= NUM_DEBUG_INSTRUMENTS)
      return;

   *sa = debug_instruments[i].sa;
   *is_muted = debug_instruments[i].is_muted;
}

void scsp_debug_set_mode(int mode)
{
   new_scsp.debug_mode = mode;
}

void scsp_debug_instrument_clear()
{
   debug_instrument_pos = 0;
   memset(debug_instruments, 0, sizeof(struct DebugInstrument) * NUM_DEBUG_INSTRUMENTS);
}

void scsp_debug_get_envelope(int chan, int * env, int * state)
{
   *env = new_scsp.slots[chan].state.attenuation;
   *state = new_scsp.slots[chan].state.envelope;
}

void scsp_debug_get_tl(int chan, int * tl, int * dldisdl, int * imxl, int * efsdl )
{
   *tl = new_scsp.slots[chan].regs.tl;
   *dldisdl = new_scsp.slots[chan].regs.disdl;
   *imxl = new_scsp.slots[chan].regs.imxl;
   *efsdl = new_scsp.slots[chan].regs.efsdl;
}

// Extended debug API (Issue #73)
void scsp_debug_get_slot_state(int slot, ScspSlotDebugInfo *info)
{
   if (slot < 0 || slot >= 32 || info == NULL)
      return;

   struct Slot *s = &new_scsp.slots[slot];

   // Key state
   info->key_on = s->regs.kx;
   info->key_bit = s->regs.kb;
   info->is_active = (s->state.envelope != RELEASE) &&
                     (s->state.attenuation < 0x3bf) &&
                     (s->regs.tl < 0x3ff);

   // Source settings
   info->source_bit_ctrl = s->regs.sbctl;
   info->source_select = s->regs.ssctl;
   info->loop_ctrl = s->regs.lpctl;
   info->pcm_8bit = s->regs.pcm8b;

   // Address
   info->start_addr = s->regs.sa;
   info->loop_start = s->regs.lsa;
   info->loop_end = s->regs.lea;
   info->current_addr = s->state.address_pointer;

   // Envelope (ADSR)
   info->attack_rate = s->regs.ar;
   info->decay1_rate = s->regs.d1r;
   info->decay2_rate = s->regs.d2r;
   info->release_rate = s->regs.rr;
   info->decay_level = s->regs.dl;
   info->key_rate_scale = s->regs.krs;
   info->envelope_state = (u8)s->state.envelope;
   info->envelope_level = s->state.attenuation;

   // Pitch
   info->octave = s->regs.oct;
   info->frequency_num = s->regs.fns;
   info->phase_value = s->state.waveform_phase_value;

   // LFO
   info->lfo_frequency = s->regs.lfof;
   info->pitch_lfo_wave = s->regs.plfows;
   info->pitch_lfo_shift = s->regs.plfos;
   info->amp_lfo_wave = s->regs.alfows;
   info->amp_lfo_shift = s->regs.alfos;
   info->lfo_position = s->state.lfo_pos;

   // Level
   info->total_level = s->regs.tl;
   info->direct_level = s->regs.disdl;
   info->direct_pan = s->regs.dipan;
   info->effect_level = s->regs.efsdl;
   info->effect_pan = s->regs.efpan;

   // Modulation
   info->mod_level = s->regs.mdl;
   info->mod_x_select = s->regs.mdxsl;
   info->mod_y_select = s->regs.mdysl;

   // Output
   info->current_output = s->state.output;
   info->is_muted = s->state.is_muted;
}

int scsp_debug_is_slot_active(int slot)
{
   if (slot < 0 || slot >= 32)
      return 0;

   struct Slot *s = &new_scsp.slots[slot];
   return (s->state.envelope != RELEASE) &&
          (s->state.attenuation < 0x3bf) &&
          (s->regs.tl < 0x3ff);
}

void scsp_debug_get_output_level(int slot, s16 *level_l, s16 *level_r)
{
   if (slot < 0 || slot >= 32 || level_l == NULL || level_r == NULL)
      return;

   struct Slot *s = &new_scsp.slots[slot];
   s16 output = s->state.output;

   // Apply direct pan (dipan: 0-31, where 0=full left, 31=full right)
   int pan = s->regs.dipan;
   int left_vol = (31 - pan) * 8;   // 0-248
   int right_vol = pan * 8;          // 0-248

   *level_l = (s16)((output * left_vol) >> 8);
   *level_r = (s16)((output * right_vol) >> 8);
}

// Sample ring buffer for waveform display
#define SCSP_DEBUG_SAMPLE_BUFFER_SIZE 1024
static s16 scsp_debug_sample_buffer[32][SCSP_DEBUG_SAMPLE_BUFFER_SIZE];
static int scsp_debug_sample_buffer_pos[32] = {0};

void scsp_debug_record_sample(int slot, s16 sample)
{
   if (slot < 0 || slot >= 32)
      return;
   scsp_debug_sample_buffer[slot][scsp_debug_sample_buffer_pos[slot]] = sample;
   scsp_debug_sample_buffer_pos[slot] =
      (scsp_debug_sample_buffer_pos[slot] + 1) % SCSP_DEBUG_SAMPLE_BUFFER_SIZE;
}

int scsp_debug_get_sample_buffer(int slot, s16 *buffer, int max_samples)
{
   if (slot < 0 || slot >= 32 || buffer == NULL || max_samples <= 0)
      return 0;

   int count = (max_samples < SCSP_DEBUG_SAMPLE_BUFFER_SIZE) ?
               max_samples : SCSP_DEBUG_SAMPLE_BUFFER_SIZE;
   int pos = scsp_debug_sample_buffer_pos[slot];

   // Copy samples in chronological order (oldest first)
   for (int i = 0; i < count; i++) {
      int idx = (pos + i) % SCSP_DEBUG_SAMPLE_BUFFER_SIZE;
      buffer[i] = scsp_debug_sample_buffer[slot][idx];
   }

   return count;
}

// High-quality output state that keyon() needs (the rest sits with
// generate_sample() below). The chip never reads any of it.
static std::atomic<int> scsp_hq_enabled(0);
// Set by keyon() while the high-quality output is on, consumed by the tap
// capture at the slot's next op2. A key on lands between generate_sample()
// calls, and for slots 26-30 op7 (which counts sample_counter) runs in the
// next call before op2 does, so the chip's own counters cannot tell.
static std::atomic<u8> scsp_hq_keyon_pending[32];
// Developer statistics (ScspHqGetKeyOnStats), only counted while it is on.
static std::atomic<u32> scsp_hq_stat_keyon_calls[32];
static std::atomic<u32> scsp_hq_stat_tap_keyons[32];
static std::atomic<u32> scsp_hq_stat_counter_zero[32];

// Float DSP (ScspSetHighQualityFloatDsp): 0 off, 1 on, 2 on and fed with the
// chip's own sends (developer verification).
static std::atomic<int> scsp_hq_float_dsp_mode(0);
// Sound RAM writes by the CPUs (and the SCSP DMA) while the float DSP may be
// on: the float DSP's copy of those words is dropped at the next sample
// (sound_hq/ram_watch.h). The DSP's own writes are not reported.
static std::atomic<int> scsp_hq_ram_watch(0);
static ScspHqRamWatch scsp_hq_ram_pages;
// Serializes the two setters below, so the watch always matches both settings.
static std::mutex scsp_hq_setting_mutex;

// Called after every CPU or DMA write to sound RAM. Off: one relaxed load.
static INLINE void scsp_hq_note_ram_write(u32 addr, u32 size)
{
   if (scsp_hq_ram_watch.load(std::memory_order_relaxed))
      scsp_hq_ram_pages.mark(addr, size);
}

// Stores a setting that can start or stop the float DSP. Starting: the watch
// goes on first, so no CPU write is missed once the float DSP can run.
// Stopping: the setting goes first. Called under scsp_hq_setting_mutex.
static void scsp_hq_store_setting(std::atomic<int> * setting, int value, int watch)
{
   if (watch)
   {
      scsp_hq_ram_watch.store(1, std::memory_order_seq_cst);
      setting->store(value, std::memory_order_seq_cst);
   }
   else
   {
      setting->store(value, std::memory_order_seq_cst);
      scsp_hq_ram_watch.store(0, std::memory_order_seq_cst);
   }
}

void keyon(struct Slot * slot)
{
   if (slot->state.envelope == RELEASE )
   {
      if (scsp_hq_enabled.load(std::memory_order_relaxed))
      {
         scsp_hq_keyon_pending[slot->state.num].store(1, std::memory_order_relaxed);
         scsp_hq_stat_keyon_calls[slot->state.num].fetch_add(1, std::memory_order_relaxed);
      }
     slot->state.envelope = ATTACK;
      slot->state.sample_counter = 0;
      slot->state.step_count = 0;
      slot->state.sample_offset = 0;
      slot->state.envelope_steps_taken = 0;
      slot->state.lfo_pos = 0;
      slot->state.lfo_time_counter = 1; // non-zero so the first decrement triggers a reload
      slot->state.in_loop = 0;
      slot->state.eg_keyon_skip = 1; // scsp3 runs RunEG before key-on: no attack step this sample

      {
         // Attack starts at full attenuation, unless the effective attack rate is so fast that
         // it begins already at max volume (scsp3 RunSample key-on).
         unsigned key_eg_scale;
         if (slot->regs.krs == 0xf)
            key_eg_scale = 0;
         else
         {
            int v = (int)slot->regs.krs + (int)(slot->regs.oct ^ 8) - 8;
            key_eg_scale = (v < 0) ? 0 : (v > 0xf ? 0xf : v);
         }
         slot->state.attenuation = ((slot->regs.ar + key_eg_scale) >= 0x20) ? 0x000 : 0x280;
      }

      if ( !slot->regs.pcm8b && (slot->regs.sa&0x01) ) {
        slot->regs.sa &= 0xFFFFFE ;
      }

#if 0
      LOG("kx:%d kb:%d sbctl:%d ssctl:%d lpctl:%d pcm8b:%d"
        " sa:%X lsa:%d, lea:%d d2r:%d d1r:%d hold:%d"
        " ar:%d ls:%d krs:%d dl:%d rr:%d si:%d sd:%d tl:%X"
        " mdl:%d mdxsl:%d mdysl:%d oct:%d fns:%d re:%d lfof:%d"
        " plfows:%d plfos:%d alfows:%d alfos:%d isel:%d imxl:%d disdl:%d"
        " dipan:%d efsdl:%d efpan:%d",
        slot->regs.kx,
        slot->regs.kb,
        slot->regs.sbctl,
        slot->regs.ssctl,
        slot->regs.lpctl,
        slot->regs.pcm8b,
        slot->regs.sa,
        slot->regs.lsa,
        slot->regs.lea,
        slot->regs.d2r,
        slot->regs.d1r,
        slot->regs.hold,
        slot->regs.ar,
        slot->regs.ls,
        slot->regs.krs,
        slot->regs.dl,
        slot->regs.rr,
        slot->regs.si,
        slot->regs.sd,
        slot->regs.tl,
        slot->regs.mdl,
        slot->regs.mdxsl,
        slot->regs.mdysl,
        slot->regs.oct,
        slot->regs.fns,
        slot->regs.re,
        slot->regs.lfof,
        slot->regs.plfows,
        slot->regs.plfos,
        slot->regs.alfows,
        slot->regs.alfos,
        slot->regs.isel,
        slot->regs.imxl,
        slot->regs.disdl,
        slot->regs.dipan,
        slot->regs.efsdl,
        slot->regs.efpan);
#endif

      if (new_scsp.debug_mode)
         scsp_debug_add_instrument(slot->regs.sa);
   }
   //otherwise ignore
}

void keyoff(struct Slot * slot)
{
   change_envelope_state(slot, RELEASE);
}

void keyonex(struct Scsp *s)
{
   int channel;
   for (channel = 0; channel < 32; channel++)
   {
     if (s->slots[channel].regs.kb) {

       //if ( s->slots[channel].state.envelope == RELEASE) {
       //LOG("keyon %d", channel);
       //}
       keyon(&s->slots[channel]);
     }
     else {
       //if ( s->slots[channel].state.envelope != RELEASE) {
       //  LOG("keyoff %d state %d", channel, s->slots[channel].state.envelope );
       //}
       keyoff(&s->slots[channel]);
     }
   }
}

void scsp_slot_write_byte(struct Scsp *s, u32 addr, u8 data)
{
   int slot_num = (addr >> 5) & 0x1f;
   struct Slot * slot = &s->slots[slot_num];
   u32 offset = (addr - (0x20 * slot_num));

   //SCSPLOG("Slot Write %d:%d \n", slot_num, addr);

   switch (offset)
   {
   case 0:
      slot->regs.kb = (data >> 3) & 1;//has to be done first

      if ((data >> 4) & 1) { //keyonex
        //SCSPLOG("KEY ON %d:1", slot_num);
        keyonex(s);
      }
 
      slot->regs.sbctl = (data >> 1) & 3;
      slot->regs.ssctl = (slot->regs.ssctl & 1) | ((data & 1) << 1);
      break;
   case 1:
      slot->regs.ssctl = (slot->regs.ssctl & 2) | ((data >> 7) & 1);
      slot->regs.lpctl = (data >> 5) & 3;
      slot->regs.pcm8b = (data >> 4) & 1;
      slot->regs.sa = (slot->regs.sa & 0xffff) | ((data & 0xf) << 16);
      break;
   case 2:
      slot->regs.sa = (slot->regs.sa & 0xf00ff) | (data << 8);
      break;
   case 3:
      slot->regs.sa = (slot->regs.sa & 0xfff00) | data;
      break;
   case 4:
      slot->regs.lsa = (slot->regs.lsa & 0x00ff) | (data << 8);
      break;
   case 5:
      slot->regs.lsa = (slot->regs.lsa & 0xff00) | data;
      break;
   case 6:
      slot->regs.lea = (slot->regs.lea & 0x00ff) | (data << 8);
      break;
   case 7:
      slot->regs.lea = (slot->regs.lea & 0xff00) | data;
      break;
   case 8:
      slot->regs.d2r = (data >> 3) & 0x1f;
      slot->regs.d1r = (slot->regs.d1r & 3) | ((data & 0x7) << 2);
      break;
   case 9:
      slot->regs.d1r = (slot->regs.d1r & 0x1c) | ((data >> 6) & 3);
      slot->regs.hold = (data >> 5) & 1;
      // scsp3 parity: use AR verbatim. The legacy "< 0x10 -> 0x10" (Darius Gaiden)
      // and "hold -> AR=0x1F" clamps force AR=0 held-attack slots to progress their
      // envelope; a self-modulating FM carrier then grows unbounded and buzzes. scsp3
      // keeps AR=0 (attack held at the key-on level) and models EGHOLD via AttackHold
      // at the output stage (op5: hold -> full volume during attack).
      slot->regs.ar = data & 0x1f;
      break;
   case 10:
      slot->regs.unknown1 = (data >> 7) & 1;
      slot->regs.ls = (data >> 6) & 1;
      slot->regs.krs = (data >> 2) & 0xf;
      slot->regs.dl = (slot->regs.dl & 0x7) | ((data & 3) << 3);
      break;
   case 11:
      slot->regs.dl = (slot->regs.dl & 0x18) | ((data >> 5) & 7);
      slot->regs.rr = data & 0x1f;
      break;
   case 12:
      slot->regs.unknown2 = (data >> 2) & 3;
      slot->regs.si = (data >> 1) & 1;
      slot->regs.sd = data & 1;
      break;
   case 13:
      slot->regs.tl = data;
      break;
   case 14:
      slot->regs.mdl = (data >> 4) & 0xf;
      slot->regs.mdxsl = (slot->regs.mdxsl & 0x3) | ((data & 0xf) << 2);
      break;
   case 15:
      slot->regs.mdxsl = (slot->regs.mdxsl & 0x3C) | ((data >> 6) & 3);
      slot->regs.mdysl = data & 0x3f;
      break;
   case 16:
      slot->regs.unknown3 = (data >> 7) & 1;
      slot->regs.shortwave = (data >> 7) & 1;
      slot->regs.oct = (data >> 3) & 0xf;
      slot->regs.unknown4 = (data >> 2) & 1;
      slot->regs.fns = (slot->regs.fns & 0xff) | ((data & 0x7) << 8);
      break;
   case 17:
      slot->regs.fns = (slot->regs.fns & 0x700) | data;
      break;
   case 18:
      slot->regs.re = (data >> 7) & 1;
      slot->regs.lfof = (data >> 2) & 0x1f;
      slot->regs.plfows = data & 3;
      break;
   case 19:
      slot->regs.plfos = (data >> 5) & 7;
      slot->regs.alfows = (data >> 3) & 3;
      slot->regs.alfos = data & 7;
      break;
   case 20:
      //nothing here
      break;
   case 21:
      slot->regs.unknown5 = (data >> 7) & 1;
      slot->regs.isel = (data >> 3) & 0xf;
      slot->regs.imxl = data & 7;
      break;
   case 22:
      slot->regs.disdl = (data >> 5) & 7;
      slot->regs.dipan = data & 0x1f;
      break;
   case 23:
      slot->regs.efsdl = (data >> 5) & 7;
      slot->regs.efpan = data & 0x1f;
//      if (slot_num == 16) {
//        printf("efsdl is updated %d", slot->regs.efsdl);
//      }
      break;
   default:
      break;
   }
}

u8 scsp_slot_read_byte(struct Scsp *s, u32 addr)
{
   int slot_num = (addr >> 5) & 0x1f;
   struct Slot * slot = &s->slots[slot_num];
   u32 offset = (addr - (0x20 * slot_num));
   u8 data = 0;

   switch (offset)
   {
   case 0:
      data |= slot->regs.kb << 3;
      data |= slot->regs.sbctl << 1;
      data |= (slot->regs.ssctl >> 1) & 1;
      break;
   case 1:
      data |= (slot->regs.ssctl & 1) << 7;
      data |= slot->regs.lpctl << 5;
      data |= slot->regs.pcm8b << 4;
      data |= (slot->regs.sa & 0xf0000) >> 16;
      break;
   case 2:
      data |= (slot->regs.sa & 0xff00) >> 8;
      break;
   case 3:
      data |= slot->regs.sa & 0xff;
      break;
   case 4:
      data |= (slot->regs.lsa & 0xff00) >> 8;
      break;
   case 5:
      data |= slot->regs.lsa & 0xff;
      break;
   case 6:
      data |= (slot->regs.lea & 0xff00) >> 8;
      break;
   case 7:
      data |= slot->regs.lea & 0xff;
      break;
   case 8:
      data |= slot->regs.d2r << 3;
      data |= slot->regs.d1r >> 2;
      break;
   case 9:
      data |= slot->regs.d1r << 6;
      data |= slot->regs.hold << 5;
      data |= slot->regs.ar;
      break;
   case 10:
      data |= slot->regs.unknown1 << 7;
      data |= slot->regs.ls << 6;
      data |= slot->regs.krs << 2;
      data |= slot->regs.dl >> 3;
      break;
   case 11:
      data |= slot->regs.dl << 5;
      data |= slot->regs.rr;
      break;
   case 12:
      data |= slot->regs.unknown2 << 2;
      data |= slot->regs.si << 1;
      data |= slot->regs.sd;
      break;
   case 13:
      data |= slot->regs.tl;
      break;
   case 14:
      data |= slot->regs.mdl << 4;
      data |= slot->regs.mdxsl >> 2;
      break;
   case 15:
      data |= slot->regs.mdxsl << 6;
      data |= slot->regs.mdysl;
      break;
   case 16:
      data |= slot->regs.unknown3 << 7;
      data |= slot->regs.oct << 3;
      data |= slot->regs.unknown4 << 2;
      data |= slot->regs.fns >> 8;
      break;
   case 17:
      data |= slot->regs.fns;
      break;
   case 18:
      data |= slot->regs.re << 7;
      data |= slot->regs.lfof << 2;
      data |= slot->regs.plfows;
      break;
   case 19:
      data |= slot->regs.plfos << 5;
      data |= slot->regs.alfows << 3;
      data |= slot->regs.alfos;
      break;
   case 20:
      //nothing
      break;
   case 21:
      data |= slot->regs.unknown5 << 7;
      data |= slot->regs.isel << 3;
      data |= slot->regs.imxl;
      break;
   case 22:
      data |= slot->regs.disdl << 5;
      data |= slot->regs.dipan;
      break;
   case 23:
      data |= slot->regs.efsdl << 5;
      data |= slot->regs.efpan;
      break;
   }

   return data;
}

void scsp_slot_write_word(struct Scsp *s, u32 addr, u16 data)
{
   int slot_num = (addr >> 5) & 0x1f;
   struct Slot * slot = &s->slots[slot_num];
   u32 offset = (addr - (0x20 * slot_num));

   //SCSPLOG("Slot Write %d:%d\n", slot_num, addr);

   switch (offset >> 1)
   {
   case 0:
      slot->regs.kb = (data >> 11) & 1;//has to be done before keyonex

      if (data & (1 << 12)){
        //SCSPLOG("KEY ON %d:2", slot_num);
        keyonex(s);
      }

      slot->regs.sbctl = (data >> 9) & 3;
      slot->regs.ssctl = (data >> 7) & 3;
      slot->regs.lpctl = (data >> 5) & 3;
      slot->regs.pcm8b = (data >> 4) & 1;
      slot->regs.sa = (slot->regs.sa & 0xffff) | ((data & 0xf) << 16);
      break;
   case 1:
      slot->regs.sa = (slot->regs.sa & 0xf0000) | data;
      break;
   case 2:
      slot->regs.lsa = data;
      break;
   case 3:
      slot->regs.lea = data;
      break;
   case 4:
      slot->regs.d2r = data >> 11;
      slot->regs.d1r = (data >> 6) & 0x1f;
      slot->regs.hold = (data >> 5) & 1;
      // scsp3 parity: use AR verbatim (see scsp_slot_write_byte case 9). The legacy
      // Darius Gaiden / EGHOLD clamps broke AR=0 held-attack FM carriers and buzzed.
      slot->regs.ar = data & 0x1f;
      break;
   case 5:
      slot->regs.unknown1 = (data >> 15) & 1;
      slot->regs.ls = (data >> 14) & 1;
      slot->regs.krs = (data >> 10) & 0xf;
      slot->regs.dl = (data >> 5) & 0x1f;
      slot->regs.rr = data & 0x1f;
      break;
   case 6:
      slot->regs.unknown2 = (data >> 10) & 3;
      slot->regs.si = (data >> 9) & 1;
      slot->regs.sd = (data >> 8) & 1;
      slot->regs.tl = data & 0xff;
      break;
   case 7:
      slot->regs.mdl = (data >> 12) & 0xf;
      slot->regs.mdxsl = (data >> 6) & 0x3f;
      slot->regs.mdysl = data & 0x3f;
      break;
   case 8:
      slot->regs.unknown3 = (data >> 15) & 1;
      slot->regs.shortwave = (data >> 15) & 1;
      slot->regs.unknown4 = (data >> 10) & 1;
      slot->regs.oct = (data >> 11) & 0xf;
      slot->regs.fns = data & 0x7ff;
      break;
   case 9:
      slot->regs.re = (data >> 15) & 1;
      slot->regs.lfof = (data >> 10) & 0x1f;
      slot->regs.plfows = (data >> 8) & 3;
      slot->regs.plfos = (data >> 5) & 7;
      slot->regs.alfows = (data >> 3) & 3;
      slot->regs.alfos = data & 7;
      break;
   case 10:
      slot->regs.unknown5 = (data >> 7) & 1;
      slot->regs.isel = (data >> 3) & 0xf;
      slot->regs.imxl = data & 7;
      break;
   case 11:
      slot->regs.disdl = (data >> 13) & 7;
      slot->regs.dipan = (data >> 8) & 0x1f;
      slot->regs.efsdl = (data >> 5) & 7;
      slot->regs.efpan = data & 0x1f;
      //if (slot_num == 16) {
        //printf("%d", slot->regs.efsdl);
      //}
      break;
   default:
      break;
   }
}

u16 scsp_slot_read_word(struct Scsp *s, u32 addr)
{
   int slot_num = (addr >> 5) & 0x1f;
   struct Slot * slot = &s->slots[slot_num];
   u32 offset = (addr - (0x20 * slot_num));
   u16 data = 0;

   switch (offset >> 1)
   {
   case 0:
      //keyonex not stored
      data |= slot->regs.kb << 11;
      data |= slot->regs.sbctl << 9;
      data |= slot->regs.ssctl << 7;
      data |= slot->regs.lpctl << 5;
      data |= slot->regs.pcm8b << 4;
      data |= (slot->regs.sa >> 16) & 0xf;
      break;
   case 1:
      data = slot->regs.sa & 0xffff;
      break;
   case 2:
      data = slot->regs.lsa;
      break;
   case 3:
      data = slot->regs.lea;
      break;
   case 4:
      data |= slot->regs.d2r << 11;
      data |= slot->regs.d1r << 6;
      data |= slot->regs.hold << 5;
      data |= slot->regs.ar;
      break;
   case 5:
      data |= slot->regs.unknown1 << 15;
      data |= slot->regs.ls << 14;
      data |= slot->regs.krs << 10;
      data |= slot->regs.dl << 5;
      data |= slot->regs.rr;
      break;
   case 6:
      data |= slot->regs.unknown2 << 10;
      data |= slot->regs.si << 9;
      data |= slot->regs.sd << 8;
      data |= slot->regs.tl;
      break;
   case 7:
      data |= slot->regs.mdl << 12;
      data |= slot->regs.mdxsl << 6;
      data |= slot->regs.mdysl;
      break;
   case 8:
      data |= slot->regs.unknown3 << 15;
      data |= slot->regs.oct << 11;
      data |= slot->regs.unknown4 << 10;
      data |= slot->regs.fns;
      break;
   case 9:
      data |= slot->regs.re << 15;
      data |= slot->regs.lfof << 10;
      data |= slot->regs.plfows << 8;
      data |= slot->regs.plfos << 5;
      data |= slot->regs.alfows << 3;
      data |= slot->regs.alfos;
      break;
   case 10:
      data |= slot->regs.unknown5 << 7;
      data |= slot->regs.isel << 3;
      data |= slot->regs.imxl;
      break;
   case 11:
      data |= slot->regs.disdl << 13;
      data |= slot->regs.dipan << 8;
      data |= slot->regs.efsdl << 5;
      data |= slot->regs.efpan;
      break;
   }
   return data;
}

void get_panning(int pan, int * pan_val_l, int * pan_val_r)
{
   if (pan & 0x10)
   {
      //negative values
      *pan_val_l = 0;
      *pan_val_r = pan & 0xf;
   }
   else
   {
      *pan_val_l = pan & 0xf;
      *pan_val_r = 0;
   }
}

int get_sdl_shift(int sdl)
{
   if (sdl == 0)
      return 16;//-infinity
   else return (7 - sdl);
}




static void scsp_hq_request_reset(void);

void new_scsp_reset(struct Scsp* s)
{
   int slot_num;
   int backDebug = s->debug_mode;
   scsp_hq_request_reset();
   memset(s, 0, sizeof(struct Scsp));
   s->debug_mode = backDebug;

   for (slot_num = 0; slot_num < 32; slot_num++)
   {
      s->slots[slot_num].state.attenuation = 0x3FF;
      s->slots[slot_num].state.envelope = RELEASE;
      s->slots[slot_num].state.num = slot_num;
   }

   fill_plfo_tables();
   fill_alfo_tables();

   memset(&scsp_dsp, 0, sizeof(ScspDsp));

   new_scsp_outbuf_pos = 0;
   new_scsp_cycles = 0;
}

void scsp_set_use_new(int which)
{
   if (which && !use_new_scsp)
      new_scsp_reset(&new_scsp);

   use_new_scsp = which;
}

////////////////////////////////////////////////////////////////

#ifndef PI
#define PI 3.14159265358979323846
#endif

#define SCSP_FREQ         44100                                     // SCSP frequency

#define SCSP_RAM_SIZE     0x080000                                  // SCSP RAM size
#define SCSP_RAM_MASK     (SCSP_RAM_SIZE - 1)

#define SCSP_MIDI_IN_EMP  0x01                                      // MIDI flags
#define SCSP_MIDI_IN_FUL  0x02
#define SCSP_MIDI_IN_OVF  0x04
#define SCSP_MIDI_OUT_EMP 0x08
#define SCSP_MIDI_OUT_FUL 0x10

#define SCSP_ENV_RELEASE  3                                         // Envelope phase
#define SCSP_ENV_SUSTAIN  2
#define SCSP_ENV_DECAY    1
#define SCSP_ENV_ATTACK   0

#define SCSP_FREQ_HB      19                                        // Freq counter int part
#define SCSP_FREQ_LB      10                                        // Freq counter float part

#define SCSP_ENV_HB       10                                        // Env counter int part
#define SCSP_ENV_LB       10                                        // Env counter float part

#define SCSP_LFO_HB       10                                        // LFO counter int part
#define SCSP_LFO_LB       10                                        // LFO counter float part

#define SCSP_ENV_LEN      (1 << SCSP_ENV_HB)                        // Env table len
#define SCSP_ENV_MASK     (SCSP_ENV_LEN - 1)                        // Env table mask

#define SCSP_FREQ_LEN     (1 << SCSP_FREQ_HB)                       // Freq table len
#define SCSP_FREQ_MASK    (SCSP_FREQ_LEN - 1)                       // Freq table mask

#define SCSP_LFO_LEN      (1 << SCSP_LFO_HB)                        // LFO table len
#define SCSP_LFO_MASK     (SCSP_LFO_LEN - 1)                        // LFO table mask

#define SCSP_ENV_AS       0                                         // Env Attack Start
#define SCSP_ENV_DS       (SCSP_ENV_LEN << SCSP_ENV_LB)             // Env Decay Start
#define SCSP_ENV_AE       (SCSP_ENV_DS - 1)                         // Env Attack End
#define SCSP_ENV_DE       (((2 * SCSP_ENV_LEN) << SCSP_ENV_LB) - 1) // Env Decay End

#define SCSP_ATTACK_R     (u32) (8 * 44100)
#define SCSP_DECAY_R      (u32) (12 * SCSP_ATTACK_R)

////////////////////////////////////////////////////////////////

typedef struct slot_t
{
  u8 swe;      // stack write enable
  u8 sdir;     // sound direct
  u8 pcm8b;    // PCM sound format

  u8 sbctl;    // source bit control
  u8 ssctl;    // sound source control
  u8 lpctl;    // loop control

  u8 key;      // KEY_ state
  u8 keyx;     // still playing regardless the KEY_ state (hold, decay)

  s8 *buf8;    // sample buffer 8 bits
  s16 *buf16;  // sample buffer 16 bits

  u32 fcnt;    // phase counter
  u32 finc;    // phase step adder
  u32 finct;   // non adjusted phase step

  s32 ecnt;    // envelope counter
  s32 *einc;    // envelope current step adder
  s32 einca;   // envelope step adder for attack
  s32 eincd;   // envelope step adder for decay 1
  s32 eincs;   // envelope step adder for decay 2
  s32 eincr;   // envelope step adder for release
  s32 ecmp;    // envelope compare to raise next phase
  u32 ecurp;   // envelope current phase (attack / decay / release ...)
  s32 env;     // envelope multiplier (at time of last update)

  void (*enxt)(struct slot_t *);  // envelope function pointer for next phase event

  u32 lfocnt;   // lfo counter
  s32 lfoinc;   // lfo step adder

  u32 sa;       // start address
  u32 lsa;      // loop start address
  u32 lea;      // loop end address

  s32 tl;       // total level
  s32 sl;       // sustain level

  s32 ar;       // attack rate
  s32 dr;       // decay rate
  s32 sr;       // sustain rate
  s32 rr;       // release rate

  s32 *arp;     // attack rate table pointer
  s32 *drp;     // decay rate table pointer
  s32 *srp;     // sustain rate table pointer
  s32 *rrp;     // release rate table pointer

  u32 krs;      // key rate scale

  s32 *lfofmw;  // lfo frequency modulation waveform pointer
  s32 *lfoemw;  // lfo envelope modulation waveform pointer
  u8 lfofms;    // lfo frequency modulation sensitivity
  u8 lfoems;    // lfo envelope modulation sensitivity
  u8 fsft;      // frequency shift (used for freq lfo)

  u8 mdl;       // modulation level
  u8 mdx;       // modulation source X 
  u8 mdy;       // modulation source Y

  u8 imxl;      // input sound level
  u8 disll;     // direct sound level left
  u8 dislr;     // direct sound level right
  u8 efsll;     // effect sound level left
  u8 efslr;     // effect sound level right

  u8 eghold;    // eg type envelope hold
  u8 lslnk;     // loop start link (start D1R when start loop adr is reached)

  // NOTE: Previously there were u8 pads here to maintain 4-byte alignment.
  //       There are current 22 u8's in this struct and 1 u16. This makes 24
  //       bytes, so there are no pads at the moment.
  //
  //       I'm not sure this is at all necessary either, but keeping this note
  //       in case.
} slot_t;

typedef struct scsp_t
{
  u32 mem4b;            // 4mbit memory
  u32 dac18b;
  u32 mvol;             // master volume

  u32 rbl;              // ring buffer lenght
  u32 rbp;              // ring buffer address (pointer)

  u32 mslc;             // monitor slot
  u32 ca;               // call address
  u32 sgc;              // phase
  u32 eg;               // envelope

  u32 dmea;             // dma memory address start
  u32 drga;             // dma register address start
  u32 dmfl;             // dma flags (direction / gate 0 ...)
  u32 dmlen;            // dma transfer len

  u8 midinbuf[4];       // midi in buffer
  u8 midoutbuf[4];      // midi out buffer
  u8 midincnt;          // midi in buffer size
  u8 midoutcnt;         // midi out buffer size
  u8 midflag;           // midi flag (empty, full, overflow ...)
  u8 midflag2;          // midi flag 2 (here only for alignement)

  s32 timacnt;          // timer A counter
  u32 timasd;           // timer A step diviser
  s32 timbcnt;          // timer B counter
  u32 timbsd;           // timer B step diviser
  s32 timccnt;          // timer C counter
  u32 timcsd;           // timer C step diviser

  u32 scieb;            // allow sound cpu interrupt
  u32 scipd;            // pending sound cpu interrupt

  u32 scilv0;           // IL0 M68000 interrupt pin state
  u32 scilv1;           // IL1 M68000 interrupt pin state
  u32 scilv2;           // IL2 M68000 interrupt pin state

  u32 mcieb;            // allow main cpu interrupt
  std::atomic<u32> mcipd;            // pending main cpu interrupt

  u8 *scsp_ram;         // scsp ram pointer
  void (*mintf)(void);  // main cpu interupt function pointer
  void (*sintf)(u32);   // sound cpu interrupt function pointer

  s32 stack[32 * 2];    // two last generation slot output (SCSP STACK)
  slot_t slot[32];      // 32 slots

} scsp_t;

////////////////////////////////////////////////////////////////

static s32 scsp_env_table[SCSP_ENV_LEN * 2];  // envelope curve table (attack & decay)

static s32 scsp_lfo_sawt_e[SCSP_LFO_LEN];     // lfo sawtooth waveform for envelope
static s32 scsp_lfo_squa_e[SCSP_LFO_LEN];     // lfo square waveform for envelope
static s32 scsp_lfo_tri_e[SCSP_LFO_LEN];      // lfo triangle waveform for envelope
static s32 scsp_lfo_noi_e[SCSP_LFO_LEN];      // lfo noise waveform for envelope

static s32 scsp_lfo_sawt_f[SCSP_LFO_LEN];     // lfo sawtooth waveform for frequency
static s32 scsp_lfo_squa_f[SCSP_LFO_LEN];     // lfo square waveform for frequency
static s32 scsp_lfo_tri_f[SCSP_LFO_LEN];      // lfo triangle waveform for frequency
static s32 scsp_lfo_noi_f[SCSP_LFO_LEN];      // lfo noise waveform frequency

static s32 scsp_attack_rate[0x40 + 0x20];     // envelope step for attack
static s32 scsp_decay_rate[0x40 + 0x20];      // envelope step for decay
static s32 scsp_null_rate[0x20];              // null envelope step

static s32 scsp_lfo_step[32];                 // directly give the lfo counter step

static s32 scsp_tl_table[256];                // table of values for total level attentuation

static u8 scsp_reg[0x1000];

static u8 *scsp_isr;
static u8 *scsp_ccr;
static u8 *scsp_dcr;

static s32 *scsp_bufL;
static s32 *scsp_bufR;
static u32 scsp_buf_len;
static u32 scsp_buf_pos;

static scsp_t   scsp;                         // SCSP structure

int scsp_debug_get_mvol(){
  return scsp.mvol;
}


#define CDDA_NUM_BUFFERS	2*75

static union {
   u8 data[CDDA_NUM_BUFFERS*2352];
} cddabuf;
// The CD block hands sectors over from the emulation thread while the SCSP
// consumes them from its own thread, so this is a single-producer /
// single-consumer ring and has to be published as one. It used to be a plain
// byte count that both sides read-modify-wrote (+2352 on one thread, -4 on the
// other) with no synchronisation at all: increments were lost, and every lost
// one dropped a whole sector, i.e. 13ms of audio, straight out of the stream.
//
// Each position below is written by exactly one side. The two totals are
// monotonic (they wrap with u32, which is fine - only their difference is ever
// used), and the release/acquire pair on them is what publishes the sector
// bytes themselves, so the memcpy no longer races the reader either.
static u32 cdda_in_pos = 0;                     // write offset, producer only
static u32 cdda_out_pos = 0;                    // read offset, consumer only
static std::atomic<u32> cdda_written(0);        // total bytes handed to the ring
static std::atomic<u32> cdda_read(0);           // total bytes played out


////////////////////////////////////////////////////////////////

static void scsp_env_null_next(slot_t *slot);
static void scsp_release_next(slot_t *slot);
static void scsp_sustain_next(slot_t *slot);
static void scsp_decay_next(slot_t *slot);
static void scsp_attack_next(slot_t *slot);
static void scsp_slot_update_keyon(slot_t *slot);

//////////////////////////////////////////////////////////////////////////////
// High-quality output (sound_hq/scsp_hq.h), an enhancement that is off by
// default.
//
// Off: generate_sample() runs the code it always ran. Every addition sits
// behind a check of its local hq_rec, which is 0, and nothing else is done.
// On: the chip still runs unchanged. Its own output is still computed, and
// every value the game or the sound driver can observe stays the same. The
// capture functions below only READ chip state, into arrays the chip never
// reads, and the rendered sample replaces the chip's at the very end.
//
// generate_sample() runs on the SCSP thread, while ScspSetHighQuality() and
// the reset requests (chip reset, state load) come from other threads. So
// they are atomics that generate_sample() consumes at the start of a sample.
//
// The pipeline is staggered (slot k: op1 at step k, mixed at step k + 6), so
// slots 26-31 are mixed in the next call. Each stage's view of a slot is kept
// in scsp_hq_pending until the slot is mixed.

static std::atomic<int> scsp_hq_reset_request(1);
static ScspHqTaps scsp_hq_taps;
// Created off the SCSP thread (ScspSetHighQuality, ScspInit) and published by
// the release store of scsp_hq_enabled; deleted in ScspDeInit after the SCSP
// thread has stopped.
static ScspHqRenderer *scsp_hq_renderer = NULL;
static std::mutex scsp_hq_create_mutex;
static ScspHqQuantizer scsp_hq_quantizer;
void (*scsp_hq_chip_output_hook)(s16 chip_l, s16 chip_r, s16 hq_l, s16 hq_r) = NULL;
void (*scsp_hq_fx_output_hook)(const float chip[2], const float flt[2]) = NULL;
// The chip DSP as the float DSP sees it, and the ring offset and sends this
// sample's program ran with (taken before the chip's DSP runs).
static ScspHqDspView scsp_hq_dsp_view;
static u32 scsp_hq_dsp_mdec_ct;

// Builds the renderer (its kernel table) on the calling thread, never on the
// SCSP thread.
static void scsp_hq_create_renderer(void)
{
   std::lock_guard<std::mutex> lock(scsp_hq_create_mutex);
   if (scsp_hq_renderer == NULL)
      scsp_hq_renderer = new ScspHqRenderer();
}

struct ScspHqPending
{
   u8 op1;       // op1 ran (slot active): phase and step are valid
   u8 op2;       // op2 ran: pos and key_on are valid
   u8 op5;       // the op5 capture ran: silent, sample and att are valid
   u8 key_on;
   u8 silent;    // op5 forced the output to 0
   u16 phase;    // read fraction, 14 bits
   u16 pos;      // read position
   u32 step;     // increment, 14-bit fraction
   s16 sample;   // op5 input (interpolated, before the level)
   u16 att;      // op5 attenuation, 0xFFFF for SDIR
};
static ScspHqPending scsp_hq_pending[32];

void ScspSetHighQuality(int on)
{
   std::lock_guard<std::mutex> lock(scsp_hq_setting_mutex);
   on = on ? 1 : 0;
   if (on)
      scsp_hq_create_renderer();
   const int changed = scsp_hq_enabled.load(std::memory_order_relaxed) != on;
   scsp_hq_store_setting(&scsp_hq_enabled, on,
                         on && scsp_hq_float_dsp_mode.load(std::memory_order_relaxed) != 0);
   if (changed)
      scsp_hq_reset_request.store(1, std::memory_order_release);
}

void ScspSetHighQualityFloatDsp(int mode)
{
   std::lock_guard<std::mutex> lock(scsp_hq_setting_mutex);
   mode = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
   // the renderer seeds the float DSP from the chip when it turns on
   scsp_hq_store_setting(&scsp_hq_float_dsp_mode, mode,
                         mode != 0 && scsp_hq_enabled.load(std::memory_order_relaxed) != 0);
}

int ScspGetHighQualityFloatDsp(void)
{
   return scsp_hq_float_dsp_mode.load(std::memory_order_relaxed);
}

u32 ScspHqGetFloatDspSeedCount(void)
{
   // the renderer is created and deleted under this mutex
   std::lock_guard<std::mutex> lock(scsp_hq_create_mutex);
   return scsp_hq_renderer ? scsp_hq_renderer->float_dsp_seed_count() : 0;
}

void ScspHqGetKeyOnStats(u32 keyon_calls[32], u32 tap_keyons[32], u32 counter_zero[32])
{
   int i;
   for (i = 0; i < 32; i++)
   {
      keyon_calls[i] = scsp_hq_stat_keyon_calls[i].load(std::memory_order_relaxed);
      tap_keyons[i] = scsp_hq_stat_tap_keyons[i].load(std::memory_order_relaxed);
      counter_zero[i] = scsp_hq_stat_counter_zero[i].load(std::memory_order_relaxed);
   }
}

int ScspGetHighQuality(void)
{
   return scsp_hq_enabled.load(std::memory_order_relaxed);
}

// Chip reset and state load: the renderer's per-voice history no longer
// matches the chip, so it is dropped at the next sample.
static void scsp_hq_request_reset(void)
{
   scsp_hq_reset_request.store(1, std::memory_order_release);
}

static void scsp_hq_begin_sample(void)
{
   if (scsp_hq_reset_request.exchange(0, std::memory_order_acquire))
   {
      int i;
      scsp_hq_renderer->reset();
      memset(scsp_hq_pending, 0, sizeof(scsp_hq_pending));
      for (i = 0; i < 32; i++)
         scsp_hq_keyon_pending[i].store(0, std::memory_order_relaxed);
   }
   const int fx_mode = scsp_hq_float_dsp_mode.load(std::memory_order_acquire);
   scsp_hq_renderer->float_dsp = fx_mode != 0;
   scsp_hq_renderer->float_dsp_chip_sends = fx_mode == 2;
   // CPU writes since the last sample: the float DSP reads those words from
   // RAM again (8 relaxed loads when nothing was written)
   if (fx_mode != 0)
   {
      ScspHqRenderer *r = scsp_hq_renderer;
      scsp_hq_ram_pages.take([r](uint32_t first, uint32_t count) { r->discard_ram(first, count); });
   }
}

// The chip's DSP for the float DSP: its program and parameters, the ring
// offset it ran with, and its datapath after the run (for seeding).
static void scsp_hq_fill_dsp_view(int rbp, int rbl)
{
   ScspHqDspView *v = &scsp_hq_dsp_view;
   v->mpro = (const uint64_t *)scsp_dsp.mpro;
   v->coef = (const uint16_t *)scsp_dsp.coef;
   v->madrs = (const uint16_t *)scsp_dsp.madrs;
   v->mdec_ct = scsp_hq_dsp_mdec_ct;
   v->rbp = rbp;
   v->rbl = rbl;
   v->last_step = scsp_dsp.last_step;
   v->temp = (const int32_t *)scsp_dsp.temp;
   v->mems = (const int32_t *)scsp_dsp.mems;
   v->efreg = (const int16_t *)scsp_dsp.efreg;
   v->shift_reg = scsp_dsp.shift_reg;
   v->inputs = scsp_dsp.inputs;
   v->y_reg = scsp_dsp.y_reg;
   v->frc_reg = scsp_dsp.frc_reg;
   v->adrs_reg = scsp_dsp.adrs_reg;
   v->read_value = scsp_dsp.read_value;
   v->read_pending = scsp_dsp.read_pending;
   v->write_pending = scsp_dsp.write_pending;
   v->io_addr = scsp_dsp.io_addr;
}

// Before op1: the phase the waveform is read with (op1 captures the same
// pre-increment value for op2) and this sample's increment, as op1 computes it.
static void scsp_hq_capture_op1(struct Slot * slot)
{
   ScspHqPending *p = &scsp_hq_pending[slot->state.num];
   p->op1 = 0;
   if (slot->state.attenuation >= 0x3bf)
      return;
   const s32 base = (s32)(0x400 ^ slot->regs.fns) + new_scsp_get_plfo(slot);
   const u32 oct = slot->regs.oct ^ 8;
   p->phase = (u16)(((u32)slot->state.waveform_phase_value & ((1u << 18) - 1)) >> 4);
   p->step = ((u32)(base << oct) & ~0xFu) >> 4;
   p->op1 = 1;
}

// Before op2: the address op2 reads at (before it advances).
static void scsp_hq_capture_op2(struct Slot * slot)
{
   ScspHqPending *p = &scsp_hq_pending[slot->state.num];
   p->op2 = 0;
   if (slot->state.attenuation >= 0x3bf)
      return;
   p->pos = (u16)slot->state.sample_offset;
   p->key_on = scsp_hq_keyon_pending[slot->state.num].exchange(0, std::memory_order_relaxed) != 0;
   if (slot->state.sample_counter == 0)  // statistics only: misses slots 26-30
      scsp_hq_stat_counter_zero[slot->state.num].fetch_add(1, std::memory_order_relaxed);
   p->op2 = 1;
}

// Before op5: its input and the attenuation it applies (op5 and apply_volume).
static void scsp_hq_capture_op5(struct Slot * slot)
{
   ScspHqPending *p = &scsp_hq_pending[slot->state.num];
   p->op5 = 1;
   if (slot->state.attenuation >= 0x3bf)
   {
      p->silent = 1;
      return;
   }
   p->silent = 0;
   p->sample = slot->state.output;
   if (slot->regs.sd)
      p->att = 0xFFFF;
   else
   {
      const u16 eff_att = (slot->state.envelope == ATTACK && slot->regs.hold) ? 0 : slot->state.attenuation;
      const u16 slot_att = (u16)(eff_att + new_scsp_get_alfo(slot));
      s32 v = slot->regs.tl * 4 + slot_att;
      if (v > 0x3ff)
         v = 0x3ff;
      p->att = (u16)v;
   }
}

// A 1.14 gain from the chip's shifts (>> sdl, then >> pan).
static s16 scsp_hq_gain(int sdl_shift, int pan_shift)
{
   const int shift = sdl_shift + pan_shift;
   return shift > 14 ? 0 : (s16)(16384 >> shift);
}

// When the slot is mixed: the tap for the renderer.
static void scsp_hq_tap_slot(struct Slot * slot, int muted)
{
   static const u16 sb_xor_table[4] = { 0x0000, 0x7FFF, 0x8000, 0xFFFF };  // as op3
   ScspHqPending *p = &scsp_hq_pending[slot->state.num];
   ScspHqVoiceTap *t = &scsp_hq_taps.v[slot->state.num];
   int pan_l = 0, pan_r = 0;

   t->key_on = p->op2 && p->key_on;
   if (t->key_on)
      scsp_hq_stat_tap_keyons[slot->state.num].fetch_add(1, std::memory_order_relaxed);
   t->pcm8 = slot->regs.pcm8b != 0;
   t->loop = slot->regs.lpctl == 1;
   t->lsa = slot->regs.lsa;
   t->lea = slot->regs.lea;
   t->sb_xor = sb_xor_table[slot->regs.sbctl & 3];
   t->sa = slot->regs.sa;
   t->pos = p->pos;
   t->phase = p->phase;
   t->step = p->step;
   if (!p->op5)
   {
      // op5 ran before this mode was enabled or the history was dropped
      // (slots 26-31 are mixed one call after their op5): take the chip's
      // own output for this one sample, level included
      t->kind = ScspHqVoiceTap::SAMPLE;
      t->sample = slot->state.output;
      t->att = 0xFFFF;
   }
   else if (p->silent)
   {
      t->kind = ScspHqVoiceTap::SAMPLE;
      t->sample = 0;
      t->att = 0xFFFF;
   }
   else
   {
      // the renderer re-reads forward-playing RAM waveforms; noise, FM,
      // reverse and alternate loops and short waves use the chip's sample
      t->kind = (p->op1 && p->op2 && slot->regs.ssctl == 0 && slot->regs.lpctl <= 1 &&
                 slot->regs.mdl <= 4 && new_scsp_shortwave_mask(slot) == 0xFFFFFFFF)
                   ? ScspHqVoiceTap::WAVE : ScspHqVoiceTap::SAMPLE;
      t->sample = p->sample;
      t->att = p->att;
   }
   get_panning(slot->regs.dipan, &pan_l, &pan_r);
   t->direct_vol[0] = muted ? 0 : scsp_hq_gain(get_sdl_shift(slot->regs.disdl), pan_l);
   t->direct_vol[1] = muted ? 0 : scsp_hq_gain(get_sdl_shift(slot->regs.disdl), pan_r);
   t->isel = slot->regs.isel;
   t->imxl = muted ? 0 : slot->regs.imxl;
   p->op1 = 0;
   p->op2 = 0;
   p->op5 = 0;
}

// After the mix: effect returns, external inputs and master volume; then the
// rendered sample replaces the chip's.
static void scsp_hq_finish_sample(struct Scsp * s, int rbp, int rbl, int mvol, s16 * out_l, s16 * out_r)
{
   int i;
   for (i = 0; i < 32; i++)
   {
      ScspHqVoiceTap *t = &scsp_hq_taps.v[i];
      int pan_l = 0, pan_r = 0;
      if (i >= 18)
      {
         t->fx = 0;
         t->effect_vol[0] = t->effect_vol[1] = 0;
         continue;
      }
      t->fx = i < 16 ? scsp_dsp.efreg[i] : scsp_dsp.exts[i - 16];
      get_panning(s->slots[i].regs.efpan, &pan_l, &pan_r);
      t->effect_vol[0] = scsp_hq_gain(get_sdl_shift(s->slots[i].regs.efsdl), pan_l);
      t->effect_vol[1] = scsp_hq_gain(get_sdl_shift(s->slots[i].regs.efsdl), pan_r);
   }
   scsp_hq_taps.master = (u16)(0x8000 >> (0xf - mvol));
   scsp_hq_taps.exts[0] = scsp_dsp.exts[0];
   scsp_hq_taps.exts[1] = scsp_dsp.exts[1];
   if (scsp_hq_renderer->float_dsp)
   {
      scsp_hq_fill_dsp_view(rbp, rbl);
      scsp_hq_taps.dsp = &scsp_hq_dsp_view;
   }
   else
      scsp_hq_taps.dsp = NULL;

   float f[2];
   scsp_hq_renderer->render(scsp_hq_taps, SoundRam, f);
   if (scsp_hq_taps.dsp != NULL && scsp_hq_fx_output_hook)
   {
      float fx_chip[2], fx_float[2];
      scsp_hq_renderer->last_effect_mix(fx_chip, fx_float);
      scsp_hq_fx_output_hook(fx_chip, fx_float);
   }
   if (scsp.dac18b != 0)
   {
      // the 18-bit DAC setting seen through 16 bits. The chip shifts the
      // clipped s16 left by 2, which wraps above |8191|; here the shifted
      // value is clipped instead, so the two differ only there.
      f[0] *= 4.0f;
      f[1] *= 4.0f;
   }
   const s16 chip_l = *out_l, chip_r = *out_r;
   *out_l = scsp_hq_quantizer(f[0]);
   *out_r = scsp_hq_quantizer(f[1]);
   if (scsp_hq_chip_output_hook)
      scsp_hq_chip_output_hook(chip_l, chip_r, *out_l, *out_r);
}

void generate_sample(struct Scsp * s, int rbp, int rbl, s16 * out_l, s16* out_r, int mvol, s16 cd_in_l, s16 cd_in_r)
{
   int step_num = 0;
   int i = 0;
   int mvol_shift = 0;
   s32 outl32 = 0;
   s32 outr32 = 0;

   // Per-sample counter (scsp3 SampleCounter), consumed by the op4 envelope generator.
   s->frame++;

   const int hq_rec = scsp_hq_enabled.load(std::memory_order_acquire) && scsp_hq_renderer != NULL;
   if (hq_rec)
      scsp_hq_begin_sample();

   scsp_dsp.rbp = rbp;
   scsp_dsp.rbl = rbl;

   scsp_dsp.exts[0] = cd_in_l;
   scsp_dsp.exts[1] = cd_in_r;

   if (hq_rec)
   {
      // what this sample's program runs with, for the float DSP
      scsp_hq_dsp_mdec_ct = scsp_dsp.mdec_ct;
      for (i = 0; i < 16; i++)
         scsp_hq_taps.chip_mixs[i] = scsp_dsp.mixs[i];
   }

   if (scsp_dsp.updated){

	   for (i = 127; i >= 0; --i)
	   {
		   if( scsp_dsp.mpro[i] != 0 )
			   break;
	   }
	   scsp_dsp.last_step = i + 1;
	   scsp_dsp.updated = 0;
   }

   for (i = 0; i < scsp_dsp.last_step; i++)
      ScspDspExec(&scsp_dsp, i, SoundRam);

   if (!scsp_dsp.mdec_ct){
     scsp_dsp.mdec_ct = (0x2000 << rbl);
   }
   scsp_dsp.mdec_ct--;

   for (i = 0; i < 16; i++)
      scsp_dsp.mixs[i] = 0;   

   //run 32 steps to generate 1 full sample (512 clock cycles at 22579200hz)
   //7 operations happen simultaneously on different channels due to pipelining
   for (step_num = 0; step_num < 32; step_num++)
   {
      int last_step = (step_num - 6) & 0x1f;
      int debug_muted = 0;

      // Captures read the state each stage is about to use (op1 and op2 work
      // on different slots, and op3 clocks the LFSR the op5 capture reads).
      if (hq_rec)
      {
         scsp_hq_capture_op1(&s->slots[step_num]);
         scsp_hq_capture_op2(&s->slots[(step_num - 1) & 0x1f]);
      }
      op1(&s->slots[step_num]);//phase, pitch lfo
      op2(&s->slots[(step_num - 1) & 0x1f],s);//address pointer, modulation data read
      op3(&s->slots[(step_num - 2) & 0x1f]);//waveform dram read
      op4(&s->slots[(step_num - 3) & 0x1f]);//interpolation, eg, amplitude lfo
      if (hq_rec)
         scsp_hq_capture_op5(&s->slots[(step_num - 4) & 0x1f]);
      op5(&s->slots[(step_num - 4) & 0x1f]);//level calc 1
      op6(&s->slots[(step_num - 5) & 0x1f]);//level calc 2
      op7(&s->slots[(step_num - 6) & 0x1f],s);//sound stack write

      if (s->debug_mode)
      {
         if (scsp_debug_instrument_check_is_muted(s->slots[last_step].regs.sa))
            debug_muted = 1;
      }

      if (hq_rec)
         scsp_hq_tap_slot(&s->slots[last_step], debug_muted);

      if (!debug_muted)
      {
         int disdl = get_sdl_shift(s->slots[last_step].regs.disdl);

         s16 disdl_applied = (s->slots[last_step].state.output >> disdl);

         s16 mixs_input = s->slots[last_step].state.output >> 
           get_sdl_shift(s->slots[last_step].regs.imxl);

         int pan_val_l = 0, pan_val_r = 0;

         get_panning(s->slots[last_step].regs.dipan, &pan_val_l, &pan_val_r);

         outl32 = outl32 + (disdl_applied >> pan_val_l);
         outr32 = outr32 + (disdl_applied >> pan_val_r);
         scsp_dsp.mixs[s->slots[last_step].regs.isel] += mixs_input << 4;
      }
   }


   for (i = 0; i < 18; i++)//16,17 are exts0/1
   {
      int efsdl = get_sdl_shift(s->slots[i].regs.efsdl);
      s16 efsdl_applied = 0; 

      int pan_val_l = 0, pan_val_r = 0;
      s16 panned_l = 0, panned_r = 0;

      if (i < 16)
        efsdl_applied = (scsp_dsp.efreg[i] >> efsdl);
      else if (i == 16)
        efsdl_applied = scsp_dsp.exts[0] >> efsdl;
      else if (i == 17)
        efsdl_applied = scsp_dsp.exts[1] >> efsdl;

      get_panning(s->slots[i].regs.efpan, &pan_val_l, &pan_val_r);

      panned_l = (efsdl_applied >> pan_val_l);
      panned_r = (efsdl_applied >> pan_val_r);

      outl32 = outl32 + panned_l;
      outr32 = outr32 + panned_r;
   }

   mvol_shift = 0xf - mvol;

   outl32 = outl32 >> mvol_shift;
   outr32 = outr32 >> mvol_shift;   
   *out_l = min(SHRT_MAX, max(SHRT_MIN, outl32));
   *out_r = min(SHRT_MAX, max(SHRT_MIN, outr32));
   if( scsp.dac18b != 0 ){
     *out_l = *out_l << 2;
     *out_r = *out_r << 2;
   }

   if (hq_rec)
      scsp_hq_finish_sample(s, rbp, rbl, mvol, out_l, out_r);
}

//////////////////////////////////////////////////////////////////////////////

static int scsp_mute_flags = 0;
static int scsp_volume = 100;
static int thread_running = 0;
static int scsp_sample_count = 0;
static int scsp_checktime = 0;
////////////////////////////////////////////////////////////////
// Misc

static int
scsp_round (double val)
{
  return (int)(val + 0.5);
}


////////////////////////////////////////////////////////////////
// Interrupts

static INLINE void
scsp_trigger_main_interrupt (u32 id)
{
  SCSPLOG ("scsp main interrupt accepted %.4X\n", id);
  scsp.mintf ();
}

void scsp_check_interrupt() {
  unsigned mask_test;
  unsigned lvmasked[3];
  unsigned level = 0;

  mask_test = scsp.scipd & scsp.scieb;
  if (mask_test &~0xFF)
    mask_test = (mask_test & 0xFF) | 0x80;

  lvmasked[0] = (scsp.scilv0 & mask_test) << 0;
  lvmasked[1] = (scsp.scilv1 & mask_test) << 1;
  lvmasked[2] = (scsp.scilv2 & mask_test) << 2;

  for (unsigned i = 0; i < 8; i++)
  {
    unsigned l = (lvmasked[0] & 0x1) | (lvmasked[1] & 0x2) | (lvmasked[2] & 0x4);

    if (l > level)
      level = l;

    lvmasked[0] >>= 1;
    lvmasked[1] >>= 1;
    lvmasked[2] >>= 1;
  }
  if (level != 0) {
    SCSPLOG("SCSP LV0=%08X, LV1=%08X, LV2=%08X, SCIPD = %08X, SCIEB = %08X\n",
      scsp.scilv0, scsp.scilv1, scsp.scilv2, scsp.scipd, scsp.scieb);
  }

  // The SCSP drives the sound CPU interrupt inputs as a level derived from
  // SCIPD & SCIEB & SCILV, so the request has to disappear again as soon as
  // the driver clears the pending bit (SCIRE) or disables the source (SCIEB).
  // Only ever raising the line left a request latched inside the M68K core:
  // X-Men vs. Street Fighter disables its timer interrupt while the sound CPU
  // is halted by SNDOFF, and the stale request was delivered to the restarted
  // driver before it had installed its own exception vectors, which parked the
  // sound CPU in the "unexpected interrupt" stub (STOP) forever and hung the
  // game waiting for the driver to acknowledge its command mailbox.
  scsp.sintf(level);
}

static INLINE void
scsp_trigger_sound_interrupt (u32 id)
{
   u32 level;
   level = 0;
   if (id > 0x80) id = 0x80;
   if (scsp.scilv0 & id) level |= 1;
   if (scsp.scilv1 & id) level |= 2;
   if (scsp.scilv2 & id) level |= 4;
#if 0
   unsigned mask_test;
   unsigned lvmasked[3];
   unsigned level = 0;

   scsp.scipd |= id;

   mask_test = scsp.scipd & scsp.scieb;
   if (mask_test &~0xFF)
     mask_test = (mask_test & 0xFF) | 0x80;

   lvmasked[0] = (scsp.scilv0 & mask_test) << 0;
   lvmasked[1] = (scsp.scilv1 & mask_test) << 1;
   lvmasked[2] = (scsp.scilv2 & mask_test) << 2;

   for (unsigned i = 0; i < 8; i++)
   {
     unsigned l = (lvmasked[0] & 0x1) | (lvmasked[1] & 0x2) | (lvmasked[2] & 0x4);

     if (l > level)
       level = l;

     lvmasked[0] >>= 1;
     lvmasked[1] >>= 1;
     lvmasked[2] >>= 1;
   }
#endif

#ifdef SCSP_DEBUG
   if (id == 0x8) SCSPLOG ("scsp sound interrupt accepted %.2X lev=%d\n", id, level);
#endif

   //if(level!=0)SCSPLOG("scsp sound interrupt accepted %.2X lev=%d\n", id, level);
   scsp.sintf (level);
}


void scsp_main_interrupt (u32 id)
{
//  if (id != 0x400) SCSPLOG("scsp main interrupt %.4X\n", id);

  //scsp.mcipd |= id;
  auto expected = scsp.mcipd.load();
  u32 desired;
  do {
    desired = expected | id;
  } while (!scsp.mcipd.compare_exchange_weak(expected, desired));

  //SCSPLOG("scsp main interrupt mcipd = %04X\n", scsp.mcipd );
  //WRITE_THROUGH (scsp.mcipd);

  if (scsp.mcieb & id)
    scsp_trigger_main_interrupt (id);
}

void scsp_sound_interrupt (u32 id)
{
//  if (scsp.scipd & id) return;

//  SCSPLOG ("scsp sound interrupt %.4X\n", id);

  scsp.scipd |= id;
  WRITE_THROUGH (scsp.scipd);

  if (scsp.scieb & id)
    scsp_trigger_sound_interrupt (id);
}

////////////////////////////////////////////////////////////////
// Direct Memory Access
// 100412h	DMEA[15:1] -
// 100414h	DMEA[19:16]	DRGA[11:1] -
// 100416h - GA	DI	EX	DTLG[11:1]
static void
scsp_dma (void)
{
  if (scsp.dmfl & 0x20)
    {
      // dsp -> scsp_ram
      SCSPLOG ("scsp dma: scsp_ram(%08lx) <- reg(%08lx) * %08lx\n",
               scsp.dmea, scsp.drga, scsp.dmlen);
      u32 from = scsp.dmea;
      u32 to = scsp.drga;
      u32 cnt = scsp.dmlen>>1;
      for (int i = 0; i < cnt; i++) {
        u16 val = scsp_r_w(from);
        //if (scsp.dmfl & 0x40) val = 0;
        T2WriteWord(SoundRam, to & 0x7FFFF, val);
        scsp_hq_note_ram_write(to, 2);
        cache_update_external_write(0x05A00000 | (to & 0x7FFFF), val, 2);
        from += 2;
        to += 2;
      }

    }
  else
    {
      // scsp_ram -> dsp
      SCSPLOG ("scsp dma: scsp_ram(%08lx) -> reg(%08lx) * %08lx\n",
               scsp.dmea, scsp.drga, scsp.dmlen);
      u32 from = scsp.dmea;
      u32 to = scsp.drga;
      u32 cnt = scsp.dmlen>>1;
      for (int i = 0; i < cnt; i++) {
        u16 val = T2ReadWord(SoundRam, from & 0x7FFFF); 
        //if (scsp.dmfl & 0x40) val = 0;
        scsp_w_w(to,val);
        from += 2;
        to += 2;
      }
    }
  scsp.dmfl &= ~0x10;
  scsp_ccr[0x16 ^ 3] &= 0xE0;

  scsp_sound_interrupt (0x10);
  scsp_main_interrupt (0x10);
}

////////////////////////////////////////////////////////////////
// Key ON/OFF event handler

static void
scsp_slot_keyon (slot_t *slot)
{
  // key need to be released before being pressed ;)
  if (slot->ecurp == SCSP_ENV_RELEASE)
    {
      SCSPLOG ("key on slot %d. 68K PC = %08X slot->sa = %08X slot->lsa = %08X "
               "slot->lea = %08X\n", slot - &(scsp.slot[0]), M68K->GetPC(),
               slot->sa, slot->lsa, slot->lea >> SCSP_FREQ_LB);

      // set buffer, loop start/end address of the slot
      if (slot->pcm8b)
        {
          slot->buf8 = (s8*) &(scsp.scsp_ram[slot->sa]);
          if ((slot->sa + (slot->lea >> SCSP_FREQ_LB)) > SCSP_RAM_MASK)
            slot->lea = (SCSP_RAM_MASK - slot->sa) << SCSP_FREQ_LB;
        }
      else
        {
          slot->buf16 = (s16*) &(scsp.scsp_ram[slot->sa & ~1]);
          if ((slot->sa + (slot->lea >> (SCSP_FREQ_LB - 1))) > SCSP_RAM_MASK)
            slot->lea = (SCSP_RAM_MASK - slot->sa) << (SCSP_FREQ_LB - 1);
        }

      slot->fcnt = 0;                 // reset frequency counter
      slot->ecnt = SCSP_ENV_AS;       // reset envelope counter (probably wrong,
                                      // should convert decay to attack?)
      slot->env = 0;                  // reset envelope

      slot->einc = &slot->einca;      // envelope counter step is attack step
      slot->ecurp = SCSP_ENV_ATTACK;  // current envelope phase is attack
      slot->ecmp = SCSP_ENV_AE;       // limit reach to next event (Attack End)
      slot->enxt = scsp_attack_next;  // function pointer to next event
    }
}

static void
scsp_slot_keyoff (slot_t *slot)
{
  // key need to be pressed before being released ;)
  if (slot->ecurp != SCSP_ENV_RELEASE)
    {
      SCSPLOG ("key off slot %d\n", slot - &(scsp.slot[0]));

      // if we still are in attack phase at release time, convert attack to decay
      if (slot->ecurp == SCSP_ENV_ATTACK)
        slot->ecnt = SCSP_ENV_DE - slot->ecnt;
      slot->einc = &slot->eincr;
      slot->ecmp = SCSP_ENV_DE;
      slot->ecurp = SCSP_ENV_RELEASE;
      slot->enxt = scsp_release_next;
    }
}

static void
scsp_slot_keyonoff (void)
{
  slot_t *slot;

  for(slot = &(scsp.slot[0]); slot < &(scsp.slot[32]); slot++)
    {
      if (slot->key)
        scsp_slot_keyon (slot);
      else
        scsp_slot_keyoff (slot);
    }
}

/*
   Envelope Events Handler

   Max EG level = 0x3FF      /|\
                            / | \
                           /  |  \_____
   Min EG level = 0x000 __/   |  |    |\___
                          A   D1 D2   R
*/

static void
scsp_env_null_next (UNUSED slot_t *slot)
{
  // only to prevent null call pointer...
}

static void
scsp_release_next (slot_t *slot)
{
  // end of release happened, update to process the next phase...

  slot->ecnt = SCSP_ENV_DE;
  slot->einc = NULL;
  slot->ecmp = SCSP_ENV_DE + 1;
  slot->enxt = scsp_env_null_next;
}

static void
scsp_sustain_next (slot_t *slot)
{
  // end of sustain happened, update to process the next phase...

  slot->ecnt = SCSP_ENV_DE;
  slot->einc = NULL;
  slot->ecmp = SCSP_ENV_DE + 1;
  slot->enxt = scsp_env_null_next;
}

static void
scsp_decay_next (slot_t *slot)
{
  // end of decay happened, update to process the next phase...

  slot->ecnt = slot->sl;
  slot->einc = &slot->eincs;
  slot->ecmp = SCSP_ENV_DE;
  slot->ecurp = SCSP_ENV_SUSTAIN;
  slot->enxt = scsp_sustain_next;
}

static void
scsp_attack_next (slot_t *slot)
{
  // end of attack happened, update to process the next phase...

  slot->ecnt = SCSP_ENV_DS;
  slot->einc = &slot->eincd;
  slot->ecmp = slot->sl;
  slot->ecurp = SCSP_ENV_DECAY;
  slot->enxt = scsp_decay_next;
}

////////////////////////////////////////////////////////////////
// Slot Access

static void
scsp_slot_refresh_einc (slot_t *slot, u32 adsr_bitmask)
{
  if (slot->arp && (adsr_bitmask & 0x1))
    slot->einca = slot->arp[(14 - slot->fsft) >> slot->krs];
  if (slot->drp && (adsr_bitmask & 0x2))
    slot->eincd = slot->drp[(14 - slot->fsft) >> slot->krs];
  if (slot->srp && (adsr_bitmask & 0x4))
    slot->eincs = slot->srp[(14 - slot->fsft) >> slot->krs];
  if (slot->rrp && (adsr_bitmask & 0x8))
    slot->eincr = slot->rrp[(14 - slot->fsft) >> slot->krs];
}

static void
scsp_slot_set_b (u32 s, u32 a, u8 d)
{

  slot_t *slot = &(scsp.slot[s]);

  //SCSPLOG("slot %d : reg %.2X = %.2X\n", s, a & 0x1F, d);

  scsp_isr[a ^ 3] = d;

  switch (a & 0x1F)
    {
    case 0x00: // KX/KB/SBCTL/SSCTL(high bit)
      slot->key = (d >> 3) & 1;
      slot->sbctl = (d >> 1) & 3;
      slot->ssctl = (slot->ssctl & 1) + ((d & 1) << 1);

      if (d & 0x10){
        SCSPLOG("slot %d : KeyOn", s);
        scsp_slot_keyonoff();
      }
      return;

    case 0x01: // SSCTL(low bit)/LPCTL/8B/SA(highest 4 bits)
      slot->ssctl = (slot->ssctl & 2) + ((d >> 7) & 1);
      slot->lpctl = (d >> 5) & 3;

      slot->pcm8b = d & 0x10;
      slot->sa = (slot->sa & 0x0FFFF) + ((d & 0xF) << 16);
      slot->sa &= SCSP_RAM_MASK;

      if (slot->ecnt < SCSP_ENV_DE) scsp_slot_update_keyon (slot);

      return;

    case 0x02: // SA(next highest byte)
      slot->sa = (slot->sa & 0xF00FF) + (d << 8);
      slot->sa &= SCSP_RAM_MASK;

      if (slot->ecnt < SCSP_ENV_DE) scsp_slot_update_keyon (slot);
      return;

    case 0x03: // SA(low byte)
      slot->sa = (slot->sa & 0xFFF00) + d;
      slot->sa &= SCSP_RAM_MASK;

      if (slot->ecnt < SCSP_ENV_DE) scsp_slot_update_keyon (slot);
      return;

    case 0x04: // LSA(high byte)
      slot->lsa = (slot->lsa & (0x00FF << SCSP_FREQ_LB)) +
                  (d << (8 + SCSP_FREQ_LB));
      return;

    case 0x05: // LSA(low byte)
      slot->lsa = (slot->lsa & (0xFF00 << SCSP_FREQ_LB)) +
                  (d << SCSP_FREQ_LB);
      return;

    case 0x06: // LEA(high byte)
      slot->lea = (slot->lea & (0x00FF << SCSP_FREQ_LB)) +
                  (d << (8 + SCSP_FREQ_LB));
      return;

    case 0x07: // LEA(low byte)
      slot->lea = (slot->lea & (0xFF00 << SCSP_FREQ_LB)) +
                  (d << SCSP_FREQ_LB);
      return;

    case 0x08: // D2R/D1R(highest 3 bits)
      slot->sr = (d >> 3) & 0x1F;
      slot->dr = (slot->dr & 0x03) + ((d & 7) << 2);

      if (slot->sr)
        slot->srp = &scsp_decay_rate[slot->sr << 1];
      else
        slot->srp = &scsp_null_rate[0];

      if (slot->dr)
        slot->drp = &scsp_decay_rate[slot->dr << 1];
      else
        slot->drp = &scsp_null_rate[0];

      scsp_slot_refresh_einc (slot, 0x2 | 0x4);
      return;

    case 0x09: // D1R(lowest 2 bits)/EGHOLD/AR
      slot->dr = (slot->dr & 0x1C) + ((d >> 6) & 3);
      slot->eghold = d & 0x20;
      slot->ar = d & 0x1F;

      if (slot->dr)
        slot->drp = &scsp_decay_rate[slot->dr << 1];
      else
        slot->drp = &scsp_null_rate[0];

      if (slot->ar)
        slot->arp = &scsp_attack_rate[slot->ar << 1];
      else
        slot->arp = &scsp_null_rate[0];

      scsp_slot_refresh_einc (slot, 0x1 | 0x2);
      return;

    case 0x0A: // LPSLNK/KRS/DL(highest 2 bits)
      slot->lslnk = d & 0x40;
      slot->krs = (d >> 2) & 0xF;

      if (slot->krs == 0xF)
        slot->krs = 4;
      else
        slot->krs >>= 2;

      slot->sl &= 0xE0 << SCSP_ENV_LB;
      slot->sl += (d & 3) << (8 + SCSP_ENV_LB);
      slot->sl += SCSP_ENV_DS; // adjusted for envelope compare (ecmp)

      scsp_slot_refresh_einc (slot, 0xF);
      return;

    case 0x0B: // DL(lowest 3 bits)/RR
      slot->sl &= 0x300 << SCSP_ENV_LB;
      slot->sl += (d & 0xE0) << SCSP_ENV_LB;
      slot->sl += SCSP_ENV_DS; // adjusted for envelope compare (ecmp)
      slot->rr = d & 0x1F;

      if (slot->rr)
        slot->rrp = &scsp_decay_rate[slot->rr << 1];
      else
        slot->rrp = &scsp_null_rate[0];

      scsp_slot_refresh_einc (slot, 0x8);
      return;

    case 0x0C: // STWINH/SDIR
      slot->sdir = d & 2;
      slot->swe = d & 1;
      return;

    case 0x0D: // TL
      slot->tl = scsp_tl_table[(d & 0xFF)];
      return;

    case 0x0E: // MDL/MDXSL(highest 4 bits)
      slot->mdl = (d >> 4) & 0xF; // need to adjust for correct shift
      slot->mdx = (slot->mdx & 3) + ((d & 0xF) << 2);
      return;

    case 0x0F: // MDXSL(lowest 2 bits)/MDYSL
      slot->mdx = (slot->mdx & 0x3C) + ((d >> 6) & 3);
      slot->mdy = d & 0x3F;
      return;

    case 0x10: // OCT/FNS(highest 2 bits)
      if (d & 0x40)
        slot->fsft = 23 - ((d >> 3) & 0xF);
      else
        slot->fsft = ((d >> 3) & 7) ^ 7;

      slot->finct = (slot->finct & 0x7F80) + ((d & 3) << (8 + 7));
      slot->finc = (0x20000 + slot->finct) >> slot->fsft;

      scsp_slot_refresh_einc (slot, 0xF);
      return;

    case 0x11: // FNS(low byte)
      slot->finct = (slot->finct & 0x18000) + (d << 7);
      slot->finc = (0x20000 + slot->finct) >> slot->fsft;
      return;

    case 0x12: // LFORE/LFOF/PLFOWS
      if (d & 0x80)
        {
          slot->lfoinc = -1;
          return;
        }
      else if (slot->lfoinc == -1)
        {
          slot->lfocnt = 0;
        }

      slot->lfoinc = scsp_lfo_step[(d >> 2) & 0x1F];

      switch (d & 3)
        {
        case 0:
          slot->lfofmw = scsp_lfo_sawt_f;
          return;

        case 1:
          slot->lfofmw = scsp_lfo_squa_f;
          return;

        case 2:
          slot->lfofmw = scsp_lfo_tri_f;
          return;

        case 3:
          slot->lfofmw = scsp_lfo_noi_f;
          return;
        }

    case 0x13: // PLFOS/ALFOWS/ALFOS
      if ((d >> 5) & 7)
        slot->lfofms = ((d >> 5) & 7) + 7;
      else
        slot->lfofms = 31;

      if (d & 7)
        slot->lfoems = ((d & 7) ^ 7) + 4;
      else
        slot->lfoems = 31;

      switch ((d >> 3) & 3)
        {
        case 0:
          slot->lfoemw = scsp_lfo_sawt_e;
          return;

        case 1:
          slot->lfoemw = scsp_lfo_squa_e;
          return;

        case 2:
          slot->lfoemw = scsp_lfo_tri_e;
          return;

        case 3:
          slot->lfoemw = scsp_lfo_noi_e;
        }
      return;

    case 0x15: // ISEL/OMXL
      if (d & 7)
        slot->imxl = ((d & 7) ^ 7) + SCSP_ENV_HB;
      else
        slot->imxl = 31;
      return;

    case 0x16: // DISDL/DIPAN
      if (d & 0xE0)
        {
          // adjusted for envelope calculation
          // some inaccuracy in panning though...
          slot->dislr = slot->disll = (((d >> 5) & 7) ^ 7) + SCSP_ENV_HB;
          if (d & 0x10)
            {
              // Panning Left
              if ((d & 0xF) == 0xF)
                slot->dislr = 31;
              else
                slot->dislr += (d >> 1) & 7;
            }
          else
            {
              // Panning Right
              if ((d & 0xF) == 0xF)
                slot->disll = 31;
              else
                slot->disll += (d >> 1) & 7;
            }
        }
      else
        {
          slot->dislr = slot->disll = 31; // muted
        }
      return;

    case 0x17: // EFSDL/EFPAN
      if (d & 0xE0)
        {
          slot->efslr = slot->efsll = (((d >> 5) & 7) ^ 7) + SCSP_ENV_HB;
          if (d & 0x10)
            {
              // Panning Left
              if ((d & 0xF) == 0xF)
                slot->efslr = 31;
              else
                slot->efslr += (d >> 1) & 7;
            }
          else
            {
              // Panning Right
              if ((d & 0xF) == 0xF)
                slot->efsll = 31;
              else
                slot->efsll += (d >> 1) & 7;
            }
        }
      else
        {
          slot->efslr = slot->efsll = 31; // muted
        }
      return;
    }
}

static void
scsp_slot_set_w (u32 s, s32 a, u16 d)
{
  slot_t *slot = &(scsp.slot[s]);

  SCSPLOG ("slot %d : reg %.2X = %.4X\n", s, a & 0x1E, d);

  *(u16 *)&scsp_isr[a ^ 2] = d;

  switch (a & 0x1E)
    {
    case 0x00: // KYONEX/KYONB/SBCTL/SSCTL/LPCTL/PCM8B/SA(highest 4 bits)
      slot->key = (d >> 11) & 1;
      slot->sbctl = (d >> 9) & 3;
      slot->ssctl = (d >> 7) & 3;
      slot->lpctl = (d >> 5) & 3;

      slot->pcm8b = d & 0x10;
      slot->sa = (slot->sa & 0x0FFFF) | ((d & 0xF) << 16);
      slot->sa &= SCSP_RAM_MASK;

      if (slot->ecnt < SCSP_ENV_DE)
        scsp_slot_update_keyon(slot);

      if (d & 0x1000)
        scsp_slot_keyonoff();
      return;

    case 0x02: // SA(low word)
      slot->sa = (slot->sa & 0xF0000) | d;
      slot->sa &= SCSP_RAM_MASK;

      if (slot->ecnt < SCSP_ENV_DE)
        scsp_slot_update_keyon(slot);
      return;

    case 0x04: // LSA
      slot->lsa = d << SCSP_FREQ_LB;
      return;

    case 0x06: // LEA
      slot->lea = d << SCSP_FREQ_LB;
      return;

    case 0x08: // D2R/D1R/EGHOLD/AR
      slot->sr = (d >> 11) & 0x1F;
      slot->dr = (d >> 6) & 0x1F;
      slot->eghold = d & 0x20;
      slot->ar = d & 0x1F;

      if (slot->sr)
        slot->srp = &scsp_decay_rate[slot->sr << 1];
      else
        slot->srp = &scsp_null_rate[0];

      if (slot->dr)
        slot->drp = &scsp_decay_rate[slot->dr << 1];
      else
        slot->drp = &scsp_null_rate[0];

      if (slot->ar)
        slot->arp = &scsp_attack_rate[slot->ar << 1];
      else
        slot->arp = &scsp_null_rate[0];

      scsp_slot_refresh_einc (slot, 0x1 | 0x2 | 0x4);
      return;

    case 0x0A: // LPSLNK/KRS/DL/RR
      slot->lslnk = (d >> 8) & 0x40;
      slot->krs = (d >> 10) & 0xF;

      if (slot->krs == 0xF)
        slot->krs = 4;
      else
        slot->krs >>= 2;

      slot->sl = ((d & 0x3E0) << SCSP_ENV_LB) + SCSP_ENV_DS; // adjusted for envelope compare (ecmp)
      slot->rr = d & 0x1F;

      if (slot->rr)
        slot->rrp = &scsp_decay_rate[slot->rr << 1];
      else
        slot->rrp = &scsp_null_rate[0];

      scsp_slot_refresh_einc (slot, 0xF);
      return;

    case 0x0C: // STWINH/SDIR
      slot->sdir = (d >> 8) & 2;
      slot->swe = (d >> 8) & 1;
      slot->tl = scsp_tl_table[(d & 0xFF)];
      return;

    case 0x0E: // MDL/MDXSL/MDYSL
      slot->mdl = (d >> 12) & 0xF; // need to adjust for correct shift
      slot->mdx = (d >> 6) & 0x3F;
      slot->mdy = d & 0x3F;
      return;

    case 0x10: // OCT/FNS
      if (d & 0x4000)
        slot->fsft = 23 - ((d >> 11) & 0xF);
      else
        slot->fsft = (((d >> 11) & 7) ^ 7);

      slot->finc = ((0x400 + (d & 0x3FF)) << 7) >> slot->fsft;

      scsp_slot_refresh_einc (slot, 0xF);
      return;

    case 0x12: // LFORE/LFOF/PLFOWS/PLFOS/ALFOWS/ALFOS
      if (d & 0x8000)
        {
          slot->lfoinc = -1;
          return;
        }
      else if (slot->lfoinc == -1)
        {
          slot->lfocnt = 0;
        }

      slot->lfoinc = scsp_lfo_step[(d >> 10) & 0x1F];
      if ((d >> 5) & 7)
        slot->lfofms = ((d >> 5) & 7) + 7;
      else
        slot->lfofms = 31;

      if (d & 7)
        slot->lfoems = ((d & 7) ^ 7) + 4;
      else
        slot->lfoems = 31;

      switch ((d >> 8) & 3)
        {
        case 0:
          slot->lfofmw = scsp_lfo_sawt_f;
          break;

        case 1:
          slot->lfofmw = scsp_lfo_squa_f;
          break;

        case 2:
          slot->lfofmw = scsp_lfo_tri_f;
          break;

        case 3:
          slot->lfofmw = scsp_lfo_noi_f;
          break;
        }

      switch ((d >> 3) & 3)
        {
        case 0:
          slot->lfoemw = scsp_lfo_sawt_e;
          return;

        case 1:
          slot->lfoemw = scsp_lfo_squa_e;
          return;

        case 2:
          slot->lfoemw = scsp_lfo_tri_e;
          return;

        case 3:
          slot->lfoemw = scsp_lfo_noi_e;
        }
      return;

    case 0x14: // ISEL/OMXL
      if (d & 7)
        slot->imxl = ((d & 7) ^ 7) + SCSP_ENV_HB;
      else
        slot->imxl = 31;
      return;

    case 0x16: // DISDL/DIPAN/EFSDL/EFPAN
      if (d & 0xE000)
        {
          // adjusted fr enveloppe calculation
          // some accuracy lose for panning here...
          slot->dislr = slot->disll = (((d >> 13) & 7) ^ 7) + SCSP_ENV_HB;
          if (d & 0x1000)
            {
              // Panning Left
              if ((d & 0xF00) == 0xF00)
                slot->dislr = 31;
              else
                slot->dislr += (d >> 9) & 7;
            }
          else
            {
              // Panning Right
              if ((d & 0xF00) == 0xF00)
                slot->disll = 31;
              else
                slot->disll += (d >> 9) & 7;
            }
        }
      else
        {
          slot->dislr = slot->disll = 31; // muted
        }

      if (d & 0xE0)
        {
          slot->efslr = slot->efsll = (((d >> 5) & 7) ^ 7) + SCSP_ENV_HB;
          if (d & 0x10)
            {
              // Panning Left
              if ((d & 0xF) == 0xF)
                slot->efslr = 31;
              else
                slot->efslr += (d >> 1) & 7;
            }
          else
            {
              // Panning Right
              if ((d & 0xF) == 0xF)
                slot->efsll = 31;
              else
                slot->efsll += (d >> 1) & 7;
            }
        }
      else
        {
          slot->efslr = slot->efsll = 31; // muted
        }
      return;
    }
}

static u8
scsp_slot_get_b (u32 s, u32 a)
{
  u8 val = scsp_isr[a ^ 3];

  // Mask out keyonx
  if ((a & 0x1F) == 0x00) val &= 0xEF;

  SCSPLOG ("r_b slot %d (%.2X) : reg %.2X = %.2X\n", s, a, a & 0x1F, val);

  return val;
}

static u16 scsp_slot_get_w(u32 s, u32 a)
{
  u16 val = *(u16 *)&scsp_isr[a ^ 2];

  if ((a & 0x1E) == 0x00) return val &= 0xEFFF;

  SCSPLOG ("r_w slot %d (%.2X) : reg %.2X = %.4X\n", s, a, a & 0x1E, val);

  return val;
}

////////////////////////////////////////////////////////////////
// SCSP Access

static void
scsp_set_b (u32 a, u8 d)
{
  if ((a != 0x408) && (a != 0x41D))
    {
      //SCSPLOG("scsp : reg %.2X = %.2X\n", a & 0x3F, d);
    }

  scsp_ccr[a ^ 3] = d;

  switch (a & 0x3F)
    {
    case 0x00: // MEM4MB/DAC18B
      scsp.dac18b = d&0x01;
      scsp.mem4b = (d >> 1) & 0x1;
      // Retail hardware has 4 Mbit (512 KB) sound RAM that stays linear
      // regardless of MEM4MB; folding the upper bank onto the lower one
      // corrupts the M68K driver when games stream into 0x40000-0x7FFFF.
      M68K->SetFetch(0x000000, 0x080000, (pointer)SoundRam);
      return;

    case 0x01: // VER/MVOL
      scsp.mvol = d & 0xF;
      return;

    case 0x02: // RBL(high bit)
      scsp.rbl = (scsp.rbl & 1) + ((d & 1) << 1);
      return;

    case 0x03: // RBL(low bit)/RBP
      scsp.rbl = (scsp.rbl & 2) + ((d >> 7) & 1);
      if (use_new_scsp)
         scsp.rbp = (d & 0x7F);
      else
         scsp.rbp = (d & 0x7F) * (4 * 1024 * 2);
      return;

    case 0x07: // MOBUF
      scsp_midi_out_send(d);
      return;

    case 0x08: // MSLC
      scsp.mslc = (d >> 3) & 0x1F;
      scsp_update_monitor ();
      return;

    case 0x12: // DMEAL(high byte)
      scsp.dmea = (scsp.dmea & 0x700FE) + (d << 8);
      return;

    case 0x13: // DMEAL(low byte)
      scsp.dmea = (scsp.dmea & 0x7FF00) + (d & 0xFE);
      return;

    case 0x14: // DMEAH(high byte)
      scsp.dmea = (scsp.dmea & 0xFFFE) + ((d & 0x70) << 12);
      scsp.drga = (scsp.drga & 0xFE) + ((d & 0xF) << 8);
      return;

    case 0x15: // DMEAH(low byte)
      scsp.drga = (scsp.drga & 0xF00) + (d & 0xFE);
      return;

    case 0x16: // DGATE/DDIR/DEXE/DTLG(upper 4 bits)
      scsp.dmlen = (scsp.dmlen & 0xFE) + ((d & 0xF) << 8);
      if ((scsp.dmfl = d & 0xF0) & 0x10) scsp_dma ();
      return;

    case 0x17: // DTLG(lower byte)
      scsp.dmlen = (scsp.dmlen & 0xF00) + (d & 0xFE);
      return;

    case 0x18: // TACTL
      scsp.timasd = d & 7;
      return;

    case 0x19: // TIMA
      scsp.timacnt = d << 8;
      return;

    case 0x1A: // TBCTL
      scsp.timbsd = d & 7;
      return;

    case 0x1B: // TIMB
      scsp.timbcnt = d << 8;
      return;

    case 0x1C: // TCCTL
      scsp.timcsd = d & 7;
      return;

    case 0x1D: // TIMC
      scsp.timccnt = d << 8;
      return;

    case 0x1E: // SCIEB(high byte)
    {
      int i;
      scsp.scieb = (scsp.scieb & 0xFF) + (d << 8);
      scsp_check_interrupt();
      return;
    }
    case 0x1F: // SCIEB(low byte)
    {
      int i;
      scsp.scieb = (scsp.scieb & 0x700) + d;
      scsp_check_interrupt();
      return;
    }
    case 0x21: // SCIPD(low byte)
      if (d & 0x20) scsp_sound_interrupt (0x20);
      return;

    case 0x22: // SCIRE(high byte)
      scsp.scipd &= ~(d << 8);
      scsp_check_interrupt();
      return;

    case 0x23: // SCIRE(low byte)
      scsp.scipd &= ~(u32)d;
      scsp_check_interrupt();
      return;

    case 0x25: // SCILV0
      scsp.scilv0 = d;
      scsp_check_interrupt();
      return;

    case 0x27: // SCILV1
      scsp.scilv1 = d;
      scsp_check_interrupt();
      return;

    case 0x29: // SCILV2
      scsp.scilv2 = d;
      scsp_check_interrupt();
      return;

    case 0x2A: // MCIEB(high byte)
      scsp.mcieb = (scsp.mcieb & 0xFF) + (d << 8);
      return;

    case 0x2B: // MCIEB(low byte)
      scsp.mcieb = (scsp.mcieb & 0x700) + d;
      return;

    case 0x2D: // MCIPD(low byte)
      if (d & 0x20)
        scsp_main_interrupt(0x20);

      SCSPLOG("scsp_set_b MCIPD %04X mcipd = %04X", d, scsp.mcipd.load());
      return;

    case 0x2E: // MCIRE(high byte)
    {
      //scsp.mcipd &= ~(d << 8);
      u32 expected = scsp.mcipd.load();
      u32 desired;
      do {
        desired = expected & ~(d << 8);
      } while (!scsp.mcipd.compare_exchange_weak(expected, desired));
      SCSPLOG("scsp_set_b MCIRE %04X mcipd = %04X", d, scsp.mcipd.load());
    }
      return;

    case 0x2F: // MCIRE(low byte)
//      scsp.mcipd &= ~(u32)d;
    {
      u32 expected = scsp.mcipd.load();
      u32 desired;
      do {
        desired = expected & ~(u32)d;
      } while (!scsp.mcipd.compare_exchange_weak(expected, desired));
      SCSPLOG("scsp_set_b MCIRE %04X mcipd = %04X", d, scsp.mcipd.load());
    }
      return;
    }
}

static void
scsp_set_w (u32 a, u16 d)
{
  if ((a != 0x418) && (a != 0x41A) && (a != 0x422))
    {
      //SCSPLOG("scsp : reg w %.2X = %.4X\n", a & 0x3E, d);
    }

  //SCSPLOG("SCSP REG WRITE WORD: addr=%08X, val = %04X\n",a,d);

  *(u16 *)&scsp_ccr[a ^ 2] = d;

  //if (0x2A == (a & 0x3E) 0x2C == (a & 0x3E) || 0x2E == (a & 0x3E) ) {
  //  SCSPLOG("scsp_set_w @0x%2X %04X\n", a, d);
  //}

  switch (a & 0x3E)
    {
    case 0x00: // MEM4MB/DAC18B/VER/MVOL
      scsp.dac18b = (d>>8) & 0x1;
      scsp.mem4b = (d >> 9) & 0x1;
      scsp.mvol = d & 0xF;
      // Sound RAM is always 512 KB linear on retail hardware (see scsp_set_b).
      M68K->SetFetch(0x000000, 0x080000, (pointer)SoundRam);
      return;

    case 0x02: // RBL/RBP
      scsp.rbl = (d >> 7) & 3;

      if (use_new_scsp)
         scsp.rbp = (d & 0x7F);
      else
         scsp.rbp = (d & 0x7F) * (4 * 1024 * 2);

      return;

    case 0x06: // MOBUF
      scsp_midi_out_send(d & 0xFF);
      return;

    case 0x08: // MSLC
      scsp.mslc = (d >> 11) & 0x1F;
      scsp_update_monitor();
      return;

    case 0x12: // DMEAL
      scsp.dmea = (scsp.dmea & 0x70000) + (d & 0xFFFE);
      return;

    case 0x14: // DMEAH/DRGA
      scsp.dmea = (scsp.dmea & 0xFFFE) + ((d & 0x7000) << 4);
      scsp.drga = d & 0xFFE;
      return;

    case 0x16: // DGATE/DDIR/DEXE/DTLG
      scsp.dmlen = d & 0xFFE;
      if ((scsp.dmfl = ((d >> 8) & 0xF0)) & 0x10) scsp_dma ();
      return;

    case 0x18: // TACTL/TIMA
      scsp.timasd = (d >> 8) & 7;
      scsp.timacnt = (d & 0xFF) << 8;
      return;

    case 0x1A: // TBCTL/TIMB
      scsp.timbsd = (d >> 8) & 7;
      scsp.timbcnt = (d & 0xFF) << 8;
      return;

    case 0x1C: // TCCTL/TIMC
      scsp.timcsd = (d >> 8) & 7;
      scsp.timccnt = (d & 0xFF) << 8;
      return;

    case 0x1E: // SCIEB
    {
      scsp.scieb = d;
      scsp_check_interrupt();
      return;
    }
    case 0x20: // SCIPD
      if (d & 0x20) scsp_sound_interrupt (0x20);
      return;

    case 0x22: // SCIRE
      scsp.scipd &= ~d;
      scsp_check_interrupt();
      return;

    case 0x24: // SCILV0
      scsp.scilv0 = d;
      return;

    case 0x26: // SCILV1
      scsp.scilv1 = d;
      return;

    case 0x28: // SCILV2
      scsp.scilv2 = d;
      return;

    case 0x2A: // MCIEB
    {
      int i;
      SCSPLOG("scsp_set_w MCIEB %04X", d);
      scsp.mcieb = d;
      auto mcipd = scsp.mcipd.load();
      for (i = 0; i < 11; i++)
        {
          if (scsp.mcieb & (1 << i) && mcipd & (1 << i))
            scsp_trigger_main_interrupt ((1 << i));
        }
      return;
    }

    case 0x2C: // MCIPD
      if (d & 0x20) scsp_main_interrupt (0x20);
      SCSPLOG("scsp_set_w MCIPD %04X mcipd = %04X", d, scsp.mcipd.load());
      return;

    case 0x2E: // MCIRE
      //scsp.mcipd &= ~d;
      u32 expected = scsp.mcipd.load();
      u32 desired;
      do {
        desired = expected & ~d;
      } while (!scsp.mcipd.compare_exchange_weak(expected, desired));
      SCSPLOG("scsp_set_w MCIRE %04X mcipd = %04X", d, scsp.mcipd.load());
      return;
    }
}

static u8
scsp_get_b (u32 a)
{
  a &= 0x3F;

  if ((a != 0x09) && (a != 0x21))
    {
      SCSPLOG("r_b s %.2X\n", a);
    }
//  if (a == 0x09) SCSPLOG("r_b scsp 09 = %.2X\n", ((scsp.slot[scsp.mslc].fcnt >> (SCSP_FREQ_LB + 12)) & 0x1) << 7);

  switch (a)
    {
    case 0x0:
      return  scsp.dac18b |  (scsp.mem4b<<1);
      break;
    case 0x01: // VER/MVOL
      scsp_ccr[a ^ 3] &= 0x0F;
      break;
    case 0x04: // Midi flags register
      return scsp.midflag;

    case 0x05: // MIBUF
      return scsp_midi_in_read();

    case 0x07: // MOBUF
      return scsp_midi_out_read();

    case 0x08: // CA(highest 3 bits)
      return (scsp.ca >> 8);

    case 0x09: // CA(lowest bit)/SGC/EG
      return (scsp.ca & 0xE0) | (scsp.sgc << 5) | scsp.eg;

    case 0x1E: // SCIEB(high byte)
      return (scsp.scieb >> 8);

    case 0x1F: // SCIEB(low byte)
      return scsp.scieb;

    case 0x20: // SCIPD(high byte)
      return (scsp.scipd >> 8);

    case 0x21: // SCIPD(low byte)
      return scsp.scipd;

    case 0x2C: // MCIPD(high byte)
      return (scsp.mcipd.load() >> 8);

    case 0x2D: // MCIPD(low byte)
      return scsp.mcipd.load();
    }

  return scsp_ccr[a ^ 3];
}

static u16
scsp_get_w (u32 a)
{
  a &= 0x3E;

  if ((a != 0x20) && (a != 0x08))
    {
      if (0x2C == a) {
       // SCSPLOG("scsp_get_w @0x%2X v%04X\n", a , scsp.mcipd);
      }
      else {
        SCSPLOG("scsp_get_w %.2X\n", a * 2);
      }
    }

  switch (a)
    {
    case 0x00: // MEM4MB/DAC18B/VER/MVOL
      *(u16 *)&scsp_ccr[a ^ 2] &= 0xFF0F;
      break;

    case 0x04: // Midi flags/MIBUF
    {
      u16 d = (scsp.midflag << 8); // this needs to be done to keep midfi status before midi in read
      d |= scsp_midi_in_read(); 
      return d;
    }

    case 0x06: // MOBUF
      return scsp_midi_out_read();

    case 0x08: // CA/SGC/EG
      return (scsp.ca & 0x780) | (scsp.sgc << 5) | scsp.eg;
     
    case 0x18: // TACTL
      return (scsp.timasd << 8);

    case 0x1A: // TBCTL
      return (scsp.timbsd << 8);

    case 0x1C: // TCCTL
      return (scsp.timcsd << 8);

    case 0x1E: // SCIEB
      return scsp.scieb;

    case 0x20: // SCIPD
      return scsp.scipd;

    case 0x22:
      return scsp.scipd;

    case 0xAC: // MCIPD
      SCSPLOG("scsp_get_w MCIEB %04X", scsp.mcieb);
      return scsp.mcieb;

    case 0x2C: // MCIPD
      SCSPLOG("scsp_get_w MCIPD %04X", scsp.mcipd.load());
      return scsp.mcipd.load();
    default:
      SCSPLOG("SCSP: unasined read %08X\n",a );
    }

  return *(u16 *)&scsp_ccr[a ^ 2];
}

////////////////////////////////////////////////////////////////
// Synth Slot
//
//      SCSPLOG("outL=%.8X bufL=%.8X disll=%d\n", outL, scsp_bufL[scsp_buf_pos], slot->disll);

////////////////////////////////////////////////////////////////

#ifdef WORDS_BIGENDIAN
#define SCSP_GET_OUT_8B \
  out = (s32) slot->buf8[(slot->fcnt >> SCSP_FREQ_LB)];
#else
#define SCSP_GET_OUT_8B \
  out = (s32) slot->buf8[(slot->fcnt >> SCSP_FREQ_LB) ^ 1];
#endif

#define SCSP_GET_OUT_16B \
  out = (s32) slot->buf16[slot->fcnt >> SCSP_FREQ_LB];

#define SCSP_GET_ENV \
  slot->env = scsp_env_table[slot->ecnt >> SCSP_ENV_LB] * slot->tl / 1024;

#define SCSP_GET_ENV_LFO \
  slot->env = (scsp_env_table[slot->ecnt >> SCSP_ENV_LB] * slot->tl / 1024) - \
              (slot->lfoemw[(slot->lfocnt >> SCSP_LFO_LB) & SCSP_LFO_MASK] >> \
               slot->lfoems);

#define SCSP_OUT_8B_L \
  if ((out) && (slot->env > 0))                            \
    {                                                      \
      out *= slot->env;                                    \
      scsp_bufL[scsp_buf_pos] += out >> (slot->disll - 8); \
    }

#define SCSP_OUT_8B_R \
  if ((out) && (slot->env > 0))                            \
    {                                                      \
      out *= slot->env;                                    \
      scsp_bufR[scsp_buf_pos] += out >> (slot->dislr - 8); \
    }

#define SCSP_OUT_8B_LR \
  if ((out) && (slot->env > 0))                            \
    {                                                      \
      out *= slot->env;                                    \
      scsp_bufL[scsp_buf_pos] += out >> (slot->disll - 8); \
      scsp_bufR[scsp_buf_pos] += out >> (slot->dislr - 8); \
    }

#define SCSP_OUT_16B_L \
  if ((out) && (slot->env > 0))                      \
    {                                                \
      out *= slot->env;                              \
      scsp_bufL[scsp_buf_pos] += out >> slot->disll; \
    }

#define SCSP_OUT_16B_R \
  if ((out) && (slot->env > 0))                      \
    {                                                \
      out *= slot->env;                              \
      scsp_bufR[scsp_buf_pos] += out >> slot->dislr; \
    }

#define SCSP_OUT_16B_LR \
  if ((out) && (slot->env > 0))                    \
  {                                                \
    out *= slot->env;                              \
    scsp_bufL[scsp_buf_pos] += out >> slot->disll; \
    scsp_bufR[scsp_buf_pos] += out >> slot->dislr; \
  }

#define SCSP_UPDATE_PHASE \
  if ((slot->fcnt += slot->finc) > slot->lea) \
  {                                           \
    if (slot->lpctl)                          \
      {                                       \
        slot->fcnt = slot->lsa;               \
      }                                       \
    else                                      \
      {                                       \
        slot->ecnt = SCSP_ENV_DE;             \
        return;                               \
      }                                       \
  }

#define SCSP_UPDATE_PHASE_LFO \
  slot->fcnt +=                                                      \
    ((slot->lfofmw[(slot->lfocnt >> SCSP_LFO_LB) & SCSP_LFO_MASK] << \
      (slot->lfofms-7)) >> (slot->fsft+1));                          \
  if ((slot->fcnt += slot->finc) > slot->lea)                        \
    {                                                                \
      if (slot->lpctl)                                               \
        {                                                            \
          slot->fcnt = slot->lsa;                                    \
        }                                                            \
      else                                                           \
        {                                                            \
          slot->ecnt = SCSP_ENV_DE;                                  \
          return;                                                    \
        }                                                            \
    }

#define SCSP_UPDATE_ENV \
  if (slot->einc) slot->ecnt += *slot->einc; \
  if (slot->ecnt >= slot->ecmp)              \
    {                                        \
      slot->enxt(slot);                      \
      if (slot->ecnt >= SCSP_ENV_DE) return; \
    }

#define SCSP_UPDATE_LFO \
  slot->lfocnt += slot->lfoinc;

////////////////////////////////////////////////////////////////

static void
scsp_slot_update_keyon (slot_t *slot)
{
  // set buffer, loop start/end address of the slot
  if (slot->pcm8b)
    {
      slot->buf8 = (s8*)&(scsp.scsp_ram[slot->sa]);

      if ((slot->sa + (slot->lea >> SCSP_FREQ_LB)) > SCSP_RAM_MASK)
        slot->lea = (SCSP_RAM_MASK - slot->sa) << SCSP_FREQ_LB;
    }
  else
    {
      slot->buf16 = (s16*)&(scsp.scsp_ram[slot->sa & ~1]);

      if ((slot->sa + (slot->lea >> (SCSP_FREQ_LB - 1))) > SCSP_RAM_MASK)
        slot->lea = (SCSP_RAM_MASK - slot->sa) << (SCSP_FREQ_LB - 1);
    }

  SCSP_UPDATE_PHASE
}

////////////////////////////////////////////////////////////////

static void
scsp_slot_update_null (slot_t *slot)
{
  for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
    {
      SCSP_GET_ENV

      SCSP_UPDATE_PHASE
      SCSP_UPDATE_ENV
    }
}

////////////////////////////////////////////////////////////////
// Normal 8 bits

static void
scsp_slot_update_8B_L (slot_t *slot)
{
  s32 out;

  for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
    {
      // env = [0..0x3FF] - slot->tl
      SCSP_GET_OUT_8B
      SCSP_GET_ENV

      // don't waste time if no sound...
      SCSP_OUT_8B_L

      // calculate new frequency (phase) counter and enveloppe counter
      SCSP_UPDATE_PHASE
      SCSP_UPDATE_ENV
    }
}

static void
scsp_slot_update_8B_R (slot_t *slot)
{
  s32 out;

  for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
    {
      SCSP_GET_OUT_8B
      SCSP_GET_ENV

      SCSP_OUT_8B_R

      SCSP_UPDATE_PHASE
      SCSP_UPDATE_ENV
    }
}

static void
scsp_slot_update_8B_LR(slot_t *slot)
{
  s32 out;

  for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
    {
      SCSP_GET_OUT_8B
      SCSP_GET_ENV

      SCSP_OUT_8B_LR

      SCSP_UPDATE_PHASE
      SCSP_UPDATE_ENV
    }
}

////////////////////////////////////////////////////////////////
// Envelope LFO modulation 8 bits

static void
scsp_slot_update_E_8B_L (slot_t *slot)
{
  s32 out;

  for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
    {
      SCSP_GET_OUT_8B
      SCSP_GET_ENV_LFO

      SCSP_OUT_8B_L

      SCSP_UPDATE_PHASE
      SCSP_UPDATE_ENV
      SCSP_UPDATE_LFO
    }
}

static void
scsp_slot_update_E_8B_R (slot_t *slot)
{
  s32 out;

  for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
    {
      SCSP_GET_OUT_8B
      SCSP_GET_ENV_LFO

      SCSP_OUT_8B_R

      SCSP_UPDATE_PHASE
      SCSP_UPDATE_ENV
      SCSP_UPDATE_LFO
    }
}

static void
scsp_slot_update_E_8B_LR (slot_t *slot)
{
  s32 out;

  for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
    {
      SCSP_GET_OUT_8B
      SCSP_GET_ENV_LFO

      SCSP_OUT_8B_LR

      SCSP_UPDATE_PHASE
      SCSP_UPDATE_ENV
      SCSP_UPDATE_LFO
    }
}

////////////////////////////////////////////////////////////////
// Frequency LFO modulation 8 bits

static void
scsp_slot_update_F_8B_L (slot_t *slot)
{
  s32 out;

  for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
    {
      SCSP_GET_OUT_8B
      SCSP_GET_ENV

      SCSP_OUT_8B_L

      SCSP_UPDATE_PHASE_LFO
      SCSP_UPDATE_ENV
      SCSP_UPDATE_LFO
    }
}

static void
scsp_slot_update_F_8B_R (slot_t *slot)
{
  s32 out;

  for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
    {
      SCSP_GET_OUT_8B
      SCSP_GET_ENV

      SCSP_OUT_8B_R

      SCSP_UPDATE_PHASE_LFO
      SCSP_UPDATE_ENV
      SCSP_UPDATE_LFO
    }
}

static void
scsp_slot_update_F_8B_LR (slot_t *slot)
{
  s32 out;

  for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
    {
      SCSP_GET_OUT_8B
      SCSP_GET_ENV

      SCSP_OUT_8B_LR

      SCSP_UPDATE_PHASE_LFO
      SCSP_UPDATE_ENV
      SCSP_UPDATE_LFO
    }
}

////////////////////////////////////////////////////////////////
// Enveloppe & Frequency LFO modulation 8 bits

static void
scsp_slot_update_F_E_8B_L (slot_t *slot)
{
  s32 out;

  for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
    {
      SCSP_GET_OUT_8B
      SCSP_GET_ENV_LFO

      SCSP_OUT_8B_L

      SCSP_UPDATE_PHASE_LFO
      SCSP_UPDATE_ENV
      SCSP_UPDATE_LFO
    }
}

static void
scsp_slot_update_F_E_8B_R (slot_t *slot)
{
  s32 out;

  for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
    {
      SCSP_GET_OUT_8B
      SCSP_GET_ENV_LFO

      SCSP_OUT_8B_R

      SCSP_UPDATE_PHASE_LFO
      SCSP_UPDATE_ENV
      SCSP_UPDATE_LFO
    }
}

static void scsp_slot_update_F_E_8B_LR(slot_t *slot)
{
  s32 out;

  for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
    {
      SCSP_GET_OUT_8B
      SCSP_GET_ENV_LFO

      SCSP_OUT_8B_LR

      SCSP_UPDATE_PHASE_LFO
      SCSP_UPDATE_ENV
      SCSP_UPDATE_LFO
    }
}

////////////////////////////////////////////////////////////////
// Normal 16 bits

static void
scsp_slot_update_16B_L (slot_t *slot)
{
  s32 out;

  for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
    {
      SCSP_GET_OUT_16B
      SCSP_GET_ENV

      SCSP_OUT_16B_L

      SCSP_UPDATE_PHASE
      SCSP_UPDATE_ENV
    }
}

static void
scsp_slot_update_16B_R (slot_t *slot)
{
  s32 out;

  for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
    {
      SCSP_GET_OUT_16B
      SCSP_GET_ENV

      SCSP_OUT_16B_R

      SCSP_UPDATE_PHASE
      SCSP_UPDATE_ENV
    }
}

static void
scsp_slot_update_16B_LR (slot_t *slot)
{
  s32 out;

  for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
    {
      SCSP_GET_OUT_16B
      SCSP_GET_ENV

      SCSP_OUT_16B_LR

      SCSP_UPDATE_PHASE
      SCSP_UPDATE_ENV
    }
}

////////////////////////////////////////////////////////////////
// Envelope LFO modulation 16 bits

static void
scsp_slot_update_E_16B_L (slot_t *slot)
{
  s32 out;

  for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
    {
      SCSP_GET_OUT_16B
      SCSP_GET_ENV_LFO

      SCSP_OUT_16B_L

      SCSP_UPDATE_PHASE
      SCSP_UPDATE_ENV
      SCSP_UPDATE_LFO
    }
}

static void
scsp_slot_update_E_16B_R (slot_t *slot)
{
  s32 out;

  for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
    {
      SCSP_GET_OUT_16B
      SCSP_GET_ENV_LFO

      SCSP_OUT_16B_R

      SCSP_UPDATE_PHASE
      SCSP_UPDATE_ENV
      SCSP_UPDATE_LFO
    }
}

static void
scsp_slot_update_E_16B_LR (slot_t *slot)
{
  s32 out;

  for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
    {
      SCSP_GET_OUT_16B
      SCSP_GET_ENV_LFO

      SCSP_OUT_16B_LR

      SCSP_UPDATE_PHASE
      SCSP_UPDATE_ENV
      SCSP_UPDATE_LFO
    }
}

////////////////////////////////////////////////////////////////
// Frequency LFO modulation 16 bits

static void
scsp_slot_update_F_16B_L (slot_t *slot)
{
  s32 out;

  for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
    {
      SCSP_GET_OUT_16B
      SCSP_GET_ENV

      SCSP_OUT_16B_L

      SCSP_UPDATE_PHASE_LFO
      SCSP_UPDATE_ENV
      SCSP_UPDATE_LFO
    }
}

static void
scsp_slot_update_F_16B_R (slot_t *slot)
{
  s32 out;

  for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
    {
      SCSP_GET_OUT_16B
      SCSP_GET_ENV

      SCSP_OUT_16B_R

      SCSP_UPDATE_PHASE_LFO
      SCSP_UPDATE_ENV
      SCSP_UPDATE_LFO
    }
}

static void
scsp_slot_update_F_16B_LR (slot_t *slot)
{
  s32 out;

  for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
    {
      SCSP_GET_OUT_16B
      SCSP_GET_ENV

      SCSP_OUT_16B_LR

      SCSP_UPDATE_PHASE_LFO
      SCSP_UPDATE_ENV
      SCSP_UPDATE_LFO
    }
}

////////////////////////////////////////////////////////////////
// Envelope & Frequency LFO modulation 16 bits

static void
scsp_slot_update_F_E_16B_L (slot_t *slot)
{
  s32 out;

  for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
    {
      SCSP_GET_OUT_16B
      SCSP_GET_ENV_LFO

      SCSP_OUT_16B_L

      SCSP_UPDATE_PHASE_LFO
      SCSP_UPDATE_ENV
      SCSP_UPDATE_LFO
    }
}

static void
scsp_slot_update_F_E_16B_R (slot_t *slot)
{
  s32 out;

  for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
    {
      SCSP_GET_OUT_16B
      SCSP_GET_ENV_LFO

      SCSP_OUT_16B_R

      SCSP_UPDATE_PHASE_LFO
      SCSP_UPDATE_ENV
      SCSP_UPDATE_LFO
    }
}

static void
scsp_slot_update_F_E_16B_LR (slot_t *slot)
{
  s32 out;

  for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
    {
      SCSP_GET_OUT_16B
      SCSP_GET_ENV_LFO

      SCSP_OUT_16B_LR

      SCSP_UPDATE_PHASE_LFO
      SCSP_UPDATE_ENV
      SCSP_UPDATE_LFO
    }
}

////////////////////////////////////////////////////////////////
// Update functions

static void (*scsp_slot_update_p[2][2][2][2][2])(slot_t *slot) =
{
  // NO FMS
  {  // NO EMS
    {  // 8 BITS
      {  // NO LEFT
        {  // NO RIGHT
          scsp_slot_update_null,
          // RIGHT
          scsp_slot_update_8B_R
        },
        // LEFT
        {  // NO RIGHT
          scsp_slot_update_8B_L,
          // RIGHT
          scsp_slot_update_8B_LR
        },
      },
      // 16 BITS
      {  // NO LEFT
        {  // NO RIGHT
          scsp_slot_update_null,
          // RIGHT
          scsp_slot_update_16B_R
        },
        // LEFT
        {  // NO RIGHT
          scsp_slot_update_16B_L,
          // RIGHT
          scsp_slot_update_16B_LR
        },
      }
    },
    // EMS
    {  // 8 BITS
      {  // NO LEFT
        {  // NO RIGHT
          scsp_slot_update_null,
          // RIGHT
          scsp_slot_update_E_8B_R
        },
        // LEFT
        {  // NO RIGHT
          scsp_slot_update_E_8B_L,
          // RIGHT
          scsp_slot_update_E_8B_LR
        },
      },
      // 16 BITS
      {  // NO LEFT
        {  // NO RIGHT
          scsp_slot_update_null,
          // RIGHT
          scsp_slot_update_E_16B_R
        },
        // LEFT
        {  // NO RIGHT
          scsp_slot_update_E_16B_L,
          // RIGHT
          scsp_slot_update_E_16B_LR
        },
      }
    }
  },
  // FMS
  {  // NO EMS
    {  // 8 BITS
      {  // NO LEFT
        {  // NO RIGHT
          scsp_slot_update_null,
          // RIGHT
          scsp_slot_update_F_8B_R
        },
        // LEFT
        {  // NO RIGHT
          scsp_slot_update_F_8B_L,
          // RIGHT
          scsp_slot_update_F_8B_LR
        },
      },
      // 16 BITS
      {  // NO LEFT
        {  // NO RIGHT
          scsp_slot_update_null,
          // RIGHT
          scsp_slot_update_F_16B_R
        },
        // LEFT
        {  // NO RIGHT
          scsp_slot_update_F_16B_L,
          // RIGHT
          scsp_slot_update_F_16B_LR
        },
      }
    },
    // EMS
    {  // 8 BITS
      {  // NO LEFT
        {  // NO RIGHT
          scsp_slot_update_null,
          // RIGHT
          scsp_slot_update_F_E_8B_R
        },
        // LEFT
        {  // NO RIGHT
          scsp_slot_update_F_E_8B_L,
          // RIGHT
          scsp_slot_update_F_E_8B_LR
        },
      },
      // 16 BITS
      {  // NO LEFT
        {  // NO RIGHT
          scsp_slot_update_null,
          // RIGHT
          scsp_slot_update_F_E_16B_R
        },
        // LEFT
        {  // NO RIGHT
          scsp_slot_update_F_E_16B_L,
          // RIGHT
          scsp_slot_update_F_E_16B_LR
        },
      }
    }
  }
};

void
scsp_update (s32 *bufL, s32 *bufR, u32 len)
{
   slot_t *slot;

   scsp_bufL = bufL;
   scsp_bufR = bufR;

   for (slot = &(scsp.slot[0]); slot < &(scsp.slot[32]); slot++)
   {
      if (slot->ecnt >= SCSP_ENV_DE) continue; // enveloppe null...

      if (slot->ssctl)
      {
         // Still not correct, but at least this fixes games
         // that rely on Call Address information
         scsp_buf_len = len;
         scsp_buf_pos = 0;

         for (; scsp_buf_pos < scsp_buf_len; scsp_buf_pos++)
         {
            if ((slot->fcnt += slot->finc) > slot->lea)
            {
               if (slot->lpctl) slot->fcnt = slot->lsa;
               else
               {
                  slot->ecnt = SCSP_ENV_DE;
                  break;
               }
            }
         }

         continue; // not yet supported!
      }

      scsp_buf_len = len;
      scsp_buf_pos = 0;

      // take effect sound volume if no direct sound volume...
      if ((slot->disll == 31) && (slot->dislr == 31))
      {
         slot->disll = slot->efsll;
         slot->dislr = slot->efslr;
      }

      // SCSPLOG("update : VL=%d  VR=%d CNT=%.8X STEP=%.8X\n", slot->disll, slot->dislr, slot->fcnt, slot->finc);

      scsp_slot_update_p[(slot->lfofms == 31) ? 0 : 1]
         [(slot->lfoems == 31) ? 0 : 1]
      [(slot->pcm8b == 0) ? 1 : 0]
      [(slot->disll == 31) ? 0 : 1]
      [(slot->dislr == 31) ? 0 : 1](slot);
   }

   u32 cdda_pos = cdda_read.load(std::memory_order_relaxed);
   u32 cdda_avail = cdda_written.load(std::memory_order_acquire) - cdda_pos;

   if (cdda_avail > 0)
   {
      if (len > cdda_avail / 4)
         scsp_buf_len = cdda_avail / 4;
      else
         scsp_buf_len = len;

      scsp_buf_pos = 0;

      /* May need to wrap around the buffer, so use nested loops */
      while (scsp_buf_pos < scsp_buf_len)
      {
         u8 *buf = &cddabuf.data[cdda_out_pos];

         u32 scsp_buf_target;
         u32 this_len = scsp_buf_len - scsp_buf_pos;
         if (this_len > (sizeof(cddabuf.data) - cdda_out_pos) / 4)
            this_len = (sizeof(cddabuf.data) - cdda_out_pos) / 4;
         scsp_buf_target = scsp_buf_pos + this_len;

         for (; scsp_buf_pos < scsp_buf_target; scsp_buf_pos++, buf += 4)
         {
            s32 out;

            out = (s32)(s16)((buf[1] << 8) | buf[0]);

            if (out)
               scsp_bufL[scsp_buf_pos] += out;

            out = (s32)(s16)((buf[3] << 8) | buf[2]);

            if (out)
               scsp_bufR[scsp_buf_pos] += out;
         }

         cdda_out_pos += this_len * 4;
         if (cdda_out_pos >= sizeof(cddabuf.data))
            cdda_out_pos = 0;

         cdda_pos += this_len * 4;
         cdda_read.store(cdda_pos, std::memory_order_release);
      }
   }
   else if (Cs2Area->isaudio)
   {
      SCSPLOG("WARNING: CDDA buffer underrun\n");
   }
}

void
scsp_update_monitor(void)
{
   if (use_new_scsp)
   {
      scsp.ca = new_scsp.slots[scsp.mslc].state.sample_offset >> 5;
      scsp.sgc = new_scsp.slots[scsp.mslc].state.envelope;
      scsp.eg = new_scsp.slots[scsp.mslc].state.attenuation >> 5;
   }
   else
   {
      slot_t *slot = &scsp.slot[scsp.mslc];
      scsp.ca = ((slot->fcnt >> (SCSP_FREQ_LB + 12)) & 0xF) << 7;
      scsp.sgc = slot->ecurp;
      scsp.eg = 0x1f - (slot->env >> (SCSP_ENV_HB - 5));
   }
#ifdef PSP
   WRITE_THROUGH(scsp.ca);
   WRITE_THROUGH(scsp.sgc);
   WRITE_THROUGH(scsp.eg);
#endif
}

void
scsp_update_timer (u32 len)
{
   scsp_sample_count += len;
   scsp.timacnt += len << (8 - scsp.timasd);

   if (scsp.timacnt >= 0xFF00)
   {
      scsp_sound_interrupt(0x40);
      scsp_main_interrupt(0x40);
      scsp.timacnt -= 0xFF00;
   }

   scsp.timbcnt += len << (8 - scsp.timbsd);

   if (scsp.timbcnt >= 0xFF00)
   {
      scsp_sound_interrupt(0x80);
      scsp_main_interrupt(0x80);
      scsp.timbcnt -= 0xFF00;
   }

   scsp.timccnt += len << (8 - scsp.timcsd);

   if (scsp.timccnt >= 0xFF00)
   {
      scsp_sound_interrupt(0x100);
      scsp_main_interrupt(0x100);
      scsp.timccnt -= 0xFF00;
   }

   // 1F interrupt can't be accurate here...
   if (len)
   {
      scsp_sound_interrupt(0x400);
      scsp_main_interrupt(0x400);
   }
}

////////////////////////////////////////////////////////////////
// MIDI

void
scsp_midi_in_send (u8 data)
{
  if (scsp.midflag & SCSP_MIDI_IN_EMP)
    {
      scsp_sound_interrupt(0x8);
      scsp_main_interrupt(0x8);
    }

  scsp.midflag &= ~SCSP_MIDI_IN_EMP;

  if (scsp.midincnt > 3)
    {
      scsp.midflag |= SCSP_MIDI_IN_OVF;
      return;
    }

  scsp.midinbuf[scsp.midincnt++] = data;

  if (scsp.midincnt > 3) scsp.midflag |= SCSP_MIDI_IN_FUL;
}

void
scsp_midi_out_send (u8 data)
{
  scsp.midflag &= ~SCSP_MIDI_OUT_EMP;

  if (scsp.midoutcnt > 3) return;

  scsp.midoutbuf[scsp.midoutcnt++] = data;

  if (scsp.midoutcnt > 3) scsp.midflag |= SCSP_MIDI_OUT_FUL;
}

u8
scsp_midi_in_read (void)
{
  u8 data;

  scsp.midflag &= ~(SCSP_MIDI_IN_OVF | SCSP_MIDI_IN_FUL);

  if (scsp.midincnt > 0)
    {
      if (scsp.midincnt > 1)
        {
          scsp_sound_interrupt(0x8);
          scsp_main_interrupt(0x8);
        }
      else
        {
          scsp.midflag |= SCSP_MIDI_IN_EMP;
        }

      data = scsp.midinbuf[0];

      switch ((--scsp.midincnt) & 3)
        {
        case 1:
          scsp.midinbuf[0] = scsp.midinbuf[1];
          break;

        case 2:
          scsp.midinbuf[0] = scsp.midinbuf[1];
          scsp.midinbuf[1] = scsp.midinbuf[2];
          break;

        case 3:
          scsp.midinbuf[0] = scsp.midinbuf[1];
          scsp.midinbuf[1] = scsp.midinbuf[2];
          scsp.midinbuf[2] = scsp.midinbuf[3];
          break;
        }

      return data;
    }

  return 0xFF;
}

u8
scsp_midi_out_read (void)
{
  u8 data;

  scsp.midflag &= ~SCSP_MIDI_OUT_FUL;

  if (scsp.midoutcnt > 0)
    {
      if (scsp.midoutcnt == 1)
        {
          scsp.midflag |= SCSP_MIDI_OUT_EMP;
          scsp_sound_interrupt(0x200);
          scsp_main_interrupt(0x200);
        }

      data = scsp.midoutbuf[0];

      switch (--scsp.midoutcnt & 3)
        {
        case 1:
          scsp.midoutbuf[0] = scsp.midoutbuf[1];
          break;

        case 2:
          scsp.midoutbuf[0] = scsp.midoutbuf[1];
          scsp.midoutbuf[1] = scsp.midoutbuf[2];
          break;

        case 3:
          scsp.midoutbuf[0] = scsp.midoutbuf[1];
          scsp.midoutbuf[1] = scsp.midoutbuf[2];
          scsp.midoutbuf[2] = scsp.midoutbuf[3];
          break;
        }

      return data;
    }

  return 0xFF;
}

////////////////////////////////////////////////////////////////
// Access

void FASTCALL
scsp_w_b (u32 a, u8 d)
{
  a &= 0xFFF;

  if (a < 0x400)
    {
      if (use_new_scsp){
        scsp_isr[a ^ 3] = d;
        scsp_slot_write_byte(&new_scsp, a, d);
      }
      else{
        scsp_slot_set_b(a >> 5, a, d);
      }
      FLUSH_SCSP ();
      return;
    }
  else if (a < 0x600)
    {
      if (a < 0x440)
        {
          scsp_set_b (a, d);
          FLUSH_SCSP ();
          return;
        }
    }
  else if (a < 0x700)
    {

    }
  else if (a >= 0x700 && a < 0x780)
  {
    u32 address = (a - 0x700)>>1;
    u16 current_val = (scsp_dsp.coef[address]<<3);
    if((a & 0x1) == 0){
      scsp_dsp.coef[address] = ((current_val & 0x00FF) | (u16)d<<8)>>3;
    }
    else{
      scsp_dsp.coef[address] = ((current_val & 0xFF00) | (u16)d)>>3;
    }
    return;
  }
  else if (a >= 0x780 && a < 0x7C0){
    u32 address = (a - 0x780)>>1;
    u16 current_val = scsp_dsp.madrs[address];
    if ((a & 0x1) == 0){
      scsp_dsp.madrs[address] = (current_val & 0x00FF) | (u16)d << 8;
    }
    else{
      scsp_dsp.madrs[address] = (current_val & 0xFF00) | (u16)d;
    }
    return;
  }
  else if (a >= 0x800 && a < 0xC00){
    u32 address = (a - 0x800) / 8;
    u64 current_val = scsp_dsp.mpro[address];

    switch (a & 0xf)
    {
    case 0:
    case 8:
      scsp_dsp.mpro[address] = (current_val & 0x00ffffffffffffff) | (u64)d << (u64)56;
      break;
    case 1:
    case 9:
      scsp_dsp.mpro[address] = (current_val & 0xff00ffffffffffff) | (u64)d << (u64)48;
      break;
    case 2:
    case 10:
      scsp_dsp.mpro[address] = (current_val & 0xffff00ffffffffff) | (u64)d << (u64)40;
      break;
    case 3:
    case 11:
      scsp_dsp.mpro[address] = (current_val & 0xffffff00ffffffff) | (u64)d << (u64)32;
      break;
    case 4:
    case 12:
      scsp_dsp.mpro[address] = (current_val & 0xffffffff00ffffff) | (u64)d << (u64)24;
      break;
    case 5:
    case 13:
      scsp_dsp.mpro[address] = (current_val & 0xffffffffff00ffff) | (u64)d << (u64)16;
      break;
    case 6:
    case 14:
      scsp_dsp.mpro[address] = (current_val & 0xffffffffffff00ff) | (u64)d << (u64)8;
      break;
    case 7:
    case 15:
      scsp_dsp.mpro[address] = (current_val & 0xffffffffffffff00) | (u64)d;
      break;
    default:
      break;
    }
    return;
  }
  else if (a > 0xC00 && a <= 0xee2)
    {
      SCSPLOG("WARNING: scsp dsp internal w_w to %08lx w/ %04x\n", a, d);
      a &= 0x3ff;
      scsp_dcr[a ^ 3] = d;
      return;
    }

  SCSPLOG("WARNING: scsp w_b to %08lx w/ %02x\n", a, d);
}

////////////////////////////////////////////////////////////////

void FASTCALL
scsp_w_w (u32 a, u16 d)
{
  if (a & 1)
    {
      SCSPLOG ("ERROR: scsp w_w misaligned : %.8X\n", a);
    }

  a &= 0xFFE;

  if (a < 0x400)
    {
      if (use_new_scsp){
        *(u16 *)&scsp_isr[a ^ 2] = d;
        scsp_slot_write_word(&new_scsp, a, d);
      }
      else{
        scsp_slot_set_w(a >> 5, a, d);
      }
      FLUSH_SCSP ();
      return;
    }
  else if (a < 0x600)
    {
      if (a < 0x440)
        {
          scsp_set_w (a, d);
          FLUSH_SCSP ();
          return;
        }
    }
  else if (a >= 0x700 && a < 0x780)
  {
     u32 address = (a - 0x700) / 2;
     scsp_dsp.coef[address] = d >> 3;//lower 3 bits seem to be discarded
     return;
  }
  else if (a >= 0x780 && a < 0x7BF)
  {
     u32 address = (a&0x3F) / 2;
     scsp_dsp.madrs[address] = d;
     return;
  }else if (a >= 0x800 && a < 0xC00)
  {
     u32 address = (a - 0x800) / 8;
     u64 current_val = scsp_dsp.mpro[address];

     switch (a & 0xf)
     {
     case 0:
     case 8:
       scsp_dsp.mpro[address] = (current_val & 0x0000ffffffffffff) | (u64)d << (u64)48;
       break;
     case 2:
     case 0xa:
        scsp_dsp.mpro[address] = (current_val & 0xffff0000ffffffff) | (u64)d << (u64)32;
        break;
     case 4:
     case 0xc:
        scsp_dsp.mpro[address] = (current_val  & 0xffffffff0000ffff) | (u64)d << (u64)16;
        break;
     case 6:
     case 0xe:
        scsp_dsp.mpro[address] = (current_val & 0xffffffffffff0000) | d;
        break;
     default:
        break;
     }
     scsp_dsp.updated = 1;
     return;
  }
  else if (a < 0xee4)
    {
      SCSPLOG("WARNING: scsp dsp internal w_w to %08lx w/ %04x\n", a, d);
      a &= 0x3ff;
      *(u16 *)&scsp_dcr[a ^ 2] = d;
      return;
    }

  SCSPLOG ("WARNING: scsp w_w to %08lx w/ %04x\n", a, d);
}

////////////////////////////////////////////////////////////////

void FASTCALL
scsp_w_d (u32 a, u32 d)
{
  if (a & 3)
    {
      SCSPLOG ("ERROR: scsp w_d misaligned : %.8X\n", a);
    }

  a &= 0xFFC;

  if (a < 0x400)
    {
       if (use_new_scsp)
       {
         *(u16 *)&scsp_isr[a ^ 2] = d >> 16;
          scsp_slot_write_word(&new_scsp, a + 0, d >> 16);
          *(u16 *)&scsp_isr[(a + 2) ^ 2] = d & 0xffff;
          scsp_slot_write_word(&new_scsp, a + 2, d & 0xffff);
       }
       else
       {
          scsp_slot_set_w(a >> 5, a + 0, d >> 16);
          scsp_slot_set_w(a >> 5, a + 2, d & 0xFFFF);
       }
      FLUSH_SCSP ();
      return;
    }
  else if (a < 0x600)
    {
      if (a < 0x440)
        {
          scsp_set_w (a + 0, d >> 16);
          scsp_set_w (a + 2, d & 0xFFFF);
          FLUSH_SCSP ();
          return;
        }
    }
  else if (a < 0x700)  {
  }else if (a >= 0xEC0 && a <= 0xEDF){
    scsp_dsp.efreg[ (a>>1) & 0x1F] = d;
  }else if (a < 0xee4)
    {
      a &= 0x3ff;
      *(u32 *)&scsp_dcr[a] = d;
      return;
    }

  SCSPLOG ("WARNING: scsp w_d to %08lx w/ %08lx\n", a, d);
}

////////////////////////////////////////////////////////////////

u8 FASTCALL
scsp_r_b (u32 a)
{
  a &= 0xFFF;

  if (a < 0x400)
    {
       if (use_new_scsp)
          return scsp_slot_read_byte(&new_scsp, a);
       else
         return scsp_slot_get_b(a >> 5, a);
    }
  else if (a < 0x600)
    {
      if (a < 0x440) return scsp_get_b(a);
    }
  else if (a < 0x700)
    {
  }else if (a >= 0xEC0 && a <= 0xEDF){
    u16 val = scsp_dsp.efreg[ (a>>1) & 0x1F];
    if( (a&0x01) == 0){
      return val >> 8;
    }else{
      return val & 0xFF;
    }
  }
  else if (a < 0xee4)
    {

    }

  SCSPLOG("WARNING: scsp r_b to %08lx\n", a);

  return 0;
}

////////////////////////////////////////////////////////////////

u16 FASTCALL
scsp_r_w (u32 a)
{
  if (a & 1)
    {
      SCSPLOG ("ERROR: scsp r_w misaligned : %.8X\n", a);
    }

  a &= 0xFFE;

  if (a < 0x400)
    {
       if (use_new_scsp)
          return scsp_slot_read_word(&new_scsp, a);
       else
          return scsp_slot_get_w(a >> 5, a);
    }
  else if (a < 0x600)
    {
      if (a < 0x440) return scsp_get_w (a);
    }
  else if (a < 0x700)
    {
       if (use_new_scsp)
       {
          u32 addr = a - 0x600;
          return new_scsp.sound_stack[(addr / 2) & 0x3f];
       }
    }
  else if (a >= 0x700 && a < 0x780)
  {
     u32 address = (a - 0x700) / 2;
     return scsp_dsp.coef[address] << 3;
  }
  else if (a >= 0x780 && a < 0x7A0)
  {
     u32 address = (a - 0x780) / 2;
     return scsp_dsp.madrs[address];
  }
  else if (a >= 0x7A0 && a < 0x7C0)
  {
     //madrs mirror
     u32 address = (a - 0x7A0) / 2;
     return scsp_dsp.madrs[address];
  }
  else if (a >= 0x800 && a < 0xC00)
  {
    u32 address = (a - 0x800) / 8;

    switch (a & 0xf)
    {
    case 0:
    case 8:
      return (scsp_dsp.mpro[address] >> (u64)48) & 0xffff;
      break;
    case 2:
    case 0xa:
      return (scsp_dsp.mpro[address] >> (u64)32) & 0xffff;
      break;
    case 4:
    case 0xc:
      return (scsp_dsp.mpro[address] >> (u64)16) & 0xffff;
      break;
    case 6:
    case 0xe:
      return scsp_dsp.mpro[address] & 0xffff;
      break;
    default:
      break;
    }
  } else if (a >= 0xE80 && a <= 0xEBF){
    if (!(a & 0x2)){
      return scsp_dsp.mixs[((a & 0x3F) >> 2)]&0x00000F;
    }
    else{
      return (scsp_dsp.mixs[((a & 0x3F) >> 2)]>>4)&0xFFFF;
    }
  }else if (a >= 0xEC0 && a <= 0xEDF){
    return scsp_dsp.efreg[ (a>>1) & 0x1F];
  }else if (a == 0xee0) { 
    return scsp_dsp.exts[0]; 
  }else if (a == 0xee2) { 
    return scsp_dsp.exts[1]; 
  }

  SCSPLOG ("WARNING: scsp r_w to %08lx\n", a);

  return 0;
}

////////////////////////////////////////////////////////////////

u32 FASTCALL
scsp_r_d (u32 a)
{
  if (a & 3)
    {
      SCSPLOG ("ERROR: scsp r_d misaligned : %.8X\n", a);
    }

  a &= 0xFFC;

  if (a < 0x400)
    {
       if (use_new_scsp)
          return (scsp_slot_read_word(&new_scsp, a + 0) << 16) | scsp_slot_read_word(&new_scsp, a + 2);
       else
          return (scsp_slot_get_w(a >> 5, a + 0) << 16) | scsp_slot_get_w(a >> 5, a + 2);
    }
  else if (a < 0x600)
    {
      if (a < 0x440) return (scsp_get_w (a + 0) << 16) | scsp_get_w (a + 2);
    }
  else if (a < 0x700)
    {

    }
  else if (a < 0xee4)
    {
      if (a == 0xee0) { return ((u32)(scsp_dsp.exts[0]) << 16) | (u32)scsp_dsp.exts[1]; }
    }

  SCSPLOG("WARNING: scsp r_d to %08lx\n", a);

  return 0;
}

////////////////////////////////////////////////////////////////
// Interface

void
scsp_shutdown (void)
{

}

void
scsp_reset (void)
{
  slot_t *slot;

  memset(scsp_reg, 0, 0x1000);

  scsp.mem4b     = 0;
  scsp.mvol      = 0;
  scsp.rbl       = 0;
  scsp.rbp       = 0;
  scsp.mslc      = 0;
  scsp.ca        = 0;

  scsp.dmea      = 0;
  scsp.drga      = 0;
  scsp.dmfl      = 0;
  scsp.dmlen     = 0;

  scsp.midincnt  = 0;
  scsp.midoutcnt = 0;
  scsp.midflag   = SCSP_MIDI_IN_EMP | SCSP_MIDI_OUT_EMP;
  scsp.midflag2  = 0;

  scsp.timacnt   = 0xFF00;
  scsp.timbcnt   = 0xFF00;
  scsp.timccnt   = 0xFF00;
  scsp.timasd    = 0;
  scsp.timbsd    = 0;
  scsp.timcsd    = 0;

  scsp.mcieb     = 0;
  scsp.mcipd.store(0);
  scsp.scieb     = 0;
  scsp.scipd     = 0;
  scsp.scilv0    = 0;
  scsp.scilv1    = 0;
  scsp.scilv2    = 0;

  for(slot = &(scsp.slot[0]); slot < &(scsp.slot[32]); slot++)
    {
      memset(slot, 0, sizeof(slot_t));
      slot->ecnt = SCSP_ENV_DE;       // slot off
      slot->ecurp = SCSP_ENV_RELEASE;
      slot->dislr = slot->disll = 31; // direct level sound off
      slot->efslr = slot->efsll = 31; // effect level sound off

		// Make sure lfofmw/lfoemw have sane values
		slot->lfofmw = scsp_lfo_sawt_f;
		slot->lfoemw = scsp_lfo_sawt_e;
    }

  if (use_new_scsp)
     new_scsp_reset(&new_scsp);

  // Every interrupt source (SCIPD/SCIEB/SCILV) is gone now, so the request
  // towards the sound CPU has to be deasserted as well (mednafen does the same
  // via RecalcSoundInt() at the end of its SCSP reset).
  if (M68K != NULL && M68K->SetIRQ != NULL)
    M68K->SetIRQ(0);
}

void
scsp_init (u8 *scsp_ram, void (*sint_hand)(u32), void (*mint_hand)(void))
{
  u32 i, j;
  double x;

  scsp_shutdown ();

  scsp_isr = &scsp_reg[0x0000];
  scsp_ccr = &scsp_reg[0x0400];
  scsp_dcr = &scsp_reg[0x0700];

  scsp.scsp_ram = scsp_ram;
  scsp.sintf = sint_hand;
  scsp.mintf = mint_hand;

  for (i = 0; i < SCSP_ENV_LEN; i++)
    {
      // Attack Curve (x^7 ?)
      x = pow (((double)(SCSP_ENV_MASK - i) / (double)SCSP_ENV_LEN), 7);
      x *= (double)SCSP_ENV_LEN;
      scsp_env_table[i] = SCSP_ENV_MASK - (s32)x;

      // Decay curve (x = linear)
      x = pow (((double)i / (double)SCSP_ENV_LEN), 1);
      x *= (double)SCSP_ENV_LEN;
      scsp_env_table[i + SCSP_ENV_LEN] = SCSP_ENV_MASK - (s32)x;
    }

  for (i = 0, j = 0; i < 32; i++)
    {
      j += 1 << (i >> 2);

      // lfo freq
      x = (SCSP_FREQ / 256.0) / (double)j;

      // converting lfo freq in lfo step
      scsp_lfo_step[31 - i] = scsp_round(x * ((double)SCSP_LFO_LEN /
                                              (double)SCSP_FREQ) *
                                         (double)(1 << SCSP_LFO_LB));
    }

  // Calculate LFO (modulation) values
  for (i = 0; i < SCSP_LFO_LEN; i++)
    {
      // Envelope modulation
      scsp_lfo_sawt_e[i] = SCSP_LFO_MASK - i;

      if (i < (SCSP_LFO_LEN / 2))
        scsp_lfo_squa_e[i] = SCSP_LFO_MASK;
      else
        scsp_lfo_squa_e[i] = 0;

      if (i < (SCSP_LFO_LEN / 2))
        scsp_lfo_tri_e[i] = SCSP_LFO_MASK - (i * 2);
      else
        scsp_lfo_tri_e[i] = (i - (SCSP_LFO_LEN / 2)) * 2;

      scsp_lfo_noi_e[i] = rand() & SCSP_LFO_MASK;

      // Frequency modulation
      scsp_lfo_sawt_f[(i + 512) & SCSP_LFO_MASK] = i - (SCSP_LFO_LEN / 2);

      if (i < (SCSP_LFO_LEN / 2))
        scsp_lfo_squa_f[i] = SCSP_LFO_MASK - (SCSP_LFO_LEN / 2) - 128;
      else
        scsp_lfo_squa_f[i] = 0 - (SCSP_LFO_LEN / 2) + 128;

      if (i < (SCSP_LFO_LEN / 2))
        scsp_lfo_tri_f[(i + 768) & SCSP_LFO_MASK] = (i * 2) -
                                                    (SCSP_LFO_LEN / 2);
      else
        scsp_lfo_tri_f[(i + 768) & SCSP_LFO_MASK] =
          (SCSP_LFO_MASK - ((i - (SCSP_LFO_LEN / 2)) * 2)) -
          (SCSP_LFO_LEN / 2) + 1;

      scsp_lfo_noi_f[i] = scsp_lfo_noi_e[i] - (SCSP_LFO_LEN / 2);
    }

  for(i = 0; i < 4; i++)
    {
      scsp_attack_rate[i] = 0;
      scsp_decay_rate[i] = 0;
    }

  for(i = 0; i < 60; i++)
    {
      x = 1.0 + ((i & 3) * 0.25);                  // bits 0-1 : x1.00, x1.25, x1.50, x1.75
      x *= (double)(1 << ((i >> 2)));              // bits 2-5 : shift bits (x2^0 - x2^15)
      x *= (double)(SCSP_ENV_LEN << SCSP_ENV_LB);  // adjust for table scsp_env_table

      scsp_attack_rate[i + 4] = scsp_round(x / (double)SCSP_ATTACK_R);
      scsp_decay_rate[i + 4] = scsp_round(x / (double)SCSP_DECAY_R);

      if (scsp_attack_rate[i + 4] == 0) scsp_attack_rate[i + 4] = 1;
      if (scsp_decay_rate[i + 4] == 0) scsp_decay_rate[i + 4] = 1;
    }

  scsp_attack_rate[63] = SCSP_ENV_AE;
  scsp_decay_rate[61] = scsp_decay_rate[60];
  scsp_decay_rate[62] = scsp_decay_rate[60];
  scsp_decay_rate[63] = scsp_decay_rate[60];

  for(i = 64; i < 96; i++)
    {
      scsp_attack_rate[i] = scsp_attack_rate[63];
      scsp_decay_rate[i] = scsp_decay_rate[63];
      scsp_null_rate[i - 64] = 0;
    }
#if 0
  for(i = 0; i < 96; i++)
    {
      SCSPLOG ("attack rate[%d] = %.8X -> %.8X\n", i, scsp_attack_rate[i],
               scsp_attack_rate[i] >> SCSP_ENV_LB);
      SCSPLOG ("decay rate[%d] = %.8X -> %.8X\n", i, scsp_decay_rate[i],
               scsp_decay_rate[i] >> SCSP_ENV_LB);
    }
#endif
  for(i = 0; i < 256; i++)
    scsp_tl_table[i] = scsp_round(pow(10, ((double)i * -0.3762) / 20) * 1024.0);

  scsp_reset();
  thread_running = 0;
  //YabAddEventQueue(q_scsp_frame_start, 0);
  g_scsp_mtx = YabThreadCreateMutex();
}

//////////////////////////////////////////////////////////////////////////////
// Yabause specific
//////////////////////////////////////////////////////////////////////////////

u8 *SoundRam = NULL;
ScspInternal *ScspInternalVars;
static SoundInterface_struct *SNDCore = NULL;
extern "C" SoundInterface_struct *SNDCoreList[];

struct sounddata
{
  u32 *data32;
} scspchannel[2];

static u32 scspsoundlen;        // Samples to output per frame
static u32 scsplines;           // Lines per frame
static u32 scspsoundbufs;       // Number of "scspsoundlen"-sample buffers
static u32 scspsoundbufsize;    // scspsoundlen * scspsoundbufs
static u32 scspsoundgenpos;     // Offset of next byte to generate
static u32 scspsoundoutleft;    // Samples not yet sent to host driver

static int
scsp_alloc_bufs (void)
{
  if (scspchannel[0].data32)
    free(scspchannel[0].data32);
  scspchannel[0].data32 = NULL;
  if (scspchannel[1].data32)
    free(scspchannel[1].data32);
  scspchannel[1].data32 = NULL;

  scspchannel[0].data32 = (u32 *)calloc(scspsoundbufsize, sizeof(u32));
  if (scspchannel[0].data32 == NULL)
    return -1;
  scspchannel[1].data32 = (u32 *)calloc(scspsoundbufsize, sizeof(u32));
  if (scspchannel[1].data32 == NULL)
    return -1;

  return 0;
}

static u8 IsM68KRunning;
static s32 FASTCALL (*m68kexecptr)(s32 cycles);  // M68K->Exec or M68KExecBP
static s32 savedcycles;  // Cycles left over from the last M68KExec() call

//////////////////////////////////////////////////////////////////////////////

static u32 FASTCALL
c68k_byte_read (const u32 adr)
{
  u32 rtn = 0;
  if (adr < 0x100000) {
    if (adr < 0x80000) {
      rtn = T2ReadByte(SoundRam, adr & 0x7FFFF);
    }
  }
  else
    rtn = scsp_r_b(adr);
  return rtn;
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL
c68k_byte_write (const u32 adr, u32 data)
{
  if (adr < 0x100000){
    //if ((adr & 0xFFF) == 0x790){
    //  SCSPLOG("c68k_word_write %08X:%02X\n", adr, data);
    //}
    if (adr < 0x80000) {
      T2WriteByte(SoundRam, adr & 0x7FFFF, data);
      scsp_hq_note_ram_write(adr, 1);
      // keep the SH2 caches coherent with sound-CPU writes
      cache_update_external_write(0x05A00000 | (adr & 0x7FFFF), data, 1);
    }
  }
  else{
    scsp_w_b(adr, data);
  }
}

//////////////////////////////////////////////////////////////////////////////

/* exported to m68kd.c */
u32 FASTCALL
c68k_word_read (const u32 adr)
{
  u32 rtn = 0;
  if (adr < 0x100000) {
    if (adr < 0x80000) {
      rtn = T2ReadWord(SoundRam, adr);
    }
  }
  else
    rtn = scsp_r_w(adr);
  return rtn;
}

//////////////////////////////////////////////////////////////////////////////

static void FASTCALL
c68k_word_write (const u32 adr, u32 data)
{
  if (adr < 0x100000){
//    if ((adr & 0x7FFF0) == 0x3c20){
//      SCSPLOG("c68k_word_write %08X:%04X @ %d\n", adr, data, (m68kcycle >> CLOCK_SYNC_SHIFT) );
//    }
    if (adr < 0x80000) {
      T2WriteWord(SoundRam, adr, data);
      scsp_hq_note_ram_write(adr, 2);
      // keep the SH2 caches coherent with sound-CPU writes
      cache_update_external_write(0x05A00000 | (adr & 0x7FFFF), data, 2);
    }
  }
  else{
    scsp_w_w(adr, data);
  }
}

//////////////////////////////////////////////////////////////////////////////

static void
c68k_interrupt_handler (u32 level)
{
  // send interrupt to 68k
  M68K->SetIRQ ((s32)level);
}

//////////////////////////////////////////////////////////////////////////////

static void
scu_interrupt_handler (void)
{
  // send interrupt to scu
  ScuSendSoundRequest ();
  SCSPLOG("ScuSendSoundRequest");
}

//////////////////////////////////////////////////////////////////////////////

// scsp_main_mode==0 only: before the SH2 touches sound RAM, wait until the
// SCSP thread has executed the M68K up to every cycle already granted by the
// SH2 side. This keeps the host-memory order of SH2/M68K stores identical to
// their emulated order and prevents lost mailbox flags (e.g. Techno Motor).
// scsp_main_mode==0 only: SH2 sound RAM writes are staged in this FIFO and
// committed as complete bursts, so the sound CPU can never observe a
// half-written command block. Real hardware never shows such states because
// the SH2 finishes its mailbox bursts orders of magnitude faster than the
// 68K reacts. The FIFO lives entirely on the emulation thread (no locking):
// it is flushed before any SH2 sound RAM read, at the per-frame handshake
// (SyncCPUtoSCSP), on M68KStart, and when full.
#define SH2_SRAM_FIFO_SIZE 1024
typedef struct { u32 addr; u32 val; u8 size; } Sh2SramWrite;
static Sh2SramWrite sh2_sram_fifo[SH2_SRAM_FIFO_SIZE];
static int sh2_sram_fifo_count = 0;

static void Sh2SramFifoCommit(void)
{
  int i;
  for (i = 0; i < sh2_sram_fifo_count; i++) {
    Sh2SramWrite *w = &sh2_sram_fifo[i];
    if (w->size == 1)      T2WriteByte(SoundRam, w->addr, (u8)w->val);
    else if (w->size == 2) T2WriteWord(SoundRam, w->addr, (u16)w->val);
    else                   T2WriteLong(SoundRam, w->addr, w->val);
    scsp_hq_note_ram_write(w->addr, w->size);
    if (M68K) M68K->WriteNotify(w->addr, w->size);
  }
  sh2_sram_fifo_count = 0;
}

static void Sh2DrainM68k(void)
{
  if (g_scsp_main_mode != 0 || thread_running == 0 || !IsM68KRunning || g_scsp_lock)
    return;

  // This thread (the emulation thread) is the only writer of the m68k
  // counter, so the target cannot move while we wait here.
  u64 target = getM68KCounter() >> SCSP_FRACTIONAL_BITS;
  if (m68k_done_upto.load(std::memory_order_acquire) >= target)
    return;

  int spins = 0;
  while (m68k_done_upto.load(std::memory_order_acquire) < target &&
         thread_running && !g_scsp_lock &&
         !m68k_at_frame_boundary.load(std::memory_order_acquire)) {
    YabThreadYield();
    // safety valve: never hard-block the emulation thread
    if (++spins > 10000) {
      // watchdog: repeated valve hits mean the SCSP thread is not draining
      static u32 valve_hits = 0;
      valve_hits++;
      if ((valve_hits & 0xFF) == 1) {
        printf("scsp0 drain valve: target=%llu done=%llu hits=%u\n",
               (unsigned long long)target,
               (unsigned long long)m68k_done_upto.load(), valve_hits);
      }
      break;
    }
  }
}

// Returns 1 when the write was staged into the burst FIFO (mode 0), or 0
// when the caller must perform the write directly.
static INLINE int Sh2SramStageWrite(u32 addr, u32 val, u8 size)
{
  if (g_scsp_main_mode != 0 || thread_running == 0 || !IsM68KRunning)
    return 0;
  if (sh2_sram_fifo_count >= SH2_SRAM_FIFO_SIZE) {
    Sh2DrainM68k();
    Sh2SramFifoCommit();
  }
  sh2_sram_fifo[sh2_sram_fifo_count].addr = addr;
  sh2_sram_fifo[sh2_sram_fifo_count].val = val;
  sh2_sram_fifo[sh2_sram_fifo_count].size = size;
  sh2_sram_fifo_count++;
  return 1;
}

// Exported: commit staged SH2 sound RAM writes (emulation thread only).
void ScspCommitSh2SoundRamWrites(void)
{
  Sh2SramFifoCommit();
}

u8 FASTCALL
SoundRamReadByte (u32 addr)
{
  addr &= 0xFFFFF;
  u8 val = 0;

  Sh2DrainM68k();
  Sh2SramFifoCommit();

  // Sound RAM is 512 KB linear on retail hardware; MEM4MB does not fold the
  // upper 256 KB onto the lower bank (mednafen parity). Folding corrupted the
  // M68K driver for games streaming into 0x40000-0x7FFFF (Gungriffon FMV).
  if (addr > 0x7FFFF)
    return 0xFF;

  val = T2ReadByte(SoundRam, addr);
  //SCSPLOG("SoundRamReadByte %08X:%02X",addr,val);
  return val;
}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL
SoundRamWriteByte (u32 addr, u8 val)
{
  addr &= 0xFFFFF;

  // Sound RAM is 512 KB linear on retail hardware (see SoundRamReadByte).
  if (addr > 0x7FFFF)
    return;

  //SCSPLOG("SoundRamWriteByte %08X:%02X", addr, val);
  if (!Sh2SramStageWrite(addr, val, 1)) {
    T2WriteByte (SoundRam, addr, val);
    scsp_hq_note_ram_write(addr, 1);
    M68K->WriteNotify (addr, 1);
  }
}

//////////////////////////////////////////////////////////////////////////////

// From CPU
int sh2_read_req = 0;
static int mem_access_counter = 0;
void SyncSh2And68k(){
  if (IsM68KRunning) {
    /*
    #if defined(ARCH_IS_LINUX)
    pthread_mutex_lock(&sync_mutex);
    pthread_cond_signal(&sync_cnd);
    pthread_mutex_unlock(&sync_mutex);
    #else
    sh2_read_req++;
    #endif
    */
    // Memory Access cycle = 128 times per 44.1Khz
    // 28437500 / 4410 / 128 = 50
    //SH2Core->AddCycle(MSH2, 50);
    //SH2Core->AddCycle(SSH2, 50);

    if (mem_access_counter++ >= 128) {
#if defined(ARCH_IS_LINUX)
      pthread_mutex_lock(&sync_mutex);
      pthread_cond_signal(&sync_cnd);
      pthread_mutex_unlock(&sync_mutex);
#else
      sh2_read_req++;
      YabThreadYield();
#endif  
      mem_access_counter = 0;
    }
  }
}

u16 FASTCALL
SoundRamReadWord (u32 addr)
{
  addr &= 0xFFFFF;
  u16 val = 0;

  Sh2DrainM68k();
  Sh2SramFifoCommit();

  // Sound RAM is 512 KB linear on retail hardware (see SoundRamReadByte).
  if (addr > 0x7FFFF)
    return 0xFFFF;

  
  SyncSh2And68k();

  val = T2ReadWord (SoundRam, addr);

  //LOG("SoundRamReadWord %08X:%08X time=%d", addr, val, MSH2->cycles);
  return val;

}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL
SoundRamWriteWord (u32 addr, u16 val)
{
  addr &= 0xFFFFF;

  // Sound RAM is 512 KB linear on retail hardware (see SoundRamReadByte).
  if (addr > 0x7FFFF)
    return;
  //LOG("SoundRamWriteWord %08X:%04X", addr, val);
  if (!Sh2SramStageWrite(addr, val, 2)) {
    T2WriteWord (SoundRam, addr, val);
    scsp_hq_note_ram_write(addr, 2);
    M68K->WriteNotify (addr, 2);
  }
  //SyncSh2And68k();
}

//////////////////////////////////////////////////////////////////////////////

u32 FASTCALL
SoundRamReadLong (u32 addr)
{
  addr &= 0xFFFFF;
  u32 val;
  u32 pre_cycle = m68kcycle;

  Sh2DrainM68k();
  Sh2SramFifoCommit();

  // Sound RAM is 512 KB linear on retail hardware (see SoundRamReadByte).
  if (addr > 0x7FFFF) {
    val = 0xFFFFFFFF;
    SyncSh2And68k();
    return val;
  }

  SyncSh2And68k();

  val = T2ReadLong(SoundRam, addr);

  return val;

}

//////////////////////////////////////////////////////////////////////////////

void FASTCALL
SoundRamWriteLong (u32 addr, u32 val)
{
  addr &= 0xFFFFF;
  //u32 pre_cycle = m68kcycle;

  // Sound RAM is 512 KB linear on retail hardware (see SoundRamReadByte).
  if (addr > 0x7FFFF)
    return;

  //LOG("SoundRamWriteLong %08X:%08X", addr, val);
  if (!Sh2SramStageWrite(addr, val, 4)) {
    T2WriteLong (SoundRam, addr, val);
    scsp_hq_note_ram_write(addr, 4);
    M68K->WriteNotify (addr, 4);
  }
  //SyncSh2And68k();

}

//////////////////////////////////////////////////////////////////////////////

int
ScspInit (int coreid, int scsp_sync_count_per_frame, int scsp_main_mode )
{
  int i;

  // the high-quality output survives a re-init: rebuild its renderer here,
  // before the SCSP thread exists
  if (ScspGetHighQuality())
    scsp_hq_create_renderer();

  if ((SoundRam = T2MemoryInit (0x80000)) == NULL)
    return -1;

  if ((ScspInternalVars = (ScspInternal *)calloc(1, sizeof(ScspInternal))) == NULL)
    return -1;

  if (M68K->Init () != 0)
    return -1;

  M68K->SetReadB ((C68K_READ *)c68k_byte_read);
  M68K->SetReadW ((C68K_READ *)c68k_word_read);
  M68K->SetWriteB ((C68K_WRITE *)c68k_byte_write);
  M68K->SetWriteW ((C68K_WRITE *)c68k_word_write);

  // Sound RAM is 512 KB linear on retail hardware (see SoundRamReadByte).
  M68K->SetFetch (0x000000, 0x080000, (pointer)SoundRam);

  IsM68KRunning = 0;

  scsp_init (SoundRam, &c68k_interrupt_handler, &scu_interrupt_handler);
  ScspInternalVars->scsptiming1 = 0;
  ScspInternalVars->scsptiming2 = 0;

  for (i = 0; i < MAX_BREAKPOINTS; i++)
    ScspInternalVars->codebreakpoint[i].addr = 0xFFFFFFFF;
  ScspInternalVars->numcodebreakpoints = 0;
  ScspInternalVars->BreakpointCallBack = NULL;
  ScspInternalVars->inbreakpoint = 0;

  m68kexecptr = M68K->Exec;

  // Allocate enough memory for each channel buffer(may have to change)
  scspsoundlen = 44100 / 60; // assume it's NTSC timing
  scsplines = 263;
  scspsoundbufs = 10; // should be enough to prevent skipping
  scspsoundbufsize = scspsoundlen * scspsoundbufs;
  if (scsp_alloc_bufs () < 0)
    return -1;

  // Reset output pointers
  scspsoundgenpos = 0;
  scspsoundoutleft = 0;

  g_scsp_lock = 0;
  
  g_scsp_sync_count_per_frame = scsp_sync_count_per_frame;
  if (g_scsp_sync_count_per_frame <= 0) {
    g_scsp_sync_count_per_frame = 1;
  }
  if (g_scsp_sync_count_per_frame >= 256 ) {
    g_scsp_sync_count_per_frame = 256;
  }

  g_scsp_main_mode = scsp_main_mode;
  return ScspChangeSoundCore (coreid);
}

//////////////////////////////////////////////////////////////////////////////

int
ScspChangeSoundCore (int coreid)
{
  int i;

  // Make sure the old core is freed
  if (SNDCore)
    SNDCore->DeInit();

  // So which core do we want?
  if (coreid == SNDCORE_DEFAULT)
    coreid = 0; // Assume we want the first one

  // Go through core list and find the id
  for (i = 0; SNDCoreList[i] != NULL; i++)
    {
      if (SNDCoreList[i]->id == coreid)
        {
          // Set to current core
          SNDCore = SNDCoreList[i];
          break;
        }
    }

  if (SNDCore == NULL)
    {
      SNDCore = &SNDDummy;
      return -1;
    }

  if (SNDCore->Init () == -1)
    {
      // Since it failed, instead of it being fatal, we'll just use the dummy
      // core instead

      // This might be helpful though.
      YabSetError (YAB_ERR_CANNOTINIT, (void *)SNDCore->Name);

      SNDCore = &SNDDummy;
    }

  if (SNDCore)
    {
      if (scsp_mute_flags) SNDCore->MuteAudio();
      else SNDCore->UnMuteAudio();
      SNDCore->SetVolume(scsp_volume);
    }

  return 0;
}

//////////////////////////////////////////////////////////////////////////////

void
ScspDeInit (void)
{
  ScspUnMuteAudio(1);
  scsp_mute_flags = 0;
  thread_running = 0;
#if defined(ASYNC_SCSP)
  //if (q_scsp_finish) YabAddEventQueue(q_scsp_finish, 0);
  if (q_scsp_frame_start)YabAddEventQueue(q_scsp_frame_start, 0);
  YabThreadWait(YAB_THREAD_SCSP);
#endif

  // The SCSP thread has stopped. The on/off setting is kept; ScspInit()
  // builds a new renderer if it is on.
  {
    std::lock_guard<std::mutex> lock(scsp_hq_create_mutex);
    delete scsp_hq_renderer;
    scsp_hq_renderer = NULL;
  }

  if (scspchannel[0].data32)
    free(scspchannel[0].data32);
  scspchannel[0].data32 = NULL;

  if (scspchannel[1].data32)
    free(scspchannel[1].data32);
  scspchannel[1].data32 = NULL;

  if (SNDCore)
    SNDCore->DeInit();
  SNDCore = NULL;

  scsp_shutdown();

  if (SoundRam)
    T2MemoryDeInit (SoundRam);
  SoundRam = NULL;
}

//////////////////////////////////////////////////////////////////////////////

void
M68KStart (void)
{
  // make sure no staged SH2 writes (e.g. the driver upload) are pending
  Sh2SramFifoCommit();
  M68K->Reset ();
  //ScspReset();
  savedcycles = 0;
  IsM68KRunning = 1;
}

//////////////////////////////////////////////////////////////////////////////

void
M68KStop (void)
{
  M68K->Reset();
  //ScspReset();
  IsM68KRunning = 0;
}

//////////////////////////////////////////////////////////////////////////////

int
M68KIsRunning (void)
{
  return IsM68KRunning;
}

//////////////////////////////////////////////////////////////////////////////

void
ScspReset (void)
{
  g_scsp_lock = 1;
  YabThreadUSleep(100000);
  scsp_reset();
  g_scsp_lock = 0;
}

//////////////////////////////////////////////////////////////////////////////

int
ScspChangeVideoFormat (int type)
{
  scspsoundlen = 44100 / (type ? 50 : 60);
  scsplines = type ? 313 : 263;
  scspsoundbufsize = scspsoundlen * scspsoundbufs;

  if (scsp_alloc_bufs () < 0)
    return -1;

  SNDCore->ChangeVideoFormat (type ? 50 : 60);

  return 0;
}

//////////////////////////////////////////////////////////////////////////////

/* Process breakpoints in a separate function to avoid unnecessary register
 * spillage on the fast path (and to avoid too much block nesting) */
#ifdef __GNUC__
__attribute__((noinline))
#endif
static s32 FASTCALL M68KExecBP (s32 cycles);

#if defined(ASYNC_SCSP)
void M68KExec(s32 cycles){}
void MM68KExec(s32 cycles)
#else
void M68KExec(s32 cycles)
#endif
{
  s32 newcycles = savedcycles - cycles;
  if (LIKELY(IsM68KRunning))
    {
      if (LIKELY(newcycles < 0))
        {
          s32 cyclestoexec = -newcycles;
          newcycles += (*m68kexecptr)(cyclestoexec);
        }
      savedcycles = newcycles;
    }
}

void new_scsp_run_sample()
{
   s16 out_l = 0;
   s16 out_r = 0;

   s16 cd_in_l = 0;
   s16 cd_in_r = 0;

   // Acquire: pairs with the producer's release store, so the sector bytes
   // below cdda_written are visible before they are read.
   u32 cdda_pos = cdda_read.load(std::memory_order_relaxed);
   u32 cdda_avail = cdda_written.load(std::memory_order_acquire) - cdda_pos;

   if (cdda_avail >= 4)
   {
      const u8 *buf = &cddabuf.data[cdda_out_pos];

      cd_in_l = (s16)((buf[1] << 8) | buf[0]);
      cd_in_r = (s16)((buf[3] << 8) | buf[2]);

      cdda_out_pos += 4;
      if (cdda_out_pos >= sizeof(cddabuf.data))
         cdda_out_pos = 0;

      cdda_read.store(cdda_pos + 4, std::memory_order_release);
   }

   scsp_update_timer(1);
   generate_sample(&new_scsp, scsp.rbp, scsp.rbl, &out_l, &out_r, scsp.mvol, cd_in_l, cd_in_r);

   if (new_scsp_outbuf_pos < 900)
   {
      new_scsp_outbuf_l[new_scsp_outbuf_pos] = out_l;
      new_scsp_outbuf_r[new_scsp_outbuf_pos] = out_r;
   }
   else
   {
      //buffer overrun
   }

   scsp_update_monitor();
   new_scsp_outbuf_pos++;
}

void new_scsp_exec(s32 cycles)
{
   s32 cycles_temp = new_scsp_cycles - cycles;
   if (cycles_temp < 0)
   {
      new_scsp_run_sample();
      cycles_temp += 512;
   }
   new_scsp_cycles = cycles_temp;
}

//----------------------------------------------------------------------------

static s32 FASTCALL
M68KExecBP (s32 cycles)
{
  s32 cyclestoexec=cycles;
  s32 cyclesexecuted=0;
  int i;

  while (cyclesexecuted < cyclestoexec)
    {
      // Make sure it isn't one of our breakpoints
      for (i = 0; i < ScspInternalVars->numcodebreakpoints; i++)
        {
          if ((M68K->GetPC () == ScspInternalVars->codebreakpoint[i].addr) &&
              ScspInternalVars->inbreakpoint == 0)
            {
              ScspInternalVars->inbreakpoint = 1;
              if (ScspInternalVars->BreakpointCallBack)
                ScspInternalVars->BreakpointCallBack (ScspInternalVars->codebreakpoint[i].addr);
              ScspInternalVars->inbreakpoint = 0;
            }
        }

      // execute instructions individually
      cyclesexecuted += M68K->Exec(1);

    }
  return cyclesexecuted;
}

//////////////////////////////////////////////////////////////////////////////

void
M68KStep (void)
{
  M68K->Exec(1);
}

//////////////////////////////////////////////////////////////////////////////

// Wait for background execution to finish (used on PSP)
#if defined(ASYNC_SCSP)
void M68KSync(void){}
void MM68KSync (void)
#else
void M68KSync(void)
#endif
{
  M68K->Sync();
}



//////////////////////////////////////////////////////////////////////////////

void
ScspConvert32uto16s (s32 *srcL, s32 *srcR, s16 *dst, u32 len)
{
  u32 i;

  for (i = 0; i < len; i++)
    {
      // Left Channel
      if (*srcL > 0x7FFF)
        *dst = 0x7FFF;
      else if (*srcL < -0x8000)
        *dst = -0x8000;
      else
        *dst = *srcL;

      srcL++;
      dst++;

      // Right Channel
      if (*srcR > 0x7FFF)
        *dst = 0x7FFF;
      else if (*srcR < -0x8000)
        *dst = -0x8000;
      else
        *dst = *srcR;

      srcR++;
      dst++;
    }
}

//////////////////////////////////////////////////////////////////////////////

void
ScspReceiveCDDA (const u8 *sector)
{
   // Note: an older version steered the CD block's sector delivery pitch
   // from here to keep this ring buffer near half full (2x boost below
   // half, 60/s above 3/4). The real CD block always delivers CD-DA at 75
   // sectors/s, and games that pace themselves on the CD block's FAD
   // progress (e.g. EMIT Vol.1 voiced scenes) ran up to 2x too fast while
   // the boost was active. An underrun simply plays silence, which is also
   // what the real SCSP outputs while the drive seeks.

  u32 written = cdda_written.load(std::memory_order_relaxed);
  u32 used = written - cdda_read.load(std::memory_order_acquire);

  if (used > sizeof(cddabuf.data) - 2352)
    {
      // Dropping the incoming sector rather than overwriting the oldest one:
      // the reader owns everything below cdda_written, and moving that line
      // under it is what the old clamp did.
      SCSPLOG ("WARNING: CDDA buffer overrun\n");
      return;
    }

  memcpy(cddabuf.data+cdda_in_pos, sector, 2352);
  cdda_in_pos += 2352;
  if (cdda_in_pos >= sizeof(cddabuf.data))
     cdda_in_pos = 0;

  // Release: publishes the sector bytes written above to the consumer.
  cdda_written.store(written + 2352, std::memory_order_release);
}


void new_scsp_update_samples(s32 *bufL, s32 *bufR, int scspsoundlen)
{
   int i;
   for (i = 0; i < new_scsp_outbuf_pos; i++)
   {
      if (i >= scspsoundlen)
         break;

      bufL[i] = new_scsp_outbuf_l[i];
      bufR[i] = new_scsp_outbuf_r[i];
   }

   new_scsp_outbuf_pos = 0;
}

void ScspLockThread() {
  g_scsp_lock = 1;
  YabThreadUSleep(16666*2);
}

void ScspUnLockThread() {
  g_scsp_lock = 0;
}


//////////////////////////////////////////////////////////////////////////////
#if !defined(ASYNC_SCSP)
void ScspExec(){
  u32 audiosize;

  ScspInternalVars->scsptiming2 +=
	  ((scspsoundlen << 16) + scsplines / 2) / scsplines;
  if (!use_new_scsp)
	  scsp_update_timer(ScspInternalVars->scsptiming2 >> 16); // Pass integer part
  ScspInternalVars->scsptiming2 &= 0xFFFF; // Keep fractional part
  ScspInternalVars->scsptiming1++;
#else


#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#define __STDC_FORMAT_MACROS

extern "C" void * ScspAsynMainCpuTime( void * p ){

  s64 before;
  s64 now;
  s64 difftime;
  const int samplecnt = 256; // 11289600/44100
  const int step = 16;
  int frame = 0;
  int frame_count = 0;
  int i;
  int frame_div = g_scsp_sync_count_per_frame;
  int framecnt = (yabsys.IsPal ? 225792 : 188160) / frame_div; // 11289600/50 or 11289600/60
  int hzcheck = 0;

#if defined(ARCH_IS_LINUX)
  struct timespec tm;
  setpriority( PRIO_PROCESS, 0, -20);
#endif
  if( yabsys.use_cpu_affinity ){
    YabThreadSetCurrentThreadAffinityMask( YabThreadGetFastestCpuIndex() );
  }
  before = YabauseGetTicks();
  u32 wait_clock = 0;
  u64 pre_m68k_cycle = 0;
  u64 m68k_inc = 0;
  int flush_timer_carry = 0;

  framecnt = yabsys.IsPal ? 225792 : 188160; // 11289600/50 or 11289600/60

  //YabWaitEventQueue(q_scsp_frame_start);
  now = 0;
  before = 0;
  while (thread_running){
    while (g_scsp_lock) { YabThreadUSleep(1000); }
    u64 m68k_done_counter = 0;
    u64 m68k_integer_part = 0;
    u64 m68k_cycle = 0;

    // End-of-frame handshake with the emulation thread. Triggered when the
    // executed-cycle position (pre_m68k_cycle - m68k_inc) reaches framecnt.
    // Everything (counter, pre, inc, done) resets to zero per frame, so any
    // transient accounting disturbance (counter true-up/regression, reset)
    // self-heals at the next boundary instead of drifting.
    auto scsp_frame_boundary = [&]() {
      // parked state first: SH2 drains must not wait on us from here on
      m68k_at_frame_boundary.store(1, std::memory_order_release);

      ScspInternalVars->scsptiming2 = 0;
      ScspInternalVars->scsptiming1 = scsplines;
      ScspExecAsync();

      YabAddEventQueue( q_scsp_finish , 0);
      // cycles granted beyond framecnt (counter rounding) are discarded
      pre_m68k_cycle = 0;
      m68k_inc = 0;
      m68k_done_upto.store(0, std::memory_order_release);
      //LOG("[SCSP] WAIT SH2");
      YabWaitEventQueue(q_scsp_frame_start);
      m68k_at_frame_boundary.store(0, std::memory_order_release);
      now = YabauseGetTicks();
      //LOG(" SCSPTIME = %d/16666666 %d/735", (s32)(now - before), hzcheck);
      hzcheck = 0;
      before = now;
    };

    u64 dbg_spin = 0;
    do {
      m68k_integer_part = getM68KCounter() >> SCSP_FRACTIONAL_BITS;
      if (thread_running == 0) break;
      // counter regression = the emulation thread reset it (YabauseReset
      // etc.): drop pending work and resync instead of computing a bogus
      // huge delta
      if (m68k_integer_part < pre_m68k_cycle) {
        pre_m68k_cycle = m68k_integer_part;
        m68k_inc = 0;
        m68k_cycle = 0;
        m68k_done_upto.store(m68k_integer_part, std::memory_order_release);
        // The emulation thread's VBlank snap (yabause.c setM68kCounter of the
        // exact frame total) is a small counter REGRESSION whenever the
        // per-deciline grants had already accumulated past one frame. If we
        // had read the higher pre-snap value (pre > framecnt) but had not yet
        // consumed up to the boundary, the resync above lands exactly on
        // pre == framecnt with inc == 0 -- and this spin loop has no other
        // path that fires the frame boundary in that state. The emulation
        // thread then waits on q_scsp_finish forever (observed deadlock:
        // watchdog counter=pre=188160, inc=0). Fire the boundary here so the
        // handshake always completes.
        if (pre_m68k_cycle >= (u64)framecnt) {
          scsp_frame_boundary();
        }
        continue;
      }
      m68k_cycle = m68k_integer_part - pre_m68k_cycle;
      // We caught up with the granted cycle counter: consume any sub-chunk
      // remainder now. The 256-cycle chunking below is only an efficiency
      // choice; consuming the tail here keeps the SH2 drain exact.
      if (m68k_inc > 0) {
        s32 c = (s32)m68k_inc;
        MM68KExec(c);
        if (use_new_scsp) {
          new_scsp_exec(c << 1);
        }
        else {
          // legacy engine: tick the timer once per accumulated sample worth
          flush_timer_carry += c;
          while (flush_timer_carry >= samplecnt) {
            flush_timer_carry -= samplecnt;
            scsp_update_timer(1);
          }
        }
        m68k_inc = 0;
        if (pre_m68k_cycle >= (u64)framecnt) {
          scsp_frame_boundary();
        }
        else {
          m68k_done_upto.store(pre_m68k_cycle, std::memory_order_release);
        }
      }
      // watchdog: this state should never persist; log it if it does
      dbg_spin++;
      if ((dbg_spin & 0x7FFFFFF) == 0) {
        printf("scsp0 watchdog: counter=%llu pre=%llu inc=%llu fcnt=%d lock=%d\n",
               (unsigned long long)m68k_integer_part,
               (unsigned long long)pre_m68k_cycle,
               (unsigned long long)m68k_inc, framecnt, g_scsp_lock);
      }
    } while (m68k_cycle == 0);

    m68k_inc += m68k_cycle;
    pre_m68k_cycle = m68k_integer_part;

    // Sync 44100KHz
    while (m68k_inc >= samplecnt) {
      m68k_inc = m68k_inc - samplecnt;
      //LOG("[SCSP] MM68KExec %d", samplecnt);
      MM68KExec(samplecnt);
      if (use_new_scsp) {
        new_scsp_exec((samplecnt << 1));
      }
      else {
        scsp_update_timer(1);
      }
      hzcheck++;

      m68k_done_upto.store(pre_m68k_cycle - m68k_inc, std::memory_order_release);

      if (pre_m68k_cycle - m68k_inc >= (u64)framecnt) {
        scsp_frame_boundary();
        break;
      }
    }
    setM68kDoneCounter(pre_m68k_cycle);
  }

  q_scsp_frame_start = 0;
  q_scsp_finish = 0;
  YabThreadWake(YAB_THREAD_SCSP);
  return NULL;
}


extern "C" void * ScspAsynMainRealtime(void * p) {

  s64 before;
  s64 now;
  s64 difftime;
  const int samplecnt = 256; // 11289600/44100
  const int step = 16;
  int frame = 0; 
  int frame_count = 0;
  int i;
  int frame_div = g_scsp_sync_count_per_frame;
  int framecnt = (yabsys.IsPal ? 225792 : 188160) / frame_div; // 11289600/50 or 11289600/60
  int hzcheck = 0;

#if defined(ARCH_IS_LINUX)
  struct timespec tm;
  struct sched_param thread_param;
  thread_param.sched_priority = 15; //sched_get_priority_max(SCHED_FIFO);
  if ( pthread_setschedparam(pthread_self(), SCHED_FIFO, &thread_param) < -1 ) {
    LOG("sched_setscheduler");
  }
  setpriority( PRIO_PROCESS, 0, -10);
#endif

  // Special for Thunder Force V
  char * pCurrentGame = Cs2GetCurrentGmaecode();
  if (!strcmp(pCurrentGame, "T-1811G") && frame_div < 4) {
    frame_div = 4; 
    framecnt = (yabsys.IsPal ? 225792 : 188160) / frame_div;
    LOG("Thunder Force V is detected. Force frame_div to 4");
  }

  const u32 base_clock = (u32)((644.8412698 / ((double)samplecnt / (double)step)) * (1 << CLOCK_SYNC_SHIFT));

  if( yabsys.use_cpu_affinity ){
    YabThreadSetCurrentThreadAffinityMask(YabThreadGetFastestCpuIndex());
  }
  
  before = YabauseGetTicks();
  u32 wait_clock = 0;

  now = 0;
  before = 0;
  while (thread_running) {
    while (g_scsp_lock) { YabThreadUSleep(1000); }
    // Run 1 sample(44100Hz)
    for (i = 0; i < samplecnt; i += step) {
      MM68KExec(step);
      m68kcycle += base_clock;
    }

    wait_clock = 0;

    if (use_new_scsp) {
      new_scsp_exec((samplecnt << 1));
    }
    else {
      scsp_update_timer(1);
    }

    // Sync 1/4 Frame(60Hz)
    frame += samplecnt;
    if (frame >= framecnt) {
      frame = frame - framecnt;
      frame_count++;
      if (frame_count >= frame_div) {
        ScspInternalVars->scsptiming2 = 0;
        ScspInternalVars->scsptiming1 = scsplines;
        ScspExecAsync();
        frame_count = 0;
      }
      s64 sleeptime = 0;
      s64 initsleeptime = 0;
      s64 initnow = 0;
      s64 initbefore = 0;
      s64 checktime = 0;
      s64 sleepchecktime = 0;
      m68kcycle = 0;
      sh2_read_req = 0;
      do {
        now = YabauseGetTicks();
        if (now >= before){
          difftime = (now - before) * 1000000000L / yabsys.tickfreq;
        }
        else {
          difftime = (now + (LLONG_MAX - before)) * 1000000000L / yabsys.tickfreq;
        }
        sleeptime = ((16666666L / frame_div) - difftime);
        if(initsleeptime==0){
          initsleeptime = sleeptime; 
          initnow = now; 
          initbefore = before;
        }
        if( sleeptime < 0 ) break;
#if defined(ANDROID)
        //tm.tv_sec = 0;
        //tm.tv_nsec = sleeptime;

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &tm);
        ts.tv_nsec = tm.tv_nsec + sleeptime;
        if (tm.tv_nsec > ts.tv_nsec) {
          tm.tv_sec += 1;
        }
        tm.tv_nsec = ts.tv_nsec;

        pthread_mutex_lock(&sync_mutex);
        int rtn = pthread_cond_timedwait(&sync_cnd, &sync_mutex, &tm);
        if (rtn == 0) {
          for (i = 0; i < samplecnt; i += step) {
            MM68KExec(step);
            m68kcycle += base_clock;
          }
          frame += samplecnt;
          if (use_new_scsp) {
            new_scsp_exec((samplecnt << 1));
          }
          else {
            scsp_update_timer(1);
          }
        }
        pthread_mutex_unlock(&sync_mutex);
#elif defined(ARCH_IS_LINUX)    
        time(&tm);    
        long n = tm.tv_nsec;
        tm.tv_nsec += sleeptime;
        if( n > tm.tv_nsec){
          tm.tv_sec += 1;
        }
        pthread_mutex_lock(&sync_mutex);
        int rtn = pthread_cond_timedwait(&sync_cnd,&sync_mutex,ctime(&tm));
        if(rtn == 0){
          for (i = 0; i < samplecnt; i += step) {
            MM68KExec(step);
            m68kcycle += base_clock;
          }
          frame += samplecnt;
          if (use_new_scsp) {
            new_scsp_exec((samplecnt << 1));
          }
          else {
            scsp_update_timer(1);
          }
        }
        pthread_mutex_unlock(&sync_mutex);
#else
        if (sleeptime > 10000) YabThreadUSleep(0);
        if (sh2_read_req != 0) {
          for (i = 0; i < samplecnt; i += step) {
            MM68KExec(step);
            m68kcycle += base_clock;
          }
          frame += samplecnt;
          if (use_new_scsp) {
            new_scsp_exec((samplecnt << 1));
          }
          else {
            scsp_update_timer(1);
          }
          sh2_read_req = 0;
        }
#endif
      } while (sleeptime > 0);

      checktime = YabauseGetTicks();
      //printf("vsynctime = %d(%d) %d(%d)\n", (s32)(checktime - before),16666666/frame_div,(s32)(checktime - initnow),(s32)initsleeptime);
      //printf("vsynctime = %d(%d) %"PRIu64"-%"PRIu64"=(%"PRId64")\n", (s32)(checktime - before),16666666/frame_div,now,before,initsleeptime);
      before = checktime;
    }
  }
  YabThreadWake(YAB_THREAD_SCSP);
  return NULL;
}


void ScspExec(){
  if (thread_running == 0){
    m68k_done_upto.store(0);
    m68k_at_frame_boundary.store(0);
    thread_running = 1;
    if (g_scsp_main_mode == 0) {
      YabThreadStart(YAB_THREAD_SCSP, "scsp sync", ScspAsynMainCpuTime, NULL);
    }
    else {
      YabThreadStart(YAB_THREAD_SCSP, "scsp async", ScspAsynMainRealtime, NULL);
    }
    YabThreadUSleep(100000);
  }
}
void ScspExecAsync() {
  u32 audiosize;

#endif


  if (ScspInternalVars->scsptiming1 >= scsplines)
  {
     s32 *bufL, *bufR;

     ScspInternalVars->scsptiming1 -= scsplines;
     ScspInternalVars->scsptiming2 = 0;

     // Update sound buffers
     if (scspsoundgenpos + scspsoundlen > scspsoundbufsize)
        scspsoundgenpos = 0;

     if (scspsoundoutleft + scspsoundlen > scspsoundbufsize)
     {
        u32 overrun = (scspsoundoutleft + scspsoundlen) -
           scspsoundbufsize;
        SCSPLOG("WARNING: Sound buffer overrun, %lu samples\n",
           (long)overrun);
        scspsoundoutleft -= overrun;
     }

     bufL = (s32 *)&scspchannel[0].data32[scspsoundgenpos];
     bufR = (s32 *)&scspchannel[1].data32[scspsoundgenpos];
     memset(bufL, 0, sizeof(u32) * scspsoundlen);
     memset(bufR, 0, sizeof(u32) * scspsoundlen);
     if (use_new_scsp)
        new_scsp_update_samples(bufL, bufR, scspsoundlen);
     else
        scsp_update(bufL, bufR, scspsoundlen);
     scspsoundgenpos += scspsoundlen;
     scspsoundoutleft += scspsoundlen;
  }

  while (scspsoundoutleft > 0 &&
     (audiosize = SNDCore->GetAudioSpace()) > 0)
  {
     s32 outstart = (s32)scspsoundgenpos - (s32)scspsoundoutleft;

     if (outstart < 0)
        outstart += scspsoundbufsize;
     if (audiosize > scspsoundoutleft)
        audiosize = scspsoundoutleft;
     if (audiosize > scspsoundbufsize - outstart)
        audiosize = scspsoundbufsize - outstart;

     SNDCore->UpdateAudio(&scspchannel[0].data32[outstart],
        &scspchannel[1].data32[outstart], audiosize);
     scspsoundoutleft -= audiosize;

#if 0
     ScspConvert32uto16s(&scspchannel[0].data32[outstart],
        &scspchannel[1].data32[outstart],
        (s16 *)stereodata16, audiosize);
     DRV_AviSoundUpdate(stereodata16, audiosize);
#endif
  }

  if (!use_new_scsp)
     scsp_update_monitor();

#ifdef USE_SCSPMIDI
  // Process Midi ports
  while (scsp.midincnt < 4)
  {
     u8 data;
     int isdata;

     data = SNDCore->MidiIn(&isdata);
     if (!isdata)
        break;
     scsp_midi_in_send(data);
  }


  while (scsp.midoutcnt)
  {
     SNDCore->MidiOut(scsp_midi_out_read());
  }
#endif

//#if defined(ASYNC_SCSP)
//  while (scsp_mute_flags){ YabThreadUSleep(16666); }
//#endif
}

//////////////////////////////////////////////////////////////////////////////

void
M68KWriteNotify (u32 address, u32 size)
{
  M68K->WriteNotify (address, size);
}

//////////////////////////////////////////////////////////////////////////////

void
M68KGetRegisters (m68kregs_struct *regs)
{
  int i;

  if (regs != NULL)
    {
      for (i = 0; i < 8; i++)
        {
          regs->D[i] = M68K->GetDReg (i);
          regs->A[i] = M68K->GetAReg (i);
        }

      regs->SR = M68K->GetSR ();
      regs->PC = M68K->GetPC ();
    }
}

//////////////////////////////////////////////////////////////////////////////

void
M68KSetRegisters (m68kregs_struct *regs)
{
  int i;

  if (regs != NULL)
    {
      for (i = 0; i < 8; i++)
        {
          M68K->SetDReg (i, regs->D[i]);
          M68K->SetAReg (i, regs->A[i]);
        }

      M68K->SetSR (regs->SR);
      M68K->SetPC (regs->PC);
    }
}

//////////////////////////////////////////////////////////////////////////////

void
ScspMuteAudio (int flags)
{
  scsp_mute_flags |= flags;
  if (SNDCore && scsp_mute_flags)
    SNDCore->MuteAudio ();

  g_scsp_lock = 1;
}

//////////////////////////////////////////////////////////////////////////////

void
ScspUnMuteAudio (int flags)
{
  scsp_mute_flags &= ~flags;
  if (SNDCore && (scsp_mute_flags == 0))
    SNDCore->UnMuteAudio ();

  g_scsp_lock = 0;
}

//////////////////////////////////////////////////////////////////////////////

void
ScspSetVolume (int volume)
{
  scsp_volume = volume;
  if (SNDCore)
    SNDCore->SetVolume (volume);
}

//////////////////////////////////////////////////////////////////////////////

void
M68KSetBreakpointCallBack (void (*func)(u32))
{
  ScspInternalVars->BreakpointCallBack = func;
}

//////////////////////////////////////////////////////////////////////////////

int
M68KAddCodeBreakpoint (u32 addr)
{
  int i;

  if (ScspInternalVars->numcodebreakpoints < MAX_BREAKPOINTS)
    {
      // Make sure it isn't already on the list
      for (i = 0; i < ScspInternalVars->numcodebreakpoints; i++)
        {
          if (addr == ScspInternalVars->codebreakpoint[i].addr)
            return -1;
        }

      ScspInternalVars->codebreakpoint[i].addr = addr;
      ScspInternalVars->numcodebreakpoints++;
      m68kexecptr = M68KExecBP;

      return 0;
    }

  return -1;
}

//////////////////////////////////////////////////////////////////////////////

void
M68KSortCodeBreakpoints (void)
{
  int i, i2;
  u32 tmp;

  for (i = 0; i < (MAX_BREAKPOINTS - 1); i++)
    {
      for (i2 = i+1; i2 < MAX_BREAKPOINTS; i2++)
        {
          if (ScspInternalVars->codebreakpoint[i].addr == 0xFFFFFFFF &&
              ScspInternalVars->codebreakpoint[i2].addr != 0xFFFFFFFF)
            {
              tmp = ScspInternalVars->codebreakpoint[i].addr;
              ScspInternalVars->codebreakpoint[i].addr =
                ScspInternalVars->codebreakpoint[i2].addr;
              ScspInternalVars->codebreakpoint[i2].addr = tmp;
            }
        }
    }
}

//////////////////////////////////////////////////////////////////////////////

int
M68KDelCodeBreakpoint (u32 addr)
{
  int i;
  if (ScspInternalVars->numcodebreakpoints > 0)
    {
      for (i = 0; i < ScspInternalVars->numcodebreakpoints; i++)
        {
          if (ScspInternalVars->codebreakpoint[i].addr == addr)
            {
              ScspInternalVars->codebreakpoint[i].addr = 0xFFFFFFFF;
              M68KSortCodeBreakpoints ();
              ScspInternalVars->numcodebreakpoints--;
              if (ScspInternalVars->numcodebreakpoints == 0)
                m68kexecptr = M68K->Exec;
              return 0;
            }
        }
    }

  return -1;
}

//////////////////////////////////////////////////////////////////////////////

m68kcodebreakpoint_struct *
M68KGetBreakpointList ()
{
  return ScspInternalVars->codebreakpoint;
}

//////////////////////////////////////////////////////////////////////////////

void
M68KClearCodeBreakpoints ()
{
  int i;
  for (i = 0; i < MAX_BREAKPOINTS; i++)
    ScspInternalVars->codebreakpoint[i].addr = 0xFFFFFFFF;

  ScspInternalVars->numcodebreakpoints = 0;
}

//////////////////////////////////////////////////////////////////////////////

int
SoundSaveState (FILE *fp)
{
  int i;
  u32 temp;
  int offset;
  u8 nextphase;
  IOCheck_struct check = { 0, 0 };

  offset = StateWriteHeader (fp, "SCSP", 6);

  // Flush the staged SH2 sound RAM burst FIFO first. Those writes have already
  // completed from the SH2's point of view, so the SoundRam image written below
  // must contain them (this is one of the FIFO's regular flush points, like a
  // sound RAM read or the per-frame handshake).
  Sh2SramFifoCommit();

  // Save 68k registers first
  ywrite (&check, (void *)&IsM68KRunning, 1, 1, fp);
  ywrite(&check, (void *)&savedcycles, sizeof(u32), 1, fp );

  // Version 5: Musashi now properly saves M68K state (124 bytes)
  M68K->SaveState(fp);

  ywrite(&check, (void *)&use_new_scsp, 1, sizeof(int), fp);
  ywrite(&check, (void *)&new_scsp_cycles, 1, sizeof(u32), fp);
  ywrite (&check, (void *)new_scsp.sound_stack, 64, sizeof(u16), fp);
  for (i = 0; i < 32; i++) {
    ywrite (&check, (void *)&new_scsp.slots[i].regs.kx, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.kb, sizeof(u8),  1,fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.sbctl, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.ssctl, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.lpctl, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.pcm8b, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.sa, sizeof(u32), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.lsa, sizeof(u16), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.lea, sizeof(u16), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.d2r, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.d1r, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.hold, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.ar, sizeof(u8),  1,fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.unknown1, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.ls, sizeof(u8),  1,fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.krs, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.dl, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.rr, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.unknown2, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.si, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.sd, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.tl, sizeof(u16), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.mdl, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.mdxsl, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.mdysl, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.unknown3, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.oct, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.unknown4, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.fns, sizeof(u16), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.re, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.lfof, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.plfows, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.plfos, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.alfows, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.alfos, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.unknown5, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.isel,  sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.imxl, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.disdl, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.dipan, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.efsdl, sizeof(u8), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].regs.efpan, sizeof(u8), 1, fp);

    ywrite (&check, (void *)&new_scsp.slots[i].state.wave, sizeof(u16), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].state.backwards, sizeof(u32), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].state.envelope, sizeof(int), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].state.output, sizeof(s16), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].state.attenuation, sizeof(u16), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].state.step_count, sizeof(int), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].state.sample_counter, sizeof(u32), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].state.envelope_steps_taken, sizeof(u32), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].state.waveform_phase_value, sizeof(s32), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].state.sample_offset, sizeof(s32), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].state.address_pointer, sizeof(u32), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].state.lfo_counter, sizeof(u32), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].state.lfo_pos, sizeof(u32), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].state.num, sizeof(u32), 1, fp);
    ywrite (&check, (void *)&new_scsp.slots[i].state.is_muted, sizeof(u32), 1, fp);

  }


  // Now for the SCSP registers
  ywrite (&check, (void *)scsp_reg, 0x1000, 1, fp);

  // Sound RAM is important
  ywrite (&check, (void *)SoundRam, 0x80000, 1, fp);

  // Write slot internal variables
  for (i = 0; i < 32; i++)
    {
      s32 einc;

      ywrite(&check, (void *)&scsp.slot[i].swe, sizeof(u8), 1, fp);
      ywrite(&check, (void *)&scsp.slot[i].sdir, sizeof(u8), 1, fp);
      ywrite(&check, (void *)&scsp.slot[i].pcm8b, sizeof(u8), 1, fp);
      ywrite(&check, (void *)&scsp.slot[i].sbctl, sizeof(u8), 1, fp);
      ywrite(&check, (void *)&scsp.slot[i].ssctl, sizeof(u8), 1, fp);
      ywrite(&check, (void *)&scsp.slot[i].lpctl, sizeof(u8), 1, fp);
      ywrite (&check, (void *)&scsp.slot[i].key, 1, 1, fp);
      ywrite(&check, (void *)&scsp.slot[i].keyx, sizeof(u8), 1, fp);

      //buf8,16 get regenerated on state load

      ywrite (&check, (void *)&scsp.slot[i].fcnt, 4, 1, fp);
      ywrite(&check, (void *)&scsp.slot[i].finc, sizeof(u32), 1, fp);
      ywrite(&check, (void *)&scsp.slot[i].finct, sizeof(u32), 1, fp);
      ywrite (&check, (void *)&scsp.slot[i].ecnt, 4, 1, fp);

      if (scsp.slot[i].einc == &scsp.slot[i].einca)
        einc = 0;
      else if (scsp.slot[i].einc == &scsp.slot[i].eincd)
        einc = 1;
      else if (scsp.slot[i].einc == &scsp.slot[i].eincs)
        einc = 2;
      else if (scsp.slot[i].einc == &scsp.slot[i].eincr)
        einc = 3;
      else
        einc = 4;

      ywrite (&check, (void *)&einc, 4, 1, fp);

      //einca,eincd,eincs,eincr

      ywrite (&check, (void *)&scsp.slot[i].ecmp, 4, 1, fp);
      ywrite (&check, (void *)&scsp.slot[i].ecurp, 4, 1, fp);
      ywrite(&check, (void *)&scsp.slot[i].env, sizeof(s32), 1, fp);

      if (scsp.slot[i].enxt == scsp_env_null_next)
        nextphase = 0;
      else if (scsp.slot[i].enxt == scsp_release_next)
        nextphase = 1;
      else if (scsp.slot[i].enxt == scsp_sustain_next)
        nextphase = 2;
      else if (scsp.slot[i].enxt == scsp_decay_next)
        nextphase = 3;
      else if (scsp.slot[i].enxt == scsp_attack_next)
        nextphase = 4;
      ywrite (&check, (void *)&nextphase, 1, 1, fp);

      ywrite (&check, (void *)&scsp.slot[i].lfocnt, 4, 1, fp);
      ywrite (&check, (void *)&scsp.slot[i].lfoinc, 4, 1, fp);
      ywrite(&check, (void *)&scsp.slot[i].sa, sizeof(u32), 1, fp);
      ywrite(&check, (void *)&scsp.slot[i].lsa, sizeof(u32), 1, fp);
      ywrite(&check, (void *)&scsp.slot[i].lea , sizeof(u32), 1, fp);

      ywrite(&check, (void *)&scsp.slot[i].tl, sizeof(s32), 1, fp);
      ywrite(&check, (void *)&scsp.slot[i].sl, sizeof(s32), 1, fp);

      ywrite(&check, (void *)&scsp.slot[i].ar, sizeof(s32), 1, fp);
      ywrite(&check, (void *)&scsp.slot[i].dr, sizeof(s32), 1, fp);
      ywrite(&check, (void *)&scsp.slot[i].sr, sizeof(s32), 1, fp);
      ywrite(&check, (void *)&scsp.slot[i].rr, sizeof(s32), 1, fp);

      //arp
      //drp
      //srp
      //rrp

      ywrite(&check, (void *)&scsp.slot[i].krs, sizeof(u32), 1, fp);

      //lfofmw
      //lfoemw

      ywrite(&check, (void *)&scsp.slot[i].lfofms, sizeof(u8), 1, fp);
      ywrite(&check, (void *)&scsp.slot[i].lfoems, sizeof(u8), 1, fp);
      ywrite(&check, (void *)&scsp.slot[i].fsft, sizeof(u8), 1, fp);

      ywrite(&check, (void *)&scsp.slot[i].mdl, sizeof(u8), 1, fp);
      ywrite(&check, (void *)&scsp.slot[i].mdx, sizeof(u8), 1, fp);
      ywrite(&check, (void *)&scsp.slot[i].mdy, sizeof(u8), 1, fp);

      ywrite(&check, (void *)&scsp.slot[i].imxl, sizeof(u8), 1, fp);
      ywrite(&check, (void *)&scsp.slot[i].disll, sizeof(u8), 1, fp);
      ywrite(&check, (void *)&scsp.slot[i].dislr, sizeof(u8), 1, fp);
      ywrite(&check, (void *)&scsp.slot[i].efsll, sizeof(u8), 1, fp);
      ywrite(&check, (void *)&scsp.slot[i].efslr, sizeof(u8), 1, fp);

      ywrite(&check, (void *)&scsp.slot[i].eghold, sizeof(u8), 1, fp);
      ywrite(&check, (void *)&scsp.slot[i].lslnk, sizeof(u8), 1, fp);
    }

  // Write main internal variables
  ywrite (&check, (void *)&scsp.mem4b, 4, 1, fp);
  ywrite (&check, (void *)&scsp.dac18b, 4, 1, fp);
  ywrite (&check, (void *)&scsp.mvol, 4, 1, fp);

  ywrite (&check, (void *)&scsp.rbl, 4, 1, fp);
  ywrite (&check, (void *)&scsp.rbp, 4, 1, fp);

  ywrite (&check, (void *)&scsp.mslc, 4, 1, fp);

  ywrite (&check, (void *)&scsp.dmea, 4, 1, fp);
  ywrite (&check, (void *)&scsp.drga, 4, 1, fp);
  ywrite (&check, (void *)&scsp.dmfl, 4, 1, fp);
  ywrite (&check, (void *)&scsp.dmlen, 4, 1, fp);

  ywrite (&check, (void *)scsp.midinbuf, 1, 4, fp);
  ywrite (&check, (void *)scsp.midoutbuf, 1, 4, fp);
  ywrite (&check, (void *)&scsp.midincnt, 1, 1, fp);
  ywrite (&check, (void *)&scsp.midoutcnt, 1, 1, fp);
  ywrite (&check, (void *)&scsp.midflag, 1, 1, fp);

  ywrite (&check, (void *)&scsp.timacnt, 4, 1, fp);
  ywrite (&check, (void *)&scsp.timasd, 4, 1, fp);
  ywrite (&check, (void *)&scsp.timbcnt, 4, 1, fp);
  ywrite (&check, (void *)&scsp.timbsd, 4, 1, fp);
  ywrite (&check, (void *)&scsp.timccnt, 4, 1, fp);
  ywrite (&check, (void *)&scsp.timcsd, 4, 1, fp);

  ywrite (&check, (void *)&scsp.scieb, 4, 1, fp);
  ywrite (&check, (void *)&scsp.scipd, 4, 1, fp);
  ywrite (&check, (void *)&scsp.scilv0, 4, 1, fp);
  ywrite (&check, (void *)&scsp.scilv1, 4, 1, fp);
  ywrite (&check, (void *)&scsp.scilv2, 4, 1, fp);
  ywrite (&check, (void *)&scsp.mcieb, 4, 1, fp);
  auto mcipd = scsp.mcipd.load();
  ywrite (&check, (void *)&mcipd, 4, 1, fp);

  ywrite (&check, (void *)scsp.stack, 4, 32 * 2, fp);


  ywrite(&check, (void *)scsp_dsp.coef, sizeof(u16), 64, fp);
  ywrite(&check, (void *)scsp_dsp.madrs, sizeof(u16), 32, fp);
  ywrite(&check, (void *)scsp_dsp.mpro, sizeof(u64), 128, fp);
  ywrite(&check, (void *)scsp_dsp.temp, sizeof(s32), 128, fp);
  ywrite(&check, (void *)scsp_dsp.mems, sizeof(s32), 32, fp);
  ywrite(&check, (void *)scsp_dsp.mixs, sizeof(s32), 16, fp);
  ywrite(&check, (void *)scsp_dsp.efreg, sizeof(s16), 16, fp);
  ywrite(&check, (void *)scsp_dsp.exts, sizeof(s16), 2, fp);
  ywrite(&check, (void *)&scsp_dsp.mdec_ct, sizeof(u32), 1, fp);
  ywrite(&check, (void *)&scsp_dsp.inputs, sizeof(s32), 1, fp);
  ywrite(&check, (void *)&scsp_dsp.b, sizeof(s32), 1, fp);
  ywrite(&check, (void *)&scsp_dsp.x, sizeof(s32), 1, fp);
  ywrite(&check, (void *)&scsp_dsp.y, sizeof(s16), 1, fp);
  ywrite(&check, (void *)&scsp_dsp.acc, sizeof(s32), 1, fp);
  ywrite(&check, (void *)&scsp_dsp.shifted, sizeof(s32), 1, fp);
  ywrite(&check, (void *)&scsp_dsp.y_reg, sizeof(s32), 1, fp);
  ywrite(&check, (void *)&scsp_dsp.frc_reg, sizeof(u16), 1, fp);
  ywrite(&check, (void *)&scsp_dsp.adrs_reg, sizeof(u16), 1, fp);
  ywrite(&check, (void *)&scsp_dsp.mul_out, sizeof(s32), 1, fp);
  ywrite(&check, (void *)&scsp_dsp.mrd_value, sizeof(u32), 1, fp);
  ywrite(&check, (void *)&scsp_dsp.rbl, sizeof(int), 1, fp);
  ywrite(&check, (void *)&scsp_dsp.rbp, sizeof(int), 1, fp);
  ywrite(&check, (void *)&scsp_dsp.need_read, sizeof(int), 1, fp);
  ywrite(&check, (void *)&scsp_dsp.io_addr, sizeof(u32), 1, fp);
  ywrite(&check, (void *)&scsp_dsp.need_write, sizeof(int), 1, fp);
  ywrite(&check, (void *)&scsp_dsp.write_data, sizeof(u16), 1, fp);
  ywrite(&check, (void *)&scsp_dsp.updated, sizeof(int), 1, fp);
  ywrite(&check, (void *)&scsp_dsp.last_step, sizeof(int), 1, fp);

  ywrite(&check, (void *)&ScspInternalVars->scsptiming1, sizeof(u32), 1, fp);
  ywrite(&check, (void *)&ScspInternalVars->scsptiming2, sizeof(u32), 1, fp);

  {
    // Same two fields as before the ring became lock-free: the write offset
    // and how many bytes are still queued, so the save format is unchanged.
    u32 saved_in = cdda_in_pos;
    u32 saved_left = cdda_written.load(std::memory_order_relaxed) -
                     cdda_read.load(std::memory_order_relaxed);
    ywrite(&check, (void *)&saved_in, sizeof(u32), 1, fp);
    ywrite(&check, (void *)&saved_left, sizeof(u32), 1, fp);
  }
  ywrite(&check, (void *)&scsp_mute_flags, sizeof(u32), 1, fp);
  ywrite(&check, (void *)&scspsoundlen, sizeof(u32), 1, fp);
  ywrite(&check, (void *)&scsplines, sizeof(u32), 1, fp);

  // Version 5: Audio buffer position variables
  ywrite(&check, (void *)&scspsoundgenpos, sizeof(u32), 1, fp);
  ywrite(&check, (void *)&scspsoundoutleft, sizeof(u32), 1, fp);
  ywrite(&check, (void *)&new_scsp_outbuf_pos, sizeof(int), 1, fp);

  // Version 6: state the scsp3-derived pipeline added on top of the per-field
  // slot serialization above. Without it a reloaded state runs with a wrong
  // short-wave wrap, a lost loop/key-on link and a desynced FM modulation ring
  // until the game happens to rewrite the slot.
  for (i = 0; i < 32; i++)
    {
      ywrite(&check, (void *)&new_scsp.slots[i].regs.shortwave, sizeof(u8), 1, fp);
      ywrite(&check, (void *)&new_scsp.slots[i].state.sample_frac, sizeof(u32), 1, fp);
      ywrite(&check, (void *)&new_scsp.slots[i].state.lfo_time_counter, sizeof(u32), 1, fp);
      ywrite(&check, (void *)&new_scsp.slots[i].state.in_loop, sizeof(int), 1, fp);
      ywrite(&check, (void *)&new_scsp.slots[i].state.eg_keyon_skip, sizeof(int), 1, fp);
      ywrite(&check, (void *)&new_scsp.slots[i].state.read_phase_frac, sizeof(u32), 1, fp);
      ywrite(&check, (void *)&new_scsp.slots[i].state.wave_index, sizeof(s32), 1, fp);
    }
  ywrite(&check, (void *)new_scsp.sound_stack_delayer, sizeof(s16), 4, fp);
  ywrite(&check, (void *)&new_scsp.global_counter, sizeof(u32), 1, fp);
  ywrite(&check, (void *)&new_scsp.frame, sizeof(u32), 1, fp);
  ywrite(&check, (void *)&new_scsp_lfsr, sizeof(u32), 1, fp);

  g_scsp_lock = 0;

  return StateFinishHeader (fp, offset);
}

//////////////////////////////////////////////////////////////////////////////

int
SoundLoadState (FILE *fp, int version, int size)
{
  int i, i2;
  u32 temp;
  u8 nextphase;
  IOCheck_struct check = { 0, 0 };

  LOG("--------------- SoundLoadState ----------------");

  // the high-quality renderer's per-voice history belongs to the old timeline
  scsp_hq_request_reset();

  // Drain the staged SH2 sound RAM burst FIFO before anything is restored.
  // Those writes belong to the pre-load timeline; left pending they would be
  // committed into the freshly loaded SoundRam and corrupt it.
  Sh2SramFifoCommit();

  // Read 68k registers first
  yread(&check, (void *)&IsM68KRunning, 1, 1, fp);
  yread(&check, (void *)&savedcycles, sizeof(u32), 1, fp);

  // Version 5+: Musashi saves M68K state (124 bytes)
  // Version < 5: Old Musashi didn't save M68K state (0 bytes), skip loading
  if (version >= 5) {
    M68K->LoadState(fp);
  }
  // For version < 5, M68K registers are not in save file - M68K will be reset

  yread(&check, (void *)&use_new_scsp, 1, sizeof(int), fp);
  yread(&check, (void *)&new_scsp_cycles, 1, sizeof(u32), fp);
  yread(&check, (void *)new_scsp.sound_stack, 64, sizeof(u16), fp);
  for (i = 0; i < 32; i++) {
    yread (&check, (void *)&new_scsp.slots[i].regs.kx, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.kb, sizeof(u8),  1,fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.sbctl, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.ssctl, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.lpctl, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.pcm8b, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.sa, sizeof(u32), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.lsa, sizeof(u16), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.lea, sizeof(u16), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.d2r, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.d1r, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.hold, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.ar, sizeof(u8),  1,fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.unknown1, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.ls, sizeof(u8),  1,fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.krs, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.dl, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.rr, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.unknown2, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.si, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.sd, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.tl, sizeof(u16), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.mdl, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.mdxsl, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.mdysl, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.unknown3, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.oct, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.unknown4, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.fns, sizeof(u16), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.re, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.lfof, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.plfows, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.plfos, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.alfows, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.alfos, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.unknown5, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.isel,  sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.imxl, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.disdl, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.dipan, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.efsdl, sizeof(u8), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].regs.efpan, sizeof(u8), 1, fp);

    yread (&check, (void *)&new_scsp.slots[i].state.wave, sizeof(u16), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].state.backwards, sizeof(u32), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].state.envelope, sizeof(int), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].state.output, sizeof(s16), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].state.attenuation, sizeof(u16), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].state.step_count, sizeof(int), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].state.sample_counter, sizeof(u32), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].state.envelope_steps_taken, sizeof(u32), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].state.waveform_phase_value, sizeof(s32), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].state.sample_offset, sizeof(s32), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].state.address_pointer, sizeof(u32), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].state.lfo_counter, sizeof(u32), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].state.lfo_pos, sizeof(u32), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].state.num, sizeof(u32), 1, fp);
    yread (&check, (void *)&new_scsp.slots[i].state.is_muted, sizeof(u32), 1, fp);

  }


  // Now for the SCSP registers
  yread (&check, (void *)scsp_reg, 0x1000, 1, fp);

  // Lastly, sound ram
  yread (&check, (void *)SoundRam, 0x80000, 1, fp);

  if (version > 1)
    {
      // Internal variables need to be regenerated
      for(i = 0; i < 32; i++)
        {
          for (i2 = 0; i2 < 0x20; i2 += 2){
            //scsp_slot_set_w (i, 0x1E - i2, scsp_slot_get_w (i, 0x1E - i2));
            u32 addr = (i << 5) + 0x1E - i2;
            u16 val = *(u16 *)&scsp_isr[addr ^ 2];
            scsp_w_w(addr, val);
          }
        }

      scsp_set_w (0x402, scsp_get_w (0x402));

      // Read slot internal variables
      for (i = 0; i < 32; i++)
        {
          s32 einc;
          yread(&check, (void *)&scsp.slot[i].swe, sizeof(u8), 1, fp);
          yread(&check, (void *)&scsp.slot[i].sdir, sizeof(u8), 1, fp);
          yread(&check, (void *)&scsp.slot[i].pcm8b, sizeof(u8), 1, fp);

          yread(&check, (void *)&scsp.slot[i].sbctl, sizeof(u8), 1, fp);
          yread(&check, (void *)&scsp.slot[i].ssctl, sizeof(u8), 1, fp);
          yread(&check, (void *)&scsp.slot[i].lpctl, sizeof(u8), 1, fp);
          yread (&check, (void *)&scsp.slot[i].key, 1, 1, fp);
          yread(&check, (void *)&scsp.slot[i].keyx, sizeof(u8), 1, fp);

          //buf8,16 regenerated at end

          yread (&check, (void *)&scsp.slot[i].fcnt, 4, 1, fp);
          yread(&check, (void *)&scsp.slot[i].finc, sizeof(u32), 1, fp);
          yread(&check, (void *)&scsp.slot[i].finct, sizeof(u32), 1, fp);
          yread (&check, (void *)&scsp.slot[i].ecnt, 4, 1, fp);

          yread (&check, (void *)&einc, 4, 1, fp);
          switch (einc)
            {
            case 0:
              scsp.slot[i].einc = &scsp.slot[i].einca;
              break;
            case 1:
              scsp.slot[i].einc = &scsp.slot[i].eincd;
              break;
            case 2:
              scsp.slot[i].einc = &scsp.slot[i].eincs;
              break;
            case 3:
              scsp.slot[i].einc = &scsp.slot[i].eincr;
              break;
            default:
              scsp.slot[i].einc = NULL;
              break;
            }

          //einca,eincd,eincs,eincr

          yread (&check, (void *)&scsp.slot[i].ecmp, 4, 1, fp);
          yread (&check, (void *)&scsp.slot[i].ecurp, 4, 1, fp);
          yread(&check, (void *)&scsp.slot[i].env, sizeof(s32), 1, fp);
          yread (&check, (void *)&nextphase, 1, 1, fp);
          switch (nextphase)
            {
            case 0:
              scsp.slot[i].enxt = scsp_env_null_next;
              break;
            case 1:
              scsp.slot[i].enxt = scsp_release_next;
              break;
            case 2:
              scsp.slot[i].enxt = scsp_sustain_next;
              break;
            case 3:
              scsp.slot[i].enxt = scsp_decay_next;
              break;
            case 4:
              scsp.slot[i].enxt = scsp_attack_next;
              break;
            default: break;
            }

          yread (&check, (void *)&scsp.slot[i].lfocnt, 4, 1, fp);
          yread (&check, (void *)&scsp.slot[i].lfoinc, 4, 1, fp);

          yread(&check, (void *)&scsp.slot[i].sa, sizeof(u32), 1, fp);
          yread(&check, (void *)&scsp.slot[i].lsa, sizeof(u32), 1, fp);
          yread(&check, (void *)&scsp.slot[i].lea, sizeof(u32), 1, fp);

          yread(&check, (void *)&scsp.slot[i].tl, sizeof(s32), 1, fp);
          yread(&check, (void *)&scsp.slot[i].sl, sizeof(s32), 1, fp);

          yread(&check, (void *)&scsp.slot[i].ar, sizeof(s32), 1, fp);
          yread(&check, (void *)&scsp.slot[i].dr, sizeof(s32), 1, fp);
          yread(&check, (void *)&scsp.slot[i].sr, sizeof(s32), 1, fp);
          yread(&check, (void *)&scsp.slot[i].rr, sizeof(s32), 1, fp);

          //arp
          //drp
          //srp
          //rrp

          yread(&check, (void *)&scsp.slot[i].krs, sizeof(u32), 1, fp);

          //lfofmw
          //lfoemw

          yread(&check, (void *)&scsp.slot[i].lfofms, sizeof(u8), 1, fp);
          yread(&check, (void *)&scsp.slot[i].lfoems, sizeof(u8), 1, fp);
          yread(&check, (void *)&scsp.slot[i].fsft, sizeof(u8), 1, fp);

          yread(&check, (void *)&scsp.slot[i].mdl, sizeof(u8), 1, fp);
          yread(&check, (void *)&scsp.slot[i].mdx, sizeof(u8), 1, fp);
          yread(&check, (void *)&scsp.slot[i].mdy, sizeof(u8), 1, fp);

          yread(&check, (void *)&scsp.slot[i].imxl, sizeof(u8), 1, fp);
          yread(&check, (void *)&scsp.slot[i].disll, sizeof(u8), 1, fp);
          yread(&check, (void *)&scsp.slot[i].dislr, sizeof(u8), 1, fp);
          yread(&check, (void *)&scsp.slot[i].efsll, sizeof(u8), 1, fp);
          yread(&check, (void *)&scsp.slot[i].efslr, sizeof(u8), 1, fp);

          yread(&check, (void *)&scsp.slot[i].eghold, sizeof(u8), 1, fp);
          yread(&check, (void *)&scsp.slot[i].lslnk, sizeof(u8), 1, fp);

          // depends on pcm8b, sa, lea being loaded first
          // Rebuild the buf8/buf16 variables
          if (scsp.slot[i].pcm8b)
            {
              scsp.slot[i].buf8 = (s8*)&(scsp.scsp_ram[scsp.slot[i].sa]);
              if ((scsp.slot[i].sa + (scsp.slot[i].lea >> SCSP_FREQ_LB)) >
                  SCSP_RAM_MASK)
                scsp.slot[i].lea = (SCSP_RAM_MASK - scsp.slot[i].sa) <<
                                   SCSP_FREQ_LB;
            }
          else
            {
              scsp.slot[i].buf16 = (s16*)&(scsp.scsp_ram[scsp.slot[i].sa & ~1]);
              if ((scsp.slot[i].sa + (scsp.slot[i].lea >> (SCSP_FREQ_LB - 1))) >
                  SCSP_RAM_MASK)
                scsp.slot[i].lea = (SCSP_RAM_MASK - scsp.slot[i].sa) <<
                                   (SCSP_FREQ_LB - 1);
            }
        }

      // Read main internal variables
      yread (&check, (void *)&scsp.mem4b, 4, 1, fp);
      if( version >= 4){
        yread (&check, (void *)&scsp.dac18b, 4, 1, fp);
      }
      yread (&check, (void *)&scsp.mvol, 4, 1, fp);

      yread (&check, (void *)&scsp.rbl, 4, 1, fp);
      yread (&check, (void *)&scsp.rbp, 4, 1, fp);

      yread (&check, (void *)&scsp.mslc, 4, 1, fp);

      yread (&check, (void *)&scsp.dmea, 4, 1, fp);
      yread (&check, (void *)&scsp.drga, 4, 1, fp);
      yread (&check, (void *)&scsp.dmfl, 4, 1, fp);
      yread (&check, (void *)&scsp.dmlen, 4, 1, fp);

      yread (&check, (void *)scsp.midinbuf, 1, 4, fp);
      yread (&check, (void *)scsp.midoutbuf, 1, 4, fp);
      yread (&check, (void *)&scsp.midincnt, 1, 1, fp);
      yread (&check, (void *)&scsp.midoutcnt, 1, 1, fp);
      yread (&check, (void *)&scsp.midflag, 1, 1, fp);

      yread (&check, (void *)&scsp.timacnt, 4, 1, fp);
      yread (&check, (void *)&scsp.timasd, 4, 1, fp);
      yread (&check, (void *)&scsp.timbcnt, 4, 1, fp);
      yread (&check, (void *)&scsp.timbsd, 4, 1, fp);
      yread (&check, (void *)&scsp.timccnt, 4, 1, fp);
      yread (&check, (void *)&scsp.timcsd, 4, 1, fp);

      yread (&check, (void *)&scsp.scieb, 4, 1, fp);
      yread (&check, (void *)&scsp.scipd, 4, 1, fp);
      yread (&check, (void *)&scsp.scilv0, 4, 1, fp);
      yread (&check, (void *)&scsp.scilv1, 4, 1, fp);
      yread (&check, (void *)&scsp.scilv2, 4, 1, fp);
      yread (&check, (void *)&scsp.mcieb, 4, 1, fp);
      u32 mcipd = 0;
      yread (&check, (void *)&mcipd, 4, 1, fp);
      scsp.mcipd.store(mcipd);
      yread (&check, (void *)scsp.stack, 4, 32 * 2, fp);

    }

    yread(&check, (void *)scsp_dsp.coef, sizeof(u16), 64, fp);
    yread(&check, (void *)scsp_dsp.madrs, sizeof(u16), 32, fp);
    yread(&check, (void *)scsp_dsp.mpro, sizeof(u64), 128, fp);
    yread(&check, (void *)scsp_dsp.temp, sizeof(s32), 128, fp);
    yread(&check, (void *)scsp_dsp.mems, sizeof(s32), 32, fp);
    yread(&check, (void *)scsp_dsp.mixs, sizeof(s32), 16, fp);
    yread(&check, (void *)scsp_dsp.efreg, sizeof(s16), 16, fp);
    yread(&check, (void *)scsp_dsp.exts, sizeof(s16), 2, fp);
    yread(&check, (void *)&scsp_dsp.mdec_ct, sizeof(u32), 1, fp);
    yread(&check, (void *)&scsp_dsp.inputs, sizeof(s32), 1, fp);
    yread(&check, (void *)&scsp_dsp.b, sizeof(s32), 1, fp);
    yread(&check, (void *)&scsp_dsp.x, sizeof(s32), 1, fp);
    yread(&check, (void *)&scsp_dsp.y, sizeof(s16), 1, fp);
    yread(&check, (void *)&scsp_dsp.acc, sizeof(s32), 1, fp);
    yread(&check, (void *)&scsp_dsp.shifted, sizeof(s32), 1, fp);
    yread(&check, (void *)&scsp_dsp.y_reg, sizeof(s32), 1, fp);
    yread(&check, (void *)&scsp_dsp.frc_reg, sizeof(u16), 1, fp);
    yread(&check, (void *)&scsp_dsp.adrs_reg, sizeof(u16), 1, fp);
    yread(&check, (void *)&scsp_dsp.mul_out, sizeof(s32), 1, fp);
    yread(&check, (void *)&scsp_dsp.mrd_value, sizeof(u32), 1, fp);
    yread(&check, (void *)&scsp_dsp.rbl, sizeof(int), 1, fp);
    yread(&check, (void *)&scsp_dsp.rbp, sizeof(int), 1, fp);
    yread(&check, (void *)&scsp_dsp.need_read, sizeof(int), 1, fp);
    yread(&check, (void *)&scsp_dsp.io_addr, sizeof(u32), 1, fp);
    yread(&check, (void *)&scsp_dsp.need_write, sizeof(int), 1, fp);
    yread(&check, (void *)&scsp_dsp.write_data, sizeof(u16), 1, fp);
    yread(&check, (void *)&scsp_dsp.updated, sizeof(int), 1, fp);
    yread(&check, (void *)&scsp_dsp.last_step, sizeof(int), 1, fp);

    yread(&check, (void *)&ScspInternalVars->scsptiming1, sizeof(u32), 1, fp);
    yread(&check, (void *)&ScspInternalVars->scsptiming2, sizeof(u32), 1, fp);

    if (version >= 3) {
      {
        u32 saved_in = 0, saved_left = 0;
        yread(&check, (void *)&saved_in, sizeof(u32), 1, fp);
        yread(&check, (void *)&saved_left, sizeof(u32), 1, fp);
        if (saved_in >= sizeof(cddabuf.data)) saved_in = 0;
        if (saved_left > sizeof(cddabuf.data)) saved_left = sizeof(cddabuf.data);
        cdda_in_pos = saved_in;
        cdda_out_pos = (saved_in >= saved_left)
                       ? (saved_in - saved_left)
                       : (u32)(saved_in + sizeof(cddabuf.data) - saved_left);
        cdda_read.store(0, std::memory_order_relaxed);
        cdda_written.store(saved_left, std::memory_order_relaxed);
      }
      yread(&check, (void *)&scsp_mute_flags, sizeof(u32), 1, fp);
      yread(&check, (void *)&scspsoundlen, sizeof(u32), 1, fp);
      yread(&check, (void *)&scsplines, sizeof(u32), 1, fp);
    }

    // Version 5: Audio buffer position variables
    if (version >= 5) {
      yread(&check, (void *)&scspsoundgenpos, sizeof(u32), 1, fp);
      yread(&check, (void *)&scspsoundoutleft, sizeof(u32), 1, fp);
      yread(&check, (void *)&new_scsp_outbuf_pos, sizeof(int), 1, fp);
    } else {
      // Reset to safe defaults for older state files
      scspsoundgenpos = 0;
      scspsoundoutleft = 0;
      new_scsp_outbuf_pos = 0;
    }

    // Version 6: new pipeline slot/global state (see SoundSaveState). Older
    // states carry none of it, so clear it to the post-reset values instead of
    // leaving whatever the pre-load session happened to have.
    if (version >= 6) {
      for (i = 0; i < 32; i++)
        {
          yread(&check, (void *)&new_scsp.slots[i].regs.shortwave, sizeof(u8), 1, fp);
          yread(&check, (void *)&new_scsp.slots[i].state.sample_frac, sizeof(u32), 1, fp);
          yread(&check, (void *)&new_scsp.slots[i].state.lfo_time_counter, sizeof(u32), 1, fp);
          yread(&check, (void *)&new_scsp.slots[i].state.in_loop, sizeof(int), 1, fp);
          yread(&check, (void *)&new_scsp.slots[i].state.eg_keyon_skip, sizeof(int), 1, fp);
          yread(&check, (void *)&new_scsp.slots[i].state.read_phase_frac, sizeof(u32), 1, fp);
          yread(&check, (void *)&new_scsp.slots[i].state.wave_index, sizeof(s32), 1, fp);
        }
      yread(&check, (void *)new_scsp.sound_stack_delayer, sizeof(s16), 4, fp);
      yread(&check, (void *)&new_scsp.global_counter, sizeof(u32), 1, fp);
      yread(&check, (void *)&new_scsp.frame, sizeof(u32), 1, fp);
      yread(&check, (void *)&new_scsp_lfsr, sizeof(u32), 1, fp);
      if (new_scsp_lfsr == 0)
        new_scsp_lfsr = 1;  // a zero LFSR would latch the noise source off
    } else {
      for (i = 0; i < 32; i++)
        {
          new_scsp.slots[i].regs.shortwave = 0;
          new_scsp.slots[i].state.sample_frac = 0;
          new_scsp.slots[i].state.lfo_time_counter = 0;
          new_scsp.slots[i].state.in_loop = 0;
          new_scsp.slots[i].state.eg_keyon_skip = 0;
          new_scsp.slots[i].state.read_phase_frac = 0;
          new_scsp.slots[i].state.wave_index = 0;
        }
      memset(new_scsp.sound_stack_delayer, 0, sizeof(new_scsp.sound_stack_delayer));
      new_scsp.global_counter = 0;
      new_scsp.frame = 0;
      new_scsp_lfsr = 1;
    }

    // ========== Post-load initialization ==========

    // 1. Re-trigger pending interrupts to M68K
    //    (The interrupt pending state is restored, but M68K needs to be notified)
    if (scsp.scipd & scsp.scieb) {
        u32 pending = scsp.scipd & scsp.scieb;
        for (int bit = 0; bit < 11; bit++) {
            u32 id = 1 << bit;
            if (pending & id) {
                scsp_trigger_sound_interrupt(id);
            }
        }
    }

    // 2. Clear audio buffers to prevent noise
    if (scspchannel[0].data32)
        memset(scspchannel[0].data32, 0, scspsoundbufsize * sizeof(u32));
    if (scspchannel[1].data32)
        memset(scspchannel[1].data32, 0, scspsoundbufsize * sizeof(u32));
    memset(new_scsp_outbuf_l, 0, sizeof(new_scsp_outbuf_l));
    memset(new_scsp_outbuf_r, 0, sizeof(new_scsp_outbuf_r));

    // 3. Unmute audio
    if (SNDCore) {
        ScspUnMuteAudio(0xFFFFFFFF);  // Clear all mute flags
    }

  return size;
}

//////////////////////////////////////////////////////////////////////////////

static char *
AddSoundLFO (char *outstring, const char *string, u16 level, u16 waveform)
{
  if (level > 0)
    {
      switch (waveform)
        {
        case 0:
          AddString(outstring, "%s Sawtooth\r\n", string);
          break;
        case 1:
          AddString(outstring, "%s Square\r\n", string);
          break;
        case 2:
          AddString(outstring, "%s Triangle\r\n", string);
          break;
        case 3:
          AddString(outstring, "%s Noise\r\n", string);
          break;
        }
    }

  return outstring;
}

//////////////////////////////////////////////////////////////////////////////

static char *
AddSoundPan (char *outstring, u16 pan)
{
  if (pan == 0x0F)
    {
      AddString(outstring, "Left = -MAX dB, Right = -0 dB\r\n");
    }
  else if (pan == 0x1F)
    {
      AddString(outstring, "Left = -0 dB, Right = -MAX dB\r\n");
    }
  else
    {
      AddString(outstring, "Left = -%d dB, Right = -%d dB\r\n", (pan & 0xF) * 3, (pan >> 4) * 3);
    }

  return outstring;
}

//////////////////////////////////////////////////////////////////////////////

static char *
AddSoundLevel (char *outstring, u16 level)
{
  if (level == 0)
    {
      AddString(outstring, "-MAX dB\r\n");
    }
  else
    {
      AddString(outstring, "-%d dB\r\n", (7-level) *  6);
    }

  return outstring;
}

//////////////////////////////////////////////////////////////////////////////

void
ScspSlotDebugStats (u8 slotnum, char *outstring)
{
  u32 slotoffset = slotnum * 0x20;

  AddString (outstring, "Sound Source = ");
  switch (scsp.slot[slotnum].ssctl)
    {
    case 0:
      AddString (outstring, "External DRAM data\r\n");
      break;
    case 1:
      AddString (outstring, "Internal(Noise)\r\n");
      break;
    case 2:
      AddString (outstring, "Internal(0's)\r\n");
      break;
    default:
      AddString (outstring, "Invalid setting\r\n");
      break;
    }

  AddString (outstring, "Source bit = ");
  switch(scsp.slot[slotnum].sbctl)
    {
    case 0:
      AddString (outstring, "No bit reversal\r\n");
      break;
    case 1:
      AddString (outstring, "Reverse other bits\r\n");
      break;
    case 2:
      AddString (outstring, "Reverse sign bit\r\n");
      break;
    case 3:
      AddString (outstring, "Reverse sign and other bits\r\n");
      break;
    }

  // Loop Control
  AddString (outstring, "Loop Mode = ");
  switch (scsp.slot[slotnum].lpctl)
    {
    case 0:
      AddString (outstring, "Off\r\n");
      break;
    case 1:
      AddString (outstring, "Normal\r\n");
      break;
    case 2:
      AddString (outstring, "Reverse\r\n");
      break;
    case 3:
      AddString (outstring, "Alternating\r\n");
      break;
    }

  // PCM8B
  // NOTE: Need curly braces here, as AddString is a macro.
  if (scsp.slot[slotnum].pcm8b)
    {
      AddString (outstring, "8-bit samples\r\n");
    }
  else
    {
      AddString (outstring, "16-bit samples\r\n");
    }

  AddString (outstring, "Start Address = %05lX\r\n", (unsigned long)scsp.slot[slotnum].sa);
  AddString (outstring, "Loop Start Address = %04lX\r\n", (unsigned long)scsp.slot[slotnum].lsa >> SCSP_FREQ_LB);
  AddString (outstring, "Loop End Address = %04lX\r\n", (unsigned long)scsp.slot[slotnum].lea >> SCSP_FREQ_LB);
  AddString (outstring, "Decay 1 Rate = %ld\r\n", (unsigned long)scsp.slot[slotnum].dr);
  AddString (outstring, "Decay 2 Rate = %ld\r\n", (unsigned long)scsp.slot[slotnum].sr);
  if (scsp.slot[slotnum].eghold)
    AddString (outstring, "EG Hold Enabled\r\n");
  AddString (outstring, "Attack Rate = %ld\r\n", (unsigned long)scsp.slot[slotnum].ar);

  if (scsp.slot[slotnum].lslnk)
    AddString (outstring, "Loop Start Link Enabled\r\n");

  if (scsp.slot[slotnum].krs != 0)
    AddString (outstring, "Key rate scaling = %ld\r\n", (unsigned long)scsp.slot[slotnum].krs);

  AddString (outstring, "Decay Level = %d\r\n", (scsp_r_w(slotoffset + 0xA) >> 5) & 0x1F);
  AddString (outstring, "Release Rate = %ld\r\n", (unsigned long)scsp.slot[slotnum].rr);

  if (scsp.slot[slotnum].swe)
    AddString (outstring, "Stack Write Inhibited\r\n");

  if (scsp.slot[slotnum].sdir)
    AddString (outstring, "Sound Direct Enabled\r\n");

  AddString (outstring, "Total Level = %ld\r\n", (unsigned long)scsp.slot[slotnum].tl);

  AddString (outstring, "Modulation Level = %d\r\n", scsp.slot[slotnum].mdl);
  AddString (outstring, "Modulation Input X = %d\r\n", scsp.slot[slotnum].mdx);
  AddString (outstring, "Modulation Input Y = %d\r\n", scsp.slot[slotnum].mdy);

  AddString (outstring, "Octave = %d\r\n", (scsp_r_w(slotoffset + 0x10) >> 11) & 0xF);
  AddString (outstring, "Frequency Number Switch = %d\r\n", scsp_r_w(slotoffset + 0x10) & 0x3FF);

  AddString (outstring, "LFO Reset = %s\r\n", ((scsp_r_w(slotoffset + 0x12) >> 15) & 0x1) ? "TRUE" : "FALSE");
  AddString (outstring, "LFO Frequency = %d\r\n", (scsp_r_w(slotoffset + 0x12) >> 10) & 0x1F);
  outstring = AddSoundLFO (outstring, "LFO Frequency modulation waveform = ",
                           (scsp_r_w(slotoffset + 0x12) >> 5) & 0x7,
                           (scsp_r_w(slotoffset + 0x12) >> 8) & 0x3);
  AddString (outstring, "LFO Frequency modulation level = %d\r\n", (scsp_r_w(slotoffset + 0x12) >> 5) & 0x7);
  outstring = AddSoundLFO (outstring, "LFO Amplitude modulation waveform = ",
                           scsp_r_w(slotoffset + 0x12) & 0x7,
                           (scsp_r_w(slotoffset + 0x12) >> 3) & 0x3);
  AddString (outstring, "LFO Amplitude modulation level = %d\r\n", scsp_r_w(slotoffset + 0x12) & 0x7);

  AddString (outstring, "Input mix level = ");
  outstring = AddSoundLevel (outstring, scsp_r_w(slotoffset + 0x14) & 0x7);
  AddString (outstring, "Input Select = %d\r\n", (scsp_r_w(slotoffset + 0x14) >> 3) & 0x1F);

  AddString (outstring, "Direct data send level = ");
  outstring = AddSoundLevel (outstring, (scsp_r_w(slotoffset + 0x16) >> 13) & 0x7);
  AddString (outstring, "Direct data panpot = ");
  outstring = AddSoundPan (outstring, (scsp_r_w(slotoffset + 0x16) >> 8) & 0x1F);

  AddString (outstring, "Effect data send level = ");
  outstring = AddSoundLevel (outstring, (scsp_r_w(slotoffset + 0x16) >> 5) & 0x7);
  AddString (outstring, "Effect data panpot = ");
  outstring = AddSoundPan (outstring, scsp_r_w(slotoffset + 0x16) & 0x1F);
}

//////////////////////////////////////////////////////////////////////////////

void
ScspCommonControlRegisterDebugStats (char *outstring)
{
   AddString (outstring, "Memory: %s\r\n", scsp.mem4b ? "4 Mbit" : "2 Mbit");
   AddString (outstring, "Master volume: %ld\r\n", (unsigned long)scsp.mvol);
   AddString (outstring, "Ring buffer length: %ld\r\n", (unsigned long)scsp.rbl);
   AddString (outstring, "Ring buffer address: %08lX\r\n", (unsigned long)scsp.rbp);
   AddString (outstring, "\r\n");

   AddString (outstring, "Slot Status Registers\r\n");
   AddString (outstring, "-----------------\r\n");
   AddString (outstring, "Monitor slot: %ld\r\n", (unsigned long)scsp.mslc);
   AddString (outstring, "Call address: %ld\r\n", (unsigned long)scsp.ca);
   AddString (outstring, "\r\n");

   AddString (outstring, "DMA Registers\r\n");
   AddString (outstring, "-----------------\r\n");
   AddString (outstring, "DMA memory address start: %08lX\r\n", (unsigned long)scsp.dmea);
   AddString (outstring, "DMA register address start: %08lX\r\n", (unsigned long)scsp.drga);
   AddString (outstring, "DMA Flags: %lX\r\n", (unsigned long)scsp.dmlen);
   AddString (outstring, "\r\n");

   AddString (outstring, "Timer Registers\r\n");
   AddString (outstring, "-----------------\r\n");
   AddString (outstring, "Timer A counter: %02lX\r\n", (unsigned long)scsp.timacnt >> 8);
   AddString (outstring, "Timer A increment: Every %d sample(s)\r\n", (int)pow(2, (double)scsp.timasd));
   AddString (outstring, "Timer B counter: %02lX\r\n", (unsigned long)scsp.timbcnt >> 8);
   AddString (outstring, "Timer B increment: Every %d sample(s)\r\n", (int)pow(2, (double)scsp.timbsd));
   AddString (outstring, "Timer C counter: %02lX\r\n", (unsigned long)scsp.timccnt >> 8);
   AddString (outstring, "Timer C increment: Every %d sample(s)\r\n", (int)pow(2, (double)scsp.timcsd));
   AddString (outstring, "\r\n");

   AddString (outstring, "Interrupt Registers\r\n");
   AddString (outstring, "-----------------\r\n");
   AddString (outstring, "Sound cpu interrupt pending: %04lX\r\n", (unsigned long)scsp.scipd);
   AddString (outstring, "Sound cpu interrupt enable: %04lX\r\n", (unsigned long)scsp.scieb);
   AddString (outstring, "Sound cpu interrupt level 0: %04lX\r\n", (unsigned long)scsp.scilv0);
   AddString (outstring, "Sound cpu interrupt level 1: %04lX\r\n", (unsigned long)scsp.scilv1);
   AddString (outstring, "Sound cpu interrupt level 2: %04lX\r\n", (unsigned long)scsp.scilv2);
   AddString (outstring, "Main cpu interrupt pending: %04lX\r\n", (unsigned long)scsp.mcipd);
   AddString (outstring, "Main cpu interrupt enable: %04lX\r\n", (unsigned long)scsp.mcieb);
   AddString (outstring, "\r\n");
}

//////////////////////////////////////////////////////////////////////////////

int
ScspSlotDebugSaveRegisters (u8 slotnum, const char *filename)
{
  FILE *fp;
  int i;
  IOCheck_struct check = { 0, 0 };

  if ((fp = fopen (filename, "wb")) == NULL)
    return -1;

  for (i = (slotnum * 0x20); i < ((slotnum+1) * 0x20); i += 2)
    {
#ifdef WORDS_BIGENDIAN
      ywrite (&check, (void *)&scsp_isr[i ^ 2], 1, 2, fp);
#else
      ywrite (&check, (void *)&scsp_isr[(i + 1) ^ 2], 1, 1, fp);
      ywrite (&check, (void *)&scsp_isr[i ^ 2], 1, 1, fp);
#endif
    }

  fclose (fp);
  return 0;
}

//////////////////////////////////////////////////////////////////////////////

static slot_t debugslot;

u32
ScspSlotDebugAudio (u32 *workbuf, s16 *buf, u32 len)
{
  u32 *bufL, *bufR;

  bufL = workbuf;
  bufR = workbuf+len;
  scsp_bufL = (s32 *)bufL;
  scsp_bufR = (s32 *)bufR;

  if (debugslot.ecnt >= SCSP_ENV_DE)
    {
      // envelope null...
      memset (buf, 0, sizeof(s16) * 2 * len);
      return 0;
    }

  if (debugslot.ssctl)
    {
      memset (buf, 0, sizeof(s16) * 2 * len);
      return 0; // not yet supported!
    }

  scsp_buf_len = len;
  scsp_buf_pos = 0;

  // take effect sound volume if no direct sound volume...
  if ((debugslot.disll == 31) && (debugslot.dislr == 31))
    {
      debugslot.disll = debugslot.efsll;
      debugslot.dislr = debugslot.efslr;
    }

  memset (bufL, 0, sizeof(u32) * len);
  memset (bufR, 0, sizeof(u32) * len);
  scsp_slot_update_p[(debugslot.lfofms == 31)?0:1]
                    [(debugslot.lfoems == 31)?0:1]
                    [(debugslot.pcm8b == 0)?1:0]
                    [(debugslot.disll == 31)?0:1]
                    [(debugslot.dislr == 31)?0:1](&debugslot);
  ScspConvert32uto16s ((s32 *)bufL, (s32 *)bufR, (s16 *)buf, len);

  return len;
}

//////////////////////////////////////////////////////////////////////////////

typedef struct
{
  char id[4];
  u32 size;
} chunk_struct;

typedef struct
{
  chunk_struct riff;
  char rifftype[4];
} waveheader_struct;

typedef struct
{
  chunk_struct chunk;
  u16 compress;
  u16 numchan;
  u32 rate;
  u32 bytespersec;
  u16 blockalign;
  u16 bitspersample;
} fmt_struct;

//////////////////////////////////////////////////////////////////////////////

void 
ScspSlotResetDebug(u8 slotnum)
{
  memcpy (&debugslot, &scsp.slot[slotnum], sizeof(slot_t));

  // Clear out the phase counter, etc.
  debugslot.fcnt = 0;
  debugslot.ecnt = SCSP_ENV_AS;
  debugslot.einc = &debugslot.einca;
  debugslot.ecmp = SCSP_ENV_AE;
  debugslot.ecurp = SCSP_ENV_ATTACK;
  debugslot.enxt = scsp_attack_next;
}

//////////////////////////////////////////////////////////////////////////////

int
ScspSlotDebugAudioSaveWav (u8 slotnum, const char *filename)
{
  u32 workbuf[512*2*2];
  s16 buf[512*2];
  FILE *fp;
  u32 counter = 0;
  waveheader_struct waveheader;
  fmt_struct fmt;
  chunk_struct data;
  long length;
  IOCheck_struct check = { 0, 0 };

  if (scsp.slot[slotnum].lea == 0)
    return 0;

  if ((fp = fopen (filename, "wb")) == NULL)
    return -1;

  // Do wave header
  memcpy (waveheader.riff.id, "RIFF", 4);
  waveheader.riff.size = 0; // we'll fix this after the file is closed
  memcpy (waveheader.rifftype, "WAVE", 4);
  ywrite (&check, (void *)&waveheader, 1, sizeof(waveheader_struct), fp);

  // fmt chunk
  memcpy (fmt.chunk.id, "fmt ", 4);
  fmt.chunk.size = 16; // we'll fix this at the end
  fmt.compress = 1; // PCM
  fmt.numchan = 2; // Stereo
  fmt.rate = 44100;
  fmt.bitspersample = 16;
  fmt.blockalign = fmt.bitspersample / 8 * fmt.numchan;
  fmt.bytespersec = fmt.rate * fmt.blockalign;
  ywrite (&check, (void *)&fmt, 1, sizeof(fmt_struct), fp);

  // data chunk
  memcpy (data.id, "data", 4);
  data.size = 0; // we'll fix this at the end
  ywrite (&check, (void *)&data, 1, sizeof(chunk_struct), fp);

  ScspSlotResetDebug(slotnum);

  // Mix the audio, and then write it to the file
  for (;;)
    {
      if (ScspSlotDebugAudio (workbuf, buf, 512) == 0)
        break;

      counter += 512;
      ywrite (&check, (void *)buf, 2, 512 * 2, fp);
      if (debugslot.lpctl != 0 && counter >= (44100 * 2 * 5))
        break;
    }

  length = ftell (fp);

  // Let's fix the riff chunk size and the data chunk size
  fseek (fp, sizeof(waveheader_struct)-0x8, SEEK_SET);
  length -= 0x4;
  ywrite (&check, (void *)&length, 1, 4, fp);

  fseek (fp, sizeof(waveheader_struct) + sizeof(fmt_struct) + 0x4, SEEK_SET);
  length -= sizeof(waveheader_struct) + sizeof(fmt_struct);
  ywrite (&check, (void *)&length, 1, 4, fp);
  fclose (fp);

  return 0;
}

//////////////////////////////////////////////////////////////////////////////
