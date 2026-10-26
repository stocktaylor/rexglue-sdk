/**
 * @file        graphics/webgpu/texture_cache.cpp
 * @brief       Guest textures for the WebGPU backend
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/texture_cache.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include <rex/assert.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/webgpu/command_processor.h>
#include <rex/graphics/webgpu/shared_memory.h>
#include <rex/logging.h>
#include <rex/math.h>

namespace rex::graphics::webgpu {

namespace shaders {
#include "../shaders/webgpu_wgsl/texture_load_128bpb_cs.h"
#include "../shaders/webgpu_wgsl/texture_load_16bpb_cs.h"
#include "../shaders/webgpu_wgsl/texture_load_32bpb_cs.h"
#include "../shaders/webgpu_wgsl/texture_load_64bpb_cs.h"
#include "../shaders/webgpu_wgsl/texture_load_8bpb_cs.h"
#include "../shaders/webgpu_wgsl/texture_load_bgrg8_rgb8_cs.h"
#include "../shaders/webgpu_wgsl/texture_load_ctx1_cs.h"
#include "../shaders/webgpu_wgsl/texture_load_depth_float_cs.h"
#include "../shaders/webgpu_wgsl/texture_load_depth_unorm_cs.h"
#include "../shaders/webgpu_wgsl/texture_load_dxn_rg8_cs.h"
#include "../shaders/webgpu_wgsl/texture_load_dxt1_rgba8_cs.h"
#include "../shaders/webgpu_wgsl/texture_load_dxt3_rgba8_cs.h"
#include "../shaders/webgpu_wgsl/texture_load_dxt3a_cs.h"
#include "../shaders/webgpu_wgsl/texture_load_dxt5_rgba8_cs.h"
#include "../shaders/webgpu_wgsl/texture_load_dxt5a_r8_cs.h"
#include "../shaders/webgpu_wgsl/texture_load_gbgr8_rgb8_cs.h"
#include "../shaders/webgpu_wgsl/texture_load_r16_snorm_float_cs.h"
#include "../shaders/webgpu_wgsl/texture_load_r16_unorm_float_cs.h"
#include "../shaders/webgpu_wgsl/texture_load_r4g4b4a4_rgba8_cs.h"
#include "../shaders/webgpu_wgsl/texture_load_r5g6b5_rgba8_cs.h"
#include "../shaders/webgpu_wgsl/texture_load_rg16_snorm_float_cs.h"
#include "../shaders/webgpu_wgsl/texture_load_rg16_unorm_float_cs.h"
#include "../shaders/webgpu_wgsl/texture_load_rgba16_snorm_float_cs.h"
#include "../shaders/webgpu_wgsl/texture_load_rgba16_unorm_float_cs.h"
}  // namespace shaders

namespace {

WGPUStringView View(const char* s) {
  return {s, WGPU_STRLEN};
}

// The load shaders' push constants as a uniform buffer: LoadConstants is 40
// bytes, 48 with WGSL's struct alignment.
constexpr uint64_t kLoadConstantsBindingSize = 48;
constexpr uint64_t kLoadConstantsStride = 256;

}  // namespace

// Indexed by xenos::TextureFormat; see the class comment for the differences
// from VulkanTextureCache::kBestHostFormats.
const WebGPUTextureCache::HostFormatPair WebGPUTextureCache::kBestHostFormats[64] = {
    // k_1_REVERSE, k_1
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_8
    {{kLoadShaderIndex8bpb, WGPUTextureFormat_R8Unorm},
     {kLoadShaderIndex8bpb, WGPUTextureFormat_R8Snorm},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_1_5_5_5: no 5:5:5:1 format, and no load shader to 8-bit yet.
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_5_6_5
    {{kLoadShaderIndexR5G6B5ToRGBA8, WGPUTextureFormat_RGBA8Unorm},
     {},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_6_5_5: no load shader to 8-bit yet.
    {{}, {}, XE_GPU_MAKE_TEXTURE_SWIZZLE(R, B, G, G)},
    // k_8_8_8_8
    {{kLoadShaderIndex32bpb, WGPUTextureFormat_RGBA8Unorm},
     {kLoadShaderIndex32bpb, WGPUTextureFormat_RGBA8Snorm},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_2_10_10_10
    {{kLoadShaderIndex32bpb, WGPUTextureFormat_RGB10A2Unorm},
     {},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_8_A
    {{kLoadShaderIndex8bpb, WGPUTextureFormat_R8Unorm},
     {kLoadShaderIndex8bpb, WGPUTextureFormat_R8Snorm},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_8_B
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_8_8
    {{kLoadShaderIndex16bpb, WGPUTextureFormat_RG8Unorm},
     {kLoadShaderIndex16bpb, WGPUTextureFormat_RG8Snorm},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_Cr_Y1_Cb_Y0_REP, k_Y1_Cr_Y0_Cb_REP: no 4:2:2 formats.
    {{kLoadShaderIndexGBGR8ToRGB8, WGPUTextureFormat_RGBA8Unorm},
     {},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    {{kLoadShaderIndexBGRG8ToRGB8, WGPUTextureFormat_RGBA8Unorm},
     {},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_16_16_EDRAM, k_8_8_8_8_A
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_4_4_4_4
    {{kLoadShaderIndexRGBA4ToRGBA8, WGPUTextureFormat_RGBA8Unorm},
     {},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_10_11_11, k_11_11_10: need the two-pass float conversion.
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_DXT1, k_DXT2_3, k_DXT4_5
    {{kLoadShaderIndex64bpb, WGPUTextureFormat_BC1RGBAUnorm, true},
     {},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    {{kLoadShaderIndex128bpb, WGPUTextureFormat_BC2RGBAUnorm, true},
     {},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    {{kLoadShaderIndex128bpb, WGPUTextureFormat_BC3RGBAUnorm, true},
     {},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_16_16_16_16_EDRAM
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_24_8, k_24_8_FLOAT
    {{kLoadShaderIndexDepthUnorm, WGPUTextureFormat_R32Float},
     {kLoadShaderIndexDepthUnorm, WGPUTextureFormat_R32Float},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    {{kLoadShaderIndexDepthFloat, WGPUTextureFormat_R32Float},
     {kLoadShaderIndexDepthFloat, WGPUTextureFormat_R32Float},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_16, k_16_16, k_16_16_16_16: to float (no 16-bit unorm/snorm formats).
    {{kLoadShaderIndexR16UNormToFloat, WGPUTextureFormat_R16Float},
     {kLoadShaderIndexR16SNormToFloat, WGPUTextureFormat_R16Float},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    {{kLoadShaderIndexRG16UNormToFloat, WGPUTextureFormat_RG16Float},
     {kLoadShaderIndexRG16SNormToFloat, WGPUTextureFormat_RG16Float},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    {{kLoadShaderIndexRGBA16UNormToFloat, WGPUTextureFormat_RGBA16Float},
     {kLoadShaderIndexRGBA16SNormToFloat, WGPUTextureFormat_RGBA16Float},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_16_EXPAND, k_16_16_EXPAND, k_16_16_16_16_EXPAND
    {{kLoadShaderIndex16bpb, WGPUTextureFormat_R16Float},
     {kLoadShaderIndex16bpb, WGPUTextureFormat_R16Float},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    {{kLoadShaderIndex32bpb, WGPUTextureFormat_RG16Float},
     {kLoadShaderIndex32bpb, WGPUTextureFormat_RG16Float},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    {{kLoadShaderIndex64bpb, WGPUTextureFormat_RGBA16Float},
     {kLoadShaderIndex64bpb, WGPUTextureFormat_RGBA16Float},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_16_FLOAT, k_16_16_FLOAT, k_16_16_16_16_FLOAT
    {{kLoadShaderIndex16bpb, WGPUTextureFormat_R16Float},
     {kLoadShaderIndex16bpb, WGPUTextureFormat_R16Float},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    {{kLoadShaderIndex32bpb, WGPUTextureFormat_RG16Float},
     {kLoadShaderIndex32bpb, WGPUTextureFormat_RG16Float},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    {{kLoadShaderIndex64bpb, WGPUTextureFormat_RGBA16Float},
     {kLoadShaderIndex64bpb, WGPUTextureFormat_RGBA16Float},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_32, k_32_32, k_32_32_32_32
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_32_FLOAT, k_32_32_FLOAT, k_32_32_32_32_FLOAT
    {{kLoadShaderIndex32bpb, WGPUTextureFormat_R32Float},
     {kLoadShaderIndex32bpb, WGPUTextureFormat_R32Float},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    {{kLoadShaderIndex64bpb, WGPUTextureFormat_RG32Float},
     {kLoadShaderIndex64bpb, WGPUTextureFormat_RG32Float},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    {{kLoadShaderIndex128bpb, WGPUTextureFormat_RGBA32Float},
     {kLoadShaderIndex128bpb, WGPUTextureFormat_RGBA32Float},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_32_AS_8, k_32_AS_8_8, k_16_MPEG, k_16_16_MPEG, k_8_INTERLACED,
    // k_32_AS_8_INTERLACED, k_32_AS_8_8_INTERLACED, k_16_INTERLACED,
    // k_16_MPEG_INTERLACED, k_16_16_MPEG_INTERLACED
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_DXN
    {{kLoadShaderIndex128bpb, WGPUTextureFormat_BC5RGUnorm, true},
     {},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_8_8_8_8_AS_16_16_16_16
    {{kLoadShaderIndex32bpb, WGPUTextureFormat_RGBA8Unorm},
     {kLoadShaderIndex32bpb, WGPUTextureFormat_RGBA8Snorm},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_DXT1_AS_16_16_16_16, k_DXT2_3_AS_16_16_16_16, k_DXT4_5_AS_16_16_16_16
    {{kLoadShaderIndex64bpb, WGPUTextureFormat_BC1RGBAUnorm, true},
     {},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    {{kLoadShaderIndex128bpb, WGPUTextureFormat_BC2RGBAUnorm, true},
     {},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    {{kLoadShaderIndex128bpb, WGPUTextureFormat_BC3RGBAUnorm, true},
     {},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_2_10_10_10_AS_16_16_16_16
    {{kLoadShaderIndex32bpb, WGPUTextureFormat_RGB10A2Unorm},
     {},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_10_11_11_AS_16_16_16_16, k_11_11_10_AS_16_16_16_16
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_32_32_32_FLOAT
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB},
    // k_DXT3A
    {{kLoadShaderIndexDXT3A, WGPUTextureFormat_R8Unorm}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_DXT5A
    {{kLoadShaderIndex64bpb, WGPUTextureFormat_BC4RUnorm, true},
     {},
     xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR},
    // k_CTX1
    {{kLoadShaderIndexCTX1, WGPUTextureFormat_RG8Unorm}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG},
    // k_DXT3A_AS_1_1_1_1: no 4:4:4:4 format.
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    // k_8_8_8_8_GAMMA_EDRAM, k_2_10_10_10_FLOAT_EDRAM
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
    {{}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA},
};

// Decompressed: unaligned sizes (WebGPU block-compressed textures must be
// whole blocks), or no texture-compression-bc.
const WebGPUTextureCache::HostFormatPair WebGPUTextureCache::kHostFormatGBGRUnaligned = {
    {kLoadShaderIndexGBGR8ToRGB8, WGPUTextureFormat_RGBA8Unorm},
    {},
    xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB};
const WebGPUTextureCache::HostFormatPair WebGPUTextureCache::kHostFormatBGRGUnaligned = {
    {kLoadShaderIndexBGRG8ToRGB8, WGPUTextureFormat_RGBA8Unorm},
    {},
    xenos::XE_GPU_TEXTURE_SWIZZLE_RGBB};
const WebGPUTextureCache::HostFormatPair WebGPUTextureCache::kHostFormatDXT1Unaligned = {
    {kLoadShaderIndexDXT1ToRGBA8, WGPUTextureFormat_RGBA8Unorm},
    {},
    xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
const WebGPUTextureCache::HostFormatPair WebGPUTextureCache::kHostFormatDXT2_3Unaligned = {
    {kLoadShaderIndexDXT3ToRGBA8, WGPUTextureFormat_RGBA8Unorm},
    {},
    xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
const WebGPUTextureCache::HostFormatPair WebGPUTextureCache::kHostFormatDXT4_5Unaligned = {
    {kLoadShaderIndexDXT5ToRGBA8, WGPUTextureFormat_RGBA8Unorm},
    {},
    xenos::XE_GPU_TEXTURE_SWIZZLE_RGBA};
const WebGPUTextureCache::HostFormatPair WebGPUTextureCache::kHostFormatDXNUnaligned = {
    {kLoadShaderIndexDXNToRG8, WGPUTextureFormat_RG8Unorm}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RGGG};
const WebGPUTextureCache::HostFormatPair WebGPUTextureCache::kHostFormatDXT5AUnaligned = {
    {kLoadShaderIndexDXT5AToR8, WGPUTextureFormat_R8Unorm}, {}, xenos::XE_GPU_TEXTURE_SWIZZLE_RRRR};

WebGPUTextureCache::WebGPUTexture::WebGPUTexture(WebGPUTextureCache& texture_cache,
                                                 const TextureKey& key, WGPUTexture texture,
                                                 WGPUTextureView view, uint64_t host_memory_usage)
    : Texture(texture_cache, key), texture_(texture), view_(view) {
  SetHostMemoryUsage(host_memory_usage);
}

WebGPUTextureCache::WebGPUTexture::~WebGPUTexture() {
  auto& cache = static_cast<WebGPUTextureCache&>(texture_cache());
  WGPUTexture texture = texture_;
  WGPUTextureView view = view_;
  // After any recorded commands that use it.
  cache.command_processor_.ReleaseAfterRecordedCommands([texture, view] {
    wgpuTextureViewRelease(view);
    wgpuTextureRelease(texture);
  });
}

WebGPUTextureCache::WebGPUTextureCache(const RegisterFile& register_file,
                                       WebGPUSharedMemory& shared_memory,
                                       WebGPUCommandProcessor& command_processor)
    : TextureCache(register_file, shared_memory, 1, 1),
      command_processor_(command_processor),
      webgpu_shared_memory_(shared_memory) {}

WebGPUTextureCache::~WebGPUTextureCache() {
  Shutdown();
}

bool WebGPUTextureCache::Initialize() {
  static const char* const kLoadShaders[kLoadShaderCount] = {
      shaders::texture_load_8bpb_cs_wgsl,
      shaders::texture_load_16bpb_cs_wgsl,
      shaders::texture_load_32bpb_cs_wgsl,
      shaders::texture_load_64bpb_cs_wgsl,
      shaders::texture_load_128bpb_cs_wgsl,
      nullptr,  // kLoadShaderIndexR5G5B5A1ToB5G5R5A1
      nullptr,  // kLoadShaderIndexR5G6B5ToB5G6R5
      shaders::texture_load_r5g6b5_rgba8_cs_wgsl,
      nullptr,  // kLoadShaderIndexR5G5B6ToB5G6R5WithRBGASwizzle
      nullptr,  // kLoadShaderIndexRGBA4ToBGRA4
      nullptr,  // kLoadShaderIndexRGBA4ToARGB4
      shaders::texture_load_r4g4b4a4_rgba8_cs_wgsl,
      nullptr,  // kLoadShaderIndexGBGR8ToGRGB8
      shaders::texture_load_gbgr8_rgb8_cs_wgsl,
      nullptr,  // kLoadShaderIndexBGRG8ToRGBG8
      shaders::texture_load_bgrg8_rgb8_cs_wgsl,
      nullptr,  // kLoadShaderIndexR10G11B11ToRGBA16
      nullptr,  // kLoadShaderIndexR10G11B11ToRGBA16SNorm
      nullptr,  // kLoadShaderIndexR11G11B10ToRGBA16
      nullptr,  // kLoadShaderIndexR11G11B10ToRGBA16SNorm
      shaders::texture_load_r16_unorm_float_cs_wgsl,
      shaders::texture_load_r16_snorm_float_cs_wgsl,
      shaders::texture_load_rg16_unorm_float_cs_wgsl,
      shaders::texture_load_rg16_snorm_float_cs_wgsl,
      shaders::texture_load_rgba16_unorm_float_cs_wgsl,
      shaders::texture_load_rgba16_snorm_float_cs_wgsl,
      shaders::texture_load_dxt1_rgba8_cs_wgsl,
      shaders::texture_load_dxt3_rgba8_cs_wgsl,
      shaders::texture_load_dxt5_rgba8_cs_wgsl,
      shaders::texture_load_dxn_rg8_cs_wgsl,
      shaders::texture_load_dxt3a_cs_wgsl,
      nullptr,  // kLoadShaderIndexDXT3AAs1111ToBGRA4
      nullptr,  // kLoadShaderIndexDXT3AAs1111ToARGB4
      shaders::texture_load_dxt5a_r8_cs_wgsl,
      shaders::texture_load_ctx1_cs_wgsl,
      shaders::texture_load_depth_unorm_cs_wgsl,
      shaders::texture_load_depth_float_cs_wgsl,
  };

  ui::webgpu::WebGPUDeviceThread& device_thread = command_processor_.device_thread();
  bool bc_supported = false, float32_filterable = false;
  bool ok = true;
  device_thread.Call([&] {
    const ui::webgpu::WebGPUDevice& device = device_thread.device();
    limits_ = device.limits();
    bc_supported = device.HasFeature(WGPUFeatureName_TextureCompressionBC);
    float32_filterable = device.HasFeature(WGPUFeatureName_Float32Filterable);

    for (uint32_t i = 0; i < kLoadShaderCount; ++i) {
      if (!kLoadShaders[i]) {
        continue;
      }
      WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
      source.code = View(kLoadShaders[i]);
      WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
      module_desc.nextInChain = &source.chain;
      module_desc.label = View("Texture load");
      WGPUShaderModule module = wgpuDeviceCreateShaderModule(device.device(), &module_desc);
      WGPUComputePipelineDescriptor pipeline_desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
      pipeline_desc.label = View("Texture load");
      pipeline_desc.compute.module = module;
      pipeline_desc.compute.entryPoint = View("main");
      load_pipelines_[i] = wgpuDeviceCreateComputePipeline(device.device(), &pipeline_desc);
      wgpuShaderModuleRelease(module);
      ok &= load_pipelines_[i] != nullptr;
    }

    // Null textures (zero, as for invalid fetch constants elsewhere).
    const WGPUTextureDimension dimensions[3] = {WGPUTextureDimension_2D, WGPUTextureDimension_3D,
                                                WGPUTextureDimension_2D};
    const WGPUTextureViewDimension view_dimensions[3] = {WGPUTextureViewDimension_2DArray,
                                                         WGPUTextureViewDimension_3D,
                                                         WGPUTextureViewDimension_Cube};
    for (uint32_t i = 0; i < 3; ++i) {
      WGPUTextureDescriptor texture_desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
      texture_desc.label = View("Null texture");
      texture_desc.dimension = dimensions[i];
      texture_desc.size = {1, 1, i == 2 ? 6u : 1u};
      texture_desc.format = WGPUTextureFormat_RGBA8Unorm;
      texture_desc.usage = WGPUTextureUsage_TextureBinding;
      null_textures_[i] = wgpuDeviceCreateTexture(device.device(), &texture_desc);
      WGPUTextureViewDescriptor view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
      view_desc.dimension = view_dimensions[i];
      null_views_[i] = wgpuTextureCreateView(null_textures_[i], &view_desc);
      ok &= null_views_[i] != nullptr;
    }
  });
  if (!ok) {
    REXGPU_ERROR("WebGPU texture cache: couldn't create the load pipelines or null textures");
    return false;
  }

  std::memcpy(host_formats_, kBestHostFormats, sizeof(host_formats_));
  for (HostFormatPair& pair : host_formats_) {
    for (HostFormat* format : {&pair.format_unsigned, &pair.format_signed}) {
      bool float32 = format->format == WGPUTextureFormat_R32Float ||
                     format->format == WGPUTextureFormat_RG32Float ||
                     format->format == WGPUTextureFormat_RGBA32Float;
      if ((float32 && !float32_filterable) || format->load_shader == kLoadShaderIndexUnknown ||
          !load_pipelines_[format->load_shader]) {
        *format = HostFormat();
      }
    }
  }
  bc_supported_ = bc_supported;
  REXGPU_INFO("WebGPU texture cache: texture-compression-bc {}, float32-filterable {}",
              bc_supported ? "yes" : "no", float32_filterable ? "yes" : "no");
  return true;
}

void WebGPUTextureCache::Shutdown() {
  DestroyAllTextures(true);
  ui::webgpu::WebGPUDeviceThread& device_thread = command_processor_.device_thread();
  device_thread.Call([&] {
    for (auto& [key, sampler] : samplers_) {
      wgpuSamplerRelease(sampler);
    }
    samplers_.clear();
    for (WGPUComputePipeline& pipeline : load_pipelines_) {
      if (pipeline) {
        wgpuComputePipelineRelease(pipeline);
        pipeline = nullptr;
      }
    }
    for (uint32_t i = 0; i < 3; ++i) {
      if (null_views_[i]) {
        wgpuTextureViewRelease(null_views_[i]);
        wgpuTextureRelease(null_textures_[i]);
        null_views_[i] = nullptr;
        null_textures_[i] = nullptr;
      }
    }
  });
}

const WebGPUTextureCache::HostFormatPair& WebGPUTextureCache::GetHostFormatPair(
    TextureKey key) const {
  const HostFormatPair& host_format_pair = host_formats_[uint32_t(key.format)];
  if (!host_format_pair.format_unsigned.block_compressed &&
      !host_format_pair.format_signed.block_compressed) {
    return host_format_pair;
  }
  const FormatInfo* format_info = FormatInfo::Get(key.format);
  if (bc_supported_ && !(key.GetWidth() & (format_info->block_width - 1)) &&
      !(key.GetHeight() & (format_info->block_height - 1))) {
    return host_format_pair;
  }
  switch (key.format) {
    case xenos::TextureFormat::k_DXT1:
    case xenos::TextureFormat::k_DXT1_AS_16_16_16_16:
      return kHostFormatDXT1Unaligned;
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT2_3_AS_16_16_16_16:
      return kHostFormatDXT2_3Unaligned;
    case xenos::TextureFormat::k_DXT4_5:
    case xenos::TextureFormat::k_DXT4_5_AS_16_16_16_16:
      return kHostFormatDXT4_5Unaligned;
    case xenos::TextureFormat::k_DXN:
      return kHostFormatDXNUnaligned;
    case xenos::TextureFormat::k_DXT5A:
      return kHostFormatDXT5AUnaligned;
    default:
      return host_format_pair;
  }
}

const WebGPUTextureCache::HostFormat& WebGPUTextureCache::GetTextureHostFormat(
    TextureKey key) const {
  const HostFormatPair& pair = GetHostFormatPair(key);
  if (IsSignedVersionSeparateForFormat(key)) {
    return key.signed_separate ? pair.format_signed : pair.format_unsigned;
  }
  return pair.format_unsigned.format != WGPUTextureFormat_Undefined ? pair.format_unsigned
                                                                    : pair.format_signed;
}

bool WebGPUTextureCache::IsSignedVersionSeparateForFormat(TextureKey key) const {
  const HostFormatPair& pair = GetHostFormatPair(key);
  if (pair.format_unsigned.format == WGPUTextureFormat_Undefined ||
      pair.format_signed.format == WGPUTextureFormat_Undefined) {
    return false;
  }
  // No view reinterpretation between unorm and snorm; the same format with
  // different conversions also needs separate data.
  return pair.format_unsigned.format != pair.format_signed.format ||
         pair.format_unsigned.load_shader != pair.format_signed.load_shader;
}

uint32_t WebGPUTextureCache::GetHostFormatSwizzle(TextureKey key) const {
  return GetHostFormatPair(key).swizzle;
}

uint32_t WebGPUTextureCache::GetMaxHostTextureWidthHeight(xenos::DataDimension dimension) const {
  return dimension == xenos::DataDimension::k3D ? limits_.maxTextureDimension3D
                                                : limits_.maxTextureDimension2D;
}

uint32_t WebGPUTextureCache::GetMaxHostTextureDepthOrArraySize(
    xenos::DataDimension dimension) const {
  switch (dimension) {
    case xenos::DataDimension::k3D:
      return limits_.maxTextureDimension3D;
    case xenos::DataDimension::kCube:
      return 6;
    default:
      return limits_.maxTextureArrayLayers;
  }
}

std::unique_ptr<TextureCache::Texture> WebGPUTextureCache::CreateTexture(TextureKey key) {
  const HostFormat& host_format = GetTextureHostFormat(key);
  if (host_format.format == WGPUTextureFormat_Undefined) {
    if (!unsupported_format_logged_[uint32_t(key.format)]) {
      unsupported_format_logged_[uint32_t(key.format)] = true;
      REXGPU_WARN("WebGPU texture cache: guest format {} ({}) isn't supported yet",
                  FormatInfo::Get(key.format)->name, uint32_t(key.format));
    }
    return nullptr;
  }
  bool is_3d = key.dimension == xenos::DataDimension::k3D;
  uint32_t depth_or_array_size = key.GetDepthOrArraySize();
  WGPUTexture texture = nullptr;
  WGPUTextureView view = nullptr;
  command_processor_.device_thread().Call([&] {
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.label = View("Guest texture");
    desc.dimension = is_3d ? WGPUTextureDimension_3D : WGPUTextureDimension_2D;
    desc.size = {key.GetWidth(), key.GetHeight(), depth_or_array_size};
    desc.format = host_format.format;
    desc.mipLevelCount = key.mip_max_level + 1;
    desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    texture = wgpuDeviceCreateTexture(command_processor_.device_thread().device().device(), &desc);
    if (!texture) {
      return;
    }
    WGPUTextureViewDescriptor view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    switch (key.dimension) {
      case xenos::DataDimension::k3D:
        view_desc.dimension = WGPUTextureViewDimension_3D;
        break;
      case xenos::DataDimension::kCube:
        view_desc.dimension = WGPUTextureViewDimension_Cube;
        break;
      default:
        view_desc.dimension = WGPUTextureViewDimension_2DArray;
        break;
    }
    view = wgpuTextureCreateView(texture, &view_desc);
  });
  if (!view) {
    if (texture) {
      command_processor_.device_thread().Post([texture] { wgpuTextureRelease(texture); });
    }
    return nullptr;
  }
  // An estimate for the cache's memory budget.
  const LoadShaderInfo& load_shader_info = GetLoadShaderInfo(host_format.load_shader);
  uint64_t texels = uint64_t(key.GetWidth()) * key.GetHeight() * depth_or_array_size;
  uint64_t bytes = host_format.block_compressed
                       ? texels * load_shader_info.bytes_per_host_block / 16
                       : texels * load_shader_info.bytes_per_host_block;
  if (key.mip_max_level) {
    bytes += bytes / 3;
  }
  return std::unique_ptr<Texture>(new WebGPUTexture(*this, key, texture, view, bytes));
}

void WebGPUTextureCache::RequestTextures(uint32_t used_texture_mask) {
  TextureCache::RequestTextures(used_texture_mask);
  uint32_t textures_remaining = used_texture_mask;
  uint32_t index;
  while (rex::bit_scan_forward(textures_remaining, &index)) {
    textures_remaining &= ~(uint32_t(1) << index);
    const TextureBinding* binding = GetValidTextureBinding(index);
    if (!binding) {
      continue;
    }
    if (binding->texture) {
      binding->texture->MarkAsUsed();
    }
    if (binding->texture_signed) {
      binding->texture_signed->MarkAsUsed();
    }
  }
}

WGPUTextureView WebGPUTextureCache::GetActiveBindingOrNullView(uint32_t fetch_constant_index,
                                                               xenos::FetchOpDimension dimension,
                                                               bool is_signed) {
  uint32_t null_index;
  xenos::DataDimension expected_dimension;
  switch (dimension) {
    case xenos::FetchOpDimension::k3DOrStacked:
      null_index = 1;
      expected_dimension = xenos::DataDimension::k3D;
      break;
    case xenos::FetchOpDimension::kCube:
      null_index = 2;
      expected_dimension = xenos::DataDimension::kCube;
      break;
    default:
      null_index = 0;
      expected_dimension = xenos::DataDimension::k2DOrStacked;
      break;
  }
  const TextureBinding* binding = GetValidTextureBinding(fetch_constant_index);
  if (binding) {
    xenos::DataDimension binding_dimension = binding->key.dimension;
    if (binding_dimension == xenos::DataDimension::k1D) {
      binding_dimension = xenos::DataDimension::k2DOrStacked;
    }
    // 3D textures fetched as 2D need a 2D view of a 3D texture, which WebGPU
    // doesn't have; those stay null.
    if (binding_dimension == expected_dimension) {
      const HostFormatPair& pair = GetHostFormatPair(binding->key);
      Texture* texture = nullptr;
      if (is_signed) {
        if (pair.format_signed.format != WGPUTextureFormat_Undefined) {
          texture = IsSignedVersionSeparateForFormat(binding->key) ? binding->texture_signed
                                                                   : binding->texture;
        }
      } else if (pair.format_unsigned.format != WGPUTextureFormat_Undefined) {
        texture = binding->texture;
      }
      if (texture) {
        return static_cast<WebGPUTexture*>(texture)->view();
      }
    }
  }
  return null_views_[null_index];
}

WGPUSampler WebGPUTextureCache::GetSampler(const WgslShader::SamplerBinding& binding) {
  xenos::xe_gpu_texture_fetch_t fetch = register_file().GetTextureFetch(binding.fetch_constant);
  xenos::ClampMode clamp[3];
  texture_util::GetClampModesForDimension(fetch, clamp[0], clamp[1], clamp[2]);
  xenos::TextureFilter mag_filter = binding.mag_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.mag_filter
                                        : binding.mag_filter;
  xenos::TextureFilter min_filter = binding.min_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.min_filter
                                        : binding.min_filter;
  xenos::TextureFilter mip_filter = binding.mip_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.mip_filter
                                        : binding.mip_filter;
  xenos::AnisoFilter aniso_filter = binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst
                                        ? fetch.aniso_filter
                                        : binding.aniso_filter;
  aniso_filter = std::min(aniso_filter, xenos::AnisoFilter::kMax_16_1);
  uint32_t mip_min_level, mip_max_level;
  texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, nullptr, nullptr,
                                                 &mip_min_level, &mip_max_level);
  bool mag_linear = mag_filter == xenos::TextureFilter::kLinear;
  bool min_linear = min_filter == xenos::TextureFilter::kLinear;
  bool mip_linear = mip_filter == xenos::TextureFilter::kLinear;
  bool mip_base_map = mip_filter == xenos::TextureFilter::kBaseMap;
  if (aniso_filter != xenos::AnisoFilter::kDisabled) {
    mag_linear = min_linear = mip_linear = true;
  }
  // WebGPU has repeat, mirrored repeat and clamp to edge only.
  WGPUAddressMode address_modes[3];
  for (uint32_t i = 0; i < 3; ++i) {
    switch (clamp[i]) {
      case xenos::ClampMode::kRepeat:
        address_modes[i] = WGPUAddressMode_Repeat;
        break;
      case xenos::ClampMode::kMirroredRepeat:
      case xenos::ClampMode::kMirrorClampToEdge:
      case xenos::ClampMode::kMirrorClampToHalfway:
      case xenos::ClampMode::kMirrorClampToBorder:
        address_modes[i] = WGPUAddressMode_MirrorRepeat;
        break;
      default:
        address_modes[i] = WGPUAddressMode_ClampToEdge;
        break;
    }
  }
  uint64_t key = uint64_t(mag_linear) | (uint64_t(min_linear) << 1) | (uint64_t(mip_linear) << 2) |
                 (uint64_t(mip_base_map) << 3) | (uint64_t(aniso_filter) << 4) |
                 (uint64_t(mip_min_level) << 8) | (uint64_t(address_modes[0]) << 16) |
                 (uint64_t(address_modes[1]) << 24) | (uint64_t(address_modes[2]) << 32);
  auto it = samplers_.find(key);
  if (it != samplers_.end()) {
    return it->second;
  }
  WGPUSampler sampler = nullptr;
  command_processor_.device_thread().Call([&] {
    WGPUSamplerDescriptor desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    desc.label = View("Guest sampler");
    desc.addressModeU = address_modes[0];
    desc.addressModeV = address_modes[1];
    desc.addressModeW = address_modes[2];
    desc.magFilter = mag_linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    desc.minFilter = min_linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    desc.mipmapFilter = mip_linear ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
    desc.lodMinClamp = float(mip_min_level);
    desc.lodMaxClamp = mip_base_map ? desc.lodMinClamp + 0.25f : 32.0f;
    if (aniso_filter != xenos::AnisoFilter::kDisabled) {
      desc.maxAnisotropy =
          uint16_t(1u << (uint32_t(aniso_filter) - uint32_t(xenos::AnisoFilter::kMax_1_1)));
    }
    sampler = wgpuDeviceCreateSampler(command_processor_.device_thread().device().device(), &desc);
  });
  samplers_.emplace(key, sampler);
  return sampler;
}

bool WebGPUTextureCache::LoadTextureDataFromResidentMemoryImpl(Texture& texture, bool load_base,
                                                               bool load_mips) {
  auto& webgpu_texture = static_cast<WebGPUTexture&>(texture);
  TextureKey texture_key = webgpu_texture.key();
  const HostFormat& host_format = GetTextureHostFormat(texture_key);
  LoadShaderIndex load_shader = host_format.load_shader;
  if (load_shader == kLoadShaderIndexUnknown || !load_pipelines_[load_shader]) {
    return false;
  }
  const LoadShaderInfo& load_shader_info = GetLoadShaderInfo(load_shader);

  // As VulkanTextureCache::LoadTextureDataFromResidentMemoryImpl, without
  // resolution scaling, and with host rows aligned to 256 bytes (WebGPU's
  // bytesPerRow alignment for buffer-to-texture copies).
  const texture_util::TextureGuestLayout& guest_layout = webgpu_texture.guest_layout();
  bool is_3d = texture_key.dimension == xenos::DataDimension::k3D;
  bool is_3d_tiling = is_3d || webgpu_texture.force_load_3d_tiling();
  uint32_t width = texture_key.GetWidth();
  uint32_t height = texture_key.GetHeight();
  uint32_t depth_or_array_size = texture_key.GetDepthOrArraySize();
  uint32_t depth = is_3d ? depth_or_array_size : 1;
  uint32_t array_size = is_3d ? 1 : depth_or_array_size;
  xenos::TextureFormat guest_format = texture_key.format;
  const FormatInfo* guest_format_info = FormatInfo::Get(guest_format);
  uint32_t block_width = guest_format_info->block_width;
  uint32_t block_height = guest_format_info->block_height;
  uint32_t bytes_per_block = guest_format_info->bytes_per_block();
  uint32_t level_first = load_base ? 0 : 1;
  uint32_t level_last = load_mips ? texture_key.mip_max_level : 0;
  assert_true(level_first <= level_last);
  uint32_t level_packed = guest_layout.packed_level;
  uint32_t level_stored_first = std::min(level_first, level_packed);
  uint32_t level_stored_last = std::min(level_last, level_packed);
  uint32_t loop_level_first, loop_level_last;
  if (level_packed == 0) {
    loop_level_first = uint32_t(level_first != 0);
    loop_level_last = uint32_t(level_last != 0);
  } else {
    loop_level_first = level_stored_first;
    loop_level_last = level_stored_last;
  }
  uint32_t host_block_width = host_format.block_compressed ? block_width : 1;
  uint32_t host_block_height = host_format.block_compressed ? block_height : 1;
  uint32_t host_x_blocks_per_thread = UINT32_C(1)
                                      << load_shader_info.guest_x_blocks_per_thread_log2;
  if (!host_format.block_compressed) {
    host_x_blocks_per_thread *= block_width;
  }
  // Both powers of two.
  uint32_t host_x_pitch_alignment_blocks =
      std::max(host_x_blocks_per_thread, 256 / load_shader_info.bytes_per_host_block);

  struct HostLayout {
    uint64_t offset_bytes;
    uint64_t slice_size_bytes;
    uint32_t x_pitch_blocks;
    uint32_t y_pitch_blocks;
  };
  uint64_t host_buffer_size = 0;
  HostLayout host_layout_base = {};
  HostLayout host_layout_mips[xenos::kTextureMaxMips] = {};
  for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last; ++loop_level) {
    bool is_base = loop_level == 0;
    uint32_t level = (level_packed == 0) ? 0 : loop_level;
    HostLayout& level_host_layout = is_base ? host_layout_base : host_layout_mips[level];
    level_host_layout.offset_bytes = host_buffer_size;
    uint32_t level_x_extent_texels, level_y_extent_texels, level_z_extent;
    if (level == level_packed) {
      const texture_util::TextureGuestLayout::Level& guest_layout_packed =
          is_base ? guest_layout.base : guest_layout.mips[level];
      level_x_extent_texels = guest_layout_packed.x_extent_blocks * block_width;
      level_y_extent_texels = guest_layout_packed.y_extent_blocks * block_height;
      level_z_extent = guest_layout_packed.z_extent;
    } else {
      level_x_extent_texels = std::max(width >> level, UINT32_C(1));
      level_y_extent_texels = std::max(height >> level, UINT32_C(1));
      level_z_extent = std::max(depth >> level, UINT32_C(1));
    }
    level_host_layout.x_pitch_blocks =
        rex::round_up((level_x_extent_texels + (host_block_width - 1)) / host_block_width,
                      host_x_pitch_alignment_blocks);
    level_host_layout.y_pitch_blocks =
        (level_y_extent_texels + (host_block_height - 1)) / host_block_height;
    level_host_layout.slice_size_bytes = uint64_t(load_shader_info.bytes_per_host_block) *
                                         level_host_layout.x_pitch_blocks *
                                         level_host_layout.y_pitch_blocks * level_z_extent;
    host_buffer_size += level_host_layout.slice_size_bytes * array_size;
  }
  host_buffer_size = rex::align(host_buffer_size, uint64_t(16));

  // Sources in shared memory (16-byte vectors in the shaders).
  struct SourceRange {
    uint64_t offset = 0;
    uint64_t size = 0;
  };
  SourceRange source_base, source_mips;
  if (level_first == 0) {
    source_base.offset = uint64_t(texture_key.base_page) << 12;
    source_base.size = rex::align(uint64_t(webgpu_texture.GetGuestBaseSize()), uint64_t(16));
    if (source_base.size > uint64_t(SharedMemory::kBufferSize) - source_base.offset) {
      return false;
    }
  }
  if (level_last != 0) {
    source_mips.offset = uint64_t(texture_key.mip_page) << 12;
    source_mips.size = rex::align(uint64_t(webgpu_texture.GetGuestMipsSize()), uint64_t(16));
    if (source_mips.size > uint64_t(SharedMemory::kBufferSize) - source_mips.offset) {
      return false;
    }
  }

  struct Dispatch {
    LoadConstants constants;
    uint32_t group_count[3];
    bool from_mips;
  };
  std::vector<Dispatch> dispatches;
  LoadConstants load_constants = {};
  load_constants.is_tiled_3d_endian_scale =
      uint32_t(texture_key.tiled) | (uint32_t(is_3d_tiling) << 1) |
      (uint32_t(texture_key.endianness) << 2) | (UINT32_C(1) << 4) | (UINT32_C(1) << 7);
  uint32_t guest_x_blocks_per_group_log2 = load_shader_info.GetGuestXBlocksPerGroupLog2();
  for (uint32_t loop_level = loop_level_first; loop_level <= loop_level_last; ++loop_level) {
    bool is_base = loop_level == 0;
    uint32_t level = (level_packed == 0) ? 0 : loop_level;
    load_constants.guest_offset = is_base ? 0 : guest_layout.mip_offsets_bytes[level];
    const texture_util::TextureGuestLayout::Level& level_guest_layout =
        is_base ? guest_layout.base : guest_layout.mips[level];
    uint32_t level_guest_pitch = level_guest_layout.row_pitch_bytes;
    if (texture_key.tiled) {
      level_guest_pitch /= bytes_per_block;
    }
    load_constants.guest_pitch_aligned = level_guest_pitch;
    load_constants.guest_z_stride_block_rows_aligned = level_guest_layout.z_slice_stride_block_rows;
    uint32_t level_width, level_height, level_depth;
    if (level == level_packed) {
      level_width = level_guest_layout.x_extent_blocks * block_width;
      level_height = level_guest_layout.y_extent_blocks * block_height;
      level_depth = level_guest_layout.z_extent;
    } else {
      level_width = std::max(width >> level, UINT32_C(1));
      level_height = std::max(height >> level, UINT32_C(1));
      level_depth = std::max(depth >> level, UINT32_C(1));
    }
    load_constants.size_blocks[0] = (level_width + (block_width - 1)) / block_width;
    load_constants.size_blocks[1] = (level_height + (block_height - 1)) / block_height;
    load_constants.size_blocks[2] = level_depth;
    load_constants.height_texels = level_height;
    const HostLayout& level_host_layout = is_base ? host_layout_base : host_layout_mips[level];
    load_constants.host_offset = uint32_t(level_host_layout.offset_bytes);
    load_constants.host_pitch =
        load_shader_info.bytes_per_host_block * level_host_layout.x_pitch_blocks;
    Dispatch dispatch;
    dispatch.group_count[0] =
        (load_constants.size_blocks[0] + ((UINT32_C(1) << guest_x_blocks_per_group_log2) - 1)) >>
        guest_x_blocks_per_group_log2;
    dispatch.group_count[1] =
        (load_constants.size_blocks[1] + ((UINT32_C(1) << kLoadGuestYBlocksPerGroupLog2) - 1)) >>
        kLoadGuestYBlocksPerGroupLog2;
    dispatch.group_count[2] = load_constants.size_blocks[2];
    dispatch.from_mips = !is_base;
    for (uint32_t slice = 0; slice < array_size; ++slice) {
      dispatch.constants = load_constants;
      dispatches.push_back(dispatch);
      load_constants.guest_offset += level_guest_layout.array_slice_stride_bytes;
      load_constants.host_offset += uint32_t(level_host_layout.slice_size_bytes);
    }
  }

  // Copies from the converted data into the texture, per level.
  std::vector<WGPUTexelCopyBufferInfo> copy_sources;
  std::vector<WGPUTexelCopyTextureInfo> copy_destinations;
  std::vector<WGPUExtent3D> copy_sizes;
  for (uint32_t level = level_first; level <= level_last; ++level) {
    const HostLayout& level_host_layout =
        level != 0 ? host_layout_mips[std::min(level, level_packed)] : host_layout_base;
    uint64_t offset = level_host_layout.offset_bytes;
    if (level >= level_packed) {
      uint32_t level_offset_blocks_x, level_offset_blocks_y, level_offset_z;
      texture_util::GetPackedMipOffset(width, height, depth, guest_format, level,
                                       level_offset_blocks_x, level_offset_blocks_y,
                                       level_offset_z);
      uint32_t level_offset_host_blocks_x = level_offset_blocks_x;
      uint32_t level_offset_host_blocks_y = level_offset_blocks_y;
      if (!host_format.block_compressed) {
        level_offset_host_blocks_x *= block_width;
        level_offset_host_blocks_y *= block_height;
      }
      offset += uint64_t(load_shader_info.bytes_per_host_block) *
                (level_offset_host_blocks_x +
                 level_host_layout.x_pitch_blocks *
                     (level_offset_host_blocks_y +
                      level_host_layout.y_pitch_blocks * uint64_t(level_offset_z)));
    }
    WGPUTexelCopyBufferInfo& source = copy_sources.emplace_back(WGPU_TEXEL_COPY_BUFFER_INFO_INIT);
    source.layout.offset = offset;
    source.layout.bytesPerRow =
        load_shader_info.bytes_per_host_block * level_host_layout.x_pitch_blocks;
    source.layout.rowsPerImage = level_host_layout.y_pitch_blocks;
    WGPUTexelCopyTextureInfo& destination =
        copy_destinations.emplace_back(WGPU_TEXEL_COPY_TEXTURE_INFO_INIT);
    destination.texture = webgpu_texture.texture();
    destination.mipLevel = level;
    // Block-compressed copies are in whole blocks (the mip's physical size).
    uint32_t level_width = std::max(width >> level, UINT32_C(1));
    uint32_t level_height = std::max(height >> level, UINT32_C(1));
    if (host_format.block_compressed) {
      level_width = rex::round_up(level_width, block_width);
      level_height = rex::round_up(level_height, block_height);
    }
    copy_sizes.push_back(
        {level_width, level_height, is_3d ? std::max(depth >> level, UINT32_C(1)) : array_size});
  }

  webgpu_texture.MarkAsUsed();
  command_processor_.EndRenderPass();
  WGPUComputePipeline pipeline = load_pipelines_[load_shader];
  WGPUBuffer shared_memory_buffer = webgpu_shared_memory_.buffer();
  command_processor_.command_list().Record([=](WebGPUCommandList::Encoder& encoder) mutable {
    WGPUDevice device = encoder.device;
    WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    buffer_desc.label = View("Texture load");
    buffer_desc.size = host_buffer_size;
    buffer_desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc;
    WGPUBuffer scratch = wgpuDeviceCreateBuffer(device, &buffer_desc);
    buffer_desc.label = View("Texture load constants");
    buffer_desc.size = kLoadConstantsStride * dispatches.size();
    buffer_desc.usage = WGPUBufferUsage_Uniform;
    buffer_desc.mappedAtCreation = true;
    WGPUBuffer constants = wgpuDeviceCreateBuffer(device, &buffer_desc);
    auto* mapping = static_cast<uint8_t*>(wgpuBufferGetMappedRange(constants, 0, buffer_desc.size));
    if (mapping) {
      for (size_t i = 0; i < dispatches.size(); ++i) {
        std::memcpy(mapping + kLoadConstantsStride * i, &dispatches[i].constants,
                    sizeof(LoadConstants));
      }
    }
    wgpuBufferUnmap(constants);

    WGPUBindGroupLayout layouts[3];
    for (uint32_t i = 0; i < 3; ++i) {
      layouts[i] = wgpuComputePipelineGetBindGroupLayout(pipeline, i);
    }
    auto create_group = [&](uint32_t group, WGPUBuffer buffer, uint64_t offset, uint64_t size) {
      WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
      entry.binding = 0;
      entry.buffer = buffer;
      entry.offset = offset;
      entry.size = size;
      WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
      desc.layout = layouts[group];
      desc.entryCount = 1;
      desc.entries = &entry;
      return wgpuDeviceCreateBindGroup(device, &desc);
    };
    std::vector<WGPUBindGroup> groups;
    WGPUBindGroup destination_group = create_group(0, scratch, 0, host_buffer_size);
    groups.push_back(destination_group);
    WGPUBindGroup base_group = source_base.size ? create_group(1, shared_memory_buffer,
                                                               source_base.offset, source_base.size)
                                                : nullptr;
    WGPUBindGroup mips_group = source_mips.size ? create_group(1, shared_memory_buffer,
                                                               source_mips.offset, source_mips.size)
                                                : nullptr;
    if (base_group) {
      groups.push_back(base_group);
    }
    if (mips_group) {
      groups.push_back(mips_group);
    }

    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(encoder.encoder, nullptr);
    wgpuComputePassEncoderSetPipeline(pass, pipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, destination_group, 0, nullptr);
    for (size_t i = 0; i < dispatches.size(); ++i) {
      const Dispatch& dispatch = dispatches[i];
      WGPUBindGroup source_group = dispatch.from_mips ? mips_group : base_group;
      if (!source_group) {
        continue;
      }
      WGPUBindGroup constants_group =
          create_group(2, constants, kLoadConstantsStride * i, kLoadConstantsBindingSize);
      groups.push_back(constants_group);
      wgpuComputePassEncoderSetBindGroup(pass, 1, source_group, 0, nullptr);
      wgpuComputePassEncoderSetBindGroup(pass, 2, constants_group, 0, nullptr);
      wgpuComputePassEncoderDispatchWorkgroups(pass, dispatch.group_count[0],
                                               dispatch.group_count[1], dispatch.group_count[2]);
    }
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);

    for (size_t i = 0; i < copy_sources.size(); ++i) {
      copy_sources[i].buffer = scratch;
      wgpuCommandEncoderCopyBufferToTexture(encoder.encoder, &copy_sources[i],
                                            &copy_destinations[i], &copy_sizes[i]);
    }

    for (WGPUBindGroup group : groups) {
      wgpuBindGroupRelease(group);
    }
    for (WGPUBindGroupLayout layout : layouts) {
      wgpuBindGroupLayoutRelease(layout);
    }
    wgpuBufferRelease(constants);
    wgpuBufferRelease(scratch);
  });
  return true;
}

}  // namespace rex::graphics::webgpu
