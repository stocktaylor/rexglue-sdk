/**
 * @file        ui/webgpu/webgpu_provider.cpp
 * @brief       WebGPU graphics provider: presenter and immediate drawer
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/webgpu/provider.h>

#include <rex/ui/webgpu/immediate_drawer.h>
#include <rex/ui/webgpu/presenter.h>

namespace rex::ui::webgpu {

std::unique_ptr<WebGPUProvider> WebGPUProvider::Create(const std::string& canvas_selector) {
  std::unique_ptr<WebGPUDeviceThread> device_thread = WebGPUDeviceThread::Start(canvas_selector);
  if (!device_thread) {
    return nullptr;
  }
  return std::unique_ptr<WebGPUProvider>(new WebGPUProvider(std::move(device_thread)));
}

std::unique_ptr<Presenter> WebGPUProvider::CreatePresenter(
    Presenter::HostGpuLossCallback host_gpu_loss_callback) {
  return WebGPUPresenter::Create(host_gpu_loss_callback, *device_thread_);
}

std::unique_ptr<ImmediateDrawer> WebGPUProvider::CreateImmediateDrawer() {
  return WebGPUImmediateDrawer::Create(*device_thread_);
}

}  // namespace rex::ui::webgpu
