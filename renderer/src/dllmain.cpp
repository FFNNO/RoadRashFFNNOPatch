#include <windows.h>
#include <ddraw.h>
#include <mutex>
#include <string>
#include "log.h"
#include "config.h"
#include "exe_identity.h"
#include "hook_sites.h"

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
        LOG_INFO("Config: Enabled=%d, LogLevel=%d", cfg.enabled ? 1 : 0, static_cast<int>(cfg.logLevel));

        if (!LoadSystemDirectDraw()) {
            LOG_ERROR("Failed to initialize system DirectDraw forwarding");
        } else {
            LOG_INFO("System DirectDraw forwarding initialized");
        }

        if (!cfg.enabled) {
            LOG_INFO("rr95gpu disabled by config, hooks skipped");
            return;
        }

        ExeIdentity id = IdentifyCurrentExe();
        if (id.build == ExeBuild::Unknown) {
            LOG_INFO("Host unknown (hash: %s), hooks skipped", id.sha256Hex.c_str());
            return;
        }

        const char* buildName = (id.build == ExeBuild::Original) ? "Original" : "Patched";
        LOG_INFO("Detected supported game build: %s (%s)", buildName, id.sha256Hex.c_str());

        if (InstallHooks(id.build)) {
            LOG_INFO("All hooks installed successfully");
        } else {
            LOG_WARN("Hook installation failed, running in unhooked mode");
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
        if (lpvReserved == nullptr) {
            RemoveHooks();
            if (g_systemDDraw) {
                FreeLibrary(g_systemDDraw);
                g_systemDDraw = nullptr;
                g_realDirectDrawCreate = nullptr;
            }
            LogClose();
        }
    }
    return TRUE;
}
