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
};

void ConfigInit(const std::wstring& iniFilePath);
const Config& GetConfig();
const char* FilterModeToString(FilterMode mode);
