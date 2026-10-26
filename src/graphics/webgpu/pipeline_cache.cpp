/**
 * @file        graphics/webgpu/pipeline_cache.cpp
 * @brief       Shader translation and render pipelines for the WebGPU backend
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/pipeline_cache.h>

#include <cstring>

#include <rex/graphics/register_file.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/webgpu/command_list.h>
#include <rex/graphics/webgpu/command_processor.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/math.h>

namespace rex::graphics::webgpu {

namespace {

WGPUStringView View(const char* s) {
  return {s, WGPU_STRLEN};
}

WGPUCompareFunction ToCompare(uint32_t function) {
  static const WGPUCompareFunction kMap[] = {
      WGPUCompareFunction_Never,        WGPUCompareFunction_Less,    WGPUCompareFunction_Equal,
      WGPUCompareFunction_LessEqual,    WGPUCompareFunction_Greater, WGPUCompareFunction_NotEqual,
      WGPUCompareFunction_GreaterEqual, WGPUCompareFunction_Always,
  };
  return kMap[function & 7];
}

WGPUStencilOperation ToStencilOp(uint32_t op) {
  static const WGPUStencilOperation kMap[] = {
      WGPUStencilOperation_Keep,           WGPUStencilOperation_Zero,
      WGPUStencilOperation_Replace,        WGPUStencilOperation_IncrementClamp,
      WGPUStencilOperation_DecrementClamp, WGPUStencilOperation_Invert,
      WGPUStencilOperation_IncrementWrap,  WGPUStencilOperation_DecrementWrap,
  };
  return kMap[op & 7];
}

WGPUBlendFactor ToBlendFactor(uint32_t factor) {
  switch (xenos::BlendFactor(factor)) {
    case xenos::BlendFactor::kOne:
      return WGPUBlendFactor_One;
    case xenos::BlendFactor::kSrcColor:
      return WGPUBlendFactor_Src;
    case xenos::BlendFactor::kOneMinusSrcColor:
      return WGPUBlendFactor_OneMinusSrc;
    case xenos::BlendFactor::kSrcAlpha:
      return WGPUBlendFactor_SrcAlpha;
    case xenos::BlendFactor::kOneMinusSrcAlpha:
      return WGPUBlendFactor_OneMinusSrcAlpha;
    case xenos::BlendFactor::kDstColor:
      return WGPUBlendFactor_Dst;
    case xenos::BlendFactor::kOneMinusDstColor:
      return WGPUBlendFactor_OneMinusDst;
    case xenos::BlendFactor::kDstAlpha:
      return WGPUBlendFactor_DstAlpha;
    case xenos::BlendFactor::kOneMinusDstAlpha:
      return WGPUBlendFactor_OneMinusDstAlpha;
    case xenos::BlendFactor::kConstantColor:
    case xenos::BlendFactor::kConstantAlpha:
      // WebGPU's constant factor is the whole color; alpha is approximated.
      return WGPUBlendFactor_Constant;
    case xenos::BlendFactor::kOneMinusConstantColor:
    case xenos::BlendFactor::kOneMinusConstantAlpha:
      return WGPUBlendFactor_OneMinusConstant;
    case xenos::BlendFactor::kSrcAlphaSaturate:
      return WGPUBlendFactor_SrcAlphaSaturated;
    default:
      return WGPUBlendFactor_Zero;
  }
}

WGPUBlendComponent ToBlendComponent(uint32_t src_factor, uint32_t op, uint32_t dst_factor) {
  WGPUBlendComponent component = WGPU_BLEND_COMPONENT_INIT;
  switch (xenos::BlendOp(op)) {
    case xenos::BlendOp::kSubtract:
      component.operation = WGPUBlendOperation_Subtract;
      break;
    case xenos::BlendOp::kMin:
      component.operation = WGPUBlendOperation_Min;
      break;
    case xenos::BlendOp::kMax:
      component.operation = WGPUBlendOperation_Max;
      break;
    case xenos::BlendOp::kRevSubtract:
      component.operation = WGPUBlendOperation_ReverseSubtract;
      break;
    default:
      component.operation = WGPUBlendOperation_Add;
      break;
  }
  if (component.operation == WGPUBlendOperation_Min ||
      component.operation == WGPUBlendOperation_Max) {
    // WebGPU requires One for min and max, which ignore the factors anyway.
    component.srcFactor = WGPUBlendFactor_One;
    component.dstFactor = WGPUBlendFactor_One;
  } else {
    component.srcFactor = ToBlendFactor(src_factor);
    component.dstFactor = ToBlendFactor(dst_factor);
  }
  return component;
}

bool IsBlendable(WGPUTextureFormat format) {
  return format != WGPUTextureFormat_R32Float && format != WGPUTextureFormat_RG32Float;
}

}  // namespace

WebGPUPipelineCache::WebGPUPipelineCache(WebGPUCommandProcessor& command_processor,
                                         const RegisterFile& register_file)
    : command_processor_(command_processor), register_file_(register_file) {}

WebGPUPipelineCache::~WebGPUPipelineCache() {
  Shutdown();
}

bool WebGPUPipelineCache::Initialize() {
  ui::webgpu::WebGPUDeviceThread& device_thread = command_processor_.device_thread();
  WgslShaderTranslator::Features features;
  device_thread.Call([&] {
    features.clip_distances = device_thread.device().HasFeature(WGPUFeatureName_ClipDistances);
  });
  translator_ = std::make_unique<WgslShaderTranslator>(features);

  bool ok = false;
  device_thread.Call([&] {
    WGPUDevice device = device_thread.device().device();
    WGPUShaderStage stages = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;

    // @group(0): shared memory.
    WGPUBindGroupLayoutEntry shared_memory_entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    shared_memory_entry.binding = 0;
    shared_memory_entry.visibility = stages;
    shared_memory_entry.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    WGPUBindGroupLayoutDescriptor layout_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    layout_desc.entryCount = 1;
    layout_desc.entries = &shared_memory_entry;
    shared_memory_layout_ = wgpuDeviceCreateBindGroupLayout(device, &layout_desc);

    // @group(1): constants, dynamic offsets into the submission's buffer.
    WGPUBindGroupLayoutEntry constant_entries[WgslShaderTranslator::kConstantBufferCount];
    for (uint32_t i = 0; i < WgslShaderTranslator::kConstantBufferCount; ++i) {
      constant_entries[i] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
      constant_entries[i].binding = i;
      constant_entries[i].visibility = stages;
      constant_entries[i].buffer.type = WGPUBufferBindingType_Uniform;
      constant_entries[i].buffer.hasDynamicOffset = true;
    }
    layout_desc.entryCount = WgslShaderTranslator::kConstantBufferCount;
    layout_desc.entries = constant_entries;
    constants_layout_ = wgpuDeviceCreateBindGroupLayout(device, &layout_desc);

    // Stages without textures.
    layout_desc.entryCount = 0;
    layout_desc.entries = nullptr;
    no_textures_vertex_.layout = wgpuDeviceCreateBindGroupLayout(device, &layout_desc);
    WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    group_desc.layout = no_textures_vertex_.layout;
    no_textures_vertex_.bind_group = wgpuDeviceCreateBindGroup(device, &group_desc);
    no_textures_pixel_ = no_textures_vertex_;

    // Placeholder textures: white.
    static const uint8_t kWhite[4 * 6] = {255, 255, 255, 255, 255, 255, 255, 255,
                                          255, 255, 255, 255, 255, 255, 255, 255,
                                          255, 255, 255, 255, 255, 255, 255, 255};
    const WGPUTextureDimension dimensions[3] = {WGPUTextureDimension_2D, WGPUTextureDimension_3D,
                                                WGPUTextureDimension_2D};
    const WGPUTextureViewDimension view_dimensions[3] = {WGPUTextureViewDimension_2DArray,
                                                         WGPUTextureViewDimension_3D,
                                                         WGPUTextureViewDimension_Cube};
    for (uint32_t i = 0; i < 3; ++i) {
      uint32_t layers = i == 2 ? 6 : 1;
      WGPUTextureDescriptor texture_desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
      texture_desc.label = View("Placeholder texture");
      texture_desc.dimension = dimensions[i];
      texture_desc.size = {1, 1, layers};
      texture_desc.format = WGPUTextureFormat_RGBA8Unorm;
      texture_desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
      placeholder_textures_[i] = wgpuDeviceCreateTexture(device, &texture_desc);
      WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
      destination.texture = placeholder_textures_[i];
      WGPUTexelCopyBufferLayout data_layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
      data_layout.bytesPerRow = 4;
      data_layout.rowsPerImage = 1;
      WGPUExtent3D size = {1, 1, layers};
      wgpuQueueWriteTexture(device_thread.device().queue(), &destination, kWhite, 4 * layers,
                            &data_layout, &size);
      WGPUTextureViewDescriptor view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
      view_desc.dimension = view_dimensions[i];
      placeholder_views_[i] = wgpuTextureCreateView(placeholder_textures_[i], &view_desc);
    }
    WGPUSamplerDescriptor sampler_desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sampler_desc.magFilter = WGPUFilterMode_Linear;
    sampler_desc.minFilter = WGPUFilterMode_Linear;
    placeholder_sampler_ = wgpuDeviceCreateSampler(device, &sampler_desc);

    empty_fragment_module_ = CreateModule("@fragment fn main() {}\n", "Empty fragment shader");

    ok = shared_memory_layout_ && constants_layout_ && no_textures_vertex_.bind_group &&
         placeholder_views_[0] && placeholder_views_[1] && placeholder_views_[2] &&
         placeholder_sampler_ && empty_fragment_module_;
  });
  if (!ok) {
    REXGPU_ERROR("WebGPU pipeline cache: couldn't create the shared objects");
  }
  return ok;
}

void WebGPUPipelineCache::Shutdown() {
  if (!translator_) {
    return;
  }
  translator_.reset();
  command_processor_.device_thread().Call([&] {
    for (auto& [hash, pipelines] : pipelines_) {
      for (Pipeline& pipeline : pipelines) {
        if (pipeline.pipeline) {
          wgpuRenderPipelineRelease(pipeline.pipeline);
        }
      }
    }
    pipelines_.clear();
    for (auto& [shader, textures] : shader_textures_) {
      wgpuBindGroupRelease(textures.bind_group);
      wgpuBindGroupLayoutRelease(textures.layout);
    }
    shader_textures_.clear();
    for (auto& [translation, module] : modules_) {
      if (module) {
        wgpuShaderModuleRelease(module);
      }
    }
    modules_.clear();
    if (no_textures_vertex_.bind_group) {
      wgpuBindGroupRelease(no_textures_vertex_.bind_group);
      wgpuBindGroupLayoutRelease(no_textures_vertex_.layout);
    }
    no_textures_vertex_ = no_textures_pixel_ = ShaderTextures();
    for (uint32_t i = 0; i < 3; ++i) {
      if (placeholder_views_[i]) {
        wgpuTextureViewRelease(placeholder_views_[i]);
        wgpuTextureRelease(placeholder_textures_[i]);
      }
      placeholder_views_[i] = nullptr;
      placeholder_textures_[i] = nullptr;
    }
    if (placeholder_sampler_) {
      wgpuSamplerRelease(placeholder_sampler_);
      placeholder_sampler_ = nullptr;
    }
    if (empty_fragment_module_) {
      wgpuShaderModuleRelease(empty_fragment_module_);
      empty_fragment_module_ = nullptr;
    }
    if (constants_layout_) {
      wgpuBindGroupLayoutRelease(constants_layout_);
      constants_layout_ = nullptr;
    }
    if (shared_memory_layout_) {
      wgpuBindGroupLayoutRelease(shared_memory_layout_);
      shared_memory_layout_ = nullptr;
    }
  });
}

uint32_t WebGPUPipelineCache::GetInterpolatorsCentroid(uint32_t interpolator_mask) const {
  const RegisterFile& regs = register_file_;
  return interpolator_mask & ~xenos::GetInterpolatorSamplingPattern(
                                 regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
                                 regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
                                 regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);
}

WgslShaderTranslator::Modification WebGPUPipelineCache::GetCurrentVertexShaderModification(
    const Shader& shader, Shader::HostVertexShaderType host_vertex_shader_type,
    uint32_t interpolator_mask, uint32_t interpolators_centroid, bool ps_param_gen_used) const {
  const RegisterFile& regs = register_file_;
  WgslShaderTranslator::Modification modification(translator_->GetDefaultVertexShaderModification(
      shader.GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg),
      host_vertex_shader_type));
  modification.vertex.interpolator_mask = interpolator_mask;
  modification.vertex.interpolators_centroid = interpolators_centroid;
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t user_clip_planes = pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
  // Culling planes have no WebGPU equivalent.
  if (pa_cl_clip_cntl.ucp_cull_only_ena) {
    user_clip_planes = 0;
  }
  modification.vertex.user_clip_plane_count = rex::bit_count(user_clip_planes);
  modification.vertex.point_ps_ucp_mode = pa_cl_clip_cntl.ps_ucp_mode;
  if (host_vertex_shader_type == Shader::HostVertexShaderType::kPointListAsTriangleStrip) {
    modification.vertex.output_point_parameters = uint32_t(ps_param_gen_used);
  }
  return modification;
}

WgslShaderTranslator::Modification WebGPUPipelineCache::GetCurrentPixelShaderModification(
    const Shader& shader, uint32_t interpolator_mask, uint32_t param_gen_pos) const {
  const RegisterFile& regs = register_file_;
  WgslShaderTranslator::Modification modification(translator_->GetDefaultPixelShaderModification(
      shader.GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg)));
  modification.pixel.interpolator_mask = interpolator_mask;
  modification.pixel.interpolators_centroid = GetInterpolatorsCentroid(interpolator_mask);
  if (param_gen_pos < xenos::kMaxInterpolators) {
    modification.pixel.param_gen_enable = 1;
    modification.pixel.param_gen_interpolator = param_gen_pos;
    modification.pixel.param_gen_point =
        uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type == xenos::PrimitiveType::kPointList);
  }
  using DepthStencilMode = WgslShaderTranslator::Modification::DepthStencilMode;
  // Float24 depth comes with a 32-bit float depth buffer.
  if (shader.implicit_early_z_write_allowed() &&
      (!shader.writes_color_target(0) ||
       !draw_util::DoesCoverageDependOnAlpha(regs.Get<reg::RB_COLORCONTROL>()))) {
    modification.pixel.depth_stencil_mode = DepthStencilMode::kEarlyHint;
  } else {
    modification.pixel.depth_stencil_mode = DepthStencilMode::kNoModifiers;
  }
  return modification;
}

bool WebGPUPipelineCache::EnsureTranslated(WgslShader::WgslTranslation& translation) {
  Shader& shader = translation.shader();
  if (!shader.is_ucode_analyzed()) {
    shader.AnalyzeUcode(ucode_disasm_buffer_);
  }
  if (!translation.is_translated()) {
    if (!translator_->TranslateAnalyzedShader(translation)) {
      REXGPU_ERROR("WebGPU: couldn't translate {} shader {:016X}",
                   shader.type() == xenos::ShaderType::kVertex ? "vertex" : "pixel",
                   shader.ucode_data_hash());
      return false;
    }
  }
  if (!translation.is_valid()) {
    return false;
  }
  auto it = modules_.find(&translation);
  if (it == modules_.end()) {
    std::string wgsl = translation.GetTranslatedBinaryString();
    WGPUShaderModule module = nullptr;
    command_processor_.device_thread().Call(
        [&] { module = CreateModule(wgsl, "Translated shader"); });
    it = modules_.emplace(&translation, module).first;
  }
  return it->second != nullptr;
}

WGPUShaderModule WebGPUPipelineCache::CreateModule(const std::string& wgsl, const char* label) {
  WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
  source.code = {wgsl.data(), wgsl.size()};
  WGPUShaderModuleDescriptor desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
  desc.nextInChain = &source.chain;
  desc.label = View(label);
  return wgpuDeviceCreateShaderModule(command_processor_.device_thread().device().device(), &desc);
}

const WebGPUPipelineCache::ShaderTextures& WebGPUPipelineCache::GetShaderTextures(
    const WgslShader& shader) {
  const auto& texture_bindings = shader.GetTextureBindingsAfterTranslation();
  const auto& sampler_bindings = shader.GetSamplerBindingsAfterTranslation();
  bool is_vertex = shader.type() == xenos::ShaderType::kVertex;
  if (texture_bindings.empty() && sampler_bindings.empty()) {
    return is_vertex ? no_textures_vertex_ : no_textures_pixel_;
  }
  auto it = shader_textures_.find(&shader);
  if (it != shader_textures_.end()) {
    return it->second;
  }
  WGPUDevice device = command_processor_.device_thread().device().device();
  WGPUShaderStage stage = is_vertex ? WGPUShaderStage_Vertex : WGPUShaderStage_Fragment;
  std::vector<WGPUBindGroupLayoutEntry> layout_entries;
  std::vector<WGPUBindGroupEntry> entries;
  for (size_t i = 0; i < texture_bindings.size(); ++i) {
    WGPUBindGroupLayoutEntry& layout_entry =
        layout_entries.emplace_back(WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT);
    layout_entry.binding = uint32_t(i);
    layout_entry.visibility = stage;
    layout_entry.texture.sampleType = WGPUTextureSampleType_Float;
    uint32_t placeholder;
    switch (texture_bindings[i].dimension) {
      case xenos::FetchOpDimension::k3DOrStacked:
        layout_entry.texture.viewDimension = WGPUTextureViewDimension_3D;
        placeholder = 1;
        break;
      case xenos::FetchOpDimension::kCube:
        layout_entry.texture.viewDimension = WGPUTextureViewDimension_Cube;
        placeholder = 2;
        break;
      default:
        layout_entry.texture.viewDimension = WGPUTextureViewDimension_2DArray;
        placeholder = 0;
        break;
    }
    WGPUBindGroupEntry& entry = entries.emplace_back(WGPU_BIND_GROUP_ENTRY_INIT);
    entry.binding = uint32_t(i);
    entry.textureView = placeholder_views_[placeholder];
  }
  for (size_t i = 0; i < sampler_bindings.size(); ++i) {
    uint32_t binding = uint32_t(texture_bindings.size() + i);
    WGPUBindGroupLayoutEntry& layout_entry =
        layout_entries.emplace_back(WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT);
    layout_entry.binding = binding;
    layout_entry.visibility = stage;
    layout_entry.sampler.type = WGPUSamplerBindingType_Filtering;
    WGPUBindGroupEntry& entry = entries.emplace_back(WGPU_BIND_GROUP_ENTRY_INIT);
    entry.binding = binding;
    entry.sampler = placeholder_sampler_;
  }
  ShaderTextures textures;
  WGPUBindGroupLayoutDescriptor layout_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
  layout_desc.entryCount = layout_entries.size();
  layout_desc.entries = layout_entries.data();
  textures.layout = wgpuDeviceCreateBindGroupLayout(device, &layout_desc);
  WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
  group_desc.layout = textures.layout;
  group_desc.entryCount = entries.size();
  group_desc.entries = entries.data();
  textures.bind_group = wgpuDeviceCreateBindGroup(device, &group_desc);
  return shader_textures_.emplace(&shader, textures).first->second;
}

bool WebGPUPipelineCache::GetDrawObjects(const PipelineKey& key,
                                         WgslShader::WgslTranslation& vertex_translation,
                                         WgslShader::WgslTranslation* pixel_translation,
                                         DrawObjects& objects_out) {
  uint64_t hash = XXH3_64bits(&key, sizeof(key));
  auto& bucket = pipelines_[hash];
  const auto& vertex_shader = static_cast<const WgslShader&>(vertex_translation.shader());
  const WgslShader* pixel_shader =
      pixel_translation ? &static_cast<const WgslShader&>(pixel_translation->shader()) : nullptr;
  for (const Pipeline& pipeline : bucket) {
    if (!std::memcmp(&pipeline.key, &key, sizeof(key))) {
      if (!pipeline.pipeline) {
        return false;
      }
      objects_out.pipeline = pipeline.pipeline;
      // Bind groups exist once a pipeline with the shaders does.
      objects_out.textures_vertex = shader_textures_.count(&vertex_shader)
                                        ? shader_textures_[&vertex_shader].bind_group
                                        : no_textures_vertex_.bind_group;
      objects_out.textures_pixel = pixel_shader && shader_textures_.count(pixel_shader)
                                       ? shader_textures_[pixel_shader].bind_group
                                       : no_textures_pixel_.bind_group;
      return true;
    }
  }
  WGPUShaderModule vertex_module = modules_[&vertex_translation];
  WGPUShaderModule pixel_module =
      pixel_translation ? modules_[pixel_translation] : empty_fragment_module_;
  WGPURenderPipeline pipeline = nullptr;
  command_processor_.device_thread().Call([&] {
    const ShaderTextures& textures_vertex = GetShaderTextures(vertex_shader);
    const ShaderTextures& textures_pixel =
        pixel_shader ? GetShaderTextures(*pixel_shader) : no_textures_pixel_;
    objects_out.textures_vertex = textures_vertex.bind_group;
    objects_out.textures_pixel = textures_pixel.bind_group;
    pipeline = CreatePipeline(key, vertex_module, pixel_module, textures_vertex.layout,
                              textures_pixel.layout);
  });
  bucket.push_back({key, pipeline});
  objects_out.pipeline = pipeline;
  return pipeline != nullptr;
}

WGPURenderPipeline WebGPUPipelineCache::CreatePipeline(const PipelineKey& key,
                                                       WGPUShaderModule vertex_module,
                                                       WGPUShaderModule pixel_module,
                                                       WGPUBindGroupLayout textures_vertex_layout,
                                                       WGPUBindGroupLayout textures_pixel_layout) {
  WGPUDevice device = command_processor_.device_thread().device().device();
  WGPUBindGroupLayout layouts[WgslShaderTranslator::kBindGroupCount] = {
      shared_memory_layout_, constants_layout_, textures_vertex_layout, textures_pixel_layout};
  WGPUPipelineLayoutDescriptor layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
  layout_desc.bindGroupLayoutCount = WgslShaderTranslator::kBindGroupCount;
  layout_desc.bindGroupLayouts = layouts;
  WGPUPipelineLayout layout = wgpuDeviceCreatePipelineLayout(device, &layout_desc);

  WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
  desc.label = View("Guest draw");
  desc.layout = layout;
  desc.vertex.module = vertex_module;
  desc.vertex.entryPoint = View("main");
  desc.primitive.topology = WGPUPrimitiveTopology(key.topology);
  desc.primitive.stripIndexFormat = WGPUIndexFormat(key.strip_index_format);
  desc.primitive.cullMode = WGPUCullMode(key.cull_mode);
  desc.primitive.frontFace = WGPUFrontFace(key.front_face);

  WGPUDepthStencilState depth_stencil = WGPU_DEPTH_STENCIL_STATE_INIT;
  if (key.depth_format) {
    depth_stencil.format = WGPUTextureFormat(key.depth_format);
    depth_stencil.depthWriteEnabled =
        key.depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
    depth_stencil.depthCompare = ToCompare(key.depth_compare);
    WGPUStencilFaceState* faces[2] = {&depth_stencil.stencilFront, &depth_stencil.stencilBack};
    for (uint32_t i = 0; i < 2; ++i) {
      if (key.stencil_enable) {
        faces[i]->compare = ToCompare(key.stencil_ops[i][0]);
        faces[i]->failOp = ToStencilOp(key.stencil_ops[i][1]);
        faces[i]->depthFailOp = ToStencilOp(key.stencil_ops[i][2]);
        faces[i]->passOp = ToStencilOp(key.stencil_ops[i][3]);
      } else {
        faces[i]->compare = WGPUCompareFunction_Always;
        faces[i]->failOp = faces[i]->depthFailOp = faces[i]->passOp = WGPUStencilOperation_Keep;
      }
    }
    depth_stencil.stencilReadMask = key.stencil_enable ? key.stencil_read_mask : 0xFF;
    depth_stencil.stencilWriteMask = key.stencil_enable ? key.stencil_write_mask : 0;
    desc.depthStencil = &depth_stencil;
  }

  WGPUColorTargetState targets[xenos::kMaxColorRenderTargets];
  WGPUBlendState blends[xenos::kMaxColorRenderTargets];
  uint32_t target_count = 0;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    targets[i] = WGPU_COLOR_TARGET_STATE_INIT;
    targets[i].format = WGPUTextureFormat(key.color_formats[i]);
    if (!key.color_formats[i]) {
      continue;
    }
    target_count = i + 1;
    targets[i].writeMask = key.color_write_masks[i];
    uint32_t blend_control = key.blend_controls[i];
    if (blend_control != 0x00010001 && IsBlendable(targets[i].format)) {
      reg::RB_BLENDCONTROL control;
      control.value = blend_control;
      blends[i].color =
          ToBlendComponent(uint32_t(control.color_srcblend), uint32_t(control.color_comb_fcn),
                           uint32_t(control.color_destblend));
      blends[i].alpha =
          ToBlendComponent(uint32_t(control.alpha_srcblend), uint32_t(control.alpha_comb_fcn),
                           uint32_t(control.alpha_destblend));
      targets[i].blend = &blends[i];
    }
  }
  WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
  fragment.module = pixel_module;
  fragment.entryPoint = View("main");
  fragment.targetCount = target_count;
  fragment.targets = targets;
  desc.fragment = &fragment;

  wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
  WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(device, &desc);
  WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
  callback.mode = ui::webgpu::kDeviceThreadCallbackMode;
  callback.userdata1 = reinterpret_cast<void*>(uintptr_t(key.vertex_shader_hash));
  callback.userdata2 = reinterpret_cast<void*>(uintptr_t(key.pixel_shader_hash));
  callback.callback = [](WGPUPopErrorScopeStatus, WGPUErrorType type, WGPUStringView message,
                         void* userdata1, void* userdata2) {
    if (type != WGPUErrorType_NoError) {
      REXGPU_ERROR("WebGPU: render pipeline for shaders {:016X}/{:016X} failed: {}",
                   uint64_t(uintptr_t(userdata1)), uint64_t(uintptr_t(userdata2)),
                   std::string_view(message.data ? message.data : "",
                                    message.data && message.length != WGPU_STRLEN
                                        ? message.length
                                        : (message.data ? std::strlen(message.data) : 0)));
    }
  };
  wgpuDevicePopErrorScope(device, callback);
  wgpuPipelineLayoutRelease(layout);
  return pipeline;
}

}  // namespace rex::graphics::webgpu
