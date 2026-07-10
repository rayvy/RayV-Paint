// =============================================================================
// RayV-Paint — ZZZ outline pass (adapted for preview, not a 1:1 game dump)
// =============================================================================
// Reference: research/zzz-materials/6883e4375b728e90-vs + 89ea088388fc8cd1-ps
//
// ZZZ: outline direction packed in TEXCOORD1.xy (tangent-space),
//      Z = sqrt(1 - |xy|^2), expanded in model space, Cull Front.
// GI: different packing (often TANGENT) — do NOT use this pass for GI.
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
};

cbuffer OutlineCB : register(b1)
{
    // x=thickness, y=albedoMul, z=useVertexColorG (1/0), w=unused
    float4 outlineParams;
    // xyz=tint when w>0.5, else albedo * mul
    float4 outlineTint;
};

Texture2D texDiffuse : register(t0);
SamplerState sampLin : register(s0);

struct VSIn
{
    float3 position  : POSITION;
    float3 normal    : NORMAL;
    float4 tangent   : TANGENT;
    float4 color     : COLOR;
    float2 uv0       : TEXCOORD0;
    float2 uvLight   : TEXCOORD1;
    float2 uvOutline : TEXCOORD2; // UV_Outline role ← dump TEXCOORD1
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

    // Reconstruct outline normal from ZZZ TEXCOORD1 packing (model space)
    float2 pack = i.uvOutline.xy;
    float len2 = min(1.0, dot(pack, pack));
    float pz = sqrt(max(0.0, 1.0 - len2));

    float3 N = normalize(i.normal);
    float3 T = normalize(i.tangent.xyz);
    T = normalize(T - N * dot(N, T));
    float signB = (i.tangent.w < 0.0) ? -1.0 : 1.0;
    float3 B = cross(N, T) * signB;

    // Dump order: v5.x * tan + v5.y * bitan + sqrt * normal
    float3 outlineN = normalize(pack.x * T + pack.y * B + pz * N);
    if (dot(pack, pack) < 1e-12)
        outlineN = N;

    float thick = outlineParams.x;
    // Game uses COLOR.y near 0.5 as thickness scale
    if (outlineParams.z > 0.5)
        thick *= max(0.05, saturate(i.color.g * 2.0));

    float3 posExp = i.position + outlineN * thick;
    o.pos = mul(float4(posExp, 1.0), worldViewProj);
    return o;
}

float4 PSOutlineZZZ(VSOut i) : SV_Target
{
    float3 albedo = texDiffuse.Sample(sampLin, i.uv0).rgb;
    if (dot(albedo, 1.0) < 0.001)
        albedo = float3(0.15, 0.15, 0.17);

    float3 col = (outlineTint.w > 0.5)
        ? outlineTint.rgb
        : albedo * outlineParams.y;

    return float4(saturate(col), 1.0);
}
