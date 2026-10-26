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

#include <rex/graphics/graphics_system.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/webgpu/graphics_system.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/webgpu/presenter.h>
#include <rex/ui/webgpu/provider.h>

namespace rex::graphics::webgpu {

WebGPUCommandProcessor::WebGPUCommandProcessor(WebGPUGraphicsSystem* graphics_system,
                                               system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {}

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
  return true;
}

void WebGPUCommandProcessor::ShutdownContext() {
  if (device_thread_) {
    Flush();
    AwaitSubmissions(0);
  }
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

void WebGPUCommandProcessor::Flush() {
  EndRenderPass();
  primitive_processor_->EndSubmission();
  if (command_list_->empty()) {
    return;
  }
  statistics_upload_bytes_ += command_list_->upload_bytes();
  // The list moves to the device thread; a fresh one takes its place.
  auto list = std::shared_ptr<WebGPUCommandList>(command_list_.release());
  command_list_ = std::make_unique<WebGPUCommandList>();
  ++submissions_flushed_;
  device_thread_->Post([this, list] {
    const ui::webgpu::WebGPUDevice& device = device_thread_->device();
    list->Execute(device.device(), device.queue());
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

bool WebGPUCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                       IndexBufferInfo* index_buffer_info,
                                       bool major_mode_explicit) {
  (void)prim_type;
  (void)index_count;
  (void)index_buffer_info;
  (void)major_mode_explicit;
  const RegisterFile& regs = *register_file_;
  if (regs.Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy) {
    return IssueCopy();
  }
  auto* vertex_shader = static_cast<WgslShader*>(active_vertex_shader());
  if (!vertex_shader) {
    return false;
  }
  if (!vertex_shader->is_ucode_analyzed()) {
    vertex_shader->AnalyzeUcode(ucode_disasm_buffer_);
  }
  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  if (!primitive_processor_->Process(primitive_processing_result)) {
    return false;
  }
  if (!primitive_processing_result.host_draw_vertex_count) {
    return true;
  }
  if (!RequestVertexBuffers(*vertex_shader)) {
    return false;
  }
  ++statistics_draws_;
  // Pipelines, render targets and the draw itself come next.
  return true;
}

bool WebGPUCommandProcessor::IssueCopy() {
  // Resolves come with the render target cache.
  return true;
}

void WebGPUCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
  (void)frontbuffer_ptr;
  Flush();
  // Keep the GPU thread at most two frames ahead of the GPU.
  AwaitSubmissions(2);

  if (++statistics_frames_ == kStatisticsFrames) {
    REXGPU_INFO("WebGPU: {} frames: {} draws, {:.2f} MB uploaded, {} submissions completed",
                statistics_frames_, statistics_draws_,
                double(statistics_upload_bytes_) / (1024.0 * 1024.0),
                submissions_completed_.load());
    statistics_frames_ = 0;
    statistics_draws_ = 0;
    statistics_upload_bytes_ = 0;
  }

  ui::Presenter* presenter = graphics_system_ ? graphics_system_->presenter() : nullptr;
  if (!presenter) {
    return;
  }
  // Until there's a frontbuffer to show, a plain color proves the path.
  presenter->RefreshGuestOutput(
      frontbuffer_width, frontbuffer_height, 1280, 720,
      [this](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& webgpu_context =
            static_cast<ui::webgpu::WebGPUPresenter::WebGPUGuestOutputRefreshContext&>(context);
        const ui::webgpu::WebGPUDevice& device = device_thread_->device();
        WGPURenderPassColorAttachment attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        attachment.view = webgpu_context.view();
        attachment.loadOp = WGPULoadOp_Clear;
        attachment.storeOp = WGPUStoreOp_Store;
        attachment.clearValue = {0.05, 0.1, 0.3, 1.0};
        WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        pass_desc.colorAttachmentCount = 1;
        pass_desc.colorAttachments = &attachment;
        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device.device(), nullptr);
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
        WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
        wgpuQueueSubmit(device.queue(), 1, &commands);
        wgpuCommandBufferRelease(commands);
        wgpuCommandEncoderRelease(encoder);
        return true;
      });
}

}  // namespace rex::graphics::webgpu
