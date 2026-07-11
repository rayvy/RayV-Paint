// =============================================================================
// RayV-Paint — ZZZ outline (view-space / screen-stable inverted hull)
// =============================================================================
// Game reference (6883e4375b728e90-vs):
//   - TEXCOORD1.xy → tangent-space outline normal, Z = sqrt(1-|xy|^2)
//   - Expand mostly in view XY (screen plane), thickness ~0.001–0.005 * depth
//   - COLOR.r scales thickness (cb4[168].z * v3.x in dump)
//   - Rasterize Cull Front
// Color: darkened diffuse (not pure black) — matches in-game soft ink lines
// =============================================================================

cbuffer FrameCB : register(b0)
{
    float4x4 worldViewProj;
    float4x4 world;
    float4x4 worldInvTranspose;
    float4   lightDirIntensity;
    float4   ambientColor;
    float4   cameraPos;
    float4   frameDebug;
    float4x4 view;
    float4x4 proj;
};

cbuffer OutlineCB : register(b1)
{
    // x = base thickness scale (UI ~0.5–3, multiplies view-space width)
    // y = albedo darken (0.25–0.55 looks game-like; 0.1 = pure black)
    // z = use vertex COLOR.r
    // w = min vertex scale
    float4 outlineParams;
    // xyz = fixed tint if w > 0.5
    float4 outlineTint;
};

Texture2D texDiffuse : register(t0);
Texture2D texLight   : register(t2);
SamplerState sampLin : register(s0);

struct VSIn
{
    float3 position  : POSITION;
    float3 normal    : NORMAL;
    float4 tangent   : TANGENT;
    float4 color     : COLOR;
    float2 uv0       : TEXCOORD0;
    float2 uvLight   : TEXCOORD1;
    float2 uvOutline : TEXCOORD2;
    float2 uvBack    : TEXCOORD3;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv0 : TEXCOORD0;
};

VSOut VSOutlineZZZ(VSIn i)
{
    VSOut o;
    o.uv0 = i.uv0;

    // --- Outline normal from TEXCOORD1 (model space TBN) ---
    float3 N = normalize(i.normal);
    float3 T = i.tangent.xyz;
    float tl = length(T);
    T = (tl > 1e-6) ? (T / tl) : float3(1, 0, 0);
    T = normalize(T - N * dot(N, T));
    float signB = (i.tangent.w < 0.0) ? -1.0 : 1.0;
    float3 B = cross(N, T) * signB;

    float2 pack = i.uvOutline.xy;
    float len2 = saturate(dot(pack, pack));
    float pz = sqrt(max(0.0, 1.0 - len2));
    // Dump: v5.x * tan + v5.y * bitan + sqrt * nrm
    float3 onM = normalize(pack.x * T + pack.y * B + pz * N);
    if (len2 < 1e-8)
        onM = N;

    // World
    float3 pW = mul(float4(i.position, 1.0), world).xyz;
    float3 nW = normalize(mul(onM, (float3x3)worldInvTranspose));

    // View space
    float4 pV = mul(float4(pW, 1.0), view);
    float3 nV = mul(nW, (float3x3)view);

    // Screen-plane direction (drop view-Z so width is silhouette, not toward camera)
    float2 nScr = nV.xy;
    float nLen = length(nScr);
    if (nLen > 1e-5)
        nScr /= nLen;
    else
    {
        // Degenerate: push along view-space normal XY fallback
        nScr = normalize(float2(nV.x, nV.y) + float2(1e-3, 0));
    }

    // Depth-aware thickness ≈ constant screen size (game uses ~1e-3..5e-3 at mid distance)
    float depth = max(0.15, abs(pV.z));
    float thick = outlineParams.x * 0.0022 * depth;

    // COLOR.r thickness (community + dump uses v3.x)
    if (outlineParams.z > 0.5)
    {
        float vr = saturate(i.color.r);
        // Mid greys (~0.5) keep scale ~1; dark verts thinner, bright slightly thicker
        thick *= lerp(outlineParams.w, 1.35, vr);
    }

    pV.xy += nScr * thick;

    // Optional tiny push away from camera so hull sits just outside silhouette
    // (helps hide z-fighting without fat 3D balloon expand)
    pV.z += depth * 0.00015;

    o.pos = mul(pV, proj);
    return o;
}

float4 PSOutlineZZZ(VSOut i) : SV_Target
{
    float3 albedo = texDiffuse.Sample(sampLin, i.uv0).rgb;
    if (dot(albedo, 1.0) < 0.001)
        albedo = float3(0.2, 0.2, 0.22);

    // LightMap.R = shadow / outline colour hint (UV0 atlas)
    float ramp = texLight.Sample(sampLin, i.uv0).r;

    float3 col;
    if (outlineTint.w > 0.5)
    {
        col = outlineTint.rgb;
    }
    else
    {
        // Game: soft ink = darkened diffuse, not pure black
        float mul = outlineParams.y; // ~0.35–0.5
        col = albedo * mul;
        // Slightly deepen using ramp (darker where shadow map is low)
        col *= lerp(0.75, 1.05, saturate(ramp));
        // Soft desaturate toward warm-dark ink
        float luma = dot(col, float3(0.299, 0.587, 0.114));
        col = lerp(col, luma.xxx * float3(0.95, 0.92, 0.9), 0.25);
    }

    return float4(saturate(col), 1.0);
}
