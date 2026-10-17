#pragma once
/**
 * @file        graphics/null/command_processor.h
 * @brief       Command processor for the null GPU backend
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <memory>
#include <unordered_map>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/string/buffer.h>

namespace rex::graphics::null {

class NullGraphicsSystem;
class WireframeSampler;

class NullCommandProcessor : public CommandProcessor {
 public:
  NullCommandProcessor(NullGraphicsSystem* graphics_system, system::KernelState* kernel_state);
  ~NullCommandProcessor() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;

  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;

 private:
  std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders_;
  string::StringBuffer ucode_disasm_buffer_;
  std::unique_ptr<WireframeSampler> sampler_;
};

}  // namespace rex::graphics::null
