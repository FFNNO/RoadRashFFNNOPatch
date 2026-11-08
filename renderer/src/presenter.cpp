#include "presenter.h"
#include "config.h"
#include "capture.h"
#include "log.h"

#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
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
};

const char* kWorldVertexShaderSrc = R"(
struct VSIn {
    float2 pos   : POSITION;
    float3 uvq   : TEXCOORD0;
    float4 color : COLOR0;
};

struct VSOut {
    float4 pos   : SV_Position;
    float3 uvq   : TEXCOORD0;
    float4 color : COLOR0;
};

VSOut main(VSIn v) {
    VSOut o;
    o.pos   = float4(v.pos, 0.0, 1.0);
    o.uvq   = v.uvq;
    o.color = v.color;
    return o;
}
)";

const char* kWorldPixelShaderSrc = R"(
Texture2D<float4> texQuad : register(t0);
SamplerState      samp    : register(s0);

struct PSIn {
    float4 pos   : SV_Position;
    float3 uvq   : TEXCOORD0;
    float4 color : COLOR0;
};

float4 main(PSIn p) : SV_Target {
    float q = max(p.uvq.z, 1e-6);
    float2 uv = p.uvq.xy / q;
    float4 texel = texQuad.Sample(samp, uv);
    float4 outColor = texel * p.color;
    if (outColor.a < 0.5) {
        discard;
    }
    return outColor;
}
)";

struct WorldVertex {
    float x, y;
    float u, v, q;
    float r, g, b, a;
};

struct WorldDrawBatch {
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    D3D11_RECT scissor = {};
    UINT startVertex = 0;
    UINT vertexCount = 0;
};

constexpr size_t kMaxWorldVertices = 65536;

void ComputeProjectiveQ(const CapturedVertex verts[4], float outQ[4]) {
    outQ[0] = 1.0f;
    outQ[1] = 1.0f;
    outQ[2] = 1.0f;
    outQ[3] = 1.0f;

    const float x0 = static_cast<float>(verts[0].x);
    const float y0 = static_cast<float>(verts[0].y);
    const float x1 = static_cast<float>(verts[1].x);
    const float y1 = static_cast<float>(verts[1].y);
    const float x2 = static_cast<float>(verts[2].x);
    const float y2 = static_cast<float>(verts[2].y);
    const float x3 = static_cast<float>(verts[3].x);
    const float y3 = static_cast<float>(verts[3].y);

    const float dx02 = x2 - x0;
    const float dy02 = y2 - y0;
    const float dx13 = x3 - x1;
    const float dy13 = y3 - y1;

    const float c0 = dx13 * (y0 - y1) - dy13 * (x0 - x1);
    const float c2 = (-dx13) * (y2 - y3) - (-dy13) * (x2 - x3);
    const float c1 = dx02 * (y1 - y2) - dy02 * (x1 - x2);
    const float c3 = (-dx02) * (y3 - y0) - (-dy02) * (x3 - x0);

    const bool allPos = (c0 > 1.0f && c1 > 1.0f && c2 > 1.0f && c3 > 1.0f);
    const bool allNeg = (c0 < -1.0f && c1 < -1.0f && c2 < -1.0f && c3 < -1.0f);
    if (!allPos && !allNeg) {
        return;
    }

    const float q0 = (c0 + c2) / c2;
    const float q1 = (c1 + c3) / c3;
    const float q2 = (c0 + c2) / c0;
    const float q3 = (c1 + c3) / c1;
    if (q0 < 0.05f || q0 > 20.0f || q1 < 0.05f || q1 > 20.0f ||
        q2 < 0.05f || q2 > 20.0f || q3 < 0.05f || q3 > 20.0f) {
        return;
    }
    outQ[0] = q0;
    outQ[1] = q1;
    outQ[2] = q2;
    outQ[3] = q3;
}

class D3D11Presenter {
public:
    bool IsLatchedOff() const {
        return m_failureState.latchedOff || m_inBackoffCooldown;
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

    void Shutdown() {
        m_worldTexSrvCache.clear();
        m_srvWorld.Reset();
        m_rtvWorld.Reset();
        m_texWorld.Reset();
        m_srvLayerRgba.Reset();
        m_texLayerRgba.Reset();
        m_srvWhite.Reset();
        m_texWhite.Reset();
        m_samplerWrap.Reset();
        m_samplerClamp.Reset();
        m_rasterScissor.Reset();
        m_blendAlpha.Reset();
        m_worldVB.Reset();
        m_worldInputLayout.Reset();
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
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        hr = m_device->CreateInputLayout(ied, static_cast<UINT>(sizeof(ied) / sizeof(ied[0])),
                                         worldVsBlob->GetBufferPointer(), worldVsBlob->GetBufferSize(),
                                         &m_worldInputLayout);
        if (FAILED(hr)) {
            LOG_ERROR("CreateInputLayout(world) failed: 0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }

        ComPtr<ID3DBlob> worldPsBlob;
        errBlob.Reset();
        hr = D3DCompile(kWorldPixelShaderSrc, std::strlen(kWorldPixelShaderSrc), "world_ps",
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

    ID3D11ShaderResourceView* GetOrCreateWorldTextureSRV(const CapturedQuad& q, const uint8_t* palette) {
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
            return it->second.Get();
        }

        int32_t tw = 0;
        int32_t th = 0;
        const uint8_t* texPixels = CaptureLookupTexturePixels(q.pdatPtr, q.level0Crc32, tw, th);
        if (!texPixels || tw <= 0 || th <= 0) {
            return m_srvWhite.Get();
        }

        m_tempRgbaUpload.resize(static_cast<size_t>(tw) * static_cast<size_t>(th));
        for (int32_t i = 0; i < tw * th; ++i) {
            const uint8_t idx = texPixels[static_cast<size_t>(i)];
            if (colorKey && idx == 0) {
                m_tempRgbaUpload[static_cast<size_t>(i)] = 0x00000000u;
            } else {
                const uint32_t r = palette[idx * 4 + 0];
                const uint32_t g = palette[idx * 4 + 1];
                const uint32_t b = palette[idx * 4 + 2];
                m_tempRgbaUpload[static_cast<size_t>(i)] = r | (g << 8) | (b << 16) | 0xFF000000u;
            }
        }

        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = m_tempRgbaUpload.data();
        initData.SysMemPitch = static_cast<UINT>(tw * sizeof(uint32_t));

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(tw);
        td.Height = static_cast<UINT>(th);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        ComPtr<ID3D11Texture2D> tex;
        HRESULT hr = m_device->CreateTexture2D(&td, &initData, &tex);
        if (FAILED(hr)) {
            return m_srvWhite.Get();
        }
        ComPtr<ID3D11ShaderResourceView> srv;
        hr = m_device->CreateShaderResourceView(tex.Get(), nullptr, &srv);
        if (FAILED(hr)) {
            return m_srvWhite.Get();
        }
        ID3D11ShaderResourceView* rawSrv = srv.Get();
        m_worldTexSrvCache.emplace(srvKey, std::move(srv));
        return rawSrv;
    }

    bool UploadLayerToRgbaTexture(const uint8_t* srcSurf, int srcPitch, const uint8_t* palette,
                                  int width, int height, bool useHudSentinelKey) {
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
                if (useHudSentinelKey && idx == kHudTransparentSentinel) {
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
        return true;
    }

    bool DrawFullscreenLayerOnWorld(ID3D11ShaderResourceView* srv, int worldW, int worldH, bool alphaBlend) {
        D3D11_MAPPED_SUBRESOURCE mappedVB = {};
        HRESULT hr = m_context->Map(m_worldVB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedVB);
        if (FAILED(hr)) {
            return false;
        }
        auto* v = static_cast<WorldVertex*>(mappedVB.pData);
        v[0] = {-1.0f,  1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        v[1] = { 1.0f,  1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        v[2] = { 1.0f, -1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        v[3] = {-1.0f,  1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        v[4] = { 1.0f, -1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        v[5] = {-1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        m_context->Unmap(m_worldVB.Get(), 0);

        const UINT stride = sizeof(WorldVertex);
        const UINT offset = 0;
        m_context->IASetInputLayout(m_worldInputLayout.Get());
        m_context->IASetVertexBuffers(0, 1, m_worldVB.GetAddressOf(), &stride, &offset);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(m_worldVS.Get(), nullptr, 0);
        m_context->PSSetShader(m_worldPS.Get(), nullptr, 0);
        m_context->PSSetShaderResources(0, 1, &srv);
        m_context->PSSetSamplers(0, 1, m_samplerClamp.GetAddressOf());
        m_context->RSSetState(m_rasterScissor.Get());

        const D3D11_RECT fullScissor = {0, 0, worldW, worldH};
        m_context->RSSetScissorRects(1, &fullScissor);

        const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        m_context->OMSetBlendState(alphaBlend ? m_blendAlpha.Get() : nullptr, blendFactor, 0xFFFFFFFFu);

        m_context->Draw(6, 0);
        return true;
    }

    bool PresentWorldFrame(const uint8_t* surface, int pitch, const uint8_t* palette,
                           int srcW, int srcH, int clientW, int clientH, uint64_t nowMs) {
        const int worldW = GetConfig().internalWidth;
        const int worldH = GetConfig().internalHeight;
        if (!EnsureWorldRenderTarget(worldW, worldH)) {
            return RecordFailure(nowMs);
        }

        const uint32_t curGen = CaptureGetCacheGeneration();
        if (curGen != m_cachedWorldGen || std::memcmp(m_cachedPalette, palette, 1024) != 0) {
            m_worldTexSrvCache.clear();
            std::memcpy(m_cachedPalette, palette, 1024);
            m_cachedWorldGen = curGen;
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
            float r = 1.0f, g = 1.0f, b = 1.0f;

            WorldVertex qv[4] = {};
            if (q.kind == QuadKind::Flat) {
                const uint8_t idx = q.flatColor;
                r = palette[idx * 4 + 0] / 255.0f;
                g = palette[idx * 4 + 1] / 255.0f;
                b = palette[idx * 4 + 2] / 255.0f;

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
                    qv[0] = {xL, yT, 0.0f, 0.0f, 1.0f, r, g, b, 1.0f};
                    qv[1] = {xR, yT, 0.0f, 0.0f, 1.0f, r, g, b, 1.0f};
                    qv[2] = {xR, yB, 0.0f, 0.0f, 1.0f, r, g, b, 1.0f};
                    qv[3] = {xL, yB, 0.0f, 0.0f, 1.0f, r, g, b, 1.0f};
                } else {
                    for (int i = 0; i < 4; ++i) {
                        const float vx = static_cast<float>(q.verts[i].x) * invSrcW * 2.0f - 1.0f;
                        const float vy = 1.0f - static_cast<float>(q.verts[i].y) * invSrcH * 2.0f;
                        qv[i] = {vx, vy, 0.0f, 0.0f, 1.0f, r, g, b, 1.0f};
                    }
                }
            } else {
                srv = GetOrCreateWorldTextureSRV(q, palette);
                const bool isDynamicSprite = (q.pdatFlags & 0x2000u) != 0;
                sampler = isDynamicSprite ? m_samplerClamp.Get() : m_samplerWrap.Get();

                float projQ[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                if (!isDynamicSprite) {
                    ComputeProjectiveQ(q.verts, projQ);
                }

                const float texW = static_cast<float>(std::max(1, q.texWidth));
                const float texH = static_cast<float>(std::max(1, q.texHeight));
                for (int i = 0; i < 4; ++i) {
                    const float vx = static_cast<float>(q.verts[i].x) * invSrcW * 2.0f - 1.0f;
                    const float vy = 1.0f - static_cast<float>(q.verts[i].y) * invSrcH * 2.0f;
                    const float u = static_cast<float>(q.verts[i].u) / texW;
                    const float v = static_cast<float>(q.verts[i].v) / texH;
                    const float wq = projQ[i];
                    qv[i] = {vx, vy, u * wq, v * wq, wq, 1.0f, 1.0f, 1.0f, 1.0f};
                }
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
    ComPtr<ID3D11InputLayout> m_worldInputLayout;
    ComPtr<ID3D11Buffer> m_worldVB;
    ComPtr<ID3D11RasterizerState> m_rasterScissor;
    ComPtr<ID3D11BlendState> m_blendAlpha;
    ComPtr<ID3D11SamplerState> m_samplerWrap;
    ComPtr<ID3D11SamplerState> m_samplerClamp;
    ComPtr<ID3D11Texture2D> m_texWhite;
    ComPtr<ID3D11ShaderResourceView> m_srvWhite;
    ComPtr<ID3D11Texture2D> m_texLayerRgba;
    ComPtr<ID3D11ShaderResourceView> m_srvLayerRgba;
    ComPtr<ID3D11Texture2D> m_texWorld;
    ComPtr<ID3D11RenderTargetView> m_rtvWorld;
    ComPtr<ID3D11ShaderResourceView> m_srvWorld;

    std::unordered_map<uint64_t, ComPtr<ID3D11ShaderResourceView>> m_worldTexSrvCache;
    uint8_t m_cachedPalette[1024] = {};
    uint64_t m_cachedWorldGen = 0;
    std::vector<uint32_t> m_tempRgbaUpload;
    std::vector<WorldVertex> m_worldVerts;
    std::vector<WorldDrawBatch> m_worldBatches;
};

D3D11Presenter g_presenter;

}  // namespace

bool PresenterPresent8bpp(HWND hwnd, const uint8_t* surface, int pitch, const uint8_t* palette, int width, int height) {
    return g_presenter.Present8bpp(hwnd, surface, pitch, palette, width, height);
}

bool PresenterPresentBgra(HWND hwnd, const uint32_t* dib, int width, int height) {
    return g_presenter.PresentBgra(hwnd, dib, width, height);
}

bool PresenterIsLatchedOff() {
    return g_presenter.IsLatchedOff();
}

void PresenterNotifyWindowChange() {
    g_presenter.NotifyWindowChange();
}

void PresenterShutdown() {
    g_presenter.Shutdown();
}
