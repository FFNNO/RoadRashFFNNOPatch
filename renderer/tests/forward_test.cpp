// forward_test: loads the built proxy ddraw.dll by full path, calls its
// DirectDrawCreate and checks that it forwards to the system DirectDraw.
// The test process is not RoadRash95.exe, so the proxy must log that the host
// is unknown and that hooks were skipped.
#include <windows.h>
#include <ddraw.h>

#include <cstdio>
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

}  // namespace

int main() {
    const std::wstring proxyPath = RR95_PROXY_PATH;
    const std::wstring logPath = SiblingPath(proxyPath, L"rr95gpu.log");
    const std::wstring iniPath = SiblingPath(proxyPath, L"rr95gpu.ini");

    // Start from a clean state: defaults (no ini) and no stale log.
    DeleteFileW(logPath.c_str());
    DeleteFileW(iniPath.c_str());

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
    Check(HasLineWith(log, "Host unknown", "hooks skipped"),
          "log states the host is unknown and hooks were skipped");

    FreeLibrary(proxy);

    std::printf("%s (%d failure(s))\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
