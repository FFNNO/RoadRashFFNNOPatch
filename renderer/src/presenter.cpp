#include "presenter.h"
#include "config.h"
#include "capture.h"
#include "log.h"
#include "quad_mapping.h"
#include "texture_pack.h"

#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

LetterboxViewport ComputeLetterboxViewport(int srcW, int srcH, int clientW, int clientH) {
    if (srcW <= 0 || srcH <= 0 || clientW <= 0 || clientH <= 0) {
        return {0, 0, 1, 1};
    }
    int vpW = clientW;
    int vpH = clientH;
    const int prod1 = srcW * clientH;
    const int prod2 = srcH * clientW;
    if (prod2 <= prod1) {
        vpW = clientW;
        vpH = prod2 / srcW;
    } else {
        vpW = prod1 / srcH;
        vpH = clientH;
    }
    if (vpW < 1) vpW = 1;
    if (vpH < 1) vpH = 1;
    const int vpX = (clientW - vpW) / 2;
    const int vpY = (clientH - vpH) / 2;
    return {vpX, vpY, vpW, vpH};
}

bool PresenterCanAttempt(const PresenterFailureState& state, uint64_t nowMs) {
    if (state.latchedOff) {
        return false;
    }
    if (state.failureCount > 0 && nowMs >= state.lastFailureMs &&
        (nowMs - state.lastFailureMs) < kPresenterRetryBackoffMs) {
        return false;
    }
    return true;
}

bool PresenterRecordFailure(PresenterFailureState& state, uint64_t nowMs) {
    if (state.latchedOff) {
        return false;
    }
    if (state.failureCount == 0 || nowMs < state.firstFailureMs ||
        (nowMs - state.firstFailureMs) > kPresenterFailureWindowMs) {
        state.failureCount = 1;
        state.firstFailureMs = nowMs;
        state.lastFailureMs = nowMs;
        return false;
    }
    ++state.failureCount;
    state.lastFailureMs = nowMs;
    if (state.failureCount >= kPresenterMaxFailuresInWindow) {
        state.latchedOff = true;
        return true;
    }
    return false;
}

bool PresenterStateAllowsGpuWorld(const PresenterFailureState& state) {
    return !state.latchedOff && state.failureCount == 0;
}

void PresenterRecordSuccess(PresenterFailureState& state) {
    if (!state.latchedOff) {
        state.failureCount = 0;
        state.firstFailureMs = 0;
        state.lastFailureMs = 0;
    }
}

namespace {

const char* kVertexShaderSrc = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
)";

const char* kPixelShaderSrc = R"(
cbuffer PresentCB : register(b0) {
    float2 srcSize;
    float2 dstSize;
    uint filterMode;
    uint useBgra;
    float2 pad;
};

Texture2D<uint>   tex8bpp    : register(t0);
Texture2D<float4> texPalette : register(t1);
Texture2D<float4> texBgra    : register(t2);

float4 FetchTexel(int2 pos) {
    int2 maxPos = int2(srcSize) - int2(1, 1);
    int2 clamped = clamp(pos, int2(0, 0), maxPos);
    if (useBgra != 0) {
        float4 c = texBgra.Load(int3(clamped, 0));
        return float4(c.rgb, 1.0);
    } else {
        uint idx = tex8bpp.Load(int3(clamped, 0));
        float4 c = texPalette.Load(int3(int(idx & 0xFFu), 0, 0));
        return float4(c.rgb, 1.0);
    }
}

float4 main(float4 svPos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    if (filterMode == 0) {
        return FetchTexel(int2(floor(uv * srcSize)));
    }
    float2 texel = uv * srcSize - 0.5;
    int2 p0 = int2(floor(texel));
    float2 f = frac(texel);
    if (filterMode == 2) {
        float2 scale = max(floor(dstSize / srcSize), 1.0);
        f = saturate((f - 0.5) * scale + 0.5);
    }
    float4 c00 = FetchTexel(p0 + int2(0, 0));
    float4 c10 = FetchTexel(p0 + int2(1, 0));
    float4 c01 = FetchTexel(p0 + int2(0, 1));
    float4 c11 = FetchTexel(p0 + int2(1, 1));
    return lerp(lerp(c00, c10, f.x), lerp(c01, c11, f.x), f.y);
}
)";

struct alignas(16) PresentConstants {
    float srcSize[2];
    float dstSize[2];
    uint32_t filterMode;
    uint32_t useBgra;
    float pad[2];
    // World pass only: per-channel palette fade applied to texture pack replacements (rgb, w unused).
    float replacementFade[4];
};

const char* kLayerVertexShaderSrc = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
)";

const char* kLayerPixelShaderSrc = R"(
Texture2D<float4> texLayer : register(t0);
SamplerState      samp     : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 c = texLayer.Sample(samp, uv);
    if (c.a < 0.5) {
        discard;
    }
    return c;
}
)";

const char* kWorldVertexShaderSrc = R"(
struct VSIn {
    float2 pos     : POSITION;
    float4 p01     : TEXCOORD0;
    float4 p23     : TEXCOORD1;
    float4 uv01    : TEXCOORD2;
    float4 uv23    : TEXCOORD3;
    float4 uvClamp : TEXCOORD4;
    float4 color   : COLOR0;
    float4 flags   : COLOR1;
    float4 texInfo : TEXCOORD5;
    float4 box     : TEXCOORD6;
};

struct VSOut {
    float4 pos                     : SV_Position;
    nointerpolation float4 p01     : TEXCOORD0;
    nointerpolation float4 p23     : TEXCOORD1;
    nointerpolation float4 uv01    : TEXCOORD2;
    nointerpolation float4 uv23    : TEXCOORD3;
    nointerpolation float4 uvClamp : TEXCOORD4;
    nointerpolation float4 color   : COLOR0;
    nointerpolation float4 flags   : COLOR1;
    nointerpolation float4 texInfo : TEXCOORD5;
    nointerpolation float4 box     : TEXCOORD6;
};

VSOut main(VSIn v) {
    VSOut o;
    o.pos     = float4(v.pos, 0.0, 1.0);
    o.p01     = v.p01;
    o.p23     = v.p23;
    o.uv01    = v.uv01;
    o.uv23    = v.uv23;
    o.uvClamp = v.uvClamp;
    o.color   = v.color;
    o.flags   = v.flags;
    o.texInfo = v.texInfo;
    o.box     = v.box;
    return o;
}
)";

const char* kWorldPixelShaderSrc = R"(
Texture2D<float4> texQuad   : register(t0);
SamplerState      samp      : register(s0);
SamplerState      sampAtlas : register(s1);

cbuffer WorldCB : register(b0) {
    float2 srcSize;
    float2 dstSize;
    uint filterMode;
    uint useBgra;
    float2 pad;
    float4 replacementFade;
};

struct PSIn {
    float4 pos                     : SV_Position;
    nointerpolation float4 p01     : TEXCOORD0;
    nointerpolation float4 p23     : TEXCOORD1;
    nointerpolation float4 uv01    : TEXCOORD2;
    nointerpolation float4 uv23    : TEXCOORD3;
    nointerpolation float4 uvClamp : TEXCOORD4;
    nointerpolation float4 color   : COLOR0;
    nointerpolation float4 flags   : COLOR1;
    nointerpolation float4 texInfo : TEXCOORD5;
    nointerpolation float4 box     : TEXCOORD6;
};

// One mip level of an atlas cell: clamp to the texel centres that lie fully inside the uv_box at
// that level, so bilinear taps never reach a neighbouring cell. texInfo = (width, height, maxLevel),
// box = inclusive normalized box (minU, minV, maxU, maxV).
float4 SampleAtlasLevel(float2 uv, float level, float4 texInfo, float4 box) {
    float scale = exp2(level);
    float2 size = max(floor(texInfo.xy / scale), float2(1.0, 1.0));
    float2 lo = ceil(box.xy * size);
    float2 hi = max(floor(box.zw * size) - 1.0, lo);
    float2 t = clamp(uv * size, lo + 0.5, hi + 0.5);
    return texQuad.SampleLevel(sampAtlas, t / size, level);
}

// Trilinear filtering done by hand for atlas cells (per-level clamp), using the analytic gradients.
float4 SampleAtlas(float2 uv, float2 gx, float2 gy, float4 texInfo, float4 box) {
    float2 sx = gx * texInfo.xy;
    float2 sy = gy * texInfo.xy;
    float rho2 = max(dot(sx, sx), dot(sy, sy));
    float lod = clamp(0.5 * log2(max(rho2, 1e-12)), 0.0, texInfo.z);
    float l0 = floor(lod);
    float l1 = min(l0 + 1.0, texInfo.z);
    float4 c0 = SampleAtlasLevel(uv, l0, texInfo, box);
    float4 c1 = SampleAtlasLevel(uv, l1, texInfo, box);
    return lerp(c0, c1, lod - l0);
}

// flags.x: 1 = flat colour quad.
// flags.y: 0 = B4 path (point sampler, level chosen+log2(k)); 1 = SampleGrad on the mip chain with the
//          trilinear/anisotropic sampler; 2 = atlas cell, per-level clamped trilinear.
// flags.z: 1 = texture pack replacement. Originals are converted through the current palette and
//          already fade; replacements are fixed full-brightness RGBA, so they get replacementFade.
float4 main(PSIn p) : SV_Target {
    if (p.flags.x > 0.5) {
        return p.color;
    }

    float curX = p.pos.x * (srcSize.x / dstSize.x);
    float curY = p.pos.y * (srcSize.y / dstSize.y);

    float2 pts[4] = { p.p01.xy, p.p01.zw, p.p23.xy, p.p23.zw };
    float2 uvs[4] = { p.uv01.xy, p.uv01.zw, p.uv23.xy, p.uv23.zw };

    // Engine scanline mapping (quad_mapping.h, shared with hook_test).
    QuadUvSample m = QuadMapUv(pts, uvs, curX, curY);
    float2 finalUv = clamp(m.uv, p.uvClamp.xy, p.uvClamp.zw);

    // Gradients per render-target pixel come from the analytic scanline derivatives of the
    // unclamped mapping, never from the clamped value, so quad edges do not spike the mip level.
    // All sampling happens before the discard and uses explicit LOD or gradients.
    float4 texel;
    if (p.flags.y < 0.5) {
        texel = texQuad.SampleLevel(samp, finalUv, p.texInfo.z);
    } else {
        float2 gx = m.dUvDx * (srcSize.x / dstSize.x);
        float2 gy = m.dUvDy * (srcSize.y / dstSize.y);
        if (p.flags.y < 1.5) {
            texel = texQuad.SampleGrad(samp, finalUv, gx, gy);
        } else {
            texel = SampleAtlas(m.uv, gx, gy, p.texInfo, p.box);
        }
    }
    // Textures are stored premultiplied (index 0 = transparent black), so filtering never pulls
    // dark or coloured fringes in from transparent texels; convert back to straight colour.
    if (texel.a >= 0.5) {
        texel.rgb = texel.rgb / texel.a;
    }
    if (texel.a < 0.5) {
        discard;
    }
    if (p.flags.z > 0.5) {
        texel.rgb *= replacementFade.rgb;
    }
    return texel * p.color;
}
)";

// The world pixel shader is the shared quad mapping (quad_mapping.h) followed by its main().
std::string BuildWorldPixelShaderSource() {
    std::string src = RR_QUAD_MAPPING_HLSL;
    src += "\n";
    src += kWorldPixelShaderSrc;
    return src;
}

struct WorldVertex {
    float x, y;
    float p01[4];
    float p23[4];
    float uv01[4];
    float uv23[4];
    float uvClamp[4];
    float color[4];
    float flags[4];
    float texInfo[4];
    float box[4];
};

struct WorldDrawBatch {
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    D3D11_RECT scissor = {};
    UINT startVertex = 0;
    UINT vertexCount = 0;
};

constexpr size_t kMaxWorldVertices = 65536;

class D3D11Presenter {
public:
    bool IsLatchedOff() const {
        return m_failureState.latchedOff || m_inBackoffCooldown;
    }

    bool GpuWorldAllowed() const {
        return PresenterStateAllowsGpuWorld(m_failureState) && !m_inBackoffCooldown;
    }

    bool Present8bpp(HWND hwnd, const uint8_t* surface, int pitch, const uint8_t* palette, int width, int height) {
        const uint64_t nowMs = GetTickCount64();
        if (!PresenterCanAttempt(m_failureState, nowMs)) {
            m_inBackoffCooldown = true;
            return false;
        }
        m_inBackoffCooldown = false;

        if (!hwnd || !surface || !palette || width <= 0 || height <= 0) {
            return false;
        }
        int clientW = 0;
        int clientH = 0;
        if (!PrepareFrame(hwnd, width, height, clientW, clientH, nowMs)) {
            return false;
        }
        if (clientW <= 0 || clientH <= 0) {
            return true;
        }

        if (GetConfig().gpuWorld && CaptureHasWorldFrame()) {
            return PresentWorldFrame(surface, pitch, palette, width, height, clientW, clientH, nowMs);
        }

        // Upload 8bpp indexed surface
        D3D11_MAPPED_SUBRESOURCE mappedSurf = {};
        HRESULT hr = m_context->Map(m_tex8bpp.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedSurf);
        if (FAILED(hr)) {
            LOG_ERROR("D3D11 Map(tex8bpp) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return RecordFailure(nowMs);
        }
        uint8_t* dstRows = static_cast<uint8_t*>(mappedSurf.pData);
        const uint8_t* srcRows = surface;
        for (int y = 0; y < height; ++y) {
            std::memcpy(dstRows + static_cast<size_t>(y) * mappedSurf.RowPitch,
                        srcRows + static_cast<ptrdiff_t>(y) * pitch,
                        static_cast<size_t>(width));
        }
        m_context->Unmap(m_tex8bpp.Get(), 0);

        // Upload 256-entry palette (PALETTEENTRY is R, G, B, Flags -> matches R8G8B8A8_UNORM RGB channels)
        D3D11_MAPPED_SUBRESOURCE mappedPal = {};
        hr = m_context->Map(m_texPalette.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedPal);
        if (FAILED(hr)) {
            LOG_ERROR("D3D11 Map(texPalette) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return RecordFailure(nowMs);
        }
        uint8_t* dstPal = static_cast<uint8_t*>(mappedPal.pData);
        for (int i = 0; i < 256; ++i) {
            dstPal[i * 4 + 0] = palette[i * 4 + 0];
            dstPal[i * 4 + 1] = palette[i * 4 + 1];
            dstPal[i * 4 + 2] = palette[i * 4 + 2];
            dstPal[i * 4 + 3] = 0xFF;
        }
        m_context->Unmap(m_texPalette.Get(), 0);

        return DrawAndPresent(width, height, clientW, clientH, false, m_srvBgra.Get(), nowMs);
    }

    bool PresentBgra(HWND hwnd, const uint32_t* dib, int width, int height) {
        const uint64_t nowMs = GetTickCount64();
        if (!PresenterCanAttempt(m_failureState, nowMs)) {
            m_inBackoffCooldown = true;
            return false;
        }
        m_inBackoffCooldown = false;

        if (!hwnd || !dib || width <= 0 || height <= 0) {
            return false;
        }
        int clientW = 0;
        int clientH = 0;
        if (!PrepareFrame(hwnd, width, height, clientW, clientH, nowMs)) {
            return false;
        }
        if (clientW <= 0 || clientH <= 0) {
            return true;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = m_context->Map(m_texBgra.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            LOG_ERROR("D3D11 Map(texBgra) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return RecordFailure(nowMs);
        }
        uint8_t* dstRows = static_cast<uint8_t*>(mapped.pData);
        const uint8_t* srcRows = reinterpret_cast<const uint8_t*>(dib);
        const size_t rowBytes = static_cast<size_t>(width) * sizeof(uint32_t);
        for (int y = 0; y < height; ++y) {
            std::memcpy(dstRows + static_cast<size_t>(y) * mapped.RowPitch,
                        srcRows + static_cast<size_t>(y) * rowBytes,
                        rowBytes);
        }
        m_context->Unmap(m_texBgra.Get(), 0);

        return DrawAndPresent(width, height, clientW, clientH, true, m_srvBgra.Get(), nowMs);
    }

    void NotifyWindowChange() {
        m_windowChangePending = true;
    }

    PresenterSubPixelStats TakeSubPixelStats() {
        const PresenterSubPixelStats s = m_subPixelStats;
        m_subPixelStats = PresenterSubPixelStats{};
        return s;
    }

    void Shutdown() {
        if (m_textureDumpInitialized) {
            TextureDumpShutdown();
            m_textureDumpInitialized = false;
        }
        if (m_texturePackInitialized) {
            TexturePackShutdown();
            m_texturePackInitialized = false;
        }
        TexturePackClearD3DResources();
        m_worldTexSrvCache.clear();
        m_fadePaletteValid = false;
        m_srvWorld.Reset();
        m_rtvWorld.Reset();
        m_texWorld.Reset();
        m_srvLayerRgba.Reset();
        m_texLayerRgba.Reset();
        m_srvWhite.Reset();
        m_texWhite.Reset();
        m_samplerWrap.Reset();
        m_samplerClamp.Reset();
        m_samplerFilteredWrap.Reset();
        m_samplerFilteredClamp.Reset();
        m_samplerAtlas.Reset();
        m_rasterScissor.Reset();
        m_blendAlpha.Reset();
        m_worldVB.Reset();
        m_worldInputLayout.Reset();
        m_layerPS.Reset();
        m_layerVS.Reset();
        m_worldPS.Reset();
        m_worldVS.Reset();
        m_srvBgra.Reset();
        m_texBgra.Reset();
        m_srvPalette.Reset();
        m_texPalette.Reset();
        m_srv8bpp.Reset();
        m_tex8bpp.Reset();
        m_constantBuffer.Reset();
        m_pixelShader.Reset();
        m_vertexShader.Reset();
        m_rtv.Reset();
        m_swapChain.Reset();
        m_context.Reset();
        m_device.Reset();
        m_hwnd = nullptr;
        m_backbufferWidth = 0;
        m_backbufferHeight = 0;
        m_srcWidth = 0;
        m_srcHeight = 0;
        m_worldWidth = 0;
        m_worldHeight = 0;
        m_windowChangePending = false;
    }

private:
    bool RecordFailure(uint64_t nowMs) {
        Shutdown();
        if (PresenterRecordFailure(m_failureState, nowMs)) {
            LOG_ERROR("D3D11 presenter latched off after %d failures within %llu ms; staying on GDI for this session",
                      m_failureState.failureCount,
                      static_cast<unsigned long long>(kPresenterFailureWindowMs));
        }
        return false;
    }

    bool PrepareFrame(HWND hwnd, int srcW, int srcH, int& outClientW, int& outClientH, uint64_t nowMs) {
        RECT rc = {};
        if (!GetClientRect(hwnd, &rc)) {
            return RecordFailure(nowMs);
        }
        outClientW = rc.right - rc.left;
        outClientH = rc.bottom - rc.top;
        if (outClientW <= 0 || outClientH <= 0) {
            return true;
        }

        if (!m_device || m_hwnd != hwnd) {
            Shutdown();
            if (!InitializeDeviceAndSwapChain(hwnd, outClientW, outClientH)) {
                return RecordFailure(nowMs);
            }
        } else if (outClientW != m_backbufferWidth || outClientH != m_backbufferHeight || m_windowChangePending) {
            if (!ResizeBackbuffer(outClientW, outClientH)) {
                return RecordFailure(nowMs);
            }
            m_windowChangePending = false;
        }

        if (srcW != m_srcWidth || srcH != m_srcHeight || !m_tex8bpp || !m_texBgra) {
            if (!CreateSourceTextures(srcW, srcH)) {
                return RecordFailure(nowMs);
            }
        }
        return true;
    }

    bool InitializeDeviceAndSwapChain(HWND hwnd, int clientW, int clientH) {
        UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0
        };
        D3D_FEATURE_LEVEL achievedLevel = D3D_FEATURE_LEVEL_11_0;

        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            createFlags,
            featureLevels,
            static_cast<UINT>(sizeof(featureLevels) / sizeof(featureLevels[0])),
            D3D11_SDK_VERSION,
            &m_device,
            &achievedLevel,
            &m_context);

        if (FAILED(hr)) {
            LOG_WARN("Hardware D3D11CreateDevice failed (0x%08lX), trying WARP", static_cast<unsigned long>(hr));
            hr = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                createFlags,
                featureLevels,
                static_cast<UINT>(sizeof(featureLevels) / sizeof(featureLevels[0])),
                D3D11_SDK_VERSION,
                &m_device,
                &achievedLevel,
                &m_context);
            if (FAILED(hr)) {
                LOG_ERROR("D3D11CreateDevice failed: 0x%08lX", static_cast<unsigned long>(hr));
                return false;
            }
        }

        ComPtr<IDXGIDevice> dxgiDevice;
        hr = m_device.As(&dxgiDevice);
        if (FAILED(hr)) {
            LOG_ERROR("QueryInterface(IDXGIDevice) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }

        ComPtr<IDXGIAdapter> adapter;
        hr = dxgiDevice->GetAdapter(&adapter);
        if (FAILED(hr)) {
            LOG_ERROR("GetAdapter failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }

        ComPtr<IDXGIFactory> factory;
        hr = adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(factory.GetAddressOf()));
        if (FAILED(hr)) {
            LOG_ERROR("GetParent(IDXGIFactory) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }

        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferDesc.Width = static_cast<UINT>(clientW);
        sd.BufferDesc.Height = static_cast<UINT>(clientH);
        sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.OutputWindow = hwnd;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        hr = factory->CreateSwapChain(m_device.Get(), &sd, &m_swapChain);
        if (FAILED(hr)) {
            LOG_WARN("FLIP_DISCARD swap chain failed (0x%08lX), falling back to DISCARD", static_cast<unsigned long>(hr));
            sd.BufferCount = 1;
            sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            hr = factory->CreateSwapChain(m_device.Get(), &sd, &m_swapChain);
            if (FAILED(hr)) {
                LOG_ERROR("CreateSwapChain failed: 0x%08lX", static_cast<unsigned long>(hr));
                return false;
            }
        }

        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);

        if (!CreateBackbufferRTV()) {
            return false;
        }
        if (!CompileShadersAndResources()) {
            return false;
        }

        m_hwnd = hwnd;
        m_backbufferWidth = clientW;
        m_backbufferHeight = clientH;
        m_windowChangePending = false;

        LOG_INFO("D3D11 presenter initialized on HWND %p (%dx%d, feature level 0x%X, filter=%s, vsync=%d)",
                 hwnd, clientW, clientH, static_cast<unsigned int>(achievedLevel),
                 FilterModeToString(GetConfig().filter), GetConfig().vsync ? 1 : 0);
        return true;
    }

    bool CreateBackbufferRTV() {
        m_rtv.Reset();
        ComPtr<ID3D11Texture2D> backBuffer;
        HRESULT hr = m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                            reinterpret_cast<void**>(backBuffer.GetAddressOf()));
        if (FAILED(hr)) {
            LOG_ERROR("SwapChain::GetBuffer failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }
        hr = m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_rtv);
        if (FAILED(hr)) {
            LOG_ERROR("CreateRenderTargetView failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }
        return true;
    }

    bool ResizeBackbuffer(int clientW, int clientH) {
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
        m_rtv.Reset();

        HRESULT hr = m_swapChain->ResizeBuffers(0, static_cast<UINT>(clientW), static_cast<UINT>(clientH),
                                                DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            LOG_ERROR("SwapChain::ResizeBuffers(%dx%d) failed: 0x%08lX",
                      clientW, clientH, static_cast<unsigned long>(hr));
            return false;
        }
        if (!CreateBackbufferRTV()) {
            return false;
        }
        m_backbufferWidth = clientW;
        m_backbufferHeight = clientH;
        LOG_INFO("D3D11 presenter resized swap chain to %dx%d", clientW, clientH);
        return true;
    }

    bool CompileShadersAndResources() {
        ComPtr<ID3DBlob> vsBlob;
        ComPtr<ID3DBlob> errBlob;
        HRESULT hr = D3DCompile(kVertexShaderSrc, std::strlen(kVertexShaderSrc), "present_vs",
                                nullptr, nullptr, "main", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                &vsBlob, &errBlob);
        if (FAILED(hr)) {
            LOG_ERROR("VS compile failed (0x%08lX): %s", static_cast<unsigned long>(hr),
                      errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "");
            return false;
        }
        hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                          nullptr, &m_vertexShader);
        if (FAILED(hr)) {
            LOG_ERROR("CreateVertexShader failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }

        ComPtr<ID3DBlob> psBlob;
        errBlob.Reset();
        hr = D3DCompile(kPixelShaderSrc, std::strlen(kPixelShaderSrc), "present_ps",
                        nullptr, nullptr, "main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                        &psBlob, &errBlob);
        if (FAILED(hr)) {
            LOG_ERROR("PS compile failed (0x%08lX): %s", static_cast<unsigned long>(hr),
                      errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "");
            return false;
        }
        hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                         nullptr, &m_pixelShader);
        if (FAILED(hr)) {
            LOG_ERROR("CreatePixelShader failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }

        D3D11_BUFFER_DESC cbd = {};
        cbd.ByteWidth = sizeof(PresentConstants);
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = m_device->CreateBuffer(&cbd, nullptr, &m_constantBuffer);
        if (FAILED(hr)) {
            LOG_ERROR("CreateBuffer(ConstantBuffer) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }

        D3D11_TEXTURE2D_DESC palDesc = {};
        palDesc.Width = 256;
        palDesc.Height = 1;
        palDesc.MipLevels = 1;
        palDesc.ArraySize = 1;
        palDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        palDesc.SampleDesc.Count = 1;
        palDesc.Usage = D3D11_USAGE_DYNAMIC;
        palDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        palDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = m_device->CreateTexture2D(&palDesc, nullptr, &m_texPalette);
        if (FAILED(hr)) {
            LOG_ERROR("CreateTexture2D(texPalette) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }
        hr = m_device->CreateShaderResourceView(m_texPalette.Get(), nullptr, &m_srvPalette);
        if (FAILED(hr)) {
            LOG_ERROR("CreateShaderResourceView(srvPalette) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }

        // Compile world VS & PS for Task T5 (1440x1080 3D world rendering)
        ComPtr<ID3DBlob> worldVsBlob;
        errBlob.Reset();
        hr = D3DCompile(kWorldVertexShaderSrc, std::strlen(kWorldVertexShaderSrc), "world_vs",
                        nullptr, nullptr, "main", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                        &worldVsBlob, &errBlob);
        if (FAILED(hr)) {
            LOG_ERROR("World VS compile failed (0x%08lX): %s", static_cast<unsigned long>(hr),
                      errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "");
            return false;
        }
        hr = m_device->CreateVertexShader(worldVsBlob->GetBufferPointer(), worldVsBlob->GetBufferSize(),
                                          nullptr, &m_worldVS);
        if (FAILED(hr)) {
            LOG_ERROR("CreateVertexShader(worldVS) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }

        D3D11_INPUT_ELEMENT_DESC ied[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,   D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8,   D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24,  D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 40,  D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 56,  D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 72,  D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 88,  D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 104, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 5, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 120, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 6, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 136, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        hr = m_device->CreateInputLayout(ied, static_cast<UINT>(sizeof(ied) / sizeof(ied[0])),
                                         worldVsBlob->GetBufferPointer(), worldVsBlob->GetBufferSize(),
                                         &m_worldInputLayout);
        if (FAILED(hr)) {
            LOG_ERROR("CreateInputLayout(world) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }

        ComPtr<ID3DBlob> layerVsBlob;
        errBlob.Reset();
        hr = D3DCompile(kLayerVertexShaderSrc, std::strlen(kLayerVertexShaderSrc), "layer_vs",
                        nullptr, nullptr, "main", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                        &layerVsBlob, &errBlob);
        if (FAILED(hr)) {
            LOG_ERROR("Layer VS compile failed (0x%08lX): %s", static_cast<unsigned long>(hr),
                      errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "");
            return false;
        }
        hr = m_device->CreateVertexShader(layerVsBlob->GetBufferPointer(), layerVsBlob->GetBufferSize(),
                                          nullptr, &m_layerVS);
        if (FAILED(hr)) {
            LOG_ERROR("CreateVertexShader(layerVS) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }

        ComPtr<ID3DBlob> layerPsBlob;
        errBlob.Reset();
        hr = D3DCompile(kLayerPixelShaderSrc, std::strlen(kLayerPixelShaderSrc), "layer_ps",
                        nullptr, nullptr, "main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                        &layerPsBlob, &errBlob);
        if (FAILED(hr)) {
            LOG_ERROR("Layer PS compile failed (0x%08lX): %s", static_cast<unsigned long>(hr),
                      errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "");
            return false;
        }
        hr = m_device->CreatePixelShader(layerPsBlob->GetBufferPointer(), layerPsBlob->GetBufferSize(),
                                         nullptr, &m_layerPS);
        if (FAILED(hr)) {
            LOG_ERROR("CreatePixelShader(layerPS) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }

        ComPtr<ID3DBlob> worldPsBlob;
        errBlob.Reset();
        const std::string worldPsSource = BuildWorldPixelShaderSource();
        hr = D3DCompile(worldPsSource.c_str(), worldPsSource.size(), "world_ps",
                        nullptr, nullptr, "main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                        &worldPsBlob, &errBlob);
        if (FAILED(hr)) {
            LOG_ERROR("World PS compile failed (0x%08lX): %s", static_cast<unsigned long>(hr),
                      errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "");
            return false;
        }
        hr = m_device->CreatePixelShader(worldPsBlob->GetBufferPointer(), worldPsBlob->GetBufferSize(),
                                         nullptr, &m_worldPS);
        if (FAILED(hr)) {
            LOG_ERROR("CreatePixelShader(worldPS) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }

        D3D11_BUFFER_DESC vbd = {};
        vbd.ByteWidth = static_cast<UINT>(kMaxWorldVertices * sizeof(WorldVertex));
        vbd.Usage = D3D11_USAGE_DYNAMIC;
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = m_device->CreateBuffer(&vbd, nullptr, &m_worldVB);
        if (FAILED(hr)) {
            LOG_ERROR("CreateBuffer(worldVB) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }

        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.ScissorEnable = TRUE;
        rd.DepthClipEnable = TRUE;
        hr = m_device->CreateRasterizerState(&rd, &m_rasterScissor);
        if (FAILED(hr)) {
            LOG_ERROR("CreateRasterizerState failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }

        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = m_device->CreateBlendState(&bd, &m_blendAlpha);
        if (FAILED(hr)) {
            LOG_ERROR("CreateBlendState failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }

        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = m_device->CreateSamplerState(&sd, &m_samplerWrap);
        if (FAILED(hr)) {
            LOG_ERROR("CreateSamplerState(Wrap) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        hr = m_device->CreateSamplerState(&sd, &m_samplerClamp);
        if (FAILED(hr)) {
            LOG_ERROR("CreateSamplerState(Clamp) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }

        // T6 filtered samplers: trilinear, or anisotropic when [World] Anisotropy is 2..16.
        const int aniso = GetConfig().anisotropy;
        D3D11_SAMPLER_DESC fd = {};
        fd.Filter = (aniso >= 2) ? D3D11_FILTER_ANISOTROPIC : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        fd.MaxAnisotropy = (aniso >= 2) ? static_cast<UINT>(aniso) : 1u;
        fd.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        fd.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        fd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        fd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = m_device->CreateSamplerState(&fd, &m_samplerFilteredWrap);
        if (FAILED(hr)) {
            LOG_ERROR("CreateSamplerState(FilteredWrap) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }
        fd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        fd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        fd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        hr = m_device->CreateSamplerState(&fd, &m_samplerFilteredClamp);
        if (FAILED(hr)) {
            LOG_ERROR("CreateSamplerState(FilteredClamp) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }
        fd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        fd.MaxAnisotropy = 1;
        hr = m_device->CreateSamplerState(&fd, &m_samplerAtlas);
        if (FAILED(hr)) {
            LOG_ERROR("CreateSamplerState(Atlas) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }

        const uint32_t whitePixel = 0xFFFFFFFFu;
        D3D11_SUBRESOURCE_DATA whiteInit = {&whitePixel, 4, 4};
        D3D11_TEXTURE2D_DESC wd = {};
        wd.Width = 1;
        wd.Height = 1;
        wd.MipLevels = 1;
        wd.ArraySize = 1;
        wd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        wd.SampleDesc.Count = 1;
        wd.Usage = D3D11_USAGE_IMMUTABLE;
        wd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr = m_device->CreateTexture2D(&wd, &whiteInit, &m_texWhite);
        if (FAILED(hr)) {
            LOG_ERROR("CreateTexture2D(texWhite) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }
        hr = m_device->CreateShaderResourceView(m_texWhite.Get(), nullptr, &m_srvWhite);
        if (FAILED(hr)) {
            LOG_ERROR("CreateShaderResourceView(srvWhite) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }

        return true;
    }

    bool CreateSourceTextures(int width, int height) {
        m_srv8bpp.Reset();
        m_tex8bpp.Reset();
        m_srvBgra.Reset();
        m_texBgra.Reset();
        m_srvLayerRgba.Reset();
        m_texLayerRgba.Reset();

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(width);
        td.Height = static_cast<UINT>(height);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8_UINT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        HRESULT hr = m_device->CreateTexture2D(&td, nullptr, &m_tex8bpp);
        if (FAILED(hr)) {
            LOG_ERROR("CreateTexture2D(tex8bpp %dx%d) failed: 0x%08lX", width, height, static_cast<unsigned long>(hr));
            return false;
        }
        hr = m_device->CreateShaderResourceView(m_tex8bpp.Get(), nullptr, &m_srv8bpp);
        if (FAILED(hr)) {
            LOG_ERROR("CreateShaderResourceView(srv8bpp) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }

        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        hr = m_device->CreateTexture2D(&td, nullptr, &m_texBgra);
        if (FAILED(hr)) {
            LOG_ERROR("CreateTexture2D(texBgra %dx%d) failed: 0x%08lX", width, height, static_cast<unsigned long>(hr));
            return false;
        }
        hr = m_device->CreateShaderResourceView(m_texBgra.Get(), nullptr, &m_srvBgra);
        if (FAILED(hr)) {
            LOG_ERROR("CreateShaderResourceView(srvBgra) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }

        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        hr = m_device->CreateTexture2D(&td, nullptr, &m_texLayerRgba);
        if (FAILED(hr)) {
            LOG_ERROR("CreateTexture2D(texLayerRgba %dx%d) failed: 0x%08lX", width, height, static_cast<unsigned long>(hr));
            return false;
        }
        hr = m_device->CreateShaderResourceView(m_texLayerRgba.Get(), nullptr, &m_srvLayerRgba);
        if (FAILED(hr)) {
            LOG_ERROR("CreateShaderResourceView(srvLayerRgba) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }

        m_srcWidth = width;
        m_srcHeight = height;
        return true;
    }

    bool EnsureWorldRenderTarget(int worldW, int worldH) {
        if (m_texWorld && m_rtvWorld && m_srvWorld &&
            m_worldWidth == worldW && m_worldHeight == worldH) {
            return true;
        }
        m_srvWorld.Reset();
        m_rtvWorld.Reset();
        m_texWorld.Reset();

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(worldW);
        td.Height = static_cast<UINT>(worldH);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = m_device->CreateTexture2D(&td, nullptr, &m_texWorld);
        if (FAILED(hr)) {
            LOG_ERROR("CreateTexture2D(texWorld %dx%d) failed: 0x%08lX",
                      worldW, worldH, static_cast<unsigned long>(hr));
            return false;
        }
        hr = m_device->CreateRenderTargetView(m_texWorld.Get(), nullptr, &m_rtvWorld);
        if (FAILED(hr)) {
            LOG_ERROR("CreateRenderTargetView(rtvWorld) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }
        hr = m_device->CreateShaderResourceView(m_texWorld.Get(), nullptr, &m_srvWorld);
        if (FAILED(hr)) {
            LOG_ERROR("CreateShaderResourceView(srvWorld) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }
        m_worldWidth = worldW;
        m_worldHeight = worldH;
        LOG_INFO("Created offscreen GPU world render target %dx%d", worldW, worldH);
        return true;
    }

    // Returns the SRV of the quad's texture; *outLevels receives its uploaded mip level count and
    // *outReplacement whether the SRV is a texture pack replacement.
    ID3D11ShaderResourceView* GetOrCreateWorldTextureSRV(const CapturedQuad& q, const uint8_t* palette,
                                                         int32_t* outLevels, int32_t* outK = nullptr,
                                                         bool* outReplacement = nullptr) {
        if (outLevels) {
            *outLevels = 1;
        }
        if (outK) {
            *outK = 1;
        }
        if (outReplacement) {
            *outReplacement = false;
        }
        if (q.pdatPtr == 0 || q.texWidth <= 0 || q.texHeight <= 0) {
            return m_srvWhite.Get();
        }
        const bool colorKey = (q.pdatFlags & 0x02u) != 0;
        const uint64_t srvKey =
            (colorKey ? (1ULL << 63) : 0ULL) |
            (static_cast<uint64_t>(q.pdatPtr) << 32) |
            static_cast<uint64_t>(q.level0Crc32);

        auto it = m_worldTexSrvCache.find(srvKey);
        if (it != m_worldTexSrvCache.end()) {
            if (outLevels) {
                *outLevels = it->second.levels;
            }
            if (outK) {
                *outK = it->second.k;
            }
            if (outReplacement) {
                *outReplacement = it->second.replacement;
            }
            return it->second.srv.Get();
        }

        if (GetConfig().texturePackDump) {
            TextureMipChainView chain;
            if (CaptureLookupTextureMipChain(q.pdatPtr, q.level0Crc32, chain) && chain.levels > 0 &&
                chain.width[0] > 0 && chain.height[0] > 0) {
                TextureDumpInfo dumpInfo;
                dumpInfo.id = FormatTextureId(q.texWidth, q.texHeight, q.level0Crc32);
                dumpInfo.width = chain.width[0];
                dumpInfo.height = chain.height[0];
                dumpInfo.colorKey = colorKey;
                dumpInfo.mipLevels = q.mipLevels;
                dumpInfo.kind = ((q.pdatFlags & 0x2000u) != 0) ? "sprite" : "texture";
                dumpInfo.spriteClut = q.spriteClut;
                dumpInfo.frameNumber = CaptureGetFrameNumber();
                // Converted with the base palette, not this frame's (possibly faded) palette.
                TextureDumpQueue(dumpInfo, chain.pixels[0]);
            }
        }

        // Set when the original is cached while its replacement is queued or decoding, so a pack
        // generation change can drop just this entry once the replacement is ready.
        std::string pendingReplacementId;
        if (GetConfig().texturePackEnabled) {
            std::string texPackId = FormatTextureId(q.texWidth, q.texHeight, q.level0Crc32);
            bool replacementPending = false;
            ReplacementTexture* rep =
                TexturePackGetOrQueue(texPackId, q.texWidth, q.texHeight, &replacementPending);
            if (replacementPending) {
                pendingReplacementId = std::move(texPackId);
            }
            if (rep && rep->srv) {
                if (outLevels) {
                    *outLevels = rep->levels;
                }
                if (outK) {
                    *outK = rep->k;
                }
                if (outReplacement) {
                    *outReplacement = true;
                }
                ID3D11ShaderResourceView* rawSrv = rep->srv.Get();
                m_worldTexSrvCache.emplace(srvKey, CachedWorldTexture{rep->srv, rep->levels, rep->k,
                                                                      std::string(), true});
                return rawSrv;
            }
        }

        TextureMipChainView chain;
        if (!CaptureLookupTextureMipChain(q.pdatPtr, q.level0Crc32, chain) || chain.levels <= 0 ||
            chain.width[0] <= 0 || chain.height[0] <= 0) {
            return m_srvWhite.Get();
        }
        // MipFilter=chosen keeps the B4 single level; trilinear uploads every engine level.
        const int32_t levels = (GetConfig().mipFilter == MipFilter::Trilinear) ? chain.levels : 1;
        const int32_t tw = chain.width[0];
        const int32_t th = chain.height[0];

        // Palette conversion with index 0 as alpha 0 for colour-keyed textures. A transparent texel
        // is stored as (0, 0, 0, 0), so every level is premultiplied.
        size_t totalTexels = 0;
        for (int32_t l = 0; l < levels; ++l) {
            totalTexels += static_cast<size_t>(chain.width[l]) * static_cast<size_t>(chain.height[l]);
        }
        m_tempRgbaUpload.resize(totalTexels);
        D3D11_SUBRESOURCE_DATA initData[kMaxTextureMipLevels] = {};
        size_t offset = 0;
        for (int32_t l = 0; l < levels; ++l) {
            const size_t count = static_cast<size_t>(chain.width[l]) * static_cast<size_t>(chain.height[l]);
            uint32_t* dst = m_tempRgbaUpload.data() + offset;
            const uint8_t* src = chain.pixels[l];
            for (size_t i = 0; i < count; ++i) {
                const uint8_t idx = src[i];
                if (colorKey && idx == 0) {
                    dst[i] = 0x00000000u;
                } else {
                    const uint32_t r = palette[idx * 4 + 0];
                    const uint32_t g = palette[idx * 4 + 1];
                    const uint32_t b = palette[idx * 4 + 2];
                    dst[i] = r | (g << 8) | (b << 16) | 0xFF000000u;
                }
            }
            initData[l].pSysMem = dst;
            initData[l].SysMemPitch = static_cast<UINT>(chain.width[l] * sizeof(uint32_t));
            offset += count;
        }

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(tw);
        td.Height = static_cast<UINT>(th);
        td.MipLevels = static_cast<UINT>(levels);
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        ComPtr<ID3D11Texture2D> tex;
        HRESULT hr = m_device->CreateTexture2D(&td, initData, &tex);
        if (FAILED(hr)) {
            return m_srvWhite.Get();
        }
        ComPtr<ID3D11ShaderResourceView> srv;
        hr = m_device->CreateShaderResourceView(tex.Get(), nullptr, &srv);
        if (FAILED(hr)) {
            return m_srvWhite.Get();
        }
        if (outLevels) {
            *outLevels = levels;
        }
        ID3D11ShaderResourceView* rawSrv = srv.Get();
        m_worldTexSrvCache.emplace(srvKey, CachedWorldTexture{std::move(srv), levels, 1,
                                                              std::move(pendingReplacementId)});
        return rawSrv;
    }

    // When useHudSentinelKey is set, pixels equal to the frame's HUD sentinel become transparent and
    // a histogram of the layer is handed to the capture module (HUD sentinel collision detection).
    bool UploadLayerToRgbaTexture(const uint8_t* srcSurf, int srcPitch, const uint8_t* palette,
                                  int width, int height, bool useHudSentinelKey) {
        const uint8_t sentinel = CaptureGetHudSentinel();
        uint32_t hist[256] = {};
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = m_context->Map(m_texLayerRgba.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            LOG_ERROR("D3D11 Map(texLayerRgba) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }
        for (int y = 0; y < height; ++y) {
            auto* dstRow = reinterpret_cast<uint32_t*>(
                static_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch);
            const uint8_t* srcRow = srcSurf + static_cast<ptrdiff_t>(y) * srcPitch;
            for (int x = 0; x < width; ++x) {
                const uint8_t idx = srcRow[x];
                if (useHudSentinelKey) {
                    ++hist[idx];
                }
                if (useHudSentinelKey && idx == sentinel) {
                    dstRow[x] = 0x00000000u;
                } else {
                    const uint32_t r = palette[idx * 4 + 0];
                    const uint32_t g = palette[idx * 4 + 1];
                    const uint32_t b = palette[idx * 4 + 2];
                    dstRow[x] = r | (g << 8) | (b << 16) | 0xFF000000u;
                }
            }
        }
        m_context->Unmap(m_texLayerRgba.Get(), 0);
        if (useHudSentinelKey) {
            CaptureObserveHudLayer(hist);
        }
        return true;
    }

    bool DrawFullscreenLayerOnWorld(ID3D11ShaderResourceView* srv, int worldW, int worldH, bool alphaBlend) {
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(m_layerVS.Get(), nullptr, 0);
        m_context->PSSetShader(m_layerPS.Get(), nullptr, 0);
        m_context->PSSetShaderResources(0, 1, &srv);
        m_context->PSSetSamplers(0, 1, m_samplerClamp.GetAddressOf());
        m_context->RSSetState(m_rasterScissor.Get());

        const D3D11_RECT fullScissor = {0, 0, worldW, worldH};
        m_context->RSSetScissorRects(1, &fullScissor);

        const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        m_context->OMSetBlendState(alphaBlend ? m_blendAlpha.Get() : nullptr, blendFactor, 0xFFFFFFFFu);

        m_context->Draw(3, 0);
        return true;
    }

    // Texture pack replacements are fixed full-brightness RGBA, while originals are converted through
    // the current palette and fade with it. The world shader scales replacements by the per-channel
    // fade f = sum(current[1..255]) / sum(base[1..255]) against DATA\PALETTE.RAW. It is computed only
    // when the pack is enabled and has at least one replacement indexed, and only when the palette
    // changes; otherwise f stays 1 and no per-frame work is added.
    void UpdateReplacementFade(const uint8_t* palette) {
        if (!GetConfig().texturePackEnabled || TexturePackGetIndexedCount() == 0) {
            m_replacementFade[0] = m_replacementFade[1] = m_replacementFade[2] = 1.0f;
            return;
        }
        if (!m_basePaletteTried) {
            m_basePaletteTried = true;
            const std::wstring path = ResolveGamePath(kBasePaletteRelPath);
            m_basePaletteValid = LoadBasePaletteFile(path, m_basePalette);
            if (!m_basePaletteValid) {
                LOG_WARN("TexturePack: base palette %ls is missing or not %u bytes; replacements ignore palette fades",
                         path.c_str(), static_cast<unsigned>(kBasePaletteBytes));
            }
        }
        if (!m_basePaletteValid) {
            m_replacementFade[0] = m_replacementFade[1] = m_replacementFade[2] = 1.0f;
            return;
        }
        if (m_fadePaletteValid && std::memcmp(m_fadePalette, palette, kBasePaletteBytes) == 0) {
            return;
        }
        std::memcpy(m_fadePalette, palette, kBasePaletteBytes);
        m_fadePaletteValid = true;
        ComputePaletteFadeFactor(m_basePalette, palette, m_replacementFade);
        if (!m_nonUniformFadeWarned && !PaletteIsUniformScale(m_basePalette, palette, m_replacementFade)) {
            m_nonUniformFadeWarned = true;
            LOG_WARN("TexturePack: non-uniform palette effect, replacements may not match");
        }
    }

    bool PresentWorldFrame(const uint8_t* surface, int pitch, const uint8_t* palette,
                           int srcW, int srcH, int clientW, int clientH, uint64_t nowMs) {
        const int worldW = GetConfig().internalWidth;
        const int worldH = GetConfig().internalHeight;
        if (!EnsureWorldRenderTarget(worldW, worldH)) {
            return RecordFailure(nowMs);
        }

        if (GetConfig().texturePackDump && !m_textureDumpInitialized) {
            TextureDumpInit(ResolveGamePath(GetConfig().texturePackDumpPath),
                            ResolveGamePath(kBasePaletteRelPath));
            m_textureDumpInitialized = true;
        }

        if (GetConfig().texturePackEnabled && !m_texturePackInitialized) {
            TexturePackInit(ResolveGamePath(GetConfig().texturePackPath),
                            GetConfig().texturePackMaxMiB,
                            GetConfig().texturePackDecodeBudgetMs);
            m_texturePackInitialized = true;
        }
        if (GetConfig().texturePackEnabled) {
            TexturePackUpdate(m_device.Get());
        }
        UpdateReplacementFade(palette);

        const uint32_t curGen = CaptureGetCacheGeneration();
        const uint64_t tpGen = TexturePackGetGeneration();
        if (curGen != m_cachedWorldGen || std::memcmp(m_cachedPalette, palette, 1024) != 0) {
            m_worldTexSrvCache.clear();
            std::memcpy(m_cachedPalette, palette, 1024);
            m_cachedWorldGen = curGen;
            m_cachedTexPackGen = tpGen;
        } else if (tpGen != m_cachedTexPackGen) {
            // A replacement finished loading: rebuild only the originals that were waiting for it.
            InvalidateReadyReplacementEntries(m_worldTexSrvCache, [](const std::string& id) {
                return TexturePackGetState(id);
            });
            m_cachedTexPackGen = tpGen;
        }

        m_context->OMSetRenderTargets(1, m_rtvWorld.GetAddressOf(), nullptr);
        const float clearColor[4] = {
            palette[0] / 255.0f,
            palette[1] / 255.0f,
            palette[2] / 255.0f,
            1.0f
        };
        m_context->ClearRenderTargetView(m_rtvWorld.Get(), clearColor);

        D3D11_VIEWPORT worldVp = {};
        worldVp.TopLeftX = 0.0f;
        worldVp.TopLeftY = 0.0f;
        worldVp.Width = static_cast<FLOAT>(worldW);
        worldVp.Height = static_cast<FLOAT>(worldH);
        worldVp.MinDepth = 0.0f;
        worldVp.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &worldVp);

        // Pass 1: Layer 1 (2D Horizon Backdrop, 640x480)
        int32_t horizonHeight = 0;
        const uint8_t* horizonSurf = CaptureGetHorizonLayer(horizonHeight);
        if (horizonSurf != nullptr && srcW == 640 && srcH == 480) {
            if (!UploadLayerToRgbaTexture(horizonSurf, 640, palette, srcW, srcH, false)) {
                return RecordFailure(nowMs);
            }
            if (!DrawFullscreenLayerOnWorld(m_srvLayerRgba.Get(), worldW, worldH, false)) {
                return RecordFailure(nowMs);
            }
        }

        // Pass 2: Layer 2 (3D World Quads at internalWidth x internalHeight)
        uint32_t quadCount = 0;
        const CapturedQuad* quads = CaptureGetWorldQuads(quadCount);
        m_worldVerts.clear();
        m_worldBatches.clear();

        const float invSrcW = 1.0f / static_cast<float>(srcW);
        const float invSrcH = 1.0f / static_cast<float>(srcH);

        // Upload constant buffer with srcSize and dstSize for PS coordinate mapping
        {
            D3D11_MAPPED_SUBRESOURCE mappedCB = {};
            HRESULT hr = m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedCB);
            if (FAILED(hr)) {
                LOG_ERROR("D3D11 Map(constantBuffer) failed: 0x%08lX", static_cast<unsigned long>(hr));
                return RecordFailure(nowMs);
            }
            auto* cb = static_cast<PresentConstants*>(mappedCB.pData);
            cb->srcSize[0] = static_cast<float>(srcW);
            cb->srcSize[1] = static_cast<float>(srcH);
            cb->dstSize[0] = static_cast<float>(worldW);
            cb->dstSize[1] = static_cast<float>(worldH);
            cb->filterMode = 0;
            cb->useBgra = 0;
            cb->pad[0] = 0.0f;
            cb->pad[1] = 0.0f;
            cb->replacementFade[0] = m_replacementFade[0];
            cb->replacementFade[1] = m_replacementFade[1];
            cb->replacementFade[2] = m_replacementFade[2];
            cb->replacementFade[3] = 1.0f;
            m_context->Unmap(m_constantBuffer.Get(), 0);
        }

        const bool subPixel = GetConfig().subPixel;
        const bool trilinear = GetConfig().mipFilter == MipFilter::Trilinear;
        uint32_t frameFloatQuads = 0;
        uint32_t frameFallbackQuads = 0;
        for (uint32_t qi = 0; qi < quadCount; ++qi) {
            const CapturedQuad& q = quads[qi];
            if (q.culled) {
                continue;
            }
            if (m_worldVerts.size() + 6 > kMaxWorldVertices) {
                break;
            }

            // Convert 640x480 clipRect ([xmin, xmax) in X, [ymin, ymax] inclusive in Y) to worldW x worldH scissor
            const int clipL = std::max(0, std::min(srcW, q.clipRect[0]));
            const int clipT = std::max(0, std::min(srcH, q.clipRect[1]));
            const int clipR = std::max(0, std::min(srcW, q.clipRect[2]));
            const int clipB = std::max(0, std::min(srcH, q.clipRect[3] + 1));
            if (clipL >= clipR || clipT >= clipB) {
                continue;
            }

            D3D11_RECT sc = {};
            sc.left   = (clipL * worldW) / srcW;
            sc.top    = (clipT * worldH) / srcH;
            sc.right  = (clipR * worldW + srcW - 1) / srcW;
            sc.bottom = (clipB * worldH + srcH - 1) / srcH;

            ID3D11ShaderResourceView* srv = m_srvWhite.Get();
            ID3D11SamplerState* sampler = m_samplerClamp.Get();

            // Vertex positions in 640x480 pixels: the engine's snapped int16 vertices, or with
            // [World] SubPixel=1 the captured 16.16 corners they were snapped from (T6).
            float posX[4];
            float posY[4];
            for (int i = 0; i < 4; ++i) {
                posX[i] = static_cast<float>(q.verts[i].x);
                posY[i] = static_cast<float>(q.verts[i].y);
            }
            if (subPixel) {
                const SubPixelVertices spv = ResolveSubPixelVertices(q);
                if (spv.useCorners) {
                    for (int i = 0; i < 4; ++i) {
                        posX[i] = spv.x[i];
                        posY[i] = spv.y[i];
                    }
                    ++frameFloatQuads;
                } else if (spv.eligible) {
                    ++frameFallbackQuads;
                }
            }

            float p01[4] = {posX[0], posY[0], posX[1], posY[1]};
            float p23[4] = {posX[2], posY[2], posX[3], posY[3]};
            float uv01[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float uv23[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float uvClamp[4] = {0.0f, 0.0f, 1.0f, 1.0f};
            float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            float flags[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float texInfo[4] = {1.0f, 1.0f, 0.0f, 0.0f};
            float box[4] = {0.0f, 0.0f, 0.0f, 0.0f};

            float vx[4] = {};
            float vy[4] = {};

            if (q.kind == QuadKind::Flat) {
                const uint8_t idx = q.flatColor;
                color[0] = palette[idx * 4 + 0] / 255.0f;
                color[1] = palette[idx * 4 + 1] / 255.0f;
                color[2] = palette[idx * 4 + 2] / 255.0f;
                color[3] = 1.0f;
                flags[0] = 1.0f; // isFlat = 1.0

                int minX = q.verts[0].x, maxX = q.verts[0].x;
                int minY = q.verts[0].y, maxY = q.verts[0].y;
                for (int i = 1; i < 4; ++i) {
                    minX = std::min(minX, static_cast<int>(q.verts[i].x));
                    maxX = std::max(maxX, static_cast<int>(q.verts[i].x));
                    minY = std::min(minY, static_cast<int>(q.verts[i].y));
                    maxY = std::max(maxY, static_cast<int>(q.verts[i].y));
                }

                if (minY == maxY) {
                    const float xL = static_cast<float>(minX) * invSrcW * 2.0f - 1.0f;
                    const float xR = static_cast<float>(maxX) * invSrcW * 2.0f - 1.0f;
                    const float yT = 1.0f - static_cast<float>(minY) * invSrcH * 2.0f;
                    const float yB = 1.0f - static_cast<float>(minY + 1) * invSrcH * 2.0f;
                    vx[0] = xL; vy[0] = yT;
                    vx[1] = xR; vy[1] = yT;
                    vx[2] = xR; vy[2] = yB;
                    vx[3] = xL; vy[3] = yB;
                } else {
                    for (int i = 0; i < 4; ++i) {
                        vx[i] = posX[i] * invSrcW * 2.0f - 1.0f;
                        vy[i] = 1.0f - posY[i] * invSrcH * 2.0f;
                    }
                }
            } else {
                int32_t texLevels = 1;
                int32_t k = 1;
                bool replacement = false;
                srv = GetOrCreateWorldTextureSRV(q, palette, &texLevels, &k, &replacement);
                flags[2] = replacement ? 1.0f : 0.0f;
                const float texW = static_cast<float>(std::max(1, q.texWidth));
                const float texH = static_cast<float>(std::max(1, q.texHeight));

                uv01[0] = static_cast<float>(q.verts[0].u) / texW;
                uv01[1] = static_cast<float>(q.verts[0].v) / texH;
                uv01[2] = static_cast<float>(q.verts[1].u) / texW;
                uv01[3] = static_cast<float>(q.verts[1].v) / texH;

                uv23[0] = static_cast<float>(q.verts[2].u) / texW;
                uv23[1] = static_cast<float>(q.verts[2].v) / texH;
                uv23[2] = static_cast<float>(q.verts[3].u) / texW;
                uv23[3] = static_cast<float>(q.verts[3].v) / texH;

                const int32_t us[4] = {q.verts[0].u, q.verts[1].u, q.verts[2].u, q.verts[3].u};
                const int32_t vs[4] = {q.verts[0].v, q.verts[1].v, q.verts[2].v, q.verts[3].v};
                const QuadAddressing addr = ChooseQuadAddressing(
                    q.uvBox, (q.pdatFlags & 0x2000u) != 0, q.texWidth, q.texHeight, us, vs);

                const float replW = texW * static_cast<float>(k);
                const float replH = texH * static_cast<float>(k);
                texInfo[0] = replW;
                texInfo[1] = replH;
                box[0] = static_cast<float>(addr.clampTexel[0]) / texW;
                box[1] = static_cast<float>(addr.clampTexel[1]) / texH;
                box[2] = static_cast<float>(addr.clampTexel[2] + 1) / texW;
                box[3] = static_cast<float>(addr.clampTexel[3] + 1) / texH;

                const float invK = 1.0f / static_cast<float>(k);
                if (trilinear) {
                    const int32_t scaledBox[4] = {
                        addr.clampTexel[0] * k,
                        addr.clampTexel[1] * k,
                        (addr.clampTexel[2] + 1) * k - 1,
                        (addr.clampTexel[3] + 1) * k - 1
                    };
                    const int32_t maxLevel = addr.atlas ? AtlasMaxMipLevel(scaledBox, texLevels)
                                                        : (texLevels - 1);
                    texInfo[2] = static_cast<float>(maxLevel < 0 ? 0 : maxLevel);
                    flags[1] = addr.atlas ? 2.0f : 1.0f;
                    sampler = addr.wrap ? m_samplerFilteredWrap.Get() : m_samplerFilteredClamp.Get();
                } else {
                    int32_t log2k = 0;
                    if (k == 2) log2k = 1;
                    else if (k == 4) log2k = 2;
                    else if (k == 8) log2k = 3;
                    const int32_t targetLevel = (k > 1) ? (std::max(0, q.chosenMip) + log2k) : 0;
                    texInfo[2] = static_cast<float>(std::min(targetLevel, texLevels - 1));
                    sampler = addr.wrap ? m_samplerWrap.Get() : m_samplerClamp.Get();
                }
                if (addr.wrap) {
                    uvClamp[0] = -1.0e6f;
                    uvClamp[1] = -1.0e6f;
                    uvClamp[2] = 1.0e6f;
                    uvClamp[3] = 1.0e6f;
                } else {
                    uvClamp[0] = (static_cast<float>(addr.clampTexel[0]) + 0.5f * invK) / texW;
                    uvClamp[1] = (static_cast<float>(addr.clampTexel[1]) + 0.5f * invK) / texH;
                    uvClamp[2] = (static_cast<float>(addr.clampTexel[2]) + 1.0f - 0.5f * invK) / texW;
                    uvClamp[3] = (static_cast<float>(addr.clampTexel[3]) + 1.0f - 0.5f * invK) / texH;
                }

                for (int i = 0; i < 4; ++i) {
                    vx[i] = posX[i] * invSrcW * 2.0f - 1.0f;
                    vy[i] = 1.0f - posY[i] * invSrcH * 2.0f;
                }
            }

            WorldVertex qv[4];
            for (int i = 0; i < 4; ++i) {
                qv[i].x = vx[i];
                qv[i].y = vy[i];
                std::memcpy(qv[i].p01, p01, sizeof(p01));
                std::memcpy(qv[i].p23, p23, sizeof(p23));
                std::memcpy(qv[i].uv01, uv01, sizeof(uv01));
                std::memcpy(qv[i].uv23, uv23, sizeof(uv23));
                std::memcpy(qv[i].uvClamp, uvClamp, sizeof(uvClamp));
                std::memcpy(qv[i].color, color, sizeof(color));
                std::memcpy(qv[i].flags, flags, sizeof(flags));
                std::memcpy(qv[i].texInfo, texInfo, sizeof(texInfo));
                std::memcpy(qv[i].box, box, sizeof(box));
            }

            // Choose diagonal (0-2 vs 1-3) so slightly concave quads do not fold over themselves
            const float cross012 =
                (qv[1].x - qv[0].x) * (qv[2].y - qv[0].y) - (qv[1].y - qv[0].y) * (qv[2].x - qv[0].x);
            const float cross023 =
                (qv[2].x - qv[0].x) * (qv[3].y - qv[0].y) - (qv[2].y - qv[0].y) * (qv[3].x - qv[0].x);

            const UINT startVert = static_cast<UINT>(m_worldVerts.size());
            if (cross012 * cross023 < 0.0f) {
                m_worldVerts.push_back(qv[0]);
                m_worldVerts.push_back(qv[1]);
                m_worldVerts.push_back(qv[3]);
                m_worldVerts.push_back(qv[1]);
                m_worldVerts.push_back(qv[2]);
                m_worldVerts.push_back(qv[3]);
            } else {
                m_worldVerts.push_back(qv[0]);
                m_worldVerts.push_back(qv[1]);
                m_worldVerts.push_back(qv[2]);
                m_worldVerts.push_back(qv[0]);
                m_worldVerts.push_back(qv[2]);
                m_worldVerts.push_back(qv[3]);
            }

            if (!m_worldBatches.empty()) {
                WorldDrawBatch& prev = m_worldBatches.back();
                if (prev.srv == srv && prev.sampler == sampler &&
                    prev.scissor.left == sc.left && prev.scissor.top == sc.top &&
                    prev.scissor.right == sc.right && prev.scissor.bottom == sc.bottom) {
                    prev.vertexCount += 6;
                    continue;
                }
            }
            WorldDrawBatch batch = {};
            batch.srv = srv;
            batch.sampler = sampler;
            batch.scissor = sc;
            batch.startVertex = startVert;
            batch.vertexCount = 6;
            m_worldBatches.push_back(batch);
        }

        if (subPixel) {
            ++m_subPixelStats.frames;
            m_subPixelStats.floatQuads += frameFloatQuads;
            m_subPixelStats.fallbackQuads += frameFallbackQuads;
            m_subPixelStats.maxFallbackPerFrame = std::max(m_subPixelStats.maxFallbackPerFrame, frameFallbackQuads);
        }

        if (!m_worldVerts.empty()) {
            D3D11_MAPPED_SUBRESOURCE mappedVB = {};
            HRESULT hr = m_context->Map(m_worldVB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedVB);
            if (FAILED(hr)) {
                LOG_ERROR("D3D11 Map(worldVB) failed: 0x%08lX", static_cast<unsigned long>(hr));
                return RecordFailure(nowMs);
            }
            std::memcpy(mappedVB.pData, m_worldVerts.data(), m_worldVerts.size() * sizeof(WorldVertex));
            m_context->Unmap(m_worldVB.Get(), 0);

            const UINT stride = sizeof(WorldVertex);
            const UINT offset = 0;
            m_context->IASetInputLayout(m_worldInputLayout.Get());
            m_context->IASetVertexBuffers(0, 1, m_worldVB.GetAddressOf(), &stride, &offset);
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_context->VSSetShader(m_worldVS.Get(), nullptr, 0);
            m_context->PSSetShader(m_worldPS.Get(), nullptr, 0);
            m_context->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());
            m_context->PSSetSamplers(1, 1, m_samplerAtlas.GetAddressOf());
            m_context->RSSetState(m_rasterScissor.Get());
            const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            m_context->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFFu);

            for (const WorldDrawBatch& b : m_worldBatches) {
                m_context->RSSetScissorRects(1, &b.scissor);
                m_context->PSSetShaderResources(0, 1, &b.srv);
                m_context->PSSetSamplers(0, 1, &b.sampler);
                m_context->Draw(b.vertexCount, b.startVertex);
            }
        }

        // Pass 3: Layer 3 (2D HUD / Dashboard Overlay with sentinel transparency)
        if (!UploadLayerToRgbaTexture(surface, pitch, palette, srcW, srcH, true)) {
            return RecordFailure(nowMs);
        }
        if (!DrawFullscreenLayerOnWorld(m_srvLayerRgba.Get(), worldW, worldH, true)) {
            return RecordFailure(nowMs);
        }

        // Restore rasterizer and blend state before final letterbox present
        ID3D11ShaderResourceView* nullSrv = nullptr;
        m_context->PSSetShaderResources(0, 1, &nullSrv);
        m_context->RSSetState(nullptr);
        const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        m_context->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFFu);

        return DrawAndPresent(worldW, worldH, clientW, clientH, true, m_srvWorld.Get(), nowMs);
    }

    bool DrawAndPresent(int srcW, int srcH, int clientW, int clientH, bool useBgra,
                        ID3D11ShaderResourceView* bgraSrv, uint64_t nowMs) {
        const LetterboxViewport lvp = ComputeLetterboxViewport(srcW, srcH, clientW, clientH);

        D3D11_MAPPED_SUBRESOURCE mappedCB = {};
        HRESULT hr = m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedCB);
        if (FAILED(hr)) {
            LOG_ERROR("D3D11 Map(constantBuffer) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return RecordFailure(nowMs);
        }
        auto* cb = static_cast<PresentConstants*>(mappedCB.pData);
        cb->srcSize[0] = static_cast<float>(srcW);
        cb->srcSize[1] = static_cast<float>(srcH);
        cb->dstSize[0] = static_cast<float>(lvp.width);
        cb->dstSize[1] = static_cast<float>(lvp.height);
        cb->filterMode = static_cast<uint32_t>(GetConfig().filter);
        cb->useBgra = useBgra ? 1u : 0u;
        cb->pad[0] = 0.0f;
        cb->pad[1] = 0.0f;
        cb->replacementFade[0] = 1.0f;
        cb->replacementFade[1] = 1.0f;
        cb->replacementFade[2] = 1.0f;
        cb->replacementFade[3] = 1.0f;
        m_context->Unmap(m_constantBuffer.Get(), 0);

        const float clearBlack[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);
        m_context->ClearRenderTargetView(m_rtv.Get(), clearBlack);

        D3D11_VIEWPORT vp = {};
        vp.TopLeftX = static_cast<FLOAT>(lvp.x);
        vp.TopLeftY = static_cast<FLOAT>(lvp.y);
        vp.Width = static_cast<FLOAT>(lvp.width);
        vp.Height = static_cast<FLOAT>(lvp.height);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &vp);

        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
        m_context->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());

        ID3D11ShaderResourceView* srvs[3] = {
            m_srv8bpp.Get(),
            m_srvPalette.Get(),
            bgraSrv
        };
        m_context->PSSetShaderResources(0, 3, srvs);

        m_context->Draw(3, 0);

        ID3D11ShaderResourceView* nullSrvs[3] = {nullptr, nullptr, nullptr};
        m_context->PSSetShaderResources(0, 3, nullSrvs);

        const UINT syncInterval = GetConfig().vsync ? 1u : 0u;
        hr = m_swapChain->Present(syncInterval, 0);
        if (hr == DXGI_STATUS_OCCLUDED) {
            PresenterRecordSuccess(m_failureState);
            return true;
        }
        if (FAILED(hr)) {
            LOG_ERROR("SwapChain::Present failed: 0x%08lX", static_cast<unsigned long>(hr));
            return RecordFailure(nowMs);
        }
        PresenterRecordSuccess(m_failureState);
        return true;
    }

    HWND m_hwnd = nullptr;
    int m_backbufferWidth = 0;
    int m_backbufferHeight = 0;
    int m_srcWidth = 0;
    int m_srcHeight = 0;
    int m_worldWidth = 0;
    int m_worldHeight = 0;
    bool m_windowChangePending = false;
    PresenterFailureState m_failureState = {};
    bool m_inBackoffCooldown = false;
    PresenterSubPixelStats m_subPixelStats = {};

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGISwapChain> m_swapChain;
    ComPtr<ID3D11RenderTargetView> m_rtv;
    ComPtr<ID3D11VertexShader> m_vertexShader;
    ComPtr<ID3D11PixelShader> m_pixelShader;
    ComPtr<ID3D11Buffer> m_constantBuffer;

    ComPtr<ID3D11Texture2D> m_tex8bpp;
    ComPtr<ID3D11ShaderResourceView> m_srv8bpp;
    ComPtr<ID3D11Texture2D> m_texPalette;
    ComPtr<ID3D11ShaderResourceView> m_srvPalette;
    ComPtr<ID3D11Texture2D> m_texBgra;
    ComPtr<ID3D11ShaderResourceView> m_srvBgra;

    // Task T5 world rendering resources
    ComPtr<ID3D11VertexShader> m_worldVS;
    ComPtr<ID3D11PixelShader> m_worldPS;
    ComPtr<ID3D11VertexShader> m_layerVS;
    ComPtr<ID3D11PixelShader> m_layerPS;
    ComPtr<ID3D11InputLayout> m_worldInputLayout;
    ComPtr<ID3D11Buffer> m_worldVB;
    ComPtr<ID3D11RasterizerState> m_rasterScissor;
    ComPtr<ID3D11BlendState> m_blendAlpha;
    ComPtr<ID3D11SamplerState> m_samplerWrap;
    ComPtr<ID3D11SamplerState> m_samplerClamp;
    ComPtr<ID3D11SamplerState> m_samplerFilteredWrap;
    ComPtr<ID3D11SamplerState> m_samplerFilteredClamp;
    ComPtr<ID3D11SamplerState> m_samplerAtlas;
    ComPtr<ID3D11Texture2D> m_texWhite;
    ComPtr<ID3D11ShaderResourceView> m_srvWhite;
    ComPtr<ID3D11Texture2D> m_texLayerRgba;
    ComPtr<ID3D11ShaderResourceView> m_srvLayerRgba;
    ComPtr<ID3D11Texture2D> m_texWorld;
    ComPtr<ID3D11RenderTargetView> m_rtvWorld;
    ComPtr<ID3D11ShaderResourceView> m_srvWorld;

    struct CachedWorldTexture {
        ComPtr<ID3D11ShaderResourceView> srv;
        int32_t levels = 1;
        int32_t k = 1;
        std::string pendingReplacementId;  // replacement ID while it is pending, else empty
        bool replacement = false;          // srv is a texture pack replacement
    };
    std::unordered_map<uint64_t, CachedWorldTexture> m_worldTexSrvCache;
    uint8_t m_cachedPalette[1024] = {};
    uint64_t m_cachedWorldGen = 0;
    uint64_t m_cachedTexPackGen = 0;
    std::vector<uint32_t> m_tempRgbaUpload;
    std::vector<WorldVertex> m_worldVerts;
    std::vector<WorldDrawBatch> m_worldBatches;
    bool m_textureDumpInitialized = false;
    bool m_texturePackInitialized = false;

    // Replacement fade (see UpdateReplacementFade). The base palette is read once per process.
    uint8_t m_basePalette[kBasePaletteBytes] = {};
    bool m_basePaletteTried = false;
    bool m_basePaletteValid = false;
    uint8_t m_fadePalette[kBasePaletteBytes] = {};
    bool m_fadePaletteValid = false;
    bool m_nonUniformFadeWarned = false;
    float m_replacementFade[3] = {1.0f, 1.0f, 1.0f};
};

// Intentionally leaked singleton: it is never destroyed. Its destructor would release D3D11/DXGI
// objects and stop the texture pack workers during DLL_PROCESS_DETACH at process exit, under the
// loader lock and after the OS killed every other thread, which can hang the exit forever.
// Explicit Shutdown() calls during a session (device loss, window change) are unaffected.
D3D11Presenter& g_presenter = *new D3D11Presenter();

}  // namespace

PresenterSubPixelStats PresenterTakeSubPixelStats() {
    return g_presenter.TakeSubPixelStats();
}

std::string PresenterWorldPixelShaderSource() {
    return BuildWorldPixelShaderSource();
}

bool PresenterPresent8bpp(HWND hwnd, const uint8_t* surface, int pitch, const uint8_t* palette, int width, int height) {
    return g_presenter.Present8bpp(hwnd, surface, pitch, palette, width, height);
}

bool PresenterPresentBgra(HWND hwnd, const uint32_t* dib, int width, int height) {
    return g_presenter.PresentBgra(hwnd, dib, width, height);
}

bool PresenterIsLatchedOff() {
    return g_presenter.IsLatchedOff();
}

bool PresenterGpuWorldAllowed() {
    return g_presenter.GpuWorldAllowed();
}

void PresenterNotifyWindowChange() {
    g_presenter.NotifyWindowChange();
}

void PresenterShutdown() {
    g_presenter.Shutdown();
}
