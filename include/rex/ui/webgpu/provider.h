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

#include <rex/ui/graphics_provider.h>
#include <rex/ui/webgpu/device.h>

namespace rex::ui::webgpu {

class WebGPUProvider : public GraphicsProvider {
 public:
#if !defined(__EMSCRIPTEN__)
  // Creates the device, waiting for it. Native only.
  static std::unique_ptr<WebGPUProvider> Create();
#endif
  static std::unique_ptr<WebGPUProvider> Create(std::unique_ptr<WebGPUDevice> device);

  WebGPUDevice& device() const { return *device_; }

  std::unique_ptr<Presenter> CreatePresenter(Presenter::HostGpuLossCallback host_gpu_loss_callback =
                                                 Presenter::FatalErrorHostGpuLossCallback) override;

  std::unique_ptr<ImmediateDrawer> CreateImmediateDrawer() override;

 private:
  explicit WebGPUProvider(std::unique_ptr<WebGPUDevice> device) : device_(std::move(device)) {}

  std::unique_ptr<WebGPUDevice> device_;
};

}  // namespace rex::ui::webgpu
