#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

// Formats the canonical texture ID: "<w>x<h>_<crc32 as 8 lowercase hex digits>"
std::string FormatTextureId(int32_t width, int32_t height, uint32_t level0Crc32);

// Validates whether level-i texel pointer is non-null, points to committed readable memory,
// and fits (width >> level) x (height >> level) texels.
bool ValidateMipLevelPointer(const void* ptr, int32_t width, int32_t height, int32_t level);

// Saves RGBA8 pixels (stride width * 4) as a PNG using WIC.
bool SaveRgbaPngWithWic(const std::wstring& path, int32_t width, int32_t height, const uint32_t* rgbaPixels);

// Loads an RGBA8 PNG using WIC and returns premultiplied RGBA8 pixels.
bool LoadRgbaPngWithWic(const std::wstring& path, int32_t& outW, int32_t& outH, std::vector<uint32_t>& outPremulRgba);

// Validates whether (replW, replH) is k * (origW, origH) with k in {1, 2, 4, 8}.
bool ValidateReplacementDimensions(int32_t origW, int32_t origH, int32_t replW, int32_t replH, int32_t& outK);

// Converts straight RGBA8 to premultiplied RGBA8.
void ConvertToPremultipliedRgba(const uint8_t* srcRgba, int32_t width, int32_t height, uint32_t* dstRgba);

// Generates a complete mip chain down to 1x1 using a 2x2 box filter on premultiplied RGBA8 pixels.
int32_t ComputeMipLevelCount(int32_t width, int32_t height);
bool GeneratePremultipliedMipChain(int32_t baseW, int32_t baseH, const uint32_t* basePixels,
                                   std::vector<std::vector<uint32_t>>& outMips);

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

// --- Work Unit 2: Replacement Texture Pack Manager ---

struct ReplacementTexture {
    ComPtr<ID3D11ShaderResourceView> srv;
    int32_t width = 0;
    int32_t height = 0;
    int32_t k = 1;
    int32_t levels = 1;
    size_t totalBytes = 0;
};

struct TexturePackStats {
    uint32_t loadedCount = 0;
    uint32_t pendingCount = 0;
    uint32_t rejectedCount = 0;
    size_t totalBytes = 0;
};

void TexturePackInit(const std::wstring& packDir, int32_t maxMiB, int32_t decodeBudgetMs);
void TexturePackShutdown();
void TexturePackClearD3DResources();
void TexturePackUpdate(ID3D11Device* device);
ReplacementTexture* TexturePackGetOrQueue(const std::string& id, int32_t origW, int32_t origH);
TexturePackStats TexturePackGetStats();
bool TexturePackHasIndexed(const std::string& id);
