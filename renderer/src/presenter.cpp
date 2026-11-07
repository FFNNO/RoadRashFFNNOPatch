#include "presenter.h"
#include "config.h"
#include "log.h"

#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstring>

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

        return DrawAndPresent(width, height, clientW, clientH, false, nowMs);
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

        return DrawAndPresent(width, height, clientW, clientH, true, nowMs);
    }

    void NotifyWindowChange() {
        m_windowChangePending = true;
    }

    void Shutdown() {
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

        return true;
    }

    bool CreateSourceTextures(int width, int height) {
        m_srv8bpp.Reset();
        m_tex8bpp.Reset();
        m_srvBgra.Reset();
        m_texBgra.Reset();

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

        m_srcWidth = width;
        m_srcHeight = height;
        return true;
    }

    bool DrawAndPresent(int srcW, int srcH, int clientW, int clientH, bool useBgra, uint64_t nowMs) {
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
            m_srvBgra.Get()
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
