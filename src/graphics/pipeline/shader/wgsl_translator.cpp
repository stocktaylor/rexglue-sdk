/**
 * @file        graphics/pipeline/shader/wgsl_translator.cpp
 * @brief       Xenos microcode to WGSL: structure, control flow, operands and
 *              results, vertex and pixel shader prologues and epilogues
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/pipeline/shader/wgsl_translator.h>

#include <cstring>

#include <algorithm>

#include <fmt/format.h>

#include <rex/assert.h>
#include <rex/graphics/pipeline/shader/wgsl.h>
#include <rex/math.h>

namespace rex::graphics {

namespace {

// Must match WgslShaderTranslator::SystemConstants.
constexpr const char kSystemConstantsWgsl[] = R"(struct XeSystemConstants {
  flags: u32,
  vertex_index_load_address: u32,
  vertex_index_endian: u32,
  line_loop_closing_index: u32,
  vertex_base_index: i32,
  vertex_index_min: u32,
  vertex_index_max: u32,
  padding_0: u32,
  user_clip_planes: array<vec4f, 6>,
  ndc_scale: vec3f,
  point_vertex_diameter_min: f32,
  ndc_offset: vec3f,
  point_vertex_diameter_max: f32,
  point_constant_diameter: vec2f,
  point_screen_diameter_to_ndc_radius: vec2f,
  textures_resolution_scaled: u32,
  alpha_test_reference: f32,
  alpha_to_mask: u32,
  padding_1: u32,
  texture_swizzled_signs: array<vec4u, 2>,
  texture_swizzles: array<vec4u, 4>,
  color_exp_bias: vec4f,
}
)";

constexpr const char kSwizzleComponents[] = "xyzw";

}  // namespace

uint64_t WgslShaderTranslator::GetDefaultVertexShaderModification(
    uint32_t dynamic_addressable_register_count,
    Shader::HostVertexShaderType host_vertex_shader_type) const {
  Modification shader_modification;
  shader_modification.vertex.dynamic_addressable_register_count =
      dynamic_addressable_register_count;
  shader_modification.vertex.host_vertex_shader_type = host_vertex_shader_type;
  return shader_modification.value;
}

uint64_t WgslShaderTranslator::GetDefaultPixelShaderModification(
    uint32_t dynamic_addressable_register_count) const {
  Modification shader_modification;
  shader_modification.pixel.dynamic_addressable_register_count = dynamic_addressable_register_count;
  return shader_modification.value;
}

void WgslShaderTranslator::Reset() {
  ShaderTranslator::Reset();
  body_.clear();
  indent_ = 0;
  next_temp_ = 0;
  case_terminated_ = false;
  cf_exec_bool_constant_or_predicate_ = kCfExecBoolConstantNone;
  cf_exec_condition_ = false;
  cf_exec_predicate_written_ = false;
  cf_instruction_predicate_open_ = false;
  cf_instruction_predicate_condition_ = false;
  uses_shared_memory_ = false;
  uses_float_constants_ = false;
  uses_bool_loop_constants_ = false;
  uses_fetch_constants_ = false;
  uses_pwl_gamma_to_linear_ = false;
  uses_linear_to_pwl_gamma_ = false;
  uses_float24_depth_ = false;
  uses_fragment_coordinates_ = false;
  uses_front_facing_ = false;
  uses_point_coordinates_ = false;
  has_point_size_edge_flag_kill_vertex_ = false;
  clip_distance_count_ = 0;
  texture_bindings_.clear();
  sampler_bindings_.clear();
  reported_memexport_ = false;
}

uint32_t WgslShaderTranslator::GetModificationRegisterCount() const {
  Modification modification = GetWgslShaderModification();
  return is_vertex_shader() ? modification.vertex.dynamic_addressable_register_count
                            : modification.pixel.dynamic_addressable_register_count;
}

uint32_t WgslShaderTranslator::GetPsParamGenInterpolator() const {
  assert_true(is_pixel_shader());
  Modification modification = GetWgslShaderModification();
  return (modification.pixel.param_gen_enable &&
          modification.pixel.param_gen_interpolator < register_count())
             ? modification.pixel.param_gen_interpolator
             : UINT32_MAX;
}

void WgslShaderTranslator::Line(const std::string& text) {
  body_.append(size_t(indent_) * 2, ' ');
  body_ += text;
  body_ += '\n';
}

void WgslShaderTranslator::OpenScope(const std::string& header) {
  Line(header + " {");
  ++indent_;
}

void WgslShaderTranslator::CloseScope() {
  assert_not_zero(indent_);
  --indent_;
  Line("}");
}

std::string WgslShaderTranslator::Let(const std::string& expr) {
  std::string name = fmt::format("t{}", next_temp_++);
  Line(fmt::format("let {} = {};", name, expr));
  return name;
}

std::string WgslShaderTranslator::FloatType(uint32_t n) {
  assert_true(n >= 1 && n <= 4);
  return n == 1 ? std::string("f32") : fmt::format("vec{}f", n);
}

std::string WgslShaderTranslator::FloatConst(float value, uint32_t n) {
  // A decimal literal with the f suffix is f32 (not abstract int, as "1" would
  // be); infinities and NaNs have no literal.
  std::string literal = fmt::format("{}f", value);
  return n == 1 ? literal : fmt::format("vec{}f({})", n, literal);
}

std::string WgslShaderTranslator::Component(const Value& value, uint32_t i) {
  assert_true(i < value.n);
  if (value.n == 1) {
    return value.expr;
  }
  return fmt::format("{}.{}", value.expr, kSwizzleComponents[i]);
}

void WgslShaderTranslator::StartTranslation() {
  Modification modification = GetWgslShaderModification();
  if (is_vertex_shader()) {
    Shader::HostVertexShaderType host_vertex_shader_type =
        modification.vertex.host_vertex_shader_type;
    if (host_vertex_shader_type != Shader::HostVertexShaderType::kVertex &&
        host_vertex_shader_type != Shader::HostVertexShaderType::kPointListAsTriangleStrip) {
      // No tessellation in WebGPU; rectangles and the memexport compute shader
      // aren't done yet.
      EmitTranslationError("Unsupported host vertex shader type");
    }
  }

  indent_ = 1;
  if (is_vertex_shader()) {
    Line("var xe_out: XeVertexOutput;");
    Line("var xe_pos = vec4f(0.0f);");
  } else if (HasFragmentOutput()) {
    Line("var xe_out: XeFragmentOutput;");
  }
  if (register_count()) {
    Line(fmt::format("var xe_r: array<vec4f, {}>;", register_count()));
  }
  Line("var xe_p = false;");
  Line("var xe_a0 = 0i;");
  Line("var xe_aL = vec4i(0i);");
  Line("var xe_lc = vec4u(0u);");
  Line("var xe_ps = 0.0f;");
  // vfetch_full's address for vfetch_mini, and the texture fetch LOD and
  // gradients set by setTexLOD and setGradientH/V.
  Line("var xe_vfetch_address = 0i;");
  Line("var xe_tfetch_lod = 0.0f;");
  Line("var xe_tfetch_gradients_h = vec3f(0.0f);");
  Line("var xe_tfetch_gradients_v = vec3f(0.0f);");

  if (is_vertex_shader()) {
    StartVertexShaderInMain();
  } else {
    StartPixelShaderInMain();
  }

  // The main loop around a switch on the program counter: jumps (including
  // guest loops) set the counter and leave the switch, and end of the shader
  // sets it to -1. Unlike in SPIR-V, WGSL switch cases can't fall through, so
  // each case also continues to the next label explicitly.
  Line("var xe_pc = 0i;");
  OpenScope("loop");
  OpenScope("switch xe_pc");
  OpenScope("case 0i:");
  case_terminated_ = false;
}

std::vector<uint8_t> WgslShaderTranslator::CompleteTranslation() {
  CloseExecConditionals();
  if (!case_terminated_) {
    Line("xe_pc = -1i;");
  }
  CloseScope();  // The last case.
  OpenScope("default:");
  Line("xe_pc = -1i;");
  CloseScope();
  CloseScope();  // switch
  Line("if (xe_pc < 0i) { break; }");
  CloseScope();  // loop

  if (is_vertex_shader()) {
    CompleteVertexShaderInMain();
  } else {
    CompletePixelShaderInMain();
  }

  std::string wgsl = BuildDeclarations();
  wgsl += body_;
  if (is_vertex_shader() || HasFragmentOutput()) {
    wgsl += "  return xe_out;\n";
  }
  wgsl += "}\n";
  return std::vector<uint8_t>(wgsl.begin(), wgsl.end());
}

std::string WgslShaderTranslator::BuildDeclarations() const {
  Modification modification = GetWgslShaderModification();
  const Shader::ConstantRegisterMap& constant_register_map =
      current_shader().constant_register_map();
  std::string out;
  if (clip_distance_count_) {
    out += "enable clip_distances;\n";
  }
  // Derivatives (texture sampling) happen under guest control flow, which
  // always looks non-uniform here.
  out += "diagnostic(off, derivative_uniformity);\n\n";
  out += "const XE_FLT_MAX = 0x1.fffffep+127f;\n\n";

  out += kSystemConstantsWgsl;
  out += fmt::format("@group({}) @binding({}) var<uniform> xe_system: XeSystemConstants;\n\n",
                     uint32_t(kBindGroupConstants), uint32_t(kConstantBufferSystem));
  if (uses_float_constants_) {
    out += fmt::format(
        "struct XeFloatConstants {{\n  c: array<vec4f, {}>,\n}}\n"
        "@group({}) @binding({}) var<uniform> xe_float: XeFloatConstants;\n\n",
        std::max(constant_register_map.float_count, uint32_t(1)), uint32_t(kBindGroupConstants),
        uint32_t(is_vertex_shader() ? kConstantBufferFloatVertex : kConstantBufferFloatPixel));
  }
  if (uses_bool_loop_constants_) {
    out += fmt::format(
        "struct XeBoolLoopConstants {{\n  b: array<vec4u, 2>,\n  l: array<vec4u, 8>,\n}}\n"
        "@group({}) @binding({}) var<uniform> xe_bool_loop: XeBoolLoopConstants;\n\n",
        uint32_t(kBindGroupConstants), uint32_t(kConstantBufferBoolLoop));
  }
  if (uses_fetch_constants_) {
    out += fmt::format(
        "struct XeFetchConstants {{\n  c: array<vec4u, 48>,\n}}\n"
        "@group({}) @binding({}) var<uniform> xe_fetch: XeFetchConstants;\n\n",
        uint32_t(kBindGroupConstants), uint32_t(kConstantBufferFetch));
  }
  if (uses_shared_memory_) {
    // Split into bindings of 2^(29 - n) bytes if needed.
    uint32_t binding_count_log2 = features_.shared_memory_binding_count_log2;
    uint32_t binding_count = uint32_t(1) << binding_count_log2;
    for (uint32_t i = 0; i < binding_count; ++i) {
      out += fmt::format(
          "@group({}) @binding({}) var<storage, read> xe_shared_memory_{}: array<u32>;\n",
          uint32_t(kBindGroupSharedMemory), i, i);
    }
    out += "\nfn xe_load_shared(address: i32) -> u32 {\n";
    if (!binding_count_log2) {
      out += "  return xe_shared_memory_0[address];\n";
    } else {
      uint32_t binding_address_bits = (29 - 2) - binding_count_log2;
      out += fmt::format(
          "  let binding_address = address & {}i;\n  var value = 0u;\n"
          "  switch (bitcast<u32>(address) >> {}u) {{\n",
          (uint32_t(1) << binding_address_bits) - 1, binding_address_bits);
      for (uint32_t i = 0; i < binding_count; ++i) {
        out += fmt::format("    case {}u: {{ value = xe_shared_memory_{}[binding_address]; }}\n", i,
                           i);
      }
      out += "    default: {}\n  }\n  return value;\n";
    }
    out += "}\n\n";
  }

  // Textures, then samplers.
  uint32_t texture_group =
      uint32_t(is_vertex_shader() ? kBindGroupTexturesVertex : kBindGroupTexturesPixel);
  for (size_t i = 0; i < texture_bindings_.size(); ++i) {
    const char* type;
    switch (texture_bindings_[i].dimension) {
      case xenos::FetchOpDimension::k3DOrStacked:
        type = "texture_3d<f32>";
        break;
      case xenos::FetchOpDimension::kCube:
        type = "texture_cube<f32>";
        break;
      default:
        type = "texture_2d_array<f32>";
        break;
    }
    out += fmt::format("@group({}) @binding({}) var {}: {};\n", texture_group, i,
                       TextureBindingName(i), type);
  }
  for (size_t i = 0; i < sampler_bindings_.size(); ++i) {
    out += fmt::format("@group({}) @binding({}) var {}: sampler;\n", texture_group,
                       texture_bindings_.size() + i, SamplerBindingName(i));
  }
  if (!texture_bindings_.empty() || !sampler_bindings_.empty()) {
    out += "\n";
  }

  if (uses_linear_to_pwl_gamma_) {
    // As SpirvShaderTranslator::LinearToPWLGamma.
    out +=
        "fn xe_linear_to_pwl_gamma(linear_unclamped: vec3f) -> vec3f {\n"
        "  let value = select(vec3f(0.0f), min(linear_unclamped, vec3f(1.0f)),\n"
        "                     linear_unclamped >= vec3f(0.0f));\n"
        "  let piece_3 = value >= vec3f(512.0f / 1023.0f);\n"
        "  let piece_1 = value >= vec3f(64.0f / 1023.0f);\n"
        "  let piece_2 = value >= vec3f(128.0f / 1023.0f);\n"
        "  let scale = select(select(vec3f(1023.0f), vec3f(1023.0f / 2.0f), piece_1),\n"
        "                     select(vec3f(1023.0f / 4.0f), vec3f(1023.0f / 8.0f), piece_3),\n"
        "                     piece_2);\n"
        "  let offset = select(select(vec3f(0.0f), vec3f(32.0f / 255.0f), piece_1),\n"
        "                      select(vec3f(64.0f / 255.0f), vec3f(128.0f / 255.0f), piece_3),\n"
        "                      piece_2);\n"
        "  return trunc(value * scale) * (1.0f / 255.0f) + offset;\n"
        "}\n\n";
  }
  if (uses_float24_depth_) {
    // Guest [0, 1] depth to float24 and back to the host [0, 0.5] range, as
    // SpirvShaderTranslator::PreClampedDepthTo20e4 and Depth20e4To32.
    bool round_to_nearest_even =
        modification.pixel.depth_stencil_mode == Modification::DepthStencilMode::kFloat24Rounding;
    out += fmt::format(
        "fn xe_depth_to_float24_host(depth: f32) -> f32 {{\n"
        "  let f32_bits = bitcast<u32>(depth) & 0x7FFFFFFFu;  // -0 as 0\n"
        "  let denormal_biased = insertBits(f32_bits, 1u, 23u, 9u) >>\n"
        "                        min(113u - (f32_bits >> 23u), 24u);\n"
        "  let normal_biased = f32_bits - (112u << 23u);\n"
        "  var biased = select(normal_biased, denormal_biased, f32_bits < 0x38800000u);\n"
        "{}"
        "  let f24 = extractBits(biased, 3u, 24u);\n"
        "  let f24_exponent = extractBits(f24, 20u, 4u);\n"
        "  let f24_mantissa = extractBits(f24, 0u, 20u);\n"
        "  let mantissa_msb = firstLeadingBit(f24_mantissa);\n"
        "  let mantissa_nonzero = f24_mantissa != 0u;\n"
        "  let denormal_exponent = select(0x{:08X}u, mantissa_msb - 19u, mantissa_nonzero);\n"
        "  let denormal_mantissa = select(0u, f24_mantissa << (20u - mantissa_msb), "
        "mantissa_nonzero);\n"
        "  let is_normal = f24_exponent != 0u;\n"
        "  let exponent = select(denormal_exponent, f24_exponent, is_normal);\n"
        "  let mantissa = select(denormal_mantissa, f24_mantissa, is_normal);\n"
        "  return bitcast<f32>(insertBits(mantissa, exponent + 111u, 20u, 8u) << 3u);\n"
        "}}\n\n",
        round_to_nearest_even ? "  biased = biased + 3u + extractBits(biased, 3u, 1u);\n" : "",
        uint32_t(-int32_t(111)));
  }
  if (uses_pwl_gamma_to_linear_) {
    // As SpirvShaderTranslator::PWLGammaToLinear.
    out +=
        "fn xe_pwl_gamma_to_linear(gamma_unclamped: f32) -> f32 {\n"
        "  let gamma = select(0.0f, min(gamma_unclamped, 1.0f), gamma_unclamped >= 0.0f);\n"
        "  let piece_3 = gamma >= 192.0f / 255.0f;\n"
        "  let piece_1 = gamma >= 64.0f / 255.0f;\n"
        "  let piece_2 = gamma >= 96.0f / 255.0f;\n"
        "  let scale = select(select(1.0f / 1024.0f, 2.0f / 1024.0f, piece_1),\n"
        "                     select(4.0f / 1024.0f, 8.0f / 1024.0f, piece_3), piece_2);\n"
        "  let offset = select(select(0.0f, -64.0f, piece_1), select(-256.0f, -1024.0f, piece_3),\n"
        "                      piece_2);\n"
        "  var value = gamma * (255.0f * 1024.0f) * scale + offset;\n"
        "  value = value + trunc(value * scale);\n"
        "  return value * (1.0f / 1023.0f);\n"
        "}\n\n";
  }

  out += fmt::format(
      "fn xe_endian_swap32(value: u32, endian: u32) -> u32 {{\n"
      "  var v = value;\n"
      "  if (endian == {}u || endian == {}u) {{\n"
      "    v = ((v >> 8u) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8u);\n"
      "  }}\n"
      "  if (endian == {}u || endian == {}u) {{\n"
      "    v = (v >> 16u) | (v << 16u);\n"
      "  }}\n"
      "  return v;\n"
      "}}\n\n",
      uint32_t(xenos::Endian::k8in16), uint32_t(xenos::Endian::k8in32),
      uint32_t(xenos::Endian::k8in32), uint32_t(xenos::Endian::k16in32));

  uint32_t interpolator_mask = GetModificationInterpolatorMask();
  if (is_vertex_shader()) {
    out += "struct XeVertexOutput {\n  @builtin(position) @invariant position: vec4f,\n";
    uint32_t location = 0;
    uint32_t interpolators_remaining = interpolator_mask;
    uint32_t interpolator_index;
    while (rex::bit_scan_forward(interpolators_remaining, &interpolator_index)) {
      interpolators_remaining &= ~(UINT32_C(1) << interpolator_index);
      bool centroid = (modification.vertex.interpolators_centroid >> interpolator_index) & 1;
      out +=
          fmt::format("  @location({}){} i{}: vec4f,\n", location++,
                      centroid ? " @interpolate(perspective, centroid)" : "", interpolator_index);
    }
    if (uses_point_coordinates_) {
      out += fmt::format("  @location({}) point_coordinates: vec2f,\n", location++);
    }
    if (clip_distance_count_) {
      out += fmt::format("  @builtin(clip_distances) clip_distances: array<f32, {}>,\n",
                         clip_distance_count_);
    }
    out += "}\n\n";
    out += "@vertex\nfn main(@builtin(vertex_index) xe_vertex_index: u32) -> XeVertexOutput {\n";
  } else {
    bool has_output = HasFragmentOutput();
    if (has_output) {
      out += "struct XeFragmentOutput {\n";
      for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
        if (current_shader().writes_color_target(i)) {
          out += fmt::format("  @location({}) c{}: vec4f,\n", i, i);
        }
      }
      if (HasFragmentDepthOutput()) {
        out += "  @builtin(frag_depth) depth: f32,\n";
      }
      if (IsAlphaToCoveragePossible()) {
        out += "  @builtin(sample_mask) sample_mask: u32,\n";
      }
      out += "}\n\n";
    }
    std::vector<std::string> inputs;
    if (uses_fragment_coordinates_) {
      inputs.push_back("@builtin(position) xe_frag_coord: vec4f");
    }
    if (uses_front_facing_) {
      inputs.push_back("@builtin(front_facing) xe_front_facing: bool");
    }
    if (IsSampleRate()) {
      inputs.push_back("@builtin(sample_index) xe_sample_index: u32");
    }
    uint32_t location = 0;
    uint32_t interpolators_remaining = interpolator_mask;
    uint32_t interpolator_index;
    while (rex::bit_scan_forward(interpolators_remaining, &interpolator_index)) {
      interpolators_remaining &= ~(UINT32_C(1) << interpolator_index);
      bool centroid = (modification.pixel.interpolators_centroid >> interpolator_index) & 1;
      inputs.push_back(fmt::format("@location({}){} xe_in_{}: vec4f", location++,
                                   centroid ? " @interpolate(perspective, centroid)" : "",
                                   interpolator_index));
    }
    if (modification.pixel.param_gen_point) {
      if (uses_point_coordinates_) {
        inputs.push_back(fmt::format("@location({}) xe_in_point_coordinates: vec2f", location));
      }
      ++location;
    }
    out += "@fragment\nfn main(";
    for (size_t i = 0; i < inputs.size(); ++i) {
      out += i ? ",\n        " : "";
      out += inputs[i];
    }
    out += has_output ? ") -> XeFragmentOutput {\n" : ") {\n";
  }
  return out;
}

void WgslShaderTranslator::StartVertexShaderInMain() {
  Modification modification = GetWgslShaderModification();
  Shader::HostVertexShaderType host_vertex_shader_type =
      modification.vertex.host_vertex_shader_type;

  // The edge flag isn't used for anything.
  if (current_shader().writes_point_size_edge_flag_kill_vertex() & 0b101) {
    has_point_size_edge_flag_kill_vertex_ = true;
    Line("var xe_psek = vec3f(-1.0f, 0.0f, 0.0f);");
  }
  if (host_vertex_shader_type == Shader::HostVertexShaderType::kPointListAsTriangleStrip &&
      modification.vertex.output_point_parameters) {
    uses_point_coordinates_ = true;
  }
  if (features_.clip_distances) {
    clip_distance_count_ = modification.GetVertexClipDistanceCount();
  }

  if (!register_count()) {
    return;
  }
  // The guest vertex index to r0.x.
  uses_shared_memory_ = true;
  Line("var xe_vi = xe_vertex_index;");
  Line("xe_vi = select(0u, xe_vi, xe_vi != xe_system.line_loop_closing_index);");
  if (host_vertex_shader_type == Shader::HostVertexShaderType::kPointListAsTriangleStrip) {
    // 4 host vertices per guest point.
    Line("xe_vi = xe_vi >> 2u;");
    OpenScope(fmt::format("if ((xe_system.flags & {}u) != 0u)",
                          uint32_t(kSysFlag_ComputeOrPrimitiveVertexIndexLoad)));
    Line(fmt::format("let xe_index_32bit = (xe_system.flags & {}u) != 0u;",
                     uint32_t(kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit)));
    Line(
        "let xe_index_address = xe_system.vertex_index_load_address + "
        "(xe_vi << select(1u, 2u, xe_index_32bit));");
    Line("var xe_index = xe_load_shared(i32(xe_index_address >> 2u));");
    OpenScope("if (!xe_index_32bit)");
    Line("xe_index = extractBits(xe_index, (xe_index_address & 2u) << 3u, 16u);");
    CloseScope();
    Line("xe_vi = xe_endian_swap32(xe_index, xe_system.vertex_index_endian);");
    CloseScope();
  } else {
    OpenScope(
        fmt::format("if ((xe_system.flags & {}u) != 0u)", uint32_t(kSysFlag_VertexIndexLoad)));
    Line("xe_vi = xe_load_shared(i32((xe_system.vertex_index_load_address >> 2u) + xe_vi));");
    CloseScope();
    Line("xe_vi = xe_endian_swap32(xe_vi, xe_system.vertex_index_endian);");
  }
  Line(fmt::format(
      "xe_r[0].x = f32(clamp((xe_vi + bitcast<u32>(xe_system.vertex_base_index)) & 0x{:X}u, "
      "xe_system.vertex_index_min, xe_system.vertex_index_max));",
      xenos::kVertexIndexMask));
}

void WgslShaderTranslator::CompleteVertexShaderInMain() {
  Modification modification = GetWgslShaderModification();
  Line(fmt::format("var xe_w = select(1.0f / xe_pos.w, xe_pos.w, (xe_system.flags & {}u) != 0u);",
                   uint32_t(kSysFlag_WNotReciprocal)));
  Line(fmt::format(
      "var xe_xyz = vec3f(select(xe_pos.xy, xe_pos.xy * xe_w, (xe_system.flags & {}u) != 0u), "
      "select(xe_pos.z, xe_pos.z * xe_w, (xe_system.flags & {}u) != 0u));",
      uint32_t(kSysFlag_XYDividedByW), uint32_t(kSysFlag_ZDividedByW)));
  for (uint32_t i = 0; i < clip_distance_count_; ++i) {
    Line(fmt::format(
        "xe_out.clip_distances[{}] = dot(vec4f(xe_xyz, xe_w), xe_system.user_clip_planes[{}]);", i,
        i));
  }
  // Guest to host viewport.
  Line("xe_xyz = xe_xyz * xe_system.ndc_scale + xe_system.ndc_offset * xe_w;");
  if (current_shader().writes_point_size_edge_flag_kill_vertex() & 0b100) {
    // Kill the primitive with a NaN W (no cull distances in WebGPU, so the
    // AND mode is approximated too).
    Line(
        "xe_w = select(xe_w, bitcast<f32>(0x7FC00000u), "
        "(bitcast<u32>(xe_psek.z) & 0x7FFFFFFFu) != 0u);");
  }
  if (modification.vertex.host_vertex_shader_type ==
      Shader::HostVertexShaderType::kPointListAsTriangleStrip) {
    // Top-left, bottom-left, top-right, bottom-right.
    Line("let xe_point_corner = (vec2u(xe_vertex_index) & vec2u(2u, 1u)) != vec2u(0u);");
    Line("var xe_point_diameter = xe_system.point_constant_diameter;");
    if (current_shader().writes_point_size_edge_flag_kill_vertex() & 0b001) {
      // The header writes -1, so a non-negative size was written by the shader.
      Line(
          "xe_point_diameter = select(xe_point_diameter, vec2f(xe_psek.x), "
          "xe_psek.x >= 0.0f);");
    }
    Line(
        "let xe_point_radius = xe_point_diameter * "
        "xe_system.point_screen_diameter_to_ndc_radius * xe_w;");
    Line(
        "xe_out.position = vec4f(xe_xyz.xy + select(-xe_point_radius, xe_point_radius, "
        "xe_point_corner), xe_xyz.z, xe_w);");
    if (uses_point_coordinates_) {
      Line("xe_out.point_coordinates = select(vec2f(0.0f), vec2f(1.0f), xe_point_corner);");
    }
  } else {
    Line("xe_out.position = vec4f(xe_xyz, xe_w);");
  }
}

void WgslShaderTranslator::StartPixelShaderInMain() {
  Modification modification = GetWgslShaderModification();
  uint32_t param_gen_interpolator = GetPsParamGenInterpolator();
  uint32_t interpolator_mask = GetModificationInterpolatorMask();
  for (uint32_t i = 0; i < std::min(register_count(), xenos::kMaxInterpolators); ++i) {
    if (i != param_gen_interpolator && (interpolator_mask & (UINT32_C(1) << i))) {
      Line(fmt::format("xe_r[{}] = xe_in_{};", i, i));
    }
  }
  if (param_gen_interpolator == UINT32_MAX) {
    return;
  }
  // PsParamGen: the position, with the sign bits telling the primitive type
  // and faceness.
  uses_fragment_coordinates_ = true;
  std::string scale_x =
      draw_resolution_scale_x_ > 1
          ? fmt::format(" * {}", FloatConst(1.0f / float(draw_resolution_scale_x_), 1))
          : std::string();
  std::string scale_y =
      draw_resolution_scale_y_ > 1
          ? fmt::format(" * {}", FloatConst(1.0f / float(draw_resolution_scale_y_), 1))
          : std::string();
  Line(fmt::format("var xe_pg_x = abs(floor(xe_frag_coord.x){});", scale_x));
  if (!modification.pixel.param_gen_point) {
    uses_front_facing_ = true;
    Line(
        fmt::format("xe_pg_x = select(bitcast<f32>(bitcast<u32>(xe_pg_x) ^ 0x80000000u), xe_pg_x, "
                    "(xe_system.flags & {}u) == 0u || xe_front_facing);",
                    uint32_t(kSysFlag_PrimitivePolygonal)));
  }
  Line(fmt::format("var xe_pg_y = abs(floor(xe_frag_coord.y){});", scale_y));
  if (modification.pixel.param_gen_point) {
    Line("xe_pg_y = bitcast<f32>(bitcast<u32>(xe_pg_y) ^ 0x80000000u);");
    uses_point_coordinates_ = true;
    Line(
        "let xe_pg_zw = select(vec2f(0.0f), min(xe_in_point_coordinates, vec2f(1.0f)), "
        "xe_in_point_coordinates >= vec2f(0.0f));");
  } else {
    Line(
        fmt::format("let xe_pg_zw = vec2f(bitcast<f32>(select(0u, 0x80000000u, "
                    "(xe_system.flags & {}u) != 0u)), 0.0f);",
                    uint32_t(kSysFlag_PrimitiveLine)));
  }
  Line(fmt::format("xe_r[{}] = vec4f(xe_pg_x, xe_pg_y, xe_pg_zw);", param_gen_interpolator));
}

void WgslShaderTranslator::CompletePixelShaderInMain() {
  // The host render target path of the SPIR-V translator (no fragment shader
  // interlock in WebGPU). Kills are already done: WGSL's discard demotes.
  uint32_t color_targets_written = current_shader().writes_color_targets();

  if (IsAlphaToCoveragePossible()) {
    Line("xe_out.sample_mask = 0xFFFFFFFFu;");

    // Alpha test on render target 0's alpha.
    Line(fmt::format("let xe_alpha_test_function = extractBits(xe_system.flags, {}u, 3u);",
                     uint32_t(kSysFlag_AlphaPassIfLess_Shift)));
    OpenScope(fmt::format("if (xe_alpha_test_function != {}u)",
                          uint32_t(xenos::CompareFunction::kAlways)));
    Line("let xe_alpha = xe_out.c0.w;");
    Line("let xe_alpha_reference = xe_system.alpha_test_reference;");
    // Not equal is unordered (passes for NaN), the others ordered.
    Line(
        fmt::format("let xe_alpha_test_passed = select("
                    "(xe_alpha < xe_alpha_reference && (xe_alpha_test_function & 1u) != 0u) || "
                    "(xe_alpha == xe_alpha_reference && (xe_alpha_test_function & 2u) != 0u) || "
                    "(xe_alpha > xe_alpha_reference && (xe_alpha_test_function & 4u) != 0u), "
                    "xe_alpha != xe_alpha_reference, xe_alpha_test_function == {}u);",
                    uint32_t(xenos::CompareFunction::kNotEqual)));
    OpenScope("if (!xe_alpha_test_passed)");
    Line("discard;");
    CloseScope();
    CloseScope();

    // Alpha to coverage with the guest's dithering: per sample, covered if
    // alpha >= base - offset * scale, the offset (0-3) chosen by the pixel's
    // position in a 2x2 quad. WebGPU has no 2x MSAA, so guest 2x uses host 4x
    // samples 0 and 3 (as the Vulkan path without native 2x).
    uses_fragment_coordinates_ = true;
    OpenScope("if (xe_system.alpha_to_mask != 0u)");
    Line("let xe_a2c_alpha = xe_out.c0.w;");
    Line("let xe_a2c_position = vec2u(xe_frag_coord.xy);");
    Line(
        "let xe_a2c_offset = f32(extractBits(xe_system.alpha_to_mask, "
        "((xe_a2c_position.x & 1u) | ((xe_a2c_position.y & 1u) << 1u)) << 1u, 2u));");
    Line("var xe_a2c_mask = 0u;");
    auto sample = [&](uint32_t host_sample, float threshold_base, float threshold_offset_scale) {
      Line(fmt::format("xe_a2c_mask |= select(0u, {}u, xe_a2c_alpha >= {} - xe_a2c_offset * {});",
                       UINT32_C(1) << host_sample, FloatConst(threshold_base, 1),
                       FloatConst(threshold_offset_scale, 1)));
    };
    OpenScope(fmt::format("switch extractBits(xe_system.flags, {}u, 2u)",
                          uint32_t(kSysFlag_MsaaSamples_Shift)));
    OpenScope(fmt::format("case {}u:", uint32_t(xenos::MsaaSamples::k2X)));
    sample(0, 0.5f, 1.0f / 8.0f);
    sample(3, 1.0f, 1.0f / 8.0f);
    CloseScope();
    OpenScope(fmt::format("case {}u:", uint32_t(xenos::MsaaSamples::k4X)));
    sample(0, 0.75f, 1.0f / 16.0f);
    sample(2, 0.25f, 1.0f / 16.0f);
    sample(1, 0.5f, 1.0f / 16.0f);
    sample(3, 1.0f, 1.0f / 16.0f);
    CloseScope();
    OpenScope("default:");
    sample(0, 1.0f, 1.0f / 4.0f);
    CloseScope();
    CloseScope();
    OpenScope("if (xe_a2c_mask == 0u)");
    Line("discard;");
    CloseScope();
    Line("xe_out.sample_mask = xe_a2c_mask;");
    CloseScope();
  }

  // Color exponent bias, then gamma (before blending rather than after as on
  // the Xbox 360, one of the host render target path's blending issues; done
  // in the shader as in the Vulkan and D3D12 host render target paths).
  uint32_t color_targets_remaining = color_targets_written;
  uint32_t color_target_index;
  while (rex::bit_scan_forward(color_targets_remaining, &color_target_index)) {
    color_targets_remaining &= ~(UINT32_C(1) << color_target_index);
    std::string color = fmt::format("xe_out.c{}", color_target_index);
    Line(fmt::format("{} = {} * xe_system.color_exp_bias[{}];", color, color, color_target_index));
    uses_linear_to_pwl_gamma_ = true;
    OpenScope(fmt::format("if ((xe_system.flags & {}u) != 0u)",
                          uint32_t(kSysFlag_ConvertColor0ToGamma) << color_target_index));
    Line(fmt::format("{} = vec4f(xe_linear_to_pwl_gamma({}.rgb), {}.a);", color, color, color));
    CloseScope();
  }

  if (IsWritingFloat24Depth()) {
    // oDepth is already in guest [0, 1]; otherwise the host depth in [0, 0.5]
    // is doubled back, per sample.
    uses_float24_depth_ = true;
    std::string depth = "xe_out.depth";
    if (!current_shader().writes_depth()) {
      uses_fragment_coordinates_ = true;
      Line("_ = xe_sample_index;");
      depth = Let("select(0.0f, min(xe_frag_coord.z * 2.0f, 1.0f), xe_frag_coord.z >= 0.0f)");
    }
    Line(fmt::format("xe_out.depth = xe_depth_to_float24_host({});", depth));
  }
}

void WgslShaderTranslator::ProcessLabel(uint32_t cf_index) {
  if (cf_index == 0) {
    return;
  }
  CloseExecConditionals();
  if (!case_terminated_) {
    // Continue from the previous case.
    Line(fmt::format("xe_pc = {}i;", cf_index));
  }
  CloseScope();
  OpenScope(fmt::format("case {}i:", cf_index));
  case_terminated_ = false;
}

void WgslShaderTranslator::BranchTo(int32_t pc) {
  Line(fmt::format("xe_pc = {}i;", pc));
  Line("break;");
  if (cf_exec_bool_constant_or_predicate_ == kCfExecBoolConstantNone &&
      !cf_instruction_predicate_open_) {
    case_terminated_ = true;
  }
}

std::string WgslShaderTranslator::BoolConstantCondition(uint32_t bool_constant_index) const {
  return fmt::format("(xe_bool_loop.b[{}][{}] & {}u) != 0u", bool_constant_index >> 7,
                     (bool_constant_index >> 5) & 3, uint32_t(1) << (bool_constant_index & 31));
}

std::string WgslShaderTranslator::LoopConstant(uint32_t loop_constant_index) const {
  return fmt::format("xe_bool_loop.l[{}][{}]", loop_constant_index >> 2, loop_constant_index & 3);
}

void WgslShaderTranslator::ProcessExecInstructionBegin(const ParsedExecInstruction& instr) {
  UpdateExecConditionals(instr.type, instr.bool_constant_index, instr.condition);
}

void WgslShaderTranslator::ProcessExecInstructionEnd(const ParsedExecInstruction& instr) {
  if (instr.is_end) {
    CloseInstructionPredication();
    BranchTo(-1);
  }
  UpdateExecConditionals(instr.type, instr.bool_constant_index, instr.condition);
}

void WgslShaderTranslator::ProcessLoopStartInstruction(const ParsedLoopStartInstruction& instr) {
  // loop il<idx>, L<idx> - loop with loop data il<idx>, end @ L<idx>
  CloseExecConditionals();
  uses_bool_loop_constants_ = true;
  std::string loop_constant = Let(LoopConstant(instr.loop_constant_index));
  std::string loop_count = Let(loop_constant + " & 0xFFu");
  // Push the count and the aL starting value (aL kept for repeat).
  Line(fmt::format("xe_lc = vec4u({}, xe_lc.xyz);", loop_count));
  if (instr.is_repeat) {
    Line("xe_aL = vec4i(xe_aL.x, xe_aL.xyz);");
  } else {
    Line(fmt::format("xe_aL = vec4i(i32(({} >> 8u) & 0xFFu), xe_aL.xyz);", loop_constant));
  }
  // Skip the loop if the count is 0.
  OpenScope(fmt::format("if ({} == 0u)", loop_count));
  // Not terminating the case: the scope is conditional.
  Line(fmt::format("xe_pc = {}i;", instr.loop_skip_address));
  Line("break;");
  CloseScope();
}

void WgslShaderTranslator::ProcessLoopEndInstruction(const ParsedLoopEndInstruction& instr) {
  // endloop il<idx>, L<idx> - end loop with loop data il<idx>, start @ L<idx>
  CloseExecConditionals();
  uses_bool_loop_constants_ = true;
  std::string loop_count = Let("xe_lc.x - 1u");
  bool break_is_true = instr.is_predicated_break && instr.predicate_condition;
  std::string condition = fmt::format("{} {} 0u", loop_count, break_is_true ? "==" : "!=");
  if (instr.is_predicated_break) {
    condition = fmt::format("({}) {} xe_p", condition, instr.predicate_condition ? "||" : "&&");
  }
  std::string continue_condition = break_is_true ? fmt::format("!({})", condition) : condition;
  OpenScope(fmt::format("if ({})", continue_condition));
  Line(fmt::format("xe_lc.x = {};", loop_count));
  Line(fmt::format("xe_aL.x = xe_aL.x + extractBits(bitcast<i32>({}), 16u, 8u);",
                   LoopConstant(instr.loop_constant_index)));
  Line(fmt::format("xe_pc = {}i;", instr.loop_body_address));
  Line("break;");
  CloseScope();
  // Leaving the loop: pop the stacks.
  Line("xe_lc = vec4u(xe_lc.yzw, 0u);");
  Line("xe_aL = vec4i(xe_aL.yzw, 0i);");
}

void WgslShaderTranslator::ProcessJumpInstruction(const ParsedJumpInstruction& instr) {
  ParsedExecInstruction::Type type;
  if (instr.type == ParsedJumpInstruction::Type::kConditional) {
    type = ParsedExecInstruction::Type::kConditional;
  } else if (instr.type == ParsedJumpInstruction::Type::kPredicated) {
    type = ParsedExecInstruction::Type::kPredicated;
  } else {
    type = ParsedExecInstruction::Type::kUnconditional;
  }
  UpdateExecConditionals(type, instr.bool_constant_index, instr.condition);
  CloseInstructionPredication();
  BranchTo(int32_t(instr.target_address));
}

void WgslShaderTranslator::ProcessAllocInstruction(const ParsedAllocInstruction& instr,
                                                   uint8_t export_eM) {
  if ((export_eM ||
       (instr.type == ucode::AllocType::kMemory && current_shader().memexport_eM_written())) &&
      !reported_memexport_) {
    reported_memexport_ = true;
    EmitTranslationError("Memory export is not implemented in the WGSL translator yet", false);
  }
}

void WgslShaderTranslator::UpdateExecConditionals(ParsedExecInstruction::Type type,
                                                  uint32_t bool_constant_index, bool condition) {
  // Merge consecutive execs with the same condition (as long as the predicate
  // isn't modified in between for predicated ones).
  bool is_open = cf_exec_bool_constant_or_predicate_ != kCfExecBoolConstantNone;
  if (type == ParsedExecInstruction::Type::kConditional) {
    if (is_open && cf_exec_bool_constant_or_predicate_ == bool_constant_index &&
        cf_exec_condition_ == condition) {
      return;
    }
  } else if (type == ParsedExecInstruction::Type::kPredicated) {
    if (!cf_exec_predicate_written_ && is_open &&
        cf_exec_bool_constant_or_predicate_ == kCfExecBoolConstantPredicate &&
        cf_exec_condition_ == condition) {
      return;
    }
  } else {
    if (!is_open) {
      return;
    }
  }
  CloseExecConditionals();
  if (type == ParsedExecInstruction::Type::kUnconditional) {
    return;
  }
  std::string condition_expr;
  if (type == ParsedExecInstruction::Type::kConditional) {
    uses_bool_loop_constants_ = true;
    condition_expr = BoolConstantCondition(bool_constant_index);
    cf_exec_bool_constant_or_predicate_ = bool_constant_index;
  } else {
    condition_expr = "xe_p";
    cf_exec_bool_constant_or_predicate_ = kCfExecBoolConstantPredicate;
  }
  cf_exec_condition_ = condition;
  OpenScope(condition ? fmt::format("if ({})", condition_expr)
                      : fmt::format("if (!({}))", condition_expr));
}

void WgslShaderTranslator::UpdateInstructionPredication(bool predicated, bool condition) {
  if (!predicated) {
    CloseInstructionPredication();
    return;
  }
  if (cf_instruction_predicate_open_) {
    if (cf_instruction_predicate_condition_ == condition) {
      return;
    }
    CloseInstructionPredication();
  }
  // Already inside an exec with the same predicate condition.
  if (!cf_exec_predicate_written_ &&
      cf_exec_bool_constant_or_predicate_ == kCfExecBoolConstantPredicate &&
      cf_exec_condition_ == condition) {
    return;
  }
  cf_instruction_predicate_condition_ = condition;
  cf_instruction_predicate_open_ = true;
  OpenScope(condition ? "if (xe_p)" : "if (!xe_p)");
}

void WgslShaderTranslator::CloseInstructionPredication() {
  if (!cf_instruction_predicate_open_) {
    return;
  }
  CloseScope();
  cf_instruction_predicate_open_ = false;
}

void WgslShaderTranslator::CloseExecConditionals() {
  CloseInstructionPredication();
  if (cf_exec_bool_constant_or_predicate_ != kCfExecBoolConstantNone) {
    CloseScope();
    cf_exec_bool_constant_or_predicate_ = kCfExecBoolConstantNone;
  }
  cf_exec_predicate_written_ = false;
}

std::string WgslShaderTranslator::GetStorageAddressingIndex(
    InstructionStorageAddressingMode addressing_mode, uint32_t storage_index,
    bool is_float_constant) {
  const Shader::ConstantRegisterMap& constant_register_map =
      current_shader().constant_register_map();
  std::string base;
  switch (addressing_mode) {
    case InstructionStorageAddressingMode::kAbsolute: {
      uint32_t static_storage_index = storage_index;
      if (is_float_constant) {
        static_storage_index = constant_register_map.GetPackedFloatConstantIndex(storage_index);
        assert_true(static_storage_index != UINT32_MAX);
        if (static_storage_index == UINT32_MAX) {
          static_storage_index = 0;
        }
      }
      return fmt::format("{}i", static_storage_index);
    }
    case InstructionStorageAddressingMode::kAddressRegisterRelative:
      base = "xe_a0";
      break;
    case InstructionStorageAddressingMode::kLoopRelative:
      base = "xe_aL.x";
      break;
  }
  assert_true(!is_float_constant || constant_register_map.float_dynamic_addressing);
  return storage_index ? fmt::format("({} + {}i)", base, storage_index) : base;
}

std::string WgslShaderTranslator::LoadOperandStorage(const InstructionOperand& operand) {
  std::string index =
      GetStorageAddressingIndex(operand.storage_addressing_mode, operand.storage_index,
                                operand.storage_source == InstructionStorageSource::kConstantFloat);
  switch (operand.storage_source) {
    case InstructionStorageSource::kRegister:
      assert_not_zero(register_count());
      return Let(fmt::format("xe_r[{}]", index));
    case InstructionStorageSource::kConstantFloat:
      uses_float_constants_ = true;
      return Let(fmt::format("xe_float.c[{}]", index));
    default:
      assert_unhandled_case(operand.storage_source);
      EmitTranslationError("Unsupported operand storage source");
      return Let("vec4f(0.0f)");
  }
}

WgslShaderTranslator::Value WgslShaderTranslator::ApplyOperandModifiers(
    const Value& value, const InstructionOperand& original_operand, bool invert_negate,
    bool force_absolute) {
  std::string expr = value.expr;
  bool modified = false;
  if (original_operand.is_absolute_value || force_absolute) {
    expr = fmt::format("abs({})", expr);
    modified = true;
  }
  if (original_operand.is_negated != invert_negate) {
    expr = fmt::format("-{}", expr);
    modified = true;
  }
  return modified ? Value{Let(expr), value.n} : value;
}

WgslShaderTranslator::Value WgslShaderTranslator::GetUnmodifiedOperandComponents(
    const std::string& operand_storage, const InstructionOperand& original_operand,
    uint32_t components) {
  assert_true(components && components <= 0b1111);
  if (components == 0b1111 && original_operand.IsStandardSwizzle()) {
    return Value{operand_storage, 4};
  }
  std::string swizzle;
  uint32_t components_remaining = components;
  uint32_t component_index;
  while (rex::bit_scan_forward(components_remaining, &component_index)) {
    components_remaining &= ~(uint32_t(1) << component_index);
    swizzle += kSwizzleComponents[uint32_t(original_operand.GetComponent(component_index)) -
                                  uint32_t(SwizzleSource::kX)];
  }
  return Value{fmt::format("{}.{}", operand_storage, swizzle), uint32_t(swizzle.size())};
}

void WgslShaderTranslator::GetOperandScalarXY(const std::string& operand_storage,
                                              const InstructionOperand& original_operand,
                                              Value& a_out, Value& b_out) {
  a_out = GetOperandComponents(operand_storage, original_operand, 0b0001);
  b_out = original_operand.GetComponent(0) != original_operand.GetComponent(1)
              ? GetOperandComponents(operand_storage, original_operand, 0b0010)
              : a_out;
}

WgslShaderTranslator::Value WgslShaderTranslator::ZeroIfAnyOperandIsZero(const Value& product,
                                                                         const Value& a,
                                                                         const Value& b,
                                                                         uint32_t components) {
  assert_true(a.n == product.n && b.n == product.n);
  components &= (uint32_t(1) << product.n) - 1;
  if (!components) {
    return product;
  }
  // NMin(|a|, |b|) == 0 in SPIR-V; the same as either being +-0.
  std::string condition;
  if (product.n == 1) {
    condition = fmt::format("{} == 0.0f || {} == 0.0f", a.expr, b.expr);
  } else if (components == (uint32_t(1) << product.n) - 1) {
    condition = fmt::format("({} == {}) | ({} == {})", a.expr, FloatConst(0.0f, a.n), b.expr,
                            FloatConst(0.0f, b.n));
  } else {
    condition = fmt::format("vec{}<bool>(", product.n);
    for (uint32_t i = 0; i < product.n; ++i) {
      condition += i ? ", " : "";
      if (components & (uint32_t(1) << i)) {
        condition += fmt::format("{} == 0.0f || {} == 0.0f", Component(a, i), Component(b, i));
      } else {
        condition += "false";
      }
    }
    condition += ")";
  }
  return Value{
      Let(fmt::format("select({}, {}, {})", product.expr, FloatConst(0.0f, product.n), condition)),
      product.n};
}

void WgslShaderTranslator::KillPixel(const std::string& condition) {
  // A vertex shader can't discard.
  if (!is_pixel_shader()) {
    return;
  }
  // WGSL's discard demotes to a helper invocation, so derivatives keep working.
  OpenScope(fmt::format("if ({})", condition));
  Line("discard;");
  CloseScope();
}

void WgslShaderTranslator::StoreResult(const InstructionResult& result, const Value& value) {
  uint32_t used_write_mask = result.GetUsedWriteMask();
  if (!used_write_mask) {
    return;
  }
  bool is_clamped = result.is_clamped;

  std::string target;
  switch (result.storage_target) {
    case InstructionStorageTarget::kNone:
      break;
    case InstructionStorageTarget::kRegister:
      if (register_count()) {
        target = fmt::format("xe_r[{}]", GetStorageAddressingIndex(result.storage_addressing_mode,
                                                                   result.storage_index));
      }
      break;
    case InstructionStorageTarget::kInterpolator:
      assert_true(is_vertex_shader());
      if (GetModificationInterpolatorMask() & (UINT32_C(1) << result.storage_index)) {
        target = fmt::format("xe_out.i{}", result.storage_index);
      }
      break;
    case InstructionStorageTarget::kPosition:
      assert_true(is_vertex_shader());
      target = "xe_pos";
      break;
    case InstructionStorageTarget::kPointSizeEdgeFlagKillVertex:
      assert_true(is_vertex_shader());
      if (has_point_size_edge_flag_kill_vertex_) {
        target = "xe_psek";
      }
      break;
    case InstructionStorageTarget::kColor:
      assert_true(is_pixel_shader());
      assert_true(current_shader().writes_color_target(result.storage_index));
      target = fmt::format("xe_out.c{}", result.storage_index);
      break;
    case InstructionStorageTarget::kDepth:
      assert_true(is_pixel_shader());
      assert_true(used_write_mask == 0b0001);
      assert_true(current_shader().writes_depth());
      target = "xe_out.depth";
      is_clamped = true;
      break;
    case InstructionStorageTarget::kExportAddress:
    case InstructionStorageTarget::kExportData:
      // Memory export comes later (reported in ProcessAllocInstruction).
      break;
    default:
      assert_unhandled_case(result.storage_target);
      break;
  }
  if (target.empty()) {
    return;
  }

  uint32_t constant_values;
  uint32_t constant_components = result.GetUsedConstantComponents(constant_values);
  if (!value) {
    constant_components = used_write_mask;
  }
  uint32_t non_constant_components = used_write_mask & ~constant_components;

  std::string value_expr = value.expr;
  if (is_clamped && non_constant_components) {
    // NClamp to [0, 1], NaN to 0.
    value_expr =
        Let(fmt::format("select({}, min({}, {}), {} >= {})", FloatConst(0.0f, value.n), value.expr,
                        FloatConst(1.0f, value.n), value.expr, FloatConst(0.0f, value.n)));
  }
  Value stored_value{value_expr, value.n};

  // The value has as many components as the result components used, packed;
  // map them to the swizzled target components.
  uint32_t used_result_components = result.GetUsedResultComponents();
  uint32_t result_unswizzled_value_components[4] = {};
  if (value.n > 1) {
    uint32_t value_component = 0;
    uint32_t used_result_components_remaining = used_result_components;
    uint32_t result_component;
    while (rex::bit_scan_forward(used_result_components_remaining, &result_component)) {
      used_result_components_remaining &= ~(uint32_t(1) << result_component);
      result_unswizzled_value_components[result_component] =
          std::min(value_component++, value.n - 1);
    }
  }
  std::string sources[4];
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(used_write_mask & (1 << i))) {
      continue;
    }
    if (constant_components & (1 << i)) {
      sources[i] = FloatConst((constant_values & (1 << i)) ? 1.0f : 0.0f, 1);
      continue;
    }
    SwizzleSource swizzle = result.components[i];
    assert_true(swizzle >= SwizzleSource::kX && swizzle <= SwizzleSource::kW);
    sources[i] = Component(
        stored_value,
        result_unswizzled_value_components[uint32_t(swizzle) - uint32_t(SwizzleSource::kX)]);
  }

  uint32_t target_num_components =
      GetInstructionStorageTargetUsedComponentCount(result.storage_target);
  uint32_t target_component_mask = (uint32_t(1) << target_num_components) - 1;
  assert_zero(used_write_mask & ~target_component_mask);
  if (target_num_components == 1) {
    Line(fmt::format("{} = {};", target, sources[0]));
  } else if (used_write_mask == target_component_mask) {
    std::string constructor = FloatType(target_num_components) + "(";
    for (uint32_t i = 0; i < target_num_components; ++i) {
      constructor += i ? ", " : "";
      constructor += sources[i];
    }
    Line(fmt::format("{} = {});", target, constructor));
  } else {
    for (uint32_t i = 0; i < target_num_components; ++i) {
      if (used_write_mask & (1 << i)) {
        Line(fmt::format("{}.{} = {};", target, kSwizzleComponents[i], sources[i]));
      }
    }
  }

  if (result.storage_target == InstructionStorageTarget::kPointSizeEdgeFlagKillVertex &&
      (used_write_mask & 0b001)) {
    // Clamp the point size (non-negative, so as integers).
    Line(
        "xe_psek.x = bitcast<f32>(min(max(bitcast<i32>(xe_psek.x), "
        "bitcast<i32>(xe_system.point_vertex_diameter_min)), "
        "bitcast<i32>(xe_system.point_vertex_diameter_max)));");
  }
}

void WgslShaderTranslator::PostTranslation() {
  Shader::Translation& translation = current_translation();
  if (!translation.is_valid()) {
    return;
  }
  auto* wgsl_shader = dynamic_cast<WgslShader*>(&translation.shader());
  if (!wgsl_shader ||
      wgsl_shader->bindings_setup_entered_.test_and_set(std::memory_order_relaxed)) {
    return;
  }
  wgsl_shader->texture_bindings_ = texture_bindings_;
  wgsl_shader->sampler_bindings_ = sampler_bindings_;
  wgsl_shader->used_texture_mask_ = 0;
  for (const TextureBinding& binding : texture_bindings_) {
    wgsl_shader->used_texture_mask_ |= UINT32_C(1) << binding.fetch_constant;
  }
}

}  // namespace rex::graphics
