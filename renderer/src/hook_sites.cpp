#include "hook_sites.h"
#include "hook.h"
#include "config.h"
#include "presenter.h"
#include "capture.h"
#include "log.h"
#include <windows.h>
#include <atomic>
#include <cstdint>

namespace {

HMODULE g_hostModule = nullptr;
std::atomic<uint64_t> g_gdiStretchDIBitsCalls{0};
bool g_iatStretchDIBitsInstalled = false;

uintptr_t Rebase(uint32_t preferredVa) {
    return reinterpret_cast<uintptr_t>(g_hostModule) + (preferredVa - rr95::kPreferredImageBase);
}

template <typename T>
T* GlobalPtr(uint32_t preferredVa) {
    return reinterpret_cast<T*>(Rebase(preferredVa));
}

// Engine globals (preferred VAs verified in RoadRash95.exe):
// 0x005ce858: HWND game window (DAT_005ce858)
// 0x005ce85c: int buffer width (DAT_005ce85c, 640)
// 0x005ce860: int buffer height (DAT_005ce860, 480)
// 0x005ce864: uint32_t* 32-bit DIB buffer (DAT_005ce864, 640x480)
// 0x005ce8a4: int cached client width (DAT_005ce8a4)
// 0x005ce8a8: int cached client height (DAT_005ce8a8)
// 0x005ce068: int RGBA overlay count (DAT_005ce068)
constexpr uint32_t kVaGameHwnd       = 0x005ce858;
constexpr uint32_t kVaBufferWidth    = 0x005ce85c;
constexpr uint32_t kVaBufferHeight   = 0x005ce860;
constexpr uint32_t kVaDibBuffer      = 0x005ce864;
constexpr uint32_t kVaCachedClientW  = 0x005ce8a4;
constexpr uint32_t kVaCachedClientH  = 0x005ce8a8;
constexpr uint32_t kVaOverlayCount   = 0x005ce068;

// Engine functions:
// 0x00464140: void __fastcall FUN_00464140(uint32_t* dib, int width, int height)
// 0x0046e0f0: void __cdecl FUN_0046e0f0(void)
using FUN_00464140_Fn = void(__fastcall*)(uint32_t* dib, int width, int height);
using FUN_0046e0f0_Fn = void(__cdecl*)();

using FUN_0046e630_Fn = void(__fastcall*)(uint8_t* buffer, int pitch, int palette);
using FUN_0046ded0_Fn = void(__cdecl*)();
using FUN_00460b00_Fn = void(__fastcall*)(char windowed);
using FUN_004688e0_Fn = uint32_t(__cdecl*)();
using FUN_0042e850_Fn = void(__cdecl*)();
using FUN_00467cb0_Fn = void(__fastcall*)(int* polyVerts, int edxArg);

FUN_0046e630_Fn g_orig_FUN_0046e630 = nullptr;
FUN_0046ded0_Fn g_orig_FUN_0046ded0 = nullptr;
FUN_00460b00_Fn g_orig_FUN_00460b00 = nullptr;
FUN_004688e0_Fn g_orig_FUN_004688e0 = nullptr;
FUN_0042e850_Fn g_orig_FUN_0042e850 = nullptr;
FUN_00467cb0_Fn g_orig_FUN_00467cb0 = nullptr;

using StretchDIBits_Fn = int(WINAPI*)(HDC hdc, int xDest, int yDest, int DestWidth, int DestHeight,
                                      int xSrc, int ySrc, int SrcWidth, int SrcHeight,
                                      const VOID* lpBits, const BITMAPINFO* lpbmi, UINT iUsage, DWORD rop);
StretchDIBits_Fn g_origStretchDIBits = nullptr;
void** g_iatStretchDIBitsSlot = nullptr;

int WINAPI Detour_StretchDIBits(HDC hdc, int xDest, int yDest, int DestWidth, int DestHeight,
                                int xSrc, int ySrc, int SrcWidth, int SrcHeight,
                                const VOID* lpBits, const BITMAPINFO* lpbmi, UINT iUsage, DWORD rop) {
    g_gdiStretchDIBitsCalls.fetch_add(1, std::memory_order_relaxed);
    if (g_origStretchDIBits) {
        return g_origStretchDIBits(hdc, xDest, yDest, DestWidth, DestHeight,
                                   xSrc, ySrc, SrcWidth, SrcHeight, lpBits, lpbmi, iUsage, rop);
    }
    return 0;
}

bool InstallIatStretchDIBitsHook(HMODULE moduleBase) {
    if (!moduleBase) {
        return false;
    }
    const auto* base = reinterpret_cast<const uint8_t*>(moduleBase);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    const IMAGE_DATA_DIRECTORY& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.VirtualAddress == 0 || importDir.Size == 0) {
        return false;
    }

    const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
    for (; desc->Name != 0; ++desc) {
        const char* dllName = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(dllName, "GDI32.dll") != 0) {
            continue;
        }
        const DWORD origFirstThunkRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        const auto* origThunk = reinterpret_cast<const IMAGE_THUNK_DATA32*>(base + origFirstThunkRva);
        auto* iatThunk = reinterpret_cast<IMAGE_THUNK_DATA32*>(
            const_cast<uint8_t*>(base) + desc->FirstThunk);

        for (; origThunk->u1.AddressOfData != 0; ++origThunk, ++iatThunk) {
            if (IMAGE_SNAP_BY_ORDINAL32(origThunk->u1.Ordinal)) {
                continue;
            }
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + origThunk->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), "StretchDIBits") == 0) {
                DWORD oldProtect = 0;
                void** slot = reinterpret_cast<void**>(&iatThunk->u1.Function);
                if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
                    return false;
                }
                g_origStretchDIBits = reinterpret_cast<StretchDIBits_Fn>(*slot);
                *slot = reinterpret_cast<void*>(&Detour_StretchDIBits);
                VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
                g_iatStretchDIBitsSlot = slot;
                LOG_INFO("IAT hook on GDI32.dll!StretchDIBits installed (slot %p)", slot);
                return true;
            }
        }
    }
    return false;
}

void UpdateCachedLetterboxIfNeeded(HWND hwnd) {
    int* cachedW = GlobalPtr<int>(kVaCachedClientW);
    int* cachedH = GlobalPtr<int>(kVaCachedClientH);
    if (*cachedW != 0 && *cachedH != 0) {
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        if ((rc.right - rc.left) != *cachedW || (rc.bottom - rc.top) != *cachedH) {
            auto fnUpdateLetterbox = reinterpret_cast<FUN_0046e0f0_Fn>(Rebase(0x0046e0f0));
            fnUpdateLetterbox();
        }
    }
}

void RecordFrameAndLogFps(bool presentedByD3D) {
    static uint64_t s_frameCount = 0;
    static LARGE_INTEGER s_lastTime = {0};
    static LARGE_INTEGER s_freq = {0};

    ++s_frameCount;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (s_lastTime.QuadPart == 0) {
        QueryPerformanceFrequency(&s_freq);
        s_lastTime = now;
        if (g_iatStretchDIBitsInstalled) {
            const uint64_t gdiBlits = g_gdiStretchDIBitsCalls.load(std::memory_order_relaxed);
            LOG_INFO("First frame presented (presenter=%s, filter=%s, gdi_blits=%llu)",
                     presentedByD3D ? "d3d11" : "gdi",
                     FilterModeToString(GetConfig().filter),
                     gdiBlits);
        } else {
            LOG_INFO("First frame presented (presenter=%s, filter=%s, gdi_blits=n/a)",
                     presentedByD3D ? "d3d11" : "gdi",
                     FilterModeToString(GetConfig().filter));
        }
        return;
    }

    const double elapsed = static_cast<double>(now.QuadPart - s_lastTime.QuadPart) /
                           static_cast<double>(s_freq.QuadPart);
    if (elapsed >= 5.0) {
        const double fps = static_cast<double>(s_frameCount) / elapsed;
        if (g_iatStretchDIBitsInstalled) {
            const uint64_t gdiBlits = g_gdiStretchDIBitsCalls.exchange(0, std::memory_order_relaxed);
            LOG_INFO("FPS: %.1f (%llu frames in %.2f s, presenter=%s, filter=%s, gdi_blits=%llu)",
                     fps,
                     s_frameCount,
                     elapsed,
                     presentedByD3D ? "d3d11" : "gdi",
                     FilterModeToString(GetConfig().filter),
                     gdiBlits);
        } else {
            LOG_INFO("FPS: %.1f (%llu frames in %.2f s, presenter=%s, filter=%s, gdi_blits=n/a)",
                     fps,
                     s_frameCount,
                     elapsed,
                     presentedByD3D ? "d3d11" : "gdi",
                     FilterModeToString(GetConfig().filter));
        }
        s_frameCount = 0;
        s_lastTime = now;
    }
}

void FinishRaceFrameAndPollHotkeys() {
    if (GetConfig().captureEnabled || GetConfig().gpuWorld) {
        CaptureEndFrame();
    }
    const uint32_t vk = GetConfig().toggleKey;
    if (vk != 0) {
        static bool s_prevKeyDown = false;
        const bool keyDown = (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
        if (keyDown && !s_prevKeyDown) {
            const bool newState = ToggleGpuWorld();
            CaptureInvalidateCache();
            LOG_INFO("GpuWorld toggled via key 0x%02X to %d (internal resolution %dx%d)",
                     vk,
                     newState ? 1 : 0,
                     GetConfig().internalWidth,
                     GetConfig().internalHeight);
        }
        s_prevKeyDown = keyDown;
    }
}

void __fastcall Detour_FUN_0046e630(uint8_t* buffer, int pitch, int palette) {
    const int width = *GlobalPtr<int>(kVaBufferWidth);
    const int height = *GlobalPtr<int>(kVaBufferHeight);

    if (GetConfig().captureEnabled) {
        if (buffer != nullptr && palette != 0 && width > 0 && height > 0) {
            CaptureOnRacePresent(buffer, pitch, reinterpret_cast<const uint8_t*>(palette), width, height);
        }
    }

    if (!GetConfig().presenterEnabled) {
        if (g_orig_FUN_0046e630) {
            g_orig_FUN_0046e630(buffer, pitch, palette);
        }
        RecordFrameAndLogFps(false);
        FinishRaceFrameAndPollHotkeys();
        return;
    }

    HWND hwnd = *GlobalPtr<HWND>(kVaGameHwnd);
    uint32_t* dib = *GlobalPtr<uint32_t*>(kVaDibBuffer);
    const int overlayCount = *GlobalPtr<int>(kVaOverlayCount);

    if (hwnd == nullptr || dib == nullptr || buffer == nullptr || palette == 0 || width <= 0 || height <= 0) {
        if (g_orig_FUN_0046e630) {
            g_orig_FUN_0046e630(buffer, pitch, palette);
        }
        RecordFrameAndLogFps(false);
        FinishRaceFrameAndPollHotkeys();
        return;
    }

    UpdateCachedLetterboxIfNeeded(hwnd);

    bool ok = false;
    const uint8_t* palBytes = reinterpret_cast<const uint8_t*>(palette);
    if (overlayCount > 0 && !(GetConfig().gpuWorld && CaptureHasWorldFrame())) {
        uint32_t lut[256];
        for (int i = 0; i < 256; ++i) {
            const uint32_t r = palBytes[i * 4 + 0];
            const uint32_t g = palBytes[i * 4 + 1];
            const uint32_t b = palBytes[i * 4 + 2];
            lut[i] = (r << 16) | (g << 8) | b;
        }
        uint32_t* dst = dib;
        const uint8_t* srcRow = buffer;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                dst[x] = lut[srcRow[x]];
            }
            dst += width;
            srcRow += pitch;
        }
        auto fnBlendOverlays = reinterpret_cast<FUN_00464140_Fn>(Rebase(0x00464140));
        fnBlendOverlays(dib, width, height);
        ok = PresenterPresentBgra(hwnd, dib, width, height);
    } else {
        ok = PresenterPresent8bpp(hwnd, buffer, pitch, palBytes, width, height);
    }

    if (!ok) {
        if (!PresenterIsLatchedOff()) {
            LOG_WARN("D3D11 race present failed, falling back to GDI FUN_0046e630");
        }
        if (g_orig_FUN_0046e630) {
            g_orig_FUN_0046e630(buffer, pitch, palette);
        }
        RecordFrameAndLogFps(false);
        FinishRaceFrameAndPollHotkeys();
        return;
    }

    RecordFrameAndLogFps(true);
    FinishRaceFrameAndPollHotkeys();
}

void __cdecl Detour_FUN_0046ded0() {
    if (!GetConfig().presenterEnabled) {
        if (g_orig_FUN_0046ded0) {
            g_orig_FUN_0046ded0();
        }
        RecordFrameAndLogFps(false);
        return;
    }

    HWND hwnd = *GlobalPtr<HWND>(kVaGameHwnd);
    const int width = *GlobalPtr<int>(kVaBufferWidth);
    const int height = *GlobalPtr<int>(kVaBufferHeight);
    const uint32_t* dib = *GlobalPtr<const uint32_t*>(kVaDibBuffer);

    if (hwnd == nullptr || dib == nullptr || width <= 0 || height <= 0) {
        if (g_orig_FUN_0046ded0) {
            g_orig_FUN_0046ded0();
        }
        RecordFrameAndLogFps(false);
        return;
    }

    UpdateCachedLetterboxIfNeeded(hwnd);

    if (!PresenterPresentBgra(hwnd, dib, width, height)) {
        if (!PresenterIsLatchedOff()) {
            LOG_WARN("D3D11 menu present failed, falling back to GDI FUN_0046ded0");
        }
        if (g_orig_FUN_0046ded0) {
            g_orig_FUN_0046ded0();
        }
        RecordFrameAndLogFps(false);
        return;
    }

    RecordFrameAndLogFps(true);
}

void __fastcall Detour_FUN_00460b00(char windowed) {
    if (g_orig_FUN_00460b00) {
        g_orig_FUN_00460b00(windowed);
    }
    PresenterNotifyWindowChange();
}

uint32_t __cdecl Detour_FUN_004688e0() {
    if (!GetConfig().captureEnabled && !GetConfig().gpuWorld) {
        if (g_orig_FUN_004688e0) {
            return g_orig_FUN_004688e0();
        }
        return 1;
    }
    CaptureOnBatchStart();
    uint32_t res = 1;
    if (g_orig_FUN_004688e0) {
        res = g_orig_FUN_004688e0();
    }
    CaptureOnBatchEnd();
    return res;
}

void __cdecl Detour_FUN_0042e850() {
    if (!GetConfig().captureEnabled && !GetConfig().gpuWorld) {
        if (g_orig_FUN_0042e850) {
            g_orig_FUN_0042e850();
        }
        return;
    }
    const int slot = CaptureTexturedQuadBefore();
    if (!CaptureShouldSkipSoftwareRasterizer() && g_orig_FUN_0042e850) {
        g_orig_FUN_0042e850();
    }
    CaptureTexturedQuadAfter(slot);
}

void __fastcall Detour_FUN_00467cb0(int* polyVerts, int edxArg) {
    if (!GetConfig().captureEnabled && !GetConfig().gpuWorld) {
        if (g_orig_FUN_00467cb0) {
            g_orig_FUN_00467cb0(polyVerts, edxArg);
        }
        return;
    }
    const int slot = CaptureFlatQuadBefore(polyVerts, edxArg);
    if (!CaptureShouldSkipSoftwareRasterizer() && g_orig_FUN_00467cb0) {
        g_orig_FUN_00467cb0(polyVerts, edxArg);
    }
    CaptureFlatQuadAfter(slot, polyVerts);
}

}  // namespace

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

// First 6 bytes at 0x0046ded0 in both original and patched exes:
// 55                push ebp          (1 byte)
// 8b ec             mov  ebp, esp     (2 bytes)
// 83 ec 24          sub  esp, 0x24    (3 bytes)
static rr95::HookSite g_site_FUN_0046ded0{
    "FUN_0046ded0",
    0x0046ded0,
    {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x24},
    6,
    {1, 2, 3},
    reinterpret_cast<void*>(&Detour_FUN_0046ded0),
    reinterpret_cast<void**>(&g_orig_FUN_0046ded0),
    false
};

// First 6 bytes at 0x00460b00 in both original and patched exes:
// 55                push ebp          (1 byte)
// 8b ec             mov  ebp, esp     (2 bytes)
// 83 ec 24          sub  esp, 0x24    (3 bytes)
static rr95::HookSite g_site_FUN_00460b00{
    "FUN_00460b00",
    0x00460b00,
    {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x24},
    6,
    {1, 2, 3},
    reinterpret_cast<void*>(&Detour_FUN_00460b00),
    reinterpret_cast<void**>(&g_orig_FUN_00460b00),
    false
};

// First 6 bytes at 0x004688e0 in both original and patched exes:
// 55                push ebp          (1 byte)
// 8b ec             mov  ebp, esp     (2 bytes)
// 83 ec 7c          sub  esp, 0x7c    (3 bytes)
static rr95::HookSite g_site_FUN_004688e0{
    "FUN_004688e0",
    0x004688e0,
    {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x7C},
    6,
    {1, 2, 3},
    reinterpret_cast<void*>(&Detour_FUN_004688e0),
    reinterpret_cast<void**>(&g_orig_FUN_004688e0),
    false
};

// First 6 bytes at 0x0042e850 in both original and patched exes:
// 55                push ebp          (1 byte)
// 8b ec             mov  ebp, esp     (2 bytes)
// 83 ec 7c          sub  esp, 0x7c    (3 bytes)
static rr95::HookSite g_site_FUN_0042e850{
    "FUN_0042e850",
    0x0042e850,
    {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x7C},
    6,
    {1, 2, 3},
    reinterpret_cast<void*>(&Detour_FUN_0042e850),
    reinterpret_cast<void**>(&g_orig_FUN_0042e850),
    false
};

// First 9 bytes at 0x00467cb0 in both original and patched exes:
// 55                push ebp          (1 byte)
// 8b ec             mov  ebp, esp     (2 bytes)
// 81 ec a8 00 00 00 sub  esp, 0xa8    (6 bytes)
static rr95::HookSite g_site_FUN_00467cb0{
    "FUN_00467cb0",
    0x00467cb0,
    {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00},
    9,
    {1, 2, 6},
    reinterpret_cast<void*>(&Detour_FUN_00467cb0),
    reinterpret_cast<void**>(&g_orig_FUN_00467cb0),
    false
};

bool InstallHooks(ExeBuild build) {
    if (build == ExeBuild::Unknown) {
        LOG_WARN("Cannot install hooks: unknown executable build");
        return false;
    }

    g_hostModule = GetModuleHandleW(nullptr);
    if (!rr95::InstallHook(g_site_FUN_0046e630, g_hostModule)) {
        LOG_ERROR("Failed to install hook on FUN_0046e630");
        return false;
    }
    if (!rr95::InstallHook(g_site_FUN_0046ded0, g_hostModule)) {
        LOG_ERROR("Failed to install hook on FUN_0046ded0");
        rr95::RemoveHook(g_site_FUN_0046e630, g_hostModule);
        return false;
    }
    if (!rr95::InstallHook(g_site_FUN_00460b00, g_hostModule)) {
        LOG_ERROR("Failed to install hook on FUN_00460b00");
        rr95::RemoveHook(g_site_FUN_0046ded0, g_hostModule);
        rr95::RemoveHook(g_site_FUN_0046e630, g_hostModule);
        return false;
    }

    g_iatStretchDIBitsInstalled = InstallIatStretchDIBitsHook(g_hostModule);
    if (!g_iatStretchDIBitsInstalled) {
        LOG_WARN("IAT hook on GDI32.dll!StretchDIBits not installed; gdi_blits will report n/a");
    }

    if (GetConfig().captureEnabled || GetConfig().presenterEnabled || GetConfig().gpuWorld) {
        if (!rr95::InstallHook(g_site_FUN_004688e0, g_hostModule)) {
            LOG_ERROR("Failed to install capture/world hook on FUN_004688e0");
        } else if (!rr95::InstallHook(g_site_FUN_0042e850, g_hostModule)) {
            LOG_ERROR("Failed to install capture/world hook on FUN_0042e850");
            rr95::RemoveHook(g_site_FUN_004688e0, g_hostModule);
        } else if (!rr95::InstallHook(g_site_FUN_00467cb0, g_hostModule)) {
            LOG_ERROR("Failed to install capture/world hook on FUN_00467cb0");
            rr95::RemoveHook(g_site_FUN_0042e850, g_hostModule);
            rr95::RemoveHook(g_site_FUN_004688e0, g_hostModule);
        } else {
            LOG_INFO("Capture/world hooks on FUN_004688e0, FUN_0042e850, and FUN_00467cb0 installed");
        }
    }

    LOG_INFO("Hooks on FUN_0046e630, FUN_0046ded0, and FUN_00460b00 installed successfully");
    return true;
}

void RemoveHooks() {
    HMODULE hostModule = g_hostModule ? g_hostModule : GetModuleHandleW(nullptr);
    if (g_iatStretchDIBitsSlot && g_origStretchDIBits) {
        DWORD oldProtect = 0;
        if (VirtualProtect(g_iatStretchDIBitsSlot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            *g_iatStretchDIBitsSlot = reinterpret_cast<void*>(g_origStretchDIBits);
            VirtualProtect(g_iatStretchDIBitsSlot, sizeof(void*), oldProtect, &oldProtect);
        }
        g_iatStretchDIBitsSlot = nullptr;
        g_origStretchDIBits = nullptr;
    }
    g_iatStretchDIBitsInstalled = false;
    rr95::RemoveHook(g_site_FUN_00467cb0, hostModule);
    rr95::RemoveHook(g_site_FUN_0042e850, hostModule);
    rr95::RemoveHook(g_site_FUN_004688e0, hostModule);
    rr95::RemoveHook(g_site_FUN_00460b00, hostModule);
    rr95::RemoveHook(g_site_FUN_0046ded0, hostModule);
    rr95::RemoveHook(g_site_FUN_0046e630, hostModule);
    g_orig_FUN_00467cb0 = nullptr;
    g_orig_FUN_0042e850 = nullptr;
    g_orig_FUN_004688e0 = nullptr;
    g_orig_FUN_00460b00 = nullptr;
    g_orig_FUN_0046ded0 = nullptr;
    g_orig_FUN_0046e630 = nullptr;
}
