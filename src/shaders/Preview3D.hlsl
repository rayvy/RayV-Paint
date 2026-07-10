// RayV-Paint unified character preview (MVP unlit + basic lambert)
// Roles already decoded on CPU into this vertex layout.

cbuffer PreviewCB : register(b0)
{
    float4x4 worldViewProj;
    float4x4 world;
    float4   lightDirIntensity; // xyz dir, w intensity
    float4   ambientColor;      // rgb + unused
    float4   debugMode;         // x: 0=shaded 1=uv0 2=normal 3=color 4=outlineUV
};

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
    float4 pos       : SV_POSITION;
    float3 nrm       : NORMAL;
    float4 color     : COLOR;
    float2 uv0       : TEXCOORD0;
    float2 uvLight   : TEXCOORD1;
    float2 uvOutline : TEXCOORD2;
};

Texture2D    texDiffuse : register(t0);
SamplerState sampLinear : register(s0);

VSOut VSMain(VSIn i)
{
    VSOut o;
    o.pos = mul(float4(i.position, 1.0), worldViewProj);
    float3 n = mul(float4(i.normal, 0.0), world).xyz;
    o.nrm = normalize(n);
    o.color = i.color;
    o.uv0 = i.uv0;
    o.uvLight = i.uvLight;
    o.uvOutline = i.uvOutline;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float mode = debugMode.x;
    if (mode > 0.5 && mode < 1.5)
        return float4(frac(i.uv0), 0.0, 1.0);
    if (mode > 1.5 && mode < 2.5)
        return float4(i.nrm * 0.5 + 0.5, 1.0);
    if (mode > 2.5 && mode < 3.5)
        return float4(i.color.rgb, 1.0);
    if (mode > 3.5 && mode < 4.5)
        return float4(frac(i.uvOutline) * 0.5 + 0.5, 0.0, 1.0);

    float4 albedo = texDiffuse.Sample(sampLinear, i.uv0);
    // If texture missing/bound black with alpha0 — fallback to vertex color-ish gray
    if (albedo.a < 0.001 && dot(albedo.rgb, 1) < 0.001)
        albedo = float4(0.65, 0.65, 0.68, 1.0);

    float3 L = normalize(-lightDirIntensity.xyz);
    float ndl = saturate(dot(normalize(i.nrm), L));
    float3 lit = albedo.rgb * (ambientColor.rgb + ndl * lightDirIntensity.w);
    return float4(lit, 1.0);
}
