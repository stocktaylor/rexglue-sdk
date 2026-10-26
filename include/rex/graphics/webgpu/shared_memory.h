/**
 * @file        graphics/webgpu/shared_memory.h
 * @brief       Guest physical memory as a WebGPU storage buffer
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <utility>
#include <vector>

#include <webgpu/webgpu.h>

#include <rex/graphics/shared_memory.h>

namespace rex::graphics::webgpu {

class WebGPUCommandProcessor;

// One 512 MB buffer (WebGPU has no sparse buffers), filled on demand like the
// Vulkan one, through the command list's staging uploads. WebGPU orders and
// synchronizes buffer accesses itself, so there are no usage transitions.
class WebGPUSharedMemory : public SharedMemory {
 public:
  WebGPUSharedMemory(WebGPUCommandProcessor& command_processor, memory::Memory& memory)
      : SharedMemory(memory), command_processor_(command_processor) {}
  ~WebGPUSharedMemory() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);

  WGPUBuffer buffer() const { return buffer_; }

 protected:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override;

 private:
  WebGPUCommandProcessor& command_processor_;
  WGPUBuffer buffer_ = nullptr;
};

}  // namespace rex::graphics::webgpu
