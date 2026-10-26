/**
 * @file        tests/webgpu/xenos_shaders_test.cpp
 * @brief       Translates guest shaders to WGSL and validates them on a device
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

// Usage: rexglue-webgpu-xenos-shaders [--dump DIR] [--dump-all] [--verbose]
//            [--resolution-scale N] FILE.xsh...
//
// Reads Xenos shader storage files (the shareable .xsh caches written by the
// Vulkan and D3D12 backends), translates every shader with
// WgslShaderTranslator under a few modifications covering the translator's
// paths (host vertex shader types: vertices, points, rectangles; PsParamGen, centroid
// interpolation, clip distances, early depth hint, float24 depth), and has the device validate the
// WGSL: a shader module, then a render pipeline with an automatic layout (vertex shaders alone,
// pixel shaders behind a vertex shader with matching outputs). With --dump, each rejected shader's
// WGSL and ucode disassembly go to DIR (every shader's with
// --dump-all). The translator's float24 depth and PWL gamma helpers, taken
// from the translated shaders, also run in a compute shader over millions of
// inputs, compared with the CPU versions in xenos.h (depth bit-exact, gamma
// within 1e-6 as GPUs may fuse multiply-adds). An invalid shader is validated too, and must be
// rejected, so a pass means the validation ran. --resolution-scale translates for a draw resolution
// scale (the resolve-scaled texture and PsParamGen paths). Shaders using more textures or samplers
// than WebGPU's default per-stage limit (16) are counted, as they need devices with higher limits.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/shader/wgsl.h>
#include <rex/graphics/xenos.h>
#include <rex/graphics/pipeline/shader/wgsl_translator.h>
#include <rex/string/buffer.h>
#include <rex/ui/webgpu/device_thread.h>

// The one GPU cvar the translator reads (src/graphics/flags.cpp would bring in
// the rest of the GPU and UI).
REXCVAR_DEFINE_STRING(dump_shaders, "", "GPU", "Path to dump shaders to");

namespace {

using rex::graphics::Shader;
using rex::graphics::WgslShader;
using rex::graphics::WgslShaderTranslator;
using rex::ui::webgpu::kDeviceThreadCallbackMode;
using rex::ui::webgpu::WebGPUDeviceThread;
namespace xenos = rex::graphics::xenos;

constexpr uint32_t kShaderStorageMagic = 0x48534558;  // 'XESH'

struct Case {
  const Shader* shader;
  std::string variant;
  WgslShaderTranslator::Modification modification;
  std::string wgsl;
  std::string vertex_wgsl;  // For pixel shaders, the vertex shader to pair with.
  std::string error;        // Translation or validation error; empty if accepted.
  bool translation_failed = false;
};

struct Run {
  std::vector<Case>* cases;
  size_t pending = 0;
  std::promise<void> done;
};

WGPUStringView View(const char* s) {
  return {s, WGPU_STRLEN};
}

std::string FromView(WGPUStringView view) {
  if (!view.data) {
    return std::string();
  }
  return std::string(view.data, view.length == WGPU_STRLEN ? std::strlen(view.data) : view.length);
}

bool ReadShaderStorage(const std::filesystem::path& path,
                       std::vector<std::unique_ptr<WgslShader>>& shaders,
                       std::set<std::pair<uint64_t, uint32_t>>& seen) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    std::printf("can't open %s\n", path.string().c_str());
    return false;
  }
  std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (data.size() < 8) {
    std::printf("%s: too small\n", path.string().c_str());
    return false;
  }
  uint32_t magic;
  std::memcpy(&magic, data.data(), sizeof(magic));
  if (magic != kShaderStorageMagic) {
    std::printf("%s: not a shader storage file\n", path.string().c_str());
    return false;
  }
  size_t offset = 8;
  size_t count = 0;
  while (offset + 12 <= data.size()) {
    uint64_t hash;
    uint32_t count_and_type;
    std::memcpy(&hash, data.data() + offset, sizeof(hash));
    std::memcpy(&count_and_type, data.data() + offset + 8, sizeof(count_and_type));
    offset += 12;
    uint32_t dword_count = count_and_type & 0x7FFFFFFF;
    auto type = xenos::ShaderType(count_and_type >> 31);
    if (offset + size_t(dword_count) * 4 > data.size()) {
      std::printf("%s: truncated record\n", path.string().c_str());
      break;
    }
    std::vector<uint32_t> dwords(dword_count);
    std::memcpy(dwords.data(), data.data() + offset, size_t(dword_count) * 4);
    offset += size_t(dword_count) * 4;
    if (!seen.emplace(hash, uint32_t(type)).second) {
      continue;
    }
    shaders.push_back(std::make_unique<WgslShader>(type, hash, dwords.data(), dwords.size()));
    ++count;
  }
  std::printf("%s: %zu new shaders\n", path.string().c_str(), count);
  return true;
}

// A vertex shader writing what a pixel shader with this modification reads,
// with the same locations and sampling as WgslShaderTranslator's.
std::string PairedVertexShader(const WgslShaderTranslator::Modification& modification) {
  std::string wgsl = "struct O {\n  @builtin(position) p: vec4f,\n";
  uint32_t location = 0;
  for (uint32_t i = 0; i < xenos::kMaxInterpolators; ++i) {
    if (modification.pixel.interpolator_mask & (1u << i)) {
      bool centroid = (modification.pixel.interpolators_centroid >> i) & 1;
      wgsl += fmt::format("  @location({}){} i{}: vec4f,\n", location++,
                          centroid ? " @interpolate(perspective, centroid)" : "", i);
    }
  }
  if (modification.pixel.param_gen_point) {
    wgsl += fmt::format("  @location({}) pc: vec2f,\n", location);
  }
  wgsl += "}\n@vertex fn main() -> O {\n  var o: O;\n  return o;\n}\n";
  return wgsl;
}

WGPUShaderModule CreateModule(WGPUDevice device, const std::string& wgsl) {
  WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
  source.code = {wgsl.data(), wgsl.size()};
  WGPUShaderModuleDescriptor desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
  desc.nextInChain = &source.chain;
  return wgpuDeviceCreateShaderModule(device, &desc);
}

// On the device thread: module and pipeline inside a validation error scope.
void Validate(WGPUDevice device, Run& run, size_t index) {
  Case& c = (*run.cases)[index];
  wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
  WGPUShaderModule module = CreateModule(device, c.wgsl);
  WGPUShaderModule vertex_module = nullptr;

  WGPUDepthStencilState depth_stencil = WGPU_DEPTH_STENCIL_STATE_INIT;
  depth_stencil.format = WGPUTextureFormat_Depth32Float;
  depth_stencil.depthWriteEnabled = WGPUOptionalBool_True;
  depth_stencil.depthCompare = WGPUCompareFunction_Always;
  WGPURenderPipelineDescriptor pipeline_desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
  pipeline_desc.depthStencil = &depth_stencil;
  pipeline_desc.vertex.entryPoint = View("main");
  WGPUColorTargetState targets[xenos::kMaxColorRenderTargets];
  WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
  if (c.shader->type() == xenos::ShaderType::kVertex) {
    pipeline_desc.vertex.module = module;
  } else {
    vertex_module = CreateModule(device, c.vertex_wgsl);
    pipeline_desc.vertex.module = vertex_module;
    uint32_t target_count = 0;
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      targets[i] = WGPU_COLOR_TARGET_STATE_INIT;
      if (c.shader->writes_color_target(i)) {
        targets[i].format = WGPUTextureFormat_RGBA16Float;
        target_count = i + 1;
      } else {
        targets[i].format = WGPUTextureFormat_Undefined;
      }
    }
    fragment.module = module;
    fragment.entryPoint = View("main");
    fragment.targetCount = target_count;
    fragment.targets = targets;
    pipeline_desc.fragment = &fragment;
  }
  WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(device, &pipeline_desc);

  WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
  callback.mode = kDeviceThreadCallbackMode;
  callback.userdata1 = &run;
  callback.userdata2 = reinterpret_cast<void*>(index);
  callback.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView message,
                         void* userdata1, void* userdata2) {
    auto& run = *static_cast<Run*>(userdata1);
    Case& c = (*run.cases)[reinterpret_cast<size_t>(userdata2)];
    if (status != WGPUPopErrorScopeStatus_Success) {
      c.error = "error scope failed";
    } else if (type != WGPUErrorType_NoError) {
      c.error = FromView(message);
      if (c.error.empty()) {
        c.error = "error";
      }
    }
    if (--run.pending == 0) {
      run.done.set_value();
    }
  };
  wgpuDevicePopErrorScope(device, callback);
  if (pipeline) {
    wgpuRenderPipelineRelease(pipeline);
  }
  if (vertex_module) {
    wgpuShaderModuleRelease(vertex_module);
  }
  wgpuShaderModuleRelease(module);
}

// The body of `fn name(...)` from translated WGSL, renamed, or empty.
std::string ExtractFunction(const std::vector<Case>& cases, const char* variant, const char* name,
                            const char* new_name) {
  std::string signature = fmt::format("fn {}(", name);
  for (const Case& c : cases) {
    if (c.translation_failed || (variant && c.variant != variant)) {
      continue;
    }
    size_t start = c.wgsl.find(signature);
    if (start == std::string::npos) {
      continue;
    }
    size_t end = c.wgsl.find("\n}\n", start);
    std::string function = c.wgsl.substr(start, end + 3 - start);
    function.replace(3, std::strlen(name), new_name);
    return function;
  }
  return std::string();
}

struct HelperResults {
  std::vector<float> outputs;  // 4 per input
  bool ok = false;
};

// On the device thread: runs the helpers over the inputs, reading back the
// results (set from the map callback, also on the device thread).
void RunHelpers(WGPUDevice device, WGPUQueue queue, const std::string& wgsl,
                const std::vector<float>& inputs, HelperResults& results,
                std::promise<void>& done) {
  uint64_t input_size = inputs.size() * sizeof(float);
  uint64_t output_size = input_size * 4;
  WGPUBufferDescriptor buffer_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
  buffer_desc.size = input_size;
  buffer_desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
  WGPUBuffer input = wgpuDeviceCreateBuffer(device, &buffer_desc);
  wgpuQueueWriteBuffer(queue, input, 0, inputs.data(), input_size);
  buffer_desc.size = output_size;
  buffer_desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc;
  WGPUBuffer output = wgpuDeviceCreateBuffer(device, &buffer_desc);
  buffer_desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
  WGPUBuffer readback = wgpuDeviceCreateBuffer(device, &buffer_desc);

  WGPUShaderModule module = CreateModule(device, wgsl);
  WGPUComputePipelineDescriptor pipeline_desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
  pipeline_desc.compute.module = module;
  pipeline_desc.compute.entryPoint = View("main");
  WGPUComputePipeline pipeline = wgpuDeviceCreateComputePipeline(device, &pipeline_desc);
  WGPUBindGroupEntry entries[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
  entries[0].binding = 0;
  entries[0].buffer = input;
  entries[0].size = input_size;
  entries[1].binding = 1;
  entries[1].buffer = output;
  entries[1].size = output_size;
  WGPUBindGroupLayout layout = wgpuComputePipelineGetBindGroupLayout(pipeline, 0);
  WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
  group_desc.layout = layout;
  group_desc.entryCount = 2;
  group_desc.entries = entries;
  WGPUBindGroup group = wgpuDeviceCreateBindGroup(device, &group_desc);

  WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
  WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(encoder, nullptr);
  wgpuComputePassEncoderSetPipeline(pass, pipeline);
  wgpuComputePassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
  // In two dimensions: one is limited to 65535 workgroups.
  uint32_t group_count = uint32_t((inputs.size() + 255) / 256);
  uint32_t group_count_x = std::min(group_count, UINT32_C(32768));
  wgpuComputePassEncoderDispatchWorkgroups(pass, group_count_x,
                                           (group_count + group_count_x - 1) / group_count_x, 1);
  wgpuComputePassEncoderEnd(pass);
  wgpuCommandEncoderCopyBufferToBuffer(encoder, output, 0, readback, 0, output_size);
  WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
  wgpuQueueSubmit(queue, 1, &commands);

  struct MapContext {
    WGPUBuffer readback;
    uint64_t size;
    HelperResults* results;
    std::promise<void>* done;
  };
  auto* context = new MapContext{readback, output_size, &results, &done};
  WGPUBufferMapCallbackInfo map_callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
  map_callback.mode = kDeviceThreadCallbackMode;
  map_callback.userdata1 = context;
  map_callback.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* userdata1, void*) {
    auto* context = static_cast<MapContext*>(userdata1);
    if (status == WGPUMapAsyncStatus_Success) {
      auto* data = static_cast<const float*>(
          wgpuBufferGetConstMappedRange(context->readback, 0, context->size));
      context->results->outputs.assign(data, data + context->size / sizeof(float));
      context->results->ok = true;
      wgpuBufferUnmap(context->readback);
    }
    wgpuBufferRelease(context->readback);
    context->done->set_value();
    delete context;
  };
  wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0, output_size, map_callback);

  wgpuCommandBufferRelease(commands);
  wgpuComputePassEncoderRelease(pass);
  wgpuCommandEncoderRelease(encoder);
  wgpuBindGroupRelease(group);
  wgpuBindGroupLayoutRelease(layout);
  wgpuComputePipelineRelease(pipeline);
  wgpuShaderModuleRelease(module);
  wgpuBufferRelease(output);
  wgpuBufferRelease(input);
}

uint32_t Bits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

float FromBits(uint32_t bits) {
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// Returns false on a mismatch or if the helpers couldn't be run.
bool CheckHelpers(WebGPUDeviceThread& thread, const std::vector<Case>& cases) {
  std::string depth_truncating =
      ExtractFunction(cases, "param_gen_float24", "xe_depth_to_float24_host", "depth_truncating");
  std::string depth_rounding = ExtractFunction(cases, "param_gen_point_float24",
                                               "xe_depth_to_float24_host", "depth_rounding");
  std::string gamma_to_linear =
      ExtractFunction(cases, nullptr, "xe_pwl_gamma_to_linear", "gamma_to_linear");
  std::string linear_to_gamma =
      ExtractFunction(cases, nullptr, "xe_linear_to_pwl_gamma", "linear_to_gamma");
  if (depth_truncating.empty() || depth_rounding.empty() || gamma_to_linear.empty() ||
      linear_to_gamma.empty()) {
    std::printf("helpers: not all found in the translated shaders, not checked\n");
    return true;
  }
  std::string wgsl = depth_truncating + depth_rounding + gamma_to_linear + linear_to_gamma +
                     R"(
@group(0) @binding(0) var<storage, read> inputs: array<f32>;
@group(0) @binding(1) var<storage, read_write> outputs: array<f32>;
@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) id: vec3u,
        @builtin(num_workgroups) groups: vec3u) {
  let i = id.y * groups.x * 256u + id.x;
  if (i >= arrayLength(&inputs)) {
    return;
  }
  let x = inputs[i];
  // The depth helpers take pre-clamped depth.
  let depth = select(0.0f, min(x, 1.0f), x >= 0.0f);
  outputs[i * 4u] = depth_truncating(depth);
  outputs[i * 4u + 1u] = depth_rounding(depth);
  outputs[i * 4u + 2u] = gamma_to_linear(x);
  outputs[i * 4u + 3u] = linear_to_gamma(vec3f(x)).x;
}
)";
  // Every 61st bit pattern in [0, 1], the piece boundaries and their
  // neighbors, and out-of-range values.
  std::vector<float> inputs;
  for (uint32_t bits = 0; bits <= 0x3F800000u; bits += 61) {
    inputs.push_back(FromBits(bits));
  }
  for (float boundary : {64.0f / 255.0f, 96.0f / 255.0f, 192.0f / 255.0f, 64.0f / 1023.0f,
                         128.0f / 1023.0f, 512.0f / 1023.0f, 1.0f, 0.5f, 0.0f}) {
    inputs.push_back(boundary);
    inputs.push_back(std::nextafter(boundary, 0.0f));
    inputs.push_back(std::nextafter(boundary, 2.0f));
  }
  for (float outside : {-1.0f, -0.0f, 2.0f, 1.0e30f, std::nanf("")}) {
    inputs.push_back(outside);
  }

  HelperResults results;
  std::promise<void> done;
  std::future<void> done_future = done.get_future();
  thread.Call([&] {
    RunHelpers(thread.device().device(), thread.device().queue(), wgsl, inputs, results, done);
  });
  done_future.wait();
  if (!results.ok) {
    std::printf("FAIL: helpers: couldn't read back the results\n");
    return false;
  }

  size_t depth_mismatches = 0, gamma_mismatches = 0;
  double gamma_max_error = 0.0;
  for (size_t i = 0; i < inputs.size(); ++i) {
    float x = inputs[i];
    float depth = x >= 0.0f ? std::min(x, 1.0f) : 0.0f;
    const float* gpu = &results.outputs[i * 4];
    for (uint32_t rounding = 0; rounding < 2; ++rounding) {
      float expected = rex::graphics::xenos::Float20e4To32(
                           rex::graphics::xenos::Float32To20e4(depth, rounding != 0)) *
                       0.5f;
      if (Bits(gpu[rounding]) != Bits(expected)) {
        if (++depth_mismatches <= 5) {
          std::printf("depth %s mismatch: %.9g (0x%08X): GPU %.9g, CPU %.9g\n",
                      rounding ? "rounding" : "truncating", depth, Bits(depth), gpu[rounding],
                      expected);
        }
      }
    }
    float expected_gamma[2] = {rex::graphics::xenos::PWLGammaToLinear(x),
                               rex::graphics::xenos::LinearToPWLGamma(x)};
    for (uint32_t j = 0; j < 2; ++j) {
      double error = std::abs(double(gpu[2 + j]) - double(expected_gamma[j]));
      if (!(error <= 1.0e-6)) {
        if (++gamma_mismatches <= 5) {
          std::printf("%s mismatch: %.9g: GPU %.9g, CPU %.9g\n",
                      j ? "linear to gamma" : "gamma to linear", x, gpu[2 + j], expected_gamma[j]);
        }
      } else {
        gamma_max_error = std::max(gamma_max_error, error);
      }
    }
  }
  std::printf(
      "helpers: %zu inputs; float24 depth: %zu mismatches; PWL gamma: %zu mismatches, max "
      "error %.3g\n",
      inputs.size(), depth_mismatches, gamma_mismatches, gamma_max_error);
  return !depth_mismatches && !gamma_mismatches;
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path dump_dir;
  bool dump_all = false;
  bool verbose = false;
  uint32_t resolution_scale = 1;
  std::vector<std::filesystem::path> inputs;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--dump") && i + 1 < argc) {
      dump_dir = argv[++i];
    } else if (!std::strcmp(argv[i], "--dump-all")) {
      dump_all = true;
    } else if (!std::strcmp(argv[i], "--resolution-scale") && i + 1 < argc) {
      resolution_scale = uint32_t(std::strtoul(argv[++i], nullptr, 10));
    } else if (!std::strcmp(argv[i], "--verbose")) {
      verbose = true;
    } else {
      inputs.emplace_back(argv[i]);
    }
  }
  if (inputs.empty()) {
    std::printf(
        "usage: %s [--dump DIR] [--dump-all] [--verbose] [--resolution-scale N] FILE.xsh...\n",
        argv[0]);
    return 2;
  }

  std::vector<std::unique_ptr<WgslShader>> shaders;
  std::set<std::pair<uint64_t, uint32_t>> seen;
  for (const auto& input : inputs) {
    ReadShaderStorage(input, shaders, seen);
  }

  auto thread = WebGPUDeviceThread::Start();
  if (!thread) {
    std::printf("FAIL: no WebGPU device\nFAILED\n");
    return 1;
  }
  WgslShaderTranslator::Features features;
  thread->Call([&] {
    features.clip_distances = thread->device().HasFeature(WGPUFeatureName_ClipDistances);
    std::printf("adapter: %s; clip-distances: %s\n", thread->device().adapter_description().c_str(),
                features.clip_distances ? "yes" : "no");
  });

  // Translate.
  WgslShaderTranslator translator(features, resolution_scale, resolution_scale);
  rex::string::StringBuffer ucode_disasm_buffer;
  std::vector<Case> cases;
  for (const auto& shader_ptr : shaders) {
    Shader& shader = *shader_ptr;
    shader.AnalyzeUcode(ucode_disasm_buffer);
    uint32_t register_count = shader.GetDynamicAddressableRegisterCount(63);
    std::vector<std::pair<std::string, WgslShaderTranslator::Modification>> variants;
    if (shader.type() == xenos::ShaderType::kVertex) {
      WgslShaderTranslator::Modification m;
      m.vertex.interpolator_mask = shader.writes_interpolators();
      m.vertex.dynamic_addressable_register_count = register_count;
      m.vertex.host_vertex_shader_type = Shader::HostVertexShaderType::kVertex;
      m.vertex.user_clip_plane_count = features.clip_distances ? 6 : 0;
      variants.emplace_back("vertex", m);
      // Point coordinates take one of the 16 inter-stage variables.
      m.vertex.interpolator_mask &= 0x7FFF;
      m.vertex.interpolators_centroid = 0x00FF;
      m.vertex.user_clip_plane_count = 0;
      m.vertex.host_vertex_shader_type = Shader::HostVertexShaderType::kPointListAsTriangleStrip;
      m.vertex.output_point_parameters = 1;
      variants.emplace_back("point", m);
      m.vertex.output_point_parameters = 0;
      m.vertex.host_vertex_shader_type =
          Shader::HostVertexShaderType::kRectangleListAsTriangleStrip;
      variants.emplace_back("rectangle", m);
    } else {
      WgslShaderTranslator::Modification m;
      m.pixel.interpolator_mask = 0xFFFF;
      m.pixel.dynamic_addressable_register_count = register_count;
      variants.emplace_back("pixel", m);
      m.pixel.depth_stencil_mode = WgslShaderTranslator::Modification::DepthStencilMode::kEarlyHint;
      variants.emplace_back("early_hint", m);
      m.pixel.interpolators_centroid = 0x00FF;
      m.pixel.param_gen_enable = 1;
      m.pixel.param_gen_interpolator = 0;
      m.pixel.depth_stencil_mode =
          WgslShaderTranslator::Modification::DepthStencilMode::kFloat24Truncating;
      variants.emplace_back("param_gen_float24", m);
      m.pixel.interpolator_mask = 0x7FFF;
      m.pixel.param_gen_point = 1;
      m.pixel.depth_stencil_mode =
          WgslShaderTranslator::Modification::DepthStencilMode::kFloat24Rounding;
      variants.emplace_back("param_gen_point_float24", m);
    }
    for (auto& [name, modification] : variants) {
      Case c;
      c.shader = &shader;
      c.variant = name;
      c.modification = modification;
      Shader::Translation* translation = shader.GetOrCreateTranslation(modification.value);
      if (!translator.TranslateAnalyzedShader(*translation)) {
        c.translation_failed = true;
        for (const auto& error : translation->errors()) {
          if (error.is_fatal) {
            c.error += (c.error.empty() ? "" : "; ") + error.message;
          }
        }
      } else {
        c.wgsl = translation->GetTranslatedBinaryString();
        if (shader.type() == xenos::ShaderType::kPixel) {
          c.vertex_wgsl = PairedVertexShader(modification);
        }
      }
      cases.push_back(std::move(c));
    }
  }

  // The canary: must be rejected.
  if (!cases.empty() && !cases.front().translation_failed) {
    Case canary = cases.front();
    canary.variant = "canary";
    canary.wgsl += "\nfn this_is_not_wgsl( {\n";
    cases.push_back(std::move(canary));
  }

  // Validate on the device.
  Run run;
  run.cases = &cases;
  std::future<void> done = run.done.get_future();
  thread->Call([&] {
    for (size_t i = 0; i < cases.size(); ++i) {
      run.pending += !cases[i].translation_failed;
    }
    if (!run.pending) {
      run.done.set_value();
      return;
    }
    for (size_t i = 0; i < cases.size(); ++i) {
      if (!cases[i].translation_failed) {
        Validate(thread->device().device(), run, i);
      }
    }
  });
  done.wait();
  bool helpers_ok = CheckHelpers(*thread, cases);
  thread.reset();

  bool canary_rejected = true;
  if (!cases.empty() && cases.back().variant == "canary") {
    canary_rejected = !cases.back().error.empty();
    cases.pop_back();
    if (!canary_rejected) {
      std::printf("FAIL: an invalid shader was accepted, so nothing was validated\n");
    }
  }

  size_t translation_failures = 0, rejected = 0;
  if (!dump_dir.empty()) {
    std::filesystem::create_directories(dump_dir);
  }
  for (const Case& c : cases) {
    const char* stage = c.shader->type() == xenos::ShaderType::kVertex ? "vs" : "ps";
    std::string name = fmt::format("{:016X}_{}_{}", c.shader->ucode_data_hash(), stage, c.variant);
    if (c.error.empty()) {
      if (dump_all && !dump_dir.empty()) {
        std::ofstream(dump_dir / (name + ".wgsl")) << c.wgsl;
        std::ofstream(dump_dir / (name + ".ucode.txt")) << c.shader->ucode_disassembly();
      }
      continue;
    }
    if (c.translation_failed) {
      ++translation_failures;
      std::printf("NOT TRANSLATED: %s\n  %s\n", name.c_str(), c.error.c_str());
      continue;
    }
    ++rejected;
    if (verbose || rejected <= 20) {
      std::printf("REJECTED: %s\n  %s\n", name.c_str(), c.error.c_str());
    }
    if (!dump_dir.empty()) {
      std::ofstream(dump_dir / (name + ".wgsl")) << c.wgsl;
      std::ofstream(dump_dir / (name + ".ucode.txt")) << c.shader->ucode_disassembly();
    }
  }
  size_t vertex = 0, over_texture_limit = 0, over_sampler_limit = 0, max_textures = 0,
         max_samplers = 0;
  for (const auto& shader : shaders) {
    vertex += shader->type() == xenos::ShaderType::kVertex;
    size_t textures = shader->GetTextureBindingsAfterTranslation().size();
    size_t samplers = shader->GetSamplerBindingsAfterTranslation().size();
    over_texture_limit += textures > 16;
    over_sampler_limit += samplers > 16;
    max_textures = std::max(max_textures, textures);
    max_samplers = std::max(max_samplers, samplers);
  }
  std::printf(
      "bindings per shader: up to %zu textures (%zu shaders over 16), up to %zu samplers (%zu "
      "over 16)\n",
      max_textures, over_texture_limit, max_samplers, over_sampler_limit);
  std::printf(
      "%zu shaders (%zu vertex, %zu pixel), %zu translations: %zu accepted, %zu rejected, "
      "%zu not translated\n",
      shaders.size(), vertex, shaders.size() - vertex, cases.size(),
      cases.size() - rejected - translation_failures, rejected, translation_failures);
  bool failed = rejected || translation_failures || !canary_rejected || !helpers_ok;
  std::printf("%s\n", failed ? "FAILED" : "PASSED");
  std::fflush(stdout);
  return failed ? 1 : 0;
}
