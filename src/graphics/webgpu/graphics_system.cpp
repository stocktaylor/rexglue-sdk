/**
 * @file        graphics/webgpu/graphics_system.cpp
 * @brief       WebGPU GPU backend
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/graphics_system.h>

#include <rex/graphics/webgpu/command_processor.h>
#include <rex/ui/webgpu/provider.h>

namespace rex::graphics::webgpu {

WebGPUGraphicsSystem::WebGPUGraphicsSystem() {}

WebGPUGraphicsSystem::~WebGPUGraphicsSystem() {}

std::string WebGPUGraphicsSystem::name() const {
  return "WebGPU";
}

void WebGPUGraphicsSystem::CreateProvider(bool with_presentation) {
  (void)with_presentation;
  provider_ = rex::ui::webgpu::WebGPUProvider::Create();
}

std::unique_ptr<CommandProcessor> WebGPUGraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<WebGPUCommandProcessor>(this, kernel_state_);
}

}  // namespace rex::graphics::webgpu
