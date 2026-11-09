#include "texture_pack.h"
#include "log.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct ScopedComInit {
    bool needUninit = false;
    ScopedComInit() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) {
            needUninit = true;
        }
    }
    ~ScopedComInit() {
        if (needUninit) {
            CoUninitialize();
        }
    }
};

void EnsureDirectoryExists(const std::wstring& path) {
    if (path.empty()) return;
    size_t pos = 0;
    while ((pos = path.find_first_of(L"\\/", pos + 1)) != std::wstring::npos) {
        std::wstring sub = path.substr(0, pos);
        CreateDirectoryW(sub.c_str(), nullptr);
    }
    CreateDirectoryW(path.c_str(), nullptr);
}

std::wstring ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int req = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    if (req <= 0) return L"";
    std::wstring out(static_cast<size_t>(req), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), &out[0], req);
    return out;
}

struct ManifestEntry {
    int32_t width = 0;
    int32_t height = 0;
    bool colorKey = false;
    int32_t mipLevels = 1;
    std::string kind;
    uint8_t spriteClut = 0;
    uint32_t firstSeenFrame = 0;
};

struct DumpQueueItem {
    TextureDumpInfo info;
    std::vector<uint32_t> rgba;
};

class TextureDumper {
public:
    void Init(const std::wstring& dumpDir) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_initialized) return;
        m_dumpDir = dumpDir;
        m_stop = false;
        m_initialized = true;
        m_worker = std::thread(&TextureDumper::WorkerLoop, this);
    }

    void Shutdown() {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (!m_initialized) return;
            m_stop = true;
            m_cv.notify_all();
        }
        if (m_worker.joinable()) {
            m_worker.join();
        }
        std::unique_lock<std::mutex> lock(m_mutex);
        m_initialized = false;
    }

    void Queue(const TextureDumpInfo& info, const uint8_t* level0Indices, const uint8_t* palette) {
        if (!level0Indices || !palette || info.width <= 0 || info.height <= 0) {
            return;
        }
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_initialized || m_stop) return;

        if (m_seenIds.count(info.id) != 0) {
            return;
        }
        m_seenIds.insert(info.id);

        DumpQueueItem item;
        item.info = info;
        const size_t total = static_cast<size_t>(info.width) * static_cast<size_t>(info.height);
        item.rgba.resize(total);
        for (size_t i = 0; i < total; ++i) {
            const uint8_t idx = level0Indices[i];
            if (info.colorKey && idx == 0) {
                item.rgba[i] = 0x00000000u;
            } else {
                const uint32_t r = palette[idx * 4 + 0];
                const uint32_t g = palette[idx * 4 + 1];
                const uint32_t b = palette[idx * 4 + 2];
                item.rgba[i] = r | (g << 8) | (b << 16) | 0xFF000000u;
            }
        }
        m_queue.push_back(std::move(item));
        m_queuedCount.store(static_cast<uint32_t>(m_queue.size()));
        m_cv.notify_one();
    }

    uint32_t GetDumpedCount() const { return m_dumpedCount.load(); }
    uint32_t GetQueuedCount() const { return m_queuedCount.load(); }

private:
    void WorkerLoop() {
        ScopedComInit comInit;
        EnsureDirectoryExists(m_dumpDir);

        while (true) {
            DumpQueueItem item;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
                if (m_queue.empty()) {
                    if (m_stop) break;
                    continue;
                }
                item = std::move(m_queue.front());
                m_queue.pop_front();
                m_queuedCount.store(static_cast<uint32_t>(m_queue.size()));
            }

            const std::wstring pngPath = m_dumpDir + L"\\" + ToWide(item.info.id) + L".png";
            if (GetFileAttributesW(pngPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                SaveRgbaPngWithWic(pngPath, item.info.width, item.info.height, item.rgba.data());
            }

            {
                std::unique_lock<std::mutex> lock(m_manifestMutex);
                ManifestEntry me;
                me.width = item.info.width;
                me.height = item.info.height;
                me.colorKey = item.info.colorKey;
                me.mipLevels = item.info.mipLevels;
                me.kind = item.info.kind;
                me.spriteClut = item.info.spriteClut;
                me.firstSeenFrame = item.info.frameNumber;
                m_manifest[item.info.id] = me;

                const std::wstring manifestPath = m_dumpDir + L"\\manifest.json";
                WriteManifestJson(manifestPath, m_manifest);
            }

            m_dumpedCount.fetch_add(1);
        }
    }

    static void WriteManifestJson(const std::wstring& filePath, const std::map<std::string, ManifestEntry>& entries) {
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, filePath.c_str(), L"wb") != 0 || !fp) {
            return;
        }
        std::fprintf(fp, "{\n");
        size_t i = 0;
        for (const auto& kv : entries) {
            const std::string& id = kv.first;
            const ManifestEntry& e = kv.second;
            std::fprintf(fp, "  \"%s\": {\n", id.c_str());
            std::fprintf(fp, "    \"width\": %d,\n", e.width);
            std::fprintf(fp, "    \"height\": %d,\n", e.height);
            std::fprintf(fp, "    \"color_key\": %s,\n", e.colorKey ? "true" : "false");
            std::fprintf(fp, "    \"mip_levels\": %d,\n", e.mipLevels);
            std::fprintf(fp, "    \"kind\": \"%s\",\n", e.kind.c_str());
            std::fprintf(fp, "    \"sprite_clut\": %u,\n", static_cast<unsigned int>(e.spriteClut));
            std::fprintf(fp, "    \"first_seen_frame\": %u\n", e.firstSeenFrame);
            std::fprintf(fp, "  }%s\n", (++i < entries.size()) ? "," : "");
        }
        std::fprintf(fp, "}\n");
        std::fclose(fp);
    }

    std::wstring m_dumpDir;
    bool m_initialized = false;
    bool m_stop = false;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<DumpQueueItem> m_queue;
    std::unordered_set<std::string> m_seenIds;
    std::thread m_worker;

    std::mutex m_manifestMutex;
    std::map<std::string, ManifestEntry> m_manifest;

    std::atomic<uint32_t> m_dumpedCount{0};
    std::atomic<uint32_t> m_queuedCount{0};
};

TextureDumper g_dumper;

}  // namespace

std::string FormatTextureId(int32_t width, int32_t height, uint32_t level0Crc32) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%dx%d_%08x", width, height, level0Crc32);
    return std::string(buf);
}

bool ValidateMipLevelPointer(const void* ptr, int32_t width, int32_t height, int32_t level) {
    if (!ptr || width <= 0 || height <= 0 || level < 0) {
        return false;
    }
    const int32_t levelW = width >> level;
    const int32_t levelH = height >> level;
    if (levelW <= 0 || levelH <= 0) {
        return false;
    }
    const size_t byteCount = static_cast<size_t>(levelW) * static_cast<size_t>(levelH);
    const auto* bytePtr = static_cast<const uint8_t*>(ptr);

    size_t checked = 0;
    while (checked < byteCount) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(bytePtr + checked, &mbi, sizeof(mbi)) == 0) {
            return false;
        }
        if (mbi.State != MEM_COMMIT) {
            return false;
        }
        const DWORD prot = (mbi.Protect & 0xFF);
        if (prot != PAGE_READONLY && prot != PAGE_READWRITE && prot != PAGE_WRITECOPY &&
            prot != PAGE_EXECUTE_READ && prot != PAGE_EXECUTE_READWRITE && prot != PAGE_EXECUTE_WRITECOPY) {
            return false;
        }
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) {
            return false;
        }
        const uintptr_t curAddr = reinterpret_cast<uintptr_t>(bytePtr + checked);
        const uintptr_t baseAddr = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const size_t bytesInRegion = mbi.RegionSize - (curAddr - baseAddr);
        if (bytesInRegion == 0) {
            return false;
        }
        checked += bytesInRegion;
    }
    return true;
}

bool SaveRgbaPngWithWic(const std::wstring& path, int32_t width, int32_t height, const uint32_t* rgbaPixels) {
    if (path.empty() || width <= 0 || height <= 0 || !rgbaPixels) {
        return false;
    }
    ScopedComInit comInit;

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        return false;
    }

    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr)) return false;

    hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) return false;

    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    hr = encoder->CreateNewFrame(&frame, nullptr);
    if (FAILED(hr)) return false;

    hr = frame->Initialize(nullptr);
    if (FAILED(hr)) return false;

    hr = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
    if (FAILED(hr)) return false;

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
    hr = frame->SetPixelFormat(&format);
    if (FAILED(hr)) return false;

    const UINT stride = static_cast<UINT>(width * 4);
    const UINT bufferSize = static_cast<UINT>(width * height * 4);
    hr = frame->WritePixels(static_cast<UINT>(height), stride, bufferSize,
                            reinterpret_cast<BYTE*>(const_cast<uint32_t*>(rgbaPixels)));
    if (FAILED(hr)) return false;

    hr = frame->Commit();
    if (FAILED(hr)) return false;

    hr = encoder->Commit();
    return SUCCEEDED(hr);
}

void TextureDumpInit(const std::wstring& dumpDir) {
    g_dumper.Init(dumpDir);
}

void TextureDumpShutdown() {
    g_dumper.Shutdown();
}

void TextureDumpQueue(const TextureDumpInfo& info,
                      const uint8_t* level0Indices,
                      const uint8_t* palette) {
    g_dumper.Queue(info, level0Indices, palette);
}

uint32_t TextureDumpGetDumpedCount() {
    return g_dumper.GetDumpedCount();
}

uint32_t TextureDumpGetQueuedCount() {
    return g_dumper.GetQueuedCount();
}
