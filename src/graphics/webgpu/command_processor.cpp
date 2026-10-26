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
#include <cstdio>
#include <cstring>

#include <rex/cvar.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/webgpu/graphics_system.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/webgpu/presenter.h>
#include <rex/ui/webgpu/provider.h>

REXCVAR_DEFINE_INT32(webgpu_capture_frame, 0, "GPU/WebGPU",
                     "Bring-up: save the guest output of this frame (counting from 1) as a BMP");
REXCVAR_DEFINE_STRING(webgpu_capture_path, "webgpu_frame.bmp", "GPU/WebGPU",
                      "Where webgpu_capture_frame saves the frame");

namespace rex::graphics::webgpu {

namespace {

void WriteBmp(const std::string& path, const ui::RawImage& image) {
  FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) {
    return;
  }
  uint32_t row = image.width * 3, padded = (row + 3) & ~3u;
  uint32_t data_size = padded * image.height;
  uint8_t header[54] = {'B', 'M'};
  auto put32 = [&](int at, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      header[at + i] = uint8_t(v >> (8 * i));
    }
  };
  put32(2, 54 + data_size);
  put32(10, 54);
  put32(14, 40);
  put32(18, image.width);
  put32(22, image.height);
  header[26] = 1;
  header[28] = 24;
  put32(34, data_size);
  std::fwrite(header, 1, 54, file);
  std::vector<uint8_t> line(padded, 0);
  for (uint32_t y = image.height; y-- > 0;) {
    const uint8_t* source = image.data.data() + image.stride * y;
    for (uint32_t x = 0; x < image.width; ++x) {
      line[x * 3 + 0] = source[x * 4 + 2];
      line[x * 3 + 1] = source[x * 4 + 1];
      line[x * 3 + 2] = source[x * 4 + 0];
    }
    std::fwrite(line.data(), 1, padded, file);
  }
  std::fclose(file);
}

}  // namespace

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
  }
  if (device_thread_) {
    WGPUBindGroup bind_group = shared_memory_bind_group_;
    WGPUTexture resolved = resolved_texture_;
    device_thread_->Post([bind_group, resolved] {
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
  render_target_cache_.reset();
  pipeline_cache_.reset();
  primitive_processor_.reset();
  shared_memory_.reset();
  shaders_.clear();
  command_list_.reset();
  CommandProcessor::ShutdownContext();
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
  // Placeholder textures until the texture cache: unsigned, identity swizzle.
  constants.textures_resolution_scaled = 0;
  std::memset(constants.texture_swizzled_signs, 0, sizeof(constants.texture_swizzled_signs));
  const uint32_t identity_swizzle = 0 | (1 << 3) | (2 << 6) | (3 << 9);
  for (uint32_t& swizzles : constants.texture_swizzles) {
    swizzles = identity_swizzle | (identity_swizzle << 12);
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
  WGPUBindGroup textures_vertex = objects.textures_vertex;
  WGPUBindGroup textures_pixel = objects.textures_pixel;
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
    wgpuRenderPassEncoderSetBindGroup(pass, WgslShaderTranslator::kBindGroupTexturesVertex,
                                      textures_vertex, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(pass, WgslShaderTranslator::kBindGroupTexturesPixel,
                                      textures_pixel, 0, nullptr);
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
  WGPUTexture resolved = resolved_format_ == ui::webgpu::WebGPUPresenter::kGuestOutputFormat
                             ? resolved_texture_
                             : nullptr;
  uint32_t copy_width = std::min(resolved_width_, frontbuffer_width);
  uint32_t copy_height = std::min(resolved_height_, frontbuffer_height);
  presenter->RefreshGuestOutput(
      frontbuffer_width, frontbuffer_height, 1280, 720,
      [this, resolved, copy_width,
       copy_height](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& webgpu_context =
            static_cast<ui::webgpu::WebGPUPresenter::WebGPUGuestOutputRefreshContext&>(context);
        const ui::webgpu::WebGPUDevice& device = device_thread_->device();
        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device.device(), nullptr);
        if (resolved && copy_width && copy_height) {
          WGPUTexelCopyTextureInfo source = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
          source.texture = resolved;
          WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
          destination.texture = webgpu_context.texture();
          WGPUExtent3D size = {copy_width, copy_height, 1};
          wgpuCommandEncoderCopyTextureToTexture(encoder, &source, &destination, &size);
        } else {
          // Nothing resolved in a format the guest output takes.
          WGPURenderPassColorAttachment attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
          attachment.view = webgpu_context.view();
          attachment.loadOp = WGPULoadOp_Clear;
          attachment.storeOp = WGPUStoreOp_Store;
          attachment.clearValue = {0.05, 0.1, 0.3, 1.0};
          WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
          pass_desc.colorAttachmentCount = 1;
          pass_desc.colorAttachments = &attachment;
          WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
          wgpuRenderPassEncoderEnd(pass);
          wgpuRenderPassEncoderRelease(pass);
        }
        WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
        wgpuQueueSubmit(device.queue(), 1, &commands);
        wgpuCommandBufferRelease(commands);
        wgpuCommandEncoderRelease(encoder);
        return true;
      });

  if (++frame_count_ == uint64_t(std::max(REXCVAR_GET(webgpu_capture_frame), 0))) {
    ui::RawImage image;
    if (presenter->CaptureGuestOutput(image)) {
      WriteBmp(REXCVAR_GET(webgpu_capture_path), image);
      REXGPU_INFO("WebGPU: saved frame {} to {}", frame_count_, REXCVAR_GET(webgpu_capture_path));
    } else {
      REXGPU_ERROR("WebGPU: couldn't capture frame {}", frame_count_);
    }
  }
}

}  // namespace rex::graphics::webgpu
