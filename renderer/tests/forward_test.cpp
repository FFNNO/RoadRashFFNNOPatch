// forward_test: loads the built proxy ddraw.dll by full path, calls its
// DirectDrawCreate and checks that it forwards to the system DirectDraw.
// Exercises both Enabled=0 (config-disabled forwarding) and default Enabled=1
// (unknown host -> hooks skipped).
#include <windows.h>
#include <ddraw.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#ifndef RR95_PROXY_PATH
#error "RR95_PROXY_PATH must be defined by the build"
#endif

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) {
        ++g_failures;
    }
}

std::wstring SiblingPath(const std::wstring& file, const wchar_t* name) {
    const size_t slash = file.find_last_of(L"\\/");
    return file.substr(0, slash + 1) + name;
}

std::string ReadAll(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void WriteText(const std::wstring& path, const char* content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

bool HasLineWith(const std::string& text, const char* a, const char* b) {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.find(a) != std::string::npos && line.find(b) != std::string::npos) {
            return true;
        }
    }
    return false;
}

int RunProxyCheck(const std::wstring& proxyPath, const std::wstring& logPath,
                  const char* expectedA, const char* expectedB) {
    HMODULE proxy = LoadLibraryW(proxyPath.c_str());
    Check(proxy != nullptr, "LoadLibraryW(proxy ddraw.dll)");
    if (proxy == nullptr) {
        return 1;
    }

    using DirectDrawCreateFn = HRESULT(WINAPI*)(GUID*, LPDIRECTDRAW*, IUnknown*);
    auto create = reinterpret_cast<DirectDrawCreateFn>(GetProcAddress(proxy, "DirectDrawCreate"));
    Check(create != nullptr, "GetProcAddress(DirectDrawCreate)");

    if (create != nullptr) {
        LPDIRECTDRAW dd = nullptr;
        const HRESULT hr = create(nullptr, &dd, nullptr);
        Check(hr == S_OK, "DirectDrawCreate returns S_OK");
        Check(dd != nullptr, "DirectDrawCreate returns a non-null IDirectDraw");
        if (dd != nullptr) {
            dd->Release();
        }
    }

    const std::string log = ReadAll(logPath);
    Check(!log.empty(), "rr95gpu.log exists next to the proxy");
    Check(HasLineWith(log, expectedA, expectedB), "log contains expected status line");

    FreeLibrary(proxy);
    return g_failures == 0 ? 0 : 1;
}

bool RunDisabledChildProcess() {
    wchar_t selfPath[MAX_PATH] = {0};
    if (GetModuleFileNameW(nullptr, selfPath, MAX_PATH) == 0) {
        return false;
    }
    std::wstring cmdLine = std::wstring(L"\"") + selfPath + L"\" --check-disabled";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(selfPath, cmdLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return exitCode == 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::wstring proxyPath = RR95_PROXY_PATH;
    const std::wstring logPath = SiblingPath(proxyPath, L"rr95gpu.log");
    const std::wstring iniPath = SiblingPath(proxyPath, L"rr95gpu.ini");

    if (argc > 1 && std::strcmp(argv[1], "--check-disabled") == 0) {
        return RunProxyCheck(proxyPath, logPath, "disabled by config", "hooks skipped");
    }

    // 1. Test Enabled=0 in a fresh child process.
    DeleteFileW(logPath.c_str());
    WriteText(iniPath, "[General]\r\nEnabled=0\r\n");
    Check(RunDisabledChildProcess(), "Enabled=0 child process forwards DirectDrawCreate and skips hooks");

    // 2. Test default Enabled=1 (no ini) in this process.
    DeleteFileW(logPath.c_str());
    DeleteFileW(iniPath.c_str());
    RunProxyCheck(proxyPath, logPath, "Host unknown", "hooks skipped");

    std::printf("%s (%d failure(s))\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
