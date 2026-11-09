#include "texture_pack.h"
#include "log.h"
#include <wincodec.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

std::string FormatTextureId(int32_t width, int32_t height, uint32_t level0Crc32) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%dx%d_%08x", width, height, level0Crc32);
    return std::string(buf);
}

static void CreateDirectoryRecursive(const std::wstring& path) {
    if (path.empty()) {
        return;
    }
    wchar_t buf[MAX_PATH];
    wcsncpy_s(buf, path.c_str(), _TRUNCATE);
    for (wchar_t* p = buf + 1; *p; ++p) {
        if (*p == L'\\' || *p == L'/') {
            *p = 0;
            CreateDirectoryW(buf, nullptr);
            *p = L'\\';
        }
    }
    CreateDirectoryW(buf, nullptr);
}

bool ValidateMipLevelPointer(const void* ptr, int32_t width, int32_t height, int32_t level) {
    if (!ptr || width <= 0 || height <= 0 || level < 0) {
        return false;
    }
    const int32_t w = width >> level;
    const int32_t h = height >> level;
    if (w <= 0 || h <= 0) {
        return false;
    }
    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h);

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) {
        return false;
    }
    const DWORD readableMask = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & readableMask) == 0) {
        return false;
    }
    const uintptr_t start = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t allocEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (start + bytes > allocEnd) {
        return false;
    }
    return true;
}

bool SaveRgbaPngWithWic(const std::wstring& path, int32_t width, int32_t height, const uint32_t* rgbaPixels) {
    if (width <= 0 || height <= 0 || !rgbaPixels) {
        return false;
    }

    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needUninit = (hrCom == S_OK || hrCom == S_FALSE);
    bool success = false;

    {
        ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) {
            ComPtr<IWICStream> stream;
            hr = factory->CreateStream(&stream);
            if (SUCCEEDED(hr)) {
                hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
                if (SUCCEEDED(hr)) {
                    ComPtr<IWICBitmapEncoder> encoder;
                    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
                    if (SUCCEEDED(hr)) {
                        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
                        if (SUCCEEDED(hr)) {
                            ComPtr<IWICBitmapFrameEncode> frame;
                            ComPtr<IPropertyBag2> propBag;
                            hr = encoder->CreateNewFrame(&frame, &propBag);
                            if (SUCCEEDED(hr)) {
                                hr = frame->Initialize(propBag.Get());
                                if (SUCCEEDED(hr)) {
                                    hr = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
                                    if (SUCCEEDED(hr)) {
                                        WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
                                        hr = frame->SetPixelFormat(&format);
                                        if (SUCCEEDED(hr)) {
                                            const UINT stride = static_cast<UINT>(width * 4);
                                            const UINT bufSize = static_cast<UINT>(width * height * 4);
                                            if (IsEqualGUID(format, GUID_WICPixelFormat32bppRGBA)) {
                                                hr = frame->WritePixels(static_cast<UINT>(height), stride, bufSize,
                                                                        reinterpret_cast<BYTE*>(const_cast<uint32_t*>(rgbaPixels)));
                                            } else if (IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) {
                                                std::vector<uint32_t> bgraPixels(static_cast<size_t>(width) * height);
                                                const size_t total = static_cast<size_t>(width) * height;
                                                for (size_t i = 0; i < total; ++i) {
                                                    const uint32_t px = rgbaPixels[i];
                                                    const uint32_t r = px & 0xFFu;
                                                    const uint32_t g = (px >> 8) & 0xFFu;
                                                    const uint32_t b = (px >> 16) & 0xFFu;
                                                    const uint32_t a = (px >> 24) & 0xFFu;
                                                    bgraPixels[i] = b | (g << 8) | (r << 16) | (a << 24);
                                                }
                                                hr = frame->WritePixels(static_cast<UINT>(height), stride, bufSize,
                                                                        reinterpret_cast<BYTE*>(bgraPixels.data()));
                                            } else {
                                                hr = E_FAIL;
                                            }
                                            if (SUCCEEDED(hr)) {
                                                hr = frame->Commit();
                                                if (SUCCEEDED(hr)) {
                                                    hr = encoder->Commit();
                                                    if (SUCCEEDED(hr)) {
                                                        success = true;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (needUninit) {
        CoUninitialize();
    }
    return success;
}

void ConvertToPremultipliedRgba(const uint8_t* srcRgba, int32_t width, int32_t height, uint32_t* dstRgba) {
    if (!srcRgba || !dstRgba || width <= 0 || height <= 0) {
        return;
    }
    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t r = srcRgba[i * 4 + 0];
        const uint8_t g = srcRgba[i * 4 + 1];
        const uint8_t b = srcRgba[i * 4 + 2];
        const uint8_t a = srcRgba[i * 4 + 3];
        if (a == 0) {
            dstRgba[i] = 0x00000000u;
        } else if (a == 255) {
            dstRgba[i] = static_cast<uint32_t>(r) |
                         (static_cast<uint32_t>(g) << 8) |
                         (static_cast<uint32_t>(b) << 16) |
                         0xFF000000u;
        } else {
            const uint32_t pr = (static_cast<uint32_t>(r) * a + 127u) / 255u;
            const uint32_t pg = (static_cast<uint32_t>(g) * a + 127u) / 255u;
            const uint32_t pb = (static_cast<uint32_t>(b) * a + 127u) / 255u;
            dstRgba[i] = pr | (pg << 8) | (pb << 16) | (static_cast<uint32_t>(a) << 24);
        }
    }
}

bool ValidateReplacementDimensions(int32_t origW, int32_t origH, int32_t replW, int32_t replH, int32_t& outK) {
    outK = 1;
    if (origW <= 0 || origH <= 0 || replW <= 0 || replH <= 0) {
        return false;
    }
    const int32_t candidateK[4] = {1, 2, 4, 8};
    for (int32_t k : candidateK) {
        if (replW == origW * k && replH == origH * k) {
            outK = k;
            return true;
        }
    }
    return false;
}

PngLoadResult LoadRgbaPngWithWicEx(const std::wstring& path, int32_t origW, int32_t origH,
                                   int32_t& outW, int32_t& outH, int32_t& outK,
                                   std::vector<uint32_t>& outPremulRgba, HRESULT& outHr) {
    outW = 0;
    outH = 0;
    outK = 1;
    outHr = S_OK;
    outPremulRgba.clear();

    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needUninit = (hrCom == S_OK || hrCom == S_FALSE);
    PngLoadResult result = PngLoadResult::DecodeFailed;
    HRESULT hr = E_FAIL;

    {
        ComPtr<IWICImagingFactory> factory;
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) {
            ComPtr<IWICBitmapDecoder> decoder;
            hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                   WICDecodeMetadataCacheOnDemand, &decoder);
            if (SUCCEEDED(hr)) {
                ComPtr<IWICBitmapFrameDecode> frame;
                hr = decoder->GetFrame(0, &frame);
                if (SUCCEEDED(hr)) {
                    UINT w = 0, h = 0;
                    hr = frame->GetSize(&w, &h);
                    if (SUCCEEDED(hr) && (w == 0 || h == 0)) {
                        hr = WINCODEC_ERR_BADIMAGE;
                    }
                    if (SUCCEEDED(hr)) {
                        const int32_t imgW = static_cast<int32_t>(w);
                        const int32_t imgH = static_cast<int32_t>(h);
                        int32_t matchedK = 1;
                        bool sizeValid = true;
                        if (origW > 0 && origH > 0) {
                            sizeValid = ValidateReplacementDimensions(origW, origH, imgW, imgH, matchedK);
                        } else if (imgW > 8192 || imgH > 8192) {
                            sizeValid = false;
                        }

                        if (!sizeValid) {
                            outW = imgW;
                            outH = imgH;
                            result = PngLoadResult::SizeRejected;
                        } else {
                            outK = matchedK;
                            ComPtr<IWICFormatConverter> converter;
                            hr = factory->CreateFormatConverter(&converter);
                            if (SUCCEEDED(hr)) {
                                hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                                           WICBitmapDitherTypeNone, nullptr, 0.0,
                                                           WICBitmapPaletteTypeCustom);
                                if (SUCCEEDED(hr)) {
                                    std::vector<uint8_t> rawPixels(static_cast<size_t>(w) * h * 4);
                                    const UINT stride = w * 4;
                                    hr = converter->CopyPixels(nullptr, stride, static_cast<UINT>(rawPixels.size()), rawPixels.data());
                                    if (SUCCEEDED(hr)) {
                                        outW = imgW;
                                        outH = imgH;
                                        outPremulRgba.resize(static_cast<size_t>(w) * h);
                                        ConvertToPremultipliedRgba(rawPixels.data(), outW, outH, outPremulRgba.data());
                                        result = PngLoadResult::Ok;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (needUninit) {
        CoUninitialize();
    }
    if (result == PngLoadResult::DecodeFailed) {
        outHr = FAILED(hr) ? hr : E_FAIL;
    }
    return result;
}

bool LoadRgbaPngWithWic(const std::wstring& path, int32_t origW, int32_t origH,
                        int32_t& outW, int32_t& outH, int32_t& outK,
                        std::vector<uint32_t>& outPremulRgba) {
    HRESULT hr = S_OK;
    return LoadRgbaPngWithWicEx(path, origW, origH, outW, outH, outK, outPremulRgba, hr) ==
           PngLoadResult::Ok;
}

std::string FormatReplacementRejectLog(const std::string& id, int32_t replW, int32_t replH,
                                       int32_t origW, int32_t origH) {
    char buf[192];
    std::snprintf(buf, sizeof(buf), "TexturePack: %s rejected (%dx%d is not a power-of-two multiple of %dx%d)",
                  id.c_str(), replW, replH, origW, origH);
    return std::string(buf);
}

bool IsTextureIdPngName(const std::wstring& fileName) {
    // ^[0-9]+x[0-9]+_[0-9a-f]{8}\.png$, with a case-insensitive extension.
    size_t i = 0;
    const size_t n = fileName.size();
    auto isDigit = [](wchar_t c) { return c >= L'0' && c <= L'9'; };
    auto isHex = [&](wchar_t c) { return isDigit(c) || (c >= L'a' && c <= L'f'); };
    const size_t wStart = i;
    while (i < n && isDigit(fileName[i])) ++i;
    if (i == wStart || i >= n || fileName[i] != L'x') return false;
    ++i;
    const size_t hStart = i;
    while (i < n && isDigit(fileName[i])) ++i;
    if (i == hStart || i >= n || fileName[i] != L'_') return false;
    ++i;
    for (int d = 0; d < 8; ++d, ++i) {
        if (i >= n || !isHex(fileName[i])) return false;
    }
    return n - i == 4 && _wcsicmp(fileName.c_str() + i, L".png") == 0;
}

bool ParseTextureIdSize(const std::string& id, int32_t& outW, int32_t& outH) {
    outW = 0;
    outH = 0;
    std::wstring name;
    name.reserve(id.size() + 4);
    for (char ch : id) {
        name.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
    }
    name += L".png";
    if (!IsTextureIdPngName(name)) {
        return false;
    }
    // The name check guarantees "<digits>x<digits>_"; strtoul saturates on overflow.
    char* end = nullptr;
    const unsigned long w = std::strtoul(id.c_str(), &end, 10);
    const unsigned long h = std::strtoul(end + 1, nullptr, 10);
    if (w == 0 || h == 0 || w > 8192 || h > 8192) {
        return false;
    }
    outW = static_cast<int32_t>(w);
    outH = static_cast<int32_t>(h);
    return true;
}

int32_t ComputeMipLevelCount(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) {
        return 0;
    }
    int32_t levels = 1;
    int32_t w = width, h = height;
    while (w > 1 || h > 1) {
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
        ++levels;
    }
    return levels;
}

bool GeneratePremultipliedMipChain(int32_t baseW, int32_t baseH, const uint32_t* basePixels,
                                   std::vector<std::vector<uint32_t>>& outMips) {
    if (baseW <= 0 || baseH <= 0 || !basePixels) {
        return false;
    }
    const int32_t levels = ComputeMipLevelCount(baseW, baseH);
    outMips.resize(levels);
    outMips[0].assign(basePixels, basePixels + (static_cast<size_t>(baseW) * baseH));

    int32_t srcW = baseW;
    int32_t srcH = baseH;
    for (int32_t l = 1; l < levels; ++l) {
        const int32_t dstW = std::max(1, srcW / 2);
        const int32_t dstH = std::max(1, srcH / 2);
        outMips[l].resize(static_cast<size_t>(dstW) * dstH);
        const uint32_t* src = outMips[l - 1].data();
        uint32_t* dst = outMips[l].data();

        for (int32_t dy = 0; dy < dstH; ++dy) {
            const int32_t sy0 = dy * 2;
            const int32_t sy1 = std::min(sy0 + 1, srcH - 1);
            for (int32_t dx = 0; dx < dstW; ++dx) {
                const int32_t sx0 = dx * 2;
                const int32_t sx1 = std::min(sx0 + 1, srcW - 1);

                const uint32_t p00 = src[sy0 * srcW + sx0];
                const uint32_t p10 = src[sy0 * srcW + sx1];
                const uint32_t p01 = src[sy1 * srcW + sx0];
                const uint32_t p11 = src[sy1 * srcW + sx1];

                const uint32_t r = (((p00 & 0xFFu) + (p10 & 0xFFu) + (p01 & 0xFFu) + (p11 & 0xFFu) + 2u) >> 2);
                const uint32_t g = ((((p00 >> 8) & 0xFFu) + ((p10 >> 8) & 0xFFu) + ((p01 >> 8) & 0xFFu) + ((p11 >> 8) & 0xFFu) + 2u) >> 2);
                const uint32_t b = ((((p00 >> 16) & 0xFFu) + ((p10 >> 16) & 0xFFu) + ((p01 >> 16) & 0xFFu) + ((p11 >> 16) & 0xFFu) + 2u) >> 2);
                const uint32_t a = ((((p00 >> 24) & 0xFFu) + ((p10 >> 24) & 0xFFu) + ((p01 >> 24) & 0xFFu) + ((p11 >> 24) & 0xFFu) + 2u) >> 2);

                dst[dy * dstW + dx] = r | (g << 8) | (b << 16) | (a << 24);
            }
        }
        srcW = dstW;
        srcH = dstH;
    }
    return true;
}

// -----------------------------------------------------------------------------
// Manifest file helpers
// -----------------------------------------------------------------------------

bool LoadDumpManifest(const std::wstring& manPath,
                      std::unordered_map<std::string, TextureDumpInfo>& entries,
                      std::unordered_set<std::string>& dumpedSet) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, manPath.c_str(), L"rt") != 0 || !fp) {
        return false;
    }
    std::fseek(fp, 0, SEEK_END);
    const long sz = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (sz <= 0) {
        std::fclose(fp);
        return false;
    }
    std::vector<char> buf(static_cast<size_t>(sz) + 1, 0);
    std::fread(buf.data(), 1, static_cast<size_t>(sz), fp);
    std::fclose(fp);

    const std::string content(buf.data());
    size_t pos = 0;
    while (pos < content.size()) {
        size_t q1 = content.find('"', pos);
        if (q1 == std::string::npos) break;
        size_t q2 = content.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        std::string key = content.substr(q1 + 1, q2 - q1 - 1);

        size_t colon = content.find(':', q2 + 1);
        if (colon == std::string::npos) break;
        size_t brace = content.find('{', colon + 1);
        if (brace == std::string::npos) break;

        bool isSection = true;
        for (size_t i = q2 + 1; i < colon; ++i) {
            if (!isspace(static_cast<unsigned char>(content[i]))) { isSection = false; break; }
        }
        for (size_t i = colon + 1; i < brace; ++i) {
            if (!isspace(static_cast<unsigned char>(content[i]))) { isSection = false; break; }
        }
        if (!isSection) {
            pos = q2 + 1;
            continue;
        }

        size_t endBrace = content.find('}', brace + 1);
        if (endBrace == std::string::npos) break;
        std::string block = content.substr(brace, endBrace - brace + 1);

        TextureDumpInfo info;
        info.id = key;

        auto parseField = [&](const char* fieldName) -> std::string {
            std::string pat = std::string("\"") + fieldName + "\"";
            size_t fpos = block.find(pat);
            if (fpos == std::string::npos) return "";
            size_t fcol = block.find(':', fpos + pat.size());
            if (fcol == std::string::npos) return "";
            size_t valStart = fcol + 1;
            while (valStart < block.size() && isspace(static_cast<unsigned char>(block[valStart]))) ++valStart;
            if (valStart >= block.size()) return "";
            if (block[valStart] == '"') {
                size_t valEnd = block.find('"', valStart + 1);
                if (valEnd == std::string::npos) return "";
                return block.substr(valStart + 1, valEnd - valStart - 1);
            } else {
                size_t valEnd = valStart;
                while (valEnd < block.size() && block[valEnd] != ',' && block[valEnd] != '\n' && block[valEnd] != '}') ++valEnd;
                std::string v = block.substr(valStart, valEnd - valStart);
                while (!v.empty() && isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
                return v;
            }
        };

        std::string sW = parseField("width");
        std::string sH = parseField("height");
        std::string sColorKey = parseField("color_key");
        std::string sMips = parseField("mip_levels");
        std::string sKind = parseField("kind");
        std::string sClut = parseField("sprite_clut");
        std::string sFrame = parseField("first_seen_frame");

        if (!sW.empty()) info.width = std::atoi(sW.c_str());
        if (!sH.empty()) info.height = std::atoi(sH.c_str());
        // A missing color_key or kind means the entry came from a disk scan: keep it unknown.
        info.colorKeyKnown = !sColorKey.empty();
        info.colorKey = (sColorKey == "true");
        if (!sMips.empty()) info.mipLevels = std::atoi(sMips.c_str());
        info.kindKnown = !sKind.empty();
        if (!sKind.empty()) info.kind = sKind;
        if (!sClut.empty()) info.spriteClut = static_cast<uint8_t>(std::atoi(sClut.c_str()));
        if (!sFrame.empty()) info.frameNumber = static_cast<uint32_t>(std::strtoul(sFrame.c_str(), nullptr, 10));

        entries[key] = info;
        dumpedSet.insert(key);

        pos = endBrace + 1;
    }
    return true;
}

bool SaveDumpManifest(const std::wstring& manPath,
                      const std::unordered_map<std::string, TextureDumpInfo>& entries) {
    const std::wstring tmpPath = manPath + L".tmp";
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, tmpPath.c_str(), L"wt") != 0 || !fp) {
        return false;
    }
    bool ok = true;
    auto put = [&](int written) {
        if (written < 0) {
            ok = false;
        }
    };
    put(std::fprintf(fp, "{\n"));
    bool first = true;
    for (const auto& kv : entries) {
        const TextureDumpInfo& e = kv.second;
        if (!first) {
            put(std::fprintf(fp, ",\n"));
        }
        first = false;
        put(std::fprintf(fp, "  \"%s\": {\n", kv.first.c_str()));
        put(std::fprintf(fp, "    \"width\": %d,\n", e.width));
        put(std::fprintf(fp, "    \"height\": %d,\n", e.height));
        // Entries recovered from a disk scan leave color_key and kind out: they are unknown.
        if (e.colorKeyKnown) {
            put(std::fprintf(fp, "    \"color_key\": %s,\n", e.colorKey ? "true" : "false"));
        }
        put(std::fprintf(fp, "    \"mip_levels\": %d,\n", e.mipLevels));
        if (e.kindKnown) {
            put(std::fprintf(fp, "    \"kind\": \"%s\",\n", e.kind.c_str()));
        }
        put(std::fprintf(fp, "    \"sprite_clut\": %u,\n", e.spriteClut));
        put(std::fprintf(fp, "    \"first_seen_frame\": %u\n", e.frameNumber));
        put(std::fprintf(fp, "  }"));
    }
    put(std::fprintf(fp, "\n}\n"));
    if (std::ferror(fp)) {
        ok = false;
    }
    if (std::fclose(fp) != 0) {
        ok = false;
    }
    if (ok && !MoveFileExW(tmpPath.c_str(), manPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        ok = false;
    }
    if (!ok) {
        // Keep the previous manifest.json and drop the partial tmp file.
        DeleteFileW(tmpPath.c_str());
    }
    return ok;
}

// -----------------------------------------------------------------------------
// Base palette (DATA\PALETTE.RAW) and texture pack replacement fade
// -----------------------------------------------------------------------------

bool LoadBasePaletteFile(const std::wstring& path, uint8_t (&outPalette)[kBasePaletteBytes]) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || !fp) {
        return false;
    }
    uint8_t buf[kBasePaletteBytes + 1] = {};
    const size_t n = std::fread(buf, 1, sizeof(buf), fp);
    std::fclose(fp);
    if (n != kBasePaletteBytes) {
        return false;
    }
    std::memcpy(outPalette, buf, kBasePaletteBytes);
    return true;
}

void ComputePaletteFadeFactor(const uint8_t* basePalette, const uint8_t* currentPalette, float outFade[3]) {
    outFade[0] = outFade[1] = outFade[2] = 1.0f;
    if (!basePalette || !currentPalette) {
        return;
    }
    for (int c = 0; c < 3; ++c) {
        uint32_t baseSum = 0;
        uint32_t curSum = 0;
        for (int i = 1; i < 256; ++i) {
            baseSum += basePalette[i * 4 + c];
            curSum += currentPalette[i * 4 + c];
        }
        if (baseSum == 0) {
            continue;
        }
        const float f = static_cast<float>(curSum) / static_cast<float>(baseSum);
        outFade[c] = std::min(1.0f, std::max(0.0f, f));
    }
}

bool PaletteIsUniformScale(const uint8_t* basePalette, const uint8_t* currentPalette, const float fade[3]) {
    if (!basePalette || !currentPalette) {
        return true;
    }
    int matching = 0;
    for (int i = 1; i < 256; ++i) {
        bool ok = true;
        for (int c = 0; c < 3 && ok; ++c) {
            const int expected = static_cast<int>(std::lround(basePalette[i * 4 + c] * fade[c]));
            ok = std::abs(static_cast<int>(currentPalette[i * 4 + c]) - expected) <= 3;
        }
        if (ok) {
            ++matching;
        }
    }
    return matching * 100 >= 255 * 95;
}

// -----------------------------------------------------------------------------
// Texture Dumper worker thread implementation
// -----------------------------------------------------------------------------

struct QueuedDumpItem {
    TextureDumpInfo info;
    std::vector<uint32_t> rgbaPixels;
};

class TextureDumper {
public:
    TextureDumper() = default;
    // The global instance is leaked on purpose and never reaches this destructor (see
    // g_textureDumper), so process exit runs no teardown under the loader lock.
    ~TextureDumper() { Shutdown(); }

    void Init(const std::wstring& dumpDir, const std::wstring& basePalettePath) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running) {
            return;
        }
        m_dumpedSessionCount.store(0, std::memory_order_relaxed);
        // Dumps use the full-brightness base palette, never the frame's palette: textures first
        // seen during a palette fade would otherwise be dumped dark or black.
        if (!LoadBasePaletteFile(basePalettePath, m_basePalette)) {
            if (!m_paletteWarned) {
                m_paletteWarned = true;
                LOG_WARN("TexturePack: base palette %ls is missing or not %u bytes; texture dump disabled",
                         basePalettePath.c_str(), static_cast<unsigned>(kBasePaletteBytes));
            }
            return;
        }
        m_dumpDir = dumpDir;
        CreateDirectoryRecursive(dumpDir);
        LoadDumpManifest(m_dumpDir + L"\\manifest.json", m_manifestEntries, m_dumpedSet);

        // Scan existing PNGs on disk in dumpDir to avoid re-dumping:
        const std::wstring search = m_dumpDir + L"\\*.png";
        WIN32_FIND_DATAW fd = {};
        HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring fn = fd.cFileName;
                // Only <w>x<h>_<crc8>.png files are dumps; stray PNGs are ignored.
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsTextureIdPngName(fn)) {
                    const std::wstring stem = fn.substr(0, fn.size() - 4);
                    std::string id;
                    id.reserve(stem.size());
                    for (wchar_t ch : stem) {
                        id.push_back(static_cast<char>(ch));
                    }
                    m_dumpedSet.insert(id);
                    if (m_manifestEntries.find(id) == m_manifestEntries.end()) {
                        // No manifest entry: the size comes from the ID, while color_key and kind
                        // are unknown and stay out of the written manifest.
                        TextureDumpInfo info;
                        info.id = id;
                        int w = 0, h = 0;
                        if (sscanf_s(id.c_str(), "%dx%d_", &w, &h) == 2) {
                            info.width = w;
                            info.height = h;
                        }
                        info.colorKeyKnown = false;
                        info.kindKnown = false;
                        m_manifestEntries.emplace(id, info);
                    }
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }

        m_stop = false;
        m_running = true;
        m_worker = std::thread(&TextureDumper::WorkerLoop, this);
    }

    void Shutdown() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running) {
                return;
            }
            m_stop = true;
            m_queue.clear();
            // IDs dropped with the queue must be queueable again after a re-Init.
            m_queuedSet.clear();
        }
        m_cv.notify_all();
        if (m_worker.joinable()) {
            m_worker.join();
        }
        m_running = false;
    }

    void Queue(const TextureDumpInfo& info, const uint8_t* level0Indices) {
        if (!m_running || !level0Indices || info.width <= 0 || info.height <= 0) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_dumpedSet.find(info.id) != m_dumpedSet.end() ||
                m_queuedSet.find(info.id) != m_queuedSet.end()) {
                return;
            }
            m_queuedSet.insert(info.id);
        }

        QueuedDumpItem item;
        item.info = info;
        const size_t total = static_cast<size_t>(info.width) * static_cast<size_t>(info.height);
        item.rgbaPixels.resize(total);

        const uint8_t* palette = m_basePalette;
        for (size_t i = 0; i < total; ++i) {
            const uint8_t idx = level0Indices[i];
            if (info.colorKey && idx == 0) {
                item.rgbaPixels[i] = 0x00000000u;
            } else {
                const uint32_t r = palette[idx * 4 + 0];
                const uint32_t g = palette[idx * 4 + 1];
                const uint32_t b = palette[idx * 4 + 2];
                item.rgbaPixels[i] = r | (g << 8) | (b << 16) | 0xFF000000u;
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push_back(std::move(item));
        }
        m_cv.notify_one();
    }

    uint32_t GetDumpedCount() const { return m_dumpedSessionCount.load(std::memory_order_relaxed); }
    uint32_t GetQueuedCount() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<uint32_t>(m_queue.size());
    }

private:
    void WorkerLoop() {
        uint32_t texturesSinceSave = 0;
        auto lastSaveTime = std::chrono::steady_clock::now();

        while (true) {
            QueuedDumpItem item;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return m_stop.load() || !m_queue.empty(); });
                if (m_stop) {
                    break;
                }
                item = std::move(m_queue.front());
                m_queue.pop_front();
            }

            const std::wstring outPng = m_dumpDir + L"\\" +
                                        std::wstring(item.info.id.begin(), item.info.id.end()) + L".png";
            DWORD attr = GetFileAttributesW(outPng.c_str());
            bool saveOk = true;
            if (attr == INVALID_FILE_ATTRIBUTES) {
                saveOk = SaveRgbaPngWithWic(outPng, item.info.width, item.info.height, item.rgbaPixels.data());
                if (!saveOk) {
                    DeleteFileW(outPng.c_str());
                }
            }

            std::unordered_map<std::string, TextureDumpInfo> entriesCopy;
            bool shouldSave = false;

            if (saveOk) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_dumpedSet.insert(item.info.id);
                m_queuedSet.erase(item.info.id);
                m_manifestEntries[item.info.id] = item.info;
                m_dumpedSessionCount.fetch_add(1, std::memory_order_relaxed);
                ++texturesSinceSave;

                const auto now = std::chrono::steady_clock::now();
                const bool timePassed = (now - lastSaveTime) > std::chrono::seconds(2);
                if (m_queue.empty() || texturesSinceSave >= 16 || timePassed) {
                    entriesCopy = m_manifestEntries;
                    shouldSave = true;
                    texturesSinceSave = 0;
                    lastSaveTime = now;
                }
            } else {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_queuedSet.erase(item.info.id);
            }

            if (shouldSave) {
                SaveDumpManifest(m_dumpDir + L"\\manifest.json", entriesCopy);
            }
        }

        std::unordered_map<std::string, TextureDumpInfo> finalEntries;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            finalEntries = m_manifestEntries;
        }
        SaveDumpManifest(m_dumpDir + L"\\manifest.json", finalEntries);
    }

    std::wstring m_dumpDir;
    uint8_t m_basePalette[kBasePaletteBytes] = {};
    bool m_paletteWarned = false;
    bool m_running = false;
    std::atomic<bool> m_stop{false};
    std::thread m_worker;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<QueuedDumpItem> m_queue;
    std::unordered_set<std::string> m_queuedSet;
    std::unordered_set<std::string> m_dumpedSet;
    std::unordered_map<std::string, TextureDumpInfo> m_manifestEntries;
    std::atomic<uint32_t> m_dumpedSessionCount{0};
};

// Intentionally leaked singleton: it is never destroyed, so DLL_PROCESS_DETACH at process exit
// runs no destructor under the loader lock. By then the OS has killed the worker, possibly while it
// held m_mutex, and joining, locking, allocating or flushing the manifest there can hang the exit.
// The periodic manifest flush and the disk scan at the next Init already cover a killed worker.
static TextureDumper& g_textureDumper = *new TextureDumper();

void TextureDumpInit(const std::wstring& dumpDir, const std::wstring& basePalettePath) {
    g_textureDumper.Init(dumpDir, basePalettePath);
}

void TextureDumpShutdown() {
    g_textureDumper.Shutdown();
}

void TextureDumpQueue(const TextureDumpInfo& info, const uint8_t* level0Indices) {
    g_textureDumper.Queue(info, level0Indices);
}

uint32_t TextureDumpGetDumpedCount() {
    return g_textureDumper.GetDumpedCount();
}

uint32_t TextureDumpGetQueuedCount() {
    return g_textureDumper.GetQueuedCount();
}

// -----------------------------------------------------------------------------
// Texture Pack Manager (Work Unit 2)
// -----------------------------------------------------------------------------

void PackLoadQueue::PushPreload(PackLoadRequest req) {
    m_preload.push_back(std::move(req));
}

void PackLoadQueue::PushDemand(PackLoadRequest req) {
    m_demand.push_back(std::move(req));
}

bool PackLoadQueue::Promote(const std::string& id) {
    auto it = std::find_if(m_preload.begin(), m_preload.end(),
                           [&](const PackLoadRequest& r) { return r.id == id; });
    if (it == m_preload.end()) {
        return false;
    }
    m_demand.push_back(std::move(*it));
    m_preload.erase(it);
    return true;
}

bool PackLoadQueue::PopNext(PackLoadRequest& out, bool allowPreload) {
    if (!m_demand.empty()) {
        out = std::move(m_demand.front());
        m_demand.pop_front();
        return true;
    }
    if (allowPreload && !m_preload.empty()) {
        out = std::move(m_preload.front());
        m_preload.pop_front();
        return true;
    }
    return false;
}

std::vector<std::string> PackLoadQueue::ClearPreload() {
    std::vector<std::string> ids;
    ids.reserve(m_preload.size());
    for (const PackLoadRequest& r : m_preload) {
        ids.push_back(r.id);
    }
    m_preload.clear();
    return ids;
}

void PackLoadQueue::Clear() {
    m_demand.clear();
    m_preload.clear();
}

// Decoded preload results waiting for TexturePackUpdate are capped at this many bytes, so a pack
// decoded while no frame consumes it (menus, a minimized window) cannot fill the 32-bit address space.
static constexpr size_t kPreloadReadyBytesLimit = 64u * 1024u * 1024u;

struct DecodedReplacement {
    std::string id;
    int32_t origW = 0;
    int32_t origH = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t k = 1;
    int32_t levels = 1;
    size_t totalBytes = 0;
    std::vector<std::vector<uint32_t>> mips;
    bool rejected = false;
    PngLoadResult loadResult = PngLoadResult::Ok;  // SizeRejected or DecodeFailed when rejected
    bool exceptionLogged = false;                  // the worker already logged the exception
    HRESULT decodeHr = S_OK;
};

class TexturePackManager {
public:
    TexturePackManager() = default;
    // Never reached by the leaked global instance (see g_texturePackManager).
    ~TexturePackManager() { Shutdown(); }

    void Init(const std::wstring& packDir, int32_t maxMiB, int32_t decodeBudgetMs, bool preload) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running) {
            return;
        }
        m_packDir = packDir;
        m_maxMiB = (maxMiB >= 16 && maxMiB <= 2048) ? maxMiB : 512;
        m_decodeBudgetMs = decodeBudgetMs > 0 ? decodeBudgetMs : 4;
        m_maxMibWarned = false;
        m_totalAllocatedBytes = 0;
        m_indexedFiles.clear();
        m_replacements.clear();
        m_pendingSet.clear();
        m_rejectedIds.clear();
        m_loadQueue.Clear();
        m_readyQueue.clear();
        ResetPreloadState();

        const std::wstring search = packDir + L"\\*.png";
        WIN32_FIND_DATAW fd = {};
        HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
        uint32_t count = 0;
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    const std::wstring fn = fd.cFileName;
                    if (fn.size() > 4) {
                        const std::wstring ext = fn.substr(fn.size() - 4);
                        if (_wcsicmp(ext.c_str(), L".png") == 0) {
                            const std::wstring stem = fn.substr(0, fn.size() - 4);
                            std::string id;
                            id.reserve(stem.size());
                            for (wchar_t ch : stem) {
                                id.push_back(static_cast<char>(ch));
                            }
                            m_indexedFiles.emplace(id, packDir + L"\\" + fn);
                            ++count;
                        }
                    }
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
        LOG_INFO("TexturePack: %u files indexed", count);
        m_indexedCount.store(count, std::memory_order_relaxed);
        if (preload) {
            QueuePreload();
        }

        m_stop = false;
        m_running = true;
        m_worker = std::thread(&TexturePackManager::WorkerLoop, this);
    }

    void Shutdown() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running) {
                return;
            }
            m_stop = true;
            m_loadQueue.Clear();
        }
        m_cv.notify_all();
        if (m_worker.joinable()) {
            m_worker.join();
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running = false;
        m_replacements.clear();
        m_indexedFiles.clear();
        m_indexedCount.store(0, std::memory_order_relaxed);
        m_pendingSet.clear();
        m_rejectedIds.clear();
        m_loadQueue.Clear();
        m_readyQueue.clear();
        ResetPreloadState();
        m_totalAllocatedBytes = 0;
        m_maxMibWarned = false;
    }

    void ClearD3DResources() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_replacements.clear();
        m_totalAllocatedBytes = 0;
        m_generation.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t GetGeneration() const {
        return m_generation.load(std::memory_order_relaxed);
    }

    // Lock-free, so the presenter can test it every frame.
    uint32_t GetIndexedCount() const {
        return m_indexedCount.load(std::memory_order_relaxed);
    }

    bool HasIndexed(const std::string& id) {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_indexedFiles.find(id) != m_indexedFiles.end();
    }

    ReplacementState GetState(const std::string& id) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_replacements.find(id) != m_replacements.end()) {
            return ReplacementState::Ready;
        }
        if (m_pendingSet.find(id) != m_pendingSet.end()) {
            return ReplacementState::Pending;
        }
        return ReplacementState::None;
    }

    ReplacementTexture* GetOrQueue(const std::string& id, int32_t origW, int32_t origH, bool* outPending) {
        if (outPending) {
            *outPending = false;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        auto itRep = m_replacements.find(id);
        if (itRep != m_replacements.end()) {
            return &itRep->second;
        }
        if (m_rejectedIds.find(id) != m_rejectedIds.end()) {
            return nullptr;
        }
        auto itFile = m_indexedFiles.find(id);
        if (itFile == m_indexedFiles.end()) {
            return nullptr;
        }
        if (m_pendingSet.find(id) != m_pendingSet.end()) {
            // Drawn while queued or decoding: the original may now be cached, so the pack generation
            // must change when this replacement arrives. A queued preload request moves ahead of the
            // remaining preload requests.
            if (m_demandedIds.insert(id).second && m_loadQueue.Promote(id)) {
                m_cv.notify_one();
            }
            if (outPending) {
                *outPending = true;
            }
            return nullptr;
        }

        const uint64_t maxBytes = static_cast<uint64_t>(m_maxMiB) * 1024ULL * 1024ULL;
        if (m_totalAllocatedBytes >= maxBytes) {
            if (!m_maxMibWarned) {
                m_maxMibWarned = true;
                LOG_WARN("TexturePack: MaxMiB (%d MiB) reached; skipping further replacements", m_maxMiB);
            }
            m_rejectedIds.insert(id);
            return nullptr;
        }

        m_pendingSet.insert(id);
        m_demandedIds.insert(id);
        m_loadQueue.PushDemand(PackLoadRequest{id, itFile->second, origW, origH});
        m_cv.notify_one();
        if (outPending) {
            *outPending = true;
        }
        return nullptr;
    }

    void Update(ID3D11Device* device, int32_t budgetMsOverride) {
        if (!device) {
            return;
        }
        LARGE_INTEGER freq, start, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);

        const int32_t budgetMs = budgetMsOverride > 0 ? budgetMsOverride : m_decodeBudgetMs;
        const double budgetSec = static_cast<double>(budgetMs) / 1000.0;
        const uint64_t maxBytes = static_cast<uint64_t>(m_maxMiB) * 1024ULL * 1024ULL;
        // Bumped only for replacements that were drawn while pending: a preloaded replacement that
        // is ready before its first use has no cached original to invalidate.
        bool loadedDemanded = false;
        bool poppedAny = false;

        while (true) {
            DecodedReplacement item;
            bool demanded = false;
            bool preloaded = false;
            bool preloadStopped = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_readyQueue.empty()) {
                    break;
                }
                item = std::move(m_readyQueue.front());
                m_readyQueue.pop_front();
                m_pendingSet.erase(item.id);
                m_readyBytes -= std::min(m_readyBytes, item.totalBytes);
                demanded = m_demandedIds.erase(item.id) > 0;
                preloaded = m_preloadIds.erase(item.id) > 0;
                preloadStopped = m_preloadStopped;
                poppedAny = true;
            }

            if (item.rejected) {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (preloaded) {
                    ++(item.loadResult == PngLoadResult::SizeRejected ? m_preloadRejected : m_preloadFailed);
                    MaybeLogPreloadDone();
                }
                // m_rejectedIds keeps the ID from being queued again, so each line is logged once.
                m_rejectedIds.insert(item.id);
                if (item.exceptionLogged) {
                    // Already logged by the worker.
                } else if (item.loadResult == PngLoadResult::SizeRejected) {
                    LOG_WARN("%s", FormatReplacementRejectLog(item.id, item.width, item.height,
                                                              item.origW, item.origH).c_str());
                } else {
                    LOG_WARN("TexturePack: %s decode failed (0x%08lX)", item.id.c_str(),
                             static_cast<unsigned long>(item.decodeHr));
                }

                QueryPerformanceCounter(&now);
                const double elapsed = static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);
                if (elapsed >= budgetSec) {
                    break;
                }
                continue;
            }

            if (preloaded && !demanded &&
                (preloadStopped || m_totalAllocatedBytes + item.totalBytes > maxBytes)) {
                // Not drawn yet and it does not fit (or preloading already stopped): stop preloading
                // and drop it without rejecting it, so a later draw can still load it if it fits.
                std::lock_guard<std::mutex> lock(m_mutex);
                ++m_preloadSkipped;
                StopPreload();
                MaybeLogPreloadDone();

                QueryPerformanceCounter(&now);
                const double elapsed = static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);
                if (elapsed >= budgetSec) {
                    break;
                }
                continue;
            }

            if (m_totalAllocatedBytes + item.totalBytes > maxBytes) {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (preloaded) {
                    ++m_preloadSkipped;
                    MaybeLogPreloadDone();
                }
                if (!m_maxMibWarned) {
                    m_maxMibWarned = true;
                    LOG_WARN("TexturePack: MaxMiB (%d MiB) reached; skipping further replacements", m_maxMiB);
                }
                m_rejectedIds.insert(item.id);

                QueryPerformanceCounter(&now);
                const double elapsed = static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);
                if (elapsed >= budgetSec) {
                    break;
                }
                continue;
            }

            D3D11_TEXTURE2D_DESC td = {};
            td.Width = static_cast<UINT>(item.width);
            td.Height = static_cast<UINT>(item.height);
            td.MipLevels = static_cast<UINT>(item.levels);
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

            std::vector<D3D11_SUBRESOURCE_DATA> initData(item.levels);
            int32_t curW = item.width;
            for (int32_t l = 0; l < item.levels; ++l) {
                initData[l].pSysMem = item.mips[l].data();
                initData[l].SysMemPitch = static_cast<UINT>(curW * sizeof(uint32_t));
                initData[l].SysMemSlicePitch = 0;
                curW = std::max(1, curW / 2);
            }

            ComPtr<ID3D11Texture2D> tex;
            HRESULT hr = device->CreateTexture2D(&td, initData.data(), &tex);
            if (FAILED(hr)) {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (preloaded) {
                    ++m_preloadFailed;
                    MaybeLogPreloadDone();
                }
                m_rejectedIds.insert(item.id);
                LOG_WARN("TexturePack: CreateTexture2D failed for %s: 0x%08lX", item.id.c_str(), static_cast<unsigned long>(hr));
                QueryPerformanceCounter(&now);
                const double elapsed = static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);
                if (elapsed >= budgetSec) {
                    break;
                }
                continue;
            }

            ComPtr<ID3D11ShaderResourceView> srv;
            hr = device->CreateShaderResourceView(tex.Get(), nullptr, &srv);
            if (FAILED(hr)) {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (preloaded) {
                    ++m_preloadFailed;
                    MaybeLogPreloadDone();
                }
                m_rejectedIds.insert(item.id);
                LOG_WARN("TexturePack: CreateShaderResourceView failed for %s: 0x%08lX", item.id.c_str(), static_cast<unsigned long>(hr));
                QueryPerformanceCounter(&now);
                const double elapsed = static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);
                if (elapsed >= budgetSec) {
                    break;
                }
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                ReplacementTexture rep;
                rep.srv = srv;
                rep.width = item.width;
                rep.height = item.height;
                rep.k = item.k;
                rep.levels = item.levels;
                rep.totalBytes = item.totalBytes;
                m_replacements.emplace(item.id, std::move(rep));
                m_totalAllocatedBytes += item.totalBytes;
                if (demanded) {
                    loadedDemanded = true;
                }
                if (preloaded) {
                    ++m_preloadLoaded;
                    MaybeLogPreloadDone();
                }
            }
            LOG_INFO("TexturePack: %s loaded %dx%d (k=%d, %zu bytes)",
                     item.id.c_str(), item.width, item.height, item.k, item.totalBytes);

            QueryPerformanceCounter(&now);
            const double elapsed = static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);
            if (elapsed >= budgetSec) {
                break;
            }
        }
        if (loadedDemanded) {
            m_generation.fetch_add(1, std::memory_order_relaxed);
        }
        if (poppedAny) {
            // The ready queue shrank: a worker held back by kPreloadReadyBytesLimit may continue.
            m_cv.notify_one();
        }
    }

    TexturePackStats GetStats() {
        std::lock_guard<std::mutex> lock(m_mutex);
        TexturePackStats s;
        s.loadedCount = static_cast<uint32_t>(m_replacements.size());
        s.pendingCount = static_cast<uint32_t>(m_pendingSet.size());
        s.rejectedCount = static_cast<uint32_t>(m_rejectedIds.size());
        s.totalBytes = m_totalAllocatedBytes;
        return s;
    }

private:
    // All helpers below run with m_mutex held.
    void ResetPreloadState() {
        m_preloadIds.clear();
        m_demandedIds.clear();
        m_preloadActive = false;
        m_preloadStopped = false;
        m_preloadLoaded = 0;
        m_preloadRejected = 0;
        m_preloadFailed = 0;
        m_preloadSkipped = 0;
        m_preloadStartMs = 0;
        m_readyBytes = 0;
    }

    // Queues every indexed file whose name is a texture ID; the original size comes from the ID.
    void QueuePreload() {
        std::vector<std::pair<std::string, std::wstring>> files(m_indexedFiles.begin(), m_indexedFiles.end());
        std::sort(files.begin(), files.end());
        for (auto& file : files) {
            int32_t w = 0, h = 0;
            if (!ParseTextureIdSize(file.first, w, h)) {
                continue;
            }
            m_pendingSet.insert(file.first);
            m_preloadIds.insert(file.first);
            m_loadQueue.PushPreload(PackLoadRequest{file.first, std::move(file.second), w, h});
        }
        if (!m_preloadIds.empty()) {
            m_preloadActive = true;
            m_preloadStartMs = GetTickCount64();
            LOG_INFO("TexturePack: preloading %u files", static_cast<unsigned>(m_preloadIds.size()));
        }
    }

    bool PreloadMayDecode() const {
        return !m_preloadStopped && m_readyBytes < kPreloadReadyBytesLimit;
    }

    // MaxMiB reached by a preload: drop every preload request still queued. Requests already decoded
    // or decoding are dropped by Update unless they were drawn meanwhile.
    void StopPreload() {
        if (m_preloadStopped) {
            return;
        }
        m_preloadStopped = true;
        LOG_WARN("TexturePack: MaxMiB (%d MiB) reached; preload stopped, on-demand loads continue while they fit",
                 m_maxMiB);
        for (const std::string& id : m_loadQueue.ClearPreload()) {
            m_pendingSet.erase(id);
            m_preloadIds.erase(id);
            ++m_preloadSkipped;
        }
    }

    void MaybeLogPreloadDone() {
        if (!m_preloadActive || !m_preloadIds.empty()) {
            return;
        }
        m_preloadActive = false;
        const unsigned long long ms = GetTickCount64() - m_preloadStartMs;
        if (m_preloadSkipped > 0) {
            LOG_INFO("TexturePack: preload done, %u loaded in %llu ms (%u rejected, %u failed, %u skipped over MaxMiB)",
                     m_preloadLoaded, ms, m_preloadRejected, m_preloadFailed, m_preloadSkipped);
        } else {
            LOG_INFO("TexturePack: preload done, %u loaded in %llu ms (%u rejected, %u failed)",
                     m_preloadLoaded, ms, m_preloadRejected, m_preloadFailed);
        }
    }

    void WorkerLoop() {
        HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool needUninit = (hrCom == S_OK || hrCom == S_FALSE);

        while (true) {
            PackLoadRequest req;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                // On-demand requests are always served; preload requests wait while preloading is
                // stopped or too many decoded bytes are waiting for TexturePackUpdate.
                m_cv.wait(lock, [this] {
                    return m_stop.load() || m_loadQueue.HasDemand() ||
                           (m_loadQueue.HasPreload() && PreloadMayDecode());
                });
                if (m_stop) {
                    break;
                }
                if (!m_loadQueue.PopNext(req, PreloadMayDecode())) {
                    continue;
                }
            }

            DecodedReplacement item;
            item.id = req.id;
            item.origW = req.origW;
            item.origH = req.origH;

            try {
                std::vector<uint32_t> premulRgba;
                int32_t w = 0, h = 0, k = 1;
                HRESULT hr = S_OK;
                item.loadResult = LoadRgbaPngWithWicEx(req.filePath, req.origW, req.origH, w, h, k, premulRgba, hr);
                if (item.loadResult != PngLoadResult::Ok) {
                    item.rejected = true;
                    item.width = w;   // the PNG size for SizeRejected
                    item.height = h;
                    item.decodeHr = hr;
                } else {
                    item.width = w;
                    item.height = h;
                    item.k = k;
                    GeneratePremultipliedMipChain(w, h, premulRgba.data(), item.mips);
                    item.levels = static_cast<int32_t>(item.mips.size());
                    size_t bytes = 0;
                    for (const auto& mip : item.mips) {
                        bytes += mip.size() * sizeof(uint32_t);
                    }
                    item.totalBytes = bytes;
                }
            } catch (const std::bad_alloc&) {
                item.rejected = true;
                item.exceptionLogged = true;
                LOG_ERROR("TexturePack: bad_alloc loading %s", req.id.c_str());
            } catch (const std::exception& e) {
                item.rejected = true;
                item.exceptionLogged = true;
                LOG_ERROR("TexturePack: exception loading %s: %s", req.id.c_str(), e.what());
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_readyBytes += item.totalBytes;
                m_readyQueue.push_back(std::move(item));
            }
        }

        if (needUninit) {
            CoUninitialize();
        }
    }

    std::wstring m_packDir;
    int32_t m_maxMiB = 512;
    int32_t m_decodeBudgetMs = 4;
    bool m_running = false;
    std::atomic<bool> m_stop{false};
    std::atomic<uint32_t> m_indexedCount{0};
    bool m_maxMibWarned = false;
    size_t m_totalAllocatedBytes = 0;

    std::thread m_worker;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;

    std::unordered_map<std::string, std::wstring> m_indexedFiles;
    std::unordered_map<std::string, ReplacementTexture> m_replacements;
    std::unordered_set<std::string> m_pendingSet;
    std::unordered_set<std::string> m_rejectedIds;
    std::atomic<uint64_t> m_generation{1};
    PackLoadQueue m_loadQueue;
    std::deque<DecodedReplacement> m_readyQueue;
    size_t m_readyBytes = 0;  // totalBytes of the decoded items in m_readyQueue

    // IDs drawn while their replacement was pending: only these bump the pack generation.
    std::unordered_set<std::string> m_demandedIds;
    // Preload IDs whose decode result has not reached Update yet.
    std::unordered_set<std::string> m_preloadIds;
    bool m_preloadActive = false;   // preload queued at Init and the done line not logged yet
    bool m_preloadStopped = false;  // MaxMiB reached by a preload: no further preload work
    uint32_t m_preloadLoaded = 0;
    uint32_t m_preloadRejected = 0;
    uint32_t m_preloadFailed = 0;
    uint32_t m_preloadSkipped = 0;
    ULONGLONG m_preloadStartMs = 0;
};

// Intentionally leaked singleton, for the same reason as g_textureDumper: at process exit its
// destructor would join a killed worker and release D3D11 SRVs under the loader lock.
static TexturePackManager& g_texturePackManager = *new TexturePackManager();

void TexturePackInit(const std::wstring& packDir, int32_t maxMiB, int32_t decodeBudgetMs, bool preload) {
    g_texturePackManager.Init(packDir, maxMiB, decodeBudgetMs, preload);
}

void TexturePackShutdown() {
    g_texturePackManager.Shutdown();
}

void TexturePackClearD3DResources() {
    g_texturePackManager.ClearD3DResources();
}

void TexturePackUpdate(ID3D11Device* device, int32_t budgetMsOverride) {
    g_texturePackManager.Update(device, budgetMsOverride);
}

uint64_t TexturePackGetGeneration() {
    return g_texturePackManager.GetGeneration();
}

uint32_t TexturePackGetIndexedCount() {
    return g_texturePackManager.GetIndexedCount();
}

ReplacementTexture* TexturePackGetOrQueue(const std::string& id, int32_t origW, int32_t origH) {
    return g_texturePackManager.GetOrQueue(id, origW, origH, nullptr);
}

ReplacementTexture* TexturePackGetOrQueue(const std::string& id, int32_t origW, int32_t origH,
                                          bool* outPending) {
    return g_texturePackManager.GetOrQueue(id, origW, origH, outPending);
}

ReplacementState TexturePackGetState(const std::string& id) {
    return g_texturePackManager.GetState(id);
}

TexturePackStats TexturePackGetStats() {
    return g_texturePackManager.GetStats();
}

bool TexturePackHasIndexed(const std::string& id) {
    return g_texturePackManager.HasIndexed(id);
}
