/**
 * @file        filesystem/devices/browser_file_device.h
 * @brief       Game data from files the user picked in the browser
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <span>
#include <string>

#include <rex/filesystem/device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>

namespace rex::filesystem {

// WebAssembly in the browser: a read-only device over the game folder the user
// picked in the page (<input webkitdirectory> or showDirectoryPicker). The page
// hands the picked File objects to every worker (browser_game_files.js), and
// reads take slices of them with FileReaderSync on the calling thread, so the
// data stays on the user's disk and is read only where the game reads it.
// Elsewhere, and when the page picked nothing, IsAvailable is false.
class BrowserFileDevice : public Device {
 public:
  explicit BrowserFileDevice(const std::string_view mount_path);
  ~BrowserFileDevice() override;

  // Whether the page provided game files to this thread.
  static bool IsAvailable();

  bool Initialize() override;
  void Dump(string::StringBuffer* string_buffer) override;
  Entry* ResolvePath(const std::string_view path) override;

  bool is_read_only() const override { return true; }

  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 255; }

  uint32_t total_allocation_units() const override { return 128 * 1024; }
  uint32_t available_allocation_units() const override { return 0; }
  uint32_t sectors_per_allocation_unit() const override { return 1; }
  uint32_t bytes_per_sector() const override { return 0x200; }

 private:
  std::string name_;
  std::unique_ptr<Entry> root_entry_;
  uint64_t file_count_ = 0;
  uint64_t total_file_size_ = 0;
};

class BrowserFileEntry : public Entry {
 public:
  // file_index is the picked file's index, or -1 for a folder.
  BrowserFileEntry(Device* device, Entry* parent, const std::string_view path, int32_t file_index);
  ~BrowserFileEntry() override;

  int32_t file_index() const { return file_index_; }

  X_STATUS Open(uint32_t desired_access, File** out_file) override;

 private:
  friend class BrowserFileDevice;

  int32_t file_index_;
};

class BrowserFile : public File {
 public:
  BrowserFile(uint32_t file_access, BrowserFileEntry* entry);
  ~BrowserFile() override;

  void Destroy() override;

  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset, size_t* out_bytes_read) override;
  X_STATUS WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                     size_t* out_bytes_written) override {
    (void)buffer;
    (void)byte_offset;
    (void)out_bytes_written;
    return X_STATUS_ACCESS_DENIED;
  }
  X_STATUS SetLength(size_t length) override {
    (void)length;
    return X_STATUS_ACCESS_DENIED;
  }

 private:
  BrowserFileEntry* entry_;
};

}  // namespace rex::filesystem
