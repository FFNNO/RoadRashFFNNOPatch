#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>

// HUD layer transparency (T5b item 5).
// With GpuWorld=1 the 8bpp surface is cleared to a sentinel palette index before the HUD writers
// run, and Layer 3 composites every pixel that differs from that sentinel. A world-coverage mask
// cannot replace the sentinel, because HUD writers draw on top of covered pixels, and it cannot be
// proven that no HUD writer ever produces a given index (GDI text and needles pick the nearest
// palette entry). Indices 0x7E and 0x60 are the only two never present in the 7 dumped software
// frames, so they start as the two candidates. The candidates alternate every frame; a candidate
// seen in the HUD output of a frame where it was not the clear value was drawn by a HUD writer,
// and is replaced by the least recently seen index. A collision therefore lasts at most one frame.
inline constexpr uint8_t kHudSentinelInitialA = 0x7Eu;
inline constexpr uint8_t kHudSentinelInitialB = 0x60u;

struct HudSentinelState {
    uint8_t candidates[2] = {kHudSentinelInitialA, kHudSentinelInitialB};
    uint32_t active = 0;              // index into candidates used as the clear value this frame
    uint32_t frame = 0;               // frames observed so far
    uint32_t lastSeen[256] = {};      // 0 = never seen in HUD output, otherwise frame + 1
};

uint8_t HudSentinelActive(const HudSentinelState& state);
// hist[i] = number of HUD-layer pixels with index i after the HUD writers ran on a surface cleared
// to HudSentinelActive(). Returns true when the inactive candidate was retired and replaced.
bool HudSentinelObserve(HudSentinelState& state, const uint32_t hist[256], uint8_t* outRetired = nullptr,
                        uint8_t* outReplacement = nullptr);
void HudSentinelAdvance(HudSentinelState& state);

enum class QuadKind : uint8_t {
    Textured = 0,
    Flat = 1
};

struct CapturedVertex {
    int16_t x;
    int16_t y;
    int16_t u;
    int16_t v;
};

struct CapturedQuad {
    uint32_t drawOrder;
    uint32_t batchIndex;
    QuadKind kind;
    bool culled;
    uint8_t uFlip;
    uint8_t vFlip;
    uint8_t flatColor;
    uint8_t spriteClut;
    int32_t spriteIndex;
    uint32_t ccbFlags;
    uint32_t pdatPtr;
    uint32_t pdatMagic;
    uint32_t pdatFlags;
    int32_t texWidth;
    int32_t texHeight;
    int32_t mipLevels;
    int32_t chosenMip;
    uint32_t level0Crc32;
    int32_t clipRect[4];
    int32_t uvBox[4];
    int32_t corners16_16[8];
    int32_t snappedPreFix[8];
    int32_t snapShift;   // DAT_005aaa14 at capture time (pixel = corner / 2^snapShift)
    uint32_t snapMask;   // DAT_005aaa18 at capture time
    CapturedVertex verts[4];
};

// Per-frame bookkeeping done after every race present (hook_sites.cpp FinishRaceFrameAndPollHotkeys).
struct RaceFrameHotkeyState {
    bool prevKeyDown = false;
};

struct RaceFrameActions {
    bool endCaptureFrame = false;  // reset the per-frame capture state (CaptureEndFrame)
    bool toggleGpuWorld = false;   // rising edge of the configured toggle key
};

RaceFrameActions AdvanceRaceFrame(RaceFrameHotkeyState& state,
                                  bool captureEnabled,
                                  bool gpuWorld,
                                  uint32_t toggleKey,
                                  bool keyDown);

struct CaptureFrameCounters {
    uint32_t capturedCount = 0;
    uint32_t overflowCount = 0;
    uint32_t batchIndex = 0;
    bool dumpThisFrame = false;
    bool horizonCapturedThisFrame = false;
};

struct LevelStateSnapshot {
    bool initialized = false;
    uint32_t globalRsc = 0;
    uint32_t trackRsc = 0;
    uint32_t spritePdat = 0;
    uint32_t bikerBuf = 0;
    uint32_t familySlots[4] = {0, 0, 0, 0};
    uint32_t lastRaceTick = 0;
};

// Pure helper functions (exposed for unit tests in hook_test.cpp):
uint32_t ComputeTextureRectCrc32(const uint8_t* level0Ptr,
                                 int32_t width,
                                 int32_t height,
                                 int32_t rowPitch,
                                 const uint8_t* extraData = nullptr,
                                 size_t extraLen = 0);
void ClampSpriteDimensions(const int32_t uvBox[4],
                           int32_t pdatPitch,
                           int32_t pdatHeight,
                           int32_t& outW,
                           int32_t& outH);
bool UpdateLevelStateSnapshot(LevelStateSnapshot& snap,
                              uint32_t globalRsc,
                              uint32_t trackRsc,
                              uint32_t spritePdat,
                              uint32_t bikerBuf,
                              const uint32_t familySlots[4],
                              uint32_t raceTick);
// What a level-state change does to the capture caches. The CRC cache is cleared at once so new
// captures hash fresh texels. The texel cache, and the generation bump that makes the presenter drop
// its SRVs, is cleared only before the first batch of a frame: quads already captured this frame
// still need their texels when the presenter draws the frame, or they are drawn white.
enum class CacheInvalidation { None, Full, CrcNowTexelsNextFrame };
CacheInvalidation DecideCacheInvalidation(bool levelChanged, uint32_t batchIndex);
bool ShouldCullTexturedQuad(const CapturedVertex verts[4],
                            const int32_t clipRect[4],
                            uint32_t pdatPtr);
bool ShouldCullFlatQuad(const CapturedVertex verts[4],
                        const int32_t clipRect[4]);

// Sub-pixel vertices (T6). FUN_00422630/FUN_004227f0 write poly vertex i as corner i (CCB +0x44)
// >> DAT_005aaa14 & DAT_005aaa18; FUN_00468d60 then reorders those four snapped vertices in place
// (and for concave or degenerate input replaces one by the midpoint of two others), and
// FUN_004688e0 copies them in that order, assigning u/v by slot from u_flip/v_flip. So the corner
// of vertex i is the corner whose snapped value equals vertex i exactly. The permutation is taken
// from that exact match; when two corners snap to the same point the tie is broken by the smallest
// distance between each corner and the centre of its snapped pixel. The quad falls back to its
// snapped vertices when no permutation matches exactly (rebuilt or clipped vertex) or when a
// matched corner lies farther than kSubPixelMaxDeltaPx (+1 px when the mask snaps to 2 px) from its
// snapped vertex. Single-scanline quads always use the snapped vertices.
inline constexpr float kSubPixelMaxDeltaPx = 1.5f;

struct SubPixelVertices {
    bool eligible = false;    // not single-scanline and a valid snap shift: corners were considered
    bool useCorners = false;
    float x[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float y[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float maxDelta = 0.0f;   // max |corner - snapped vertex| over the matched corners, in pixels
    int permutation[4] = {0, 1, 2, 3};  // vertex i uses corner permutation[i]
};

SubPixelVertices ResolveSubPixelVertices(const CapturedQuad& q);

void CaptureInit(HMODULE hostModule, const std::wstring& dumpDir);
void CaptureInvalidateCache();
void CaptureOnBatchStart();
void CaptureOnBatchEnd();
int CaptureTexturedQuadBefore();
void CaptureTexturedQuadAfter(int slotIndex, bool rasterizerSkipped);
int CaptureFlatQuadBefore(const int32_t* polyVerts, int edxArg);
// rasterizerSkipped must be the decision the detour used for this call (the original
// FUN_00467cb0 did not run), so the emulated prologue never runs on top of the original one.
void CaptureFlatQuadAfter(int slotIndex, int32_t* polyVerts, bool rasterizerSkipped);
void CaptureOnRacePresent(const uint8_t* surface, int pitch, const uint8_t* palette, int width, int height);
void CaptureEndFrame();
CaptureFrameCounters CaptureGetFrameCounters();

// Texture pixel cache (8bpp indices of every captured texture, read by the presenter).
// Capped by bytes; when an insert would exceed the cap the whole cache is cleared (the presenter
// re-uploads on the next generation) and the eviction is logged.
inline constexpr size_t kDefaultTexturePixelCacheCapBytes = 256u * 1024u * 1024u;
inline constexpr int32_t kMaxTextureMipLevels = 12;
void CaptureSetTexturePixelCacheCap(size_t capBytes);
size_t CaptureGetTexturePixelCacheBytes();
uint32_t CaptureGetTexturePixelCacheEvictions();
// extraLevels[i - 1] points to engine mip level i ((width >> i) x (height >> i), rows packed).
void CaptureCacheTexturePixels(uint32_t pdatPtr,
                               uint32_t level0Crc32,
                               const uint8_t* level0Ptr,
                               int32_t width,
                               int32_t height,
                               int32_t rowPitch,
                               const uint8_t* const* extraLevels = nullptr,
                               int32_t extraLevelCount = 0);
// Number of usable levels for a width x height chain claiming levelCount levels (1..12, stops
// before a dimension would reach 0).
int32_t ClampMipLevelCount(int32_t width, int32_t height, int32_t levelCount);

struct TextureMipChainView {
    int32_t levels = 0;
    int32_t width[kMaxTextureMipLevels] = {};
    int32_t height[kMaxTextureMipLevels] = {};
    const uint8_t* pixels[kMaxTextureMipLevels] = {};
};
bool CaptureLookupTextureMipChain(uint32_t pdatPtr, uint32_t level0Crc32, TextureMipChainView& out);

// HUD sentinel for the current frame and the observation hook called by the presenter.
uint8_t CaptureGetHudSentinel();
void CaptureObserveHudLayer(const uint32_t hist[256]);

// GPU World (Task T5) query API:
bool CaptureShouldSkipSoftwareRasterizer();
bool CaptureHasWorldFrame();
const CapturedQuad* CaptureGetWorldQuads(uint32_t& outCount);
const uint8_t* CaptureGetHorizonLayer(int32_t& outHorizonHeight);
const uint8_t* CaptureLookupTexturePixels(uint32_t pdatPtr,
                                          uint32_t level0Crc32,
                                          int32_t& outWidth,
                                          int32_t& outHeight);
uint32_t CaptureGetCacheGeneration();
uint32_t CaptureGetFrameNumber();

bool WriteIndexedPng(const std::wstring& path, const uint8_t* surface, int pitch, const uint8_t* palette, int width, int height);
bool WriteFrameJsonFromQuads(const std::wstring& path,
                             uint32_t frameNum,
                             int width,
                             int height,
                             int32_t snapShift,
                             uint32_t snapMask,
                             const CapturedQuad* quads,
                             uint32_t quadCount,
                             uint32_t overflowCount);
