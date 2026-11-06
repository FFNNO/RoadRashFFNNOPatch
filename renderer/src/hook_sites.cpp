#include "hook_sites.h"
#include "hook.h"
#include "log.h"
#include <windows.h>
#include <cstdint>

// FUN_0046e630 (race present): 8bpp buffer -> 32-bit DIB via palette -> StretchDIBits
// Signature: void __fastcall FUN_0046e630(uint8_t* buffer, int pitch, int palette)
// Calling convention: fastcall (ECX = buffer, EDX = pitch, [ESP+4] = palette)
typedef void (__fastcall *FUN_0046e630_Fn)(uint8_t* buffer, int pitch, int palette);

static FUN_0046e630_Fn g_orig_FUN_0046e630 = nullptr;

static void __fastcall Detour_FUN_0046e630(uint8_t* buffer, int pitch, int palette) {
    static uint64_t s_frameCount = 0;
    static LARGE_INTEGER s_lastTime = {0};
    static LARGE_INTEGER s_freq = {0};

    s_frameCount++;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (s_lastTime.QuadPart == 0) {
        QueryPerformanceFrequency(&s_freq);
        s_lastTime = now;
    } else {
        const double elapsed = static_cast<double>(now.QuadPart - s_lastTime.QuadPart) / static_cast<double>(s_freq.QuadPart);
        if (elapsed >= 5.0) {
            const double fps = static_cast<double>(s_frameCount) / elapsed;
            LOG_INFO("FPS: %.1f (%llu frames in %.2f s)", fps, s_frameCount, elapsed);
            s_frameCount = 0;
            s_lastTime = now;
        }
    }

    if (g_orig_FUN_0046e630) {
        g_orig_FUN_0046e630(buffer, pitch, palette);
    }
}

// First 9 bytes at 0x0046e630 in both original and patched exes:
// 55                push ebp          (1 byte)
// 8b ec             mov  ebp, esp     (2 bytes)
// 81 ec 2c 04 00 00 sub  esp, 0x42c   (6 bytes)
static rr95::HookSite g_site_FUN_0046e630{
    "FUN_0046e630",
    0x0046e630,
    {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x2C, 0x04, 0x00, 0x00},
    9,
    {1, 2, 6},
    reinterpret_cast<void*>(&Detour_FUN_0046e630),
    reinterpret_cast<void**>(&g_orig_FUN_0046e630),
    false
};

bool InstallHooks(ExeBuild build) {
    if (build == ExeBuild::Unknown) {
        LOG_WARN("Cannot install hooks: unknown executable build");
        return false;
    }

    HMODULE hostModule = GetModuleHandleW(nullptr);
    if (!rr95::InstallHook(g_site_FUN_0046e630, hostModule)) {
        LOG_ERROR("Failed to install hook on FUN_0046e630");
        return false;
    }

    LOG_INFO("Hook on FUN_0046e630 installed successfully");
    return true;
}

void RemoveHooks() {
    HMODULE hostModule = GetModuleHandleW(nullptr);
    rr95::RemoveHook(g_site_FUN_0046e630, hostModule);
    g_orig_FUN_0046e630 = nullptr;
}
