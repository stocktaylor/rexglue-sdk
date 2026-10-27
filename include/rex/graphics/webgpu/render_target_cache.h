/**
 * @file        graphics/webgpu/render_target_cache.h
 * @brief       Host render targets for the WebGPU backend (minimal)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>

#include <webgpu/webgpu.h>

#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::webgpu {

class WebGPUCommandProcessor;
class WebGPUSharedMemory;
class WebGPUTextureCache;

// For bring-up, a far simpler model than RenderTargetCache's EDRAM emulation:
// one host texture per EDRAM surface (base tile, pitch, format), as tall as
// the EDRAM allows, never sharing contents with others at overlapping
// addresses. No MSAA or format reinterpretation.
//
// Resolves work as the Vulkan backend's host render target path: the resolved
// area of the source render target is dumped into an EDRAM buffer in the
// guest's layout and encoding (internal WGSL below), from which the shared
// resolve copy shaders write the destination texture to shared memory.
class WebGPURenderTargetCache {
 public:
  static constexpr uint32_t kMaxHeight = 2048;

  struct RenderTarget {
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;
    // Depth render targets only: single-aspect views for resolves.
    WGPUTextureView depth_view = nullptr;
    WGPUTextureView stencil_view = nullptr;
    WGPUTextureFormat format = WGPUTextureFormat_Undefined;
    uint32_t width = 0;
    uint32_t height = 0;
    bool is_depth = false;
  };

  explicit WebGPURenderTargetCache(WebGPUCommandProcessor& command_processor)
      : command_processor_(command_processor) {}
  ~WebGPURenderTargetCache();

  bool Initialize();

  // The host format for a guest format: 8-bit gamma as 8-bit UNORM (the
  // shader converts), 16-bit fixed and 2:10:10:10 float as 16-bit float.
  static WGPUTextureFormat GetColorFormat(xenos::ColorRenderTargetFormat format);
  static WGPUTextureFormat GetDepthFormat(xenos::DepthRenderTargetFormat format);

  RenderTarget* GetColor(uint32_t base_tiles, uint32_t pitch,
                         xenos::ColorRenderTargetFormat format);
  RenderTarget* GetDepth(uint32_t base_tiles, uint32_t pitch,
                         xenos::DepthRenderTargetFormat format);

  // Records copying the resolve area of `source` (the color or depth render
  // target the resolve reads) to its destination in shared memory. Clears are
  // separate.
  void ResolveCopy(const draw_util::ResolveInfo& resolve_info, RenderTarget& source,
                   WebGPUSharedMemory& shared_memory, WebGPUTextureCache& texture_cache);

  void Shutdown();

 private:
  RenderTarget* GetOrCreate(uint64_t key, uint32_t pitch, WGPUTextureFormat format, bool is_depth);

  WebGPUCommandProcessor& command_processor_;
  std::unordered_map<uint64_t, std::unique_ptr<RenderTarget>> render_targets_;

  // Device objects, created in Initialize.
  WGPUBuffer edram_buffer_ = nullptr;
  WGPUBindGroupLayout dump_layout_ = nullptr;
  WGPUComputePipeline dump_pipeline_ = nullptr;
  WGPUBindGroupLayout depth_dump_layout_ = nullptr;
  WGPUComputePipeline depth_dump_pipeline_ = nullptr;
  std::array<WGPUComputePipeline, size_t(draw_util::ResolveCopyShaderIndex::kCount)>
      copy_pipelines_{};
  uint32_t min_storage_offset_alignment_ = 256;
};

}  // namespace rex::graphics::webgpu
