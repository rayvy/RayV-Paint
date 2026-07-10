// =============================================================================
// RayV-Paint — ZZZ outline pass (preview-adapted)
// =============================================================================
// TEXCOORD1.xy = outline dir in tangent space; Z = sqrt(1-|xy|^2)
// Vertex COLOR.r = outline thickness scale (community pattern)
// Expand in **world space** after orientation, Cull Front (inverted hull)
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
    // x=baseThickness, y=albedoMul, z=useVertexColorR (1/0), w=minScale
    float4 outlineParams;
    // xyz=tint when w>0.5
    float4 outlineTint;
};

Texture2D texDiffuse : register(t0);
Texture2D texLight   : register(t2); // LightMap.R can be outline/shadow color config
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
    float2 uvL : TEXCOORD1;
};

VSOut VSOutlineZZZ(VSIn i)
{
    VSOut o;
    o.uv0 = i.uv0;
    o.uvL = i.uvLight;

    // TBN in model space
    float3 N = normalize(i.normal);
    float3 T = i.tangent.xyz;
    float tlen = length(T);
    if (tlen > 1e-6)
        T = T / tlen;
    else
        T = float3(1, 0, 0);
    T = normalize(T - N * dot(N, T));
    float signB = (i.tangent.w < 0.0) ? -1.0 : 1.0;
    float3 B = cross(N, T) * signB;

    // ZZZ outline pack (TEXCOORD1)
    float2 pack = i.uvOutline.xy;
    float len2 = saturate(dot(pack, pack));
    float pz = sqrt(max(0.0, 1.0 - len2));
    float3 outlineN_m = normalize(pack.x * T + pack.y * B + pz * N);
    if (len2 < 1e-8)
        outlineN_m = N;

    // Transform outline direction to world (orientation included in world matrix)
    float3 outlineN_w = normalize(mul(outlineN_m, (float3x3)worldInvTranspose));

    float thick = outlineParams.x;
    // Community: Vertex COLOR.r = outline thickness
    if (outlineParams.z > 0.5)
    {
        float vr = i.color.r;
        // Often ~0.5 mid; map to scale. Avoid zeroing outlines completely.
        float sc = lerp(outlineParams.w, 1.5, saturate(vr));
        thick *= sc;
    }

    float3 worldPos = mul(float4(i.position, 1.0), world).xyz;
    worldPos += outlineN_w * thick;

    // Clip from world position: wvp already = world*view*proj, so need viewProj only.
    // We pass worldViewProj = world*view*proj, so transform model pos normally after expand:
    // Re-expand in model space by inverse scale of direction:
    float3 posExp = i.position + outlineN_m * thick;
    o.pos = mul(float4(posExp, 1.0), worldViewProj);
    return o;
}

float4 PSOutlineZZZ(VSOut i) : SV_Target
{
    float3 albedo = texDiffuse.Sample(sampLin, i.uv0).rgb;
    if (dot(albedo, 1.0) < 0.001)
        albedo = float3(0.12, 0.12, 0.14);

    // LightMap.R often shadowramp / outline colour config — darken diffuse toward it
    float2 uvL = i.uvL;
    if (dot(uvL, uvL) < 1e-10)
        uvL = i.uv0;
    float ramp = texLight.Sample(sampLin, uvL).r;

    float3 col;
    if (outlineTint.w > 0.5)
        col = outlineTint.rgb;
    else
    {
        float mul = outlineParams.y;
        // Mix toward darkened diffuse; ramp can push outline tone
        col = albedo * mul * lerp(0.85, 1.15, ramp);
    }

    return float4(saturate(col), 1.0);
}
