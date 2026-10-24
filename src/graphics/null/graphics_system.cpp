/**
 * @file        graphics/null/graphics_system.cpp
 * @brief       GPU backend that executes the command stream without a host GPU
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/null/command_processor.h>
#include <rex/graphics/null/graphics_system.h>
#include <rex/system/kernel_state.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/software/provider.h>
#if REXGLUE_HAS_WEBGPU_RUNTIME
#include <rex/ui/webgpu/factory.h>
#endif

REXCVAR_DEFINE_STRING(null_gpu_presenter, "software", "GPU/Null",
                      "How the null GPU shows its sampled wireframes: software (SDL window "
                      "framebuffer) or webgpu (WebGPU presenter; needs an SDK built with "
                      "REXGLUE_USE_WEBGPU)")
    .allowed({"software", "webgpu"});

namespace rex::graphics::null {

NullGraphicsSystem::NullGraphicsSystem() {}

NullGraphicsSystem::~NullGraphicsSystem() {}

std::string NullGraphicsSystem::name() const {
  return "Null";
}

void NullGraphicsSystem::CreateProvider(bool with_presentation) {
  if (with_presentation && REXCVAR_GET(null_gpu_presenter) == "webgpu") {
#if REXGLUE_HAS_WEBGPU_RUNTIME
    provider_ = rex::ui::webgpu::CreateWebGPUProvider();
    if (provider_) {
      return;
    }
    REXGPU_WARN("Null GPU: no WebGPU device; presenting in software");
#else
    REXGPU_WARN("Null GPU: this SDK was built without WebGPU; presenting in software");
#endif
  }
  // The software provider needs nothing from the host, so headless and
  // presenting setups use the same one.
  provider_ = rex::ui::software::SoftwareProvider::Create();
}

std::unique_ptr<CommandProcessor> NullGraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<NullCommandProcessor>(this, kernel_state_);
}

}  // namespace rex::graphics::null
