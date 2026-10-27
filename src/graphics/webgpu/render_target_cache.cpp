/**
 * @file        graphics/webgpu/render_target_cache.cpp
 * @brief       Host render targets for the WebGPU backend (minimal)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/render_target_cache.h>

#include <algorithm>
#include <cstring>

#include <rex/graphics/webgpu/command_processor.h>
#include <rex/graphics/webgpu/shared_memory.h>
#include <rex/graphics/webgpu/texture_cache.h>
#include <rex/logging.h>
#include <rex/math.h>

namespace rex::graphics::webgpu {

namespace shaders {
#include "../shaders/webgpu_wgsl/resolve_fast_32bpp_1x2xmsaa_cs.h"
#include "../shaders/webgpu_wgsl/resolve_fast_32bpp_4xmsaa_cs.h"
#include "../shaders/webgpu_wgsl/resolve_fast_64bpp_1x2xmsaa_cs.h"
#include "../shaders/webgpu_wgsl/resolve_fast_64bpp_4xmsaa_cs.h"
#include "../shaders/webgpu_wgsl/resolve_full_128bpp_cs.h"
#include "../shaders/webgpu_wgsl/resolve_full_16bpp_cs.h"
#include "../shaders/webgpu_wgsl/resolve_full_32bpp_cs.h"
#include "../shaders/webgpu_wgsl/resolve_full_64bpp_cs.h"
#include "../shaders/webgpu_wgsl/resolve_full_8bpp_cs.h"
}  // namespace shaders

namespace {

WGPUStringView View(const char* s) {
  return {s, WGPU_STRLEN};
}

// Writes a rectangle of a render target to the EDRAM buffer in the guest's
// layout (XeEdramOffsetBytes in Xenia's edram.xesli, every sample of each
// pixel as the host render target has one, depth tiles with their halves
// swapped) and encoding (the inverse of the resolve shaders' unpacking: 7e3
// floats as DirectXTex, 16-bit fixed with the -32...32 range, depth in the
// upper 24 bits as unorm or 20e4 as Float32To20e4, stencil in the lower 8).
// main dumps color render targets, main_depth depth ones.
const char kDumpShader[] = R"(
struct Dump {
  base_tiles: u32,
  pitch_tiles: u32,
  msaa_samples: u32,
  format: u32,
  format_ints_log2: u32,
  origin_x: u32,
  origin_y: u32,
  width: u32,
  height: u32,
}
@group(0) @binding(0) var<storage, read_write> edram: array<u32>;
@group(0) @binding(1) var source: texture_2d<f32>;
@group(0) @binding(2) var<uniform> dump: Dump;
@group(0) @binding(3) var source_depth: texture_depth_2d;
@group(0) @binding(4) var source_stencil: texture_2d<u32>;

fn float_to_7e3(value: f32) -> u32 {
  let f = clamp(value, 0.0, 31.875);
  let bits = bitcast<u32>(f);
  var biased: u32;
  if (f < 0.25) {
    biased = ((bits & 0x7FFFFFu) | 0x800000u) >> min(125u - (bits >> 23u), 24u);
  } else {
    biased = bits + 0xC2000000u;
  }
  return ((biased + 0x7FFFu + ((biased >> 16u) & 1u)) >> 16u) & 0x3FFu;
}

fn float_to_20e4(value: f32) -> u32 {
  if (!(value > 0.0)) {
    return 0u;
  }
  var bits = bitcast<u32>(value);
  if (bits >= 0x3FFFFFF8u) {
    return 0xFFFFFFu;
  }
  if (bits < 0x38800000u) {
    bits = (0x800000u | (bits & 0x7FFFFFu)) >> min(113u - (bits >> 23u), 24u);
  } else {
    bits += 0xC8000000u;
  }
  bits += 3u + ((bits >> 3u) & 1u);
  return (bits >> 3u) & 0xFFFFFFu;
}

fn fixed16(value: f32) -> u32 {
  return u32(i32(clamp(value, -32.0, 32.0) * (32767.0 / 32.0))) & 0xFFFFu;
}

fn dump_pixel(id: vec3u) -> vec2u {
  return vec2u(dump.origin_x + id.x, dump.origin_y + id.y);
}

fn write_pixel(pixel: vec2u, packed: vec2u, is_depth: bool) {
  let sample_shift = vec2u(select(0u, 1u, dump.msaa_samples >= 2u),
                           select(0u, 1u, dump.msaa_samples >= 1u));
  let tile_size = vec2u(80u >> dump.format_ints_log2, 16u);
  for (var s = 0u; s < (1u << dump.msaa_samples); s++) {
    var sample_index = (pixel << sample_shift) + ((vec2u(s) >> vec2u(1u, 0u)) & vec2u(1u));
    let tile = sample_index / tile_size;
    sample_index -= tile * tile_size;
    if (is_depth) {
      let half_width = tile_size.x >> 1u;
      sample_index.x = select(sample_index.x + half_width, sample_index.x - half_width,
                              sample_index.x >= half_width);
    }
    let address = ((dump.base_tiles + tile.y * dump.pitch_tiles + tile.x) * 1280u +
                   ((sample_index.y * tile_size.x + sample_index.x) << dump.format_ints_log2)) %
                  (1280u * 2048u);
    edram[address] = packed.x;
    if (dump.format_ints_log2 != 0u) {
      edram[address + 1u] = packed.y;
    }
  }
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) id: vec3u) {
  if (id.x >= dump.width || id.y >= dump.height) {
    return;
  }
  let pixel = dump_pixel(id);
  let color = textureLoad(source, pixel, 0);
  var packed = vec2u(0u, 0u);
  switch (dump.format) {
    case 0u, 1u: {  // k_8_8_8_8, k_8_8_8_8_GAMMA (gamma applied when drawing)
      packed.x = pack4x8unorm(color);
    }
    case 2u, 10u: {  // k_2_10_10_10, k_2_10_10_10_AS_10_10_10_10
      let c = vec4u(round(saturate(color) * vec4f(1023.0, 1023.0, 1023.0, 3.0)));
      packed.x = c.r | (c.g << 10u) | (c.b << 20u) | (c.a << 30u);
    }
    case 3u, 12u: {  // k_2_10_10_10_FLOAT, k_2_10_10_10_FLOAT_AS_16_16_16_16
      packed.x = float_to_7e3(color.r) | (float_to_7e3(color.g) << 10u) |
                 (float_to_7e3(color.b) << 20u) | (u32(round(saturate(color.a) * 3.0)) << 30u);
    }
    case 4u: {  // k_16_16
      packed.x = fixed16(color.r) | (fixed16(color.g) << 16u);
    }
    case 5u: {  // k_16_16_16_16
      packed = vec2u(fixed16(color.r) | (fixed16(color.g) << 16u),
                     fixed16(color.b) | (fixed16(color.a) << 16u));
    }
    case 6u: {  // k_16_16_FLOAT
      packed.x = pack2x16float(color.rg);
    }
    case 7u: {  // k_16_16_16_16_FLOAT
      packed = vec2u(pack2x16float(color.rg), pack2x16float(color.ba));
    }
    case 14u: {  // k_32_FLOAT
      packed.x = bitcast<u32>(color.r);
    }
    case 15u: {  // k_32_32_FLOAT
      packed = bitcast<vec2u>(color.rg);
    }
    default: {}
  }
  write_pixel(pixel, packed, false);
}

@compute @workgroup_size(8, 8, 1)
fn main_depth(@builtin(global_invocation_id) id: vec3u) {
  if (id.x >= dump.width || id.y >= dump.height) {
    return;
  }
  let pixel = dump_pixel(id);
  let depth = textureLoad(source_depth, pixel, 0);
  let stencil = textureLoad(source_stencil, pixel, 0).r & 0xFFu;
  var depth24: u32;
  if (dump.format == 1u) {  // kD24FS8
    depth24 = float_to_20e4(depth);
  } else {  // kD24S8
    depth24 = u32(round(saturate(depth) * 16777215.0));
  }
  write_pixel(pixel, vec2u((depth24 << 8u) | stencil, 0u), true);
}
)";

struct DumpConstants {
  uint32_t base_tiles;
  uint32_t pitch_tiles;
  uint32_t msaa_samples;
  uint32_t format;
  uint32_t format_ints_log2;
  uint32_t origin_x;
  uint32_t origin_y;
  uint32_t width;
  uint32_t height;
};

// The resolve copy shaders' push constants as a uniform buffer: 20 bytes.
constexpr uint64_t kCopyConstantsBindingSize = 32;
constexpr uint64_t kConstantsStride = 256;

}  // namespace

WebGPURenderTargetCache::~WebGPURenderTargetCache() {
  Shutdown();
}

bool WebGPURenderTargetCache::Initialize() {
  static const char* const kCopyShaders[size_t(draw_util::ResolveCopyShaderIndex::kCount)] = {
      shaders::resolve_fast_32bpp_1x2xmsaa_cs_wgsl, shaders::resolve_fast_32bpp_4xmsaa_cs_wgsl,
      shaders::resolve_fast_64bpp_1x2xmsaa_cs_wgsl, shaders::resolve_fast_64bpp_4xmsaa_cs_wgsl,
      shaders::resolve_full_8bpp_cs_wgsl,           shaders::resolve_full_16bpp_cs_wgsl,
      shaders::resolve_full_32bpp_cs_wgsl,          shaders::resolve_full_64bpp_cs_wgsl,
      shaders::resolve_full_128bpp_cs_wgsl,
  };
  bool ok = true;
  ui::webgpu::WebGPUDeviceThread& device_thread = command_processor_.device_thread();
  device_thread.Call([&] {
    const ui::webgpu::WebGPUDevice& device = device_thread.device();
    WGPUDevice wgpu_device = device.device();
    min_storage_offset_alignment_ = device.limits().minStorageBufferOffsetAlignment;

    WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    buffer_desc.label = View("EDRAM");
    buffer_desc.size = xenos::kEdramSizeBytes;
    buffer_desc.usage = WGPUBufferUsage_Storage;
    edram_buffer_ = wgpuDeviceCreateBuffer(wgpu_device, &buffer_desc);

    auto create_module = [&](const char* code, const char* label) {
      WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
      source.code = View(code);
      WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
      module_desc.nextInChain = &source.chain;
      module_desc.label = View(label);
      return wgpuDeviceCreateShaderModule(wgpu_device, &module_desc);
    };

    // The dump layouts are explicit: color render targets may be 32-bit float
    // (unfilterable), and depth ones are read through single-aspect views.
    WGPUShaderModule dump_module = create_module(kDumpShader, "Render target dump");
    auto create_dump_pipeline = [&](const WGPUBindGroupLayoutEntry* texture_entries,
                                    size_t texture_entry_count, const char* entry_point,
                                    WGPUBindGroupLayout& layout_out) {
      WGPUBindGroupLayoutEntry entries[4] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
                                             WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
      entries[0].binding = 0;
      entries[0].visibility = WGPUShaderStage_Compute;
      entries[0].buffer.type = WGPUBufferBindingType_Storage;
      entries[1].binding = 2;
      entries[1].visibility = WGPUShaderStage_Compute;
      entries[1].buffer.type = WGPUBufferBindingType_Uniform;
      for (size_t i = 0; i < texture_entry_count; ++i) {
        entries[2 + i] = texture_entries[i];
      }
      WGPUBindGroupLayoutDescriptor layout_desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
      layout_desc.entryCount = 2 + texture_entry_count;
      layout_desc.entries = entries;
      layout_out = wgpuDeviceCreateBindGroupLayout(wgpu_device, &layout_desc);
      WGPUPipelineLayoutDescriptor pipeline_layout_desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
      pipeline_layout_desc.bindGroupLayoutCount = 1;
      pipeline_layout_desc.bindGroupLayouts = &layout_out;
      WGPUPipelineLayout pipeline_layout =
          wgpuDeviceCreatePipelineLayout(wgpu_device, &pipeline_layout_desc);
      WGPUComputePipelineDescriptor pipeline_desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
      pipeline_desc.label = View("Render target dump");
      pipeline_desc.layout = pipeline_layout;
      pipeline_desc.compute.module = dump_module;
      pipeline_desc.compute.entryPoint = View(entry_point);
      WGPUComputePipeline pipeline = wgpuDeviceCreateComputePipeline(wgpu_device, &pipeline_desc);
      wgpuPipelineLayoutRelease(pipeline_layout);
      return pipeline;
    };
    WGPUBindGroupLayoutEntry color_entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    color_entry.binding = 1;
    color_entry.visibility = WGPUShaderStage_Compute;
    color_entry.texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
    color_entry.texture.viewDimension = WGPUTextureViewDimension_2D;
    dump_pipeline_ = create_dump_pipeline(&color_entry, 1, "main", dump_layout_);
    WGPUBindGroupLayoutEntry depth_entries[2] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
                                                 WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
    depth_entries[0].binding = 3;
    depth_entries[0].visibility = WGPUShaderStage_Compute;
    depth_entries[0].texture.sampleType = WGPUTextureSampleType_Depth;
    depth_entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    depth_entries[1].binding = 4;
    depth_entries[1].visibility = WGPUShaderStage_Compute;
    depth_entries[1].texture.sampleType = WGPUTextureSampleType_Uint;
    depth_entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
    depth_dump_pipeline_ = create_dump_pipeline(depth_entries, 2, "main_depth", depth_dump_layout_);
    wgpuShaderModuleRelease(dump_module);

    for (size_t i = 0; i < copy_pipelines_.size(); ++i) {
      const char* name = draw_util::resolve_copy_shader_info[i].debug_name;
      WGPUShaderModule module = create_module(kCopyShaders[i], name);
      WGPUComputePipelineDescriptor copy_desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
      copy_desc.label = View(name);
      copy_desc.compute.module = module;
      copy_desc.compute.entryPoint = View("main");
      copy_pipelines_[i] = wgpuDeviceCreateComputePipeline(wgpu_device, &copy_desc);
      wgpuShaderModuleRelease(module);
      ok &= copy_pipelines_[i] != nullptr;
    }
    ok &= edram_buffer_ && dump_layout_ && dump_pipeline_ && depth_dump_layout_ &&
          depth_dump_pipeline_;
  });
  if (!ok) {
    REXGPU_ERROR("WebGPU: couldn't create the resolve objects");
  }
  return ok;
}

WGPUTextureFormat WebGPURenderTargetCache::GetColorFormat(xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return WGPUTextureFormat_RGBA8Unorm;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return WGPUTextureFormat_RGB10A2Unorm;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return WGPUTextureFormat_R32Float;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return WGPUTextureFormat_RG32Float;
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return WGPUTextureFormat_RG16Float;
    default:
      return WGPUTextureFormat_RGBA16Float;
  }
}

WGPUTextureFormat WebGPURenderTargetCache::GetDepthFormat(xenos::DepthRenderTargetFormat format) {
  (void)format;
  // Float24 needs the pixel shader conversion with a 32-bit float format
  // (depth32float-stencil8 is optional); unorm is enough to start.
  return WGPUTextureFormat_Depth24PlusStencil8;
}

WebGPURenderTargetCache::RenderTarget* WebGPURenderTargetCache::GetColor(
    uint32_t base_tiles, uint32_t pitch, xenos::ColorRenderTargetFormat format) {
  WGPUTextureFormat host_format = GetColorFormat(format);
  uint64_t key = uint64_t(base_tiles) | (uint64_t(pitch) << 12) | (uint64_t(host_format) << 32);
  return GetOrCreate(key, pitch, host_format, false);
}

WebGPURenderTargetCache::RenderTarget* WebGPURenderTargetCache::GetDepth(
    uint32_t base_tiles, uint32_t pitch, xenos::DepthRenderTargetFormat format) {
  WGPUTextureFormat host_format = GetDepthFormat(format);
  uint64_t key = uint64_t(base_tiles) | (uint64_t(pitch) << 12) | (uint64_t(host_format) << 32) |
                 (uint64_t(1) << 63);
  return GetOrCreate(key, pitch, host_format, true);
}

WebGPURenderTargetCache::RenderTarget* WebGPURenderTargetCache::GetOrCreate(
    uint64_t key, uint32_t pitch, WGPUTextureFormat format, bool is_depth) {
  auto it = render_targets_.find(key);
  if (it != render_targets_.end()) {
    return it->second.get();
  }
  auto render_target = std::make_unique<RenderTarget>();
  render_target->format = format;
  render_target->width = std::clamp(pitch, uint32_t(1), uint32_t(8192));
  render_target->height = kMaxHeight;
  render_target->is_depth = is_depth;
  command_processor_.device_thread().Call([&] {
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.label = {is_depth ? "Depth render target" : "Color render target", WGPU_STRLEN};
    desc.size = {render_target->width, render_target->height, 1};
    desc.format = format;
    desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding |
                 WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst;
    render_target->texture =
        wgpuDeviceCreateTexture(command_processor_.device_thread().device().device(), &desc);
    if (render_target->texture) {
      render_target->view = wgpuTextureCreateView(render_target->texture, nullptr);
      if (is_depth) {
        WGPUTextureViewDescriptor view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        view_desc.aspect = WGPUTextureAspect_DepthOnly;
        render_target->depth_view = wgpuTextureCreateView(render_target->texture, &view_desc);
        view_desc.aspect = WGPUTextureAspect_StencilOnly;
        render_target->stencil_view = wgpuTextureCreateView(render_target->texture, &view_desc);
      }
    }
  });
  if (!render_target->view) {
    REXGPU_ERROR("WebGPU: couldn't create a {}x{} render target", render_target->width,
                 render_target->height);
    return nullptr;
  }
  REXGPU_INFO("WebGPU: {} render target at tile {}, {}x{}, host format {}",
              is_depth ? "depth" : "color", uint32_t(key & 0xFFF), render_target->width,
              render_target->height, uint32_t(format));
  RenderTarget* result = render_target.get();
  render_targets_.emplace(key, std::move(render_target));
  return result;
}

void WebGPURenderTargetCache::ResolveCopy(const draw_util::ResolveInfo& resolve_info,
                                          RenderTarget& source, WebGPUSharedMemory& shared_memory,
                                          WebGPUTextureCache& texture_cache) {
  if (!resolve_info.copy_dest_extent_length) {
    return;
  }
  bool is_depth = resolve_info.IsCopyingDepth();
  if (is_depth != source.is_depth || (is_depth && !source.stencil_view)) {
    return;
  }
  draw_util::ResolveCopyShaderConstants copy_constants;
  uint32_t group_count_x, group_count_y;
  draw_util::ResolveCopyShaderIndex copy_shader =
      resolve_info.GetCopyShader(1, 1, copy_constants, group_count_x, group_count_y);
  if (copy_shader == draw_util::ResolveCopyShaderIndex::kUnknown || !group_count_x ||
      !group_count_y) {
    return;
  }

  // The whole tiles the copy reads, as a rectangle of the render target.
  const draw_util::ResolveEdramInfo& edram_info =
      is_depth ? resolve_info.depth_edram_info : resolve_info.color_edram_info;
  uint32_t original_base =
      is_depth ? resolve_info.depth_original_base : resolve_info.color_original_base;
  uint32_t span_base, span_row_length, span_rows, span_pitch;
  resolve_info.GetCopyEdramTileSpan(span_base, span_row_length, span_rows, span_pitch);
  span_pitch = std::max(span_pitch, uint32_t(1));
  uint32_t relative_tiles =
      (span_base + xenos::kEdramTileCount - original_base) % xenos::kEdramTileCount;
  uint32_t tile_width = xenos::kEdramTileWidthSamples >> uint32_t(edram_info.format_is_64bpp);
  uint32_t sample_shift_x = uint32_t(edram_info.msaa_samples >= xenos::MsaaSamples::k4X);
  uint32_t sample_shift_y = uint32_t(edram_info.msaa_samples >= xenos::MsaaSamples::k2X);
  DumpConstants dump;
  dump.base_tiles = original_base;
  dump.pitch_tiles = span_pitch;
  dump.msaa_samples = uint32_t(edram_info.msaa_samples);
  dump.format = edram_info.format;
  dump.format_ints_log2 = edram_info.format_is_64bpp;
  dump.origin_x = ((relative_tiles % span_pitch) * tile_width) >> sample_shift_x;
  dump.origin_y =
      ((relative_tiles / span_pitch) * xenos::kEdramTileHeightSamples) >> sample_shift_y;
  if (dump.origin_x >= source.width || dump.origin_y >= source.height) {
    return;
  }
  dump.width =
      std::min((span_row_length * tile_width) >> sample_shift_x, source.width - dump.origin_x);
  dump.height = std::min((span_rows * xenos::kEdramTileHeightSamples) >> sample_shift_y,
                         source.height - dump.origin_y);

  // The destination, bound from an aligned offset that dest_base is relative to.
  if (!shared_memory.RequestRange(resolve_info.copy_dest_extent_start,
                                  resolve_info.copy_dest_extent_length)) {
    REXGPU_ERROR("WebGPU: couldn't obtain the resolve destination");
    return;
  }
  uint64_t dest_offset = resolve_info.copy_dest_base & ~uint64_t(min_storage_offset_alignment_ - 1);
  uint64_t dest_size = rex::round_up(uint64_t(resolve_info.copy_dest_extent_start) +
                                         resolve_info.copy_dest_extent_length - dest_offset,
                                     uint64_t(16));
  dest_size = std::min(dest_size, uint64_t(SharedMemory::kBufferSize) - dest_offset);
  copy_constants.dest_base -= uint32_t(dest_offset);

  command_processor_.EndRenderPass();
  WGPUBuffer edram_buffer = edram_buffer_;
  WGPUBindGroupLayout dump_layout = is_depth ? depth_dump_layout_ : dump_layout_;
  WGPUComputePipeline dump_pipeline = is_depth ? depth_dump_pipeline_ : dump_pipeline_;
  WGPUComputePipeline copy_pipeline = copy_pipelines_[size_t(copy_shader)];
  WGPUTextureView source_view = is_depth ? source.depth_view : source.view;
  WGPUTextureView stencil_view = source.stencil_view;
  WGPUBuffer shared_memory_buffer = shared_memory.buffer();
  command_processor_.command_list().Record([=](WebGPUCommandList::Encoder& encoder) {
    WGPUDevice device = encoder.device;
    WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    buffer_desc.label = View("Resolve constants");
    buffer_desc.size = kConstantsStride * 2;
    buffer_desc.usage = WGPUBufferUsage_Uniform;
    buffer_desc.mappedAtCreation = true;
    WGPUBuffer constants = wgpuDeviceCreateBuffer(device, &buffer_desc);
    auto* mapping = static_cast<uint8_t*>(wgpuBufferGetMappedRange(constants, 0, buffer_desc.size));
    if (mapping) {
      std::memcpy(mapping, &dump, sizeof(dump));
      std::memcpy(mapping + kConstantsStride, &copy_constants, sizeof(copy_constants));
    }
    wgpuBufferUnmap(constants);

    WGPUBindGroupEntry dump_entries[4] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
                                          WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    dump_entries[0].binding = 0;
    dump_entries[0].buffer = edram_buffer;
    dump_entries[0].size = xenos::kEdramSizeBytes;
    dump_entries[1].binding = 2;
    dump_entries[1].buffer = constants;
    dump_entries[1].size = rex::round_up(sizeof(DumpConstants), size_t(16));
    dump_entries[2].binding = is_depth ? 3 : 1;
    dump_entries[2].textureView = source_view;
    dump_entries[3].binding = 4;
    dump_entries[3].textureView = stencil_view;
    WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    group_desc.layout = dump_layout;
    group_desc.entryCount = is_depth ? 4 : 3;
    group_desc.entries = dump_entries;
    WGPUBindGroup dump_group = wgpuDeviceCreateBindGroup(device, &group_desc);

    auto create_copy_group = [&](uint32_t group, WGPUBuffer buffer, uint64_t offset,
                                 uint64_t size) {
      WGPUBindGroupLayout layout = wgpuComputePipelineGetBindGroupLayout(copy_pipeline, group);
      WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
      entry.binding = 0;
      entry.buffer = buffer;
      entry.offset = offset;
      entry.size = size;
      WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
      desc.layout = layout;
      desc.entryCount = 1;
      desc.entries = &entry;
      WGPUBindGroup bind_group = wgpuDeviceCreateBindGroup(device, &desc);
      wgpuBindGroupLayoutRelease(layout);
      return bind_group;
    };
    WGPUBindGroup copy_groups[3] = {
        create_copy_group(0, edram_buffer, 0, xenos::kEdramSizeBytes),
        create_copy_group(1, shared_memory_buffer, dest_offset, dest_size),
        create_copy_group(2, constants, kConstantsStride, kCopyConstantsBindingSize),
    };

    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(encoder.encoder, nullptr);
    wgpuComputePassEncoderSetPipeline(pass, dump_pipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, dump_group, 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(pass, (dump.width + 7) / 8, (dump.height + 7) / 8, 1);
    wgpuComputePassEncoderSetPipeline(pass, copy_pipeline);
    for (uint32_t i = 0; i < 3; ++i) {
      wgpuComputePassEncoderSetBindGroup(pass, i, copy_groups[i], 0, nullptr);
    }
    wgpuComputePassEncoderDispatchWorkgroups(pass, group_count_x, group_count_y, 1);
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);

    wgpuBindGroupRelease(dump_group);
    for (WGPUBindGroup group : copy_groups) {
      wgpuBindGroupRelease(group);
    }
    wgpuBufferRelease(constants);
  });

  texture_cache.MarkRangeAsResolved(resolve_info.copy_dest_extent_start,
                                    resolve_info.copy_dest_extent_length);
}

void WebGPURenderTargetCache::Shutdown() {
  auto render_targets = std::make_shared<decltype(render_targets_)>(std::move(render_targets_));
  render_targets_.clear();
  WGPUBuffer edram_buffer = edram_buffer_;
  WGPUBindGroupLayout dump_layouts[2] = {dump_layout_, depth_dump_layout_};
  WGPUComputePipeline dump_pipelines[2] = {dump_pipeline_, depth_dump_pipeline_};
  auto copy_pipelines = copy_pipelines_;
  edram_buffer_ = nullptr;
  dump_layout_ = nullptr;
  dump_pipeline_ = nullptr;
  depth_dump_layout_ = nullptr;
  depth_dump_pipeline_ = nullptr;
  copy_pipelines_.fill(nullptr);
  command_processor_.device_thread().Post(
      [render_targets, edram_buffer, dump_layouts, dump_pipelines, copy_pipelines] {
        for (auto& [key, render_target] : *render_targets) {
          for (WGPUTextureView view :
               {render_target->view, render_target->depth_view, render_target->stencil_view}) {
            if (view) {
              wgpuTextureViewRelease(view);
            }
          }
          wgpuTextureRelease(render_target->texture);
        }
        for (WGPUComputePipeline pipeline : copy_pipelines) {
          if (pipeline) {
            wgpuComputePipelineRelease(pipeline);
          }
        }
        for (WGPUComputePipeline pipeline : dump_pipelines) {
          if (pipeline) {
            wgpuComputePipelineRelease(pipeline);
          }
        }
        for (WGPUBindGroupLayout layout : dump_layouts) {
          if (layout) {
            wgpuBindGroupLayoutRelease(layout);
          }
        }
        if (edram_buffer) {
          wgpuBufferRelease(edram_buffer);
        }
      });
}

}  // namespace rex::graphics::webgpu
