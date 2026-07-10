// ---- Created with 3Dmigoto v1.3.16 on Fri Jul 10 23:15:47 2026
Texture2D<float4> t8 : register(t8);

Texture2DArray<float4> t7 : register(t7);

Texture2D<float4> t6 : register(t6);

Texture2D<float4> t5 : register(t5);

Texture2D<float4> t4 : register(t4);

Texture2D<float4> t3 : register(t3);

Texture2D<float4> t2 : register(t2);

struct t1_t {
  float val[32];
};
StructuredBuffer<t1_t> t1 : register(t1);

Texture2DArray<float4> t0 : register(t0);

SamplerState s2_s : register(s2);

SamplerComparisonState s1_s : register(s1);

SamplerState s0_s : register(s0);

cbuffer cb4 : register(b4)
{
  float4 cb4[170];
}

cbuffer cb3 : register(b3)
{
  float4 cb3[41];
}

cbuffer cb2 : register(b2)
{
  float4 cb2[27];
}

cbuffer cb1 : register(b1)
{
  float4 cb1[29];
}

cbuffer cb0 : register(b0)
{
  float4 cb0[209];
}




// 3Dmigoto declarations
#define cmp -
Texture1D<float4> IniParams : register(t120);


void main(
  float4 v0 : TEXCOORD0,
  float4 v1 : TEXCOORD1,
  float4 v2 : TEXCOORD2,
  float4 v3 : TEXCOORD3,
  float4 v4 : TEXCOORD4,
  float4 v5 : TEXCOORD5,
  float4 v6 : TEXCOORD6,
  float4 v7 : TEXCOORD7,
  float3 v8 : TEXCOORD8,
  float4 v9 : SV_POSITION0,
  uint v10 : SV_IsFrontFace0,
  out float4 o0 : SV_Target0,
  out float4 o1 : SV_Target1,
  out float4 o2 : SV_Target2,
  out float4 o3 : SV_Target3)
{
  float4 r0,r1,r2,r3,r4,r5,r6,r7,r8,r9,r10,r11,r12,r13,r14,r15,r16,r17,r18,r19,r20,r21;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.x = v10.x ? 1 : -1;
  r0.y = cmp(0.5 < cb1[28].y);
  r0.zw = v5.xy / v5.ww;
  r1.xy = v6.xy / v6.ww;
  r1.xy = -r1.xy + r0.zw;
  r0.zw = float2(0.5,-0.5) * r1.xy;
  r0.zw = sqrt(abs(r0.zw));
  r1.z = -r1.y;
  r1.yw = cmp(float2(0,0) < r1.xz);
  r1.xz = cmp(r1.xz < float2(0,0));
  r1.xy = (int2)-r1.yw + (int2)r1.xz;
  r1.xy = (int2)r1.xy;
  r0.zw = r1.xy * r0.zw;
  r0.zw = r0.zw * float2(0.5,0.5) + float2(0.498039216,0.498039216);
  o2.xy = r0.yy ? r0.zw : float2(0.497999996,0.497999996);
  r0.y = cmp(cb3[40].x == 1.000000);
  r0.zw = cmp(float2(0,0) != cb4[154].wz);
  r1.x = cmp(0 != cb4[155].x);
  bitmask.x = ((~(-1 << 1)) << 3) & 0xffffffff;  r1.x = (((uint)r1.x << 3) & bitmask.x) | ((uint)0 & ~bitmask.x);
  bitmask.y = ((~(-1 << 1)) << 2) & 0xffffffff;  r0.y = (((uint)r0.y << 2) & bitmask.y) | ((uint)0 & ~bitmask.y);
  bitmask.w = ((~(-1 << 1)) << 1) & 0xffffffff;  r0.w = (((uint)r0.w << 1) & bitmask.w) | ((uint)0 & ~bitmask.w);
  r0.y = (int)r1.x + (int)r0.y;
  r0.y = (int)r0.y + (int)r0.w;
  bitmask.y = ((~(-1 << 1)) << 0) & 0xffffffff;  r0.y = (((uint)r0.z << 0) & bitmask.y) | ((uint)r0.y & ~bitmask.y);
  r0.y = (uint)r0.y;
  o2.z = 0.00392156886 * r0.y;
  r0.yz = cmp(float2(0.5,0.5) < cb4[150].xy);
  r0.w = cmp((int)v10.x == 0);
  r0.y = r0.w ? r0.y : 0;
  r0.w = cmp(1 < v0.z);
  r0.z = r0.w ? r0.z : 0;
  r0.z = (int)r0.z | (int)r0.y;
  r0.zw = r0.zz ? v0.zw : v0.xy;
  r1.xyz = t3.SampleBias(s0_s, r0.zw, cb0[199].x).xyz;
  r2.xyz = cb4[58].xyz * r1.xyz;
  r0.yz = r0.yy ? v0.zw : v0.xy;
  r3.xyz = t4.SampleBias(s0_s, r0.yz, cb0[199].x).xyz;
  r3.xyz = saturate(r3.xyz);
  r3.xyz = r3.xyz * float3(2,2,2) + float3(-1.00399995,-1.00399995,-1);
  r3.xy = cb4[140].yy * r3.xy;
  r0.w = dot(r3.xy, r3.xy);
  r0.w = min(1, r0.w);
  r0.w = 1 + -r0.w;
  r0.w = sqrt(r0.w);
  r0.x = r0.w * r0.x;
  r4.xyz = v4.xyz * r3.yyy;
  r3.xyw = r3.xxx * v3.xyz + r4.xyz;
  r3.xyw = r0.xxx * v2.xyz + r3.xyw;
  r0.x = dot(r3.xyw, r3.xyw);
  r0.x = rsqrt(r0.x);
  r3.xyw = r3.xyw * r0.xxx;
  r4.xyz = t5.SampleBias(s0_s, r0.yz, cb0[199].x).zxy;
  r4.xyz = saturate(r4.xyz);
  r0.x = cb4[140].z * r4.z;
  r0.yzw = t6.SampleBias(s0_s, r0.yz, cb0[199].x).xyz;
  r0.yzw = saturate(r0.yzw);
  r5.xy = cmp(float2(0.5,0.5) < cb4[147].xy);
  r1.w = r5.y ? r5.x : 0;
  r2.w = -0.200000003 + r0.w;
  r2.w = 1.25 * r2.w;
  r2.w = max(0, r2.w);
  r2.w = r1.w ? r2.w : r0.w;
  r2.w = r5.x ? r2.w : 0;
  r4.w = 5 * r4.y;
  r4.w = floor(r4.w);
  r4.w = 4 + -r4.w;
  r4.w = max(0, r4.w);
  r4.w = (int)r4.w;
  r5.x = cmp((int)r4.w == asint(cb4[135].y));
  r5.y = r5.x ? 0.000000 : 0;
  r5.z = cmp(0.5 < cb4[139].w);
  r6.xy = cb4[140].xx * v0.xy;
  r6.xyz = t2.SampleBias(s0_s, r6.xy, cb0[199].x).xyz;
  r1.xyz = r1.xyz * cb4[58].xyz + r6.xyz;
  r1.xyz = float3(-0.5,-0.5,-0.5) + r1.xyz;
  r1.xyz = max(float3(0,0,0), r1.xyz);
  r1.xyz = r5.zzz ? r1.xyz : r2.xyz;
  r6.xyzw = cmp(r4.yyyy < float4(0.200000003,0.400000006,0.600000024,0.800000012));
  r2.x = r6.w ? cb4[137].x : cb4[136].w;
  r2.x = r6.z ? cb4[137].y : r2.x;
  r2.x = r6.y ? cb4[137].z : r2.x;
  r2.x = r6.x ? cb4[137].w : r2.x;
  r7.x = v2.w;
  r7.y = v3.w;
  r7.z = v4.w;
  r8.xyz = cb0[53].xyz + -r7.xyz;
  r2.y = dot(r8.xyz, r8.xyz);
  r2.z = max(1.17549435e-38, r2.y);
  r2.z = rsqrt(r2.z);
  r9.xyz = r8.xyz * r2.zzz;
  r4.y = sqrt(r2.y);
  r5.z = cmp(0 < asint(cb0[196].x));
  if (r5.z != 0) {
    r5.w = (int)cb3[2].z;
    r7.w = asint(cb0[196].x) + -1;
    r5.w = min((int)r7.w, (int)r5.w);
    r5.w = max(0, (int)r5.w);
    r10.x = t1[r5.w].val[0/4];
    r10.y = t1[r5.w].val[0/4+1];
    r10.z = t1[r5.w].val[0/4+2];
    r10.w = t1[r5.w].val[0/4+3];
    r11.x = t1[r5.w].val[16/4];
    r11.y = t1[r5.w].val[16/4+1];
    r11.z = t1[r5.w].val[16/4+2];
    r11.w = t1[r5.w].val[16/4+3];
    r12.y = t1[r5.w].val[32/4];
    r12.x = r11.w;
  } else {
    r12.xy = float2(0,0);
    r10.xyzw = float4(0,0,0,0);
    r11.xyz = float3(0,0,0);
  }
  r11.xyz = r11.xyz + -r7.xyz;
  r5.w = dot(r11.xyz, r11.xyz);
  r5.w = max(1.17549435e-38, r5.w);
  r7.w = rsqrt(r5.w);
  r8.w = r10.w * r10.w;
  r5.w = r5.w / r8.w;
  r5.w = 1 + -r5.w;
  r5.w = max(0, r5.w);
  r10.xyz = -cb0[197].xyz + r10.xyz;
  r10.xyz = r5.www * r10.xyz + cb0[197].xyz;
  r8.w = -1 + r5.w;
  r5.w = r5.w * r8.w + 1;
  r8.w = cmp(0.5 < cb0[22].x);
  if (r8.w != 0) {
    r8.w = r6.w ? cb4[166].x : cb4[165].w;
    r8.w = r6.z ? cb4[166].y : r8.w;
    r8.w = r6.y ? cb4[166].z : r8.w;
    r8.w = r6.x ? cb4[166].w : r8.w;
    r13.xyz = r3.xyw * cb4[138].xxx + r7.xyz;
    r13.xyz = -cb3[39].xyz + r13.xyz;
    r14.xyz = cb3[34].xyz * r13.yyy;
    r13.xyw = cb3[33].xyz * r13.xxx + r14.xyz;
    r13.xyz = cb3[35].xyz * r13.zzz + r13.xyw;
    r13.xyz = cb3[36].xyz + r13.xyz;
    r12.zw = r13.xy * cb3[38].xy + cb3[38].zw;
    r13.xy = -cb0[208].xy + r12.zw;
    r9.w = t8.SampleCmpLevelZero(s1_s, r13.xy, r13.z).x;
    r14.xyzw = cb0[208].xyxy * float4(-1,1,1,-1) + r12.zwzw;
    r10.w = t8.SampleCmpLevelZero(s1_s, r14.xy, r13.z).x;
    r9.w = r10.w + r9.w;
    r10.w = t8.SampleCmpLevelZero(s1_s, r14.zw, r13.z).x;
    r9.w = r10.w + r9.w;
    r13.xy = cb0[208].xy + r12.zw;
    r10.w = t8.SampleCmpLevelZero(s1_s, r13.xy, r13.z).x;
    r9.w = r10.w + r9.w;
    r14.xyzw = cb0[208].xyxy * float4(-1.41421294,0,1.41421294,0) + r12.zwzw;
    r10.w = t8.SampleCmpLevelZero(s1_s, r14.xy, r13.z).x;
    r9.w = r10.w + r9.w;
    r10.w = t8.SampleCmpLevelZero(s1_s, r14.zw, r13.z).x;
    r9.w = r10.w + r9.w;
    r14.xyzw = cb0[208].xyxy * float4(0,-1.41421294,0,1.41421294) + r12.zwzw;
    r10.w = t8.SampleCmpLevelZero(s1_s, r14.xy, r13.z).x;
    r9.w = r10.w + r9.w;
    r10.w = t8.SampleCmpLevelZero(s1_s, r14.zw, r13.z).x;
    r9.w = r10.w + r9.w;
    r10.w = t8.SampleCmpLevelZero(s1_s, r12.zw, r13.z).x;
    r9.w = r10.w + r9.w;
    r9.w = r9.w * 0.111100003 + -1;
    r8.w = r9.w * r8.w;
    r8.w = cb3[37].x * r8.w;
    r13.xyz = -cb2[20].xyz + r7.xyz;
    r14.xyz = -cb2[21].xyz + r7.xyz;
    r15.xyz = -cb2[22].xyz + r7.xyz;
    r16.xyz = -cb2[23].xyz + r7.xyz;
    r13.x = dot(r13.xyz, r13.xyz);
    r13.y = dot(r14.xyz, r14.xyz);
    r13.z = dot(r15.xyz, r15.xyz);
    r13.w = dot(r16.xyz, r16.xyz);
    r13.xyzw = cmp(r13.xyzw < cb2[24].xyzw);
    r14.xyzw = r13.xyzw ? float4(1,1,1,1) : 0;
    r13.xyz = r13.xyz ? float3(-1,-1,-1) : float3(-0,-0,-0);
    r13.xyz = r14.yzw + r13.xyz;
    r14.yzw = max(float3(0,0,0), r13.xyz);
    r9.w = dot(r14.xyzw, float4(4,3,2,1));
    r13.z = 4 + -r9.w;
    r9.w = (uint)r13.z;
    r9.w = (uint)r9.w << 2;
    r14.xyz = cb2[r9.w+1].xyz * v3.www;
    r14.xyz = cb2[r9.w+0].xyz * v2.www + r14.xyz;
    r14.xyz = cb2[r9.w+2].xyz * v4.www + r14.xyz;
    r14.xyz = cb2[r9.w+3].xyz + r14.xyz;
    r12.zw = float2(1024,1024) * r14.xy;
    r12.zw = frac(r12.zw);
    r9.w = dot(r12.zw, float2(12.9898005,78.2330017));
    r9.w = sin(r9.w);
    r9.w = 43758.5469 * r9.w;
    r9.w = frac(r9.w);
    sincos(r9.w, r15.x, r16.x);
    r17.xyzw = float4(1.29999995,1.29999995,1.29999995,1.29999995) * cb2[26].xxyy;
    r15.xz = r15.xx;
    r15.yw = r16.xx;
    r15.xyzw = r15.xyzw * r17.xyzw;
    r16.xyzw = float4(-0.172399998,-0.978299975,-0.978299975,0.172399998) * r15.xyzw;
    r12.zw = r16.xz + r16.yw;
    r13.xy = r12.zw + r14.xy;
    r9.w = t0.SampleCmpLevelZero(s1_s, r13.xyz, r14.z).x;
    r16.xyzw = float4(0.87470001,0.484600008,0.484600008,-0.87470001) * r15.xyzw;
    r12.zw = r16.xz + r16.yw;
    r13.xy = r12.zw + r14.xy;
    r10.w = t0.SampleCmpLevelZero(s1_s, r13.xyz, r14.z).x;
    r9.w = r10.w + r9.w;
    r16.xyzw = float4(-0.968299985,-0.0373999998,-0.0373999998,0.968299985) * r15.xyzw;
    r12.zw = r16.xz + r16.yw;
    r13.xy = r12.zw + r14.xy;
    r10.w = t0.SampleCmpLevelZero(s1_s, r13.xyz, r14.z).x;
    r9.w = r10.w + r9.w;
    r16.xyzw = float4(0.278299987,0.41960001,0.41960001,-0.278299987) * r15.xyzw;
    r12.zw = r16.xz + r16.yw;
    r13.xy = r12.zw + r14.xy;
    r10.w = t0.SampleCmpLevelZero(s1_s, r13.xyz, r14.z).x;
    r9.w = r10.w + r9.w;
    r16.xyzw = float4(-0.150700003,0.839100003,0.839100003,-0.150700003) * r15.xyzw;
    r12.zw = r16.xz + r16.yw;
    r13.xy = r12.zw + r14.xy;
    r10.w = t0.SampleCmpLevelZero(s1_s, r13.xyz, r14.z).x;
    r9.w = r10.w + r9.w;
    r16.xyzw = float4(-0.641700029,0.479299992,0.479299992,-0.641700029) * r15.xyzw;
    r12.zw = r16.xz + r16.yw;
    r13.xy = r12.zw + r14.xy;
    r10.w = t0.SampleCmpLevelZero(s1_s, r13.xyz, r14.z).x;
    r9.w = r10.w + r9.w;
    r16.xyzw = float4(0.577899992,-0.816100001,-0.816100001,0.577899992) * r15.xyzw;
    r12.zw = r16.xz + r16.yw;
    r13.xy = r12.zw + r14.xy;
    r10.w = t0.SampleCmpLevelZero(s1_s, r13.xyz, r14.z).x;
    r9.w = r10.w + r9.w;
    r16.xyzw = float4(-0.540899992,-0.458799988,-0.458799988,0.540899992) * r15.xyzw;
    r12.zw = r16.xz + r16.yw;
    r13.xy = r12.zw + r14.xy;
    r10.w = t0.SampleCmpLevelZero(s1_s, r13.xyz, r14.z).x;
    r9.w = r10.w + r9.w;
    r16.xyzw = float4(0.704400003,-0.1919,-0.1919,0.704400003) * r15.xyzw;
    r12.zw = r16.xz + r16.yw;
    r13.xy = r12.zw + r14.xy;
    r10.w = t0.SampleCmpLevelZero(s1_s, r13.xyz, r14.z).x;
    r9.w = r10.w + r9.w;
    r16.xyzw = float4(0.105300002,-0.446399987,-0.446399987,0.105300002) * r15.xyzw;
    r12.zw = r16.xz + r16.yw;
    r13.xy = r12.zw + r14.xy;
    r10.w = t0.SampleCmpLevelZero(s1_s, r13.xyz, r14.z).x;
    r9.w = r10.w + r9.w;
    r15.xyzw = float4(-0.206599995,0.0661000013,0.0661000013,-0.206599995) * r15.xyzw;
    r12.zw = r15.xz + r15.yw;
    r13.xy = r12.zw + r14.xy;
    r10.w = t0.SampleCmpLevelZero(s1_s, r13.xyz, r14.z).x;
    r9.w = r10.w + r9.w;
    r9.w = cb2[25].x * r9.w;
    r10.w = 1 + -cb2[25].x;
    r9.w = r9.w * 0.0908999965 + r10.w;
    r10.w = cmp(0 >= r14.z);
    r11.w = cmp(r14.z >= 1);
    r10.w = (int)r10.w | (int)r11.w;
    r9.w = r10.w ? 1 : r9.w;
    r10.w = 1 + -r12.x;
    r10.w = cb3[40].y * r10.w + r12.x;
    r9.w = r10.w * r9.w;
    r10.w = saturate(r12.y * 2 + -1);
    r11.w = cb0[197].w * r10.w;
    r12.x = cmp(0.5 < cb3[37].x);
    r8.w = r10.w * r8.w + 1;
    r8.w = min(1, r8.w);
    r8.w = r12.x ? r8.w : 1;
    r8.w = r8.w * r9.w;
    r9.w = 1 + -cb4[136].z;
    r9.w = r8.w * cb4[136].z + r9.w;
    r10.w = cb4[136].z * r11.w;
    r11.w = -r11.w * cb4[136].z + 1;
    r8.w = r8.w * r10.w + r11.w;
  } else {
    r9.w = 1;
    r8.w = 1;
  }
  r10.w = saturate(2.5 * cb3[1].w);
  r11.w = 1 + -r10.w;
  r8.w = r11.w * r8.w + r10.w;
  r10.xyz = float3(-0.800000012,-0.800000012,-0.800000012) + r10.xyz;
  r10.xyz = cb4[58].www * r10.xyz + float3(0.800000012,0.800000012,0.800000012);
  r2.y = rsqrt(r2.y);
  r12.xyz = r8.xyz * r2.yyy;
  r11.xyz = r11.xyz * r7.www + -r12.xyz;
  r11.xyz = cb4[58].www * r11.xyz + r12.xyz;
  r2.y = cmp(cb3[3].w != 0.000000);
  r7.x = dot(cb3[3].xyz, r7.xyz);
  r7.x = saturate(-cb3[3].w + r7.x);
  r7.yzw = cb0[15].xyz * r10.xyz;
  r12.xyz = cb0[15].xyz + r10.xyz;
  r12.xyz = -r10.xyz * cb0[15].xyz + r12.xyz;
  r7.yzw = cb0[15].www * r12.xyz + r7.yzw;
  r7.yzw = r7.yzw + -r10.xyz;
  r7.xyz = r7.xxx * r7.yzw + r10.xyz;
  r7.xyz = r2.yyy ? r7.xyz : r10.xyz;
  r2.x = max(9.99999975e-06, r2.x);
  r2.y = rcp(r2.x);
  r7.w = dot(r3.xyw, r11.xyz);
  r10.x = 1 + r7.w;
  r10.y = 3 * r11.y;
  r10.y = min(1, r10.y);
  r10.y = -r10.y * 0.5 + r3.y;
  r10.y = saturate(1.5 + r10.y);
  r10.x = r10.x * r10.y + -1;
  r10.x = r10.x + -r7.w;
  r10.x = v7.y * r10.x + r7.w;
  r3.z = r3.z * 2 + r10.x;
  r10.x = 3 * r3.z;
  r12.xy = float2(1.5,4.5) * r2.xx;
  r10.yz = r3.zz * float2(3,3) + -r12.xy;
  r10.xyz = float3(3,1,-1) + r10.xyz;
  r2.x = -r2.x * 3 + 2;
  r10.xyw = r10.xyz / r2.xxx;
  r12.xyz = float3(1,1,1) + -r10.xyw;
  r13.xyz = float3(0.333299994,-0.333299994,-0.333299994) + r3.zzz;
  r13.xyz = r2.yyy * r13.xyz + float3(0.5,0.5,-0.5);
  r14.xyz = float3(1,1,1) + -r13.xyz;
  r15.xy = min(r14.yx, r10.yx);
  r10.xz = min(r13.xz, r12.yz);
  r15.z = r12.x;
  r15.w = r10.x;
  r12.xyz = saturate(r15.zyw);
  r15.y = saturate(min(r14.z, r13.y));
  r15.x = saturate(r15.x);
  r10.zw = saturate(r10.zw);
  r2.x = r9.w + -r8.w;
  r2.x = cb3[40].y * r2.x + r8.w;
  r13.xyzw = r2.xxxx * float4(2,2,-2,-2) + float4(0,-1,1,2);
  r13.x = saturate(min(r13.x, r13.w));
  r13.yz = saturate(r13.yz);
  r2.x = cmp(0.5 < cb3[40].y);
  if (r2.x != 0) {
    if (r5.z != 0) {
      r2.x = (int)cb3[2].z;
      r2.y = asint(cb0[196].x) + -1;
      r2.x = min((int)r2.x, (int)r2.y);
      r2.x = max(0, (int)r2.x);
      r2.x = t1[r2.x].val[32/4];
    } else {
      r2.x = 0;
    }
    r2.x = cb0[197].w * r2.x;
    r14.y = r13.x * r2.x;
    r10.xy = -r13.zx * r2.xx + r13.zx;
    r2.y = r10.x + r10.y;
    r14.z = r13.y + r2.y;
    if (r5.z != 0) {
      r2.y = (int)cb3[2].z;
      r10.x = asint(cb0[196].x) + -1;
      r2.y = min((int)r10.x, (int)r2.y);
      r2.y = max(0, (int)r2.y);
      r2.y = t1[r2.y].val[28/4];
    } else {
      r2.y = 0;
    }
    r13.xy = r14.yz * r2.yy;
    r10.xy = -r14.yz * r2.yy + r14.yz;
    r2.y = r10.x + r10.y;
    r13.z = r13.z * r2.x + r2.y;
  } else {
    if (r5.z != 0) {
      r2.x = (int)cb3[2].z;
      r2.y = asint(cb0[196].x) + -1;
      r2.x = min((int)r2.x, (int)r2.y);
      r2.x = max(0, (int)r2.x);
      r2.x = t1[r2.x].val[32/4];
    } else {
      r2.x = 0;
    }
    r13.xz = r13.xz * r2.xx;
  }
  r2.x = 1 + -r12.x;
  r2.x = r2.x + -r12.y;
  r2.x = r2.x + -r12.z;
  r2.x = r13.z * r2.x + r12.z;
  r2.y = r13.x + r13.y;
  r10.xy = r15.xy * r2.yy;
  r2.y = r10.z + r10.w;
  r2.y = r2.y * r13.x + r10.y;
  r10.y = r13.y * r10.z;
  r10.z = v7.x * r12.x;
  r11.w = -r12.x * v7.x + r12.x;
  r11.w = r12.y + r11.w;
  r12.xyz = r6.www ? cb4[61].xyz : cb4[60].xyz;
  r12.xyz = r6.zzz ? cb4[62].xyz : r12.xyz;
  r12.xyz = r6.yyy ? cb4[63].xyz : r12.xyz;
  r12.xyz = r6.xxx ? cb4[64].xyz : r12.xyz;
  r13.xzw = r6.www ? cb4[66].xyz : cb4[65].xyz;
  r13.xzw = r6.zzz ? cb4[67].xyz : r13.xzw;
  r13.xzw = r6.yyy ? cb4[68].xyz : r13.xzw;
  r13.xzw = r6.xxx ? cb4[69].xyz : r13.xzw;
  r14.xyz = r5.xxx ? cb0[10].xyz : cb0[3].xyz;
  r15.xyz = r5.xxx ? cb0[11].xyz : cb0[4].xyz;
  r16.xyz = r5.xxx ? cb0[12].xyz : cb0[5].xyz;
  r17.xyz = r5.xxx ? cb0[9].xyz : cb0[6].xyz;
  r18.xyz = r5.xxx ? cb0[13].xyz : cb0[7].xyz;
  r19.xyz = r5.xxx ? cb0[14].xyz : cb0[8].xyz;
  r12.w = 0.437249988 * r4.y;
  r12.w = min(1, r12.w);
  r20.x = r12.w * cb3[1].w + -r12.w;
  r20.y = -r12.w * cb3[1].w + r12.w;
  r20.xy = float2(1,-1) + r20.xy;
  r20.xy = cb4[136].yy * r20.xy + float2(0,1);
  r12.xyz = float3(6.10351562e-05,6.10351562e-05,6.10351562e-05) + r12.xyz;
  r12.w = r12.x + r12.y;
  r12.w = r12.w + r12.z;
  r12.w = 0.333330005 * r12.w;
  r21.xyz = saturate(r12.xyz / r12.www);
  r12.xyz = r12.xyz * r20.yyy;
  r12.xyz = r21.xyz * r20.xxx + r12.xyz;
  r13.xzw = float3(6.10351562e-05,6.10351562e-05,6.10351562e-05) + r13.xzw;
  r12.w = r13.x + r13.z;
  r12.w = r12.w + r13.w;
  r12.w = 0.333330005 * r12.w;
  r21.xyz = saturate(r13.xzw / r12.www);
  r13.xzw = r13.xzw * r20.yyy;
  r13.xzw = r21.xyz * r20.xxx + r13.xzw;
  r14.xyz = r12.xyz * r14.xyz;
  r12.xyz = r12.xyz * r15.xyz;
  r15.xyz = r13.xzw * r16.xyz;
  r13.xzw = r13.xzw * r19.xyz;
  r16.xyz = float3(1.17549435e-38,1.17549435e-38,1.17549435e-38) + r7.xyz;
  r12.w = max(r16.x, r16.y);
  r12.w = max(r12.w, r16.z);
  r12.w = rcp(r12.w);
  if (r5.z != 0) {
    r14.w = (int)cb3[2].z;
    r15.w = asint(cb0[196].x) + -1;
    r14.w = min((int)r15.w, (int)r14.w);
    r14.w = max(0, (int)r14.w);
    r14.w = t1[r14.w].val[32/4];
  } else {
    r14.w = 0;
  }
  r15.w = 1 + -r5.w;
  r5.w = r14.w * r15.w + r5.w;
  r16.xyz = r7.xyz * r5.www;
  r5.w = min(1, r12.w);
  r19.xyz = r16.xyz * r5.www;
  r17.xyz = r17.xyz * r10.yyy;
  r17.xyz = r18.xyz * r2.yyy + r17.xyz;
  r17.xyz = r10.www * r13.yyy + r17.xyz;
  r15.xyz = r15.xyz * r11.www;
  r10.yzw = r13.xzw * r10.zzz + r15.xyz;
  r10.yzw = r12.xyz * r2.xxx + r10.yzw;
  r10.xyz = r14.xyz * r10.xxx + r10.yzw;
  r10.xyz = r10.xyz * r19.xyz;
  r10.xyz = r16.xyz * r17.xyz + r10.xyz;
  r10.xyz = r10.xyz + -r7.xyz;
  r7.xyz = cb4[58].www * r10.xyz + r7.xyz;
  r2.x = cmp(cb0[22].z >= 0.5);
  if (r2.x != 0) {
    r2.x = cmp(cb4[r4.w+10].x < 50);
    if (r2.x != 0) {
      r2.x = 5.0999999 * r0.w;
      r2.x = min(1, r2.x);
      r2.y = cmp(0.200000003 >= r0.w);
      r2.y = r2.y ? 1.000000 : 0;
      r2.x = r2.x * r2.y;
      r0.w = r1.w ? r2.x : r0.w;
      r1.w = cmp(cb4[147].y >= 0.5);
      r1.w = r1.w ? 1.000000 : 0;
      r0.w = -1 + r0.w;
      r0.w = r1.w * r0.w + 1;
      r2.xy = cb0[118].xy * r3.yy;
      r2.xy = cb0[117].xy * r3.xx + r2.xy;
      r2.xy = cb0[119].xy * r3.ww + r2.xy;
      r2.xy = r2.xy * float2(0.5,0.5) + float2(0.5,0.5);
      r1.w = cmp(0.5 < cb4[r4.w+15].z);
      if (r1.w != 0) {
        r0.y = cb4[r4.w+15].w * r0.y;
        r10.xy = cb4[r4.w+0].xy * v0.xy + cb4[r4.w+0].zw;
        r2.xy = r0.yy * r2.xy + r10.xy;
      }
      r10.x = cb4[r4.w+10].w * cb0[40].y;
      r10.y = cb4[r4.w+15].x * cb0[40].y;
      r10.xy = r10.xy + r2.xy;
      r10.z = cb4[r4.w+10].x;
      r10.xyzw = t7.Sample(s2_s, r10.xyz).xyzw;
      r12.xyz = cb4[r4.w+5].xyz * r10.xyz;
      r0.y = r10.w * r0.w;
      r0.w = cmp(cb4[r4.w+15].y < 0.5);
      if (r0.w != 0) {
        r0.w = saturate(cb4[r4.w+10].z * r0.y);
        r13.xyz = r12.xyz * cb4[r4.w+10].yyy + -r1.xyz;
        r1.xyz = r0.www * r13.xyz + r1.xyz;
      } else {
        r0.w = cmp(cb4[r4.w+15].y < 1.5);
        if (r0.w != 0) {
          r0.w = saturate(cb4[r4.w+10].z * r0.y);
          r13.xyz = r12.xyz * r0.www;
          r1.xyz = r13.xyz * cb4[r4.w+10].yyy + r1.xyz;
        } else {
          r10.xyz = r10.xyz * cb4[r4.w+5].xyz + float3(-0.5,-0.5,-0.5);
          r10.xyz = saturate(r10.xyz * cb4[r4.w+10].yyy + r12.xyz);
          r0.y = saturate(cb4[r4.w+10].z * r0.y);
          r10.xyz = float3(-0.5,-0.5,-0.5) + r10.xyz;
          r10.xyz = r0.yyy * r10.xyz + float3(0.5,0.5,0.5);
          r12.xyz = r10.xyz * r1.xyz;
          r13.xyz = r12.xyz + r12.xyz;
          r14.xyz = float3(1,1,1) + -r1.xyz;
          r14.xyz = r14.xyz + r14.xyz;
          r10.xyz = float3(1,1,1) + -r10.xyz;
          r10.xyz = -r14.xyz * r10.xyz + float3(1,1,1);
          r14.xyz = cmp(r1.xyz >= float3(0.5,0.5,0.5));
          r14.xyz = r14.xyz ? float3(1,1,1) : 0;
          r10.xyz = -r12.xyz * float3(2,2,2) + r10.xyz;
          r1.xyz = r14.xyz * r10.xyz + r13.xyz;
        }
      }
    }
  }
  r0.y = cmp(0.5 < v7.z);
  if (r5.x == 0) {
    r10.xyz = r0.yyy ? r3.xyw : v2.xyz;
    r0.w = dot(r1.xyz, float3(0.289999992,0.600000024,0.109999999));
    r1.w = cmp(v7.z < 0.5);
    r2.xy = r0.ww * float2(0.287499994,0.400000006) + float2(1.4375,1);
    r4.w = dot(r11.xyz, r10.xyz);
    r5.x = r4.w + -r7.w;
    r5.x = saturate(-r5.x * 3 + 1);
    r5.w = r5.x + r5.x;
    r5.x = sqrt(r5.x);
    r5.x = r5.w * r5.x;
    r5.x = min(1, r5.x);
    r5.w = r7.w * 0.5 + 0.5;
    r10.x = saturate(r7.w);
    r5.x = r5.w * r5.x + -r10.x;
    r5.x = r5.x * 0.5 + r10.x;
    r4.w = saturate(r4.w);
    r5.w = max(r1.y, r1.z);
    r5.w = max(r5.w, r1.x);
    r10.x = cmp(1 < r5.w);
    r10.yzw = r1.xyz / r5.www;
    r10.xyz = r10.xxx ? r10.yzw : r1.xyz;
    r5.w = 1 + -r2.x;
    r2.x = r5.x * r5.w + r2.x;
    r10.xyz = log2(r10.xyz);
    r10.xyz = r10.xyz * r2.xxx;
    r10.xyz = exp2(r10.xyz);
    r12.xyz = r10.xyz + -r1.xyz;
    r12.xyz = r12.xyz * float3(0.5,0.5,0.5) + r1.xyz;
    r10.xyz = -r12.xyz + r10.xyz;
    r10.xyz = r4.www * r10.xyz + r12.xyz;
    r0.w = -r0.w * 0.0500000007 + 1.04999995;
    r12.xyz = log2(r1.xyz);
    r12.xyz = r12.xyz * r2.yyy;
    r12.xyz = exp2(r12.xyz);
    r12.xyz = r12.xyz * r0.www;
    r1.xyz = r1.www ? r10.xyz : r12.xyz;
  }
  r0.w = -r0.x * 0.959999979 + 0.959999979;
  r10.xyz = r1.xyz * r0.www;
  r12.xyz = float3(-0.0399999991,-0.0399999991,-0.0399999991) + r1.xyz;
  r12.xyz = r0.xxx * r12.xyz + float3(0.0399999991,0.0399999991,0.0399999991);
  r1.w = -r0.z * cb4[140].w + 1;
  r1.w = r1.w * r1.w;
  r2.x = r1.w * 4 + 2;
  r2.y = r1.w * r1.w;
  r4.w = r1.w * r1.w + -1;
  r13.xyz = cb0[2].xyz + r7.xyz;
  r13.xyz = v8.xyz + r13.xyz;
  r5.x = dot(r13.xyz, float3(0.212672904,0.715152204,0.0721750036));
  r5.w = cb0[19].y + -cb0[19].x;
  r10.w = 1 / r5.w;
  r11.w = -cb0[19].x * r10.w + 1;
  r10.w = r5.x * r10.w + r11.w;
  r10.w = rcp(r10.w);
  r5.w = -r5.w * r10.w + cb0[19].y;
  r10.w = cmp(r5.x < cb0[19].x);
  r5.w = r10.w ? r5.x : r5.w;
  r5.x = 9.99999975e-05 + r5.x;
  r5.x = r5.w / r5.x;
  r13.xyz = r7.xyz * r5.xxx;
  r14.xyz = r6.www ? cb4[76].xyz : cb4[75].xyz;
  r14.xyz = r6.zzz ? cb4[77].xyz : r14.xyz;
  r14.xyz = r6.yyy ? cb4[78].xyz : r14.xyz;
  r14.xyz = r6.xxx ? cb4[79].xyz : r14.xyz;
  r5.w = r6.w ? cb4[144].z : cb4[144].y;
  r5.w = r6.z ? cb4[144].w : r5.w;
  r5.w = r6.y ? cb4[145].x : r5.w;
  r5.w = r6.x ? cb4[145].y : r5.w;
  r10.w = cmp(0.5 < r5.w);
  if (r10.w != 0) {
    r3.z = saturate(r3.z * 1.5 + -0.5);
    r10.w = r6.w ? cb4[145].w : cb4[145].z;
    r10.w = r6.z ? cb4[146].x : r10.w;
    r10.w = r6.y ? cb4[146].y : r10.w;
    r10.w = r6.x ? cb4[146].z : r10.w;
    r3.z = r4.x + r3.z;
    r3.z = -1 + r3.z;
    r10.w = max(9.99999975e-06, r10.w);
    r4.x = saturate(r3.z / r10.w);
  }
  r3.z = cb4[146].w * r4.x;
  r14.xyz = r3.zzz * r14.xyz;
  r14.xyz = r14.xyz * r12.xyz;
  r3.z = cmp(r5.w < 0.5);
  r8.xyz = r8.xyz * r2.zzz + r11.xyz;
  r2.z = dot(r8.xyz, r8.xyz);
  r2.z = rsqrt(r2.z);
  r8.xyz = r8.xyz * r2.zzz;
  r2.z = r6.w ? cb4[143].y : cb4[143].x;
  r2.z = r6.z ? cb4[143].z : r2.z;
  r2.z = r6.y ? cb4[143].w : r2.z;
  r2.z = r6.x ? cb4[144].x : r2.z;
  r4.x = r2.z * r7.w;
  r4.x = saturate(r4.x * 0.75 + 0.25);
  r5.w = dot(r3.xyw, r8.xyz);
  r5.w = r5.w * r2.z;
  r5.w = saturate(r5.w * 0.75 + 0.25);
  r8.x = dot(r11.xyz, r8.xyz);
  r2.z = r8.x * r2.z;
  r2.z = saturate(r2.z * 0.75 + 0.25);
  r5.w = r5.w * r5.w;
  r4.w = r5.w * r4.w + 1.00001001;
  r2.z = r2.z * r2.z;
  r4.w = r4.w * r4.w;
  r2.z = max(0.100000001, r2.z);
  r2.z = r4.w * r2.z;
  r2.x = r2.z * r2.x;
  r2.x = r2.y / r2.x;
  r0.z = saturate(-r0.z * cb4[140].w + r2.x);
  r0.z = r0.z * r4.x;
  r1.w = max(9.99999975e-06, r1.w);
  r0.z = r0.z / r1.w;
  r1.w = r6.w ? cb4[142].x : cb4[141].w;
  r1.w = r6.z ? cb4[142].y : r1.w;
  r1.w = r6.y ? cb4[142].z : r1.w;
  r1.w = r6.x ? cb4[142].w : r1.w;
  r2.x = r6.w ? cb4[169].x : cb4[168].w;
  r2.x = r6.z ? cb4[169].y : r2.x;
  r2.x = r6.y ? cb4[169].z : r2.x;
  r2.x = r6.x ? cb4[169].w : r2.x;
  r1.w = r2.x * r1.w;
  r0.z = r1.w * r0.z;
  r0.z = saturate(10 * r0.z);
  r0.z = 100 * r0.z;
  r0.z = r3.z ? r0.z : 16.6669998;
  r2.xyz = r0.zzz * r14.xyz;
  r8.xyz = r2.xyz * r13.xyz;
  r2.xyz = r2.xyz * r13.xyz + float3(-1,-1,-1);
  r2.xyz = max(float3(0,0,0), r2.xyz);
  r8.xyz = r10.xyz * r13.xyz + r8.xyz;
  r0.z = cmp(cb4[147].x >= 0.5);
  r14.xyz = r6.www ? cb4[81].xyz : cb4[80].xyz;
  r14.xyz = r6.zzz ? cb4[82].xyz : r14.xyz;
  r14.xyz = r6.yyy ? cb4[83].xyz : r14.xyz;
  r14.xyz = r6.xxx ? cb4[84].xyz : r14.xyz;
  r14.xyz = r14.xyz * r2.www;
  r14.xyz = r14.xyz * r1.xyz;
  r14.xyz = r0.zzz ? r14.xyz : 0;
  r0.z = r14.x + r14.y;
  r0.z = r0.z + r14.z;
  r15.xyz = v8.xyz * r5.xxx;
  r8.xyz = r15.xyz * r10.xyz + r8.xyz;
  if (r5.z != 0) {
    r1.w = (int)cb3[2].z;
    r2.w = asint(cb0[196].x) + -1;
    r1.w = min((int)r2.w, (int)r1.w);
    r1.w = max(0, (int)r1.w);
    r16.x = t1[r1.w].val[96/4];
    r16.y = t1[r1.w].val[96/4+1];
    r16.z = t1[r1.w].val[96/4+2];
    r17.x = t1[r1.w].val[112/4];
    r17.y = t1[r1.w].val[112/4+1];
    r17.z = t1[r1.w].val[112/4+2];
  } else {
    r16.xyz = float3(0,0,0);
    r17.xyz = float3(0,0,0);
  }
  r1.w = cmp(0.5 < cb0[23].y);
  r16.xyz = r5.yyy ? r16.xyz : r17.xyz;
  r16.xyz = r1.www ? float3(0.0500000007,0.0500000007,0.0500000007) : r16.xyz;
  r1.xyw = r5.xxx * r1.xyz;
  r1.xyw = r16.xyz * r1.xyw + r8.xyz;
  r1.xyw = r1.xyw + r2.xyz;
  r2.x = cmp(0.5 >= cb0[196].y);
  if (r2.x != 0) {
    r2.x = dot(r9.xyz, r11.xyz);
    r2.x = saturate(-r2.x * 0.5 + 0.5);
    r2.y = r2.x * 0.800000012 + 0.200000003;
    r2.z = r3.y * 0.5 + 0.5;
    r2.w = r2.z * r2.z;
    r2.z = r5.y ? r2.z : r2.w;
    r2.z = -0.200000003 + r2.z;
    r2.z = saturate(1.25 * r2.z);
    r2.w = r2.z * -2 + 3;
    r2.xz = r2.xz * r2.xz;
    r2.z = r2.w * r2.z;
    r2.w = r2.z * r2.z;
    r2.w = r2.w * r2.w;
    r2.w = r2.z * r2.w;
    r8.xyz = r5.yyy ? float3(1,0.300000012,-1) : float3(0.5,1,-0.5);
    r3.z = r8.y + r8.z;
    r2.w = r2.w * r3.z + r8.x;
    r2.z = r2.z * r2.w + -0.100000001;
    r2.z = cb0[207].x * r2.z + 0.100000001;
    r2.w = r7.w * 0.5 + 0.5;
    r2.w = r2.w * r8.w;
    r2.w = r2.w * 1.39999998 + 0.100000001;
    if (r5.z != 0) {
      r3.z = (int)cb3[2].z;
      r4.x = asint(cb0[196].x) + -1;
      r3.z = min((int)r4.x, (int)r3.z);
      r3.z = max(0, (int)r3.z);
      r3.z = t1[r3.z].val[32/4];
    } else {
      r3.z = 0;
    }
    r3.z = r3.z * 0.399999976 + r9.w;
    r3.z = saturate(0.600000024 + r3.z);
    r2.y = r2.y * r2.z;
    r2.y = r2.y * r2.w;
    r2.y = r2.y * r3.z;
    r2.z = cmp(0 != v7.z);
    r2.w = saturate(-r9.y);
    r3.z = r4.z * cb4[140].z + 2.5;
    r2.z = r2.z ? 4.5 : r3.z;
    r2.z = r2.w * r2.z + -0.5;
    r2.z = cb0[207].y * r2.z + 1;
    r2.y = r2.y * r2.z;
    r2.z = 0.0833333358 * r4.y;
    r2.z = min(1, r2.z);
    r2.zw = r2.zz * float2(-0.300000012,-0.300000012) + float2(0.5,0.600000024);
    r3.z = dot(r9.xyz, r3.xyw);
    r3.z = 1 + -r3.z;
    r2.zw = r3.zz + -r2.zw;
    r2.zw = saturate(float2(3.33333325,5.00000048) * r2.zw);
    r4.xz = r2.zw * float2(-2,-2) + float2(3,3);
    r2.zw = r2.zw * r2.zw;
    r2.zw = r4.xz * r2.zw;
    r2.z = r5.y ? r2.z : r2.w;
    r2.w = dot(cb0[197].xyz, float3(0.330000013,0.330000013,0.330000013));
    r4.xzw = cb0[197].xyz * cb0[197].xyz;
    r4.xzw = r4.xzw * r4.xzw;
    r4.xzw = r4.xzw * r4.xzw;
    r3.z = dot(r4.xzw, float3(0.699999988,0.699999988,0.699999988));
    r3.z = max(0.00999999978, r3.z);
    r3.z = rcp(r3.z);
    r2.w = r3.z * r2.w;
    r4.xzw = r2.www * r4.xzw + -r13.xyz;
    r4.xzw = r9.www * r4.xzw + r13.xyz;
    r2.x = log2(r2.x);
    r2.x = 20 * r2.x;
    r2.x = exp2(r2.x);
    r8.xyz = r7.xyz * r5.xxx + -r4.xzw;
    r4.xzw = r2.xxx * r8.xyz + r4.xzw;
    r4.xzw = -r7.xyz * r5.xxx + r4.xzw;
    r4.xzw = cb0[207].zzz * r4.xzw + r13.xyz;
    r2.x = r10.x + r10.y;
    r0.w = r1.z * r0.w + r2.x;
    r0.w = 0.330000013 * r0.w;
    r0.w = r0.w * r0.w;
    r0.w = cb0[207].w * r0.w;
    r0.w = r0.w * -0.199999988 + 1;
    r10.xyz = saturate(r10.xyz);
    r5.xzw = log2(r10.xyz);
    r5.xzw = float3(0.200000003,0.200000003,0.200000003) * r5.xzw;
    r5.xzw = exp2(r5.xzw);
    r1.z = dot(r5.xzw, r5.xzw);
    r1.z = max(6.10351562e-05, r1.z);
    r1.z = rsqrt(r1.z);
    r5.xzw = r5.xzw * r1.zzz;
    r1.z = 48 * cb0[206].w;
    r0.w = 0.100000001 * r0.w;
    r7.xyz = r0.www * r5.xzw;
    r5.xzw = -r0.www * r5.xzw + r12.xyz;
    r5.xzw = r0.xxx * r5.xzw + r7.xyz;
    r5.xzw = r5.xzw * r1.zzz;
    r0.x = r2.y * r2.z;
    r2.xyz = r0.xxx * r4.xzw;
    r2.xyz = r2.xyz * r5.xzw;
    r4.xzw = r6.www ? cb4[88].xyz : cb4[87].xyz;
    r4.xzw = r6.zzz ? cb4[89].xyz : r4.xzw;
    r4.xzw = r6.yyy ? cb4[90].xyz : r4.xzw;
    r4.xzw = r6.xxx ? cb4[91].xyz : r4.xzw;
    r2.xyz = r4.xzw * r2.xyz;
    r0.x = saturate(r4.y * 0.200000003 + -1);
    r0.x = r0.x * -0.699999988 + 1;
    r2.xyw = r2.xyz * r0.xxx;
    r0.w = r2.x + r2.y;
    r0.x = r2.z * r0.x + r0.w;
    r0.x = r0.x * r0.x;
    r0.x = r0.x * 0.0500000007 + 1;
    r2.xyz = r2.xyw * r0.xxx;
    r0.x = r0.y ? 0.5 : 1;
    r0.xyw = r2.xyz * r0.xxx;
    r1.z = cmp(0.5 < cb3[1].w);
    r2.xyz = min(float3(0.699999988,0.699999988,0.699999988), r0.xyw);
    r0.xyw = r1.zzz ? r2.xyz : r0.xyw;
    r0.xyw = cb0[206].xyz * r0.xyw;
  } else {
    r0.xyw = float3(0,0,0);
  }
  r0.xyw = cb4[58].www * r0.xyw;
  r2.xyz = r15.xyz * float3(2,2,2) + float3(1,1,1);
  r4.xyz = r2.xyz * r0.xyw;
  r1.xyz = r1.xyw + r14.xyz;
  r1.w = dot(r4.xyz, float3(0.298999995,0.587000012,0.114));
  r0.xyw = r0.xyw * r2.xyz + -r1.www;
  r0.xyw = cb4[41].xxx * r0.xyw + r1.www;
  r0.xyw = float3(-0.5,-0.5,-0.5) + r0.xyw;
  r0.xyw = cb4[41].yyy * r0.xyw + float3(0.5,0.5,0.5);
  r0.xyw = max(float3(0,0,0), r0.xyw);
  r0.xyw = cb4[41].yyy * r0.xyw;
  r1.w = dot(r1.xyz, float3(0.298999995,0.587000012,0.114));
  r1.xyz = r1.xyz + -r1.www;
  r1.xyz = cb4[41].xxx * r1.xyz + r1.www;
  r1.xyz = float3(-0.5,-0.5,-0.5) + r1.xyz;
  r1.xyz = cb4[41].yyy * r1.xyz + float3(0.5,0.5,0.5);
  r1.xyz = max(float3(0,0,0), r1.xyz);
  r1.w = cb4[158].w * cb4[29].w;
  r2.xyz = cb4[29].xyz + -r1.xyz;
  o0.xyz = r1.www * r2.xyz + r1.xyz;
  r1.x = -cb4[29].w * cb4[158].w + 1;
  r0.xyw = r1.xxx * r0.xyw;
  r0.xyw = max(float3(0,0,0), r0.xyw);
  r0.xyw = sqrt(r0.xyw);
  r0.xyw = float3(0.200000003,0.200000003,0.200000003) * r0.xyw;
  o1.xyz = min(float3(1,1,1), r0.xyw);
  o1.w = 0.333299994 * r0.z;
  o2.w = r5.y ? 0.340000004 : 0;
  o3.xyz = r3.xyw * float3(0.5,0.5,0.5) + float3(0.5,0.5,0.5);
  o0.w = 1;
  o3.w = 1;
  return;
}

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Generated by Microsoft (R) D3D Shader Disassembler
//
//   using 3Dmigoto v1.3.16 on Fri Jul 10 23:15:47 2026
//
//
// Input signature:
//
// Name                 Index   Mask Register SysValue  Format   Used
// -------------------- ----- ------ -------- -------- ------- ------
// TEXCOORD                 0   xyzw        0     NONE   float   xyzw
// TEXCOORD                 1   xyzw        1     NONE   float
// TEXCOORD                 2   xyzw        2     NONE   float   xyzw
// TEXCOORD                 3   xyzw        3     NONE   float   xyzw
// TEXCOORD                 4   xyzw        4     NONE   float   xyzw
// TEXCOORD                 5   xyzw        5     NONE   float   xy w
// TEXCOORD                 6   xyzw        6     NONE   float   xy w
// TEXCOORD                 7   xyzw        7     NONE   float   xyz
// TEXCOORD                 8   xyz         8     NONE   float   xyz
// SV_POSITION              0   xyzw        9      POS   float
// SV_IsFrontFace           0   x          10    FFACE    uint   x
//
//
// Output signature:
//
// Name                 Index   Mask Register SysValue  Format   Used
// -------------------- ----- ------ -------- -------- ------- ------
// SV_Target                0   xyzw        0   TARGET   float   xyzw
// SV_Target                1   xyzw        1   TARGET   float   xyzw
// SV_Target                2   xyzw        2   TARGET   float   xyzw
// SV_Target                3   xyzw        3   TARGET   float   xyzw
//
ps_5_0
dcl_globalFlags refactoringAllowed
dcl_constantbuffer CB0[209], immediateIndexed
dcl_constantbuffer CB1[29], immediateIndexed
dcl_constantbuffer CB2[27], dynamicIndexed
dcl_constantbuffer CB3[41], immediateIndexed
dcl_constantbuffer CB4[170], dynamicIndexed
dcl_sampler s0, mode_default
dcl_sampler s1, mode_comparison
dcl_sampler s2, mode_default
dcl_resource_texture2darray (float,float,float,float) t0
dcl_resource_structured t1, 128
dcl_resource_texture2d (float,float,float,float) t2
dcl_resource_texture2d (float,float,float,float) t3
dcl_resource_texture2d (float,float,float,float) t4
dcl_resource_texture2d (float,float,float,float) t5
dcl_resource_texture2d (float,float,float,float) t6
dcl_resource_texture2darray (float,float,float,float) t7
dcl_resource_texture2d (float,float,float,float) t8
dcl_input_ps linear v0.xyzw
dcl_input_ps linear v2.xyzw
dcl_input_ps linear v3.xyzw
dcl_input_ps linear v4.xyzw
dcl_input_ps linear v5.xyw
dcl_input_ps linear v6.xyw
dcl_input_ps linear v7.xyz
dcl_input_ps linear v8.xyz
dcl_input_ps_sgv constant v10.x, is_front_face
dcl_output o0.xyzw
dcl_output o1.xyzw
dcl_output o2.xyzw
dcl_output o3.xyzw
dcl_temps 22
movc r0.x, v10.x, l(1.000000), l(-1.000000)
lt r0.y, l(0.500000), cb1[28].y
div r0.zw, v5.xxxy, v5.wwww
div r1.xy, v6.xyxx, v6.wwww
add r1.xy, r0.zwzz, -r1.xyxx
mul r0.zw, r1.xxxy, l(0.000000, 0.000000, 0.500000, -0.500000)
sqrt r0.zw, |r0.zzzw|
mov r1.z, -r1.y
lt r1.yw, l(0.000000, 0.000000, 0.000000, 0.000000), r1.xxxz
lt r1.xz, r1.xxzx, l(0.000000, 0.000000, 0.000000, 0.000000)
iadd r1.xy, -r1.ywyy, r1.xzxx
itof r1.xy, r1.xyxx
mul r0.zw, r0.zzzw, r1.xxxy
mad r0.zw, r0.zzzw, l(0.000000, 0.000000, 0.500000, 0.500000), l(0.000000, 0.000000, 0.498039216, 0.498039216)
movc o2.xy, r0.yyyy, r0.zwzz, l(0.498000,0.498000,0,0)
eq r0.y, cb3[40].x, l(1.000000)
ne r0.zw, l(0.000000, 0.000000, 0.000000, 0.000000), cb4[154].wwwz
ne r1.x, l(0.000000, 0.000000, 0.000000, 0.000000), cb4[155].x
bfi r1.x, l(1), l(3), r1.x, l(0)
bfi r0.yw, l(0, 1, 0, 1), l(0, 2, 0, 1), r0.yyyw, l(0, 0, 0, 0)
iadd r0.y, r1.x, r0.y
iadd r0.y, r0.y, r0.w
bfi r0.y, l(1), l(0), r0.z, r0.y
utof r0.y, r0.y
mul o2.z, r0.y, l(0.00392156886)
lt r0.yz, l(0.000000, 0.500000, 0.500000, 0.000000), cb4[150].xxyx
ieq r0.w, v10.x, l(0)
and r0.y, r0.w, r0.y
lt r0.w, l(1.000000), v0.z
and r0.z, r0.w, r0.z
or r0.z, r0.z, r0.y
movc r0.zw, r0.zzzz, v0.zzzw, v0.xxxy
sample_b_indexable(texture2d)(float,float,float,float) r1.xyz, r0.zwzz, t3.xyzw, s0, cb0[199].x
mul r2.xyz, r1.xyzx, cb4[58].xyzx
movc r0.yz, r0.yyyy, v0.zzwz, v0.xxyx
sample_b_indexable(texture2d)(float,float,float,float) r3.xyz, r0.yzyy, t4.xyzw, s0, cb0[199].x
mov_sat r3.xyz, r3.xyzx
mad r3.xyz, r3.xyzx, l(2.000000, 2.000000, 2.000000, 0.000000), l(-1.004000, -1.004000, -1.000000, 0.000000)
mul r3.xy, r3.xyxx, cb4[140].yyyy
dp2 r0.w, r3.xyxx, r3.xyxx
min r0.w, r0.w, l(1.000000)
add r0.w, -r0.w, l(1.000000)
sqrt r0.w, r0.w
mul r0.x, r0.x, r0.w
mul r4.xyz, r3.yyyy, v4.xyzx
mad r3.xyw, r3.xxxx, v3.xyxz, r4.xyxz
mad r3.xyw, r0.xxxx, v2.xyxz, r3.xyxw
dp3 r0.x, r3.xywx, r3.xywx
rsq r0.x, r0.x
mul r3.xyw, r0.xxxx, r3.xyxw
sample_b_indexable(texture2d)(float,float,float,float) r4.xyz, r0.yzyy, t5.zxyw, s0, cb0[199].x
mov_sat r4.xyz, r4.xyzx
mul r0.x, r4.z, cb4[140].z
sample_b_indexable(texture2d)(float,float,float,float) r0.yzw, r0.yzyy, t6.wxyz, s0, cb0[199].x
mov_sat r0.yzw, r0.yyzw
lt r5.xy, l(0.500000, 0.500000, 0.000000, 0.000000), cb4[147].xyxx
and r1.w, r5.y, r5.x
add r2.w, r0.w, l(-0.200000)
mul r2.w, r2.w, l(1.250000)
max r2.w, r2.w, l(0.000000)
movc r2.w, r1.w, r2.w, r0.w
and r2.w, r2.w, r5.x
mul r4.w, r4.y, l(5.000000)
round_ni r4.w, r4.w
add r4.w, -r4.w, l(4.000000)
max r4.w, r4.w, l(0.000000)
ftoi r4.w, r4.w
ieq r5.x, r4.w, cb4[135].y
and r5.y, r5.x, l(1)
lt r5.z, l(0.500000), cb4[139].w
mul r6.xy, v0.xyxx, cb4[140].xxxx
sample_b_indexable(texture2d)(float,float,float,float) r6.xyz, r6.xyxx, t2.xyzw, s0, cb0[199].x
mad r1.xyz, r1.xyzx, cb4[58].xyzx, r6.xyzx
add r1.xyz, r1.xyzx, l(-0.500000, -0.500000, -0.500000, 0.000000)
max r1.xyz, r1.xyzx, l(0.000000, 0.000000, 0.000000, 0.000000)
movc r1.xyz, r5.zzzz, r1.xyzx, r2.xyzx
lt r6.xyzw, r4.yyyy, l(0.200000, 0.400000, 0.600000, 0.800000)
movc r2.x, r6.w, cb4[137].x, cb4[136].w
movc r2.x, r6.z, cb4[137].y, r2.x
movc r2.x, r6.y, cb4[137].z, r2.x
movc r2.x, r6.x, cb4[137].w, r2.x
mov r7.x, v2.w
mov r7.y, v3.w
mov r7.z, v4.w
add r8.xyz, -r7.xyzx, cb0[53].xyzx
dp3 r2.y, r8.xyzx, r8.xyzx
max r2.z, r2.y, l(1.175494351E-38)
rsq r2.z, r2.z
mul r9.xyz, r2.zzzz, r8.xyzx
sqrt r4.y, r2.y
ilt r5.z, l(0), cb0[196].x
if_nz r5.z
  ftoi r5.w, cb3[2].z
  iadd r7.w, cb0[196].x, l(-1)
  imin r5.w, r5.w, r7.w
  imax r5.w, r5.w, l(0)
  ld_structured_indexable(structured_buffer, stride=128)(mixed,mixed,mixed,mixed) r10.xyzw, r5.w, l(0), t1.xyzw
  ld_structured_indexable(structured_buffer, stride=128)(mixed,mixed,mixed,mixed) r11.xyzw, r5.w, l(16), t1.xyzw
  ld_structured_indexable(structured_buffer, stride=128)(mixed,mixed,mixed,mixed) r12.y, r5.w, l(32), t1.xxxx
  mov r12.x, r11.w
else
  mov r12.xy, l(0,0,0,0)
  mov r10.xyzw, l(0,0,0,0)
  mov r11.xyz, l(0,0,0,0)
endif
add r11.xyz, -r7.xyzx, r11.xyzx
dp3 r5.w, r11.xyzx, r11.xyzx
max r5.w, r5.w, l(1.175494351E-38)
rsq r7.w, r5.w
mul r8.w, r10.w, r10.w
div r5.w, r5.w, r8.w
add r5.w, -r5.w, l(1.000000)
max r5.w, r5.w, l(0.000000)
add r10.xyz, r10.xyzx, -cb0[197].xyzx
mad r10.xyz, r5.wwww, r10.xyzx, cb0[197].xyzx
add r8.w, r5.w, l(-1.000000)
mad r5.w, r5.w, r8.w, l(1.000000)
lt r8.w, l(0.500000), cb0[22].x
if_nz r8.w
  movc r8.w, r6.w, cb4[166].x, cb4[165].w
  movc r8.w, r6.z, cb4[166].y, r8.w
  movc r8.w, r6.y, cb4[166].z, r8.w
  movc r8.w, r6.x, cb4[166].w, r8.w
  mad r13.xyz, r3.xywx, cb4[138].xxxx, r7.xyzx
  add r13.xyz, r13.xyzx, -cb3[39].xyzx
  mul r14.xyz, r13.yyyy, cb3[34].xyzx
  mad r13.xyw, cb3[33].xyxz, r13.xxxx, r14.xyxz
  mad r13.xyz, cb3[35].xyzx, r13.zzzz, r13.xywx
  add r13.xyz, r13.xyzx, cb3[36].xyzx
  mad r12.zw, r13.xxxy, cb3[38].xxxy, cb3[38].zzzw
  add r13.xy, r12.zwzz, -cb0[208].xyxx
  sample_c_lz_indexable(texture2d)(float,float,float,float) r9.w, r13.xyxx, t8.xxxx, s1, r13.z
  mad r14.xyzw, cb0[208].xyxy, l(-1.000000, 1.000000, 1.000000, -1.000000), r12.zwzw
  sample_c_lz_indexable(texture2d)(float,float,float,float) r10.w, r14.xyxx, t8.xxxx, s1, r13.z
  add r9.w, r9.w, r10.w
  sample_c_lz_indexable(texture2d)(float,float,float,float) r10.w, r14.zwzz, t8.xxxx, s1, r13.z
  add r9.w, r9.w, r10.w
  add r13.xy, r12.zwzz, cb0[208].xyxx
  sample_c_lz_indexable(texture2d)(float,float,float,float) r10.w, r13.xyxx, t8.xxxx, s1, r13.z
  add r9.w, r9.w, r10.w
  mad r14.xyzw, cb0[208].xyxy, l(-1.414213, 0.000000, 1.414213, 0.000000), r12.zwzw
  sample_c_lz_indexable(texture2d)(float,float,float,float) r10.w, r14.xyxx, t8.xxxx, s1, r13.z
  add r9.w, r9.w, r10.w
  sample_c_lz_indexable(texture2d)(float,float,float,float) r10.w, r14.zwzz, t8.xxxx, s1, r13.z
  add r9.w, r9.w, r10.w
  mad r14.xyzw, cb0[208].xyxy, l(0.000000, -1.414213, 0.000000, 1.414213), r12.zwzw
  sample_c_lz_indexable(texture2d)(float,float,float,float) r10.w, r14.xyxx, t8.xxxx, s1, r13.z
  add r9.w, r9.w, r10.w
  sample_c_lz_indexable(texture2d)(float,float,float,float) r10.w, r14.zwzz, t8.xxxx, s1, r13.z
  add r9.w, r9.w, r10.w
  sample_c_lz_indexable(texture2d)(float,float,float,float) r10.w, r12.zwzz, t8.xxxx, s1, r13.z
  add r9.w, r9.w, r10.w
  mad r9.w, r9.w, l(0.111100), l(-1.000000)
  mul r8.w, r8.w, r9.w
  mul r8.w, r8.w, cb3[37].x
  add r13.xyz, r7.xyzx, -cb2[20].xyzx
  add r14.xyz, r7.xyzx, -cb2[21].xyzx
  add r15.xyz, r7.xyzx, -cb2[22].xyzx
  add r16.xyz, r7.xyzx, -cb2[23].xyzx
  dp3 r13.x, r13.xyzx, r13.xyzx
  dp3 r13.y, r14.xyzx, r14.xyzx
  dp3 r13.z, r15.xyzx, r15.xyzx
  dp3 r13.w, r16.xyzx, r16.xyzx
  lt r13.xyzw, r13.xyzw, cb2[24].xyzw
  and r14.xyzw, r13.xyzw, l(0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000)
  movc r13.xyz, r13.xyzx, l(-1.000000,-1.000000,-1.000000,0), l(-0.000000,-0.000000,-0.000000,0)
  add r13.xyz, r13.xyzx, r14.yzwy
  max r14.yzw, r13.xxyz, l(0.000000, 0.000000, 0.000000, 0.000000)
  dp4 r9.w, r14.xyzw, l(4.000000, 3.000000, 2.000000, 1.000000)
  add r13.z, -r9.w, l(4.000000)
  ftou r9.w, r13.z
  ishl r9.w, r9.w, l(2)
  mul r14.xyz, v3.wwww, cb2[r9.w + 1].xyzx
  mad r14.xyz, cb2[r9.w + 0].xyzx, v2.wwww, r14.xyzx
  mad r14.xyz, cb2[r9.w + 2].xyzx, v4.wwww, r14.xyzx
  add r14.xyz, r14.xyzx, cb2[r9.w + 3].xyzx
  mul r12.zw, r14.xxxy, l(0.000000, 0.000000, 1024.000000, 1024.000000)
  frc r12.zw, r12.zzzw
  dp2 r9.w, r12.zwzz, l(12.989800, 78.233002, 0.000000, 0.000000)
  sincos r9.w, null, r9.w
  mul r9.w, r9.w, l(43758.546875)
  frc r9.w, r9.w
  sincos r15.x, r16.x, r9.w
  mul r17.xyzw, cb2[26].xxyy, l(1.300000, 1.300000, 1.300000, 1.300000)
  mov r15.xz, r15.xxxx
  mov r15.yw, r16.xxxx
  mul r15.xyzw, r17.xyzw, r15.xyzw
  mul r16.xyzw, r15.xyzw, l(-0.172400, -0.978300, -0.978300, 0.172400)
  add r12.zw, r16.yyyw, r16.xxxz
  add r13.xy, r14.xyxx, r12.zwzz
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r9.w, r13.xyzx, t0.xxxx, s1, r14.z
  mul r16.xyzw, r15.xyzw, l(0.874700, 0.484600, 0.484600, -0.874700)
  add r12.zw, r16.yyyw, r16.xxxz
  add r13.xy, r14.xyxx, r12.zwzz
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r10.w, r13.xyzx, t0.xxxx, s1, r14.z
  add r9.w, r9.w, r10.w
  mul r16.xyzw, r15.xyzw, l(-0.968300, -0.037400, -0.037400, 0.968300)
  add r12.zw, r16.yyyw, r16.xxxz
  add r13.xy, r14.xyxx, r12.zwzz
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r10.w, r13.xyzx, t0.xxxx, s1, r14.z
  add r9.w, r9.w, r10.w
  mul r16.xyzw, r15.xyzw, l(0.278300, 0.419600, 0.419600, -0.278300)
  add r12.zw, r16.yyyw, r16.xxxz
  add r13.xy, r14.xyxx, r12.zwzz
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r10.w, r13.xyzx, t0.xxxx, s1, r14.z
  add r9.w, r9.w, r10.w
  mul r16.xyzw, r15.xyzw, l(-0.150700, 0.839100, 0.839100, -0.150700)
  add r12.zw, r16.yyyw, r16.xxxz
  add r13.xy, r14.xyxx, r12.zwzz
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r10.w, r13.xyzx, t0.xxxx, s1, r14.z
  add r9.w, r9.w, r10.w
  mul r16.xyzw, r15.xyzw, l(-0.641700, 0.479300, 0.479300, -0.641700)
  add r12.zw, r16.yyyw, r16.xxxz
  add r13.xy, r14.xyxx, r12.zwzz
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r10.w, r13.xyzx, t0.xxxx, s1, r14.z
  add r9.w, r9.w, r10.w
  mul r16.xyzw, r15.xyzw, l(0.577900, -0.816100, -0.816100, 0.577900)
  add r12.zw, r16.yyyw, r16.xxxz
  add r13.xy, r14.xyxx, r12.zwzz
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r10.w, r13.xyzx, t0.xxxx, s1, r14.z
  add r9.w, r9.w, r10.w
  mul r16.xyzw, r15.xyzw, l(-0.540900, -0.458800, -0.458800, 0.540900)
  add r12.zw, r16.yyyw, r16.xxxz
  add r13.xy, r14.xyxx, r12.zwzz
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r10.w, r13.xyzx, t0.xxxx, s1, r14.z
  add r9.w, r9.w, r10.w
  mul r16.xyzw, r15.xyzw, l(0.704400, -0.191900, -0.191900, 0.704400)
  add r12.zw, r16.yyyw, r16.xxxz
  add r13.xy, r14.xyxx, r12.zwzz
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r10.w, r13.xyzx, t0.xxxx, s1, r14.z
  add r9.w, r9.w, r10.w
  mul r16.xyzw, r15.xyzw, l(0.105300, -0.446400, -0.446400, 0.105300)
  add r12.zw, r16.yyyw, r16.xxxz
  add r13.xy, r14.xyxx, r12.zwzz
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r10.w, r13.xyzx, t0.xxxx, s1, r14.z
  add r9.w, r9.w, r10.w
  mul r15.xyzw, r15.xyzw, l(-0.206600, 0.066100, 0.066100, -0.206600)
  add r12.zw, r15.yyyw, r15.xxxz
  add r13.xy, r14.xyxx, r12.zwzz
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r10.w, r13.xyzx, t0.xxxx, s1, r14.z
  add r9.w, r9.w, r10.w
  mul r9.w, r9.w, cb2[25].x
  add r10.w, -cb2[25].x, l(1.000000)
  mad r9.w, r9.w, l(0.090900), r10.w
  ge r10.w, l(0.000000), r14.z
  ge r11.w, r14.z, l(1.000000)
  or r10.w, r10.w, r11.w
  movc r9.w, r10.w, l(1.000000), r9.w
  add r10.w, -r12.x, l(1.000000)
  mad r10.w, cb3[40].y, r10.w, r12.x
  mul r9.w, r9.w, r10.w
  mad_sat r10.w, r12.y, l(2.000000), l(-1.000000)
  mul r11.w, r10.w, cb0[197].w
  lt r12.x, l(0.500000), cb3[37].x
  mad r8.w, r10.w, r8.w, l(1.000000)
  min r8.w, r8.w, l(1.000000)
  movc r8.w, r12.x, r8.w, l(1.000000)
  mul r8.w, r9.w, r8.w
  add r9.w, -cb4[136].z, l(1.000000)
  mad r9.w, r8.w, cb4[136].z, r9.w
  mul r10.w, r11.w, cb4[136].z
  mad r11.w, -r11.w, cb4[136].z, l(1.000000)
  mad r8.w, r8.w, r10.w, r11.w
else
  mov r9.w, l(1.000000)
  mov r8.w, l(1.000000)
endif
mul_sat r10.w, cb3[1].w, l(2.500000)
add r11.w, -r10.w, l(1.000000)
mad r8.w, r11.w, r8.w, r10.w
add r10.xyz, r10.xyzx, l(-0.800000, -0.800000, -0.800000, 0.000000)
mad r10.xyz, cb4[58].wwww, r10.xyzx, l(0.800000, 0.800000, 0.800000, 0.000000)
rsq r2.y, r2.y
mul r12.xyz, r2.yyyy, r8.xyzx
mad r11.xyz, r11.xyzx, r7.wwww, -r12.xyzx
mad r11.xyz, cb4[58].wwww, r11.xyzx, r12.xyzx
ne r2.y, cb3[3].w, l(0.000000)
dp3 r7.x, cb3[3].xyzx, r7.xyzx
add_sat r7.x, r7.x, -cb3[3].w
mul r7.yzw, r10.xxyz, cb0[15].xxyz
add r12.xyz, r10.xyzx, cb0[15].xyzx
mad r12.xyz, -r10.xyzx, cb0[15].xyzx, r12.xyzx
mad r7.yzw, cb0[15].wwww, r12.xxyz, r7.yyzw
add r7.yzw, -r10.xxyz, r7.yyzw
mad r7.xyz, r7.xxxx, r7.yzwy, r10.xyzx
movc r7.xyz, r2.yyyy, r7.xyzx, r10.xyzx
max r2.x, r2.x, l(0.000010)
rcp r2.y, r2.x
dp3 r7.w, r3.xywx, r11.xyzx
add r10.x, r7.w, l(1.000000)
mul r10.y, r11.y, l(3.000000)
min r10.y, r10.y, l(1.000000)
mad r10.y, -r10.y, l(0.500000), r3.y
add_sat r10.y, r10.y, l(1.500000)
mad r10.x, r10.x, r10.y, l(-1.000000)
add r10.x, -r7.w, r10.x
mad r10.x, v7.y, r10.x, r7.w
mad r3.z, r3.z, l(2.000000), r10.x
mul r10.x, r3.z, l(3.000000)
mul r12.xy, r2.xxxx, l(1.500000, 4.500000, 0.000000, 0.000000)
mad r10.yz, r3.zzzz, l(0.000000, 3.000000, 3.000000, 0.000000), -r12.xxyx
add r10.xyz, r10.xyzx, l(3.000000, 1.000000, -1.000000, 0.000000)
mad r2.x, -r2.x, l(3.000000), l(2.000000)
div r10.xyw, r10.xyxz, r2.xxxx
add r12.xyz, -r10.xywx, l(1.000000, 1.000000, 1.000000, 0.000000)
add r13.xyz, r3.zzzz, l(0.333300, -0.333300, -0.333300, 0.000000)
mad r13.xyz, r2.yyyy, r13.xyzx, l(0.500000, 0.500000, -0.500000, 0.000000)
add r14.xyz, -r13.xyzx, l(1.000000, 1.000000, 1.000000, 0.000000)
min r15.xy, r10.yxyy, r14.yxyy
min r10.xz, r12.yyzy, r13.xxzx
mov r15.z, r12.x
mov r15.w, r10.x
mov_sat r12.xyz, r15.zywz
min_sat r15.y, r13.y, r14.z
mov_sat r15.x, r15.x
mov_sat r10.zw, r10.zzzw
add r2.x, -r8.w, r9.w
mad r2.x, cb3[40].y, r2.x, r8.w
mad r13.xyzw, r2.xxxx, l(2.000000, 2.000000, -2.000000, -2.000000), l(0.000000, -1.000000, 1.000000, 2.000000)
min_sat r13.x, r13.w, r13.x
mov_sat r13.yz, r13.yyzy
lt r2.x, l(0.500000), cb3[40].y
if_nz r2.x
  if_nz r5.z
    ftoi r2.x, cb3[2].z
    iadd r2.y, cb0[196].x, l(-1)
    imin r2.x, r2.y, r2.x
    imax r2.x, r2.x, l(0)
    ld_structured_indexable(structured_buffer, stride=128)(mixed,mixed,mixed,mixed) r2.x, r2.x, l(32), t1.xxxx
  else
    mov r2.x, l(0)
  endif
  mul r2.x, r2.x, cb0[197].w
  mul r14.y, r2.x, r13.x
  mad r10.xy, -r13.zxzz, r2.xxxx, r13.zxzz
  add r2.y, r10.y, r10.x
  add r14.z, r2.y, r13.y
  if_nz r5.z
    ftoi r2.y, cb3[2].z
    iadd r10.x, cb0[196].x, l(-1)
    imin r2.y, r2.y, r10.x
    imax r2.y, r2.y, l(0)
    ld_structured_indexable(structured_buffer, stride=128)(mixed,mixed,mixed,mixed) r2.y, r2.y, l(28), t1.xxxx
  else
    mov r2.y, l(0)
  endif
  mul r13.xy, r2.yyyy, r14.yzyy
  mad r10.xy, -r14.yzyy, r2.yyyy, r14.yzyy
  add r2.y, r10.y, r10.x
  mad r13.z, r13.z, r2.x, r2.y
else
  if_nz r5.z
    ftoi r2.x, cb3[2].z
    iadd r2.y, cb0[196].x, l(-1)
    imin r2.x, r2.y, r2.x
    imax r2.x, r2.x, l(0)
    ld_structured_indexable(structured_buffer, stride=128)(mixed,mixed,mixed,mixed) r2.x, r2.x, l(32), t1.xxxx
  else
    mov r2.x, l(0)
  endif
  mul r13.xz, r2.xxxx, r13.xxzx
endif
add r2.x, -r12.x, l(1.000000)
add r2.x, -r12.y, r2.x
add r2.x, -r12.z, r2.x
mad r2.x, r13.z, r2.x, r12.z
add r2.y, r13.y, r13.x
mul r10.xy, r2.yyyy, r15.xyxx
add r2.y, r10.w, r10.z
mad r2.y, r2.y, r13.x, r10.y
mul r10.y, r10.z, r13.y
mul r10.z, r12.x, v7.x
mad r11.w, -r12.x, v7.x, r12.x
add r11.w, r11.w, r12.y
movc r12.xyz, r6.wwww, cb4[61].xyzx, cb4[60].xyzx
movc r12.xyz, r6.zzzz, cb4[62].xyzx, r12.xyzx
movc r12.xyz, r6.yyyy, cb4[63].xyzx, r12.xyzx
movc r12.xyz, r6.xxxx, cb4[64].xyzx, r12.xyzx
movc r13.xzw, r6.wwww, cb4[66].xxyz, cb4[65].xxyz
movc r13.xzw, r6.zzzz, cb4[67].xxyz, r13.xxzw
movc r13.xzw, r6.yyyy, cb4[68].xxyz, r13.xxzw
movc r13.xzw, r6.xxxx, cb4[69].xxyz, r13.xxzw
movc r14.xyz, r5.xxxx, cb0[10].xyzx, cb0[3].xyzx
movc r15.xyz, r5.xxxx, cb0[11].xyzx, cb0[4].xyzx
movc r16.xyz, r5.xxxx, cb0[12].xyzx, cb0[5].xyzx
movc r17.xyz, r5.xxxx, cb0[9].xyzx, cb0[6].xyzx
movc r18.xyz, r5.xxxx, cb0[13].xyzx, cb0[7].xyzx
movc r19.xyz, r5.xxxx, cb0[14].xyzx, cb0[8].xyzx
mul r12.w, r4.y, l(0.437250)
min r12.w, r12.w, l(1.000000)
mad r20.x, r12.w, cb3[1].w, -r12.w
mad r20.y, -r12.w, cb3[1].w, r12.w
add r20.xy, r20.xyxx, l(1.000000, -1.000000, 0.000000, 0.000000)
mad r20.xy, cb4[136].yyyy, r20.xyxx, l(0.000000, 1.000000, 0.000000, 0.000000)
add r12.xyz, r12.xyzx, l(0.0000610351562, 0.0000610351562, 0.0000610351562, 0.000000)
add r12.w, r12.y, r12.x
add r12.w, r12.z, r12.w
mul r12.w, r12.w, l(0.333330)
div_sat r21.xyz, r12.xyzx, r12.wwww
mul r12.xyz, r20.yyyy, r12.xyzx
mad r12.xyz, r21.xyzx, r20.xxxx, r12.xyzx
add r13.xzw, r13.xxzw, l(0.0000610351562, 0.000000, 0.0000610351562, 0.0000610351562)
add r12.w, r13.z, r13.x
add r12.w, r13.w, r12.w
mul r12.w, r12.w, l(0.333330)
div_sat r21.xyz, r13.xzwx, r12.wwww
mul r13.xzw, r20.yyyy, r13.xxzw
mad r13.xzw, r21.xxyz, r20.xxxx, r13.xxzw
mul r14.xyz, r14.xyzx, r12.xyzx
mul r12.xyz, r15.xyzx, r12.xyzx
mul r15.xyz, r16.xyzx, r13.xzwx
mul r13.xzw, r19.xxyz, r13.xxzw
add r16.xyz, r7.xyzx, l(1.175494351E-38, 1.175494351E-38, 1.175494351E-38, 0.000000)
max r12.w, r16.y, r16.x
max r12.w, r16.z, r12.w
rcp r12.w, r12.w
if_nz r5.z
  ftoi r14.w, cb3[2].z
  iadd r15.w, cb0[196].x, l(-1)
  imin r14.w, r14.w, r15.w
  imax r14.w, r14.w, l(0)
  ld_structured_indexable(structured_buffer, stride=128)(mixed,mixed,mixed,mixed) r14.w, r14.w, l(32), t1.xxxx
else
  mov r14.w, l(0)
endif
add r15.w, -r5.w, l(1.000000)
mad r5.w, r14.w, r15.w, r5.w
mul r16.xyz, r5.wwww, r7.xyzx
min r5.w, r12.w, l(1.000000)
mul r19.xyz, r5.wwww, r16.xyzx
mul r17.xyz, r10.yyyy, r17.xyzx
mad r17.xyz, r18.xyzx, r2.yyyy, r17.xyzx
mad r17.xyz, r10.wwww, r13.yyyy, r17.xyzx
mul r15.xyz, r11.wwww, r15.xyzx
mad r10.yzw, r13.xxzw, r10.zzzz, r15.xxyz
mad r10.yzw, r12.xxyz, r2.xxxx, r10.yyzw
mad r10.xyz, r14.xyzx, r10.xxxx, r10.yzwy
mul r10.xyz, r19.xyzx, r10.xyzx
mad r10.xyz, r16.xyzx, r17.xyzx, r10.xyzx
add r10.xyz, -r7.xyzx, r10.xyzx
mad r7.xyz, cb4[58].wwww, r10.xyzx, r7.xyzx
ge r2.x, cb0[22].z, l(0.500000)
if_nz r2.x
  lt r2.x, cb4[r4.w + 10].x, l(50.000000)
  if_nz r2.x
    mul r2.x, r0.w, l(5.100000)
    min r2.x, r2.x, l(1.000000)
    ge r2.y, l(0.200000), r0.w
    and r2.y, r2.y, l(0x3f800000)
    mul r2.x, r2.y, r2.x
    movc r0.w, r1.w, r2.x, r0.w
    ge r1.w, cb4[147].y, l(0.500000)
    and r1.w, r1.w, l(0x3f800000)
    add r0.w, r0.w, l(-1.000000)
    mad r0.w, r1.w, r0.w, l(1.000000)
    mul r2.xy, r3.yyyy, cb0[118].xyxx
    mad r2.xy, cb0[117].xyxx, r3.xxxx, r2.xyxx
    mad r2.xy, cb0[119].xyxx, r3.wwww, r2.xyxx
    mad r2.xy, r2.xyxx, l(0.500000, 0.500000, 0.000000, 0.000000), l(0.500000, 0.500000, 0.000000, 0.000000)
    lt r1.w, l(0.500000), cb4[r4.w + 15].z
    if_nz r1.w
      mul r0.y, r0.y, cb4[r4.w + 15].w
      mad r10.xy, cb4[r4.w + 0].xyxx, v0.xyxx, cb4[r4.w + 0].zwzz
      mad r2.xy, r0.yyyy, r2.xyxx, r10.xyxx
    endif
    mul r10.x, cb0[40].y, cb4[r4.w + 10].w
    mul r10.y, cb0[40].y, cb4[r4.w + 15].x
    add r10.xy, r2.xyxx, r10.xyxx
    mov r10.z, cb4[r4.w + 10].x
    sample_indexable(texture2darray)(float,float,float,float) r10.xyzw, r10.xyzx, t7.xyzw, s2
    mul r12.xyz, r10.xyzx, cb4[r4.w + 5].xyzx
    mul r0.y, r0.w, r10.w
    lt r0.w, cb4[r4.w + 15].y, l(0.500000)
    if_nz r0.w
      mul_sat r0.w, r0.y, cb4[r4.w + 10].z
      mad r13.xyz, r12.xyzx, cb4[r4.w + 10].yyyy, -r1.xyzx
      mad r1.xyz, r0.wwww, r13.xyzx, r1.xyzx
    else
      lt r0.w, cb4[r4.w + 15].y, l(1.500000)
      if_nz r0.w
        mul_sat r0.w, r0.y, cb4[r4.w + 10].z
        mul r13.xyz, r0.wwww, r12.xyzx
        mad r1.xyz, r13.xyzx, cb4[r4.w + 10].yyyy, r1.xyzx
      else
        mad r10.xyz, r10.xyzx, cb4[r4.w + 5].xyzx, l(-0.500000, -0.500000, -0.500000, 0.000000)
        mad_sat r10.xyz, r10.xyzx, cb4[r4.w + 10].yyyy, r12.xyzx
        mul_sat r0.y, r0.y, cb4[r4.w + 10].z
        add r10.xyz, r10.xyzx, l(-0.500000, -0.500000, -0.500000, 0.000000)
        mad r10.xyz, r0.yyyy, r10.xyzx, l(0.500000, 0.500000, 0.500000, 0.000000)
        mul r12.xyz, r1.xyzx, r10.xyzx
        add r13.xyz, r12.xyzx, r12.xyzx
        add r14.xyz, -r1.xyzx, l(1.000000, 1.000000, 1.000000, 0.000000)
        add r14.xyz, r14.xyzx, r14.xyzx
        add r10.xyz, -r10.xyzx, l(1.000000, 1.000000, 1.000000, 0.000000)
        mad r10.xyz, -r14.xyzx, r10.xyzx, l(1.000000, 1.000000, 1.000000, 0.000000)
        ge r14.xyz, r1.xyzx, l(0.500000, 0.500000, 0.500000, 0.000000)
        and r14.xyz, r14.xyzx, l(0x3f800000, 0x3f800000, 0x3f800000, 0)
        mad r10.xyz, -r12.xyzx, l(2.000000, 2.000000, 2.000000, 0.000000), r10.xyzx
        mad r1.xyz, r14.xyzx, r10.xyzx, r13.xyzx
      endif
    endif
  endif
endif
lt r0.y, l(0.500000), v7.z
if_z r5.x
  movc r10.xyz, r0.yyyy, r3.xywx, v2.xyzx
  dp3 r0.w, r1.xyzx, l(0.290000, 0.600000, 0.110000, 0.000000)
  lt r1.w, v7.z, l(0.500000)
  mad r2.xy, r0.wwww, l(0.287500, 0.400000, 0.000000, 0.000000), l(1.437500, 1.000000, 0.000000, 0.000000)
  dp3 r4.w, r11.xyzx, r10.xyzx
  add r5.x, -r7.w, r4.w
  mad_sat r5.x, -r5.x, l(3.000000), l(1.000000)
  add r5.w, r5.x, r5.x
  sqrt r5.x, r5.x
  mul r5.x, r5.x, r5.w
  min r5.x, r5.x, l(1.000000)
  mad r5.w, r7.w, l(0.500000), l(0.500000)
  mov_sat r10.x, r7.w
  mad r5.x, r5.w, r5.x, -r10.x
  mad r5.x, r5.x, l(0.500000), r10.x
  mov_sat r4.w, r4.w
  max r5.w, r1.z, r1.y
  max r5.w, r1.x, r5.w
  lt r10.x, l(1.000000), r5.w
  div r10.yzw, r1.xxyz, r5.wwww
  movc r10.xyz, r10.xxxx, r10.yzwy, r1.xyzx
  add r5.w, -r2.x, l(1.000000)
  mad r2.x, r5.x, r5.w, r2.x
  log r10.xyz, r10.xyzx
  mul r10.xyz, r2.xxxx, r10.xyzx
  exp r10.xyz, r10.xyzx
  add r12.xyz, -r1.xyzx, r10.xyzx
  mad r12.xyz, r12.xyzx, l(0.500000, 0.500000, 0.500000, 0.000000), r1.xyzx
  add r10.xyz, r10.xyzx, -r12.xyzx
  mad r10.xyz, r4.wwww, r10.xyzx, r12.xyzx
  mad r0.w, -r0.w, l(0.050000), l(1.050000)
  log r12.xyz, r1.xyzx
  mul r12.xyz, r2.yyyy, r12.xyzx
  exp r12.xyz, r12.xyzx
  mul r12.xyz, r0.wwww, r12.xyzx
  movc r1.xyz, r1.wwww, r10.xyzx, r12.xyzx
endif
mad r0.w, -r0.x, l(0.960000), l(0.960000)
mul r10.xyz, r0.wwww, r1.xyzx
add r12.xyz, r1.xyzx, l(-0.040000, -0.040000, -0.040000, 0.000000)
mad r12.xyz, r0.xxxx, r12.xyzx, l(0.040000, 0.040000, 0.040000, 0.000000)
mad r1.w, -r0.z, cb4[140].w, l(1.000000)
mul r1.w, r1.w, r1.w
mad r2.x, r1.w, l(4.000000), l(2.000000)
mul r2.y, r1.w, r1.w
mad r4.w, r1.w, r1.w, l(-1.000000)
add r13.xyz, r7.xyzx, cb0[2].xyzx
add r13.xyz, r13.xyzx, v8.xyzx
dp3 r5.x, r13.xyzx, l(0.212672904, 0.715152204, 0.072175, 0.000000)
add r5.w, -cb0[19].x, cb0[19].y
div r10.w, l(1.000000, 1.000000, 1.000000, 1.000000), r5.w
mad r11.w, -cb0[19].x, r10.w, l(1.000000)
mad r10.w, r5.x, r10.w, r11.w
rcp r10.w, r10.w
mad r5.w, -r5.w, r10.w, cb0[19].y
lt r10.w, r5.x, cb0[19].x
movc r5.w, r10.w, r5.x, r5.w
add r5.x, r5.x, l(0.000100)
div r5.x, r5.w, r5.x
mul r13.xyz, r5.xxxx, r7.xyzx
movc r14.xyz, r6.wwww, cb4[76].xyzx, cb4[75].xyzx
movc r14.xyz, r6.zzzz, cb4[77].xyzx, r14.xyzx
movc r14.xyz, r6.yyyy, cb4[78].xyzx, r14.xyzx
movc r14.xyz, r6.xxxx, cb4[79].xyzx, r14.xyzx
movc r5.w, r6.w, cb4[144].z, cb4[144].y
movc r5.w, r6.z, cb4[144].w, r5.w
movc r5.w, r6.y, cb4[145].x, r5.w
movc r5.w, r6.x, cb4[145].y, r5.w
lt r10.w, l(0.500000), r5.w
if_nz r10.w
  mad_sat r3.z, r3.z, l(1.500000), l(-0.500000)
  movc r10.w, r6.w, cb4[145].w, cb4[145].z
  movc r10.w, r6.z, cb4[146].x, r10.w
  movc r10.w, r6.y, cb4[146].y, r10.w
  movc r10.w, r6.x, cb4[146].z, r10.w
  add r3.z, r3.z, r4.x
  add r3.z, r3.z, l(-1.000000)
  max r10.w, r10.w, l(0.000010)
  div_sat r4.x, r3.z, r10.w
endif
mul r3.z, r4.x, cb4[146].w
mul r14.xyz, r14.xyzx, r3.zzzz
mul r14.xyz, r12.xyzx, r14.xyzx
lt r3.z, r5.w, l(0.500000)
mad r8.xyz, r8.xyzx, r2.zzzz, r11.xyzx
dp3 r2.z, r8.xyzx, r8.xyzx
rsq r2.z, r2.z
mul r8.xyz, r2.zzzz, r8.xyzx
movc r2.z, r6.w, cb4[143].y, cb4[143].x
movc r2.z, r6.z, cb4[143].z, r2.z
movc r2.z, r6.y, cb4[143].w, r2.z
movc r2.z, r6.x, cb4[144].x, r2.z
mul r4.x, r7.w, r2.z
mad_sat r4.x, r4.x, l(0.750000), l(0.250000)
dp3 r5.w, r3.xywx, r8.xyzx
mul r5.w, r2.z, r5.w
mad_sat r5.w, r5.w, l(0.750000), l(0.250000)
dp3 r8.x, r11.xyzx, r8.xyzx
mul r2.z, r2.z, r8.x
mad_sat r2.z, r2.z, l(0.750000), l(0.250000)
mul r5.w, r5.w, r5.w
mad r4.w, r5.w, r4.w, l(1.000010)
mul r2.z, r2.z, r2.z
mul r4.w, r4.w, r4.w
max r2.z, r2.z, l(0.100000)
mul r2.z, r2.z, r4.w
mul r2.x, r2.x, r2.z
div r2.x, r2.y, r2.x
mad_sat r0.z, -r0.z, cb4[140].w, r2.x
mul r0.z, r4.x, r0.z
max r1.w, r1.w, l(0.000010)
div r0.z, r0.z, r1.w
movc r1.w, r6.w, cb4[142].x, cb4[141].w
movc r1.w, r6.z, cb4[142].y, r1.w
movc r1.w, r6.y, cb4[142].z, r1.w
movc r1.w, r6.x, cb4[142].w, r1.w
movc r2.x, r6.w, cb4[169].x, cb4[168].w
movc r2.x, r6.z, cb4[169].y, r2.x
movc r2.x, r6.y, cb4[169].z, r2.x
movc r2.x, r6.x, cb4[169].w, r2.x
mul r1.w, r1.w, r2.x
mul r0.z, r0.z, r1.w
mul_sat r0.z, r0.z, l(10.000000)
mul r0.z, r0.z, l(100.000000)
movc r0.z, r3.z, r0.z, l(16.667000)
mul r2.xyz, r14.xyzx, r0.zzzz
mul r8.xyz, r13.xyzx, r2.xyzx
mad r2.xyz, r2.xyzx, r13.xyzx, l(-1.000000, -1.000000, -1.000000, 0.000000)
max r2.xyz, r2.xyzx, l(0.000000, 0.000000, 0.000000, 0.000000)
mad r8.xyz, r10.xyzx, r13.xyzx, r8.xyzx
ge r0.z, cb4[147].x, l(0.500000)
movc r14.xyz, r6.wwww, cb4[81].xyzx, cb4[80].xyzx
movc r14.xyz, r6.zzzz, cb4[82].xyzx, r14.xyzx
movc r14.xyz, r6.yyyy, cb4[83].xyzx, r14.xyzx
movc r14.xyz, r6.xxxx, cb4[84].xyzx, r14.xyzx
mul r14.xyz, r2.wwww, r14.xyzx
mul r14.xyz, r1.xyzx, r14.xyzx
and r14.xyz, r0.zzzz, r14.xyzx
add r0.z, r14.y, r14.x
add r0.z, r14.z, r0.z
mul r15.xyz, r5.xxxx, v8.xyzx
mad r8.xyz, r15.xyzx, r10.xyzx, r8.xyzx
if_nz r5.z
  ftoi r1.w, cb3[2].z
  iadd r2.w, cb0[196].x, l(-1)
  imin r1.w, r1.w, r2.w
  imax r1.w, r1.w, l(0)
  ld_structured_indexable(structured_buffer, stride=128)(mixed,mixed,mixed,mixed) r16.xyz, r1.w, l(96), t1.xyzx
  ld_structured_indexable(structured_buffer, stride=128)(mixed,mixed,mixed,mixed) r17.xyz, r1.w, l(112), t1.xyzx
else
  mov r16.xyz, l(0,0,0,0)
  mov r17.xyz, l(0,0,0,0)
endif
lt r1.w, l(0.500000), cb0[23].y
movc r16.xyz, r5.yyyy, r16.xyzx, r17.xyzx
movc r16.xyz, r1.wwww, l(0.050000,0.050000,0.050000,0), r16.xyzx
mul r1.xyw, r1.xyxz, r5.xxxx
mad r1.xyw, r16.xyxz, r1.xyxw, r8.xyxz
add r1.xyw, r2.xyxz, r1.xyxw
ge r2.x, l(0.500000), cb0[196].y
if_nz r2.x
  dp3 r2.x, r9.xyzx, r11.xyzx
  mad_sat r2.x, -r2.x, l(0.500000), l(0.500000)
  mad r2.y, r2.x, l(0.800000), l(0.200000)
  mad r2.z, r3.y, l(0.500000), l(0.500000)
  mul r2.w, r2.z, r2.z
  movc r2.z, r5.y, r2.z, r2.w
  add r2.z, r2.z, l(-0.200000)
  mul_sat r2.z, r2.z, l(1.250000)
  mad r2.w, r2.z, l(-2.000000), l(3.000000)
  mul r2.xz, r2.xxzx, r2.xxzx
  mul r2.z, r2.z, r2.w
  mul r2.w, r2.z, r2.z
  mul r2.w, r2.w, r2.w
  mul r2.w, r2.w, r2.z
  movc r8.xyz, r5.yyyy, l(1.000000,0.300000,-1.000000,0), l(0.500000,1.000000,-0.500000,0)
  add r3.z, r8.z, r8.y
  mad r2.w, r2.w, r3.z, r8.x
  mad r2.z, r2.z, r2.w, l(-0.100000)
  mad r2.z, cb0[207].x, r2.z, l(0.100000)
  mad r2.w, r7.w, l(0.500000), l(0.500000)
  mul r2.w, r8.w, r2.w
  mad r2.w, r2.w, l(1.400000), l(0.100000)
  if_nz r5.z
    ftoi r3.z, cb3[2].z
    iadd r4.x, cb0[196].x, l(-1)
    imin r3.z, r3.z, r4.x
    imax r3.z, r3.z, l(0)
    ld_structured_indexable(structured_buffer, stride=128)(mixed,mixed,mixed,mixed) r3.z, r3.z, l(32), t1.xxxx
  else
    mov r3.z, l(0)
  endif
  mad r3.z, r3.z, l(0.399999976), r9.w
  add_sat r3.z, r3.z, l(0.600000)
  mul r2.y, r2.z, r2.y
  mul r2.y, r2.w, r2.y
  mul r2.y, r3.z, r2.y
  ne r2.z, l(0.000000, 0.000000, 0.000000, 0.000000), v7.z
  mov_sat r2.w, -r9.y
  mad r3.z, r4.z, cb4[140].z, l(2.500000)
  movc r2.z, r2.z, l(4.500000), r3.z
  mad r2.z, r2.w, r2.z, l(-0.500000)
  mad r2.z, cb0[207].y, r2.z, l(1.000000)
  mul r2.y, r2.z, r2.y
  mul r2.z, r4.y, l(0.0833333358)
  min r2.z, r2.z, l(1.000000)
  mad r2.zw, r2.zzzz, l(0.000000, 0.000000, -0.300000, -0.300000), l(0.000000, 0.000000, 0.500000, 0.600000)
  dp3 r3.z, r9.xyzx, r3.xywx
  add r3.z, -r3.z, l(1.000000)
  add r2.zw, -r2.zzzw, r3.zzzz
  mul_sat r2.zw, r2.zzzw, l(0.000000, 0.000000, 3.33333325, 5.00000048)
  mad r4.xz, r2.zzwz, l(-2.000000, 0.000000, -2.000000, 0.000000), l(3.000000, 0.000000, 3.000000, 0.000000)
  mul r2.zw, r2.zzzw, r2.zzzw
  mul r2.zw, r2.zzzw, r4.xxxz
  movc r2.z, r5.y, r2.z, r2.w
  dp3 r2.w, cb0[197].xyzx, l(0.330000, 0.330000, 0.330000, 0.000000)
  mul r4.xzw, cb0[197].xxyz, cb0[197].xxyz
  mul r4.xzw, r4.xxzw, r4.xxzw
  mul r4.xzw, r4.xxzw, r4.xxzw
  dp3 r3.z, r4.xzwx, l(0.700000, 0.700000, 0.700000, 0.000000)
  max r3.z, r3.z, l(0.010000)
  rcp r3.z, r3.z
  mul r2.w, r2.w, r3.z
  mad r4.xzw, r2.wwww, r4.xxzw, -r13.xxyz
  mad r4.xzw, r9.wwww, r4.xxzw, r13.xxyz
  log r2.x, r2.x
  mul r2.x, r2.x, l(20.000000)
  exp r2.x, r2.x
  mad r8.xyz, r7.xyzx, r5.xxxx, -r4.xzwx
  mad r4.xzw, r2.xxxx, r8.xxyz, r4.xxzw
  mad r4.xzw, -r7.xxyz, r5.xxxx, r4.xxzw
  mad r4.xzw, cb0[207].zzzz, r4.xxzw, r13.xxyz
  add r2.x, r10.y, r10.x
  mad r0.w, r1.z, r0.w, r2.x
  mul r0.w, r0.w, l(0.330000)
  mul r0.w, r0.w, r0.w
  mul r0.w, r0.w, cb0[207].w
  mad r0.w, r0.w, l(-0.199999988), l(1.000000)
  mov_sat r10.xyz, r10.xyzx
  log r5.xzw, r10.xxyz
  mul r5.xzw, r5.xxzw, l(0.200000, 0.000000, 0.200000, 0.200000)
  exp r5.xzw, r5.xxzw
  dp3 r1.z, r5.xzwx, r5.xzwx
  max r1.z, r1.z, l(0.0000610351562)
  rsq r1.z, r1.z
  mul r5.xzw, r1.zzzz, r5.xxzw
  mul r1.z, cb0[206].w, l(48.000000)
  mul r0.w, r0.w, l(0.100000)
  mul r7.xyz, r5.xzwx, r0.wwww
  mad r5.xzw, -r0.wwww, r5.xxzw, r12.xxyz
  mad r5.xzw, r0.xxxx, r5.xxzw, r7.xxyz
  mul r5.xzw, r1.zzzz, r5.xxzw
  mul r0.x, r2.z, r2.y
  mul r2.xyz, r4.xzwx, r0.xxxx
  mul r2.xyz, r5.xzwx, r2.xyzx
  movc r4.xzw, r6.wwww, cb4[88].xxyz, cb4[87].xxyz
  movc r4.xzw, r6.zzzz, cb4[89].xxyz, r4.xxzw
  movc r4.xzw, r6.yyyy, cb4[90].xxyz, r4.xxzw
  movc r4.xzw, r6.xxxx, cb4[91].xxyz, r4.xxzw
  mul r2.xyz, r2.xyzx, r4.xzwx
  mad_sat r0.x, r4.y, l(0.200000), l(-1.000000)
  mad r0.x, r0.x, l(-0.700000), l(1.000000)
  mul r2.xyw, r0.xxxx, r2.xyxz
  add r0.w, r2.y, r2.x
  mad r0.x, r2.z, r0.x, r0.w
  mul r0.x, r0.x, r0.x
  mad r0.x, r0.x, l(0.050000), l(1.000000)
  mul r2.xyz, r0.xxxx, r2.xywx
  movc r0.x, r0.y, l(0.500000), l(1.000000)
  mul r0.xyw, r0.xxxx, r2.xyxz
  lt r1.z, l(0.500000), cb3[1].w
  min r2.xyz, r0.xywx, l(0.700000, 0.700000, 0.700000, 0.000000)
  movc r0.xyw, r1.zzzz, r2.xyxz, r0.xyxw
  mul r0.xyw, r0.xyxw, cb0[206].xyxz
else
  mov r0.xyw, l(0,0,0,0)
endif
mul r0.xyw, r0.xyxw, cb4[58].wwww
mad r2.xyz, r15.xyzx, l(2.000000, 2.000000, 2.000000, 0.000000), l(1.000000, 1.000000, 1.000000, 0.000000)
mul r4.xyz, r0.xywx, r2.xyzx
add r1.xyz, r14.xyzx, r1.xywx
dp3 r1.w, r4.xyzx, l(0.299000, 0.587000, 0.114000, 0.000000)
mad r0.xyw, r0.xyxw, r2.xyxz, -r1.wwww
mad r0.xyw, cb4[41].xxxx, r0.xyxw, r1.wwww
add r0.xyw, r0.xyxw, l(-0.500000, -0.500000, 0.000000, -0.500000)
mad r0.xyw, cb4[41].yyyy, r0.xyxw, l(0.500000, 0.500000, 0.000000, 0.500000)
max r0.xyw, r0.xyxw, l(0.000000, 0.000000, 0.000000, 0.000000)
mul r0.xyw, r0.xyxw, cb4[41].yyyy
dp3 r1.w, r1.xyzx, l(0.299000, 0.587000, 0.114000, 0.000000)
add r1.xyz, -r1.wwww, r1.xyzx
mad r1.xyz, cb4[41].xxxx, r1.xyzx, r1.wwww
add r1.xyz, r1.xyzx, l(-0.500000, -0.500000, -0.500000, 0.000000)
mad r1.xyz, cb4[41].yyyy, r1.xyzx, l(0.500000, 0.500000, 0.500000, 0.000000)
max r1.xyz, r1.xyzx, l(0.000000, 0.000000, 0.000000, 0.000000)
mul r1.w, cb4[29].w, cb4[158].w
add r2.xyz, -r1.xyzx, cb4[29].xyzx
mad o0.xyz, r1.wwww, r2.xyzx, r1.xyzx
mad r1.x, -cb4[29].w, cb4[158].w, l(1.000000)
mul r0.xyw, r0.xyxw, r1.xxxx
max r0.xyw, r0.xyxw, l(0.000000, 0.000000, 0.000000, 0.000000)
sqrt r0.xyw, r0.xyxw
mul r0.xyw, r0.xyxw, l(0.200000, 0.200000, 0.000000, 0.200000)
min o1.xyz, r0.xywx, l(1.000000, 1.000000, 1.000000, 0.000000)
mul o1.w, r0.z, l(0.333300)
movc o2.w, r5.y, l(0.340000), l(0)
mad o3.xyz, r3.xywx, l(0.500000, 0.500000, 0.500000, 0.000000), l(0.500000, 0.500000, 0.500000, 0.000000)
mov o0.w, l(1.000000)
mov o3.w, l(1.000000)
ret
// Approximately 0 instruction slots used

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
