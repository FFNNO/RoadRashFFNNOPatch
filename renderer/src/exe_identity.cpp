#include "exe_identity.h"
#include <windows.h>
#include <bcrypt.h>
#include <vector>

static const char* const HASH_ORIGINAL = "9e40e25737149ab095061b7154797a35495faea97644fafc5a1e616f1400c968";
static const char* const HASH_PATCHED  = "40a0d85f7b8ed8c8e9bcb267954a645228d545e0b4a3f86241c6d936dfe88150";

static bool ComputeFileSha256(const wchar_t* path, std::string& outHashHex) {
    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status != 0) {
        CloseHandle(hFile);
        return false;
    }

    BCRYPT_HASH_HANDLE hHash = nullptr;
    status = BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0);
    if (status != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        CloseHandle(hFile);
        return false;
    }

    uint8_t buffer[65536];
    DWORD bytesRead = 0;
    bool success = true;
    while (ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
        status = BCryptHashData(hHash, buffer, bytesRead, 0);
        if (status != 0) {
            success = false;
            break;
        }
    }

    uint8_t hash[32];
    if (success && BCryptFinishHash(hHash, hash, sizeof(hash), 0) == 0) {
        char hex[65];
        for (int i = 0; i < 32; ++i) {
            sprintf_s(&hex[i * 2], 3, "%02x", hash[i]);
        }
        hex[64] = '\0';
        outHashHex = hex;
    } else {
        success = false;
    }

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    CloseHandle(hFile);
    return success;
}

ExeIdentity IdentifyCurrentExe() {
    ExeIdentity result;
    wchar_t exePath[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return result;
    }
    result.exePath = exePath;

    if (!ComputeFileSha256(exePath, result.sha256Hex)) {
        return result;
    }

    if (_stricmp(result.sha256Hex.c_str(), HASH_ORIGINAL) == 0) {
        result.build = ExeBuild::Original;
    } else if (_stricmp(result.sha256Hex.c_str(), HASH_PATCHED) == 0) {
        result.build = ExeBuild::Patched;
    } else {
        result.build = ExeBuild::Unknown;
    }

    return result;
}
