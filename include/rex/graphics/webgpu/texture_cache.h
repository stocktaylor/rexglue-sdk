/**
 * @file        graphics/webgpu/texture_cache.h
 * @brief       Guest textures for the WebGPU backend
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

#include <rex/graphics/pipeline/shader/wgsl.h>
#include <rex/graphics/pipeline/texture/cache.h>

namespace rex::graphics::webgpu {

class WebGPUCommandProcessor;
class WebGPUSharedMemory;

// Like VulkanTextureCache: guest data is untiled and converted by the internal
// load compute shaders (src/graphics/shaders/webgpu_wgsl/texture_load_*) into
// a buffer, then copied into the texture. WebGPU differences:
// - No packed 16-bit formats, 16-bit unorm/snorm (without an optional feature)
//   or 4:2:2 formats: converted to 8-bit or float formats where load shaders
//   exist, unsupported otherwise.
// - Views can't reinterpret unorm as snorm, so signed versions are separate
//   textures, and views can't swizzle (the shader does; GetHostFormatSwizzle).
// - Block-compressed formats need texture-compression-bc, and 32-bit float
//   ones float32-filterable (they'd otherwise need a non-filtering layout).
// - Loads and copies run in order with the rest of the command list.
// No resolution scaling yet.
class WebGPUTextureCache final : public TextureCache {
 public:
  WebGPUTextureCache(const RegisterFile& register_file, WebGPUSharedMemory& shared_memory,
                     WebGPUCommandProcessor& command_processor);
  ~WebGPUTextureCache() override;

  bool Initialize();
  void Shutdown();

  void RequestTextures(uint32_t used_texture_mask) override;

  // The view for a shader texture binding, or a null (all-zero) view of the
  // binding's dimension.
  WGPUTextureView GetActiveBindingOrNullView(uint32_t fetch_constant_index,
                                             xenos::FetchOpDimension dimension, bool is_signed);
  WGPUSampler GetSampler(const WgslShader::SamplerBinding& binding);

 protected:
  bool IsSignedVersionSeparateForFormat(TextureKey key) const override;
  uint32_t GetHostFormatSwizzle(TextureKey key) const override;
  uint32_t GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const override;
  uint32_t GetMaxHostTextureDepthOrArraySize(xenos::DataDimension dimension) const override;
  std::unique_ptr<Texture> CreateTexture(TextureKey key) override;
  bool LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                             bool load_mips) override;

 private:
  struct HostFormat {
    LoadShaderIndex load_shader = kLoadShaderIndexUnknown;
    WGPUTextureFormat format = WGPUTextureFormat_Undefined;
    // On the host too (the host block is the guest block), not decompressed.
    bool block_compressed = false;
  };
  struct HostFormatPair {
    HostFormat format_unsigned;
    HostFormat format_signed;
    // Xenos swizzle components to host format components.
    uint32_t swizzle;
  };

  class WebGPUTexture final : public Texture {
   public:
    WebGPUTexture(WebGPUTextureCache& texture_cache, const TextureKey& key, WGPUTexture texture,
                  WGPUTextureView view, uint64_t host_memory_usage);
    ~WebGPUTexture() override;
    WGPUTexture texture() const { return texture_; }
    WGPUTextureView view() const { return view_; }

   private:
    WGPUTexture texture_;
    // Of the texture's dimension: 2D array, 3D or cube.
    WGPUTextureView view_;
  };

  static const HostFormatPair kBestHostFormats[64];
  static const HostFormatPair kHostFormatGBGRUnaligned;
  static const HostFormatPair kHostFormatBGRGUnaligned;
  static const HostFormatPair kHostFormatDXT1Unaligned;
  static const HostFormatPair kHostFormatDXT2_3Unaligned;
  static const HostFormatPair kHostFormatDXT4_5Unaligned;
  static const HostFormatPair kHostFormatDXNUnaligned;
  static const HostFormatPair kHostFormatDXT5AUnaligned;

  const HostFormatPair& GetHostFormatPair(TextureKey key) const;
  // The host format a texture with this key holds.
  const HostFormat& GetTextureHostFormat(TextureKey key) const;

  WebGPUCommandProcessor& command_processor_;
  WebGPUSharedMemory& webgpu_shared_memory_;
  HostFormatPair host_formats_[64];
  bool bc_supported_ = false;
  bool unsupported_format_logged_[64] = {};
  WGPULimits limits_ = WGPU_LIMITS_INIT;

  std::array<WGPUComputePipeline, kLoadShaderCount> load_pipelines_{};
  // Null views: 2D array, 3D, cube.
  WGPUTexture null_textures_[3] = {};
  WGPUTextureView null_views_[3] = {};
  std::unordered_map<uint64_t, WGPUSampler> samplers_;
};

}  // namespace rex::graphics::webgpu
