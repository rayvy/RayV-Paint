#include "GpuFxDispatch.h"
#include <d3dcompiler.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <random>

// ============================================================
//  Internal helpers
// ============================================================

static std::wstring GetExeDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(path, L'\\');
    if (lastSlash) *lastSlash = 0;
    return path;
}

static HRESULT CompilePS(ID3D11Device* device, const std::wstring& hlslPath,
                          const char* entry, const char* model,
                          ID3D11PixelShader** outPS) {
    // Try cached .cso first
    std::wstring csoPath = GetExeDir() + L"\\shaders\\" +
        std::wstring(entry, entry + strlen(entry)) + L".cso";

    // Try load cached
    {
        std::ifstream f(csoPath, std::ios::binary);
        if (f.is_open()) {
            f.seekg(0, std::ios::end);
            size_t sz = (size_t)f.tellg();
            f.seekg(0, std::ios::beg);
            std::vector<char> blob(sz);
            f.read(blob.data(), sz);
            if (sz > 0 && SUCCEEDED(device->CreatePixelShader(blob.data(), blob.size(), nullptr, outPS)))
                return S_OK;
        }
    }

    // Compile from source
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ID3DBlob* codeBlob = nullptr;
    ID3DBlob* errBlob = nullptr;
    HRESULT hr = D3DCompileFromFile(hlslPath.c_str(), nullptr, nullptr,
        entry, model, flags, 0, &codeBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) {
            std::cerr << "GpuFx: compile error " << entry << ": "
                      << (char*)errBlob->GetBufferPointer() << std::endl;
            errBlob->Release();
        }
        return hr;
    }

    hr = device->CreatePixelShader(codeBlob->GetBufferPointer(),
        codeBlob->GetBufferSize(), nullptr, outPS);

    // Cache .cso
    if (SUCCEEDED(hr)) {
        std::wstring dir = GetExeDir() + L"\\shaders\\";
        CreateDirectoryW(dir.c_str(), nullptr);
        std::ofstream out(csoPath, std::ios::binary);
        if (out.is_open())
            out.write((const char*)codeBlob->GetBufferPointer(), codeBlob->GetBufferSize());
    }

    if (codeBlob) codeBlob->Release();
    if (errBlob) errBlob->Release();
    return hr;
}

static void ReleaseCOM(IUnknown*& p) { if (p) { p->Release(); p = nullptr; } }

// ============================================================
//  GpuFxTarget
// ============================================================

void GpuFxTarget::Release() {
    ReleaseCOM(srv);
    ReleaseCOM(rtv);
    ReleaseCOM(texture);
    width = height = 0;
}

// ============================================================
//  Init
// ============================================================

namespace gpu_fx {

static const char* const kShaderEntries[] = {
    "PS_HSV", "PS_HSV_Selection", "PS_Curves", "PS_Noise",
    "PS_AlphaInvert", "PS_InvertColors", "PS_MaskAlpha",
    "PS_BoxBlurH", "PS_BoxBlurV",
    "PS_FillSolid", "PS_FillTexture", "PS_ExtractAlpha", "PS_Spread", "PS_Offset",
    "PS_DilateH", "PS_DilateV", "PS_ErodeH", "PS_ErodeV",
    "PS_ShadowColorize", "PS_OutlineSolid", "PS_OutlineGradient", "PS_OutlineTexture",
    "PS_Subtract", "PS_NearestUp"
};
static ID3D11PixelShader** kShaderSlots[] = {
    nullptr // filled in Init
};

bool Init(GpuFxContext& ctx, ID3D11Device* device, const std::wstring& shaderDir) {
    ctx.device = device;
    device->GetImmediateContext(&ctx.context);
    if (!ctx.context) return false;

    std::wstring hlslPath = shaderDir.empty()
        ? GetExeDir() + L"\\shaders\\LayerFx.hlsl"
        : shaderDir + L"LayerFx.hlsl";

    // Compile VS
    {
        ID3DBlob* vsBlob = nullptr;
        std::wstring csoVS = GetExeDir() + L"\\shaders\\VSMain_Fx.cso";
        std::ifstream f(csoVS, std::ios::binary);
        bool loaded = false;
        if (f.is_open()) {
            f.seekg(0, std::ios::end);
            size_t sz = f.tellg(); f.seekg(0);
            std::vector<char> d(sz); f.read(d.data(), sz);
            if (sz > 0) { device->CreateVertexShader(d.data(), sz, nullptr, &ctx.vsFullscreen); loaded = true; }
        }
        if (!loaded) {
            UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
            ID3DBlob* err = nullptr;
            HRESULT hr = D3DCompileFromFile(hlslPath.c_str(), nullptr, nullptr,
                "VSMain", "vs_4_0", flags, 0, &vsBlob, &err);
            if (FAILED(hr)) {
                if (err) { std::cerr << "GpuFx VS error: " << (char*)err->GetBufferPointer(); err->Release(); }
                return false;
            }
            device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                nullptr, &ctx.vsFullscreen);
            // Cache
            std::wstring dir = GetExeDir() + L"\\shaders\\";
            CreateDirectoryW(dir.c_str(), nullptr);
            std::ofstream out(csoVS, std::ios::binary);
            if (out.is_open()) out.write((const char*)vsBlob->GetBufferPointer(), vsBlob->GetBufferSize());
        }
        if (vsBlob) vsBlob->Release();
    }

    // Compile all PS
    ID3D11PixelShader** slots[] = {
        &ctx.psHSV, &ctx.psHSVSelection, &ctx.psCurves, &ctx.psNoise,
        &ctx.psAlphaInvert, &ctx.psInvertColors, &ctx.psMaskAlpha,
        &ctx.psBoxBlurH, &ctx.psBoxBlurV,
        &ctx.psFillSolid, &ctx.psFillTexture, &ctx.psExtractAlpha, &ctx.psSpread, &ctx.psOffset,
        &ctx.psDilateH, &ctx.psDilateV, &ctx.psErodeH, &ctx.psErodeV,
        &ctx.psShadowColorize, &ctx.psOutlineSolid, &ctx.psOutlineGradient, &ctx.psOutlineTexture,
        &ctx.psSubtract, &ctx.psNearestUp
    };
    for (int i = 0; i < 24; ++i) {
        if (FAILED(CompilePS(device, hlslPath, kShaderEntries[i], "ps_4_0", slots[i]))) {
            std::cerr << "GpuFx: failed " << kShaderEntries[i] << std::endl;
            // Non-fatal: individual shaders can fail, caller checks
        }
    }

    // Create constant buffers
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = (UINT)GpuFilterParams::ByteSize();
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        device->CreateBuffer(&bd, nullptr, &ctx.cbFilter);

        bd.ByteWidth = (UINT)GpuBlurParams::ByteSize();
        device->CreateBuffer(&bd, nullptr, &ctx.cbBlur);
    }

    // Create samplers
    {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        device->CreateSamplerState(&sd, &ctx.samplerLinearClamp);

        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        device->CreateSamplerState(&sd, &ctx.samplerLinearWrap);

        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        device->CreateSamplerState(&sd, &ctx.samplerPointClamp);
    }

    // Create LUT 1D texture (256 R32_FLOAT, updated per-curves-call)
    {
        D3D11_TEXTURE1D_DESC td = {};
        td.Width = 256;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32_FLOAT;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        device->CreateTexture1D(&td, nullptr, &ctx.lutTexture1D);

        D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
        srvd.Format = DXGI_FORMAT_R32_FLOAT;
        srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D;
        srvd.Texture1D.MipLevels = 1;
        device->CreateShaderResourceView(ctx.lutTexture1D, &srvd, &ctx.lutSRV);
    }

    // Create noise texture (256x256 R32G32B32A32_FLOAT, filled once)
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = 256;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        std::mt19937 rng(42);
        std::uniform_real_distribution<float> dist(0.f, 1.f);
        std::vector<float> noise(256 * 256 * 4);
        for (int i = 0; i < 256 * 256 * 4; ++i)
            noise[i] = dist(rng);

        D3D11_SUBRESOURCE_DATA init = {};
        init.pSysMem = noise.data();
        init.SysMemPitch = 256 * 4 * sizeof(float);

        device->CreateTexture2D(&td, &init, &ctx.noiseTexture);
        D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
        srvd.Format = td.Format;
        srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvd.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(ctx.noiseTexture, &srvd, &ctx.noiseSRV);
    }

    return true;
}

void Shutdown(GpuFxContext& ctx) {
    if (ctx.context) { ctx.context->Release(); ctx.context = nullptr; }

    ReleaseCOM(ctx.vsFullscreen);
    // Release all pixel shaders
    ID3D11PixelShader** allPS[] = {
        &ctx.psHSV, &ctx.psHSVSelection, &ctx.psCurves, &ctx.psNoise,
        &ctx.psAlphaInvert, &ctx.psInvertColors, &ctx.psMaskAlpha,
        &ctx.psBoxBlurH, &ctx.psBoxBlurV,
        &ctx.psFillSolid, &ctx.psFillTexture, &ctx.psExtractAlpha, &ctx.psSpread, &ctx.psOffset,
        &ctx.psDilateH, &ctx.psDilateV, &ctx.psErodeH, &ctx.psErodeV,
        &ctx.psShadowColorize, &ctx.psOutlineSolid, &ctx.psOutlineGradient, &ctx.psOutlineTexture,
        &ctx.psSubtract, &ctx.psNearestUp
    };
    for (auto p : allPS) ReleaseCOM(*p);

    ReleaseCOM(ctx.cbFilter);
    ReleaseCOM(ctx.cbBlur);
    ReleaseCOM(ctx.samplerLinearClamp);
    ReleaseCOM(ctx.samplerLinearWrap);
    ReleaseCOM(ctx.samplerPointClamp);
    ReleaseCOM(ctx.lutTexture1D);
    ReleaseCOM(ctx.lutSRV);
    ReleaseCOM(ctx.noiseTexture);
    ReleaseCOM(ctx.noiseSRV);
}

// ============================================================
//  Target management
// ============================================================

GpuFxTarget CreateTarget(GpuFxContext& ctx, int w, int h, DXGI_FORMAT fmt) {
    GpuFxTarget t;
    t.width = w; t.height = h;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)w; td.Height = (UINT)h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(ctx.device->CreateTexture2D(&td, nullptr, &t.texture)))
        return t;

    ctx.device->CreateRenderTargetView(t.texture, nullptr, &t.rtv);
    ctx.device->CreateShaderResourceView(t.texture, nullptr, &t.srv);
    return t;
}

GpuFxTarget UploadBuffer(GpuFxContext& ctx, const float* rgba32f, int w, int h, DXGI_FORMAT fmt) {
    GpuFxTarget t;
    t.width = w; t.height = h;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)w; td.Height = (UINT)h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = rgba32f;
    init.SysMemPitch = (UINT)(w * 4 * sizeof(float));

    if (FAILED(ctx.device->CreateTexture2D(&td, &init, &t.texture)))
        return t;

    ctx.device->CreateShaderResourceView(t.texture, nullptr, &t.srv);
    return t;
}

GpuFxTarget UploadMask(GpuFxContext& ctx, const uint8_t* mask, int w, int h) {
    GpuFxTarget t;
    t.width = w; t.height = h;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)w; td.Height = (UINT)h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = mask;
    init.SysMemPitch = (UINT)w;

    if (FAILED(ctx.device->CreateTexture2D(&td, &init, &t.texture)))
        return t;

    ctx.device->CreateShaderResourceView(t.texture, nullptr, &t.srv);
    return t;
}

void ReadBack(const GpuFxContext& ctx, GpuFxTarget& target, float* outRGBA32F, int w, int h) {
    if (!target.texture || !ctx.context) return;
    // Staging readback
    D3D11_TEXTURE2D_DESC desc;
    target.texture->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D* staging = nullptr;
    ctx.device->CreateTexture2D(&sd, nullptr, &staging);
    if (!staging) return;

    ctx.context->CopyResource(staging, target.texture);

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        const uint8_t* src = (const uint8_t*)mapped.pData;
        int srcPitch = mapped.RowPitch;
        for (int y = 0; y < h; ++y) {
            memcpy(outRGBA32F + (size_t)y * w * 4, src + y * srcPitch, (size_t)w * 4 * sizeof(float));
        }
        ctx.context->Unmap(staging, 0);
    }
    staging->Release();
}

// ============================================================
//  Internal: run a single fullscreen PS pass
// ============================================================

static void DispatchPass(GpuFxContext& ctx,
                         ID3D11PixelShader* ps,
                         ID3D11ShaderResourceView* inputSRV,
                         GpuFxTarget& output,
                         const GpuFilterParams* filterParams,
                         const GpuBlurParams* blurParams,
                         ID3D11ShaderResourceView* extraSRV0,  // t1 or t2
                         ID3D11ShaderResourceView* extraSRV1,  // t3 or t5
                         int extraSlot0 = 1,
                         int extraSlot1 = 2) {
    auto* c = ctx.context;

    // Bind output RT
    c->OMSetRenderTargets(1, &output.rtv, nullptr);

    // Viewport
    D3D11_VIEWPORT vp = { 0, 0, (float)output.width, (float)output.height, 0, 1 };
    c->RSSetViewports(1, &vp);

    // IA: no vertex buffer (fullscreen triangle via SV_VertexID)
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    c->IASetInputLayout(nullptr);

    // Shaders
    c->VSSetShader(ctx.vsFullscreen, nullptr, 0);
    c->PSSetShader(ps, nullptr, 0);

    // Samplers: s0=linear clamp, s1=linear wrap, s2=point clamp
    ID3D11SamplerState* samplers[] = { ctx.samplerLinearClamp, ctx.samplerLinearWrap, ctx.samplerPointClamp };
    c->PSSetSamplers(0, 3, samplers);

    // SRVs
    ID3D11ShaderResourceView* srvs[6] = {};
    srvs[0] = inputSRV;
    if (extraSRV0 && extraSlot0 >= 1 && extraSlot0 < 6) srvs[extraSlot0] = extraSRV0;
    if (extraSRV1 && extraSlot1 >= 1 && extraSlot1 < 6) srvs[extraSlot1] = extraSRV1;
    c->PSSetShaderResources(0, 6, srvs);

    // Constant buffers
    if (filterParams) {
        D3D11_MAPPED_SUBRESOURCE mr;
        c->Map(ctx.cbFilter, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr);
        memcpy(mr.pData, filterParams->Data(), filterParams->ByteSize());
        c->Unmap(ctx.cbFilter, 0);
        c->PSSetConstantBuffers(0, 1, &ctx.cbFilter);
    }
    if (blurParams) {
        D3D11_MAPPED_SUBRESOURCE mr;
        c->Map(ctx.cbBlur, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr);
        memcpy(mr.pData, blurParams->Data(), blurParams->ByteSize());
        c->Unmap(ctx.cbBlur, 0);
        c->PSSetConstantBuffers(1, 1, &ctx.cbBlur);
    }

    // Draw fullscreen triangle (3 vertices, no VB)
    c->Draw(3, 0);

    // Unbind
    ID3D11ShaderResourceView* nullSRVs[6] = {};
    c->PSSetShaderResources(0, 6, nullSRVs);
    c->OMSetRenderTargets(0, nullptr, nullptr);
}

// ============================================================
//  Public API implementations
// ============================================================

void ApplyHSV(GpuFxContext& ctx,
              ID3D11ShaderResourceView* inputSRV, GpuFxTarget& output,
              int w, int h, float hueShift, float satShift, float valShift,
              ID3D11ShaderResourceView* selectionMaskSRV, float selectionWeight) {
    if (!ctx.psHSV) return;

    GpuFilterParams fp;
    fp.params[0] = hueShift;
    fp.params[1] = satShift;
    fp.params[2] = valShift;
    fp.selectionWeight = selectionWeight;

    ID3D11PixelShader* ps = (selectionMaskSRV && selectionWeight < 1.f)
        ? ctx.psHSVSelection : ctx.psHSV;

    DispatchPass(ctx, ps, inputSRV, output, &fp, nullptr,
                 selectionMaskSRV, nullptr, 2, 3);
}

void ApplyCurves(GpuFxContext& ctx,
                 ID3D11ShaderResourceView* inputSRV, GpuFxTarget& output,
                 int w, int h, const float* lut256,
                 int channelsMask,
                 ID3D11ShaderResourceView* selectionMaskSRV, float selectionWeight) {
    if (!ctx.psCurves || !lut256) return;

    // Update LUT texture
    D3D11_MAPPED_SUBRESOURCE mr;
    ctx.context->Map(ctx.lutTexture1D, 0, D3D11_MAP_WRITE_DISCARD, 0, &mr);
    memcpy(mr.pData, lut256, 256 * sizeof(float));
    ctx.context->Unmap(ctx.lutTexture1D, 0);

    GpuFilterParams fp;
    fp.params[0] = *(float*)&channelsMask; // bit pattern as float
    fp.selectionWeight = selectionWeight;

    DispatchPass(ctx, ctx.psCurves, inputSRV, output, &fp, nullptr,
                 selectionMaskSRV, ctx.lutSRV, 2, 5);
}

void ApplyNoise(GpuFxContext& ctx,
                ID3D11ShaderResourceView* inputSRV, GpuFxTarget& output,
                int w, int h, float strength, bool colorNoise, uint32_t /*seed*/,
                ID3D11ShaderResourceView* selectionMaskSRV, float selectionWeight) {
    if (!ctx.psNoise) return;

    GpuFilterParams fp;
    fp.params[0] = strength;
    fp.params[1] = colorNoise ? 1.f : 0.f;
    fp.selectionWeight = selectionWeight;

    DispatchPass(ctx, ctx.psNoise, inputSRV, output, &fp, nullptr,
                 selectionMaskSRV, ctx.noiseSRV, 2, 6);
}

void ApplyAlphaInvert(GpuFxContext& ctx,
                      ID3D11ShaderResourceView* inputSRV, GpuFxTarget& output,
                      int w, int h) {
    if (!ctx.psAlphaInvert) return;
    DispatchPass(ctx, ctx.psAlphaInvert, inputSRV, output, nullptr, nullptr);
}

void ApplyInvertColors(GpuFxContext& ctx,
                       ID3D11ShaderResourceView* inputSRV, GpuFxTarget& output,
                       int w, int h,
                       ID3D11ShaderResourceView* selectionMaskSRV, float selectionWeight) {
    if (!ctx.psInvertColors) return;
    GpuFilterParams fp;
    fp.selectionWeight = selectionWeight;
    DispatchPass(ctx, ctx.psInvertColors, inputSRV, output, &fp, nullptr,
                 selectionMaskSRV, nullptr, 2, 3);
}

void ApplyMaskToAlpha(GpuFxContext& ctx,
                      ID3D11ShaderResourceView* inputSRV, GpuFxTarget& output,
                      int w, int h, float fillOpacity,
                      ID3D11ShaderResourceView* maskSRV) {
    if (!ctx.psMaskAlpha) return;
    GpuFilterParams fp;
    fp.params[0] = fillOpacity;
    DispatchPass(ctx, ctx.psMaskAlpha, inputSRV, output, &fp, nullptr,
                 maskSRV, nullptr, 2, 3);
}

void ApplyBoxBlur(GpuFxContext& ctx,
                  ID3D11ShaderResourceView* inputSRV, GpuFxTarget& output,
                  int w, int h, int radius, int passes, int /*channelCount*/) {
    if (!ctx.psBoxBlurH || !ctx.psBoxBlurV || radius < 1) return;

    GpuBlurParams bp;
    bp.radius = radius;
    bp.pass = 0;
    bp.channelCount = 4;

    // Create ping-pong targets
    GpuFxTarget ping = CreateTarget(ctx, w, h);
    GpuFxTarget pong = CreateTarget(ctx, w, h);
    if (!ping.Valid() || !pong.Valid()) {
        ping.Release(); pong.Release();
        return;
    }

    // Input -> ping (first H pass)
    DispatchPass(ctx, ctx.psBoxBlurH, inputSRV, ping, nullptr, &bp);

    for (int p = 0; p < passes; ++p) {
        // H -> V
        bp.pass = 1;
        DispatchPass(ctx, ctx.psBoxBlurV, ping.srv, pong, nullptr, &bp);
        // V -> H (skip on last pass if we're done)
        if (p < passes - 1 || passes == 1) {
            bp.pass = 0;
            DispatchPass(ctx, ctx.psBoxBlurH, pong.srv, ping, nullptr, &bp);
        }
    }

    // Copy last result to output
    // (pong has the final result after last V pass)
    ctx.context->CopyResource(output.texture, pong.texture);
    // Recreate output SRV (already valid from CreateTarget)
    // Actually output was passed in, assume it already has RTV+SRV

    ping.Release();
    pong.Release();
}

GpuFxTarget BuildShadow(GpuFxContext& ctx,
                        ID3D11ShaderResourceView* contentSRV,
                        int w, int h,
                        float shadowColor[4], float opacity,
                        float distance, float angleDeg, float offsetX, float offsetY,
                        float spread01, float blurRadius,
                        int blurPasses, bool previewQuality) {
    GpuFxTarget result;
    result.width = w; result.height = h;
    if (!contentSRV) return result;

    int br = std::max(0, (int)std::lround(blurRadius));
    int passes = blurPasses;
    if (previewQuality) {
        br = std::min(br, 40);
        passes = (br > 12) ? 1 : 2;
    }

    // Step 1: Extract alpha
    GpuFxTarget alphaTex = CreateTarget(ctx, w, h);
    if (ctx.psExtractAlpha && alphaTex.Valid())
        DispatchPass(ctx, ctx.psExtractAlpha, contentSRV, alphaTex, nullptr, nullptr);

    // Step 2: Apply spread
    GpuFxTarget spreadTex = CreateTarget(ctx, w, h);
    if (spread01 > 0 && ctx.psSpread && spreadTex.Valid()) {
        GpuFilterParams fp;
        fp.params[0] = spread01;
        DispatchPass(ctx, ctx.psSpread, alphaTex.srv, spreadTex, &fp, nullptr);
        alphaTex.Release();
        alphaTex = std::move(spreadTex);
    }

    // Step 3: Offset
    float rad = angleDeg * 3.14159265f / 180.f;
    float dx = std::lround(std::cos(rad) * distance + offsetX);
    float dy = std::lround(std::sin(rad) * distance + offsetY);

    GpuFxTarget offsetTex = CreateTarget(ctx, w, h);
    if ((dx != 0 || dy != 0) && ctx.psOffset && offsetTex.Valid()) {
        GpuFilterParams fp;
        fp.params[0] = dx;
        fp.params[1] = dy;
        DispatchPass(ctx, ctx.psOffset, alphaTex.srv, offsetTex, &fp, nullptr);
        alphaTex.Release();
        alphaTex = std::move(offsetTex);
    }

    // Step 4: Blur
    if (br > 0) {
        GpuFxTarget blurred = CreateTarget(ctx, w, h);
        ApplyBoxBlur(ctx, alphaTex.srv, blurred, w, h, br, passes, 1);
        alphaTex.Release();
        alphaTex = std::move(blurred);
    }

    // Step 5: Colorize (shadow color × opacity)
    result = CreateTarget(ctx, w, h);
    if (ctx.psShadowColorize && result.Valid()) {
        GpuFilterParams fp;
        fp.params[0] = shadowColor[0];
        fp.params[1] = shadowColor[1];
        fp.params[2] = shadowColor[2];
        fp.params[3] = shadowColor[3] * opacity;
        DispatchPass(ctx, ctx.psShadowColorize, alphaTex.srv, result, &fp, nullptr);
    }

    alphaTex.Release();
    return result;
}

GpuFxTarget BuildOutline(GpuFxContext& ctx,
                         ID3D11ShaderResourceView* contentSRV,
                         int w, int h,
                         float color[4], float opacity, float size,
                         int outlinePos, int outlineFill,
                         const float* gradientStops, int gradientStopCount,
                         ID3D11ShaderResourceView* textureSRV,
                         float texScale[2], float texOffset[2],
                         int gradientMap, bool previewQuality) {
    GpuFxTarget result;
    result.width = w; result.height = h;
    if (!contentSRV) return result;

    int r = std::max(0, (int)std::lround(size));
    if (previewQuality) r = std::min(r, 32);

    // Step 1: Extract alpha
    GpuFxTarget alphaTex = CreateTarget(ctx, w, h);
    if (ctx.psExtractAlpha && alphaTex.Valid())
        DispatchPass(ctx, ctx.psExtractAlpha, contentSRV, alphaTex, nullptr, nullptr);

    // Step 2: Compute ring (dilated - original / eroded, etc.)
    GpuFxTarget ringTex = CreateTarget(ctx, w, h);
    if (!ringTex.Valid()) { alphaTex.Release(); return result; }

    if (r < 1) {
        // Simple edge detection: alpha - min(neighbors)
        // For simplicity, approximate: blur slightly and subtract original
        GpuBlurParams bp;
        bp.radius = 1; bp.pass = 0; bp.channelCount = 1;
        GpuFxTarget blurred = CreateTarget(ctx, w, h);
        if (ctx.psBoxBlurH && blurred.Valid()) {
            DispatchPass(ctx, ctx.psBoxBlurH, alphaTex.srv, blurred, nullptr, &bp);
            bp.pass = 1;
            DispatchPass(ctx, ctx.psBoxBlurV, blurred.srv, ringTex, nullptr, &bp);
        }
        blurred.Release();
        // Subtract: blurred - alpha
        GpuFxTarget sub = CreateTarget(ctx, w, h);
        if (ctx.psSubtract && sub.Valid()) {
            DispatchPass(ctx, ctx.psSubtract, ringTex.srv, sub, nullptr, nullptr, alphaTex.srv, 1);
            ringTex.Release();
            ringTex = std::move(sub);
        }
    } else {
        // Dilate
        GpuFxTarget dilated = CreateTarget(ctx, w, h);
        GpuBlurParams bp;
        bp.radius = r; bp.pass = 0; bp.channelCount = 1;

        if (outlinePos == 0 || outlinePos == 2) {
            // Outside or Center: need dilate
            int dilateR = (outlinePos == 2) ? (r + 1) / 2 : r;
            if (ctx.psDilateH && ctx.psDilateV && dilated.Valid()) {
                bp.radius = dilateR;
                DispatchPass(ctx, ctx.psDilateH, alphaTex.srv, dilated, nullptr, &bp);
                bp.pass = 1;
                GpuFxTarget dilated2 = CreateTarget(ctx, w, h);
                DispatchPass(ctx, ctx.psDilateV, dilated.srv, dilated2, nullptr, &bp);
                dilated.Release();
                dilated = std::move(dilated2);
            }
        }

        if (outlinePos == 1 || outlinePos == 2) {
            // Inside or Center: need erode
            int erodeR = (outlinePos == 2) ? r / 2 : r;
            GpuFxTarget eroded = CreateTarget(ctx, w, h);
            if (ctx.psErodeH && ctx.psErodeV && eroded.Valid()) {
                bp.radius = erodeR; bp.pass = 0;
                DispatchPass(ctx, ctx.psErodeH, alphaTex.srv, eroded, nullptr, &bp);
                bp.pass = 1;
                GpuFxTarget eroded2 = CreateTarget(ctx, w, h);
                DispatchPass(ctx, ctx.psErodeV, eroded.srv, eroded2, nullptr, &bp);
                eroded.Release();
                eroded = std::move(eroded2);

                // ring = dilated - eroded (Center)
                // ring = alpha - eroded (Inside)
                GpuFxTarget first = (outlinePos == 2) ? dilated : alphaTex;
                GpuFxTarget sub = CreateTarget(ctx, w, h);
                if (ctx.psSubtract && sub.Valid()) {
                    DispatchPass(ctx, ctx.psSubtract, first.srv, sub, nullptr, nullptr, eroded.srv, 1);
                    ringTex.Release();
                    ringTex = std::move(sub);
                }
                if (outlinePos == 1) dilated.Release();
            }
        } else {
            // Outside: ring = dilated - alpha
            GpuFxTarget sub = CreateTarget(ctx, w, h);
            if (ctx.psSubtract && sub.Valid()) {
                DispatchPass(ctx, ctx.psSubtract, dilated.srv, sub, nullptr, nullptr, alphaTex.srv, 1);
                ringTex.Release();
                ringTex = std::move(sub);
            }
            dilated.Release();
        }
    }

    // Step 3: Soften ring (light blur)
    if (r >= 1 && ctx.psBoxBlurH) {
        GpuFxTarget blurred = CreateTarget(ctx, w, h);
        GpuBlurParams bp;
        bp.radius = 1; bp.pass = 0; bp.channelCount = 1;
        DispatchPass(ctx, ctx.psBoxBlurH, ringTex.srv, blurred, nullptr, &bp);
        bp.pass = 1;
        GpuFxTarget blurred2 = CreateTarget(ctx, w, h);
        DispatchPass(ctx, ctx.psBoxBlurV, blurred.srv, blurred2, nullptr, &bp);
        blurred.Release();
        ringTex.Release();
        ringTex = std::move(blurred2);
    }

    // Step 4: Fill (Solid / Gradient / Texture)
    result = CreateTarget(ctx, w, h);
    GpuFilterParams fp;
    fp.params[0] = color[0]; fp.params[1] = color[1];
    fp.params[2] = color[2]; fp.params[3] = opacity;

    if (outlineFill == 2 && textureSRV) {
        fp.texScale[0] = texScale ? texScale[0] : 1.f;
        fp.texScale[1] = texScale ? texScale[1] : 1.f;
        fp.texOffset[0] = texOffset ? texOffset[0] : 0.f;
        fp.texOffset[1] = texOffset ? texOffset[1] : 0.f;
        if (ctx.psOutlineTexture && result.Valid())
            DispatchPass(ctx, ctx.psOutlineTexture, ringTex.srv, result, &fp, nullptr, textureSRV, nullptr, 1, 3);
    } else if (outlineFill == 1 && gradientStops && gradientStopCount >= 2) {
        // Upload gradient as 256x1 texture
        GpuFxTarget gradTex = CreateTarget(ctx, 256, 1);
        if (gradTex.Valid()) {
            std::vector<float> gradData(256 * 4);
            for (int i = 0; i < 256; ++i) {
                float t = (float)i / 255.f;
                // Sample gradient
                float gc[4] = {};
                const float* s0 = gradientStops;
                const float* sN = gradientStops + (gradientStopCount - 1) * 4;
                if (t <= s0[0]) { for (int c = 0; c < 4; ++c) gc[c] = s0[c+1]; }
                else if (t >= sN[0]) { for (int c = 0; c < 4; ++c) gc[c] = sN[c+1]; }
                else {
                    for (int j = 0; j < gradientStopCount - 1; ++j) {
                        const float* sa = gradientStops + j * 5;
                        const float* sb = gradientStops + (j + 1) * 5;
                        if (t >= sa[0] && t <= sb[0]) {
                            float u = (sb[0] - sa[0] > 1e-6f) ? (t - sa[0]) / (sb[0] - sa[0]) : 0.f;
                            for (int c = 0; c < 4; ++c) gc[c] = sa[c+1] * (1-u) + sb[c+1] * u;
                            break;
                        }
                    }
                }
                for (int c = 0; c < 4; ++c) gradData[i * 4 + c] = gc[c];
            }
            // Upload gradient to texture
            D3D11_BOX box = { 0, 0, 0, 256, 1, 1 };
            ctx.context->UpdateSubresource(gradTex.texture, 0, &box, gradData.data(), 256 * 16, 0);

            if (ctx.psOutlineGradient && result.Valid())
                DispatchPass(ctx, ctx.psOutlineGradient, ringTex.srv, result, &fp, nullptr, gradTex.srv, nullptr, 4, 3);
            gradTex.Release();
        }
    } else {
        // Solid
        if (ctx.psOutlineSolid && result.Valid())
            DispatchPass(ctx, ctx.psOutlineSolid, ringTex.srv, result, &fp, nullptr);
    }

    alphaTex.Release();
    ringTex.Release();
    return result;
}

GpuFxTarget BuildFill(GpuFxContext& ctx, int w, int h, float color[4],
                      bool hasTexture, ID3D11ShaderResourceView* textureSRV,
                      float texScale[2], float texOffset[2]) {
    GpuFxTarget result = CreateTarget(ctx, w, h);
    if (!result.Valid()) return result;

    GpuFilterParams fp;
    fp.params[0] = color[0]; fp.params[1] = color[1];
    fp.params[2] = color[2]; fp.params[3] = color[3];
    if (texScale) { fp.texScale[0] = texScale[0]; fp.texScale[1] = texScale[1]; }
    if (texOffset) { fp.texOffset[0] = texOffset[0]; fp.texOffset[1] = texOffset[1]; }

    if (hasTexture && textureSRV && ctx.psFillTexture) {
        DispatchPass(ctx, ctx.psFillTexture, textureSRV, result, &fp, nullptr);
    } else if (ctx.psFillSolid) {
        // Create 1x1 dummy texture (shader uses cbuffer Params, not texture)
        float dummy[4] = { 1, 1, 1, 1 };
        GpuFxTarget dummyTex = UploadBuffer(ctx, dummy, 1, 1);
        DispatchPass(ctx, ctx.psFillSolid, dummyTex.srv, result, &fp, nullptr);
        dummyTex.Release();
    }
    return result;
}

} // namespace gpu_fx