/**
 * @file        graphics/pipeline/shader/wgsl_translator_alu.cpp
 * @brief       Xenos microcode to WGSL: ALU instructions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/pipeline/shader/wgsl_translator.h>

#include <fmt/format.h>

#include <rex/assert.h>
#include <rex/math.h>

namespace rex::graphics {

// Semantics follow SpirvShaderTranslator: Xenos 0 * anything = 0 where the
// operands differ, max/min as selects (so a NaN first operand gives the second),
// and the "c"/"f" variants of log, rcp and rsq clamping infinities.

void WgslShaderTranslator::ProcessAluInstruction(
    const ParsedAluInstruction& instr, uint8_t /*memexport_eM_potentially_written_before*/) {
  if (instr.IsNop()) {
    // Don't even disassemble or update predication.
    return;
  }

  UpdateInstructionPredication(instr.is_predicated, instr.predicate_condition);

  // Both operations read the operands before either result is written.
  bool predicate_written_vector = false;
  Value vector_result = ProcessVectorAluOperation(instr, predicate_written_vector);
  bool predicate_written_scalar = false;
  Value scalar_result = ProcessScalarAluOperation(instr, predicate_written_scalar);
  if (scalar_result) {
    Line(fmt::format("xe_ps = {};", scalar_result.expr));
  } else if (instr.scalar_result.GetUsedWriteMask()) {
    // retain_prev, written to the result.
    scalar_result = Value{Let("xe_ps"), 1};
  }

  StoreResult(instr.vector_and_constant_result, vector_result);
  StoreResult(instr.scalar_result, scalar_result);

  if (predicate_written_vector || predicate_written_scalar) {
    cf_exec_predicate_written_ = true;
    CloseInstructionPredication();
  }
}

WgslShaderTranslator::Value WgslShaderTranslator::ProcessVectorAluOperation(
    const ParsedAluInstruction& instr, bool& predicate_written) {
  predicate_written = false;

  uint32_t used_result_components = instr.vector_and_constant_result.GetUsedResultComponents();
  if (!used_result_components &&
      !ucode::GetAluVectorOpcodeInfo(instr.vector_opcode).changed_state) {
    return Value();
  }
  uint32_t used_result_component_count = rex::bit_count(used_result_components);

  // cube takes the first operand twice, so it's loaded once.
  uint32_t operand_count = instr.vector_opcode == ucode::AluVectorOpcode::kCube
                               ? 1
                               : uint32_t(instr.vector_operand_count);
  std::string operand_storage[3];
  for (uint32_t i = 0; i < operand_count; ++i) {
    operand_storage[i] = LoadOperandStorage(instr.vector_operands[i]);
  }
  auto operand = [&](uint32_t i, uint32_t components) {
    return GetOperandComponents(operand_storage[i], instr.vector_operands[i], components);
  };
  auto let_value = [&](const std::string& expr, uint32_t n) { return Value{Let(expr), n}; };
  auto zero = [](uint32_t n) { return FloatConst(0.0f, n); };
  auto one = [](uint32_t n) { return FloatConst(1.0f, n); };

  // Comparison for the opcodes that have one.
  const char* comparison = nullptr;
  switch (instr.vector_opcode) {
    case ucode::AluVectorOpcode::kMax:
    case ucode::AluVectorOpcode::kMaxA:
    case ucode::AluVectorOpcode::kSge:
    case ucode::AluVectorOpcode::kCndGe:
    case ucode::AluVectorOpcode::kSetpGePush:
    case ucode::AluVectorOpcode::kKillGe:
      comparison = ">=";
      break;
    case ucode::AluVectorOpcode::kMin:
      comparison = "<";
      break;
    case ucode::AluVectorOpcode::kSeq:
    case ucode::AluVectorOpcode::kCndEq:
    case ucode::AluVectorOpcode::kSetpEqPush:
    case ucode::AluVectorOpcode::kKillEq:
      comparison = "==";
      break;
    case ucode::AluVectorOpcode::kSgt:
    case ucode::AluVectorOpcode::kCndGt:
    case ucode::AluVectorOpcode::kSetpGtPush:
    case ucode::AluVectorOpcode::kKillGt:
      comparison = ">";
      break;
    case ucode::AluVectorOpcode::kSne:
    case ucode::AluVectorOpcode::kSetpNePush:
    case ucode::AluVectorOpcode::kKillNe:
      // Unordered: true for NaN.
      comparison = "!=";
      break;
    default:
      break;
  }

  switch (instr.vector_opcode) {
    case ucode::AluVectorOpcode::kAdd: {
      Value a = operand(0, used_result_components), b = operand(1, used_result_components);
      return let_value(fmt::format("{} + {}", a.expr, b.expr), a.n);
    }
    case ucode::AluVectorOpcode::kMul:
    case ucode::AluVectorOpcode::kMad: {
      Value a = operand(0, used_result_components), b = operand(1, used_result_components);
      Value result = let_value(fmt::format("{} * {}", a.expr, b.expr), a.n);
      // Where the operands are different, 0 * anything = 0.
      uint32_t multiplicands_different =
          used_result_components &
          ~instr.vector_operands[0].GetIdenticalComponents(instr.vector_operands[1]);
      uint32_t different_value_components = 0;
      uint32_t components_remaining = used_result_components;
      uint32_t component;
      for (uint32_t i = 0; rex::bit_scan_forward(components_remaining, &component); ++i) {
        components_remaining &= ~(uint32_t(1) << component);
        if (multiplicands_different & (uint32_t(1) << component)) {
          different_value_components |= uint32_t(1) << i;
        }
      }
      result = ZeroIfAnyOperandIsZero(result, a, b, different_value_components);
      if (instr.vector_opcode == ucode::AluVectorOpcode::kMad) {
        Value c = operand(2, used_result_components);
        result = let_value(fmt::format("{} + {}", result.expr, c.expr), result.n);
      }
      return result;
    }

    case ucode::AluVectorOpcode::kMax:
    case ucode::AluVectorOpcode::kMin:
    case ucode::AluVectorOpcode::kMaxA: {
      if (instr.vector_opcode == ucode::AluVectorOpcode::kMaxA) {
        // a0 = clamp(floor(src0.w + 0.5), -256, 255), even without a result.
        Value w = operand(0, 0b1000);
        Line(fmt::format("xe_a0 = i32(clamp(floor({} + 0.5f), -256.0f, 255.0f));", w.expr));
      }
      if (!used_result_components) {
        return Value();
      }
      Value a = operand(0, used_result_components);
      uint32_t identical =
          instr.vector_operands[0].GetIdenticalComponents(instr.vector_operands[1]) &
          used_result_components;
      if (identical == used_result_components) {
        return a;
      }
      Value b = operand(1, used_result_components);
      return let_value(
          fmt::format("select({}, {}, {} {} {})", b.expr, a.expr, a.expr, comparison, b.expr), a.n);
    }

    case ucode::AluVectorOpcode::kSeq:
    case ucode::AluVectorOpcode::kSgt:
    case ucode::AluVectorOpcode::kSge:
    case ucode::AluVectorOpcode::kSne: {
      Value a = operand(0, used_result_components), b = operand(1, used_result_components);
      return let_value(
          fmt::format("select({}, {}, {} {} {})", zero(a.n), one(a.n), a.expr, comparison, b.expr),
          a.n);
    }

    case ucode::AluVectorOpcode::kFrc:
    case ucode::AluVectorOpcode::kTrunc:
    case ucode::AluVectorOpcode::kFloor: {
      const char* function = instr.vector_opcode == ucode::AluVectorOpcode::kFrc     ? "fract"
                             : instr.vector_opcode == ucode::AluVectorOpcode::kTrunc ? "trunc"
                                                                                     : "floor";
      Value a = operand(0, used_result_components);
      return let_value(fmt::format("{}({})", function, a.expr), a.n);
    }

    case ucode::AluVectorOpcode::kCndEq:
    case ucode::AluVectorOpcode::kCndGe:
    case ucode::AluVectorOpcode::kCndGt: {
      Value a = operand(0, used_result_components), b = operand(1, used_result_components),
            c = operand(2, used_result_components);
      return let_value(
          fmt::format("select({}, {}, {} {} {})", c.expr, b.expr, a.expr, comparison, zero(a.n)),
          a.n);
    }

    case ucode::AluVectorOpcode::kDp4:
    case ucode::AluVectorOpcode::kDp3:
    case ucode::AluVectorOpcode::kDp2Add: {
      uint32_t component_count = instr.vector_opcode == ucode::AluVectorOpcode::kDp4   ? 4
                                 : instr.vector_opcode == ucode::AluVectorOpcode::kDp3 ? 3
                                                                                       : 2;
      uint32_t component_mask = (uint32_t(1) << component_count) - 1;
      Value a = operand(0, component_mask), b = operand(1, component_mask);
      uint32_t different = component_mask & ~instr.vector_operands[0].GetIdenticalComponents(
                                                instr.vector_operands[1]);
      // Products in order, each with the 0 * anything rule where the operands
      // differ, summed left to right as in the SPIR-V translator.
      std::string result;
      for (uint32_t i = 0; i < component_count; ++i) {
        Value a_i{Component(a, i), 1}, b_i{Component(b, i), 1};
        Value product = let_value(fmt::format("{} * {}", a_i.expr, b_i.expr), 1);
        if (different & (uint32_t(1) << i)) {
          product = ZeroIfAnyOperandIsZero(product, a_i, b_i, 0b1);
        }
        result = i ? Let(fmt::format("{} + {}", result, product.expr)) : product.expr;
      }
      if (instr.vector_opcode == ucode::AluVectorOpcode::kDp2Add) {
        Value c = operand(2, 0b0001);
        result = Let(fmt::format("{} + {}", result, c.expr));
      }
      return Value{result, 1};
    }

    case ucode::AluVectorOpcode::kCube: {
      // The operand is src0.zzxy-like: o0 = z, o1 = w, o2 = x of the swizzle
      // (operand[i] = (x, z, w)[(i + 1) % 3] in the SPIR-V translator).
      Value v = operand(0, 0b1101);
      std::string o[3] = {Component(v, 1), Component(v, 2), Component(v, 0)};
      std::string o_abs[3];
      for (uint32_t i = 0; i < 3; ++i) {
        o_abs[i] =
            (instr.vector_operands[0].is_absolute_value && !instr.vector_operands[0].is_negated)
                ? o[i]
                : Let(fmt::format("abs({})", o[i]));
      }
      std::string ma_z =
          Let(fmt::format("{} >= {} && {} >= {}", o_abs[2], o_abs[0], o_abs[2], o_abs[1]));
      std::string ma_y = Let(fmt::format("{} >= {}", o_abs[1], o_abs[0]));
      std::string x_neg = Let(fmt::format("{} < 0.0f", o[0]));
      std::string y_neg = Let(fmt::format("{} < 0.0f", o[1]));
      std::string z_neg = Let(fmt::format("{} < 0.0f", o[2]));
      // select(x-major, y-major, ma_y), then z-major over that.
      auto by_major_axis = [&](const std::string& x_major, const std::string& y_major,
                               const std::string& z_major) {
        return Let(fmt::format("select(select({}, {}, {}), {}, {})", x_major, y_major, ma_y,
                               z_major, ma_z));
      };
      std::string results[4];
      results[0] = by_major_axis(fmt::format("-{}", o[1]),
                                 fmt::format("select({}, -{}, {})", o[2], o[2], y_neg),
                                 fmt::format("-{}", o[1]));
      results[1] = by_major_axis(fmt::format("select(-{}, {}, {})", o[2], o[2], x_neg), o[0],
                                 fmt::format("select({}, -{}, {})", o[0], o[0], z_neg));
      // 2 * the major axis.
      results[2] = Let(fmt::format("2.0f * {}", by_major_axis(o[0], o[1], o[2])));
      results[3] = by_major_axis(fmt::format("select(0.0f, 1.0f, {})", x_neg),
                                 fmt::format("select(2.0f, 3.0f, {})", y_neg),
                                 fmt::format("select(4.0f, 5.0f, {})", z_neg));
      if (used_result_component_count == 1) {
        uint32_t component;
        rex::bit_scan_forward(used_result_components, &component);
        return Value{results[component], 1};
      }
      std::string constructor = FloatType(used_result_component_count) + "(";
      bool first = true;
      for (uint32_t i = 0; i < 4; ++i) {
        if (used_result_components & (uint32_t(1) << i)) {
          constructor += first ? "" : ", ";
          constructor += results[i];
          first = false;
        }
      }
      return let_value(constructor + ")", used_result_component_count);
    }

    case ucode::AluVectorOpcode::kMax4: {
      // The maximum of the source components referenced by the swizzle.
      uint32_t components_remaining = 0;
      for (uint32_t i = 0; i < 4; ++i) {
        SwizzleSource swizzle_source = instr.vector_operands[0].GetComponent(i);
        assert_true(swizzle_source >= SwizzleSource::kX && swizzle_source <= SwizzleSource::kW);
        components_remaining |= 1 << (uint32_t(swizzle_source) - uint32_t(SwizzleSource::kX));
      }
      Value v = ApplyOperandModifiers(Value{operand_storage[0], 4}, instr.vector_operands[0]);
      std::string result;
      uint32_t component;
      while (rex::bit_scan_forward(components_remaining, &component)) {
        components_remaining &= ~(uint32_t(1) << component);
        std::string c = Component(v, component);
        result = result.empty() ? c : Let(fmt::format("max({}, {})", result, c));
      }
      return Value{result, 1};
    }

    case ucode::AluVectorOpcode::kSetpEqPush:
    case ucode::AluVectorOpcode::kSetpNePush:
    case ucode::AluVectorOpcode::kSetpGtPush:
    case ucode::AluVectorOpcode::kSetpGePush: {
      // p = src0.w == 0 && src1.w op 0
      Value a_w = operand(0, 0b1000), b_w = operand(1, 0b1000);
      Line(fmt::format("xe_p = {} == 0.0f && {} {} 0.0f;", a_w.expr, b_w.expr, comparison));
      predicate_written = true;
      if (!used_result_components) {
        return Value();
      }
      // result = (src0.x == 0 && src1.x op 0 ? -1 : src0.x) + 1
      Value a_x = operand(0, 0b0001), b_x = operand(1, 0b0001);
      return let_value(fmt::format("select({}, -1.0f, {} == 0.0f && {} {} 0.0f) + 1.0f", a_x.expr,
                                   a_x.expr, b_x.expr, comparison),
                       1);
    }

    case ucode::AluVectorOpcode::kKillEq:
    case ucode::AluVectorOpcode::kKillGt:
    case ucode::AluVectorOpcode::kKillGe:
    case ucode::AluVectorOpcode::kKillNe: {
      Value a = operand(0, 0b1111), b = operand(1, 0b1111);
      KillPixel(fmt::format("any({} {} {})", a.expr, comparison, b.expr));
      return Value{"0.0f", 1};
    }

    case ucode::AluVectorOpcode::kDst: {
      // (1, src0.y * src1.y, src0.z, src1.w)
      std::string results[4];
      results[0] = "1.0f";
      if (used_result_components & 0b0010) {
        Value a_y = operand(0, 0b0010), b_y = operand(1, 0b0010);
        Value product = let_value(fmt::format("{} * {}", a_y.expr, b_y.expr), 1);
        if (!(instr.vector_operands[0].GetIdenticalComponents(instr.vector_operands[1]) & 0b0010)) {
          product = ZeroIfAnyOperandIsZero(product, a_y, b_y, 0b1);
        }
        results[1] = product.expr;
      }
      if (used_result_components & 0b0100) {
        results[2] = operand(0, 0b0100).expr;
      }
      if (used_result_components & 0b1000) {
        results[3] = operand(1, 0b1000).expr;
      }
      if (used_result_component_count == 1) {
        uint32_t component;
        rex::bit_scan_forward(used_result_components, &component);
        return Value{results[component], 1};
      }
      std::string constructor = FloatType(used_result_component_count) + "(";
      bool first = true;
      for (uint32_t i = 0; i < 4; ++i) {
        if (used_result_components & (uint32_t(1) << i)) {
          constructor += first ? "" : ", ";
          constructor += results[i];
          first = false;
        }
      }
      return let_value(constructor + ")", used_result_component_count);
    }
  }

  assert_unhandled_case(instr.vector_opcode);
  EmitTranslationError("Unknown ALU vector operation");
  return Value();
}

WgslShaderTranslator::Value WgslShaderTranslator::ProcessScalarAluOperation(
    const ParsedAluInstruction& instr, bool& predicate_written) {
  predicate_written = false;

  if (instr.scalar_opcode == ucode::AluScalarOpcode::kRetainPrev) {
    return Value();
  }

  std::string operand_storage[2];
  for (uint32_t i = 0; i < instr.scalar_operand_count; ++i) {
    operand_storage[i] = LoadOperandStorage(instr.scalar_operands[i]);
  }
  auto operand = [&](uint32_t i, uint32_t components) {
    return GetOperandComponents(operand_storage[i], instr.scalar_operands[i], components);
  };
  auto let_value = [&](const std::string& expr) { return Value{Let(expr), 1}; };

  const char* comparison = nullptr;
  switch (instr.scalar_opcode) {
    case ucode::AluScalarOpcode::kMaxs:
    case ucode::AluScalarOpcode::kMaxAs:
    case ucode::AluScalarOpcode::kMaxAsf:
    case ucode::AluScalarOpcode::kSges:
    case ucode::AluScalarOpcode::kSetpGe:
    case ucode::AluScalarOpcode::kKillsGe:
      comparison = ">=";
      break;
    case ucode::AluScalarOpcode::kMins:
      comparison = "<";
      break;
    case ucode::AluScalarOpcode::kSeqs:
    case ucode::AluScalarOpcode::kSetpEq:
    case ucode::AluScalarOpcode::kKillsEq:
    case ucode::AluScalarOpcode::kKillsOne:
      comparison = "==";
      break;
    case ucode::AluScalarOpcode::kSgts:
    case ucode::AluScalarOpcode::kSetpGt:
    case ucode::AluScalarOpcode::kKillsGt:
      comparison = ">";
      break;
    case ucode::AluScalarOpcode::kSnes:
    case ucode::AluScalarOpcode::kSetpNe:
    case ucode::AluScalarOpcode::kKillsNe:
      comparison = "!=";
      break;
    default:
      break;
  }

  switch (instr.scalar_opcode) {
    case ucode::AluScalarOpcode::kAdds:
    case ucode::AluScalarOpcode::kSubs: {
      Value a, b;
      GetOperandScalarXY(operand_storage[0], instr.scalar_operands[0], a, b);
      return let_value(fmt::format("{} {} {}", a.expr,
                                   instr.scalar_opcode == ucode::AluScalarOpcode::kAdds ? "+" : "-",
                                   b.expr));
    }
    case ucode::AluScalarOpcode::kAddsPrev:
    case ucode::AluScalarOpcode::kSubsPrev: {
      Value a = operand(0, 0b0001);
      return let_value(
          fmt::format("{} {} xe_ps", a.expr,
                      instr.scalar_opcode == ucode::AluScalarOpcode::kAddsPrev ? "+" : "-"));
    }
    case ucode::AluScalarOpcode::kMuls: {
      Value a, b;
      GetOperandScalarXY(operand_storage[0], instr.scalar_operands[0], a, b);
      Value result = let_value(fmt::format("{} * {}", a.expr, b.expr));
      if (a.expr != b.expr) {
        result = ZeroIfAnyOperandIsZero(result, a, b, 0b1);
      }
      return result;
    }
    case ucode::AluScalarOpcode::kMulsPrev: {
      Value a = operand(0, 0b0001);
      Value ps = let_value("xe_ps");
      return ZeroIfAnyOperandIsZero(let_value(fmt::format("{} * {}", a.expr, ps.expr)), a, ps, 0b1);
    }
    case ucode::AluScalarOpcode::kMulsPrev2: {
      // ps == -FLT_MAX, ps not finite, src0.y not finite or src0.y <= 0 gives
      // -FLT_MAX, otherwise src0.x * ps.
      Value ps = let_value("xe_ps");
      Value b = operand(0, 0b0010);
      std::string condition = Let(fmt::format(
          "{} != -XE_FLT_MAX && -abs({}) >= -XE_FLT_MAX && -abs({}) >= -XE_FLT_MAX && {} > 0.0f",
          ps.expr, ps.expr, b.expr, b.expr));
      Value a = instr.scalar_operands[0].GetComponent(0) != instr.scalar_operands[0].GetComponent(1)
                    ? operand(0, 0b0001)
                    : b;
      Value product =
          ZeroIfAnyOperandIsZero(let_value(fmt::format("{} * {}", a.expr, ps.expr)), a, ps, 0b1);
      return let_value(fmt::format("select(-XE_FLT_MAX, {}, {})", product.expr, condition));
    }
    case ucode::AluScalarOpcode::kMaxs:
    case ucode::AluScalarOpcode::kMins:
    case ucode::AluScalarOpcode::kMaxAs:
    case ucode::AluScalarOpcode::kMaxAsf: {
      Value a, b;
      GetOperandScalarXY(operand_storage[0], instr.scalar_operands[0], a, b);
      if (instr.scalar_opcode == ucode::AluScalarOpcode::kMaxAs ||
          instr.scalar_opcode == ucode::AluScalarOpcode::kMaxAsf) {
        // maxas rounds, maxasf floors.
        Line(fmt::format("xe_a0 = i32(clamp(floor({}{}), -256.0f, 255.0f));", a.expr,
                         instr.scalar_opcode == ucode::AluScalarOpcode::kMaxAs ? " + 0.5f" : ""));
      }
      if (a.expr == b.expr) {
        return a;
      }
      return let_value(
          fmt::format("select({}, {}, {} {} {})", b.expr, a.expr, a.expr, comparison, b.expr));
    }
    case ucode::AluScalarOpcode::kSeqs:
    case ucode::AluScalarOpcode::kSgts:
    case ucode::AluScalarOpcode::kSges:
    case ucode::AluScalarOpcode::kSnes: {
      Value a = operand(0, 0b0001);
      return let_value(fmt::format("select(0.0f, 1.0f, {} {} 0.0f)", a.expr, comparison));
    }
    case ucode::AluScalarOpcode::kFrcs:
    case ucode::AluScalarOpcode::kTruncs:
    case ucode::AluScalarOpcode::kFloors:
    case ucode::AluScalarOpcode::kExp:
    case ucode::AluScalarOpcode::kLog:
    case ucode::AluScalarOpcode::kRsq:
    case ucode::AluScalarOpcode::kSqrt:
    case ucode::AluScalarOpcode::kSin:
    case ucode::AluScalarOpcode::kCos: {
      const char* function;
      switch (instr.scalar_opcode) {
        case ucode::AluScalarOpcode::kFrcs:
          function = "fract";
          break;
        case ucode::AluScalarOpcode::kTruncs:
          function = "trunc";
          break;
        case ucode::AluScalarOpcode::kFloors:
          function = "floor";
          break;
        case ucode::AluScalarOpcode::kExp:
          function = "exp2";
          break;
        case ucode::AluScalarOpcode::kLog:
          function = "log2";
          break;
        case ucode::AluScalarOpcode::kRsq:
          function = "inverseSqrt";
          break;
        case ucode::AluScalarOpcode::kSqrt:
          function = "sqrt";
          break;
        case ucode::AluScalarOpcode::kSin:
          function = "sin";
          break;
        default:
          function = "cos";
          break;
      }
      Value a = operand(0, 0b0001);
      return let_value(fmt::format("{}({})", function, a.expr));
    }
    case ucode::AluScalarOpcode::kLogc: {
      // -infinity to -FLT_MAX.
      Value r = let_value(fmt::format("log2({})", operand(0, 0b0001).expr));
      return let_value(fmt::format("select({}, -XE_FLT_MAX, {} < -XE_FLT_MAX)", r.expr, r.expr));
    }
    case ucode::AluScalarOpcode::kRcpc:
    case ucode::AluScalarOpcode::kRsqc: {
      // Infinities to +-FLT_MAX.
      Value a = operand(0, 0b0001);
      Value r = let_value(instr.scalar_opcode == ucode::AluScalarOpcode::kRcpc
                              ? fmt::format("1.0f / {}", a.expr)
                              : fmt::format("inverseSqrt({})", a.expr));
      return let_value(fmt::format(
          "select(select({}, -XE_FLT_MAX, {} < -XE_FLT_MAX), XE_FLT_MAX, {} > XE_FLT_MAX)", r.expr,
          r.expr, r.expr));
    }
    case ucode::AluScalarOpcode::kRcpf:
    case ucode::AluScalarOpcode::kRsqf: {
      // Infinities to +-0.
      Value a = operand(0, 0b0001);
      Value r = let_value(instr.scalar_opcode == ucode::AluScalarOpcode::kRcpf
                              ? fmt::format("1.0f / {}", a.expr)
                              : fmt::format("inverseSqrt({})", a.expr));
      return let_value(
          fmt::format("select({}, bitcast<f32>(bitcast<u32>({}) & 0x80000000u), abs({}) > "
                      "XE_FLT_MAX)",
                      r.expr, r.expr, r.expr));
    }
    case ucode::AluScalarOpcode::kRcp: {
      Value a = operand(0, 0b0001);
      return let_value(fmt::format("1.0f / {}", a.expr));
    }

    case ucode::AluScalarOpcode::kSetpEq:
    case ucode::AluScalarOpcode::kSetpNe:
    case ucode::AluScalarOpcode::kSetpGt:
    case ucode::AluScalarOpcode::kSetpGe: {
      Value a = operand(0, 0b0001);
      Line(fmt::format("xe_p = {} {} 0.0f;", a.expr, comparison));
      predicate_written = true;
      return let_value("select(1.0f, 0.0f, xe_p)");
    }
    case ucode::AluScalarOpcode::kSetpInv: {
      // p = src0 == 1; result = p ? 0 : (src0 == 0 ? 1 : src0)
      Value a = operand(0, 0b0001);
      Line(fmt::format("xe_p = {} == 1.0f;", a.expr));
      predicate_written = true;
      return let_value(
          fmt::format("select(select({}, 1.0f, {} == 0.0f), 0.0f, xe_p)", a.expr, a.expr));
    }
    case ucode::AluScalarOpcode::kSetpPop: {
      // p = src0 - 1 <= 0; result = p ? 0 : src0 - 1
      Value a_minus_1 = let_value(fmt::format("{} - 1.0f", operand(0, 0b0001).expr));
      Line(fmt::format("xe_p = {} <= 0.0f;", a_minus_1.expr));
      predicate_written = true;
      return let_value(fmt::format("select({}, 0.0f, xe_p)", a_minus_1.expr));
    }
    case ucode::AluScalarOpcode::kSetpClr:
      Line("xe_p = false;");
      predicate_written = true;
      return Value{"XE_FLT_MAX", 1};
    case ucode::AluScalarOpcode::kSetpRstr: {
      // p = src0 == 0; result = p ? 0 : src0
      Value a = operand(0, 0b0001);
      Line(fmt::format("xe_p = {} == 0.0f;", a.expr));
      predicate_written = true;
      return let_value(fmt::format("select({}, 0.0f, xe_p)", a.expr));
    }

    case ucode::AluScalarOpcode::kKillsEq:
    case ucode::AluScalarOpcode::kKillsGt:
    case ucode::AluScalarOpcode::kKillsGe:
    case ucode::AluScalarOpcode::kKillsNe:
    case ucode::AluScalarOpcode::kKillsOne: {
      Value a = operand(0, 0b0001);
      KillPixel(
          fmt::format("{} {} {}", a.expr, comparison,
                      instr.scalar_opcode == ucode::AluScalarOpcode::kKillsOne ? "1.0f" : "0.0f"));
      return Value{"0.0f", 1};
    }

    case ucode::AluScalarOpcode::kMulsc0:
    case ucode::AluScalarOpcode::kMulsc1: {
      Value a = operand(0, 0b0001), b = operand(1, 0b0001);
      Value result = let_value(fmt::format("{} * {}", a.expr, b.expr));
      if (!(instr.scalar_operands[0].GetIdenticalComponents(instr.scalar_operands[1]) & 0b0001)) {
        result = ZeroIfAnyOperandIsZero(result, a, b, 0b1);
      }
      return result;
    }
    case ucode::AluScalarOpcode::kAddsc0:
    case ucode::AluScalarOpcode::kAddsc1:
    case ucode::AluScalarOpcode::kSubsc0:
    case ucode::AluScalarOpcode::kSubsc1: {
      Value a = operand(0, 0b0001), b = operand(1, 0b0001);
      bool is_add = instr.scalar_opcode == ucode::AluScalarOpcode::kAddsc0 ||
                    instr.scalar_opcode == ucode::AluScalarOpcode::kAddsc1;
      return let_value(fmt::format("{} {} {}", a.expr, is_add ? "+" : "-", b.expr));
    }

    default:
      break;
  }

  assert_unhandled_case(instr.scalar_opcode);
  EmitTranslationError("Unknown ALU scalar operation");
  return Value();
}

}  // namespace rex::graphics
