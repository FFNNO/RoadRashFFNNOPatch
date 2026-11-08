#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>

inline constexpr uint8_t kHudTransparentSentinel = 0x7Eu;

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
    CapturedVertex verts[4];
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
bool ShouldCullTexturedQuad(const CapturedVertex verts[4],
                            const int32_t clipRect[4],
                            uint32_t pdatPtr);
bool ShouldCullFlatQuad(const CapturedVertex verts[4],
                        const int32_t clipRect[4]);

void CaptureInit(HMODULE hostModule, const std::wstring& dumpDir);
void CaptureInvalidateCache();
void CaptureOnBatchStart();
void CaptureOnBatchEnd();
int CaptureTexturedQuadBefore();
void CaptureTexturedQuadAfter(int slotIndex);
int CaptureFlatQuadBefore(const int32_t* polyVerts, int edxArg);
void CaptureFlatQuadAfter(int slotIndex, int32_t* polyVerts);
void CaptureOnRacePresent(const uint8_t* surface, int pitch, const uint8_t* palette, int width, int height);
void CaptureEndFrame();

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
