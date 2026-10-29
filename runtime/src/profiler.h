// Sampling profiler (Windows; no-op elsewhere). See profiler.cpp.
#pragma once

namespace wet::profiler
{
    void Start(double seconds);     // 0 = until Toggle()
    void Toggle();                  // F10
    void StartFromEnv();            // WET_PROFILE=<seconds>[,<delay>]
}
