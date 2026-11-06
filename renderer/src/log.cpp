#include "log.h"
#include <cstdio>
#include <cstdarg>
#include <share.h>

static CRITICAL_SECTION g_logCs;
static bool g_logCsInitialized = false;
static FILE* g_logFile = nullptr;
static LogLevel g_minLogLevel = LogLevel::Info;

void LogInit(const std::wstring& logFilePath, LogLevel level) {
    if (!g_logCsInitialized) {
        InitializeCriticalSection(&g_logCs);
        g_logCsInitialized = true;
    }

    EnterCriticalSection(&g_logCs);
    g_minLogLevel = level;
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
    g_logFile = _wfsopen(logFilePath.c_str(), L"a+", _SH_DENYNO);
    LeaveCriticalSection(&g_logCs);
}

void LogClose() {
    if (g_logCsInitialized) {
        EnterCriticalSection(&g_logCs);
        if (g_logFile) {
            fclose(g_logFile);
            g_logFile = nullptr;
        }
        LeaveCriticalSection(&g_logCs);
        DeleteCriticalSection(&g_logCs);
        g_logCsInitialized = false;
    }
}

void LogWrite(LogLevel level, const char* fmt, ...) {
    if (level < g_minLogLevel) {
        return;
    }

    if (!g_logCsInitialized) {
        InitializeCriticalSection(&g_logCs);
        g_logCsInitialized = true;
    }

    EnterCriticalSection(&g_logCs);
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
    LeaveCriticalSection(&g_logCs);
}
