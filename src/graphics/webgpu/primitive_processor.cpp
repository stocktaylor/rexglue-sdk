/**
 * @file        graphics/webgpu/primitive_processor.cpp
 * @brief       Primitive and index buffer conversion for WebGPU
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/primitive_processor.h>

#include <rex/assert.h>
#include <rex/graphics/webgpu/command_processor.h>
#include <rex/logging.h>

namespace rex::graphics::webgpu {

WebGPUPrimitiveProcessor::~WebGPUPrimitiveProcessor() {
  Shutdown(true);
}

bool WebGPUPrimitiveProcessor::Initialize() {
  if (!InitializeCommon(true, false, false, false, false, false)) {
    Shutdown();
    return false;
  }
  return true;
}

void WebGPUPrimitiveProcessor::Shutdown(bool from_destructor) {
  converted_index_buffer_offsets_.clear();
  if (builtin_index_buffer_) {
    WGPUBuffer buffer = builtin_index_buffer_;
    builtin_index_buffer_ = nullptr;
    command_processor_.device_thread().Post([buffer] { wgpuBufferRelease(buffer); });
  }
  if (!from_destructor) {
    ShutdownCommon();
  }
}

bool WebGPUPrimitiveProcessor::InitializeBuiltinIndexBuffer(
    size_t size_bytes, std::function<void(void*)> fill_callback) {
  assert_not_zero(size_bytes);
  assert_null(builtin_index_buffer_);
  ui::webgpu::WebGPUDeviceThread& device_thread = command_processor_.device_thread();
  device_thread.Call([&] {
    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    desc.label = {"Built-in index buffer", WGPU_STRLEN};
    desc.size = rex::align(uint64_t(size_bytes), uint64_t(4));
    desc.usage = WGPUBufferUsage_Index;
    desc.mappedAtCreation = true;
    builtin_index_buffer_ = wgpuDeviceCreateBuffer(device_thread.device().device(), &desc);
    if (!builtin_index_buffer_) {
      return;
    }
    void* mapping = wgpuBufferGetMappedRange(builtin_index_buffer_, 0, desc.size);
    if (mapping) {
      fill_callback(mapping);
    }
    wgpuBufferUnmap(builtin_index_buffer_);
  });
  if (!builtin_index_buffer_) {
    REXGPU_ERROR("WebGPU primitive processor: couldn't create the {}-byte built-in index buffer",
                 size_bytes);
    return false;
  }
  return true;
}

void* WebGPUPrimitiveProcessor::RequestHostConvertedIndexBufferForCurrentFrame(
    xenos::IndexFormat format, uint32_t index_count, bool coalign_for_simd,
    uint32_t coalignment_original_address, size_t& backend_handle_out) {
  size_t index_size = format == xenos::IndexFormat::kInt16 ? sizeof(uint16_t) : sizeof(uint32_t);
  uint64_t offset;
  auto* mapping = static_cast<uint8_t*>(command_processor_.command_list().AllocateUpload(
      index_size * index_count + (coalign_for_simd ? XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE : 0),
      offset));
  if (coalign_for_simd) {
    ptrdiff_t coalignment_offset = GetSimdCoalignmentOffset(mapping, coalignment_original_address);
    mapping += coalignment_offset;
    offset += uint64_t(coalignment_offset);
  }
  backend_handle_out = converted_index_buffer_offsets_.size();
  converted_index_buffer_offsets_.push_back(offset);
  return mapping;
}

}  // namespace rex::graphics::webgpu
