/**
 * @file        tests/webgpu/presenter_test.cpp
 * @brief       WebGPU presenter test window
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

// Opens a window presented by WebGPUPresenter for a few seconds: a guest output
// refreshed from another thread (an animated gradient with a white border, 16:9
// in a 16:10 window, so letterboxed) under an ImGui window drawn by the WebGPU
// immediate drawer. Saves one presented frame and one guest output image as
// BMPs in the working directory, then quits. Native only.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include <imgui.h>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/webgpu/presenter.h>
#include <rex/ui/webgpu/provider.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context.h>

namespace {

using namespace rex::ui;

constexpr uint32_t kGuestWidth = 640;
constexpr uint32_t kGuestHeight = 360;

void WriteBmp(const char* path, const RawImage& image) {
  FILE* file = std::fopen(path, "wb");
  if (!file) {
    return;
  }
  uint32_t row = image.width * 3, padded = (row + 3) & ~3u;
  uint32_t data_size = padded * image.height;
  uint8_t header[54] = {'B', 'M'};
  auto put32 = [&](int at, uint32_t v) {
    for (int i = 0; i < 4; ++i)
      header[at + i] = uint8_t(v >> (8 * i));
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
    const uint8_t* src = image.data.data() + image.stride * y;
    for (uint32_t x = 0; x < image.width; ++x) {
      line[x * 3 + 0] = src[x * 4 + 2];
      line[x * 3 + 1] = src[x * 4 + 1];
      line[x * 3 + 2] = src[x * 4 + 0];
    }
    std::fwrite(line.data(), 1, padded, file);
  }
  std::fclose(file);
  std::printf("wrote %s (%ux%u)\n", path, image.width, image.height);
}

class TestDialog final : public ImGuiDialog {
 public:
  explicit TestDialog(ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  std::atomic<uint32_t> frames{0};

 protected:
  void OnDraw(ImGuiIO& io) override {
    ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_Always);
    ImGui::Begin("WebGPU presenter");
    ImGui::Text("Guest output frames: %u", frames.load());
    ImGui::Text("%.0f x %.0f", io.DisplaySize.x, io.DisplaySize.y);
    ImGui::End();
  }
};

class PresenterTestApp final : public WindowedApp {
 public:
  static std::unique_ptr<WindowedApp> Create(WindowedAppContext& app_context) {
    return std::unique_ptr<WindowedApp>(new PresenterTestApp(app_context));
  }

  bool OnInitialize() override {
    provider_ = webgpu::WebGPUProvider::Create();
    if (!provider_) {
      return false;
    }
    presenter_ = provider_->CreatePresenter();
    window_ = Window::Create(app_context(), "rexglue WebGPU presenter test", 1280, 800);
    if (!presenter_ || !window_ || !window_->Open()) {
      return false;
    }
    window_->SetPresenter(presenter_.get());
    immediate_drawer_ = provider_->CreateImmediateDrawer();
    immediate_drawer_->SetPresenter(presenter_.get());
    imgui_drawer_ = std::make_unique<ImGuiDrawer>(window_.get(), 64);
    imgui_drawer_->SetPresenterAndImmediateDrawer(presenter_.get(), immediate_drawer_.get());
    dialog_ = new TestDialog(imgui_drawer_.get());

    refresher_ = std::thread([this] { RefreshLoop(); });
    return true;
  }

  void OnDestroy() override {
    stop_ = true;
    if (refresher_.joinable()) {
      refresher_.join();
    }
    delete dialog_;
    if (imgui_drawer_) {
      imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
      imgui_drawer_.reset();
    }
    if (immediate_drawer_) {
      immediate_drawer_->SetPresenter(nullptr);
      immediate_drawer_.reset();
    }
    if (window_) {
      window_->SetPresenter(nullptr);
    }
    presenter_.reset();
    window_.reset();
    provider_.reset();
  }

 private:
  explicit PresenterTestApp(WindowedAppContext& app_context)
      : WindowedApp(app_context, "rexglue-webgpu-presenter") {}

  void RefreshLoop() {
    auto* presenter = static_cast<webgpu::WebGPUPresenter*>(presenter_.get());
    std::vector<uint8_t> pixels(size_t(kGuestWidth) * kGuestHeight * 4);
    for (uint32_t frame = 0; !stop_ && frame < 180; ++frame) {
      for (uint32_t y = 0; y < kGuestHeight; ++y) {
        for (uint32_t x = 0; x < kGuestWidth; ++x) {
          uint8_t* p = &pixels[(size_t(y) * kGuestWidth + x) * 4];
          bool border = x < 4 || y < 4 || x >= kGuestWidth - 4 || y >= kGuestHeight - 4;
          p[0] = border ? 255 : uint8_t(x * 255 / kGuestWidth);
          p[1] = border ? 255 : uint8_t(y * 255 / kGuestHeight);
          p[2] = border ? 255 : uint8_t(frame * 4);
          p[3] = 255;
        }
      }
      presenter->RefreshGuestOutput(
          kGuestWidth, kGuestHeight, 16, 9, [&](Presenter::GuestOutputRefreshContext& context) {
            auto& webgpu_context =
                static_cast<webgpu::WebGPUPresenter::WebGPUGuestOutputRefreshContext&>(context);
            WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            destination.texture = webgpu_context.texture();
            WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
            layout.bytesPerRow = kGuestWidth * 4;
            layout.rowsPerImage = kGuestHeight;
            WGPUExtent3D extent = {kGuestWidth, kGuestHeight, 1};
            // The refresher runs on the device thread.
            WGPUQueue queue = provider_->device_thread().device().queue();
            wgpuQueueWriteTexture(queue, &destination, pixels.data(), pixels.size(), &layout,
                                  &extent);
            return true;
          });
      dialog_->frames = frame + 1;
      if (frame == 120) {
        RawImage guest_output;
        if (presenter->CaptureGuestOutput(guest_output)) {
          WriteBmp("webgpu_guest_output.bmp", guest_output);
        }
        presenter->CaptureNextPaint(
            [](RawImage&& image) { WriteBmp("webgpu_presented.bmp", image); });
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    app_context().CallInUIThread([this] { app_context().QuitFromUIThread(); });
  }

  std::unique_ptr<webgpu::WebGPUProvider> provider_;
  std::unique_ptr<Presenter> presenter_;
  std::unique_ptr<Window> window_;
  std::unique_ptr<ImmediateDrawer> immediate_drawer_;
  std::unique_ptr<ImGuiDrawer> imgui_drawer_;
  TestDialog* dialog_ = nullptr;
  std::thread refresher_;
  std::atomic<bool> stop_{false};
};

}  // namespace

REX_DEFINE_APP(rexglue_webgpu_presenter, PresenterTestApp::Create)
