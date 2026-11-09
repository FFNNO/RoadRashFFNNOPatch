#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

using Microsoft::WRL::ComPtr;

// Formats the canonical texture ID: "<w>x<h>_<crc32 as 8 lowercase hex digits>"
std::string FormatTextureId(int32_t width, int32_t height, uint32_t level0Crc32);

// Validates whether level-i texel pointer is non-null, points to committed readable memory,
// and fits (width >> level) x (height >> level) texels.
bool ValidateMipLevelPointer(const void* ptr, int32_t width, int32_t height, int32_t level);

// Saves RGBA8 pixels (stride width * 4) as a PNG using WIC.
bool SaveRgbaPngWithWic(const std::wstring& path, int32_t width, int32_t height, const uint32_t* rgbaPixels);

// Loads an RGBA8 PNG using WIC and returns premultiplied RGBA8 pixels.
// If origW > 0 and origH > 0, validates that image size is a power-of-two multiple (1,2,4,8) before allocating pixel buffers.
bool LoadRgbaPngWithWic(const std::wstring& path, int32_t origW, int32_t origH,
                        int32_t& outW, int32_t& outH, int32_t& outK,
                        std::vector<uint32_t>& outPremulRgba);

enum class PngLoadResult {
    Ok,
    SizeRejected,  // outW/outH hold the PNG size, which is not a valid multiple of the original
    DecodeFailed,  // outHr holds the failing HRESULT
};

// Same as LoadRgbaPngWithWic, but tells a size rejection apart from a decode failure.
PngLoadResult LoadRgbaPngWithWicEx(const std::wstring& path, int32_t origW, int32_t origH,
                                   int32_t& outW, int32_t& outH, int32_t& outK,
                                   std::vector<uint32_t>& outPremulRgba, HRESULT& outHr);

// "TexturePack: <id> rejected (<W>x<H> is not a power-of-two multiple of <w>x<h>)"
std::string FormatReplacementRejectLog(const std::string& id, int32_t replW, int32_t replH,
                                       int32_t origW, int32_t origH);

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
    bool colorKeyKnown = true;    // false: recovered from a disk scan, "color_key" is omitted
    int32_t mipLevels = 1;
    std::string kind = "texture"; // "texture" or "sprite"
    bool kindKnown = true;        // false: recovered from a disk scan, "kind" is omitted
    uint8_t spriteClut = 0;
    uint32_t frameNumber = 0;
};

// Manifest file I/O helpers (exposed for testing)
bool LoadDumpManifest(const std::wstring& manPath,
                      std::unordered_map<std::string, TextureDumpInfo>& entries,
                      std::unordered_set<std::string>& dumpedSet);
// Writes manifest.json.tmp and moves it over manPath. On any write, close or move error the tmp
// file is deleted, the old manifest is kept and false is returned.
bool SaveDumpManifest(const std::wstring& manPath,
                      const std::unordered_map<std::string, TextureDumpInfo>& entries);

// True when fileName matches ^[0-9]+x[0-9]+_[0-9a-f]{8}\.png$ (extension case-insensitive).
bool IsTextureIdPngName(const std::wstring& fileName);

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
uint64_t TexturePackGetGeneration();
ReplacementTexture* TexturePackGetOrQueue(const std::string& id, int32_t origW, int32_t origH);
TexturePackStats TexturePackGetStats();
bool TexturePackHasIndexed(const std::string& id);

// Marks DLL detach (process exit or FreeLibrary). From then on the worker destructors never join
// and never block on a mutex: they set the stop flag and detach the worker thread.
void TexturePackSetProcessTerminating();

enum class ReplacementState {
    None,     // no replacement will arrive (not indexed, rejected or over MaxMiB)
    Pending,  // queued or decoding; it becomes Ready or None in a later TexturePackUpdate
    Ready,    // the replacement SRV exists
};

// Same as TexturePackGetOrQueue; *outPending is true when the replacement is queued or decoding.
ReplacementTexture* TexturePackGetOrQueue(const std::string& id, int32_t origW, int32_t origH,
                                          bool* outPending);
ReplacementState TexturePackGetState(const std::string& id);

// On a pack generation change, erases only the cache entries that were built from the original
// texture while their replacement was pending and whose replacement is now ready. Entries whose
// replacement can no longer arrive keep their original SRV and lose the pending mark.
// Cache is a map whose mapped type has a std::string member named pendingReplacementId.
template <typename Cache, typename StateFn>
size_t InvalidateReadyReplacementEntries(Cache& cache, StateFn&& stateOf) {
    size_t erased = 0;
    for (auto it = cache.begin(); it != cache.end();) {
        std::string& pendingId = it->second.pendingReplacementId;
        if (pendingId.empty()) {
            ++it;
            continue;
        }
        const ReplacementState state = stateOf(pendingId);
        if (state == ReplacementState::Ready) {
            it = cache.erase(it);
            ++erased;
            continue;
        }
        if (state == ReplacementState::None) {
            pendingId.clear();
        }
        ++it;
    }
    return erased;
}

// Test seam: starts a dumper and a pack worker in dir, lets both worker threads return while their
// std::thread objects stay joinable (the state a thread killed at process exit leaves), simulates
// process termination and destroys both objects. Returns false if a worker did not exit in time.
bool TexturePackTestDestroyAtProcessExit(const std::wstring& dir);
