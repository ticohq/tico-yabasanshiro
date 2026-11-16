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

/* The sound RAM pages the CPUs wrote, for the float DSP's shadow memory
 * (dsp_float.h). Any thread marks a write; the SCSP thread takes the marked
 * pages and has their words read from RAM again.
 *
 * 1 KB pages (512 words), one bit each, in 8 64-bit words. A page is
 * cleared before its words are dropped, so a write that marks it again
 * while it is being taken is seen the next time. Header-only, C++11. */
#ifndef YAB_SOUND_HQ_RAM_WATCH_H
#define YAB_SOUND_HQ_RAM_WATCH_H

#include <stdint.h>
#include <atomic>

class ScspHqRamWatch {
public:
  enum { PAGE_SHIFT = 10, PAGES = 0x80000 >> PAGE_SHIFT, PAGE_WORDS = 1 << (PAGE_SHIFT - 1) };

  ScspHqRamWatch() {
    for (int i = 0; i < PAGES / 64; i++) bits_[i].store(0, std::memory_order_relaxed);
  }

  /* `size` bytes written at byte address `addr` (wraps at 512 KB) */
  void mark(uint32_t addr, uint32_t size) {
    const uint32_t first = (addr & 0x7FFFF) >> PAGE_SHIFT;
    const uint32_t last = ((addr + size - 1) & 0x7FFFF) >> PAGE_SHIFT;
    set(first);
    if (last != first) set(last);
  }

  /* calls f(first_word, PAGE_WORDS) for each marked page and unmarks it */
  template <class F>
  void take(F f) {
    for (int i = 0; i < PAGES / 64; i++) {
      if (bits_[i].load(std::memory_order_relaxed) == 0) continue;
      uint64_t b = bits_[i].exchange(0, std::memory_order_acquire);
      for (int k = 0; b != 0; k++, b >>= 1)
        if (b & 1) f((uint32_t)(i * 64 + k) * PAGE_WORDS, (uint32_t)PAGE_WORDS);
    }
  }

private:
  void set(uint32_t page) {
    std::atomic<uint64_t>& w = bits_[page >> 6];
    const uint64_t bit = (uint64_t)1 << (page & 63);
    /* always a read-modify-write: a plain "already set" test could let the
     * RAM store of this write become visible after the page was taken */
    w.fetch_or(bit, std::memory_order_release);
  }

  std::atomic<uint64_t> bits_[PAGES / 64];
};

#endif
