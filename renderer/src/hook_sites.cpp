#include "hook_sites.h"
#include "hook.h"
#include "config.h"
#include "presenter.h"
#include "capture.h"
#include "log.h"
#include "texture_pack.h"
#include "save_redirect.h"
#include "overlay_text.h"
#include "widescreen.h"
#include "options_menu.h"
#include "hud_icons.h"
#include <windows.h>
#include <commdlg.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <vector>

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
using FUN_00457ef0_Fn = void(__cdecl*)();
using FUN_0045b930_Fn = const char*(__fastcall*)(void* item);
// Road slice draws (distance fade D1): ECX = slice; EDX is passed through unchanged.
using FUN_0045a790_Fn = void(__fastcall*)(uint8_t* slice, int edxArg);
using FUN_0045a870_Fn = void(__fastcall*)(uint8_t* slice, int edxArg);

FUN_0046e630_Fn g_orig_FUN_0046e630 = nullptr;
FUN_0046ded0_Fn g_orig_FUN_0046ded0 = nullptr;
FUN_00460b00_Fn g_orig_FUN_00460b00 = nullptr;
FUN_004688e0_Fn g_orig_FUN_004688e0 = nullptr;
FUN_0042e850_Fn g_orig_FUN_0042e850 = nullptr;
FUN_00467cb0_Fn g_orig_FUN_00467cb0 = nullptr;
FUN_00457ef0_Fn g_orig_FUN_00457ef0 = nullptr;
FUN_0045b930_Fn g_orig_FUN_0045b930 = nullptr;
FUN_0045a790_Fn g_orig_FUN_0045a790 = nullptr;
FUN_0045a870_Fn g_orig_FUN_0045a870 = nullptr;

// FUN_004688e0, FUN_0042e850 and FUN_00467cb0 are all hooked; widescreen depends on it, because
// those detours are what keep the software world rasterizers from running.
bool g_worldHooksInstalled = false;

// Diagnostics (D2b): the engine's crest rule, read without hooking at each race present.
// 0x0060bd00: road slice array DAT_0060bd00 (stride 0x224, static .data)
// 0x0059ec90: int slice count DAT_0059ec90 (FUN_00459cb0 clamps it to [1, 0x30])
constexpr uint32_t kVaRoadSlices     = 0x0060bd00;
constexpr uint32_t kVaRoadSliceCount = 0x0059ec90;
RoadSliceStats g_roadSliceStats;

// Only with the world hooks installed (the race code is the verified build) and from the race
// present. The count is checked before anything else is read; the array is static .data, so the
// flag bytes of slices 0..0x30 are always inside the image.
void SampleRoadSlicesForLog() {
    if (!g_worldHooksInstalled || !g_hostModule) {
        return;
    }
    const int32_t count = *GlobalPtr<const int32_t>(kVaRoadSliceCount);
    AccumulateRoadSliceSample(g_roadSliceStats, SampleRoadSliceFlags(GlobalPtr<const uint8_t>(kVaRoadSlices), count));
}
// GpuWorld toggle key pressed during a widescreen race; applied at the next race start.
bool g_gpuWorldTogglePending = false;

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
        const ThinQuadStats tq = CaptureTakeThinQuadStats();
        if (GetConfig().gpuWorld && tq.frames > 0) {
            const double frames = static_cast<double>(tq.frames);
            auto total = [&tq](ThinQuadVerdict v) { return tq.total[static_cast<size_t>(v)]; };
            const uint64_t culled = total(ThinQuadVerdict::CulledDisabled) + total(ThinQuadVerdict::CulledNoPdat) +
                                    total(ThinQuadVerdict::CulledZeroHeight) +
                                    total(ThinQuadVerdict::CulledNoCornerMatch) + total(ThinQuadVerdict::CulledClip);
            LOG_INFO("ThinQuads: %llu GPU world frames, kept %.2f/frame (%llu), culled %.2f/frame (%llu, max %u in one "
                     "frame): zeroHeight %.2f (%llu), noCornerMatch %.2f (%llu), noPdat %.2f (%llu), clip %.2f (%llu), "
                     "disabled %.2f (%llu)",
                     tq.frames,
                     static_cast<double>(total(ThinQuadVerdict::Kept)) / frames, total(ThinQuadVerdict::Kept),
                     static_cast<double>(culled) / frames, culled, tq.maxCulledPerFrame,
                     static_cast<double>(total(ThinQuadVerdict::CulledZeroHeight)) / frames,
                     total(ThinQuadVerdict::CulledZeroHeight),
                     static_cast<double>(total(ThinQuadVerdict::CulledNoCornerMatch)) / frames,
                     total(ThinQuadVerdict::CulledNoCornerMatch),
                     static_cast<double>(total(ThinQuadVerdict::CulledNoPdat)) / frames,
                     total(ThinQuadVerdict::CulledNoPdat),
                     static_cast<double>(total(ThinQuadVerdict::CulledClip)) / frames,
                     total(ThinQuadVerdict::CulledClip),
                     static_cast<double>(total(ThinQuadVerdict::CulledDisabled)) / frames,
                     total(ThinQuadVerdict::CulledDisabled));
        }
        if (g_roadSliceStats.frames > 0 || g_roadSliceStats.invalidFrames > 0) {
            const RoadSliceStats& rs = g_roadSliceStats;
            const double frames = rs.frames > 0 ? static_cast<double>(rs.frames) : 1.0;
            LOG_INFO("RoadSlices: %llu race frames (%llu with count out of range), %.1f slices/frame, "
                     "%.2f dropped by the horizon/frame (%llu total, max %u in one frame), %llu frames with a drop, "
                     "nearest dropped index avg %.1f min %d",
                     rs.frames,
                     rs.invalidFrames,
                     static_cast<double>(rs.slicesTotal) / frames,
                     static_cast<double>(rs.droppedTotal) / frames,
                     rs.droppedTotal,
                     rs.maxDroppedInFrame,
                     rs.framesWithDrop,
                     rs.framesWithDrop > 0 ? static_cast<double>(rs.nearestDroppedSum) /
                                                 static_cast<double>(rs.framesWithDrop)
                                           : -1.0,
                     rs.minNearestDropped);
            g_roadSliceStats = RoadSliceStats{};
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
    SampleRoadSlicesForLog();
    const Config& cfg = GetConfig();
    const uint32_t vk = cfg.toggleKey;
    const bool keyDown = (vk != 0) && (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
    const RaceFrameActions actions = AdvanceRaceFrame(s_hotkeyState, cfg.captureEnabled, cfg.gpuWorld, vk, keyDown);
    if (actions.endCaptureFrame) {
        CaptureEndFrame();
    }
    if (actions.toggleGpuWorld && WidescreenIsActive()) {
        // Turning GpuWorld off mid-race would leave the world undrawn: the software rasterizers stay
        // skipped while the widened projection width is applied. Defer to the next race start.
        g_gpuWorldTogglePending = !g_gpuWorldTogglePending;
        LOG_INFO("GpuWorld toggle via key 0x%02X %s: widescreen is active for this race (projection width %d); "
                 "the toggle applies at the next race start",
                 vk, g_gpuWorldTogglePending ? "deferred" : "cancelled", WidescreenGetAppliedWidth());
    } else if (actions.toggleGpuWorld) {
        const bool newState = ToggleGpuWorld();
        CaptureInvalidateCache();
        LOG_INFO("GpuWorld toggled via key 0x%02X to %d (internal resolution %dx%d)",
                 vk,
                 newState ? 1 : 0,
                 GetConfig().internalWidth,
                 GetConfig().internalHeight);
    }
}

// GpuWorld: the race present does not compose the 32bpp DIB, so the HUD key icons queued for
// FUN_00464140 (ESC, arrows, ...) are recovered as their own premultiplied layer by running it over
// a black and a white scratch buffer (see hud_icons.h); the game's DIB is not touched. FUN_00464140
// clears the queue count on return, so the count is restored after each run and the caller clears
// it once the frame is presented, as the software path does. Returns nullptr when no icon pixel
// is visible or the scratch buffers cannot be allocated (logged once; the frame goes on without
// icons).
const uint32_t* BuildHudIconLayer(int width, int height) {
    static std::vector<uint32_t> onBlack;
    static std::vector<uint32_t> onWhite;
    static std::vector<uint32_t> layer;
    static bool allocFailedLogged = false;
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    try {
        onBlack.resize(pixelCount);
        onWhite.resize(pixelCount);
        layer.resize(pixelCount);
    } catch (const std::bad_alloc&) {
        if (!allocFailedLogged) {
            allocFailedLogged = true;
            LOG_ERROR("HUD icon layer: %ux%u scratch buffers could not be allocated; key icons skipped",
                      static_cast<unsigned>(width), static_cast<unsigned>(height));
        }
        return nullptr;
    }
    std::fill(onBlack.begin(), onBlack.end(), 0xFF000000u);
    std::fill(onWhite.begin(), onWhite.end(), 0xFFFFFFFFu);

    int* overlayCount = GlobalPtr<int>(kVaOverlayCount);
    const int queued = *overlayCount;
    auto fnBlendOverlays = reinterpret_cast<FUN_00464140_Fn>(Rebase(0x00464140));
    fnBlendOverlays(onBlack.data(), width, height);
    *overlayCount = queued;
    fnBlendOverlays(onWhite.data(), width, height);
    *overlayCount = queued;

    if (RecoverHudIconLayer(onBlack.data(), onWhite.data(), layer.data(), pixelCount) == 0) {
        return nullptr;
    }
    return layer.data();
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
    const bool gpuWorldFrame = GetConfig().gpuWorld && CaptureHasWorldFrame();
    if (overlayCount > 0 && !gpuWorldFrame) {
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
        const bool withIcons = overlayCount > 0 && gpuWorldFrame;
        const uint32_t* hudIcons = withIcons ? BuildHudIconLayer(width, height) : nullptr;
        ok = PresenterPresent8bpp(hwnd, buffer, pitch, palBytes, width, height, hudIcons);
        if (ok && withIcons) {
            // Consumed, as FUN_00464140 does on the software path. On a failed present the queue is
            // kept, so the GDI fallback below still blends the icons.
            *GlobalPtr<int>(kVaOverlayCount) = 0;
        }
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
    // Distance fade: the entries this flush draws get their slice depth before it reads them, and
    // tagging restarts at entry 0 afterwards (the caller resets the write cursor).
    CaptureDepthOnFlushStart();
    if (!GetConfig().captureEnabled && !GetConfig().gpuWorld && !CaptureSoftwareWorldWritesForbidden()) {
        uint32_t res = 1;
        if (g_orig_FUN_004688e0) {
            res = g_orig_FUN_004688e0();
        }
        CaptureDepthOnFlushEnd();
        return res;
    }
    CaptureOnBatchStart();
    uint32_t res = 1;
    if (g_orig_FUN_004688e0) {
        res = g_orig_FUN_004688e0();
    }
    CaptureOnBatchEnd();
    CaptureDepthOnFlushEnd();
    return res;
}

// Distance fade (D1): FUN_0045ac50 calls these once per road slice, far to near. Everything the
// slice queues into the polygon buffer between entry and return is tagged with its depth.
void __fastcall Detour_FUN_0045a790(uint8_t* slice, int edxArg) {
    CaptureDepthBeginSlice(slice);
    if (g_orig_FUN_0045a790) {
        g_orig_FUN_0045a790(slice, edxArg);
    }
    CaptureDepthEndSlice();
}

void __fastcall Detour_FUN_0045a870(uint8_t* slice, int edxArg) {
    CaptureDepthBeginSlice(slice);
    if (g_orig_FUN_0045a870) {
        g_orig_FUN_0045a870(slice, edxArg);
    }
    CaptureDepthEndSlice();
}

void __cdecl Detour_FUN_0042e850() {
    if (!GetConfig().captureEnabled && !GetConfig().gpuWorld && !CaptureSoftwareWorldWritesForbidden()) {
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
    if (!GetConfig().captureEnabled && !GetConfig().gpuWorld && !CaptureSoftwareWorldWritesForbidden()) {
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

// ---------------------------------------------------------------------------------------------
// Widescreen ([World] Widescreen in rr95gpu.ini, feature rr95-widescreen W1).
//
// Engine globals (preferred VAs verified in RoadRash95.exe, out/decomp.c):
// 0x00591c0c: int projection width (320): screen centre X = width / 2 (FUN_00433730, FUN_00459170),
//             object and road screen-X culls test against width << 16, and race init FUN_00457ef0
//             (0x457f4a..0x457f56) passes it with 0x00591c08 to FUN_0046de40, which sets the clip
//             rect DAT_006036c0..cc to (0, 0, width << HiRes, (height << HiRes) - 1).
// 0x00591c04: int viewport width (320) used only by the startup clip setup FUN_0046d620.
// 0x005a292a: uint8_t HiRes flag (1 = 640x480 surface coordinates).
// 0x005cd47c: uint8_t low-res inset mode flag (horizon FUN_00469770 takes a separate path).
// The engine never writes 0x00591c0c or 0x00591c04 (no store in out/decomp.c), so both stay at
// their shipped value unless this detour changes them.
constexpr uint32_t kVaProjectionWidth = 0x00591c0c;
constexpr uint32_t kVaViewportWidth   = 0x00591c04;
constexpr uint32_t kVaHiResFlag       = 0x005a292a;
constexpr uint32_t kVaInsetFlag       = 0x005cd47c;

// Graphics options menu item (W3): the state it shows, last read from the registry or the ini.
// Read by the menu (window procedure thread) and written there and by race init (game thread).
std::atomic<bool> g_menuWidescreenOn{false};
bool g_registryInvalidWarned = false;

// [World] Widescreen from the ini, overridden by HKCU\SOFTWARE\Electronic Arts\RoadRash 95\Widescreen
// (REG_BINARY 1 byte: 1 = auto, 0 = off) when that value exists.
EffectiveWidescreenSetting CurrentWidescreenSetting() {
    const int registryValue = ReadWidescreenRegistryValue();
    if (registryValue == kWidescreenRegistryInvalid && !g_registryInvalidWarned) {
        g_registryInvalidWarned = true;
        LOG_WARN("Registry value %s\\%s is not a 1-byte REG_BINARY or a REG_DWORD holding 0 or 1; using the ini",
                 kGameRegistryKey, kWidescreenRegistryValueName);
    }
    return ResolveEffectiveWidescreen(GetConfig().widescreen, registryValue);
}

bool g_engineWidthsRecorded = false;
int g_engineProjectionWidth = kEngineProjectionWidth;
int g_engineViewportWidth = kEngineProjectionWidth;

// Writes the projection width for the race FUN_00457ef0 is about to set up (before it reads
// 0x00591c0c at 0x457f50). Every race start re-decides; nothing changes mid-race.
void ApplyRaceProjectionWidth() {
    int* projection = GlobalPtr<int>(kVaProjectionWidth);
    int* viewport = GlobalPtr<int>(kVaViewportWidth);
    if (!g_engineWidthsRecorded) {
        g_engineProjectionWidth = *projection;
        g_engineViewportWidth = *viewport;
        g_engineWidthsRecorded = true;
    }

    if (g_gpuWorldTogglePending) {
        g_gpuWorldTogglePending = false;
        const bool newState = ToggleGpuWorld();
        CaptureInvalidateCache();
        LOG_INFO("Deferred GpuWorld toggle applied at race start: GpuWorld=%d", newState ? 1 : 0);
    }

    const Config& cfg = GetConfig();
    const EffectiveWidescreenSetting effective = CurrentWidescreenSetting();
    g_menuWidescreenOn = effective.setting.mode != WidescreenMode::Off;
    const std::string settingName = DescribeWidescreenSetting(effective.setting);
    const char* settingSource = WidescreenSettingSourceToString(effective.source);
    const bool hiRes = *GlobalPtr<const uint8_t>(kVaHiResFlag) != 0;
    const bool inset = *GlobalPtr<const uint8_t>(kVaInsetFlag) != 0;
    const bool gpuWorldEffective = cfg.gpuWorld && cfg.presenterEnabled && g_worldHooksInstalled &&
                                   !PresenterIsLatchedOff();
    int clientW = 0;
    int clientH = 0;
    HWND hwnd = *GlobalPtr<HWND>(kVaGameHwnd);
    RECT rc = {};
    if (hwnd != nullptr && GetClientRect(hwnd, &rc)) {
        clientW = rc.right - rc.left;
        clientH = rc.bottom - rc.top;
    }

    WidescreenResolution res =
        ResolveRaceProjectionWidth(effective.setting, gpuWorldEffective, hiRes, inset, clientW, clientH);
    const bool engineDefaults = g_engineProjectionWidth == kEngineProjectionWidth &&
                                g_engineViewportWidth == kEngineProjectionWidth;
    if (res.reason == WidescreenReason::Applied && !engineDefaults) {
        LOG_WARN("Widescreen: engine widths are %d/%d, not %d; keeping them unchanged",
                 g_engineProjectionWidth, g_engineViewportWidth, kEngineProjectionWidth);
        res.width = kEngineProjectionWidth;
    }

    const bool widen = res.width > kEngineProjectionWidth;
    const int projectionWidth = widen ? res.width : g_engineProjectionWidth;
    const int viewportWidth = widen ? res.width : g_engineViewportWidth;
    // Publish a wider width before the engine can use it (the world rasterizers are skipped from
    // here on), and clear it only after the engine widths are back to their shipped values.
    if (widen) {
        WidescreenSetAppliedWidth(res.width);
    }
    if (*projection != projectionWidth) {
        *projection = projectionWidth;
    }
    if (*viewport != viewportWidth) {
        *viewport = viewportWidth;
    }
    if (!widen) {
        WidescreenSetAppliedWidth(kEngineProjectionWidth);
    }
    // The race may use another track's sky: the widescreen horizon reads the surfaces again.
    CaptureInvalidateHorizonPanorama();

    if (widen) {
        LOG_INFO("Widescreen: race start with projection width %d (world source %dx%d, window %dx%d; setting %s from %s)",
                 res.width, res.width << 1, kSoftwareSurfaceHeight, clientW, clientH, settingName.c_str(),
                 settingSource);
    } else if (effective.setting.mode != WidescreenMode::Off) {
        LOG_INFO("Widescreen: race start with projection width %d (%s; HiRes=%d, Inset=%d, GpuWorld=%d, window %dx%d; "
                 "setting %s from %s)",
                 projectionWidth, WidescreenReasonToString(res.reason), hiRes ? 1 : 0, inset ? 1 : 0,
                 gpuWorldEffective ? 1 : 0, clientW, clientH, settingName.c_str(), settingSource);
    } else {
        LOG_INFO("Widescreen: race start with projection width %d (off; setting from %s)", projectionWidth,
                 settingSource);
    }
}

void __cdecl Detour_FUN_00457ef0() {
    ApplyRaceProjectionWidth();
    if (g_orig_FUN_00457ef0) {
        g_orig_FUN_00457ef0();
    }
}

// ---------------------------------------------------------------------------------------------
// Widescreen item in graphics options menu 19, slot 5, labelled in the game's language (feature
// rr95-widescreen W3, task I4 of rr95-gpu-hud-icons; item layout and verification in options_menu.h).
//
// Engine code (preferred VAs, same bytes in both exe builds):
// 0x0045b930: const char* __fastcall FUN_0045b930(item): text of a flag-0x100000 item; its only
//             caller is FUN_00439160 at 0x439455 (mov ecx, esi). For labels other than 0xd0/0x3da
//             it formats a volume slider from DAT_0058e010[item id * 6], so it is hooked and returns
//             the widescreen item's own text.
// 0x00439160: void __fastcall FUN_00439160(item, selected): draws one item; the on/off handlers
//             0x45b540/0x45b590 call it with ECX = EDX = item after toggling.
// 0x0043ce30: FUN_0043ce30 calls the select handler at item+0x2c with ECX = item (call eax) and
//             opens the menu it returns when nonzero.
constexpr uint32_t kVaItemRedraw = 0x00439160;
constexpr uint8_t kItemRedrawPrologue[9] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x94, 0x00, 0x00, 0x00};
using FUN_00439160_Fn = void(__fastcall*)(void* item, void* selected);

bool g_widescreenMenuInstalled = false;
uint8_t g_menuShippedSlots[kGraphicsMenuSlotsSize] = {};
std::string g_menuOnText;
std::string g_menuOffText;
char g_widescreenMenuText[96] = {};

const char* __fastcall Detour_FUN_0045b930(void* item) {
    if (g_widescreenMenuInstalled && item == GlobalPtr<void>(kVaWidescreenMenuItem)) {
        // The language is read on every draw: the game sets it at startup, before any menu opens.
        const std::string text = WidescreenMenuText(g_menuWidescreenOn.load(), g_menuOnText.c_str(),
                                                    g_menuOffText.c_str(), *GlobalPtr<const uint32_t>(kVaGameLanguage));
        strncpy_s(g_widescreenMenuText, text.c_str(), _TRUNCATE);
        return g_widescreenMenuText;
    }
    return g_orig_FUN_0045b930(item);
}

// Select handler of the widescreen item: flips the registry value, redraws the item, stays in the
// menu (returns 0). The width itself changes at the next race start (ApplyRaceProjectionWidth).
int __fastcall WidescreenMenuSelect(void* item) {
    const bool newOn = !g_menuWidescreenOn.load();
    if (WriteWidescreenRegistryValue(newOn)) {
        g_menuWidescreenOn = newOn;
        LOG_INFO("Options menu: Widescreen set to %s (HKCU\\%s\\%s = %d, REG_BINARY); applied at the next "
                 "race start",
                 newOn ? "On" : "Off", kGameRegistryKey, kWidescreenRegistryValueName, newOn ? 1 : 0);
    } else {
        LOG_WARN("Options menu: could not write HKCU\\%s\\%s; Widescreen stays %s", kGameRegistryKey,
                 kWidescreenRegistryValueName, newOn ? "Off" : "On");
    }
    auto fnRedraw = reinterpret_cast<FUN_00439160_Fn>(Rebase(kVaItemRedraw));
    fnRedraw(item, item);
    return 0;
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

// First 5 bytes at 0x00457ef0 (race init) in both original and patched exes:
// a0 59 80 61 00    mov  al, byte ptr ds:[0x00618059]   (5 bytes, absolute operand at offset 1)
// The image is dynamic base, so the loader relocates the operand; InstallHook rebases it before
// comparing. FUN_00457ef0 takes no register or stack arguments (it reads ESI only after push esi).
static rr95::HookSite g_site_FUN_00457ef0{
    "FUN_00457ef0",
    0x00457ef0,
    {0xA0, 0x59, 0x80, 0x61, 0x00},
    5,
    {5},
    reinterpret_cast<void*>(&Detour_FUN_00457ef0),
    reinterpret_cast<void**>(&g_orig_FUN_00457ef0),
    false,
    {1}
};

// First 6 bytes at 0x0045b930 (menu item value text) in both original and patched exes:
// 55                push ebp          (1 byte)
// 8b ec             mov  ebp, esp     (2 bytes)
// 83 ec 20          sub  esp, 0x20    (3 bytes)
// The next instruction (a1 c0 58 5a 00, security cookie) stays outside the span.
static rr95::HookSite g_site_FUN_0045b930{
    "FUN_0045b930",
    0x0045b930,
    {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x20},
    6,
    {1, 2, 3},
    reinterpret_cast<void*>(&Detour_FUN_0045b930),
    reinterpret_cast<void**>(&g_orig_FUN_0045b930),
    false
};

// First 10 bytes at 0x0045a790 (slice ground draw) and at 0x0045a870 (slice object draw) in both
// original and patched exes:
// 69 05 90 ec 59 00 24 02 00 00   imul eax, dword ptr ds:[0x0059ec90], 0x224   (10 bytes)
// The slice count operand at offset 2 is an absolute address the loader relocates; InstallHook
// rebases it before comparing. Both are __fastcall with the slice in ECX.
static rr95::HookSite g_site_FUN_0045a790{
    "FUN_0045a790",
    0x0045a790,
    {0x69, 0x05, 0x90, 0xEC, 0x59, 0x00, 0x24, 0x02, 0x00, 0x00},
    10,
    {10},
    reinterpret_cast<void*>(&Detour_FUN_0045a790),
    reinterpret_cast<void**>(&g_orig_FUN_0045a790),
    false,
    {2}
};

static rr95::HookSite g_site_FUN_0045a870{
    "FUN_0045a870",
    0x0045a870,
    {0x69, 0x05, 0x90, 0xEC, 0x59, 0x00, 0x24, 0x02, 0x00, 0x00},
    10,
    {10},
    reinterpret_cast<void*>(&Detour_FUN_0045a870),
    reinterpret_cast<void**>(&g_orig_FUN_0045a870),
    false,
    {2}
};

// Distance fade needs both slice draw hooks; without them no quad carries a depth and nothing fades.
static void InstallDistanceFadeHooks() {
    if (!GetConfig().distanceFade) {
        LOG_INFO("[World] DistanceFade=0: farthest road slices are not faded");
        return;
    }
    if (!rr95::InstallHook(g_site_FUN_0045a790, g_hostModule)) {
        LOG_WARN("Failed to install slice draw hook on FUN_0045a790; distance fade unavailable");
        return;
    }
    if (!rr95::InstallHook(g_site_FUN_0045a870, g_hostModule)) {
        LOG_WARN("Failed to install slice draw hook on FUN_0045a870; distance fade unavailable");
        rr95::RemoveHook(g_site_FUN_0045a790, g_hostModule);
        return;
    }
    CaptureSetDistanceFadeHooks(true);
    LOG_INFO("[World] DistanceFade=1 (start at %.2f of the far depth): slice draw hooks installed",
             static_cast<double>(GetConfig().distanceFadeStart));
}

// Copies bytes [4, kMenuItemSize) of each slot (never the HWND dword the engine owns), slot 5 and 6
// before slot 4 so slot 4 never leads to a half-written item.
static void WriteGraphicsMenuSlots(uint8_t* slots, const uint8_t* bytes) {
    for (int s = 2; s >= 0; --s) {
        const size_t base = static_cast<size_t>(s) * kMenuItemSize;
        std::memcpy(slots + base + 4, bytes + base + 4, kMenuItemSize - 4);
    }
}

// Adds the item when GpuWorld is configured on and the world and race-init hooks are installed
// (the caller checks the hooks). Any unexpected byte leaves the menu exactly as shipped.
static void InstallWidescreenMenuItem() {
    const Config& cfg = GetConfig();
    if (!cfg.gpuWorld || !cfg.presenterEnabled) {
        LOG_INFO("Options menu: GpuWorld is off; the Widescreen item is not added");
        return;
    }
    const intptr_t delta = reinterpret_cast<intptr_t>(g_hostModule) - static_cast<intptr_t>(rr95::kPreferredImageBase);
    const uint32_t count = *GlobalPtr<const uint32_t>(kVaGraphicsMenuCount);
    uint8_t* slots = GlobalPtr<uint8_t>(kVaGraphicsMenuSlots);
    const uint8_t* onOff = GlobalPtr<const uint8_t>(kVaOnOffStrings);
    const uint8_t* redraw = GlobalPtr<const uint8_t>(kVaItemRedraw);
    const bool countOk = count == kGraphicsMenuCount;
    const bool slotsOk = GraphicsMenuSlotsAreShipped(slots, delta);
    const bool stringsOk = std::memcmp(onOff, kShippedOnOffStrings, sizeof(kShippedOnOffStrings)) == 0;
    const bool redrawOk = std::memcmp(redraw, kItemRedrawPrologue, sizeof(kItemRedrawPrologue)) == 0;
    if (!countOk || !slotsOk || !stringsOk || !redrawOk) {
        LOG_WARN("Options menu: unexpected engine bytes (item count %u ok=%d, slots 4..6 ok=%d, On/Off strings ok=%d, "
                 "FUN_00439160 ok=%d); the graphics menu is left unchanged",
                 count, countOk ? 1 : 0, slotsOk ? 1 : 0, stringsOk ? 1 : 0, redrawOk ? 1 : 0);
        return;
    }
    g_menuOnText.assign(reinterpret_cast<const char*>(onOff));
    g_menuOffText.assign(reinterpret_cast<const char*>(onOff + 4));
    if (!rr95::InstallHook(g_site_FUN_0045b930, g_hostModule)) {
        LOG_WARN("Options menu: failed to install the hook on FUN_0045b930; the graphics menu is left unchanged");
        return;
    }
    const EffectiveWidescreenSetting effective = CurrentWidescreenSetting();
    g_menuWidescreenOn = effective.setting.mode != WidescreenMode::Off;
    std::memcpy(g_menuShippedSlots, slots, kGraphicsMenuSlotsSize);
    uint8_t rewritten[kGraphicsMenuSlotsSize];
    BuildWidescreenMenuSlots(slots, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&WidescreenMenuSelect)),
                             rewritten);
    g_widescreenMenuInstalled = true;
    WriteGraphicsMenuSlots(slots, rewritten);
    LOG_INFO("Options menu: Widescreen added to the graphics menu (slot 5 at 0x%08X), showing %s "
             "(setting %s from %s)",
             static_cast<unsigned>(reinterpret_cast<uintptr_t>(slots + kMenuItemSize)),
             g_menuWidescreenOn.load() ? "On" : "Off", DescribeWidescreenSetting(effective.setting).c_str(),
             WidescreenSettingSourceToString(effective.source));
}

static void RemoveWidescreenMenuItem(HMODULE hostModule) {
    if (g_widescreenMenuInstalled) {
        WriteGraphicsMenuSlots(
            reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(hostModule) +
                                       (kVaGraphicsMenuSlots - rr95::kPreferredImageBase)),
            g_menuShippedSlots);
        g_widescreenMenuInstalled = false;
    }
    rr95::RemoveHook(g_site_FUN_0045b930, hostModule);
    g_orig_FUN_0045b930 = nullptr;
}

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
            g_worldHooksInstalled = true;
            LOG_INFO("Capture/world hooks on FUN_004688e0, FUN_0042e850, and FUN_00467cb0 installed");
            InstallDistanceFadeHooks();
        }
    }

    const Config& cfg = GetConfig();
    if (cfg.invalidWidescreenValue) {
        LOG_WARN("[World] Widescreen value '%ls' is not off, auto, or an even width 320..640; widescreen is off",
                 cfg.rawWidescreenString.c_str());
    }
    if (g_worldHooksInstalled) {
        if (rr95::InstallHook(g_site_FUN_00457ef0, g_hostModule)) {
            if (cfg.widescreen.mode == WidescreenMode::Auto) {
                LOG_INFO("[World] Widescreen=auto (applied at each race start when GpuWorld is on)");
            } else if (cfg.widescreen.mode == WidescreenMode::Fixed) {
                LOG_INFO("[World] Widescreen=%d (applied at each race start when GpuWorld is on)",
                         cfg.widescreen.fixedWidth);
            }
            const EffectiveWidescreenSetting effective = CurrentWidescreenSetting();
            LOG_INFO("Widescreen: effective setting %s (source: %s)",
                     DescribeWidescreenSetting(effective.setting).c_str(),
                     WidescreenSettingSourceToString(effective.source));
            InstallWidescreenMenuItem();
        } else {
            LOG_WARN("Failed to install race init hook on FUN_00457ef0; widescreen unavailable");
        }
    } else if (cfg.widescreen.mode != WidescreenMode::Off) {
        LOG_WARN("[World] Widescreen needs the world hooks; widescreen unavailable");
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
    RemoveWidescreenMenuItem(hostModule);
    rr95::RemoveHook(g_site_FUN_00457ef0, hostModule);
    g_worldHooksInstalled = false;
    CaptureSetDistanceFadeHooks(false);
    rr95::RemoveHook(g_site_FUN_0045a870, hostModule);
    rr95::RemoveHook(g_site_FUN_0045a790, hostModule);
    rr95::RemoveHook(g_site_FUN_00467cb0, hostModule);
    rr95::RemoveHook(g_site_FUN_0042e850, hostModule);
    rr95::RemoveHook(g_site_FUN_004688e0, hostModule);
    rr95::RemoveHook(g_site_FUN_00460b00, hostModule);
    rr95::RemoveHook(g_site_FUN_0046ded0, hostModule);
    rr95::RemoveHook(g_site_FUN_0046e630, hostModule);
    g_orig_FUN_0043f580 = nullptr;
    g_orig_FUN_0043f7c0 = nullptr;
    g_orig_FUN_00457ef0 = nullptr;
    g_orig_FUN_0045a870 = nullptr;
    g_orig_FUN_0045a790 = nullptr;
    g_orig_FUN_00467cb0 = nullptr;
    g_orig_FUN_0042e850 = nullptr;
    g_orig_FUN_004688e0 = nullptr;
    g_orig_FUN_00460b00 = nullptr;
    g_orig_FUN_0046ded0 = nullptr;
    g_orig_FUN_0046e630 = nullptr;
}
