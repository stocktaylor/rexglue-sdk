/**
 * @file        graphics/webgpu/shared_memory.cpp
 * @brief       Guest physical memory as a WebGPU storage buffer
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/shared_memory.h>

#include <cstring>

#include <rex/graphics/webgpu/command_processor.h>
#include <rex/logging.h>
#include <rex/memory.h>

namespace rex::graphics::webgpu {

WebGPUSharedMemory::~WebGPUSharedMemory() {
  Shutdown(true);
}

bool WebGPUSharedMemory::Initialize() {
  InitializeCommon();
  ui::webgpu::WebGPUDeviceThread& device_thread = command_processor_.device_thread();
  device_thread.Call([&] {
    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    desc.label = {"Shared memory", WGPU_STRLEN};
    desc.size = kBufferSize;
    desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_Index | WGPUBufferUsage_CopySrc |
                 WGPUBufferUsage_CopyDst;
    buffer_ = wgpuDeviceCreateBuffer(device_thread.device().device(), &desc);
  });
  if (!buffer_) {
    REXGPU_ERROR("WebGPU shared memory: couldn't create the {} MB buffer", kBufferSize >> 20);
    Shutdown();
    return false;
  }
  return true;
}

void WebGPUSharedMemory::Shutdown(bool from_destructor) {
  if (buffer_) {
    WGPUBuffer buffer = buffer_;
    buffer_ = nullptr;
    command_processor_.device_thread().Post([buffer] { wgpuBufferRelease(buffer); });
  }
  if (!from_destructor) {
    ShutdownCommon();
  }
}

bool WebGPUSharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  WebGPUCommandList& command_list = command_processor_.command_list();
  command_processor_.EndRenderPass();
  for (auto [page_start, page_count] : upload_page_ranges) {
    uint32_t start = page_start << page_size_log2();
    uint32_t length = page_count << page_size_log2();
    uint64_t staging_offset;
    void* staging = command_list.AllocateUpload(length, staging_offset);
    MakeRangeValid(start, length, false);
    std::memcpy(staging, memory().TranslatePhysical(start), length);
    command_list.RecordUploadCopy(staging_offset, buffer_, start, length);
  }
  return true;
}

}  // namespace rex::graphics::webgpu
