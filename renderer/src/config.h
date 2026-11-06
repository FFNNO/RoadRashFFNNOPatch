#pragma once
#include <string>
#include "log.h"

struct Config {
    bool enabled = true;
    LogLevel logLevel = LogLevel::Info;
};

void ConfigInit(const std::wstring& iniFilePath);
const Config& GetConfig();
