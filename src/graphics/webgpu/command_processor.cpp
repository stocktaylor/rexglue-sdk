/**
 * @file        graphics/webgpu/command_processor.cpp
 * @brief       Command processor for the WebGPU GPU backend
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/command_processor.h>

#include <algorithm>
#include <array>
#include <cstring>

#include <rex/graphics/graphics_system.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/webgpu/graphics_system.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/webgpu/presenter.h>
#include <rex/ui/webgpu/provider.h>

namespace rex::graphics::webgpu {

using SystemConstants = WgslShaderTranslator::SystemConstants;

WebGPUCommandProcessor::WebGPUCommandProcessor(WebGPUGraphicsSystem* graphics_system,
                                               system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {
  std::memset(&system_constants_, 0, sizeof(system_constants_));
}

WebGPUCommandProcessor::~WebGPUCommandProcessor() = default;

bool WebGPUCommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) {
    return false;
  }
  auto* provider = static_cast<ui::webgpu::WebGPUProvider*>(graphics_system_->provider());
  if (!provider) {
    REXGPU_ERROR("WebGPU: no provider");
    return false;
  }
  device_thread_ = &provider->device_thread();
  command_list_ = std::make_unique<WebGPUCommandList>();

  shared_memory_ = std::make_unique<WebGPUSharedMemory>(*this, *memory_);
  if (!shared_memory_->Initialize()) {
    REXGPU_ERROR("WebGPU: couldn't initialize shared memory");
    return false;
  }
  primitive_processor_ =
      std::make_unique<WebGPUPrimitiveProcessor>(*register_file_, *memory_, *shared_memory_, *this);
  if (!primitive_processor_->Initialize()) {
    REXGPU_ERROR("WebGPU: couldn't initialize the primitive processor");
    return false;
  }
  pipeline_cache_ = std::make_unique<WebGPUPipelineCache>(*this, *register_file_);
  if (!pipeline_cache_->Initialize()) {
    return false;
  }
  render_target_cache_ = std::make_unique<WebGPURenderTargetCache>(*this);
  texture_cache_ = std::make_unique<WebGPUTextureCache>(*register_file_, *shared_memory_, *this);
  if (!texture_cache_->Initialize()) {
    return false;
  }
  texture_cache_->BeginSubmission(1);

  device_thread_->Call([&] {
    WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
    entry.binding = 0;
    entry.buffer = shared_memory_->buffer();
    entry.size = SharedMemory::kBufferSize;
    WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    desc.layout = pipeline_cache_->shared_memory_layout();
    desc.entryCount = 1;
    desc.entries = &entry;
    shared_memory_bind_group_ = wgpuDeviceCreateBindGroup(device_thread_->device().device(), &desc);
  });
  return shared_memory_bind_group_ != nullptr;
}

void WebGPUCommandProcessor::ShutdownContext() {
  if (device_thread_ && command_list_) {
    Flush();
    AwaitSubmissions(0);
    // Textures release after the commands recorded before them.
    texture_cache_.reset();
    Flush();
    AwaitSubmissions(0);
  }
  if (device_thread_) {
    WGPUBindGroup bind_group = shared_memory_bind_group_;
    WGPUTexture resolved = resolved_texture_;
    WGPURenderPipeline swap_pipeline = swap_pipeline_;
    swap_pipeline_ = nullptr;
    device_thread_->Post([bind_group, resolved, swap_pipeline] {
      if (swap_pipeline) {
        wgpuRenderPipelineRelease(swap_pipeline);
      }
      if (bind_group) {
        wgpuBindGroupRelease(bind_group);
      }
      if (resolved) {
        wgpuTextureRelease(resolved);
      }
    });
  }
  shared_memory_bind_group_ = nullptr;
  resolved_texture_ = nullptr;
  texture_cache_.reset();
  render_target_cache_.reset();
  pipeline_cache_.reset();
  primitive_processor_.reset();
  shared_memory_.reset();
  shaders_.clear();
  command_list_.reset();
  CommandProcessor::ShutdownContext();
}

void WebGPUCommandProcessor::ReleaseAfterRecordedCommands(std::function<void()> release) {
  if (command_list_) {
    command_list_->Record([release](WebGPUCommandList::Encoder&) { release(); });
  } else if (device_thread_) {
    device_thread_->Post(std::move(release));
  }
}

void WebGPUCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  CommandProcessor::WriteRegister(index, value);
  if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
      index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5 && texture_cache_) {
    texture_cache_->TextureFetchConstantWritten((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) /
                                                6);
  }
}

void WebGPUCommandProcessor::EndRenderPass() {
  if (!render_pass_open_) {
    return;
  }
  render_pass_open_ = false;
  command_list_->Record([](WebGPUCommandList::Encoder& encoder) {
    if (encoder.pass) {
      wgpuRenderPassEncoderEnd(encoder.pass);
      wgpuRenderPassEncoderRelease(encoder.pass);
      encoder.pass = nullptr;
    }
  });
}

void WebGPUCommandProcessor::BeginRenderPass(RenderTarget* const* colors, RenderTarget* depth) {
  if (render_pass_open_ && pass_depth_ == depth &&
      std::equal(colors, colors + xenos::kMaxColorRenderTargets, pass_colors_)) {
    return;
  }
  EndRenderPass();
  std::copy(colors, colors + xenos::kMaxColorRenderTargets, pass_colors_);
  pass_depth_ = depth;
  render_pass_open_ = true;
  WGPUTextureView color_views[xenos::kMaxColorRenderTargets] = {};
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    color_views[i] = colors[i] ? colors[i]->view : nullptr;
  }
  WGPUTextureView depth_view = depth ? depth->view : nullptr;
  command_list_->Record([=](WebGPUCommandList::Encoder& encoder) {
    WGPURenderPassColorAttachment attachments[xenos::kMaxColorRenderTargets];
    uint32_t attachment_count = 0;
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      attachments[i] = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
      attachments[i].view = color_views[i];
      if (color_views[i]) {
        attachments[i].loadOp = WGPULoadOp_Load;
        attachments[i].storeOp = WGPUStoreOp_Store;
        attachment_count = i + 1;
      }
    }
    WGPURenderPassDepthStencilAttachment depth_attachment =
        WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    depth_attachment.view = depth_view;
    depth_attachment.depthLoadOp = WGPULoadOp_Load;
    depth_attachment.depthStoreOp = WGPUStoreOp_Store;
    depth_attachment.stencilLoadOp = WGPULoadOp_Load;
    depth_attachment.stencilStoreOp = WGPUStoreOp_Store;
    WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    desc.colorAttachmentCount = attachment_count;
    desc.colorAttachments = attachments;
    desc.depthStencilAttachment = depth_view ? &depth_attachment : nullptr;
    encoder.pass = wgpuCommandEncoderBeginRenderPass(encoder.encoder, &desc);
  });
}

void WebGPUCommandProcessor::ClearRenderTarget(RenderTarget* render_target, const double color[4],
                                               float depth, uint32_t stencil) {
  if (!render_target) {
    return;
  }
  EndRenderPass();
  WGPUTextureView view = render_target->view;
  bool is_depth = render_target->is_depth;
  WGPUColor clear_color = {color[0], color[1], color[2], color[3]};
  command_list_->Record([=](WebGPUCommandList::Encoder& encoder) {
    WGPURenderPassColorAttachment color_attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    WGPURenderPassDepthStencilAttachment depth_attachment =
        WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    if (is_depth) {
      depth_attachment.view = view;
      depth_attachment.depthLoadOp = WGPULoadOp_Clear;
      depth_attachment.depthStoreOp = WGPUStoreOp_Store;
      depth_attachment.depthClearValue = depth;
      depth_attachment.stencilLoadOp = WGPULoadOp_Clear;
      depth_attachment.stencilStoreOp = WGPUStoreOp_Store;
      depth_attachment.stencilClearValue = stencil;
      desc.depthStencilAttachment = &depth_attachment;
    } else {
      color_attachment.view = view;
      color_attachment.loadOp = WGPULoadOp_Clear;
      color_attachment.storeOp = WGPUStoreOp_Store;
      color_attachment.clearValue = clear_color;
      desc.colorAttachmentCount = 1;
      desc.colorAttachments = &color_attachment;
    }
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder.encoder, &desc);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
  });
}

void WebGPUCommandProcessor::Flush() {
  EndRenderPass();
  primitive_processor_->EndSubmission();
  for (bool& valid : last_constants_valid_) {
    valid = false;
  }
  if (command_list_->empty()) {
    return;
  }
  statistics_upload_bytes_ += command_list_->upload_bytes();
  // The list moves to the device thread; a fresh one takes its place.
  auto list = std::shared_ptr<WebGPUCommandList>(command_list_.release());
  command_list_ = std::make_unique<WebGPUCommandList>();
  ++submissions_flushed_;
  texture_cache_->BeginSubmission(submissions_flushed_ + 1);
  WGPUBindGroupLayout constants_layout = pipeline_cache_->constants_layout();
  device_thread_->Post([this, list, constants_layout] {
    const ui::webgpu::WebGPUDevice& device = device_thread_->device();
    list->Execute(device.device(), device.queue(), constants_layout,
                  WgslShaderTranslator::kConstantBufferCount);
    WGPUQueueWorkDoneCallbackInfo callback = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
    callback.mode = ui::webgpu::kDeviceThreadCallbackMode;
    callback.userdata1 = this;
    callback.callback = [](WGPUQueueWorkDoneStatus, WGPUStringView, void* userdata1, void*) {
      auto* self = static_cast<WebGPUCommandProcessor*>(userdata1);
      {
        std::lock_guard<std::mutex> lock(self->submission_mutex_);
        ++self->submissions_completed_;
      }
      self->submission_completed_.notify_all();
    };
    wgpuQueueOnSubmittedWorkDone(device.queue(), callback);
  });
}

void WebGPUCommandProcessor::AwaitSubmissions(uint64_t max_pending) {
  std::unique_lock<std::mutex> lock(submission_mutex_);
  submission_completed_.wait(
      lock, [&] { return submissions_flushed_ - submissions_completed_.load() <= max_pending; });
}

Shader* WebGPUCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                           const uint32_t* host_address, uint32_t dword_count) {
  (void)guest_address;
  uint64_t data_hash = XXH3_64bits(host_address, dword_count * sizeof(uint32_t));
  auto it = shaders_.find(data_hash);
  if (it != shaders_.end()) {
    return it->second.get();
  }
  auto shader = std::make_unique<WgslShader>(shader_type, data_hash, host_address, dword_count);
  Shader* result = shader.get();
  shaders_.emplace(data_hash, std::move(shader));
  return result;
}

bool WebGPUCommandProcessor::RequestVertexBuffers(const WgslShader& vertex_shader) {
  const RegisterFile& regs = *register_file_;
  const Shader::ConstantRegisterMap& constant_map = vertex_shader.constant_register_map();
  for (uint32_t i = 0; i < rex::countof(constant_map.vertex_fetch_bitmap); ++i) {
    uint32_t vfetch_bits_remaining = constant_map.vertex_fetch_bitmap[i];
    uint32_t j;
    while (rex::bit_scan_forward(vfetch_bits_remaining, &j)) {
      vfetch_bits_remaining &= ~(uint32_t(1) << j);
      uint32_t vfetch_index = i * 32 + j;
      xenos::xe_gpu_vertex_fetch_t vfetch_constant = regs.GetVertexFetch(vfetch_index);
      if (vfetch_constant.type != xenos::FetchConstantType::kVertex) {
        // As with --gpu_allow_invalid_fetch_constants on Vulkan, for now.
        continue;
      }
      if (!shared_memory_->RequestRange(vfetch_constant.address << 2, vfetch_constant.size << 2)) {
        REXGPU_ERROR("WebGPU: couldn't request the vertex buffer at 0x{:08X} ({} bytes)",
                     vfetch_constant.address << 2, vfetch_constant.size << 2);
        return false;
      }
    }
  }
  return true;
}

uint32_t WebGPUCommandProcessor::WriteConstants(uint32_t buffer, const void* data, size_t size) {
  std::vector<uint8_t>& last = last_constants_[buffer];
  if (last_constants_valid_[buffer] && last.size() == size &&
      !std::memcmp(last.data(), data, size)) {
    return last_constants_offsets_[buffer];
  }
  uint32_t offset;
  void* mapping = command_list_->AllocateUniforms(std::max(size, size_t(16)), offset);
  std::memcpy(mapping, data, size);
  last.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
  last_constants_offsets_[buffer] = offset;
  last_constants_valid_[buffer] = true;
  return offset;
}

void WebGPUCommandProcessor::UpdateSystemConstants(
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    bool primitive_polygonal, const draw_util::ViewportInfo& viewport_info) {
  const RegisterFile& regs = *register_file_;
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto vgt_dma_size = regs.Get<reg::VGT_DMA_SIZE>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  SystemConstants& constants = system_constants_;

  uint32_t flags = 0;
  using PIBT = PrimitiveProcessor::ProcessedIndexBufferType;
  if (primitive_processing_result.index_buffer_type == PIBT::kHostBuiltinForDMA ||
      primitive_processing_result.index_buffer_type == PIBT::kGuestDMA ||
      primitive_processing_result.index_buffer_type == PIBT::kHostConverted) {
    flags |= WgslShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
    if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32) {
      flags |= WgslShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
    }
  }
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= WgslShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= WgslShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= WgslShaderTranslator::kSysFlag_WNotReciprocal;
  }
  if (primitive_polygonal) {
    flags |= WgslShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= WgslShaderTranslator::kSysFlag_PrimitiveLine;
  }
  flags |= uint32_t(rb_surface_info.msaa_samples)
           << WgslShaderTranslator::kSysFlag_MsaaSamples_Shift;
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= WgslShaderTranslator::kSysFlag_DepthFloat24;
  }
  xenos::CompareFunction alpha_test_function = rb_colorcontrol.alpha_test_enable
                                                   ? rb_colorcontrol.alpha_func
                                                   : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function) << WgslShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    if (color_info.color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
      flags |= WgslShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
    }
    int32_t color_exp_bias = color_info.color_exp_bias;
    uint32_t color_exp_bias_bits = UINT32_C(0x3F800000) + uint32_t(color_exp_bias << 23);
    std::memcpy(&constants.color_exp_bias[i], &color_exp_bias_bits, sizeof(float));
  }
  constants.flags = flags;

  if (flags & WgslShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad) {
    constants.vertex_index_load_address = primitive_processing_result.guest_index_base;
  }
  xenos::Endian guest_index_endian = vgt_dma_size.swap_mode;
  if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16 &&
      guest_index_endian != xenos::Endian::kNone && guest_index_endian != xenos::Endian::k8in16) {
    guest_index_endian =
        guest_index_endian == xenos::Endian::k8in32 ? xenos::Endian::k8in16 : xenos::Endian::kNone;
  }
  constants.vertex_index_endian =
      (flags & WgslShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad)
          ? guest_index_endian
          : primitive_processing_result.host_shader_index_endian;
  constants.line_loop_closing_index = primitive_processing_result.line_loop_closing_index;
  constants.vertex_base_index = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);
  constants.vertex_index_min = regs.Get<uint32_t>(XE_GPU_REG_VGT_MIN_VTX_INDX);
  constants.vertex_index_max = regs.Get<uint32_t>(XE_GPU_REG_VGT_MAX_VTX_INDX);

  if (!pa_cl_clip_cntl.clip_disable) {
    float* user_clip_plane_write_ptr = constants.user_clip_planes[0];
    uint32_t user_clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t user_clip_plane_index;
    while (rex::bit_scan_forward(user_clip_planes_remaining, &user_clip_plane_index)) {
      user_clip_planes_remaining &= ~(UINT32_C(1) << user_clip_plane_index);
      std::memcpy(user_clip_plane_write_ptr,
                  &regs[XE_GPU_REG_PA_CL_UCP_0_X + user_clip_plane_index * 4], 4 * sizeof(float));
      user_clip_plane_write_ptr += 4;
    }
  }
  for (uint32_t i = 0; i < 3; ++i) {
    constants.ndc_scale[i] = viewport_info.ndc_scale[i];
    constants.ndc_offset[i] = viewport_info.ndc_offset[i];
  }
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    constants.point_vertex_diameter_min = float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    constants.point_vertex_diameter_max = float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    constants.point_constant_diameter[0] = float(pa_su_point_size.width) * (2.0f / 16.0f);
    constants.point_constant_diameter[1] = float(pa_su_point_size.height) * (2.0f / 16.0f);
    constants.point_screen_diameter_to_ndc_radius[0] =
        1.0f / float(std::max(viewport_info.xy_extent[0], uint32_t(1)));
    constants.point_screen_diameter_to_ndc_radius[1] =
        1.0f / float(std::max(viewport_info.xy_extent[1], uint32_t(1)));
  }
  // Texture signs and swizzles (the guest's and the host format's; WebGPU
  // views can't swizzle).
  constants.textures_resolution_scaled = 0;
  std::memset(constants.texture_swizzled_signs, 0, sizeof(constants.texture_swizzled_signs));
  std::memset(constants.texture_swizzles, 0, sizeof(constants.texture_swizzles));
  for (uint32_t i = 0; i < xenos::kTextureFetchConstantCount; ++i) {
    constants.texture_swizzled_signs[i >> 2] |=
        uint32_t(texture_cache_->GetActiveTextureSwizzledSigns(i)) << (8 * (i & 3));
    constants.texture_swizzles[i >> 1] |= texture_cache_->GetActiveTextureHostSwizzle(i)
                                          << (12 * (i & 1));
  }
  constants.alpha_test_reference = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  constants.alpha_to_mask =
      rb_colorcontrol.alpha_to_mask_enable ? (rb_colorcontrol.value >> 24) | (UINT32_C(1) << 8) : 0;
}

bool WebGPUCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                       IndexBufferInfo* index_buffer_info,
                                       bool major_mode_explicit) {
  (void)prim_type;
  (void)index_count;
  (void)index_buffer_info;
  (void)major_mode_explicit;
  const RegisterFile& regs = *register_file_;
  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    return IssueCopy();
  }
  ++statistics_draws_;

  auto* vertex_shader = static_cast<WgslShader*>(active_vertex_shader());
  if (!vertex_shader) {
    return false;
  }
  // Memory export isn't translated yet, and memexport-only draws are skipped.
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  if (!draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal)) {
    return true;
  }
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  if (!rb_surface_info.surface_pitch) {
    return true;
  }
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  if (primitive_polygonal && pa_su_sc_mode_cntl.cull_front && pa_su_sc_mode_cntl.cull_back) {
    return true;
  }

  WgslShader* pixel_shader = nullptr;
  if (edram_mode == xenos::EdramMode::kColorDepth) {
    pixel_shader = static_cast<WgslShader*>(active_pixel_shader());
  }
  string::StringBuffer ucode_disasm_buffer;
  if (!vertex_shader->is_ucode_analyzed()) {
    vertex_shader->AnalyzeUcode(ucode_disasm_buffer);
  }
  if (pixel_shader) {
    if (!pixel_shader->is_ucode_analyzed()) {
      pixel_shader->AnalyzeUcode(ucode_disasm_buffer);
    }
    if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {
      pixel_shader = nullptr;
    }
  }

  reg::RB_DEPTHCONTROL normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);
  uint32_t normalized_color_mask =
      pixel_shader ? draw_util::GetNormalizedColorMask(regs, pixel_shader->writes_color_targets())
                   : 0;
  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      pixel_shader ? (vertex_shader->writes_interpolators() &
                      pixel_shader->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                                             regs.Get<reg::SQ_CONTEXT_MISC>(),
                                                             ps_param_gen_pos))
                   : 0;

  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  if (!primitive_processor_->Process(primitive_processing_result)) {
    return false;
  }
  if (!primitive_processing_result.host_draw_vertex_count) {
    return true;
  }

  // Shaders.
  auto vertex_shader_modification = pipeline_cache_->GetCurrentVertexShaderModification(
      *vertex_shader, primitive_processing_result.host_vertex_shader_type, interpolator_mask,
      pipeline_cache_->GetInterpolatorsCentroid(interpolator_mask), ps_param_gen_pos != UINT32_MAX);
  auto* vertex_translation = static_cast<WgslShader::WgslTranslation*>(
      vertex_shader->GetOrCreateTranslation(vertex_shader_modification.value));
  if (!pipeline_cache_->EnsureTranslated(*vertex_translation)) {
    return false;
  }
  WgslShader::WgslTranslation* pixel_translation = nullptr;
  WgslShaderTranslator::Modification pixel_shader_modification;
  if (pixel_shader) {
    pixel_shader_modification = pipeline_cache_->GetCurrentPixelShaderModification(
        *pixel_shader, interpolator_mask, ps_param_gen_pos);
    pixel_translation = static_cast<WgslShader::WgslTranslation*>(
        pixel_shader->GetOrCreateTranslation(pixel_shader_modification.value));
    if (!pipeline_cache_->EnsureTranslated(*pixel_translation)) {
      return false;
    }
  }

  // Render targets.
  RenderTarget* colors[xenos::kMaxColorRenderTargets] = {};
  RenderTarget* depth = nullptr;
  uint32_t pitch = rb_surface_info.surface_pitch;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (!((normalized_color_mask >> (i * 4)) & 0b1111)) {
      continue;
    }
    auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    colors[i] =
        render_target_cache_->GetColor(color_info.color_base, pitch, color_info.color_format);
  }
  if (normalized_depth_control.z_enable || normalized_depth_control.stencil_enable) {
    auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
    depth = render_target_cache_->GetDepth(depth_info.depth_base, pitch, depth_info.depth_format);
  }
  RenderTarget* any_target = depth;
  for (RenderTarget* color : colors) {
    any_target = any_target ? any_target : color;
  }
  if (!any_target) {
    // Nothing to draw to (WebGPU passes need an attachment).
    return true;
  }

  // Pipeline.
  WebGPUPipelineCache::PipelineKey key;
  std::memset(&key, 0, sizeof(key));
  key.vertex_shader_hash = vertex_shader->ucode_data_hash();
  key.vertex_shader_modification = vertex_shader_modification.value;
  if (pixel_shader) {
    key.pixel_shader_hash = pixel_shader->ucode_data_hash();
    key.pixel_shader_modification = pixel_shader_modification.value;
  }
  bool indexed = primitive_processing_result.index_buffer_type !=
                 PrimitiveProcessor::ProcessedIndexBufferType::kNone;
  WGPUIndexFormat index_format =
      primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16
          ? WGPUIndexFormat_Uint16
          : WGPUIndexFormat_Uint32;
  switch (primitive_processing_result.host_primitive_type) {
    case xenos::PrimitiveType::kPointList:
      key.topology = WGPUPrimitiveTopology_PointList;
      break;
    case xenos::PrimitiveType::kLineList:
      key.topology = WGPUPrimitiveTopology_LineList;
      break;
    case xenos::PrimitiveType::kLineStrip:
      key.topology = WGPUPrimitiveTopology_LineStrip;
      break;
    case xenos::PrimitiveType::kTriangleList:
    case xenos::PrimitiveType::kRectangleList:
      key.topology = WGPUPrimitiveTopology_TriangleList;
      break;
    case xenos::PrimitiveType::kTriangleStrip:
      key.topology = WGPUPrimitiveTopology_TriangleStrip;
      break;
    default:
      return false;
  }
  if (indexed && (key.topology == WGPUPrimitiveTopology_LineStrip ||
                  key.topology == WGPUPrimitiveTopology_TriangleStrip)) {
    key.strip_index_format = index_format;
  }
  key.cull_mode = WGPUCullMode_None;
  key.front_face = WGPUFrontFace_CCW;
  if (primitive_polygonal) {
    if (pa_su_sc_mode_cntl.cull_front) {
      key.cull_mode = WGPUCullMode_Front;
    } else if (pa_su_sc_mode_cntl.cull_back) {
      key.cull_mode = WGPUCullMode_Back;
    }
    // Y is up in WebGPU's normalized device coordinates, as in Direct3D.
    key.front_face = pa_su_sc_mode_cntl.face ? WGPUFrontFace_CW : WGPUFrontFace_CCW;
  }
  if (depth) {
    key.depth_format = depth->format;
    if (normalized_depth_control.z_enable) {
      key.depth_write = normalized_depth_control.z_write_enable;
      key.depth_compare = uint32_t(normalized_depth_control.zfunc);
    } else {
      key.depth_compare = uint32_t(xenos::CompareFunction::kAlways);
    }
    if (normalized_depth_control.stencil_enable) {
      key.stencil_enable = 1;
      key.stencil_ops[0][0] = uint32_t(normalized_depth_control.stencilfunc);
      key.stencil_ops[0][1] = uint32_t(normalized_depth_control.stencilfail);
      key.stencil_ops[0][2] = uint32_t(normalized_depth_control.stencilzfail);
      key.stencil_ops[0][3] = uint32_t(normalized_depth_control.stencilzpass);
      if (primitive_polygonal && normalized_depth_control.backface_enable) {
        key.stencil_ops[1][0] = uint32_t(normalized_depth_control.stencilfunc_bf);
        key.stencil_ops[1][1] = uint32_t(normalized_depth_control.stencilfail_bf);
        key.stencil_ops[1][2] = uint32_t(normalized_depth_control.stencilzfail_bf);
        key.stencil_ops[1][3] = uint32_t(normalized_depth_control.stencilzpass_bf);
      } else {
        std::memcpy(key.stencil_ops[1], key.stencil_ops[0], sizeof(key.stencil_ops[0]));
      }
      auto stencil_ref_mask = regs.Get<reg::RB_STENCILREFMASK>();
      key.stencil_read_mask = stencil_ref_mask.stencilmask;
      key.stencil_write_mask = stencil_ref_mask.stencilwritemask;
    }
  }
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (!colors[i]) {
      continue;
    }
    key.color_formats[i] = colors[i]->format;
    key.color_write_masks[i] = (normalized_color_mask >> (i * 4)) & 0b1111;
    key.blend_controls[i] = regs[reg::RB_BLENDCONTROL::rt_register_indices[i]] & 0x1FFF1FFF;
  }
  WebGPUPipelineCache::DrawObjects objects;
  if (!pipeline_cache_->GetDrawObjects(key, *vertex_translation, pixel_translation, objects)) {
    return false;
  }

  // Viewport and scissor, clamped to the targets.
  draw_util::ViewportInfo viewport_info;
  draw_util::GetHostViewportInfo(regs, 1, 1, true, any_target->width, any_target->height, false,
                                 normalized_depth_control, false, true,
                                 pixel_shader && pixel_shader->writes_depth(), viewport_info);
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  scissor.offset[0] = std::min(scissor.offset[0], any_target->width);
  scissor.offset[1] = std::min(scissor.offset[1], any_target->height);
  scissor.extent[0] = std::min(scissor.extent[0], any_target->width - scissor.offset[0]);
  scissor.extent[1] = std::min(scissor.extent[1], any_target->height - scissor.offset[1]);

  // Textures first: the system constants take their swizzles and signs.
  uint32_t used_texture_mask = vertex_shader->GetUsedTextureMaskAfterTranslation();
  if (pixel_shader) {
    used_texture_mask |= pixel_shader->GetUsedTextureMaskAfterTranslation();
  }
  texture_cache_->RequestTextures(used_texture_mask);

  // Constants.
  UpdateSystemConstants(primitive_processing_result, primitive_polygonal, viewport_info);
  uint32_t constant_offsets[WgslShaderTranslator::kConstantBufferCount];
  constant_offsets[WgslShaderTranslator::kConstantBufferSystem] = WriteConstants(
      WgslShaderTranslator::kConstantBufferSystem, &system_constants_, sizeof(system_constants_));
  for (uint32_t stage = 0; stage < 2; ++stage) {
    const Shader* shader = stage ? static_cast<const Shader*>(pixel_shader) : vertex_shader;
    uint32_t buffer = stage ? WgslShaderTranslator::kConstantBufferFloatPixel
                            : WgslShaderTranslator::kConstantBufferFloatVertex;
    float_constants_.clear();
    if (shader) {
      const Shader::ConstantRegisterMap& map = shader->constant_register_map();
      uint32_t register_base =
          stage ? XE_GPU_REG_SHADER_CONSTANT_256_X : XE_GPU_REG_SHADER_CONSTANT_000_X;
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t bits = map.float_bitmap[i];
        uint32_t constant_index;
        while (rex::bit_scan_forward(bits, &constant_index)) {
          bits &= ~(uint64_t(1) << constant_index);
          const auto* source = reinterpret_cast<const uint8_t*>(
              &regs[register_base + (i << 8) + (constant_index << 2)]);
          float_constants_.insert(float_constants_.end(), source, source + 4 * sizeof(float));
        }
      }
    }
    if (float_constants_.empty()) {
      float_constants_.resize(16);
    }
    constant_offsets[buffer] =
        WriteConstants(buffer, float_constants_.data(), float_constants_.size());
  }
  constant_offsets[WgslShaderTranslator::kConstantBufferBoolLoop] =
      WriteConstants(WgslShaderTranslator::kConstantBufferBoolLoop,
                     &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], (8 + 32) * sizeof(uint32_t));
  constant_offsets[WgslShaderTranslator::kConstantBufferFetch] =
      WriteConstants(WgslShaderTranslator::kConstantBufferFetch,
                     &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], 6 * 32 * sizeof(uint32_t));

  // Textures and the bindings to them (loads end passes too).
  struct StageTextures {
    WGPUBindGroupLayout layout = nullptr;
    std::vector<WGPUTextureView> views;
    std::vector<WGPUSampler> samplers;
  };
  auto stage_textures = std::make_shared<std::array<StageTextures, 2>>();
  for (uint32_t stage = 0; stage < 2; ++stage) {
    const WgslShader* shader = stage ? pixel_shader : vertex_shader;
    StageTextures& textures = (*stage_textures)[stage];
    textures.layout = stage ? objects.textures_pixel_layout : objects.textures_vertex_layout;
    if (!shader || !textures.layout) {
      textures.layout = nullptr;
      continue;
    }
    for (const auto& binding : shader->GetTextureBindingsAfterTranslation()) {
      textures.views.push_back(texture_cache_->GetActiveBindingOrNullView(
          binding.fetch_constant, binding.dimension, binding.is_signed));
    }
    for (const auto& binding : shader->GetSamplerBindingsAfterTranslation()) {
      textures.samplers.push_back(texture_cache_->GetSampler(binding));
    }
  }

  // Memory the draw reads, uploaded before the pass (copies end passes).
  if (!RequestVertexBuffers(*vertex_shader)) {
    return false;
  }

  // The index buffer.
  WGPUBuffer index_buffer = nullptr;
  uint64_t index_offset = 0;
  bool index_from_staging = false;
  switch (primitive_processing_result.index_buffer_type) {
    case PrimitiveProcessor::ProcessedIndexBufferType::kNone:
      break;
    case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA:
      index_buffer = shared_memory_->buffer();
      index_offset = primitive_processing_result.guest_index_base;
      break;
    case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
      index_from_staging = true;
      index_offset = primitive_processor_->GetConvertedIndexBufferOffset(
          primitive_processing_result.host_index_buffer_handle);
      break;
    case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
    case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA: {
      auto builtin = primitive_processor_->GetBuiltinIndexBuffer(
          primitive_processing_result.host_index_buffer_handle);
      index_buffer = builtin.first;
      index_offset = builtin.second;
    } break;
    default:
      return false;
  }

  BeginRenderPass(colors, depth);
  WGPURenderPipeline pipeline = objects.pipeline;
  WGPUBindGroup shared_memory_bind_group = shared_memory_bind_group_;
  WGPUBindGroup textures_empty = objects.textures_empty;
  uint32_t vertex_count = primitive_processing_result.host_draw_vertex_count;
  WGPUColor blend_constant = {
      regs.Get<float>(XE_GPU_REG_RB_BLEND_RED), regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
      regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE), regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA)};
  uint32_t stencil_reference = regs.Get<reg::RB_STENCILREFMASK>().stencilref;
  command_list_->Record([=](WebGPUCommandList::Encoder& encoder) {
    WGPURenderPassEncoder pass = encoder.pass;
    wgpuRenderPassEncoderSetPipeline(pass, pipeline);
    wgpuRenderPassEncoderSetBindGroup(pass, WgslShaderTranslator::kBindGroupSharedMemory,
                                      shared_memory_bind_group, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(pass, WgslShaderTranslator::kBindGroupConstants,
                                      encoder.constants, WgslShaderTranslator::kConstantBufferCount,
                                      constant_offsets);
    // Texture bind groups from the views and samplers chosen at recording.
    for (uint32_t stage = 0; stage < 2; ++stage) {
      const StageTextures& textures = (*stage_textures)[stage];
      uint32_t group = stage ? WgslShaderTranslator::kBindGroupTexturesPixel
                             : WgslShaderTranslator::kBindGroupTexturesVertex;
      if (!textures.layout) {
        wgpuRenderPassEncoderSetBindGroup(pass, group, textures_empty, 0, nullptr);
        continue;
      }
      std::vector<WGPUBindGroupEntry> entries;
      for (size_t i = 0; i < textures.views.size(); ++i) {
        WGPUBindGroupEntry& entry = entries.emplace_back(WGPU_BIND_GROUP_ENTRY_INIT);
        entry.binding = uint32_t(i);
        entry.textureView = textures.views[i];
      }
      for (size_t i = 0; i < textures.samplers.size(); ++i) {
        WGPUBindGroupEntry& entry = entries.emplace_back(WGPU_BIND_GROUP_ENTRY_INIT);
        entry.binding = uint32_t(textures.views.size() + i);
        entry.sampler = textures.samplers[i];
      }
      WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
      group_desc.layout = textures.layout;
      group_desc.entryCount = entries.size();
      group_desc.entries = entries.data();
      WGPUBindGroup bind_group = wgpuDeviceCreateBindGroup(encoder.device, &group_desc);
      wgpuRenderPassEncoderSetBindGroup(pass, group, bind_group, 0, nullptr);
      // The pass holds a reference.
      wgpuBindGroupRelease(bind_group);
    }
    wgpuRenderPassEncoderSetViewport(
        pass, float(viewport_info.xy_offset[0]), float(viewport_info.xy_offset[1]),
        float(viewport_info.xy_extent[0]), float(viewport_info.xy_extent[1]), viewport_info.z_min,
        viewport_info.z_max);
    wgpuRenderPassEncoderSetScissorRect(pass, scissor.offset[0], scissor.offset[1],
                                        scissor.extent[0], scissor.extent[1]);
    wgpuRenderPassEncoderSetBlendConstant(pass, &blend_constant);
    wgpuRenderPassEncoderSetStencilReference(pass, stencil_reference);
    if (index_buffer || index_from_staging) {
      wgpuRenderPassEncoderSetIndexBuffer(pass, index_from_staging ? encoder.staging : index_buffer,
                                          index_format, index_offset, WGPU_WHOLE_SIZE);
      wgpuRenderPassEncoderDrawIndexed(pass, vertex_count, 1, 0, 0, 0);
    } else {
      wgpuRenderPassEncoderDraw(pass, vertex_count, 1, 0, 0);
    }
  });
  ++statistics_draws_done_;
  return true;
}

bool WebGPUCommandProcessor::IssueCopy() {
  const RegisterFile& regs = *register_file_;
  draw_util::ResolveInfo resolve_info;
  if (!draw_util::GetResolveInfo(regs, *memory_, 1, 1, false, false, resolve_info)) {
    return false;
  }
  ++statistics_resolves_;
  uint32_t pitch = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch;
  if (!pitch) {
    return true;
  }
  reg::RB_COPY_CONTROL copy_control = resolve_info.rb_copy_control;
  RenderTarget* color = nullptr;
  if (copy_control.copy_src_select < xenos::kMaxColorRenderTargets) {
    auto color_info = regs.Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[copy_control.copy_src_select]);
    color = render_target_cache_->GetColor(color_info.color_base, pitch, color_info.color_format);
  }

  // Copy the area out (the source is often cleared right after).
  uint32_t width = resolve_info.coordinate_info.width_div_8 * 8;
  uint32_t height = resolve_info.height_div_8 * 8;
  if (color && width && height) {
    const draw_util::ResolveEdramInfo& edram = resolve_info.color_edram_info;
    uint32_t tile_offset = edram.base_tiles - resolve_info.color_original_base;
    uint32_t tile_width = xenos::kEdramTileWidthSamples >> uint32_t(edram.format_is_64bpp);
    uint32_t pitch_tiles = std::max(uint32_t(edram.pitch_tiles), uint32_t(1));
    uint32_t x = (tile_offset % pitch_tiles) * tile_width +
                 resolve_info.coordinate_info.edram_offset_x_div_8 * 8;
    uint32_t y = (tile_offset / pitch_tiles) * xenos::kEdramTileHeightSamples +
                 resolve_info.coordinate_info.edram_offset_y_div_8 * 8;
    if (x < color->width && y < color->height) {
      width = std::min(width, color->width - x);
      height = std::min(height, color->height - y);
      if (!resolved_texture_ || resolved_format_ != color->format || resolved_width_ != width ||
          resolved_height_ != height) {
        WGPUTexture old_texture = resolved_texture_;
        if (old_texture) {
          // Commands recorded earlier in this list may still use it.
          command_list_->Record(
              [old_texture](WebGPUCommandList::Encoder&) { wgpuTextureRelease(old_texture); });
        }
        device_thread_->Call([&] {
          WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
          desc.label = {"Resolved", WGPU_STRLEN};
          desc.size = {width, height, 1};
          desc.format = color->format;
          desc.usage =
              WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc | WGPUTextureUsage_TextureBinding;
          resolved_texture_ = wgpuDeviceCreateTexture(device_thread_->device().device(), &desc);
        });
        resolved_format_ = color->format;
        resolved_width_ = width;
        resolved_height_ = height;
      }
      EndRenderPass();
      WGPUTexture source = color->texture;
      WGPUTexture destination = resolved_texture_;
      command_list_->Record([=](WebGPUCommandList::Encoder& encoder) {
        WGPUTexelCopyTextureInfo source_info = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        source_info.texture = source;
        source_info.origin = {x, y, 0};
        WGPUTexelCopyTextureInfo destination_info = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        destination_info.texture = destination;
        WGPUExtent3D size = {width, height, 1};
        wgpuCommandEncoderCopyTextureToTexture(encoder.encoder, &source_info, &destination_info,
                                               &size);
      });
    }
  }

  // Clears (of the whole surface for now, not just the resolved area).
  if (copy_control.color_clear_enable && color) {
    uint32_t value = resolve_info.rb_color_clear;
    double clear_color[4] = {};
    if (color->format == WGPUTextureFormat_RGBA8Unorm) {
      for (uint32_t i = 0; i < 4; ++i) {
        clear_color[i] = double((value >> (8 * i)) & 0xFF) / 255.0;
      }
    } else if (color->format == WGPUTextureFormat_RGB10A2Unorm) {
      for (uint32_t i = 0; i < 3; ++i) {
        clear_color[i] = double((value >> (10 * i)) & 0x3FF) / 1023.0;
      }
      clear_color[3] = double(value >> 30) / 3.0;
    }
    ClearRenderTarget(color, clear_color, 0.0f, 0);
  }
  if (copy_control.depth_clear_enable) {
    auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
    RenderTarget* depth =
        render_target_cache_->GetDepth(depth_info.depth_base, pitch, depth_info.depth_format);
    uint32_t value = resolve_info.rb_depth_clear;
    float depth_value = depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8
                            ? xenos::Float20e4To32(value >> 8)
                            : float(value >> 8) / float(0xFFFFFF);
    const double no_color[4] = {};
    ClearRenderTarget(depth, no_color, std::clamp(depth_value, 0.0f, 1.0f), value & 0xFF);
  }
  return true;
}

void WebGPUCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
  (void)frontbuffer_ptr;
  // The resolved texture as of this point is what the refresher copies; a
  // later resolve can only replace it after the refresher has run.
  Flush();
  // Keep the GPU thread at most two frames ahead of the GPU.
  AwaitSubmissions(2);
  texture_cache_->CompletedSubmissionUpdated(submissions_completed_.load());
  texture_cache_->BeginFrame();

  if (++statistics_frames_ == kStatisticsFrames) {
    REXGPU_INFO(
        "WebGPU: {} frames: {} draws ({} drawn), {} resolves, {:.2f} MB uploaded, {} submissions "
        "completed",
        statistics_frames_, statistics_draws_, statistics_draws_done_, statistics_resolves_,
        double(statistics_upload_bytes_) / (1024.0 * 1024.0), submissions_completed_.load());
    statistics_frames_ = 0;
    statistics_draws_ = 0;
    statistics_draws_done_ = 0;
    statistics_resolves_ = 0;
    statistics_upload_bytes_ = 0;
  }

  ui::Presenter* presenter = graphics_system_ ? graphics_system_->presenter() : nullptr;
  if (!presenter) {
    return;
  }
  // The display gamma ramp of the guest: the 256-entry table for 8-bit
  // frontbuffers, the piecewise linear one for 10-bit ones.
  auto ramps = std::make_shared<SwapRamps>();
  static_assert(sizeof(ramps->table) == sizeof(reg::DC_LUT_30_COLOR) * 256);
  static_assert(sizeof(ramps->pwl) == sizeof(reg::DC_LUT_PWL_DATA) * 128 * 3);
  std::memcpy(ramps->table, gamma_ramp_256_entry_table(), sizeof(ramps->table));
  std::memcpy(ramps->pwl, gamma_ramp_pwl_rgb(), sizeof(ramps->pwl));
  ramps->use_pwl[0] = resolved_format_ == WGPUTextureFormat_RGB10A2Unorm;
  WGPUTexture resolved = resolved_texture_;
  uint32_t copy_width = std::min(resolved_width_, frontbuffer_width);
  uint32_t copy_height = std::min(resolved_height_, frontbuffer_height);
  presenter->RefreshGuestOutput(
      frontbuffer_width, frontbuffer_height, 1280, 720,
      [this, resolved, copy_width, copy_height,
       ramps](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& webgpu_context =
            static_cast<ui::webgpu::WebGPUPresenter::WebGPUGuestOutputRefreshContext&>(context);
        context.SetIs8bpc(!ramps->use_pwl[0]);
        WGPUDevice device = device_thread_->device().device();
        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
        WGPURenderPassColorAttachment attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        attachment.view = webgpu_context.view();
        attachment.loadOp = WGPULoadOp_Clear;
        attachment.storeOp = WGPUStoreOp_Store;
        attachment.clearValue = {0.0, 0.0, 0.0, 1.0};
        WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        pass_desc.colorAttachmentCount = 1;
        pass_desc.colorAttachments = &attachment;
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
        if (resolved && copy_width && copy_height && EnsureSwapPipeline()) {
          // The resolved area through the gamma ramp into the guest output.
          WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
          buffer_desc.size = sizeof(SwapRamps);
          buffer_desc.usage = WGPUBufferUsage_Uniform;
          buffer_desc.mappedAtCreation = true;
          WGPUBuffer ramp_buffer = wgpuDeviceCreateBuffer(device, &buffer_desc);
          std::memcpy(wgpuBufferGetMappedRange(ramp_buffer, 0, sizeof(SwapRamps)), ramps.get(),
                      sizeof(SwapRamps));
          wgpuBufferUnmap(ramp_buffer);
          WGPUTextureView source_view = wgpuTextureCreateView(resolved, nullptr);
          WGPUBindGroupEntry entries[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
          entries[0].binding = 0;
          entries[0].buffer = ramp_buffer;
          entries[0].size = sizeof(SwapRamps);
          entries[1].binding = 1;
          entries[1].textureView = source_view;
          WGPUBindGroupLayout layout = wgpuRenderPipelineGetBindGroupLayout(swap_pipeline_, 0);
          WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
          group_desc.layout = layout;
          group_desc.entryCount = 2;
          group_desc.entries = entries;
          WGPUBindGroup bind_group = wgpuDeviceCreateBindGroup(device, &group_desc);
          wgpuRenderPassEncoderSetPipeline(pass, swap_pipeline_);
          wgpuRenderPassEncoderSetBindGroup(pass, 0, bind_group, 0, nullptr);
          wgpuRenderPassEncoderSetScissorRect(pass, 0, 0, copy_width, copy_height);
          wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
          wgpuBindGroupRelease(bind_group);
          wgpuBindGroupLayoutRelease(layout);
          wgpuTextureViewRelease(source_view);
          wgpuBufferRelease(ramp_buffer);
        }
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
        WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
        wgpuQueueSubmit(device_thread_->device().queue(), 1, &commands);
        wgpuCommandBufferRelease(commands);
        wgpuCommandEncoderRelease(encoder);
        return true;
      });
}

bool WebGPUCommandProcessor::EnsureSwapPipeline() {
  if (swap_pipeline_) {
    return true;
  }
  // As the Vulkan backend's apply_gamma shaders (table and PWL), with a full
  // screen triangle.
  static const char kSwapShader[] = R"(
struct Ramps {
  table: array<vec4u, 64>,
  pwl: array<vec4u, 96>,
  use_pwl: vec4u,
}
@group(0) @binding(0) var<uniform> ramps: Ramps;
@group(0) @binding(1) var source: texture_2d<f32>;

fn table_entry(i: u32) -> u32 { return ramps.table[i >> 2u][i & 3u]; }

fn apply_pwl(value_10b: u32, channel: u32) -> f32 {
  let index = (value_10b >> 3u) * 3u + channel;
  let entry = ramps.pwl[index >> 2u][index & 3u];
  let value = f32(entry & 0xFFFFu) + f32((value_10b & 7u) * (entry >> 16u)) * 0.125;
  return clamp(value * 1.52737048e-05, 0.0, 1.0);
}

@vertex fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4f {
  let xy = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
  return vec4f(xy * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
}

@fragment fn fs_main(@builtin(position) position: vec4f) -> @location(0) vec4f {
  let color = textureLoad(source, vec2i(position.xy), 0);
  if (ramps.use_pwl.x != 0u) {
    let value = vec3u(color.rgb * 1023.0 + 0.5);
    return vec4f(apply_pwl(value.r, 0u), apply_pwl(value.g, 1u), apply_pwl(value.b, 2u), 1.0);
  }
  let value = vec3u(color.rgb * 255.0 + 0.5);
  // 10-bit blue, green, red in bits 0, 10, 20.
  return vec4f(f32((table_entry(value.r) >> 20u) & 0x3FFu),
               f32((table_entry(value.g) >> 10u) & 0x3FFu),
               f32(table_entry(value.b) & 0x3FFu), 1023.0) / 1023.0;
}
)";
  WGPUDevice device = device_thread_->device().device();
  WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
  source.code = {kSwapShader, WGPU_STRLEN};
  WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
  module_desc.nextInChain = &source.chain;
  WGPUShaderModule module = wgpuDeviceCreateShaderModule(device, &module_desc);
  WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
  target.format = ui::webgpu::WebGPUPresenter::kGuestOutputFormat;
  WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
  fragment.module = module;
  fragment.entryPoint = {"fs_main", WGPU_STRLEN};
  fragment.targetCount = 1;
  fragment.targets = &target;
  WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
  desc.label = {"Swap gamma ramp", WGPU_STRLEN};
  desc.vertex.module = module;
  desc.vertex.entryPoint = {"vs_main", WGPU_STRLEN};
  desc.fragment = &fragment;
  swap_pipeline_ = wgpuDeviceCreateRenderPipeline(device, &desc);
  wgpuShaderModuleRelease(module);
  return swap_pipeline_ != nullptr;
}

}  // namespace rex::graphics::webgpu
