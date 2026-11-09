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

// Import address table hook on one by-name import (dllName!functionName) of a module.
struct IatHook {
    const char* dllName;       // compared case-insensitively, e.g. "COMDLG32.dll"
    const char* functionName;  // e.g. "GetSaveFileNameA"
    void* detour;
    void** original;           // receives the previous slot value on install
    void** slot;               // patched IAT slot while installed, else nullptr
};

// Returns the IAT slot of dllName!functionName in moduleBase, or nullptr when it is not imported
// by name.
void** FindIatSlot(HMODULE moduleBase, const char* dllName, const char* functionName);
bool InstallIatHook(IatHook& hook, HMODULE moduleBase);
// Writes the saved original back into the slot.
void RemoveIatHook(IatHook& hook);

}  // namespace rr95
