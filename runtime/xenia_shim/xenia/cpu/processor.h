// WET runtime stand-in for Xenia's cpu::Processor: guest code is recompiled ahead
// of time, so the only things xenia-gpu needs are the memory and a way to run the
// title's graphics interrupt callback (implemented in xenia_bridge.cpp).
#ifndef XENIA_CPU_PROCESSOR_H_
#define XENIA_CPU_PROCESSOR_H_

#include <cstddef>
#include <cstdint>

#include "xenia/cpu/mmio_handler.h"
#include "xenia/base/byte_stream.h"
#include "xenia/memory.h"

namespace xe {
namespace cpu {

class ThreadState;  // opaque: the runtime's guest thread

class Processor {
 public:
  explicit Processor(Memory* memory) : memory_(memory) {}
  Memory* memory() const { return memory_; }
  // Calls guest `address` with up to 8 arguments on the given thread (an
  // interrupt: the thread's registers are preserved).
  uint64_t ExecuteInterrupt(ThreadState* thread_state, uint32_t address, uint64_t args[],
                            size_t arg_count);

 private:
  Memory* memory_;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_PROCESSOR_H_
