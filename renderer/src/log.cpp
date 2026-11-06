#include "log.h"
#include <cstdio>
#include <cstdarg>
#include <share.h>

namespace {

struct LogLock {
    CRITICAL_SECTION cs;
    LogLock() {
        InitializeCriticalSection(&cs);
    }
};

LogLock g_logLock;
FILE* g_logFile = nullptr;
LogLevel g_minLogLevel = LogLevel::Info;

}  // namespace

void LogInit(const std::wstring& logFilePath, LogLevel level) {
    EnterCriticalSection(&g_logLock.cs);
    g_minLogLevel = level;
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
    g_logFile = _wfsopen(logFilePath.c_str(), L"a+", _SH_DENYNO);
    LeaveCriticalSection(&g_logLock.cs);
}

void LogClose() {
    EnterCriticalSection(&g_logLock.cs);
    if (g_logFile) {
        std::fflush(g_logFile);
        fclose(g_logFile);
        g_logFile = nullptr;
    }
    LeaveCriticalSection(&g_logLock.cs);
}

void LogWrite(LogLevel level, const char* fmt, ...) {
    if (level < g_minLogLevel) {
        return;
    }

    EnterCriticalSection(&g_logLock.cs);
    if (g_logFile) {
        SYSTEMTIME st;
        GetLocalTime(&st);

        const char* levelStr = "INFO";
        switch (level) {
            case LogLevel::Debug: levelStr = "DEBUG"; break;
            case LogLevel::Info:  levelStr = "INFO";  break;
            case LogLevel::Warn:  levelStr = "WARN";  break;
            case LogLevel::Error: levelStr = "ERROR"; break;
        }

        std::fprintf(g_logFile, "[%02d:%02d:%02d.%03d] [%s] ",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, levelStr);

        va_list args;
        va_start(args, fmt);
        std::vfprintf(g_logFile, fmt, args);
        va_end(args);

        std::fprintf(g_logFile, "\n");
        std::fflush(g_logFile);
    }
    LeaveCriticalSection(&g_logLock.cs);
}
