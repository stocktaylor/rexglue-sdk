/**
 * @file        graphics/webgpu/render_target_cache.cpp
 * @brief       Host render targets for the WebGPU backend (minimal)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/render_target_cache.h>

#include <algorithm>

#include <rex/graphics/webgpu/command_processor.h>
#include <rex/logging.h>

namespace rex::graphics::webgpu {

WebGPURenderTargetCache::~WebGPURenderTargetCache() {
  Shutdown();
}

WGPUTextureFormat WebGPURenderTargetCache::GetColorFormat(xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return WGPUTextureFormat_RGBA8Unorm;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return WGPUTextureFormat_RGB10A2Unorm;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return WGPUTextureFormat_R32Float;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return WGPUTextureFormat_RG32Float;
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return WGPUTextureFormat_RG16Float;
    default:
      return WGPUTextureFormat_RGBA16Float;
  }
}

WGPUTextureFormat WebGPURenderTargetCache::GetDepthFormat(xenos::DepthRenderTargetFormat format) {
  (void)format;
  // Float24 needs the pixel shader conversion with a 32-bit float format
  // (depth32float-stencil8 is optional); unorm is enough to start.
  return WGPUTextureFormat_Depth24PlusStencil8;
}

WebGPURenderTargetCache::RenderTarget* WebGPURenderTargetCache::GetColor(
    uint32_t base_tiles, uint32_t pitch, xenos::ColorRenderTargetFormat format) {
  WGPUTextureFormat host_format = GetColorFormat(format);
  uint64_t key = uint64_t(base_tiles) | (uint64_t(pitch) << 12) | (uint64_t(host_format) << 32);
  return GetOrCreate(key, pitch, host_format, false);
}

WebGPURenderTargetCache::RenderTarget* WebGPURenderTargetCache::GetDepth(
    uint32_t base_tiles, uint32_t pitch, xenos::DepthRenderTargetFormat format) {
  WGPUTextureFormat host_format = GetDepthFormat(format);
  uint64_t key = uint64_t(base_tiles) | (uint64_t(pitch) << 12) | (uint64_t(host_format) << 32) |
                 (uint64_t(1) << 63);
  return GetOrCreate(key, pitch, host_format, true);
}

WebGPURenderTargetCache::RenderTarget* WebGPURenderTargetCache::GetOrCreate(
    uint64_t key, uint32_t pitch, WGPUTextureFormat format, bool is_depth) {
  auto it = render_targets_.find(key);
  if (it != render_targets_.end()) {
    return it->second.get();
  }
  auto render_target = std::make_unique<RenderTarget>();
  render_target->format = format;
  render_target->width = std::clamp(pitch, uint32_t(1), uint32_t(8192));
  render_target->height = kMaxHeight;
  render_target->is_depth = is_depth;
  command_processor_.device_thread().Call([&] {
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.label = {is_depth ? "Depth render target" : "Color render target", WGPU_STRLEN};
    desc.size = {render_target->width, render_target->height, 1};
    desc.format = format;
    desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding |
                 WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst;
    render_target->texture =
        wgpuDeviceCreateTexture(command_processor_.device_thread().device().device(), &desc);
    if (render_target->texture) {
      render_target->view = wgpuTextureCreateView(render_target->texture, nullptr);
    }
  });
  if (!render_target->view) {
    REXGPU_ERROR("WebGPU: couldn't create a {}x{} render target", render_target->width,
                 render_target->height);
    return nullptr;
  }
  REXGPU_INFO("WebGPU: {} render target at tile {}, {}x{}, host format {}",
              is_depth ? "depth" : "color", uint32_t(key & 0xFFF), render_target->width,
              render_target->height, uint32_t(format));
  RenderTarget* result = render_target.get();
  render_targets_.emplace(key, std::move(render_target));
  return result;
}

void WebGPURenderTargetCache::Shutdown() {
  last_resolved_ = nullptr;
  if (render_targets_.empty()) {
    return;
  }
  auto render_targets = std::make_shared<decltype(render_targets_)>(std::move(render_targets_));
  render_targets_.clear();
  command_processor_.device_thread().Post([render_targets] {
    for (auto& [key, render_target] : *render_targets) {
      wgpuTextureViewRelease(render_target->view);
      wgpuTextureRelease(render_target->texture);
    }
  });
}

}  // namespace rex::graphics::webgpu
