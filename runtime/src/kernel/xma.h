// XMA decoder hardware (silent stand-in), see xma.cpp.
#pragma once
#include <cstdint>

namespace wet
{
    void InitXma();
    // A store to the APU registers (0x7FEA0000-0x7FEAFFFF); `value` as the device sees it.
    bool XmaMmioWrite(uint32_t address, uint32_t value);
    uint32_t XmaAllocateContext();
    void XmaReleaseContext(uint32_t guest);
}
