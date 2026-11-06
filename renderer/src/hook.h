#pragma once
#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace rr95 {

constexpr uint32_t kPreferredImageBase = 0x00400000;

struct HookSite {
    const char* name;
    uint32_t preferredVa;
    uint8_t expected[16];
    size_t spanLength;
    std::vector<size_t> instructionLengths;
    void* detour;
    void** originalTrampoline;
    bool installed;
};

const char* ValidateHookDecoding(const HookSite& site);
bool InstallHook(HookSite& site, HMODULE moduleBase = nullptr);
void RemoveHook(HookSite& site, HMODULE moduleBase = nullptr);

}  // namespace rr95
