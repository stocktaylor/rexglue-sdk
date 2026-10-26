/**
 * @file        graphics/webgpu/primitive_processor.h
 * @brief       Primitive and index buffer conversion for WebGPU
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include <webgpu/webgpu.h>

#include <rex/graphics/primitive_processor.h>

namespace rex::graphics::webgpu {

class WebGPUCommandProcessor;

// WebGPU has full 32-bit indices but no triangle fans, line loops, quads,
// geometry shaders or point sizes: those are converted, and points and
// rectangles expanded in the vertex shader. Converted indices live in the
// current submission's staging buffer.
class WebGPUPrimitiveProcessor final : public PrimitiveProcessor {
 public:
  WebGPUPrimitiveProcessor(const RegisterFile& register_file, memory::Memory& memory,
                           SharedMemory& shared_memory, WebGPUCommandProcessor& command_processor)
      : PrimitiveProcessor(register_file, memory, shared_memory),
        command_processor_(command_processor) {}
  ~WebGPUPrimitiveProcessor();

  bool Initialize();
  void Shutdown(bool from_destructor = false);

  // Converted index buffers are in the submission's staging buffer, so the
  // per-frame conversion cache can only live as long as the submission.
  void EndSubmission() {
    ClearPerFrameCache();
    converted_index_buffer_offsets_.clear();
  }

  std::pair<WGPUBuffer, uint64_t> GetBuiltinIndexBuffer(size_t handle) const {
    return {builtin_index_buffer_, GetBuiltinIndexBufferOffsetBytes(handle)};
  }
  // In the submission's staging buffer.
  uint64_t GetConvertedIndexBufferOffset(size_t handle) const {
    return converted_index_buffer_offsets_[handle];
  }

 protected:
  bool InitializeBuiltinIndexBuffer(size_t size_bytes,
                                    std::function<void(void*)> fill_callback) override;
  void* RequestHostConvertedIndexBufferForCurrentFrame(xenos::IndexFormat format,
                                                       uint32_t index_count, bool coalign_for_simd,
                                                       uint32_t coalignment_original_address,
                                                       size_t& backend_handle_out) override;

 private:
  WebGPUCommandProcessor& command_processor_;
  WGPUBuffer builtin_index_buffer_ = nullptr;
  std::vector<uint64_t> converted_index_buffer_offsets_;
};

}  // namespace rex::graphics::webgpu
