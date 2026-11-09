#include <windows.h>
#include <ddraw.h>
#include <mutex>
#include <string>
#include "log.h"
#include "config.h"
#include "exe_identity.h"
#include "hook_sites.h"
#include "capture.h"
#include "texture_pack.h"

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
            return;
        }

        ExeIdentity id = IdentifyCurrentExe();
        if (id.build == ExeBuild::Unknown) {
            LOG_INFO("Host unknown (hash: %s), hooks skipped", id.sha256Hex.c_str());
            return;
        }

        const char* buildName = (id.build == ExeBuild::Original) ? "Original" : "Patched";
        LOG_INFO("Detected supported game build: %s (%s)", buildName, id.sha256Hex.c_str());

        if (cfg.captureEnabled || cfg.presenterEnabled || cfg.gpuWorld) {
            CaptureInit(GetModuleHandleW(nullptr), dir);
        }

        if (InstallHooks(id.build)) {
            HMODULE pinned = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                               reinterpret_cast<LPCWSTR>(&InitializeProxy),
                               &pinned);
            LOG_INFO("All hooks installed successfully (proxy module pinned)");
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
        // Never call FreeLibrary or unpatch code under the loader lock.
        // Both paths run the static destructors under the loader lock, so the texture pack
        // workers must be stopped without joining: mark the detach before they run.
        TexturePackSetProcessTerminating();
        if (lpvReserved == nullptr) {
            LogClose();
        }
    }
    return TRUE;
}
