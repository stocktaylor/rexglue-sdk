/**
 * @file        ui/webgpu/device_thread.h
 * @brief       The thread that owns the WebGPU device and makes every WebGPU call
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <rex/ui/webgpu/device.h>

#if defined(__EMSCRIPTEN__)
#include <pthread.h>
#endif

struct em_proxying_queue;

namespace rex::ui::webgpu {

// Callback mode for asynchronous requests made on the device thread, so that
// their callbacks run on it too: from its event loop in the browser, from its
// event processing natively (Dawn's spontaneous callbacks may come from its
// own threads).
#if defined(__EMSCRIPTEN__)
inline constexpr WGPUCallbackMode kDeviceThreadCallbackMode = WGPUCallbackMode_AllowSpontaneous;
#else
inline constexpr WGPUCallbackMode kDeviceThreadCallbackMode = WGPUCallbackMode_AllowProcessEvents;
#endif

// In the browser a WebGPU device and everything created from it belong to the
// worker that created them, buffer maps and other requests complete only when
// that worker returns to its event loop, and a canvas shows a frame only then
// too. So one thread owns the device: every WebGPU call, releases included, is
// made on it, through Call (run there and wait) or Post (queue and return).
// Nothing on it may wait for the GPU; asynchronous results come back through
// callbacks, and other threads block for them if they must.
//
// In the browser the thread is created from the browser's main thread, the only
// one that can hand it the page's canvas (as an OffscreenCanvas), and it lives
// in its event loop, receiving work through an Emscripten proxying queue.
// Natively it is an ordinary thread running a task queue and Dawn's event
// processing, so the native build exercises the same threading.
class WebGPUDeviceThread {
 public:
  // Starts the thread and creates the device on it, waiting for both. Call from
  // any thread except, in the browser, the main one. canvas_selector names the
  // canvas the thread takes over in the browser (empty for none); natively
  // it's unused.
  static std::unique_ptr<WebGPUDeviceThread> Start(const std::string& canvas_selector = "");

  // Destroys the device on the thread and stops it.
  ~WebGPUDeviceThread();

  WebGPUDeviceThread(const WebGPUDeviceThread&) = delete;
  WebGPUDeviceThread& operator=(const WebGPUDeviceThread&) = delete;

  // Only for use on the device thread.
  WebGPUDevice& device() const { return *device_; }
  const std::string& canvas_selector() const { return canvas_selector_; }

  bool IsCurrentThread() const;

  // Runs function on the device thread and returns once it has run. Runs it
  // directly when already on the device thread.
  void Call(const std::function<void()>& function);
  // Queues function to run on the device thread, after anything posted or
  // called before it, and returns at once.
  void Post(std::function<void()> function);

 private:
  WebGPUDeviceThread() = default;

  std::unique_ptr<WebGPUDevice> device_;
  std::string canvas_selector_;

#if defined(__EMSCRIPTEN__)
  static void* ThreadMain(void* self);
  // Ends the thread from its event loop.
  void Stop();
  pthread_t thread_ = {};
  em_proxying_queue* queue_ = nullptr;
  // While starting: where ThreadMain reports the device.
  void* start_state_ = nullptr;
#else
  void ThreadMain();
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::function<void()>> tasks_;
  bool stopping_ = false;
#endif
};

}  // namespace rex::ui::webgpu
