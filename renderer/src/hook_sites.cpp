#include "hook_sites.h"
#include "hook.h"
#include "config.h"
#include "presenter.h"
#include "capture.h"
#include "log.h"
#include "texture_pack.h"
#include "save_redirect.h"
#include "overlay_text.h"
#include <windows.h>
#include <commdlg.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

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

rr95::IatHook g_iatStretchDIBits{"GDI32.dll", "StretchDIBits", reinterpret_cast<void*>(&Detour_StretchDIBits),
                                reinterpret_cast<void**>(&g_origStretchDIBits), nullptr};

// ---------------------------------------------------------------------------------------------
// Fixed save file and save/load feedback ([Save] in rr95gpu.ini).
//
// Engine globals (preferred VAs verified in RoadRash95.exe, out/decomp.c:32437-32595):
// 0x00604320: char[0x103] current save file name, lpstrFile of the save/open dialogs
// 0x005c4d08: 160-byte save block scratch buffer, used only by the save and load routines
// 0x005cd47e: byte "game changed since the last save" flag, cleared only on save/load success
constexpr uint32_t kVaSaveFileName = 0x00604320;
constexpr size_t kSaveFileNameMax = 0x103;
constexpr uint32_t kVaSaveBlock = 0x005c4d08;
constexpr uint32_t kVaSaveDirtyFlag = 0x005cd47e;

// 0x0043f7c0: void __cdecl FUN_0043f7c0(void)  save the game to the file named at 0x00604320
// 0x0043f580: void __cdecl FUN_0043f580(void)  load the game from the file named at 0x00604320
using FUN_0043f7c0_Fn = void(__cdecl*)();
using FUN_0043f580_Fn = void(__cdecl*)();
FUN_0043f7c0_Fn g_orig_FUN_0043f7c0 = nullptr;
FUN_0043f580_Fn g_orig_FUN_0043f580 = nullptr;

using GetFileNameA_Fn = BOOL(WINAPI*)(LPOPENFILENAMEA ofn);
GetFileNameA_Fn g_origGetSaveFileNameA = nullptr;
GetFileNameA_Fn g_origGetOpenFileNameA = nullptr;
std::string g_saveFilePathA;  // resolved [Save] File in the ANSI code page; empty = do not redirect

BOOL CallOriginalFileDialog(GetFileNameA_Fn original, LPOPENFILENAMEA ofn) {
    return original ? original(ofn) : FALSE;
}

// Answers every save dialog (Salvar Juego, Salvar Juego Como, save before quitting) with the
// default file instead of showing it.
BOOL WINAPI Detour_GetSaveFileNameA(LPOPENFILENAMEA ofn) {
    if (ofn != nullptr && !g_saveFilePathA.empty()) {
        const SaveRedirectResult r = RedirectSaveFileName(*ofn, g_saveFilePathA);
        if (r == SaveRedirectResult::Filled) {
            LOG_INFO("Save dialog answered with %s", ofn->lpstrFile);
            return TRUE;
        }
        LOG_WARN("Save dialog not redirected to %s (%s); showing the dialog",
                 g_saveFilePathA.c_str(), SaveRedirectResultToString(r));
    }
    return CallOriginalFileDialog(g_origGetSaveFileNameA, ofn);
}

// Answers the load dialog with the default file when it exists; otherwise the dialog opens as before.
BOOL WINAPI Detour_GetOpenFileNameA(LPOPENFILENAMEA ofn) {
    if (ofn != nullptr && !g_saveFilePathA.empty()) {
        const DWORD attr = GetFileAttributesA(g_saveFilePathA.c_str());
        const bool exists = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
        const SaveRedirectResult r = RedirectOpenFileName(*ofn, g_saveFilePathA, exists);
        if (r == SaveRedirectResult::Filled) {
            LOG_INFO("Open dialog answered with %s", ofn->lpstrFile);
            return TRUE;
        }
        if (r == SaveRedirectResult::FileMissing) {
            LOG_INFO("Default save file %s not found; showing the open dialog", g_saveFilePathA.c_str());
        } else {
            LOG_WARN("Open dialog not redirected to %s (%s); showing the dialog",
                     g_saveFilePathA.c_str(), SaveRedirectResultToString(r));
        }
    }
    return CallOriginalFileDialog(g_origGetOpenFileNameA, ofn);
}

rr95::IatHook g_iatGetSaveFileNameA{"COMDLG32.dll", "GetSaveFileNameA",
                                    reinterpret_cast<void*>(&Detour_GetSaveFileNameA),
                                    reinterpret_cast<void**>(&g_origGetSaveFileNameA), nullptr};
rr95::IatHook g_iatGetOpenFileNameA{"COMDLG32.dll", "GetOpenFileNameA",
                                    reinterpret_cast<void*>(&Detour_GetOpenFileNameA),
                                    reinterpret_cast<void**>(&g_origGetOpenFileNameA), nullptr};

const char* GameSaveFileName() {
    const char* name = GlobalPtr<const char>(kVaSaveFileName);
    const size_t len = strnlen(name, kSaveFileNameMax);
    return (len == 0 || len >= kSaveFileNameMax) ? nullptr : name;
}

SaveFileProbe ProbeGameSaveFile() {
    SaveFileProbe probe;
    const char* name = GameSaveFileName();
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (name == nullptr || !GetFileAttributesExA(name, GetFileExInfoStandard, &data) ||
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return probe;
    }
    probe.exists = true;
    probe.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    probe.lastWrite = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                      data.ftLastWriteTime.dwLowDateTime;
    return probe;
}

// Clearing the magic before a call lets a valid block afterwards prove that this call built (save)
// or read (load) it; both routines overwrite the whole block before they use it, and nothing else
// in the game reads it.
void ClearSaveBlockMagic() {
    std::memset(GlobalPtr<uint8_t>(kVaSaveBlock) + kSaveBlockMagicOffset, 0, sizeof(uint32_t));
}

bool SaveBlockValid() {
    return IsValidSaveBlock(GlobalPtr<const uint8_t>(kVaSaveBlock), kSaveBlockSize);
}

uint8_t SaveDirtyFlag() {
    return *GlobalPtr<const uint8_t>(kVaSaveDirtyFlag);
}

void __cdecl Detour_FUN_0043f7c0() {
    const SaveFileProbe before = ProbeGameSaveFile();
    ClearSaveBlockMagic();
    if (g_orig_FUN_0043f7c0) {
        g_orig_FUN_0043f7c0();
    }
    const SaveFileProbe after = ProbeGameSaveFile();
    const bool blockValid = SaveBlockValid();
    const uint8_t dirty = SaveDirtyFlag();
    const char* name = GameSaveFileName();
    if (SaveRoutineSucceeded(before, after, blockValid, dirty)) {
        LOG_INFO("Game saved to %s", name ? name : "?");
        PresenterShowToast(ToastKind::Saved);
    } else {
        LOG_WARN("Game save to %s not confirmed (block valid=%d, dirty=%u, exists=%d, size=%llu, timestamp changed=%d)",
                 name ? name : "?", blockValid ? 1 : 0, static_cast<unsigned>(dirty), after.exists ? 1 : 0,
                 static_cast<unsigned long long>(after.size), before.lastWrite != after.lastWrite ? 1 : 0);
    }
}

void __cdecl Detour_FUN_0043f580() {
    ClearSaveBlockMagic();
    if (g_orig_FUN_0043f580) {
        g_orig_FUN_0043f580();
    }
    const bool blockValid = SaveBlockValid();
    const uint8_t dirty = SaveDirtyFlag();
    const char* name = GameSaveFileName();
    if (LoadRoutineSucceeded(blockValid, dirty)) {
        LOG_INFO("Game loaded from %s", name ? name : "?");
        PresenterShowToast(ToastKind::Loaded);
    } else {
        LOG_WARN("Game load from %s not confirmed (block valid=%d, dirty=%u)",
                 name ? name : "?", blockValid ? 1 : 0, static_cast<unsigned>(dirty));
    }
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
        const PresenterSubPixelStats sp = PresenterTakeSubPixelStats();
        if (GetConfig().gpuWorld && GetConfig().subPixel && sp.frames > 0) {
            LOG_INFO("SubPixel: %llu GPU world frames, %.1f quads/frame at 16.16 corners, %.2f fallback quads/frame "
                     "(%llu total, max %u in one frame)",
                     sp.frames,
                     static_cast<double>(sp.floatQuads) / static_cast<double>(sp.frames),
                     static_cast<double>(sp.fallbackQuads) / static_cast<double>(sp.frames),
                     sp.fallbackQuads,
                     sp.maxFallbackPerFrame);
        }
        if (GetConfig().texturePackDump) {
            LOG_INFO("TexturePack: %u textures dumped (%u queued)",
                     TextureDumpGetDumpedCount(),
                     TextureDumpGetQueuedCount());
        }
        if (GetConfig().gpuWorld && GetConfig().texturePackEnabled) {
            const TexturePackStats stats = TexturePackGetStats();
            LOG_INFO("TexturePack: %u loaded, %u pending, %u rejected (%.2f MiB)",
                     stats.loadedCount,
                     stats.pendingCount,
                     stats.rejectedCount,
                     static_cast<double>(stats.totalBytes) / (1024.0 * 1024.0));
        }
        s_frameCount = 0;
        s_lastTime = now;
    }
}

void FinishRaceFrameAndPollHotkeys() {
    static RaceFrameHotkeyState s_hotkeyState;
    const Config& cfg = GetConfig();
    const uint32_t vk = cfg.toggleKey;
    const bool keyDown = (vk != 0) && (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
    const RaceFrameActions actions = AdvanceRaceFrame(s_hotkeyState, cfg.captureEnabled, cfg.gpuWorld, vk, keyDown);
    if (actions.endCaptureFrame) {
        CaptureEndFrame();
    }
    if (actions.toggleGpuWorld) {
        const bool newState = ToggleGpuWorld();
        CaptureInvalidateCache();
        LOG_INFO("GpuWorld toggled via key 0x%02X to %d (internal resolution %dx%d)",
                 vk,
                 newState ? 1 : 0,
                 GetConfig().internalWidth,
                 GetConfig().internalHeight);
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
    const bool skip = CaptureShouldSkipSoftwareRasterizer();
    if (!skip && g_orig_FUN_0042e850) {
        g_orig_FUN_0042e850();
    }
    CaptureTexturedQuadAfter(slot, skip);
}

void __fastcall Detour_FUN_00467cb0(int* polyVerts, int edxArg) {
    if (!GetConfig().captureEnabled && !GetConfig().gpuWorld) {
        if (g_orig_FUN_00467cb0) {
            g_orig_FUN_00467cb0(polyVerts, edxArg);
        }
        return;
    }
    const int slot = CaptureFlatQuadBefore(polyVerts, edxArg);
    const bool skip = CaptureShouldSkipSoftwareRasterizer();
    if (!skip && g_orig_FUN_00467cb0) {
        g_orig_FUN_00467cb0(polyVerts, edxArg);
    }
    CaptureFlatQuadAfter(slot, polyVerts, skip);
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

// First 6 bytes at 0x0043f580 in both original and patched exes:
// 55                push ebp          (1 byte)
// 8b ec             mov  ebp, esp     (2 bytes)
// 83 ec 10          sub  esp, 0x10    (3 bytes)
static rr95::HookSite g_site_FUN_0043f580{
    "FUN_0043f580",
    0x0043f580,
    {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10},
    6,
    {1, 2, 3},
    reinterpret_cast<void*>(&Detour_FUN_0043f580),
    reinterpret_cast<void**>(&g_orig_FUN_0043f580),
    false
};

// First 6 bytes at 0x0043f7c0 in both original and patched exes:
// 55                push ebp          (1 byte)
// 8b ec             mov  ebp, esp     (2 bytes)
// 83 ec 0c          sub  esp, 0x0c    (3 bytes)
static rr95::HookSite g_site_FUN_0043f7c0{
    "FUN_0043f7c0",
    0x0043f7c0,
    {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C},
    6,
    {1, 2, 3},
    reinterpret_cast<void*>(&Detour_FUN_0043f7c0),
    reinterpret_cast<void**>(&g_orig_FUN_0043f7c0),
    false
};

// [Save] Enabled=1: dialog redirect (when the default path is usable) and save/load success
// detection for the toast. Every failure only logs; the game keeps its original behaviour there.
static void InstallSaveHooks() {
    const std::wstring resolved = ResolveGamePath(GetConfig().saveFile);
    g_saveFilePathA.clear();
    if (resolved.empty()) {
        LOG_WARN("[Save] File is empty; save and open dialogs are not redirected");
    } else if (!WidePathToAnsi(resolved, g_saveFilePathA)) {
        LOG_WARN("[Save] File '%ls' cannot be expressed in the ANSI code page; dialogs are not redirected",
                 resolved.c_str());
    }
    if (!g_saveFilePathA.empty()) {
        const bool saveOk = rr95::InstallIatHook(g_iatGetSaveFileNameA, g_hostModule);
        const bool openOk = rr95::InstallIatHook(g_iatGetOpenFileNameA, g_hostModule);
        if (!saveOk || !openOk) {
            LOG_WARN("IAT hooks on COMDLG32.dll not fully installed (GetSaveFileNameA=%d, GetOpenFileNameA=%d)",
                     saveOk ? 1 : 0, openOk ? 1 : 0);
        }
        LOG_INFO("[Save] Default save file: %s", g_saveFilePathA.c_str());
    }
    if (!rr95::InstallHook(g_site_FUN_0043f7c0, g_hostModule)) {
        LOG_WARN("Failed to install save hook on FUN_0043f7c0; no save message");
    }
    if (!rr95::InstallHook(g_site_FUN_0043f580, g_hostModule)) {
        LOG_WARN("Failed to install load hook on FUN_0043f580; no load message");
    }
}

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

    g_iatStretchDIBitsInstalled = rr95::InstallIatHook(g_iatStretchDIBits, g_hostModule);
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

    if (GetConfig().saveRedirectEnabled) {
        InstallSaveHooks();
    } else {
        LOG_INFO("[Save] Enabled=0: original save/open dialogs, no save message");
    }

    LOG_INFO("Hooks on FUN_0046e630, FUN_0046ded0, and FUN_00460b00 installed successfully");
    return true;
}

void RemoveHooks() {
    HMODULE hostModule = g_hostModule ? g_hostModule : GetModuleHandleW(nullptr);
    rr95::RemoveIatHook(g_iatStretchDIBits);
    g_iatStretchDIBitsInstalled = false;
    rr95::RemoveIatHook(g_iatGetOpenFileNameA);
    rr95::RemoveIatHook(g_iatGetSaveFileNameA);
    rr95::RemoveHook(g_site_FUN_0043f580, hostModule);
    rr95::RemoveHook(g_site_FUN_0043f7c0, hostModule);
    rr95::RemoveHook(g_site_FUN_00467cb0, hostModule);
    rr95::RemoveHook(g_site_FUN_0042e850, hostModule);
    rr95::RemoveHook(g_site_FUN_004688e0, hostModule);
    rr95::RemoveHook(g_site_FUN_00460b00, hostModule);
    rr95::RemoveHook(g_site_FUN_0046ded0, hostModule);
    rr95::RemoveHook(g_site_FUN_0046e630, hostModule);
    g_orig_FUN_0043f580 = nullptr;
    g_orig_FUN_0043f7c0 = nullptr;
    g_orig_FUN_00467cb0 = nullptr;
    g_orig_FUN_0042e850 = nullptr;
    g_orig_FUN_004688e0 = nullptr;
    g_orig_FUN_00460b00 = nullptr;
    g_orig_FUN_0046ded0 = nullptr;
    g_orig_FUN_0046e630 = nullptr;
}
