#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

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

void CaptureInit(HMODULE hostModule, const std::wstring& dumpDir);
void CaptureOnBatchStart();
int CaptureTexturedQuadBefore();
void CaptureTexturedQuadAfter(int slotIndex);
int CaptureFlatQuadBefore();
void CaptureFlatQuadAfter(int slotIndex);
void CaptureOnRacePresent(const uint8_t* surface, int pitch, const uint8_t* palette, int width, int height);
void CaptureEndFrame();
