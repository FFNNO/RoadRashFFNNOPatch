#pragma once
#include <windows.h>
#include <string>

enum class LogLevel {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3
};

void LogInit(const std::wstring& logFilePath, LogLevel level);
void LogClose();
void LogWrite(LogLevel level, const char* fmt, ...);

#define LOG_DEBUG(fmt, ...) LogWrite(LogLevel::Debug, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  LogWrite(LogLevel::Info,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  LogWrite(LogLevel::Warn,  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) LogWrite(LogLevel::Error, fmt, ##__VA_ARGS__)
