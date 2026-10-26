/**
 * @file        graphics/webgpu/pipeline_cache.h
 * @brief       Shader translation and render pipelines for the WebGPU backend
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <webgpu/webgpu.h>

#include <rex/graphics/pipeline/shader/wgsl.h>
#include <rex/graphics/pipeline/shader/wgsl_translator.h>
#include <rex/graphics/primitive_processor.h>
#include <rex/graphics/registers.h>
#include <rex/string/buffer.h>

namespace rex::graphics {
class RegisterFile;
}

namespace rex::graphics::webgpu {

class WebGPUCommandProcessor;

class WebGPUPipelineCache {
 public:
  // Everything a render pipeline depends on, zero-initialized and hashed as
  // bytes.
  struct PipelineKey {
    uint64_t vertex_shader_hash;
    uint64_t vertex_shader_modification;
    uint64_t pixel_shader_hash;  // 0 without a pixel shader.
    uint64_t pixel_shader_modification;
    uint32_t topology;            // WGPUPrimitiveTopology
    uint32_t strip_index_format;  // WGPUIndexFormat
    uint32_t cull_mode;           // WGPUCullMode
    uint32_t front_face;          // WGPUFrontFace
    uint32_t depth_format;        // WGPUTextureFormat, undefined without depth
    uint32_t depth_write;
    uint32_t depth_compare;  // xenos::CompareFunction
    uint32_t stencil_enable;
    // compare, fail, depth fail, pass (xenos values), front then back.
    uint32_t stencil_ops[2][4];
    uint32_t stencil_read_mask;
    uint32_t stencil_write_mask;
    uint32_t color_formats[xenos::kMaxColorRenderTargets];  // WGPUTextureFormat
    uint32_t color_write_masks[xenos::kMaxColorRenderTargets];
    // RB_BLENDCONTROL & 0x1FFF1FFF, or 0x00010001 (no blending).
    uint32_t blend_controls[xenos::kMaxColorRenderTargets];
  };

  WebGPUPipelineCache(WebGPUCommandProcessor& command_processor, const RegisterFile& register_file);
  ~WebGPUPipelineCache();

  bool Initialize();
  void Shutdown();

  WGPUBindGroupLayout shared_memory_layout() const { return shared_memory_layout_; }
  WGPUBindGroupLayout constants_layout() const { return constants_layout_; }

  WgslShaderTranslator::Modification GetCurrentVertexShaderModification(
      const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
      uint32_t interpolator_mask, uint32_t interpolators_centroid, bool ps_param_gen_used) const;
  WgslShaderTranslator::Modification GetCurrentPixelShaderModification(
      const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos) const;
  // Interpolators to sample at the centroid, as the pixel shader modification
  // and the vertex shader (WebGPU requires them to match) both need it.
  uint32_t GetInterpolatorsCentroid(uint32_t interpolator_mask) const;

  // Translates if needed; false if the translation failed.
  bool EnsureTranslated(WgslShader::WgslTranslation& translation);

  // Device-thread objects for a draw. The pipeline is nullptr if it couldn't
  // be created. A stage with texture bindings gets its layout (the draw
  // creates the bind group from the texture cache's views and samplers), one
  // without gets an empty bind group.
  struct DrawObjects {
    WGPURenderPipeline pipeline = nullptr;
    WGPUBindGroupLayout textures_vertex_layout = nullptr;
    WGPUBindGroupLayout textures_pixel_layout = nullptr;
    WGPUBindGroup textures_empty = nullptr;
  };
  bool GetDrawObjects(const PipelineKey& key, WgslShader::WgslTranslation& vertex_translation,
                      WgslShader::WgslTranslation* pixel_translation, DrawObjects& objects_out);

 private:
  struct ShaderTextures {
    WGPUBindGroupLayout layout = nullptr;
    WGPUBindGroup bind_group = nullptr;
  };

  // On the device thread.
  WGPUShaderModule CreateModule(const std::string& wgsl, const char* label);
  const ShaderTextures& GetShaderTextures(const WgslShader& shader);
  WGPURenderPipeline CreatePipeline(const PipelineKey& key, WGPUShaderModule vertex_module,
                                    WGPUShaderModule pixel_module,
                                    WGPUBindGroupLayout textures_vertex_layout,
                                    WGPUBindGroupLayout textures_pixel_layout);

  WebGPUCommandProcessor& command_processor_;
  const RegisterFile& register_file_;

  std::unique_ptr<WgslShaderTranslator> translator_;
  string::StringBuffer ucode_disasm_buffer_;

  // Device-thread objects.
  WGPUBindGroupLayout shared_memory_layout_ = nullptr;
  WGPUBindGroupLayout constants_layout_ = nullptr;
  // Placeholder views (1x1 white) for each texture dimension, and a sampler.
  WGPUTexture placeholder_textures_[3] = {};
  WGPUTextureView placeholder_views_[3] = {};  // 2D array, 3D, cube
  WGPUSampler placeholder_sampler_ = nullptr;
  // Also for draws without a pixel shader (color targets with no writes).
  WGPUShaderModule empty_fragment_module_ = nullptr;

  std::unordered_map<const Shader::Translation*, WGPUShaderModule> modules_;
  // By shader (the bindings don't depend on the modification).
  std::unordered_map<const Shader*, ShaderTextures> shader_textures_;
  ShaderTextures no_textures_vertex_;
  ShaderTextures no_textures_pixel_;
  struct Pipeline {
    PipelineKey key;
    WGPURenderPipeline pipeline;
  };
  std::unordered_map<uint64_t, std::vector<Pipeline>> pipelines_;
};

}  // namespace rex::graphics::webgpu
