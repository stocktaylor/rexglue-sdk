#pragma once
/**
 * @file        memory/guest_layout.h
 * @brief       Guest address aliasing without host page mapping (WebAssembly)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <cstdint>

namespace rex::memory {

// Native builds map several guest ranges onto the same memory with file views
// (see map_info in xmemory.cpp). WebAssembly linear memory can't be mapped
// twice, so there the guest space is one host block laid out as
//
//   [0x000000000, 0x100000000)  guest virtual addresses, 1:1
//   [0x100000000, 0x120001000)  guest physical memory (+4 KB for 0xE0000000)
//
// and the aliased ranges are redirected in software: a guest address lives at
// block + address + GuestAliasHostOffset(address). Every boundary is 16 MB
// aligned, so the offset depends on the top address byte alone.
//
//   0x7F000000-0x7FFFFFFF  GPU writeback         -> physical + 0
//   0xA0000000-0xBFFFFFFF  physical, 64 KB pages -> physical + 0
//   0xC0000000-0xDFFFFFFF  physical, 16 MB pages -> physical + 0
//   0xE0000000-0xFFFFFFFF  physical, 4 KB pages  -> physical + 0x1000
//
// 0x90000000 is not aliased onto 0x80000000 as it is natively: both keep their
// own storage, which only differs if a title reaches the same XEX memory
// through both ranges.
inline constexpr uint64_t kGuestPhysicalHostBase = 0x100000000ull;
inline constexpr uint64_t kGuestHostBlockSize = 0x120000000ull + 0x1000;

inline constexpr std::array<uint32_t, 256> kGuestAliasHostOffsets = [] {
  std::array<uint32_t, 256> offsets{};
  offsets[0x7F] = uint32_t(kGuestPhysicalHostBase - 0x7F000000);
  for (uint32_t i = 0xA0; i < 0xC0; ++i) {
    offsets[i] = uint32_t(kGuestPhysicalHostBase - 0xA0000000);
  }
  for (uint32_t i = 0xC0; i < 0xE0; ++i) {
    offsets[i] = uint32_t(kGuestPhysicalHostBase - 0xC0000000);
  }
  for (uint32_t i = 0xE0; i < 0x100; ++i) {
    offsets[i] = uint32_t(kGuestPhysicalHostBase + 0x1000 - 0xE0000000);
  }
  return offsets;
}();

constexpr uint32_t GuestAliasHostOffset(uint32_t guest_address) noexcept {
  return kGuestAliasHostOffsets[guest_address >> 24];
}

}  // namespace rex::memory
