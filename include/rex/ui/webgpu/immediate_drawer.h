/**
 * @file        ui/webgpu/immediate_drawer.h
 * @brief       WebGPU immediate drawer (ImGui and other UI overlays)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <vector>

#include <webgpu/webgpu.h>

#include <rex/ui/immediate_drawer.h>
#include <rex/ui/webgpu/device_thread.h>

namespace rex::ui::webgpu {

// Draws into the presenter's render pass (WebGPUPresenter::WebGPUUIDrawContext),
// on the device thread, where the presenter runs the UI drawers. Textures are
// created through the device thread from any thread and released on it.
// Vertices are transformed to clip space on the CPU while uploading, so there
// are no per-Begin uniforms; vertex and index data for a frame go into growing
// buffers through the queue, whose writes WebGPU orders before the paint
// submission that reads them.
class WebGPUImmediateDrawer : public ImmediateDrawer {
 public:
  static std::unique_ptr<WebGPUImmediateDrawer> Create(WebGPUDeviceThread& device_thread);
  ~WebGPUImmediateDrawer() override;

  std::unique_ptr<ImmediateTexture> CreateTexture(uint32_t width, uint32_t height,
                                                  ImmediateTextureFilter filter, bool is_repeated,
                                                  const uint8_t* data) override;

  void Begin(UIDrawContext& ui_draw_context, float coordinate_space_width,
             float coordinate_space_height) override;
  void BeginDrawBatch(const ImmediateDrawBatch& batch) override;
  void Draw(const ImmediateDraw& draw) override;
  void EndDrawBatch() override;
  void End() override;

 private:
  class WebGPUImmediateTexture;

  explicit WebGPUImmediateDrawer(WebGPUDeviceThread& device_thread)
      : device_thread_(device_thread) {}
  // These run on the device thread.
  WebGPUDevice& device() const { return device_thread_.device(); }
  bool Initialize();
  void Shutdown();
  std::unique_ptr<ImmediateTexture> CreateTextureOnDeviceThread(uint32_t width, uint32_t height,
                                                                ImmediateTextureFilter filter,
                                                                bool is_repeated,
                                                                const uint8_t* data);
  WGPUSampler GetSampler(ImmediateTextureFilter filter, bool is_repeated);
  WGPURenderPipeline GetPipeline(ImmediatePrimitiveType primitive_type, WGPUTextureFormat format);
  // Appends size bytes (padded to 4) to a growing per-frame buffer.
  uint64_t Upload(WGPUBuffer& buffer, uint64_t& capacity, uint64_t& used, WGPUBufferUsage usage,
                  const void* data, uint64_t size);

  WebGPUDeviceThread& device_thread_;

  WGPUShaderModule shader_ = nullptr;
  WGPUBindGroupLayout texture_layout_ = nullptr;
  WGPUPipelineLayout pipeline_layout_ = nullptr;
  WGPUSampler samplers_[4] = {};          // [filter][repeat]
  WGPURenderPipeline pipelines_[2] = {};  // [primitive type], for pipeline_format_
  WGPUTextureFormat pipeline_format_ = WGPUTextureFormat_Undefined;
  std::unique_ptr<ImmediateTexture> white_texture_;

  WGPUBuffer vertex_buffer_ = nullptr;
  uint64_t vertex_capacity_ = 0;
  uint64_t vertex_used_ = 0;
  WGPUBuffer index_buffer_ = nullptr;
  uint64_t index_capacity_ = 0;
  uint64_t index_used_ = 0;

  // Between Begin and End.
  WGPURenderPassEncoder pass_ = nullptr;
  WGPUTextureFormat format_ = WGPUTextureFormat_Undefined;
  std::vector<ImmediateVertex> transformed_;
  // Current batch.
  uint64_t batch_vertex_offset_ = 0;
  uint64_t batch_index_offset_ = 0;
  bool batch_indexed_ = false;
  bool batch_valid_ = false;
};

}  // namespace rex::ui::webgpu
