#include "config.h"
#include <windows.h>
#include <cwctype>

static Config g_config;

static std::wstring ToLowerTrimmed(const wchar_t* str) {
    std::wstring s = str ? str : L"";
    size_t start = 0;
    while (start < s.size() && std::iswspace(s[start])) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::iswspace(s[end - 1])) {
        --end;
    }
    s = s.substr(start, end - start);
    for (auto& c : s) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return s;
}

static LogLevel ParseLogLevel(const wchar_t* str) {
    std::wstring s = ToLowerTrimmed(str);
    if (s == L"debug") return LogLevel::Debug;
    if (s == L"warn")  return LogLevel::Warn;
    if (s == L"error") return LogLevel::Error;
    return LogLevel::Info;
}

bool ParseFilterMode(const wchar_t* str, FilterMode& outMode) {
    std::wstring s = ToLowerTrimmed(str);
    if (s == L"nearest" || s == L"point") {
        outMode = FilterMode::Nearest;
        return true;
    }
    if (s == L"bilinear" || s == L"linear") {
        outMode = FilterMode::Bilinear;
        return true;
    }
    if (s == L"sharp-bilinear" || s == L"sharp_bilinear" || s == L"sharpbilinear") {
        outMode = FilterMode::SharpBilinear;
        return true;
    }
    outMode = FilterMode::Bilinear;
    return false;
}

const char* FilterModeToString(FilterMode mode) {
    switch (mode) {
        case FilterMode::Nearest:       return "nearest";
        case FilterMode::Bilinear:      return "bilinear";
        case FilterMode::SharpBilinear: return "sharp-bilinear";
    }
    return "bilinear";
}

void ConfigInit(const std::wstring& iniFilePath) {
    UINT enabledVal = GetPrivateProfileIntW(L"General", L"Enabled", 1, iniFilePath.c_str());
    g_config.enabled = (enabledVal != 0);

    wchar_t levelBuf[64] = {0};
    GetPrivateProfileStringW(L"General", L"LogLevel", L"info", levelBuf, 64, iniFilePath.c_str());
    g_config.logLevel = ParseLogLevel(levelBuf);

    UINT presenterEnabledVal = GetPrivateProfileIntW(L"Presenter", L"Enabled", 1, iniFilePath.c_str());
    g_config.presenterEnabled = (presenterEnabledVal != 0);

    wchar_t generalFilterBuf[64] = {0};
    GetPrivateProfileStringW(L"General", L"Filter", L"bilinear", generalFilterBuf, 64, iniFilePath.c_str());

    wchar_t filterBuf[64] = {0};
    GetPrivateProfileStringW(L"Presenter", L"Filter", generalFilterBuf, filterBuf, 64, iniFilePath.c_str());
    if (!ParseFilterMode(filterBuf, g_config.filter)) {
        g_config.unknownFilterWarning = true;
        g_config.rawFilterString = filterBuf;
    } else {
        g_config.unknownFilterWarning = false;
        g_config.rawFilterString.clear();
    }

    UINT vsyncVal = GetPrivateProfileIntW(L"Presenter", L"VSync", 0, iniFilePath.c_str());
    g_config.vsync = (vsyncVal != 0);

    UINT captureEnabledVal = GetPrivateProfileIntW(L"Capture", L"Enabled", 0, iniFilePath.c_str());
    g_config.captureEnabled = (captureEnabledVal != 0);

    UINT autoDumpVal = GetPrivateProfileIntW(L"Capture", L"AutoDumpFrame", 0, iniFilePath.c_str());
    g_config.captureAutoDumpFrame = static_cast<int>(autoDumpVal);

    UINT generalGpuWorld = GetPrivateProfileIntW(L"General", L"GpuWorld", 0, iniFilePath.c_str());
    UINT presenterGpuWorld = GetPrivateProfileIntW(L"Presenter", L"GpuWorld", generalGpuWorld, iniFilePath.c_str());
    UINT worldGpuWorld = GetPrivateProfileIntW(L"World", L"GpuWorld", presenterGpuWorld, iniFilePath.c_str());
    g_config.gpuWorld = (worldGpuWorld != 0);

    INT generalHeight = static_cast<INT>(GetPrivateProfileIntW(L"General", L"InternalHeight", 1080, iniFilePath.c_str()));
    INT presenterHeight = static_cast<INT>(GetPrivateProfileIntW(L"Presenter", L"InternalHeight", generalHeight, iniFilePath.c_str()));
    INT worldHeight = static_cast<INT>(GetPrivateProfileIntW(L"World", L"InternalHeight", presenterHeight, iniFilePath.c_str()));
    g_config.internalHeight = ClampInternalHeight(worldHeight);
    g_config.internalWidth = (g_config.internalHeight * 4) / 3;
}

int ClampInternalHeight(int h) {
    if (h < 240) return 240;
    if (h > 4320) return 4320;
    return h;
}

bool ToggleGpuWorld() {
    g_config.gpuWorld = !g_config.gpuWorld;
    return g_config.gpuWorld;
}

const Config& GetConfig() {
    return g_config;
}
