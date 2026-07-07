// CanvasTiles.hlsl
// Tile-space vertex + blend pixel shaders for tiled layer compositing.
// Declares shared cbuffers to allow standalone fxc compilation.

cbuffer CanvasBuffer : register(b0)
{
    float4 u_ViewportSizeAndZoom;
    float4 u_OffsetAndCanvasSize;
    float4 u_ChannelMasksAndFlags;
    float4 u_ViewportFlags;
};

cbuffer LayerBuffer : register(b1)
{
    float4 u_LayerParams;
    float4 u_TransformParams;
    float4 u_CenterParams;
};

cbuffer TileParams : register(b2)
{
    float4 u_TileParams; // x: tileX, y: tileY, z: canvasWidth, w: canvasHeight
};

struct VS_TILE_INPUT
{
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

struct PS_TILE_INPUT
{
    float4 pos       : SV_POSITION;
    float2 uv        : TEXCOORD0;
    float2 screenPos : TEXCOORD1;
};

// Maps a 256×256 tile quad into exact canvas-space NDC coordinates.
PS_TILE_INPUT VSTileMain(VS_TILE_INPUT input)
{
    PS_TILE_INPUT output;

    float tileX       = u_TileParams.x;
    float tileY       = u_TileParams.y;
    float canvasWidth = u_TileParams.z;
    float canvasHeight= u_TileParams.w;

    // Tile vertex in canvas pixels [0 .. canvasSize]
    float2 pixelPos = float2(
        tileX * 256.0f + input.pos.x * 256.0f,
        tileY * 256.0f + input.pos.y * 256.0f
    );

    // If this is a floating layer, apply rotation, scale, and translation around the center pivot (u_CenterParams.xy)
    if (u_TransformParams.w > 0.5f) {
        float2 center = u_CenterParams.xy * float2(canvasWidth, canvasHeight);
        float2 rel = pixelPos - center;
        
        // 1. Scale
        rel *= u_TransformParams.xy;
        
        // 2. Rotate
        float angle = u_TransformParams.z;
        float cosA = cos(angle);
        float sinA = sin(angle);
        float2 rotated;
        rotated.x = rel.x * cosA - rel.y * sinA;
        rotated.y = rel.x * sinA + rel.y * cosA;
        
        // 3. Translate (u_LayerParams.zw is normalized translation offset)
        float2 translation = u_LayerParams.zw * float2(canvasWidth, canvasHeight);
        
        pixelPos = rotated + center + translation;
    }

    // Canvas pixels → NDC [-1, 1]
    float2 ndc = (pixelPos / float2(canvasWidth, canvasHeight)) * 2.0f - 1.0f;
    ndc.y = -ndc.y; // DirectX Y inversion

    output.pos       = float4(ndc, 0.0f, 1.0f);
    output.uv        = input.uv;
    output.screenPos = pixelPos;
    return output;
}

Texture2D    g_Texture   : register(t0);
Texture2D    g_LayerMask : register(t1);
Texture2D    g_Composite : register(t2);
SamplerState g_SamplerPoint  : register(s0);
SamplerState g_SamplerLinear : register(s1);

// Blends one 256×256 tile onto the ping-pong composite RT,
// applying opacity, layer mask, and the encoded blend mode.
float4 PSTileBlend(PS_TILE_INPUT input) : SV_TARGET
{
    // UV within this tile maps 1-to-1 to [0,1] of the tile texture.
    float4 col = g_Texture.Sample(g_SamplerPoint, input.uv);

    // If alpha channel is globally disabled, treat tile as fully opaque.
    if (u_ChannelMasksAndFlags.w < 0.5f)
    {
        col.a = 1.0f;
    }

    // Multiply by layer opacity (stored in u_LayerParams.x).
    col.a *= u_LayerParams.x;

    // Apply layer mask if enabled.
    if (u_LayerParams.y > 0.5f)
    {
        float maskVal = g_LayerMask.Sample(g_SamplerPoint, input.uv).r;
        col.a *= maskVal;
    }

    // Blend mode composite (u_CenterParams.z encodes BlendMode as float).
    uint blendMode = (uint)(u_CenterParams.z + 0.5f);
    if (blendMode > 0u)
    {
        // Convert tile screen-space position back to canvas UV for reading composite.
        float2 canvasUV = input.screenPos / u_OffsetAndCanvasSize.zw;
        float4 dst = g_Composite.Sample(g_SamplerPoint, canvasUV);
        float3 s = col.rgb;
        float3 d = dst.rgb;
        float3 result = s;

        if      (blendMode == 1u) result = s * d;
        else if (blendMode == 2u) result = 1.0f - (1.0f - s) * (1.0f - d);
        else if (blendMode == 3u)
        {
            result.r = (d.r < 0.5f) ? 2.0f*s.r*d.r : 1.0f - 2.0f*(1.0f-s.r)*(1.0f-d.r);
            result.g = (d.g < 0.5f) ? 2.0f*s.g*d.g : 1.0f - 2.0f*(1.0f-s.g)*(1.0f-d.g);
            result.b = (d.b < 0.5f) ? 2.0f*s.b*d.b : 1.0f - 2.0f*(1.0f-s.b)*(1.0f-d.b);
        }
        else if (blendMode == 4u) result = min(s + d, 1.0f);
        else if (blendMode == 5u) result = max(d - s, 0.0f);
        else if (blendMode == 6u) result = min(s, d);
        else if (blendMode == 7u) result = max(s, d);
        else if (blendMode == 8u)
        {
            result.r = (s.r < 0.5f) ? 2.0f*s.r*d.r : 1.0f - 2.0f*(1.0f-s.r)*(1.0f-d.r);
            result.g = (s.g < 0.5f) ? 2.0f*s.g*d.g : 1.0f - 2.0f*(1.0f-s.g)*(1.0f-d.g);
            result.b = (s.b < 0.5f) ? 2.0f*s.b*d.b : 1.0f - 2.0f*(1.0f-s.b)*(1.0f-d.b);
        }
        else if (blendMode == 9u)
        {
            result.r = (s.r < 0.5f) ? d.r - (1.0f-2.0f*s.r)*d.r*(1.0f-d.r) : d.r + (2.0f*s.r-1.0f)*(sqrt(d.r)-d.r);
            result.g = (s.g < 0.5f) ? d.g - (1.0f-2.0f*s.g)*d.g*(1.0f-d.g) : d.g + (2.0f*s.g-1.0f)*(sqrt(d.g)-d.g);
            result.b = (s.b < 0.5f) ? d.b - (1.0f-2.0f*s.b)*d.b*(1.0f-d.b) : d.b + (2.0f*s.b-1.0f)*(sqrt(d.b)-d.b);
        }

        col.rgb = result;
    }

    return col;
}
