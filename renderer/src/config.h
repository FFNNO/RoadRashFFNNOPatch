#pragma once
#include <string>
#include "log.h"

enum class FilterMode {
    Nearest = 0,
    Bilinear = 1,
    SharpBilinear = 2
};

struct Config {
    bool enabled = true;
    LogLevel logLevel = LogLevel::Info;
    bool presenterEnabled = true;
    FilterMode filter = FilterMode::Bilinear;
    bool vsync = false;
    bool unknownFilterWarning = false;
    std::wstring rawFilterString;
    bool captureEnabled = false;
    int captureAutoDumpFrame = 0;
    bool gpuWorld = false;
    int internalWidth = 1440;
    int internalHeight = 1080;
    uint32_t toggleKey = 0x91;  // VK_SCROLL (Scroll Lock)
};

void ConfigInit(const std::wstring& iniFilePath);
const Config& GetConfig();
bool ToggleGpuWorld();
int ClampInternalHeight(int h);
// Accepts F1..F12, ScrollLock, Pause, or a full decimal/hex code in 1..255.
// Anything else (including trailing garbage such as "0x7Bzz") returns defaultKey.
uint32_t ParseVirtualKey(const wchar_t* str, uint32_t defaultKey = 0x91);
bool ParseFilterMode(const wchar_t* str, FilterMode& outMode);
const char* FilterModeToString(FilterMode mode);
