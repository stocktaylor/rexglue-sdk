/**
 * @file        graphics/webgpu/command_processor.h
 * @brief       Command processor for the WebGPU GPU backend
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/pipeline/shader/wgsl.h>
#include <rex/graphics/webgpu/command_list.h>
#include <rex/graphics/webgpu/primitive_processor.h>
#include <rex/graphics/webgpu/shared_memory.h>
#include <rex/string/buffer.h>
#include <rex/ui/webgpu/device_thread.h>

namespace rex::graphics::webgpu {

class WebGPUGraphicsSystem;

// A port of the Vulkan backend in progress (docs/WASM.md, phase 5). Runs on
// the GPU worker thread and records into a WebGPUCommandList; the device
// thread executes each list, one submission per guest frame for now.
class WebGPUCommandProcessor : public CommandProcessor {
 public:
  WebGPUCommandProcessor(WebGPUGraphicsSystem* graphics_system, system::KernelState* kernel_state);
  ~WebGPUCommandProcessor() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

  ui::webgpu::WebGPUDeviceThread& device_thread() const { return *device_thread_; }
  WebGPUCommandList& command_list() { return *command_list_; }

  // Ends the render pass the command list has open, if any (before copies).
  void EndRenderPass();

  // Hands the command list to the device thread and starts a new one.
  void Flush();
  // Waits until at most `max_pending` submissions are executing.
  void AwaitSubmissions(uint64_t max_pending);

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;

  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;

 private:
  bool RequestVertexBuffers(const WgslShader& vertex_shader);

  ui::webgpu::WebGPUDeviceThread* device_thread_ = nullptr;
  std::unique_ptr<WebGPUCommandList> command_list_;
  bool render_pass_open_ = false;

  // Submissions handed to the device thread, and finished on the GPU.
  uint64_t submissions_flushed_ = 0;
  std::atomic<uint64_t> submissions_completed_{0};
  std::mutex submission_mutex_;
  std::condition_variable submission_completed_;

  std::unique_ptr<WebGPUSharedMemory> shared_memory_;
  std::unique_ptr<WebGPUPrimitiveProcessor> primitive_processor_;

  // Bring-up statistics, logged every kStatisticsFrames frames.
  static constexpr uint32_t kStatisticsFrames = 300;
  uint32_t statistics_frames_ = 0;
  uint64_t statistics_draws_ = 0;
  uint64_t statistics_upload_bytes_ = 0;

  std::unordered_map<uint64_t, std::unique_ptr<WgslShader>> shaders_;
  string::StringBuffer ucode_disasm_buffer_;
};

}  // namespace rex::graphics::webgpu
