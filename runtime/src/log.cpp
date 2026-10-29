#include "log.h"
#include "os.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace wet
{
    static std::mutex s_logMutex;
    static FILE* s_logFile = nullptr;
    static uint64_t s_startNs = 0;

    void LogInit(const char* path)
    {
        s_logFile = std::fopen(path, "w");
        s_startNs = os::MonotonicNs();
    }

    static void Write(const char* tag, const char* fmt, va_list args)
    {
        char msg[2048];
        std::vsnprintf(msg, sizeof(msg), fmt, args);
        double t = double(os::MonotonicNs() - s_startNs) / 1e9;

        std::lock_guard lock(s_logMutex);
        std::fprintf(stderr, "[%9.4f][%5u]%s %s\n", t, os::CurrentHostThreadId(), tag, msg);
        if (s_logFile)
        {
            std::fprintf(s_logFile, "[%9.4f][%5u]%s %s\n", t, os::CurrentHostThreadId(), tag, msg);
            std::fflush(s_logFile); // every line reaches disk even if we crash right after
        }
    }

    void Log(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        Write("", fmt, args);
        va_end(args);
    }

    void LogFatal(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        Write(" FATAL:", fmt, args);
        va_end(args);
        LogFlush();
    }

    void LogFlush()
    {
        std::fflush(stderr);
        if (s_logFile)
            std::fflush(s_logFile);
    }
}
