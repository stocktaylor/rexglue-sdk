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

void WebGPUCommandList::RecordUploadCopy(uint64_t staging_offset, WGPUBuffer destination,
                                         uint64_t destination_offset, uint64_t size) {
  Record([=](Encoder& encoder) {
    wgpuCommandEncoderCopyBufferToBuffer(encoder.encoder, encoder.staging, staging_offset,
                                         destination, destination_offset, size);
  });
}

void WebGPUCommandList::Execute(WGPUDevice device, WGPUQueue queue) {
  Encoder encoder{device, wgpuDeviceCreateCommandEncoder(device, nullptr), nullptr};
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
  commands_.clear();
  chunks_.clear();
  upload_size_ = 0;
  upload_bytes_ = 0;
}

}  // namespace rex::graphics::webgpu
