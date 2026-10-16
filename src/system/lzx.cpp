/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <climits>

#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/system/lzx.h>
#include <rex/system/util/xex2_info.h>
#include <rex/types.h>

#include <lzx.h>
#include <mspack.h>

typedef struct mspack_memory_file_t {
  mspack_system sys;
  void* buffer;
  off_t buffer_size;
  off_t offset;
} mspack_memory_file;

mspack_memory_file* mspack_memory_open(mspack_system* sys, void* buffer, const size_t buffer_size) {
  (void)sys;
  assert_true(buffer_size < INT_MAX);
  if (buffer_size >= INT_MAX) {
    return NULL;
  }
  auto memfile = (mspack_memory_file*)std::calloc(1, sizeof(mspack_memory_file));
  if (!memfile) {
    return NULL;
  }
  memfile->buffer = buffer;
  memfile->buffer_size = (off_t)buffer_size;
  memfile->offset = 0;
  return memfile;
}

void mspack_memory_close(mspack_memory_file* file) {
  auto memfile = (mspack_memory_file*)file;
  std::free(memfile);
}

int mspack_memory_read(mspack_file* file, void* buffer, int chars) {
  auto memfile = (mspack_memory_file*)file;
  const off_t remaining = memfile->buffer_size - memfile->offset;
  const off_t total = std::min(static_cast<off_t>(chars), remaining);
  std::memcpy(buffer, (uint8_t*)memfile->buffer + memfile->offset, total);
  memfile->offset += total;
  return (int)total;
}

int mspack_memory_write(mspack_file* file, void* buffer, int chars) {
  auto memfile = (mspack_memory_file*)file;
  const off_t remaining = memfile->buffer_size - memfile->offset;
  const off_t total = std::min(static_cast<off_t>(chars), remaining);
  std::memcpy((uint8_t*)memfile->buffer + memfile->offset, buffer, total);
  memfile->offset += total;
  return (int)total;
}

void* mspack_memory_alloc(mspack_system* sys, size_t chars) {
  return std::calloc(chars, 1);
}

void mspack_memory_free(void* ptr) {
  std::free(ptr);
}

void mspack_memory_copy(void* src, void* dest, size_t chars) {
  std::memcpy(dest, src, chars);
}

mspack_system* mspack_memory_sys_create() {
  auto sys = (mspack_system*)std::calloc(1, sizeof(mspack_system));
  if (!sys) {
    return NULL;
  }
  sys->read = mspack_memory_read;
  sys->write = mspack_memory_write;
  sys->alloc = mspack_memory_alloc;
  sys->free = mspack_memory_free;
  sys->copy = mspack_memory_copy;
  return sys;
}

void mspack_memory_sys_destroy(struct mspack_system* sys) {
  free(sys);
}

int lzx_decompress(const void* lzx_data, size_t lzx_len, void* dest, size_t dest_len,
                   uint32_t window_size, void* window_data, size_t window_data_len) {
  int result_code = 1;

  uint32_t window_bits;
  if (!rex::bit_scan_forward(window_size, &window_bits)) {
    return result_code;
  }

  mspack_system* sys = mspack_memory_sys_create();
  mspack_memory_file* lzxsrc = mspack_memory_open(sys, (void*)lzx_data, lzx_len);
  mspack_memory_file* lzxdst = mspack_memory_open(sys, dest, dest_len);
  lzxd_stream* lzxd = lzxd_init(sys, (mspack_file*)lzxsrc, (mspack_file*)lzxdst, window_bits, 0,
                                0x8000, (off_t)dest_len, 0);

  if (lzxd) {
    if (window_data) {
      // zero the window and then copy window_data to the end of it
      auto padding_len = window_size - window_data_len;
      std::memset(&lzxd->window[0], 0, padding_len);
      std::memcpy(&lzxd->window[padding_len], window_data, window_data_len);
      // TODO(gibbed): should this be set regardless if source window data is
      // available or not?
      lzxd->ref_data_size = window_size;
    }

    result_code = lzxd_decompress(lzxd, (off_t)dest_len);

    lzxd_free(lzxd);
    lzxd = NULL;
  }

  if (lzxsrc) {
    mspack_memory_close(lzxsrc);
    lzxsrc = NULL;
  }

  if (lzxdst) {
    mspack_memory_close(lzxdst);
    lzxdst = NULL;
  }

  if (sys) {
    mspack_memory_sys_destroy(sys);
    sys = NULL;
  }

  return result_code;
}

struct lzx_stream_ctx {
  mspack_system* sys = nullptr;
  mspack_memory_file* src = nullptr;
  mspack_memory_file* dst = nullptr;
  lzxd_stream* lzxd = nullptr;
  uint32_t window_bits = 0;
};

// Source reads past the end of the current block return zeros: the decoder
// reads ahead a few bytes at frame ends, and an empty read would latch its
// end-of-input error for the rest of the stream.
static int lzx_stream_read(mspack_file* file, void* buffer, int chars) {
  auto memfile = (mspack_memory_file*)file;
  const off_t remaining = memfile->buffer_size - memfile->offset;
  if (remaining <= 0) {
    std::memset(buffer, 0, chars);
    return chars;
  }
  return mspack_memory_read(file, buffer, chars);
}

lzx_stream_ctx* lzx_stream_create(uint32_t window_size) {
  uint32_t window_bits;
  if (!rex::bit_scan_forward(window_size, &window_bits)) {
    return nullptr;
  }
  auto ctx = new lzx_stream_ctx();
  ctx->window_bits = window_bits;
  ctx->sys = mspack_memory_sys_create();
  if (ctx->sys) {
    ctx->sys->read = lzx_stream_read;
    ctx->src = mspack_memory_open(ctx->sys, nullptr, 0);
    ctx->dst = mspack_memory_open(ctx->sys, nullptr, 0);
  }
  if (!ctx->src || !ctx->dst || lzx_stream_reset(ctx) != 0) {
    lzx_stream_destroy(ctx);
    return nullptr;
  }
  return ctx;
}

int lzx_stream_reset(lzx_stream_ctx* ctx) {
  if (ctx->lzxd) {
    lzxd_free(ctx->lzxd);
  }
  ctx->lzxd = lzxd_init(ctx->sys, (mspack_file*)ctx->src, (mspack_file*)ctx->dst,
                        ctx->window_bits, 0, 0x10000, 0, 0);
  return ctx->lzxd ? 0 : 1;
}

int lzx_stream_decompress(lzx_stream_ctx* ctx, const void* src, size_t src_len, void* dst,
                          size_t dst_len) {
  auto lzx = ctx->lzxd;
  if (!lzx) {
    return 1;
  }
  if (!dst_len) {
    return 0;
  }
  ctx->src->buffer = const_cast<void*>(src);
  ctx->src->buffer_size = (off_t)src_len;
  ctx->src->offset = 0;
  ctx->dst->buffer = dst;
  ctx->dst->buffer_size = (off_t)dst_len;
  ctx->dst->offset = 0;

  // Each call carries one block: drop whatever the decoder read ahead past the
  // previous one (frames end 16-bit aligned, so nothing real is lost).
  lzx->i_ptr = lzx->i_end = lzx->inbuf;
  lzx->bit_buffer = 0;
  lzx->bits_left = 0;
  lzx->input_end = 0;
  // A block shorter than a frame is the final frame; its size comes from the
  // stream length.
  lzx->length = lzx->offset + (off_t)dst_len;

  // lzxd_decompress(n) decodes through frame (offset + n) / 32K, i.e. one
  // frame too many when n ends exactly on a frame boundary, and that frame's
  // input isn't here yet. Decode n - 1 bytes, then flush the last one.
  int result = MSPACK_ERR_OK;
  if (dst_len > 1) {
    result = lzxd_decompress(lzx, (off_t)dst_len - 1);
  }
  if (result == MSPACK_ERR_OK) {
    result = lzxd_decompress(lzx, 1);
  }
  if (result != MSPACK_ERR_OK || ctx->dst->offset != (off_t)dst_len) {
    return result ? result : 1;
  }
  return 0;
}

void lzx_stream_destroy(lzx_stream_ctx* ctx) {
  if (!ctx) {
    return;
  }
  if (ctx->lzxd) {
    lzxd_free(ctx->lzxd);
  }
  if (ctx->src) {
    mspack_memory_close(ctx->src);
  }
  if (ctx->dst) {
    mspack_memory_close(ctx->dst);
  }
  if (ctx->sys) {
    mspack_memory_sys_destroy(ctx->sys);
  }
  delete ctx;
}

int lzxdelta_apply_patch(rex::xex2_delta_patch* patch, size_t patch_len, uint32_t window_size,
                         void* dest) {
  void* patch_end = (char*)patch + patch_len;
  auto* cur_patch = patch;

  while (patch_end > cur_patch) {
    int patch_sz = -4;  // 0 byte patches need us to remove 4 byte from next
                        // patch addr because of patch_data field
    if (cur_patch->compressed_len == 0 && cur_patch->uncompressed_len == 0 &&
        cur_patch->new_addr == 0 && cur_patch->old_addr == 0)
      break;
    switch (cur_patch->compressed_len) {
      case 0:  // fill with 0
        std::memset((char*)dest + cur_patch->new_addr, 0, cur_patch->uncompressed_len);
        break;
      case 1:  // copy from old -> new
        std::memmove((char*)dest + cur_patch->new_addr, (char*)dest + cur_patch->old_addr,
                     cur_patch->uncompressed_len);
        break;
      default:                                     // delta patch
        patch_sz = cur_patch->compressed_len - 4;  // -4 because of patch_data field

        int result = lzx_decompress(cur_patch->patch_data, cur_patch->compressed_len,
                                    (char*)dest + cur_patch->new_addr, cur_patch->uncompressed_len,
                                    window_size, (char*)dest + cur_patch->old_addr,
                                    cur_patch->uncompressed_len);

        if (result) {
          return result;
        }
        break;
    }

    cur_patch++;
    cur_patch = (rex::xex2_delta_patch*)((char*)cur_patch + patch_sz);  // TODO: make this less ugly
  }

  return 0;
}
