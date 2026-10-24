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
// paths (host vertex shader types, PsParamGen, centroid interpolation, clip
// distances), and has the device validate the WGSL: a shader module, then a
// render pipeline with an automatic layout (vertex shaders alone, pixel
// shaders behind a vertex shader with matching outputs). With --dump, each
// rejected shader's WGSL and ucode disassembly go to DIR (every shader's with
// --dump-all). An invalid shader is validated too, and must be rejected, so a
// pass means the validation ran. --resolution-scale translates for a draw
// resolution scale (the resolve-scaled texture and PsParamGen paths). Shaders
// using more textures or samplers than WebGPU's default per-stage limit (16)
// are counted, as they need devices with higher limits.

#include <algorithm>
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
    } else {
      WgslShaderTranslator::Modification m;
      m.pixel.interpolator_mask = 0xFFFF;
      m.pixel.dynamic_addressable_register_count = register_count;
      variants.emplace_back("pixel", m);
      m.pixel.interpolators_centroid = 0x00FF;
      m.pixel.param_gen_enable = 1;
      m.pixel.param_gen_interpolator = 0;
      variants.emplace_back("param_gen", m);
      m.pixel.interpolator_mask = 0x7FFF;
      m.pixel.param_gen_point = 1;
      variants.emplace_back("param_gen_point", m);
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
  bool failed = rejected || translation_failures || !canary_rejected;
  std::printf("%s\n", failed ? "FAILED" : "PASSED");
  std::fflush(stdout);
  return failed ? 1 : 0;
}
