// ---- Created with 3Dmigoto v1.3.16 on Fri Jul 10 23:16:07 2026
Texture2D<float4> t7 : register(t7);

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
  float4 r0,r1,r2,r3,r4,r5,r6,r7,r8,r9,r10,r11,r12,r13,r14,r15,r16,r17,r18,r19,r20;
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
  r0.yz = t6.SampleBias(s0_s, r0.yz, cb0[199].x).zy;
  r0.yz = saturate(r0.yz);
  r0.w = cmp(0.5 < cb4[138].z);
  if (r0.w != 0) {
    r5.xy = cmp(float2(0.5,0.5) < cb4[147].xy);
    r0.w = r5.y ? r5.x : 0;
    r1.w = -0.200000003 + r0.y;
    r1.w = 1.25 * r1.w;
    r1.w = max(0, r1.w);
    r0.w = r0.w ? r1.w : r0.y;
    r0.y = r5.x ? r0.w : 0;
  }
  r0.w = 5 * r4.y;
  r0.w = floor(r0.w);
  r0.w = 4 + -r0.w;
  r0.w = max(0, r0.w);
  r0.w = (int)r0.w;
  r0.w = cmp((int)r0.w == asint(cb4[135].y));
  r1.w = r0.w ? 0.000000 : 0;
  r2.w = cmp(0.5 < cb4[139].w);
  r5.xy = cb4[140].xx * v0.xy;
  r5.xyz = t2.SampleBias(s0_s, r5.xy, cb0[199].x).xyz;
  r1.xyz = r1.xyz * cb4[58].xyz + r5.xyz;
  r1.xyz = float3(-0.5,-0.5,-0.5) + r1.xyz;
  r1.xyz = max(float3(0,0,0), r1.xyz);
  r1.xyz = r2.www ? r1.xyz : r2.xyz;
  r2.xyzw = cmp(r4.yyyy < float4(0.200000003,0.400000006,0.600000024,0.800000012));
  r4.y = r2.w ? cb4[137].x : cb4[136].w;
  r4.y = r2.z ? cb4[137].y : r4.y;
  r4.y = r2.y ? cb4[137].z : r4.y;
  r4.y = r2.x ? cb4[137].w : r4.y;
  r5.x = v2.w;
  r5.y = v3.w;
  r5.z = v4.w;
  r6.xyz = cb0[53].xyz + -r5.xyz;
  r4.w = dot(r6.xyz, r6.xyz);
  r5.w = max(1.17549435e-38, r4.w);
  r5.w = rsqrt(r5.w);
  r7.xyz = r6.xyz * r5.www;
  r6.w = sqrt(r4.w);
  r7.w = cmp(0 < asint(cb0[196].x));
  if (r7.w != 0) {
    r8.x = (int)cb3[2].z;
    r8.y = asint(cb0[196].x) + -1;
    r8.x = min((int)r8.x, (int)r8.y);
    r8.x = max(0, (int)r8.x);
    r9.x = t1[r8.x].val[0/4];
    r9.y = t1[r8.x].val[0/4+1];
    r9.z = t1[r8.x].val[0/4+2];
    r9.w = t1[r8.x].val[0/4+3];
    r10.x = t1[r8.x].val[16/4];
    r10.y = t1[r8.x].val[16/4+1];
    r10.z = t1[r8.x].val[16/4+2];
    r10.w = t1[r8.x].val[16/4+3];
    r8.y = t1[r8.x].val[32/4];
    r8.x = r10.w;
  } else {
    r8.xy = float2(0,0);
    r9.xyzw = float4(0,0,0,0);
    r10.xyz = float3(0,0,0);
  }
  r10.xyz = r10.xyz + -r5.xyz;
  r8.z = dot(r10.xyz, r10.xyz);
  r8.z = max(1.17549435e-38, r8.z);
  r8.w = rsqrt(r8.z);
  r9.w = r9.w * r9.w;
  r8.z = r8.z / r9.w;
  r8.z = 1 + -r8.z;
  r8.z = max(0, r8.z);
  r9.xyz = -cb0[197].xyz + r9.xyz;
  r9.xyz = r8.zzz * r9.xyz + cb0[197].xyz;
  r9.w = -1 + r8.z;
  r8.z = r8.z * r9.w + 1;
  r9.w = cmp(0.5 < cb0[22].x);
  if (r9.w != 0) {
    r9.w = r2.w ? cb4[166].x : cb4[165].w;
    r9.w = r2.z ? cb4[166].y : r9.w;
    r9.w = r2.y ? cb4[166].z : r9.w;
    r9.w = r2.x ? cb4[166].w : r9.w;
    r11.xyz = r3.xyw * cb4[138].xxx + r5.xyz;
    r11.xyz = -cb3[39].xyz + r11.xyz;
    r12.xyz = cb3[34].xyz * r11.yyy;
    r11.xyw = cb3[33].xyz * r11.xxx + r12.xyz;
    r11.xyz = cb3[35].xyz * r11.zzz + r11.xyw;
    r11.xyz = cb3[36].xyz + r11.xyz;
    r11.xy = r11.xy * cb3[38].xy + cb3[38].zw;
    r12.xy = -cb0[208].xy + r11.xy;
    r10.w = t7.SampleCmpLevelZero(s1_s, r12.xy, r11.z).x;
    r12.xyzw = cb0[208].xyxy * float4(-1,1,1,-1) + r11.xyxy;
    r11.w = t7.SampleCmpLevelZero(s1_s, r12.xy, r11.z).x;
    r10.w = r11.w + r10.w;
    r11.w = t7.SampleCmpLevelZero(s1_s, r12.zw, r11.z).x;
    r10.w = r11.w + r10.w;
    r12.xy = cb0[208].xy + r11.xy;
    r11.w = t7.SampleCmpLevelZero(s1_s, r12.xy, r11.z).x;
    r10.w = r11.w + r10.w;
    r12.xyzw = cb0[208].xyxy * float4(-1.41421294,0,1.41421294,0) + r11.xyxy;
    r11.w = t7.SampleCmpLevelZero(s1_s, r12.xy, r11.z).x;
    r10.w = r11.w + r10.w;
    r11.w = t7.SampleCmpLevelZero(s1_s, r12.zw, r11.z).x;
    r10.w = r11.w + r10.w;
    r12.xyzw = cb0[208].xyxy * float4(0,-1.41421294,0,1.41421294) + r11.xyxy;
    r11.w = t7.SampleCmpLevelZero(s1_s, r12.xy, r11.z).x;
    r10.w = r11.w + r10.w;
    r11.w = t7.SampleCmpLevelZero(s1_s, r12.zw, r11.z).x;
    r10.w = r11.w + r10.w;
    r11.x = t7.SampleCmpLevelZero(s1_s, r11.xy, r11.z).x;
    r10.w = r11.x + r10.w;
    r10.w = r10.w * 0.111100003 + -1;
    r9.w = r10.w * r9.w;
    r9.w = cb3[37].x * r9.w;
    r11.xyz = -cb2[20].xyz + r5.xyz;
    r12.xyz = -cb2[21].xyz + r5.xyz;
    r13.xyz = -cb2[22].xyz + r5.xyz;
    r14.xyz = -cb2[23].xyz + r5.xyz;
    r11.x = dot(r11.xyz, r11.xyz);
    r11.y = dot(r12.xyz, r12.xyz);
    r11.z = dot(r13.xyz, r13.xyz);
    r11.w = dot(r14.xyz, r14.xyz);
    r11.xyzw = cmp(r11.xyzw < cb2[24].xyzw);
    r12.xyzw = r11.xyzw ? float4(1,1,1,1) : 0;
    r11.xyz = r11.xyz ? float3(-1,-1,-1) : float3(-0,-0,-0);
    r11.xyz = r12.yzw + r11.xyz;
    r12.yzw = max(float3(0,0,0), r11.xyz);
    r10.w = dot(r12.xyzw, float4(4,3,2,1));
    r11.z = 4 + -r10.w;
    r10.w = (uint)r11.z;
    r10.w = (uint)r10.w << 2;
    r12.xyz = cb2[r10.w+1].xyz * v3.www;
    r12.xyz = cb2[r10.w+0].xyz * v2.www + r12.xyz;
    r12.xyz = cb2[r10.w+2].xyz * v4.www + r12.xyz;
    r12.xyz = cb2[r10.w+3].xyz + r12.xyz;
    r13.xy = float2(1024,1024) * r12.xy;
    r13.xy = frac(r13.xy);
    r10.w = dot(r13.xy, float2(12.9898005,78.2330017));
    r10.w = sin(r10.w);
    r10.w = 43758.5469 * r10.w;
    r10.w = frac(r10.w);
    sincos(r10.w, r13.x, r14.x);
    r15.xyzw = float4(1.29999995,1.29999995,1.29999995,1.29999995) * cb2[26].xxyy;
    r13.xz = r13.xx;
    r13.yw = r14.xx;
    r13.xyzw = r13.xyzw * r15.xyzw;
    r14.xyzw = float4(-0.172399998,-0.978299975,-0.978299975,0.172399998) * r13.xyzw;
    r14.xy = r14.xz + r14.yw;
    r11.xy = r14.xy + r12.xy;
    r10.w = t0.SampleCmpLevelZero(s1_s, r11.xyz, r12.z).x;
    r14.xyzw = float4(0.87470001,0.484600008,0.484600008,-0.87470001) * r13.xyzw;
    r14.xy = r14.xz + r14.yw;
    r11.xy = r14.xy + r12.xy;
    r11.w = t0.SampleCmpLevelZero(s1_s, r11.xyz, r12.z).x;
    r10.w = r11.w + r10.w;
    r14.xyzw = float4(-0.968299985,-0.0373999998,-0.0373999998,0.968299985) * r13.xyzw;
    r14.xy = r14.xz + r14.yw;
    r11.xy = r14.xy + r12.xy;
    r11.w = t0.SampleCmpLevelZero(s1_s, r11.xyz, r12.z).x;
    r10.w = r11.w + r10.w;
    r14.xyzw = float4(0.278299987,0.41960001,0.41960001,-0.278299987) * r13.xyzw;
    r14.xy = r14.xz + r14.yw;
    r11.xy = r14.xy + r12.xy;
    r11.w = t0.SampleCmpLevelZero(s1_s, r11.xyz, r12.z).x;
    r10.w = r11.w + r10.w;
    r14.xyzw = float4(-0.150700003,0.839100003,0.839100003,-0.150700003) * r13.xyzw;
    r14.xy = r14.xz + r14.yw;
    r11.xy = r14.xy + r12.xy;
    r11.w = t0.SampleCmpLevelZero(s1_s, r11.xyz, r12.z).x;
    r10.w = r11.w + r10.w;
    r14.xyzw = float4(-0.641700029,0.479299992,0.479299992,-0.641700029) * r13.xyzw;
    r14.xy = r14.xz + r14.yw;
    r11.xy = r14.xy + r12.xy;
    r11.w = t0.SampleCmpLevelZero(s1_s, r11.xyz, r12.z).x;
    r10.w = r11.w + r10.w;
    r14.xyzw = float4(0.577899992,-0.816100001,-0.816100001,0.577899992) * r13.xyzw;
    r14.xy = r14.xz + r14.yw;
    r11.xy = r14.xy + r12.xy;
    r11.w = t0.SampleCmpLevelZero(s1_s, r11.xyz, r12.z).x;
    r10.w = r11.w + r10.w;
    r14.xyzw = float4(-0.540899992,-0.458799988,-0.458799988,0.540899992) * r13.xyzw;
    r14.xy = r14.xz + r14.yw;
    r11.xy = r14.xy + r12.xy;
    r11.w = t0.SampleCmpLevelZero(s1_s, r11.xyz, r12.z).x;
    r10.w = r11.w + r10.w;
    r14.xyzw = float4(0.704400003,-0.1919,-0.1919,0.704400003) * r13.xyzw;
    r14.xy = r14.xz + r14.yw;
    r11.xy = r14.xy + r12.xy;
    r11.w = t0.SampleCmpLevelZero(s1_s, r11.xyz, r12.z).x;
    r10.w = r11.w + r10.w;
    r14.xyzw = float4(0.105300002,-0.446399987,-0.446399987,0.105300002) * r13.xyzw;
    r14.xy = r14.xz + r14.yw;
    r11.xy = r14.xy + r12.xy;
    r11.w = t0.SampleCmpLevelZero(s1_s, r11.xyz, r12.z).x;
    r10.w = r11.w + r10.w;
    r13.xyzw = float4(-0.206599995,0.0661000013,0.0661000013,-0.206599995) * r13.xyzw;
    r13.xy = r13.xz + r13.yw;
    r11.xy = r13.xy + r12.xy;
    r11.x = t0.SampleCmpLevelZero(s1_s, r11.xyz, r12.z).x;
    r10.w = r11.x + r10.w;
    r10.w = cb2[25].x * r10.w;
    r11.x = 1 + -cb2[25].x;
    r10.w = r10.w * 0.0908999965 + r11.x;
    r11.x = cmp(0 >= r12.z);
    r11.y = cmp(r12.z >= 1);
    r11.x = (int)r11.y | (int)r11.x;
    r10.w = r11.x ? 1 : r10.w;
    r11.x = 1 + -r8.x;
    r8.x = cb3[40].y * r11.x + r8.x;
    r8.x = r10.w * r8.x;
    r8.y = saturate(r8.y * 2 + -1);
    r10.w = cb0[197].w * r8.y;
    r11.x = cmp(0.5 < cb3[37].x);
    r8.y = r8.y * r9.w + 1;
    r8.y = min(1, r8.y);
    r8.y = r11.x ? r8.y : 1;
    r8.x = r8.y * r8.x;
    r8.y = 1 + -cb4[136].z;
    r8.y = r8.x * cb4[136].z + r8.y;
    r9.w = cb4[136].z * r10.w;
    r10.w = -r10.w * cb4[136].z + 1;
    r8.x = r8.x * r9.w + r10.w;
  } else {
    r8.xy = float2(1,1);
  }
  r9.w = saturate(2.5 * cb3[1].w);
  r10.w = 1 + -r9.w;
  r8.x = r10.w * r8.x + r9.w;
  r9.xyz = float3(-0.800000012,-0.800000012,-0.800000012) + r9.xyz;
  r9.xyz = cb4[58].www * r9.xyz + float3(0.800000012,0.800000012,0.800000012);
  r4.w = rsqrt(r4.w);
  r11.xyz = r6.xyz * r4.www;
  r10.xyz = r10.xyz * r8.www + -r11.xyz;
  r10.xyz = cb4[58].www * r10.xyz + r11.xyz;
  r4.w = cmp(cb3[3].w != 0.000000);
  r5.x = dot(cb3[3].xyz, r5.xyz);
  r5.x = saturate(-cb3[3].w + r5.x);
  r11.xyz = cb0[15].xyz * r9.xyz;
  r12.xyz = cb0[15].xyz + r9.xyz;
  r12.xyz = -r9.xyz * cb0[15].xyz + r12.xyz;
  r11.xyz = cb0[15].www * r12.xyz + r11.xyz;
  r11.xyz = r11.xyz + -r9.xyz;
  r5.xyz = r5.xxx * r11.xyz + r9.xyz;
  r5.xyz = r4.www ? r5.xyz : r9.xyz;
  r4.y = max(9.99999975e-06, r4.y);
  r4.w = rcp(r4.y);
  r8.w = dot(r3.xyw, r10.xyz);
  r9.x = 1 + r8.w;
  r9.y = 3 * r10.y;
  r9.y = min(1, r9.y);
  r9.y = -r9.y * 0.5 + r3.y;
  r9.y = saturate(1.5 + r9.y);
  r9.x = r9.x * r9.y + -1;
  r9.x = r9.x + -r8.w;
  r9.x = v7.y * r9.x + r8.w;
  r3.z = r3.z * 2 + r9.x;
  r9.x = 3 * r3.z;
  r11.xy = float2(1.5,4.5) * r4.yy;
  r9.yz = r3.zz * float2(3,3) + -r11.xy;
  r9.xyz = float3(3,1,-1) + r9.xyz;
  r4.y = -r4.y * 3 + 2;
  r9.xyw = r9.xyz / r4.yyy;
  r11.xyz = float3(1,1,1) + -r9.xyw;
  r12.xyz = float3(0.333299994,-0.333299994,-0.333299994) + r3.zzz;
  r12.xyz = r4.www * r12.xyz + float3(0.5,0.5,-0.5);
  r13.xyz = float3(1,1,1) + -r12.xyz;
  r14.xy = min(r13.yx, r9.yx);
  r9.xz = min(r12.xz, r11.yz);
  r14.z = r11.x;
  r14.w = r9.x;
  r11.xyz = saturate(r14.zyw);
  r14.y = saturate(min(r13.z, r12.y));
  r14.x = saturate(r14.x);
  r9.zw = saturate(r9.zw);
  r4.y = r8.y + -r8.x;
  r4.y = cb3[40].y * r4.y + r8.x;
  r12.xyzw = r4.yyyy * float4(2,2,-2,-2) + float4(0,-1,1,2);
  r12.x = saturate(min(r12.x, r12.w));
  r12.yz = saturate(r12.yz);
  r4.y = cmp(0.5 < cb3[40].y);
  if (r4.y != 0) {
    if (r7.w != 0) {
      r4.y = (int)cb3[2].z;
      r4.w = asint(cb0[196].x) + -1;
      r4.y = min((int)r4.y, (int)r4.w);
      r4.y = max(0, (int)r4.y);
      r4.y = t1[r4.y].val[32/4];
    } else {
      r4.y = 0;
    }
    r4.y = cb0[197].w * r4.y;
    r13.y = r12.x * r4.y;
    r9.xy = -r12.zx * r4.yy + r12.zx;
    r4.w = r9.x + r9.y;
    r13.z = r12.y + r4.w;
    if (r7.w != 0) {
      r4.w = (int)cb3[2].z;
      r9.x = asint(cb0[196].x) + -1;
      r4.w = min((int)r9.x, (int)r4.w);
      r4.w = max(0, (int)r4.w);
      r4.w = t1[r4.w].val[28/4];
    } else {
      r4.w = 0;
    }
    r12.xy = r13.yz * r4.ww;
    r9.xy = -r13.yz * r4.ww + r13.yz;
    r4.w = r9.x + r9.y;
    r12.z = r12.z * r4.y + r4.w;
  } else {
    if (r7.w != 0) {
      r4.y = (int)cb3[2].z;
      r4.w = asint(cb0[196].x) + -1;
      r4.y = min((int)r4.y, (int)r4.w);
      r4.y = max(0, (int)r4.y);
      r4.y = t1[r4.y].val[32/4];
    } else {
      r4.y = 0;
    }
    r12.xz = r12.xz * r4.yy;
  }
  r4.y = 1 + -r11.x;
  r4.y = r4.y + -r11.y;
  r4.y = r4.y + -r11.z;
  r4.y = r12.z * r4.y + r11.z;
  r4.w = r12.x + r12.y;
  r9.xy = r14.xy * r4.ww;
  r4.w = r9.z + r9.w;
  r4.w = r4.w * r12.x + r9.y;
  r9.y = r12.y * r9.z;
  r9.z = v7.x * r11.x;
  r10.w = -r11.x * v7.x + r11.x;
  r10.w = r11.y + r10.w;
  r11.xyz = r2.www ? cb4[61].xyz : cb4[60].xyz;
  r11.xyz = r2.zzz ? cb4[62].xyz : r11.xyz;
  r11.xyz = r2.yyy ? cb4[63].xyz : r11.xyz;
  r11.xyz = r2.xxx ? cb4[64].xyz : r11.xyz;
  r12.xzw = r2.www ? cb4[66].xyz : cb4[65].xyz;
  r12.xzw = r2.zzz ? cb4[67].xyz : r12.xzw;
  r12.xzw = r2.yyy ? cb4[68].xyz : r12.xzw;
  r12.xzw = r2.xxx ? cb4[69].xyz : r12.xzw;
  r13.xyz = r0.www ? cb0[10].xyz : cb0[3].xyz;
  r14.xyz = r0.www ? cb0[11].xyz : cb0[4].xyz;
  r15.xyz = r0.www ? cb0[12].xyz : cb0[5].xyz;
  r16.xyz = r0.www ? cb0[9].xyz : cb0[6].xyz;
  r17.xyz = r0.www ? cb0[13].xyz : cb0[7].xyz;
  r18.xyz = r0.www ? cb0[14].xyz : cb0[8].xyz;
  r11.w = 0.437249988 * r6.w;
  r11.w = min(1, r11.w);
  r19.x = r11.w * cb3[1].w + -r11.w;
  r19.y = -r11.w * cb3[1].w + r11.w;
  r19.xy = float2(1,-1) + r19.xy;
  r19.xy = cb4[136].yy * r19.xy + float2(0,1);
  r11.xyz = float3(6.10351562e-05,6.10351562e-05,6.10351562e-05) + r11.xyz;
  r11.w = r11.x + r11.y;
  r11.w = r11.w + r11.z;
  r11.w = 0.333330005 * r11.w;
  r20.xyz = saturate(r11.xyz / r11.www);
  r11.xyz = r11.xyz * r19.yyy;
  r11.xyz = r20.xyz * r19.xxx + r11.xyz;
  r12.xzw = float3(6.10351562e-05,6.10351562e-05,6.10351562e-05) + r12.xzw;
  r11.w = r12.x + r12.z;
  r11.w = r11.w + r12.w;
  r11.w = 0.333330005 * r11.w;
  r20.xyz = saturate(r12.xzw / r11.www);
  r12.xzw = r12.xzw * r19.yyy;
  r12.xzw = r20.xyz * r19.xxx + r12.xzw;
  r13.xyz = r11.xyz * r13.xyz;
  r11.xyz = r11.xyz * r14.xyz;
  r14.xyz = r12.xzw * r15.xyz;
  r12.xzw = r12.xzw * r18.xyz;
  r15.xyz = float3(1.17549435e-38,1.17549435e-38,1.17549435e-38) + r5.xyz;
  r11.w = max(r15.x, r15.y);
  r11.w = max(r11.w, r15.z);
  r11.w = rcp(r11.w);
  if (r7.w != 0) {
    r13.w = (int)cb3[2].z;
    r14.w = asint(cb0[196].x) + -1;
    r13.w = min((int)r14.w, (int)r13.w);
    r13.w = max(0, (int)r13.w);
    r13.w = t1[r13.w].val[32/4];
  } else {
    r13.w = 0;
  }
  r14.w = 1 + -r8.z;
  r8.z = r13.w * r14.w + r8.z;
  r15.xyz = r8.zzz * r5.xyz;
  r8.z = min(1, r11.w);
  r18.xyz = r15.xyz * r8.zzz;
  r16.xyz = r16.xyz * r9.yyy;
  r16.xyz = r17.xyz * r4.www + r16.xyz;
  r16.xyz = r9.www * r12.yyy + r16.xyz;
  r14.xyz = r14.xyz * r10.www;
  r9.yzw = r12.xzw * r9.zzz + r14.xyz;
  r9.yzw = r11.xyz * r4.yyy + r9.yzw;
  r9.xyz = r13.xyz * r9.xxx + r9.yzw;
  r9.xyz = r9.xyz * r18.xyz;
  r9.xyz = r15.xyz * r16.xyz + r9.xyz;
  r9.xyz = r9.xyz + -r5.xyz;
  r5.xyz = cb4[58].www * r9.xyz + r5.xyz;
  r4.y = cmp(0.5 < v7.z);
  if (r0.w == 0) {
    r9.xyz = r4.yyy ? r3.xyw : v2.xyz;
    r0.w = dot(r1.xyz, float3(0.289999992,0.600000024,0.109999999));
    r4.w = cmp(v7.z < 0.5);
    r11.xy = r0.ww * float2(0.287499994,0.400000006) + float2(1.4375,1);
    r8.z = dot(r10.xyz, r9.xyz);
    r9.x = r8.z + -r8.w;
    r9.x = saturate(-r9.x * 3 + 1);
    r9.y = r9.x + r9.x;
    r9.x = sqrt(r9.x);
    r9.x = r9.y * r9.x;
    r9.x = min(1, r9.x);
    r9.y = r8.w * 0.5 + 0.5;
    r9.z = saturate(r8.w);
    r9.x = r9.y * r9.x + -r9.z;
    r9.x = r9.x * 0.5 + r9.z;
    r8.z = saturate(r8.z);
    r9.y = max(r1.y, r1.z);
    r9.y = max(r9.y, r1.x);
    r9.z = cmp(1 < r9.y);
    r12.xyz = r1.xyz / r9.yyy;
    r9.yzw = r9.zzz ? r12.xyz : r1.xyz;
    r10.w = 1 + -r11.x;
    r9.x = r9.x * r10.w + r11.x;
    r9.yzw = log2(r9.yzw);
    r9.xyz = r9.xxx * r9.yzw;
    r9.xyz = exp2(r9.xyz);
    r11.xzw = r9.xyz + -r1.xyz;
    r11.xzw = r11.xzw * float3(0.5,0.5,0.5) + r1.xyz;
    r9.xyz = -r11.xzw + r9.xyz;
    r9.xyz = r8.zzz * r9.xyz + r11.xzw;
    r0.w = -r0.w * 0.0500000007 + 1.04999995;
    r11.xzw = log2(r1.xyz);
    r11.xyz = r11.yyy * r11.xzw;
    r11.xyz = exp2(r11.xyz);
    r11.xyz = r11.xyz * r0.www;
    r1.xyz = r4.www ? r9.xyz : r11.xyz;
  }
  r0.w = -r0.x * 0.959999979 + 0.959999979;
  r9.xyz = r1.xyz * r0.www;
  r11.xyz = float3(-0.0399999991,-0.0399999991,-0.0399999991) + r1.xyz;
  r11.xyz = r0.xxx * r11.xyz + float3(0.0399999991,0.0399999991,0.0399999991);
  r4.w = -r0.z * cb4[140].w + 1;
  r4.w = r4.w * r4.w;
  r8.z = r4.w * 4 + 2;
  r9.w = r4.w * r4.w;
  r10.w = r4.w * r4.w + -1;
  r12.xyz = cb0[2].xyz + r5.xyz;
  r12.xyz = v8.xyz + r12.xyz;
  r11.w = dot(r12.xyz, float3(0.212672904,0.715152204,0.0721750036));
  r12.x = cb0[19].y + -cb0[19].x;
  r12.y = 1 / r12.x;
  r12.z = -cb0[19].x * r12.y + 1;
  r12.y = r11.w * r12.y + r12.z;
  r12.y = rcp(r12.y);
  r12.x = -r12.x * r12.y + cb0[19].y;
  r12.y = cmp(r11.w < cb0[19].x);
  r12.x = r12.y ? r11.w : r12.x;
  r11.w = 9.99999975e-05 + r11.w;
  r11.w = r12.x / r11.w;
  r12.xyz = r11.www * r5.xyz;
  r13.xyz = r2.www ? cb4[76].xyz : cb4[75].xyz;
  r13.xyz = r2.zzz ? cb4[77].xyz : r13.xyz;
  r13.xyz = r2.yyy ? cb4[78].xyz : r13.xyz;
  r13.xyz = r2.xxx ? cb4[79].xyz : r13.xyz;
  r12.w = r2.w ? cb4[144].z : cb4[144].y;
  r12.w = r2.z ? cb4[144].w : r12.w;
  r12.w = r2.y ? cb4[145].x : r12.w;
  r12.w = r2.x ? cb4[145].y : r12.w;
  r13.w = cmp(0.5 < r12.w);
  if (r13.w != 0) {
    r3.z = saturate(r3.z * 1.5 + -0.5);
    r13.w = r2.w ? cb4[145].w : cb4[145].z;
    r13.w = r2.z ? cb4[146].x : r13.w;
    r13.w = r2.y ? cb4[146].y : r13.w;
    r13.w = r2.x ? cb4[146].z : r13.w;
    r3.z = r4.x + r3.z;
    r3.z = -1 + r3.z;
    r13.w = max(9.99999975e-06, r13.w);
    r4.x = saturate(r3.z / r13.w);
  }
  r3.z = cb4[146].w * r4.x;
  r13.xyz = r3.zzz * r13.xyz;
  r13.xyz = r13.xyz * r11.xyz;
  r3.z = cmp(r12.w < 0.5);
  r6.xyz = r6.xyz * r5.www + r10.xyz;
  r4.x = dot(r6.xyz, r6.xyz);
  r4.x = rsqrt(r4.x);
  r6.xyz = r6.xyz * r4.xxx;
  r4.x = r2.w ? cb4[143].y : cb4[143].x;
  r4.x = r2.z ? cb4[143].z : r4.x;
  r4.x = r2.y ? cb4[143].w : r4.x;
  r4.x = r2.x ? cb4[144].x : r4.x;
  r5.w = r4.x * r8.w;
  r5.w = saturate(r5.w * 0.75 + 0.25);
  r12.w = dot(r3.xyw, r6.xyz);
  r12.w = r12.w * r4.x;
  r12.w = saturate(r12.w * 0.75 + 0.25);
  r6.x = dot(r10.xyz, r6.xyz);
  r4.x = r6.x * r4.x;
  r4.x = saturate(r4.x * 0.75 + 0.25);
  r6.x = r12.w * r12.w;
  r6.x = r6.x * r10.w + 1.00001001;
  r4.x = r4.x * r4.x;
  r6.x = r6.x * r6.x;
  r4.x = max(0.100000001, r4.x);
  r4.x = r6.x * r4.x;
  r4.x = r4.x * r8.z;
  r4.x = r9.w / r4.x;
  r0.z = saturate(-r0.z * cb4[140].w + r4.x);
  r0.z = r0.z * r5.w;
  r4.x = max(9.99999975e-06, r4.w);
  r0.z = r0.z / r4.x;
  r4.x = r2.w ? cb4[142].x : cb4[141].w;
  r4.x = r2.z ? cb4[142].y : r4.x;
  r4.x = r2.y ? cb4[142].z : r4.x;
  r4.x = r2.x ? cb4[142].w : r4.x;
  r4.w = r2.w ? cb4[169].x : cb4[168].w;
  r4.w = r2.z ? cb4[169].y : r4.w;
  r4.w = r2.y ? cb4[169].z : r4.w;
  r4.w = r2.x ? cb4[169].w : r4.w;
  r4.x = r4.x * r4.w;
  r0.z = r4.x * r0.z;
  r0.z = saturate(10 * r0.z);
  r0.z = 100 * r0.z;
  r0.z = r3.z ? r0.z : 16.6669998;
  r6.xyz = r0.zzz * r13.xyz;
  r13.xyz = r6.xyz * r12.xyz;
  r6.xyz = r6.xyz * r12.xyz + float3(-1,-1,-1);
  r6.xyz = max(float3(0,0,0), r6.xyz);
  r13.xyz = r9.xyz * r12.xyz + r13.xyz;
  r0.z = cmp(cb4[147].x >= 0.5);
  r14.xyz = r2.www ? cb4[81].xyz : cb4[80].xyz;
  r14.xyz = r2.zzz ? cb4[82].xyz : r14.xyz;
  r14.xyz = r2.yyy ? cb4[83].xyz : r14.xyz;
  r14.xyz = r2.xxx ? cb4[84].xyz : r14.xyz;
  r14.xyz = r14.xyz * r0.yyy;
  r14.xyz = r14.xyz * r1.xyz;
  r14.xyz = r0.zzz ? r14.xyz : 0;
  r0.y = r14.x + r14.y;
  r0.y = r0.y + r14.z;
  r15.xyz = v8.xyz * r11.www;
  r13.xyz = r15.xyz * r9.xyz + r13.xyz;
  if (r7.w != 0) {
    r0.z = (int)cb3[2].z;
    r3.z = asint(cb0[196].x) + -1;
    r0.z = min((int)r3.z, (int)r0.z);
    r0.z = max(0, (int)r0.z);
    r16.x = t1[r0.z].val[96/4];
    r16.y = t1[r0.z].val[96/4+1];
    r16.z = t1[r0.z].val[96/4+2];
    r17.x = t1[r0.z].val[112/4];
    r17.y = t1[r0.z].val[112/4+1];
    r17.z = t1[r0.z].val[112/4+2];
  } else {
    r16.xyz = float3(0,0,0);
    r17.xyz = float3(0,0,0);
  }
  r0.z = cmp(0.5 < cb0[23].y);
  r16.xyz = r1.www ? r16.xyz : r17.xyz;
  r16.xyz = r0.zzz ? float3(0.0500000007,0.0500000007,0.0500000007) : r16.xyz;
  r17.xyz = r11.www * r1.xyz;
  r13.xyz = r16.xyz * r17.xyz + r13.xyz;
  r6.xyz = r13.xyz + r6.xyz;
  r0.z = cmp(0.5 >= cb0[196].y);
  if (r0.z != 0) {
    r0.z = dot(r7.xyz, r10.xyz);
    r0.z = saturate(-r0.z * 0.5 + 0.5);
    r1.x = r0.z * 0.800000012 + 0.200000003;
    r1.y = r3.y * 0.5 + 0.5;
    r3.z = r1.y * r1.y;
    r1.y = r1.w ? r1.y : r3.z;
    r1.y = -0.200000003 + r1.y;
    r1.y = saturate(1.25 * r1.y);
    r3.z = r1.y * -2 + 3;
    r1.y = r1.y * r1.y;
    r1.y = r3.z * r1.y;
    r3.z = r1.y * r1.y;
    r3.z = r3.z * r3.z;
    r3.z = r3.z * r1.y;
    r10.xyz = r1.www ? float3(1,0.300000012,-1) : float3(0.5,1,-0.5);
    r4.x = r10.y + r10.z;
    r3.z = r3.z * r4.x + r10.x;
    r1.y = r1.y * r3.z + -0.100000001;
    r1.y = cb0[207].x * r1.y + 0.100000001;
    r3.z = r8.w * 0.5 + 0.5;
    r3.z = r3.z * r8.x;
    r3.z = r3.z * 1.39999998 + 0.100000001;
    if (r7.w != 0) {
      r4.x = (int)cb3[2].z;
      r4.w = asint(cb0[196].x) + -1;
      r4.x = min((int)r4.x, (int)r4.w);
      r4.x = max(0, (int)r4.x);
      r4.x = t1[r4.x].val[32/4];
    } else {
      r4.x = 0;
    }
    r4.x = r4.x * 0.399999976 + r8.y;
    r4.x = saturate(0.600000024 + r4.x);
    r1.x = r1.x * r1.y;
    r1.x = r1.x * r3.z;
    r1.x = r1.x * r4.x;
    r1.y = cmp(0 != v7.z);
    r3.z = saturate(-r7.y);
    r4.x = r4.z * cb4[140].z + 2.5;
    r1.y = r1.y ? 4.5 : r4.x;
    r1.y = r3.z * r1.y + -0.5;
    r1.y = cb0[207].y * r1.y + 1;
    r1.x = r1.x * r1.y;
    r1.y = 0.0833333358 * r6.w;
    r1.y = min(1, r1.y);
    r4.xz = r1.yy * float2(-0.300000012,-0.300000012) + float2(0.5,0.600000024);
    r1.y = dot(r7.xyz, r3.xyw);
    r1.y = 1 + -r1.y;
    r4.xz = r1.yy + -r4.xz;
    r4.xz = saturate(float2(3.33333325,5.00000048) * r4.xz);
    r7.xy = r4.xz * float2(-2,-2) + float2(3,3);
    r4.xz = r4.xz * r4.xz;
    r4.xz = r7.xy * r4.xz;
    r1.y = r1.w ? r4.x : r4.z;
    r3.z = dot(cb0[197].xyz, float3(0.330000013,0.330000013,0.330000013));
    r4.xzw = cb0[197].xyz * cb0[197].xyz;
    r4.xzw = r4.xzw * r4.xzw;
    r4.xzw = r4.xzw * r4.xzw;
    r5.w = dot(r4.xzw, float3(0.699999988,0.699999988,0.699999988));
    r5.w = max(0.00999999978, r5.w);
    r5.w = rcp(r5.w);
    r3.z = r5.w * r3.z;
    r4.xzw = r3.zzz * r4.xzw + -r12.xyz;
    r4.xzw = r8.yyy * r4.xzw + r12.xyz;
    r0.z = r0.z * r0.z;
    r0.z = log2(r0.z);
    r0.z = 20 * r0.z;
    r0.z = exp2(r0.z);
    r7.xyz = r5.xyz * r11.www + -r4.xzw;
    r4.xzw = r0.zzz * r7.xyz + r4.xzw;
    r4.xzw = -r5.xyz * r11.www + r4.xzw;
    r4.xzw = cb0[207].zzz * r4.xzw + r12.xyz;
    r0.z = r9.x + r9.y;
    r0.z = r1.z * r0.w + r0.z;
    r0.z = 0.330000013 * r0.z;
    r0.z = r0.z * r0.z;
    r0.z = cb0[207].w * r0.z;
    r0.z = r0.z * -0.199999988 + 1;
    r9.xyz = saturate(r9.xyz);
    r5.xyz = log2(r9.xyz);
    r5.xyz = float3(0.200000003,0.200000003,0.200000003) * r5.xyz;
    r5.xyz = exp2(r5.xyz);
    r0.w = dot(r5.xyz, r5.xyz);
    r0.w = max(6.10351562e-05, r0.w);
    r0.w = rsqrt(r0.w);
    r5.xyz = r5.xyz * r0.www;
    r0.w = 48 * cb0[206].w;
    r0.z = 0.100000001 * r0.z;
    r7.xyz = r0.zzz * r5.xyz;
    r5.xyz = -r0.zzz * r5.xyz + r11.xyz;
    r5.xyz = r0.xxx * r5.xyz + r7.xyz;
    r0.xzw = r5.xyz * r0.www;
    r1.x = r1.x * r1.y;
    r1.xyz = r1.xxx * r4.xzw;
    r0.xzw = r1.xyz * r0.xzw;
    r1.xyz = r2.www ? cb4[88].xyz : cb4[87].xyz;
    r1.xyz = r2.zzz ? cb4[89].xyz : r1.xyz;
    r1.xyz = r2.yyy ? cb4[90].xyz : r1.xyz;
    r1.xyz = r2.xxx ? cb4[91].xyz : r1.xyz;
    r0.xzw = r1.xyz * r0.xzw;
    r1.x = saturate(r6.w * 0.200000003 + -1);
    r1.x = r1.x * -0.699999988 + 1;
    r2.xyz = r1.xxx * r0.xzw;
    r0.x = r2.x + r2.y;
    r0.x = r0.w * r1.x + r0.x;
    r0.x = r0.x * r0.x;
    r0.x = r0.x * 0.0500000007 + 1;
    r0.xzw = r2.xyz * r0.xxx;
    r1.x = r4.y ? 0.5 : 1;
    r0.xzw = r1.xxx * r0.xzw;
    r1.x = cmp(0.5 < cb3[1].w);
    r2.xyz = min(float3(0.699999988,0.699999988,0.699999988), r0.xzw);
    r0.xzw = r1.xxx ? r2.xyz : r0.xzw;
    r0.xzw = cb0[206].xyz * r0.xzw;
  } else {
    r0.xzw = float3(0,0,0);
  }
  r0.xzw = cb4[58].www * r0.xzw;
  r1.xyz = r15.xyz * float3(2,2,2) + float3(1,1,1);
  r2.xyz = r1.xyz * r0.xzw;
  r4.xyz = r6.xyz + r14.xyz;
  r2.x = dot(r2.xyz, float3(0.298999995,0.587000012,0.114));
  r0.xzw = r0.xzw * r1.xyz + -r2.xxx;
  r0.xzw = cb4[41].xxx * r0.xzw + r2.xxx;
  r0.xzw = float3(-0.5,-0.5,-0.5) + r0.xzw;
  r0.xzw = cb4[41].yyy * r0.xzw + float3(0.5,0.5,0.5);
  r0.xzw = max(float3(0,0,0), r0.xzw);
  r0.xzw = cb4[41].yyy * r0.xzw;
  r1.x = dot(r4.xyz, float3(0.298999995,0.587000012,0.114));
  r2.xyz = r4.xyz + -r1.xxx;
  r1.xyz = cb4[41].xxx * r2.xyz + r1.xxx;
  r1.xyz = float3(-0.5,-0.5,-0.5) + r1.xyz;
  r1.xyz = cb4[41].yyy * r1.xyz + float3(0.5,0.5,0.5);
  r1.xyz = max(float3(0,0,0), r1.xyz);
  r2.x = cb4[158].w * cb4[29].w;
  r2.yzw = cb4[29].xyz + -r1.xyz;
  o0.xyz = r2.xxx * r2.yzw + r1.xyz;
  r1.x = -cb4[29].w * cb4[158].w + 1;
  r0.xzw = r1.xxx * r0.xzw;
  r0.xzw = max(float3(0,0,0), r0.xzw);
  r0.xzw = sqrt(r0.xzw);
  r0.xzw = float3(0.200000003,0.200000003,0.200000003) * r0.xzw;
  o1.xyz = min(float3(1,1,1), r0.xzw);
  o1.w = 0.333299994 * r0.y;
  o2.w = r1.w ? 0.340000004 : 0;
  o3.xyz = r3.xyw * float3(0.5,0.5,0.5) + float3(0.5,0.5,0.5);
  o0.w = 1;
  o3.w = 1;
  return;
}

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Generated by Microsoft (R) D3D Shader Disassembler
//
//   using 3Dmigoto v1.3.16 on Fri Jul 10 23:16:07 2026
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
dcl_constantbuffer CB4[170], immediateIndexed
dcl_sampler s0, mode_default
dcl_sampler s1, mode_comparison
dcl_resource_texture2darray (float,float,float,float) t0
dcl_resource_structured t1, 128
dcl_resource_texture2d (float,float,float,float) t2
dcl_resource_texture2d (float,float,float,float) t3
dcl_resource_texture2d (float,float,float,float) t4
dcl_resource_texture2d (float,float,float,float) t5
dcl_resource_texture2d (float,float,float,float) t6
dcl_resource_texture2d (float,float,float,float) t7
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
dcl_temps 21
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
sample_b_indexable(texture2d)(float,float,float,float) r0.yz, r0.yzyy, t6.xzyw, s0, cb0[199].x
mov_sat r0.yz, r0.yyzy
lt r0.w, l(0.500000), cb4[138].z
if_nz r0.w
  lt r5.xy, l(0.500000, 0.500000, 0.000000, 0.000000), cb4[147].xyxx
  and r0.w, r5.y, r5.x
  add r1.w, r0.y, l(-0.200000)
  mul r1.w, r1.w, l(1.250000)
  max r1.w, r1.w, l(0.000000)
  movc r0.w, r0.w, r1.w, r0.y
  and r0.y, r0.w, r5.x
endif
mul r0.w, r4.y, l(5.000000)
round_ni r0.w, r0.w
add r0.w, -r0.w, l(4.000000)
max r0.w, r0.w, l(0.000000)
ftoi r0.w, r0.w
ieq r0.w, r0.w, cb4[135].y
and r1.w, r0.w, l(1)
lt r2.w, l(0.500000), cb4[139].w
mul r5.xy, v0.xyxx, cb4[140].xxxx
sample_b_indexable(texture2d)(float,float,float,float) r5.xyz, r5.xyxx, t2.xyzw, s0, cb0[199].x
mad r1.xyz, r1.xyzx, cb4[58].xyzx, r5.xyzx
add r1.xyz, r1.xyzx, l(-0.500000, -0.500000, -0.500000, 0.000000)
max r1.xyz, r1.xyzx, l(0.000000, 0.000000, 0.000000, 0.000000)
movc r1.xyz, r2.wwww, r1.xyzx, r2.xyzx
lt r2.xyzw, r4.yyyy, l(0.200000, 0.400000, 0.600000, 0.800000)
movc r4.y, r2.w, cb4[137].x, cb4[136].w
movc r4.y, r2.z, cb4[137].y, r4.y
movc r4.y, r2.y, cb4[137].z, r4.y
movc r4.y, r2.x, cb4[137].w, r4.y
mov r5.x, v2.w
mov r5.y, v3.w
mov r5.z, v4.w
add r6.xyz, -r5.xyzx, cb0[53].xyzx
dp3 r4.w, r6.xyzx, r6.xyzx
max r5.w, r4.w, l(1.175494351E-38)
rsq r5.w, r5.w
mul r7.xyz, r5.wwww, r6.xyzx
sqrt r6.w, r4.w
ilt r7.w, l(0), cb0[196].x
if_nz r7.w
  ftoi r8.x, cb3[2].z
  iadd r8.y, cb0[196].x, l(-1)
  imin r8.x, r8.y, r8.x
  imax r8.x, r8.x, l(0)
  ld_structured_indexable(structured_buffer, stride=128)(mixed,mixed,mixed,mixed) r9.xyzw, r8.x, l(0), t1.xyzw
  ld_structured_indexable(structured_buffer, stride=128)(mixed,mixed,mixed,mixed) r10.xyzw, r8.x, l(16), t1.xyzw
  ld_structured_indexable(structured_buffer, stride=128)(mixed,mixed,mixed,mixed) r8.y, r8.x, l(32), t1.xxxx
  mov r8.x, r10.w
else
  mov r8.xy, l(0,0,0,0)
  mov r9.xyzw, l(0,0,0,0)
  mov r10.xyz, l(0,0,0,0)
endif
add r10.xyz, -r5.xyzx, r10.xyzx
dp3 r8.z, r10.xyzx, r10.xyzx
max r8.z, r8.z, l(1.175494351E-38)
rsq r8.w, r8.z
mul r9.w, r9.w, r9.w
div r8.z, r8.z, r9.w
add r8.z, -r8.z, l(1.000000)
max r8.z, r8.z, l(0.000000)
add r9.xyz, r9.xyzx, -cb0[197].xyzx
mad r9.xyz, r8.zzzz, r9.xyzx, cb0[197].xyzx
add r9.w, r8.z, l(-1.000000)
mad r8.z, r8.z, r9.w, l(1.000000)
lt r9.w, l(0.500000), cb0[22].x
if_nz r9.w
  movc r9.w, r2.w, cb4[166].x, cb4[165].w
  movc r9.w, r2.z, cb4[166].y, r9.w
  movc r9.w, r2.y, cb4[166].z, r9.w
  movc r9.w, r2.x, cb4[166].w, r9.w
  mad r11.xyz, r3.xywx, cb4[138].xxxx, r5.xyzx
  add r11.xyz, r11.xyzx, -cb3[39].xyzx
  mul r12.xyz, r11.yyyy, cb3[34].xyzx
  mad r11.xyw, cb3[33].xyxz, r11.xxxx, r12.xyxz
  mad r11.xyz, cb3[35].xyzx, r11.zzzz, r11.xywx
  add r11.xyz, r11.xyzx, cb3[36].xyzx
  mad r11.xy, r11.xyxx, cb3[38].xyxx, cb3[38].zwzz
  add r12.xy, r11.xyxx, -cb0[208].xyxx
  sample_c_lz_indexable(texture2d)(float,float,float,float) r10.w, r12.xyxx, t7.xxxx, s1, r11.z
  mad r12.xyzw, cb0[208].xyxy, l(-1.000000, 1.000000, 1.000000, -1.000000), r11.xyxy
  sample_c_lz_indexable(texture2d)(float,float,float,float) r11.w, r12.xyxx, t7.xxxx, s1, r11.z
  add r10.w, r10.w, r11.w
  sample_c_lz_indexable(texture2d)(float,float,float,float) r11.w, r12.zwzz, t7.xxxx, s1, r11.z
  add r10.w, r10.w, r11.w
  add r12.xy, r11.xyxx, cb0[208].xyxx
  sample_c_lz_indexable(texture2d)(float,float,float,float) r11.w, r12.xyxx, t7.xxxx, s1, r11.z
  add r10.w, r10.w, r11.w
  mad r12.xyzw, cb0[208].xyxy, l(-1.414213, 0.000000, 1.414213, 0.000000), r11.xyxy
  sample_c_lz_indexable(texture2d)(float,float,float,float) r11.w, r12.xyxx, t7.xxxx, s1, r11.z
  add r10.w, r10.w, r11.w
  sample_c_lz_indexable(texture2d)(float,float,float,float) r11.w, r12.zwzz, t7.xxxx, s1, r11.z
  add r10.w, r10.w, r11.w
  mad r12.xyzw, cb0[208].xyxy, l(0.000000, -1.414213, 0.000000, 1.414213), r11.xyxy
  sample_c_lz_indexable(texture2d)(float,float,float,float) r11.w, r12.xyxx, t7.xxxx, s1, r11.z
  add r10.w, r10.w, r11.w
  sample_c_lz_indexable(texture2d)(float,float,float,float) r11.w, r12.zwzz, t7.xxxx, s1, r11.z
  add r10.w, r10.w, r11.w
  sample_c_lz_indexable(texture2d)(float,float,float,float) r11.x, r11.xyxx, t7.xxxx, s1, r11.z
  add r10.w, r10.w, r11.x
  mad r10.w, r10.w, l(0.111100), l(-1.000000)
  mul r9.w, r9.w, r10.w
  mul r9.w, r9.w, cb3[37].x
  add r11.xyz, r5.xyzx, -cb2[20].xyzx
  add r12.xyz, r5.xyzx, -cb2[21].xyzx
  add r13.xyz, r5.xyzx, -cb2[22].xyzx
  add r14.xyz, r5.xyzx, -cb2[23].xyzx
  dp3 r11.x, r11.xyzx, r11.xyzx
  dp3 r11.y, r12.xyzx, r12.xyzx
  dp3 r11.z, r13.xyzx, r13.xyzx
  dp3 r11.w, r14.xyzx, r14.xyzx
  lt r11.xyzw, r11.xyzw, cb2[24].xyzw
  and r12.xyzw, r11.xyzw, l(0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000)
  movc r11.xyz, r11.xyzx, l(-1.000000,-1.000000,-1.000000,0), l(-0.000000,-0.000000,-0.000000,0)
  add r11.xyz, r11.xyzx, r12.yzwy
  max r12.yzw, r11.xxyz, l(0.000000, 0.000000, 0.000000, 0.000000)
  dp4 r10.w, r12.xyzw, l(4.000000, 3.000000, 2.000000, 1.000000)
  add r11.z, -r10.w, l(4.000000)
  ftou r10.w, r11.z
  ishl r10.w, r10.w, l(2)
  mul r12.xyz, v3.wwww, cb2[r10.w + 1].xyzx
  mad r12.xyz, cb2[r10.w + 0].xyzx, v2.wwww, r12.xyzx
  mad r12.xyz, cb2[r10.w + 2].xyzx, v4.wwww, r12.xyzx
  add r12.xyz, r12.xyzx, cb2[r10.w + 3].xyzx
  mul r13.xy, r12.xyxx, l(1024.000000, 1024.000000, 0.000000, 0.000000)
  frc r13.xy, r13.xyxx
  dp2 r10.w, r13.xyxx, l(12.989800, 78.233002, 0.000000, 0.000000)
  sincos r10.w, null, r10.w
  mul r10.w, r10.w, l(43758.546875)
  frc r10.w, r10.w
  sincos r13.x, r14.x, r10.w
  mul r15.xyzw, cb2[26].xxyy, l(1.300000, 1.300000, 1.300000, 1.300000)
  mov r13.xz, r13.xxxx
  mov r13.yw, r14.xxxx
  mul r13.xyzw, r15.xyzw, r13.xyzw
  mul r14.xyzw, r13.xyzw, l(-0.172400, -0.978300, -0.978300, 0.172400)
  add r14.xy, r14.ywyy, r14.xzxx
  add r11.xy, r12.xyxx, r14.xyxx
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r10.w, r11.xyzx, t0.xxxx, s1, r12.z
  mul r14.xyzw, r13.xyzw, l(0.874700, 0.484600, 0.484600, -0.874700)
  add r14.xy, r14.ywyy, r14.xzxx
  add r11.xy, r12.xyxx, r14.xyxx
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r11.w, r11.xyzx, t0.xxxx, s1, r12.z
  add r10.w, r10.w, r11.w
  mul r14.xyzw, r13.xyzw, l(-0.968300, -0.037400, -0.037400, 0.968300)
  add r14.xy, r14.ywyy, r14.xzxx
  add r11.xy, r12.xyxx, r14.xyxx
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r11.w, r11.xyzx, t0.xxxx, s1, r12.z
  add r10.w, r10.w, r11.w
  mul r14.xyzw, r13.xyzw, l(0.278300, 0.419600, 0.419600, -0.278300)
  add r14.xy, r14.ywyy, r14.xzxx
  add r11.xy, r12.xyxx, r14.xyxx
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r11.w, r11.xyzx, t0.xxxx, s1, r12.z
  add r10.w, r10.w, r11.w
  mul r14.xyzw, r13.xyzw, l(-0.150700, 0.839100, 0.839100, -0.150700)
  add r14.xy, r14.ywyy, r14.xzxx
  add r11.xy, r12.xyxx, r14.xyxx
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r11.w, r11.xyzx, t0.xxxx, s1, r12.z
  add r10.w, r10.w, r11.w
  mul r14.xyzw, r13.xyzw, l(-0.641700, 0.479300, 0.479300, -0.641700)
  add r14.xy, r14.ywyy, r14.xzxx
  add r11.xy, r12.xyxx, r14.xyxx
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r11.w, r11.xyzx, t0.xxxx, s1, r12.z
  add r10.w, r10.w, r11.w
  mul r14.xyzw, r13.xyzw, l(0.577900, -0.816100, -0.816100, 0.577900)
  add r14.xy, r14.ywyy, r14.xzxx
  add r11.xy, r12.xyxx, r14.xyxx
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r11.w, r11.xyzx, t0.xxxx, s1, r12.z
  add r10.w, r10.w, r11.w
  mul r14.xyzw, r13.xyzw, l(-0.540900, -0.458800, -0.458800, 0.540900)
  add r14.xy, r14.ywyy, r14.xzxx
  add r11.xy, r12.xyxx, r14.xyxx
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r11.w, r11.xyzx, t0.xxxx, s1, r12.z
  add r10.w, r10.w, r11.w
  mul r14.xyzw, r13.xyzw, l(0.704400, -0.191900, -0.191900, 0.704400)
  add r14.xy, r14.ywyy, r14.xzxx
  add r11.xy, r12.xyxx, r14.xyxx
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r11.w, r11.xyzx, t0.xxxx, s1, r12.z
  add r10.w, r10.w, r11.w
  mul r14.xyzw, r13.xyzw, l(0.105300, -0.446400, -0.446400, 0.105300)
  add r14.xy, r14.ywyy, r14.xzxx
  add r11.xy, r12.xyxx, r14.xyxx
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r11.w, r11.xyzx, t0.xxxx, s1, r12.z
  add r10.w, r10.w, r11.w
  mul r13.xyzw, r13.xyzw, l(-0.206600, 0.066100, 0.066100, -0.206600)
  add r13.xy, r13.ywyy, r13.xzxx
  add r11.xy, r12.xyxx, r13.xyxx
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r11.x, r11.xyzx, t0.xxxx, s1, r12.z
  add r10.w, r10.w, r11.x
  mul r10.w, r10.w, cb2[25].x
  add r11.x, -cb2[25].x, l(1.000000)
  mad r10.w, r10.w, l(0.090900), r11.x
  ge r11.x, l(0.000000), r12.z
  ge r11.y, r12.z, l(1.000000)
  or r11.x, r11.y, r11.x
  movc r10.w, r11.x, l(1.000000), r10.w
  add r11.x, -r8.x, l(1.000000)
  mad r8.x, cb3[40].y, r11.x, r8.x
  mul r8.x, r8.x, r10.w
  mad_sat r8.y, r8.y, l(2.000000), l(-1.000000)
  mul r10.w, r8.y, cb0[197].w
  lt r11.x, l(0.500000), cb3[37].x
  mad r8.y, r8.y, r9.w, l(1.000000)
  min r8.y, r8.y, l(1.000000)
  movc r8.y, r11.x, r8.y, l(1.000000)
  mul r8.x, r8.x, r8.y
  add r8.y, -cb4[136].z, l(1.000000)
  mad r8.y, r8.x, cb4[136].z, r8.y
  mul r9.w, r10.w, cb4[136].z
  mad r10.w, -r10.w, cb4[136].z, l(1.000000)
  mad r8.x, r8.x, r9.w, r10.w
else
  mov r8.xy, l(1.000000,1.000000,0,0)
endif
mul_sat r9.w, cb3[1].w, l(2.500000)
add r10.w, -r9.w, l(1.000000)
mad r8.x, r10.w, r8.x, r9.w
add r9.xyz, r9.xyzx, l(-0.800000, -0.800000, -0.800000, 0.000000)
mad r9.xyz, cb4[58].wwww, r9.xyzx, l(0.800000, 0.800000, 0.800000, 0.000000)
rsq r4.w, r4.w
mul r11.xyz, r4.wwww, r6.xyzx
mad r10.xyz, r10.xyzx, r8.wwww, -r11.xyzx
mad r10.xyz, cb4[58].wwww, r10.xyzx, r11.xyzx
ne r4.w, cb3[3].w, l(0.000000)
dp3 r5.x, cb3[3].xyzx, r5.xyzx
add_sat r5.x, r5.x, -cb3[3].w
mul r11.xyz, r9.xyzx, cb0[15].xyzx
add r12.xyz, r9.xyzx, cb0[15].xyzx
mad r12.xyz, -r9.xyzx, cb0[15].xyzx, r12.xyzx
mad r11.xyz, cb0[15].wwww, r12.xyzx, r11.xyzx
add r11.xyz, -r9.xyzx, r11.xyzx
mad r5.xyz, r5.xxxx, r11.xyzx, r9.xyzx
movc r5.xyz, r4.wwww, r5.xyzx, r9.xyzx
max r4.y, r4.y, l(0.000010)
rcp r4.w, r4.y
dp3 r8.w, r3.xywx, r10.xyzx
add r9.x, r8.w, l(1.000000)
mul r9.y, r10.y, l(3.000000)
min r9.y, r9.y, l(1.000000)
mad r9.y, -r9.y, l(0.500000), r3.y
add_sat r9.y, r9.y, l(1.500000)
mad r9.x, r9.x, r9.y, l(-1.000000)
add r9.x, -r8.w, r9.x
mad r9.x, v7.y, r9.x, r8.w
mad r3.z, r3.z, l(2.000000), r9.x
mul r9.x, r3.z, l(3.000000)
mul r11.xy, r4.yyyy, l(1.500000, 4.500000, 0.000000, 0.000000)
mad r9.yz, r3.zzzz, l(0.000000, 3.000000, 3.000000, 0.000000), -r11.xxyx
add r9.xyz, r9.xyzx, l(3.000000, 1.000000, -1.000000, 0.000000)
mad r4.y, -r4.y, l(3.000000), l(2.000000)
div r9.xyw, r9.xyxz, r4.yyyy
add r11.xyz, -r9.xywx, l(1.000000, 1.000000, 1.000000, 0.000000)
add r12.xyz, r3.zzzz, l(0.333300, -0.333300, -0.333300, 0.000000)
mad r12.xyz, r4.wwww, r12.xyzx, l(0.500000, 0.500000, -0.500000, 0.000000)
add r13.xyz, -r12.xyzx, l(1.000000, 1.000000, 1.000000, 0.000000)
min r14.xy, r9.yxyy, r13.yxyy
min r9.xz, r11.yyzy, r12.xxzx
mov r14.z, r11.x
mov r14.w, r9.x
mov_sat r11.xyz, r14.zywz
min_sat r14.y, r12.y, r13.z
mov_sat r14.x, r14.x
mov_sat r9.zw, r9.zzzw
add r4.y, -r8.x, r8.y
mad r4.y, cb3[40].y, r4.y, r8.x
mad r12.xyzw, r4.yyyy, l(2.000000, 2.000000, -2.000000, -2.000000), l(0.000000, -1.000000, 1.000000, 2.000000)
min_sat r12.x, r12.w, r12.x
mov_sat r12.yz, r12.yyzy
lt r4.y, l(0.500000), cb3[40].y
if_nz r4.y
  if_nz r7.w
    ftoi r4.y, cb3[2].z
    iadd r4.w, cb0[196].x, l(-1)
    imin r4.y, r4.w, r4.y
    imax r4.y, r4.y, l(0)
    ld_structured_indexable(structured_buffer, stride=128)(mixed,mixed,mixed,mixed) r4.y, r4.y, l(32), t1.xxxx
  else
    mov r4.y, l(0)
  endif
  mul r4.y, r4.y, cb0[197].w
  mul r13.y, r4.y, r12.x
  mad r9.xy, -r12.zxzz, r4.yyyy, r12.zxzz
  add r4.w, r9.y, r9.x
  add r13.z, r4.w, r12.y
  if_nz r7.w
    ftoi r4.w, cb3[2].z
    iadd r9.x, cb0[196].x, l(-1)
    imin r4.w, r4.w, r9.x
    imax r4.w, r4.w, l(0)
    ld_structured_indexable(structured_buffer, stride=128)(mixed,mixed,mixed,mixed) r4.w, r4.w, l(28), t1.xxxx
  else
    mov r4.w, l(0)
  endif
  mul r12.xy, r4.wwww, r13.yzyy
  mad r9.xy, -r13.yzyy, r4.wwww, r13.yzyy
  add r4.w, r9.y, r9.x
  mad r12.z, r12.z, r4.y, r4.w
else
  if_nz r7.w
    ftoi r4.y, cb3[2].z
    iadd r4.w, cb0[196].x, l(-1)
    imin r4.y, r4.w, r4.y
    imax r4.y, r4.y, l(0)
    ld_structured_indexable(structured_buffer, stride=128)(mixed,mixed,mixed,mixed) r4.y, r4.y, l(32), t1.xxxx
  else
    mov r4.y, l(0)
  endif
  mul r12.xz, r4.yyyy, r12.xxzx
endif
add r4.y, -r11.x, l(1.000000)
add r4.y, -r11.y, r4.y
add r4.y, -r11.z, r4.y
mad r4.y, r12.z, r4.y, r11.z
add r4.w, r12.y, r12.x
mul r9.xy, r4.wwww, r14.xyxx
add r4.w, r9.w, r9.z
mad r4.w, r4.w, r12.x, r9.y
mul r9.y, r9.z, r12.y
mul r9.z, r11.x, v7.x
mad r10.w, -r11.x, v7.x, r11.x
add r10.w, r10.w, r11.y
movc r11.xyz, r2.wwww, cb4[61].xyzx, cb4[60].xyzx
movc r11.xyz, r2.zzzz, cb4[62].xyzx, r11.xyzx
movc r11.xyz, r2.yyyy, cb4[63].xyzx, r11.xyzx
movc r11.xyz, r2.xxxx, cb4[64].xyzx, r11.xyzx
movc r12.xzw, r2.wwww, cb4[66].xxyz, cb4[65].xxyz
movc r12.xzw, r2.zzzz, cb4[67].xxyz, r12.xxzw
movc r12.xzw, r2.yyyy, cb4[68].xxyz, r12.xxzw
movc r12.xzw, r2.xxxx, cb4[69].xxyz, r12.xxzw
movc r13.xyz, r0.wwww, cb0[10].xyzx, cb0[3].xyzx
movc r14.xyz, r0.wwww, cb0[11].xyzx, cb0[4].xyzx
movc r15.xyz, r0.wwww, cb0[12].xyzx, cb0[5].xyzx
movc r16.xyz, r0.wwww, cb0[9].xyzx, cb0[6].xyzx
movc r17.xyz, r0.wwww, cb0[13].xyzx, cb0[7].xyzx
movc r18.xyz, r0.wwww, cb0[14].xyzx, cb0[8].xyzx
mul r11.w, r6.w, l(0.437250)
min r11.w, r11.w, l(1.000000)
mad r19.x, r11.w, cb3[1].w, -r11.w
mad r19.y, -r11.w, cb3[1].w, r11.w
add r19.xy, r19.xyxx, l(1.000000, -1.000000, 0.000000, 0.000000)
mad r19.xy, cb4[136].yyyy, r19.xyxx, l(0.000000, 1.000000, 0.000000, 0.000000)
add r11.xyz, r11.xyzx, l(0.0000610351562, 0.0000610351562, 0.0000610351562, 0.000000)
add r11.w, r11.y, r11.x
add r11.w, r11.z, r11.w
mul r11.w, r11.w, l(0.333330)
div_sat r20.xyz, r11.xyzx, r11.wwww
mul r11.xyz, r19.yyyy, r11.xyzx
mad r11.xyz, r20.xyzx, r19.xxxx, r11.xyzx
add r12.xzw, r12.xxzw, l(0.0000610351562, 0.000000, 0.0000610351562, 0.0000610351562)
add r11.w, r12.z, r12.x
add r11.w, r12.w, r11.w
mul r11.w, r11.w, l(0.333330)
div_sat r20.xyz, r12.xzwx, r11.wwww
mul r12.xzw, r19.yyyy, r12.xxzw
mad r12.xzw, r20.xxyz, r19.xxxx, r12.xxzw
mul r13.xyz, r13.xyzx, r11.xyzx
mul r11.xyz, r14.xyzx, r11.xyzx
mul r14.xyz, r15.xyzx, r12.xzwx
mul r12.xzw, r18.xxyz, r12.xxzw
add r15.xyz, r5.xyzx, l(1.175494351E-38, 1.175494351E-38, 1.175494351E-38, 0.000000)
max r11.w, r15.y, r15.x
max r11.w, r15.z, r11.w
rcp r11.w, r11.w
if_nz r7.w
  ftoi r13.w, cb3[2].z
  iadd r14.w, cb0[196].x, l(-1)
  imin r13.w, r13.w, r14.w
  imax r13.w, r13.w, l(0)
  ld_structured_indexable(structured_buffer, stride=128)(mixed,mixed,mixed,mixed) r13.w, r13.w, l(32), t1.xxxx
else
  mov r13.w, l(0)
endif
add r14.w, -r8.z, l(1.000000)
mad r8.z, r13.w, r14.w, r8.z
mul r15.xyz, r5.xyzx, r8.zzzz
min r8.z, r11.w, l(1.000000)
mul r18.xyz, r8.zzzz, r15.xyzx
mul r16.xyz, r9.yyyy, r16.xyzx
mad r16.xyz, r17.xyzx, r4.wwww, r16.xyzx
mad r16.xyz, r9.wwww, r12.yyyy, r16.xyzx
mul r14.xyz, r10.wwww, r14.xyzx
mad r9.yzw, r12.xxzw, r9.zzzz, r14.xxyz
mad r9.yzw, r11.xxyz, r4.yyyy, r9.yyzw
mad r9.xyz, r13.xyzx, r9.xxxx, r9.yzwy
mul r9.xyz, r18.xyzx, r9.xyzx
mad r9.xyz, r15.xyzx, r16.xyzx, r9.xyzx
add r9.xyz, -r5.xyzx, r9.xyzx
mad r5.xyz, cb4[58].wwww, r9.xyzx, r5.xyzx
lt r4.y, l(0.500000), v7.z
if_z r0.w
  movc r9.xyz, r4.yyyy, r3.xywx, v2.xyzx
  dp3 r0.w, r1.xyzx, l(0.290000, 0.600000, 0.110000, 0.000000)
  lt r4.w, v7.z, l(0.500000)
  mad r11.xy, r0.wwww, l(0.287500, 0.400000, 0.000000, 0.000000), l(1.437500, 1.000000, 0.000000, 0.000000)
  dp3 r8.z, r10.xyzx, r9.xyzx
  add r9.x, -r8.w, r8.z
  mad_sat r9.x, -r9.x, l(3.000000), l(1.000000)
  add r9.y, r9.x, r9.x
  sqrt r9.x, r9.x
  mul r9.x, r9.x, r9.y
  min r9.x, r9.x, l(1.000000)
  mad r9.y, r8.w, l(0.500000), l(0.500000)
  mov_sat r9.z, r8.w
  mad r9.x, r9.y, r9.x, -r9.z
  mad r9.x, r9.x, l(0.500000), r9.z
  mov_sat r8.z, r8.z
  max r9.y, r1.z, r1.y
  max r9.y, r1.x, r9.y
  lt r9.z, l(1.000000), r9.y
  div r12.xyz, r1.xyzx, r9.yyyy
  movc r9.yzw, r9.zzzz, r12.xxyz, r1.xxyz
  add r10.w, -r11.x, l(1.000000)
  mad r9.x, r9.x, r10.w, r11.x
  log r9.yzw, r9.yyzw
  mul r9.xyz, r9.yzwy, r9.xxxx
  exp r9.xyz, r9.xyzx
  add r11.xzw, -r1.xxyz, r9.xxyz
  mad r11.xzw, r11.xxzw, l(0.500000, 0.000000, 0.500000, 0.500000), r1.xxyz
  add r9.xyz, r9.xyzx, -r11.xzwx
  mad r9.xyz, r8.zzzz, r9.xyzx, r11.xzwx
  mad r0.w, -r0.w, l(0.050000), l(1.050000)
  log r11.xzw, r1.xxyz
  mul r11.xyz, r11.xzwx, r11.yyyy
  exp r11.xyz, r11.xyzx
  mul r11.xyz, r0.wwww, r11.xyzx
  movc r1.xyz, r4.wwww, r9.xyzx, r11.xyzx
endif
mad r0.w, -r0.x, l(0.960000), l(0.960000)
mul r9.xyz, r0.wwww, r1.xyzx
add r11.xyz, r1.xyzx, l(-0.040000, -0.040000, -0.040000, 0.000000)
mad r11.xyz, r0.xxxx, r11.xyzx, l(0.040000, 0.040000, 0.040000, 0.000000)
mad r4.w, -r0.z, cb4[140].w, l(1.000000)
mul r4.w, r4.w, r4.w
mad r8.z, r4.w, l(4.000000), l(2.000000)
mul r9.w, r4.w, r4.w
mad r10.w, r4.w, r4.w, l(-1.000000)
add r12.xyz, r5.xyzx, cb0[2].xyzx
add r12.xyz, r12.xyzx, v8.xyzx
dp3 r11.w, r12.xyzx, l(0.212672904, 0.715152204, 0.072175, 0.000000)
add r12.x, -cb0[19].x, cb0[19].y
div r12.y, l(1.000000, 1.000000, 1.000000, 1.000000), r12.x
mad r12.z, -cb0[19].x, r12.y, l(1.000000)
mad r12.y, r11.w, r12.y, r12.z
rcp r12.y, r12.y
mad r12.x, -r12.x, r12.y, cb0[19].y
lt r12.y, r11.w, cb0[19].x
movc r12.x, r12.y, r11.w, r12.x
add r11.w, r11.w, l(0.000100)
div r11.w, r12.x, r11.w
mul r12.xyz, r5.xyzx, r11.wwww
movc r13.xyz, r2.wwww, cb4[76].xyzx, cb4[75].xyzx
movc r13.xyz, r2.zzzz, cb4[77].xyzx, r13.xyzx
movc r13.xyz, r2.yyyy, cb4[78].xyzx, r13.xyzx
movc r13.xyz, r2.xxxx, cb4[79].xyzx, r13.xyzx
movc r12.w, r2.w, cb4[144].z, cb4[144].y
movc r12.w, r2.z, cb4[144].w, r12.w
movc r12.w, r2.y, cb4[145].x, r12.w
movc r12.w, r2.x, cb4[145].y, r12.w
lt r13.w, l(0.500000), r12.w
if_nz r13.w
  mad_sat r3.z, r3.z, l(1.500000), l(-0.500000)
  movc r13.w, r2.w, cb4[145].w, cb4[145].z
  movc r13.w, r2.z, cb4[146].x, r13.w
  movc r13.w, r2.y, cb4[146].y, r13.w
  movc r13.w, r2.x, cb4[146].z, r13.w
  add r3.z, r3.z, r4.x
  add r3.z, r3.z, l(-1.000000)
  max r13.w, r13.w, l(0.000010)
  div_sat r4.x, r3.z, r13.w
endif
mul r3.z, r4.x, cb4[146].w
mul r13.xyz, r13.xyzx, r3.zzzz
mul r13.xyz, r11.xyzx, r13.xyzx
lt r3.z, r12.w, l(0.500000)
mad r6.xyz, r6.xyzx, r5.wwww, r10.xyzx
dp3 r4.x, r6.xyzx, r6.xyzx
rsq r4.x, r4.x
mul r6.xyz, r4.xxxx, r6.xyzx
movc r4.x, r2.w, cb4[143].y, cb4[143].x
movc r4.x, r2.z, cb4[143].z, r4.x
movc r4.x, r2.y, cb4[143].w, r4.x
movc r4.x, r2.x, cb4[144].x, r4.x
mul r5.w, r8.w, r4.x
mad_sat r5.w, r5.w, l(0.750000), l(0.250000)
dp3 r12.w, r3.xywx, r6.xyzx
mul r12.w, r4.x, r12.w
mad_sat r12.w, r12.w, l(0.750000), l(0.250000)
dp3 r6.x, r10.xyzx, r6.xyzx
mul r4.x, r4.x, r6.x
mad_sat r4.x, r4.x, l(0.750000), l(0.250000)
mul r6.x, r12.w, r12.w
mad r6.x, r6.x, r10.w, l(1.000010)
mul r4.x, r4.x, r4.x
mul r6.x, r6.x, r6.x
max r4.x, r4.x, l(0.100000)
mul r4.x, r4.x, r6.x
mul r4.x, r8.z, r4.x
div r4.x, r9.w, r4.x
mad_sat r0.z, -r0.z, cb4[140].w, r4.x
mul r0.z, r5.w, r0.z
max r4.x, r4.w, l(0.000010)
div r0.z, r0.z, r4.x
movc r4.x, r2.w, cb4[142].x, cb4[141].w
movc r4.x, r2.z, cb4[142].y, r4.x
movc r4.x, r2.y, cb4[142].z, r4.x
movc r4.x, r2.x, cb4[142].w, r4.x
movc r4.w, r2.w, cb4[169].x, cb4[168].w
movc r4.w, r2.z, cb4[169].y, r4.w
movc r4.w, r2.y, cb4[169].z, r4.w
movc r4.w, r2.x, cb4[169].w, r4.w
mul r4.x, r4.w, r4.x
mul r0.z, r0.z, r4.x
mul_sat r0.z, r0.z, l(10.000000)
mul r0.z, r0.z, l(100.000000)
movc r0.z, r3.z, r0.z, l(16.667000)
mul r6.xyz, r13.xyzx, r0.zzzz
mul r13.xyz, r12.xyzx, r6.xyzx
mad r6.xyz, r6.xyzx, r12.xyzx, l(-1.000000, -1.000000, -1.000000, 0.000000)
max r6.xyz, r6.xyzx, l(0.000000, 0.000000, 0.000000, 0.000000)
mad r13.xyz, r9.xyzx, r12.xyzx, r13.xyzx
ge r0.z, cb4[147].x, l(0.500000)
movc r14.xyz, r2.wwww, cb4[81].xyzx, cb4[80].xyzx
movc r14.xyz, r2.zzzz, cb4[82].xyzx, r14.xyzx
movc r14.xyz, r2.yyyy, cb4[83].xyzx, r14.xyzx
movc r14.xyz, r2.xxxx, cb4[84].xyzx, r14.xyzx
mul r14.xyz, r0.yyyy, r14.xyzx
mul r14.xyz, r1.xyzx, r14.xyzx
and r14.xyz, r0.zzzz, r14.xyzx
add r0.y, r14.y, r14.x
add r0.y, r14.z, r0.y
mul r15.xyz, r11.wwww, v8.xyzx
mad r13.xyz, r15.xyzx, r9.xyzx, r13.xyzx
if_nz r7.w
  ftoi r0.z, cb3[2].z
  iadd r3.z, cb0[196].x, l(-1)
  imin r0.z, r0.z, r3.z
  imax r0.z, r0.z, l(0)
  ld_structured_indexable(structured_buffer, stride=128)(mixed,mixed,mixed,mixed) r16.xyz, r0.z, l(96), t1.xyzx
  ld_structured_indexable(structured_buffer, stride=128)(mixed,mixed,mixed,mixed) r17.xyz, r0.z, l(112), t1.xyzx
else
  mov r16.xyz, l(0,0,0,0)
  mov r17.xyz, l(0,0,0,0)
endif
lt r0.z, l(0.500000), cb0[23].y
movc r16.xyz, r1.wwww, r16.xyzx, r17.xyzx
movc r16.xyz, r0.zzzz, l(0.050000,0.050000,0.050000,0), r16.xyzx
mul r17.xyz, r1.xyzx, r11.wwww
mad r13.xyz, r16.xyzx, r17.xyzx, r13.xyzx
add r6.xyz, r6.xyzx, r13.xyzx
ge r0.z, l(0.500000), cb0[196].y
if_nz r0.z
  dp3 r0.z, r7.xyzx, r10.xyzx
  mad_sat r0.z, -r0.z, l(0.500000), l(0.500000)
  mad r1.x, r0.z, l(0.800000), l(0.200000)
  mad r1.y, r3.y, l(0.500000), l(0.500000)
  mul r3.z, r1.y, r1.y
  movc r1.y, r1.w, r1.y, r3.z
  add r1.y, r1.y, l(-0.200000)
  mul_sat r1.y, r1.y, l(1.250000)
  mad r3.z, r1.y, l(-2.000000), l(3.000000)
  mul r1.y, r1.y, r1.y
  mul r1.y, r1.y, r3.z
  mul r3.z, r1.y, r1.y
  mul r3.z, r3.z, r3.z
  mul r3.z, r1.y, r3.z
  movc r10.xyz, r1.wwww, l(1.000000,0.300000,-1.000000,0), l(0.500000,1.000000,-0.500000,0)
  add r4.x, r10.z, r10.y
  mad r3.z, r3.z, r4.x, r10.x
  mad r1.y, r1.y, r3.z, l(-0.100000)
  mad r1.y, cb0[207].x, r1.y, l(0.100000)
  mad r3.z, r8.w, l(0.500000), l(0.500000)
  mul r3.z, r8.x, r3.z
  mad r3.z, r3.z, l(1.400000), l(0.100000)
  if_nz r7.w
    ftoi r4.x, cb3[2].z
    iadd r4.w, cb0[196].x, l(-1)
    imin r4.x, r4.w, r4.x
    imax r4.x, r4.x, l(0)
    ld_structured_indexable(structured_buffer, stride=128)(mixed,mixed,mixed,mixed) r4.x, r4.x, l(32), t1.xxxx
  else
    mov r4.x, l(0)
  endif
  mad r4.x, r4.x, l(0.399999976), r8.y
  add_sat r4.x, r4.x, l(0.600000)
  mul r1.x, r1.y, r1.x
  mul r1.x, r3.z, r1.x
  mul r1.x, r4.x, r1.x
  ne r1.y, l(0.000000, 0.000000, 0.000000, 0.000000), v7.z
  mov_sat r3.z, -r7.y
  mad r4.x, r4.z, cb4[140].z, l(2.500000)
  movc r1.y, r1.y, l(4.500000), r4.x
  mad r1.y, r3.z, r1.y, l(-0.500000)
  mad r1.y, cb0[207].y, r1.y, l(1.000000)
  mul r1.x, r1.y, r1.x
  mul r1.y, r6.w, l(0.0833333358)
  min r1.y, r1.y, l(1.000000)
  mad r4.xz, r1.yyyy, l(-0.300000, 0.000000, -0.300000, 0.000000), l(0.500000, 0.000000, 0.600000, 0.000000)
  dp3 r1.y, r7.xyzx, r3.xywx
  add r1.y, -r1.y, l(1.000000)
  add r4.xz, -r4.xxzx, r1.yyyy
  mul_sat r4.xz, r4.xxzx, l(3.33333325, 0.000000, 5.00000048, 0.000000)
  mad r7.xy, r4.xzxx, l(-2.000000, -2.000000, 0.000000, 0.000000), l(3.000000, 3.000000, 0.000000, 0.000000)
  mul r4.xz, r4.xxzx, r4.xxzx
  mul r4.xz, r4.xxzx, r7.xxyx
  movc r1.y, r1.w, r4.x, r4.z
  dp3 r3.z, cb0[197].xyzx, l(0.330000, 0.330000, 0.330000, 0.000000)
  mul r4.xzw, cb0[197].xxyz, cb0[197].xxyz
  mul r4.xzw, r4.xxzw, r4.xxzw
  mul r4.xzw, r4.xxzw, r4.xxzw
  dp3 r5.w, r4.xzwx, l(0.700000, 0.700000, 0.700000, 0.000000)
  max r5.w, r5.w, l(0.010000)
  rcp r5.w, r5.w
  mul r3.z, r3.z, r5.w
  mad r4.xzw, r3.zzzz, r4.xxzw, -r12.xxyz
  mad r4.xzw, r8.yyyy, r4.xxzw, r12.xxyz
  mul r0.z, r0.z, r0.z
  log r0.z, r0.z
  mul r0.z, r0.z, l(20.000000)
  exp r0.z, r0.z
  mad r7.xyz, r5.xyzx, r11.wwww, -r4.xzwx
  mad r4.xzw, r0.zzzz, r7.xxyz, r4.xxzw
  mad r4.xzw, -r5.xxyz, r11.wwww, r4.xxzw
  mad r4.xzw, cb0[207].zzzz, r4.xxzw, r12.xxyz
  add r0.z, r9.y, r9.x
  mad r0.z, r1.z, r0.w, r0.z
  mul r0.z, r0.z, l(0.330000)
  mul r0.z, r0.z, r0.z
  mul r0.z, r0.z, cb0[207].w
  mad r0.z, r0.z, l(-0.199999988), l(1.000000)
  mov_sat r9.xyz, r9.xyzx
  log r5.xyz, r9.xyzx
  mul r5.xyz, r5.xyzx, l(0.200000, 0.200000, 0.200000, 0.000000)
  exp r5.xyz, r5.xyzx
  dp3 r0.w, r5.xyzx, r5.xyzx
  max r0.w, r0.w, l(0.0000610351562)
  rsq r0.w, r0.w
  mul r5.xyz, r0.wwww, r5.xyzx
  mul r0.w, cb0[206].w, l(48.000000)
  mul r0.z, r0.z, l(0.100000)
  mul r7.xyz, r5.xyzx, r0.zzzz
  mad r5.xyz, -r0.zzzz, r5.xyzx, r11.xyzx
  mad r5.xyz, r0.xxxx, r5.xyzx, r7.xyzx
  mul r0.xzw, r0.wwww, r5.xxyz
  mul r1.x, r1.y, r1.x
  mul r1.xyz, r4.xzwx, r1.xxxx
  mul r0.xzw, r0.xxzw, r1.xxyz
  movc r1.xyz, r2.wwww, cb4[88].xyzx, cb4[87].xyzx
  movc r1.xyz, r2.zzzz, cb4[89].xyzx, r1.xyzx
  movc r1.xyz, r2.yyyy, cb4[90].xyzx, r1.xyzx
  movc r1.xyz, r2.xxxx, cb4[91].xyzx, r1.xyzx
  mul r0.xzw, r0.xxzw, r1.xxyz
  mad_sat r1.x, r6.w, l(0.200000), l(-1.000000)
  mad r1.x, r1.x, l(-0.700000), l(1.000000)
  mul r2.xyz, r0.xzwx, r1.xxxx
  add r0.x, r2.y, r2.x
  mad r0.x, r0.w, r1.x, r0.x
  mul r0.x, r0.x, r0.x
  mad r0.x, r0.x, l(0.050000), l(1.000000)
  mul r0.xzw, r0.xxxx, r2.xxyz
  movc r1.x, r4.y, l(0.500000), l(1.000000)
  mul r0.xzw, r0.xxzw, r1.xxxx
  lt r1.x, l(0.500000), cb3[1].w
  min r2.xyz, r0.xzwx, l(0.700000, 0.700000, 0.700000, 0.000000)
  movc r0.xzw, r1.xxxx, r2.xxyz, r0.xxzw
  mul r0.xzw, r0.xxzw, cb0[206].xxyz
else
  mov r0.xzw, l(0,0,0,0)
endif
mul r0.xzw, r0.xxzw, cb4[58].wwww
mad r1.xyz, r15.xyzx, l(2.000000, 2.000000, 2.000000, 0.000000), l(1.000000, 1.000000, 1.000000, 0.000000)
mul r2.xyz, r0.xzwx, r1.xyzx
add r4.xyz, r14.xyzx, r6.xyzx
dp3 r2.x, r2.xyzx, l(0.299000, 0.587000, 0.114000, 0.000000)
mad r0.xzw, r0.xxzw, r1.xxyz, -r2.xxxx
mad r0.xzw, cb4[41].xxxx, r0.xxzw, r2.xxxx
add r0.xzw, r0.xxzw, l(-0.500000, 0.000000, -0.500000, -0.500000)
mad r0.xzw, cb4[41].yyyy, r0.xxzw, l(0.500000, 0.000000, 0.500000, 0.500000)
max r0.xzw, r0.xxzw, l(0.000000, 0.000000, 0.000000, 0.000000)
mul r0.xzw, r0.xxzw, cb4[41].yyyy
dp3 r1.x, r4.xyzx, l(0.299000, 0.587000, 0.114000, 0.000000)
add r2.xyz, -r1.xxxx, r4.xyzx
mad r1.xyz, cb4[41].xxxx, r2.xyzx, r1.xxxx
add r1.xyz, r1.xyzx, l(-0.500000, -0.500000, -0.500000, 0.000000)
mad r1.xyz, cb4[41].yyyy, r1.xyzx, l(0.500000, 0.500000, 0.500000, 0.000000)
max r1.xyz, r1.xyzx, l(0.000000, 0.000000, 0.000000, 0.000000)
mul r2.x, cb4[29].w, cb4[158].w
add r2.yzw, -r1.xxyz, cb4[29].xxyz
mad o0.xyz, r2.xxxx, r2.yzwy, r1.xyzx
mad r1.x, -cb4[29].w, cb4[158].w, l(1.000000)
mul r0.xzw, r0.xxzw, r1.xxxx
max r0.xzw, r0.xxzw, l(0.000000, 0.000000, 0.000000, 0.000000)
sqrt r0.xzw, r0.xxzw
mul r0.xzw, r0.xxzw, l(0.200000, 0.000000, 0.200000, 0.200000)
min o1.xyz, r0.xzwx, l(1.000000, 1.000000, 1.000000, 0.000000)
mul o1.w, r0.y, l(0.333300)
movc o2.w, r1.w, l(0.340000), l(0)
mad o3.xyz, r3.xywx, l(0.500000, 0.500000, 0.500000, 0.000000), l(0.500000, 0.500000, 0.500000, 0.000000)
mov o0.w, l(1.000000)
mov o3.w, l(1.000000)
ret
// Approximately 0 instruction slots used

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
