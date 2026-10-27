/**
 * @file        filesystem/devices/browser_file_device.cpp
 * @brief       Game data from files the user picked in the browser
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/filesystem/devices/browser_file_device.h>

#include <algorithm>
#include <cstring>

#include <rex/logging.h>
#include <rex/math.h>
#include <rex/string.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten/em_js.h>

// The picked files, as browser_game_files.js stores them on each worker:
// globalThis.rexGameFiles = [{path: "relative/path", file: File}, ...].
// Sizes and offsets are doubles (exact to 2^53), pointers BigInts (memory64).
// clang-format off
EM_JS(int, rex_game_file_count, (), {
  return typeof rexGameFiles === 'object' && rexGameFiles ? rexGameFiles.length : -1;
});
EM_JS(int, rex_game_file_path, (int index, char* buffer, int size), {
  return stringToUTF8(rexGameFiles[index].path, Number(buffer), size);
});
EM_JS(double, rex_game_file_size, (int index), {
  return rexGameFiles[index].file.size;
});
EM_JS(double, rex_game_file_read, (int index, double offset, char* buffer, double length), {
  var file = rexGameFiles[index].file;
  var end = Math.min(file.size, offset + length);
  if (!(offset < end)) {
    return 0;
  }
  var data = new FileReaderSync().readAsArrayBuffer(file.slice(offset, end));
  HEAPU8.set(new Uint8Array(data), Number(buffer));
  return data.byteLength;
});
// clang-format on
#endif

namespace rex::filesystem {

BrowserFileDevice::BrowserFileDevice(const std::string_view mount_path)
    : Device(mount_path), name_("BROWSER") {}

BrowserFileDevice::~BrowserFileDevice() = default;

bool BrowserFileDevice::IsAvailable() {
#if defined(__EMSCRIPTEN__)
  return rex_game_file_count() >= 0;
#else
  return false;
#endif
}

bool BrowserFileDevice::Initialize() {
#if defined(__EMSCRIPTEN__)
  int count = rex_game_file_count();
  if (count < 0) {
    REXFS_ERROR("BrowserFileDevice: the page provided no game files");
    return false;
  }
  auto root = std::make_unique<BrowserFileEntry>(this, nullptr, "", -1);
  root->attributes_ = kFileAttributeDirectory;
  for (int index = 0; index < count; ++index) {
    char path_buffer[1024];
    rex_game_file_path(index, path_buffer, sizeof(path_buffer));
    auto parts = rex::string::utf8_split_path(path_buffer);
    if (parts.empty()) {
      continue;
    }
    BrowserFileEntry* parent = root.get();
    for (size_t i = 0; i < parts.size(); ++i) {
      bool is_file = i + 1 == parts.size();
      auto* existing = static_cast<BrowserFileEntry*>(parent->GetChild(parts[i]));
      if (existing && !is_file) {
        parent = existing;
        continue;
      }
      if (existing) {
        // A duplicate (only possible with case-insensitive collisions).
        break;
      }
      auto entry = std::make_unique<BrowserFileEntry>(
          this, parent, rex::string::utf8_join_guest_paths(parent->path(), parts[i]),
          is_file ? index : -1);
      // Set to January 1, 1970 (UTC) in 100-nanosecond intervals.
      entry->create_timestamp_ = 10000 * 11644473600000LL;
      entry->access_timestamp_ = entry->create_timestamp_;
      entry->write_timestamp_ = entry->create_timestamp_;
      if (is_file) {
        size_t size = size_t(rex_game_file_size(index));
        entry->attributes_ = kFileAttributeReadOnly;
        entry->size_ = size;
        entry->allocation_size_ = rex::round_up(size, size_t(bytes_per_sector()));
        ++file_count_;
        total_file_size_ += size;
      } else {
        entry->attributes_ = kFileAttributeDirectory | kFileAttributeReadOnly;
      }
      auto* raw_entry = entry.get();
      parent->children_.emplace_back(std::move(entry));
      parent = raw_entry;
    }
  }
  root_entry_ = std::move(root);
  REXFS_INFO("BrowserFileDevice: {} picked files, {} MB", file_count_, total_file_size_ >> 20);
  return true;
#else
  return false;
#endif
}

void BrowserFileDevice::Dump(string::StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  string_buffer->AppendFormat("{}: {} picked files, {} bytes\n", mount_path(), file_count_,
                              total_file_size_);
}

Entry* BrowserFileDevice::ResolvePath(const std::string_view path) {
  return root_entry_ ? root_entry_->ResolvePath(path) : nullptr;
}

BrowserFileEntry::BrowserFileEntry(Device* device, Entry* parent, const std::string_view path,
                                   int32_t file_index)
    : Entry(device, parent, path), file_index_(file_index) {}

BrowserFileEntry::~BrowserFileEntry() = default;

X_STATUS BrowserFileEntry::Open(uint32_t desired_access, File** out_file) {
  if (file_index_ < 0) {
    return X_STATUS_FILE_IS_A_DIRECTORY;
  }
  *out_file = new BrowserFile(desired_access, this);
  return X_STATUS_SUCCESS;
}

BrowserFile::BrowserFile(uint32_t file_access, BrowserFileEntry* entry)
    : File(file_access, entry), entry_(entry) {}

BrowserFile::~BrowserFile() = default;

void BrowserFile::Destroy() {
  delete this;
}

X_STATUS BrowserFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                               size_t* out_bytes_read) {
  if (byte_offset >= entry_->size()) {
    return X_STATUS_END_OF_FILE;
  }
#if defined(__EMSCRIPTEN__)
  size_t length = std::min(buffer.size(), entry_->size() - byte_offset);
  *out_bytes_read =
      size_t(rex_game_file_read(entry_->file_index(), double(byte_offset),
                                reinterpret_cast<char*>(buffer.data()), double(length)));
  return X_STATUS_SUCCESS;
#else
  (void)buffer;
  *out_bytes_read = 0;
  return X_STATUS_UNSUCCESSFUL;
#endif
}

}  // namespace rex::filesystem
