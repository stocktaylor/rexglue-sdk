/**
 * @file        graphics/null/wireframe_sampler.cpp
 * @brief       Periodic CPU wireframe capture of one guest frame
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "wireframe_sampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/graphics/registers.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>
#include <rex/ui/software/presenter.h>

REXCVAR_DEFINE_INT32(null_gpu_sample_interval_ms, 1000, "GPU/Null",
                     "Null GPU: capture one frame as a CPU wireframe this often, in milliseconds "
                     "(0 = never). Other frames are not drawn at all");
REXCVAR_DEFINE_INT32(null_gpu_sample_max_vertices, 300000, "GPU/Null",
                     "Null GPU: vertex shader invocations per sampled frame before further draws "
                     "are skipped, bounding the hitch a sample causes");
REXCVAR_DEFINE_BOOL(null_gpu_sample_depth_only, false, "GPU/Null",
                    "Null GPU: include depth-only draws (shadow maps, depth prepasses) in "
                    "sampled frames");
REXCVAR_DEFINE_STRING(null_gpu_sample_dump_dir, "", "GPU/Null",
                      "Null GPU: also write each sampled frame to this directory as a BMP");

namespace rex::graphics::null {

namespace {

constexpr uint32_t kBackground = 0xFF181818;

// R8 G8 B8 X8 in memory order on little-endian hosts.
constexpr uint32_t PackColor(uint8_t r, uint8_t g, uint8_t b) {
  return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | 0xFF000000u;
}

// A distinct bright colour per render target (EDRAM base), so passes into
// different targets can be told apart.
uint32_t ColorForRenderTarget(uint32_t edram_base) {
  float hue = std::fmod(float(edram_base) * 0.61803398875f, 1.0f) * 6.0f;
  float x = 1.0f - std::fabs(std::fmod(hue, 2.0f) - 1.0f);
  float r = 0, g = 0, b = 0;
  switch (int(hue)) {
    case 0:
      r = 1, g = x;
      break;
    case 1:
      r = x, g = 1;
      break;
    case 2:
      g = 1, b = x;
      break;
    case 3:
      g = x, b = 1;
      break;
    case 4:
      r = x, b = 1;
      break;
    default:
      r = 1, b = x;
      break;
  }
  auto channel = [](float v) { return uint8_t(80.0f + v * 175.0f); };
  return PackColor(channel(r), channel(g), channel(b));
}

// Primitives are rasterized from cached screen-space vertices; anything far
// outside the target (or not a number) is dropped rather than clipped.
bool IsDrawable(const float v) {
  return std::isfinite(v) && std::fabs(v) < 65536.0f;
}

}  // namespace

void WireframeSampler::PositionExportSink::Export(ucode::ExportRegister export_register,
                                                  const float* value, uint32_t value_mask) {
  if (export_register == ucode::ExportRegister::kVSPosition) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (value_mask & (1u << i)) {
        position_[i] = value[i];
      }
    }
    has_position_ = true;
  } else if (export_register == ucode::ExportRegister::kVSPointSizeEdgeFlagKillVertex) {
    if ((value_mask & 0b0100) &&
        (rex::memory::Reinterpret<uint32_t>(value[2]) & ~(UINT32_C(1) << 31))) {
      killed_ = true;
    }
  }
}

WireframeSampler::WireframeSampler(const RegisterFile& register_file, const memory::Memory& memory)
    : register_file_(register_file), memory_(memory), interpreter_(register_file, memory) {}

void WireframeSampler::OnSwap(uint32_t frontbuffer_width, uint32_t frontbuffer_height,
                              ui::Presenter* presenter) {
  ++swaps_since_capture_;
  if (capturing_) {
    FinishCapture(presenter);
  }
  int32_t interval_ms = REXCVAR_GET(null_gpu_sample_interval_ms);
  if (interval_ms <= 0 || !frontbuffer_width || !frontbuffer_height) {
    return;
  }
  auto now = std::chrono::steady_clock::now();
  auto elapsed = now - last_capture_time_;
  if (elapsed < std::chrono::milliseconds(interval_ms)) {
    return;
  }
  guest_fps_ = last_capture_time_.time_since_epoch().count()
                   ? float(swaps_since_capture_) / std::chrono::duration<float>(elapsed).count()
                   : 0.0f;
  swaps_since_capture_ = 0;
  last_capture_time_ = now;
  // The frame after this swap is the one sampled; it targets the same
  // frontbuffer size in practice.
  BeginCapture(frontbuffer_width, frontbuffer_height);
}

void WireframeSampler::BeginCapture(uint32_t width, uint32_t height) {
  capturing_ = true;
  width_ = std::min(width, uint32_t(xenos::kTexture2DCubeMaxWidthHeight));
  height_ = std::min(height, uint32_t(xenos::kTexture2DCubeMaxWidthHeight));
  pixels_.assign(size_t(width_) * height_, kBackground);
  draws_ = 0;
  draws_drawn_ = 0;
  draws_skipped_shader_ = 0;
  draws_skipped_budget_ = 0;
  draws_skipped_depth_only_ = 0;
  vertices_shaded_ = 0;
}

void WireframeSampler::FinishCapture(ui::Presenter* presenter) {
  capturing_ = false;
  ++capture_index_;
  REXGPU_DEBUG(
      "Null GPU sample {}: guest {:.1f} fps; {} draws, {} drawn, {} vertices shaded; skipped {} "
      "(shader), {} (depth only), {} (vertex budget)",
      capture_index_, guest_fps_, draws_, draws_drawn_, vertices_shaded_, draws_skipped_shader_,
      draws_skipped_depth_only_, draws_skipped_budget_);

  if (presenter) {
    presenter->RefreshGuestOutput(width_, height_, width_, height_,
                                  [this](ui::Presenter::GuestOutputRefreshContext& context) {
                                    // Software or WebGPU presenter (null_gpu_presenter).
                                    context.SetIs8bpc(true);
                                    return context.WriteRgba8(pixels_.data(),
                                                              size_t(width_) * sizeof(uint32_t));
                                  });
  }

  std::string dump_dir = REXCVAR_GET(null_gpu_sample_dump_dir);
  if (!dump_dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(dump_dir, ec);
    WriteBmp(std::filesystem::path(dump_dir) / fmt::format("sample_{:06}.bmp", capture_index_));
  }
}

void WireframeSampler::OnDraw(const Shader& vertex_shader, xenos::PrimitiveType prim_type,
                              const Indices& indices) {
  if (!capturing_) {
    return;
  }
  ++draws_;
  const RegisterFile& regs = register_file_;

  // Resolve-style 2D draws and tessellation are not reproduced.
  if (prim_type >= xenos::PrimitiveType::kExplicitMajorModeForceStart) {
    return;
  }
  auto edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode != xenos::EdramMode::kColorDepth && edram_mode != xenos::EdramMode::kDepthOnly) {
    return;
  }
  bool depth_only = edram_mode == xenos::EdramMode::kDepthOnly ||
                    !(regs.Get<reg::RB_COLOR_MASK>().value & 0xFFFF);
  if (depth_only && !REXCVAR_GET(null_gpu_sample_depth_only)) {
    ++draws_skipped_depth_only_;
    return;
  }
  if (!ShaderInterpreter::CanInterpretShader(vertex_shader)) {
    ++draws_skipped_shader_;
    return;
  }
  if (vertices_shaded_ >= uint64_t(std::max(REXCVAR_GET(null_gpu_sample_max_vertices), 0))) {
    ++draws_skipped_budget_;
    return;
  }

  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  viewport_scale_[0] =
      pa_cl_vte_cntl.vport_x_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE) : 1.0f;
  viewport_scale_[1] =
      pa_cl_vte_cntl.vport_y_scale_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.0f;
  viewport_offset_[0] =
      pa_cl_vte_cntl.vport_x_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET) : 0.0f;
  viewport_offset_[1] =
      pa_cl_vte_cntl.vport_y_offset_ena ? regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f;
  // The window offset is not applied: predicated tiling draws the frame once
  // per EDRAM tile, shifting each pass by its tile's offset, and leaving it
  // out puts every pass back at its screen position.
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  xy_screen_space_ = pa_cl_vte_cntl.vtx_xy_fmt;
  w_is_reciprocal_ = pa_cl_vte_cntl.vtx_w0_fmt;

  // Index stream, with primitive reset marked as UINT32_MAX.
  const void* index_data = nullptr;
  xenos::Endian index_endian = indices.endianness;
  if (indices.indexed) {
    if (indices.format == xenos::IndexFormat::kInt16) {
      // As in DrawExtentEstimator / PrimitiveProcessor.
      if (index_endian == xenos::Endian::k8in32) {
        index_endian = xenos::Endian::k8in16;
      } else if (index_endian == xenos::Endian::k16in32) {
        index_endian = xenos::Endian::kNone;
      }
    }
    index_data = memory_.TranslatePhysical(indices.guest_base);
  }
  bool reset_enabled = pa_su_sc_mode_cntl.multi_prim_ib_ena;
  uint32_t reset_index = regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
  uint32_t index_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  uint32_t min_index = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;
  uint32_t max_index = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;

  uint32_t color = ColorForRenderTarget(regs.Get<reg::RB_COLOR_INFO>().color_base);

  interpreter_.SetShader(vertex_shader);
  interpreter_.SetExportSink(&export_sink_);
  vertex_cache_.clear();

  // Vertices of the primitive being assembled since the last reset.
  std::vector<Vertex> strip;
  strip.reserve(indices.count);
  // Zero-area triangles are skipped: strips use them to join separate
  // primitives, and their edges would connect unrelated geometry.
  auto draw_triangle = [&](const Vertex& a, const Vertex& b, const Vertex& c) {
    if (!a.valid || !b.valid || !c.valid) {
      return;
    }
    float area2 = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    if (std::fabs(area2) < 0.5f) {
      return;
    }
    DrawLine(a, b, color);
    DrawLine(b, c, color);
    DrawLine(c, a, color);
  };
  auto flush_strip = [&]() {
    size_t n = strip.size();
    switch (prim_type) {
      case xenos::PrimitiveType::kPointList:
        for (const Vertex& v : strip) {
          DrawPoint(v, color);
        }
        break;
      case xenos::PrimitiveType::kLineList:
        for (size_t i = 1; i < n; i += 2) {
          DrawLine(strip[i - 1], strip[i], color);
        }
        break;
      case xenos::PrimitiveType::kLineStrip:
      case xenos::PrimitiveType::kLineLoop:
        for (size_t i = 1; i < n; ++i) {
          DrawLine(strip[i - 1], strip[i], color);
        }
        if (prim_type == xenos::PrimitiveType::kLineLoop && n > 2) {
          DrawLine(strip[n - 1], strip[0], color);
        }
        break;
      case xenos::PrimitiveType::kTriangleList:
      case xenos::PrimitiveType::kTriangleWithWFlags:
        for (size_t i = 2; i < n; i += 3) {
          draw_triangle(strip[i - 2], strip[i - 1], strip[i]);
        }
        break;
      case xenos::PrimitiveType::kTriangleStrip:
        for (size_t i = 2; i < n; ++i) {
          draw_triangle(strip[i - 2], strip[i - 1], strip[i]);
        }
        break;
      case xenos::PrimitiveType::kTriangleFan:
      case xenos::PrimitiveType::kPolygon:
        for (size_t i = 2; i < n; ++i) {
          draw_triangle(strip[0], strip[i - 1], strip[i]);
        }
        break;
      case xenos::PrimitiveType::kRectangleList:
        // Three corners of a screen-aligned rectangle: draw its bounds.
        for (size_t i = 2; i < n; i += 3) {
          const Vertex* v[3] = {&strip[i - 2], &strip[i - 1], &strip[i]};
          if (!v[0]->valid || !v[1]->valid || !v[2]->valid) {
            continue;
          }
          float x0 = std::min({v[0]->x, v[1]->x, v[2]->x});
          float x1 = std::max({v[0]->x, v[1]->x, v[2]->x});
          float y0 = std::min({v[0]->y, v[1]->y, v[2]->y});
          float y1 = std::max({v[0]->y, v[1]->y, v[2]->y});
          Vertex c[4] = {{x0, y0, true}, {x1, y0, true}, {x1, y1, true}, {x0, y1, true}};
          for (int e = 0; e < 4; ++e) {
            DrawLine(c[e], c[(e + 1) & 3], color);
          }
        }
        break;
      case xenos::PrimitiveType::kQuadList:
        for (size_t i = 3; i < n; i += 4) {
          for (size_t e = 0; e < 4; ++e) {
            DrawLine(strip[i - 3 + e], strip[i - 3 + ((e + 1) & 3)], color);
          }
        }
        break;
      case xenos::PrimitiveType::kQuadStrip:
        for (size_t i = 3; i < n; i += 2) {
          DrawLine(strip[i - 3], strip[i - 2], color);
          DrawLine(strip[i - 2], strip[i], color);
          DrawLine(strip[i], strip[i - 1], color);
          DrawLine(strip[i - 1], strip[i - 3], color);
        }
        break;
      default:
        break;
    }
    strip.clear();
  };

  for (uint32_t i = 0; i < indices.count; ++i) {
    uint32_t vertex_index;
    if (indices.indexed) {
      if (indices.format == xenos::IndexFormat::kInt16) {
        vertex_index = static_cast<const uint16_t*>(index_data)[i];
      } else {
        vertex_index = static_cast<const uint32_t*>(index_data)[i];
      }
      // The Xenos only uses 24 bits of the index (reset_indx is 24-bit).
      vertex_index = xenos::GpuSwap(vertex_index, index_endian) & 0xFFFFFF;
      if (reset_enabled && vertex_index == reset_index) {
        flush_strip();
        continue;
      }
    } else {
      vertex_index = i;
    }
    vertex_index =
        std::min(max_index, std::max(min_index, (vertex_index + index_offset) & 0xFFFFFF));
    strip.push_back(TransformVertex(vertex_index));
  }
  flush_strip();

  interpreter_.SetExportSink(nullptr);
  ++draws_drawn_;
}

WireframeSampler::Vertex WireframeSampler::TransformVertex(uint32_t vertex_index) {
  auto cached = vertex_cache_.find(vertex_index);
  if (cached != vertex_cache_.end()) {
    return cached->second;
  }
  export_sink_.Reset();
  interpreter_.temp_registers()[0] = float(vertex_index);
  interpreter_.Execute();
  ++vertices_shaded_;

  Vertex v{0.0f, 0.0f, false};
  if (export_sink_.has_position_ && !export_sink_.killed_) {
    float x = export_sink_.position_[0];
    float y = export_sink_.position_[1];
    float w = export_sink_.position_[3];
    bool valid = true;
    if (!xy_screen_space_) {
      if (w_is_reciprocal_) {
        x *= w;
        y *= w;
      } else if (w > 0.0f) {
        x /= w;
        y /= w;
      } else {
        // Behind the eye: dropping the vertex beats drawing it mirrored.
        valid = false;
      }
    }
    x = x * viewport_scale_[0] + viewport_offset_[0];
    y = y * viewport_scale_[1] + viewport_offset_[1];
    v = {x, y, valid && IsDrawable(x) && IsDrawable(y)};
  }
  vertex_cache_.emplace(vertex_index, v);
  return v;
}

void WireframeSampler::DrawPoint(const Vertex& v, uint32_t color) {
  if (!v.valid) {
    return;
  }
  int32_t x = int32_t(std::floor(v.x)), y = int32_t(std::floor(v.y));
  if (x >= 0 && y >= 0 && uint32_t(x) < width_ && uint32_t(y) < height_) {
    pixels_[size_t(y) * width_ + uint32_t(x)] = color;
  }
}

void WireframeSampler::DrawLine(const Vertex& a, const Vertex& b, uint32_t color) {
  if (!a.valid || !b.valid) {
    return;
  }
  // Liang-Barsky clip to the image, then a DDA step per pixel.
  float x0 = a.x, y0 = a.y, dx = b.x - a.x, dy = b.y - a.y;
  float t0 = 0.0f, t1 = 1.0f;
  float max_x = float(width_) - 0.001f, max_y = float(height_) - 0.001f;
  const float p[4] = {-dx, dx, -dy, dy};
  const float q[4] = {x0, max_x - x0, y0, max_y - y0};
  for (int i = 0; i < 4; ++i) {
    if (p[i] == 0.0f) {
      if (q[i] < 0.0f) {
        return;
      }
      continue;
    }
    float t = q[i] / p[i];
    if (p[i] < 0.0f) {
      t0 = std::max(t0, t);
    } else {
      t1 = std::min(t1, t);
    }
    if (t0 > t1) {
      return;
    }
  }
  float sx = x0 + t0 * dx, sy = y0 + t0 * dy;
  float ex = x0 + t1 * dx, ey = y0 + t1 * dy;
  int32_t steps = int32_t(std::ceil(std::max(std::fabs(ex - sx), std::fabs(ey - sy))));
  if (steps <= 0) {
    DrawPoint({sx, sy, true}, color);
    return;
  }
  float step_x = (ex - sx) / float(steps), step_y = (ey - sy) / float(steps);
  for (int32_t i = 0; i <= steps; ++i) {
    uint32_t px = uint32_t(std::clamp(sx + step_x * float(i), 0.0f, max_x));
    uint32_t py = uint32_t(std::clamp(sy + step_y * float(i), 0.0f, max_y));
    pixels_[size_t(py) * width_ + px] = color;
  }
}

void WireframeSampler::WriteBmp(const std::filesystem::path& path) const {
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    REXGPU_WARN("Null GPU: can't write sample to {}", path.string());
    return;
  }
  uint32_t row_bytes = width_ * 3;
  uint32_t row_padding = (4 - row_bytes % 4) % 4;
  uint32_t image_size = (row_bytes + row_padding) * height_;
  auto put16 = [&](uint16_t v) { file.write(reinterpret_cast<const char*>(&v), 2); };
  auto put32 = [&](uint32_t v) { file.write(reinterpret_cast<const char*>(&v), 4); };
  // BITMAPFILEHEADER + BITMAPINFOHEADER, 24 bpp, bottom-up rows.
  file.write("BM", 2);
  put32(54 + image_size);
  put32(0);
  put32(54);
  put32(40);
  put32(width_);
  put32(height_);
  put16(1);
  put16(24);
  put32(0);
  put32(image_size);
  put32(2835);
  put32(2835);
  put32(0);
  put32(0);
  std::vector<char> row(row_bytes + row_padding, 0);
  for (uint32_t y = height_; y-- > 0;) {
    const uint32_t* src = pixels_.data() + size_t(y) * width_;
    for (uint32_t x = 0; x < width_; ++x) {
      row[x * 3 + 0] = char((src[x] >> 16) & 0xFF);
      row[x * 3 + 1] = char((src[x] >> 8) & 0xFF);
      row[x * 3 + 2] = char(src[x] & 0xFF);
    }
    file.write(row.data(), std::streamsize(row.size()));
  }
}

}  // namespace rex::graphics::null
