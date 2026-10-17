#pragma once
/**
 * @file        ui/surface_sdl.h
 * @brief       Surface wrapping an SDL window, for CPU-side presentation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/surface.h>

struct SDL_Window;

namespace rex::ui {

class SdlWindowSurface final : public Surface {
 public:
  explicit SdlWindowSurface(SDL_Window* window) : window_(window) {}
  TypeIndex GetType() const override { return kTypeIndex_SdlWindow; }
  SDL_Window* window() const { return window_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  SDL_Window* window_;
};

}  // namespace rex::ui
