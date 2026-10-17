#pragma once
/**
 * @file        ui/software/presenter.h
 * @brief       Presenter for CPU-generated guest output, painted via SDL
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <array>
#include <memory>

#include <rex/ui/presenter.h>

struct SDL_Window;

namespace rex::ui::software {

// Guest output images live in host memory (R8 G8 B8 X8, as RawImage) and are
// blitted to the window through SDL's window framebuffer, so no host GPU API
// is involved. Used by the null GPU backend; works wherever SDL has a window
// framebuffer (Windows, X11, Emscripten). UI drawers are not supported: there
// is no immediate drawer to draw them with.
class SoftwarePresenter final : public Presenter {
 public:
  class SoftwareGuestOutputRefreshContext final : public GuestOutputRefreshContext {
   public:
    SoftwareGuestOutputRefreshContext(bool& is_8bpc_out_ref, RawImage& image)
        : GuestOutputRefreshContext(is_8bpc_out_ref), image_(image) {}

    // Sized to the frontbuffer before the refresher is called.
    RawImage& image() const { return image_; }

   private:
    RawImage& image_;
  };

  static std::unique_ptr<SoftwarePresenter> Create(HostGpuLossCallback host_gpu_loss_callback =
                                                       FatalErrorHostGpuLossCallback);

  Surface::TypeFlags GetSupportedSurfaceTypes() const override {
    return Surface::kTypeFlag_SdlWindow;
  }

  bool CaptureGuestOutput(RawImage& image_out) override;

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
  explicit SoftwarePresenter(HostGpuLossCallback host_gpu_loss_callback)
      : Presenter(host_gpu_loss_callback) {}

  std::array<RawImage, kGuestOutputMailboxSize> guest_output_images_;
  SDL_Window* window_ = nullptr;
};

}  // namespace rex::ui::software
