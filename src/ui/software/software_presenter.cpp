/**
 * @file        ui/software/software_presenter.cpp
 * @brief       Presenter for CPU-generated guest output, painted via SDL
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/software/presenter.h>
#include <rex/ui/software/provider.h>

#include <algorithm>
#include <cstring>

#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_video.h>

#include <rex/logging.h>
#include <rex/ui/surface_sdl.h>

namespace rex::ui::software {

std::unique_ptr<Presenter> SoftwareProvider::CreatePresenter(
    Presenter::HostGpuLossCallback host_gpu_loss_callback) {
  return SoftwarePresenter::Create(host_gpu_loss_callback);
}

std::unique_ptr<SoftwarePresenter> SoftwarePresenter::Create(
    HostGpuLossCallback host_gpu_loss_callback) {
  auto presenter = std::unique_ptr<SoftwarePresenter>(new SoftwarePresenter(host_gpu_loss_callback));
  if (!presenter->InitializeCommonSurfaceIndependent()) {
    return nullptr;
  }
  return presenter;
}

bool SoftwarePresenter::CaptureGuestOutput(RawImage& image_out) {
  uint32_t mailbox_index;
  auto consumer_lock = ConsumeGuestOutput(mailbox_index, nullptr, nullptr);
  if (mailbox_index == UINT32_MAX) {
    return false;
  }
  image_out = guest_output_images_[mailbox_index];
  return !image_out.data.empty();
}

Presenter::SurfacePaintConnectResult SoftwarePresenter::ConnectOrReconnectPaintingToSurfaceFromUIThread(
    Surface& new_surface, uint32_t new_surface_width, uint32_t new_surface_height,
    bool was_paintable, bool& is_vsync_implicit_out) {
  (void)new_surface_width;
  (void)new_surface_height;
  if (new_surface.GetType() != Surface::kTypeIndex_SdlWindow) {
    return SurfacePaintConnectResult::kFailureSurfaceUnusable;
  }
  SDL_Window* window = static_cast<SdlWindowSurface&>(new_surface).window();
  // The window framebuffer is re-fetched on every paint, which also handles
  // resizes, so only a window change needs reporting.
  bool unchanged = was_paintable && window == window_;
  window_ = window;
  // Keep painting on the UI thread: SDL window functions belong there.
  is_vsync_implicit_out = true;
  return unchanged ? SurfacePaintConnectResult::kSuccessUnchanged
                   : SurfacePaintConnectResult::kSuccess;
}

void SoftwarePresenter::DisconnectPaintingFromSurfaceFromUIThreadImpl() {
  window_ = nullptr;
}

bool SoftwarePresenter::RefreshGuestOutputImpl(
    uint32_t mailbox_index, uint32_t frontbuffer_width, uint32_t frontbuffer_height,
    std::function<bool(GuestOutputRefreshContext& context)> refresher, bool& is_8bpc_out_ref) {
  RawImage& image = guest_output_images_[mailbox_index];
  image.width = frontbuffer_width;
  image.height = frontbuffer_height;
  image.stride = size_t(frontbuffer_width) * 4;
  image.data.resize(image.stride * frontbuffer_height);
  SoftwareGuestOutputRefreshContext context(is_8bpc_out_ref, image);
  context.SetIs8bpc(true);
  return refresher(context);
}

Presenter::PaintResult SoftwarePresenter::PaintAndPresentImpl(bool execute_ui_drawers) {
  (void)execute_ui_drawers;
  if (!window_) {
    return PaintResult::kNotPresentedConnectionOutdated;
  }
  SDL_Surface* target = SDL_GetWindowSurface(window_);
  if (!target) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_ERROR("SoftwarePresenter: no SDL window framebuffer: {}", SDL_GetError());
    }
    return PaintResult::kNotPresented;
  }
  SDL_FillSurfaceRect(target, nullptr, SDL_MapSurfaceRGB(target, 0, 0, 0));

  {
    uint32_t mailbox_index;
    GuestOutputProperties properties;
    auto consumer_lock = ConsumeGuestOutput(mailbox_index, &properties, nullptr);
    if (mailbox_index != UINT32_MAX && properties.IsActive()) {
      RawImage& image = guest_output_images_[mailbox_index];
      SDL_Surface* source =
          SDL_CreateSurfaceFrom(int(image.width), int(image.height), SDL_PIXELFORMAT_RGBX32,
                                image.data.data(), int(image.stride));
      if (source) {
        // Letterbox to the guest display aspect ratio.
        int target_w = target->w, target_h = target->h;
        double aspect =
            double(properties.display_aspect_ratio_x) / double(properties.display_aspect_ratio_y);
        int w = target_w, h = int(target_w / aspect + 0.5);
        if (h > target_h) {
          h = target_h;
          w = int(target_h * aspect + 0.5);
        }
        SDL_Rect dest = {(target_w - w) / 2, (target_h - h) / 2, w, h};
        SDL_BlitSurfaceScaled(source, nullptr, target, &dest, SDL_SCALEMODE_LINEAR);
        SDL_DestroySurface(source);
      }
    }
  }

  if (!SDL_UpdateWindowSurface(window_)) {
    return PaintResult::kNotPresented;
  }
  return PaintResult::kPresented;
}

}  // namespace rex::ui::software
