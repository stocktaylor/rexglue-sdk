/**
 * @file        ui/webgpu/webgpu_presenter.cpp
 * @brief       WebGPU presenter: guest output and UI drawing on a window surface
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/webgpu/presenter.h>

#include <algorithm>
#include <cstring>
#include <future>

#include <rex/logging.h>
#include <rex/platform.h>

#if REX_PLATFORM_GNU_LINUX
#include <rex/ui/surface_gnulinux.h>
#elif REX_PLATFORM_WIN32
#include <rex/ui/surface_win.h>
#elif REX_PLATFORM_MAC
#include <rex/ui/surface_mac.h>
#endif
#if defined(__EMSCRIPTEN__)
#include <rex/ui/surface_sdl.h>
#endif

namespace rex::ui::webgpu {

namespace {

WGPUStringView View(const char* s) {
  return {s, WGPU_STRLEN};
}

// Stretches the guest output over the viewport, which is set to the
// letterboxed output rectangle.
const char kGuestOutputShader[] = R"(
@group(0) @binding(0) var guest_output : texture_2d<f32>;
@group(0) @binding(1) var guest_output_sampler : sampler;

struct Interpolants {
  @builtin(position) position : vec4f,
  @location(0) uv : vec2f,
}

@vertex
fn vs(@builtin(vertex_index) index : u32) -> Interpolants {
  // One triangle covering the viewport.
  let uv = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
  var out : Interpolants;
  out.position = vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
  out.uv = uv;
  return out;
}

@fragment
fn fs(in : Interpolants) -> @location(0) vec4f {
  return vec4f(textureSample(guest_output, guest_output_sampler, in.uv).rgb, 1.0);
}
)";

bool IsBgra(WGPUTextureFormat format) {
  return format == WGPUTextureFormat_BGRA8Unorm || format == WGPUTextureFormat_BGRA8UnormSrgb;
}

}  // namespace

std::unique_ptr<WebGPUPresenter> WebGPUPresenter::Create(HostGpuLossCallback host_gpu_loss_callback,
                                                         WebGPUDeviceThread& device_thread) {
  auto presenter =
      std::unique_ptr<WebGPUPresenter>(new WebGPUPresenter(host_gpu_loss_callback, device_thread));
  if (!presenter->InitializeCommonSurfaceIndependent()) {
    return nullptr;
  }
  bool initialized = false;
  device_thread.Call([&] { initialized = presenter->Initialize(); });
  if (!initialized) {
    return nullptr;
  }
  return presenter;
}

bool WebGPUPresenter::Initialize() {
  WGPUDevice dev = device().device();

  WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
  wgsl.code = View(kGuestOutputShader);
  WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
  module_desc.nextInChain = &wgsl.chain;
  module_desc.label = View("Guest output");
  guest_output_shader_ = wgpuDeviceCreateShaderModule(dev, &module_desc);

  WGPUBindGroupLayoutEntry entries[2] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
                                         WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
  entries[0].binding = 0;
  entries[0].visibility = WGPUShaderStage_Fragment;
  entries[0].texture.sampleType = WGPUTextureSampleType_Float;
  entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
  entries[1].binding = 1;
  entries[1].visibility = WGPUShaderStage_Fragment;
  entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
  WGPUBindGroupLayoutDescriptor layout_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
  layout_desc.entryCount = 2;
  layout_desc.entries = entries;
  guest_output_bind_group_layout_ = wgpuDeviceCreateBindGroupLayout(dev, &layout_desc);

  WGPUPipelineLayoutDescriptor pipeline_layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
  pipeline_layout_desc.bindGroupLayoutCount = 1;
  pipeline_layout_desc.bindGroupLayouts = &guest_output_bind_group_layout_;
  guest_output_pipeline_layout_ = wgpuDeviceCreatePipelineLayout(dev, &pipeline_layout_desc);

  WGPUSamplerDescriptor sampler_desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
  sampler_desc.magFilter = WGPUFilterMode_Linear;
  sampler_desc.minFilter = WGPUFilterMode_Linear;
  guest_output_sampler_ = wgpuDeviceCreateSampler(dev, &sampler_desc);

  return guest_output_shader_ && guest_output_bind_group_layout_ && guest_output_pipeline_layout_ &&
         guest_output_sampler_;
}

WebGPUPresenter::~WebGPUPresenter() {
  device_thread_.Call([this] { Shutdown(); });
}

void WebGPUPresenter::Shutdown() {
  Disconnect();
  for (GuestOutputImage& image : guest_output_images_) {
    ReleaseGuestOutputImage(image);
  }
  if (guest_output_pipeline_) {
    wgpuRenderPipelineRelease(guest_output_pipeline_);
  }
  if (guest_output_sampler_) {
    wgpuSamplerRelease(guest_output_sampler_);
  }
  if (guest_output_pipeline_layout_) {
    wgpuPipelineLayoutRelease(guest_output_pipeline_layout_);
  }
  if (guest_output_bind_group_layout_) {
    wgpuBindGroupLayoutRelease(guest_output_bind_group_layout_);
  }
  if (guest_output_shader_) {
    wgpuShaderModuleRelease(guest_output_shader_);
  }
}

void WebGPUPresenter::ReleaseGuestOutputImage(GuestOutputImage& image) {
  if (image.bind_group) {
    wgpuBindGroupRelease(image.bind_group);
  }
  if (image.view) {
    wgpuTextureViewRelease(image.view);
  }
  if (image.texture) {
    wgpuTextureRelease(image.texture);
  }
  image = {};
}

Surface::TypeFlags WebGPUPresenter::GetSupportedSurfaceTypes() const {
#if REX_PLATFORM_GNU_LINUX
#if defined(REX_DAWN_HAS_WAYLAND)
  return Surface::kTypeFlag_WaylandSurface | Surface::kTypeFlag_XcbWindow;
#else
  // The prebuilt Dawn has no Wayland surfaces (cmake/dawn.cmake).
  return Surface::kTypeFlag_XcbWindow;
#endif
#elif REX_PLATFORM_WIN32
  return Surface::kTypeFlag_Win32Hwnd;
#elif REX_PLATFORM_MAC
  return Surface::kTypeFlag_CAMetalLayer;
#elif defined(__EMSCRIPTEN__)
  // The SDL window is on the page's canvas, which the device thread owns.
  return Surface::kTypeFlag_SdlWindow;
#else
  return 0;
#endif
}

WGPUSurface WebGPUPresenter::CreateSurface(Surface& surface) const {
  WGPUSurfaceDescriptor descriptor = WGPU_SURFACE_DESCRIPTOR_INIT;
  switch (surface.GetType()) {
#if REX_PLATFORM_GNU_LINUX
    case Surface::kTypeIndex_WaylandSurface: {
      auto& wayland = static_cast<WaylandSurface&>(surface);
      WGPUSurfaceSourceWaylandSurface source = WGPU_SURFACE_SOURCE_WAYLAND_SURFACE_INIT;
      source.display = wayland.display();
      source.surface = wayland.surface();
      descriptor.nextInChain = &source.chain;
      return wgpuInstanceCreateSurface(device().instance(), &descriptor);
    }
    case Surface::kTypeIndex_XcbWindow: {
      // Dawn takes X11 windows through Xlib, not XCB.
      auto& xcb = static_cast<XcbWindowSurface&>(surface);
      if (!xcb.xlib_display()) {
        return nullptr;
      }
      WGPUSurfaceSourceXlibWindow source = WGPU_SURFACE_SOURCE_XLIB_WINDOW_INIT;
      source.display = xcb.xlib_display();
      source.window = xcb.window();
      descriptor.nextInChain = &source.chain;
      return wgpuInstanceCreateSurface(device().instance(), &descriptor);
    }
#elif REX_PLATFORM_WIN32
    case Surface::kTypeIndex_Win32Hwnd: {
      auto& win32 = static_cast<Win32HwndSurface&>(surface);
      WGPUSurfaceSourceWindowsHWND source = WGPU_SURFACE_SOURCE_WINDOWS_HWND_INIT;
      source.hinstance = win32.hinstance();
      source.hwnd = win32.hwnd();
      descriptor.nextInChain = &source.chain;
      return wgpuInstanceCreateSurface(device().instance(), &descriptor);
    }
#elif REX_PLATFORM_MAC
    case Surface::kTypeIndex_CAMetalLayer: {
      auto& metal = static_cast<CAMetalLayerSurface&>(surface);
      WGPUSurfaceSourceMetalLayer source = WGPU_SURFACE_SOURCE_METAL_LAYER_INIT;
      source.layer = const_cast<void*>(metal.layer());
      descriptor.nextInChain = &source.chain;
      return wgpuInstanceCreateSurface(device().instance(), &descriptor);
    }
#endif
#if defined(__EMSCRIPTEN__)
    case Surface::kTypeIndex_SdlWindow: {
      if (device_thread_.canvas_selector().empty()) {
        return nullptr;
      }
      WGPUEmscriptenSurfaceSourceCanvasHTMLSelector source =
          WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
      source.selector = {device_thread_.canvas_selector().c_str(), WGPU_STRLEN};
      descriptor.nextInChain = &source.chain;
      return wgpuInstanceCreateSurface(device().instance(), &descriptor);
    }
#endif
    default:
      return nullptr;
  }
}

bool WebGPUPresenter::ConfigureSurface(uint32_t width, uint32_t height) {
  WGPUSurfaceCapabilities capabilities = WGPU_SURFACE_CAPABILITIES_INIT;
  if (wgpuSurfaceGetCapabilities(surface_, device().adapter(), &capabilities) !=
      WGPUStatus_Success) {
    return false;
  }
  // Guest output is already in display gamma: a plain unorm format.
  surface_format_ = WGPUTextureFormat_Undefined;
  for (size_t i = 0; i < capabilities.formatCount; ++i) {
    if (capabilities.formats[i] == WGPUTextureFormat_BGRA8Unorm ||
        capabilities.formats[i] == WGPUTextureFormat_RGBA8Unorm) {
      surface_format_ = capabilities.formats[i];
      break;
    }
  }
  surface_copy_src_ = (capabilities.usages & WGPUTextureUsage_CopySrc) != 0;
  wgpuSurfaceCapabilitiesFreeMembers(capabilities);
  if (surface_format_ == WGPUTextureFormat_Undefined) {
    REXLOG_ERROR("WebGPU presenter: the surface has no 8-bit unorm format");
    return false;
  }

  WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
  config.device = device().device();
  config.format = surface_format_;
  config.usage = WGPUTextureUsage_RenderAttachment |
                 (surface_copy_src_ ? WGPUTextureUsage_CopySrc : WGPUTextureUsage_None);
  config.width = width;
  config.height = height;
  config.alphaMode = WGPUCompositeAlphaMode_Auto;
  config.presentMode = WGPUPresentMode_Fifo;
  wgpuSurfaceConfigure(surface_, &config);
  surface_width_ = width;
  surface_height_ = height;
  return true;
}

Presenter::SurfacePaintConnectResult
WebGPUPresenter::ConnectOrReconnectPaintingToSurfaceFromUIThread(Surface& new_surface,
                                                                 uint32_t new_surface_width,
                                                                 uint32_t new_surface_height,
                                                                 bool was_paintable,
                                                                 bool& is_vsync_implicit_out) {
  // FIFO: presenting waits for vertical blank.
  is_vsync_implicit_out = true;
  SurfacePaintConnectResult result;
  device_thread_.Call(
      [&] { result = Connect(new_surface, new_surface_width, new_surface_height, was_paintable); });
  return result;
}

Presenter::SurfacePaintConnectResult WebGPUPresenter::Connect(Surface& new_surface,
                                                              uint32_t new_surface_width,
                                                              uint32_t new_surface_height,
                                                              bool was_paintable) {
  if (was_paintable && surface_ && connected_surface_ == &new_surface &&
      surface_width_ == new_surface_width && surface_height_ == new_surface_height) {
    return SurfacePaintConnectResult::kSuccessUnchanged;
  }
  if (connected_surface_ != &new_surface || !surface_) {
    Disconnect();
    surface_ = CreateSurface(new_surface);
    if (!surface_) {
      return SurfacePaintConnectResult::kFailureSurfaceUnusable;
    }
    connected_surface_ = &new_surface;
  }
  if (!ConfigureSurface(new_surface_width, new_surface_height)) {
    Disconnect();
    return SurfacePaintConnectResult::kFailureSurfaceUnusable;
  }
  return SurfacePaintConnectResult::kSuccess;
}

void WebGPUPresenter::DisconnectPaintingFromSurfaceFromUIThreadImpl() {
  device_thread_.Call([this] { Disconnect(); });
}

void WebGPUPresenter::Disconnect() {
  if (surface_) {
    wgpuSurfaceUnconfigure(surface_);
    wgpuSurfaceRelease(surface_);
    surface_ = nullptr;
  }
  connected_surface_ = nullptr;
  surface_width_ = 0;
  surface_height_ = 0;
}

bool WebGPUPresenter::WebGPUGuestOutputRefreshContext::WriteRgba8(const void* pixels,
                                                                  size_t row_pitch) {
  // On the device thread, like the refresher calling this.
  WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
  destination.texture = texture_;
  WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
  layout.bytesPerRow = uint32_t(row_pitch);
  layout.rowsPerImage = height_;
  WGPUExtent3D extent = {width_, height_, 1};
  wgpuQueueWriteTexture(queue_, &destination, pixels, row_pitch * height_, &layout, &extent);
  return true;
}

bool WebGPUPresenter::RefreshGuestOutputImpl(
    uint32_t mailbox_index, uint32_t frontbuffer_width, uint32_t frontbuffer_height,
    std::function<bool(GuestOutputRefreshContext& context)> refresher, bool& is_8bpc_out_ref) {
  bool refreshed = false;
  device_thread_.Call([&] {
    GuestOutputImage& image = guest_output_images_[mailbox_index];
    if (!image.texture || image.width != frontbuffer_width || image.height != frontbuffer_height) {
      // Any paint still using the old texture holds its own reference.
      ReleaseGuestOutputImage(image);
      WGPUTextureDescriptor texture_desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
      texture_desc.label = View("Guest output");
      texture_desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_RenderAttachment |
                           WGPUTextureUsage_StorageBinding | WGPUTextureUsage_CopyDst |
                           WGPUTextureUsage_CopySrc;
      texture_desc.size = {frontbuffer_width, frontbuffer_height, 1};
      texture_desc.format = kGuestOutputFormat;
      image.texture = wgpuDeviceCreateTexture(device().device(), &texture_desc);
      if (!image.texture) {
        return;
      }
      image.view = wgpuTextureCreateView(image.texture, nullptr);
      WGPUBindGroupEntry entries[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
      entries[0].binding = 0;
      entries[0].textureView = image.view;
      entries[1].binding = 1;
      entries[1].sampler = guest_output_sampler_;
      WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
      group_desc.layout = guest_output_bind_group_layout_;
      group_desc.entryCount = 2;
      group_desc.entries = entries;
      image.bind_group = wgpuDeviceCreateBindGroup(device().device(), &group_desc);
      image.width = frontbuffer_width;
      image.height = frontbuffer_height;
    }
    WebGPUGuestOutputRefreshContext context(is_8bpc_out_ref, device().queue(), image.texture,
                                            image.view, image.width, image.height);
    context.SetIs8bpc(true);
    refreshed = refresher(context);
  });
  return refreshed;
}

WGPURenderPipeline WebGPUPresenter::GetGuestOutputPipeline(WGPUTextureFormat format) {
  if (guest_output_pipeline_ && guest_output_pipeline_format_ == format) {
    return guest_output_pipeline_;
  }
  if (guest_output_pipeline_) {
    wgpuRenderPipelineRelease(guest_output_pipeline_);
  }
  WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
  target.format = format;
  WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
  fragment.module = guest_output_shader_;
  fragment.entryPoint = View("fs");
  fragment.targetCount = 1;
  fragment.targets = &target;
  WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
  desc.label = View("Guest output");
  desc.layout = guest_output_pipeline_layout_;
  desc.vertex.module = guest_output_shader_;
  desc.vertex.entryPoint = View("vs");
  desc.fragment = &fragment;
  guest_output_pipeline_ = wgpuDeviceCreateRenderPipeline(device().device(), &desc);
  guest_output_pipeline_format_ = format;
  return guest_output_pipeline_;
}

Presenter::PaintResult WebGPUPresenter::PaintAndPresentImpl(bool execute_ui_drawers) {
  // The UI drawers run on the device thread too, while the UI thread waits.
  PaintResult result;
  device_thread_.Call([&] { result = Paint(execute_ui_drawers); });
  return result;
}

Presenter::PaintResult WebGPUPresenter::Paint(bool execute_ui_drawers) {
  if (device().is_lost()) {
    return PaintResult::kGpuLostExternally;
  }
  if (!surface_) {
    return PaintResult::kNotPresentedConnectionOutdated;
  }

  WGPUSurfaceTexture surface_texture = WGPU_SURFACE_TEXTURE_INIT;
  wgpuSurfaceGetCurrentTexture(surface_, &surface_texture);
  switch (surface_texture.status) {
    case WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal:
    case WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal:
      break;
    case WGPUSurfaceGetCurrentTextureStatus_Outdated:
    case WGPUSurfaceGetCurrentTextureStatus_Lost:
      if (surface_texture.texture) {
        wgpuTextureRelease(surface_texture.texture);
      }
      return PaintResult::kNotPresentedConnectionOutdated;
    default:
      if (surface_texture.texture) {
        wgpuTextureRelease(surface_texture.texture);
      }
      return PaintResult::kNotPresented;
  }
  bool suboptimal = surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal;
  WGPUTextureView target = wgpuTextureCreateView(surface_texture.texture, nullptr);

  WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device().device(), nullptr);
  WGPURenderPassColorAttachment attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
  attachment.view = target;
  attachment.loadOp = WGPULoadOp_Clear;
  attachment.storeOp = WGPUStoreOp_Store;
  attachment.clearValue = {0.0, 0.0, 0.0, 1.0};
  WGPURenderPassDescriptor pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
  pass_desc.colorAttachmentCount = 1;
  pass_desc.colorAttachments = &attachment;
  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);

  // Take a reference to the current guest output image, so the consumer lock
  // can be dropped before drawing.
  GuestOutputProperties properties;
  GuestOutputPaintConfig paint_config;
  WGPUBindGroup guest_output_group = nullptr;
  {
    uint32_t mailbox_index;
    auto consumer_lock = ConsumeGuestOutput(mailbox_index, &properties, &paint_config);
    if (mailbox_index != UINT32_MAX && guest_output_images_[mailbox_index].bind_group) {
      guest_output_group = guest_output_images_[mailbox_index].bind_group;
      wgpuBindGroupAddRef(guest_output_group);
    }
  }
  if (guest_output_group) {
    uint32_t max_size = device().limits().maxTextureDimension2D;
    GuestOutputPaintFlow flow = GetGuestOutputPaintFlow(properties, surface_width_, surface_height_,
                                                        max_size, max_size, paint_config);
    if (flow.effect_count) {
      // Every effect is drawn as a bilinear stretch to the final size.
      const auto& output_size = flow.effect_output_sizes[flow.effect_count - 1];
      int32_t x0 = std::max(flow.output_x, int32_t(0));
      int32_t y0 = std::max(flow.output_y, int32_t(0));
      int32_t x1 = std::min(flow.output_x + int32_t(output_size.first), int32_t(surface_width_));
      int32_t y1 = std::min(flow.output_y + int32_t(output_size.second), int32_t(surface_height_));
      if (x1 > x0 && y1 > y0) {
        wgpuRenderPassEncoderSetViewport(pass, float(flow.output_x), float(flow.output_y),
                                         float(output_size.first), float(output_size.second), 0.0f,
                                         1.0f);
        wgpuRenderPassEncoderSetScissorRect(pass, uint32_t(x0), uint32_t(y0), uint32_t(x1 - x0),
                                            uint32_t(y1 - y0));
        wgpuRenderPassEncoderSetPipeline(pass, GetGuestOutputPipeline(surface_format_));
        wgpuRenderPassEncoderSetBindGroup(pass, 0, guest_output_group, 0, nullptr);
        wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        wgpuRenderPassEncoderSetViewport(pass, 0.0f, 0.0f, float(surface_width_),
                                         float(surface_height_), 0.0f, 1.0f);
        wgpuRenderPassEncoderSetScissorRect(pass, 0, 0, surface_width_, surface_height_);
      }
    }
    wgpuBindGroupRelease(guest_output_group);
  }

  if (execute_ui_drawers) {
    WebGPUUIDrawContext ui_draw_context(*this, surface_width_, surface_height_, pass,
                                        surface_format_);
    ExecuteUIDrawersFromUIThread(ui_draw_context);
  }

  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);
  WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
  wgpuQueueSubmit(device().queue(), 1, &commands);
  wgpuCommandBufferRelease(commands);
  wgpuCommandEncoderRelease(encoder);

  std::function<void(RawImage&&)> capture;
  {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    if (surface_copy_src_) {
      capture = std::move(capture_next_paint_);
      capture_next_paint_ = nullptr;
    }
  }
  if (capture) {
    ReadBackTexture(surface_texture.texture, surface_format_, surface_width_, surface_height_,
                    [capture = std::move(capture)](bool ok, RawImage&& image) {
                      if (ok) {
                        capture(std::move(image));
                      }
                    });
  }

#if defined(__EMSCRIPTEN__)
  // The canvas presents by itself when the device thread returns to its event
  // loop, after this task; emdawnwebgpu aborts on wgpuSurfacePresent.
  WGPUStatus present_status = WGPUStatus_Success;
#else
  WGPUStatus present_status = wgpuSurfacePresent(surface_);
#endif
  wgpuTextureViewRelease(target);
  wgpuTextureRelease(surface_texture.texture);
  if (present_status != WGPUStatus_Success) {
    return PaintResult::kNotPresented;
  }
  return suboptimal ? PaintResult::kPresentedSuboptimal : PaintResult::kPresented;
}

void WebGPUPresenter::ReadBackTexture(WGPUTexture texture, WGPUTextureFormat format, uint32_t width,
                                      uint32_t height,
                                      std::function<void(bool ok, RawImage&& image)> done) {
  struct Readback {
    WGPUBuffer buffer;
    uint32_t width, height, row_pitch;
    bool bgra;
    std::function<void(bool, RawImage&&)> done;
  };
  auto* readback = new Readback{
      nullptr, width, height, (width * 4 + 255) & ~uint32_t(255), IsBgra(format), std::move(done)};
  WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
  buffer_desc.size = uint64_t(readback->row_pitch) * height;
  buffer_desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
  readback->buffer = wgpuDeviceCreateBuffer(device().device(), &buffer_desc);

  WGPUTexelCopyTextureInfo source = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
  source.texture = texture;
  WGPUTexelCopyBufferInfo destination = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
  destination.buffer = readback->buffer;
  destination.layout.bytesPerRow = readback->row_pitch;
  destination.layout.rowsPerImage = height;
  WGPUExtent3D extent = {width, height, 1};
  WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device().device(), nullptr);
  wgpuCommandEncoderCopyTextureToBuffer(encoder, &source, &destination, &extent);
  WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
  wgpuQueueSubmit(device().queue(), 1, &commands);
  wgpuCommandBufferRelease(commands);
  wgpuCommandEncoderRelease(encoder);

  // Completes on the device thread once the copy has run; nothing waits here.
  WGPUBufferMapCallbackInfo callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
  callback.mode = kDeviceThreadCallbackMode;
  callback.userdata1 = readback;
  callback.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* userdata, void*) {
    std::unique_ptr<Readback> readback(static_cast<Readback*>(userdata));
    RawImage image;
    bool ok = status == WGPUMapAsyncStatus_Success;
    if (ok) {
      uint64_t size = uint64_t(readback->row_pitch) * readback->height;
      auto* data =
          static_cast<const uint8_t*>(wgpuBufferGetConstMappedRange(readback->buffer, 0, size));
      image.width = readback->width;
      image.height = readback->height;
      image.stride = size_t(readback->width) * 4;
      image.data.resize(image.stride * readback->height);
      for (uint32_t y = 0; y < readback->height; ++y) {
        const uint8_t* src = data + size_t(readback->row_pitch) * y;
        uint8_t* dst = image.data.data() + image.stride * y;
        for (uint32_t x = 0; x < readback->width; ++x) {
          dst[x * 4 + 0] = src[x * 4 + (readback->bgra ? 2 : 0)];
          dst[x * 4 + 1] = src[x * 4 + 1];
          dst[x * 4 + 2] = src[x * 4 + (readback->bgra ? 0 : 2)];
          dst[x * 4 + 3] = 0xFF;
        }
      }
      wgpuBufferUnmap(readback->buffer);
    }
    wgpuBufferRelease(readback->buffer);
    readback->done(ok, std::move(image));
  };
  wgpuBufferMapAsync(readback->buffer, WGPUMapMode_Read, 0, buffer_desc.size, callback);
}

bool WebGPUPresenter::CaptureGuestOutput(RawImage& image_out) {
  if (device_thread_.IsCurrentThread()) {
    // Waiting here would block the readback it waits for.
    return false;
  }
  std::promise<bool> captured;
  std::future<bool> captured_future = captured.get_future();
  device_thread_.Call([&] {
    WGPUTexture texture = nullptr;
    uint32_t width = 0, height = 0;
    {
      uint32_t mailbox_index;
      auto consumer_lock = ConsumeGuestOutput(mailbox_index, nullptr, nullptr);
      if (mailbox_index != UINT32_MAX && guest_output_images_[mailbox_index].texture) {
        const GuestOutputImage& image = guest_output_images_[mailbox_index];
        texture = image.texture;
        width = image.width;
        height = image.height;
      }
    }
    if (!texture) {
      captured.set_value(false);
      return;
    }
    ReadBackTexture(texture, kGuestOutputFormat, width, height, [&](bool ok, RawImage&& image) {
      if (ok) {
        image_out = std::move(image);
      }
      captured.set_value(ok);
    });
  });
  return captured_future.get();
}

void WebGPUPresenter::CaptureNextPaint(std::function<void(RawImage&& image)> callback) {
  std::lock_guard<std::mutex> lock(capture_mutex_);
  capture_next_paint_ = std::move(callback);
}

}  // namespace rex::ui::webgpu
