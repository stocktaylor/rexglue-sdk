/**
 * @file        ui/webgpu/provider.h
 * @brief       WebGPU graphics provider: presenter and immediate drawer
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <string>

#include <rex/ui/graphics_provider.h>
#include <rex/ui/webgpu/device_thread.h>

namespace rex::ui::webgpu {

class WebGPUProvider : public GraphicsProvider {
 public:
  // Starts the device thread and creates the device on it, waiting for both.
  // In the browser the thread takes over the canvas named by canvas_selector,
  // the one the SDL window is on (Emscripten's default is "#canvas").
  static std::unique_ptr<WebGPUProvider> Create(const std::string& canvas_selector = "#canvas");

  WebGPUDeviceThread& device_thread() const { return *device_thread_; }

  std::unique_ptr<Presenter> CreatePresenter(Presenter::HostGpuLossCallback host_gpu_loss_callback =
                                                 Presenter::FatalErrorHostGpuLossCallback) override;

  std::unique_ptr<ImmediateDrawer> CreateImmediateDrawer() override;

 private:
  explicit WebGPUProvider(std::unique_ptr<WebGPUDeviceThread> device_thread)
      : device_thread_(std::move(device_thread)) {}

  std::unique_ptr<WebGPUDeviceThread> device_thread_;
};

}  // namespace rex::ui::webgpu
