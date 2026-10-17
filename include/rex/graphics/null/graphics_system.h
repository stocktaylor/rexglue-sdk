#pragma once
/**
 * @file        graphics/null/graphics_system.h
 * @brief       GPU backend that executes the command stream without a host GPU
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <memory>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>

namespace rex::graphics::null {

// Runs the PM4 command stream (registers, fences, waits, interrupts) and the
// vblank timer like any backend, but draws and resolves nothing, so titles
// keep running at full rate without D3D12 or Vulkan. Optionally samples one
// frame per interval as an untextured wireframe (see null_gpu_sample_* cvars).
class NullGraphicsSystem : public GraphicsSystem {
 public:
  NullGraphicsSystem();
  ~NullGraphicsSystem() override;

  static bool IsAvailable() { return true; }

  std::string name() const override;

 protected:
  void CreateProvider(bool with_presentation) override;

 private:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace rex::graphics::null
