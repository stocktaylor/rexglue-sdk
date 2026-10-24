/**
 * @file        graphics/pipeline/shader/wgsl.h
 * @brief       Shader translated to WGSL, with its texture and sampler bindings
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <vector>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/shader/wgsl_translator.h>

namespace rex::graphics {

// Like SpirvShader: the bindings come from the first successful translation of
// any modification (they don't depend on the modification bits).
class WgslShader : public Shader {
 public:
  class WgslTranslation : public Translation {
   public:
    explicit WgslTranslation(WgslShader& shader, uint64_t modification)
        : Translation(shader, modification) {}
  };

  explicit WgslShader(xenos::ShaderType shader_type, uint64_t ucode_data_hash,
                      const uint32_t* ucode_dwords, size_t ucode_dword_count,
                      std::endian ucode_source_endian = std::endian::big)
      : Shader(shader_type, ucode_data_hash, ucode_dwords, ucode_dword_count, ucode_source_endian) {
  }

  using TextureBinding = WgslShaderTranslator::TextureBinding;
  using SamplerBinding = WgslShaderTranslator::SamplerBinding;
  // Textures at bindings 0 and up of the stage's bind group, then samplers.
  const std::vector<TextureBinding>& GetTextureBindingsAfterTranslation() const {
    return texture_bindings_;
  }
  uint32_t GetUsedTextureMaskAfterTranslation() const { return used_texture_mask_; }
  const std::vector<SamplerBinding>& GetSamplerBindingsAfterTranslation() const {
    return sampler_bindings_;
  }

 protected:
  Translation* CreateTranslationInstance(uint64_t modification) override {
    return new WgslTranslation(*this, modification);
  }

 private:
  friend class WgslShaderTranslator;

  std::atomic_flag bindings_setup_entered_ = ATOMIC_FLAG_INIT;
  std::vector<TextureBinding> texture_bindings_;
  std::vector<SamplerBinding> sampler_bindings_;
  uint32_t used_texture_mask_ = 0;
};

}  // namespace rex::graphics
