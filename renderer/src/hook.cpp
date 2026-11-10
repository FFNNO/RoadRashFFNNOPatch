#include "hook.h"
#include "log.h"
#include <cstring>

namespace rr95 {

namespace {

bool IsLegacyPrefix(uint8_t b) {
    switch (b) {
        case 0x66:  // Operand-size override
        case 0x67:  // Address-size override
        case 0xF0:  // LOCK
        case 0xF2:  // REPNE/REPNZ
        case 0xF3:  // REP/REPE/REPZ
        case 0x26:  // ES segment override
        case 0x2E:  // CS segment override
        case 0x36:  // SS segment override
        case 0x3E:  // DS segment override
        case 0x64:  // FS segment override
        case 0x65:  // GS segment override
            return true;
        default:
            return false;
    }
}

}  // namespace

void BuildRelocatedExpected(const HookSite& site, intptr_t delta, uint8_t out[16]) {
    std::memcpy(out, site.expected, sizeof(site.expected));
    for (size_t offset : site.absoluteOperandOffsets) {
        if (offset + sizeof(uint32_t) > sizeof(site.expected)) {
            continue;
        }
        uint32_t value = 0;
        std::memcpy(&value, out + offset, sizeof(value));
        value = static_cast<uint32_t>(static_cast<intptr_t>(value) + delta);
        std::memcpy(out + offset, &value, sizeof(value));
    }
}

namespace {

intptr_t RebaseDelta(HMODULE moduleBase) {
    if (moduleBase == nullptr) {
        return 0;
    }
    return static_cast<intptr_t>(reinterpret_cast<uintptr_t>(moduleBase)) -
           static_cast<intptr_t>(kPreferredImageBase);
}

}  // namespace

const char* ValidateHookDecoding(const HookSite& site) {
    if (site.spanLength < 5) {
        return "span shorter than 5 bytes is refused";
    }
    if (site.spanLength > sizeof(site.expected)) {
        return "span exceeds expected buffer size";
    }

    size_t totalLen = 0;
    for (size_t len : site.instructionLengths) {
        if (len == 0) {
            return "zero-length instruction inside the span";
        }
        totalLen += len;
    }
    if (totalLen != site.spanLength) {
        return "instruction lengths not adding up to the span are refused";
    }

    for (size_t offset : site.absoluteOperandOffsets) {
        if (offset + sizeof(uint32_t) > site.spanLength) {
            return "absolute operand outside the span is refused";
        }
    }

    size_t offset = 0;
    for (size_t len : site.instructionLengths) {
        const uint8_t* insn = site.expected + offset;
        size_t p = 0;
        while (p < len && IsLegacyPrefix(insn[p])) {
            ++p;
        }
        if (p == len) {
            return "instruction consists solely of prefixes";
        }

        const uint8_t op = insn[p];
        const size_t rem = len - p;

        // E8: CALL rel32
        // E9: JMP rel32
        // EB: JMP rel8
        // E0..E2: LOOPNZ, LOOPZ, LOOP
        // E3: JCXZ / JECXZ
        if (op == 0xE8 || op == 0xE9 || op == 0xEB || (op >= 0xE0 && op <= 0xE3)) {
            return "relative instruction inside the span is refused";
        }

        // 70..7F: Jcc rel8
        if (op >= 0x70 && op <= 0x7F) {
            return "relative instruction (jcc rel8) inside the span is refused";
        }

        // 0F 80..0F 8F: Jcc rel32
        if (op == 0x0F && rem >= 2) {
            const uint8_t op2 = insn[p + 1];
            if (op2 >= 0x80 && op2 <= 0x8F) {
                return "jcc rel32 inside the span is refused";
            }
        }

        offset += len;
    }

    return nullptr;
}

bool InstallHook(HookSite& site, HMODULE moduleBase) {
    if (site.installed) {
        return true;
    }

    const char* validationError = ValidateHookDecoding(site);
    if (validationError != nullptr) {
        LOG_ERROR("Hook '%s' decoding validation failed: %s",
                  site.name ? site.name : "<unnamed>", validationError);
        return false;
    }

    uintptr_t targetAddr = 0;
    if (moduleBase != nullptr) {
        targetAddr = reinterpret_cast<uintptr_t>(moduleBase) + (site.preferredVa - kPreferredImageBase);
    } else {
        targetAddr = site.preferredVa;
    }

    void* target = reinterpret_cast<void*>(targetAddr);
    uint8_t liveExpected[sizeof(site.expected)];
    BuildRelocatedExpected(site, RebaseDelta(moduleBase), liveExpected);
    if (std::memcmp(target, liveExpected, site.spanLength) != 0) {
        LOG_ERROR("Hook '%s' target %p byte mismatch: memory does not match expected bytes",
                  site.name ? site.name : "<unnamed>", target);
        return false;
    }

    const size_t trampolineSize = site.spanLength + 5;
    void* trampoline = VirtualAlloc(nullptr, trampolineSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) {
        LOG_ERROR("VirtualAlloc failed for trampoline of hook '%s' (error: %lu)",
                  site.name ? site.name : "<unnamed>", GetLastError());
        return false;
    }

    std::memcpy(trampoline, target, site.spanLength);

    uint8_t* trampJmp = reinterpret_cast<uint8_t*>(trampoline) + site.spanLength;
    *trampJmp = 0xE9; // JMP rel32
    const uintptr_t resumeAddr = targetAddr + site.spanLength;
    const uintptr_t trampNextIp = reinterpret_cast<uintptr_t>(trampJmp) + 5;
    const int32_t trampRel = static_cast<int32_t>(resumeAddr - trampNextIp);
    std::memcpy(trampJmp + 1, &trampRel, sizeof(trampRel));

    DWORD oldProtect = 0;
    VirtualProtect(trampoline, trampolineSize, PAGE_EXECUTE_READ, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), trampoline, trampolineSize);

    if (!VirtualProtect(target, site.spanLength, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LOG_ERROR("VirtualProtect failed on target %p (error: %lu)", target, GetLastError());
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    uint8_t* targetBytes = reinterpret_cast<uint8_t*>(target);
    *targetBytes = 0xE9; // JMP rel32
    const uintptr_t targetNextIp = targetAddr + 5;
    const int32_t detourRel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(site.detour) - targetNextIp);
    std::memcpy(targetBytes + 1, &detourRel, sizeof(detourRel));

    if (site.spanLength > 5) {
        std::memset(targetBytes + 5, 0x90, site.spanLength - 5);
    }

    VirtualProtect(target, site.spanLength, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, site.spanLength);

    if (site.originalTrampoline) {
        *site.originalTrampoline = trampoline;
    }
    site.installed = true;

    LOG_INFO("Hook '%s' installed at %p -> %p (trampoline: %p)",
             site.name ? site.name : "<unnamed>", target, site.detour, trampoline);
    return true;
}

void RemoveHook(HookSite& site, HMODULE moduleBase) {
    if (!site.installed) {
        return;
    }

    uintptr_t targetAddr = 0;
    if (moduleBase != nullptr) {
        targetAddr = reinterpret_cast<uintptr_t>(moduleBase) + (site.preferredVa - kPreferredImageBase);
    } else {
        targetAddr = site.preferredVa;
    }

    void* target = reinterpret_cast<void*>(targetAddr);
    DWORD oldProtect = 0;
    uint8_t liveExpected[sizeof(site.expected)];
    BuildRelocatedExpected(site, RebaseDelta(moduleBase), liveExpected);
    if (VirtualProtect(target, site.spanLength, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        std::memcpy(target, liveExpected, site.spanLength);
        VirtualProtect(target, site.spanLength, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), target, site.spanLength);
    }

    if (site.originalTrampoline && *site.originalTrampoline) {
        VirtualFree(*site.originalTrampoline, 0, MEM_RELEASE);
        *site.originalTrampoline = nullptr;
    }

    site.installed = false;
    LOG_INFO("Hook '%s' removed from %p", site.name ? site.name : "<unnamed>", target);
}

void** FindIatSlot(HMODULE moduleBase, const char* dllName, const char* functionName) {
    if (!moduleBase || !dllName || !functionName) {
        return nullptr;
    }
    const auto* base = reinterpret_cast<const uint8_t*>(moduleBase);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return nullptr;
    }
    const IMAGE_DATA_DIRECTORY& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.VirtualAddress == 0 || importDir.Size == 0) {
        return nullptr;
    }

    const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
    for (; desc->Name != 0; ++desc) {
        const char* name = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(name, dllName) != 0) {
            continue;
        }
        const DWORD origFirstThunkRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        const auto* origThunk = reinterpret_cast<const IMAGE_THUNK_DATA32*>(base + origFirstThunkRva);
        auto* iatThunk = reinterpret_cast<IMAGE_THUNK_DATA32*>(const_cast<uint8_t*>(base) + desc->FirstThunk);
        for (; origThunk->u1.AddressOfData != 0; ++origThunk, ++iatThunk) {
            if (IMAGE_SNAP_BY_ORDINAL32(origThunk->u1.Ordinal)) {
                continue;
            }
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + origThunk->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), functionName) == 0) {
                return reinterpret_cast<void**>(&iatThunk->u1.Function);
            }
        }
    }
    return nullptr;
}

bool InstallIatHook(IatHook& hook, HMODULE moduleBase) {
    if (hook.slot != nullptr || !hook.detour || !hook.original) {
        return false;
    }
    void** slot = FindIatSlot(moduleBase, hook.dllName, hook.functionName);
    if (!slot) {
        return false;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        return false;
    }
    *hook.original = *slot;
    *slot = hook.detour;
    VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
    hook.slot = slot;
    LOG_INFO("IAT hook on %s!%s installed (slot %p)", hook.dllName, hook.functionName, slot);
    return true;
}

void RemoveIatHook(IatHook& hook) {
    if (hook.slot && hook.original && *hook.original) {
        DWORD oldProtect = 0;
        if (VirtualProtect(hook.slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            *hook.slot = *hook.original;
            VirtualProtect(hook.slot, sizeof(void*), oldProtect, &oldProtect);
        }
    }
    hook.slot = nullptr;
    if (hook.original) {
        *hook.original = nullptr;
    }
}

}  // namespace rr95
