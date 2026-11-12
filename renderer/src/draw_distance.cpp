#include "draw_distance.h"
#include "log.h"
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <cwctype>

namespace rr95 {

namespace {

constexpr uint32_t kPreferredBase = 0x00400000;
constexpr uint32_t kVaSliceArray = 0x0060bd00;       // DAT_0060bd00, 0x32 slices of 0x224
constexpr uint32_t kVaRowCache1 = 0x006084a0;        // DAT_006084a0, 0x32 entries of 0x90
constexpr uint32_t kVaRowCache2 = 0x0060a0c0;        // DAT_0060a0c0, 0x32 entries of 0x90
constexpr uint32_t kVaThreshold = 0x00608320;        // DAT_00608320, 0x30 entries (+ 0x6083e0 = last)
constexpr uint32_t kVaReciprocal = 0x00597c90;       // DAT_00597c90, 0x1c00 entries
constexpr uint32_t kVaThresholdSource = 0x0059ec98;  // DAT_0059ec98, 0x32 entries

// Every patch site, byte-verified on both exe builds (original and patched RoadRash95.exe, which
// differ in 4 bytes outside this code) and generated from the image itself:
// - Absolute references: every base relocation of the image whose target lies in
//   [0x00597c90, 0x0059ec90), [0x00608320, 0x006083e0) or [0x006084a0, 0x006127c8) is listed here,
//   except the ones that keep using the engine's arrays on purpose:
//     0x45aac4  FUN_0045aa90 fill loop of DAT_00597c90 (the shipped table stays filled as before)
//     0x45ab35  FUN_0045aa90 threshold loop reading DAT_00597c90 (indices below 0x1900)
//     0x45ab3c  FUN_0045aa90 threshold loop writing DAT_00608320 (no longer read)
//     0x44ee47  FUN_0044edf0 reads DAT_00597c90[0x69] (fixed index, same value in both tables)
//     0x41eda0, 0x41ef6e, 0x41efc8, 0x45a494  the horizon DAT_0060bce0 (a scalar, not moved)
//   check_hook_sites.py re-checks this against the .reloc section of each exe.
// - Constants: every 0x1bff / 0x1c00 immediate (byte scan) in the functions that index
//   DAT_00597c90 (FUN_0041cfc0, FUN_0041d3b0, FUN_0044e870, FUN_00459170, FUN_00459cb0,
//   FUN_0045a6a0, FUN_004666a0, FUN_00466a60, FUN_00466eb0, FUN_00467290, FUN_00467670) is listed;
//   each one clamps the index of a table read in the same function. FUN_0045aa90's own clamp
//   (0x45ab12, 0x45ab19) stays: it reads the shipped table. The segment count is the push at
//   0x45aca3 and the clamp at 0x45a3b6 / 0x45a3bb; FUN_00459cb0 takes the row cache modulo with
//   idiv by its argument (0x459daa, 0x459f18, ...), so the push sets it too. FUN_0045aa90's loop
//   counts (0x45aa9f caches, 0x45ab52 slices) make race init initialise the moved arrays.
// - Readers: no AI, traffic or collision code references these arrays (only FUN_0041f020,
//   FUN_00459170, FUN_00459cb0, FUN_0045a6a0, FUN_0045a790, FUN_0045a870, FUN_0045aa90,
//   FUN_0045ac50 and the projection helpers above); objects are drawn only up to z 0x1fff
//   (FUN_0044ac60), so they never reach the new far rows.
const DrawPatchSite kSites[] = {
    {0x0041D033, 5, {0xB9, 0xFF, 0x1B, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalMax},
    {0x0041D053, 5, {0x3D, 0x00, 0x1C, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalLimit},
    {0x0041D062, 7, {0x8B, 0x04, 0x8D, 0x90, 0x7C, 0x59, 0x00}, 3, 3, 4, DrawField::ReciprocalAddr},
    {0x0041D3F3, 5, {0xBB, 0xFF, 0x1B, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalMax},
    {0x0041D3F8, 6, {0x81, 0xF9, 0x00, 0x1C, 0x00, 0x00}, -1, 2, 4, DrawField::ReciprocalLimit},
    {0x0041D408, 7, {0x8B, 0x34, 0x85, 0x90, 0x7C, 0x59, 0x00}, 3, 3, 4, DrawField::ReciprocalAddr},
    {0x0041D415, 6, {0x81, 0xFA, 0x00, 0x1C, 0x00, 0x00}, -1, 2, 4, DrawField::ReciprocalLimit},
    {0x0041D428, 7, {0x8B, 0x1C, 0x9D, 0x90, 0x7C, 0x59, 0x00}, 3, 3, 4, DrawField::ReciprocalAddr},
    {0x0041D5F2, 5, {0xBB, 0xFF, 0x1B, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalMax},
    {0x0041D5FE, 6, {0x81, 0xF9, 0x00, 0x1C, 0x00, 0x00}, -1, 2, 4, DrawField::ReciprocalLimit},
    {0x0041D610, 7, {0x8B, 0x04, 0x85, 0x90, 0x7C, 0x59, 0x00}, 3, 3, 4, DrawField::ReciprocalAddr},
    {0x0041D61A, 6, {0x81, 0xFA, 0x00, 0x1C, 0x00, 0x00}, -1, 2, 4, DrawField::ReciprocalLimit},
    {0x0041D626, 7, {0x8B, 0x1C, 0x9D, 0x90, 0x7C, 0x59, 0x00}, 3, 3, 4, DrawField::ReciprocalAddr},
    {0x0041F06C, 6, {0x3B, 0x81, 0xE4, 0xBA, 0x60, 0x00}, 2, 2, 4, DrawField::SliceAddr},
    {0x0044E972, 5, {0xB8, 0xFF, 0x1B, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalMax},
    {0x0044E977, 6, {0x81, 0xF9, 0x00, 0x1C, 0x00, 0x00}, -1, 2, 4, DrawField::ReciprocalLimit},
    {0x0044E983, 11, {0x69, 0x04, 0x85, 0x90, 0x7C, 0x59, 0x00, 0xFA, 0x00, 0x00, 0x00}, 3, 3, 4, DrawField::ReciprocalAddr},
    {0x0044E9E1, 6, {0x81, 0xFF, 0x00, 0x1C, 0x00, 0x00}, -1, 2, 4, DrawField::ReciprocalLimit},
    {0x0044E9E7, 5, {0xB8, 0xFF, 0x1B, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalMax},
    {0x0044E9FB, 8, {0x0F, 0xAF, 0x14, 0x85, 0x90, 0x7C, 0x59, 0x00}, 4, 4, 4, DrawField::ReciprocalAddr},
    {0x0044EA03, 5, {0xB8, 0xFF, 0x1B, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalMax},
    {0x0044EA10, 6, {0x81, 0xFF, 0x00, 0x1C, 0x00, 0x00}, -1, 2, 4, DrawField::ReciprocalLimit},
    {0x0044EA26, 8, {0x0F, 0xAF, 0x14, 0x85, 0x90, 0x7C, 0x59, 0x00}, 4, 4, 4, DrawField::ReciprocalAddr},
    {0x00459200, 5, {0xB8, 0xFF, 0x1B, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalMax},
    {0x0045920A, 6, {0x81, 0xF9, 0x00, 0x1C, 0x00, 0x00}, -1, 2, 4, DrawField::ReciprocalLimit},
    {0x00459213, 7, {0x8B, 0x0C, 0x85, 0x90, 0x7C, 0x59, 0x00}, 3, 3, 4, DrawField::ReciprocalAddr},
    {0x00459622, 5, {0xB8, 0xFF, 0x1B, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalMax},
    {0x00459632, 6, {0x81, 0xF9, 0x00, 0x1C, 0x00, 0x00}, -1, 2, 4, DrawField::ReciprocalLimit},
    {0x0045963E, 7, {0x8B, 0x04, 0x85, 0x90, 0x7C, 0x59, 0x00}, 3, 3, 4, DrawField::ReciprocalAddr},
    {0x00459671, 5, {0xB8, 0xFF, 0x1B, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalMax},
    {0x00459689, 6, {0x81, 0xF9, 0x00, 0x1C, 0x00, 0x00}, -1, 2, 4, DrawField::ReciprocalLimit},
    {0x00459696, 5, {0xB9, 0xFF, 0x1B, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalMax},
    {0x0045969B, 7, {0x8B, 0x04, 0x85, 0x90, 0x7C, 0x59, 0x00}, 3, 3, 4, DrawField::ReciprocalAddr},
    {0x004596A6, 6, {0x81, 0xFA, 0x00, 0x1C, 0x00, 0x00}, -1, 2, 4, DrawField::ReciprocalLimit},
    {0x004596AF, 7, {0x8B, 0x0C, 0x8D, 0x90, 0x7C, 0x59, 0x00}, 3, 3, 4, DrawField::ReciprocalAddr},
    {0x004596DF, 6, {0x81, 0xF9, 0x00, 0x1C, 0x00, 0x00}, -1, 2, 4, DrawField::ReciprocalLimit},
    {0x004596E8, 5, {0xB8, 0xFF, 0x1B, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalMax},
    {0x004596F3, 7, {0x8B, 0x04, 0x85, 0x90, 0x7C, 0x59, 0x00}, 3, 3, 4, DrawField::ReciprocalAddr},
    {0x0045972E, 5, {0xB8, 0xFF, 0x1B, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalMax},
    {0x00459746, 6, {0x81, 0xF9, 0x00, 0x1C, 0x00, 0x00}, -1, 2, 4, DrawField::ReciprocalLimit},
    {0x00459760, 7, {0x8B, 0x04, 0x85, 0x90, 0x7C, 0x59, 0x00}, 3, 3, 4, DrawField::ReciprocalAddr},
    {0x0045976B, 6, {0x81, 0xFA, 0x00, 0x1C, 0x00, 0x00}, -1, 2, 4, DrawField::ReciprocalLimit},
    {0x00459774, 5, {0xB8, 0xFF, 0x1B, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalMax},
    {0x0045977C, 8, {0x0F, 0xAF, 0x0C, 0x85, 0x90, 0x7C, 0x59, 0x00}, 4, 4, 4, DrawField::ReciprocalAddr},
    {0x004598AE, 5, {0xB9, 0xFF, 0x1B, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalMax},
    {0x004598B3, 6, {0x81, 0xFA, 0x00, 0x1C, 0x00, 0x00}, -1, 2, 4, DrawField::ReciprocalLimit},
    {0x004598C4, 8, {0x0F, 0xAF, 0x34, 0x85, 0x90, 0x7C, 0x59, 0x00}, 4, 4, 4, DrawField::ReciprocalAddr},
    {0x004598D9, 6, {0x81, 0xFA, 0x00, 0x1C, 0x00, 0x00}, -1, 2, 4, DrawField::ReciprocalLimit},
    {0x004598F5, 8, {0x0F, 0xAF, 0x04, 0x8D, 0x90, 0x7C, 0x59, 0x00}, 4, 4, 4, DrawField::ReciprocalAddr},
    {0x004599D5, 5, {0xB8, 0xFF, 0x1B, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalMax},
    {0x004599EA, 6, {0x81, 0xF9, 0x00, 0x1C, 0x00, 0x00}, -1, 2, 4, DrawField::ReciprocalLimit},
    {0x004599F9, 8, {0x0F, 0xAF, 0x14, 0x85, 0x90, 0x7C, 0x59, 0x00}, 4, 4, 4, DrawField::ReciprocalAddr},
    {0x00459A11, 6, {0x81, 0xF9, 0x00, 0x1C, 0x00, 0x00}, -1, 2, 4, DrawField::ReciprocalLimit},
    {0x00459A1D, 5, {0xBA, 0xFF, 0x1B, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalMax},
    {0x00459A2F, 8, {0x0F, 0xAF, 0x04, 0x95, 0x90, 0x7C, 0x59, 0x00}, 4, 4, 4, DrawField::ReciprocalAddr},
    {0x00459DB6, 6, {0x81, 0xC1, 0xA0, 0x84, 0x60, 0x00}, 2, 2, 4, DrawField::Cache1Addr},
    {0x00459F24, 6, {0x81, 0xC1, 0xA0, 0x84, 0x60, 0x00}, 2, 2, 4, DrawField::Cache1Addr},
    {0x00459F40, 6, {0x81, 0xC1, 0xC0, 0xA0, 0x60, 0x00}, 2, 2, 4, DrawField::Cache2Addr},
    {0x0045A106, 6, {0x81, 0xC1, 0xA0, 0x84, 0x60, 0x00}, 2, 2, 4, DrawField::Cache1Addr},
    {0x0045A122, 6, {0x81, 0xC1, 0xC0, 0xA0, 0x60, 0x00}, 2, 2, 4, DrawField::Cache2Addr},
    {0x0045A1BD, 5, {0xBA, 0xFF, 0x1B, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalMax},
    {0x0045A20D, 5, {0x3D, 0x00, 0x1C, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalLimit},
    {0x0045A229, 8, {0x0F, 0xAF, 0x0C, 0x95, 0x90, 0x7C, 0x59, 0x00}, 4, 4, 4, DrawField::ReciprocalAddr},
    {0x0045A231, 5, {0xBA, 0xFF, 0x1B, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalMax},
    {0x0045A243, 7, {0x81, 0x7D, 0x10, 0x00, 0x1C, 0x00, 0x00}, -1, 3, 4, DrawField::ReciprocalLimit},
    {0x0045A265, 8, {0x0F, 0xAF, 0x0C, 0x95, 0x90, 0x7C, 0x59, 0x00}, 4, 4, 4, DrawField::ReciprocalAddr},
    {0x0045A32F, 6, {0x81, 0xC1, 0xA0, 0x84, 0x60, 0x00}, 2, 2, 4, DrawField::Cache1Addr},
    {0x0045A34D, 6, {0x81, 0xC2, 0xC0, 0xA0, 0x60, 0x00}, 2, 2, 4, DrawField::Cache2Addr},
    {0x0045A3B6, 3, {0x83, 0xF8, 0x30}, -1, 2, 1, DrawField::DrawCap},
    {0x0045A3BB, 10, {0xC7, 0x05, 0x90, 0xEC, 0x59, 0x00, 0x30, 0x00, 0x00, 0x00}, 2, 6, 4, DrawField::DrawCap},
    {0x0045A400, 7, {0x3B, 0x0C, 0xBD, 0x20, 0x83, 0x60, 0x00}, 3, 3, 4, DrawField::ThresholdAddr},
    {0x0045A420, 7, {0x3B, 0x0C, 0xBD, 0x20, 0x83, 0x60, 0x00}, 3, 3, 4, DrawField::ThresholdAddr},
    {0x0045A6AD, 5, {0xBA, 0xFF, 0x1B, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalMax},
    {0x0045A6B2, 6, {0x81, 0xFF, 0x00, 0x1C, 0x00, 0x00}, -1, 2, 4, DrawField::ReciprocalLimit},
    {0x0045A6C8, 8, {0x0F, 0xAF, 0x34, 0x85, 0x90, 0x7C, 0x59, 0x00}, 4, 4, 4, DrawField::ReciprocalAddr},
    {0x0045A6DD, 6, {0x81, 0xFF, 0x00, 0x1C, 0x00, 0x00}, -1, 2, 4, DrawField::ReciprocalLimit},
    {0x0045A6F1, 8, {0x0F, 0xAF, 0x04, 0x95, 0x90, 0x7C, 0x59, 0x00}, 4, 4, 4, DrawField::ReciprocalAddr},
    {0x0045A79D, 5, {0x05, 0x00, 0xBD, 0x60, 0x00}, 1, 1, 4, DrawField::SliceAddr},
    {0x0045A87A, 5, {0x05, 0x00, 0xBD, 0x60, 0x00}, 1, 1, 4, DrawField::SliceAddr},
    {0x0045AA95, 5, {0xB9, 0xA0, 0x84, 0x60, 0x00}, 1, 1, 4, DrawField::Cache1Addr},
    {0x0045AA9F, 5, {0xB8, 0x32, 0x00, 0x00, 0x00}, -1, 1, 4, DrawField::CacheEntries},
    {0x0045AB52, 5, {0xBA, 0x32, 0x00, 0x00, 0x00}, -1, 1, 4, DrawField::SliceEntries},
    {0x0045AB5F, 5, {0xB8, 0x14, 0xBD, 0x60, 0x00}, 1, 1, 4, DrawField::SliceAddr},
    {0x0045AC92, 5, {0xBA, 0x00, 0xBD, 0x60, 0x00}, 1, 1, 4, DrawField::SliceAddr},
    {0x0045ACA3, 2, {0x6A, 0x31}, -1, 1, 1, DrawField::DrawPush},
    {0x0045ACB7, 6, {0x8B, 0x80, 0x14, 0xBD, 0x60, 0x00}, 2, 2, 4, DrawField::SliceAddr},
    {0x0045AD1D, 6, {0x81, 0xC1, 0x00, 0xBD, 0x60, 0x00}, 2, 2, 4, DrawField::SliceAddr},
    {0x0045AD32, 6, {0x8D, 0xB8, 0xDC, 0xBA, 0x60, 0x00}, 2, 2, 4, DrawField::SliceAddr},
    {0x004666B9, 5, {0xB8, 0xFF, 0x1B, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalMax},
    {0x004666C9, 6, {0x81, 0xFA, 0x00, 0x1C, 0x00, 0x00}, -1, 2, 4, DrawField::ReciprocalLimit},
    {0x004666DD, 7, {0x8B, 0x14, 0x85, 0x90, 0x7C, 0x59, 0x00}, 3, 3, 4, DrawField::ReciprocalAddr},
    {0x00466B53, 5, {0xB8, 0xFF, 0x1B, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalMax},
    {0x00466B5A, 6, {0x81, 0xF9, 0x00, 0x1C, 0x00, 0x00}, -1, 2, 4, DrawField::ReciprocalLimit},
    {0x00466B6C, 7, {0x8B, 0x04, 0x85, 0x90, 0x7C, 0x59, 0x00}, 3, 3, 4, DrawField::ReciprocalAddr},
    {0x00466F0B, 5, {0xB8, 0xFF, 0x1B, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalMax},
    {0x00466F1B, 6, {0x81, 0xF9, 0x00, 0x1C, 0x00, 0x00}, -1, 2, 4, DrawField::ReciprocalLimit},
    {0x00466F27, 7, {0x8B, 0x0C, 0x85, 0x90, 0x7C, 0x59, 0x00}, 3, 3, 4, DrawField::ReciprocalAddr},
    {0x00467409, 5, {0xBA, 0xFF, 0x1B, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalMax},
    {0x00467410, 6, {0x81, 0xF9, 0x00, 0x1C, 0x00, 0x00}, -1, 2, 4, DrawField::ReciprocalLimit},
    {0x0046741B, 7, {0x8B, 0x34, 0x95, 0x90, 0x7C, 0x59, 0x00}, 3, 3, 4, DrawField::ReciprocalAddr},
    {0x004677DF, 5, {0xBA, 0xFF, 0x1B, 0x00, 0x00}, -1, 1, 4, DrawField::ReciprocalMax},
    {0x004677E4, 6, {0x81, 0xF9, 0x00, 0x1C, 0x00, 0x00}, -1, 2, 4, DrawField::ReciprocalLimit},
    {0x004677EF, 7, {0x8B, 0x3C, 0x95, 0x90, 0x7C, 0x59, 0x00}, 3, 3, 4, DrawField::ReciprocalAddr},
};
static_assert(sizeof(kSites) / sizeof(kSites[0]) == kDrawPatchSiteCount, "kDrawPatchSiteCount");

uint32_t ReadU32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

// The site's instruction bytes as the loader left them at moduleBase.
void RelocatedExpected(const DrawPatchSite& site, const uint8_t* moduleBase, uint8_t out[12]) {
    std::memcpy(out, site.expected, sizeof(site.expected));
    if (site.absoluteOffset >= 0) {
        const uint32_t delta = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(moduleBase)) - kPreferredBase;
        const uint32_t v = ReadU32(out + site.absoluteOffset) + delta;
        std::memcpy(out + site.absoluteOffset, &v, sizeof(v));
    }
}

uint8_t* SiteAddress(uint8_t* moduleBase, const DrawPatchSite& site) {
    return moduleBase + (site.va - kPreferredBase);
}

bool WriteBytes(uint8_t* dst, const uint8_t* src, size_t size) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(dst, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    std::memcpy(dst, src, size);
    DWORD ignored = 0;
    VirtualProtect(dst, size, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), dst, size);
    return true;
}

}  // namespace

bool ParseDrawSegments(const wchar_t* str, int& out) {
    out = kEngineDrawSegments;
    if (str == nullptr) {
        return false;
    }
    while (*str != L'\0' && std::iswspace(*str)) {
        ++str;
    }
    if (!std::iswdigit(*str)) {
        return false;
    }
    wchar_t* end = nullptr;
    const long value = std::wcstol(str, &end, 10);
    while (*end != L'\0' && std::iswspace(*end)) {
        ++end;
    }
    if (*end != L'\0' || value < kEngineDrawSegments) {
        return false;
    }
    if (value > kMaxDrawSegments) {
        out = kMaxDrawSegments;
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

const DrawPatchSite* DrawPatchSites() {
    return kSites;
}

uint32_t DrawPatchFieldValue(const DrawPatchSite& site, const DrawTargets& t) {
    const uint32_t original = site.fieldSize == 4 ? ReadU32(site.expected + site.fieldOffset)
                                                  : site.expected[site.fieldOffset];
    const uint32_t n = static_cast<uint32_t>(t.segments);
    switch (site.field) {
        case DrawField::DrawPush: return n + 1;
        case DrawField::DrawCap: return n;
        case DrawField::CacheEntries:
        case DrawField::SliceEntries: return n + 2;
        case DrawField::SliceAddr: return t.sliceArray + (original - kVaSliceArray);
        case DrawField::Cache1Addr: return t.cache1 + (original - kVaRowCache1);
        case DrawField::Cache2Addr: return t.cache2 + (original - kVaRowCache2);
        case DrawField::ThresholdAddr: return t.threshold + (original - kVaThreshold);
        case DrawField::ReciprocalAddr: return t.reciprocal + (original - kVaReciprocal);
        case DrawField::ReciprocalMax: return static_cast<uint32_t>(kDrawReciprocalEntries - 1);
        case DrawField::ReciprocalLimit: return static_cast<uint32_t>(kDrawReciprocalEntries);
    }
    return original;
}

DrawLayout ComputeDrawLayout(uintptr_t blockStart, int segments) {
    const uintptr_t entries =
        static_cast<uintptr_t>(std::clamp(segments, kEngineDrawSegments, kMaxDrawSegments)) + 2;
    DrawLayout l;
    l.sliceArray = blockStart + kDrawSliceBlockOffset;
    l.cache2 = l.sliceArray - kDrawCacheSliceGap - entries * kDrawRowCacheStride;
    l.cache1 = l.cache2 - entries * kDrawRowCacheStride;
    return l;
}

int32_t EngineReciprocal(int index) {
    if (index <= 0) {
        return 0;
    }
    return static_cast<int32_t>(0x1000000LL / (2LL * index));
}

bool EngineReciprocalsLookShipped(const int32_t* engine) {
    if (engine == nullptr || engine[0] == 0) {
        return false;
    }
    for (int i = kFirstComputedReciprocal; i < kEngineReciprocalEntries; ++i) {
        if (engine[i] != 0 && engine[i] != EngineReciprocal(i)) {
            return false;
        }
    }
    return true;
}

void BuildReciprocalTable(const int32_t* engine, int32_t* out, int outCount) {
    for (int i = 0; i < outCount; ++i) {
        const int32_t e = i < kEngineReciprocalEntries ? engine[i] : 0;
        out[i] = e != 0 ? e : EngineReciprocal(i);
    }
}

void BuildThresholdTable(const int32_t* source, const int32_t* recip, int recipCount, int32_t* out, int count) {
    int32_t src[kThresholdSourceEntries];
    std::memcpy(src, source, sizeof(src));
    for (int i = 1; i < kThresholdSourceEntries; ++i) {
        if (src[i] == 0) {
            src[i] = src[i - 1];
        }
    }
    for (int i = 0; i < count; ++i) {
        const uint32_t idx = std::min<uint32_t>((0x200u + 0x100u * static_cast<uint32_t>(i)) >> 1,
                                                static_cast<uint32_t>(recipCount - 1));
        const int32_t numerator =
            static_cast<int32_t>(static_cast<uint32_t>(src[std::min(i, kThresholdSourceEntries - 1)]) << 16);
        out[i] = recip[idx] != 0 ? numerator / recip[idx] : 0;
    }
}

uint32_t VerifyDrawPatchSites(const uint8_t* moduleBase) {
    for (const DrawPatchSite& site : kSites) {
        uint8_t expected[12];
        RelocatedExpected(site, moduleBase, expected);
        if (std::memcmp(moduleBase + (site.va - kPreferredBase), expected, site.length) != 0) {
            return site.va;
        }
    }
    return 0;
}

bool ApplyDrawPatches(DrawPatchState& state, uint8_t* moduleBase, const DrawTargets& targets) {
    if (state.applied || moduleBase == nullptr || VerifyDrawPatchSites(moduleBase) != 0) {
        return false;
    }
    for (size_t i = 0; i < kDrawPatchSiteCount; ++i) {
        std::memcpy(state.saved[i], SiteAddress(moduleBase, kSites[i]), kSites[i].length);
    }
    for (size_t i = 0; i < kDrawPatchSiteCount; ++i) {
        const DrawPatchSite& site = kSites[i];
        uint8_t bytes[12];
        std::memcpy(bytes, state.saved[i], site.length);
        const uint32_t value = DrawPatchFieldValue(site, targets);
        if (site.fieldSize == 4) {
            std::memcpy(bytes + site.fieldOffset, &value, sizeof(value));
        } else {
            bytes[site.fieldOffset] = static_cast<uint8_t>(value);
        }
        if (!WriteBytes(SiteAddress(moduleBase, site), bytes, site.length)) {
            for (size_t j = 0; j < i; ++j) {
                WriteBytes(SiteAddress(moduleBase, kSites[j]), state.saved[j], kSites[j].length);
            }
            return false;
        }
    }
    state.moduleBase = moduleBase;
    state.applied = true;
    return true;
}

bool RevertDrawPatches(DrawPatchState& state) {
    if (!state.applied) {
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < kDrawPatchSiteCount; ++i) {
        ok = WriteBytes(SiteAddress(state.moduleBase, kSites[i]), state.saved[i], kSites[i].length) && ok;
    }
    state.applied = false;
    state.moduleBase = nullptr;
    return ok;
}

// ---------------------------------------------------------------------------------------------
// Runtime state (game thread only: the race init detour and hook removal).
namespace {

// The bytes before the table are copied too: a negative index read the engine's neighbouring
// .data and now reads that copy instead of whatever precedes this array.
alignas(64) int32_t g_reciprocal[kReciprocalGuardEntries + kDrawReciprocalEntries];
alignas(64) int32_t g_threshold[kMaxDrawSegments + 2];
alignas(64) uint8_t g_block[kDrawBlockSize];
DrawPatchState g_state;
int g_appliedSegments = kEngineDrawSegments;
const uint8_t* g_sliceOverride = nullptr;
bool g_unavailable = false;  // a check failed once: logged, never retried

// What FUN_0045aa90 does for the moved arrays (it does it again inside race init): caches cleared,
// each slice's row pointers (+0x14 -> +0x1c, +0x18 -> +0x120) and the rows' back pointers
// (row + 8 -> slice).
void InitBlock(const DrawLayout& layout, int segments) {
    std::memset(g_block, 0, sizeof(g_block));
    const uint32_t slices = static_cast<uint32_t>(segments) + 2;
    for (uint32_t i = 0; i < slices; ++i) {
        uint8_t* s = reinterpret_cast<uint8_t*>(layout.sliceArray) + i * kDrawSliceStride;
        const uint32_t self = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s));
        const uint32_t row1 = self + 0x1c;
        const uint32_t row2 = self + 0x120;
        std::memcpy(s + 0x14, &row1, 4);
        std::memcpy(s + 0x18, &row2, 4);
        std::memcpy(s + 0x1c + 8, &self, 4);
        std::memcpy(s + 0x120 + 8, &self, 4);
    }
}

void MarkUnavailable(const char* why, uint32_t va) {
    g_unavailable = true;
    LOG_WARN("Draw distance: %s (VA 0x%08X); the engine keeps %d segments", why, va, kEngineDrawSegments);
}

}  // namespace

int DrawDistanceRaceStart(HMODULE module, int segments, bool enable) {
    uint8_t* base = reinterpret_cast<uint8_t*>(module);
    segments = std::clamp(segments, kEngineDrawSegments, kMaxDrawSegments);
    const bool want = enable && segments > kEngineDrawSegments && base != nullptr;
    if (g_state.applied && (!want || segments != g_appliedSegments)) {
        const bool ok = RevertDrawPatches(g_state);
        g_appliedSegments = kEngineDrawSegments;
        g_sliceOverride = nullptr;
        LOG_INFO("Draw distance: back to the engine's %d segments%s", kEngineDrawSegments,
                 ok ? "" : " (a page protection change failed while restoring)");
    }
    if (!want) {
        return kEngineDrawSegments;
    }
    if (g_state.applied) {
        InitBlock(ComputeDrawLayout(reinterpret_cast<uintptr_t>(g_block), segments), segments);
        return g_appliedSegments;
    }
    if (g_unavailable) {
        return kEngineDrawSegments;
    }
    const uint32_t mismatch = VerifyDrawPatchSites(base);
    if (mismatch != 0) {
        MarkUnavailable("unexpected engine bytes", mismatch);
        return kEngineDrawSegments;
    }
    const int32_t* engineRecip = reinterpret_cast<const int32_t*>(base + (kVaReciprocal - kPreferredBase));
    if (!EngineReciprocalsLookShipped(engineRecip)) {
        MarkUnavailable("the reciprocal table is not the shipped one", kVaReciprocal);
        return kEngineDrawSegments;
    }
    std::memcpy(g_reciprocal, engineRecip - kReciprocalGuardEntries, kReciprocalGuardEntries * sizeof(int32_t));
    int32_t* recip = g_reciprocal + kReciprocalGuardEntries;
    BuildReciprocalTable(engineRecip, recip, kDrawReciprocalEntries);
    std::memset(g_threshold, 0, sizeof(g_threshold));
    BuildThresholdTable(reinterpret_cast<const int32_t*>(base + (kVaThresholdSource - kPreferredBase)), recip,
                        kDrawReciprocalEntries, g_threshold, segments + 1);
    const DrawLayout layout = ComputeDrawLayout(reinterpret_cast<uintptr_t>(g_block), segments);
    InitBlock(layout, segments);

    DrawTargets t;
    t.segments = segments;
    t.sliceArray = static_cast<uint32_t>(layout.sliceArray);
    t.cache1 = static_cast<uint32_t>(layout.cache1);
    t.cache2 = static_cast<uint32_t>(layout.cache2);
    t.threshold = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_threshold));
    t.reciprocal = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(recip));
    if (!ApplyDrawPatches(g_state, base, t)) {
        MarkUnavailable("a page protection change failed; nothing was changed", 0);
        return kEngineDrawSegments;
    }
    g_appliedSegments = segments;
    g_sliceOverride = reinterpret_cast<const uint8_t*>(layout.sliceArray);
    LOG_INFO("Draw distance: %d segments for this race (%u sites patched; slices at 0x%08X, row caches at "
             "0x%08X/0x%08X, reciprocal table of %d entries at 0x%08X)",
             segments, static_cast<unsigned>(kDrawPatchSiteCount), t.sliceArray, t.cache1, t.cache2,
             kDrawReciprocalEntries, t.reciprocal);
    return segments;
}

void DrawDistanceShutdown() {
    if (g_state.applied) {
        RevertDrawPatches(g_state);
    }
    g_appliedSegments = kEngineDrawSegments;
    g_sliceOverride = nullptr;
}

const uint8_t* DrawDistanceSliceArrayOverride() {
    return g_sliceOverride;
}

int32_t DrawDistanceSliceMax() {
    return g_appliedSegments;
}

}  // namespace rr95
