// hook_test: exercises the inline hook engine in-process on a naked x86
// function that starts with the same prologue as FUN_0046e630
// (push ebp / mov ebp,esp / sub esp,42Ch) and uses the same __fastcall ABI,
// plus unit tests for ParseFilterMode, ComputeLetterboxViewport,
// PresenterFailureState time-windowed latch/backoff, WriteIndexedPng,
// and WriteFrameJsonFromQuads.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/hook.h"
#include "../src/config.h"
#include "../src/presenter.h"
#include "../src/capture.h"

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

    // 6. ParseVirtualKey tests (Defect 4 hotkey configuration)
    {
        Check(ParseVirtualKey(L"F12", 0x7B) == 0x7B, "ParseVirtualKey resolves F12 to 0x7B");
        Check(ParseVirtualKey(L"f12", 0x7B) == 0x7B, "ParseVirtualKey case-insensitive f12");
        Check(ParseVirtualKey(L"F10", 0x7B) == 0x79, "ParseVirtualKey resolves F10 to 0x79");
        Check(ParseVirtualKey(L"0x7B", 0) == 0x7B, "ParseVirtualKey resolves hex 0x7B");
        Check(ParseVirtualKey(L"123", 0) == 123, "ParseVirtualKey resolves decimal 123");
        Check(ParseVirtualKey(L"invalid", 0x7B) == 0x7B, "ParseVirtualKey falls back to default on invalid key");
    }

    // 7. Engine scanline walk vs CPU shader math vs Two-triangle interpolation (Defect 1)
    {
        struct TestPoint { float x, y; };
        struct TestUv { float u, v; };
        struct TestQuad { TestPoint p[4]; TestUv uv[4]; };

        auto CpuScanlineWalk = [](const TestQuad& q, float curX, float curY) -> TestUv {
            float ymin = std::min(std::min(q.p[0].y, q.p[1].y), std::min(q.p[2].y, q.p[3].y));
            float ymax = std::max(std::max(q.p[0].y, q.p[1].y), std::max(q.p[2].y, q.p[3].y));
            float yC = std::max(ymin, std::min(ymax, curY));

            float xL = 0.0f, xR = 0.0f;
            TestUv uvL = {0.0f, 0.0f}, uvR = {0.0f, 0.0f};
            bool foundL = false, foundR = false;

            for (int i = 0; i < 4; ++i) {
                int next = (i + 1) & 3;
                TestPoint p0 = q.p[i];
                TestPoint p1 = q.p[next];
                TestUv uv0 = q.uv[i];
                TestUv uv1 = q.uv[next];

                if (p1.y > p0.y) {
                    if (yC >= p0.y && yC <= p1.y && !foundR) {
                        float s = (yC - p0.y) / (p1.y - p0.y);
                        xR = p0.x + (p1.x - p0.x) * s;
                        uvR.u = uv0.u + (uv1.u - uv0.u) * s;
                        uvR.v = uv0.v + (uv1.v - uv0.v) * s;
                        foundR = true;
                    }
                } else if (p1.y < p0.y) {
                    if (yC >= p1.y && yC <= p0.y && !foundL) {
                        float s = (yC - p1.y) / (p0.y - p1.y);
                        xL = p1.x + (p0.x - p1.x) * s;
                        uvL.u = uv1.u + (uv0.u - uv1.u) * s;
                        uvL.v = uv1.v + (uv0.v - uv1.v) * s;
                        foundL = true;
                    }
                }
            }
            if (!foundL) {
                xL = std::min(std::min(q.p[0].x, q.p[1].x), std::min(q.p[2].x, q.p[3].x));
                uvL = q.uv[0];
            }
            if (!foundR) {
                xR = std::max(std::max(q.p[0].x, q.p[1].x), std::max(q.p[2].x, q.p[3].x));
                uvR = q.uv[1];
            }
            float span = std::max(xR - xL, 1e-4f);
            float t = std::max(0.0f, std::min(1.0f, (curX - xL) / span));
            return { uvL.u + (uvR.u - uvL.u) * t, uvL.v + (uvR.v - uvL.v) * t };
        };

        auto CpuShaderMath = [](const TestQuad& q, float curX, float curY) -> TestUv {
            float ymin = std::min(std::min(q.p[0].y, q.p[1].y), std::min(q.p[2].y, q.p[3].y));
            float ymax = std::max(std::max(q.p[0].y, q.p[1].y), std::max(q.p[2].y, q.p[3].y));
            float yClamped = std::max(ymin, std::min(ymax, curY));

            float xL = 0.0f, xR = 0.0f;
            TestUv uvL = {0.0f, 0.0f}, uvR = {0.0f, 0.0f};
            bool foundL = false, foundR = false;

            for (int i = 0; i < 4; ++i) {
                int next = (i + 1) & 3;
                TestPoint p0 = q.p[i];
                TestPoint p1 = q.p[next];
                TestUv uv0 = q.uv[i];
                TestUv uv1 = q.uv[next];

                if (p1.y > p0.y) {
                    if (yClamped >= p0.y && yClamped <= p1.y && !foundR) {
                        float s = (yClamped - p0.y) / (p1.y - p0.y);
                        xR = p0.x + (p1.x - p0.x) * s;
                        uvR.u = uv0.u + (uv1.u - uv0.u) * s;
                        uvR.v = uv0.v + (uv1.v - uv0.v) * s;
                        foundR = true;
                    }
                } else if (p1.y < p0.y) {
                    if (yClamped >= p1.y && yClamped <= p0.y && !foundL) {
                        float s = (yClamped - p1.y) / (p0.y - p1.y);
                        xL = p1.x + (p0.x - p1.x) * s;
                        uvL.u = uv1.u + (uv0.u - uv1.u) * s;
                        uvL.v = uv1.v + (uv0.v - uv1.v) * s;
                        foundL = true;
                    }
                }
            }

            if (!foundL) {
                xL = std::min(std::min(q.p[0].x, q.p[1].x), std::min(q.p[2].x, q.p[3].x));
                uvL = q.uv[0];
            }
            if (!foundR) {
                xR = std::max(std::max(q.p[0].x, q.p[1].x), std::max(q.p[2].x, q.p[3].x));
                uvR = q.uv[1];
            }

            float span = std::max(xR - xL, 1e-4f);
            float t = std::max(0.0f, std::min(1.0f, (curX - xL) / span));
            return { uvL.u + (uvR.u - uvL.u) * t, uvL.v + (uvR.v - uvL.v) * t };
        };

        auto BarycentricUv = [](float px, float py,
                               TestPoint p0, TestPoint p1, TestPoint p2,
                               TestUv uv0, TestUv uv1, TestUv uv2,
                               TestUv& outUv) -> bool {
            float denom = (p1.y - p2.y) * (p0.x - p2.x) + (p2.x - p1.x) * (p0.y - p2.y);
            if (std::abs(denom) < 1e-6f) return false;
            float w0 = ((p1.y - p2.y) * (px - p2.x) + (p2.x - p1.x) * (py - p2.y)) / denom;
            float w1 = ((p2.y - p0.y) * (px - p2.x) + (p0.x - p2.x) * (py - p2.y)) / denom;
            float w2 = 1.0f - w0 - w1;
            if (w0 >= -1e-4f && w1 >= -1e-4f && w2 >= -1e-4f) {
                outUv.u = w0 * uv0.u + w1 * uv1.u + w2 * uv2.u;
                outUv.v = w0 * uv0.v + w1 * uv1.v + w2 * uv2.v;
                return true;
            }
            return false;
        };

        auto TwoTrianglesUv = [&](const TestQuad& q, float px, float py, TestUv& outUv) -> bool {
            if (BarycentricUv(px, py, q.p[0], q.p[1], q.p[2], q.uv[0], q.uv[1], q.uv[2], outUv)) {
                return true;
            }
            if (BarycentricUv(px, py, q.p[0], q.p[2], q.p[3], q.uv[0], q.uv[2], q.uv[3], outUv)) {
                return true;
            }
            return false;
        };

        // 1. Road trapezoid
        const TestQuad roadTrapezoid = {
            {{280.0f, 100.0f}, {360.0f, 100.0f}, {560.0f, 300.0f}, {80.0f, 300.0f}},
            {{0.0f, 0.0f}, {64.0f, 0.0f}, {64.0f, 256.0f}, {0.0f, 256.0f}}
        };

        // 2. Strongly non-planar skew
        const TestQuad skewQuad = {
            {{200.0f, 80.0f}, {450.0f, 120.0f}, {520.0f, 380.0f}, {120.0f, 310.0f}},
            {{0.0f, 0.0f}, {256.0f, 0.0f}, {256.0f, 256.0f}, {0.0f, 256.0f}}
        };

        // 3. Sprite rectangle
        const TestQuad spriteRect = {
            {{150.0f, 200.0f}, {350.0f, 200.0f}, {350.0f, 420.0f}, {150.0f, 420.0f}},
            {{0.0f, 0.0f}, {128.0f, 0.0f}, {128.0f, 128.0f}, {0.0f, 128.0f}}
        };

        auto TestQuadDiff = [&](const TestQuad& q, float& outMaxShaderDiff, float& outMaxTriDiff) {
            outMaxShaderDiff = 0.0f;
            outMaxTriDiff = 0.0f;
            float ymin = std::min(std::min(q.p[0].y, q.p[1].y), std::min(q.p[2].y, q.p[3].y));
            float ymax = std::max(std::max(q.p[0].y, q.p[1].y), std::max(q.p[2].y, q.p[3].y));
            for (int y = static_cast<int>(ymin); y <= static_cast<int>(ymax); ++y) {
                float py = static_cast<float>(y) + 0.5f;
                for (int x = 0; x < 640; ++x) {
                    float px = static_cast<float>(x) + 0.5f;
                    TestUv uvTri = {};
                    if (!TwoTrianglesUv(q, px, py, uvTri)) {
                        continue; // outside quad coverage
                    }
                    TestUv uvWalk = CpuScanlineWalk(q, px, py);
                    TestUv uvShader = CpuShaderMath(q, px, py);

                    float shaderDiff = std::max(std::abs(uvWalk.u - uvShader.u), std::abs(uvWalk.v - uvShader.v));
                    outMaxShaderDiff = std::max(outMaxShaderDiff, shaderDiff);

                    float triDiff = std::max(std::abs(uvWalk.u - uvTri.u), std::abs(uvWalk.v - uvTri.v));
                    outMaxTriDiff = std::max(outMaxTriDiff, triDiff);
                }
            }
        };

        float maxShaderDiff = 0.0f, maxTriDiff = 0.0f;

        // Test road trapezoid
        TestQuadDiff(roadTrapezoid, maxShaderDiff, maxTriDiff);
        Check(maxShaderDiff <= 0.0625f, "Road trapezoid: scanline walk vs shader math error <= 1/16 texel");
        Check(maxTriDiff > 25.0f, "Road trapezoid: two-triangle interpolation error > 25 texels (proves defect 1 root cause)");

        // Test non-planar skew
        TestQuadDiff(skewQuad, maxShaderDiff, maxTriDiff);
        Check(maxShaderDiff <= 0.0625f, "Non-planar skew: scanline walk vs shader math error <= 1/16 texel");
        Check(maxTriDiff > 30.0f, "Non-planar skew: two-triangle interpolation error > 30 texels (proves defect 1 root cause)");

        // Test sprite rectangle
        TestQuadDiff(spriteRect, maxShaderDiff, maxTriDiff);
        Check(maxShaderDiff <= 0.0625f, "Sprite rectangle: scanline walk vs shader math error <= 1/16 texel");
        Check(maxTriDiff <= 0.0625f, "Sprite rectangle: two-triangle affine mapping matches rectangle scanlines");
    }

    // PNG and JSON writer unit tests (B3 item 5).
    RunCaptureWritersTest();

    std::printf("%s (%d failure(s))\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
