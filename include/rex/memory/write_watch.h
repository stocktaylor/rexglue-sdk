/**
 * @file        memory/write_watch.h
 * @brief       Software write watches on guest physical memory
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>

#include <rex/platform.h>

// GPU caches (shared memory, primitive processor) watch the physical pages
// they copy and drop the copy on the first CPU write. Natively the watch is a
// read-only page whose write fault calls PhysicalHeap::TriggerCallbacks. Where
// pages can't be protected (WebAssembly), writes check this bitmap instead,
// one bit per 4 KB guest page, kept equal to the physical heaps' watch flags,
// and a hit makes the same TriggerCallbacks call the fault would have.
//
// REX_SOFTWARE_WRITE_WATCH is on for WebAssembly. Natively it is an audit mode
// (CMake option REXGLUE_SOFTWARE_WRITE_WATCH): software checks run with page
// protection still on, so a write that still faults on a watched page is one
// the checks miss, and is logged.
#if !defined(REX_SOFTWARE_WRITE_WATCH)
#if REX_PLATFORM_WASM
#define REX_SOFTWARE_WRITE_WATCH 1
#else
#define REX_SOFTWARE_WRITE_WATCH 0
#endif
#endif

namespace rex::memory {

constexpr uint32_t kWriteWatchPageShift = 12;
constexpr uint32_t kWriteWatchWords = uint32_t((uint64_t(1) << 32) >> (kWriteWatchPageShift + 6));

// Lowest guest address a watch can be on: the physical views at 0xA0000000,
// 0xC0000000 and 0xE0000000.
constexpr uint32_t kWriteWatchFirstAddress = 0xA0000000u;

#if REX_PLATFORM_WASM
// Statically linked: generated code reads the bitmap directly.
inline uint64_t g_write_watch_bits[kWriteWatchWords] = {};
inline const uint64_t* WriteWatchBits() {
  return g_write_watch_bits;
}
#else
const uint64_t* WriteWatchBits();
#endif

// Runs the invalidation callbacks for the watched pages in [address,
// address + length), as a write fault on each would.
void OnWatchedWrite(uint32_t address, uint32_t length);

inline bool IsWriteWatched(const uint64_t* bits, uint32_t address) {
  uint32_t page = address >> kWriteWatchPageShift;
  return ((bits[page >> 6] >> (page & 63)) & 1) != 0;
}

// Call after a guest write of length bytes (at most one page) at address.
inline void CheckWriteWatch(uint32_t address, uint32_t length) {
  if (address < kWriteWatchFirstAddress) [[likely]] {
    return;
  }
  const uint64_t* bits = WriteWatchBits();
  if (IsWriteWatched(bits, address) || IsWriteWatched(bits, address + length - 1)) [[unlikely]] {
    OnWatchedWrite(address, length);
  }
}

}  // namespace rex::memory
