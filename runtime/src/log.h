#pragma once
#include <cstdint>

namespace wet
{
    void LogInit(const char* path);
    void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
    void LogFatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
    void LogFlush();

    // Implemented by the kernel: describes the current guest thread (id, last import, LR).
    void LogCrashContext();
}
