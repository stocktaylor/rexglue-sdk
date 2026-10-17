#pragma once
/**
 * @file        graphics/null/wireframe_sampler.h
 * @brief       Periodic CPU wireframe capture of one guest frame
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <vector>

#include <rex/graphics/pipeline/shader/interpreter.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>
#include <rex/memory.h>

namespace rex::ui {
class Presenter;
}

namespace rex::graphics::null {

// Once per null_gpu_sample_interval_ms, captures the next whole guest frame:
// every draw's vertex shader runs on the CPU (ShaderInterpreter) and the
// primitive edges are rasterized into an image the size of the frontbuffer,
// coloured by render target. The result is presented at the frame's swap and
// optionally written as a BMP. Nothing is textured or shaded: this shows what
// the title submits, not what it would look like. Frames in between cost
// nothing beyond the null backend's command stream processing.
class WireframeSampler {
 public:
  struct Indices {
    // Guest physical address and count; source is auto-indexed when !indexed.
    bool indexed = false;
    uint32_t guest_base = 0;
    uint32_t count = 0;
    xenos::IndexFormat format = xenos::IndexFormat::kInt16;
    xenos::Endian endianness = xenos::Endian::kNone;
  };

  WireframeSampler(const RegisterFile& register_file, const memory::Memory& memory);

  bool capturing() const { return capturing_; }

  // Called for every draw while capturing.
  void OnDraw(const Shader& vertex_shader, xenos::PrimitiveType prim_type, const Indices& indices);

  // Called at every guest swap. Finishes and publishes a capture in progress,
  // and starts the next one when the interval has passed.
  void OnSwap(uint32_t frontbuffer_width, uint32_t frontbuffer_height, ui::Presenter* presenter);

 private:
  struct Vertex {
    float x, y;
    bool valid;
  };

  class PositionExportSink : public ShaderInterpreter::ExportSink {
   public:
    void Reset() {
      has_position_ = false;
      killed_ = false;
    }
    void Export(ucode::ExportRegister export_register, const float* value,
                uint32_t value_mask) override;
    bool has_position_ = false;
    bool killed_ = false;
    float position_[4] = {};
  };

  void BeginCapture(uint32_t width, uint32_t height);
  void FinishCapture(ui::Presenter* presenter);
  void WriteBmp(const std::filesystem::path& path) const;

  Vertex TransformVertex(uint32_t vertex_index);
  void DrawLine(const Vertex& a, const Vertex& b, uint32_t color);
  void DrawPoint(const Vertex& v, uint32_t color);

  const RegisterFile& register_file_;
  const memory::Memory& memory_;
  ShaderInterpreter interpreter_;
  PositionExportSink export_sink_;

  std::chrono::steady_clock::time_point last_capture_time_{};
  bool capturing_ = false;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  // R8 G8 B8 X8 in memory order (RawImage layout).
  std::vector<uint32_t> pixels_;
  uint64_t capture_index_ = 0;
  // Guest swaps between capture starts, as a frame rate.
  uint32_t swaps_since_capture_ = 0;
  float guest_fps_ = 0.0f;

  // Per draw: viewport transform and transformed-vertex cache.
  float viewport_scale_[2] = {};
  float viewport_offset_[2] = {};
  bool xy_screen_space_ = false;
  bool w_is_reciprocal_ = false;
  std::unordered_map<uint32_t, Vertex> vertex_cache_;

  // Per capture statistics.
  uint32_t draws_ = 0;
  uint32_t draws_drawn_ = 0;
  uint32_t draws_skipped_shader_ = 0;
  uint32_t draws_skipped_budget_ = 0;
  uint32_t draws_skipped_depth_only_ = 0;
  uint64_t vertices_shaded_ = 0;
};

}  // namespace rex::graphics::null
