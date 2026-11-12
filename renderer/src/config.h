#pragma once
#include <string>
#include "log.h"
#include "widescreen.h"

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
    bool keepThinQuads = true;  // [World] KeepThinQuads: draw one-scanline textured quads with 16.16 height
    MipFilter mipFilter = MipFilter::Trilinear;
    int anisotropy = 8;         // [World] Anisotropy: 2..16 = anisotropic filtering, 0 or 1 = off
    bool distanceFade = true;        // [World] DistanceFade: fade the farthest road slices into the sky
    float distanceFadeStart = 0.8f;  // [World] DistanceFadeStart: fade start as a fraction of the far depth
    int drawSegments = 72;           // [World] DrawSegments: road segments drawn ahead, 48..72 (48 = engine)
    bool drawSegmentsAdjusted = false;  // the ini value was clamped or not a number
    WidescreenSetting widescreen;          // [World] Widescreen: off (default) | auto | 320..640 even
    bool invalidWidescreenValue = false;   // the ini value was not understood; widescreen is off
    std::wstring rawWidescreenString;      // the ini value when invalidWidescreenValue
    bool texturePackEnabled = true;
    std::wstring texturePackPath = L"texture-pack";
    int texturePackMaxMiB = 512;
    int texturePackDecodeBudgetMs = 4;
    bool texturePackPreload = true;  // [TexturePack] Preload: decode the whole pack in the background at Init
    bool texturePackDump = false;
    std::wstring texturePackDumpPath = L"texture-dump";
    bool saveRedirectEnabled = true;            // [Save] Enabled: fixed save file, no dialogs, toast
    std::wstring saveFile = L"Partida 1.RRS";   // [Save] File: relative to the game folder or absolute
};

void ConfigInit(const std::wstring& iniFilePath);
// Reads every key of iniFilePath into cfg (missing keys keep their defaults). ConfigInit applies it
// to the global config; tests call it on a local Config.
void ParseConfigFile(const std::wstring& iniFilePath, Config& cfg);
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
// [World] DistanceFadeStart: a decimal fraction in [0, 1). Anything else sets 0.8 and returns false.
bool ParseDistanceFadeStart(const wchar_t* str, float& out);
const char* FilterModeToString(FilterMode mode);
