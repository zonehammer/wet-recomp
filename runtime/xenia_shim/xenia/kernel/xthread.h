// WET runtime stand-in for Xenia's guest threads as used by xenia-gpu: host
// threads that can call guest code (the command processor and vsync workers,
// which deliver graphics interrupts). Implemented in xenia_bridge.cpp.
#ifndef XENIA_KERNEL_XTHREAD_H_
#define XENIA_KERNEL_XTHREAD_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "xenia/base/threading.h"
#include "xenia/cpu/processor.h"
#include "xenia/xbox.h"

namespace xe {
namespace cpu {
class ThreadState;
}
namespace kernel {

class KernelState;

class XThread {
 public:
  virtual ~XThread() = default;
  static XThread* GetCurrentThread();
  static bool IsInThread(XThread* thread) { return thread && GetCurrentThread() == thread; }
  static bool IsInThread() { return GetCurrentThread() != nullptr; }

  void SetActiveCpu(uint32_t cpu);
  cpu::ThreadState* thread_state() const { return thread_state_; }

 protected:
  cpu::ThreadState* thread_state_ = nullptr;
};

class XHostThread : public XThread {
 public:
  XHostThread(KernelState* kernel_state, uint32_t stack_size, uint32_t creation_flags,
              std::function<int()> host_fn)
      : stack_size_(stack_size), host_fn_(std::move(host_fn)) {}
  ~XHostThread() override;

  void set_name(const std::string& name) { name_ = name; }
  void set_can_debugger_suspend(bool) {}
  X_STATUS Create();
  X_STATUS Wait(uint32_t wait_reason, uint32_t processor_mode, uint32_t alertable,
                uint64_t* opt_timeout);

  struct HostThreadHandle {
    void Resume() {}
    void Suspend() {}
  };
  HostThreadHandle* thread() { return &handle_; }

 private:
  static void Entry(void* self);
  uint32_t stack_size_;
  std::function<int()> host_fn_;
  std::string name_;
  HostThreadHandle handle_;
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

// Xenia's intrusive reference; a plain owning pointer is enough here.
template <typename T>
class object_ref {
 public:
  object_ref() = default;
  explicit object_ref(T* value) : value_(value) {}
  T* get() const { return value_.get(); }
  T* operator->() const { return value_.get(); }
  explicit operator bool() const { return bool(value_); }
  void reset() { value_.reset(); }

 private:
  std::shared_ptr<T> value_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XTHREAD_H_
