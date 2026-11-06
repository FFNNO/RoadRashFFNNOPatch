#include "hook.h"
#include "log.h"
#include <cstring>

namespace rr95 {

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

    size_t offset = 0;
    for (size_t len : site.instructionLengths) {
        const uint8_t* insn = site.expected + offset;
        const uint8_t op = insn[0];

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
        if (op == 0x0F && len >= 2) {
            const uint8_t op2 = insn[1];
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
    if (std::memcmp(target, site.expected, site.spanLength) != 0) {
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
    if (VirtualProtect(target, site.spanLength, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        std::memcpy(target, site.expected, site.spanLength);
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

}  // namespace rr95
