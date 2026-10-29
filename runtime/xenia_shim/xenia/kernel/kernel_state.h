// WET runtime stand-in for Xenia's kernel::KernelState (title id only).
#ifndef XENIA_KERNEL_KERNEL_STATE_H_
#define XENIA_KERNEL_KERNEL_STATE_H_

#include <cstdint>

#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {

class UserModule {
 public:
  explicit UserModule(uint32_t title_id) : title_id_(title_id) {}
  uint32_t title_id() const { return title_id_; }

 private:
  uint32_t title_id_;
};

class KernelState {
 public:
  explicit KernelState(uint32_t title_id) : module_(title_id) {}
  UserModule* GetExecutableModule() { return &module_; }

 private:
  UserModule module_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_KERNEL_STATE_H_
