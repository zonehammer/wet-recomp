// WET runtime stand-in for Xenia's xe::Memory, exposing only what xenia-gpu uses.
//
// Physical memory (512 MB) is the runtime's guest view at 0xA0000000 (the 0xA, 0xC
// and 0xE views alias the same pages), so a physical address translates with a
// plain offset. Write watches on physical pages (texture / vertex cache
// invalidation) are implemented by the runtime in runtime/src/gpu/xenia_bridge.cpp.
#ifndef XENIA_MEMORY_H_
#define XENIA_MEMORY_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "xenia/base/memory.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/mmio_handler.h"

namespace xe {

class Memory {
 public:
  Memory(uint8_t* virtual_membase, uint8_t* physical_membase)
      : virtual_membase_(virtual_membase), physical_membase_(physical_membase) {}

  inline uint8_t* virtual_membase() const { return virtual_membase_; }
  inline uint8_t* physical_membase() const { return physical_membase_; }

  template <typename T = uint8_t*>
  inline T TranslateVirtual(uint32_t guest_address) const {
    return reinterpret_cast<T>(virtual_membase_ + guest_address);
  }

  template <typename T = uint8_t*>
  inline T TranslatePhysical(uint32_t guest_address) const {
    return reinterpret_cast<T>(physical_membase_ + (guest_address & 0x1FFFFFFF));
  }

  uint32_t HostToGuestVirtual(const void* host_address) const {
    return uint32_t(reinterpret_cast<const uint8_t*>(host_address) - virtual_membase_);
  }

  // MMIO is plain memory in the runtime; the bridge polls the registers it needs.
  bool AddVirtualMappedRange(uint32_t, uint32_t, uint32_t, void*, cpu::MMIOReadCallback,
                             cpu::MMIOWriteCallback) {
    return true;
  }

  typedef std::pair<uint32_t, uint32_t> (*PhysicalMemoryInvalidationCallback)(
      void* context_ptr, uint32_t physical_address_start, uint32_t length, bool exact_range);

  // Implemented in the runtime (xenia_bridge.cpp).
  void* RegisterPhysicalMemoryInvalidationCallback(PhysicalMemoryInvalidationCallback callback,
                                                   void* callback_context);
  void UnregisterPhysicalMemoryInvalidationCallback(void* callback_handle);
  void EnablePhysicalMemoryAccessCallbacks(uint32_t physical_address, uint32_t length,
                                           bool enable_invalidation_notifications,
                                           bool enable_data_providers);

 private:
  uint8_t* virtual_membase_;
  uint8_t* physical_membase_;
};

}  // namespace xe

#endif  // XENIA_MEMORY_H_
