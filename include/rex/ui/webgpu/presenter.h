/**
 * @file        ui/webgpu/presenter.h
 * @brief       WebGPU presenter: guest output and UI drawing on a window surface
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <mutex>

#include <webgpu/webgpu.h>

#include <rex/ui/presenter.h>
#include <rex/ui/webgpu/device.h>

namespace rex::ui::webgpu {

// Draws the guest output (bilinear stretch; the FidelityFX effects aren't
// ported) and the UI overlays onto a window surface.
//
// WebGPU objects are reference counted and the device has one queue that runs
// submissions in order, so the guest output mailbox needs none of the Vulkan
// presenter's submission tracking: a refresh submitted before a paint is
// complete before that paint reads it.
//
// Natively the UI thread paints while the GPU thread refreshes the guest
// output, on a device created with ImplicitDeviceSynchronization. In the
// browser all WebGPU calls go through the device thread (docs/WASM.md, phase
// 5), which this presenter doesn't do yet.
class WebGPUPresenter : public Presenter {
 public:
  // The guest output image the refresher draws into: a 2D texture of the
  // frontbuffer size, usable as a render attachment, storage texture, copy
  // destination and sampled texture.
  class WebGPUGuestOutputRefreshContext final : public GuestOutputRefreshContext {
   public:
    WebGPUGuestOutputRefreshContext(bool& is_8bpc_out_ref, WGPUTexture texture,
                                    WGPUTextureView view)
        : GuestOutputRefreshContext(is_8bpc_out_ref), texture_(texture), view_(view) {}

    WGPUTexture texture() const { return texture_; }
    WGPUTextureView view() const { return view_; }

   private:
    WGPUTexture texture_;
    WGPUTextureView view_;
  };

  // What the immediate drawer and other UI drawers record into: the render
  // pass on the surface texture, open between the guest output and present.
  class WebGPUUIDrawContext final : public UIDrawContext {
   public:
    WebGPUUIDrawContext(Presenter& presenter, uint32_t render_target_width,
                        uint32_t render_target_height, WGPURenderPassEncoder pass,
                        WGPUTextureFormat format)
        : UIDrawContext(presenter, render_target_width, render_target_height),
          pass_(pass),
          format_(format) {}

    WGPURenderPassEncoder pass() const { return pass_; }
    WGPUTextureFormat format() const { return format_; }

   private:
    WGPURenderPassEncoder pass_;
    WGPUTextureFormat format_;
  };

  static constexpr WGPUTextureFormat kGuestOutputFormat = WGPUTextureFormat_RGBA8Unorm;

  static std::unique_ptr<WebGPUPresenter> Create(HostGpuLossCallback host_gpu_loss_callback,
                                                 WebGPUDevice& device);

  ~WebGPUPresenter() override;

  WebGPUDevice& device() const { return device_; }

  Surface::TypeFlags GetSupportedSurfaceTypes() const override;

  bool CaptureGuestOutput(RawImage& image_out) override;

  // Debugging: after the next paint, reads back what was presented (guest
  // output and UI, before the window system composites it) and calls back on
  // the painting thread. Native only, as it waits for the readback.
  void CaptureNextPaint(std::function<void(RawImage&& image)> callback);

 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      Surface& new_surface, uint32_t new_surface_width, uint32_t new_surface_height,
      bool was_paintable, bool& is_vsync_implicit_out) override;
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override;
  bool RefreshGuestOutputImpl(uint32_t mailbox_index, uint32_t frontbuffer_width,
                              uint32_t frontbuffer_height,
                              std::function<bool(GuestOutputRefreshContext& context)> refresher,
                              bool& is_8bpc_out_ref) override;
  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override;

 private:
  struct GuestOutputImage {
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;
    WGPUBindGroup bind_group = nullptr;  // for painting
    uint32_t width = 0;
    uint32_t height = 0;
  };

  WebGPUPresenter(HostGpuLossCallback host_gpu_loss_callback, WebGPUDevice& device)
      : Presenter(host_gpu_loss_callback), device_(device) {}

  bool Initialize();
  WGPUSurface CreateSurface(Surface& surface) const;
  bool ConfigureSurface(uint32_t width, uint32_t height);
  WGPURenderPipeline GetGuestOutputPipeline(WGPUTextureFormat format);
  static void ReleaseGuestOutputImage(GuestOutputImage& image);
  // Copies texture (of the given format) to image_out, waiting. Native only.
  bool ReadBackTexture(WGPUTexture texture, WGPUTextureFormat format, uint32_t width,
                       uint32_t height, RawImage& image_out);

  WebGPUDevice& device_;

  WGPUShaderModule guest_output_shader_ = nullptr;
  WGPUBindGroupLayout guest_output_bind_group_layout_ = nullptr;
  WGPUPipelineLayout guest_output_pipeline_layout_ = nullptr;
  WGPUSampler guest_output_sampler_ = nullptr;
  WGPURenderPipeline guest_output_pipeline_ = nullptr;
  WGPUTextureFormat guest_output_pipeline_format_ = WGPUTextureFormat_Undefined;

  // Guest output images by mailbox index, owned by the refresher while it's
  // writing one and by the painting consumer (under the consumer lock) while
  // it takes a reference.
  std::array<GuestOutputImage, kGuestOutputMailboxSize> guest_output_images_;

  // Painting connection, UI thread.
  const Surface* connected_surface_ = nullptr;
  WGPUSurface surface_ = nullptr;
  WGPUTextureFormat surface_format_ = WGPUTextureFormat_Undefined;
  bool surface_copy_src_ = false;
  uint32_t surface_width_ = 0;
  uint32_t surface_height_ = 0;

  std::mutex capture_mutex_;
  std::function<void(RawImage&& image)> capture_next_paint_;
};

}  // namespace rex::ui::webgpu
