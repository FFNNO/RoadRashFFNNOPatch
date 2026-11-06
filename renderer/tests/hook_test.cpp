// hook_test: exercises the inline hook engine in-process on a naked x86
// function that starts with the same prologue as FUN_0046e630
// (push ebp / mov ebp,esp / sub esp,42Ch) and uses the same __fastcall ABI,
// plus pure unit tests for ParseFilterMode and ComputeLetterboxViewport.
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "../src/hook.h"
#include "../src/config.h"
#include "../src/presenter.h"

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

    std::printf("%s (%d failure(s))\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
