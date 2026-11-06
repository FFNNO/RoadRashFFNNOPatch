#include "config.h"
#include <windows.h>
#include <cwctype>

static Config g_config;

static LogLevel ParseLogLevel(const wchar_t* str) {
    std::wstring s = str;
    for (auto& c : s) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    if (s == L"debug") return LogLevel::Debug;
    if (s == L"warn")  return LogLevel::Warn;
    if (s == L"error") return LogLevel::Error;
    return LogLevel::Info;
}

void ConfigInit(const std::wstring& iniFilePath) {
    UINT enabledVal = GetPrivateProfileIntW(L"General", L"Enabled", 1, iniFilePath.c_str());
    g_config.enabled = (enabledVal != 0);

    wchar_t levelBuf[64] = {0};
    GetPrivateProfileStringW(L"General", L"LogLevel", L"info", levelBuf, 64, iniFilePath.c_str());
    g_config.logLevel = ParseLogLevel(levelBuf);
}

const Config& GetConfig() {
    return g_config;
}
