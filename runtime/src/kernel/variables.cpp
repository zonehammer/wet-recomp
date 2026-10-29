// Kernel variables imported by the title, the KeTimeStampBundle updater and the timebase.
#include "kernel.h"
#include "init.h"
#include "module.h"
#include "os.h"

#include <thread>

namespace wet
{
    static uint64_t s_timebaseStartNs = 0;
    static uint32_t s_timeStampBundle = 0;

    uint64_t CurrentFileTime() { return os::SystemTimeFileTime(); }

    static uint32_t Block(uint32_t size)
    {
        return SystemAlloc(size, 0x10);
    }

    static void UpdateTimeStampBundle()
    {
        // X_TIME_STAMP_BUNDLE { be64 interrupt_time; be64 system_time; be32 tick_count; }
        uint64_t sinceStart = os::MonotonicNs() - s_timebaseStartNs;
        St64(s_timeStampBundle + 0x00, sinceStart / 100);
        St64(s_timeStampBundle + 0x08, CurrentFileTime());
        St32(s_timeStampBundle + 0x10, uint32_t(sinceStart / 1000000));
    }

    void InitKernelVariables()
    {
        s_timebaseStartNs = os::MonotonicNs();

        for (auto& imp : g_module.imports)
        {
            if (!imp.variable)
                continue;
            const std::string& n = imp.name;
            uint32_t value = 0;

            if (n == "XboxHardwareInfo")
            {
                // { be32 Flags; u8 NumberOfProcessors; u8 PCIBridgeRevisionID; u8 Reserved[6]; be16 BldrMagic; be16 BldrFlags; }
                value = Block(0x10);
                St32(value, 0x20);
                St8(value + 4, 6);
            }
            else if (n == "XboxKrnlVersion")
            {
                value = Block(8);
                St16(value + 0, 2);       // major
                St16(value + 2, 0);       // minor
                St16(value + 4, 17559);   // build
                St16(value + 6, 0);       // qfe
            }
            else if (n == "KeTimeStampBundle")
            {
                value = s_timeStampBundle = Block(0x18);
                UpdateTimeStampBundle();
            }
            else if (n == "ExLoadedCommandLine")
            {
                value = Block(0x400);
                WriteCString(value, "\"default.xex\"");
            }
            else if (n == "XexExecutableModuleHandle")
            {
                // Pointer to a cell holding the module handle (the LDR entry).
                value = Block(4);
                St32(value, g_module.ldrEntry);
            }
            else if (n == "VdGpuClockInMHz")
            {
                value = Block(4);
                St32(value, 500);
            }
            else if (n == "ExThreadObjectType" || n == "ExEventObjectType" || n == "ExSemaphoreObjectType" ||
                     n == "ExMutantObjectType" || n == "ExTimerObjectType" || n == "IoFileObjectType")
            {
                value = Block(0x40);  // object-type descriptor; only compared by address
            }
            else
            {
                // KeDebugMonitorData, KeCertMonitorData, VdGlobalDevice, VdGlobalXamDevice,
                // VdHSIOCalibrationLock and anything new: zeroed storage.
                value = Block(0x100);
            }

            St32(imp.slot, value);
            Log("Kernel variable %s!%s -> 0x%08X", imp.library.c_str(), n.c_str(), value);
        }

        if (s_timeStampBundle)
        {
            std::thread([] {
                for (;;)
                {
                    UpdateTimeStampBundle();
                    os::SleepNs(1000000);
                }
            }).detach();
        }
    }
}

// 50 MHz Xbox 360 timebase (mftb), see runtime/include/wet_ppc_hooks.h.
uint64_t WetQueryTimebase()
{
    return (wet::os::MonotonicNs() - wet::s_timebaseStartNs) / 20;
}
