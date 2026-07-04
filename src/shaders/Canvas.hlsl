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
};

PS_INPUT VSMain(VS_INPUT input)
{
    PS_INPUT output;
    
    float2 viewportSize = u_ViewportSizeAndZoom.xy;
    float zoom = u_ViewportSizeAndZoom.z;
    float2 panOffset = u_OffsetAndCanvasSize.xy;
    float2 canvasSize = u_OffsetAndCanvasSize.zw;
    
    // Map unit quad position (0 to 1) to canvas pixel size
    float2 canvasPixelPos = input.pos * canvasSize;
    // Apply zoom, translation, and center relative to viewport
    float2 screenPixelPos = canvasPixelPos * zoom + panOffset + viewportSize * 0.5f;
    
    // Transform from Screen-space pixels to NDC [-1, 1]
    float2 ndcPos = (screenPixelPos / viewportSize) * 2.0f - 1.0f;
    ndcPos.y = -ndcPos.y; // Invert Y for DirectX coordinate space
    
    output.pos = float4(ndcPos, 0.0f, 1.0f);
    output.uv = input.uv;
    output.screenPos = screenPixelPos;
    
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
    // Handle negative values correctly if any
    if (sum < 0) sum = -sum;
    
    float check = (sum % 2 == 0) ? 1.0f : 0.0f;
    float3 color1 = float3(0.18f, 0.18f, 0.18f); // Dark gray
    float3 color2 = float3(0.24f, 0.24f, 0.24f); // Lighter gray
    
    float3 finalColor = lerp(color1, color2, check);
    
    // Draw canvas border
    float2 pixelDist = min(input.uv, 1.0f - input.uv) * u_OffsetAndCanvasSize.zw;
    float distToEdge = min(pixelDist.x, pixelDist.y);
    
    if (distToEdge < 1.0f) // 1 pixel border
    {
        finalColor = float3(0.5f, 0.5f, 0.5f); // Border color
    }
    
    return float4(finalColor, 1.0f);
}
