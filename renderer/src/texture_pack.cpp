#include "texture_pack.h"
#include "log.h"
#include <wincodec.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
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
    if ((mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) == 0) {
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
                                            hr = frame->WritePixels(static_cast<UINT>(height), stride, bufSize,
                                                                    reinterpret_cast<BYTE*>(const_cast<uint32_t*>(rgbaPixels)));
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

bool LoadRgbaPngWithWic(const std::wstring& path, int32_t& outW, int32_t& outH, std::vector<uint32_t>& outPremulRgba) {
    outW = 0;
    outH = 0;
    outPremulRgba.clear();

    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needUninit = (hrCom == S_OK || hrCom == S_FALSE);
    bool success = false;

    {
        ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
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
                    if (SUCCEEDED(hr) && w > 0 && h > 0) {
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
                                    outW = static_cast<int32_t>(w);
                                    outH = static_cast<int32_t>(h);
                                    outPremulRgba.resize(static_cast<size_t>(w) * h);
                                    ConvertToPremultipliedRgba(rawPixels.data(), outW, outH, outPremulRgba.data());
                                    success = true;
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
// Texture Dumper worker thread implementation
// -----------------------------------------------------------------------------

struct QueuedDumpItem {
    TextureDumpInfo info;
    std::vector<uint32_t> rgbaPixels;
};

class TextureDumper {
public:
    TextureDumper() = default;
    ~TextureDumper() { Shutdown(); }

    void Init(const std::wstring& dumpDir) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running) {
            return;
        }
        m_dumpDir = dumpDir;
        CreateDirectoryW(dumpDir.c_str(), nullptr);
        LoadManifest();
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
        }
        m_cv.notify_all();
        if (m_worker.joinable()) {
            m_worker.join();
        }
        m_running = false;
    }

    void Queue(const TextureDumpInfo& info, const uint8_t* level0Indices, const uint8_t* palette) {
        if (!m_running || !level0Indices || !palette || info.width <= 0 || info.height <= 0) {
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

    uint32_t GetDumpedCount() const { return m_dumpedCount.load(std::memory_order_relaxed); }
    uint32_t GetQueuedCount() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<uint32_t>(m_queue.size());
    }

private:
    void WorkerLoop() {
        while (true) {
            QueuedDumpItem item;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
                if (m_stop && m_queue.empty()) {
                    break;
                }
                item = std::move(m_queue.front());
                m_queue.pop_front();
            }

            const std::wstring outPng = m_dumpDir + L"\\" +
                                        std::wstring(item.info.id.begin(), item.info.id.end()) + L".png";
            DWORD attr = GetFileAttributesW(outPng.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES) {
                SaveRgbaPngWithWic(outPng, item.info.width, item.info.height, item.rgbaPixels.data());
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_dumpedSet.insert(item.info.id);
                m_queuedSet.erase(item.info.id);
                m_manifestEntries[item.info.id] = item.info;
                m_dumpedCount.store(static_cast<uint32_t>(m_dumpedSet.size()), std::memory_order_relaxed);
            }
            WriteManifest();
        }
    }

    void LoadManifest() {
        const std::wstring manPath = m_dumpDir + L"\\manifest.json";
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, manPath.c_str(), L"rt") != 0 || !fp) {
            return;
        }
        std::fseek(fp, 0, SEEK_END);
        const long sz = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        if (sz > 0) {
            std::vector<char> buf(static_cast<size_t>(sz) + 1, 0);
            std::fread(buf.data(), 1, static_cast<size_t>(sz), fp);
            std::string content(buf.data());
            size_t pos = 0;
            while ((pos = content.find("\"id\": \"", pos)) != std::string::npos) {
                pos += 7;
                size_t endPos = content.find('"', pos);
                if (endPos != std::string::npos) {
                    std::string id = content.substr(pos, endPos - pos);
                    m_dumpedSet.insert(id);
                    pos = endPos;
                }
            }
        }
        std::fclose(fp);
        m_dumpedCount.store(static_cast<uint32_t>(m_dumpedSet.size()), std::memory_order_relaxed);
    }

    void WriteManifest() {
        const std::wstring manPath = m_dumpDir + L"\\manifest.json";
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, manPath.c_str(), L"wt") != 0 || !fp) {
            return;
        }
        std::fprintf(fp, "{\n");
        bool first = true;
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& kv : m_manifestEntries) {
            const TextureDumpInfo& e = kv.second;
            if (!first) {
                std::fprintf(fp, ",\n");
            }
            first = false;
            std::fprintf(fp, "  \"%s\": {\n", kv.first.c_str());
            std::fprintf(fp, "    \"width\": %d,\n", e.width);
            std::fprintf(fp, "    \"height\": %d,\n", e.height);
            std::fprintf(fp, "    \"color_key\": %s,\n", e.colorKey ? "true" : "false");
            std::fprintf(fp, "    \"mip_levels\": %d,\n", e.mipLevels);
            std::fprintf(fp, "    \"kind\": \"%s\",\n", e.kind.c_str());
            std::fprintf(fp, "    \"sprite_clut\": %u,\n", e.spriteClut);
            std::fprintf(fp, "    \"first_seen_frame\": %u\n", e.frameNumber);
            std::fprintf(fp, "  }");
        }
        std::fprintf(fp, "\n}\n");
        std::fclose(fp);
    }

    std::wstring m_dumpDir;
    bool m_running = false;
    bool m_stop = false;
    std::thread m_worker;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<QueuedDumpItem> m_queue;
    std::unordered_set<std::string> m_queuedSet;
    std::unordered_set<std::string> m_dumpedSet;
    std::unordered_map<std::string, TextureDumpInfo> m_manifestEntries;
    std::atomic<uint32_t> m_dumpedCount{0};
};

static TextureDumper g_textureDumper;

void TextureDumpInit(const std::wstring& dumpDir) {
    g_textureDumper.Init(dumpDir);
}

void TextureDumpShutdown() {
    g_textureDumper.Shutdown();
}

void TextureDumpQueue(const TextureDumpInfo& info,
                      const uint8_t* level0Indices,
                      const uint8_t* palette) {
    g_textureDumper.Queue(info, level0Indices, palette);
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

struct PackLoadRequest {
    std::string id;
    std::wstring filePath;
    int32_t origW = 0;
    int32_t origH = 0;
};

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
    std::string rejectReason;
};

class TexturePackManager {
public:
    TexturePackManager() = default;
    ~TexturePackManager() { Shutdown(); }

    void Init(const std::wstring& packDir, int32_t maxMiB, int32_t decodeBudgetMs) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running) {
            return;
        }
        m_packDir = packDir;
        m_maxMiB = maxMiB > 0 ? maxMiB : 512;
        m_decodeBudgetMs = decodeBudgetMs > 0 ? decodeBudgetMs : 4;
        m_maxMibWarned = false;
        m_totalAllocatedBytes = 0;
        m_indexedFiles.clear();
        m_replacements.clear();
        m_pendingSet.clear();
        m_rejectedIds.clear();
        m_loadQueue.clear();
        m_readyQueue.clear();

        // Index all <Path>/*.png once
        const std::wstring search = packDir + L"\\*.png";
        WIN32_FIND_DATAW fd = {};
        HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
        uint32_t count = 0;
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    const std::wstring fn = fd.cFileName;
                    if (fn.size() > 4 && fn.substr(fn.size() - 4) == L".png") {
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
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
        LOG_INFO("TexturePack: %u files indexed", count);

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
        }
        m_cv.notify_all();
        if (m_worker.joinable()) {
            m_worker.join();
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running = false;
        m_replacements.clear();
        m_indexedFiles.clear();
        m_pendingSet.clear();
        m_rejectedIds.clear();
        m_loadQueue.clear();
        m_readyQueue.clear();
        m_totalAllocatedBytes = 0;
        m_maxMibWarned = false;
    }

    void ClearD3DResources() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_replacements.clear();
        m_totalAllocatedBytes = 0;
    }

    bool HasIndexed(const std::string& id) {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_indexedFiles.find(id) != m_indexedFiles.end();
    }

    ReplacementTexture* GetOrQueue(const std::string& id, int32_t origW, int32_t origH) {
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
            return nullptr; // Not in pack, never touch disk
        }
        if (m_pendingSet.find(id) != m_pendingSet.end()) {
            return nullptr; // Already in flight
        }

        const size_t maxBytes = static_cast<size_t>(m_maxMiB) * 1024u * 1024u;
        if (m_totalAllocatedBytes >= maxBytes) {
            if (!m_maxMibWarned) {
                m_maxMibWarned = true;
                LOG_WARN("TexturePack: MaxMiB (%d MiB) reached; skipping further replacements", m_maxMiB);
            }
            m_rejectedIds.insert(id);
            return nullptr;
        }

        m_pendingSet.insert(id);
        m_loadQueue.push_back(PackLoadRequest{id, itFile->second, origW, origH});
        m_cv.notify_one();
        return nullptr;
    }

    void Update(ID3D11Device* device) {
        if (!device) {
            return;
        }
        LARGE_INTEGER freq, start, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);

        const double budgetSec = static_cast<double>(m_decodeBudgetMs) / 1000.0;
        const size_t maxBytes = static_cast<size_t>(m_maxMiB) * 1024u * 1024u;

        while (true) {
            DecodedReplacement item;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_readyQueue.empty()) {
                    break;
                }
                item = std::move(m_readyQueue.front());
                m_readyQueue.pop_front();
                m_pendingSet.erase(item.id);
            }

            if (item.rejected) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_rejectedIds.insert(item.id);
                LOG_WARN("TexturePack: %s rejected %s", item.id.c_str(), item.rejectReason.c_str());
                continue;
            }

            if (m_totalAllocatedBytes + item.totalBytes > maxBytes) {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_maxMibWarned) {
                    m_maxMibWarned = true;
                    LOG_WARN("TexturePack: MaxMiB (%d MiB) reached; skipping further replacements", m_maxMiB);
                }
                m_rejectedIds.insert(item.id);
                continue;
            }

            // Upload D3D11 Texture2D with full mip chain
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
                m_rejectedIds.insert(item.id);
                LOG_WARN("TexturePack: CreateTexture2D failed for %s: 0x%08lX", item.id.c_str(), static_cast<unsigned long>(hr));
                continue;
            }

            ComPtr<ID3D11ShaderResourceView> srv;
            hr = device->CreateShaderResourceView(tex.Get(), nullptr, &srv);
            if (FAILED(hr)) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_rejectedIds.insert(item.id);
                LOG_WARN("TexturePack: CreateShaderResourceView failed for %s: 0x%08lX", item.id.c_str(), static_cast<unsigned long>(hr));
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
            }
            LOG_INFO("TexturePack: %s loaded %dx%d (k=%d, %zu bytes)",
                     item.id.c_str(), item.width, item.height, item.k, item.totalBytes);

            QueryPerformanceCounter(&now);
            const double elapsed = static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);
            if (elapsed >= budgetSec) {
                break;
            }
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
    void WorkerLoop() {
        HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool needUninit = (hrCom == S_OK || hrCom == S_FALSE);

        while (true) {
            PackLoadRequest req;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return m_stop || !m_loadQueue.empty(); });
                if (m_stop && m_loadQueue.empty()) {
                    break;
                }
                req = std::move(m_loadQueue.front());
                m_loadQueue.pop_front();
            }

            DecodedReplacement item;
            item.id = req.id;
            item.origW = req.origW;
            item.origH = req.origH;

            std::vector<uint32_t> premulRgba;
            int32_t w = 0, h = 0;
            if (!LoadRgbaPngWithWic(req.filePath, w, h, premulRgba)) {
                item.rejected = true;
                item.rejectReason = "failed to decode PNG";
            } else {
                int32_t k = 1;
                if (!ValidateReplacementDimensions(req.origW, req.origH, w, h, k)) {
                    item.rejected = true;
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "(%dx%d is not a power-of-two multiple of %dx%d)",
                                  w, h, req.origW, req.origH);
                    item.rejectReason = buf;
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
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
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
    bool m_stop = false;
    bool m_maxMibWarned = false;
    size_t m_totalAllocatedBytes = 0;

    std::thread m_worker;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;

    std::unordered_map<std::string, std::wstring> m_indexedFiles;
    std::unordered_map<std::string, ReplacementTexture> m_replacements;
    std::unordered_set<std::string> m_pendingSet;
    std::unordered_set<std::string> m_rejectedIds;
    std::deque<PackLoadRequest> m_loadQueue;
    std::deque<DecodedReplacement> m_readyQueue;
};

static TexturePackManager g_texturePackManager;

void TexturePackInit(const std::wstring& packDir, int32_t maxMiB, int32_t decodeBudgetMs) {
    g_texturePackManager.Init(packDir, maxMiB, decodeBudgetMs);
}

void TexturePackShutdown() {
    g_texturePackManager.Shutdown();
}

void TexturePackClearD3DResources() {
    g_texturePackManager.ClearD3DResources();
}

void TexturePackUpdate(ID3D11Device* device) {
    g_texturePackManager.Update(device);
}

ReplacementTexture* TexturePackGetOrQueue(const std::string& id, int32_t origW, int32_t origH) {
    return g_texturePackManager.GetOrQueue(id, origW, origH);
}

TexturePackStats TexturePackGetStats() {
    return g_texturePackManager.GetStats();
}

bool TexturePackHasIndexed(const std::string& id) {
    return g_texturePackManager.HasIndexed(id);
}
