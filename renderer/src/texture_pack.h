#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <deque>
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

// Parses the original size encoded in a texture ID "<w>x<h>_<crc32 as 8 lowercase hex digits>".
// Uses the IsTextureIdPngName rule on "<id>.png". False when the ID does not match it or when
// w or h is outside 1..8192; outW and outH are 0 then.
bool ParseTextureIdSize(const std::string& id, int32_t& outW, int32_t& outH);

// The game's full-brightness palette, relative to the game folder: 256 entries x 4 bytes (RGBx).
constexpr const wchar_t* kBasePaletteRelPath = L"DATA\\PALETTE.RAW";
constexpr size_t kBasePaletteBytes = 1024;

// Reads a PALETTE.RAW file into outPalette. False when the file is missing or is not exactly
// kBasePaletteBytes long; outPalette is left untouched then.
bool LoadBasePaletteFile(const std::wstring& path, uint8_t (&outPalette)[kBasePaletteBytes]);

// Per-channel fade of the current palette against the base one:
// outFade[c] = sum(current[1..255].c) / sum(base[1..255].c), clamped to [0, 1]. Index 0 is excluded.
// A null base, or a channel whose base sum is 0, gives 1.
void ComputePaletteFadeFactor(const uint8_t* basePalette, const uint8_t* currentPalette, float outFade[3]);

// True when at least 95% of entries 1..255 of currentPalette are within 3 of round(base * fade) on
// every channel, i.e. the current palette is about a uniform scale of the base one.
bool PaletteIsUniformScale(const uint8_t* basePalette, const uint8_t* currentPalette, const float fade[3]);

// Starts the dumper. basePalettePath is the game's PALETTE.RAW: dumps are converted with that
// palette, never with the frame's palette. When it is missing or not 1024 bytes, dumping is disabled.
void TextureDumpInit(const std::wstring& dumpDir, const std::wstring& basePalettePath);
void TextureDumpShutdown();
// Queues level 0 of a texture; it is converted with the base palette loaded at TextureDumpInit.
void TextureDumpQueue(const TextureDumpInfo& info, const uint8_t* level0Indices);
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

struct PackLoadRequest {
    std::string id;
    std::wstring filePath;
    int32_t origW = 0;
    int32_t origH = 0;
};

// Decode queue of the texture pack. On-demand requests (a texture drawn now) are always served
// before preload requests, and each kind is served in FIFO order. Not thread-safe: the owner locks.
class PackLoadQueue {
public:
    void PushPreload(PackLoadRequest req);
    void PushDemand(PackLoadRequest req);
    // Moves a still-queued preload request for id behind the on-demand requests, ahead of every
    // other preload request. False when id is not waiting as a preload request.
    bool Promote(const std::string& id);
    // Pops the next request: on-demand first, then preload only when allowPreload is true.
    bool PopNext(PackLoadRequest& out, bool allowPreload);
    bool HasDemand() const { return !m_demand.empty(); }
    bool HasPreload() const { return !m_preload.empty(); }
    size_t DemandSize() const { return m_demand.size(); }
    size_t PreloadSize() const { return m_preload.size(); }
    // Drops every queued preload request and returns their IDs; on-demand requests stay.
    std::vector<std::string> ClearPreload();
    void Clear();

private:
    std::deque<PackLoadRequest> m_demand;
    std::deque<PackLoadRequest> m_preload;
};

struct TexturePackStats {
    uint32_t loadedCount = 0;
    uint32_t pendingCount = 0;
    uint32_t rejectedCount = 0;
    size_t totalBytes = 0;
};

// Indexes <packDir>/*.png. With preload, every file whose name is a texture ID is queued for a
// background decode at a lower priority than on-demand requests; TexturePackUpdate still creates
// the D3D textures on the game thread within decodeBudgetMs.
void TexturePackInit(const std::wstring& packDir, int32_t maxMiB, int32_t decodeBudgetMs, bool preload);
void TexturePackShutdown();
void TexturePackClearD3DResources();
// budgetMsOverride > 0 replaces DecodeBudgetMs for this call (used by the menu present path, where
// a longer frame is not visible, so the preload finishes before the race).
void TexturePackUpdate(ID3D11Device* device, int32_t budgetMsOverride = 0);
uint64_t TexturePackGetGeneration();
// Number of PNG files indexed by the last TexturePackInit (0 when not initialized). Lock-free.
uint32_t TexturePackGetIndexedCount();
ReplacementTexture* TexturePackGetOrQueue(const std::string& id, int32_t origW, int32_t origH);
TexturePackStats TexturePackGetStats();
bool TexturePackHasIndexed(const std::string& id);

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
