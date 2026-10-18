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
// must reach the same host bytes (memory/guest_layout.h). Run with node.

#include <cstdio>

#include <rex/cvar.h>
#include <rex/platform.h>
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
}

}  // namespace

int main(int argc, char** argv) {
  rex::cvar::Init(argc, argv);

  auto graphics = rex::system::LoadGpuPlugin("xenos", "null");
  Check(graphics != nullptr, "null GPU created from the statically linked xenos plugin");

  CheckGuestMemory();

  std::printf("%s\n", failures ? "FAILED" : "PASSED");
  return failures ? 1 : 0;
}
