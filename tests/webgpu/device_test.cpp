/**
 * @file        tests/webgpu/device_test.cpp
 * @brief       WebGPU device thread bring-up test
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

// Starts the WebGPU device thread and, through it, runs a compute shader over a
// 512 MB storage buffer, the size of guest shared memory, reading a word back:
// the map completes on the device thread and the main thread waits for it, as
// the backend will. In the browser it then clears the page's canvas, which the
// device thread owns, to green for a few frames. Same code natively (Dawn) and
// in the browser (emdawnwebgpu, run through emrun; see docs/WASM.md).

#include <cstdint>
#include <cstdio>
#include <future>

#include <rex/ui/webgpu/device_thread.h>

namespace {

using rex::ui::webgpu::kDeviceThreadCallbackMode;
using rex::ui::webgpu::WebGPUDevice;
using rex::ui::webgpu::WebGPUDeviceThread;

int failures = 0;

void Check(bool ok, const char* what) {
  std::printf("%s: %s\n", ok ? "OK" : "FAIL", what);
  std::fflush(stdout);
  if (!ok) {
    ++failures;
  }
}

WGPUStringView View(const char* s) {
  return {s, WGPU_STRLEN};
}

constexpr uint64_t kSharedMemorySize = uint64_t(512) << 20;
constexpr uint64_t kProbeOffset = uint64_t(256) << 20;  // halfway in

const char kShader[] = R"(
@group(0) @binding(0) var<storage, read_write> memory : array<u32>;
@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) id : vec3u) {
  memory[67108864u + id.x] = 0xC0DE0000u + id.x;  // word 67108864 = byte 256 MB
}
)";

// On the device thread: dispatches the shader, copies the probe out and maps
// it; result is set from the map callback, also on the device thread.
void RunCompute(WebGPUDeviceThread& thread, std::promise<uint32_t>& result) {
  WGPUDevice dev = thread.device().device();
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
  WGPUBindGroupLayout layout = wgpuComputePipelineGetBindGroupLayout(pipeline, 0);
  WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
  group_desc.layout = layout;
  group_desc.entryCount = 1;
  group_desc.entries = &entry;
  WGPUBindGroup group = wgpuDeviceCreateBindGroup(dev, &group_desc);

  WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(dev, nullptr);
  WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(encoder, nullptr);
  wgpuComputePassEncoderSetPipeline(pass, pipeline);
  wgpuComputePassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
  wgpuComputePassEncoderDispatchWorkgroups(pass, 1, 1, 1);
  wgpuComputePassEncoderEnd(pass);
  wgpuCommandEncoderCopyBufferToBuffer(encoder, memory, kProbeOffset, readback, 0, 256);
  WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
  wgpuQueueSubmit(thread.device().queue(), 1, &commands);

  WGPUBufferMapCallbackInfo map_callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
  map_callback.mode = kDeviceThreadCallbackMode;
  map_callback.userdata1 = readback;
  map_callback.userdata2 = &result;
  map_callback.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* userdata1,
                             void* userdata2) {
    auto readback = static_cast<WGPUBuffer>(userdata1);
    uint32_t words[2] = {};
    if (status == WGPUMapAsyncStatus_Success) {
      auto* data = static_cast<const uint32_t*>(wgpuBufferGetConstMappedRange(readback, 0, 256));
      words[0] = data[0];
      words[1] = data[63];
      wgpuBufferUnmap(readback);
    }
    wgpuBufferRelease(readback);
    bool ok = words[0] == 0xC0DE0000u && words[1] == 0xC0DE003Fu;
    static_cast<std::promise<uint32_t>*>(userdata2)->set_value(ok ? 1 : 0);
  };
  wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0, 256, map_callback);

  wgpuCommandBufferRelease(commands);
  wgpuComputePassEncoderRelease(pass);
  wgpuCommandEncoderRelease(encoder);
  wgpuBindGroupRelease(group);
  wgpuBindGroupLayoutRelease(layout);
  wgpuComputePipelineRelease(pipeline);
  wgpuShaderModuleRelease(module);
  wgpuBufferRelease(memory);
}

#if defined(__EMSCRIPTEN__)
// On the device thread: clears the canvas it owns. The frame shows when the
// thread returns to its event loop, after this task.
bool ClearCanvas(WebGPUDeviceThread& thread, WGPUSurface& surface) {
  WebGPUDevice& device = thread.device();
  if (!surface) {
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector source =
        WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    source.selector = View(thread.canvas_selector().c_str());
    WGPUSurfaceDescriptor descriptor = WGPU_SURFACE_DESCRIPTOR_INIT;
    descriptor.nextInChain = &source.chain;
    surface = wgpuInstanceCreateSurface(device.instance(), &descriptor);
    WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
    config.device = device.device();
    config.format = WGPUTextureFormat_BGRA8Unorm;
    config.usage = WGPUTextureUsage_RenderAttachment;
    config.width = 640;
    config.height = 360;
    wgpuSurfaceConfigure(surface, &config);
  }
  WGPUSurfaceTexture surface_texture = WGPU_SURFACE_TEXTURE_INIT;
  wgpuSurfaceGetCurrentTexture(surface, &surface_texture);
  if (surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
      surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
    return false;
  }
  WGPUTextureView view = wgpuTextureCreateView(surface_texture.texture, nullptr);
  WGPURenderPassColorAttachment attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
  attachment.view = view;
  attachment.loadOp = WGPULoadOp_Clear;
  attachment.storeOp = WGPUStoreOp_Store;
  attachment.clearValue = {0.15, 0.75, 0.3, 1.0};
  WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
  pass_desc.colorAttachmentCount = 1;
  pass_desc.colorAttachments = &attachment;
  WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device.device(), nullptr);
  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
  wgpuRenderPassEncoderEnd(pass);
  WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
  wgpuQueueSubmit(device.queue(), 1, &commands);
  wgpuCommandBufferRelease(commands);
  wgpuRenderPassEncoderRelease(pass);
  wgpuCommandEncoderRelease(encoder);
  wgpuTextureViewRelease(view);
  wgpuTextureRelease(surface_texture.texture);
  return true;
}
#endif

}  // namespace

int main() {
#if defined(__EMSCRIPTEN__)
  auto thread = WebGPUDeviceThread::Start("#canvas");
#else
  auto thread = WebGPUDeviceThread::Start();
#endif
  Check(thread != nullptr, "device thread started with a device");
  if (!thread) {
    std::printf("FAILED\n");
    return 1;
  }
  thread->Call([&] {
    std::printf("adapter: %s\n", thread->device().adapter_description().c_str());
    Check(thread->device().limits().maxStorageBufferBindingSize >= kSharedMemorySize,
          "a 512 MB storage binding is allowed");
  });
  Check(!thread->IsCurrentThread(), "the caller isn't the device thread");

  std::promise<uint32_t> result;
  std::future<uint32_t> result_future = result.get_future();
  thread->Call([&] { RunCompute(*thread, result); });
  Check(result_future.get() == 1,
        "a compute shader writes halfway into the 512 MB buffer; the map completes on the "
        "device thread while the caller waits");

#if defined(__EMSCRIPTEN__)
  WGPUSurface surface = nullptr;
  bool cleared = true;
  for (int frame = 0; frame < 30; ++frame) {
    // Call returns after the task; the thread presents when it goes back to
    // its event loop, before taking the next task.
    thread->Call([&] { cleared &= ClearCanvas(*thread, surface); });
  }
  Check(cleared, "the device thread clears the canvas it owns, 30 frames");
  thread->Call([&] {
    if (surface) {
      wgpuSurfaceUnconfigure(surface);
      wgpuSurfaceRelease(surface);
    }
  });
#endif

  bool lost = false;
  thread->Call([&] { lost = thread->device().is_lost(); });
  Check(!lost, "device not lost");
  thread.reset();
  std::printf("%s\n", failures ? "FAILED" : "PASSED");
  std::fflush(stdout);
  return failures ? 1 : 0;
}
