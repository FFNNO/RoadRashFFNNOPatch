#pragma once
// Fixed save path support: answers the game's COMDLG32 save/open dialogs with one default file and
// decides, from observable state, whether the game's own save and load routines succeeded.
// Everything here is pure (no game memory access) so it can be unit tested.
#include <windows.h>
#include <commdlg.h>
#include <cstddef>
#include <cstdint>
#include <string>

// Road Rash 95 save file: 160 bytes, a checksum dword, then the magic 0xCAFEDEAD and 152 bytes of
// game state. The checksum covers the 156 bytes after it (out/decomp.c:32561-32581).
constexpr size_t kSaveBlockSize = 0xA0;
constexpr uint32_t kSaveBlockMagic = 0xCAFEDEADu;
constexpr size_t kSaveBlockMagicOffset = 4;
constexpr size_t kSaveBlockChecksumSpan = 0x9C;

enum class SaveRedirectResult {
    Filled,          // ofn->lpstrFile holds the default path; the hook returns TRUE without a dialog
    BufferTooSmall,  // the path (plus default extension) does not fit nMaxFile; show the real dialog
    FileMissing,     // open only: the default file does not exist; show the real dialog
    Unsupported,     // multi-select request or empty path; show the real dialog
    InvalidArgs      // null buffer or zero nMaxFile; show the real dialog
};

const char* SaveRedirectResultToString(SaveRedirectResult result);

// Writes path into ofn.lpstrFile (respecting nMaxFile, including the terminator) the way the real
// dialog would: the default extension (ofn.lpstrDefExt) is appended when the file name has none,
// nFileOffset/nFileExtension are set, and lpstrFileTitle receives the file name when it fits.
// Nothing in ofn is modified unless the result is Filled.
SaveRedirectResult FillOpenFileNameFromPath(OPENFILENAMEA& ofn, const std::string& path);
// Save dialog: always answered with the default path when it fits.
SaveRedirectResult RedirectSaveFileName(OPENFILENAMEA& ofn, const std::string& path);
// Open dialog: answered with the default path only when that file exists.
SaveRedirectResult RedirectOpenFileName(OPENFILENAMEA& ofn, const std::string& path, bool fileExists);

// Converts a resolved wide path to the game's ANSI code page. Fails (returns false) when a character
// has no exact ANSI equivalent, because the game opens the file with CreateFileA.
bool WidePathToAnsi(const std::wstring& wide, std::string& out);

// Checksum the game stores at offset 0 of the save block, over `size` bytes starting at offset 4.
uint32_t ComputeSaveChecksum(const uint8_t* data, size_t size);
// True when block holds the magic and a matching checksum (what the game's load routine accepts).
bool IsValidSaveBlock(const uint8_t* block, size_t size);

struct SaveFileProbe {
    bool exists = false;
    uint64_t size = 0;
    uint64_t lastWrite = 0;  // FILETIME as a 64-bit value
};

// Save success rule (the save routine FUN_0043f7c0 returns void and fails silently when CreateFileA
// fails, out/decomp.c:32543-32544, or shows a MessageBox when WriteFile fails, :32582-32589):
// the hook clears the magic in the scratch block before the call, then success requires
//  - a valid block afterwards (CreateFileA succeeded and the block was built in this call),
//  - the dirty flag 0x5cd47e cleared (only the success path clears it, :32591),
//  - the file present with exactly 160 bytes, and
//  - the file new or its last-write time changed (rules out a failed WriteFile when the dirty flag
//    was already clear before the call).
bool SaveRoutineSucceeded(const SaveFileProbe& before, const SaveFileProbe& after, bool blockValidAfter,
                          uint8_t dirtyAfter);
// Load success rule (FUN_0043f580, out/decomp.c:32437-32518): the hook clears the magic in the
// scratch block before the call. Every failure exit leaves the block invalid (CreateFileA failed: the
// cleared magic stays; ReadFile failed, bad magic or bad checksum: :32460-32493), and only the
// success path clears the dirty flag (:32515). Success = valid block afterwards and dirty flag 0.
bool LoadRoutineSucceeded(bool blockValidAfter, uint8_t dirtyAfter);
