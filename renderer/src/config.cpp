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

bool ParseMipFilter(const wchar_t* str, MipFilter& outMode) {
    std::wstring s = ToLowerTrimmed(str);
    if (s == L"trilinear") {
        outMode = MipFilter::Trilinear;
        return true;
    }
    if (s == L"chosen") {
        outMode = MipFilter::Chosen;
        return true;
    }
    outMode = MipFilter::Trilinear;
    return false;
}

int ClampAnisotropy(int value) {
    if (value < 2) return 0;
    if (value > 16) return 16;
    return value;
}

const char* FilterModeToString(FilterMode mode) {
    switch (mode) {
        case FilterMode::Nearest:       return "nearest";
        case FilterMode::Bilinear:      return "bilinear";
        case FilterMode::SharpBilinear: return "sharp-bilinear";
    }
    return "bilinear";
}

uint32_t ParseVirtualKey(const wchar_t* str, uint32_t defaultKey) {
    std::wstring s = ToLowerTrimmed(str);
    if (s.empty()) return defaultKey;
    static const struct {
        const wchar_t* name;
        uint32_t vk;
    } kNamedKeys[] = {
        {L"f1", VK_F1},   {L"f2", VK_F2},   {L"f3", VK_F3},   {L"f4", VK_F4},
        {L"f5", VK_F5},   {L"f6", VK_F6},   {L"f7", VK_F7},   {L"f8", VK_F8},
        {L"f9", VK_F9},   {L"f10", VK_F10}, {L"f11", VK_F11}, {L"f12", VK_F12},
        {L"scrolllock", VK_SCROLL}, {L"scroll_lock", VK_SCROLL}, {L"scroll", VK_SCROLL},
        {L"pause", VK_PAUSE},
    };
    for (const auto& k : kNamedKeys) {
        if (s == k.name) return k.vk;
    }
    // Numeric codes must consume the whole string: "0x7Bzz" or "12abc" fall back to the default.
    if (!std::iswdigit(s[0])) return defaultKey;
    wchar_t* end = nullptr;
    const unsigned long val = std::wcstoul(s.c_str(), &end, 0);
    if (end == nullptr || end == s.c_str() || *end != L'\0') return defaultKey;
    if (val > 0 && val < 256) {
        return static_cast<uint32_t>(val);
    }
    return defaultKey;
}

void ParseConfigFile(const std::wstring& iniFilePath, Config& cfg) {
    UINT enabledVal = GetPrivateProfileIntW(L"General", L"Enabled", 1, iniFilePath.c_str());
    cfg.enabled = (enabledVal != 0);

    wchar_t levelBuf[64] = {0};
    GetPrivateProfileStringW(L"General", L"LogLevel", L"info", levelBuf, 64, iniFilePath.c_str());
    cfg.logLevel = ParseLogLevel(levelBuf);

    UINT presenterEnabledVal = GetPrivateProfileIntW(L"Presenter", L"Enabled", 1, iniFilePath.c_str());
    cfg.presenterEnabled = (presenterEnabledVal != 0);

    wchar_t generalFilterBuf[64] = {0};
    GetPrivateProfileStringW(L"General", L"Filter", L"bilinear", generalFilterBuf, 64, iniFilePath.c_str());

    wchar_t filterBuf[64] = {0};
    GetPrivateProfileStringW(L"Presenter", L"Filter", generalFilterBuf, filterBuf, 64, iniFilePath.c_str());
    if (!ParseFilterMode(filterBuf, cfg.filter)) {
        cfg.unknownFilterWarning = true;
        cfg.rawFilterString = filterBuf;
    } else {
        cfg.unknownFilterWarning = false;
        cfg.rawFilterString.clear();
    }

    UINT vsyncVal = GetPrivateProfileIntW(L"Presenter", L"VSync", 0, iniFilePath.c_str());
    cfg.vsync = (vsyncVal != 0);

    UINT captureEnabledVal = GetPrivateProfileIntW(L"Capture", L"Enabled", 0, iniFilePath.c_str());
    cfg.captureEnabled = (captureEnabledVal != 0);

    UINT autoDumpVal = GetPrivateProfileIntW(L"Capture", L"AutoDumpFrame", 0, iniFilePath.c_str());
    cfg.captureAutoDumpFrame = static_cast<int>(autoDumpVal);

    UINT generalGpuWorld = GetPrivateProfileIntW(L"General", L"GpuWorld", 0, iniFilePath.c_str());
    UINT presenterGpuWorld = GetPrivateProfileIntW(L"Presenter", L"GpuWorld", generalGpuWorld, iniFilePath.c_str());
    UINT worldGpuWorld = GetPrivateProfileIntW(L"World", L"GpuWorld", presenterGpuWorld, iniFilePath.c_str());
    cfg.gpuWorld = (worldGpuWorld != 0);

    INT generalHeight = static_cast<INT>(GetPrivateProfileIntW(L"General", L"InternalHeight", 1080, iniFilePath.c_str()));
    INT presenterHeight = static_cast<INT>(GetPrivateProfileIntW(L"Presenter", L"InternalHeight", generalHeight, iniFilePath.c_str()));
    INT worldHeight = static_cast<INT>(GetPrivateProfileIntW(L"World", L"InternalHeight", presenterHeight, iniFilePath.c_str()));
    cfg.internalHeight = ClampInternalHeight(worldHeight);
    cfg.internalWidth = (cfg.internalHeight * 4) / 3;

    wchar_t toggleKeyBuf[64] = {0};
    GetPrivateProfileStringW(L"World", L"ToggleKey", L"ScrollLock", toggleKeyBuf, 64, iniFilePath.c_str());
    cfg.toggleKey = ParseVirtualKey(toggleKeyBuf, VK_SCROLL);

    UINT subPixelVal = GetPrivateProfileIntW(L"World", L"SubPixel", 1, iniFilePath.c_str());
    cfg.subPixel = (subPixelVal != 0);

    UINT keepThinVal = GetPrivateProfileIntW(L"World", L"KeepThinQuads", 1, iniFilePath.c_str());
    cfg.keepThinQuads = (keepThinVal != 0);

    wchar_t mipBuf[64] = {0};
    GetPrivateProfileStringW(L"World", L"MipFilter", L"trilinear", mipBuf, 64, iniFilePath.c_str());
    ParseMipFilter(mipBuf, cfg.mipFilter);

    const INT aniso = static_cast<INT>(GetPrivateProfileIntW(L"World", L"Anisotropy", 8, iniFilePath.c_str()));
    cfg.anisotropy = ClampAnisotropy(aniso);

    wchar_t widescreenBuf[64] = {0};
    GetPrivateProfileStringW(L"World", L"Widescreen", L"off", widescreenBuf, 64, iniFilePath.c_str());
    cfg.invalidWidescreenValue = !ParseWidescreenSetting(widescreenBuf, cfg.widescreen);
    cfg.rawWidescreenString = cfg.invalidWidescreenValue ? widescreenBuf : L"";

    UINT tpEnabled = GetPrivateProfileIntW(L"TexturePack", L"Enabled", 1, iniFilePath.c_str());
    cfg.texturePackEnabled = (tpEnabled != 0);

    wchar_t tpPathBuf[MAX_PATH] = {0};
    GetPrivateProfileStringW(L"TexturePack", L"Path", L"texture-pack", tpPathBuf, MAX_PATH, iniFilePath.c_str());
    cfg.texturePackPath = tpPathBuf;

    INT tpMaxMiB = static_cast<INT>(GetPrivateProfileIntW(L"TexturePack", L"MaxMiB", 512, iniFilePath.c_str()));
    if (tpMaxMiB < 16) tpMaxMiB = 16;
    if (tpMaxMiB > 2048) tpMaxMiB = 2048;
    cfg.texturePackMaxMiB = tpMaxMiB;

    INT tpBudget = static_cast<INT>(GetPrivateProfileIntW(L"TexturePack", L"DecodeBudgetMs", 4, iniFilePath.c_str()));
    cfg.texturePackDecodeBudgetMs = (tpBudget < 1) ? 1 : tpBudget;

    UINT tpPreload = GetPrivateProfileIntW(L"TexturePack", L"Preload", 1, iniFilePath.c_str());
    cfg.texturePackPreload = (tpPreload != 0);

    UINT tpDump = GetPrivateProfileIntW(L"TexturePack", L"Dump", 0, iniFilePath.c_str());
    cfg.texturePackDump = (tpDump != 0);

    wchar_t tpDumpPathBuf[MAX_PATH] = {0};
    GetPrivateProfileStringW(L"TexturePack", L"DumpPath", L"texture-dump", tpDumpPathBuf, MAX_PATH, iniFilePath.c_str());
    cfg.texturePackDumpPath = tpDumpPathBuf;

    UINT saveEnabled = GetPrivateProfileIntW(L"Save", L"Enabled", 1, iniFilePath.c_str());
    cfg.saveRedirectEnabled = (saveEnabled != 0);

    wchar_t saveFileBuf[MAX_PATH] = {0};
    GetPrivateProfileStringW(L"Save", L"File", L"Partida 1.RRS", saveFileBuf, MAX_PATH, iniFilePath.c_str());
    cfg.saveFile = saveFileBuf;
}

void ConfigInit(const std::wstring& iniFilePath) {
    ParseConfigFile(iniFilePath, g_config);
}

int ClampInternalHeight(int h) {
    if (h < 240) return 240;
    if (h > 4320) return 4320;
    return h;
}

std::wstring ResolveGamePath(const std::wstring& relOrAbsPath) {
    if (relOrAbsPath.empty()) return L"";
    if (relOrAbsPath.size() >= 2 && relOrAbsPath[1] == L':') {
        return relOrAbsPath;
    }
    if (relOrAbsPath[0] == L'\\' || relOrAbsPath[0] == L'/') {
        return relOrAbsPath;
    }
    wchar_t exePath[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return relOrAbsPath;
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (!lastSlash) lastSlash = wcsrchr(exePath, L'/');
    if (lastSlash) {
        *lastSlash = L'\0';
    }
    return std::wstring(exePath) + L"\\" + relOrAbsPath;
}

bool ToggleGpuWorld() {
    g_config.gpuWorld = !g_config.gpuWorld;
    return g_config.gpuWorld;
}

const Config& GetConfig() {
    return g_config;
}
