/**
 * @file        graphics/webgpu/graphics_system.h
 * @brief       WebGPU GPU backend
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>

namespace rex::graphics::webgpu {

// gpu_backend=webgpu: in development (docs/WASM.md, phase 5), so never chosen
// by "any".
class WebGPUGraphicsSystem : public GraphicsSystem {
 public:
  WebGPUGraphicsSystem();
  ~WebGPUGraphicsSystem() override;

  static bool IsAvailable() { return true; }

  std::string name() const override;

 protected:
  void CreateProvider(bool with_presentation) override;

 private:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace rex::graphics::webgpu
