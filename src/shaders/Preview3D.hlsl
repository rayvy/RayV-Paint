// =============================================================================
// RayV-Paint — Unified character PREVIEW uber-shader
// =============================================================================
// NOT a 1:1 port of game frame dumps (research/zzz-materials is reference only).
// One PS/VS binary; material differences = cbuffer channel remaps + style knobs.
//
// Textures (fixed slots):
//   t0 Diffuse   — RGB always BaseColor
//   t1 NormalMap
//   t2 LightMap
//   t3 MaterialMap
//
// Channel packs (float4): mapIdx (-1=const→return .scale), swizzle(+10=invert),
//   scale, bias.  Sample: v = sample*scale+bias; if invert v=1-v.
// =============================================================================

cbuffer FrameCB : register(b0)
{
    float4x4 worldViewProj;
    float4x4 world;
    float4x4 worldInvTranspose;
    float4   lightDirIntensity; // xyz = light FROM dir, w = intensity
    float4   ambientColor;      // rgb ambient * intensity in w unused
    float4   cameraPos;         // xyz world camera
    float4   frameDebug;        // x = global debug mode
};

cbuffer MaterialCB : register(b1)
{
    float4 chOpacity;
    float4 chShadowMask;
    float4 chSpecular;
    float4 chMetallic;
    float4 chRoughness;
    float4 chAO;
    float4 chAniso;
    float4 chSss;
    float4 chGlow;
    float4 chRimMask;
    float4 style0; // toonThreshold, toonSoftness, toonShadowTint, normalStrength
    float4 style1; // rimStrength, rimPower, anisoStrength, anisoSharpness
    float4 style2; // sssStrength, metalF0, glowStrength, alphaCutoff
    float4 style3; // flags, debugMode, pad, pad
};

Texture2D texDiffuse  : register(t0);
Texture2D texNormal   : register(t1);
Texture2D texLight    : register(t2);
Texture2D texMaterial : register(t3);
SamplerState sampLin  : register(s0);

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
    float3 worldPos  : TEXCOORD4;
    float3 nrm       : NORMAL;
    float3 tng       : TANGENT;
    float3 btg       : BITANGENT;
    float4 color     : COLOR;
    float2 uv0       : TEXCOORD0;
    float2 uvLight   : TEXCOORD1;
    float2 uvOutline : TEXCOORD2;
};

float3 SampleMapRGB(int mapIdx, float2 uv)
{
    if (mapIdx <= 0) return texDiffuse.Sample(sampLin, uv).rgb;
    if (mapIdx == 1) return texNormal.Sample(sampLin, uv).rgb;
    if (mapIdx == 2) return texLight.Sample(sampLin, uv).rgb;
    return texMaterial.Sample(sampLin, uv).rgb;
}

float4 SampleMapRGBA(int mapIdx, float2 uv)
{
    if (mapIdx <= 0) return texDiffuse.Sample(sampLin, uv);
    if (mapIdx == 1) return texNormal.Sample(sampLin, uv);
    if (mapIdx == 2) return texLight.Sample(sampLin, uv);
    return texMaterial.Sample(sampLin, uv);
}

// Channel remap
float EvalChannel(float4 pack, float2 uv0, float2 uvL)
{
    float mapIdx = pack.x;
    float swz = pack.y;
    float scale = pack.z;
    float bias = pack.w;
    bool inv = false;
    if (swz >= 9.5)
    {
        inv = true;
        swz -= 10.0;
    }

    // Constant: mapIdx < 0 → value lives in scale
    if (mapIdx < -0.5)
        return scale;

    // ZZZ character materials: ALL maps authored on UV0 (primary atlas).
    // Sampling LightMap with TEXCOORD2 looked like "wrong DDS" (dark islands).
    // uvL kept in signature for future optional secondary-UV mode.
    float2 uv = uv0 + uvL * 0.0; // silence unused without C++ (void) cast

    float4 t = SampleMapRGBA((int)mapIdx, uv);
    float v = 0.0;
    int s = (int)swz;
    if (s == 0) v = t.r;
    else if (s == 1) v = t.g;
    else if (s == 2) v = t.b;
    else if (s == 3) v = t.a;
    else if (s == 4) v = dot(t.rgb, float3(0.299, 0.587, 0.114));
    else if (s == 5) v = 1.0;
    else v = 0.0;

    v = v * scale + bias;
    if (inv) v = 1.0 - v;
    return saturate(v);
}

float3 PerturbNormal(float3 N, float3 T, float3 B, float2 uv, float strength, bool rgOnly)
{
    float3 nrm = texNormal.Sample(sampLin, uv).xyz;
    float3 mapN;
    // ZZZ: R/G = normal XY, B = occlusion (not Z) when rgOnly
    if (rgOnly)
    {
        mapN.xy = nrm.xy * 2.0 - 1.0;
        mapN.z = sqrt(saturate(1.0 - dot(mapN.xy, mapN.xy)));
    }
    else
    {
        mapN = nrm * 2.0 - 1.0;
        if (nrm.z < 0.05 || nrm.z > 0.95)
            mapN.z = sqrt(saturate(1.0 - dot(mapN.xy, mapN.xy)));
    }
    mapN.xy *= strength;
    float3x3 tbn = float3x3(normalize(T), normalize(B), normalize(N));
    return normalize(mul(mapN, tbn));
}

VSOut VSMain(VSIn i)
{
    VSOut o;
    float4 wp = mul(float4(i.position, 1.0), world);
    o.worldPos = wp.xyz;
    o.pos = mul(float4(i.position, 1.0), worldViewProj);

    float3 N = normalize(mul(i.normal, (float3x3)worldInvTranspose));
    float3 T = normalize(mul(i.tangent.xyz, (float3x3)world));
    // Gram-Schmidt
    T = normalize(T - N * dot(N, T));
    float3 B = cross(N, T) * i.tangent.w;

    o.nrm = N;
    o.tng = T;
    o.btg = B;
    o.color = i.color;
    o.uv0 = i.uv0;
    o.uvLight = i.uvLight;
    o.uvOutline = i.uvOutline;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float dbg = max(frameDebug.x, style3.y);

    // Debug views
    if (dbg > 0.5 && dbg < 1.5)
        return float4(frac(i.uv0), 0.0, 1.0);
    if (dbg > 1.5 && dbg < 2.5)
        return float4(normalize(i.nrm) * 0.5 + 0.5, 1.0);
    if (dbg > 2.5 && dbg < 3.5)
        return float4(i.color.rgb, 1.0);
    if (dbg > 3.5 && dbg < 4.5)
        return float4(frac(i.uvOutline) * 0.5 + 0.5, 0.0, 1.0);

    float2 uv = i.uv0;
    float2 uvL = i.uvLight;

    float4 diffS = texDiffuse.Sample(sampLin, uv);
    float3 albedo = diffS.rgb;
    if (dot(albedo, 1.0) < 0.001 && diffS.a < 0.001)
        albedo = float3(0.65, 0.65, 0.68);

    float opacity = EvalChannel(chOpacity, uv, uvL);
    // Prefer diffuse alpha if opacity channel is constant 1 and diffuse has alpha use
    if (chOpacity.x < -0.5 && diffS.a < 0.999)
        opacity = diffS.a;

    float flags = style3.x;
    bool useN = fmod(flags, 2.0) >= 1.0;
    bool rgOnly = fmod(floor(flags * 0.5), 2.0) >= 1.0;
    bool alphaClip = fmod(floor(flags * 0.25), 2.0) >= 1.0;
    if (alphaClip && opacity < style2.w)
        discard;

    float3 N = normalize(i.nrm);
    if (useN)
        N = PerturbNormal(N, i.tng, i.btg, uv, style0.w, rgOnly);

    float shadowM = EvalChannel(chShadowMask, uv, uvL);
    float specM   = EvalChannel(chSpecular, uv, uvL);
    float metal   = EvalChannel(chMetallic, uv, uvL);
    float rough   = EvalChannel(chRoughness, uv, uvL);
    float ao      = EvalChannel(chAO, uv, uvL);
    float aniso   = EvalChannel(chAniso, uv, uvL);
    float sssM    = EvalChannel(chSss, uv, uvL);
    float glowM   = EvalChannel(chGlow, uv, uvL);
    float rimM    = EvalChannel(chRimMask, uv, uvL);

    if (dbg > 4.5 && dbg < 5.5) // shadow mask
        return float4(shadowM.xxx, 1);
    if (dbg > 5.5 && dbg < 6.5) // material pack viz
        return float4(metal, rough, ao, 1);
    if (dbg > 6.5 && dbg < 7.5) // lightmap raw — UV0 (same as diffuse atlas)
        return float4(texLight.Sample(sampLin, uv).rgb, 1);
    if (dbg > 7.5 && dbg < 8.5) // materialmap raw
        return float4(texMaterial.Sample(sampLin, uv).rgb, 1);
    if (dbg > 8.5 && dbg < 9.5) // lightmap with UV2 (diagnose secondary UV)
        return float4(texLight.Sample(sampLin, uvL).rgb, 1);

    float3 L = normalize(-lightDirIntensity.xyz);
    float3 V = normalize(cameraPos.xyz - i.worldPos);
    float3 H = normalize(L + V);

    float ndlRaw = dot(N, L);
    float ndl = ndlRaw;

    // Soft wrap / SSS approximation (skin)
    float sss = style2.x * sssM;
    if (sss > 0.001)
        ndl = saturate((ndlRaw + sss) / (1.0 + sss));
    else
        ndl = saturate(ndlRaw);

    // Soft cel: shadowMask from LightMap tints the lit/unlit blend (ZZZ-style)
    float thr = style0.x;
    float soft = max(style0.y, 1e-4);
    float litFactor = ndl * lerp(0.55, 1.0, shadowM);
    float shade = saturate((litFactor - thr) / soft + 0.5);
    float toon = smoothstep(0.0, 1.0, shade);
    // LightMap as soft color grade on UV0 (ZZZ character atlas)
    float3 lmRgb = texLight.Sample(sampLin, uv).rgb;
    float3 albedoTint = albedo * lerp(float3(1, 1, 1), saturate(lmRgb * 1.15 + 0.15), 0.25);
    float shadowTint = style0.z;
    float3 diffuseLit = albedoTint * lerp(shadowTint, 1.0, toon);

    // Specular (Blinn + optional anisotropic ring for hair)
    float rough01 = max(rough, 0.04);
    float gloss = exp2(10.0 * (1.0 - rough01) + 1.0);
    float ndh = saturate(dot(N, H));
    float spec = pow(ndh, gloss) * specM;

    float anisoStr = style1.z * aniso;
    if (anisoStr > 0.001)
    {
        // Kajiya-esque: shift along bitangent
        float3 B = normalize(i.btg);
        float tdoth = dot(B, H);
        float an = sqrt(max(0.0, 1.0 - tdoth * tdoth));
        float anSpec = pow(an, style1.w) * anisoStr;
        spec = max(spec, anSpec * specM);
    }

    float3 F0 = lerp(style2.y.xxx, albedo, metal);
    float3 specCol = F0 * spec * lightDirIntensity.w;

    // Rim
    float fres = pow(1.0 - saturate(dot(N, V)), style1.y);
    float3 rim = fres * style1.x * rimM * albedo;

    float3 ambient = ambientColor.rgb * albedo * ao;
    float3 color = ambient + diffuseLit * lightDirIntensity.w * ao + specCol + rim;

    // Glow add
    color += albedo * glowM * style2.z;

    return float4(saturate(color), opacity);
}
