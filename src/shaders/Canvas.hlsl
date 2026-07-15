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
    float4 u_ViewportSizeAndZoom; // xy: Viewport size in pixels, z: Zoom, w: rotation
    float4 u_OffsetAndCanvasSize; // xy: Offset/Pan in pixels, zw: Canvas size in pixels
    float4 u_ChannelMasksAndFlags; // x: R active, y: G active, z: B active, w: A active (1.0f or 0.0f)
    float4 u_ViewportFlags; // x: flipH, y: flipV, zw: unused
};

cbuffer LayerBuffer : register(b1)
{
    // x: opacity, y: hasMask, z: firstLayer (1=init buffer, keep RGB if A=0), w: uOff when floating else 0
    // When isFloating, zw = translation; firstLayer is only used when not floating.
    float4 u_LayerParams;
    // x: scaleX, y: scaleY, z: rotation, w: isFloating (1) or fillTexture (2)
    float4 u_TransformParams;
    // x,y: center; z: blendMode; w: alphaRewrite (1=overwrite A, 0=A is RGB strength)
    float4 u_CenterParams;
    // Floating tex rect in document UV: xy=origin, zw=size (0 size = full-doc legacy)
    // When fillTexture mode (transform.w > 1.5): xy=texScale, zw=texOffset
    float4 u_FloatRect;
    // Fill texture tint (RGBA). Used when transform.w > 1.5 (fill pattern sample).
    float4 u_FillColor;
};

Texture2D g_Texture : register(t0);
SamplerState g_Sampler : register(s0);

PS_INPUT VSMain(VS_INPUT input)
{
    PS_INPUT output;
    
    float2 viewportSize = u_ViewportSizeAndZoom.xy;
    float zoom = u_ViewportSizeAndZoom.z;
    float rotation = u_ViewportSizeAndZoom.w;
    float2 panOffset = u_OffsetAndCanvasSize.xy;
    float2 canvasSize = u_OffsetAndCanvasSize.zw;
    
    // Map unit quad position (0 to 1) to canvas pixel size
    float2 canvasPixelPos = input.pos * canvasSize;
    
    // Apply viewport flips
    if (u_ViewportFlags.x > 0.5f) {
        canvasPixelPos.x = canvasSize.x - canvasPixelPos.x;
    }
    if (u_ViewportFlags.y > 0.5f) {
        canvasPixelPos.y = canvasSize.y - canvasPixelPos.y;
    }
    
    // Rotate canvasPixelPos around canvas center (canvasSize * 0.5f)
    float2 center = canvasSize * 0.5f;
    float2 rel = canvasPixelPos - center;
    float cosA = cos(rotation);
    float sinA = sin(rotation);
    float2 rotatedPixelPos;
    rotatedPixelPos.x = rel.x * cosA - rel.y * sinA;
    rotatedPixelPos.y = rel.x * sinA + rel.y * cosA;
    rotatedPixelPos += center;
    
    // Apply zoom, translation, and center relative to viewport with integer pixel alignment
    float2 screenOrigin = floor(panOffset + viewportSize * 0.5f);
    float2 screenPixelPos = floor(rotatedPixelPos * zoom) + screenOrigin;
    
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
    
    bool r = u_ChannelMasksAndFlags.x > 0.5f;
    bool g = u_ChannelMasksAndFlags.y > 0.5f;
    bool b = u_ChannelMasksAndFlags.z > 0.5f;
    bool a = u_ChannelMasksAndFlags.w > 0.5f;
    
    float3 finalColor = checkColor;
    
    int activeCount = 0;
    if (r) activeCount++;
    if (g) activeCount++;
    if (b) activeCount++;
    if (a) activeCount++;
    
    if (activeCount == 1)
    {
        float val = 0.0f;
        if (r) val = texCol.r;
        else if (g) val = texCol.g;
        else if (b) val = texCol.b;
        else if (a) val = texCol.a;
        finalColor = float3(val, val, val);
    }
    else
    {
        float3 rgb = float3(r ? texCol.r : 0.0f, g ? texCol.g : 0.0f, b ? texCol.b : 0.0f);
        if (a)
        {
            // Alpha channel ON: real transparency over checker (A=0 → checker only).
            finalColor = lerp(checkColor, rgb, saturate(texCol.a));
        }
        else
        {
            // Alpha channel OFF: pure RGB buffer view (ignore coverage).
            finalColor = rgb;
        }
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

Texture2D g_LayerMask : register(t1);
Texture2D g_Composite  : register(t2); // current composite for blend modes

// Simple pixel shader to output layer contents multiplied by layer opacity
float4 PSLayerBlend(PS_INPUT input) : SV_TARGET
{
    float2 uv = input.uv;
    // Fill texture mode: sample pattern at source res with wrap — no full-doc bake.
    const bool fillTexMode = (u_TransformParams.w > 1.5f);
    
    // Check if we are drawing floating pixels (w ≈ 1, not fillTex 2)
    if (!fillTexMode && u_TransformParams.w > 0.5f)
    {
        // Center of transformation in UV space
        float2 center = u_CenterParams.xy;
        float2 rel = uv - center;
        
        // Inverse translation (translation is in UV space)
        rel -= u_LayerParams.zw;
        
        // Inverse rotation (angle is u_TransformParams.z, inverse is -z)
        float angle = -u_TransformParams.z;
        float cosA = cos(angle);
        float sinA = sin(angle);
        float2 rotated;
        rotated.x = rel.x * cosA - rel.y * sinA;
        rotated.y = rel.x * sinA + rel.y * cosA;
        
        // Inverse scale (scale is u_TransformParams.xy)
        float2 scale = u_TransformParams.xy;
        if (scale.x > 0.0001f && scale.y > 0.0001f)
        {
            rotated /= scale;
        }
        
        uv = rotated + center;

        // Tight floating texture: map document UV → local floating UV
        if (u_FloatRect.z > 1e-6f && u_FloatRect.w > 1e-6f)
        {
            float2 fuv = (uv - u_FloatRect.xy) / u_FloatRect.zw;
            if (fuv.x < 0.0f || fuv.x > 1.0f || fuv.y < 0.0f || fuv.y > 1.0f)
            {
                discard;
                return float4(0.0f, 0.0f, 0.0f, 0.0f);
            }
            uv = fuv;
        }
        else if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
        {
            discard;
            return float4(0.0f, 0.0f, 0.0f, 0.0f);
        }
    }
    else if (!fillTexMode)
    {
        // Normal layer offset translation
        uv -= u_LayerParams.zw;

        if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)
        {
            discard;
            return float4(0.0f, 0.0f, 0.0f, 0.0f);
        }
    }

    float4 col;
    if (fillTexMode)
    {
        // Document UV → tiled pattern UV (wrap via frac)
        float2 tuv = input.uv * u_FloatRect.xy + u_FloatRect.zw;
        tuv = tuv - floor(tuv);
        float4 tex = g_Texture.Sample(g_Sampler, tuv);
        col = tex * u_FillColor;
    }
    else
    {
        col = g_Texture.Sample(g_Sampler, uv);
    }

    // Pixel A * layer opacity [* mask] = coverage / morph strength for RGB.
    // Alpha Rewrite OFF: blend state keeps dest A; col.a is strength only.
    // firstLayer (centerParams.w >= 2): straight RGBA init — RGB kept if A=0.
    float opacity = u_LayerParams.x;
    float flags = u_CenterParams.w;
    bool firstLayer = (flags >= 1.5f);

    if (u_LayerParams.y > 0.5f)
    {
        // Mask always in document UV for fill-texture; content UV otherwise
        float2 maskUv = fillTexMode ? input.uv : uv;
        float maskVal = g_LayerMask.Sample(g_Sampler, maskUv).r;
        col.a *= maskVal;
    }

    if (firstLayer)
    {
        // Bottom layer: write RGB as-is; A = pixelA * opacity (may be 0).
        col.a *= opacity;
        // ONE/ZERO blend → composite stores full buffer (RGB + A).
        return col;
    }

    col.a *= opacity; // coverage / strength for subsequent layers

    // Blend mode (u_CenterParams.z)
    uint blendMode = (uint)(u_CenterParams.z + 0.5f);
    if (blendMode > 0u)
    {
        float4 dst = g_Composite.Sample(g_Sampler, input.uv); // existing composite
        float3 s = col.rgb;
        float3 d = dst.rgb;
        float3 result = s; // default = Normal

        if (blendMode == 1u) // Multiply
            result = s * d;
        else if (blendMode == 2u) // Screen
            result = 1.0f - (1.0f - s) * (1.0f - d);
        else if (blendMode == 3u) // Overlay
        {
            result.r = (d.r < 0.5f) ? 2.0f*s.r*d.r : 1.0f - 2.0f*(1.0f-s.r)*(1.0f-d.r);
            result.g = (d.g < 0.5f) ? 2.0f*s.g*d.g : 1.0f - 2.0f*(1.0f-s.g)*(1.0f-d.g);
            result.b = (d.b < 0.5f) ? 2.0f*s.b*d.b : 1.0f - 2.0f*(1.0f-s.b)*(1.0f-d.b);
        }
        else if (blendMode == 4u) // Add
            result = min(s + d, 1.0f);
        else if (blendMode == 5u) // Subtract
            result = max(d - s, 0.0f);
        else if (blendMode == 6u) // Darken
            result = min(s, d);
        else if (blendMode == 7u) // Lighten
            result = max(s, d);
        else if (blendMode == 8u) // HardLight
        {
            result.r = (s.r < 0.5f) ? 2.0f*s.r*d.r : 1.0f - 2.0f*(1.0f-s.r)*(1.0f-d.r);
            result.g = (s.g < 0.5f) ? 2.0f*s.g*d.g : 1.0f - 2.0f*(1.0f-s.g)*(1.0f-d.g);
            result.b = (s.b < 0.5f) ? 2.0f*s.b*d.b : 1.0f - 2.0f*(1.0f-s.b)*(1.0f-d.b);
        }
        else if (blendMode == 9u) // SoftLight
        {
            result.r = (s.r < 0.5f) ? d.r - (1.0f-2.0f*s.r)*d.r*(1.0f-d.r) : d.r + (2.0f*s.r-1.0f)*(sqrt(d.r)-d.r);
            result.g = (s.g < 0.5f) ? d.g - (1.0f-2.0f*s.g)*d.g*(1.0f-d.g) : d.g + (2.0f*s.g-1.0f)*(sqrt(d.g)-d.g);
            result.b = (s.b < 0.5f) ? d.b - (1.0f-2.0f*s.b)*d.b*(1.0f-d.b) : d.b + (2.0f*s.b-1.0f)*(sqrt(d.b)-d.b);
        }

        // Compose: replace col.rgb with blended result, keep alpha for D3D blending
        col.rgb = result;
    }

    return col;
}


Texture2D g_SelectionMask : register(t1);

float4 PSSelectionOutline(PS_INPUT input) : SV_TARGET
{
    float mask = g_SelectionMask.Sample(g_Sampler, input.uv).r;
    
    float2 canvasSize = u_OffsetAndCanvasSize.zw;
    float zoom = u_ViewportSizeAndZoom.z;
    // One canvas texel in UV (not screen-pixel): edge detect in document space
    float2 uvStep = 1.0f / max(canvasSize, float2(1.0f, 1.0f));
    
    float mLeft  = g_SelectionMask.Sample(g_Sampler, input.uv - float2(uvStep.x, 0.0f)).r;
    float mRight = g_SelectionMask.Sample(g_Sampler, input.uv + float2(uvStep.x, 0.0f)).r;
    float mUp    = g_SelectionMask.Sample(g_Sampler, input.uv - float2(0.0f, uvStep.y)).r;
    float mDown  = g_SelectionMask.Sample(g_Sampler, input.uv + float2(0.0f, uvStep.y)).r;
    
    // Interior mask edges (selected | unselected)
    bool edge = abs(mask - mLeft) > 0.1f || abs(mask - mRight) > 0.1f
             || abs(mask - mUp) > 0.1f || abs(mask - mDown) > 0.1f;

    // Document border: if selection touches canvas edge, draw ants there too.
    // Without this, a selection flush with the right/bottom edge has no visible
    // outline on that side (no "outside" neighbor inside the texture).
    bool onDocBorder = (input.uv.x <= uvStep.x * 1.5f) || (input.uv.x >= 1.0f - uvStep.x * 1.5f)
                    || (input.uv.y <= uvStep.y * 1.5f) || (input.uv.y >= 1.0f - uvStep.y * 1.5f);
    if (mask > 0.5f && onDocBorder)
        edge = true;

    if (edge && mask > 0.05f)
    {
        float time = u_ViewportFlags.z;
        float dash = (input.screenPos.x + input.screenPos.y - time * 30.0f) % 10.0f;
        if (dash < 0.0f) dash += 10.0f;
        float3 col = (dash < 5.0f) ? float3(0.0f, 0.0f, 0.0f) : float3(1.0f, 1.0f, 1.0f);
        return float4(col, 1.0f);
    }
    
    discard;
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
}
