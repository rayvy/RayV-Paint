// Separable box blur for filter preview (ping-pong H then V).
// VS: full-screen triangle; PS samples input with uniform kernel.

cbuffer BlurCB : register(b0)
{
    float4 u_Params; // x: texelW, y: texelH, z: radius, w: unused
};

Texture2D g_Input : register(t0);
SamplerState g_Samp : register(s0);

struct VS_OUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VS_OUT VSMain(uint id : SV_VertexID)
{
    VS_OUT o;
    // Fullscreen triangle
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}

float4 BlurAxis(float2 uv, float2 dir, float radius)
{
    int r = (int)radius;
    if (r < 1) r = 1;
    float4 sum = 0;
    float wsum = 0;
    // Box: equal weights, sample 2r+1 taps (cap 32 for SM4)
    int taps = min(r, 31);
    for (int i = -taps; i <= taps; ++i)
    {
        float2 suv = uv + dir * (float)i;
        sum += g_Input.SampleLevel(g_Samp, suv, 0);
        wsum += 1.0;
    }
    return sum / max(wsum, 1.0);
}

float4 PSBlurH(VS_OUT i) : SV_TARGET
{
    float2 dir = float2(u_Params.x, 0.0); // one texel horizontal
    return BlurAxis(i.uv, dir, u_Params.z);
}

float4 PSBlurV(VS_OUT i) : SV_TARGET
{
    float2 dir = float2(0.0, u_Params.y);
    return BlurAxis(i.uv, dir, u_Params.z);
}
