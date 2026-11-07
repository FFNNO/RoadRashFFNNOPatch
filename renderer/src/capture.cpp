#include "capture.h"
#include "config.h"
#include "hook.h"
#include "log.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kMaxCapturedQuads = 4096;
constexpr uint32_t kCrcCacheSize     = 512;  // Power of two for open-addressed table

// Engine globals (preferred VAs in RoadRash95.exe):
// 0x00596a90: uint8_t[0xde][256] sprite CLUT remap tables (DAT_00596a90)
// 0x005aaa04: const uint8_t* dynamic sprite template PDAT pointer (DAT_005aaa04)
// 0x005aaa14: int32_t  snap shift (DAT_005aaa14; 15 in original HiRes, 16 in patched)
// 0x005aaa18: uint32_t snap mask  (DAT_005aaa18; 0xfffffffe in original HiRes, 0xffffffff in patched)
// 0x005ca79c: uint32_t global RSC file handle (DAT_005ca79c)
// 0x005ca7a0: uint32_t track/level RSC file handle (DAT_005ca7a0)
// 0x005cab48: uint32_t biker sprite resource buffer (DAT_005cab48)
// 0x006036c0: int32_t[4] clip rect [xmin, ymin, xmax, ymax] (DAT_006036c0..0x006036cc)
// 0x006036e8: uint32_t race tick counter (DAT_006036e8)
// 0x00603740: uint32_t[4] streaming sprite family slot IDs (DAT_00603740..0x0060374c)
// 0x00618580: int16_t[16] 4 vertices {x, y, u, v} (DAT_00618580..0x0061859e)
// 0x006185a4: const uint8_t* active mip level texel pointer set by FUN_0042e850 (DAT_006185a4)
// 0x006185e0: const uint8_t* current render node poly = ccb + 0x68 (DAT_006185e0)
constexpr uint32_t kVaSpriteClutTable = 0x00596a90;
constexpr uint32_t kVaSpritePdatPtr   = 0x005aaa04;
constexpr uint32_t kVaSnapShift       = 0x005aaa14;
constexpr uint32_t kVaSnapMask        = 0x005aaa18;
constexpr uint32_t kVaGlobalRscHandle = 0x005ca79c;
constexpr uint32_t kVaTrackRscHandle  = 0x005ca7a0;
constexpr uint32_t kVaBikerSprBuf     = 0x005cab48;
constexpr uint32_t kVaClipRect        = 0x006036c0;
constexpr uint32_t kVaRaceTick        = 0x006036e8;
constexpr uint32_t kVaFamilySlots     = 0x00603740;
constexpr uint32_t kVaVertsInt16      = 0x00618580;
constexpr uint32_t kVaActiveMipPtr    = 0x006185a4;
constexpr uint32_t kVaCurrentPoly     = 0x006185e0;

HMODULE g_hostModule = nullptr;
std::wstring g_dumpDir;

CapturedQuad g_capturedQuads[kMaxCapturedQuads];
uint32_t g_capturedCount = 0;
uint32_t g_overflowCount = 0;
uint32_t g_batchIndex = 0;
uint32_t g_raceFrameNumber = 0;
bool g_f11WasDown = false;

struct LevelStateSnapshot {
    bool initialized = false;
    uint32_t globalRsc = 0;
    uint32_t trackRsc = 0;
    uint32_t spritePdat = 0;
    uint32_t bikerBuf = 0;
    uint32_t familySlots[4] = {0, 0, 0, 0};
    uint32_t lastRaceTick = 0;
};

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

    if (!g_levelSnapshot.initialized) {
        g_levelSnapshot.initialized = true;
        g_levelSnapshot.globalRsc = globalRsc;
        g_levelSnapshot.trackRsc = trackRsc;
        g_levelSnapshot.spritePdat = spritePdat;
        g_levelSnapshot.bikerBuf = bikerBuf;
        for (int i = 0; i < 4; ++i) {
            g_levelSnapshot.familySlots[i] = families[i];
        }
        g_levelSnapshot.lastRaceTick = raceTick;
        return;
    }

    bool changed = (globalRsc != g_levelSnapshot.globalRsc) ||
                   (trackRsc != g_levelSnapshot.trackRsc) ||
                   (spritePdat != g_levelSnapshot.spritePdat) ||
                   (bikerBuf != g_levelSnapshot.bikerBuf) ||
                   (raceTick < g_levelSnapshot.lastRaceTick);
    for (int i = 0; i < 4; ++i) {
        if (families[i] != g_levelSnapshot.familySlots[i]) {
            changed = true;
        }
    }

    if (changed) {
        CaptureInvalidateCache();
        g_levelSnapshot.globalRsc = globalRsc;
        g_levelSnapshot.trackRsc = trackRsc;
        g_levelSnapshot.spritePdat = spritePdat;
        g_levelSnapshot.bikerBuf = bikerBuf;
        for (int i = 0; i < 4; ++i) {
            g_levelSnapshot.familySlots[i] = families[i];
        }
    }
    g_levelSnapshot.lastRaceTick = raceTick;
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
        char texId[64];
        if (q.kind == QuadKind::Flat) {
            std::snprintf(texId, sizeof(texId), "flat_%u", static_cast<unsigned int>(q.flatColor));
        } else {
            std::snprintf(texId, sizeof(texId), "%dx%d_%08x", q.texWidth, q.texHeight, q.level0Crc32);
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
            texId,
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
    g_levelSnapshot = LevelStateSnapshot{};
    CaptureInvalidateCache();
    EnsureCrcTable();
}

void CaptureInvalidateCache() {
    std::memset(g_crcCache, 0, sizeof(g_crcCache));
}

void CaptureOnBatchStart() {
    CheckAndInvalidateCacheOnLevelChange();
    ++g_batchIndex;
}

int CaptureTexturedQuadBefore() {
    if (!g_hostModule) {
        return -1;
    }
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
    q.culled = false;
    q.ccbFlags = *reinterpret_cast<const uint32_t*>(ccb + 0x00);
    q.uFlip = *(poly + 0x3d);
    q.vFlip = *(poly + 0x3e);
    q.spriteClut = *(poly + 0x3c);
    q.spriteIndex = *reinterpret_cast<const int32_t*>(poly + 0x38);

    const int32_t shift = *GlobalPtr<int32_t>(kVaSnapShift);
    const uint32_t mask = *GlobalPtr<uint32_t>(kVaSnapMask);
    const int32_t* corners16 = reinterpret_cast<const int32_t*>(ccb + 0x44);
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
            int32_t spriteW = q.uvBox[2] - q.uvBox[0];
            int32_t spriteH = q.uvBox[3] - q.uvBox[1];
            if (spriteW <= 0 || spriteW > pdatPitch) spriteW = pdatPitch;
            if (spriteH <= 0 || spriteH > pdatHeight) spriteH = pdatHeight;
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
        } else {
            q.texWidth = pdatPitch;
            q.texHeight = pdatHeight;
            const uint32_t keyB = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(level0Ptr));
            q.level0Crc32 = LookupOrComputeLevel0Crc32(
                q.pdatPtr, keyB, level0Ptr, pdatPitch, pdatHeight, pdatPitch, nullptr, 0);
        }
    }

    // Observation-only: never write to engine globals.
    return static_cast<int>(slot);
}

void CaptureTexturedQuadAfter(int slotIndex) {
    if (slotIndex < 0 || static_cast<uint32_t>(slotIndex) >= g_capturedCount) {
        return;
    }
    CapturedQuad& q = g_capturedQuads[slotIndex];

    int16_t minX = q.verts[0].x;
    int16_t maxX = q.verts[0].x;
    int16_t minY = q.verts[0].y;
    int16_t maxY = q.verts[0].y;
    for (int i = 1; i < 4; ++i) {
        if (q.verts[i].x < minX) minX = q.verts[i].x;
        if (q.verts[i].x > maxX) maxX = q.verts[i].x;
        if (q.verts[i].y < minY) minY = q.verts[i].y;
        if (q.verts[i].y > maxY) maxY = q.verts[i].y;
    }

    // In FUN_0042e850, local_c == local_18 (minY == maxY) is the only early return before DAT_006185a4 is written.
    if (minY == maxY || q.pdatPtr == 0) {
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

    if (maxY < q.clipRect[1] || minY > q.clipRect[3] || maxX < q.clipRect[0] || minX > q.clipRect[2]) {
        q.culled = true;
    }
}

int CaptureFlatQuadBefore(const int32_t* polyVerts, int edxArg) {
    if (!g_hostModule || !polyVerts) {
        return -1;
    }
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

void CaptureFlatQuadAfter(int slotIndex, const int32_t* polyVerts) {
    if (slotIndex < 0 || static_cast<uint32_t>(slotIndex) >= g_capturedCount || !polyVerts) {
        return;
    }

    CapturedQuad& q = g_capturedQuads[slotIndex];
    for (int i = 0; i < 4; ++i) {
        q.verts[i].x = static_cast<int16_t>(polyVerts[i * 2 + 0]);
        q.verts[i].y = static_cast<int16_t>(polyVerts[i * 2 + 1]);
        q.verts[i].u = 0;
        q.verts[i].v = 0;
    }

    int32_t minX = q.verts[0].x;
    int32_t maxX = q.verts[0].x;
    int32_t minY = q.verts[0].y;
    int32_t maxY = q.verts[0].y;
    for (int i = 1; i < 4; ++i) {
        if (q.verts[i].x < minX) minX = q.verts[i].x;
        if (q.verts[i].x > maxX) maxX = q.verts[i].x;
        if (q.verts[i].y < minY) minY = q.verts[i].y;
        if (q.verts[i].y > maxY) maxY = q.verts[i].y;
    }

    // Note: in FUN_00467cb0, minY == maxY takes the single-scanline FUN_00468ce0 fast path (not culled!)
    // as long as the horizontal span [max(minX, clip_xmin) .. min(maxX, clip_xmax)] is non-empty.
    const int32_t clipL = (minX > q.clipRect[0]) ? minX : q.clipRect[0];
    const int32_t clipR = (maxX < q.clipRect[2]) ? maxX : q.clipRect[2];
    if (maxY < q.clipRect[1] || minY > q.clipRect[3] || clipL >= clipR) {
        q.culled = true;
    }
}

void CaptureOnRacePresent(const uint8_t* surface, int pitch, const uint8_t* palette, int width, int height) {
    ++g_raceFrameNumber;

    const bool f11Down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    const bool f11Pressed = f11Down && !g_f11WasDown;
    g_f11WasDown = f11Down;

    const int autoDumpFrame = GetConfig().captureAutoDumpFrame;
    const bool autoTriggered = (autoDumpFrame > 0 && static_cast<int>(g_raceFrameNumber) == autoDumpFrame);

    if (f11Pressed || autoTriggered) {
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

void CaptureEndFrame() {
    g_capturedCount = 0;
    g_overflowCount = 0;
    g_batchIndex = 0;
}
