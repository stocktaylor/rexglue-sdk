/**
 * @file        core/exception_handler_wasm.cpp
 * @brief       WebAssembly backend for rex::arch::ExceptionHandler
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/exception_handler.h>

#if REX_PLATFORM_WASM

namespace rex::arch {

// WebAssembly has no hardware exceptions to intercept: memory accesses never
// fault (there is no page protection) and a trap ends the instance. Handlers
// are accepted so callers like MMIOHandler work unchanged, but never run; MMIO
// goes through the inline REX_MM_* checks instead.

void ExceptionHandler::Install(Handler fn, void* data) {
  (void)fn;
  (void)data;
}

void ExceptionHandler::Uninstall(Handler fn, void* data) {
  (void)fn;
  (void)data;
}

}  // namespace rex::arch

#endif  // REX_PLATFORM_WASM
