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
// (static, plugin whole-archive) and creates the null GPU through the normal
// plugin loader, proving static registration works. Run with node.

#include <cstdio>

#include <rex/cvar.h>
#include <rex/platform.h>
#include <rex/system/gpu_plugin.h>

static_assert(REX_PLATFORM_WASM && REX_ARCH_WASM, "WebAssembly-only test");
static_assert(sizeof(void*) == 8, "wasm64 (memory64) expected");

int main(int argc, char** argv) {
  rex::cvar::Init(argc, argv);
  auto graphics = rex::system::LoadGpuPlugin("xenos", "null");
  if (!graphics) {
    std::puts("FAIL: the xenos GPU plugin is not linked in");
    return 1;
  }
  std::puts("OK: created the null GPU from the statically linked xenos plugin");
  return 0;
}
