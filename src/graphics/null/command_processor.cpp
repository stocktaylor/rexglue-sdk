/**
 * @file        graphics/null/command_processor.cpp
 * @brief       Command processor for the null GPU backend
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/null/command_processor.h>

#include <algorithm>

#include <xxhash.h>

#include <rex/graphics/graphics_system.h>
#include <rex/graphics/null/graphics_system.h>

#include "wireframe_sampler.h"

namespace rex::graphics::null {

NullCommandProcessor::NullCommandProcessor(NullGraphicsSystem* graphics_system,
                                           system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {}

NullCommandProcessor::~NullCommandProcessor() = default;

bool NullCommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) {
    return false;
  }
  sampler_ = std::make_unique<WireframeSampler>(*register_file_, *memory_);
  return true;
}

void NullCommandProcessor::ShutdownContext() {
  sampler_.reset();
  shaders_.clear();
  CommandProcessor::ShutdownContext();
}

Shader* NullCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                         const uint32_t* host_address, uint32_t dword_count) {
  (void)guest_address;
  uint64_t data_hash = XXH3_64bits(host_address, dword_count * sizeof(uint32_t));
  auto it = shaders_.find(data_hash);
  if (it != shaders_.end()) {
    return it->second.get();
  }
  auto shader = std::make_unique<Shader>(shader_type, data_hash, host_address, dword_count);
  // Analysis is all the sampler needs (to interpret vertex shaders); nothing is
  // translated.
  shader->AnalyzeUcode(ucode_disasm_buffer_);
  Shader* result = shader.get();
  shaders_.emplace(data_hash, std::move(shader));
  return result;
}

bool NullCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                     IndexBufferInfo* index_buffer_info,
                                     bool major_mode_explicit) {
  (void)major_mode_explicit;
  if (!sampler_ || !sampler_->capturing()) {
    return true;
  }
  Shader* vertex_shader = active_vertex_shader();
  if (!vertex_shader) {
    return true;
  }
  WireframeSampler::Indices indices;
  indices.count = index_count;
  if (index_buffer_info) {
    indices.indexed = true;
    indices.guest_base = index_buffer_info->guest_base;
    indices.format = index_buffer_info->format;
    indices.endianness = index_buffer_info->endianness;
    // Indices past the buffer read as vertex 0 on the GPU; stay inside it.
    uint32_t index_size =
        indices.format == xenos::IndexFormat::kInt16 ? sizeof(uint16_t) : sizeof(uint32_t);
    indices.count = std::min(index_count, uint32_t(index_buffer_info->length / index_size));
  }
  sampler_->OnDraw(*vertex_shader, prim_type, indices);
  return true;
}

bool NullCommandProcessor::IssueCopy() {
  // Resolves write nothing: render-to-texture results never reach memory.
  return true;
}

void NullCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                     uint32_t frontbuffer_height) {
  (void)frontbuffer_ptr;
  if (sampler_) {
    sampler_->OnSwap(frontbuffer_width, frontbuffer_height,
                     graphics_system_ ? graphics_system_->presenter() : nullptr);
  }
}

}  // namespace rex::graphics::null
