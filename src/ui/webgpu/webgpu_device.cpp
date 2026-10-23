/**
 * @file        ui/webgpu/webgpu_device.cpp
 * @brief       WebGPU instance, adapter and device creation (webgpu.h on Dawn)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/webgpu/device.h>

#include <cstdint>
#include <string_view>
#include <vector>

#include <rex/logging.h>

namespace rex::ui::webgpu {

namespace {

std::string_view ToStringView(WGPUStringView view) {
  if (!view.data) {
    return {};
  }
  return view.length == WGPU_STRLEN ? std::string_view(view.data)
                                    : std::string_view(view.data, view.length);
}

// Optional features the GPU backend uses when present, each with a fallback
// (docs/WASM.md, phase 5).
constexpr WGPUFeatureName kOptionalFeatures[] = {
    WGPUFeatureName_TextureFormatsTier1,   // 16-bit unorm/snorm render targets
    WGPUFeatureName_DualSourceBlending,    //
    WGPUFeatureName_ClipDistances,         // user clip planes
    WGPUFeatureName_DepthClipControl,      // depth clamp without clipping
    WGPUFeatureName_Float32Blendable,      //
    WGPUFeatureName_TextureCompressionBC,  // DXT textures without decompressing
    WGPUFeatureName_PrimitiveIndex,        //
#if !defined(__EMSCRIPTEN__)
    // Dawn native: lets the UI thread (painting) and the GPU thread (guest
    // output, emulation) share the device. In the browser every call goes
    // through the device thread instead.
    WGPUFeatureName_ImplicitDeviceSynchronization,
#endif
};

}  // namespace

struct WebGPUDevice::Creation {
  std::unique_ptr<WebGPUDevice> device;
  CreatedCallback created;
  std::vector<WGPUFeatureName> features;
  WGPUDeviceDescriptor descriptor = WGPU_DEVICE_DESCRIPTOR_INIT;
};

WGPUInstance WebGPUDevice::CreateInstance() {
#if defined(__EMSCRIPTEN__)
  return wgpuCreateInstance(nullptr);
#else
  // Native creation waits on futures with a timeout.
  WGPUInstanceFeatureName features[] = {WGPUInstanceFeatureName_TimedWaitAny};
  WGPUInstanceDescriptor descriptor = WGPU_INSTANCE_DESCRIPTOR_INIT;
  descriptor.requiredFeatureCount = 1;
  descriptor.requiredFeatures = features;
  return wgpuCreateInstance(&descriptor);
#endif
}

WGPURequestAdapterOptions WebGPUDevice::AdapterOptions() {
  WGPURequestAdapterOptions options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
  options.powerPreference = WGPUPowerPreference_HighPerformance;
  return options;
}

bool WebGPUDevice::PrepareDevice(Creation& creation, WGPUAdapter adapter) {
  WebGPUDevice& device = *creation.device;
  device.adapter_ = adapter;

  WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
  if (wgpuAdapterGetInfo(adapter, &info) == WGPUStatus_Success) {
    device.adapter_description_ = std::string(ToStringView(info.device));
    std::string_view description = ToStringView(info.description);
    if (!description.empty()) {
      device.adapter_description_ += " (" + std::string(description) + ")";
    }
    wgpuAdapterInfoFreeMembers(info);
  }

  if (wgpuAdapterGetLimits(adapter, &device.limits_) != WGPUStatus_Success) {
    REXLOG_ERROR("WebGPU: couldn't get the adapter's limits");
    return false;
  }

  WGPUSupportedFeatures supported = WGPU_SUPPORTED_FEATURES_INIT;
  wgpuAdapterGetFeatures(adapter, &supported);
  for (WGPUFeatureName feature : kOptionalFeatures) {
    for (size_t i = 0; i < supported.featureCount; ++i) {
      if (supported.features[i] == feature) {
        creation.features.push_back(feature);
        break;
      }
    }
  }
  wgpuSupportedFeaturesFreeMembers(supported);

  WGPUDeviceDescriptor& descriptor = creation.descriptor;
  descriptor.requiredFeatureCount = creation.features.size();
  descriptor.requiredFeatures = creation.features.data();
  // Everything the adapter allows: shared memory alone needs a 512 MB buffer
  // and binding, past the defaults.
  descriptor.requiredLimits = &device.limits_;
  descriptor.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
  descriptor.deviceLostCallbackInfo.callback = [](WGPUDevice const*, WGPUDeviceLostReason reason,
                                                  WGPUStringView message, void* userdata, void*) {
    if (reason == WGPUDeviceLostReason_Destroyed ||
        reason == WGPUDeviceLostReason_CallbackCancelled) {
      return;
    }
    static_cast<WebGPUDevice*>(userdata)->lost_ = true;
    REXLOG_ERROR("WebGPU device lost: {}", ToStringView(message));
  };
  descriptor.deviceLostCallbackInfo.userdata1 = &device;
  descriptor.uncapturedErrorCallbackInfo.callback = [](WGPUDevice const*, WGPUErrorType type,
                                                       WGPUStringView message, void*, void*) {
    REXLOG_ERROR("WebGPU error ({}): {}", int(type), ToStringView(message));
  };
  return true;
}

void WebGPUDevice::OnDeviceCreated(WGPUDevice device) {
  device_ = device;
  queue_ = wgpuDeviceGetQueue(device);
  wgpuDeviceGetFeatures(device, &features_);
  REXLOG_INFO("WebGPU device: {}; max buffer {} MB, max storage binding {} MB",
              adapter_description_, limits_.maxBufferSize >> 20,
              limits_.maxStorageBufferBindingSize >> 20);
}

void WebGPUDevice::CreateAsync(CreatedCallback created) {
  auto* creation = new Creation;
  creation->device.reset(new WebGPUDevice);
  creation->created = std::move(created);
  auto finish = [](Creation* creation, bool succeeded) {
    std::unique_ptr<WebGPUDevice> device;
    if (succeeded) {
      device = std::move(creation->device);
    }
    CreatedCallback created = std::move(creation->created);
    delete creation;
    created(std::move(device));
  };

  creation->device->instance_ = CreateInstance();
  if (!creation->device->instance_) {
    REXLOG_ERROR("WebGPU: couldn't create an instance");
    finish(creation, false);
    return;
  }

  WGPURequestAdapterOptions options = AdapterOptions();
  WGPURequestAdapterCallbackInfo adapter_callback = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
  adapter_callback.mode = WGPUCallbackMode_AllowSpontaneous;
  adapter_callback.userdata1 = creation;
  adapter_callback.userdata2 = reinterpret_cast<void*>(+finish);
  adapter_callback.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter,
                                 WGPUStringView message, void* userdata1, void* userdata2) {
    auto* creation = static_cast<Creation*>(userdata1);
    auto finish = reinterpret_cast<void (*)(Creation*, bool)>(userdata2);
    if (status != WGPURequestAdapterStatus_Success) {
      REXLOG_ERROR("WebGPU: no adapter: {}", ToStringView(message));
      finish(creation, false);
      return;
    }
    if (!PrepareDevice(*creation, adapter)) {
      finish(creation, false);
      return;
    }
    WGPURequestDeviceCallbackInfo device_callback = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    device_callback.mode = WGPUCallbackMode_AllowSpontaneous;
    device_callback.userdata1 = creation;
    device_callback.userdata2 = userdata2;
    device_callback.callback = [](WGPURequestDeviceStatus status, WGPUDevice device,
                                  WGPUStringView message, void* userdata1, void* userdata2) {
      auto* creation = static_cast<Creation*>(userdata1);
      auto finish = reinterpret_cast<void (*)(Creation*, bool)>(userdata2);
      if (status != WGPURequestDeviceStatus_Success) {
        REXLOG_ERROR("WebGPU: device request failed: {}", ToStringView(message));
        finish(creation, false);
        return;
      }
      creation->device->OnDeviceCreated(device);
      finish(creation, true);
    };
    wgpuAdapterRequestDevice(adapter, &creation->descriptor, device_callback);
  };
  wgpuInstanceRequestAdapter(creation->device->instance_, &options, adapter_callback);
}

#if !defined(__EMSCRIPTEN__)
std::unique_ptr<WebGPUDevice> WebGPUDevice::Create() {
  Creation creation;
  creation.device.reset(new WebGPUDevice);
  WebGPUDevice& device = *creation.device;
  device.instance_ = CreateInstance();
  if (!device.instance_) {
    REXLOG_ERROR("WebGPU: couldn't create an instance");
    return nullptr;
  }

  WGPUAdapter adapter = nullptr;
  WGPURequestAdapterOptions options = AdapterOptions();
  WGPURequestAdapterCallbackInfo adapter_callback = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
  adapter_callback.mode = WGPUCallbackMode_WaitAnyOnly;
  adapter_callback.userdata1 = &adapter;
  adapter_callback.callback = [](WGPURequestAdapterStatus status, WGPUAdapter result,
                                 WGPUStringView message, void* userdata, void*) {
    if (status == WGPURequestAdapterStatus_Success) {
      *static_cast<WGPUAdapter*>(userdata) = result;
    } else {
      REXLOG_ERROR("WebGPU: no adapter: {}", ToStringView(message));
    }
  };
  WGPUFutureWaitInfo wait = {
      wgpuInstanceRequestAdapter(device.instance_, &options, adapter_callback), false};
  wgpuInstanceWaitAny(device.instance_, 1, &wait, UINT64_MAX);
  if (!adapter || !PrepareDevice(creation, adapter)) {
    return nullptr;
  }

  WGPUDevice created = nullptr;
  WGPURequestDeviceCallbackInfo device_callback = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
  device_callback.mode = WGPUCallbackMode_WaitAnyOnly;
  device_callback.userdata1 = &created;
  device_callback.callback = [](WGPURequestDeviceStatus status, WGPUDevice result,
                                WGPUStringView message, void* userdata, void*) {
    if (status == WGPURequestDeviceStatus_Success) {
      *static_cast<WGPUDevice*>(userdata) = result;
    } else {
      REXLOG_ERROR("WebGPU: device request failed: {}", ToStringView(message));
    }
  };
  wait = {wgpuAdapterRequestDevice(adapter, &creation.descriptor, device_callback), false};
  wgpuInstanceWaitAny(device.instance_, 1, &wait, UINT64_MAX);
  if (!created) {
    return nullptr;
  }
  device.OnDeviceCreated(created);
  return std::move(creation.device);
}
#endif

WebGPUDevice::~WebGPUDevice() {
  wgpuSupportedFeaturesFreeMembers(features_);
  if (queue_) {
    wgpuQueueRelease(queue_);
  }
  if (device_) {
    wgpuDeviceDestroy(device_);
    wgpuDeviceRelease(device_);
  }
  if (adapter_) {
    wgpuAdapterRelease(adapter_);
  }
  if (instance_) {
    wgpuInstanceRelease(instance_);
  }
}

bool WebGPUDevice::HasFeature(WGPUFeatureName feature) const {
  for (size_t i = 0; i < features_.featureCount; ++i) {
    if (features_.features[i] == feature) {
      return true;
    }
  }
  return false;
}

}  // namespace rex::ui::webgpu
