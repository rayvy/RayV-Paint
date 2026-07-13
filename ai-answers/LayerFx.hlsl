// LayerFx.hlsl — GPU Driven pixel/filter/style shaders
// Replaces CPU per-pixel loops in LayerStyles.cpp + Canvas.cpp
// Target: ps_5_0 (D3D11 feature level 11_0 for UAV writes; fallback to ps_4_0 RT path)
//
// Each filter is a separate pixel shader entry point.
// Composition is done via D3D11 blend states (no extra shader).
// For "compute-like" read-modify-write, use two-pass ping-pong on render targets.

SamplerState LinearClampSampler : register(s0);
SamplerState LinearWrapSampler  : register(s1);
SamplerState PointClampSampler  : register(s2);

Texture2D<float4> InputTex      : register(t0);  // source layer content (RGBA float)
Texture2D<float4> SrcOverTex    : register(t1);  // second source for composite ops
Texture2D<uint>   MaskTex       : register(t2);  // R8 mask (0..255)
Texture2D<float4> LutTex        : register(t3);  // 256x1 curves LUT
Texture2D<float4> GradientTex   : register(t4);  // outline gradient fill
Texture1D<float4> Lut1D         : register(t5);  // 256x1 per-channel LUT
Texture2D<float4> NoiseTex      : register(t6);  // pre-baked noise texture

cbuffer FilterParams : register(b0)
{
    float4 Params;       // x,y,z,w = filter-specific params
    // HSV:     x= HueShift, y= SatShift, z= ValShift
    // Blur:    x= unused (radius set via cbuffer)
    // Curves:  x= channelsMask (R=1,G=2,B=4,A=8 packed as float bits)
    // Noise:   x= strength, y= colorNoise (0/1)
    // Mask:    x= fillOpacity
    // Invert:  x= channelMask (1=R, 2=G, 4=B, 8=A)
    float2 TexScale;     // fill texture scale
    float2 TexOffset;    // fill texture offset
    float  SelectionWeight; // 0 = no selection, 1 = full selection
    float  Pad1;
};

cbuffer BlurParams : register(b1)
{
    int   BlurRadius;
    int   BlurPass;     // 0=H, 1=V
    int   BlurChannelCount; // 1 or 4
    float BlurPad;
};

// ============================================================
//  Utility: RGB <-> HSV (same logic as CPU, branchless where possible)
// ============================================================

static float3 RGBtoHSV(float3 c) {
    float mx = max(c.r, max(c.g, c.b));
    float mn = min(c.r, min(c.g, c.b));
    float v = mx;
    float d = mx - mn;
    float s = (mx > 1e-6) ? d / mx : 0.0;

    float h = 0.0;
    if (d > 1e-6) {
        float dr = c.r == mx ? (c.g - c.b) / d : 0.0;
        float dg = c.g == mx ? (c.b - c.r) / d + 2.0 : 0.0;
        float db = c.b == mx ? (c.r - c.g) / d + 4.0 : 0.0;
        h = (c.r == mx ? dr : (c.g == mx ? dg : db));
        if (c.r == mx && c.g < c.b) h += 6.0;
        h /= 6.0;
    }
    return float3(h, s, v);
}

static float3 HSVtoRGB(float3 hsv) {
    float h = hsv.x, s = hsv.y, v = hsv.z;
    if (s < 1e-6) return float3(v, v, v);

    h = fmod(h, 1.0);
    if (h < 0.0) h += 1.0;
    float hh = h * 6.0;
    float i = floor(hh);
    float f = hh - i;
    float p = v * (1.0 - s);
    float q = v * (1.0 - s * f);
    float t = v * (1.0 - s * (1.0 - f));

    // Branchless selection via step/lerp
    float3 rgb;
    float ii = fmod(i, 6.0);
    // case 0: r=v, g=t, b=p
    // case 1: r=q, g=v, b=p
    // case 2: r=p, g=v, b=t
    // case 3: r=p, g=q, b=v
    // case 4: r=t, g=p, b=v
    // default (5): r=v, g=p, b=q

    float s0 = step(ii, 0.5); // case 0
    float s1 = step(ii, 1.5) * step(0.5, ii); // case 1
    float s2 = step(ii, 2.5) * step(1.5, ii); // case 2
    float s3 = step(ii, 3.5) * step(2.5, ii); // case 3
    float s4 = step(ii, 4.5) * step(3.5, ii); // case 4

    rgb.r = v * (s0 + (1-s0)*(1-s4)) + q * s1 * (1-s0) + p * (s2+s3) * (1-s0) + t * s4;
    rgb.g = t * s0 + v * (s1 + s2) * (1-s0-s4) + q * s3 + p * s4;
    rgb.b = p * (s0 + s1) + t * s2 + v * (s3 + s4);

    // Simpler: just use the classic switch via step ladder
    // The above is getting complex. Use the straightforward approach:
    rgb = float3(v, t, p) * s0
        + float3(q, v, p) * s1
        + float3(p, v, t) * s2
        + float3(p, q, v) * s3
        + float3(t, p, v) * s4
        + float3(v, p, q) * (1.0 - saturate(ii));

    return rgb;
}

// ============================================================
//  VS: simple fullscreen triangle (no VB needed)
// ============================================================

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vertexID : SV_VertexID) {
    // Fullscreen triangle from 3 vertices
    float2 uv = float2(
        (vertexID << 1) & 2,
        vertexID & 2
    );
    VSOut o;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv  = uv;
    return o;
}

// ============================================================
//  PS: HSV Filter
//  Params.x = HueShift, .y = SatShift, .z = ValShift
// ============================================================

float4 PS_HSV(VSOut i) : SV_Target {
    float4 c = InputTex.Sample(LinearClampSampler, i.uv);
    float3 hsv = RGBtoHSV(c.rgb);
    hsv.x = fmod(hsv.x + Params.x + 1.0, 1.0);
    hsv.y = saturate(hsv.y + Params.y);
    hsv.z = saturate(hsv.z + Params.z);
    c.rgb = HSVtoRGB(hsv);
    return c;
}

// ============================================================
//  PS: HSV with Selection Blend
//  SelectionWeight from cbuffer (uniform for adjust preview)
//  For per-pixel selection, use PS_HSV_SelectionMask below
// ============================================================

float4 PS_HSV_Selection(VSOut i) : SV_Target {
    float4 c = InputTex.Sample(LinearClampSampler, i.uv);
    float3 hsv = RGBtoHSV(c.rgb);
    hsv.x = fmod(hsv.x + Params.x + 1.0, 1.0);
    hsv.y = saturate(hsv.y + Params.y);
    hsv.z = saturate(hsv.z + Params.z);
    float3 filtered = HSVtoRGB(hsv);
    float sel = SelectionWeight;
    // Per-pixel selection mask (optional)
    if (MaskTex.Width > 0) {
        float maskVal = MaskTex.Sample(PointClampSampler, i.uv).r / 255.0;
        // Smooth selection (antialiased edges)
        sel *= smoothstep(0.4, 0.6, maskVal);
    }
    c.rgb = c.rgb * (1.0 - sel) + filtered.rgb * sel;
    return c;
}

// ============================================================
//  PS: Curves LUT
//  Params.x = channel mask (1=R, 2=G, 4=B, 8=A) packed
// ============================================================

float SampleLUT(float v) {
    return Lut1D.SampleLevel(LinearClampSampler, v, 0).r;
}

float4 PS_Curves(VSOut i) : SV_Target {
    float4 c = InputTex.Sample(LinearClampSampler, i.uv);
    int mask = (int)Params.x;

    float sel = SelectionWeight;
    if (MaskTex.Width > 0) {
        float maskVal = MaskTex.Sample(PointClampSampler, i.uv).r / 255.0;
        sel *= smoothstep(0.4, 0.6, maskVal);
    }

    if (sel < 1e-4) return c;

    float4 filtered = c;
    if (mask & 1) filtered.r = SampleLUT(c.r);
    if (mask & 2) filtered.g = SampleLUT(c.g);
    if (mask & 4) filtered.b = SampleLUT(c.b);
    if (mask & 8) filtered.a = SampleLUT(c.a);

    return float4(
        c.rgb * (1.0 - sel) + filtered.rgb * sel,
        lerp(c.a, filtered.a, sel)
    );
}

// ============================================================
//  PS: Noise
//  Params.x = strength, .y = colorNoise (0=grayscale, 1=color)
// ============================================================

float4 PS_Noise(VSOut i) : SV_Target {
    float4 c = InputTex.Sample(LinearClampSampler, i.uv);
    float4 n = NoiseTex.Sample(LinearWrapSampler, i.uv * 13.37); // pseudo-random lookup

    float sel = SelectionWeight;
    if (MaskTex.Width > 0) {
        float maskVal = MaskTex.Sample(PointClampSampler, i.uv).r / 255.0;
        sel *= smoothstep(0.4, 0.6, maskVal);
    }
    if (sel < 1e-4) return c;

    float strength = Params.x * sel;
    if (Params.y > 0.5) {
        // Color noise
        c.rgb += (n.rgb * 2.0 - 1.0) * strength;
    } else {
        // Grayscale noise
        float gn = (n.r * 2.0 - 1.0) * strength;
        c.rgb += gn;
    }
    return saturate(c);
}

// ============================================================
//  PS: Alpha Invert
// ============================================================

float4 PS_AlphaInvert(VSOut i) : SV_Target {
    float4 c = InputTex.Sample(LinearClampSampler, i.uv);
    c.a = 1.0 - c.a;
    return c;
}

// ============================================================
//  PS: Invert Colors (RGB only)
// ============================================================

float4 PS_InvertColors(VSOut i) : SV_Target {
    float4 c = InputTex.Sample(LinearClampSampler, i.uv);
    float sel = SelectionWeight;
    if (MaskTex.Width > 0) {
        float maskVal = MaskTex.Sample(PointClampSampler, i.uv).r / 255.0;
        sel *= smoothstep(0.4, 0.6, maskVal);
    }
    float3 inv = 1.0 - c.rgb;
    c.rgb = c.rgb * (1.0 - sel) + inv * sel;
    return c;
}

// ============================================================
//  PS: Apply Mask to Alpha
//  Params.x = fillOpacity
// ============================================================

float4 PS_MaskAlpha(VSOut i) : SV_Target {
    float4 c = InputTex.Sample(LinearClampSampler, i.uv);
    float mask = 1.0;
    if (MaskTex.Width > 0) {
        mask = MaskTex.Sample(PointClampSampler, i.uv).r / 255.0;
    }
    c.a = saturate(c.a * Params.x * mask);
    return c;
}

// ============================================================
//  PS: Box Blur (Separable — Horizontal or Vertical)
//  Used with ping-pong: H pass -> V pass -> repeat
//  BlurRadius, BlurPass (0=H,1=V), BlurChannelCount in cbuffer
// ============================================================

float4 PS_BoxBlurH(VSOut i) : SV_Target {
    // Pixel size
    float2 texelSize = float2(1.0 / InputTex.Width, 1.0 / InputTex.Height);
    float2 uv = i.uv;

    float4 acc = 0;
    float count = 0;
    int r = BlurRadius;

    [unroll]
    for (int k = -r; k <= r; k++) {
        float2 offset = float2(k * texelSize.x, 0);
        acc += InputTex.Sample(LinearClampSampler, uv + offset);
        count += 1.0;
    }

    return acc / count;
}

float4 PS_BoxBlurV(VSOut i) : SV_Target {
    float2 texelSize = float2(1.0 / InputTex.Width, 1.0 / InputTex.Height);
    float2 uv = i.uv;

    float4 acc = 0;
    float count = 0;
    int r = BlurRadius;

    [unroll]
    for (int k = -r; k <= r; k++) {
        float2 offset = float2(0, k * texelSize.y);
        acc += InputTex.Sample(LinearClampSampler, uv + offset);
        count += 1.0;
    }

    return acc / count;
}

// ============================================================
//  PS: Fill Layer (solid or texture × color)
//  Params = fill color RGBA
//  TexScale/TexOffset = texture tiling
//  GradientTex = optional fill texture
// ============================================================

float4 PS_FillSolid(VSOut i) : SV_Target {
    // Solid color from cbuffer
    return float4(Params.rgb, Params.w);
}

float4 PS_FillTexture(VSOut i) : SV_Target {
    float2 uv = i.uv * TexScale + TexOffset;
    float4 texel = InputTex.Sample(LinearWrapSampler, uv);
    float4 tint = float4(Params.rgb, Params.w);
    return texel * tint;
}

// ============================================================
//  PS: Extract Alpha (writes alpha to RGB, A=1)
//  Used for shadow/outline silhouette generation
// ============================================================

float4 PS_ExtractAlpha(VSOut i) : SV_Target {
    float4 c = InputTex.Sample(LinearClampSampler, i.uv);
    return float4(c.a, c.a, c.a, 1.0);
}

// ============================================================
//  PS: Apply Spread (power curve on alpha channel)
//  Params.x = spread (0..1)
// ============================================================

float4 PS_Spread(VSOut i) : SV_Target {
    float4 c = InputTex.Sample(LinearClampSampler, i.uv);
    float spread = Params.x;
    if (spread <= 0 || c.r <= 0) return c;
    float exponent = 1.0 - spread * 0.85;
    float a = pow(saturate(c.r), exponent);
    return float4(a, a, a, 1.0);
}

// ============================================================
//  PS: Offset (shift texture by dx,dy pixels)
//  Params.xy = offset in pixels
// ============================================================

float4 PS_Offset(VSOut i) : SV_Target {
    float2 texelSize = float2(1.0 / InputTex.Width, 1.0 / InputTex.Height);
    float2 offsetUV = i.uv - Params.xy * texelSize;
    return InputTex.Sample(LinearClampSampler, offsetUV);
}

// ============================================================
//  PS: Dilate (max filter, separable H)
//  BlurRadius = dilation radius
// ============================================================

float4 PS_DilateH(VSOut i) : SV_Target {
    float texelW = 1.0 / InputTex.Width;
    float2 uv = i.uv;
    float m = 0;
    int r = BlurRadius;
    [unroll]
    for (int k = -r; k <= r; k++) {
        float s = InputTex.Sample(LinearClampSampler, uv + float2(k * texelW, 0)).r;
        m = max(m, s);
    }
    return float4(m, m, m, 1.0);
}

float4 PS_DilateV(VSOut i) : SV_Target {
    float texelH = 1.0 / InputTex.Height;
    float2 uv = i.uv;
    float m = 0;
    int r = BlurRadius;
    [unroll]
    for (int k = -r; k <= r; k++) {
        float s = InputTex.Sample(LinearClampSampler, uv + float2(0, k * texelH)).r;
        m = max(m, s);
    }
    return float4(m, m, m, 1.0);
}

// ============================================================
//  PS: Erode (min filter, separable H)
// ============================================================

float4 PS_ErodeH(VSOut i) : SV_Target {
    float texelW = 1.0 / InputTex.Width;
    float2 uv = i.uv;
    float m = 1;
    int r = BlurRadius;
    [unroll]
    for (int k = -r; k <= r; k++) {
        float s = InputTex.Sample(LinearClampSampler, uv + float2(k * texelW, 0)).r;
        m = min(m, s);
    }
    return float4(m, m, m, 1.0);
}

float4 PS_ErodeV(VSOut i) : SV_Target {
    float texelH = 1.0 / InputTex.Height;
    float2 uv = i.uv;
    float m = 1;
    int r = BlurRadius;
    [unroll]
    for (int k = -r; k <= r; k++) {
        float s = InputTex.Sample(LinearClampSampler, uv + float2(0, k * texelH)).r;
        m = min(m, s);
    }
    return float4(m, m, m, 1.0);
}

// ============================================================
//  PS: Shadow Colorize
//  Params.rgb = shadow color, Params.w = shadow opacity
//  InputTex = blurred alpha silhouette (R channel)
// ============================================================

float4 PS_ShadowColorize(VSOut i) : SV_Target {
    float alpha = InputTex.Sample(LinearClampSampler, i.uv).r;
    float a = saturate(alpha * Params.w);
    return float4(Params.rgb, a);
}

// ============================================================
//  PS: Outline Ring (dilated - eroded / dilated - original)
//  InputTex = ring strength (R), SrcOverTex = unused
//  Params.rgb = outline color, Params.w = outline opacity
// ============================================================

float4 PS_OutlineSolid(VSOut i) : SV_Target {
    float ring = InputTex.Sample(LinearClampSampler, i.uv).r;
    if (ring < 1e-5) return 0;
    float a = saturate(ring * Params.w);
    return float4(Params.rgb, a);
}

// ============================================================
//  PS: Outline with Gradient Fill
//  Params.xy = gradient mapping type (ignored — UV-based)
//  GradientTex = gradient texture (1xN or Nx1)
// ============================================================

float4 PS_OutlineGradient(VSOut i) : SV_Target {
    float ring = InputTex.Sample(LinearClampSampler, i.uv).r;
    if (ring < 1e-5) return 0;

    // Gradient parameter t based on position
    float t = ring; // distance-from-edge (default)
    float4 gc = GradientTex.Sample(LinearClampSampler, float2(t, 0.5));

    float a = saturate(ring * gc.a * Params.w);
    return float4(gc.rgb, a);
}

// ============================================================
//  PS: Outline with Texture Fill
//  InputTex = ring strength, t3 = fill texture
// ============================================================

float4 PS_OutlineTexture(VSOut i) : SV_Target {
    float ring = InputTex.Sample(LinearClampSampler, i.uv).r;
    if (ring < 1e-5) return 0;

    float2 uv = i.uv * TexScale + TexOffset;
    float4 tc = InputTex.Sample(LinearWrapSampler, uv);
    float4 tint = float4(Params.rgb, Params.w);

    float a = saturate(ring * tc.a * tint.a);
    return float4(tc.rgb * tint.rgb, a);
}

// ============================================================
//  PS: Subtract (for ring computation: dilated - eroded)
//  InputTex = first operand, SrcOverTex = second operand
//  Writes max(0, Input - Src) to R channel
// ============================================================

float4 PS_Subtract(VSOut i) : SV_Target {
    float a = InputTex.Sample(LinearClampSampler, i.uv).r;
    float b = SrcOverTex.Sample(LinearClampSampler, i.uv).r;
    float d = max(0.0, a - b);
    return float4(d, d, d, 1.0);
}

// ============================================================
//  PS: Nearest-neighbor upscale (for proxy -> full-res)
//  InputTex = small source, UVs mapped by VS
// ============================================================

float4 PS_NearestUpscale(VSOut i) : SV_Target {
    return InputTex.Sample(PointClampSampler, i.uv);
}