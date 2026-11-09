// hook_test: exercises the inline hook engine in-process on a naked x86
// function that starts with the same prologue as FUN_0046e630
// (push ebp / mov ebp,esp / sub esp,42Ch) and uses the same __fastcall ABI,
// plus unit tests for ParseFilterMode, ComputeLetterboxViewport,
// PresenterFailureState time-windowed latch/backoff, WriteIndexedPng,
// and WriteFrameJsonFromQuads.
#include <windows.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "../src/hook.h"
#include "../src/config.h"
#include "../src/presenter.h"
#include "../src/capture.h"
#include "../src/quad_mapping.h"
#include "../src/texture_pack.h"

namespace {

int g_failures = 0;

std::wstring MakeTestTempPath(const wchar_t* name) {
    wchar_t tempDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempDir);
    return std::wstring(tempDir) + name + std::to_wstring(GetCurrentProcessId());
}

std::string ReadWholeFile(const std::wstring& path) {
    std::string content;
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || !fp) {
        return content;
    }
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) {
        content.append(buf, n);
    }
    std::fclose(fp);
    return content;
}

size_t CountOccurrences(const std::string& haystack, const std::string& needle) {
    size_t count = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

void WriteDummyFile(const std::wstring& path) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"wb") == 0 && fp) {
        std::fputc(0, fp);
        std::fclose(fp);
    }
}

void WriteBytesFile(const std::wstring& path, const uint8_t* bytes, size_t size) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"wb") == 0 && fp) {
        std::fwrite(bytes, 1, size, fp);
        std::fclose(fp);
    }
}

// A 256-entry RGBx palette whose entries 1..255 are all non-zero.
void FillTestBasePalette(uint8_t (&palette)[1024]) {
    for (int i = 0; i < 256; ++i) {
        palette[i * 4 + 0] = static_cast<uint8_t>(i);
        palette[i * 4 + 1] = static_cast<uint8_t>(255 - i / 2);
        palette[i * 4 + 2] = static_cast<uint8_t>(64 + i / 4);
        palette[i * 4 + 3] = 0;
    }
}

uint32_t PackRgba(const uint8_t* palette, int idx) {
    return static_cast<uint32_t>(palette[idx * 4 + 0]) |
           (static_cast<uint32_t>(palette[idx * 4 + 1]) << 8) |
           (static_cast<uint32_t>(palette[idx * 4 + 2]) << 16) | 0xFF000000u;
}

void Check(bool condition, const char* what) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    if (!condition) {
        ++g_failures;
    }
}

// Returns a + b + c. One stack argument, callee cleans it (RET 4).
extern "C" __declspec(naked) int __fastcall Target(int /*a: ECX*/, int /*b: EDX*/, int /*c: stack*/) {
    __asm {
        push ebp
        mov ebp, esp
        sub esp, 0x42C
        mov eax, ecx
        add eax, edx
        add eax, [ebp + 8]
        mov esp, ebp
        pop ebp
        ret 4
    }
}

using TargetFn = int(__fastcall*)(int, int, int);
TargetFn g_original = nullptr;
int g_detourCalls = 0;

int __fastcall Detour(int a, int b, int c) {
    ++g_detourCalls;
    return g_original(a, b, c) * 10;
}

uint32_t PreferredVa(const void* address) {
    const auto image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(address) - image) + rr95::kPreferredImageBase;
}

uint32_t ReadBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint32_t TestCrc32(const uint8_t* data, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        c ^= data[i];
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
    }
    return c ^ 0xFFFFFFFFu;
}

uint32_t TestAdler32(const uint8_t* data, size_t len) {
    uint32_t s1 = 1u;
    uint32_t s2 = 0u;
    for (size_t i = 0; i < len; ++i) {
        s1 = (s1 + data[i]) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    return (s2 << 16) | s1;
}

std::vector<uint8_t> ReadFileBytes(const std::wstring& path) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || !fp) {
        return {};
    }
    std::fseek(fp, 0, SEEK_END);
    const long sz = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> buf(sz > 0 ? static_cast<size_t>(sz) : 0u);
    if (!buf.empty()) {
        std::fread(buf.data(), 1, buf.size(), fp);
    }
    std::fclose(fp);
    return buf;
}

void RunCaptureWritersTest() {
    wchar_t tempDir[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tempDir);
    const std::wstring pngPath = std::wstring(tempDir) + L"rr95_hook_test_frame.png";
    const std::wstring jsonPath = std::wstring(tempDir) + L"rr95_hook_test_frame.json";

    constexpr int kW = 16;
    constexpr int kH = 12;
    constexpr int kPitch = 20;  // Pitch > width to verify stride handling
    uint8_t surface[kH * kPitch];
    std::memset(surface, 0xEE, sizeof(surface));
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            surface[y * kPitch + x] = static_cast<uint8_t>((y * 17 + x * 3) & 0xFF);
        }
    }

    uint8_t palette[256 * 4];
    for (int i = 0; i < 256; ++i) {
        palette[i * 4 + 0] = static_cast<uint8_t>(i);
        palette[i * 4 + 1] = static_cast<uint8_t>(255 - i);
        palette[i * 4 + 2] = static_cast<uint8_t>((i * 7) & 0xFF);
        palette[i * 4 + 3] = 0;
    }

    Check(WriteIndexedPng(pngPath, surface, kPitch, palette, kW, kH),
          "WriteIndexedPng writes test PNG file");

    const std::vector<uint8_t> png = ReadFileBytes(pngPath);
    DeleteFileW(pngPath.c_str());

    static const uint8_t kPngSig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    Check(png.size() > 64 && std::memcmp(png.data(), kPngSig, 8) == 0,
          "PNG starts with valid 8-byte signature");

    bool sawIhdr = false;
    bool sawPlte = false;
    bool sawIdat = false;
    bool sawIend = false;
    bool allChunkCrcsValid = true;
    std::vector<uint8_t> idatPayload;

    size_t pos = 8;
    while (pos + 12 <= png.size()) {
        const uint32_t len = ReadBe32(png.data() + pos);
        if (pos + 12 + len > png.size()) {
            allChunkCrcsValid = false;
            break;
        }
        const char* type = reinterpret_cast<const char*>(png.data() + pos + 4);
        const uint8_t* data = png.data() + pos + 8;
        const uint32_t expectedCrc = ReadBe32(png.data() + pos + 8 + len);
        const uint32_t actualCrc = TestCrc32(png.data() + pos + 4, 4 + len);
        if (expectedCrc != actualCrc) {
            allChunkCrcsValid = false;
        }
        if (std::memcmp(type, "IHDR", 4) == 0 && len == 13) {
            sawIhdr = (ReadBe32(data) == static_cast<uint32_t>(kW)) &&
                      (ReadBe32(data + 4) == static_cast<uint32_t>(kH)) &&
                      data[8] == 8 && data[9] == 3;
        } else if (std::memcmp(type, "PLTE", 4) == 0 && len == 768) {
            sawPlte = true;
            for (int i = 0; i < 256; ++i) {
                if (data[i * 3 + 0] != palette[i * 4 + 0] ||
                    data[i * 3 + 1] != palette[i * 4 + 1] ||
                    data[i * 3 + 2] != palette[i * 4 + 2]) {
                    sawPlte = false;
                }
            }
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            sawIdat = true;
            idatPayload.insert(idatPayload.end(), data, data + len);
        } else if (std::memcmp(type, "IEND", 4) == 0 && len == 0) {
            sawIend = true;
        }
        pos += 12 + len;
    }

    Check(allChunkCrcsValid && pos == png.size(), "all PNG chunk CRCs and lengths are valid");
    Check(sawIhdr && sawPlte && sawIdat && sawIend, "PNG contains valid IHDR, PLTE, IDAT, and IEND chunks");

    // Verify stored deflate stream & Adler-32 in IDAT:
    // 2-byte zlib header (0x78, 0x01) + 5-byte stored block header (0x01, len_lo, len_hi, nlen_lo, nlen_hi)
    // + rawScanlines + 4-byte big-endian Adler-32.
    const size_t expectedRawSize = static_cast<size_t>(kH) * (1 + static_cast<size_t>(kW));
    Check(idatPayload.size() == 2 + 5 + expectedRawSize + 4,
          "IDAT stored deflate payload has exact expected byte length");
    if (idatPayload.size() == 2 + 5 + expectedRawSize + 4) {
        const uint16_t bLen = static_cast<uint16_t>(idatPayload[3] | (idatPayload[4] << 8));
        const uint16_t bNlen = static_cast<uint16_t>(idatPayload[5] | (idatPayload[6] << 8));
        Check(idatPayload[0] == 0x78 && idatPayload[1] == 0x01 && idatPayload[2] == 0x01 &&
                  bLen == expectedRawSize && bNlen == static_cast<uint16_t>(~bLen),
              "IDAT zlib header and stored block LEN/NLEN are valid");

        const uint8_t* rawScanlines = idatPayload.data() + 7;
        bool pixelsMatch = true;
        for (int y = 0; y < kH; ++y) {
            const uint8_t* row = rawScanlines + y * (1 + kW);
            if (row[0] != 0x00) {
                pixelsMatch = false;
            }
            for (int x = 0; x < kW; ++x) {
                if (row[1 + x] != surface[y * kPitch + x]) {
                    pixelsMatch = false;
                }
            }
        }
        Check(pixelsMatch, "IDAT scanlines match input 8bpp surface with pitch > width");

        const uint32_t storedAdler = ReadBe32(idatPayload.data() + 7 + expectedRawSize);
        const uint32_t computedAdler = TestAdler32(rawScanlines, expectedRawSize);
        Check(storedAdler == computedAdler, "IDAT Adler-32 checksum matches decompressed scanlines");
    }

    // Test WriteFrameJsonFromQuads
    CapturedQuad quads[2] = {};
    quads[0].drawOrder = 0;
    quads[0].batchIndex = 1;
    quads[0].kind = QuadKind::Flat;
    quads[0].culled = false;
    quads[0].flatColor = 12;
    quads[0].spriteIndex = -1;
    quads[0].chosenMip = -1;
    quads[0].pdatPtr = 0x005AA9D4u;
    quads[0].pdatMagic = 0x00000010u;
    quads[0].pdatFlags = 0x00000010u;
    quads[0].clipRect[0] = 0;
    quads[0].clipRect[1] = 100;
    quads[0].clipRect[2] = 639;
    quads[0].clipRect[3] = 359;
    quads[0].verts[0] = {10, 120, 0, 0};
    quads[0].verts[1] = {200, 120, 0, 0};
    quads[0].verts[2] = {200, 180, 0, 0};
    quads[0].verts[3] = {10, 180, 0, 0};

    quads[1].drawOrder = 1;
    quads[1].batchIndex = 1;
    quads[1].kind = QuadKind::Textured;
    quads[1].culled = false;
    quads[1].spriteClut = 3;
    quads[1].spriteIndex = 42;
    quads[1].pdatPtr = 0x005AAA04u;
    quads[1].pdatMagic = 0x00BC614Eu;
    quads[1].pdatFlags = 0x00002002u;
    quads[1].texWidth = 64;
    quads[1].texHeight = 32;
    quads[1].mipLevels = 1;
    quads[1].chosenMip = 0;
    quads[1].level0Crc32 = 0xDEADBEEFu;
    quads[1].clipRect[0] = 0;
    quads[1].clipRect[1] = 100;
    quads[1].clipRect[2] = 639;
    quads[1].clipRect[3] = 359;
    quads[1].uvBox[0] = 0;
    quads[1].uvBox[1] = 0;
    quads[1].uvBox[2] = 64;
    quads[1].uvBox[3] = 32;
    quads[1].verts[0] = {50, 150, 1, 1};
    quads[1].verts[1] = {114, 150, 63, 1};
    quads[1].verts[2] = {114, 182, 63, 31};
    quads[1].verts[3] = {50, 182, 1, 31};

    Check(WriteFrameJsonFromQuads(jsonPath, 7, 640, 480, 16, 0xFFFFFFFFu, quads, 2, 0),
          "WriteFrameJsonFromQuads writes test JSON file");

    const std::vector<uint8_t> jsonBytes = ReadFileBytes(jsonPath);
    DeleteFileW(jsonPath.c_str());
    const std::string jsonStr(jsonBytes.begin(), jsonBytes.end());

    Check(jsonStr.find("\"frame_number\": 7") != std::string::npos &&
              jsonStr.find("\"snap_shift\": 16") != std::string::npos &&
              jsonStr.find("\"snap_mask\": \"0xFFFFFFFF\"") != std::string::npos &&
              jsonStr.find("\"quad_count\": 2") != std::string::npos,
          "JSON header contains expected frame metadata");
    Check(jsonStr.find("\"texture_id\": \"flat_12\"") != std::string::npos &&
              jsonStr.find("\"texture_id\": \"64x32_deadbeef\"") != std::string::npos &&
              jsonStr.find("\"sprite_index\": 42") != std::string::npos &&
              jsonStr.find("\"clip_rect\": [0, 100, 639, 359]") != std::string::npos,
          "JSON quad entries contain expected texture_id, sprite_index, and clip_rect fields");
}

// Independent integer model of the engine rasterizer FUN_0042e850 + FUN_0042e4d0, written from the
// decompilation (out/gemini/B4/decomp_excerpts_b3.c) and used as the reference for the shared
// quad mapping in quad_mapping.h. It deliberately shares no code with QuadMapUv.
namespace engine_ref {

struct Vtx {
    int16_t x;
    int16_t y;
    int16_t u;
    int16_t v;
};

// One edge-table row (DAT_005ac768 / DAT_005adde8, 12 bytes per scanline): 16.16 x, u, v.
struct Row {
    int32_t x = 0;
    int32_t u = 0;
    int32_t v = 0;
    bool set = false;
};

constexpr int kRows = 480;

int32_t To16(int32_t value) {
    return static_cast<int32_t>(static_cast<uint32_t>(value) << 16);
}

int32_t Floor16(int32_t value16) {
    return value16 >> 16;  // arithmetic shift: the integer word read at +2 of each table entry
}

// FUN_0042e4d0: walk one edge from vertex a down to vertex b, writing rows a.y..b.y inclusive
// (clamped to 479, negative rows skipped) with x, u, v starting at a and stepping by a per-row
// delta. Horizontal edges (a.y == b.y) write nothing. u and v are read as unsigned 16-bit.
void WalkEdge(Row* table, const Vtx& a, const Vtx& b) {
    if (a.y == b.y) {
        return;
    }
    const int dy = b.y - a.y;
    if (dy < 0) {
        return;  // the engine loop runs from a.y while y <= b.y, so an upward edge writes nothing
    }
    int32_t x = To16(a.x);
    int32_t u = To16(static_cast<uint16_t>(a.u));
    int32_t v = To16(static_cast<uint16_t>(a.v));
    const int32_t dx = static_cast<int32_t>((static_cast<int64_t>(b.x - a.x) * 65536) / dy);
    const int32_t du = static_cast<int32_t>(
        (static_cast<int64_t>(static_cast<uint16_t>(b.u) - static_cast<uint16_t>(a.u)) * 65536) / dy);
    const int32_t dv = static_cast<int32_t>(
        (static_cast<int64_t>(static_cast<uint16_t>(b.v) - static_cast<uint16_t>(a.v)) * 65536) / dy);
    const int yEnd = (b.y < kRows - 1) ? b.y : kRows - 1;
    for (int y = a.y; y <= yEnd; ++y) {
        if (y >= 0) {
            table[y].x = x;
            table[y].u = u;
            table[y].v = v;
            table[y].set = true;
        }
        x += dx;
        u += du;
        v += dv;
    }
}

struct Pixel {
    int x;
    int y;
    double sampleX;  // continuous x at which the engine's u/v for this pixel lies on the scanline
    double u;        // texel units
    double v;
};

// FUN_0042e850 without mip selection and without left/right screen clipping (the test shapes lie
// inside the screen): top vertex = first vertex with the smallest y; table A walks the backward
// chain (top-1, top-2, ...), table B the forward chain (top+1, ...), both skipping vertices that
// share the top y; the chain whose current edge ends higher advances first (A on ties). Each row
// spans [floor(xL), floor(xR)) after swapping the two entries when B lies left of A, rows whose
// table-B integer x is <= 0 are skipped, and u/v start at the left entry's exact value and step by
// (right - left) / width per pixel. The engine's reciprocal table DAT_0059253c is not in the
// excerpt, so the per-pixel step uses exact integer division.
std::vector<Pixel> Rasterize(const Vtx vtx[4]) {
    std::vector<Pixel> out;
    std::vector<Row> tableA(kRows);
    std::vector<Row> tableB(kRows);

    int top = (vtx[1].y < vtx[0].y) ? 1 : 0;
    if (vtx[2].y < vtx[top].y) top = 2;
    if (vtx[3].y < vtx[top].y) top = 3;
    const int yTop = vtx[top].y;
    int yBot = vtx[0].y;
    for (int i = 1; i < 4; ++i) {
        yBot = (vtx[i].y > yBot) ? vtx[i].y : yBot;
    }
    if (yTop == yBot) {
        return out;
    }

    int aCur = top;
    int aNext = (top + 3) & 3;
    while (vtx[aNext].y == yTop) {
        aCur = aNext;
        aNext = (aNext + 3) & 3;
    }
    int bCur = top;
    int bNext = (top + 1) & 3;
    while (vtx[bNext].y == yTop) {
        bCur = bNext;
        bNext = (bNext + 1) & 3;
    }
    WalkEdge(tableA.data(), vtx[aCur], vtx[aNext]);
    WalkEdge(tableB.data(), vtx[bCur], vtx[bNext]);
    int aEnd = vtx[aNext].y;
    int bEnd = vtx[bNext].y;
    for (int guard = 0; (aEnd != yBot || bEnd != yBot) && guard < 8; ++guard) {
        if (aEnd <= bEnd) {
            aCur = aNext;
            aNext = (aNext + 3) & 3;
            WalkEdge(tableA.data(), vtx[aCur], vtx[aNext]);
            aEnd = vtx[aNext].y;
        } else {
            bCur = bNext;
            bNext = (bNext + 1) & 3;
            WalkEdge(tableB.data(), vtx[bCur], vtx[bNext]);
            bEnd = vtx[bNext].y;
        }
    }

    const int yFirst = (yTop > 0) ? yTop : 0;
    const int yLast = (yBot < kRows - 1) ? yBot : kRows - 1;
    for (int y = yFirst; y <= yLast; ++y) {
        const Row& ra = tableA[y];
        const Row& rb = tableB[y];
        if (!ra.set || !rb.set) {
            continue;
        }
        const int xa = Floor16(ra.x);
        const int xb = Floor16(rb.x);
        if (xb <= 0) {
            continue;
        }
        const Row& left = (xb < xa) ? rb : ra;
        const Row& right = (xb < xa) ? ra : rb;
        const int xLeft = (xb < xa) ? xb : xa;
        const int width = (xb < xa) ? (xa - xb) : (xb - xa);
        if (width == 0) {
            continue;
        }
        const int32_t du = (right.u - left.u) / width;
        const int32_t dv = (right.v - left.v) / width;
        for (int k = 0; k < width; ++k) {
            Pixel px;
            px.x = xLeft + k;
            px.y = y;
            px.sampleX = (static_cast<double>(left.x) +
                          static_cast<double>(right.x - left.x) * k / width) / 65536.0;
            px.u = static_cast<double>(left.u + du * k) / 65536.0;
            px.v = static_cast<double>(left.v + dv * k) / 65536.0;
            out.push_back(px);
        }
    }
    return out;
}

}  // namespace engine_ref

struct MappingStats {
    size_t pixels = 0;
    double maxShaderDiff = 0.0;    // shared QuadMapUv vs engine reference, texels
    double maxTriangleDiff = 0.0;  // two independently interpolated triangles vs engine reference
};

bool BarycentricUv(double px, double py, const engine_ref::Vtx& a, const engine_ref::Vtx& b,
                   const engine_ref::Vtx& c, double& u, double& v) {
    const double denom = static_cast<double>(b.y - c.y) * (a.x - c.x) + static_cast<double>(c.x - b.x) * (a.y - c.y);
    if (std::fabs(denom) < 1e-9) {
        return false;
    }
    const double w0 = ((b.y - c.y) * (px - c.x) + (c.x - b.x) * (py - c.y)) / denom;
    const double w1 = ((c.y - a.y) * (px - c.x) + (a.x - c.x) * (py - c.y)) / denom;
    const double w2 = 1.0 - w0 - w1;
    if (w0 < -1e-6 || w1 < -1e-6 || w2 < -1e-6) {
        return false;
    }
    u = w0 * a.u + w1 * b.u + w2 * c.u;
    v = w0 * a.v + w1 * b.v + w2 * c.v;
    return true;
}

// Evaluates the shared mapping at the engine's own sample position of every pixel the engine
// rasterizer covers. The engine starts each span at pixel floor(xL) with the exact left-edge u/v,
// so its sample for pixel x lies at xL + (x - floor(xL)) * (xR - xL) / (floor(xR) - floor(xL)) on
// scanline y; a mapping that matches the engine at the geometric pixel centre would have to
// replicate that per-row floor quantization, which is the stair-stepping the GPU path removes.
MappingStats CompareWithEngine(const engine_ref::Vtx vtx[4]) {
    MappingStats stats;
    rr::qm::float2 pts[4];
    rr::qm::float2 uvs[4];
    for (int i = 0; i < 4; ++i) {
        pts[i] = rr::qm::float2(static_cast<float>(vtx[i].x), static_cast<float>(vtx[i].y));
        uvs[i] = rr::qm::float2(static_cast<float>(static_cast<uint16_t>(vtx[i].u)),
                                static_cast<float>(static_cast<uint16_t>(vtx[i].v)));
    }
    const std::vector<engine_ref::Pixel> pixels = engine_ref::Rasterize(vtx);
    stats.pixels = pixels.size();
    for (const engine_ref::Pixel& px : pixels) {
        const rr::qm::QuadUvSample m = rr::qm::QuadMapUv(pts, uvs, static_cast<float>(px.sampleX),
                                                         static_cast<float>(px.y));
        const double d = std::max(std::fabs(m.uv.x - px.u), std::fabs(m.uv.y - px.v));
        stats.maxShaderDiff = std::max(stats.maxShaderDiff, d);

        double tu = 0.0;
        double tv = 0.0;
        if (BarycentricUv(px.sampleX, px.y, vtx[0], vtx[1], vtx[2], tu, tv) ||
            BarycentricUv(px.sampleX, px.y, vtx[0], vtx[2], vtx[3], tu, tv)) {
            const double td = std::max(std::fabs(tu - px.u), std::fabs(tv - px.v));
            stats.maxTriangleDiff = std::max(stats.maxTriangleDiff, td);
        }
    }
    return stats;
}

void ReverseWinding(const engine_ref::Vtx in[4], engine_ref::Vtx out[4]) {
    out[0] = in[0];
    out[1] = in[3];
    out[2] = in[2];
    out[3] = in[1];
}

}  // namespace

int main() {
    HMODULE self = GetModuleHandleW(nullptr);
    volatile TargetFn call = &Target;
    Check(call(1, 2, 3) == 6, "target works before hooking");

    // Decoding validation rejects relative instructions (including prefixed ones) and bad spans.
    rr95::HookSite relative{"relative", 0, {0x55, 0xE8, 0, 0, 0, 0}, 6, {1, 5}, nullptr, nullptr, false};
    Check(rr95::ValidateHookDecoding(relative) != nullptr, "call rel32 inside the span is refused");
    rr95::HookSite prefixedRel{"prefixed_rel", 0, {0x55, 0x66, 0xE8, 0, 0, 0, 0}, 7, {1, 6}, nullptr, nullptr, false};
    Check(rr95::ValidateHookDecoding(prefixedRel) != nullptr, "prefixed call rel inside the span is refused");
    rr95::HookSite jcc{"jcc", 0, {0x55, 0x0F, 0x84, 0, 0, 0, 0}, 7, {1, 6}, nullptr, nullptr, false};
    Check(rr95::ValidateHookDecoding(jcc) != nullptr, "jcc rel32 inside the span is refused");
    rr95::HookSite prefixedJcc{"prefixed_jcc", 0, {0x55, 0x3E, 0x74, 0x04, 0x90}, 5, {1, 3, 1}, nullptr, nullptr, false};
    Check(rr95::ValidateHookDecoding(prefixedJcc) != nullptr, "segment-prefixed jcc rel8 inside the span is refused");
    rr95::HookSite shortSpan{"short", 0, {0x55, 0x8B, 0xEC}, 3, {1, 2}, nullptr, nullptr, false};
    Check(rr95::ValidateHookDecoding(shortSpan) != nullptr, "span shorter than 5 bytes is refused");
    rr95::HookSite mismatch{"mismatch", 0, {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x2C, 0x04, 0x00, 0x00}, 9, {1, 2, 5},
                            nullptr, nullptr, false};
    Check(rr95::ValidateHookDecoding(mismatch) != nullptr, "lengths not adding up to the span are refused");

    // Wrong expected bytes: refused, code untouched.
    uint8_t before[9];
    std::memcpy(before, reinterpret_cast<const void*>(&Target), sizeof(before));
    void* unused = nullptr;
    rr95::HookSite wrong{"wrong_bytes", PreferredVa(reinterpret_cast<const void*>(&Target)),
                         {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x2C, 0x05, 0x00, 0x00}, 9, {1, 2, 6},
                         reinterpret_cast<void*>(&Detour), &unused, false};
    Check(!rr95::InstallHook(wrong, self), "mismatching bytes are refused");
    Check(std::memcmp(before, reinterpret_cast<const void*>(&Target), sizeof(before)) == 0,
          "refused hook leaves the code untouched");

    // Correct site: hook installs and the detour wraps the original.
    rr95::HookSite site{"target", PreferredVa(reinterpret_cast<const void*>(&Target)),
                        {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x2C, 0x04, 0x00, 0x00}, 9, {1, 2, 6},
                        reinterpret_cast<void*>(&Detour), reinterpret_cast<void**>(&g_original), false};
    Check(rr95::ValidateHookDecoding(site) == nullptr, "FUN_0046e630-style prologue decoding is valid");
    Check(rr95::InstallHook(site, self), "hook installs");
    Check(call(1, 2, 3) == 60, "hooked call goes through detour and trampoline");
    Check(g_detourCalls == 1, "detour called exactly once");
    Check(g_original != nullptr && g_original(4, 5, 6) == 15, "trampoline runs the original");

    // RemoveHook restores original bytes, clears trampoline pointer, and unhooks execution.
    rr95::RemoveHook(site, self);
    Check(!site.installed, "RemoveHook marks site uninstalled");
    Check(g_original == nullptr, "RemoveHook nulls the trampoline pointer");
    Check(std::memcmp(before, reinterpret_cast<const void*>(&Target), sizeof(before)) == 0,
          "RemoveHook restores the original prologue bytes");
    Check(call(1, 2, 3) == 6, "unhooked call executes original directly");
    Check(g_detourCalls == 1, "detour is not called after RemoveHook");

    // Filter parsing unit tests (T3b hardening).
    FilterMode mode = FilterMode::Bilinear;
    Check(ParseFilterMode(L"nearest", mode) && mode == FilterMode::Nearest,
          "ParseFilterMode('nearest') -> Nearest");
    Check(ParseFilterMode(L"BILINEAR", mode) && mode == FilterMode::Bilinear,
          "ParseFilterMode('BILINEAR') -> Bilinear");
    Check(ParseFilterMode(L"Sharp-Bilinear", mode) && mode == FilterMode::SharpBilinear,
          "ParseFilterMode('Sharp-Bilinear') -> SharpBilinear");
    Check(!ParseFilterMode(L"invalid-filter", mode) && mode == FilterMode::Bilinear,
          "ParseFilterMode('invalid-filter') returns false and defaults to Bilinear");

    // Letterbox viewport math unit tests (T3b hardening).
    const LetterboxViewport vp1080 = ComputeLetterboxViewport(640, 480, 1920, 1080);
    Check(vp1080.x == 240 && vp1080.y == 0 && vp1080.width == 1440 && vp1080.height == 1080,
          "ComputeLetterboxViewport(640x480 -> 1920x1080) == {240, 0, 1440, 1080}");

    const LetterboxViewport vp1024 = ComputeLetterboxViewport(640, 480, 1280, 1024);
    Check(vp1024.x == 0 && vp1024.y == 32 && vp1024.width == 1280 && vp1024.height == 960,
          "ComputeLetterboxViewport(640x480 -> 1280x1024) == {0, 32, 1280, 960}");

    const LetterboxViewport vpNative = ComputeLetterboxViewport(640, 480, 640, 480);
    Check(vpNative.x == 0 && vpNative.y == 0 && vpNative.width == 640 && vpNative.height == 480,
          "ComputeLetterboxViewport(640x480 -> 640x480) == {0, 0, 640, 480}");

    const LetterboxViewport vpZero = ComputeLetterboxViewport(640, 480, 0, 0);
    Check(vpZero.width == 1 && vpZero.height == 1,
          "ComputeLetterboxViewport(640x480 -> 0x0) clamps to width=1, height=1");

    // Time-windowed presenter failure latch & backoff unit tests (B3 item 6 + T4c window fix).
    {
        PresenterFailureState fs = {};
        Check(PresenterCanAttempt(fs, 1000), "presenter can attempt initially");
        Check(!PresenterRecordFailure(fs, 1000) && fs.failureCount == 1 && !fs.latchedOff,
              "first failure records count=1 without latching");
        Check(!PresenterCanAttempt(fs, 1500),
              "presenter backs off during 1s cooldown after failure");
        Check(PresenterCanAttempt(fs, 2000),
              "presenter allows retry after 1000ms backoff expires");

        // Successful present resets failure counter.
        PresenterRecordSuccess(fs);
        Check(fs.failureCount == 0 && !fs.latchedOff,
              "PresenterRecordSuccess resets failure count to 0");

        // Failure window expiration (> 5000ms since first failure resets count to 1).
        Check(!PresenterRecordFailure(fs, 3000) && fs.failureCount == 1,
              "failure at t=3000 starts a new window");
        Check(!PresenterRecordFailure(fs, 4050) && fs.failureCount == 2 && !fs.latchedOff,
              "second failure at t=4050 increments count to 2");
        Check(!PresenterRecordFailure(fs, 9000) && fs.failureCount == 1 && !fs.latchedOff,
              "failure at t=9000 (>5000ms after firstFailureMs=3000) resets window to count=1");

        // 3 failures spaced by realistic backoff (t=9000, t=10050, t=11100) within 5000ms window latches off permanently.
        Check(!PresenterRecordFailure(fs, 10050) && fs.failureCount == 2 && !fs.latchedOff,
              "second failure in window at t=10050 sets count=2");
        Check(PresenterRecordFailure(fs, 11100) && fs.failureCount == 3 && fs.latchedOff,
              "third failure in 5s window at t=11100 latches presenter off");
        Check(!PresenterCanAttempt(fs, 30000),
              "latched presenter stays off even after long delay");
    }

    // T4c & T5 pure helper unit tests:
    // 1. ClampInternalHeight
    Check(ClampInternalHeight(100) == 240 && ClampInternalHeight(1080) == 1080 && ClampInternalHeight(9999) == 4320,
          "ClampInternalHeight clamps to [240, 4320] and preserves 1080");

    // 2. ComputeTextureRectCrc32 (stride padding invariance + extraData sensitivity)
    {
        uint8_t bufA[4 * 8];
        uint8_t bufB[4 * 8];
        std::memset(bufA, 0x11, sizeof(bufA));
        std::memset(bufB, 0xFE, sizeof(bufB));
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 5; ++x) {
                const uint8_t v = static_cast<uint8_t>(y * 10 + x + 1);
                bufA[y * 8 + x] = v;
                bufB[y * 8 + x] = v;
            }
        }
        const uint32_t crcA = ComputeTextureRectCrc32(bufA, 5, 4, 8, nullptr, 0);
        const uint32_t crcB = ComputeTextureRectCrc32(bufB, 5, 4, 8, nullptr, 0);
        Check(crcA != 0 && crcA == crcB,
              "ComputeTextureRectCrc32 ignores rowPitch padding bytes outside width");

        const uint8_t extra1[2] = {0x01, 0x02};
        const uint8_t extra2[2] = {0x01, 0x03};
        const uint32_t crcE1 = ComputeTextureRectCrc32(bufA, 5, 4, 8, extra1, sizeof(extra1));
        const uint32_t crcE2 = ComputeTextureRectCrc32(bufA, 5, 4, 8, extra2, sizeof(extra2));
        Check(crcE1 != crcA && crcE1 != crcE2,
              "ComputeTextureRectCrc32 incorporates extraData (CLUT descriptor)");
    }

    // 3. ClampSpriteDimensions
    {
        int32_t w = 0, h = 0;
        const int32_t uvNormal[4] = {0, 0, 64, 48};
        ClampSpriteDimensions(uvNormal, 256, 256, w, h);
        Check(w == 64 && h == 48, "ClampSpriteDimensions preserves normal sprite box");

        const int32_t uvInverted[4] = {50, 80, 10, 20};
        ClampSpriteDimensions(uvInverted, 256, 256, w, h);
        Check(w == 1 && h == 1, "ClampSpriteDimensions clamps inverted box to 1x1");

        const int32_t uvOversize[4] = {-20, -10, 500, 400};
        ClampSpriteDimensions(uvOversize, 256, 256, w, h);
        Check(w == 256 && h == 256, "ClampSpriteDimensions clamps negative/oversize box to pdat bounds");
    }

    // 4. UpdateLevelStateSnapshot
    {
        LevelStateSnapshot snap = {};
        const uint32_t famA[4] = {10, 20, 30, 40};
        const uint32_t famB[4] = {10, 20, 99, 40};
        Check(!UpdateLevelStateSnapshot(snap, 1, 2, 3, 4, famA, 100) && snap.initialized,
              "first UpdateLevelStateSnapshot initializes snapshot without invalidation");
        Check(!UpdateLevelStateSnapshot(snap, 1, 2, 3, 4, famA, 101),
              "monotonic raceTick with identical pointers does not invalidate");
        Check(UpdateLevelStateSnapshot(snap, 1, 9, 3, 4, famA, 102),
              "changed trackRsc triggers cache invalidation");
        Check(UpdateLevelStateSnapshot(snap, 1, 9, 3, 4, famB, 103),
              "changed familySlots[2] triggers cache invalidation");
        Check(UpdateLevelStateSnapshot(snap, 1, 9, 3, 4, famB, 50),
              "decreased raceTick (new race) triggers cache invalidation");
    }

    // 5. ShouldCullTexturedQuad & ShouldCullFlatQuad
    {
        const int32_t clip[4] = {0, 0, 640, 479};
        const CapturedVertex vValid[4] = {{10, 20, 0, 0}, {50, 20, 0, 0}, {50, 40, 0, 0}, {10, 40, 0, 0}};
        const CapturedVertex vSingleLine[4] = {{10, 20, 0, 0}, {50, 20, 0, 0}, {50, 20, 0, 0}, {10, 20, 0, 0}};
        const CapturedVertex vZeroWidth[4] = {{30, 20, 0, 0}, {30, 20, 0, 0}, {30, 40, 0, 0}, {30, 40, 0, 0}};
        const CapturedVertex vOutsideRight[4] = {{640, 20, 0, 0}, {650, 20, 0, 0}, {650, 40, 0, 0}, {640, 40, 0, 0}};

        Check(!ShouldCullTexturedQuad(vValid, clip, 0x005AAA04u),
              "ShouldCullTexturedQuad keeps valid inside-clip quad");
        Check(ShouldCullTexturedQuad(vValid, clip, 0u),
              "ShouldCullTexturedQuad culls when pdatPtr == 0");
        Check(ShouldCullTexturedQuad(vSingleLine, clip, 0x005AAA04u),
              "ShouldCullTexturedQuad culls single-scanline quad (minY == maxY)");
        Check(!ShouldCullFlatQuad(vSingleLine, clip),
              "ShouldCullFlatQuad keeps single-scanline flat quad (FUN_00468ce0 fast path)");
        Check(ShouldCullFlatQuad(vZeroWidth, clip),
              "ShouldCullFlatQuad culls zero-width flat quad (clipL >= clipR)");
        Check(ShouldCullFlatQuad(vOutsideRight, clip),
              "ShouldCullFlatQuad culls quad at x=[640,650) when clip_xmax=640 (half-open X)");
    }

    // 6. ParseVirtualKey tests (T5d: Scroll Lock default, named keys, strict numeric parsing)
    {
        Check(ParseVirtualKey(L"F12", 0x91) == 0x7B, "ParseVirtualKey resolves F12 to 0x7B");
        Check(ParseVirtualKey(L"f12", 0x91) == 0x7B, "ParseVirtualKey case-insensitive f12");
        Check(ParseVirtualKey(L"F10", 0x91) == 0x79, "ParseVirtualKey resolves F10 to 0x79");
        Check(ParseVirtualKey(L"F1", 0x91) == 0x70, "ParseVirtualKey resolves F1 to 0x70");
        Check(ParseVirtualKey(L"ScrollLock", 0) == 0x91 && ParseVirtualKey(L" scrolllock ", 0) == 0x91,
              "ParseVirtualKey resolves ScrollLock to VK_SCROLL 0x91");
        Check(ParseVirtualKey(L"Pause", 0) == 0x13, "ParseVirtualKey resolves Pause to VK_PAUSE 0x13");
        Check(ParseVirtualKey(L"0x7B", 0) == 0x7B, "ParseVirtualKey resolves hex 0x7B");
        Check(ParseVirtualKey(L"123", 0) == 123, "ParseVirtualKey resolves decimal 123");
        Check(ParseVirtualKey(L"0x7Bzz", 0x91) == 0x91 && ParseVirtualKey(L"12abc", 0x91) == 0x91 &&
                  ParseVirtualKey(L"0x", 0x91) == 0x91,
              "ParseVirtualKey rejects trailing garbage and falls back to the default");
        Check(ParseVirtualKey(L"0x100", 0x91) == 0x91 && ParseVirtualKey(L"0", 0x91) == 0x91 &&
                  ParseVirtualKey(L"-5", 0x91) == 0x91,
              "ParseVirtualKey rejects codes outside 1..255");
        Check(ParseVirtualKey(L"invalid", 0x91) == 0x91, "ParseVirtualKey falls back to default on invalid key");
        Check(Config{}.toggleKey == 0x91, "default ToggleKey is Scroll Lock (outside the 0x10..0x79 game key table)");
    }

    // 7. Shared quad mapping (quad_mapping.h) vs an independent integer model of FUN_0042e850.
    {
        using engine_ref::Vtx;
        // Road trapezoid (flat top), strong skew (no parallel edges) and an axis-aligned sprite rectangle.
        const Vtx trapezoid[4] = {{280, 100, 0, 0}, {360, 100, 64, 0}, {560, 300, 64, 256}, {80, 300, 0, 256}};
        const Vtx skew[4] = {{200, 80, 0, 0}, {450, 120, 256, 0}, {520, 380, 256, 256}, {120, 310, 0, 256}};
        const Vtx sprite[4] = {{150, 200, 1, 1}, {350, 200, 127, 1}, {350, 420, 127, 127}, {150, 420, 1, 127}};
        struct Case {
            const char* name;
            const Vtx* vtx;
            double minTriangleDiff;  // < 0: rectangle, two triangles must match too
        };
        const Case cases[] = {
            {"trapezoid", trapezoid, 25.0},
            {"strong skew", skew, 30.0},
            {"sprite rectangle", sprite, -1.0},
        };
        char what[256];
        for (const Case& c : cases) {
            for (int reversed = 0; reversed < 2; ++reversed) {
                Vtx vtx[4];
                if (reversed) {
                    ReverseWinding(c.vtx, vtx);
                } else {
                    for (int i = 0; i < 4; ++i) vtx[i] = c.vtx[i];
                }
                const MappingStats st = CompareWithEngine(vtx);
                const char* winding = reversed ? "reversed winding" : "engine winding";
                std::snprintf(what, sizeof(what),
                              "%s (%s): shared QuadMapUv within 1/16 texel of the engine walk at all %zu covered pixels (max %.4f)",
                              c.name, winding, st.pixels, st.maxShaderDiff);
                Check(st.pixels > 1000 && st.maxShaderDiff <= 0.0625, what);
                if (c.minTriangleDiff > 0.0) {
                    std::snprintf(what, sizeof(what),
                                  "%s (%s): two-triangle interpolation is off by > %.0f texels (max %.2f), so it is rejected",
                                  c.name, winding, c.minTriangleDiff, st.maxTriangleDiff);
                    Check(st.maxTriangleDiff > c.minTriangleDiff, what);
                } else {
                    std::snprintf(what, sizeof(what),
                                  "%s (%s): two triangles match the engine on an axis-aligned rectangle (max %.4f)",
                                  c.name, winding, st.maxTriangleDiff);
                    Check(st.maxTriangleDiff <= 0.0625, what);
                }
            }
        }

        // The HLSL text is the same source (macro-stringified) as the C++ instantiation used above.
        const std::string hlsl = RR_QUAD_MAPPING_HLSL;
        Check(hlsl.find("QuadUvSample QuadMapUv(float2 pts[4], float2 uvs[4], float curX, float curY)") !=
                      std::string::npos &&
                  hlsl.find("inline") == std::string::npos && hlsl.find("//") == std::string::npos,
              "RR_QUAD_MAPPING_HLSL carries the shared QuadMapUv source without C++-only tokens");

        const std::string psSource = PresenterWorldPixelShaderSource();
        ID3DBlob* psBlob = nullptr;
        ID3DBlob* psErrors = nullptr;
        const HRESULT psHr = D3DCompile(psSource.c_str(), psSource.size(), "world_ps", nullptr, nullptr, "main",
                                        "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &psErrors);
        if (FAILED(psHr) && psErrors) {
            std::printf("%s\n", static_cast<const char*>(psErrors->GetBufferPointer()));
        }
        Check(SUCCEEDED(psHr) && psBlob != nullptr, "world pixel shader (shared mapping + main) compiles as ps_4_0");
        if (psBlob) psBlob->Release();
        if (psErrors) psErrors->Release();
    }

    // 8. Texture addressing (T5d): atlas boxes clamp, UVs past the texture wrap, others clamp.
    {
        const int32_t inU[4] = {1, 62, 62, 1};
        const int32_t inV[4] = {1, 1, 254, 254};
        const int32_t fullBox[4] = {0, 0, 63, 255};
        QuadAddressing a = ChooseQuadAddressing(fullBox, false, 64, 256, inU, inV);
        Check(!a.wrap && !a.atlas && a.clampTexel[2] == 63 && a.clampTexel[3] == 255,
              "full-texture uv_box with UVs inside clamps to the texture rect");

        const int32_t atlasBox[4] = {0, 128, 63, 255};
        const int32_t pastU[4] = {-3, 70, 70, -3};
        a = ChooseQuadAddressing(atlasBox, false, 64, 256, pastU, inV);
        Check(!a.wrap && a.atlas && a.clampTexel[1] == 128 && a.clampTexel[3] == 255,
              "atlas sub-rectangle clamps to its uv_box even when UVs go past it");

        // The 4 dumped quads with UVs past the texture: dynamic sprites, uv_box {0,0,0,0}, u/v in {-1, 1}.
        const int32_t emptyBox[4] = {0, 0, 0, 0};
        const int32_t badU[4] = {1, -1, -1, 1};
        const int32_t badV[4] = {1, 1, -1, -1};
        a = ChooseQuadAddressing(emptyBox, true, 256, 256, badU, badV);
        Check(a.wrap && !a.atlas, "dynamic sprite with UVs outside [0, size] wraps instead of stretching");

        a = ChooseQuadAddressing(fullBox, false, 64, 256, pastU, inV);
        Check(a.wrap, "non-atlas texture with UVs outside [0, size] wraps");

        const int32_t edgeU[4] = {0, 64, 64, 0};
        const int32_t edgeV[4] = {0, 0, 256, 256};
        a = ChooseQuadAddressing(emptyBox, false, 64, 256, edgeU, edgeV);
        Check(!a.wrap && a.clampTexel[2] == 63, "UVs exactly on [0, size] still clamp");
    }

    // 9. Presenter health gates the software-rasterizer skip (T5b item 1).
    {
        PresenterFailureState fs = {};
        Check(PresenterStateAllowsGpuWorld(fs), "healthy presenter allows skipping the software rasterizer");
        PresenterRecordFailure(fs, 1000);
        Check(!PresenterStateAllowsGpuWorld(fs), "a failed present (transient failure) keeps the software world");
        Check(!PresenterCanAttempt(fs, 1500) && !PresenterStateAllowsGpuWorld(fs),
              "presenter in retry backoff keeps the software world");
        PresenterRecordSuccess(fs);
        Check(PresenterStateAllowsGpuWorld(fs), "a successful present re-enables the GPU world");
        fs.latchedOff = true;
        Check(!PresenterStateAllowsGpuWorld(fs), "latched-off presenter keeps the software world");
    }

    // 10. Per-frame reset and toggle-key edge detection (T5b item 4, hook_sites.cpp FinishRaceFrameAndPollHotkeys).
    {
        RaceFrameHotkeyState hs;
        RaceFrameActions act = AdvanceRaceFrame(hs, false, false, 0x91, false);
        Check(!act.endCaptureFrame && !act.toggleGpuWorld, "no capture and no GpuWorld: frame state untouched");
        act = AdvanceRaceFrame(hs, true, false, 0x91, false);
        Check(act.endCaptureFrame, "capture enabled: per-frame capture state is reset");
        act = AdvanceRaceFrame(hs, false, true, 0x91, false);
        Check(act.endCaptureFrame, "GpuWorld enabled: per-frame capture state is reset");
        act = AdvanceRaceFrame(hs, false, true, 0x91, true);
        Check(act.toggleGpuWorld, "toggle key press toggles once on the rising edge");
        act = AdvanceRaceFrame(hs, false, true, 0x91, true);
        Check(!act.toggleGpuWorld, "held toggle key does not toggle again");
        AdvanceRaceFrame(hs, false, true, 0x91, false);
        act = AdvanceRaceFrame(hs, false, true, 0x91, true);
        Check(act.toggleGpuWorld, "release and press toggles again");
        act = AdvanceRaceFrame(hs, false, true, 0, true);
        Check(!act.toggleGpuWorld && !hs.prevKeyDown, "toggle key 0 never toggles");

        CaptureInit(nullptr, L"");
        CaptureOnBatchStart();
        CaptureOnBatchStart();
        CaptureFrameCounters fc = CaptureGetFrameCounters();
        Check(fc.batchIndex == 2 && !fc.dumpThisFrame, "two FUN_004688e0 batches counted within a frame");
        CaptureEndFrame();
        fc = CaptureGetFrameCounters();
        Check(fc.batchIndex == 0 && fc.capturedCount == 0 && fc.overflowCount == 0 && !fc.dumpThisFrame &&
                  !fc.horizonCapturedThisFrame,
              "CaptureEndFrame resets batch, quad, overflow, dump, and horizon state");
    }

    // 11. Texture pixel cache byte cap (T5b item 2).
    {
        CaptureInit(nullptr, L"");
        CaptureSetTexturePixelCacheCap(3000);
        uint8_t tex[32 * 32];
        for (int i = 0; i < 32 * 32; ++i) tex[i] = static_cast<uint8_t>(i);
        const uint32_t gen0 = CaptureGetCacheGeneration();
        const uint32_t ev0 = CaptureGetTexturePixelCacheEvictions();
        CaptureCacheTexturePixels(0x1000, 1, tex, 32, 32, 32);
        CaptureCacheTexturePixels(0x2000, 2, tex, 32, 32, 32);
        Check(CaptureGetTexturePixelCacheBytes() == 2048 && CaptureGetCacheGeneration() == gen0,
              "texture pixel cache accounts bytes below the cap");
        CaptureCacheTexturePixels(0x3000, 3, tex, 32, 32, 32);
        int32_t w = 0;
        int32_t h = 0;
        const bool evictedOld = CaptureLookupTexturePixels(0x1000, 1, w, h) == nullptr;
        const bool keptNew = CaptureLookupTexturePixels(0x3000, 3, w, h) != nullptr && w == 32 && h == 32;
        Check(CaptureGetTexturePixelCacheBytes() == 1024 && CaptureGetTexturePixelCacheEvictions() == ev0 + 1 &&
                  CaptureGetCacheGeneration() == gen0 + 1 && evictedOld && keptNew,
              "insert past the byte cap clears the cache, bumps the generation, and keeps the new texture");
        CaptureSetTexturePixelCacheCap(kDefaultTexturePixelCacheCapBytes);
        CaptureInvalidateCache();
        Check(CaptureGetTexturePixelCacheBytes() == 0, "cache invalidation resets the byte count");
    }

    // 12. HUD sentinel selection (T5b item 5).
    {
        HudSentinelState st;
        uint32_t hist[256] = {};
        Check(HudSentinelActive(st) == 0x7E, "first HUD sentinel is 0x7E");
        hist[0x7E] = 300000;  // clear value
        hist[0] = 5000;       // black HUD art
        hist[200] = 100;
        Check(!HudSentinelObserve(st, hist), "HUD output without the inactive candidate keeps both sentinels");
        HudSentinelAdvance(st);
        Check(HudSentinelActive(st) == 0x60, "sentinels alternate between frames");

        uint32_t hist2[256] = {};
        hist2[0x60] = 290000;  // clear value of this frame
        hist2[0x7E] = 12;      // a HUD writer drew 0x7E while it was not the clear value
        hist2[0] = 4000;
        uint8_t retired = 0;
        uint8_t repl = 0;
        Check(HudSentinelObserve(st, hist2, &retired, &repl) && retired == 0x7E && repl != 0 && repl != 0x60 &&
                  repl != 0x7E && repl != 200 && st.lastSeen[repl] == 0,
              "a candidate drawn by a HUD writer is replaced by an index never seen in HUD output");
        HudSentinelAdvance(st);
        Check(HudSentinelActive(st) == repl, "the replacement becomes the next frame's clear value");
    }

    // 13. Sub-pixel corners (T6): exact snapped-corner matching, permutations, ties, and fallbacks.
    {
        auto makeQuad = [](const double cornersPx[8], const int perm[4], uint32_t mask) {
            CapturedQuad q = {};
            q.kind = QuadKind::Textured;
            q.snapShift = 15;
            q.snapMask = mask;
            for (int i = 0; i < 8; ++i) {
                q.corners16_16[i] = static_cast<int32_t>(std::floor(cornersPx[i] * 32768.0));
                q.snappedPreFix[i] = static_cast<int32_t>(static_cast<uint32_t>(q.corners16_16[i] >> 15) & mask);
            }
            for (int i = 0; i < 4; ++i) {
                q.verts[i].x = static_cast<int16_t>(q.snappedPreFix[perm[i] * 2 + 0]);
                q.verts[i].y = static_cast<int16_t>(q.snappedPreFix[perm[i] * 2 + 1]);
            }
            return q;
        };
        const double corners[8] = {100.25, 50.75, 200.5, 50.125, 200.875, 150.25, 100.0, 150.96875};
        const int identity[4] = {0, 1, 2, 3};
        const int swapped[4] = {1, 0, 3, 2};

        CapturedQuad q = makeQuad(corners, identity, 0xFFFFFFFFu);
        SubPixelVertices s = ResolveSubPixelVertices(q);
        Check(s.eligible && s.useCorners && s.x[0] == 100.25f && s.y[3] == 150.96875f && s.maxDelta < 1.0f,
              "identity order: vertices use their exact 16.16 corners");

        q = makeQuad(corners, swapped, 0xFFFFFFFFu);
        s = ResolveSubPixelVertices(q);
        Check(s.useCorners && s.permutation[0] == 1 && s.permutation[1] == 0 && s.permutation[2] == 3 &&
                  s.permutation[3] == 2 && s.x[0] == 200.5f && s.y[1] == 50.75f,
              "FUN_00468d60 reorder (1,0,3,2) is recovered from the exact snapped match");

        // Two corners inside the same pixel: both permutations match, the tie-break must stay valid.
        const double dupCorners[8] = {100.125, 50.25, 100.875, 50.75, 160.5, 150.5, 60.5, 150.5};
        q = makeQuad(dupCorners, identity, 0xFFFFFFFFu);
        s = ResolveSubPixelVertices(q);
        Check(s.useCorners && s.maxDelta < 1.0f && s.permutation[2] == 2 && s.permutation[3] == 3 &&
                  ((s.permutation[0] == 0 && s.permutation[1] == 1) || (s.permutation[0] == 1 && s.permutation[1] == 0)),
              "corners snapping to the same pixel resolve to a valid exact match");

        // Rebuilt vertex (FUN_00468d60 midpoint repair): no exact match, fall back to the snapped vertices.
        q = makeQuad(corners, identity, 0xFFFFFFFFu);
        q.verts[2].x = 150;
        q.verts[2].y = 100;
        s = ResolveSubPixelVertices(q);
        Check(s.eligible && !s.useCorners && s.x[2] == 150.0f && s.y[2] == 100.0f,
              "vertex without an exact corner match falls back to the snapped vertices");

        // 4-pixel snapping puts a corner 3.9 px from its vertex: beyond the tolerance, fall back.
        const double farCorners[8] = {103.9, 50.0, 200.0, 50.0, 200.0, 150.0, 100.0, 150.0};
        q = makeQuad(farCorners, identity, 0xFFFFFFFCu);
        s = ResolveSubPixelVertices(q);
        Check(s.eligible && !s.useCorners, "matched corner farther than the tolerance falls back to snapped vertices");

        // 2-pixel snapping (original exe mask 0xFFFFFFFE): deltas up to 2 px stay within the widened tolerance.
        const double evenCorners[8] = {101.9, 51.5, 201.75, 50.5, 201.0, 151.9, 100.5, 151.0};
        q = makeQuad(evenCorners, identity, 0xFFFFFFFEu);
        s = ResolveSubPixelVertices(q);
        Check(s.useCorners && s.maxDelta > 1.5f && s.maxDelta < 2.0f,
              "2-pixel snap mask widens the tolerance so its corners are still used");

        const double flatCorners[8] = {100.5, 50.5, 200.5, 50.5, 200.5, 50.5, 100.5, 50.5};
        q = makeQuad(flatCorners, identity, 0xFFFFFFFFu);
        s = ResolveSubPixelVertices(q);
        Check(!s.eligible && !s.useCorners, "single-scanline quads keep their snapped vertices");
    }

    // 14. Blended mipmaps (T6): config, engine mip chain cache, atlas level limit, analytic gradients.
    {
        char what[256];
        MipFilter mf = MipFilter::Chosen;
        Check(ParseMipFilter(L"Trilinear", mf) && mf == MipFilter::Trilinear &&
                  ParseMipFilter(L" chosen ", mf) && mf == MipFilter::Chosen,
              "ParseMipFilter accepts trilinear and chosen");
        Check(!ParseMipFilter(L"bogus", mf) && mf == MipFilter::Trilinear,
              "ParseMipFilter falls back to trilinear on unknown values");
        Check(ClampAnisotropy(0) == 0 && ClampAnisotropy(1) == 0 && ClampAnisotropy(2) == 2 &&
                  ClampAnisotropy(8) == 8 && ClampAnisotropy(64) == 16,
              "Anisotropy: 0 or 1 = off, 2..16 kept, larger values clamp to 16");
        Check(Config{}.mipFilter == MipFilter::Trilinear && Config{}.anisotropy == 8 && Config{}.subPixel,
              "defaults: MipFilter=trilinear, Anisotropy=8, SubPixel=1");

        Check(ClampMipLevelCount(64, 256, 7) == 7 && ClampMipLevelCount(64, 256, 8) == 7 &&
                  ClampMipLevelCount(64, 64, 0) == 1 && ClampMipLevelCount(256, 256, 20) == 9,
              "ClampMipLevelCount stops before a level dimension reaches 0");

        // A 8x4 texture with two engine levels below it (4x2, 2x1).
        uint8_t l0[8 * 4];
        uint8_t l1[4 * 2];
        uint8_t l2[2 * 1];
        for (int i = 0; i < 32; ++i) l0[i] = static_cast<uint8_t>(i);
        for (int i = 0; i < 8; ++i) l1[i] = static_cast<uint8_t>(100 + i);
        l2[0] = 200;
        l2[1] = 201;
        const uint8_t* extra[2] = {l1, l2};
        CaptureInit(nullptr, L"");
        CaptureCacheTexturePixels(0x4000, 9, l0, 8, 4, 8, extra, 2);
        TextureMipChainView chain;
        const bool found = CaptureLookupTextureMipChain(0x4000, 9, chain);
        Check(found && chain.levels == 3 && chain.width[1] == 4 && chain.height[1] == 2 && chain.width[2] == 2 &&
                  chain.height[2] == 1 && chain.pixels[0][31] == 31 && chain.pixels[1][7] == 107 &&
                  chain.pixels[2][1] == 201 && CaptureGetTexturePixelCacheBytes() == 32 + 8 + 2,
              "engine mip levels are cached back to back with their own sizes (no regeneration)");
        CaptureCacheTexturePixels(0x5000, 9, l0, 8, 4, 8);
        Check(CaptureLookupTextureMipChain(0x5000, 9, chain) && chain.levels == 1,
              "MipFilter=chosen caches level 0 only");
        CaptureInvalidateCache();

        const int32_t fullBox[4] = {0, 0, 63, 255};
        const int32_t halfBox[4] = {0, 128, 63, 255};
        const int32_t oddBox[4] = {3, 0, 12, 63};
        Check(AtlasMaxMipLevel(fullBox, 7) == 5, "64x256 box keeps levels up to 32 -> 2 texels wide (level 5)");
        Check(AtlasMaxMipLevel(halfBox, 7) == 5, "aligned half-texture atlas cell keeps levels 0..5");
        Check(AtlasMaxMipLevel(oddBox, 7) == 2,
              "unaligned cell [3..12] stops at level 2 (level 3 covers no whole texel inside it)");

        // Analytic gradients of the shared mapping vs central differences inside a strongly skewed quad.
        rr::qm::float2 pts[4] = {rr::qm::float2(200.0f, 80.0f), rr::qm::float2(450.0f, 120.0f),
                                 rr::qm::float2(520.0f, 380.0f), rr::qm::float2(120.0f, 310.0f)};
        rr::qm::float2 uvs[4] = {rr::qm::float2(0.0f, 0.0f), rr::qm::float2(256.0f, 0.0f),
                                 rr::qm::float2(256.0f, 256.0f), rr::qm::float2(0.0f, 256.0f)};
        const float vertexY[4] = {80.0f, 120.0f, 380.0f, 310.0f};
        double maxRelErr = 0.0;
        int samples = 0;
        for (int y = 90; y <= 370; y += 7) {
            bool nearVertex = false;
            for (float vy : vertexY) nearVertex = nearVertex || std::fabs(static_cast<float>(y) - vy) < 2.0f;
            if (nearVertex) continue;
            for (int x = 130; x <= 510; x += 11) {
                const float fx = static_cast<float>(x);
                const float fy = static_cast<float>(y);
                const rr::qm::QuadUvSample m = rr::qm::QuadMapUv(pts, uvs, fx, fy);
                if (fx < m.xL + 2.0f || fx > m.xR - 2.0f) continue;
                const float h = 0.25f;
                const rr::qm::QuadUvSample mx0 = rr::qm::QuadMapUv(pts, uvs, fx - h, fy);
                const rr::qm::QuadUvSample mx1 = rr::qm::QuadMapUv(pts, uvs, fx + h, fy);
                const rr::qm::QuadUvSample my0 = rr::qm::QuadMapUv(pts, uvs, fx, fy - h);
                const rr::qm::QuadUvSample my1 = rr::qm::QuadMapUv(pts, uvs, fx, fy + h);
                const double fdux = (mx1.uv.x - mx0.uv.x) / (2.0 * h);
                const double fdvy = (my1.uv.y - my0.uv.y) / (2.0 * h);
                const double fduy = (my1.uv.x - my0.uv.x) / (2.0 * h);
                const double scale = 1.0 + std::fabs(fdux) + std::fabs(fdvy) + std::fabs(fduy);
                maxRelErr = std::max(maxRelErr, std::fabs(fdux - m.dUvDx.x) / scale);
                maxRelErr = std::max(maxRelErr, std::fabs(fdvy - m.dUvDy.y) / scale);
                maxRelErr = std::max(maxRelErr, std::fabs(fduy - m.dUvDy.x) / scale);
                ++samples;
            }
        }
        std::snprintf(what, sizeof(what),
                      "analytic scanline gradients match central differences at %d interior points (max rel err %.5f)",
                      samples, maxRelErr);
        Check(samples > 200 && maxRelErr < 0.01, what);

        // Pixels just outside the quad (clamped y, saturated t) keep the edge-row gradient: no spike, no zero.
        rr::qm::float2 tpts[4] = {rr::qm::float2(280.0f, 100.0f), rr::qm::float2(360.0f, 100.0f),
                                  rr::qm::float2(560.0f, 300.0f), rr::qm::float2(80.0f, 300.0f)};
        rr::qm::float2 tuvs[4] = {rr::qm::float2(0.0f, 0.0f), rr::qm::float2(64.0f, 0.0f),
                                  rr::qm::float2(64.0f, 256.0f), rr::qm::float2(0.0f, 256.0f)};
        const rr::qm::QuadUvSample edgeRow = rr::qm::QuadMapUv(tpts, tuvs, 320.0f, 100.0f);
        const rr::qm::QuadUvSample above = rr::qm::QuadMapUv(tpts, tuvs, 320.0f, 99.6f);
        const rr::qm::QuadUvSample rightOut = rr::qm::QuadMapUv(tpts, tuvs, 470.0f, 200.0f);
        const rr::qm::QuadUvSample rightIn = rr::qm::QuadMapUv(tpts, tuvs, 455.0f, 200.0f);
        Check(std::fabs(above.dUvDy.y - edgeRow.dUvDy.y) < 1e-4f && std::fabs(above.dUvDx.x - edgeRow.dUvDx.x) < 1e-4f &&
                  edgeRow.dUvDy.y > 0.5f,
              "pixel above the top edge (clamped y) keeps the top-row gradient");
        Check(rightOut.uv.x == 64.0f && std::fabs(rightOut.dUvDx.x - rightIn.dUvDx.x) < 1e-4f && rightOut.dUvDx.x > 0.0f,
              "pixel past the right edge (saturated t) keeps the span gradient instead of dropping to zero");
    }

    // B6 Work Unit 1 unit tests:
    // 1. FormatTextureId tests (scenery, track, dynamic sprite with CLUT from frame dumps)
    {
        Check(FormatTextureId(32, 67, 0xc1968fce) == "32x67_c1968fce",
              "FormatTextureId formats scenery texture 32x67_c1968fce");
        Check(FormatTextureId(256, 32, 0xe2819544) == "256x32_e2819544",
              "FormatTextureId formats track texture 256x32_e2819544");
        Check(FormatTextureId(54, 24, 0xc6f91bb1) == "54x24_c6f91bb1",
              "FormatTextureId formats dynamic sprite with CLUT 54x24_c6f91bb1");
    }

    // 2. ValidateMipLevelPointer tests (advisory B5 review)
    {
        uint8_t dummyLevel[32 * 32];
        Check(ValidateMipLevelPointer(dummyLevel, 64, 64, 1),
              "ValidateMipLevelPointer accepts valid allocated memory");
        Check(!ValidateMipLevelPointer(nullptr, 64, 64, 1),
              "ValidateMipLevelPointer rejects null pointer");
        Check(!ValidateMipLevelPointer(reinterpret_cast<const void*>(0xDEADBEEF), 64, 64, 1),
              "ValidateMipLevelPointer rejects uncommitted pointer 0xDEADBEEF");
        Check(!ValidateMipLevelPointer(dummyLevel, 32, 32, 6),
              "ValidateMipLevelPointer rejects level where dimension shifts to zero (32 >> 6 == 0)");
    }

    // 3. SaveRgbaPngWithWic test
    {
        wchar_t tempPath[MAX_PATH] = {0};
        GetTempPathW(MAX_PATH, tempPath);
        std::wstring testPng = std::wstring(tempPath) + L"rr95_wic_test.png";
        const uint32_t pixels[4] = {0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0x00000000u};
        bool saved = SaveRgbaPngWithWic(testPng, 2, 2, pixels);
        Check(saved, "SaveRgbaPngWithWic encodes a 2x2 RGBA PNG using WIC");
        DWORD attr = GetFileAttributesW(testPng.c_str());
        Check(attr != INVALID_FILE_ATTRIBUTES, "WIC test PNG was written to disk");
        DeleteFileW(testPng.c_str());
    }

    // 4. [TexturePack] config defaults and path resolution
    {
        const Config& cfg = GetConfig();
        Check(cfg.texturePackEnabled && cfg.texturePackPath == L"texture-pack" &&
              cfg.texturePackMaxMiB == 512 && cfg.texturePackDecodeBudgetMs == 4 &&
              !cfg.texturePackDump && cfg.texturePackDumpPath == L"texture-dump" && cfg.texturePackPreload,
              "[TexturePack] defaults match brief (Enabled=1, Path=texture-pack, MaxMiB=512, Budget=4, Dump=0, DumpPath=texture-dump, Preload=1)");
        std::wstring absPath = ResolveGamePath(L"texture-pack");
        Check(!absPath.empty() && absPath.find(L"texture-pack") != std::wstring::npos,
              "ResolveGamePath resolves relative pack path");
    }

    // B6 Work Unit 2 unit tests:
    // 5. Pack index lookup test
    {
        wchar_t tempDir[MAX_PATH] = {0};
        GetTempPathW(MAX_PATH, tempDir);
        std::wstring testDir = std::wstring(tempDir) + L"rr95_pack_test_" + std::to_wstring(GetCurrentProcessId());
        CreateDirectoryW(testDir.c_str(), nullptr);
        std::wstring dummyFile1 = testDir + L"\\32x67_c1968fce.png";
        std::wstring dummyFile2 = testDir + L"\\256x32_e2819544.png";
        std::wstring dummyTxt = testDir + L"\\readme.txt";
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, dummyFile1.c_str(), L"wb") == 0 && fp) { fputc(0, fp); fclose(fp); }
        if (_wfopen_s(&fp, dummyFile2.c_str(), L"wb") == 0 && fp) { fputc(0, fp); fclose(fp); }
        if (_wfopen_s(&fp, dummyTxt.c_str(), L"wb") == 0 && fp) { fputc(0, fp); fclose(fp); }

        TexturePackInit(testDir, 512, 4, false);
        Check(TexturePackHasIndexed("32x67_c1968fce"), "TexturePack indexes 32x67_c1968fce.png");
        Check(TexturePackHasIndexed("256x32_e2819544"), "TexturePack indexes 256x32_e2819544.png");
        Check(!TexturePackHasIndexed("readme"), "TexturePack ignores non-PNG files");
        Check(!TexturePackHasIndexed("nonexistent"), "TexturePack returns false for unindexed IDs");
        TexturePackShutdown();

        DeleteFileW(dummyFile1.c_str());
        DeleteFileW(dummyFile2.c_str());
        DeleteFileW(dummyTxt.c_str());
        RemoveDirectoryW(testDir.c_str());
    }

    // 6. The k-multiple validation test
    {
        int32_t k = 0;
        Check(ValidateReplacementDimensions(32, 64, 32, 64, k) && k == 1,
              "ValidateReplacementDimensions accepts k=1 (32x64 -> 32x64)");
        Check(ValidateReplacementDimensions(32, 64, 64, 128, k) && k == 2,
              "ValidateReplacementDimensions accepts k=2 (32x64 -> 64x128)");
        Check(ValidateReplacementDimensions(32, 64, 128, 256, k) && k == 4,
              "ValidateReplacementDimensions accepts k=4 (32x64 -> 128x256)");
        Check(ValidateReplacementDimensions(32, 64, 256, 512, k) && k == 8,
              "ValidateReplacementDimensions accepts k=8 (32x64 -> 256x512)");
        Check(!ValidateReplacementDimensions(32, 64, 64, 64, k),
              "ValidateReplacementDimensions rejects asymmetric scale (64x64 for 32x64)");
        Check(!ValidateReplacementDimensions(32, 64, 96, 192, k),
              "ValidateReplacementDimensions rejects non-power-of-two multiple k=3 (96x192)");
        Check(!ValidateReplacementDimensions(32, 64, 512, 1024, k),
              "ValidateReplacementDimensions rejects scale > 8 (k=16: 512x1024)");
        Check(!ValidateReplacementDimensions(32, 64, 16, 32, k),
              "ValidateReplacementDimensions rejects scale < 1 (16x32)");
    }

    // 7. Premultiplied conversion of a replacement
    {
        // 3 pixels:
        // Pixel 0: Opaque (100, 150, 200, 255)
        // Pixel 1: Transparent with non-zero color (255, 128, 64, 0) -> must become (0,0,0,0)
        // Pixel 2: Semi-transparent (200, 100, 50, 128)
        const uint8_t src[12] = {
            100, 150, 200, 255,
            255, 128, 64, 0,
            200, 100, 50, 128
        };
        uint32_t dst[3] = {};
        ConvertToPremultipliedRgba(src, 3, 1, dst);

        const uint32_t p0 = dst[0];
        Check((p0 & 0xFF) == 100 && ((p0 >> 8) & 0xFF) == 150 && ((p0 >> 16) & 0xFF) == 200 && ((p0 >> 24) & 0xFF) == 255,
              "ConvertToPremultipliedRgba preserves opaque pixels");
        Check(dst[1] == 0x00000000u,
              "ConvertToPremultipliedRgba zeros transparent pixels (alpha 0 -> 0x00000000)");

        const uint32_t p2 = dst[2];
        const uint32_t expR = (200u * 128u + 127u) / 255u;
        const uint32_t expG = (100u * 128u + 127u) / 255u;
        const uint32_t expB = (50u * 128u + 127u) / 255u;
        Check((p2 & 0xFF) == expR && ((p2 >> 8) & 0xFF) == expG && ((p2 >> 16) & 0xFF) == expB && ((p2 >> 24) & 0xFF) == 128,
              "ConvertToPremultipliedRgba correctly premultiplies semi-transparent pixels");
    }

    // 8. Box mip filter on a colour-keyed edge (no halos)
    {
        // 2x2 image: (0,0) is pure green (0, 255, 0, 255), others are transparent (0, 0, 0, 0).
        // Stored in premultiplied RGBA8:
        // p00 = 0xFF00FF00u (a=255, g=255)
        // p10 = 0, p01 = 0, p11 = 0
        const uint32_t base2x2[4] = {
            0xFF00FF00u, 0x00000000u,
            0x00000000u, 0x00000000u
        };
        std::vector<std::vector<uint32_t>> mips;
        bool ok = GeneratePremultipliedMipChain(2, 2, base2x2, mips);
        Check(ok && mips.size() == 2, "GeneratePremultipliedMipChain builds 2 levels for 2x2 base");
        Check(mips[1].size() == 1, "Level 1 is 1x1 texel");

        const uint32_t p = mips[1][0];
        const uint32_t r = p & 0xFFu;
        const uint32_t g = (p >> 8) & 0xFFu;
        const uint32_t b = (p >> 16) & 0xFFu;
        const uint32_t a = (p >> 24) & 0xFFu;

        // Box filter averages components: g = (255 + 2) / 4 = 64, a = (255 + 2) / 4 = 64.
        Check(r == 0 && b == 0 && g == 64 && a == 64,
              "Premultiplied 2x2 box filter averages premultiplied channels (G=64, A=64, R=0, B=0)");

        // Un-premultiplied in shader: g_unpremul = g / (a / 255.0) = 64 / (64 / 255.0) = 255!
        const float unpremulG = (static_cast<float>(g) / 255.0f) / (static_cast<float>(a) / 255.0f);
        Check(std::fabs(unpremulG - 1.0f) < 1e-4f,
              "Un-premultiplying the downsampled pixel recovers pure green 1.0 with ZERO halo");
    }

    // 9. Normalised uv_box for k = 4
    {
        const int32_t origW = 64;
        const int32_t origH = 64;
        const int32_t clampTexel[4] = { 16, 20, 31, 39 }; // minX, minY, maxX, maxY
        const float normBox[4] = {
            static_cast<float>(clampTexel[0]) / static_cast<float>(origW),
            static_cast<float>(clampTexel[1]) / static_cast<float>(origH),
            static_cast<float>(clampTexel[2] + 1) / static_cast<float>(origW),
            static_cast<float>(clampTexel[3] + 1) / static_cast<float>(origH)
        };

        // At level 0 of a k=4 replacement (256x256):
        const float size4x[2] = { 256.0f, 256.0f };
        const float loX = std::ceil(normBox[0] * size4x[0]);
        const float loY = std::ceil(normBox[1] * size4x[1]);
        const float hiX = std::max(std::floor(normBox[2] * size4x[0]) - 1.0f, loX);
        const float hiY = std::max(std::floor(normBox[3] * size4x[1]) - 1.0f, loY);

        Check(static_cast<int32_t>(loX) == clampTexel[0] * 4 &&
              static_cast<int32_t>(loY) == clampTexel[1] * 4,
              "Normalized uv_box at level 0 of k=4 replacement scales min texel by 4");
        Check(static_cast<int32_t>(hiX) == (clampTexel[2] + 1) * 4 - 1 &&
              static_cast<int32_t>(hiY) == (clampTexel[3] + 1) * 4 - 1,
              "Normalized uv_box at level 0 of k=4 replacement scales max texel by 4");

        // At level log2(4) = 2 of the k=4 replacement (size 64x64):
        const float size2x[2] = { 64.0f, 64.0f };
        const float loX_lvl2 = std::ceil(normBox[0] * size2x[0]);
        const float hiX_lvl2 = std::max(std::floor(normBox[2] * size2x[0]) - 1.0f, loX_lvl2);
        Check(static_cast<int32_t>(loX_lvl2) == clampTexel[0] &&
              static_cast<int32_t>(hiX_lvl2) == clampTexel[2],
              "Normalized uv_box at level 2 of k=4 replacement exactly recovers original level 0 clamp range");
    }

    // 10. Dump manifest round-trip and persistence test (B6 review Blocker 1).
    {
        wchar_t tempDir[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tempDir);
        const std::wstring testManPath = std::wstring(tempDir) + L"test_manifest_" +
                                         std::to_wstring(GetCurrentProcessId()) + L".json";
        DeleteFileW(testManPath.c_str());

        std::unordered_map<std::string, TextureDumpInfo> entriesA;
        TextureDumpInfo itemA;
        itemA.id = "32x67_c1968fce";
        itemA.width = 32;
        itemA.height = 67;
        itemA.colorKey = true;
        itemA.mipLevels = 1;
        itemA.kind = "texture";
        itemA.spriteClut = 0;
        itemA.frameNumber = 12;
        entriesA.emplace(itemA.id, itemA);

        const bool save1Ok = SaveDumpManifest(testManPath, entriesA);
        Check(save1Ok, "SaveDumpManifest successfully writes initial manifest");

        std::unordered_map<std::string, TextureDumpInfo> loadedEntries1;
        std::unordered_set<std::string> loadedDumped1;
        const bool load1Ok = LoadDumpManifest(testManPath, loadedEntries1, loadedDumped1);
        Check(load1Ok, "LoadDumpManifest successfully parses manifest");
        Check(loadedEntries1.size() == 1, "LoadDumpManifest parsed exactly 1 entry");
        Check(loadedDumped1.find("32x67_c1968fce") != loadedDumped1.end(), "Entry A is in dumped set");
        auto itA = loadedEntries1.find("32x67_c1968fce");
        const bool matchA = (itA != loadedEntries1.end() &&
                             itA->second.width == 32 &&
                             itA->second.height == 67 &&
                             itA->second.colorKey == true &&
                             itA->second.mipLevels == 1 &&
                             itA->second.kind == "texture" &&
                             itA->second.spriteClut == 0 &&
                             itA->second.frameNumber == 12);
        Check(matchA, "Loaded entry A metadata fields exactly match saved fields");

        // Add entry B and re-save
        TextureDumpInfo itemB;
        itemB.id = "256x32_e2819544";
        itemB.width = 256;
        itemB.height = 32;
        itemB.colorKey = false;
        itemB.mipLevels = 3;
        itemB.kind = "sprite";
        itemB.spriteClut = 5;
        itemB.frameNumber = 42;
        loadedEntries1.emplace(itemB.id, itemB);

        const bool save2Ok = SaveDumpManifest(testManPath, loadedEntries1);
        Check(save2Ok, "SaveDumpManifest successfully appends and writes updated manifest");

        std::unordered_map<std::string, TextureDumpInfo> loadedEntries2;
        std::unordered_set<std::string> loadedDumped2;
        const bool load2Ok = LoadDumpManifest(testManPath, loadedEntries2, loadedDumped2);
        Check(load2Ok, "LoadDumpManifest re-loads updated manifest");
        Check(loadedEntries2.size() == 2, "LoadDumpManifest parsed exactly 2 entries");
        Check(loadedDumped2.find("32x67_c1968fce") != loadedDumped2.end() &&
              loadedDumped2.find("256x32_e2819544") != loadedDumped2.end(),
              "Both entries A and B are present in dumped set");

        auto itB = loadedEntries2.find("256x32_e2819544");
        const bool matchB = (itB != loadedEntries2.end() &&
                             itB->second.width == 256 &&
                             itB->second.height == 32 &&
                             itB->second.colorKey == false &&
                             itB->second.mipLevels == 3 &&
                             itB->second.kind == "sprite" &&
                             itB->second.spriteClut == 5 &&
                             itB->second.frameNumber == 42);
        Check(matchB, "Loaded entry B metadata fields exactly match saved fields");

        DeleteFileW(testManPath.c_str());
    }

    // 11. Manifest round-trip with entries recovered from a disk scan (unknown color_key/kind).
    {
        const std::wstring manPath = MakeTestTempPath(L"rr95_manifest_unknown_") + L".json";
        DeleteFileW(manPath.c_str());
        std::unordered_map<std::string, TextureDumpInfo> entries;
        TextureDumpInfo known;
        known.id = "32x67_c1968fce";
        known.width = 32;
        known.height = 67;
        known.colorKey = true;
        known.kind = "sprite";
        entries.emplace(known.id, known);
        TextureDumpInfo scanned;
        scanned.id = "64x64_0badf00d";
        scanned.width = 64;
        scanned.height = 64;
        scanned.colorKeyKnown = false;
        scanned.kindKnown = false;
        entries.emplace(scanned.id, scanned);

        Check(SaveDumpManifest(manPath, entries), "SaveDumpManifest writes a manifest with a scanned entry");
        const std::string text = ReadWholeFile(manPath);
        Check(CountOccurrences(text, "\"color_key\"") == 1 && CountOccurrences(text, "\"kind\"") == 1,
              "SaveDumpManifest omits color_key and kind for scanned entries only");

        std::unordered_map<std::string, TextureDumpInfo> loaded;
        std::unordered_set<std::string> dumped;
        Check(LoadDumpManifest(manPath, loaded, dumped) && loaded.size() == 2,
              "LoadDumpManifest accepts entries without color_key and kind");
        auto itKnown = loaded.find(known.id);
        auto itScanned = loaded.find(scanned.id);
        Check(itKnown != loaded.end() && itKnown->second.colorKeyKnown && itKnown->second.colorKey &&
                  itKnown->second.kindKnown && itKnown->second.kind == "sprite",
              "LoadDumpManifest keeps known color_key and kind");
        Check(itScanned != loaded.end() && !itScanned->second.colorKeyKnown && !itScanned->second.kindKnown &&
                  itScanned->second.width == 64 && itScanned->second.height == 64,
              "LoadDumpManifest keeps missing color_key and kind unknown");

        // A second save keeps the scanned entry unknown.
        Check(SaveDumpManifest(manPath, loaded), "SaveDumpManifest re-writes the loaded manifest");
        const std::string text2 = ReadWholeFile(manPath);
        Check(CountOccurrences(text2, "\"color_key\"") == 1, "Scanned entry stays unknown after a round trip");
        DeleteFileW(manPath.c_str());
    }

    // 12. SaveDumpManifest keeps the old manifest and deletes the tmp file when the move fails.
    {
        const std::wstring manPath = MakeTestTempPath(L"rr95_manifest_isdir_") + L".json";
        RemoveDirectoryW(manPath.c_str());
        CreateDirectoryW(manPath.c_str(), nullptr);  // a directory cannot be replaced by a file
        std::unordered_map<std::string, TextureDumpInfo> entries;
        TextureDumpInfo e;
        e.id = "8x8_00000000";
        e.width = 8;
        e.height = 8;
        entries.emplace(e.id, e);
        const bool ok = SaveDumpManifest(manPath, entries);
        Check(!ok, "SaveDumpManifest returns false when MoveFileExW fails");
        Check(GetFileAttributesW((manPath + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES,
              "SaveDumpManifest deletes manifest.json.tmp after a failed move");
        const DWORD attr = GetFileAttributesW(manPath.c_str());
        Check(attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0,
              "SaveDumpManifest leaves the existing target untouched after a failed move");
        DeleteFileW((manPath + L".tmp").c_str());
        RemoveDirectoryW(manPath.c_str());
    }

    // 13. Only <w>x<h>_<crc8>.png names count as dumped texture files.
    {
        Check(IsTextureIdPngName(L"32x67_c1968fce.png"), "IsTextureIdPngName accepts 32x67_c1968fce.png");
        Check(IsTextureIdPngName(L"256x32_e2819544.PNG"), "IsTextureIdPngName accepts an upper-case extension");
        Check(!IsTextureIdPngName(L"stray.png"), "IsTextureIdPngName rejects stray.png");
        Check(!IsTextureIdPngName(L"32x67_C1968FCE.png"), "IsTextureIdPngName rejects upper-case hex");
        Check(!IsTextureIdPngName(L"32x67_c1968fc.png"), "IsTextureIdPngName rejects a 7-digit CRC");
        Check(!IsTextureIdPngName(L"32x67_c1968fce0.png"), "IsTextureIdPngName rejects a 9-digit CRC");
        Check(!IsTextureIdPngName(L"x67_c1968fce.png"), "IsTextureIdPngName rejects a missing width");
        Check(!IsTextureIdPngName(L"32x67_c1968fce.png.bak"), "IsTextureIdPngName rejects a trailing suffix");
    }

    // 14. The dumper adds scanned PNGs to the manifest as unknown and skips stray files.
    {
        const std::wstring dir = MakeTestTempPath(L"rr95_dump_scan_");
        CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring files[] = {dir + L"\\32x67_c1968fce.png", dir + L"\\64x64_0badf00d.PNG",
                                      dir + L"\\stray.png", dir + L"\\32x67_C1968FCE.png"};
        for (const auto& f : files) {
            WriteDummyFile(f);
        }
        uint8_t basePalette[1024] = {};
        FillTestBasePalette(basePalette);
        const std::wstring palPath = dir + L"\\PALETTE.RAW";
        WriteBytesFile(palPath, basePalette, sizeof(basePalette));
        TextureDumpInit(dir, palPath);
        TextureDumpShutdown();
        DeleteFileW(palPath.c_str());
        std::unordered_map<std::string, TextureDumpInfo> loaded;
        std::unordered_set<std::string> dumped;
        LoadDumpManifest(dir + L"\\manifest.json", loaded, dumped);
        auto it = loaded.find("32x67_c1968fce");
        Check(loaded.size() == 2 && loaded.count("64x64_0badf00d") == 1 && it != loaded.end(),
              "Dumper scan adds only files named <w>x<h>_<crc8>.png to the manifest");
        Check(it != loaded.end() && !it->second.colorKeyKnown && !it->second.kindKnown &&
                  it->second.width == 32 && it->second.height == 67,
              "Dumper scan records scanned files with unknown color_key and kind");
        for (const auto& f : files) {
            DeleteFileW(f.c_str());
        }
        DeleteFileW((dir + L"\\manifest.json").c_str());
        RemoveDirectoryW(dir.c_str());
    }

    // 15. Rejected replacements log the exact line; decode failures are told apart.
    {
        Check(FormatReplacementRejectLog("8x8_00000000", 24, 24, 8, 8) ==
                  "TexturePack: 8x8_00000000 rejected (24x24 is not a power-of-two multiple of 8x8)",
              "FormatReplacementRejectLog matches the brief's rejection line");
        const std::wstring png = MakeTestTempPath(L"rr95_reject_") + L".png";
        const uint32_t px[15] = {};
        SaveRgbaPngWithWic(png, 3, 5, px);
        int32_t w = 0, h = 0, k = 0;
        std::vector<uint32_t> pixels;
        HRESULT hr = S_OK;
        const PngLoadResult sizeResult = LoadRgbaPngWithWicEx(png, 2, 2, w, h, k, pixels, hr);
        Check(sizeResult == PngLoadResult::SizeRejected && w == 3 && h == 5 && pixels.empty(),
              "LoadRgbaPngWithWicEx reports a size rejection with the PNG size");
        const std::wstring bad = MakeTestTempPath(L"rr95_badpng_") + L".png";
        WriteDummyFile(bad);
        const PngLoadResult badResult = LoadRgbaPngWithWicEx(bad, 2, 2, w, h, k, pixels, hr);
        Check(badResult == PngLoadResult::DecodeFailed && FAILED(hr),
              "LoadRgbaPngWithWicEx reports a decode failure with its HRESULT");
        DeleteFileW(png.c_str());
        DeleteFileW(bad.c_str());
    }

    // 16. A pack generation change invalidates only entries whose replacement became ready.
    {
        struct Entry {
            int value = 0;
            std::string pendingReplacementId;
        };
        std::unordered_map<uint64_t, Entry> cache;
        cache[1] = Entry{1, ""};                    // no replacement expected
        cache[2] = Entry{2, "8x8_00000002"};        // replacement now ready
        cache[3] = Entry{3, "8x8_00000003"};        // still decoding
        cache[4] = Entry{4, "8x8_00000004"};        // rejected while pending
        auto stateOf = [](const std::string& id) {
            if (id == "8x8_00000002") return ReplacementState::Ready;
            if (id == "8x8_00000003") return ReplacementState::Pending;
            return ReplacementState::None;
        };
        const size_t erased = InvalidateReadyReplacementEntries(cache, stateOf);
        Check(erased == 1 && cache.size() == 3 && cache.count(2) == 0,
              "InvalidateReadyReplacementEntries erases only the entry whose replacement is ready");
        Check(cache.count(1) == 1 && cache[1].pendingReplacementId.empty(),
              "InvalidateReadyReplacementEntries keeps entries with no pending replacement");
        Check(cache.count(3) == 1 && cache[3].pendingReplacementId == "8x8_00000003",
              "InvalidateReadyReplacementEntries keeps still-pending entries marked");
        Check(cache.count(4) == 1 && cache[4].pendingReplacementId.empty(),
              "InvalidateReadyReplacementEntries clears the mark of entries that will never be replaced");
    }

    // PNG and JSON writer unit tests (B3 item 5).
    RunCaptureWritersTest();

    // 17. Dumps are converted with the base palette (DATA\PALETTE.RAW), never with the frame's
    // palette, so a texture first seen during the fade-in from black is not dumped black.
    {
        const std::wstring dir = MakeTestTempPath(L"rr95_dump_base_");
        CreateDirectoryW(dir.c_str(), nullptr);
        uint8_t basePalette[1024] = {};
        FillTestBasePalette(basePalette);
        const std::wstring palPath = dir + L"\\PALETTE.RAW";
        WriteBytesFile(palPath, basePalette, sizeof(basePalette));
        // The frame's palette at frame 1 of a race is all black (fade-in). The dumper has no
        // access to it: it converts only with the base palette loaded at Init.
        TextureDumpInit(dir, palPath);
        TextureDumpInfo info;
        info.id = "2x2_0000beef";
        info.width = 2;
        info.height = 2;
        info.colorKey = true;
        const uint8_t indices[4] = {0, 5, 200, 255};
        TextureDumpQueue(info, indices);
        for (int i = 0; i < 500 && TextureDumpGetDumpedCount() == 0; ++i) {
            Sleep(10);
        }
        TextureDumpShutdown();

        const std::wstring png = dir + L"\\2x2_0000beef.png";
        int32_t w = 0, h = 0, k = 0;
        std::vector<uint32_t> pixels;
        const bool loaded = LoadRgbaPngWithWic(png, 2, 2, w, h, k, pixels);
        Check(loaded && pixels.size() == 4 && pixels[1] == PackRgba(basePalette, 5) &&
                  pixels[2] == PackRgba(basePalette, 200) && pixels[3] == PackRgba(basePalette, 255),
              "Dump conversion uses the base palette and ignores a faded frame palette");
        Check(loaded && pixels.size() == 4 && pixels[0] == 0u,
              "Dump conversion keeps index 0 of colour-keyed textures transparent");
        DeleteFileW(png.c_str());
        DeleteFileW((dir + L"\\manifest.json").c_str());

        // A missing or wrongly sized palette disables dumping: nothing is queued or written.
        const std::wstring badPal = dir + L"\\SHORT.RAW";
        WriteBytesFile(badPal, basePalette, 1023);
        const std::wstring missingPal = dir + L"\\MISSING.RAW";
        const std::wstring palCases[2] = {missingPal, badPal};
        for (const std::wstring& pal : palCases) {
            TextureDumpInit(dir, pal);
            TextureDumpQueue(info, indices);
            Sleep(50);
            const uint32_t queued = TextureDumpGetQueuedCount();
            const uint32_t dumped = TextureDumpGetDumpedCount();
            TextureDumpShutdown();
            Check(queued == 0 && dumped == 0 &&
                      GetFileAttributesW(png.c_str()) == INVALID_FILE_ATTRIBUTES &&
                      GetFileAttributesW((dir + L"\\manifest.json").c_str()) == INVALID_FILE_ATTRIBUTES,
                  pal == missingPal ? "A missing PALETTE.RAW disables dumping"
                                    : "A PALETTE.RAW that is not 1024 bytes disables dumping");
            DeleteFileW(png.c_str());
            DeleteFileW((dir + L"\\manifest.json").c_str());
        }
        DeleteFileW(badPal.c_str());
        DeleteFileW(palPath.c_str());
        RemoveDirectoryW(dir.c_str());
    }

    // 18. Replacement fade factor: per channel sum(current[1..255]) / sum(base[1..255]), in [0, 1].
    {
        uint8_t base[1024] = {};
        FillTestBasePalette(base);
        float f[3] = {};

        ComputePaletteFadeFactor(base, base, f);
        Check(f[0] == 1.0f && f[1] == 1.0f && f[2] == 1.0f, "Fade factor of the base palette itself is 1");

        const uint8_t black[1024] = {};
        ComputePaletteFadeFactor(base, black, f);
        Check(f[0] == 0.0f && f[1] == 0.0f && f[2] == 0.0f, "Fade factor of an all-zero palette is 0");

        uint8_t half[1024] = {};
        for (int i = 0; i < 1024; ++i) {
            half[i] = static_cast<uint8_t>(base[i] / 2);
        }
        ComputePaletteFadeFactor(base, half, f);
        Check(std::fabs(f[0] - 0.5f) < 0.01f && std::fabs(f[1] - 0.5f) < 0.01f && std::fabs(f[2] - 0.5f) < 0.01f,
              "Fade factor of a half-brightness palette is about 0.5");
        Check(PaletteIsUniformScale(base, half, f), "A half-brightness palette is a uniform scale of the base");

        ComputePaletteFadeFactor(black, half, f);
        Check(f[0] == 1.0f && f[1] == 1.0f && f[2] == 1.0f, "Fade factor with an empty base palette is 1");
        ComputePaletteFadeFactor(nullptr, half, f);
        Check(f[0] == 1.0f && f[1] == 1.0f && f[2] == 1.0f, "Fade factor without a base palette is 1");

        uint8_t bright[1024] = {};
        for (int i = 0; i < 1024; ++i) {
            bright[i] = static_cast<uint8_t>(std::min(255, base[i] * 2));
        }
        ComputePaletteFadeFactor(base, bright, f);
        Check(f[0] == 1.0f && f[1] == 1.0f && f[2] == 1.0f, "Fade factor is clamped to 1");

        // Index 0 is excluded from the sums: a non-black index 0 does not change the factor.
        uint8_t blackWithIdx0[1024] = {};
        blackWithIdx0[0] = 255;
        blackWithIdx0[1] = 255;
        blackWithIdx0[2] = 255;
        ComputePaletteFadeFactor(base, blackWithIdx0, f);
        Check(f[0] == 0.0f && f[1] == 0.0f && f[2] == 0.0f, "Fade factor ignores palette index 0");

        // A remapped palette (red kept, green darkened, blue inverted) is not a uniform scale.
        uint8_t tinted[1024] = {};
        for (int i = 0; i < 256; ++i) {
            tinted[i * 4 + 0] = base[i * 4 + 0];
            tinted[i * 4 + 1] = static_cast<uint8_t>(base[i * 4 + 1] / 4);
            tinted[i * 4 + 2] = static_cast<uint8_t>(255 - base[i * 4 + 2]);
        }
        ComputePaletteFadeFactor(base, tinted, f);
        Check(!PaletteIsUniformScale(base, tinted, f), "A remapped palette is not a uniform scale of the base");
    }


    // 19. The original size is parsed from a texture ID with the IsTextureIdPngName rule.
    {
        int32_t w = -1, h = -1;
        Check(ParseTextureIdSize("32x67_c1968fce", w, h) && w == 32 && h == 67,
              "ParseTextureIdSize reads 32x67 from 32x67_c1968fce");
        Check(ParseTextureIdSize("256x32_e2819544", w, h) && w == 256 && h == 32,
              "ParseTextureIdSize reads 256x32 from 256x32_e2819544");
        Check(!ParseTextureIdSize("32x67_C1968FCE", w, h) && w == 0 && h == 0,
              "ParseTextureIdSize rejects upper-case CRC digits");
        Check(!ParseTextureIdSize("32x67_c1968fc", w, h), "ParseTextureIdSize rejects a 7-digit CRC");
        Check(!ParseTextureIdSize("32x67_c1968fce.png", w, h), "ParseTextureIdSize rejects an ID with an extension");
        Check(!ParseTextureIdSize("readme", w, h), "ParseTextureIdSize rejects a stray name");
        Check(!ParseTextureIdSize("0x8_00000000", w, h), "ParseTextureIdSize rejects a zero width");
        Check(!ParseTextureIdSize("8x0_00000000", w, h), "ParseTextureIdSize rejects a zero height");
        Check(!ParseTextureIdSize("99999999999x8_00000000", w, h) && !ParseTextureIdSize("8x8193_00000000", w, h),
              "ParseTextureIdSize rejects sizes above 8192");
    }

    // 20. On-demand decode requests are served before preload requests.
    {
        auto req = [](const char* id) {
            PackLoadRequest r;
            r.id = id;
            r.origW = 8;
            r.origH = 8;
            return r;
        };
        PackLoadQueue q;
        q.PushPreload(req("a"));
        q.PushPreload(req("b"));
        q.PushPreload(req("c"));
        q.PushDemand(req("d"));
        PackLoadRequest out;
        std::string order;
        while (q.PopNext(out, true)) {
            order += out.id;
        }
        Check(order == "dabc", "PackLoadQueue serves an on-demand request ahead of queued preload requests");

        q.PushPreload(req("a"));
        q.PushPreload(req("b"));
        q.PushPreload(req("c"));
        q.PushDemand(req("d"));
        Check(q.Promote("c"), "PackLoadQueue promotes a still-queued preload request");
        Check(!q.Promote("c") && !q.Promote("zz"),
              "PackLoadQueue does not promote an ID that is not waiting as a preload request");
        Check(q.DemandSize() == 2 && q.PreloadSize() == 2, "Promotion moves the request between the queues");
        order.clear();
        while (q.PopNext(out, true)) {
            order += out.id;
        }
        Check(order == "dcab", "A promoted preload request jumps ahead of the remaining preload requests");

        q.PushPreload(req("a"));
        Check(!q.PopNext(out, false) && q.PreloadSize() == 1,
              "PackLoadQueue holds preload requests back when preload is not allowed");
        q.PushPreload(req("b"));
        q.PushDemand(req("d"));
        const std::vector<std::string> dropped = q.ClearPreload();
        Check(dropped.size() == 2 && dropped[0] == "a" && dropped[1] == "b" && !q.HasPreload() && q.HasDemand(),
              "ClearPreload drops only the preload requests and returns their IDs");
        q.Clear();
        Check(!q.HasDemand() && !q.HasPreload() && !q.PopNext(out, true), "Clear empties both queues");
    }

    // 21. Init queues every texture-ID file with Preload=1 and nothing with Preload=0.
    {
        const std::wstring dir = MakeTestTempPath(L"rr95_preload_");
        CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring f1 = dir + L"\\32x67_c1968fce.png";
        const std::wstring f2 = dir + L"\\256x32_e2819544.png";
        const std::wstring stray = dir + L"\\notes.png";
        WriteDummyFile(f1);
        WriteDummyFile(f2);
        WriteDummyFile(stray);

        // Without a device TexturePackUpdate never runs, so queued IDs stay pending.
        TexturePackInit(dir, 512, 4, false);
        TexturePackStats stats = TexturePackGetStats();
        Check(TexturePackGetIndexedCount() == 3 && stats.pendingCount == 0,
              "Preload=0 indexes the pack and queues nothing at Init");
        Check(TexturePackGetState("32x67_c1968fce") == ReplacementState::None,
              "Preload=0 leaves an indexed ID idle until it is drawn");
        TexturePackShutdown();

        TexturePackInit(dir, 512, 4, true);
        stats = TexturePackGetStats();
        Check(stats.pendingCount == 2 && TexturePackGetState("32x67_c1968fce") == ReplacementState::Pending &&
                  TexturePackGetState("256x32_e2819544") == ReplacementState::Pending,
              "Preload=1 queues every texture-ID file at Init");
        Check(TexturePackGetState("notes") == ReplacementState::None,
              "Preload=1 skips files whose name is not a texture ID");
        TexturePackShutdown();
        Check(TexturePackGetStats().pendingCount == 0, "Shutdown drops the preload queue");

        DeleteFileW(f1.c_str());
        DeleteFileW(f2.c_str());
        DeleteFileW(stray.c_str());
        RemoveDirectoryW(dir.c_str());
    }

    // 22. A preloaded replacement ready before first use does not bump the pack generation, while
    // one that was drawn as the original first does (WARP device).
    {
        ComPtr<ID3D11Device> device;
        const HRESULT hrDev = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                                D3D11_SDK_VERSION, &device, nullptr, nullptr);
        if (FAILED(hrDev) || !device) {
            std::printf("[SKIP] WARP device unavailable (0x%08lX); pack generation test skipped\n",
                        static_cast<unsigned long>(hrDev));
        } else {
            const std::wstring dir = MakeTestTempPath(L"rr95_pregen_");
            CreateDirectoryW(dir.c_str(), nullptr);
            std::vector<uint32_t> px(8 * 8, 0xFF336699u);
            const std::wstring pngA = dir + L"\\8x8_0000000a.png";
            const std::wstring pngB = dir + L"\\8x8_0000000b.png";
            SaveRgbaPngWithWic(pngA, 8, 8, px.data());
            SaveRgbaPngWithWic(pngB, 16, 4, px.data());  // not a k multiple of 8x8: rejected

            auto pumpUntil = [&](uint32_t loaded, uint32_t rejected) {
                const ULONGLONG deadline = GetTickCount64() + 10000;
                TexturePackStats st = TexturePackGetStats();
                while (GetTickCount64() < deadline) {
                    TexturePackUpdate(device.Get());
                    st = TexturePackGetStats();
                    if (st.loadedCount >= loaded && st.rejectedCount >= rejected && st.pendingCount == 0) {
                        break;
                    }
                    Sleep(2);
                }
                return st;
            };

            TexturePackInit(dir, 512, 4, true);
            const uint64_t genBefore = TexturePackGetGeneration();
            TexturePackStats st = pumpUntil(1, 1);
            Check(st.loadedCount == 1 && st.rejectedCount == 1 && st.pendingCount == 0,
                  "Preload loads the valid replacement and rejects the bad size without a draw");
            Check(TexturePackGetState("8x8_0000000a") == ReplacementState::Ready,
                  "A preloaded replacement is ready before its first use");
            Check(TexturePackGetGeneration() == genBefore,
                  "A replacement ready before first use does not bump the pack generation");
            bool pending = true;
            ReplacementTexture* rep = TexturePackGetOrQueue("8x8_0000000a", 8, 8, &pending);
            Check(rep && rep->srv && rep->k == 1 && !pending, "The first draw gets the preloaded replacement");
            TexturePackShutdown();

            TexturePackInit(dir, 512, 4, false);
            const uint64_t genLazy = TexturePackGetGeneration();
            pending = false;
            rep = TexturePackGetOrQueue("8x8_0000000a", 8, 8, &pending);
            Check(!rep && pending, "Preload=0 queues the replacement on its first draw");
            st = pumpUntil(1, 0);
            Check(st.loadedCount == 1 && TexturePackGetGeneration() > genLazy,
                  "A replacement drawn as the original first bumps the pack generation");
            TexturePackShutdown();

            DeleteFileW(pngA.c_str());
            DeleteFileW(pngB.c_str());
            RemoveDirectoryW(dir.c_str());
        }
    }

    std::printf("%s (%d failure(s))\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
