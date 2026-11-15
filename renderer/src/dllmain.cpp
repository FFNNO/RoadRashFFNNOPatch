#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <ddraw.h>
#include <mutex>
#include <string>
#include "log.h"
#include "config.h"
#include "exe_identity.h"
#include "hook_sites.h"
#include "capture.h"
#include "mod_api.h"

static HMODULE g_hModule = nullptr;
static HMODULE g_systemDDraw = nullptr;
using DirectDrawCreateFn = HRESULT(WINAPI*)(GUID*, LPDIRECTDRAW*, IUnknown*);
static DirectDrawCreateFn g_realDirectDrawCreate = nullptr;

static std::wstring GetModuleDir(HMODULE hMod) {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(hMod, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return L"";
    }
    std::wstring s(path);
    size_t slash = s.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        return s.substr(0, slash);
    }
    return L"";
}

// Crash diagnostics: an access violation in the game or in this DLL is written to the log
// (address, registers, likely return addresses on the stack) before the process dies, so a
// player's rr95gpu.log says where it crashed. Observation only: the exception keeps going.
static uintptr_t ModuleEnd(HMODULE mod) {
    const auto base = reinterpret_cast<uintptr_t>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return base + nt->OptionalHeader.SizeOfImage;
}

static uintptr_t g_exeBase = 0;
static uintptr_t g_exeEnd = 0;
static uintptr_t g_selfBase = 0;
static uintptr_t g_selfEnd = 0;

// "exe+0x655b7" / "ddraw+0x15f60" for addresses in either module, empty otherwise.
static bool DescribeCodeAddress(uintptr_t addr, char* out, size_t outLen) {
    if (addr >= g_exeBase && addr < g_exeEnd) {
        std::snprintf(out, outLen, "exe+0x%x (VA 0x%08x)", static_cast<unsigned>(addr - g_exeBase),
                      static_cast<unsigned>(addr - g_exeBase + 0x400000u));
        return true;
    }
    if (addr >= g_selfBase && addr < g_selfEnd) {
        std::snprintf(out, outLen, "ddraw+0x%x", static_cast<unsigned>(addr - g_selfBase));
        return true;
    }
    return false;
}

static LONG CALLBACK CrashLogHandler(EXCEPTION_POINTERS* info) {
    static LONG s_logged = 0;
    if (!info || !info->ExceptionRecord || !info->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const EXCEPTION_RECORD& rec = *info->ExceptionRecord;
    if (rec.ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const CONTEXT& ctx = *info->ContextRecord;
    char where[64] = {};
    if (!DescribeCodeAddress(static_cast<uintptr_t>(ctx.Eip), where, sizeof(where))) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (InterlockedIncrement(&s_logged) > 3) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const unsigned faultAddr = rec.NumberParameters >= 2 ? static_cast<unsigned>(rec.ExceptionInformation[1]) : 0u;
    const char* op = (rec.NumberParameters >= 1 && rec.ExceptionInformation[0] == 1) ? "write" : "read";
    // Vectored handlers see first-chance exceptions: if the game handles this one it keeps running,
    // so a crash is the last of these lines before the log ends.
    LOG_ERROR("Crash: access violation (%s of 0x%08x) at %s (fatal unless the game handles it)", op, faultAddr,
              where);
    LOG_ERROR("Crash: eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x",
              static_cast<unsigned>(ctx.Eax), static_cast<unsigned>(ctx.Ebx), static_cast<unsigned>(ctx.Ecx),
              static_cast<unsigned>(ctx.Edx), static_cast<unsigned>(ctx.Esi), static_cast<unsigned>(ctx.Edi),
              static_cast<unsigned>(ctx.Ebp), static_cast<unsigned>(ctx.Esp));
    // Stack words that point into either module are likely return addresses (newest first).
    const auto* stack = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(ctx.Esp));
    int found = 0;
    for (int i = 0; i < 256 && found < 16; ++i) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(stack + i, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
            break;
        }
        char ret[64] = {};
        if (DescribeCodeAddress(stack[i], ret, sizeof(ret))) {
            LOG_ERROR("Crash: stack[%d] -> %s", i, ret);
            ++found;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void InstallCrashLog() {
    g_exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    g_exeEnd = ModuleEnd(GetModuleHandleW(nullptr));
    g_selfBase = reinterpret_cast<uintptr_t>(g_hModule);
    g_selfEnd = ModuleEnd(g_hModule);
    if (AddVectoredExceptionHandler(0, CrashLogHandler)) {
        LOG_INFO("Crash log armed (access violations in the game or this DLL are logged)");
    }
}

static bool LoadSystemDirectDraw() {
    if (g_realDirectDrawCreate) {
        return true;
    }
    wchar_t sysDir[MAX_PATH];
    UINT len = GetSystemDirectoryW(sysDir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return false;
    }
    std::wstring path = std::wstring(sysDir) + L"\\ddraw.dll";
    g_systemDDraw = LoadLibraryW(path.c_str());
    if (!g_systemDDraw) {
        LOG_ERROR("Failed to load system ddraw.dll from %ls (error: %lu)", path.c_str(), GetLastError());
        return false;
    }
    g_realDirectDrawCreate = reinterpret_cast<DirectDrawCreateFn>(
        GetProcAddress(g_systemDDraw, "DirectDrawCreate"));
    if (!g_realDirectDrawCreate) {
        LOG_ERROR("Failed to find DirectDrawCreate in system ddraw.dll");
        return false;
    }
    return true;
}

static void InitializeProxy() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        std::wstring dir = GetModuleDir(g_hModule);
        std::wstring iniPath = dir.empty() ? L"rr95gpu.ini" : dir + L"\\rr95gpu.ini";
        std::wstring logPath = dir.empty() ? L"rr95gpu.log" : dir + L"\\rr95gpu.log";

        ConfigInit(iniPath);
        const Config& cfg = GetConfig();
        LogInit(logPath, cfg.logLevel);

        LOG_INFO("rr95gpu proxy ddraw.dll loaded");
        InstallCrashLog();
        if (cfg.unknownFilterWarning) {
            LOG_WARN("Unknown Filter value '%ls' in rr95gpu.ini; defaulting to bilinear",
                     cfg.rawFilterString.c_str());
        }
        LOG_INFO("Config: Enabled=%d, LogLevel=%d, Presenter.Enabled=%d, Filter=%s, VSync=%d, Capture.Enabled=%d, Capture.AutoDumpFrame=%d, World.GpuWorld=%d, World.InternalRes=%dx%d",
                 cfg.enabled ? 1 : 0,
                 static_cast<int>(cfg.logLevel),
                 cfg.presenterEnabled ? 1 : 0,
                 FilterModeToString(cfg.filter),
                 cfg.vsync ? 1 : 0,
                 cfg.captureEnabled ? 1 : 0,
                 cfg.captureAutoDumpFrame,
                 cfg.gpuWorld ? 1 : 0,
                 cfg.internalWidth,
                 cfg.internalHeight);

        if (!LoadSystemDirectDraw()) {
            LOG_ERROR("Failed to initialize system DirectDraw forwarding");
        } else {
            LOG_INFO("System DirectDraw forwarding initialized");
        }

        if (!cfg.enabled) {
            LOG_INFO("rr95gpu disabled by config, hooks skipped");
            rr95::modapi::SetHookState(RR95_HOOK_STATE_FAILED);
            rr95::modapi::CloseGraphicsMenuRowSlot();
            return;
        }

        ExeIdentity id = IdentifyCurrentExe();
        if (id.build == ExeBuild::Unknown) {
            LOG_INFO("Host unknown (hash: %s), hooks skipped", id.sha256Hex.c_str());
            rr95::modapi::SetHookState(RR95_HOOK_STATE_FAILED);
            rr95::modapi::CloseGraphicsMenuRowSlot();
            return;
        }

        const char* buildName = (id.build == ExeBuild::Original) ? "Original" : "Patched";
        LOG_INFO("Detected supported game build: %s (%s)", buildName, id.sha256Hex.c_str());

        if (cfg.captureEnabled || cfg.presenterEnabled || cfg.gpuWorld) {
            CaptureInit(GetModuleHandleW(nullptr), dir);
        }

        if (InstallHooks(id.build)) {
            rr95::modapi::SetHookState(RR95_HOOK_STATE_ACTIVE);
            HMODULE pinned = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                               reinterpret_cast<LPCWSTR>(&InitializeProxy),
                               &pinned);
            LOG_INFO("All hooks installed successfully (proxy module pinned)");
        } else {
            LOG_WARN("Hook installation failed, running in unhooked mode");
            rr95::modapi::SetHookState(RR95_HOOK_STATE_FAILED);
            rr95::modapi::CloseGraphicsMenuRowSlot();
        }
    });
}

extern "C" HRESULT WINAPI DirectDrawCreate(GUID* lpGUID, LPDIRECTDRAW* lplpDD, IUnknown* pUnkOuter) {
    InitializeProxy();
    if (!g_realDirectDrawCreate) {
        if (lplpDD) {
            *lplpDD = nullptr;
        }
        return DDERR_GENERIC;
    }
    return g_realDirectDrawCreate(lpGUID, lplpDD, pUnkOuter);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        g_hModule = hinstDLL;
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        // No client callback may run from here on, including during process exit.
        rr95::modapi::SetDetached();
        // Never call FreeLibrary or unpatch code under the loader lock, and do no cleanup here.
        // At process exit (lpvReserved != nullptr) the OS has already killed every other thread,
        // possibly while they held locks, and the static destructors run under the loader lock.
        // The objects that own D3D11/DXGI resources, worker threads or mutexes (presenter,
        // texture dumper, texture pack, capture caches) are leaked singletons for that reason:
        // their teardown there could block forever. The OS reclaims everything at exit.
        if (lpvReserved == nullptr) {
            LogClose();
        }
    }
    return TRUE;
}
