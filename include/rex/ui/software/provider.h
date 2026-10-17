#pragma once
/**
 * @file        ui/software/provider.h
 * @brief       Graphics provider without a host GPU API
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <memory>

#include <rex/ui/graphics_provider.h>

namespace rex::ui::software {

// Presents CPU-generated images (SoftwarePresenter). There is no immediate
// drawer, so ImGui overlays are unavailable with this provider.
class SoftwareProvider final : public GraphicsProvider {
 public:
  static std::unique_ptr<SoftwareProvider> Create() {
    return std::unique_ptr<SoftwareProvider>(new SoftwareProvider());
  }

  std::unique_ptr<Presenter> CreatePresenter(Presenter::HostGpuLossCallback host_gpu_loss_callback =
                                                 Presenter::FatalErrorHostGpuLossCallback) override;

  std::unique_ptr<ImmediateDrawer> CreateImmediateDrawer() override { return nullptr; }

 private:
  SoftwareProvider() = default;
};

}  // namespace rex::ui::software
