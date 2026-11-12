#include "capture.h"
#include "config.h"
#include "hook.h"
#include "log.h"
#include "presenter.h"
#include "texture_pack.h"
#include "widescreen.h"

#include <ddraw.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr uint32_t kMaxCapturedQuads = 4096;
constexpr uint32_t kCrcCacheSize     = 512;  // Power of two for open-addressed table
constexpr int32_t  kSurfaceWidth     = 640;
constexpr int32_t  kSurfaceHeight    = 480;

// Engine globals (preferred VAs in RoadRash95.exe):
// 0x00596a90: uint8_t[0xde][256] sprite CLUT remap tables (DAT_00596a90)
// 0x005aaa04: const uint8_t* dynamic sprite template PDAT pointer (DAT_005aaa04)
// 0x005aaa14: int32_t  snap shift (DAT_005aaa14; 15 in original HiRes, 16 in patched)
// 0x005aaa18: uint32_t snap mask  (DAT_005aaa18; 0xfffffffe in original HiRes, 0xffffffff in patched)
// 0x005ca79c: uint32_t global RSC file handle (DAT_005ca79c)
// 0x005ca7a0: uint32_t track/level RSC file handle (DAT_005ca7a0)
// 0x005cab48: uint32_t biker sprite resource buffer (DAT_005cab48)
// 0x005ce34c: uint8_t  surface locked flag set by FUN_004688e0 (DAT_005ce34c)
// 0x005a2934: int32_t  horizon blit width, 640 in HiRes (DAT_005a2934)
// 0x005a292a: uint8_t  HiRes flag (DAT_005a292a)
// 0x005cd4b4: int32_t  horizon blit origin X (DAT_005cd4b4; 0 in HiRes)
// 0x005cd4b8: int32_t  horizon blit origin Y (DAT_005cd4b8; 0 in HiRes)
// 0x005cd4c4: IDirectDrawSurface* horizon panorama columns [0, half) (DAT_005cd4c4)
// 0x005cd4c8: IDirectDrawSurface* horizon panorama columns [half, width) (DAT_005cd4c8)
// 0x005ce340: int32_t  horizon panorama width, 720 in HiRes (DAT_005ce340)
// 0x005ce344: int32_t  horizon surface width, half the panorama (DAT_005ce344)
// 0x006036a4: int32_t  horizon height in scanlines used by FUN_00469770 (DAT_006036a4)
// 0x006036b8: int32_t  locked 8bpp surface pitch set by FUN_004688e0 (DAT_006036b8)
// 0x006036bc: uint8_t* locked 8bpp viewport base pointer set by FUN_004688e0 (DAT_006036bc)
// 0x006036c0: int32_t[4] clip rect [xmin, ymin, xmax, ymax] (DAT_006036c0..0x006036cc)
// 0x006036e8: uint32_t race tick counter (DAT_006036e8)
// 0x00603740: uint32_t[4] streaming sprite family slot IDs (DAT_00603740..0x0060374c)
// 0x00618580: int16_t[16] 4 vertices {x, y, u, v} (DAT_00618580..0x0061859e)
// 0x006185a4: const uint8_t* active mip level texel pointer set by FUN_0042e850 (DAT_006185a4)
// 0x006185e0: const uint8_t* current render node poly = ccb + 0x68 (DAT_006185e0)
// 0x00612838: int32_t  camera heading term FUN_0045ac50 turns into the horizon scroll (DAT_00612838)
constexpr uint32_t kVaSpriteClutTable   = 0x00596a90;
constexpr uint32_t kVaSpritePdatPtr     = 0x005aaa04;
constexpr uint32_t kVaSnapShift         = 0x005aaa14;
constexpr uint32_t kVaSnapMask          = 0x005aaa18;
constexpr uint32_t kVaGlobalRscHandle   = 0x005ca79c;
constexpr uint32_t kVaTrackRscHandle    = 0x005ca7a0;
constexpr uint32_t kVaBikerSprBuf       = 0x005cab48;
constexpr uint32_t kVaSurfaceLockedFlag = 0x005ce34c;
constexpr uint32_t kVaHorizonBlitWidth  = 0x005a2934;
constexpr uint32_t kVaHiResFlag         = 0x005a292a;
constexpr uint32_t kVaHorizonBlitX      = 0x005cd4b4;
constexpr uint32_t kVaHorizonBlitY      = 0x005cd4b8;
constexpr uint32_t kVaHorizonSurfaceA   = 0x005cd4c4;
constexpr uint32_t kVaHorizonSurfaceB   = 0x005cd4c8;
constexpr uint32_t kVaHorizonPanoWidth  = 0x005ce340;
constexpr uint32_t kVaHorizonHalfWidth  = 0x005ce344;
constexpr uint32_t kVaHorizonCamera     = 0x00612838;
constexpr uint32_t kVaHorizonHeight     = 0x006036a4;
constexpr uint32_t kVaSurfacePitch      = 0x006036b8;
constexpr uint32_t kVaSurfaceBasePtr    = 0x006036bc;
constexpr uint32_t kVaClipRect          = 0x006036c0;
constexpr uint32_t kVaRaceTick          = 0x006036e8;
constexpr uint32_t kVaFamilySlots       = 0x00603740;
constexpr uint32_t kVaVertsInt16        = 0x00618580;
constexpr uint32_t kVaActiveMipPtr      = 0x006185a4;
constexpr uint32_t kVaCurrentPoly       = 0x006185e0;
// Distance fade (D1): polygon buffer pointer DAT_005ce844 (heap, 250 x 0xA8, allocated at startup),
// its write cursor DAT_005ce84c, road slices DAT_0060bd00 and their count DAT_0059ec90.
constexpr uint32_t kVaPolyBufferBase    = 0x005ce844;
constexpr uint32_t kVaPolyBufferCursor  = 0x005ce84c;
constexpr uint32_t kVaRoadSliceArray    = 0x0060bd00;
constexpr uint32_t kVaRoadSliceCountVar = 0x0059ec90;

HMODULE g_hostModule = nullptr;
// Heap-owning globals in this file are leaked on purpose (never destroyed), like the presenter and
// texture pack singletons: nothing should run at DLL_PROCESS_DETACH during process exit.
std::wstring& g_dumpDir = *new std::wstring();

CapturedQuad g_capturedQuads[kMaxCapturedQuads];
uint32_t g_capturedCount = 0;
uint32_t g_overflowCount = 0;
uint32_t g_batchIndex = 0;
// Set when the level state changed after the first batch of a frame; the texel cache is cleared
// before the first batch of the next frame (see DecideCacheInvalidation).
bool g_texelClearPending = false;
uint32_t g_raceFrameNumber = 0;
bool g_f11WasDown = false;
bool g_dumpThisFrame = false;
bool g_dumpTriggeredCheckedThisFrame = false;

// Diagnostics (D2b): thin textured quad verdicts on the GPU world path, this frame and since the
// last CaptureTakeThinQuadStats. Game thread only, like the rest of the capture state.
ThinQuadFrameCounts g_thinQuadFrame;
bool g_thinQuadFrameActive = false;
ThinQuadStats g_thinQuadStats;

// Distance fade (D1): slot tags of the polygon buffer and the far reference. Game thread only.
bool g_distanceFadeHooks = false;
DepthTagState g_depthTags;
float g_distanceFadeFarZ = 0.0f;

bool g_horizonCapturedThisFrame = false;
int32_t g_horizonHeight = 360;
uint8_t g_horizonBuffer[kSurfaceWidth * kSurfaceHeight];

// Widescreen horizon (W2): the composed wide layer and a copy of the engine's sky panorama, read
// from the horizon surfaces DAT_005cd4c4/DAT_005cd4c8 (8bpp system-memory surfaces created in
// FUN_0045f7f0 with caps 0x840). Only the game thread touches these.
uint8_t g_wideHorizonBuffer[kMaxWideHorizonWidth * kSurfaceHeight];
int32_t g_wideHorizonWidth = 0;
bool g_wideHorizonComposedThisFrame = false;

struct HorizonPanoramaCache {
    std::vector<uint8_t> pixels;  // width x height, pitch width
    int width = 0;
    int height = 0;
    uintptr_t surfaceA = 0;
    uintptr_t surfaceB = 0;
    bool valid = false;
    uint32_t frame = 0;             // widescreen horizon frames composed
    uint32_t retryAfterFrame = 0;   // no re-read before this frame after a failed one
    uint32_t mismatchFrames = 0;    // frames whose sides fell back to edge repeat
};
HorizonPanoramaCache& g_horizonPanorama = *new HorizonPanoramaCache();

LevelStateSnapshot g_levelSnapshot;

struct CrcCacheEntry {
    uint32_t keyA;
    uint32_t keyB;
    int32_t width;
    int32_t height;
    uint32_t crc32;
    bool occupied;
};

CrcCacheEntry g_crcCache[kCrcCacheSize];
uint32_t g_crcTable[256];
bool g_crcTableReady = false;

struct CachedTexturePixels {
    int32_t width = 0;
    int32_t height = 0;
    int32_t levels = 1;                          // engine mip levels stored back to back
    size_t levelOffset[kMaxTextureMipLevels] = {};
    std::vector<uint8_t> pixels;
};

// Leaked on purpose: freeing up to the cache cap of pixel buffers at process exit is pointless work
// under the loader lock.
std::unordered_map<uint64_t, CachedTexturePixels>& g_texPixelCache =
    *new std::unordered_map<uint64_t, CachedTexturePixels>();
uint32_t g_cacheGeneration = 1;
size_t g_texPixelCacheBytes = 0;
size_t g_texPixelCacheCapBytes = kDefaultTexturePixelCacheCapBytes;
uint32_t g_texPixelCacheEvictions = 0;

HudSentinelState g_hudSentinel;

void EnsureCrcTable() {
    if (g_crcTableReady) {
        return;
    }
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        g_crcTable[i] = c;
    }
    g_crcTableReady = true;
}

uint32_t UpdateCrc32(uint32_t runningCrc, const uint8_t* data, size_t len) {
    EnsureCrcTable();
    uint32_t c = runningCrc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        c = g_crcTable[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

uint32_t ComputeCrc32(const uint8_t* data, size_t len) {
    return UpdateCrc32(0u, data, len);
}

uint32_t LookupOrComputeLevel0Crc32(uint32_t keyA,
                                    uint32_t keyB,
                                    const uint8_t* level0Ptr,
                                    int32_t width,
                                    int32_t height,
                                    int32_t rowPitch,
                                    const uint8_t* extraData,
                                    size_t extraLen) {
    if (!level0Ptr || width <= 0 || height <= 0 || width > 4096 || height > 4096 || rowPitch < width) {
        return 0;
    }
    uint32_t hash = (keyA * 2654435761u) ^ (keyB * 2246822519u) ^
                    (static_cast<uint32_t>(width) * 3266489917u) ^ static_cast<uint32_t>(height);
    uint32_t idx = hash & (kCrcCacheSize - 1u);
    for (uint32_t probe = 0; probe < kCrcCacheSize; ++probe) {
        CrcCacheEntry& entry = g_crcCache[idx];
        if (!entry.occupied) {
            entry.keyA = keyA;
            entry.keyB = keyB;
            entry.width = width;
            entry.height = height;
            entry.crc32 = ComputeTextureRectCrc32(level0Ptr, width, height, rowPitch, extraData, extraLen);
            entry.occupied = true;
            return entry.crc32;
        }
        if (entry.keyA == keyA && entry.keyB == keyB && entry.width == width && entry.height == height) {
            return entry.crc32;
        }
        idx = (idx + 1u) & (kCrcCacheSize - 1u);
    }
    return ComputeTextureRectCrc32(level0Ptr, width, height, rowPitch, extraData, extraLen);
}

uintptr_t Rebase(uint32_t preferredVa) {
    return reinterpret_cast<uintptr_t>(g_hostModule) + (preferredVa - rr95::kPreferredImageBase);
}

template <typename T>
T* GlobalPtr(uint32_t preferredVa) {
    return reinterpret_cast<T*>(Rebase(preferredVa));
}

void CheckAndInvalidateCacheOnLevelChange() {
    if (!g_hostModule) {
        return;
    }
    const uint32_t globalRsc = *GlobalPtr<const uint32_t>(kVaGlobalRscHandle);
    const uint32_t trackRsc = *GlobalPtr<const uint32_t>(kVaTrackRscHandle);
    const uint32_t spritePdat = *GlobalPtr<const uint32_t>(kVaSpritePdatPtr);
    const uint32_t bikerBuf = *GlobalPtr<const uint32_t>(kVaBikerSprBuf);
    const uint32_t* families = GlobalPtr<const uint32_t>(kVaFamilySlots);
    const uint32_t raceTick = *GlobalPtr<const uint32_t>(kVaRaceTick);

    if (g_texelClearPending && g_batchIndex == 0) {
        CaptureInvalidateCache();
    }
    const bool changed =
        UpdateLevelStateSnapshot(g_levelSnapshot, globalRsc, trackRsc, spritePdat, bikerBuf, families, raceTick);
    switch (DecideCacheInvalidation(changed, g_batchIndex)) {
    case CacheInvalidation::Full:
        CaptureInvalidateCache();
        break;
    case CacheInvalidation::CrcNowTexelsNextFrame:
        std::memset(g_crcCache, 0, sizeof(g_crcCache));
        if (!g_texelClearPending) {
            LOG_INFO("Level state changed in batch %u of frame %u; texel cache clear deferred to the next frame",
                     g_batchIndex, g_raceFrameNumber);
        }
        g_texelClearPending = true;
        break;
    case CacheInvalidation::None:
        break;
    }
}

// Frames to wait before reading the horizon surfaces again after a read that failed or did not
// reproduce the engine's blit.
constexpr uint32_t kHorizonPanoramaRetryFrames = 120;

// Copies the two horizon surfaces into g_horizonPanorama as one width x height panorama (columns
// [0, half) from DAT_005cd4c4, [half, width) from DAT_005cd4c8, the order FUN_00468560 fills them
// from the file). Read-only locks on system-memory surfaces; nothing is written to the game.
bool ReadHorizonPanorama(int width, int half, int height) {
    HorizonPanoramaCache& c = g_horizonPanorama;
    c.valid = false;
    IDirectDrawSurface* surfaces[2] = {*GlobalPtr<IDirectDrawSurface*>(kVaHorizonSurfaceA),
                                       *GlobalPtr<IDirectDrawSurface*>(kVaHorizonSurfaceB)};
    if (!surfaces[0] || !surfaces[1] || half <= 0 || width != 2 * half || width > kMaxHorizonPanoramaWidth ||
        height <= 0 || height > kSurfaceHeight) {
        return false;
    }
    c.pixels.assign(static_cast<size_t>(width) * height, 0);
    for (int i = 0; i < 2; ++i) {
        DDSURFACEDESC desc = {};
        desc.dwSize = sizeof(desc);
        const HRESULT hr = surfaces[i]->Lock(nullptr, &desc, DDLOCK_WAIT | DDLOCK_READONLY, nullptr);
        if (FAILED(hr)) {
            LOG_WARN("Widescreen horizon: Lock of horizon surface %d failed (0x%08lX)", i,
                     static_cast<unsigned long>(hr));
            return false;
        }
        const bool formatOk = (desc.dwFlags & DDSD_PIXELFORMAT) == 0 || desc.ddpfPixelFormat.dwRGBBitCount == 8;
        const bool ok = desc.lpSurface != nullptr && desc.lPitch >= half && static_cast<int>(desc.dwWidth) >= half &&
                        static_cast<int>(desc.dwHeight) >= height && formatOk;
        if (ok) {
            const auto* src = static_cast<const uint8_t*>(desc.lpSurface);
            for (int y = 0; y < height; ++y) {
                std::memcpy(&c.pixels[static_cast<size_t>(y) * width + static_cast<size_t>(i) * half],
                            src + static_cast<ptrdiff_t>(y) * desc.lPitch, static_cast<size_t>(half));
            }
        }
        surfaces[i]->Unlock(nullptr);
        if (!ok) {
            LOG_WARN("Widescreen horizon: horizon surface %d is %lux%lu pitch %ld, %lu bpp; expected %dx%d 8 bpp", i,
                     desc.dwWidth, desc.dwHeight, desc.lPitch, desc.ddpfPixelFormat.dwRGBBitCount, half, height);
            return false;
        }
    }
    c.width = width;
    c.height = height;
    c.surfaceA = reinterpret_cast<uintptr_t>(surfaces[0]);
    c.surfaceB = reinterpret_cast<uintptr_t>(surfaces[1]);
    c.valid = true;
    return true;
}

// Widescreen only: builds g_wideHorizonBuffer from this frame's horizon snapshot. The sides
// continue the panorama when the blit geometry is the HiRes one and the panorama reproduces the
// snapshot (ComposeWideHorizon verifies it); otherwise they repeat the edge columns.
void ComposeWideHorizonIfActive() {
    g_wideHorizonComposedThisFrame = false;
    if (!WidescreenIsActive()) {
        return;
    }
    HorizonPanoramaCache& c = g_horizonPanorama;
    ++c.frame;
    int projectionWidth = WidescreenGetAppliedWidth();
    if (projectionWidth > kMaxProjectionWidth) {
        projectionWidth = kMaxProjectionWidth;
    }

    WideHorizonInput in;
    in.snapshot = g_horizonBuffer;
    in.snapshotRows = g_horizonHeight;
    in.wideWidth = projectionWidth * 2;
    in.cropTop = 0;  // FUN_0045ac50 calls FUN_00469770 with EDX = 0 (0x45ad06)

    const bool hiRes = *GlobalPtr<const uint8_t>(kVaHiResFlag) != 0;
    const int panoW = *GlobalPtr<const int32_t>(kVaHorizonPanoWidth);
    const int half = *GlobalPtr<const int32_t>(kVaHorizonHalfWidth);
    const int blitX = *GlobalPtr<const int32_t>(kVaHorizonBlitX);
    const int blitY = *GlobalPtr<const int32_t>(kVaHorizonBlitY);
    const int blitW = *GlobalPtr<const int32_t>(kVaHorizonBlitWidth);
    const int surfaceH = *GlobalPtr<const int32_t>(kVaHorizonHeight);
    const bool geometryOk = hiRes && blitX == 0 && blitY == 0 && blitW == kHorizonBlitWidth && half > 0 &&
                            panoW == 2 * half && panoW <= kMaxHorizonPanoramaWidth && surfaceH == g_horizonHeight;
    in.blitY = blitY;

    bool freshRead = false;
    auto panoramaMatches = [&]() {
        return c.valid && c.width == panoW && c.height == surfaceH &&
               c.surfaceA == reinterpret_cast<uintptr_t>(*GlobalPtr<IDirectDrawSurface*>(kVaHorizonSurfaceA)) &&
               c.surfaceB == reinterpret_cast<uintptr_t>(*GlobalPtr<IDirectDrawSurface*>(kVaHorizonSurfaceB));
    };
    auto readNow = [&]() {
        const bool firstRead = !c.valid;
        freshRead = ReadHorizonPanorama(panoW, half, surfaceH);
        if (freshRead && firstRead) {
            LOG_INFO("Widescreen horizon: read the %dx%d sky panorama from the horizon surfaces", panoW, surfaceH);
        }
        return freshRead;
    };
    if (geometryOk) {
        in.scroll = HorizonScrollFromCamera(*GlobalPtr<const int32_t>(kVaHorizonCamera), true);
        if (!panoramaMatches() && c.frame >= c.retryAfterFrame) {
            readNow();
        }
        if (panoramaMatches()) {
            in.panorama = HorizonPanoramaView{c.pixels.data(), c.width, c.height, c.width};
        }
    }

    WideHorizonResult result = ComposeWideHorizon(in, g_wideHorizonBuffer);
    if (result == WideHorizonResult::EdgeRepeat && in.panorama.pixels != nullptr && !freshRead &&
        c.frame >= c.retryAfterFrame) {
        // The cached panorama no longer reproduces the blit (the surfaces were refilled): read once.
        if (readNow()) {
            in.panorama = HorizonPanoramaView{c.pixels.data(), c.width, c.height, c.width};
            result = ComposeWideHorizon(in, g_wideHorizonBuffer);
        }
    }
    if (result == WideHorizonResult::Invalid) {
        return;
    }
    if (result == WideHorizonResult::EdgeRepeat) {
        if (c.frame >= c.retryAfterFrame) {
            c.retryAfterFrame = c.frame + kHorizonPanoramaRetryFrames;
        }
        if (c.mismatchFrames++ % 600 == 0) {
            LOG_WARN("Widescreen horizon: sides repeat the edge columns (%u frame(s); geometry %s: HiRes=%d "
                     "blit %d,%d w%d panorama %d/%d h%d/%d; panorama %s)",
                     c.mismatchFrames, geometryOk ? "ok" : "unexpected", hiRes ? 1 : 0, blitX, blitY, blitW, panoW,
                     half, surfaceH, g_horizonHeight, in.panorama.pixels ? "did not match the blit" : "unavailable");
        }
    }
    g_wideHorizonWidth = in.wideWidth;
    g_wideHorizonComposedThisFrame = true;
}

void SnapshotHorizonAndClearSurfaceIfNeeded() {
    if (g_horizonCapturedThisFrame || !g_hostModule || !CaptureShouldSkipSoftwareRasterizer()) {
        return;
    }
    if (*GlobalPtr<const uint8_t>(kVaSurfaceLockedFlag) == 0) {
        return;
    }
    uint8_t* surfBase = *GlobalPtr<uint8_t*>(kVaSurfaceBasePtr);
    const int32_t surfPitch = *GlobalPtr<int32_t>(kVaSurfacePitch);
    if (!surfBase || surfPitch < kSurfaceWidth) {
        return;
    }

    int32_t horizonH = *GlobalPtr<int32_t>(kVaHorizonHeight);
    if (horizonH <= 0 || horizonH > kSurfaceHeight) {
        horizonH = 360;
    }
    g_horizonHeight = horizonH;

    // Copy Layer 1 (sky/horizon blitted by FUN_00469770 before FUN_004688e0) into g_horizonBuffer.
    for (int32_t y = 0; y < horizonH; ++y) {
        std::memcpy(g_horizonBuffer + static_cast<size_t>(y) * kSurfaceWidth,
                    surfBase + static_cast<ptrdiff_t>(y) * surfPitch,
                    kSurfaceWidth);
    }
    // Extend bottom horizon scanline across the remaining rows so there is never an uninitialized seam.
    const uint8_t* fillRow = (horizonH > 0)
                                 ? (g_horizonBuffer + static_cast<size_t>(horizonH - 1) * kSurfaceWidth)
                                 : nullptr;
    for (int32_t y = horizonH; y < kSurfaceHeight; ++y) {
        uint8_t* dstRow = g_horizonBuffer + static_cast<size_t>(y) * kSurfaceWidth;
        if (fillRow) {
            std::memcpy(dstRow, fillRow, kSurfaceWidth);
        } else {
            std::memset(dstRow, 0, kSurfaceWidth);
        }
    }
    ComposeWideHorizonIfActive();

    // Clear the locked 8bpp surface to this frame's HUD sentinel so that after FUN_004688e0
    // finishes and FUN_0045d6d0 draws the dashboard/HUD/gauges/text onto DAT_005cd4bc, only pixels
    // actually written by Layer 3 (including black index 0 gauge interiors and text drop-shadows)
    // differ from the sentinel. See HudSentinelState in capture.h for how collisions are handled.
    const uint8_t sentinel = HudSentinelActive(g_hudSentinel);
    for (int32_t y = 0; y < kSurfaceHeight; ++y) {
        std::memset(surfBase + static_cast<ptrdiff_t>(y) * surfPitch, sentinel, kSurfaceWidth);
    }

    g_horizonCapturedThisFrame = true;
}

void WriteBe32(std::vector<uint8_t>& out, uint32_t val) {
    out.push_back(static_cast<uint8_t>((val >> 24) & 0xFFu));
    out.push_back(static_cast<uint8_t>((val >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((val >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(val & 0xFFu));
}

void WritePngChunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* data, size_t len) {
    WriteBe32(out, static_cast<uint32_t>(len));
    const size_t typeOffset = out.size();
    out.push_back(static_cast<uint8_t>(type[0]));
    out.push_back(static_cast<uint8_t>(type[1]));
    out.push_back(static_cast<uint8_t>(type[2]));
    out.push_back(static_cast<uint8_t>(type[3]));
    if (data && len > 0) {
        out.insert(out.end(), data, data + len);
    }
    const uint32_t crc = ComputeCrc32(out.data() + typeOffset, 4 + len);
    WriteBe32(out, crc);
}

bool WriteFrameJson(const std::wstring& path, uint32_t frameNum, int width, int height) {
    const int32_t snapShift = g_hostModule ? *GlobalPtr<int32_t>(kVaSnapShift) : 16;
    const uint32_t snapMask = g_hostModule ? *GlobalPtr<uint32_t>(kVaSnapMask) : 0xFFFFFFFFu;
    return WriteFrameJsonFromQuads(
        path, frameNum, width, height, snapShift, snapMask, g_capturedQuads, g_capturedCount, g_overflowCount);
}

}  // namespace

// Distance fade (D1), defined with the other engine-side fade functions below.
static int32_t DepthOfPoly(const uint8_t* poly);

CacheInvalidation DecideCacheInvalidation(bool levelChanged, uint32_t batchIndex) {
    if (!levelChanged) {
        return CacheInvalidation::None;
    }
    return (batchIndex == 0) ? CacheInvalidation::Full : CacheInvalidation::CrcNowTexelsNextFrame;
}

uint32_t ComputeTextureRectCrc32(const uint8_t* level0Ptr,
                                 int32_t width,
                                 int32_t height,
                                 int32_t rowPitch,
                                 const uint8_t* extraData,
                                 size_t extraLen) {
    if (!level0Ptr || width <= 0 || height <= 0 || rowPitch < width) {
        return 0;
    }
    uint32_t crc = 0u;
    if (rowPitch == width) {
        crc = UpdateCrc32(crc, level0Ptr, static_cast<size_t>(width) * static_cast<size_t>(height));
    } else {
        for (int32_t y = 0; y < height; ++y) {
            crc = UpdateCrc32(crc,
                              level0Ptr + static_cast<size_t>(y) * static_cast<size_t>(rowPitch),
                              static_cast<size_t>(width));
        }
    }
    if (extraData && extraLen > 0) {
        crc = UpdateCrc32(crc, extraData, extraLen);
    }
    return crc;
}

void ClampSpriteDimensions(const int32_t uvBox[4],
                           int32_t pdatPitch,
                           int32_t pdatHeight,
                           int32_t& outW,
                           int32_t& outH) {
    const int32_t maxW = (pdatPitch > 0) ? pdatPitch : 1;
    const int32_t maxH = (pdatHeight > 0) ? pdatHeight : 1;
    if (!uvBox) {
        outW = maxW;
        outH = maxH;
        return;
    }
    int32_t w = uvBox[2] - uvBox[0];
    int32_t h = uvBox[3] - uvBox[1];
    if (w < 1) w = 1;
    if (w > maxW) w = maxW;
    if (h < 1) h = 1;
    if (h > maxH) h = maxH;
    outW = w;
    outH = h;
}

bool UpdateLevelStateSnapshot(LevelStateSnapshot& snap,
                              uint32_t globalRsc,
                              uint32_t trackRsc,
                              uint32_t spritePdat,
                              uint32_t bikerBuf,
                              const uint32_t familySlots[4],
                              uint32_t raceTick) {
    if (!snap.initialized) {
        snap.initialized = true;
        snap.globalRsc = globalRsc;
        snap.trackRsc = trackRsc;
        snap.spritePdat = spritePdat;
        snap.bikerBuf = bikerBuf;
        for (int i = 0; i < 4; ++i) {
            snap.familySlots[i] = familySlots ? familySlots[i] : 0u;
        }
        snap.lastRaceTick = raceTick;
        return false;
    }

    bool changed = (globalRsc != snap.globalRsc) ||
                   (trackRsc != snap.trackRsc) ||
                   (spritePdat != snap.spritePdat) ||
                   (bikerBuf != snap.bikerBuf) ||
                   (raceTick < snap.lastRaceTick);
    if (familySlots) {
        for (int i = 0; i < 4; ++i) {
            if (familySlots[i] != snap.familySlots[i]) {
                changed = true;
            }
        }
    }

    if (changed) {
        snap.globalRsc = globalRsc;
        snap.trackRsc = trackRsc;
        snap.spritePdat = spritePdat;
        snap.bikerBuf = bikerBuf;
        if (familySlots) {
            for (int i = 0; i < 4; ++i) {
                snap.familySlots[i] = familySlots[i];
            }
        }
    }
    snap.lastRaceTick = raceTick;
    return changed;
}

static bool CullTexturedQuadBounds(const CapturedVertex verts[4],
                                   const int32_t clipRect[4],
                                   uint32_t pdatPtr,
                                   bool allowSingleScanline) {
    if (!verts || !clipRect || pdatPtr == 0) {
        return true;
    }
    int32_t minX = verts[0].x;
    int32_t maxX = verts[0].x;
    int32_t minY = verts[0].y;
    int32_t maxY = verts[0].y;
    for (int i = 1; i < 4; ++i) {
        if (verts[i].x < minX) minX = verts[i].x;
        if (verts[i].x > maxX) maxX = verts[i].x;
        if (verts[i].y < minY) minY = verts[i].y;
        if (verts[i].y > maxY) maxY = verts[i].y;
    }
    // Engine semantics (FUN_0042e850):
    // - local_c == local_18 (minY == maxY) early-outs before mip selection and draws 0 scanlines.
    // - Y scanlines are inclusive [clipT, clipB] (clipT > clipB culls).
    // - X spans are half-open [clipL, clipR) where uVar13 = right - left (clipL >= clipR draws 0 pixels).
    if (minY == maxY && !allowSingleScanline) {
        return true;
    }
    const int32_t clipL = (minX > clipRect[0]) ? minX : clipRect[0];
    const int32_t clipT = (minY > clipRect[1]) ? minY : clipRect[1];
    const int32_t clipR = (maxX < clipRect[2]) ? maxX : clipRect[2];
    const int32_t clipB = (maxY < clipRect[3]) ? maxY : clipRect[3];
    return (clipL >= clipR) || (clipT > clipB);
}

bool ShouldCullTexturedQuad(const CapturedVertex verts[4],
                            const int32_t clipRect[4],
                            uint32_t pdatPtr) {
    return CullTexturedQuadBounds(verts, clipRect, pdatPtr, false);
}

static bool IsSingleScanline(const CapturedVertex verts[4]) {
    return verts[0].y == verts[1].y && verts[0].y == verts[2].y && verts[0].y == verts[3].y;
}

static bool CornersSpanHeight(const CapturedQuad& q) {
    for (int c = 1; c < 4; ++c) {
        if (q.corners16_16[c * 2 + 1] != q.corners16_16[1]) {
            return true;
        }
    }
    return false;
}

ThinQuadVerdict ClassifyThinTexturedQuadForGpu(const CapturedQuad& q, bool subPixel, bool keepThinQuads) {
    if (!IsSingleScanline(q.verts)) {
        return ThinQuadVerdict::NotThin;
    }
    if (!keepThinQuads || !subPixel) {
        return ThinQuadVerdict::CulledDisabled;
    }
    if (q.pdatPtr == 0) {
        return ThinQuadVerdict::CulledNoPdat;
    }
    if (!CornersSpanHeight(q)) {
        return ThinQuadVerdict::CulledZeroHeight;
    }
    // FUN_0042e850 draws no scanline for these quads; at 640x480 the neighbouring rows cover that
    // line. The GPU world is drawn larger and at the 16.16 corners, so a quad whose corners still
    // span a height is drawn there instead of leaving a gap. Every other cull rule stays.
    const SubPixelVertices spv = ResolveSubPixelVertices(q);
    if (!spv.useCorners) {
        return ThinQuadVerdict::CulledNoCornerMatch;
    }
    float minY = spv.y[0];
    float maxY = spv.y[0];
    for (int i = 1; i < 4; ++i) {
        minY = std::min(minY, spv.y[i]);
        maxY = std::max(maxY, spv.y[i]);
    }
    if (!(maxY > minY)) {
        return ThinQuadVerdict::CulledZeroHeight;
    }
    return CullTexturedQuadBounds(q.verts, q.clipRect, q.pdatPtr, true) ? ThinQuadVerdict::CulledClip
                                                                         : ThinQuadVerdict::Kept;
}

static bool ShouldCullTexturedQuadForVerdict(const CapturedQuad& q, ThinQuadVerdict verdict) {
    if (verdict == ThinQuadVerdict::NotThin) {
        return ShouldCullTexturedQuad(q.verts, q.clipRect, q.pdatPtr);
    }
    return verdict != ThinQuadVerdict::Kept;
}

bool ShouldCullTexturedQuadForGpu(const CapturedQuad& q, bool subPixel, bool keepThinQuads) {
    return ShouldCullTexturedQuadForVerdict(q, ClassifyThinTexturedQuadForGpu(q, subPixel, keepThinQuads));
}

void RecordThinQuadVerdict(ThinQuadFrameCounts& frame, ThinQuadVerdict verdict) {
    const size_t index = static_cast<size_t>(verdict);
    if (index < kThinQuadVerdictCount) {
        ++frame.count[index];
    }
}

uint32_t ThinQuadCulledCount(const ThinQuadFrameCounts& frame) {
    uint32_t culled = 0;
    for (size_t i = static_cast<size_t>(ThinQuadVerdict::CulledDisabled); i < kThinQuadVerdictCount; ++i) {
        culled += frame.count[i];
    }
    return culled;
}

void AccumulateThinQuadFrame(ThinQuadStats& stats, const ThinQuadFrameCounts& frame) {
    ++stats.frames;
    for (size_t i = 0; i < kThinQuadVerdictCount; ++i) {
        stats.total[i] += frame.count[i];
    }
    stats.maxCulledPerFrame = std::max(stats.maxCulledPerFrame, ThinQuadCulledCount(frame));
}

RoadSliceFrameSample SampleRoadSliceFlags(const uint8_t* sliceArray, int32_t count) {
    RoadSliceFrameSample out;
    if (!sliceArray || count < 1 || count > kRoadSliceMaxCount) {
        return out;
    }
    out.valid = true;
    // FUN_00459cb0 writes the flag of slices 0..count inclusive.
    for (int32_t i = 0; i <= count; ++i) {
        ++out.slices;
        if (sliceArray[static_cast<size_t>(i) * kRoadSliceStride + kRoadSliceFlagOffset] == 0) {
            ++out.dropped;
            if (out.nearestDropped < 0) {
                out.nearestDropped = i;
            }
            out.farthestDropped = i;
        }
    }
    return out;
}

void AccumulateRoadSliceSample(RoadSliceStats& stats, const RoadSliceFrameSample& sample) {
    if (!sample.valid) {
        ++stats.invalidFrames;
        return;
    }
    ++stats.frames;
    stats.slicesTotal += sample.slices;
    stats.droppedTotal += sample.dropped;
    stats.maxDroppedInFrame = std::max(stats.maxDroppedInFrame, sample.dropped);
    if (sample.nearestDropped >= 0) {
        ++stats.framesWithDrop;
        stats.nearestDroppedSum += static_cast<uint64_t>(sample.nearestDropped);
        if (stats.minNearestDropped < 0 || sample.nearestDropped < stats.minNearestDropped) {
            stats.minNearestDropped = sample.nearestDropped;
        }
    }
}

int32_t PolyBufferIndex(uintptr_t entry, uintptr_t base, bool allowEnd) {
    if (base == 0 || entry < base) {
        return -1;
    }
    const uintptr_t offset = entry - base;
    if (offset % kPolyEntrySize != 0) {
        return -1;
    }
    const uintptr_t index = offset / kPolyEntrySize;
    const uintptr_t limit = allowEnd ? kPolyBufferEntries : kPolyBufferEntries - 1;
    return index <= limit ? static_cast<int32_t>(index) : -1;
}

void DepthTagFill(DepthTagState& s, int32_t cursorIndex) {
    if (cursorIndex < 0 || cursorIndex > static_cast<int32_t>(kPolyBufferEntries)) {
        return;
    }
    const uint32_t cursor = static_cast<uint32_t>(cursorIndex);
    if (cursor < s.lastTagged) {
        s.lastTagged = 0;
    }
    for (uint32_t i = s.lastTagged; i < cursor; ++i) {
        s.tags[i] = s.currentZ;
    }
    s.lastTagged = cursor;
}

void DepthTagBeginSlice(DepthTagState& s, int32_t cursorIndex, int32_t z) {
    DepthTagFill(s, cursorIndex);
    s.currentZ = z > 0 ? z : kNoDepth;
    s.frameMaxZ = std::max(s.frameMaxZ, s.currentZ);
}

void DepthTagEndSlice(DepthTagState& s, int32_t cursorIndex) {
    DepthTagFill(s, cursorIndex);
    s.currentZ = kNoDepth;
}

void DepthTagFlushEnd(DepthTagState& s) {
    const uint32_t drawn = std::min(s.lastTagged, kPolyBufferEntries);
    for (uint32_t i = 0; i < drawn; ++i) {
        s.tags[i] = kNoDepth;
    }
    s.lastTagged = 0;
}

void DepthTagEndFrame(DepthTagState& s) {
    for (int32_t& tag : s.tags) {
        tag = kNoDepth;
    }
    s.lastTagged = 0;
    s.frameMaxZ = 0;
    s.currentZ = kNoDepth;
}

void DistanceFadeReset(DepthTagState& s, float& farZ) {
    s = DepthTagState{};
    farZ = 0.0f;
}

int32_t DepthTagLookup(const DepthTagState& s, int32_t entryIndex) {
    if (entryIndex < 0 || entryIndex >= static_cast<int32_t>(kPolyBufferEntries)) {
        return kNoDepth;
    }
    return s.tags[entryIndex];
}

int32_t ReadRoadSliceDepth(const uint8_t* slice, const uint8_t* sliceArray, int32_t count) {
    if (!slice || !sliceArray || count < 1 || count > kRoadSliceMaxCount || slice < sliceArray) {
        return kNoDepth;
    }
    const uintptr_t offset = static_cast<uintptr_t>(slice - sliceArray);
    if (offset % kRoadSliceStride != 0 || offset / kRoadSliceStride >= static_cast<uintptr_t>(count)) {
        return kNoDepth;
    }
    uint32_t rowAddr = 0;
    std::memcpy(&rowAddr, slice + kRoadSliceRowPtrOffset, sizeof(rowAddr));
    // Rows sit inline in the slices; the array holds kRoadSliceMaxCount + 1 slices.
    const uintptr_t lo = reinterpret_cast<uintptr_t>(sliceArray);
    const uintptr_t hi = lo + static_cast<uintptr_t>(kRoadSliceMaxCount + 1) * kRoadSliceStride;
    if (rowAddr < lo || static_cast<uintptr_t>(rowAddr) + kRoadRowDepthOffset + sizeof(int32_t) > hi) {
        return kNoDepth;
    }
    int32_t z = 0;
    std::memcpy(&z, reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(rowAddr)) + kRoadRowDepthOffset, sizeof(z));
    return z > 0 ? z : kNoDepth;
}

float DistanceFadeFactor(int32_t z, float farZ, float startFraction, bool enabled) {
    if (!enabled || z <= 0 || !(farZ > 0.0f)) {
        return 0.0f;
    }
    const float fz = static_cast<float>(z);
    if (fz >= farZ) {
        return 1.0f;
    }
    const float start = farZ * startFraction;
    if (fz <= start) {
        return 0.0f;
    }
    return (fz - start) / (farZ - start);
}

float UpdateDistanceFadeFarZ(float previous, int32_t frameMaxZ) {
    if (frameMaxZ <= 0) {
        return 0.0f;  // no slice this frame (menu, loading, new track): the next one starts fresh
    }
    return std::max(static_cast<float>(std::max(frameMaxZ, 0)), previous * kDistanceFadeFarZDecay);
}

bool ShouldCullFlatQuad(const CapturedVertex verts[4],
                        const int32_t clipRect[4]) {
    if (!verts || !clipRect) {
        return true;
    }
    int32_t minX = verts[0].x;
    int32_t maxX = verts[0].x;
    int32_t minY = verts[0].y;
    int32_t maxY = verts[0].y;
    for (int i = 1; i < 4; ++i) {
        if (verts[i].x < minX) minX = verts[i].x;
        if (verts[i].x > maxX) maxX = verts[i].x;
        if (verts[i].y < minY) minY = verts[i].y;
        if (verts[i].y > maxY) maxY = verts[i].y;
    }
    // Engine semantics (FUN_00467cb0 & FUN_00468ce0):
    // - Y scanlines are inclusive [clipT, clipB]; minY == maxY takes the single-scanline FUN_00468ce0
    //   fast path and is NOT culled when clipT <= clipB.
    // - X spans are half-open [clipL, clipR) where ECX = xR - xL is passed to FUN_00468ce0
    //   (clipL >= clipR draws 0 pixels).
    const int32_t clipL = (minX > clipRect[0]) ? minX : clipRect[0];
    const int32_t clipT = (minY > clipRect[1]) ? minY : clipRect[1];
    const int32_t clipR = (maxX < clipRect[2]) ? maxX : clipRect[2];
    const int32_t clipB = (maxY < clipRect[3]) ? maxY : clipRect[3];
    return (clipL >= clipR) || (clipT > clipB);
}

SubPixelVertices ResolveSubPixelVertices(const CapturedQuad& q) {
    SubPixelVertices out;
    for (int i = 0; i < 4; ++i) {
        out.x[i] = static_cast<float>(q.verts[i].x);
        out.y[i] = static_cast<float>(q.verts[i].y);
    }
    int16_t minY = q.verts[0].y;
    int16_t maxY = q.verts[0].y;
    for (int i = 1; i < 4; ++i) {
        minY = (q.verts[i].y < minY) ? q.verts[i].y : minY;
        maxY = (q.verts[i].y > maxY) ? q.verts[i].y : maxY;
    }
    const int32_t shift = q.snapShift & 0x1F;
    // A textured single-scanline quad whose 16.16 corners span a height is kept for the GPU world
    // (ShouldCullTexturedQuadForGpu), so it needs its corners; every other single-scanline quad keeps
    // its snapped vertices.
    if (shift == 0 || (minY == maxY && (q.kind != QuadKind::Textured || !CornersSpanHeight(q)))) {
        return out;
    }
    out.eligible = true;
    const float scale = 1.0f / static_cast<float>(1u << shift);
    float cx[4];
    float cy[4];
    for (int c = 0; c < 4; ++c) {
        cx[c] = static_cast<float>(q.corners16_16[c * 2 + 0]) * scale;
        cy[c] = static_cast<float>(q.corners16_16[c * 2 + 1]) * scale;
    }

    // match[i][c]: snapped corner c equals vertex i exactly (int16 store in FUN_004688e0).
    bool match[4][4];
    for (int i = 0; i < 4; ++i) {
        for (int c = 0; c < 4; ++c) {
            match[i][c] = static_cast<int16_t>(q.snappedPreFix[c * 2 + 0]) == q.verts[i].x &&
                          static_cast<int16_t>(q.snappedPreFix[c * 2 + 1]) == q.verts[i].y;
        }
    }

    int best[4] = {-1, -1, -1, -1};
    float bestCost = 0.0f;
    int perm[4] = {0, 1, 2, 3};
    // Enumerate the 24 permutations in lexicographic order (deterministic tie-break order).
    for (int p0 = 0; p0 < 4; ++p0) {
        for (int p1 = 0; p1 < 4; ++p1) {
            for (int p2 = 0; p2 < 4; ++p2) {
                const int p3 = 6 - p0 - p1 - p2;
                if (p0 == p1 || p0 == p2 || p1 == p2 || p3 < 0 || p3 > 3 || p3 == p0 || p3 == p1 || p3 == p2) {
                    continue;
                }
                perm[0] = p0;
                perm[1] = p1;
                perm[2] = p2;
                perm[3] = p3;
                bool ok = true;
                float cost = 0.0f;
                for (int i = 0; i < 4 && ok; ++i) {
                    ok = match[i][perm[i]];
                    const float dx = cx[perm[i]] - (out.x[i] + 0.5f);
                    const float dy = cy[perm[i]] - (out.y[i] + 0.5f);
                    cost += dx * dx + dy * dy;
                }
                if (ok && (best[0] < 0 || cost < bestCost)) {
                    for (int i = 0; i < 4; ++i) best[i] = perm[i];
                    bestCost = cost;
                }
            }
        }
    }
    if (best[0] < 0) {
        return out;
    }

    const float maxAllowed = kSubPixelMaxDeltaPx + (((q.snapMask & 1u) == 0) ? 1.0f : 0.0f);
    float maxDelta = 0.0f;
    for (int i = 0; i < 4; ++i) {
        maxDelta = std::max(maxDelta, std::fabs(cx[best[i]] - out.x[i]));
        maxDelta = std::max(maxDelta, std::fabs(cy[best[i]] - out.y[i]));
    }
    if (maxDelta > maxAllowed) {
        return out;
    }
    out.useCorners = true;
    out.maxDelta = maxDelta;
    for (int i = 0; i < 4; ++i) {
        out.permutation[i] = best[i];
        out.x[i] = cx[best[i]];
        out.y[i] = cy[best[i]];
    }
    return out;
}

uint8_t HudSentinelActive(const HudSentinelState& state) {
    return state.candidates[state.active & 1u];
}

bool HudSentinelObserve(HudSentinelState& state, const uint32_t hist[256], uint8_t* outRetired,
                        uint8_t* outReplacement) {
    if (!hist) {
        return false;
    }
    const uint8_t active = HudSentinelActive(state);
    const uint8_t inactive = state.candidates[(state.active + 1u) & 1u];
    const uint32_t stamp = state.frame + 1u;
    for (int i = 0; i < 256; ++i) {
        // Pixels equal to the active sentinel are mostly the clear value, so they prove nothing.
        if (i != active && hist[i] > 0) {
            state.lastSeen[i] = stamp;
        }
    }
    if (hist[inactive] == 0) {
        return false;
    }
    // The inactive candidate was not the clear value this frame, so every pixel with that index was
    // written by a HUD writer: retire it and pick the least recently seen index instead. Index 0 is
    // the black colour key used by HUD art and is never a candidate.
    int best = -1;
    for (int i = 1; i < 256; ++i) {
        if (i == active || i == inactive) {
            continue;
        }
        if (best < 0 || state.lastSeen[i] < state.lastSeen[best]) {
            best = i;
        }
    }
    if (best < 0) {
        return false;
    }
    state.candidates[(state.active + 1u) & 1u] = static_cast<uint8_t>(best);
    if (outRetired) {
        *outRetired = inactive;
    }
    if (outReplacement) {
        *outReplacement = static_cast<uint8_t>(best);
    }
    return true;
}

void HudSentinelAdvance(HudSentinelState& state) {
    state.active = (state.active + 1u) & 1u;
    ++state.frame;
}

RaceFrameActions AdvanceRaceFrame(RaceFrameHotkeyState& state,
                                  bool captureEnabled,
                                  bool gpuWorld,
                                  uint32_t toggleKey,
                                  bool keyDown) {
    RaceFrameActions actions;
    actions.endCaptureFrame = captureEnabled || gpuWorld;
    if (toggleKey == 0) {
        state.prevKeyDown = false;
        return actions;
    }
    actions.toggleGpuWorld = keyDown && !state.prevKeyDown;
    state.prevKeyDown = keyDown;
    return actions;
}

bool WriteIndexedPng(const std::wstring& path, const uint8_t* surface, int pitch, const uint8_t* palette, int width, int height) {
    if (!surface || !palette || width <= 0 || height <= 0) {
        return false;
    }

    // Build raw filtered scanlines: each row has filter byte 0x00 + width bytes of 8bpp indices.
    const size_t rowStride = 1 + static_cast<size_t>(width);
    std::vector<uint8_t> rawScanlines(static_cast<size_t>(height) * rowStride);
    for (int y = 0; y < height; ++y) {
        uint8_t* dst = rawScanlines.data() + static_cast<size_t>(y) * rowStride;
        dst[0] = 0x00;  // Filter type 0 (None)
        std::memcpy(dst + 1, surface + static_cast<ptrdiff_t>(y) * pitch, static_cast<size_t>(width));
    }

    // Wrap rawScanlines in a zlib stream with uncompressed deflate stored blocks (BTYPE=00).
    std::vector<uint8_t> idat;
    idat.reserve(rawScanlines.size() + 64);
    idat.push_back(0x78);  // CMF: deflate, 32K window
    idat.push_back(0x01);  // FLG: no dict, check bits so (0x7801 % 31 == 0)

    size_t offset = 0;
    const size_t total = rawScanlines.size();
    while (offset < total) {
        const size_t remaining = total - offset;
        const uint16_t blockLen = static_cast<uint16_t>(remaining > 65535u ? 65535u : remaining);
        const bool isFinal = (offset + blockLen == total);
        idat.push_back(isFinal ? 0x01u : 0x00u);
        idat.push_back(static_cast<uint8_t>(blockLen & 0xFFu));
        idat.push_back(static_cast<uint8_t>((blockLen >> 8) & 0xFFu));
        const uint16_t nlen = static_cast<uint16_t>(~blockLen);
        idat.push_back(static_cast<uint8_t>(nlen & 0xFFu));
        idat.push_back(static_cast<uint8_t>((nlen >> 8) & 0xFFu));
        idat.insert(idat.end(), rawScanlines.data() + offset, rawScanlines.data() + offset + blockLen);
        offset += blockLen;
    }

    // Compute Adler-32 checksum over rawScanlines.
    uint32_t s1 = 1u;
    uint32_t s2 = 0u;
    for (size_t i = 0; i < total;) {
        const size_t chunkEnd = (i + 5552u < total) ? (i + 5552u) : total;
        for (; i < chunkEnd; ++i) {
            s1 += rawScanlines[i];
            s2 += s1;
        }
        s1 %= 65521u;
        s2 %= 65521u;
    }
    const uint32_t adler = (s2 << 16) | s1;
    WriteBe32(idat, adler);

    std::vector<uint8_t> png;
    png.reserve(idat.size() + 1024);
    static const uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    png.insert(png.end(), kPngSig, kPngSig + 8);

    uint8_t ihdr[13] = {
        static_cast<uint8_t>((width >> 24) & 0xFF),
        static_cast<uint8_t>((width >> 16) & 0xFF),
        static_cast<uint8_t>((width >> 8) & 0xFF),
        static_cast<uint8_t>(width & 0xFF),
        static_cast<uint8_t>((height >> 24) & 0xFF),
        static_cast<uint8_t>((height >> 16) & 0xFF),
        static_cast<uint8_t>((height >> 8) & 0xFF),
        static_cast<uint8_t>(height & 0xFF),
        8,  // bit depth: 8 bits per sample/palette index
        3,  // color type: 3 (indexed-color)
        0,  // compression method: 0
        0,  // filter method: 0
        0   // interlace method: 0
    };
    WritePngChunk(png, "IHDR", ihdr, sizeof(ihdr));

    uint8_t plte[768];
    for (int i = 0; i < 256; ++i) {
        plte[i * 3 + 0] = palette[i * 4 + 0];
        plte[i * 3 + 1] = palette[i * 4 + 1];
        plte[i * 3 + 2] = palette[i * 4 + 2];
    }
    WritePngChunk(png, "PLTE", plte, sizeof(plte));
    WritePngChunk(png, "IDAT", idat.data(), idat.size());
    WritePngChunk(png, "IEND", nullptr, 0);

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"wb") != 0 || !fp) {
        return false;
    }
    const size_t written = std::fwrite(png.data(), 1, png.size(), fp);
    std::fclose(fp);
    return written == png.size();
}

bool WriteFrameJsonFromQuads(const std::wstring& path,
                             uint32_t frameNum,
                             int width,
                             int height,
                             int32_t snapShift,
                             uint32_t snapMask,
                             const CapturedQuad* quads,
                             uint32_t quadCount,
                             uint32_t overflowCount) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"wb") != 0 || !fp) {
        return false;
    }

    std::fprintf(fp, "{\n");
    std::fprintf(fp, "  \"frame_number\": %u,\n", frameNum);
    std::fprintf(fp, "  \"width\": %d,\n", width);
    std::fprintf(fp, "  \"height\": %d,\n", height);
    std::fprintf(fp, "  \"snap_shift\": %d,\n", snapShift);
    std::fprintf(fp, "  \"snap_mask\": \"0x%08X\",\n", snapMask);
    std::fprintf(fp, "  \"quad_count\": %u,\n", quadCount);
    std::fprintf(fp, "  \"overflow_count\": %u,\n", overflowCount);
    std::fprintf(fp, "  \"non_rasterizer_writers\": [\n");
    std::fprintf(fp, "    {\"va\": \"0x00469770\", \"role\": \"horizon_sky_scroll_blit\", \"discovery\": \"FUN_0045ac50 callee; BltFast from sky surfaces DAT_005cd4c4/DAT_005cd4c8 into DAT_005cd4bc\"},\n");
    std::fprintf(fp, "    {\"va\": \"0x00468780\", \"role\": \"windowed_border_bitmap_copy\", \"discovery\": \"FUN_00469770 callee; locks DAT_005cd4bc via FUN_00436420 and copies border bitmap\"},\n");
    std::fprintf(fp, "    {\"va\": \"0x00467cb0\", \"role\": \"flat_polygon_scanline_fill\", \"discovery\": \"FUN_004688e0 flat branch & FUN_00468ce0; writes road/terrain/stripe spans to DAT_006036bc (captured as kind=flat)\"},\n");
    std::fprintf(fp, "    {\"va\": \"0x00469460\", \"role\": \"dashboard_bar_blit\", \"discovery\": \"FUN_0045d6d0 callee; color-keyed BltFast of dashboard bar DAT_005cd4d0 into DAT_005cd4bc\"},\n");
    std::fprintf(fp, "    {\"va\": \"0x00469530\", \"role\": \"hud_health_stamina_bars_blit\", \"discovery\": \"FUN_0045d6d0 callee; BltFast of meter bars from DAT_005cd4c0 into DAT_005cd4bc\"},\n");
    std::fprintf(fp, "    {\"va\": \"0x00469990\", \"role\": \"hud_status_icons_blit\", \"discovery\": \"FUN_0045d6d0 callee; BltFast of HUD icons from DAT_005cd4c0 into DAT_005cd4bc\"},\n");
    std::fprintf(fp, "    {\"va\": \"0x0045d6d0\", \"role\": \"hud_gauges_and_gdi_text\", \"discovery\": \"FUN_00467b80 callee; IDirectDrawSurface::GetDC + MoveToEx/LineTo needles, FillRect boxes, and FUN_0045d630 DrawTextA\"},\n");
    std::fprintf(fp, "    {\"va\": \"0x004581f0\", \"role\": \"surface_colorfill_clear\", \"discovery\": \"xrefs to DAT_005cd4bc; IDirectDrawSurface::Blt DDBLT_COLORFILL screen clear\"}\n");
    std::fprintf(fp, "  ],\n");
    std::fprintf(fp, "  \"quads\": [\n");

    for (uint32_t i = 0; i < quadCount; ++i) {
        const CapturedQuad& q = quads[i];
        std::string texId;
        if (q.kind == QuadKind::Flat) {
            char flatBuf[32];
            std::snprintf(flatBuf, sizeof(flatBuf), "flat_%u", static_cast<unsigned int>(q.flatColor));
            texId = flatBuf;
        } else {
            texId = FormatTextureId(q.texWidth, q.texHeight, q.level0Crc32);
        }

        std::fprintf(
            fp,
            "    {\"draw_order\": %u, \"batch\": %u, \"kind\": \"%s\", \"culled\": %s, "
            "\"pdat\": \"0x%08X\", \"pdat_magic\": \"0x%08X\", \"texture_id\": \"%s\", "
            "\"width\": %d, \"height\": %d, \"mip_levels\": %d, \"chosen_mip\": %d, "
            "\"pdat_flags\": \"0x%08X\", \"ccb_flags\": \"0x%08X\", "
            "\"flat_color\": %u, \"sprite_clut\": %u, \"sprite_index\": %d, "
            "\"u_flip\": %u, \"v_flip\": %u, "
            "\"clip_rect\": [%d, %d, %d, %d], "
            "\"uv_box\": [%d, %d, %d, %d], "
            "\"corners_16_16\": [%d, %d, %d, %d, %d, %d, %d, %d], "
            "\"snapped_pre_fix\": [%d, %d, %d, %d, %d, %d, %d, %d], "
            "\"vertices\": ["
            "{\"x\": %d, \"y\": %d, \"u\": %d, \"v\": %d}, "
            "{\"x\": %d, \"y\": %d, \"u\": %d, \"v\": %d}, "
            "{\"x\": %d, \"y\": %d, \"u\": %d, \"v\": %d}, "
            "{\"x\": %d, \"y\": %d, \"u\": %d, \"v\": %d}"
            "]}%s\n",
            q.drawOrder,
            q.batchIndex,
            q.kind == QuadKind::Textured ? "textured" : "flat",
            q.culled ? "true" : "false",
            q.pdatPtr,
            q.pdatMagic,
            texId.c_str(),
            q.texWidth,
            q.texHeight,
            q.mipLevels,
            q.chosenMip,
            q.pdatFlags,
            q.ccbFlags,
            static_cast<unsigned int>(q.flatColor),
            static_cast<unsigned int>(q.spriteClut),
            q.spriteIndex,
            static_cast<unsigned int>(q.uFlip),
            static_cast<unsigned int>(q.vFlip),
            q.clipRect[0], q.clipRect[1], q.clipRect[2], q.clipRect[3],
            q.uvBox[0], q.uvBox[1], q.uvBox[2], q.uvBox[3],
            q.corners16_16[0], q.corners16_16[1], q.corners16_16[2], q.corners16_16[3],
            q.corners16_16[4], q.corners16_16[5], q.corners16_16[6], q.corners16_16[7],
            q.snappedPreFix[0], q.snappedPreFix[1], q.snappedPreFix[2], q.snappedPreFix[3],
            q.snappedPreFix[4], q.snappedPreFix[5], q.snappedPreFix[6], q.snappedPreFix[7],
            static_cast<int>(q.verts[0].x), static_cast<int>(q.verts[0].y),
            static_cast<int>(q.verts[0].u), static_cast<int>(q.verts[0].v),
            static_cast<int>(q.verts[1].x), static_cast<int>(q.verts[1].y),
            static_cast<int>(q.verts[1].u), static_cast<int>(q.verts[1].v),
            static_cast<int>(q.verts[2].x), static_cast<int>(q.verts[2].y),
            static_cast<int>(q.verts[2].u), static_cast<int>(q.verts[2].v),
            static_cast<int>(q.verts[3].x), static_cast<int>(q.verts[3].y),
            static_cast<int>(q.verts[3].u), static_cast<int>(q.verts[3].v),
            (i + 1 < quadCount) ? "," : "");
    }

    std::fprintf(fp, "  ]\n");
    std::fprintf(fp, "}\n");
    std::fclose(fp);
    return true;
}

void CaptureInit(HMODULE hostModule, const std::wstring& dumpDir) {
    g_hostModule = hostModule;
    g_dumpDir = dumpDir;
    g_capturedCount = 0;
    g_overflowCount = 0;
    g_batchIndex = 0;
    g_raceFrameNumber = 0;
    g_f11WasDown = false;
    g_dumpThisFrame = false;
    g_dumpTriggeredCheckedThisFrame = false;
    g_horizonCapturedThisFrame = false;
    g_horizonHeight = 360;
    std::memset(g_horizonBuffer, 0, sizeof(g_horizonBuffer));
    g_wideHorizonComposedThisFrame = false;
    g_wideHorizonWidth = 0;
    g_levelSnapshot = LevelStateSnapshot{};
    g_hudSentinel = HudSentinelState{};
    CaptureInvalidateCache();
    EnsureCrcTable();
}

void CaptureInvalidateHorizonPanorama() {
    HorizonPanoramaCache& c = g_horizonPanorama;
    c.valid = false;
    c.retryAfterFrame = 0;
    c.mismatchFrames = 0;
}

void CaptureInvalidateCache() {
    CaptureInvalidateHorizonPanorama();
    std::memset(g_crcCache, 0, sizeof(g_crcCache));
    g_texelClearPending = false;
    g_texPixelCache.clear();
    g_texPixelCacheBytes = 0;
    ++g_cacheGeneration;
}

void CaptureSetTexturePixelCacheCap(size_t capBytes) {
    g_texPixelCacheCapBytes = (capBytes > 0) ? capBytes : kDefaultTexturePixelCacheCapBytes;
}

size_t CaptureGetTexturePixelCacheBytes() {
    return g_texPixelCacheBytes;
}

uint32_t CaptureGetTexturePixelCacheEvictions() {
    return g_texPixelCacheEvictions;
}

int32_t ClampMipLevelCount(int32_t width, int32_t height, int32_t levelCount) {
    int32_t levels = (levelCount < 1) ? 1 : levelCount;
    levels = (levels > kMaxTextureMipLevels) ? kMaxTextureMipLevels : levels;
    int32_t valid = 1;
    while (valid < levels && (width >> valid) >= 1 && (height >> valid) >= 1) {
        ++valid;
    }
    return valid;
}

void CaptureCacheTexturePixels(uint32_t pdatPtr,
                               uint32_t level0Crc32,
                               const uint8_t* level0Ptr,
                               int32_t width,
                               int32_t height,
                               int32_t rowPitch,
                               const uint8_t* const* extraLevels,
                               int32_t extraLevelCount) {
    if (!level0Ptr || width <= 0 || height <= 0 || width > 1024 || height > 1024 || rowPitch < width) {
        return;
    }
    // Level i (i >= 1) is the engine's own (width >> i) x (height >> i) block, rows packed.
    int32_t levels = 1;
    if (extraLevels && extraLevelCount > 0) {
        levels = ClampMipLevelCount(width, height, extraLevelCount + 1);
        for (int32_t i = 1; i < levels; ++i) {
            if (!extraLevels[i - 1]) {
                levels = i;
                break;
            }
        }
    }
    const uint64_t key = (static_cast<uint64_t>(pdatPtr) << 32) | level0Crc32;
    auto it = g_texPixelCache.find(key);
    if (it != g_texPixelCache.end()) {
        if (it->second.width == width && it->second.height == height) {
            return;
        }
        g_texPixelCacheBytes -= it->second.pixels.size();
        g_texPixelCache.erase(it);
    }
    size_t entryBytes = 0;
    size_t levelOffset[kMaxTextureMipLevels] = {};
    for (int32_t i = 0; i < levels; ++i) {
        levelOffset[i] = entryBytes;
        entryBytes += static_cast<size_t>(width >> i) * static_cast<size_t>(height >> i);
    }
    if (g_texPixelCacheBytes + entryBytes > g_texPixelCacheCapBytes) {
        ++g_texPixelCacheEvictions;
        LOG_INFO("Texture pixel cache reached %zu of %zu bytes (%zu textures); clearing it (eviction %u)",
                 g_texPixelCacheBytes, g_texPixelCacheCapBytes, g_texPixelCache.size(), g_texPixelCacheEvictions);
        g_texPixelCache.clear();
        g_texPixelCacheBytes = 0;
        ++g_cacheGeneration;
        if (entryBytes > g_texPixelCacheCapBytes) {
            return;
        }
    }
    CachedTexturePixels entry;
    entry.width = width;
    entry.height = height;
    entry.levels = levels;
    for (int32_t i = 0; i < levels; ++i) {
        entry.levelOffset[i] = levelOffset[i];
    }
    entry.pixels.resize(entryBytes);
    const size_t level0Bytes = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (rowPitch == width) {
        std::memcpy(entry.pixels.data(), level0Ptr, level0Bytes);
    } else {
        for (int32_t y = 0; y < height; ++y) {
            std::memcpy(entry.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(width),
                        level0Ptr + static_cast<size_t>(y) * static_cast<size_t>(rowPitch),
                        static_cast<size_t>(width));
        }
    }
    for (int32_t i = 1; i < levels; ++i) {
        std::memcpy(entry.pixels.data() + levelOffset[i], extraLevels[i - 1],
                    static_cast<size_t>(width >> i) * static_cast<size_t>(height >> i));
    }
    g_texPixelCacheBytes += entry.pixels.size();
    g_texPixelCache[key] = std::move(entry);
}

uint8_t CaptureGetHudSentinel() {
    return HudSentinelActive(g_hudSentinel);
}

void CaptureObserveHudLayer(const uint32_t hist[256]) {
    uint8_t retired = 0;
    uint8_t replacement = 0;
    if (HudSentinelObserve(g_hudSentinel, hist, &retired, &replacement)) {
        LOG_INFO("HUD sentinel index 0x%02X was drawn by a HUD writer; replaced by 0x%02X",
                 static_cast<unsigned int>(retired), static_cast<unsigned int>(replacement));
    }
}

bool DecideSkipSoftwareRasterizer(bool forbidden, bool gpuWorld, bool presenterEnabled, bool presenterAllowsGpuWorld,
                                  bool dumpThisFrame) {
    if (forbidden) {
        return true;
    }
    return gpuWorld && presenterEnabled && presenterAllowsGpuWorld && !dumpThisFrame;
}

bool CaptureSoftwareWorldWritesForbidden() {
    // DAT_006036c8 is the half-open right edge of the clip rect FUN_0042e850 and FUN_00467cb0 clip
    // their spans to; race init sets it to projection width << HiRes.
    const int32_t clipRight = g_hostModule ? GlobalPtr<const int32_t>(kVaClipRect)[2] : 0;
    return SoftwareWorldWritesForbidden(WidescreenGetAppliedWidth(), clipRight);
}

bool CaptureShouldSkipSoftwareRasterizer() {
    const Config& cfg = GetConfig();
    // PresenterGpuWorldAllowed() is false while latched off, after any failed present that has not
    // been followed by a successful one, and during the retry backoff: a GDI fallback frame must
    // always contain the software world. That rule does not apply while software world writes are
    // forbidden (widescreen): the rasterizers would write past the 640-wide surface, so they are
    // skipped even on fallback and dump frames.
    return DecideSkipSoftwareRasterizer(CaptureSoftwareWorldWritesForbidden(), cfg.gpuWorld, cfg.presenterEnabled,
                                        PresenterGpuWorldAllowed(), g_dumpThisFrame);
}

bool CaptureHasWorldFrame() {
    return g_horizonCapturedThisFrame && g_batchIndex > 0;
}

const CapturedQuad* CaptureGetWorldQuads(uint32_t& outCount) {
    outCount = g_capturedCount;
    return g_capturedQuads;
}

const uint8_t* CaptureGetHorizonLayer(int32_t& outHorizonHeight) {
    outHorizonHeight = g_horizonHeight;
    return g_horizonCapturedThisFrame ? g_horizonBuffer : nullptr;
}

const uint8_t* CaptureGetWideHorizonLayer(int32_t& outWidth) {
    const bool available = g_horizonCapturedThisFrame && g_wideHorizonComposedThisFrame;
    outWidth = available ? g_wideHorizonWidth : 0;
    return available ? g_wideHorizonBuffer : nullptr;
}

const uint8_t* CaptureLookupTexturePixels(uint32_t pdatPtr,
                                          uint32_t level0Crc32,
                                          int32_t& outWidth,
                                          int32_t& outHeight) {
    const uint64_t key = (static_cast<uint64_t>(pdatPtr) << 32) | level0Crc32;
    auto it = g_texPixelCache.find(key);
    if (it == g_texPixelCache.end() || it->second.pixels.empty()) {
        outWidth = 0;
        outHeight = 0;
        return nullptr;
    }
    outWidth = it->second.width;
    outHeight = it->second.height;
    return it->second.pixels.data();
}

bool CaptureLookupTextureMipChain(uint32_t pdatPtr, uint32_t level0Crc32, TextureMipChainView& out) {
    out = TextureMipChainView{};
    const uint64_t key = (static_cast<uint64_t>(pdatPtr) << 32) | level0Crc32;
    auto it = g_texPixelCache.find(key);
    if (it == g_texPixelCache.end() || it->second.pixels.empty()) {
        return false;
    }
    const CachedTexturePixels& e = it->second;
    out.levels = e.levels;
    for (int32_t i = 0; i < e.levels; ++i) {
        out.width[i] = e.width >> i;
        out.height[i] = e.height >> i;
        out.pixels[i] = e.pixels.data() + e.levelOffset[i];
    }
    return true;
}

uint32_t CaptureGetCacheGeneration() {
    return g_cacheGeneration;
}

uint32_t CaptureGetFrameNumber() {
    return g_raceFrameNumber;
}

void CaptureOnBatchStart() {
    CheckAndInvalidateCacheOnLevelChange();
    if (g_batchIndex == 0 && !g_dumpTriggeredCheckedThisFrame) {
        g_dumpTriggeredCheckedThisFrame = true;
        if (GetConfig().captureEnabled) {
            const bool f11Down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
            const bool f11Pressed = f11Down && !g_f11WasDown;
            g_f11WasDown = f11Down;
            const uint32_t nextFrameNum = g_raceFrameNumber + 1u;
            const int autoDumpFrame = GetConfig().captureAutoDumpFrame;
            const bool autoTriggered = (autoDumpFrame > 0 && static_cast<int>(nextFrameNum) == autoDumpFrame);
            g_dumpThisFrame = f11Pressed || autoTriggered;
        } else {
            g_dumpThisFrame = false;
        }
    }
    ++g_batchIndex;
}

void CaptureOnBatchEnd() {
    SnapshotHorizonAndClearSurfaceIfNeeded();
}

int CaptureTexturedQuadBefore() {
    if (!g_hostModule) {
        return -1;
    }
    SnapshotHorizonAndClearSurfaceIfNeeded();
    if (g_capturedCount >= kMaxCapturedQuads) {
        ++g_overflowCount;
        return -1;
    }

    const uint8_t* poly = *GlobalPtr<const uint8_t*>(kVaCurrentPoly);
    if (!poly) {
        return -1;
    }
    const uint8_t* ccb = poly - 0x68;
    const uint8_t* pdat = *reinterpret_cast<const uint8_t* const*>(poly + 0x04);

    const uint32_t slot = g_capturedCount++;
    CapturedQuad& q = g_capturedQuads[slot];
    std::memset(&q, 0, sizeof(q));

    q.drawOrder = slot;
    q.batchIndex = g_batchIndex;
    q.kind = QuadKind::Textured;
    q.depthZ = DepthOfPoly(poly);
    q.culled = false;
    q.ccbFlags = *reinterpret_cast<const uint32_t*>(ccb + 0x00);
    q.uFlip = *(poly + 0x3d);
    q.vFlip = *(poly + 0x3e);
    q.spriteClut = *(poly + 0x3c);
    q.spriteIndex = *reinterpret_cast<const int32_t*>(poly + 0x38);

    const int32_t shift = *GlobalPtr<int32_t>(kVaSnapShift);
    const uint32_t mask = *GlobalPtr<uint32_t>(kVaSnapMask);
    const int32_t* corners16 = reinterpret_cast<const int32_t*>(ccb + 0x44);
    q.snapShift = shift;
    q.snapMask = mask;
    for (int i = 0; i < 8; ++i) {
        q.corners16_16[i] = corners16[i];
        q.snappedPreFix[i] = static_cast<int32_t>(
            static_cast<uint32_t>(corners16[i] >> (static_cast<uint8_t>(shift) & 0x1Fu)) & mask);
    }

    const int32_t* uvBox = reinterpret_cast<const int32_t*>(poly + 0x08);
    for (int i = 0; i < 4; ++i) {
        q.uvBox[i] = uvBox[i];
    }

    const int32_t* clipRect = GlobalPtr<const int32_t>(kVaClipRect);
    for (int i = 0; i < 4; ++i) {
        q.clipRect[i] = clipRect[i];
    }

    const int16_t* rawVerts = GlobalPtr<const int16_t>(kVaVertsInt16);
    for (int i = 0; i < 4; ++i) {
        q.verts[i].x = rawVerts[i * 4 + 0];
        q.verts[i].y = rawVerts[i * 4 + 1];
        q.verts[i].u = rawVerts[i * 4 + 2];
        q.verts[i].v = rawVerts[i * 4 + 3];
    }

    if (pdat) {
        q.pdatPtr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pdat));
        q.pdatMagic = *reinterpret_cast<const uint32_t*>(pdat + 0x00);
        q.pdatFlags = *reinterpret_cast<const uint32_t*>(pdat + 0x0c);
        const int32_t pdatPitch = *reinterpret_cast<const int32_t*>(pdat + 0x20);
        const int32_t pdatHeight = *reinterpret_cast<const int32_t*>(pdat + 0x24);
        q.mipLevels = *reinterpret_cast<const int32_t*>(pdat + 0x28);
        const uint8_t* level0Ptr = *reinterpret_cast<const uint8_t* const*>(pdat + 0x38);

        if ((q.pdatFlags & 0x2000u) != 0) {
            // Dynamic sprite unpacked by FUN_00458f30 into top-left spriteW x spriteH of DAT_005aaa04 (stride pdatPitch=256).
            int32_t spriteW = 0;
            int32_t spriteH = 0;
            ClampSpriteDimensions(q.uvBox, pdatPitch, pdatHeight, spriteW, spriteH);
            q.texWidth = spriteW;
            q.texHeight = spriteH;

            uint8_t clutDescriptor[257];
            clutDescriptor[0] = q.spriteClut;
            size_t clutLen = 1;
            if (q.spriteClut < 0xdeu) {
                const uint8_t* clutRow = GlobalPtr<const uint8_t>(kVaSpriteClutTable) +
                                         static_cast<size_t>(q.spriteClut) * 256u;
                std::memcpy(clutDescriptor + 1, clutRow, 256u);
                clutLen = 257u;
            }
            const uint32_t keyB = (static_cast<uint32_t>(q.spriteClut) << 24) |
                                  (static_cast<uint32_t>(q.spriteIndex) & 0x00FFFFFFu);
            q.level0Crc32 = LookupOrComputeLevel0Crc32(
                q.pdatPtr, keyB, level0Ptr, spriteW, spriteH, pdatPitch, clutDescriptor, clutLen);
            if (GetConfig().gpuWorld) {
                const uint64_t cacheKey = (static_cast<uint64_t>(q.pdatPtr) << 32) | q.level0Crc32;
                if (g_texPixelCache.find(cacheKey) == g_texPixelCache.end()) {
                    CaptureCacheTexturePixels(q.pdatPtr, q.level0Crc32, level0Ptr, spriteW, spriteH, pdatPitch);
                }
            }
        } else {
            q.texWidth = pdatPitch;
            q.texHeight = pdatHeight;
            const uint32_t keyB = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(level0Ptr));
            q.level0Crc32 = LookupOrComputeLevel0Crc32(
                q.pdatPtr, keyB, level0Ptr, pdatPitch, pdatHeight, pdatPitch, nullptr, 0);
            if (GetConfig().gpuWorld) {
                const uint64_t cacheKey = (static_cast<uint64_t>(q.pdatPtr) << 32) | q.level0Crc32;
                if (g_texPixelCache.find(cacheKey) == g_texPixelCache.end()) {
                    // Engine mip chain (T6): PDAT +0x28 holds the level count and +0x38 + 12 * i the
                    // level-i texel pointer FUN_0042e850 reads; level i is (w >> i) x (h >> i).
                    const uint8_t* extraLevels[kMaxTextureMipLevels - 1] = {};
                    int32_t extraCount = 0;
                    if (GetConfig().mipFilter == MipFilter::Trilinear) {
                        const int32_t levels = ClampMipLevelCount(pdatPitch, pdatHeight, q.mipLevels);
                        for (int32_t level = 1; level < levels; ++level) {
                            const uint8_t* lvlPtr =
                                *reinterpret_cast<const uint8_t* const*>(pdat + 0x38 + level * 0x0c);
                            if (!ValidateMipLevelPointer(lvlPtr, pdatPitch, pdatHeight, level)) {
                                static std::unordered_set<uint32_t>& s_warnedPdats = *new std::unordered_set<uint32_t>();
                                if (s_warnedPdats.insert(q.pdatPtr).second) {
                                    LOG_WARN("PDAT 0x%08X level %d pointer %p is invalid; truncating mip chain to %d levels",
                                             q.pdatPtr, level, lvlPtr, level);
                                }
                                break;
                            }
                            extraLevels[level - 1] = lvlPtr;
                            ++extraCount;
                        }
                    }
                    CaptureCacheTexturePixels(q.pdatPtr, q.level0Crc32, level0Ptr, pdatPitch, pdatHeight, pdatPitch,
                                              extraLevels, extraCount);
                }
            }
        }
    }

    // Observation-only: never write to engine globals.
    return static_cast<int>(slot);
}

void CaptureTexturedQuadAfter(int slotIndex, bool rasterizerSkipped) {
    if (slotIndex < 0 || static_cast<uint32_t>(slotIndex) >= g_capturedCount) {
        return;
    }
    CapturedQuad& q = g_capturedQuads[slotIndex];

    int16_t minY = q.verts[0].y;
    int16_t maxY = q.verts[0].y;
    for (int i = 1; i < 4; ++i) {
        if (q.verts[i].y < minY) minY = q.verts[i].y;
        if (q.verts[i].y > maxY) maxY = q.verts[i].y;
    }

    if (rasterizerSkipped) {
        // The verdict is the ShouldCullTexturedQuadForGpu decision; it is also counted for the log (D2b).
        const Config& cfg = GetConfig();
        const ThinQuadVerdict verdict = ClassifyThinTexturedQuadForGpu(q, cfg.subPixel, cfg.keepThinQuads);
        RecordThinQuadVerdict(g_thinQuadFrame, verdict);
        g_thinQuadFrameActive = true;
        if (q.pdatPtr != 0) {
            // The engine selected no mip: every GPU-drawn quad, thin quads kept by
            // ShouldCullTexturedQuadForGpu included, uses level 0 and the sampler minifies from there.
            q.culled = ShouldCullTexturedQuadForVerdict(q, verdict);
            q.chosenMip = (q.culled && minY == maxY) ? -1 : 0;
            return;
        }
    }

    if (q.pdatPtr == 0) {
        q.chosenMip = -1;
        q.culled = true;
        return;
    }

    // In FUN_0042e850, local_c == local_18 (minY == maxY) is the only early return before DAT_006185a4 is written.
    if (minY == maxY) {
        q.chosenMip = -1;
        q.culled = true;
        return;
    }

    const uint8_t* activeMip = *GlobalPtr<const uint8_t*>(kVaActiveMipPtr);
    if (!activeMip) {
        q.chosenMip = -1;
        q.culled = true;
        return;
    }

    const auto* pdat = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(q.pdatPtr));
    q.chosenMip = 0;
    const int32_t maxMips = (q.mipLevels > 0 && q.mipLevels <= 12) ? q.mipLevels : 1;
    for (int32_t level = 0; level < maxMips; ++level) {
        const uint8_t* levelPtr = *reinterpret_cast<const uint8_t* const*>(pdat + 0x38 + level * 0x0c);
        if (levelPtr == activeMip) {
            q.chosenMip = level;
            break;
        }
    }

    q.culled = ShouldCullTexturedQuad(q.verts, q.clipRect, q.pdatPtr);
}

int CaptureFlatQuadBefore(const int32_t* polyVerts, int edxArg) {
    if (!g_hostModule || !polyVerts) {
        return -1;
    }
    SnapshotHorizonAndClearSurfaceIfNeeded();
    if (g_capturedCount >= kMaxCapturedQuads) {
        ++g_overflowCount;
        return -1;
    }

    // In FUN_004688e0 (0x00468a7e..0x00468aa3), ECX (polyVerts) is poly + 0x18, and DL (edxArg & 0xFF) is flatColor.
    const uint8_t* poly = reinterpret_cast<const uint8_t*>(polyVerts) - 0x18;
    const uint8_t* ccb = poly - 0x68;
    const uint8_t* pdat = *reinterpret_cast<const uint8_t* const*>(poly + 0x04);

    const uint32_t slot = g_capturedCount++;
    CapturedQuad& q = g_capturedQuads[slot];
    std::memset(&q, 0, sizeof(q));

    q.drawOrder = slot;
    q.batchIndex = g_batchIndex;
    q.kind = QuadKind::Flat;
    q.depthZ = DepthOfPoly(poly);
    q.culled = false;
    q.chosenMip = -1;
    q.ccbFlags = *reinterpret_cast<const uint32_t*>(ccb + 0x00);
    q.uFlip = *(poly + 0x3d);
    q.vFlip = *(poly + 0x3e);
    q.spriteIndex = -1;
    q.flatColor = static_cast<uint8_t>(edxArg & 0xFF);

    const int32_t shift = *GlobalPtr<int32_t>(kVaSnapShift);
    const uint32_t mask = *GlobalPtr<uint32_t>(kVaSnapMask);
    const int32_t* corners16 = reinterpret_cast<const int32_t*>(ccb + 0x44);
    q.snapShift = shift;
    q.snapMask = mask;
    for (int i = 0; i < 8; ++i) {
        q.corners16_16[i] = corners16[i];
        q.snappedPreFix[i] = static_cast<int32_t>(
            static_cast<uint32_t>(corners16[i] >> (static_cast<uint8_t>(shift) & 0x1Fu)) & mask);
    }

    for (int i = 0; i < 4; ++i) {
        q.verts[i].x = static_cast<int16_t>(polyVerts[i * 2 + 0]);
        q.verts[i].y = static_cast<int16_t>(polyVerts[i * 2 + 1]);
        q.verts[i].u = 0;
        q.verts[i].v = 0;
    }

    const int32_t* clipRect = GlobalPtr<const int32_t>(kVaClipRect);
    for (int i = 0; i < 4; ++i) {
        q.clipRect[i] = clipRect[i];
    }

    if (pdat) {
        q.pdatPtr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pdat));
        q.pdatMagic = *reinterpret_cast<const uint32_t*>(pdat + 0x00);
        q.pdatFlags = *reinterpret_cast<const uint32_t*>(pdat + 0x0c);
    }

    return static_cast<int>(slot);
}

void CaptureFlatQuadAfter(int slotIndex, int32_t* polyVerts, bool rasterizerSkipped) {
    if (slotIndex < 0 || static_cast<uint32_t>(slotIndex) >= g_capturedCount || !polyVerts) {
        return;
    }

    CapturedQuad& q = g_capturedQuads[slotIndex];
    if (rasterizerSkipped && g_hostModule) {
        // The original FUN_00467cb0 did not run, so emulate exactly the state changes its prologue
        // makes before the span loop, and nothing else (verified in the exe bytes):
        // - 0x467cb9..0x467cd5: clamp DAT_0061854c (max y) to the clip bottom DAT_006036cc;
        // - 0x467ce9..0x467cf3: single-scanline polygons (DAT_00618544 == DAT_0061854c) branch to
        //   FUN_00468ce0 and return without touching the vertices;
        // - 0x467d2f..0x467d34: push edi / xor edx,edx / mov ecx,esi / call FUN_00468d60, which
        //   reorders (and for concave inputs repairs) the four vertices at poly+0x18 in place.
        //   With EDX=0 it never takes the branch that toggles the u-flip byte at poly+0x3d.
        // The return value of FUN_00468d60 is ignored by FUN_00467cb0; here 0 marks the quad culled.
        int32_t* pYMin = GlobalPtr<int32_t>(0x00618544);
        int32_t* pYMax = GlobalPtr<int32_t>(0x0061854c);
        const int32_t clipYMax = *GlobalPtr<const int32_t>(0x006036cc);
        if (*pYMax > clipYMax) {
            *pYMax = clipYMax;
        }
        if (*pYMin != *pYMax) {
            using FUN_00468d60_Fn = uint8_t(__fastcall*)(int32_t*, uint32_t);
            auto fnOrderVerts = reinterpret_cast<FUN_00468d60_Fn>(Rebase(0x00468d60));
            if (fnOrderVerts(polyVerts, 0) == 0) {
                q.culled = true;
                return;
            }
        }
    }

    for (int i = 0; i < 4; ++i) {
        q.verts[i].x = static_cast<int16_t>(polyVerts[i * 2 + 0]);
        q.verts[i].y = static_cast<int16_t>(polyVerts[i * 2 + 1]);
        q.verts[i].u = 0;
        q.verts[i].v = 0;
    }

    q.culled = ShouldCullFlatQuad(q.verts, q.clipRect);
}

void CaptureOnRacePresent(const uint8_t* surface, int pitch, const uint8_t* palette, int width, int height) {
    ++g_raceFrameNumber;

    if (!g_dumpTriggeredCheckedThisFrame && GetConfig().captureEnabled) {
        const bool f11Down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        const bool f11Pressed = f11Down && !g_f11WasDown;
        g_f11WasDown = f11Down;
        const int autoDumpFrame = GetConfig().captureAutoDumpFrame;
        const bool autoTriggered = (autoDumpFrame > 0 && static_cast<int>(g_raceFrameNumber) == autoDumpFrame);
        g_dumpThisFrame = f11Pressed || autoTriggered;
    }

    if (g_dumpThisFrame) {
        wchar_t jsonName[64] = {0};
        wchar_t pngName[64] = {0};
        std::swprintf(jsonName, 64, L"frame_%u.json", g_raceFrameNumber);
        std::swprintf(pngName, 64, L"frame_%u.png", g_raceFrameNumber);

        const std::wstring jsonPath = g_dumpDir.empty() ? jsonName : (g_dumpDir + L"\\" + jsonName);
        const std::wstring pngPath = g_dumpDir.empty() ? pngName : (g_dumpDir + L"\\" + pngName);

        const bool okJson = WriteFrameJson(jsonPath, g_raceFrameNumber, width, height);
        const bool okPng = WriteIndexedPng(pngPath, surface, pitch, palette, width, height);
        if (okJson && okPng) {
            LOG_INFO("Captured frame %u dumped to %ls and %ls (%u quads, %u overflow)",
                     g_raceFrameNumber, jsonPath.c_str(), pngPath.c_str(), g_capturedCount, g_overflowCount);
        } else {
            LOG_ERROR("Failed to dump captured frame %u (json=%d, png=%d)",
                      g_raceFrameNumber, okJson ? 1 : 0, okPng ? 1 : 0);
        }
    }
}

// Write cursor of the polygon buffer as an entry index (0..250), or -1 when unreadable.
static int32_t PolyBufferCursorIndex() {
    if (!g_distanceFadeHooks || !g_hostModule) {
        return -1;
    }
    const uintptr_t base = *GlobalPtr<const uintptr_t>(kVaPolyBufferBase);
    const uintptr_t cursor = *GlobalPtr<const uintptr_t>(kVaPolyBufferCursor);
    return PolyBufferIndex(cursor, base, true);
}

// Slice depth of the polygon-buffer entry a captured quad came from (poly = entry + 0x68).
static int32_t DepthOfPoly(const uint8_t* poly) {
    if (!g_distanceFadeHooks || !g_hostModule || !poly) {
        return kNoDepth;
    }
    const uintptr_t base = *GlobalPtr<const uintptr_t>(kVaPolyBufferBase);
    return DepthTagLookup(g_depthTags, PolyBufferIndex(reinterpret_cast<uintptr_t>(poly) - 0x68, base, false));
}

void CaptureSetDistanceFadeHooks(bool installed) {
    g_distanceFadeHooks = installed;
    DistanceFadeReset(g_depthTags, g_distanceFadeFarZ);
}

void CaptureDistanceFadeRaceStart() {
    DistanceFadeReset(g_depthTags, g_distanceFadeFarZ);
}

void CaptureDepthEndFrame() {
    if (!g_distanceFadeHooks) {
        return;
    }
    g_distanceFadeFarZ = UpdateDistanceFadeFarZ(g_distanceFadeFarZ, g_depthTags.frameMaxZ);
    DepthTagEndFrame(g_depthTags);
}

void CaptureDepthBeginSlice(const uint8_t* slice) {
    if (!g_distanceFadeHooks || !g_hostModule) {
        return;
    }
    const int32_t count = *GlobalPtr<const int32_t>(kVaRoadSliceCountVar);
    const int32_t z = ReadRoadSliceDepth(slice, GlobalPtr<const uint8_t>(kVaRoadSliceArray), count);
    DepthTagBeginSlice(g_depthTags, PolyBufferCursorIndex(), z);
}

void CaptureDepthEndSlice() {
    if (!g_distanceFadeHooks) {
        return;
    }
    DepthTagEndSlice(g_depthTags, PolyBufferCursorIndex());
}

void CaptureDepthOnFlushStart() {
    if (!g_distanceFadeHooks) {
        return;
    }
    DepthTagFill(g_depthTags, PolyBufferCursorIndex());
}

void CaptureDepthOnFlushEnd() {
    if (!g_distanceFadeHooks) {
        return;
    }
    DepthTagFlushEnd(g_depthTags);
}

float CaptureGetDistanceFadeFarZ() {
    if (!g_distanceFadeHooks) {
        return 0.0f;
    }
    return UpdateDistanceFadeFarZ(g_distanceFadeFarZ, g_depthTags.frameMaxZ);
}

ThinQuadStats CaptureTakeThinQuadStats() {
    const ThinQuadStats out = g_thinQuadStats;
    g_thinQuadStats = ThinQuadStats{};
    return out;
}

void CaptureEndFrame() {
    if (g_thinQuadFrameActive) {
        AccumulateThinQuadFrame(g_thinQuadStats, g_thinQuadFrame);
        g_thinQuadFrame = ThinQuadFrameCounts{};
        g_thinQuadFrameActive = false;
    }
    if (g_horizonCapturedThisFrame) {
        HudSentinelAdvance(g_hudSentinel);
    }
    g_capturedCount = 0;
    g_overflowCount = 0;
    g_batchIndex = 0;
    g_dumpThisFrame = false;
    g_dumpTriggeredCheckedThisFrame = false;
    g_horizonCapturedThisFrame = false;
    g_wideHorizonComposedThisFrame = false;
}

CaptureFrameCounters CaptureGetFrameCounters() {
    CaptureFrameCounters c;
    c.capturedCount = g_capturedCount;
    c.overflowCount = g_overflowCount;
    c.batchIndex = g_batchIndex;
    c.dumpThisFrame = g_dumpThisFrame;
    c.horizonCapturedThisFrame = g_horizonCapturedThisFrame;
    return c;
}
