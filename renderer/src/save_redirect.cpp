#include "save_redirect.h"

#include <cstring>

const char* SaveRedirectResultToString(SaveRedirectResult result) {
    switch (result) {
        case SaveRedirectResult::Filled:         return "filled";
        case SaveRedirectResult::BufferTooSmall: return "buffer too small";
        case SaveRedirectResult::FileMissing:    return "file missing";
        case SaveRedirectResult::Unsupported:    return "unsupported request";
        case SaveRedirectResult::InvalidArgs:    return "invalid arguments";
    }
    return "unknown";
}

SaveRedirectResult FillOpenFileNameFromPath(OPENFILENAMEA& ofn, const std::string& path) {
    if (ofn.lpstrFile == nullptr || ofn.nMaxFile == 0) {
        return SaveRedirectResult::InvalidArgs;
    }
    if (path.empty() || (ofn.Flags & OFN_ALLOWMULTISELECT) != 0) {
        return SaveRedirectResult::Unsupported;
    }

    const size_t lastSep = path.find_last_of("\\/");
    const size_t nameOffset = (lastSep == std::string::npos) ? 0 : lastSep + 1;
    if (nameOffset >= path.size()) {
        return SaveRedirectResult::Unsupported;  // a folder, not a file
    }

    std::string full = path;
    size_t dot = full.find_last_of('.');
    if (dot == std::string::npos || dot < nameOffset) {
        // The real dialog appends lpstrDefExt when the typed name has no extension.
        if (ofn.lpstrDefExt != nullptr && ofn.lpstrDefExt[0] != '\0') {
            full += '.';
            full += ofn.lpstrDefExt;
            dot = full.size() - std::strlen(ofn.lpstrDefExt) - 1;
        } else {
            dot = std::string::npos;
        }
    }
    if (full.size() + 1 > ofn.nMaxFile || full.size() > 0xFFFF) {
        return SaveRedirectResult::BufferTooSmall;
    }

    std::memcpy(ofn.lpstrFile, full.c_str(), full.size() + 1);
    ofn.nFileOffset = static_cast<WORD>(nameOffset);
    // Offset of the extension (past the dot); a name ending in '.' gives 0, no extension gives the
    // offset of the terminator, as documented for OPENFILENAME.
    if (dot == std::string::npos) {
        ofn.nFileExtension = static_cast<WORD>(full.size());
    } else if (dot + 1 == full.size()) {
        ofn.nFileExtension = 0;
    } else {
        ofn.nFileExtension = static_cast<WORD>(dot + 1);
    }
    if (ofn.lpstrFileTitle != nullptr && ofn.nMaxFileTitle > full.size() - nameOffset) {
        std::memcpy(ofn.lpstrFileTitle, full.c_str() + nameOffset, full.size() - nameOffset + 1);
    }
    return SaveRedirectResult::Filled;
}

SaveRedirectResult RedirectSaveFileName(OPENFILENAMEA& ofn, const std::string& path) {
    return FillOpenFileNameFromPath(ofn, path);
}

SaveRedirectResult RedirectOpenFileName(OPENFILENAMEA& ofn, const std::string& path, bool fileExists) {
    if (!fileExists) {
        return SaveRedirectResult::FileMissing;
    }
    return FillOpenFileNameFromPath(ofn, path);
}

bool WidePathToAnsi(const std::wstring& wide, std::string& out) {
    out.clear();
    if (wide.empty()) {
        return false;
    }
    const UINT cp = GetACP();
    // CP_UTF8 rejects the best-fit flag and the used-default pointer, and can encode everything.
    const DWORD flags = (cp == CP_UTF8) ? 0 : WC_NO_BEST_FIT_CHARS;
    BOOL usedDefault = FALSE;
    BOOL* usedDefaultPtr = (cp == CP_UTF8) ? nullptr : &usedDefault;
    const int len = WideCharToMultiByte(cp, flags, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0,
                                        nullptr, usedDefaultPtr);
    if (len <= 0 || usedDefault) {
        return false;
    }
    std::string result(static_cast<size_t>(len), '\0');
    if (WideCharToMultiByte(cp, flags, wide.c_str(), static_cast<int>(wide.size()), &result[0], len, nullptr,
                            usedDefaultPtr) != len ||
        usedDefault) {
        return false;
    }
    out = std::move(result);
    return true;
}

uint32_t ComputeSaveChecksum(const uint8_t* data, size_t size) {
    // Port of the loop in FUN_0043f7c0 / FUN_0043f580: a byte sum, a weighted sum (weights size..1)
    // and an alternating sum (negated when the weight is even).
    uint32_t byteSum = 0;
    uint32_t weightedSum = 0;
    uint32_t alternatingSum = 0;
    uint32_t weight = static_cast<uint32_t>(size);
    for (size_t i = 0; i < size; ++i, --weight) {
        const uint32_t b = data[i];
        byteSum += b;
        weightedSum += b * weight;
        alternatingSum += ((weight & 1u) == 0) ? (0u - b) : b;
    }
    return ((alternatingSum << 16) | (byteSum & 0xFFFFu)) ^ weightedSum;
}

bool IsValidSaveBlock(const uint8_t* block, size_t size) {
    if (block == nullptr || size < kSaveBlockSize) {
        return false;
    }
    uint32_t stored = 0;
    uint32_t magic = 0;
    std::memcpy(&stored, block, sizeof(stored));
    std::memcpy(&magic, block + kSaveBlockMagicOffset, sizeof(magic));
    return magic == kSaveBlockMagic &&
           stored == ComputeSaveChecksum(block + kSaveBlockMagicOffset, kSaveBlockChecksumSpan);
}

bool SaveRoutineSucceeded(const SaveFileProbe& before, const SaveFileProbe& after, bool blockValidAfter,
                          uint8_t dirtyAfter) {
    if (!blockValidAfter || dirtyAfter != 0) {
        return false;
    }
    if (!after.exists || after.size != kSaveBlockSize) {
        return false;
    }
    return !before.exists || before.lastWrite != after.lastWrite;
}

bool LoadRoutineSucceeded(bool blockValidAfter, uint8_t dirtyAfter) {
    return blockValidAfter && dirtyAfter == 0;
}
