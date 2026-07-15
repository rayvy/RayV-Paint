#pragma once

#include <d3d11.h>
#include <vector>
#include <cstdint>

// Lightweight GPU box-blur for filter preview (D3D11 pixel-shader ping-pong).
// Consumer-agnostic: operates on raw RGBA8/16F/32F regions supplied by caller.
// Falls back silently — caller keeps CPU path.
namespace gpu_fx {

struct GpuBlurContext {
    ID3D11Device*        device  = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11VertexShader*  vs      = nullptr;
    ID3D11PixelShader*   psH     = nullptr;
    ID3D11PixelShader*   psV     = nullptr;
    ID3D11Buffer*        cb      = nullptr;
    ID3D11Buffer*        vb      = nullptr;
    ID3D11InputLayout*   layout  = nullptr;
    ID3D11SamplerState*  samp    = nullptr;
    bool ready = false;

    void Shutdown();
};

// Compile shaders from file (LayerFxBlur.hlsl next to Canvas.hlsl).
bool InitGpuBlur(GpuBlurContext& ctx, ID3D11Device* device, ID3D11DeviceContext* context,
                 const wchar_t* hlslPath);

// Blur RGBA32F region in-place (w*h*4 floats). radius in pixels, 3 box passes.
// Returns false → caller must use CPU blur.
bool BlurRGBA32F(GpuBlurContext& ctx, std::vector<float>& rgba, int w, int h, int radius);

} // namespace gpu_fx
