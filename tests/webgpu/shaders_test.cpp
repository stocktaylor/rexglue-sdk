/**
 * @file        tests/webgpu/shaders_test.cpp
 * @brief       Compiles every converted internal WGSL shader on a real device
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

// The internal shaders converted by scripts/convert_shaders_wgsl.py pass Tint's
// own validation; this checks that a WebGPU implementation accepts them too:
// Dawn natively, Firefox's wgpu/naga in the browser (a separate compiler). Each
// becomes a shader module, and each compute shader a pipeline with an
// automatic layout, which also checks bindings and limits. Errors are caught
// with error scopes on the device thread.

#include <cstdio>
#include <cstring>
#include <future>
#include <string>
#include <vector>

#include <rex/ui/webgpu/device_thread.h>

#include "../../src/graphics/shaders/webgpu_wgsl/all_shaders.h"

namespace {

using rex::ui::webgpu::kDeviceThreadCallbackMode;
using rex::ui::webgpu::WebGPUDeviceThread;

struct Result {
  std::string name;
  std::string error;  // empty if accepted
  bool skipped = false;
};

struct Run {
  std::vector<Result> results;
  size_t pending = 0;
  std::promise<void> done;
};

WGPUStringView View(const char* s) {
  return {s, WGPU_STRLEN};
}

// On the device thread: creates the module (and pipeline) inside a validation
// error scope, whose result arrives later on the same thread.
void Compile(WGPUDevice device, const WgslShaderEntry& shader, Run& run, size_t index) {
  wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
  WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
  wgsl.code = View(shader.wgsl);
  WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
  module_desc.nextInChain = &wgsl.chain;
  module_desc.label = View(shader.name);
  WGPUShaderModule module = wgpuDeviceCreateShaderModule(device, &module_desc);
  WGPUComputePipeline pipeline = nullptr;
  if (shader.compute) {
    WGPUComputePipelineDescriptor pipeline_desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    pipeline_desc.label = View(shader.name);
    pipeline_desc.compute.module = module;
    pipeline_desc.compute.entryPoint = View("main");
    pipeline = wgpuDeviceCreateComputePipeline(device, &pipeline_desc);
  }
  WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
  callback.mode = kDeviceThreadCallbackMode;
  callback.userdata1 = &run;
  callback.userdata2 = reinterpret_cast<void*>(index);
  callback.callback = [](WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView message,
                         void* userdata1, void* userdata2) {
    auto& run = *static_cast<Run*>(userdata1);
    Result& result = run.results[reinterpret_cast<size_t>(userdata2)];
    if (status != WGPUPopErrorScopeStatus_Success) {
      result.error = "error scope failed";
    } else if (type != WGPUErrorType_NoError) {
      result.error = message.data ? std::string(message.data, message.length == WGPU_STRLEN
                                                                  ? std::strlen(message.data)
                                                                  : message.length)
                                  : "error";
    }
    if (--run.pending == 0) {
      run.done.set_value();
    }
  };
  wgpuDevicePopErrorScope(device, callback);
  if (pipeline) {
    wgpuComputePipelineRelease(pipeline);
  }
  wgpuShaderModuleRelease(module);
}

}  // namespace

int main() {
  auto thread = WebGPUDeviceThread::Start();
  if (!thread) {
    std::printf("FAIL: no WebGPU device\nFAILED\n");
    return 1;
  }
  Run run;
  std::future<void> done = run.done.get_future();
  thread->Call([&] {
    bool tier1 = thread->device().HasFeature(WGPUFeatureName_TextureFormatsTier1);
    std::printf("adapter: %s; texture-formats-tier1: %s\n",
                thread->device().adapter_description().c_str(), tier1 ? "yes" : "no");
    run.results.resize(std::size(kAllWgslShaders));
    run.pending = run.results.size();
    for (size_t i = 0; i < run.results.size(); ++i) {
      run.results[i].name = kAllWgslShaders[i].name;
      // The backend uses these only with the feature (else their fragment
      // shader twins, which write the format as a render target).
      if (kAllWgslShaders[i].needs_texture_formats_tier1 && !tier1) {
        run.results[i].skipped = true;
        --run.pending;
        continue;
      }
      Compile(thread->device().device(), kAllWgslShaders[i], run, i);
    }
    if (!run.pending) {
      run.done.set_value();
    }
  });
  done.wait();

  size_t compute = 0, failed = 0, skipped = 0;
  for (size_t i = 0; i < run.results.size(); ++i) {
    compute += kAllWgslShaders[i].compute;
    if (run.results[i].skipped) {
      ++skipped;
      std::printf("skipped (needs texture-formats-tier1): %s\n", run.results[i].name.c_str());
      continue;
    }
    if (!run.results[i].error.empty()) {
      ++failed;
      std::printf("FAIL: %s\n  %s\n", run.results[i].name.c_str(), run.results[i].error.c_str());
    }
  }
  std::printf("%zu shaders (%zu compute): %zu accepted, %zu rejected, %zu skipped\n",
              run.results.size(), compute, run.results.size() - failed - skipped, failed, skipped);
  thread.reset();
  std::printf("%s\n", failed ? "FAILED" : "PASSED");
  std::fflush(stdout);
  return failed ? 1 : 0;
}
