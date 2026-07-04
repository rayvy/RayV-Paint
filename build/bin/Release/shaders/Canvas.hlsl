struct VS_INPUT
{
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
    float2 screenPos : TEXCOORD1;
};

cbuffer CanvasBuffer : register(b0)
{
    float4 u_ViewportSizeAndZoom; // xy: Viewport size in pixels, z: Zoom, w: Padding
    float4 u_OffsetAndCanvasSize; // xy: Offset/Pan in pixels, zw: Canvas size in pixels
    float4 u_VisModeAndMaskColor; // x: Vis Mode, yzw: Alpha Mask Color
};

cbuffer LayerBuffer : register(b1)
{
    float4 u_LayerParams; // x: opacity, yzw: unused
};

Texture2D g_Texture : register(t0);
SamplerState g_Sampler : register(s0);

PS_INPUT VSMain(VS_INPUT input)
{
    PS_INPUT output;
    
    float2 viewportSize = u_ViewportSizeAndZoom.xy;
    float zoom = u_ViewportSizeAndZoom.z;
    float2 panOffset = u_OffsetAndCanvasSize.xy;
    float2 canvasSize = u_OffsetAndCanvasSize.zw;
    
    // Map unit quad position (0 to 1) to canvas pixel size
    float2 canvasPixelPos = input.pos * canvasSize;
    // Apply zoom, translation, and center relative to viewport with integer pixel alignment
    float2 screenOrigin = floor(panOffset + viewportSize * 0.5f);
    float2 screenPixelPos = floor(canvasPixelPos * zoom) + screenOrigin;
    
    // Transform from Screen-space pixels to NDC [-1, 1]
    float2 ndcPos = (screenPixelPos / viewportSize) * 2.0f - 1.0f;
    ndcPos.y = -ndcPos.y; // Invert Y for DirectX coordinate space
    
    output.pos = float4(ndcPos, 0.0f, 1.0f);
    output.uv = input.uv;
    output.screenPos = screenPixelPos;
    
    return output;
}

PS_INPUT VSLayerMain(VS_INPUT input)
{
    PS_INPUT output;
    
    // Map unit quad pos (0 to 1) to NDC (-1 to 1)
    float2 ndcPos;
    ndcPos.x = input.pos.x * 2.0f - 1.0f;
    ndcPos.y = 1.0f - input.pos.y * 2.0f;
    
    output.pos = float4(ndcPos, 0.0f, 1.0f);
    output.uv = input.uv;
    output.screenPos = float2(0.0f, 0.0f);
    
    return output;
}


float4 PSMain(PS_INPUT input) : SV_TARGET
{
    float2 canvasCoords = input.uv * u_OffsetAndCanvasSize.zw;
    
    // 16x16 pixel checkerboard cells
    float cellSize = 16.0f;
    int2 cellIndex = (int2)floor(canvasCoords / cellSize);
    
    // Alternating checkerboard color
    int sum = cellIndex.x + cellIndex.y;
    if (sum < 0) sum = -sum;
    
    float check = (sum % 2 == 0) ? 1.0f : 0.0f;
    float3 color1 = float3(0.18f, 0.18f, 0.18f); // Dark gray
    float3 color2 = float3(0.24f, 0.24f, 0.24f); // Lighter gray
    float3 checkColor = lerp(color1, color2, check);
    
    // Sample composed layer texture
    float4 texCol = g_Texture.Sample(g_Sampler, input.uv);
    
    float3 finalColor = checkColor;
    int visMode = (int)u_VisModeAndMaskColor.x;
    
    if (visMode == 0) // Normal RGBA blended
    {
        finalColor = lerp(checkColor, texCol.rgb, texCol.a);
    }
    else if (visMode == 1) // RGB only (no alpha blending, show flat color or checkered)
    {
        finalColor = texCol.rgb;
    }
    else if (visMode == 2) // Alpha channel only
    {
        finalColor = float3(texCol.a, texCol.a, texCol.a);
    }
    else if (visMode == 3) // Alpha mask (custom color blended by alpha)
    {
        float3 maskColor = u_VisModeAndMaskColor.yzw;
        finalColor = lerp(checkColor, maskColor, texCol.a);
    }
    
    // Draw canvas border (adapts to zoom so it remains 1 pixel wide on screen)
    float2 pixelDist = min(input.uv, 1.0f - input.uv) * u_OffsetAndCanvasSize.zw;
    float distToEdge = min(pixelDist.x, pixelDist.y);
    float borderThreshold = 1.0f / u_ViewportSizeAndZoom.z;
    
    if (distToEdge < borderThreshold)
    {
        finalColor = float3(0.5f, 0.5f, 0.5f); // Border color
    }
    
    return float4(finalColor, 1.0f);
}

// Simple pixel shader to output layer contents multiplied by layer opacity
float4 PSLayerBlend(PS_INPUT input) : SV_TARGET
{
    float4 col = g_Texture.Sample(g_Sampler, input.uv);
    col.a *= u_LayerParams.x; // Multiply alpha by opacity
    return col;
}
