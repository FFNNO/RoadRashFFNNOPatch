#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

// Formats the canonical texture ID: "<w>x<h>_<crc32 as 8 lowercase hex digits>"
std::string FormatTextureId(int32_t width, int32_t height, uint32_t level0Crc32);

// Validates whether level-i texel pointer is non-null, points to committed readable memory,
// and fits (width >> level) x (height >> level) texels.
bool ValidateMipLevelPointer(const void* ptr, int32_t width, int32_t height, int32_t level);

// Saves RGBA8 pixels (stride width * 4) as a PNG using WIC.
bool SaveRgbaPngWithWic(const std::wstring& path, int32_t width, int32_t height, const uint32_t* rgbaPixels);

struct TextureDumpInfo {
    std::string id;
    int32_t width = 0;
    int32_t height = 0;
    bool colorKey = false;
    int32_t mipLevels = 1;
    std::string kind = "texture"; // "texture" or "sprite"
    uint8_t spriteClut = 0;
    uint32_t frameNumber = 0;
};

void TextureDumpInit(const std::wstring& dumpDir);
void TextureDumpShutdown();
void TextureDumpQueue(const TextureDumpInfo& info,
                      const uint8_t* level0Indices,
                      const uint8_t* palette);
uint32_t TextureDumpGetDumpedCount();
uint32_t TextureDumpGetQueuedCount();
