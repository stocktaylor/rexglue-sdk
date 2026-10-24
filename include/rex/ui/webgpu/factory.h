/**
 * @file        ui/webgpu/factory.h
 * @brief       Creates the WebGPU graphics provider, without WebGPU types
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>

#include <rex/ui/graphics_provider.h>

namespace rex::ui::webgpu {

// For code outside the runtime (the GPU plugin), which doesn't see webgpu.h:
// WebGPUProvider::Create() as a plain GraphicsProvider, or nullptr if the
// device can't be created.
std::unique_ptr<GraphicsProvider> CreateWebGPUProvider();

}  // namespace rex::ui::webgpu
