/**
 * @file        tests/wasm/smoke.cpp
 * @brief       WebAssembly link and startup smoke test
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

// Links the runtime and the xenos GPU plugin the way a game does on wasm
// (static, plugin whole-archive), creates the null GPU through the normal
// plugin loader, and checks the guest memory layout: the aliased guest ranges
// must reach the same host bytes (memory/guest_layout.h), and floating point
// that must match x86. Run with node.

#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

#include <rex/cvar.h>
#include <rex/memory/write_watch.h>
#include <rex/platform.h>
#include <rex/ppc/fpu.h>
#include <rex/ppc/intrinsics.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/xmemory.h>

static_assert(REX_PLATFORM_WASM && REX_ARCH_WASM, "WebAssembly-only test");
static_assert(sizeof(void*) == 8, "wasm64 (memory64) expected");

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
  std::printf("%s: %s\n", ok ? "OK" : "FAIL", what);
  if (!ok) {
    ++failures;
  }
}

void CheckWriteWatch(rex::memory::Memory& memory, uint32_t address);

void CheckGuestMemory() {
  rex::memory::Memory memory;
  if (!memory.Initialize()) {
    Check(false, "guest memory initializes");
    return;
  }
  Check(true, "guest memory initializes");

  // One physical byte, reached through every physical view.
  *memory.TranslatePhysical<uint32_t*>(0x00012340) = 0xA1B2C3D4;
  Check(*memory.TranslateVirtual<uint32_t*>(0xA0012340) == 0xA1B2C3D4,
        "0xA0000000 view reaches physical memory");
  Check(*memory.TranslateVirtual<uint32_t*>(0xC0012340) == 0xA1B2C3D4,
        "0xC0000000 view reaches physical memory");
  Check(*memory.TranslateVirtual<uint32_t*>(0xE0011340) == 0xA1B2C3D4,
        "0xE0000000 view is offset by 4 KB");
  Check(rex::memory::GuestPtr<uint32_t*>(memory.virtual_membase(), 0xA0012340) ==
            memory.TranslatePhysical<uint32_t*>(0x00012340),
        "GuestPtr (as generated code) matches the runtime");
  Check(memory.HostToGuestVirtual(memory.TranslatePhysical(0x00012340)) == 0xA0012340,
        "physical host addresses map back to 0xA0000000");

  // A physical heap allocation is visible through its physical address.
  auto* heap = memory.LookupHeap(0xA0000000);
  uint32_t address = 0;
  bool allocated =
      heap &&
      heap->Alloc(0x10000, 0x10000,
                  rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                  rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false,
                  &address);
  Check(allocated, "0xA0000000 heap allocates");
  if (allocated) {
    *memory.TranslateVirtual<uint32_t*>(address) = 0x12345678;
    Check(*memory.TranslatePhysical<uint32_t*>(memory.GetPhysicalAddress(address)) == 0x12345678,
          "heap allocation is backed by physical memory");
  }

  // Plain virtual memory is not aliased.
  *memory.TranslateVirtual<uint32_t*>(0x40001000) = 0x55AA55AA;
  Check(*memory.TranslatePhysical<uint32_t*>(0x00001000) != 0x55AA55AA,
        "virtual memory is separate from physical");

  if (allocated) {
    CheckWriteWatch(memory, address);
  }
}

// GPU caches watch physical pages and drop their copy on the first CPU write.
// Wasm can't fault on a read-only page, so writes check a bitmap
// (memory/write_watch.h) and make the same callback.
void CheckWriteWatch(rex::memory::Memory& memory, uint32_t address) {
  struct Seen {
    uint32_t calls = 0;
    uint32_t start = 0;
  } seen;
  auto callback = [](void* context, uint32_t start, uint32_t length,
                     bool exact) -> std::pair<uint32_t, uint32_t> {
    auto* s = static_cast<Seen*>(context);
    ++s->calls;
    s->start = start;
    return {0, UINT32_MAX};
  };
  void* handle = memory.RegisterPhysicalMemoryInvalidationCallback(callback, &seen);
  uint32_t physical = memory.GetPhysicalAddress(address);

  memory.EnablePhysicalMemoryAccessCallbacks(physical, 0x1000, true, false);
  Check(rex::memory::IsWriteWatched(rex::memory::WriteWatchBits(), address),
        "watching a physical page sets its write-watch bit");
  Check(!rex::memory::IsWriteWatched(rex::memory::WriteWatchBits(), address + 0x1000),
        "the next page stays unwatched");

  // As generated code stores: write, then check.
  *memory.TranslateVirtual<uint32_t*>(address + 0x10) = 0x01020304;
  rex::memory::CheckWriteWatch(address + 0x10, 4);
  Check(seen.calls == 1 && seen.start == (physical & ~0xFFFu),
        "a write to a watched page runs the invalidation callback for it");
  Check(!rex::memory::IsWriteWatched(rex::memory::WriteWatchBits(), address),
        "the callback unwatches the page");
  rex::memory::CheckWriteWatch(address + 0x20, 4);
  Check(seen.calls == 1, "a watch fires once");

  memory.UnregisterPhysicalMemoryInvalidationCallback(handle);
}

// Sets the guest rounding mode (FPSCR[RN]: 0 nearest, 1 toward zero, 2 up,
// 3 down) the way generated code's mtfsf does.
void SetGuestRounding(uint32_t mode) {
  using Platform = rex::platform::FPSCRPlatform;
  Platform::setcsr((Platform::getcsr() & ~Platform::RoundMaskVal) | Platform::GuestToHost[mode]);
}

// Wasm has no rounding control, so the FPU macros emulate FPSCR's directed
// modes (ppc/fpu.h), and a few SIMDe fallbacks are replaced to match x86
// (ppc/intrinsics.h). Each expected value is what x86 gives natively.
void CheckFloatingPoint() {
  volatile double tiny = 0x1p-60, one = 1.0, three = 3.0;

  SetGuestRounding(0);
  Check(REX_FADD(one, tiny) == 1.0, "fadd rounds to nearest by default");
  SetGuestRounding(2);
  Check(REX_FADD(one, tiny) == std::nextafter(1.0, 2.0), "fadd rounds up");
  Check(REX_FDIV(one, three) == std::nextafter(1.0 / 3.0, 1.0), "fdiv rounds up");
  Check(REX_FMUL(0x1p-600 * one, 0x1p-600 * one) == std::numeric_limits<double>::denorm_min(),
        "fmul rounds an underflow up to the smallest denormal");
  Check(REX_FMA(one, one, 0x1p-200 * one) == std::nextafter(1.0, 2.0), "fmadd rounds up");
  Check(REX_FSINGLE(REX_FADD(one, 0x1p-40 * one)) == double(std::nextafter(1.0f, 2.0f)),
        "single precision rounds up");
  Check(REX_FCFID((int64_t(1) << 53) + 1 + int64_t(one) - 1) == 0x1p53 + 2, "fcfid rounds up");
  SetGuestRounding(3);
  Check(REX_FADD(one, -tiny) == std::nextafter(1.0, 0.0), "fadd rounds down");
  Check(std::signbit(REX_FSUB(one, one)), "x - x is -0 when rounding down");
  Check(REX_FMUL(1e308 * one, 10.0) == std::numeric_limits<double>::max(),
        "fmul rounds a positive overflow down to the largest finite value");
  SetGuestRounding(1);
  Check(REX_FSUB(tiny, one) == std::nextafter(-1.0, 0.0), "fsub rounds toward zero");
  SetGuestRounding(0);

  // fctid: ties to even (SIMDe's fallback rounds them away from zero), and
  // the guest's directed modes.
  Check(simde_mm_cvtsd_si64(simde_mm_set_sd(2.5)) == 2, "fctid rounds ties to even");
  Check(simde_mm_cvtsd_si32(simde_mm_set_sd(-3.5)) == -4, "fctiw rounds ties to even");
  SetGuestRounding(2);
  Check(simde_mm_cvtsd_si64(simde_mm_set_sd(2.25)) == 3, "fctid rounds up");
  SetGuestRounding(0);

  // vmsum4fp: dpps adds (p0 + p1) + (p2 + p3), here (1e8 + 1) + (-1e8 + 1) = 0
  // in single precision; adding in sequence gives 1.
  simde__m128 products = simde_mm_set_ps(1.0f, -1e8f, 1.0f, 1e8f);
  Check(simde_mm_cvtss_f32(simde_mm_dp_ps(products, simde_mm_set1_ps(1.0f), 0xF1)) == 0.0f,
        "dp_ps adds pairwise like dpps");
}

}  // namespace

int main(int argc, char** argv) {
  rex::cvar::Init(argc, argv);

  auto graphics = rex::system::LoadGpuPlugin("xenos", "null");
  Check(graphics != nullptr, "null GPU created from the statically linked xenos plugin");

  CheckGuestMemory();
  CheckFloatingPoint();

  std::printf("%s\n", failures ? "FAILED" : "PASSED");
  return failures ? 1 : 0;
}
