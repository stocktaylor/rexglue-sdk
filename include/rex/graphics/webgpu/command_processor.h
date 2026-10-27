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
#include <vector>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/pipeline/shader/wgsl.h>
#include <rex/graphics/pipeline/shader/wgsl_translator.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/webgpu/command_list.h>
#include <rex/graphics/webgpu/pipeline_cache.h>
#include <rex/graphics/webgpu/primitive_processor.h>
#include <rex/graphics/webgpu/render_target_cache.h>
#include <rex/graphics/webgpu/shared_memory.h>
#include <rex/graphics/webgpu/texture_cache.h>
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

  // Runs `release` on the device thread after the commands recorded so far
  // (with no command list, after the work already handed over).
  void ReleaseAfterRecordedCommands(std::function<void()> release);

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  void WriteRegister(uint32_t index, uint32_t value) override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;

  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;

 private:
  using RenderTarget = WebGPURenderTargetCache::RenderTarget;

  bool RequestVertexBuffers(const WgslShader& vertex_shader);
  // Opens a render pass on these targets unless one is open on them.
  void BeginRenderPass(RenderTarget* const* colors, RenderTarget* depth);
  void ClearRenderTarget(RenderTarget* render_target, const double color[4], float depth,
                         uint32_t stencil);
  void UpdateSystemConstants(
      const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
      bool primitive_polygonal, const draw_util::ViewportInfo& viewport_info);
  // Writes a constant block to the uniform arena, or reuses the previous one
  // if it's the same within the submission; returns its dynamic offset.
  uint32_t WriteConstants(uint32_t buffer, const void* data, size_t size);

  ui::webgpu::WebGPUDeviceThread* device_thread_ = nullptr;
  std::unique_ptr<WebGPUCommandList> command_list_;

  // The render pass the command list has open.
  bool render_pass_open_ = false;
  RenderTarget* pass_colors_[xenos::kMaxColorRenderTargets] = {};
  RenderTarget* pass_depth_ = nullptr;

  // Submissions handed to the device thread, and finished on the GPU.
  uint64_t submissions_flushed_ = 0;
  std::atomic<uint64_t> submissions_completed_{0};
  std::mutex submission_mutex_;
  std::condition_variable submission_completed_;

  std::unique_ptr<WebGPUSharedMemory> shared_memory_;
  std::unique_ptr<WebGPUPrimitiveProcessor> primitive_processor_;
  std::unique_ptr<WebGPUPipelineCache> pipeline_cache_;
  std::unique_ptr<WebGPURenderTargetCache> render_target_cache_;
  std::unique_ptr<WebGPUTextureCache> texture_cache_;
  WGPUBindGroup shared_memory_bind_group_ = nullptr;

  WgslShaderTranslator::SystemConstants system_constants_;
  // The last block of each constant buffer in this submission.
  std::vector<uint8_t> last_constants_[WgslShaderTranslator::kConstantBufferCount];
  uint32_t last_constants_offsets_[WgslShaderTranslator::kConstantBufferCount] = {};
  bool last_constants_valid_[WgslShaderTranslator::kConstantBufferCount] = {};
  std::vector<uint8_t> float_constants_;

  // Device thread only: the swap's gamma ramp pipeline, created on first use.
  bool EnsureSwapPipeline();
  struct SwapRamps {
    uint32_t table[256];
    uint32_t pwl[128 * 3];
    uint32_t use_pwl[4];
  };
  WGPURenderPipeline swap_pipeline_ = nullptr;

  // The last resolved color area, copied out at resolve time (resolves often
  // clear the source right after), shown by the swap until resolves write
  // memory and the swap reads the frontbuffer.
  WGPUTexture resolved_texture_ = nullptr;
  WGPUTextureFormat resolved_format_ = WGPUTextureFormat_Undefined;
  uint32_t resolved_width_ = 0;
  uint32_t resolved_height_ = 0;

  // Bring-up statistics, logged every kStatisticsFrames frames.
  static constexpr uint32_t kStatisticsFrames = 300;
  uint32_t statistics_frames_ = 0;
  uint64_t statistics_draws_ = 0;
  uint64_t statistics_draws_done_ = 0;
  uint64_t statistics_resolves_ = 0;
  uint64_t statistics_upload_bytes_ = 0;

  std::unordered_map<uint64_t, std::unique_ptr<WgslShader>> shaders_;
};

}  // namespace rex::graphics::webgpu
