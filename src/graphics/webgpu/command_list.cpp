/**
 * @file        graphics/webgpu/command_list.cpp
 * @brief       GPU work recorded off the device thread, replayed on it
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/command_list.h>

#include <algorithm>
#include <cstring>

#include <rex/math.h>

namespace rex::graphics::webgpu {

void* WebGPUCommandList::AllocateUpload(size_t size, uint64_t& offset_out) {
  upload_bytes_ += size;
  size = rex::align(std::max(size, size_t(1)), kUploadAlignment);
  if (chunks_.empty() || chunks_.back().size - chunks_.back().used < size) {
    // Big uploads get a chunk of their own.
    size_t chunk_size = std::max(size, kChunkSize);
    chunks_.push_back(Chunk{std::make_unique<uint8_t[]>(chunk_size), upload_size_, chunk_size, 0});
    upload_size_ += chunk_size;
  }
  Chunk& chunk = chunks_.back();
  void* data = chunk.data.get() + chunk.used;
  offset_out = chunk.offset + chunk.used;
  chunk.used += size;
  return data;
}

void* WebGPUCommandList::AllocateUniforms(size_t size, uint32_t& offset_out) {
  size_t offset = rex::align(uniforms_.size(), kUniformAlignment);
  uniforms_.resize(offset + size);
  offset_out = uint32_t(offset);
  return uniforms_.data() + offset;
}

void WebGPUCommandList::RecordUploadCopy(uint64_t staging_offset, WGPUBuffer destination,
                                         uint64_t destination_offset, uint64_t size) {
  Record([=](Encoder& encoder) {
    wgpuCommandEncoderCopyBufferToBuffer(encoder.encoder, encoder.staging, staging_offset,
                                         destination, destination_offset, size);
  });
}

void WebGPUCommandList::Execute(WGPUDevice device, WGPUQueue queue,
                                WGPUBindGroupLayout constants_layout, uint32_t binding_count) {
  Encoder encoder{device, wgpuDeviceCreateCommandEncoder(device, nullptr), nullptr};
  WGPUBuffer uniform_buffer = nullptr;
  if (!uniforms_.empty()) {
    // Every binding's window must fit from any offset.
    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    desc.size = rex::align(uint64_t(uniforms_.size()), uint64_t(4)) + kUniformBindingSize;
    desc.usage = WGPUBufferUsage_Uniform;
    desc.mappedAtCreation = true;
    uniform_buffer = wgpuDeviceCreateBuffer(device, &desc);
    void* mapping = wgpuBufferGetMappedRange(uniform_buffer, 0, desc.size);
    if (mapping) {
      std::memcpy(mapping, uniforms_.data(), uniforms_.size());
    }
    wgpuBufferUnmap(uniform_buffer);
    std::vector<WGPUBindGroupEntry> entries(binding_count, WGPU_BIND_GROUP_ENTRY_INIT);
    for (uint32_t i = 0; i < binding_count; ++i) {
      entries[i].binding = i;
      entries[i].buffer = uniform_buffer;
      entries[i].size = kUniformBindingSize;
    }
    WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    group_desc.layout = constants_layout;
    group_desc.entryCount = binding_count;
    group_desc.entries = entries.data();
    encoder.constants = wgpuDeviceCreateBindGroup(device, &group_desc);
  }
  if (upload_size_) {
    // The used part of the last chunk is enough.
    uint64_t staging_size = chunks_.back().offset + chunks_.back().used;
    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    desc.size = staging_size;
    desc.usage = WGPUBufferUsage_CopySrc | WGPUBufferUsage_Index;
    desc.mappedAtCreation = true;
    encoder.staging = wgpuDeviceCreateBuffer(device, &desc);
    auto* mapping =
        static_cast<uint8_t*>(wgpuBufferGetMappedRange(encoder.staging, 0, staging_size));
    if (mapping) {
      for (const Chunk& chunk : chunks_) {
        std::memcpy(mapping + chunk.offset, chunk.data.get(), chunk.used);
      }
    }
    wgpuBufferUnmap(encoder.staging);
  }
  for (Command& command : commands_) {
    command(encoder);
  }
  if (encoder.pass) {
    wgpuRenderPassEncoderEnd(encoder.pass);
    wgpuRenderPassEncoderRelease(encoder.pass);
    encoder.pass = nullptr;
  }
  WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder.encoder, nullptr);
  wgpuQueueSubmit(queue, 1, &commands);
  wgpuCommandBufferRelease(commands);
  wgpuCommandEncoderRelease(encoder.encoder);
  // Kept alive by the submission until the GPU is done with it.
  if (encoder.staging) {
    wgpuBufferRelease(encoder.staging);
  }
  if (encoder.constants) {
    wgpuBindGroupRelease(encoder.constants);
  }
  if (uniform_buffer) {
    wgpuBufferRelease(uniform_buffer);
  }
  commands_.clear();
  uniforms_.clear();
  chunks_.clear();
  upload_size_ = 0;
  upload_bytes_ = 0;
}

}  // namespace rex::graphics::webgpu
