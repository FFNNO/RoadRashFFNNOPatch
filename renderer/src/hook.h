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
    // Offsets inside expected[] of 4-byte absolute addresses (e.g. A0 moffs32), written as preferred
    // VAs. The loader relocates them when the image is rebased (dynamic base), so InstallHook adds
    // the rebase delta before comparing, the trampoline copies the relocated bytes, and RemoveHook
    // restores them.
    std::vector<size_t> absoluteOperandOffsets = {};
};

// expected[] of site with every absolute operand rebased by delta (module base - preferred base).
void BuildRelocatedExpected(const HookSite& site, intptr_t delta, uint8_t out[16]);

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
