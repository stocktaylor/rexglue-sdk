/**
 * @file        tests/webgpu/device_test.cpp
 * @brief       WebGPU device bring-up test
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

// Creates the WebGPU device the GPU backend will use and runs a compute shader
// over a 512 MB storage buffer, the size of guest shared memory, then reads a
// word back. Natively on Dawn; the WebAssembly build only proves it compiles
// and links against emdawnwebgpu, as node has no WebGPU (the browser test
// comes with the device thread).

#include <cstdint>
#include <cstdio>

#include <rex/ui/webgpu/device.h>

#if defined(__EMSCRIPTEN__)

int main() {
  rex::ui::webgpu::WebGPUDevice::CreateAsync(
      [](auto device) { std::printf("%s\n", device ? "device created" : "no device"); });
  return 0;
}

#else

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
  std::printf("%s: %s\n", ok ? "OK" : "FAIL", what);
  if (!ok) {
    ++failures;
  }
}

WGPUStringView View(const char* s) {
  return {s, WGPU_STRLEN};
}

constexpr uint64_t kSharedMemorySize = uint64_t(512) << 20;
constexpr uint32_t kProbeOffset = (256u << 20) / 4;  // word index halfway in

const char kShader[] = R"(
@group(0) @binding(0) var<storage, read_write> memory : array<u32>;
@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) id : vec3u) {
  memory[)"
                       "67108864"
                       R"( + id.x] = 0xC0DE0000u + id.x;
}
)";

}  // namespace

int main() {
  using rex::ui::webgpu::WebGPUDevice;
  auto device = WebGPUDevice::Create();
  Check(device != nullptr, "WebGPU device created");
  if (!device) {
    return 1;
  }
  std::printf("adapter: %s\n", device->adapter_description().c_str());
  Check(device->limits().maxStorageBufferBindingSize >= kSharedMemorySize,
        "a 512 MB storage binding is allowed");
  WGPUDevice dev = device->device();

  WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
  buffer_desc.size = kSharedMemorySize;
  buffer_desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc;
  WGPUBuffer memory = wgpuDeviceCreateBuffer(dev, &buffer_desc);
  buffer_desc.size = 256;
  buffer_desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
  WGPUBuffer readback = wgpuDeviceCreateBuffer(dev, &buffer_desc);

  WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
  wgsl.code = View(kShader);
  WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
  module_desc.nextInChain = &wgsl.chain;
  WGPUShaderModule module = wgpuDeviceCreateShaderModule(dev, &module_desc);
  WGPUComputePipelineDescriptor pipeline_desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
  pipeline_desc.compute.module = module;
  pipeline_desc.compute.entryPoint = View("main");
  WGPUComputePipeline pipeline = wgpuDeviceCreateComputePipeline(dev, &pipeline_desc);

  WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
  entry.binding = 0;
  entry.buffer = memory;
  entry.size = kSharedMemorySize;
  WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
  group_desc.layout = wgpuComputePipelineGetBindGroupLayout(pipeline, 0);
  group_desc.entryCount = 1;
  group_desc.entries = &entry;
  WGPUBindGroup group = wgpuDeviceCreateBindGroup(dev, &group_desc);

  WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(dev, nullptr);
  WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(encoder, nullptr);
  wgpuComputePassEncoderSetPipeline(pass, pipeline);
  wgpuComputePassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
  wgpuComputePassEncoderDispatchWorkgroups(pass, 1, 1, 1);
  wgpuComputePassEncoderEnd(pass);
  wgpuCommandEncoderCopyBufferToBuffer(encoder, memory, uint64_t(kProbeOffset) * 4, readback, 0,
                                       256);
  WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
  wgpuQueueSubmit(device->queue(), 1, &commands);

  bool mapped = false;
  WGPUBufferMapCallbackInfo map_callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
  map_callback.mode = WGPUCallbackMode_WaitAnyOnly;
  map_callback.userdata1 = &mapped;
  map_callback.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* userdata, void*) {
    *static_cast<bool*>(userdata) = status == WGPUMapAsyncStatus_Success;
  };
  WGPUFutureWaitInfo wait = {wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0, 256, map_callback),
                             false};
  wgpuInstanceWaitAny(device->instance(), 1, &wait, UINT64_MAX);
  Check(mapped, "readback buffer maps");
  if (mapped) {
    auto* words = static_cast<const uint32_t*>(wgpuBufferGetConstMappedRange(readback, 0, 256));
    Check(words[0] == 0xC0DE0000u && words[63] == 0xC0DE003Fu,
          "a compute shader writes halfway into the 512 MB buffer");
    wgpuBufferUnmap(readback);
  }
  Check(!device->is_lost(), "device not lost");

  wgpuCommandBufferRelease(commands);
  wgpuComputePassEncoderRelease(pass);
  wgpuCommandEncoderRelease(encoder);
  wgpuBindGroupRelease(group);
  wgpuBindGroupLayoutRelease(group_desc.layout);
  wgpuComputePipelineRelease(pipeline);
  wgpuShaderModuleRelease(module);
  wgpuBufferRelease(readback);
  wgpuBufferRelease(memory);

  std::printf("%s\n", failures ? "FAILED" : "PASSED");
  return failures ? 1 : 0;
}

#endif
