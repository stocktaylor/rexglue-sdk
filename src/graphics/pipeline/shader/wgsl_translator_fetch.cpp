/**
 * @file        graphics/pipeline/shader/wgsl_translator_fetch.cpp
 * @brief       Xenos microcode to WGSL: vertex and texture fetches
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/pipeline/shader/wgsl_translator.h>

#include <algorithm>
#include <cmath>

#include <fmt/format.h>

#include <rex/assert.h>
#include <rex/math.h>

namespace rex::graphics {

// Semantics follow SpirvShaderTranslator: vertex data comes from shared
// memory, textures are sampled with an explicit LOD or explicit gradients
// (never implicitly), and the guest swizzle and signedness are applied in the
// shader. Where WebGPU differs, see the comments.

std::string WgslShaderTranslator::LoadUint32FromSharedMemory(const std::string& address_dwords) {
  uses_shared_memory_ = true;
  return fmt::format("xe_load_shared({})", address_dwords);
}

std::string WgslShaderTranslator::FetchConstantWord(uint32_t dword_index) {
  return fmt::format("xe_fetch.c[{}][{}]", dword_index >> 2, dword_index & 3);
}

void WgslShaderTranslator::ProcessVertexFetchInstruction(
    const ParsedVertexFetchInstruction& instr) {
  UpdateInstructionPredication(instr.is_predicated, instr.predicate_condition);

  uint32_t used_result_components = instr.result.GetUsedResultComponents();
  uint32_t needed_words =
      xenos::GetVertexFormatNeededWords(instr.attributes.data_format, used_result_components);
  // vfetch_full computes the address for the vfetch_mini instructions after it
  // even if it loads nothing itself.
  if (!needed_words && instr.is_mini_fetch) {
    StoreResult(instr.result, Value());
    return;
  }

  uses_fetch_constants_ = true;
  uint32_t fetch_constant_word_0_index = instr.operands[1].storage_index << 1;
  std::string address;
  if (instr.is_mini_fetch) {
    address = Let("xe_vfetch_address");
  } else {
    // The base address in dwords from bits 2:31 of the first word, plus the
    // index (floored or rounded) times the stride.
    std::string address_expr =
        fmt::format("bitcast<i32>({} >> 2u)", FetchConstantWord(fetch_constant_word_0_index));
    if (instr.attributes.stride) {
      Value index =
          GetOperandComponents(LoadOperandStorage(instr.operands[0]), instr.operands[0], 0b0001);
      std::string index_int = fmt::format("i32(floor({}{}))", index.expr,
                                          instr.attributes.is_index_rounded ? " + 0.5f" : "");
      if (instr.attributes.stride > 1) {
        index_int = fmt::format("{} * {}i", index_int, instr.attributes.stride);
      }
      address_expr = fmt::format("{} + {}", address_expr, index_int);
    }
    address = Let(address_expr);
    Line(fmt::format("xe_vfetch_address = {};", address));
  }
  if (!needed_words) {
    StoreResult(instr.result, Value());
    return;
  }

  // Load and endian-swap the needed words (no bounds checking, as in SPIR-V).
  std::string endian =
      Let(fmt::format("{} & 3u", FetchConstantWord(fetch_constant_word_0_index + 1)));
  std::string words[4];
  uint32_t words_remaining = needed_words;
  uint32_t word_index;
  while (rex::bit_scan_forward(words_remaining, &word_index)) {
    words_remaining &= ~(UINT32_C(1) << word_index);
    int32_t word_offset = instr.attributes.offset + int32_t(word_index);
    std::string word_address =
        word_offset ? fmt::format("{} + {}i", address, word_offset) : address;
    words[word_index] = Let(
        fmt::format("xe_endian_swap32({}, {})", LoadUint32FromSharedMemory(word_address), endian));
  }

  uint32_t used_format_components =
      used_result_components &
      ((UINT32_C(1) << xenos::GetVertexFormatComponentCount(instr.attributes.data_format)) - 1);
  assert_not_zero(used_format_components);
  bool is_signed = instr.attributes.is_signed;
  bool is_normalized = !instr.attributes.is_integer;
  bool no_zero = instr.attributes.signed_rf_mode == xenos::SignedRepeatingFractionMode::kNoZero;

  // Per format component: packed bit fields, halves, or whole words.
  int packed_widths[4] = {}, packed_offsets[4] = {};
  uint32_t packed_words[4] = {};
  bool format_is_packed = false;
  switch (instr.attributes.data_format) {
    case xenos::VertexFormat::k_8_8_8_8:
      format_is_packed = true;
      packed_widths[0] = packed_widths[1] = packed_widths[2] = packed_widths[3] = 8;
      packed_offsets[1] = 8;
      packed_offsets[2] = 16;
      packed_offsets[3] = 24;
      break;
    case xenos::VertexFormat::k_2_10_10_10:
      format_is_packed = true;
      packed_widths[0] = packed_widths[1] = packed_widths[2] = 10;
      packed_widths[3] = 2;
      packed_offsets[1] = 10;
      packed_offsets[2] = 20;
      packed_offsets[3] = 30;
      break;
    case xenos::VertexFormat::k_10_11_11:
      format_is_packed = true;
      packed_widths[0] = packed_widths[1] = 11;
      packed_widths[2] = 10;
      packed_offsets[1] = 11;
      packed_offsets[2] = 22;
      break;
    case xenos::VertexFormat::k_11_11_10:
      format_is_packed = true;
      packed_widths[0] = 10;
      packed_widths[1] = packed_widths[2] = 11;
      packed_offsets[1] = 10;
      packed_offsets[2] = 21;
      break;
    case xenos::VertexFormat::k_16_16:
      format_is_packed = true;
      packed_widths[0] = packed_widths[1] = 16;
      packed_offsets[1] = 16;
      break;
    case xenos::VertexFormat::k_16_16_16_16:
      format_is_packed = true;
      packed_widths[0] = packed_widths[1] = packed_widths[2] = packed_widths[3] = 16;
      packed_offsets[1] = packed_offsets[3] = 16;
      packed_words[2] = packed_words[3] = 1;
      break;
    default:
      break;
  }

  std::string components[4];
  switch (instr.attributes.data_format) {
    case xenos::VertexFormat::k_16_16_FLOAT:
    case xenos::VertexFormat::k_16_16_16_16_FLOAT: {
      // Host float16 (with NaNs) rather than the Xbox 360 one with extended
      // range, as in the SPIR-V translator.
      std::string halves[2];
      for (uint32_t i = 0; i < 4; ++i) {
        if (!(used_format_components & (UINT32_C(1) << i))) {
          continue;
        }
        std::string& word_halves = halves[i >> 1];
        if (word_halves.empty()) {
          word_halves = Let(fmt::format("unpack2x16float({})", words[i >> 1]));
        }
        components[i] = fmt::format("{}.{}", word_halves, (i & 1) ? 'y' : 'x');
      }
    } break;
    case xenos::VertexFormat::k_32:
    case xenos::VertexFormat::k_32_32:
    case xenos::VertexFormat::k_32_32_32_32:
      for (uint32_t i = 0; i < 4; ++i) {
        if (!(used_format_components & (UINT32_C(1) << i))) {
          continue;
        }
        std::string value = is_signed ? fmt::format("f32(bitcast<i32>({}))", words[i])
                                      : fmt::format("f32({})", words[i]);
        if (is_normalized) {
          if (is_signed) {
            if (no_zero) {
              value = fmt::format("{} * {} + {}", value, FloatConst(1.0f / 2147483647.5f, 1),
                                  FloatConst(0.5f / 2147483647.5f, 1));
            } else {
              // 1/(2^31-1) rounds to 1/2^31, so no clamping to -1 is needed.
              value = fmt::format("{} * {}", value, FloatConst(1.0f / 2147483647.0f, 1));
            }
          } else {
            value = fmt::format("{} * {}", value, FloatConst(1.0f / 4294967295.0f, 1));
          }
        }
        components[i] = value;
      }
      break;
    case xenos::VertexFormat::k_32_FLOAT:
    case xenos::VertexFormat::k_32_32_FLOAT:
    case xenos::VertexFormat::k_32_32_32_32_FLOAT:
    case xenos::VertexFormat::k_32_32_32_FLOAT:
      for (uint32_t i = 0; i < 4; ++i) {
        if (used_format_components & (UINT32_C(1) << i)) {
          components[i] = fmt::format("bitcast<f32>({})", words[i]);
        }
      }
      break;
    default:
      if (!format_is_packed) {
        assert_unhandled_case(instr.attributes.data_format);
        EmitTranslationError("Unsupported vertex format");
        return;
      }
      for (uint32_t i = 0; i < 4; ++i) {
        if (!(used_format_components & (UINT32_C(1) << i))) {
          continue;
        }
        int width = packed_widths[i];
        assert_true(width >= 2);
        const std::string& word = words[packed_words[i]];
        std::string value = is_signed ? fmt::format("f32(extractBits(bitcast<i32>({}), {}u, {}u))",
                                                    word, packed_offsets[i], width)
                                      : fmt::format("f32(extractBits({}, {}u, {}u))", word,
                                                    packed_offsets[i], width);
        if (is_normalized) {
          float scale_inv;
          if (is_signed) {
            scale_inv = float((UINT32_C(1) << (width - 1)) - 1);
            if (no_zero) {
              scale_inv += 0.5f;
            }
          } else {
            scale_inv = float((UINT32_C(1) << width) - 1);
          }
          float scale = 1.0f / scale_inv;
          value = fmt::format("{} * {}", value, FloatConst(scale, 1));
          if (is_signed) {
            // Both -(2^(n-1)) and -(2^(n-1)-1) are -1, or there's no zero.
            value = no_zero ? fmt::format("{} + {}", value, FloatConst(0.5f * scale, 1))
                            : fmt::format("max({}, -1.0f)", value);
          }
        }
        components[i] = value;
      }
      break;
  }

  if (instr.attributes.exp_adjust) {
    std::string exp_scale = FloatConst(std::ldexp(1.0f, instr.attributes.exp_adjust), 1);
    for (uint32_t i = 0; i < 4; ++i) {
      if (!components[i].empty()) {
        components[i] = fmt::format("({}) * {}", components[i], exp_scale);
      }
    }
  }

  // Components missing from the format are 0.
  uint32_t used_result_component_count = rex::bit_count(used_result_components);
  std::string result =
      used_result_component_count > 1 ? FloatType(used_result_component_count) + "(" : "";
  bool first = true;
  for (uint32_t i = 0; i < 4; ++i) {
    if (used_result_components & (UINT32_C(1) << i)) {
      result += first ? "" : ", ";
      result += components[i].empty() ? std::string("0.0f") : components[i];
      first = false;
    }
  }
  if (used_result_component_count > 1) {
    result += ")";
  }
  StoreResult(instr.result, Value{Let(result), used_result_component_count});
}

size_t WgslShaderTranslator::FindOrAddTextureBinding(uint32_t fetch_constant,
                                                     xenos::FetchOpDimension dimension,
                                                     bool is_signed) {
  // 1D textures are 2D arrays too.
  if (dimension == xenos::FetchOpDimension::k1D) {
    dimension = xenos::FetchOpDimension::k2D;
  }
  for (size_t i = 0; i < texture_bindings_.size(); ++i) {
    const TextureBinding& binding = texture_bindings_[i];
    if (binding.fetch_constant == fetch_constant && binding.dimension == dimension &&
        binding.is_signed == is_signed) {
      return i;
    }
  }
  texture_bindings_.push_back({fetch_constant, dimension, is_signed});
  return texture_bindings_.size() - 1;
}

size_t WgslShaderTranslator::FindOrAddSamplerBinding(uint32_t fetch_constant,
                                                     xenos::TextureFilter mag_filter,
                                                     xenos::TextureFilter min_filter,
                                                     xenos::TextureFilter mip_filter,
                                                     xenos::AnisoFilter aniso_filter) {
  if (aniso_filter != xenos::AnisoFilter::kUseFetchConst) {
    // WebGPU's maximum, as Vulkan's in the SPIR-V translator.
    aniso_filter = std::min(aniso_filter, xenos::AnisoFilter::kMax_16_1);
  }
  for (size_t i = 0; i < sampler_bindings_.size(); ++i) {
    const SamplerBinding& binding = sampler_bindings_[i];
    if (binding.fetch_constant == fetch_constant && binding.mag_filter == mag_filter &&
        binding.min_filter == min_filter && binding.mip_filter == mip_filter &&
        binding.aniso_filter == aniso_filter) {
      return i;
    }
  }
  sampler_bindings_.push_back({fetch_constant, mag_filter, min_filter, mip_filter, aniso_filter});
  return sampler_bindings_.size() - 1;
}

std::string WgslShaderTranslator::TextureBindingName(size_t index) const {
  const TextureBinding& binding = texture_bindings_[index];
  const char* dimension_name;
  switch (binding.dimension) {
    case xenos::FetchOpDimension::k3DOrStacked:
      dimension_name = "3d";
      break;
    case xenos::FetchOpDimension::kCube:
      dimension_name = "cube";
      break;
    default:
      dimension_name = "2d";
      break;
  }
  return fmt::format("xe_texture{}_{}_{}", binding.fetch_constant, dimension_name,
                     binding.is_signed ? 's' : 'u');
}

std::string WgslShaderTranslator::SamplerBindingName(size_t index) const {
  const SamplerBinding& binding = sampler_bindings_[index];
  static const char kFilterSuffixes[] = {'p', 'l', 'b', 'f'};
  std::string name = fmt::format(
      "xe_sampler{}_{}{}{}", binding.fetch_constant, kFilterSuffixes[uint32_t(binding.mag_filter)],
      kFilterSuffixes[uint32_t(binding.min_filter)], kFilterSuffixes[uint32_t(binding.mip_filter)]);
  if (binding.aniso_filter != xenos::AnisoFilter::kUseFetchConst) {
    name += binding.aniso_filter == xenos::AnisoFilter::kDisabled
                ? std::string("_a0")
                : fmt::format("_a{}", UINT32_C(1) << (uint32_t(binding.aniso_filter) - 1));
  }
  return name;
}

void WgslShaderTranslator::SampleTexture(
    const std::string& sample_unsigned, const std::string& sample_signed,
    const std::string& is_any_unsigned, const std::string& is_any_signed,
    std::string& result_unsigned_out, std::string& result_signed_out,
    const std::string& lerp_factor, const std::string& lerp_first_unsigned,
    const std::string& lerp_first_signed) {
  // Only the variants some component needs are sampled; the other is 0.
  for (uint32_t i = 0; i < 2; ++i) {
    std::string result = fmt::format("t{}", next_temp_++);
    Line(fmt::format("var {} = vec4f(0.0f);", result));
    OpenScope(fmt::format("if ({})", i ? is_any_signed : is_any_unsigned));
    const std::string& lerp_first = i ? lerp_first_signed : lerp_first_unsigned;
    if (!lerp_factor.empty() && !lerp_first.empty()) {
      Line(fmt::format("{} = mix({}, {}, {});", result, lerp_first,
                       i ? sample_signed : sample_unsigned, lerp_factor));
    } else {
      Line(fmt::format("{} = {};", result, i ? sample_signed : sample_unsigned));
    }
    CloseScope();
    (i ? result_signed_out : result_unsigned_out) = result;
  }
}

void WgslShaderTranslator::ProcessTextureFetchInstruction(
    const ParsedTextureFetchInstruction& instr) {
  UpdateInstructionPredication(instr.is_predicated, instr.predicate_condition);

  switch (instr.opcode) {
    case ucode::FetchOpcode::kSetTextureLod:
      Line(fmt::format(
          "xe_tfetch_lod = {};",
          GetOperandComponents(LoadOperandStorage(instr.operands[0]), instr.operands[0], 0b0001)
              .expr));
      return;
    case ucode::FetchOpcode::kSetTextureGradientsHorz:
    case ucode::FetchOpcode::kSetTextureGradientsVert:
      Line(fmt::format(
          "xe_tfetch_gradients_{} = {};",
          instr.opcode == ucode::FetchOpcode::kSetTextureGradientsHorz ? 'h' : 'v',
          GetOperandComponents(LoadOperandStorage(instr.operands[0]), instr.operands[0], 0b0111)
              .expr));
      return;
    default:
      break;
  }

  uint32_t used_result_components = instr.result.GetUsedResultComponents();
  uint32_t used_result_nonzero_components = instr.GetNonZeroResultComponents();
  switch (instr.opcode) {
    case ucode::FetchOpcode::kTextureFetch:
    case ucode::FetchOpcode::kGetTextureComputedLod:
    case ucode::FetchOpcode::kGetTextureGradients:
      break;
    case ucode::FetchOpcode::kGetTextureBorderColorFrac:
      EmitTranslationError("getBCF is unimplemented", false);
      used_result_nonzero_components = 0;
      break;
    case ucode::FetchOpcode::kGetTextureWeights:
      // The fourth component would be the mip weight, which isn't available.
      used_result_nonzero_components &= ~uint32_t(0b1000);
      break;
    default:
      assert_unhandled_case(instr.opcode);
      EmitTranslationError("Unknown texture fetch operation");
      used_result_nonzero_components = 0;
      break;
  }
  if (instr.opcode == ucode::FetchOpcode::kGetTextureGradients && !is_pixel_shader()) {
    // No derivatives outside pixel shaders.
    EmitTranslationError("getGradients in a vertex shader", false);
    used_result_nonzero_components = 0;
  }
  if (!used_result_nonzero_components) {
    if (used_result_components) {
      StoreResult(instr.result, Value{"0.0f", 1});
    }
    return;
  }

  std::string result[4] = {"0.0f", "0.0f", "0.0f", "0.0f"};

  if (instr.opcode == ucode::FetchOpcode::kGetTextureGradients) {
    std::string operand_storage = LoadOperandStorage(instr.operands[0]);
    std::string function_x, function_y;
    if (used_result_nonzero_components & 0b0011) {
      function_x = GetOperandComponents(operand_storage, instr.operands[0], 0b0001).expr;
    }
    if (used_result_nonzero_components & 0b1100) {
      function_y = GetOperandComponents(operand_storage, instr.operands[0], 0b0010).expr;
    }
    // (dx/dx, dx/dy, dy/dx, dy/dy).
    uint32_t components_remaining = used_result_nonzero_components;
    uint32_t component;
    while (rex::bit_scan_forward(components_remaining, &component)) {
      components_remaining &= ~(UINT32_C(1) << component);
      result[component] =
          Let(fmt::format("{}({})", (component & 0b01) ? "dpdyCoarse" : "dpdxCoarse",
                          (component & 0b10) ? function_y : function_x));
    }
  } else {
    // Computed LOD needs derivatives, unless the gradients come from registers.
    bool use_computed_lod = instr.attributes.use_computed_lod &&
                            (is_pixel_shader() || instr.attributes.use_register_gradients);
    if (instr.opcode == ucode::FetchOpcode::kGetTextureComputedLod &&
        (!use_computed_lod || instr.attributes.use_register_gradients)) {
      EmitTranslationError("getCompTexLOD used with explicit LOD or gradients - contradicts MSDN",
                           false);
      StoreResult(instr.result, Value{"0.0f", 1});
      return;
    }

    uses_fetch_constants_ = true;
    uint32_t fetch_constant_index = instr.operands[1].storage_index;
    uint32_t fetch_constant_word_0_index = 6 * fetch_constant_index;

    std::string sampler, texture_unsigned, texture_signed, texture_3d_unsigned, texture_3d_signed;
    if (instr.opcode != ucode::FetchOpcode::kGetTextureWeights) {
      sampler = SamplerBindingName(FindOrAddSamplerBinding(
          fetch_constant_index, instr.attributes.mag_filter, instr.attributes.min_filter,
          instr.opcode == ucode::FetchOpcode::kGetTextureComputedLod ? xenos::TextureFilter::kLinear
                                                                     : instr.attributes.mip_filter,
          use_computed_lod ? instr.attributes.aniso_filter : xenos::AnisoFilter::kDisabled));
      // Stacked textures are 2D arrays.
      xenos::FetchOpDimension dimension_2d_array_or_cube =
          instr.dimension == xenos::FetchOpDimension::k3DOrStacked ? xenos::FetchOpDimension::k2D
                                                                   : instr.dimension;
      texture_unsigned = TextureBindingName(
          FindOrAddTextureBinding(fetch_constant_index, dimension_2d_array_or_cube, false));
      texture_signed = TextureBindingName(
          FindOrAddTextureBinding(fetch_constant_index, dimension_2d_array_or_cube, true));
      if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked) {
        texture_3d_unsigned = TextureBindingName(FindOrAddTextureBinding(
            fetch_constant_index, xenos::FetchOpDimension::k3DOrStacked, false));
        texture_3d_signed = TextureBindingName(FindOrAddTextureBinding(
            fetch_constant_index, xenos::FetchOpDimension::k3DOrStacked, true));
      }
    }

    // Offsets, with the rounding offset as in the SPIR-V translator (1.5/1024
    // of a texel, so 0.5 doesn't land exactly between texels).
    float offset_values[3] = {};
    if (instr.opcode != ucode::FetchOpcode::kGetTextureComputedLod) {
      const float kRoundingOffset = 1.5f / 1024.0f;
      bool weights = instr.opcode == ucode::FetchOpcode::kGetTextureWeights;
      uint32_t offset_count = 0;
      switch (instr.dimension) {
        case xenos::FetchOpDimension::k1D:
          offset_count = 1;
          break;
        case xenos::FetchOpDimension::k2D:
        case xenos::FetchOpDimension::kCube:
          offset_count = 2;
          break;
        case xenos::FetchOpDimension::k3DOrStacked:
          offset_count = 3;
          break;
      }
      const float offsets[3] = {instr.attributes.offset_x, instr.attributes.offset_y,
                                instr.attributes.offset_z};
      for (uint32_t i = 0; i < offset_count; ++i) {
        offset_values[i] = offsets[i] + kRoundingOffset - (weights ? 0.5f : 0.0f);
      }
      // The cube face offset is not normalized or rounded.
      if (instr.dimension == xenos::FetchOpDimension::kCube && !weights) {
        offset_values[2] = instr.attributes.offset_z;
      }
    }
    uint32_t offsets_not_zero = 0b000;
    for (uint32_t i = 0; i < 3; ++i) {
      if (offset_values[i]) {
        offsets_not_zero |= 1 << i;
      }
    }

    // Which size components are needed.
    uint32_t size_needed_components = 0b000;
    bool data_is_3d_needed = false;
    if (instr.opcode == ucode::FetchOpcode::kGetTextureWeights) {
      if (!instr.attributes.unnormalized_coordinates) {
        switch (instr.dimension) {
          case xenos::FetchOpDimension::k1D:
            size_needed_components |= used_result_nonzero_components & 0b0001;
            break;
          case xenos::FetchOpDimension::k2D:
          case xenos::FetchOpDimension::kCube:
            size_needed_components |= used_result_nonzero_components & 0b0011;
            break;
          case xenos::FetchOpDimension::k3DOrStacked:
            size_needed_components |= used_result_nonzero_components & 0b0111;
            break;
        }
      }
    } else {
      size_needed_components |= offsets_not_zero;
      switch (instr.dimension) {
        case xenos::FetchOpDimension::k1D:
          if (instr.attributes.unnormalized_coordinates) {
            size_needed_components |= 0b0001;
          }
          break;
        case xenos::FetchOpDimension::k2D:
          if (instr.attributes.unnormalized_coordinates) {
            size_needed_components |= 0b0011;
          }
          break;
        case xenos::FetchOpDimension::k3DOrStacked:
          data_is_3d_needed = true;
          // The layer index of stacked textures is unnormalized.
          size_needed_components |= instr.attributes.unnormalized_coordinates ? 0b0111 : 0b0100;
          break;
        case xenos::FetchOpDimension::kCube:
          if (instr.attributes.unnormalized_coordinates) {
            size_needed_components |= 0b0011;
          }
          // The face isn't normalized.
          size_needed_components &= 0b0011;
          break;
      }
    }
    if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked && size_needed_components) {
      data_is_3d_needed = true;
    }

    std::string data_is_3d;
    if (data_is_3d_needed) {
      data_is_3d = Let(fmt::format("extractBits({}, 9u, 2u) == {}u",
                                   FetchConstantWord(fetch_constant_word_0_index + 5),
                                   uint32_t(xenos::DataDimension::k3D)));
    }

    // Sizes from word 2 (minus 1 there), as f32.
    std::string size[3];
    if (size_needed_components) {
      std::string word_2 = Let(FetchConstantWord(fetch_constant_word_0_index + 2));
      auto field = [&](uint32_t offset, uint32_t bits) {
        return fmt::format("extractBits({}, {}u, {}u)", word_2, offset, bits);
      };
      std::string size_uint[3];
      switch (instr.dimension) {
        case xenos::FetchOpDimension::k1D:
          size_uint[0] = field(0, xenos::kTexture1DMaxWidthLog2);
          break;
        case xenos::FetchOpDimension::k2D:
        case xenos::FetchOpDimension::kCube:
          size_uint[0] = field(0, xenos::kTexture2DCubeMaxWidthHeightLog2);
          size_uint[1] = field(xenos::kTexture2DCubeMaxWidthHeightLog2,
                               xenos::kTexture2DCubeMaxWidthHeightLog2);
          break;
        case xenos::FetchOpDimension::k3DOrStacked:
          size_uint[0] =
              fmt::format("select({}, {}, {})", field(0, xenos::kTexture2DCubeMaxWidthHeightLog2),
                          field(0, xenos::kTexture3DMaxWidthHeightLog2), data_is_3d);
          size_uint[1] = fmt::format(
              "select({}, {}, {})",
              field(xenos::kTexture2DCubeMaxWidthHeightLog2,
                    xenos::kTexture2DCubeMaxWidthHeightLog2),
              field(xenos::kTexture3DMaxWidthHeightLog2, xenos::kTexture3DMaxWidthHeightLog2),
              data_is_3d);
          size_uint[2] = fmt::format(
              "select({}, {}, {})",
              field(xenos::kTexture2DCubeMaxWidthHeightLog2 * 2,
                    xenos::kTexture2DMaxStackDepthLog2),
              field(xenos::kTexture3DMaxWidthHeightLog2 * 2, xenos::kTexture3DMaxDepthLog2),
              data_is_3d);
          break;
      }
      for (uint32_t i = 0; i < 3; ++i) {
        if ((size_needed_components & (UINT32_C(1) << i)) && !size_uint[i].empty()) {
          size[i] = Let(fmt::format("f32({} + 1u)", size_uint[i]));
        }
      }
    }

    // Coordinates.
    uint32_t coordinates_needed_components =
        instr.opcode == ucode::FetchOpcode::kGetTextureWeights
            ? used_result_nonzero_components
            : ((UINT32_C(1) << xenos::GetFetchOpDimensionComponentCount(instr.dimension)) - 1);
    assert_not_zero(coordinates_needed_components);
    Value coordinates_operand = GetOperandComponents(
        LoadOperandStorage(instr.operands[0]), instr.operands[0], coordinates_needed_components);
    std::string coordinates[3] = {"0.0f", "0.0f", "0.0f"};
    {
      uint32_t components_remaining = coordinates_needed_components;
      uint32_t component;
      for (uint32_t i = 0; rex::bit_scan_forward(components_remaining, &component); ++i) {
        components_remaining &= ~(UINT32_C(1) << component);
        coordinates[component] = Component(coordinates_operand, i);
      }
    }

    // Draw resolution scaling: guest texel coordinates of resolution-scaled
    // textures (from resolves) are in guest texels.
    std::string texture_resolution_scaled;
    uint32_t revert_resolution_scale_axes =
        uint32_t(draw_resolution_scale_x_ > 1) | (uint32_t(draw_resolution_scale_y_ > 1) << 1);
    const float resolution_scale[2] = {float(draw_resolution_scale_x_),
                                       float(draw_resolution_scale_y_)};
    if (revert_resolution_scale_axes) {
      texture_resolution_scaled =
          Let(fmt::format("(xe_system.textures_resolution_scaled & {}u) != 0u",
                          UINT32_C(1) << fetch_constant_index));
    }

    if (instr.opcode == ucode::FetchOpcode::kGetTextureWeights) {
      // fract of the position in texels (for the bilinear weights).
      uint32_t resolution_scaled_result_components =
          used_result_nonzero_components & revert_resolution_scale_axes;
      uint32_t resolution_scaled_coord_components =
          instr.attributes.unnormalized_coordinates ? resolution_scaled_result_components : 0b000;
      uint32_t resolution_scaled_size_components =
          size_needed_components & resolution_scaled_result_components;
      if (!texture_resolution_scaled.empty()) {
        for (uint32_t i = 0; i < 2; ++i) {
          std::string scale = FloatConst(resolution_scale[i], 1);
          if (resolution_scaled_coord_components & (UINT32_C(1) << i)) {
            coordinates[i] = Let(fmt::format("select({}, {} * {}, {})", coordinates[i],
                                             coordinates[i], scale, texture_resolution_scaled));
          }
          if (resolution_scaled_size_components & (UINT32_C(1) << i)) {
            size[i] = Let(fmt::format("select({}, {} * {}, {})", size[i], size[i], scale,
                                      texture_resolution_scaled));
          }
        }
      }
      uint32_t components_remaining = coordinates_needed_components;
      uint32_t component;
      while (rex::bit_scan_forward(components_remaining, &component)) {
        components_remaining &= ~(UINT32_C(1) << component);
        std::string value = coordinates[component];
        if (!instr.attributes.unnormalized_coordinates) {
          assert_false(size[component].empty());
          value = fmt::format("{} * {}", value, size[component]);
        }
        if (offset_values[component]) {
          value = fmt::format("{} + {}", value, FloatConst(offset_values[component], 1));
        }
        result[component] = Let(fmt::format("fract({})", value));
      }
    } else {
      // Normalize X and Y (unnormalized coordinates or offsets in texels).
      for (uint32_t i = 0; i <= uint32_t(instr.dimension != xenos::FetchOpDimension::k1D); ++i) {
        std::string offset = offset_values[i] ? FloatConst(offset_values[i], 1) : std::string();
        if (!offset.empty() && !texture_resolution_scaled.empty() &&
            (revert_resolution_scale_axes & (UINT32_C(1) << i))) {
          offset = Let(fmt::format("select({}, {} * {}, {})", offset, offset,
                                   FloatConst(1.0f / resolution_scale[i], 1),
                                   texture_resolution_scaled));
        }
        if (instr.attributes.unnormalized_coordinates) {
          assert_false(size[i].empty());
          coordinates[i] =
              Let(offset.empty() ? fmt::format("{} / {}", coordinates[i], size[i])
                                 : fmt::format("({} + {}) / {}", coordinates[i], offset, size[i]));
        } else if (!offset.empty()) {
          assert_false(size[i].empty());
          coordinates[i] = Let(fmt::format("{} + {} / {}", coordinates[i], offset, size[i]));
        }
      }
      if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked) {
        // Z: normalized for 3D, the layer index for stacked.
        std::string z_offset = offset_values[2] ? FloatConst(offset_values[2], 1) : std::string();
        const std::string& z = coordinates[2];
        assert_false(size[2].empty());
        if (instr.attributes.unnormalized_coordinates) {
          std::string z_offset_applied =
              z_offset.empty() ? z : Let(fmt::format("{} + {}", z, z_offset));
          coordinates[2] = Let(fmt::format("select({}, {} / {}, {})", z_offset_applied,
                                           z_offset_applied, size[2], data_is_3d));
        } else {
          std::string z_3d =
              z_offset.empty() ? z : fmt::format("{} + {} / {}", z, z_offset, size[2]);
          std::string z_stacked = z_offset.empty()
                                      ? fmt::format("{} * {}", z, size[2])
                                      : fmt::format("{} * {} + {}", z, size[2], z_offset);
          coordinates[2] = Let(fmt::format("select({}, {}, {})", z_stacked, z_3d, data_is_3d));
        }
      } else if (instr.dimension == xenos::FetchOpDimension::kCube) {
        // The face index and 2D coordinates in [1, 2] to a direction.
        std::string sc = Let(fmt::format("{} * 2.0f - 3.0f", coordinates[0]));
        std::string tc = Let(fmt::format("{} * 2.0f - 3.0f", coordinates[1]));
        std::string face =
            offset_values[2]
                ? Let(fmt::format("{} + {}", coordinates[2], FloatConst(offset_values[2], 1)))
                : coordinates[2];
        std::string face_index =
            Let(fmt::format("u32(select(0.0f, min({}, 5.0f), {} >= 0.0f))", face, face));
        std::string face_axis = Let(fmt::format("{} >> 1u", face_index));
        std::string face_is_negative = Let(fmt::format("({} & 1u) != 0u", face_index));
        std::string face_sign = Let(fmt::format("select(1.0f, -1.0f, {})", face_is_negative));
        // X: +-1, sc, (neg ? -sc : sc); Y: -tc, +-1, -tc; Z: (neg ? sc : -sc),
        // (neg ? -tc : tc), +-1 for the X, Y and Z major axes.
        coordinates[0] =
            Let(fmt::format("select(select(select({}, -{}, {}), {}, {} == 1u), {}, {} == 0u)", sc,
                            sc, face_is_negative, sc, face_axis, face_sign, face_axis));
        coordinates[1] = Let(fmt::format("select(select(-{}, {}, {} == 1u), -{}, {} == 0u)", tc,
                                         face_sign, face_axis, tc, face_axis));
        coordinates[2] = Let(fmt::format(
            "select(select({}, select({}, -{}, {}), {} == 1u), select(-{}, {}, {}), {} == 0u)",
            face_sign, tc, tc, face_is_negative, face_axis, sc, sc, face_is_negative, face_axis));
      }

      std::string swizzled_signs_word =
          Let(fmt::format("xe_system.texture_swizzled_signs[{}][{}]", fetch_constant_index >> 4,
                          (fetch_constant_index >> 2) & 3));
      uint32_t swizzled_signs_word_offset = 8 * (fetch_constant_index & 3);

      if (instr.opcode == ucode::FetchOpcode::kGetTextureComputedLod) {
        // WGSL has no LOD query: the LOD from the coordinate derivatives in
        // texels, clamped to the texture's levels. Unlike a query, this ignores
        // anisotropy and the sampler's LOD clamp. Signed and unsigned views
        // have the same size, so the unsigned one is used.
        auto lod_2d = [&](const std::string& texture, const std::string& coords,
                          const std::string& size_expr) {
          std::string dx = Let(fmt::format("dpdxCoarse({}) * {}", coords, size_expr));
          std::string dy = Let(fmt::format("dpdyCoarse({}) * {}", coords, size_expr));
          return Let(
              fmt::format("clamp(0.5f * log2(max(dot({}, {}), dot({}, {}))), 0.0f, "
                          "f32(textureNumLevels({}) - 1u))",
                          dx, dx, dy, dy, texture));
        };
        if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked) {
          std::string lod_3d = lod_2d(
              texture_3d_unsigned,
              fmt::format("vec3f({}, {}, {})", coordinates[0], coordinates[1], coordinates[2]),
              fmt::format("vec3f(textureDimensions({}, 0))", texture_3d_unsigned));
          std::string lod_stacked =
              lod_2d(texture_unsigned, fmt::format("vec2f({}, {})", coordinates[0], coordinates[1]),
                     fmt::format("vec2f(textureDimensions({}, 0))", texture_unsigned));
          result[0] = Let(fmt::format("select({}, {}, {})", lod_stacked, lod_3d, data_is_3d));
        } else if (instr.dimension == xenos::FetchOpDimension::kCube) {
          // The direction's derivatives, on the major axis face (2 units wide).
          std::string direction =
              Let(fmt::format("vec3f({}, {}, {})", coordinates[0], coordinates[1], coordinates[2]));
          std::string scale = Let(fmt::format(
              "0.5f * f32(textureDimensions({}, 0).x) / max(abs({}.x), max(abs({}.y), abs({}.z)))",
              texture_unsigned, direction, direction, direction));
          result[0] = lod_2d(texture_unsigned, direction, scale);
        } else {
          result[0] =
              lod_2d(texture_unsigned, fmt::format("vec2f({}, {})", coordinates[0], coordinates[1]),
                     fmt::format("vec2f(textureDimensions({}, 0))", texture_unsigned));
        }
      } else {
        assert_true(instr.opcode == ucode::FetchOpcode::kTextureFetch);
        // Per component: post-swizzle signedness, and which views to sample.
        std::string swizzled_signs[4], result_is_signed[4];
        std::string is_all_signed, is_any_signed;
        {
          uint32_t components_remaining = used_result_nonzero_components;
          uint32_t component;
          while (rex::bit_scan_forward(components_remaining, &component)) {
            components_remaining &= ~(UINT32_C(1) << component);
            swizzled_signs[component] =
                Let(fmt::format("extractBits({}, {}u, 2u)", swizzled_signs_word,
                                swizzled_signs_word_offset + 2 * component));
            result_is_signed[component] = Let(fmt::format("{} == {}u", swizzled_signs[component],
                                                          uint32_t(xenos::TextureSign::kSigned)));
            is_all_signed = is_all_signed.empty() ? result_is_signed[component]
                                                  : fmt::format("{} && {}", is_all_signed,
                                                                result_is_signed[component]);
            is_any_signed = is_any_signed.empty() ? result_is_signed[component]
                                                  : fmt::format("{} || {}", is_any_signed,
                                                                result_is_signed[component]);
          }
        }
        is_all_signed = Let(is_all_signed);
        is_any_signed = Let(is_any_signed);
        std::string is_any_unsigned = Let(fmt::format("!{}", is_all_signed));

        std::string word_3 = Let(FetchConstantWord(fetch_constant_word_0_index + 3));
        std::string word_4 = Let(FetchConstantWord(fetch_constant_word_0_index + 4));

        // The LOD bias from the fetch constant (signed 5.5), plus the register
        // LOD and the instruction's bias.
        std::string lod =
            fmt::format("f32(extractBits(bitcast<i32>({}), 12u, 10u)) * (1.0f / 32.0f)", word_4);
        if (instr.attributes.use_register_lod) {
          lod = fmt::format("xe_tfetch_lod + {}", lod);
        }
        if (instr.attributes.lod_bias) {
          lod = fmt::format("{} + {}", lod, FloatConst(instr.attributes.lod_bias, 1));
        }
        lod = Let(lod);

        // Explicit gradients, scaled by 2^lod, for the computed LOD.
        std::string gradients_h, gradients_v;
        if (use_computed_lod) {
          std::string lod_gradient_scale = Let(fmt::format("exp2({})", lod));
          bool register_gradients = instr.attributes.use_register_gradients;
          bool unnormalized = instr.attributes.unnormalized_coordinates;
          switch (instr.dimension) {
            case xenos::FetchOpDimension::k1D: {
              std::string h, v;
              if (register_gradients) {
                h = "xe_tfetch_gradients_h.x";
                v = "xe_tfetch_gradients_v.x";
                if (unnormalized) {
                  h = fmt::format("{} / {}", h, size[0]);
                  v = fmt::format("{} / {}", v, size[0]);
                }
              } else {
                h = fmt::format("dpdxCoarse({})", coordinates[0]);
                v = fmt::format("dpdyCoarse({})", coordinates[0]);
              }
              gradients_h = Let(fmt::format("vec2f(({}) * {}, 0.0f)", h, lod_gradient_scale));
              gradients_v = Let(fmt::format("vec2f(({}) * {}, 0.0f)", v, lod_gradient_scale));
            } break;
            case xenos::FetchOpDimension::k2D: {
              std::string h, v;
              if (register_gradients) {
                h = "xe_tfetch_gradients_h.xy";
                v = "xe_tfetch_gradients_v.xy";
                if (unnormalized) {
                  h = fmt::format("{} / vec2f({}, {})", h, size[0], size[1]);
                  v = fmt::format("{} / vec2f({}, {})", v, size[0], size[1]);
                }
              } else {
                std::string coords =
                    Let(fmt::format("vec2f({}, {})", coordinates[0], coordinates[1]));
                h = fmt::format("dpdxCoarse({})", coords);
                v = fmt::format("dpdyCoarse({})", coords);
              }
              gradients_h = Let(fmt::format("({}) * {}", h, lod_gradient_scale));
              gradients_v = Let(fmt::format("({}) * {}", v, lod_gradient_scale));
            } break;
            case xenos::FetchOpDimension::k3DOrStacked:
            case xenos::FetchOpDimension::kCube: {
              std::string h, v;
              if (register_gradients) {
                h = "xe_tfetch_gradients_h";
                v = "xe_tfetch_gradients_v";
                if (unnormalized && instr.dimension == xenos::FetchOpDimension::k3DOrStacked) {
                  std::string sizes = fmt::format("vec3f({}, {}, {})", size[0], size[1], size[2]);
                  h = fmt::format("{} / {}", h, sizes);
                  v = fmt::format("{} / {}", v, sizes);
                }
              } else {
                std::string coords = Let(fmt::format("vec3f({}, {}, {})", coordinates[0],
                                                     coordinates[1], coordinates[2]));
                h = fmt::format("dpdxCoarse({})", coords);
                v = fmt::format("dpdyCoarse({})", coords);
              }
              gradients_h = Let(fmt::format("({}) * {}", h, lod_gradient_scale));
              gradients_v = Let(fmt::format("({}) * {}", v, lod_gradient_scale));
            } break;
          }
        }

        // textureSampleLevel or textureSampleGrad arguments after the texture
        // and the sampler.
        auto sample_2d_array = [&](const std::string& texture, const std::string& layer,
                                   bool gradients_xy) {
          std::string coords = fmt::format("vec2f({}, {})", coordinates[0], coordinates[1]);
          if (use_computed_lod) {
            return fmt::format("textureSampleGrad({}, {}, {}, i32({}), {}{}, {}{})", texture,
                               sampler, coords, layer, gradients_h, gradients_xy ? ".xy" : "",
                               gradients_v, gradients_xy ? ".xy" : "");
          }
          return fmt::format("textureSampleLevel({}, {}, {}, i32({}), {})", texture, sampler,
                             coords, layer, lod);
        };
        auto sample_3d_or_cube = [&](const std::string& texture) {
          std::string coords =
              fmt::format("vec3f({}, {}, {})", coordinates[0], coordinates[1], coordinates[2]);
          if (use_computed_lod) {
            return fmt::format("textureSampleGrad({}, {}, {}, {}, {})", texture, sampler, coords,
                               gradients_h, gradients_v);
          }
          return fmt::format("textureSampleLevel({}, {}, {}, {})", texture, sampler, coords, lod);
        };

        std::string sample_unsigned, sample_signed;
        if (instr.dimension == xenos::FetchOpDimension::k3DOrStacked) {
          sample_unsigned = fmt::format("t{}", next_temp_++);
          sample_signed = fmt::format("t{}", next_temp_++);
          Line(fmt::format("var {} = vec4f(0.0f);", sample_unsigned));
          Line(fmt::format("var {} = vec4f(0.0f);", sample_signed));
          OpenScope(fmt::format("if ({})", data_is_3d));
          {
            std::string u, s;
            SampleTexture(sample_3d_or_cube(texture_3d_unsigned),
                          sample_3d_or_cube(texture_3d_signed), is_any_unsigned, is_any_signed, u,
                          s);
            Line(fmt::format("{} = {};", sample_unsigned, u));
            Line(fmt::format("{} = {};", sample_signed, s));
          }
          indent_--;
          Line("} else {");
          indent_++;
          {
            // Stacked: filtering between layers is done here, by sampling two
            // layers, per the volume filters (from the fetch constant, or
            // depending on whether Z is magnified with the computed LOD).
            bool vol_mag_filter_is_fetch_const =
                instr.attributes.vol_mag_filter == xenos::TextureFilter::kUseFetchConst;
            bool vol_min_filter_is_fetch_const =
                instr.attributes.vol_min_filter == xenos::TextureFilter::kUseFetchConst;
            bool vol_mag_filter_is_linear =
                instr.attributes.vol_mag_filter == xenos::TextureFilter::kLinear;
            bool vol_min_filter_is_linear =
                instr.attributes.vol_min_filter == xenos::TextureFilter::kLinear;
            std::string vol_mag_filter_is_linear_loaded =
                vol_mag_filter_is_fetch_const ? fmt::format("({} & 1u) != 0u", word_4)
                                              : (vol_mag_filter_is_linear ? "true" : "false");
            std::string vol_filter_is_linear;
            if (use_computed_lod &&
                (vol_mag_filter_is_fetch_const || vol_min_filter_is_fetch_const ||
                 vol_mag_filter_is_linear != vol_min_filter_is_linear)) {
              std::string layer_max_gradient =
                  fmt::format("max({}.z, {}.z)", gradients_h, gradients_v);
              if (!instr.attributes.unnormalized_coordinates) {
                layer_max_gradient = fmt::format("{} * {}", layer_max_gradient, size[2]);
              }
              std::string vol_min_filter_is_linear_loaded =
                  vol_min_filter_is_fetch_const ? fmt::format("({} & 2u) != 0u", word_4)
                                                : (vol_min_filter_is_linear ? "true" : "false");
              vol_filter_is_linear =
                  Let(fmt::format("select({}, {}, {} < 1.0f)", vol_mag_filter_is_linear_loaded,
                                  vol_min_filter_is_linear_loaded, layer_max_gradient));
            } else if (vol_mag_filter_is_fetch_const) {
              vol_filter_is_linear = Let(vol_mag_filter_is_linear_loaded);
            }
            std::string layer = coordinates[2];
            if (!vol_filter_is_linear.empty()) {
              layer =
                  Let(fmt::format("select({}, {} - 0.5f, {})", layer, layer, vol_filter_is_linear));
            } else if (vol_mag_filter_is_linear) {
              layer = Let(fmt::format("{} - 0.5f", layer));
            }
            std::string layer_0 = Let(fmt::format("floor({})", layer));
            std::string u, s;
            SampleTexture(sample_2d_array(texture_unsigned, layer_0, true),
                          sample_2d_array(texture_signed, layer_0, true), is_any_unsigned,
                          is_any_signed, u, s);
            Line(fmt::format("{} = {};", sample_unsigned, u));
            Line(fmt::format("{} = {};", sample_signed, s));
            if (!vol_filter_is_linear.empty() || vol_mag_filter_is_linear) {
              if (!vol_filter_is_linear.empty()) {
                OpenScope(fmt::format("if ({})", vol_filter_is_linear));
              }
              std::string layer_1 = Let(fmt::format("{} + 1.0f", layer_0));
              std::string lerp_factor = Let(fmt::format("fract({})", layer));
              std::string u1, s1;
              SampleTexture(sample_2d_array(texture_unsigned, layer_1, true),
                            sample_2d_array(texture_signed, layer_1, true), is_any_unsigned,
                            is_any_signed, u1, s1, lerp_factor, u, s);
              Line(fmt::format("{} = {};", sample_unsigned, u1));
              Line(fmt::format("{} = {};", sample_signed, s1));
              if (!vol_filter_is_linear.empty()) {
                CloseScope();
              }
            }
          }
          CloseScope();
        } else if (instr.dimension == xenos::FetchOpDimension::kCube) {
          SampleTexture(sample_3d_or_cube(texture_unsigned), sample_3d_or_cube(texture_signed),
                        is_any_unsigned, is_any_signed, sample_unsigned, sample_signed);
        } else {
          SampleTexture(sample_2d_array(texture_unsigned, coordinates[2], false),
                        sample_2d_array(texture_signed, coordinates[2], false), is_any_unsigned,
                        is_any_signed, sample_unsigned, sample_signed);
        }

        // The guest swizzle (WebGPU views can't swizzle): 3 bits per
        // component, bit 2 for a constant (bit 0: 0 or 1), else bits 0:1 the
        // source component, from the signed or the unsigned sample.
        std::string swizzle_word =
            Let(fmt::format("xe_system.texture_swizzles[{}][{}]", fetch_constant_index >> 3,
                            (fetch_constant_index >> 1) & 3));
        uint32_t swizzle_word_offset = 3 * 4 * (fetch_constant_index & 1);
        std::string exponent_bias =
            Let(fmt::format("ldexp(1.0f, extractBits(bitcast<i32>({}), 13u, 6u))", word_3));
        uint32_t components_remaining = used_result_nonzero_components;
        uint32_t component;
        while (rex::bit_scan_forward(components_remaining, &component)) {
          components_remaining &= ~(UINT32_C(1) << component);
          uint32_t swizzle_bit_0 = UINT32_C(1) << (swizzle_word_offset + 3 * component);
          auto bit = [&](uint32_t value) {
            return fmt::format("({} & {}u) != 0u", swizzle_word, value);
          };
          std::string sample = Let(fmt::format("select({}, {}, {})", sample_unsigned, sample_signed,
                                               result_is_signed[component]));
          std::string value = Let(
              fmt::format("select(select(select({}.x, {}.y, {}), select({}.z, {}.w, {}), {}), "
                          "select(0.0f, 1.0f, {}), {})",
                          sample, sample, bit(swizzle_bit_0), sample, sample, bit(swizzle_bit_0),
                          bit(swizzle_bit_0 << 1), bit(swizzle_bit_0), bit(swizzle_bit_0 << 2)));
          // Biased (2x - 1) and gamma (piecewise linear) conversion; signed
          // values are already signed.
          uses_pwl_gamma_to_linear_ = true;
          value =
              Let(fmt::format("select(select({}, {} * 2.0f - 1.0f, {} == {}u), "
                              "xe_pwl_gamma_to_linear({}), {} == {}u)",
                              value, value, swizzled_signs[component],
                              uint32_t(xenos::TextureSign::kUnsignedBiased), value,
                              swizzled_signs[component], uint32_t(xenos::TextureSign::kGamma)));
          result[component] = Let(fmt::format("{} * {}", value, exponent_bias));
        }
      }
    }
  }

  uint32_t used_result_component_count = rex::bit_count(used_result_components);
  std::string result_vector =
      used_result_component_count > 1 ? FloatType(used_result_component_count) + "(" : "";
  bool first = true;
  for (uint32_t i = 0; i < 4; ++i) {
    if (used_result_components & (UINT32_C(1) << i)) {
      result_vector += first ? "" : ", ";
      result_vector += result[i];
      first = false;
    }
  }
  if (used_result_component_count > 1) {
    result_vector += ")";
  }
  StoreResult(instr.result, Value{Let(result_vector), used_result_component_count});
}

}  // namespace rex::graphics
