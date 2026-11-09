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

void Check(bool condition, const char* what) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", what);
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
              !cfg.texturePackDump && cfg.texturePackDumpPath == L"texture-dump",
              "[TexturePack] defaults match brief (Enabled=1, Path=texture-pack, MaxMiB=512, Budget=4, Dump=0, DumpPath=texture-dump)");
        std::wstring absPath = ResolveGamePath(L"texture-pack");
        Check(!absPath.empty() && absPath.find(L"texture-pack") != std::wstring::npos,
              "ResolveGamePath resolves relative pack path");
    }

    // PNG and JSON writer unit tests (B3 item 5).
    RunCaptureWritersTest();

    std::printf("%s (%d failure(s))\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
