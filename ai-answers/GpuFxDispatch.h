#pragma once
// GpuFxDispatch.h — GPU filter/style dispatch via D3D11 pixel shader ping-pong
// Eliminates CPU per-pixel loops for: HSV, Blur, Curves, Noise, Alpha ops,
// Shadow build, Outline build, Fill texture, Mask apply.

#include <d3d11.h>
#include <d3d11shader.h>
#include <dxgi.h>
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>

struct GpuFxContext {
    ID3D11Device*        device  = nullptr;
    ID3D11DeviceContext* context = nullptr;

    // Shaders (compiled at init from LayerFx.hlsl)
    ID3D11VertexShader*  vsFullscreen  = nullptr;
    ID3D11PixelShader*   psHSV          = nullptr;
    ID3D11PixelShader*   psHSVSelection = nullptr;
    ID3D11PixelShader*   psCurves       = nullptr;
    ID3D11PixelShader*   psNoise        = nullptr;
    ID3D11PixelShader*   psAlphaInvert  = nullptr;
    ID3D11PixelShader*   psInvertColors = nullptr;
    ID3D11PixelShader*   psMaskAlpha    = nullptr;
    ID3D11PixelShader*   psBoxBlurH     = nullptr;
    ID3D11PixelShader*   psBoxBlurV     = nullptr;
    ID3D11PixelShader*   psFillSolid    = nullptr;
    ID3D11PixelShader*   psFillTexture  = nullptr;
    ID3D11PixelShader*   psExtractAlpha = nullptr;
    ID3D11PixelShader*   psSpread       = nullptr;
    ID3D11PixelShader*   psOffset       = nullptr;
    ID3D11PixelShader*   psDilateH      = nullptr;
    ID3D11PixelShader*   psDilateV      = nullptr;
    ID3D11PixelShader*   psErodeH       = nullptr;
    ID3D11PixelShader*   psErodeV       = nullptr;
    ID3D11PixelShader*   psShadowColor  = nullptr;
    ID3D11PixelShader*   psOutlineSolid = nullptr;
    ID3D11PixelShader*   psOutlineGrad  = nullptr;
    ID3D11PixelShader*   psOutlineTex   = nullptr;
    ID3D11PixelShader*   psSubtract     = nullptr;
    ID3D11PixelShader*   psNearestUp    = nullptr;

    // Constant buffers
    ID3D11Buffer* cbFilter = nullptr;
    ID3D11Buffer* cbBlur   = nullptr;

    // Samplers
    ID3D11SamplerState* samplerLinearClamp = nullptr;
    ID3D11SamplerState* samplerLinearWrap  = nullptr;
    ID3D11SamplerState* samplerPointClamp  = nullptr;

    // Shared LUT texture (256x1 R32_FLOAT for curves)
    ID3D11Texture1D*        lutTexture1D   = nullptr;
    ID3D11ShaderResourceView* lutSRV       = nullptr;

    // Shared noise texture (256x256 R32_FLOAT)
    ID3D11Texture2D*        noiseTexture   = nullptr;
    ID3D11ShaderResourceView* noiseSRV     = nullptr;
};

// Filter parameters (maps to HLSL cbuffer b0)
struct GpuFilterParams {
    float params[4]      = {};  // filter-specific (HSV shifts, noise strength, etc.)
    float texScale[2]    = {1.f, 1.f};
    float texOffset[2]   = {0.f, 0.f};
    float selectionWeight = 1.f;
    float _pad            = 0.f;

    // Raw bytes for D3D11 UpdateSubresource (must be 48 bytes = 12 floats)
    // Layout matches HLSL cbuffer: float4 Params, float2 TexScale, float2 TexOffset,
    //                               float SelectionWeight, float Pad1
    static constexpr size_t ByteSize() { return 48; }
    const void* Data() const { return this; }
};

// Blur parameters (maps to HLSL cbuffer b1)
struct GpuBlurParams {
    int   radius        = 1;
    int   pass          = 0;   // 0=H, 1=V
    int   channelCount  = 4;
    float pad           = 0.f;

    static constexpr size_t ByteSize() { return 16; }
    const void* Data() const { return this; }
};

// Helper: RAII pair of render target + SRV for ping-pong
struct GpuFxTarget {
    ID3D11Texture2D*            texture = nullptr;
    ID3D11RenderTargetView*     rtv     = nullptr;
    ID3D11ShaderResourceView*   srv     = nullptr;
    int width  = 0;
    int height = 0;

    bool Valid() const { return texture != nullptr; }
    void Release();
};

// ============================================================
//  Public API
// ============================================================

namespace gpu_fx {

// Initialize all shaders and resources. Call once after D3D11 device creation.
bool Init(GpuFxContext& ctx, ID3D11Device* device, const std::wstring& shaderDir);

// Release all GPU resources.
void Shutdown(GpuFxContext& ctx);

// Create a temporary render target (RGBA32_FLOAT) for ping-pong.
GpuFxTarget CreateTarget(GpuFxContext& ctx, int w, int h, DXGI_FORMAT fmt = DXGI_FORMAT_R32G32B32A32_FLOAT);

// Upload a CPU float RGBA buffer to a GPU texture.
GpuFxTarget UploadBuffer(GpuFxContext& ctx, const float* rgba32f, int w, int h,
                         DXGI_FORMAT fmt = DXGI_FORMAT_R32G32B32A32_FLOAT);

// Upload an R8 mask buffer.
GpuFxTarget UploadMask(GpuFxContext& ctx, const uint8_t* mask, int w, int h);

// Read back GPU texture to CPU float RGBA buffer.
void ReadBack(const GpuFxContext& ctx, GpuFxTarget& target, float* outRGBA32F, int w, int h);

// ---- Single-pass filter dispatches (input SRV -> output RT) ----

// HSV: params = {hueShift, satShift, valShift, 0}
void ApplyHSV(GpuFxContext& ctx,
              ID3D11ShaderResourceView* inputSRV, GpuFxTarget& output,
              int w, int h, float hueShift, float satShift, float valShift,
              ID3D11ShaderResourceView* selectionMaskSRV = nullptr,
              float selectionWeight = 1.f);

// Curves: uploads LUT to 1D texture, samples in shader
// channelsMask: bit0=R, bit1=G, bit2=B, bit3=A
void ApplyCurves(GpuFxContext& ctx,
                 ID3D11ShaderResourceView* inputSRV, GpuFxTarget& output,
                 int w, int h, const float* lut256,
                 int channelsMask,
                 ID3D11ShaderResourceView* selectionMaskSRV = nullptr,
                 float selectionWeight = 1.f);

// Noise: strength, colorNoise (true = per-channel RNG)
void ApplyNoise(GpuFxContext& ctx,
                ID3D11ShaderResourceView* inputSRV, GpuFxTarget& output,
                int w, int h, float strength, bool colorNoise, uint32_t seed = 1,
                ID3D11ShaderResourceView* selectionMaskSRV = nullptr,
                float selectionWeight = 1.f);

// Alpha Invert
void ApplyAlphaInvert(GpuFxContext& ctx,
                      ID3D11ShaderResourceView* inputSRV, GpuFxTarget& output,
                      int w, int h);

// Invert Colors (RGB only, with optional selection)
void ApplyInvertColors(GpuFxContext& ctx,
                       ID3D11ShaderResourceView* inputSRV, GpuFxTarget& output,
                       int w, int h,
                       ID3D11ShaderResourceView* selectionMaskSRV = nullptr,
                       float selectionWeight = 1.f);

// Mask Alpha: multiply alpha by mask * fillOpacity
void ApplyMaskToAlpha(GpuFxContext& ctx,
                      ID3D11ShaderResourceView* inputSRV, GpuFxTarget& output,
                      int w, int h, float fillOpacity,
                      ID3D11ShaderResourceView* maskSRV = nullptr);

// ---- Multi-pass operations ----

// Box Blur (Gaussian approx): radius, passes (typically 3), channelCount (1 or 4)
// Uses internal ping-pong. Caller can provide external targets to avoid alloc.
void ApplyBoxBlur(GpuFxContext& ctx,
                  ID3D11ShaderResourceView* inputSRV, GpuFxTarget& output,
                  int w, int h, int radius, int passes = 3, int channelCount = 4);

// Shadow: alpha -> spread -> offset -> blur -> colorize
// Returns RGBA texture with shadow (straight alpha).
GpuFxTarget BuildShadow(GpuFxContext& ctx,
                        ID3D11ShaderResourceView* contentSRV,
                        int w, int h,
                        float shadowColor[4], float opacity,
                        float distance, float angleDeg, float offsetX, float offsetY,
                        float spread, float blurRadius,
                        int blurPasses = 3, bool previewQuality = false);

// Outline: alpha -> dilate/erode -> ring -> blur -> fill
GpuFxTarget BuildOutline(GpuFxContext& ctx,
                         ID3D11ShaderResourceView* contentSRV,
                         int w, int h,
                         float color[4], float opacity, float size,
                         int outlinePos, // 0=Outside, 1=Inside, 2=Center
                         int outlineFill, // 0=Solid, 1=Gradient, 2=Texture
                         const float* gradientStops, int gradientStopCount,
                         ID3D11ShaderResourceView* textureSRV,
                         float texScale[2], float texOffset[2],
                         int gradientMap, // 0=distance, 1=horizontal, 2=vertical
                         bool previewQuality = false);

// Fill layer: solid color or texture × color
GpuFxTarget BuildFill(GpuFxContext& ctx,
                      int w, int h,
                      float color[4],
                      bool hasTexture,
                      ID3D11ShaderResourceView* textureSRV = nullptr,
                      float texScale[2] = nullptr, float texOffset[2] = nullptr);

} // namespace gpu_fx