/**
 * @file        rex/core/fiber_wasm.cpp
 * @brief       WebAssembly backend for rex::thread::Fiber (thread fibers only)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>
#if REX_PLATFORM_WASM

#include <cassert>
#include <cstdlib>

#include <rex/logging.h>
#include <rex/thread/fiber.h>

namespace rex::thread {

// WebAssembly can't switch stacks by itself: that takes JSPI or Asyncify,
// which is still to be chosen (docs/WASM.md, phase 2). Until then a thread can
// become a fiber, which every guest thread does, but guest-created fibers
// can't be made or switched to.

thread_local Fiber* Fiber::tls_current_ = nullptr;

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  (void)stack_size;
  (void)entry;
  (void)arg;
  REXLOG_ERROR("Fiber::Create: guest fibers are not supported on WebAssembly yet");
  return nullptr;
}

void Fiber::SwitchTo(Fiber* target) {
  if (target == tls_current_) {
    return;
  }
  REXLOG_CRITICAL("Fiber::SwitchTo: fiber switching is not supported on WebAssembly yet");
  std::abort();
}

void Fiber::Destroy() {
  if (is_thread_fiber_) {
    tls_current_ = nullptr;
  } else {
    assert(this != tls_current_ && "Destroy called on the currently running fiber");
  }
  delete this;
}

}  // namespace rex::thread

#endif  // REX_PLATFORM_WASM
