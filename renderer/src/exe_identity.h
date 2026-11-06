#pragma once
#include <string>

enum class ExeBuild {
    Unknown,
    Original,
    Patched
};

struct ExeIdentity {
    ExeBuild build = ExeBuild::Unknown;
    std::string sha256Hex;
    std::wstring exePath;
};

ExeIdentity IdentifyCurrentExe();
