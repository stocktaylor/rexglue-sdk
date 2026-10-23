/**
 * @file        ui/webgpu/webgpu_device_thread.cpp
 * @brief       The thread that owns the WebGPU device and makes every WebGPU call
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/webgpu/device_thread.h>

#include <chrono>
#include <future>

#include <rex/logging.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#endif

namespace rex::ui::webgpu {

#if defined(__EMSCRIPTEN__)

namespace {

struct StartState {
  std::mutex mutex;
  std::condition_variable ready;
  bool done = false;
};

}  // namespace

std::unique_ptr<WebGPUDeviceThread> WebGPUDeviceThread::Start(const std::string& canvas_selector) {
  auto self = std::unique_ptr<WebGPUDeviceThread>(new WebGPUDeviceThread);
  self->canvas_selector_ = canvas_selector;
  self->queue_ = em_proxying_queue_create();

  // The canvas can be handed to the new thread only by the thread that owns
  // it: with PROXY_TO_PTHREAD, Emscripten gives it to main's pthread (the UI
  // thread) by default; otherwise the browser's main thread still has it. Try
  // the calling thread first, then the browser's main thread.
  StartState state;
  struct CreateArgs {
    WebGPUDeviceThread* self;
    StartState* state;
    int result;
  } args = {self.get(), &state, -1};
  auto create = [](void* arg) {
    auto* args = static_cast<CreateArgs*>(arg);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (!args->self->canvas_selector_.empty()) {
      emscripten_pthread_attr_settransferredcanvases(&attr, args->self->canvas_selector_.c_str());
    }
    args->result = pthread_create(&args->self->thread_, &attr, ThreadMain, args->self);
    pthread_attr_destroy(&attr);
  };
  // ThreadMain reports the device through this.
  self->start_state_ = &state;
  create(&args);
  if (args.result != 0 && !pthread_equal(pthread_self(), emscripten_main_runtime_thread_id())) {
    emscripten_proxy_sync(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(),
                          create, &args);
  }
  if (args.result != 0) {
    REXLOG_ERROR("WebGPU: couldn't start the device thread ({})", args.result);
    em_proxying_queue_destroy(self->queue_);
    self->queue_ = nullptr;
    return nullptr;
  }
  {
    std::unique_lock<std::mutex> lock(state.mutex);
    state.ready.wait(lock, [&] { return state.done; });
  }
  self->start_state_ = nullptr;
  if (!self->device_) {
    // The thread stays in its event loop; stop it.
    self->Stop();
    return nullptr;
  }
  return self;
}

void* WebGPUDeviceThread::ThreadMain(void* arg) {
  auto* self = static_cast<WebGPUDeviceThread*>(arg);
  emscripten_set_thread_name(pthread_self(), "WebGPU device");
  WebGPUDevice::CreateAsync([self](std::unique_ptr<WebGPUDevice> device) {
    self->device_ = std::move(device);
    StartState& state = *static_cast<StartState*>(self->start_state_);
    std::lock_guard<std::mutex> lock(state.mutex);
    state.done = true;
    state.ready.notify_all();
  });
  // Live on in the event loop: requests complete, proxied work runs and the
  // canvas presents there.
  emscripten_exit_with_live_runtime();
}

void WebGPUDeviceThread::Stop() {
  if (!queue_) {
    return;
  }
  // Ends the thread from inside its event loop; the sync call then reports
  // that the thread exited, which is expected.
  emscripten_proxy_sync(queue_, thread_, [](void*) { pthread_exit(nullptr); }, nullptr);
  pthread_join(thread_, nullptr);
  em_proxying_queue_destroy(queue_);
  queue_ = nullptr;
}

WebGPUDeviceThread::~WebGPUDeviceThread() {
  if (device_) {
    Call([this] { device_.reset(); });
  }
  Stop();
}

bool WebGPUDeviceThread::IsCurrentThread() const {
  return pthread_equal(pthread_self(), thread_);
}

void WebGPUDeviceThread::Call(const std::function<void()>& function) {
  if (IsCurrentThread()) {
    function();
    return;
  }
  emscripten_proxy_sync(
      queue_, thread_, [](void* arg) { (*static_cast<const std::function<void()>*>(arg))(); },
      const_cast<std::function<void()>*>(&function));
}

void WebGPUDeviceThread::Post(std::function<void()> function) {
  auto* task = new std::function<void()>(std::move(function));
  if (!emscripten_proxy_async(
          queue_, thread_,
          [](void* arg) {
            auto* task = static_cast<std::function<void()>*>(arg);
            (*task)();
            delete task;
          },
          task)) {
    delete task;
  }
}

#else  // Native.

std::unique_ptr<WebGPUDeviceThread> WebGPUDeviceThread::Start(const std::string& canvas_selector) {
  auto self = std::unique_ptr<WebGPUDeviceThread>(new WebGPUDeviceThread);
  self->canvas_selector_ = canvas_selector;
  std::promise<bool> created;
  std::future<bool> created_future = created.get_future();
  self->thread_ = std::thread([raw = self.get(), &created] {
    raw->device_ = WebGPUDevice::Create();
    created.set_value(raw->device_ != nullptr);
    raw->ThreadMain();
  });
  if (!created_future.get()) {
    return nullptr;  // The destructor stops the thread.
  }
  return self;
}

void WebGPUDeviceThread::ThreadMain() {
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    // Wake at least every millisecond to give Dawn's events a turn, as the
    // browser's event loop would.
    wake_.wait_for(lock, std::chrono::milliseconds(1),
                   [this] { return stopping_ || !tasks_.empty(); });
    while (!tasks_.empty()) {
      std::function<void()> task = std::move(tasks_.front());
      tasks_.pop_front();
      lock.unlock();
      task();
      lock.lock();
    }
    if (stopping_) {
      break;
    }
    if (device_) {
      lock.unlock();
      wgpuInstanceProcessEvents(device_->instance());
      lock.lock();
    }
  }
}

WebGPUDeviceThread::~WebGPUDeviceThread() {
  if (!thread_.joinable()) {
    return;
  }
  Call([this] { device_.reset(); });
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  thread_.join();
}

bool WebGPUDeviceThread::IsCurrentThread() const {
  return std::this_thread::get_id() == thread_.get_id();
}

void WebGPUDeviceThread::Call(const std::function<void()>& function) {
  if (IsCurrentThread()) {
    function();
    return;
  }
  std::promise<void> done;
  std::future<void> done_future = done.get_future();
  Post([&function, &done] {
    function();
    done.set_value();
  });
  done_future.wait();
}

void WebGPUDeviceThread::Post(std::function<void()> function) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.push_back(std::move(function));
  }
  wake_.notify_one();
}

#endif

}  // namespace rex::ui::webgpu
