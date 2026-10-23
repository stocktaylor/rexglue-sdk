/**
 * @file        ui/webgpu/webgpu_immediate_drawer.cpp
 * @brief       WebGPU immediate drawer (ImGui and other UI overlays)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/webgpu/immediate_drawer.h>

#include <algorithm>
#include <cstring>

#include <rex/assert.h>
#include <rex/ui/webgpu/presenter.h>

namespace rex::ui::webgpu {

namespace {

WGPUStringView View(const char* s) {
  return {s, WGPU_STRLEN};
}

const char kShader[] = R"(
@group(0) @binding(0) var ui_texture : texture_2d<f32>;
@group(0) @binding(1) var ui_sampler : sampler;

struct Vertex {
  @location(0) position : vec2f,  // clip space, transformed on upload
  @location(1) uv : vec2f,
  @location(2) color : vec4f,
}

struct Interpolants {
  @builtin(position) position : vec4f,
  @location(0) uv : vec2f,
  @location(1) color : vec4f,
}

@vertex
fn vs(in : Vertex) -> Interpolants {
  var out : Interpolants;
  out.position = vec4f(in.position, 0.0, 1.0);
  out.uv = in.uv;
  out.color = in.color;
  return out;
}

@fragment
fn fs(in : Interpolants) -> @location(0) vec4f {
  return textureSample(ui_texture, ui_sampler, in.uv) * in.color;
}
)";

}  // namespace

class WebGPUImmediateDrawer::WebGPUImmediateTexture final : public ImmediateTexture {
 public:
  WebGPUImmediateTexture(WebGPUDeviceThread& device_thread, uint32_t width, uint32_t height,
                         WGPUTexture texture, WGPUTextureView view, WGPUBindGroup bind_group)
      : ImmediateTexture(width, height),
        device_thread_(device_thread),
        texture_(texture),
        view_(view),
        bind_group_(bind_group) {}
  ~WebGPUImmediateTexture() override {
    // Possibly from the UI thread. Draws already recorded hold their own
    // references.
    device_thread_.Post([texture = texture_, view = view_, bind_group = bind_group_] {
      wgpuBindGroupRelease(bind_group);
      wgpuTextureViewRelease(view);
      wgpuTextureRelease(texture);
    });
  }

  WGPUBindGroup bind_group() const { return bind_group_; }

 private:
  WebGPUDeviceThread& device_thread_;
  WGPUTexture texture_;
  WGPUTextureView view_;
  WGPUBindGroup bind_group_;
};

std::unique_ptr<WebGPUImmediateDrawer> WebGPUImmediateDrawer::Create(
    WebGPUDeviceThread& device_thread) {
  auto drawer = std::unique_ptr<WebGPUImmediateDrawer>(new WebGPUImmediateDrawer(device_thread));
  bool initialized = false;
  device_thread.Call([&] { initialized = drawer->Initialize(); });
  if (!initialized) {
    return nullptr;
  }
  return drawer;
}

bool WebGPUImmediateDrawer::Initialize() {
  WGPUDevice dev = device().device();

  WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
  wgsl.code = View(kShader);
  WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
  module_desc.nextInChain = &wgsl.chain;
  module_desc.label = View("Immediate drawer");
  shader_ = wgpuDeviceCreateShaderModule(dev, &module_desc);

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
  texture_layout_ = wgpuDeviceCreateBindGroupLayout(dev, &layout_desc);

  WGPUPipelineLayoutDescriptor pipeline_layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
  pipeline_layout_desc.bindGroupLayoutCount = 1;
  pipeline_layout_desc.bindGroupLayouts = &texture_layout_;
  pipeline_layout_ = wgpuDeviceCreatePipelineLayout(dev, &pipeline_layout_desc);

  // Untextured draws sample this.
  const uint8_t white[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  white_texture_ =
      CreateTextureOnDeviceThread(1, 1, ImmediateTextureFilter::kNearest, false, white);

  return shader_ && texture_layout_ && pipeline_layout_ && white_texture_;
}

WebGPUImmediateDrawer::~WebGPUImmediateDrawer() {
  device_thread_.Call([this] { Shutdown(); });
}

void WebGPUImmediateDrawer::Shutdown() {
  white_texture_.reset();
  if (index_buffer_) {
    wgpuBufferRelease(index_buffer_);
  }
  if (vertex_buffer_) {
    wgpuBufferRelease(vertex_buffer_);
  }
  for (WGPURenderPipeline pipeline : pipelines_) {
    if (pipeline) {
      wgpuRenderPipelineRelease(pipeline);
    }
  }
  for (WGPUSampler sampler : samplers_) {
    if (sampler) {
      wgpuSamplerRelease(sampler);
    }
  }
  if (pipeline_layout_) {
    wgpuPipelineLayoutRelease(pipeline_layout_);
  }
  if (texture_layout_) {
    wgpuBindGroupLayoutRelease(texture_layout_);
  }
  if (shader_) {
    wgpuShaderModuleRelease(shader_);
  }
}

WGPUSampler WebGPUImmediateDrawer::GetSampler(ImmediateTextureFilter filter, bool is_repeated) {
  WGPUSampler& sampler =
      samplers_[(filter == ImmediateTextureFilter::kLinear ? 2 : 0) + (is_repeated ? 1 : 0)];
  if (!sampler) {
    WGPUSamplerDescriptor desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    WGPUAddressMode address = is_repeated ? WGPUAddressMode_Repeat : WGPUAddressMode_ClampToEdge;
    desc.addressModeU = address;
    desc.addressModeV = address;
    WGPUFilterMode mode =
        filter == ImmediateTextureFilter::kLinear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    desc.magFilter = mode;
    desc.minFilter = mode;
    sampler = wgpuDeviceCreateSampler(device().device(), &desc);
  }
  return sampler;
}

std::unique_ptr<ImmediateTexture> WebGPUImmediateDrawer::CreateTexture(
    uint32_t width, uint32_t height, ImmediateTextureFilter filter, bool is_repeated,
    const uint8_t* data) {
  std::unique_ptr<ImmediateTexture> texture;
  device_thread_.Call(
      [&] { texture = CreateTextureOnDeviceThread(width, height, filter, is_repeated, data); });
  return texture;
}

std::unique_ptr<ImmediateTexture> WebGPUImmediateDrawer::CreateTextureOnDeviceThread(
    uint32_t width, uint32_t height, ImmediateTextureFilter filter, bool is_repeated,
    const uint8_t* data) {
  WGPUTextureDescriptor texture_desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
  texture_desc.label = View("Immediate texture");
  texture_desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
  texture_desc.size = {width, height, 1};
  texture_desc.format = WGPUTextureFormat_RGBA8Unorm;
  WGPUTexture texture = wgpuDeviceCreateTexture(device().device(), &texture_desc);
  if (!texture) {
    return nullptr;
  }
  if (data) {
    WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    destination.texture = texture;
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = width * 4;
    layout.rowsPerImage = height;
    WGPUExtent3D extent = {width, height, 1};
    wgpuQueueWriteTexture(device().queue(), &destination, data, size_t(width) * height * 4, &layout,
                          &extent);
  }
  WGPUTextureView view = wgpuTextureCreateView(texture, nullptr);
  WGPUBindGroupEntry entries[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
  entries[0].binding = 0;
  entries[0].textureView = view;
  entries[1].binding = 1;
  entries[1].sampler = GetSampler(filter, is_repeated);
  WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
  group_desc.layout = texture_layout_;
  group_desc.entryCount = 2;
  group_desc.entries = entries;
  WGPUBindGroup bind_group = wgpuDeviceCreateBindGroup(device().device(), &group_desc);
  return std::make_unique<WebGPUImmediateTexture>(device_thread_, width, height, texture, view,
                                                  bind_group);
}

WGPURenderPipeline WebGPUImmediateDrawer::GetPipeline(ImmediatePrimitiveType primitive_type,
                                                      WGPUTextureFormat format) {
  if (pipeline_format_ != format) {
    for (WGPURenderPipeline& pipeline : pipelines_) {
      if (pipeline) {
        wgpuRenderPipelineRelease(pipeline);
        pipeline = nullptr;
      }
    }
    pipeline_format_ = format;
  }
  bool lines = primitive_type == ImmediatePrimitiveType::kLines;
  WGPURenderPipeline& pipeline = pipelines_[lines ? 1 : 0];
  if (pipeline) {
    return pipeline;
  }

  WGPUVertexAttribute attributes[3] = {WGPU_VERTEX_ATTRIBUTE_INIT, WGPU_VERTEX_ATTRIBUTE_INIT,
                                       WGPU_VERTEX_ATTRIBUTE_INIT};
  attributes[0].format = WGPUVertexFormat_Float32x2;
  attributes[0].offset = offsetof(ImmediateVertex, x);
  attributes[0].shaderLocation = 0;
  attributes[1].format = WGPUVertexFormat_Float32x2;
  attributes[1].offset = offsetof(ImmediateVertex, u);
  attributes[1].shaderLocation = 1;
  attributes[2].format = WGPUVertexFormat_Unorm8x4;
  attributes[2].offset = offsetof(ImmediateVertex, color);
  attributes[2].shaderLocation = 2;
  WGPUVertexBufferLayout vertex_layout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
  vertex_layout.arrayStride = sizeof(ImmediateVertex);
  vertex_layout.attributeCount = 3;
  vertex_layout.attributes = attributes;

  WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
  blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
  blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
  blend.alpha.srcFactor = WGPUBlendFactor_One;
  blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
  WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
  target.format = format;
  target.blend = &blend;
  WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
  fragment.module = shader_;
  fragment.entryPoint = View("fs");
  fragment.targetCount = 1;
  fragment.targets = &target;

  WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
  desc.label = View("Immediate drawer");
  desc.layout = pipeline_layout_;
  desc.vertex.module = shader_;
  desc.vertex.entryPoint = View("vs");
  desc.vertex.bufferCount = 1;
  desc.vertex.buffers = &vertex_layout;
  desc.primitive.topology =
      lines ? WGPUPrimitiveTopology_LineList : WGPUPrimitiveTopology_TriangleList;
  desc.fragment = &fragment;
  pipeline = wgpuDeviceCreateRenderPipeline(device().device(), &desc);
  return pipeline;
}

uint64_t WebGPUImmediateDrawer::Upload(WGPUBuffer& buffer, uint64_t& capacity, uint64_t& used,
                                       WGPUBufferUsage usage, const void* data, uint64_t size) {
  uint64_t padded = (size + 3) & ~uint64_t(3);
  if (!buffer || used + padded > capacity) {
    // Draws recorded earlier keep the old buffer alive; this frame continues
    // in the new one.
    if (buffer) {
      wgpuBufferRelease(buffer);
    }
    capacity = std::max<uint64_t>(std::max<uint64_t>(capacity * 2, padded), 64 * 1024);
    WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    desc.size = capacity;
    desc.usage = usage | WGPUBufferUsage_CopyDst;
    buffer = wgpuDeviceCreateBuffer(device().device(), &desc);
    used = 0;
  }
  uint64_t offset = used;
  if (padded == size) {
    wgpuQueueWriteBuffer(device().queue(), buffer, offset, data, size);
  } else {
    // writeBuffer sizes must be multiples of 4.
    std::vector<uint8_t> staging(padded, 0);
    std::memcpy(staging.data(), data, size);
    wgpuQueueWriteBuffer(device().queue(), buffer, offset, staging.data(), padded);
  }
  used += padded;
  return offset;
}

void WebGPUImmediateDrawer::Begin(UIDrawContext& ui_draw_context, float coordinate_space_width,
                                  float coordinate_space_height) {
  assert_true(device_thread_.IsCurrentThread());
  ImmediateDrawer::Begin(ui_draw_context, coordinate_space_width, coordinate_space_height);
  auto& webgpu_context = static_cast<WebGPUPresenter::WebGPUUIDrawContext&>(ui_draw_context);
  if (webgpu_context.pass() != pass_) {
    // A new paint: the previous one's uploads are already queued before it.
    vertex_used_ = 0;
    index_used_ = 0;
  }
  pass_ = webgpu_context.pass();
  format_ = webgpu_context.format();
}

void WebGPUImmediateDrawer::BeginDrawBatch(const ImmediateDrawBatch& batch) {
  assert_not_null(pass_);
  batch_valid_ = batch.vertex_count > 0;
  if (!batch_valid_) {
    return;
  }
  // To clip space: x from [0, width] to [-1, 1], y from [0, height] to [1, -1].
  float scale_x = 2.0f / coordinate_space_width();
  float scale_y = -2.0f / coordinate_space_height();
  transformed_.assign(batch.vertices, batch.vertices + batch.vertex_count);
  for (ImmediateVertex& vertex : transformed_) {
    vertex.x = vertex.x * scale_x - 1.0f;
    vertex.y = vertex.y * scale_y + 1.0f;
  }
  batch_vertex_offset_ =
      Upload(vertex_buffer_, vertex_capacity_, vertex_used_, WGPUBufferUsage_Vertex,
             transformed_.data(), transformed_.size() * sizeof(ImmediateVertex));
  wgpuRenderPassEncoderSetVertexBuffer(pass_, 0, vertex_buffer_, batch_vertex_offset_,
                                       transformed_.size() * sizeof(ImmediateVertex));
  batch_indexed_ = batch.indices && batch.index_count > 0;
  if (batch_indexed_) {
    batch_index_offset_ = Upload(index_buffer_, index_capacity_, index_used_, WGPUBufferUsage_Index,
                                 batch.indices, uint64_t(batch.index_count) * sizeof(uint16_t));
    wgpuRenderPassEncoderSetIndexBuffer(pass_, index_buffer_, WGPUIndexFormat_Uint16,
                                        batch_index_offset_,
                                        uint64_t(batch.index_count) * sizeof(uint16_t));
  }
}

void WebGPUImmediateDrawer::Draw(const ImmediateDraw& draw) {
  if (!batch_valid_ || draw.count <= 0) {
    return;
  }
  uint32_t left, top, width, height;
  if (!ScissorToRenderTarget(draw, left, top, width, height)) {
    return;
  }
  wgpuRenderPassEncoderSetScissorRect(pass_, left, top, width, height);
  wgpuRenderPassEncoderSetPipeline(pass_, GetPipeline(draw.primitive_type, format_));
  auto* texture =
      static_cast<WebGPUImmediateTexture*>(draw.texture ? draw.texture : white_texture_.get());
  wgpuRenderPassEncoderSetBindGroup(pass_, 0, texture->bind_group(), 0, nullptr);
  if (batch_indexed_) {
    wgpuRenderPassEncoderDrawIndexed(pass_, uint32_t(draw.count), 1, uint32_t(draw.index_offset),
                                     draw.base_vertex, 0);
  } else {
    wgpuRenderPassEncoderDraw(pass_, uint32_t(draw.count), 1, uint32_t(draw.base_vertex), 0);
  }
}

void WebGPUImmediateDrawer::EndDrawBatch() {
  batch_valid_ = false;
}

void WebGPUImmediateDrawer::End() {
  if (pass_) {
    // Leave the pass as the presenter set it.
    auto* context = ui_draw_context();
    wgpuRenderPassEncoderSetScissorRect(pass_, 0, 0, context->render_target_width(),
                                        context->render_target_height());
  }
  ImmediateDrawer::End();
}

}  // namespace rex::ui::webgpu
