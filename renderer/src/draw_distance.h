#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>

// Larger road draw distance (feature rr95-draw-distance, task D3).
//
// FUN_0045ac50 asks FUN_00459cb0 for 0x31 track segments ahead of the camera (push 0x31 at
// 0x45aca3). FUN_00459cb0 writes one road slice per segment into DAT_0060bd00 (stride 0x224, 0x32
// slices, two rows inline at +0x1c and +0x120), keys two row caches DAT_006084a0 / DAT_0060a0c0
// (0x32 entries of 0x90 each) by segment % 0x31, reads the per-slice crest threshold DAT_00608320
// (0x31 entries) and stores the slice count clamped to [1, 0x30] in DAT_0059ec90. Each row lies
// 0x100 deeper than the previous one, and every projection reads the reciprocal table DAT_00597c90
// (0x1c00 entries, 2^24 / z at index z >> 1, index clamped to 0x1bff), which saturates past ~54
// rows. Raising the count to N therefore moves those arrays into this DLL (sized for N), rewrites
// every absolute reference to them and every constant that encodes the cap, and extends the
// reciprocal table. The full byte-verified patch list is in draw_distance.cpp.

namespace rr95 {

inline constexpr int kEngineDrawSegments = 48;      // DAT_0059ec90 cap 0x30
// The track ring DAT_005f3a80 holds 128 segments streamed from 48 behind to 80 ahead of the camera
// (FUN_00474970 moves both edges with the camera position DAT_00603714 = DAT_00612834). FUN_00459cb0
// touches segments up to camera + N + 1; 72 keeps that 6 segments inside the loaded window.
inline constexpr int kMaxDrawSegments = 72;
inline constexpr int kEngineReciprocalEntries = 0x1c00;
inline constexpr int kDrawReciprocalEntries = 0x4000;  // z up to 0x7ffe: rows up to ~126
// FUN_0045aa90 computes the entries the shipped .data leaves 0 (index 0x1100 and up).
inline constexpr int kFirstComputedReciprocal = 0x1100;
inline constexpr int kReciprocalGuardEntries = 0x1000;  // bytes before the table, copied as they are
inline constexpr int kThresholdSourceEntries = 0x32;    // DAT_0059ec98
inline constexpr uint32_t kDrawSliceStride = 0x224;
inline constexpr uint32_t kDrawRowCacheStride = 0x90;
// The engine keeps the horizon DAT_0060bce0 in the 0x20 bytes between the second row cache and
// the slice array; the relocated layout keeps the same gap so slice[-1] reads land where they did.
inline constexpr uint32_t kDrawCacheSliceGap = 0x20;

// [World] DrawSegments: a decimal integer. Below 48 or not a number: 48 and false. Above
// kMaxDrawSegments: kMaxDrawSegments and false. Otherwise the value and true.
bool ParseDrawSegments(const wchar_t* str, int& out);

enum class DrawField : uint8_t {
    DrawPush,         // imm8  N + 1 (segments requested)
    DrawCap,          // imm   N (count clamp)
    CacheEntries,     // imm32 N + 2 (row cache zero loop)
    SliceEntries,     // imm32 N + 2 (slice init loop)
    SliceAddr,        // abs   slice array (0x0060bd00-relative)
    Cache1Addr,       // abs   first row cache (0x006084a0)
    Cache2Addr,       // abs   second row cache (0x0060a0c0)
    ThresholdAddr,    // abs   crest thresholds (0x00608320)
    ReciprocalAddr,   // abs   reciprocal table (0x00597c90)
    ReciprocalMax,    // imm32 kDrawReciprocalEntries - 1 (was 0x1bff)
    ReciprocalLimit   // imm32 kDrawReciprocalEntries (was 0x1c00)
};

struct DrawPatchSite {
    uint32_t va;             // preferred VA of the instruction
    uint8_t length;          // instruction length
    uint8_t expected[12];    // instruction bytes at the preferred base (both exe builds)
    int8_t absoluteOffset;   // offset of a 4-byte absolute operand the loader relocates, or -1
    uint8_t fieldOffset;     // offset of the rewritten field
    uint8_t fieldSize;       // 1 or 4
    DrawField field;
};

inline constexpr size_t kDrawPatchSiteCount = 103;
const DrawPatchSite* DrawPatchSites();

// Runtime addresses of the relocated arrays (this process).
struct DrawTargets {
    int segments = kEngineDrawSegments;
    uint32_t sliceArray = 0;   // slice 0
    uint32_t cache1 = 0;
    uint32_t cache2 = 0;
    uint32_t threshold = 0;
    uint32_t reciprocal = 0;   // entry 0
};

// The value written into site's field for targets.
uint32_t DrawPatchFieldValue(const DrawPatchSite& site, const DrawTargets& targets);

// Caches and slices inside one block: the slice array at a fixed offset (sized for
// kMaxDrawSegments), the second cache ending kDrawCacheSliceGap bytes before it and the first cache
// right before the second, both with segments + 2 entries, as in the engine's .data.
inline constexpr size_t kDrawCacheEntriesMax = static_cast<size_t>(kMaxDrawSegments) + 2;
inline constexpr size_t kDrawSliceBlockOffset = 2 * kDrawCacheEntriesMax * kDrawRowCacheStride + kDrawCacheSliceGap;
inline constexpr size_t kDrawBlockSize = kDrawSliceBlockOffset + kDrawCacheEntriesMax * kDrawSliceStride;
struct DrawLayout {
    uintptr_t cache1 = 0;
    uintptr_t cache2 = 0;
    uintptr_t sliceArray = 0;
};
DrawLayout ComputeDrawLayout(uintptr_t blockStart, int segments);

// FUN_0045aa90 (0x45aac1..0x45aae3): an entry left 0 becomes 2^24 / (2 * index) (64-bit signed).
int32_t EngineReciprocal(int index);
// True when engine[0] != 0 and every entry in [kFirstComputedReciprocal, kEngineReciprocalEntries)
// is 0 or EngineReciprocal(index): the table this patch was written for.
bool EngineReciprocalsLookShipped(const int32_t* engine);
// out[i] = engine[i] when i < kEngineReciprocalEntries and engine[i] != 0, else EngineReciprocal(i)
// (the engine's own fill rule), for i in [0, outCount).
void BuildReciprocalTable(const int32_t* engine, int32_t* out, int outCount);
// FUN_0045aa90 (0x45aae5..0x45ab4b): source = DAT_0059ec98 (kThresholdSourceEntries entries) with
// each 0 entry from index 1 copying the previous one; out[i] = int32(source[min(i, 0x31)] << 16) /
// recip[min((0x200 + 0x100 * i) >> 1, recipCount - 1)] for i in [0, count).
void BuildThresholdTable(const int32_t* source, const int32_t* recip, int recipCount, int32_t* out, int count);

// Patch state over an image mapped at moduleBase (preferred VA + moduleBase - 0x400000).
struct DrawPatchState {
    uint8_t* moduleBase = nullptr;
    bool applied = false;
    uint8_t saved[kDrawPatchSiteCount][12] = {};
};
// Preferred VA of the first site whose live bytes differ from the expected ones (absolute operands
// rebased), 0 when every site matches.
uint32_t VerifyDrawPatchSites(const uint8_t* moduleBase);
// All or nothing: verifies every site, then writes every field (VirtualProtect per site). Any
// failure leaves the image unchanged and returns false. Refuses when already applied.
bool ApplyDrawPatches(DrawPatchState& state, uint8_t* moduleBase, const DrawTargets& targets);
// Writes the saved instruction bytes back. No-op when not applied.
bool RevertDrawPatches(DrawPatchState& state);

// Runtime (game thread). At race start, before FUN_00457ef0 runs: applies `segments` when enable is
// true and segments > 48 (verifying every byte first), otherwise reverts to the engine's 48.
// Returns the active segment count.
int DrawDistanceRaceStart(HMODULE module, int segments, bool enable);
// Reverts (hook removal).
void DrawDistanceShutdown();
// The relocated slice array while applied, otherwise nullptr (the engine's DAT_0060bd00).
const uint8_t* DrawDistanceSliceArrayOverride();
// The active count cap: 48 or the applied segments.
int32_t DrawDistanceSliceMax();

}  // namespace rr95
