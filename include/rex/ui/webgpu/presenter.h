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
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

#include <webgpu/webgpu.h>

#include <rex/ui/presenter.h>
#include <rex/ui/webgpu/device_thread.h>

namespace rex::ui::webgpu {

// Draws the guest output (bilinear stretch; the FidelityFX effects aren't
// ported) and the UI overlays onto a window surface.
//
// WebGPU objects are reference counted and the device has one queue that runs
// submissions in order, so the guest output mailbox needs none of the Vulkan
// presenter's submission tracking: a refresh submitted before a paint is
// complete before that paint reads it.
//
// Every WebGPU call runs on the device thread (WebGPUDeviceThread): painting,
// including the UI drawers, while the UI thread waits for it, and guest output
// refreshes, with the refresher called there too. In the browser the surface is
// the page's canvas, which the device thread owns.
class WebGPUPresenter : public Presenter {
 public:
  // The guest output image the refresher draws into: a 2D texture of the
  // frontbuffer size, usable as a render attachment, storage texture, copy
  // destination and sampled texture. The refresher runs on the device thread.
  class WebGPUGuestOutputRefreshContext final : public GuestOutputRefreshContext {
   public:
    WebGPUGuestOutputRefreshContext(bool& is_8bpc_out_ref, WGPUQueue queue, WGPUTexture texture,
                                    WGPUTextureView view, uint32_t width, uint32_t height)
        : GuestOutputRefreshContext(is_8bpc_out_ref),
          queue_(queue),
          texture_(texture),
          view_(view),
          width_(width),
          height_(height) {}

    WGPUTexture texture() const { return texture_; }
    WGPUTextureView view() const { return view_; }

    bool WriteRgba8(const void* pixels, size_t row_pitch) override;

   private:
    WGPUQueue queue_;
    WGPUTexture texture_;
    WGPUTextureView view_;
    uint32_t width_;
    uint32_t height_;
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
                                                 WebGPUDeviceThread& device_thread);

  ~WebGPUPresenter() override;

  WebGPUDeviceThread& device_thread() const { return device_thread_; }

  Surface::TypeFlags GetSupportedSurfaceTypes() const override;

  // Waits for the readback, so not from the device thread.
  bool CaptureGuestOutput(RawImage& image_out) override;

  // Debugging: reads back what the next paint presents (guest output and UI,
  // before the window system composites it) and calls back on the device
  // thread once it's read.
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

  WebGPUPresenter(HostGpuLossCallback host_gpu_loss_callback, WebGPUDeviceThread& device_thread)
      : Presenter(host_gpu_loss_callback), device_thread_(device_thread) {}

  // The rest run on the device thread.
  WebGPUDevice& device() const { return device_thread_.device(); }
  bool Initialize();
  void Shutdown();
  SurfacePaintConnectResult Connect(Surface& new_surface, uint32_t new_surface_width,
                                    uint32_t new_surface_height, bool was_paintable);
  void Disconnect();
  PaintResult Paint(bool execute_ui_drawers);
  WGPUSurface CreateSurface(Surface& surface) const;
  bool ConfigureSurface(uint32_t width, uint32_t height);
  WGPURenderPipeline GetGuestOutputPipeline(WGPUTextureFormat format);
  static void ReleaseGuestOutputImage(GuestOutputImage& image);
  // Copies texture (of the given format) to an image and calls done with it, or
  // with false on failure, once the copy is mapped.
  void ReadBackTexture(WGPUTexture texture, WGPUTextureFormat format, uint32_t width,
                       uint32_t height, std::function<void(bool ok, RawImage&& image)> done);

  WebGPUDeviceThread& device_thread_;

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

  // Painting connection, device thread.
  const Surface* connected_surface_ = nullptr;
  WGPUSurface surface_ = nullptr;
  WGPUTextureFormat surface_format_ = WGPUTextureFormat_Undefined;
  bool surface_copy_src_ = false;
  uint32_t surface_width_ = 0;
  uint32_t surface_height_ = 0;

  std::mutex capture_mutex_;
  std::function<void(RawImage&& image)> capture_next_paint_;

#if defined(__EMSCRIPTEN__)
  // The browser presents without blocking, unlike a FIFO swap chain, so paints
  // wait for the animation frame after the last present instead.
  std::mutex animation_frame_mutex_;
  std::condition_variable animation_frame_reached_;
  bool animation_frame_pending_ = false;
  // Device thread: the requested animation frame callback, 0 if none.
  long animation_frame_id_ = 0;
#endif
};

}  // namespace rex::ui::webgpu
