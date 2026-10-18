/**
 * @file        rex/core/memory_wasm.cpp
 * @brief       WebAssembly backend for rex::memory page management
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>
#if REX_PLATFORM_WASM

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_set>

#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/utils.h>

// WebAssembly linear memory can't be reserved, protected or mapped twice. Pages
// are handed out from malloc'd blocks (fresh linear memory is already zero),
// "committing" memory inside a block is a no-op, decommitting zeroes it so a
// later commit sees fresh pages as on other hosts, and protection is ignored:
// nothing ever faults. The guest address space is a single file mapping block
// (Memory::Initialize); its aliased ranges are handled in guest_layout.h.

namespace rex {
namespace memory {

namespace {

constexpr size_t kPageSize = 4096;
// Big blocks (the guest address space) are aligned for wasm's 64 KB pages.
constexpr size_t kBlockAlignment = 64 * 1024;

std::mutex blocks_mutex;
// Blocks from AllocFixed(nullptr, ...): only these are freed on release.
std::unordered_set<void*> owned_blocks;

void* AllocateBlock(size_t length) {
  void* block = nullptr;
  if (posix_memalign(&block, kBlockAlignment, rex::round_up(length, kPageSize))) {
    return nullptr;
  }
  return block;
}

}  // namespace

size_t page_size() {
  return kPageSize;
}

size_t allocation_granularity() {
  return kPageSize;
}

bool IsWritableExecutableMemorySupported() {
  // There is no executable host memory to write: code is compiled ahead.
  return false;
}

void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type,
                 PageAccess access) {
  (void)allocation_type;
  (void)access;
  if (base_address) {
    // Inside a block handed out earlier: already backed.
    return base_address;
  }
  void* block = AllocateBlock(length);
  if (!block) {
    return nullptr;
  }
  // malloc may reuse freed memory, which unlike fresh pages isn't zero.
  std::memset(block, 0, length);
  std::lock_guard lock(blocks_mutex);
  owned_blocks.insert(block);
  return block;
}

bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type) {
  if (deallocation_type == DeallocationType::kDecommit) {
    std::memset(base_address, 0, length);
    return true;
  }
  std::lock_guard lock(blocks_mutex);
  if (owned_blocks.erase(base_address)) {
    std::free(base_address);
  }
  // Releasing part of a bigger block (a guest heap region) frees nothing.
  return true;
}

bool Protect(void* base_address, size_t length, PageAccess access, PageAccess* out_old_access) {
  (void)base_address;
  (void)length;
  (void)access;
  if (out_old_access) {
    *out_old_access = PageAccess::kReadWrite;
  }
  return true;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
  (void)base_address;
  (void)length;
  access_out = PageAccess::kReadWrite;
  return true;
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path, size_t length,
                                          PageAccess access, bool commit) {
  (void)path;
  (void)access;
  (void)commit;
  // The handle is the block itself. Not zeroed: fresh linear memory already
  // is, and touching gigabytes here would make the browser commit them.
  void* block = AllocateBlock(length);
  if (!block) {
    REXLOG_ERROR("CreateFileMappingHandle: can't allocate {} MB", length >> 20);
    return kFileMappingHandleInvalid;
  }
  return reinterpret_cast<FileMappingHandle>(block);
}

void CloseFileMappingHandle(FileMappingHandle handle, const std::filesystem::path& path) {
  (void)path;
  std::free(reinterpret_cast<void*>(handle));
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length, PageAccess access,
                  size_t file_offset) {
  (void)base_address;
  (void)length;
  (void)access;
  // A view can't be placed or aliased: it is the block at that offset.
  return reinterpret_cast<uint8_t*>(handle) + file_offset;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address, size_t length) {
  (void)handle;
  (void)base_address;
  (void)length;
  return true;
}

}  // namespace memory
}  // namespace rex

#endif  // REX_PLATFORM_WASM
