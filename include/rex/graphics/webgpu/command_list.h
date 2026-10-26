/**
 * @file        graphics/webgpu/command_list.h
 * @brief       GPU work recorded off the device thread, replayed on it
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <webgpu/webgpu.h>

namespace rex::graphics::webgpu {

// Every WebGPU call happens on the device thread (ui/webgpu/device_thread.h),
// so the command processor records a submission's work here, on its own
// thread, and hands the list over at the end (WebGPUCommandProcessor::Flush).
//
// Uploads go to an arena that becomes one staging buffer (copy source and
// index usage) when the list executes: recorded copies out of it run in order
// with the other commands (unlike wgpuQueueWriteBuffer, which takes effect
// before the whole submission), and converted index buffers are drawn from it
// directly. Guest constants go to a second arena, which becomes the
// submission's uniform buffer, bound through one bind group with dynamic
// offsets (WgslShaderTranslator's @group(1)).
class WebGPUCommandList {
 public:
  // What commands record into, on the device thread.
  struct Encoder {
    WGPUDevice device;
    WGPUCommandEncoder encoder;
    // The submission's staging buffer, or nullptr if nothing was uploaded.
    WGPUBuffer staging;
    // The open render pass, if any.
    WGPURenderPassEncoder pass = nullptr;
    // @group(1) over the submission's uniform buffer, or nullptr if no
    // constants were written.
    WGPUBindGroup constants = nullptr;
  };
  using Command = std::function<void(Encoder& encoder)>;

  static constexpr size_t kUploadAlignment = 16;
  // The largest minUniformBufferOffsetAlignment WebGPU allows.
  static constexpr size_t kUniformAlignment = 256;
  // Bindings of the constants bind group, all windows of this size into the
  // uniform buffer (the largest constant buffer, 256 float4 constants).
  static constexpr uint32_t kUniformBindingSize = 4096;

  WebGPUCommandList() = default;
  WebGPUCommandList(const WebGPUCommandList&) = delete;
  WebGPUCommandList& operator=(const WebGPUCommandList&) = delete;

  bool empty() const { return commands_.empty(); }
  // Bytes of upload data requested so far.
  uint64_t upload_bytes() const { return upload_bytes_; }

  void Record(Command command) { commands_.push_back(std::move(command)); }

  // Memory for `size` bytes of upload data, valid until the list executes;
  // offset_out is its offset in the staging buffer.
  void* AllocateUpload(size_t size, uint64_t& offset_out);

  // Memory for `size` bytes of constants, valid until the next allocation;
  // offset_out is their (dynamic) offset in the uniform buffer.
  void* AllocateUniforms(size_t size, uint32_t& offset_out);

  // Records a copy from the staging buffer.
  void RecordUploadCopy(uint64_t staging_offset, WGPUBuffer destination,
                        uint64_t destination_offset, uint64_t size);

  // On the device thread: creates the staging and uniform buffers and the
  // constants bind group (with constants_layout, binding_count bindings),
  // replays the commands and submits. Ends a render pass left open.
  void Execute(WGPUDevice device, WGPUQueue queue, WGPUBindGroupLayout constants_layout,
               uint32_t binding_count);

 private:
  struct Chunk {
    std::unique_ptr<uint8_t[]> data;
    uint64_t offset;
    size_t size;
    size_t used;
  };
  static constexpr size_t kChunkSize = size_t(4) << 20;

  std::vector<Command> commands_;
  std::vector<uint8_t> uniforms_;
  std::vector<Chunk> chunks_;
  uint64_t upload_size_ = 0;
  uint64_t upload_bytes_ = 0;
};

}  // namespace rex::graphics::webgpu
