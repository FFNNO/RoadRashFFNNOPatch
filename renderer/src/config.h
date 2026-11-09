#pragma once
#include <string>
#include "log.h"

enum class FilterMode {
    Nearest = 0,
    Bilinear = 1,
    SharpBilinear = 2
};

// [World] MipFilter: trilinear = upload the engine's PDAT mip chain and sample it with trilinear (or
// anisotropic) filtering; chosen = B4 behaviour (level 0 only, point sampling).
enum class MipFilter {
    Chosen = 0,
    Trilinear = 1
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
    bool subPixel = true;       // [World] SubPixel: draw world quads at their 16.16 corners
    MipFilter mipFilter = MipFilter::Trilinear;
    int anisotropy = 8;         // [World] Anisotropy: 2..16 = anisotropic filtering, 0 or 1 = off
    bool texturePackEnabled = true;
    std::wstring texturePackPath = L"texture-pack";
    int texturePackMaxMiB = 512;
    int texturePackDecodeBudgetMs = 4;
    bool texturePackDump = false;
    std::wstring texturePackDumpPath = L"texture-dump";
};

void ConfigInit(const std::wstring& iniFilePath);
const Config& GetConfig();
bool ToggleGpuWorld();
int ClampInternalHeight(int h);
std::wstring ResolveGamePath(const std::wstring& relOrAbsPath);
// Accepts F1..F12, ScrollLock, Pause, or a full decimal/hex code in 1..255.
// Anything else (including trailing garbage such as "0x7Bzz") returns defaultKey.
uint32_t ParseVirtualKey(const wchar_t* str, uint32_t defaultKey = 0x91);
bool ParseFilterMode(const wchar_t* str, FilterMode& outMode);
bool ParseMipFilter(const wchar_t* str, MipFilter& outMode);
int ClampAnisotropy(int value);
const char* FilterModeToString(FilterMode mode);
