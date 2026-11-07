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

    // Time-windowed presenter failure latch & backoff unit tests (B3 item 6).
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

        // Failure window expiration (> 2000ms since first failure resets count to 1).
        Check(!PresenterRecordFailure(fs, 3000) && fs.failureCount == 1,
              "failure at t=3000 starts a new window");
        Check(!PresenterRecordFailure(fs, 4000) && fs.failureCount == 2 && !fs.latchedOff,
              "second failure at t=4000 increments count to 2");
        Check(!PresenterRecordFailure(fs, 6000) && fs.failureCount == 1 && !fs.latchedOff,
              "failure at t=6000 (>2000ms after firstFailureMs=3000) resets window to count=1");

        // 3 failures within 2000ms window (t=6000, t=7000, t=8000) latches off permanently.
        Check(!PresenterRecordFailure(fs, 7000) && fs.failureCount == 2 && !fs.latchedOff,
              "second failure in window at t=7000 sets count=2");
        Check(PresenterRecordFailure(fs, 8000) && fs.failureCount == 3 && fs.latchedOff,
              "third failure in 2s window at t=8000 latches presenter off");
        Check(!PresenterCanAttempt(fs, 20000),
              "latched presenter stays off even after long delay");
    }

    // PNG and JSON writer unit tests (B3 item 5).
    RunCaptureWritersTest();

    std::printf("%s (%d failure(s))\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
