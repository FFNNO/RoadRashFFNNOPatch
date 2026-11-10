#include "widescreen.h"

#include <atomic>
#include <cmath>
#include <cwctype>
#include <string>

namespace {

std::atomic<int> g_appliedProjectionWidth{kEngineProjectionWidth};

std::wstring LowerTrimmed(const wchar_t* str) {
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

bool IsValidProjectionWidth(int width) {
    return width >= kEngineProjectionWidth && width <= kMaxProjectionWidth && (width % 2) == 0;
}

// Nearest even integer, halves rounding up (x >= 0).
int RoundToEven(double x) {
    return 2 * static_cast<int>(std::floor(x / 2.0 + 0.5));
}

int RoundNearest(double x) {
    return static_cast<int>(std::floor(x + 0.5));
}

}  // namespace

bool ParseWidescreenSetting(const wchar_t* str, WidescreenSetting& out) {
    out = WidescreenSetting{};
    const std::wstring s = LowerTrimmed(str);
    if (s == L"off") {
        return true;
    }
    if (s == L"auto") {
        out.mode = WidescreenMode::Auto;
        return true;
    }
    if (s.empty() || s.size() > 4) {
        return false;
    }
    int value = 0;
    for (wchar_t c : s) {
        if (c < L'0' || c > L'9') {
            return false;
        }
        value = value * 10 + static_cast<int>(c - L'0');
    }
    if (!IsValidProjectionWidth(value)) {
        return false;
    }
    out.mode = WidescreenMode::Fixed;
    out.fixedWidth = value;
    return true;
}

int DeriveAutoProjectionWidth(int clientW, int clientH) {
    if (clientW <= 0 || clientH <= 0) {
        return kEngineProjectionWidth;
    }
    // 2 * floor(120 * W / H + 1/2) in integers: halves round up, no floating-point ties.
    const int64_t half = (240LL * clientW + clientH) / (2LL * clientH);
    const int64_t width = 2 * half;
    if (width < kEngineProjectionWidth) {
        return kEngineProjectionWidth;
    }
    if (width > kMaxProjectionWidth) {
        return kMaxProjectionWidth;
    }
    return static_cast<int>(width);
}

WidescreenResolution ResolveRaceProjectionWidth(const WidescreenSetting& setting,
                                                bool gpuWorldEffective,
                                                bool hiRes,
                                                bool inset,
                                                int clientW,
                                                int clientH) {
    WidescreenResolution r;
    if (setting.mode == WidescreenMode::Off) {
        r.reason = WidescreenReason::SettingOff;
        return r;
    }
    if (!gpuWorldEffective) {
        r.reason = WidescreenReason::GpuWorldOff;
        return r;
    }
    if (!hiRes) {
        r.reason = WidescreenReason::LowRes;
        return r;
    }
    if (inset) {
        r.reason = WidescreenReason::Inset;
        return r;
    }
    int width = kEngineProjectionWidth;
    if (setting.mode == WidescreenMode::Auto) {
        if (clientW <= 0 || clientH <= 0) {
            r.reason = WidescreenReason::NoWindowSize;
            return r;
        }
        width = DeriveAutoProjectionWidth(clientW, clientH);
    } else if (IsValidProjectionWidth(setting.fixedWidth)) {
        width = setting.fixedWidth;
    }
    if (width <= kEngineProjectionWidth) {
        r.reason = WidescreenReason::SettingOff;
        return r;
    }
    r.width = width;
    r.reason = WidescreenReason::Applied;
    return r;
}

const char* WidescreenReasonToString(WidescreenReason reason) {
    switch (reason) {
        case WidescreenReason::Applied:      return "applied";
        case WidescreenReason::SettingOff:   return "widescreen off or 4:3 window";
        case WidescreenReason::GpuWorldOff:  return "GpuWorld not active";
        case WidescreenReason::LowRes:       return "low-res mode (HiRes=0)";
        case WidescreenReason::Inset:        return "inset mode";
        case WidescreenReason::NoWindowSize: return "no window size";
    }
    return "unknown";
}

bool SoftwareWorldWritesForbidden(int appliedProjectionWidth, int32_t clipRight) {
    return appliedProjectionWidth > kEngineProjectionWidth || clipRight > kSoftwareSurfaceWidth;
}

WorldTargetLayout ComputeWorldTargetLayout(int projectionWidth,
                                           int internalW,
                                           int internalH,
                                           int clientW,
                                           int clientH) {
    WorldTargetLayout l;
    l.targetH = internalH;
    if (projectionWidth <= kEngineProjectionWidth || internalH <= 0) {
        l.targetW = internalW;
        l.layerLeft = 0;
        l.layerRight = internalW;
        return l;
    }
    const int width = projectionWidth > kMaxProjectionWidth ? kMaxProjectionWidth : projectionWidth;
    l.widescreen = true;
    l.worldSrcW = width * 2;  // HiRes source pixels; widescreen is only applied with HiRes=1
    l.worldSrcH = kSoftwareSurfaceHeight;

    const double natural = static_cast<double>(internalH) * l.worldSrcW / l.worldSrcH;
    double chosen = natural;
    if (clientW > 0 && clientH > 0) {
        const double windowMatched = static_cast<double>(internalH) * clientW / clientH;
        if (std::fabs(windowMatched - natural) <= natural * 0.01) {
            chosen = windowMatched;
        }
    }
    int targetW = RoundToEven(chosen);
    constexpr int kMaxTextureDimension = 16384;  // D3D11 feature level 11_0
    if (targetW > kMaxTextureDimension) {
        targetW = kMaxTextureDimension;
    }
    if (targetW < internalW) {
        targetW = internalW;
    }
    l.targetW = targetW;

    const double scale = static_cast<double>(targetW) / l.worldSrcW;
    l.layerLeft = RoundNearest((l.worldSrcW - kSoftwareSurfaceWidth) / 2.0 * scale);
    l.layerRight = RoundNearest((l.worldSrcW + kSoftwareSurfaceWidth) / 2.0 * scale);
    return l;
}

void WidescreenSetAppliedWidth(int width) {
    g_appliedProjectionWidth.store(width, std::memory_order_relaxed);
}

int WidescreenGetAppliedWidth() {
    return g_appliedProjectionWidth.load(std::memory_order_relaxed);
}

bool WidescreenIsActive() {
    return WidescreenGetAppliedWidth() > kEngineProjectionWidth;
}
