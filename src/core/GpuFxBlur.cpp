#include "GpuFxBlur.h"
#include "Logger.h"
#include "PathUtil.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

namespace gpu_fx {
namespace {

struct BlurCBData {
    float params[4];
};

HRESULT Compile(const wchar_t* path, const char* entry, const char* target, ID3DBlob** out) {
    ID3DBlob* err = nullptr;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG;
#endif
    HRESULT hr = D3DCompileFromFile(path, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                    entry, target, flags, 0, out, &err);
    if (FAILED(hr) && err) {
        Logger::Get().ErrorTag("gpu",
            std::string("GpuFxBlur compile ") + entry + ": " +
            (const char*)err->GetBufferPointer());
        err->Release();
    }
    return hr;
}

} // namespace

void GpuBlurContext::Shutdown() {
    if (vs) { vs->Release(); vs = nullptr; }
    if (psH) { psH->Release(); psH = nullptr; }
    if (psV) { psV->Release(); psV = nullptr; }
    if (cb) { cb->Release(); cb = nullptr; }
    if (vb) { vb->Release(); vb = nullptr; }
    if (layout) { layout->Release(); layout = nullptr; }
    if (samp) { samp->Release(); samp = nullptr; }
    device = nullptr;
    context = nullptr;
    ready = false;
}

bool InitGpuBlur(GpuBlurContext& ctx, ID3D11Device* device, ID3D11DeviceContext* context,
                 const wchar_t* hlslPath) {
    ctx.Shutdown();
    if (!device || !context || !hlslPath) return false;
    ctx.device = device;
    ctx.context = context;

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psHBlob = nullptr;
    ID3DBlob* psVBlob = nullptr;
    if (FAILED(Compile(hlslPath, "VSMain", "vs_4_0", &vsBlob)) ||
        FAILED(Compile(hlslPath, "PSBlurH", "ps_4_0", &psHBlob)) ||
        FAILED(Compile(hlslPath, "PSBlurV", "ps_4_0", &psVBlob))) {
        if (vsBlob) vsBlob->Release();
        if (psHBlob) psHBlob->Release();
        if (psVBlob) psVBlob->Release();
        return false;
    }

    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &ctx.vs);
    device->CreatePixelShader(psHBlob->GetBufferPointer(), psHBlob->GetBufferSize(), nullptr, &ctx.psH);
    device->CreatePixelShader(psVBlob->GetBufferPointer(), psVBlob->GetBufferSize(), nullptr, &ctx.psV);

    // Empty input layout (SV_VertexID only)
    D3D11_INPUT_ELEMENT_DESC ied = { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                     D3D11_INPUT_PER_VERTEX_DATA, 0 };
    // VS has no input — create dummy layout from empty
    // Actually VSMain only uses SV_VertexID — CreateInputLayout with 0 elements may fail.
    // Draw with IASetInputLayout(nullptr) and topology triangle list of 3 verts.
    vsBlob->Release();
    psHBlob->Release();
    psVBlob->Release();

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(BlurCBData);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&cbd, nullptr, &ctx.cb);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device->CreateSamplerState(&sd, &ctx.samp);

    ctx.ready = (ctx.vs && ctx.psH && ctx.psV && ctx.cb && ctx.samp);
    if (!ctx.ready) {
        ctx.Shutdown();
        return false;
    }
    Logger::Get().InfoTag("gpu", "GpuFxBlur ready");
    return true;
}

static bool CreateRT(ID3D11Device* device, int w, int h, DXGI_FORMAT fmt,
                     ID3D11Texture2D** tex, ID3D11RenderTargetView** rtv,
                     ID3D11ShaderResourceView** srv) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = fmt; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&td, nullptr, tex)) || !*tex) return false;
    device->CreateRenderTargetView(*tex, nullptr, rtv);
    device->CreateShaderResourceView(*tex, nullptr, srv);
    return *rtv && *srv;
}

bool BlurRGBA32F(GpuBlurContext& ctx, std::vector<float>& rgba, int w, int h, int radius) {
    if (!ctx.ready || !ctx.device || !ctx.context || w < 1 || h < 1) return false;
    if ((int)rgba.size() < w * h * 4) return false;
    // Cap region size for interactive preview (avoid giant temp RTs)
    if (w > 2048 || h > 2048) return false;
    radius = std::clamp(radius, 1, 64);

    ID3D11Device* device = ctx.device;
    ID3D11DeviceContext* ic = ctx.context;

    // Upload source as RGBA32F texture
    D3D11_TEXTURE2D_DESC srcDesc = {};
    srcDesc.Width = (UINT)w; srcDesc.Height = (UINT)h; srcDesc.MipLevels = 1; srcDesc.ArraySize = 1;
    srcDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    srcDesc.SampleDesc.Count = 1;
    srcDesc.Usage = D3D11_USAGE_DEFAULT;
    srcDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = rgba.data();
    init.SysMemPitch = (UINT)(w * 4 * sizeof(float));
    ID3D11Texture2D* srcTex = nullptr;
    ID3D11ShaderResourceView* srcSRV = nullptr;
    if (FAILED(device->CreateTexture2D(&srcDesc, &init, &srcTex)) || !srcTex) return false;
    device->CreateShaderResourceView(srcTex, nullptr, &srcSRV);

    ID3D11Texture2D* rtA = nullptr, *rtB = nullptr;
    ID3D11RenderTargetView* rtvA = nullptr, *rtvB = nullptr;
    ID3D11ShaderResourceView* srvA = nullptr, *srvB = nullptr;
    if (!CreateRT(device, w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, &rtA, &rtvA, &srvA) ||
        !CreateRT(device, w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, &rtB, &rtvB, &srvB)) {
        if (srcSRV) srcSRV->Release();
        if (srcTex) srcTex->Release();
        if (rtA) rtA->Release(); if (rtvA) rtvA->Release(); if (srvA) srvA->Release();
        if (rtB) rtB->Release(); if (rtvB) rtvB->Release(); if (srvB) srvB->Release();
        return false;
    }

    // Save state
    ID3D11RenderTargetView* prevRTV = nullptr;
    ID3D11DepthStencilView* prevDSV = nullptr;
    ic->OMGetRenderTargets(1, &prevRTV, &prevDSV);
    D3D11_VIEWPORT prevVP = {};
    UINT nvp = 1;
    ic->RSGetViewports(&nvp, &prevVP);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)w; vp.Height = (float)h; vp.MaxDepth = 1.f;
    ic->RSSetViewports(1, &vp);
    ic->IASetInputLayout(nullptr);
    ic->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ic->VSSetShader(ctx.vs, nullptr, 0);
    ic->PSSetSamplers(0, 1, &ctx.samp);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(ic->Map(ctx.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        auto* cb = (BlurCBData*)mapped.pData;
        cb->params[0] = 1.f / (float)w;
        cb->params[1] = 1.f / (float)h;
        cb->params[2] = (float)radius;
        cb->params[3] = 0.f;
        ic->Unmap(ctx.cb, 0);
    }
    ic->PSSetConstantBuffers(0, 1, &ctx.cb);

    auto pass = [&](ID3D11ShaderResourceView* inSRV, ID3D11RenderTargetView* outRTV,
                    ID3D11PixelShader* ps) {
        ic->OMSetRenderTargets(1, &outRTV, nullptr);
        ic->PSSetShader(ps, nullptr, 0);
        ic->PSSetShaderResources(0, 1, &inSRV);
        ic->Draw(3, 0);
        ID3D11ShaderResourceView* nulls[1] = { nullptr };
        ic->PSSetShaderResources(0, 1, nulls);
    };

    // 3 box passes (H+V) ≈ gaussian-ish
    // pass 0: src → A (H), A → B (V), B → A (H), A → B (V), B → A (H), A → B (V)
    ID3D11ShaderResourceView* cur = srcSRV;
    ID3D11RenderTargetView* outRTV = rtvA;
    ID3D11ShaderResourceView* outSRV = srvA;
    ID3D11RenderTargetView* altRTV = rtvB;
    ID3D11ShaderResourceView* altSRV = srvB;

    for (int p = 0; p < 3; ++p) {
        pass(cur, outRTV, ctx.psH);
        // unbind out before sampling
        ID3D11RenderTargetView* nullR = nullptr;
        ic->OMSetRenderTargets(1, &nullR, nullptr);
        pass(outSRV, altRTV, ctx.psV);
        ic->OMSetRenderTargets(1, &nullR, nullptr);
        cur = altSRV;
        std::swap(outRTV, altRTV);
        std::swap(outSRV, altSRV);
    }

    // Readback final (cur = last V output = altSRV after loop... track carefully)
    // After loop: last written is alt (V of last pass). cur = altSRV at end of iteration.
    ID3D11Texture2D* finalTex = (cur == srvB) ? rtB : rtA;

    D3D11_TEXTURE2D_DESC stDesc = {};
    stDesc.Width = (UINT)w; stDesc.Height = (UINT)h; stDesc.MipLevels = 1; stDesc.ArraySize = 1;
    stDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    stDesc.SampleDesc.Count = 1;
    stDesc.Usage = D3D11_USAGE_STAGING;
    stDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&stDesc, nullptr, &staging)) || !staging) {
        // restore + cleanup
        ic->OMSetRenderTargets(1, &prevRTV, prevDSV);
        if (prevRTV) prevRTV->Release();
        if (prevDSV) prevDSV->Release();
        ic->RSSetViewports(1, &prevVP);
        srcSRV->Release(); srcTex->Release();
        rtvA->Release(); srvA->Release(); rtA->Release();
        rtvB->Release(); srvB->Release(); rtB->Release();
        return false;
    }
    ic->CopyResource(staging, finalTex);

    D3D11_MAPPED_SUBRESOURCE mapR = {};
    if (SUCCEEDED(ic->Map(staging, 0, D3D11_MAP_READ, 0, &mapR))) {
        for (int y = 0; y < h; ++y) {
            const float* srcRow = (const float*)((const uint8_t*)mapR.pData + (size_t)y * mapR.RowPitch);
            float* dstRow = rgba.data() + (size_t)y * w * 4;
            std::memcpy(dstRow, srcRow, (size_t)w * 4 * sizeof(float));
        }
        ic->Unmap(staging, 0);
    }
    staging->Release();

    ic->OMSetRenderTargets(1, &prevRTV, prevDSV);
    if (prevRTV) prevRTV->Release();
    if (prevDSV) prevDSV->Release();
    ic->RSSetViewports(1, &prevVP);
    ic->VSSetShader(nullptr, nullptr, 0);
    ic->PSSetShader(nullptr, nullptr, 0);

    srcSRV->Release(); srcTex->Release();
    rtvA->Release(); srvA->Release(); rtA->Release();
    rtvB->Release(); srvB->Release(); rtB->Release();
    return true;
}

} // namespace gpu_fx
