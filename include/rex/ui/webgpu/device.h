/**
 * @file        ui/webgpu/device.h
 * @brief       WebGPU instance, adapter and device creation (webgpu.h on Dawn)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <functional>
#include <memory>
#include <string>

#include <webgpu/webgpu.h>

namespace rex::ui::webgpu {

// The WebGPU device used by the presenter and the GPU backend. Natively it is
// Dawn; in the browser, emdawnwebgpu over the page's navigator.gpu.
//
// Adapter and device requests are asynchronous. In the browser the device
// belongs to the worker that creates it (the WebGPU device thread, docs/WASM.md
// phase 5), which must return to its event loop for them to complete, so use
// CreateAsync there. Natively Create waits for them.
class WebGPUDevice {
 public:
  using CreatedCallback = std::function<void(std::unique_ptr<WebGPUDevice> device)>;

  // Creates the instance and requests an adapter, then a device with the
  // adapter's limits and the optional features the backend can use. Calls
  // created exactly once, with nullptr on failure; in the browser, from the
  // calling thread's event loop.
  static void CreateAsync(CreatedCallback created);

#if !defined(__EMSCRIPTEN__)
  // Blocks until the device is created. Native only.
  static std::unique_ptr<WebGPUDevice> Create();
#endif

  ~WebGPUDevice();
  WebGPUDevice(const WebGPUDevice&) = delete;
  WebGPUDevice& operator=(const WebGPUDevice&) = delete;

  WGPUInstance instance() const { return instance_; }
  WGPUAdapter adapter() const { return adapter_; }
  WGPUDevice device() const { return device_; }
  WGPUQueue queue() const { return queue_; }

  // Limits the device was created with: the adapter's own.
  const WGPULimits& limits() const { return limits_; }
  bool HasFeature(WGPUFeatureName feature) const;

  const std::string& adapter_description() const { return adapter_description_; }

  // Set once the device is lost; it can't be used afterwards.
  bool is_lost() const { return lost_; }

 private:
  struct Creation;

  WebGPUDevice() = default;

  static WGPUInstance CreateInstance();
  static WGPURequestAdapterOptions AdapterOptions();
  // Records the adapter and prepares the device descriptor in creation.
  static bool PrepareDevice(Creation& creation, WGPUAdapter adapter);
  void OnDeviceCreated(WGPUDevice device);

  WGPUInstance instance_ = nullptr;
  WGPUAdapter adapter_ = nullptr;
  WGPUDevice device_ = nullptr;
  WGPUQueue queue_ = nullptr;
  WGPULimits limits_ = {};
  WGPUSupportedFeatures features_ = {};
  std::string adapter_description_;
  bool lost_ = false;
};

}  // namespace rex::ui::webgpu
