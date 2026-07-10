// ---- Created with 3Dmigoto v1.3.16 on Fri Jul 10 23:16:16 2026
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
  float4 cb4[167];
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
  float4 v0 : SV_POSITION0,
  float4 v1 : TEXCOORD0,
  float4 v2 : TEXCOORD1,
  float4 v3 : TEXCOORD5,
  float4 v4 : TEXCOORD6,
  float4 v5 : TEXCOORD4,
  float4 v6 : TEXCOORD3,
  float4 v7 : TEXCOORD7,
  float3 v8 : TEXCOORD8,
  out float4 o0 : SV_Target0,
  out float4 o1 : SV_Target1,
  out float4 o2 : SV_Target2,
  out float4 o3 : SV_Target3)
{
  float4 r0,r1,r2,r3,r4,r5,r6,r7,r8,r9,r10;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.x = cmp(0.5 < cb1[28].y);
  r0.yz = v1.zw / v2.xx;
  r1.xy = v2.yz / v2.ww;
  r0.yz = -r1.xy + r0.yz;
  r0.yz = float2(0.5,-0.5) * r0.yz;
  r0.xy = r0.xx ? r0.yz : 0;
  r0.zw = sqrt(abs(r0.xy));
  r1.xy = cmp(float2(0,0) < r0.xy);
  r0.xy = cmp(r0.xy < float2(0,0));
  r0.xy = (int2)-r1.xy + (int2)r0.xy;
  r0.xy = (int2)r0.xy;
  r0.xy = r0.zw * r0.xy;
  o2.xy = r0.xy * float2(0.5,0.5) + float2(0.498039216,0.498039216);
  r0.x = cmp(cb3[40].x == 1.000000);
  r0.yz = cmp(float2(0,0) != cb4[154].wz);
  r0.w = cmp(0 != cb4[155].x);
  bitmask.x = ((~(-1 << 1)) << 2) & 0xffffffff;  r0.x = (((uint)r0.x << 2) & bitmask.x) | ((uint)0 & ~bitmask.x);
  bitmask.z = ((~(-1 << 1)) << 1) & 0xffffffff;  r0.z = (((uint)r0.z << 1) & bitmask.z) | ((uint)0 & ~bitmask.z);
  bitmask.w = ((~(-1 << 1)) << 3) & 0xffffffff;  r0.w = (((uint)r0.w << 3) & bitmask.w) | ((uint)0 & ~bitmask.w);
  r0.x = (int)r0.w + (int)r0.x;
  r0.x = (int)r0.x + (int)r0.z;
  bitmask.x = ((~(-1 << 1)) << 0) & 0xffffffff;  r0.x = (((uint)r0.y << 0) & bitmask.x) | ((uint)r0.x & ~bitmask.x);
  r0.x = (uint)r0.x;
  o2.z = 0.00392156886 * r0.x;
  r0.x = cmp(0.5 < cb4[150].y);
  r0.y = cmp(1 < v5.x);
  r0.x = r0.y ? r0.x : 0;
  r0.xy = r0.xx ? v5.xy : v1.xy;
  r0.xyz = t2.SampleBias(s0_s, r0.xy, cb0[199].x).xyz;
  r0.xyz = cb4[58].xyz * r0.xyz;
  r1.xy = t3.SampleBias(s0_s, v1.xy, cb0[199].x).xy;
  r0.w = cb4[140].z * r1.y;
  r1.xyzw = cmp(r1.xxxx < float4(0.200000003,0.400000006,0.600000024,0.800000012));
  r2.xyzw = r1.wwww ? cb4[71].xyzw : cb4[70].xyzw;
  r2.xyzw = r1.zzzz ? cb4[72].xyzw : r2.xyzw;
  r2.xyzw = r1.yyyy ? cb4[73].xyzw : r2.xyzw;
  r2.xyzw = r1.xxxx ? cb4[74].xyzw : r2.xyzw;
  r3.x = dot(v7.xyz, v7.xyz);
  r3.x = rsqrt(r3.x);
  r3.xyz = v7.xyz * r3.xxx;
  r4.xyz = cb0[118].xyz * r3.yyy;
  r3.xyw = cb0[117].xyz * r3.xxx + r4.xyz;
  r3.xyz = cb0[119].xyz * r3.zzz + r3.xyw;
  r3.xyz = float3(1,1,-1) * r3.xyz;
  r4.xyz = cb0[122].xyz * r3.yyy;
  r3.xyw = cb0[121].xyz * r3.xxx + r4.xyz;
  r3.xyz = cb0[123].xyz * r3.zzz + r3.xyw;
  r3.w = dot(r3.xyz, r3.xyz);
  r3.w = rsqrt(r3.w);
  r3.xyz = r3.xyz * r3.www;
  r3.w = cmp(0 < asint(cb0[196].x));
  if (r3.w != 0) {
    r4.x = (int)cb3[2].z;
    r4.y = asint(cb0[196].x) + -1;
    r4.x = min((int)r4.x, (int)r4.y);
    r4.x = max(0, (int)r4.x);
    r5.x = t1[r4.x].val[0/4];
    r5.y = t1[r4.x].val[0/4+1];
    r5.z = t1[r4.x].val[0/4+2];
    r5.w = t1[r4.x].val[0/4+3];
    r6.x = t1[r4.x].val[16/4];
    r6.y = t1[r4.x].val[16/4+1];
    r6.z = t1[r4.x].val[16/4+2];
    r6.w = t1[r4.x].val[16/4+3];
    r4.y = t1[r4.x].val[32/4];
    r4.x = r6.w;
  } else {
    r4.xy = float2(0,0);
    r5.xyzw = float4(0,0,0,0);
    r6.xyz = float3(0,0,0);
  }
  r6.xyz = -v3.xyz + r6.xyz;
  r4.z = dot(r6.xyz, r6.xyz);
  r4.z = max(1.17549435e-38, r4.z);
  r4.w = rsqrt(r4.z);
  r5.w = r5.w * r5.w;
  r4.z = r4.z / r5.w;
  r4.z = 1 + -r4.z;
  r4.z = max(0, r4.z);
  r5.xyz = -cb0[197].xyz + r5.xyz;
  r5.xyz = r4.zzz * r5.xyz + cb0[197].xyz;
  r4.z = cmp(0.5 < cb0[22].x);
  if (r4.z != 0) {
    r1.w = r1.w ? cb4[166].x : cb4[165].w;
    r1.z = r1.z ? cb4[166].y : r1.w;
    r1.y = r1.y ? cb4[166].z : r1.z;
    r1.x = r1.x ? cb4[166].w : r1.y;
    r1.yzw = r3.xyz * cb4[138].xxx + v3.xyz;
    r1.yzw = -cb3[39].xyz + r1.yzw;
    r7.xyz = cb3[34].xyz * r1.zzz;
    r7.xyz = cb3[33].xyz * r1.yyy + r7.xyz;
    r1.yzw = cb3[35].xyz * r1.www + r7.xyz;
    r1.yzw = cb3[36].xyz + r1.yzw;
    r1.yz = r1.yz * cb3[38].xy + cb3[38].zw;
    r7.xy = -cb0[208].xy + r1.yz;
    r4.z = t4.SampleCmpLevelZero(s1_s, r7.xy, r1.w).x;
    r7.xyzw = cb0[208].xyxy * float4(-1,1,1,-1) + r1.yzyz;
    r5.w = t4.SampleCmpLevelZero(s1_s, r7.xy, r1.w).x;
    r4.z = r5.w + r4.z;
    r5.w = t4.SampleCmpLevelZero(s1_s, r7.zw, r1.w).x;
    r4.z = r5.w + r4.z;
    r7.xy = cb0[208].xy + r1.yz;
    r5.w = t4.SampleCmpLevelZero(s1_s, r7.xy, r1.w).x;
    r4.z = r5.w + r4.z;
    r7.xyzw = cb0[208].xyxy * float4(-1.41421294,0,1.41421294,0) + r1.yzyz;
    r5.w = t4.SampleCmpLevelZero(s1_s, r7.xy, r1.w).x;
    r4.z = r5.w + r4.z;
    r5.w = t4.SampleCmpLevelZero(s1_s, r7.zw, r1.w).x;
    r4.z = r5.w + r4.z;
    r7.xyzw = cb0[208].xyxy * float4(0,-1.41421294,0,1.41421294) + r1.yzyz;
    r5.w = t4.SampleCmpLevelZero(s1_s, r7.xy, r1.w).x;
    r4.z = r5.w + r4.z;
    r5.w = t4.SampleCmpLevelZero(s1_s, r7.zw, r1.w).x;
    r4.z = r5.w + r4.z;
    r1.y = t4.SampleCmpLevelZero(s1_s, r1.yz, r1.w).x;
    r1.y = r4.z + r1.y;
    r1.y = r1.y * 0.111100003 + -1;
    r1.x = r1.x * r1.y;
    r1.x = cb3[37].x * r1.x;
    r1.yzw = -cb2[20].xyz + v3.xyz;
    r7.xyz = -cb2[21].xyz + v3.xyz;
    r8.xyz = -cb2[22].xyz + v3.xyz;
    r9.xyz = -cb2[23].xyz + v3.xyz;
    r10.x = dot(r1.yzw, r1.yzw);
    r10.y = dot(r7.xyz, r7.xyz);
    r10.z = dot(r8.xyz, r8.xyz);
    r10.w = dot(r9.xyz, r9.xyz);
    r7.xyzw = cmp(r10.xyzw < cb2[24].xyzw);
    r8.xyzw = r7.xyzw ? float4(1,1,1,1) : 0;
    r1.yzw = r7.xyz ? float3(-1,-1,-1) : float3(-0,-0,-0);
    r1.yzw = r8.yzw + r1.yzw;
    r8.yzw = max(float3(0,0,0), r1.yzw);
    r1.y = dot(r8.xyzw, float4(4,3,2,1));
    r7.z = 4 + -r1.y;
    r1.y = (uint)r7.z;
    r1.y = (uint)r1.y << 2;
    r8.xyz = cb2[r1.y+1].xyz * v3.yyy;
    r8.xyz = cb2[r1.y+0].xyz * v3.xxx + r8.xyz;
    r8.xyz = cb2[r1.y+2].xyz * v3.zzz + r8.xyz;
    r1.yzw = cb2[r1.y+3].xyz + r8.xyz;
    r8.xy = float2(1024,1024) * r1.yz;
    r8.xy = frac(r8.xy);
    r4.z = dot(r8.xy, float2(12.9898005,78.2330017));
    r4.z = sin(r4.z);
    r4.z = 43758.5469 * r4.z;
    r4.z = frac(r4.z);
    sincos(r4.z, r8.x, r9.x);
    r10.xyzw = float4(1.29999995,1.29999995,1.29999995,1.29999995) * cb2[26].xxyy;
    r8.xz = r8.xx;
    r8.yw = r9.xx;
    r8.xyzw = r8.xyzw * r10.xyzw;
    r9.xyzw = float4(-0.172399998,-0.978299975,-0.978299975,0.172399998) * r8.xyzw;
    r9.xy = r9.xz + r9.yw;
    r7.xy = r9.xy + r1.yz;
    r4.z = t0.SampleCmpLevelZero(s1_s, r7.xyz, r1.w).x;
    r9.xyzw = float4(0.87470001,0.484600008,0.484600008,-0.87470001) * r8.xyzw;
    r9.xy = r9.xz + r9.yw;
    r7.xy = r9.xy + r1.yz;
    r5.w = t0.SampleCmpLevelZero(s1_s, r7.xyz, r1.w).x;
    r4.z = r5.w + r4.z;
    r9.xyzw = float4(-0.968299985,-0.0373999998,-0.0373999998,0.968299985) * r8.xyzw;
    r9.xy = r9.xz + r9.yw;
    r7.xy = r9.xy + r1.yz;
    r5.w = t0.SampleCmpLevelZero(s1_s, r7.xyz, r1.w).x;
    r4.z = r5.w + r4.z;
    r9.xyzw = float4(0.278299987,0.41960001,0.41960001,-0.278299987) * r8.xyzw;
    r9.xy = r9.xz + r9.yw;
    r7.xy = r9.xy + r1.yz;
    r5.w = t0.SampleCmpLevelZero(s1_s, r7.xyz, r1.w).x;
    r4.z = r5.w + r4.z;
    r9.xyzw = float4(-0.150700003,0.839100003,0.839100003,-0.150700003) * r8.xyzw;
    r9.xy = r9.xz + r9.yw;
    r7.xy = r9.xy + r1.yz;
    r5.w = t0.SampleCmpLevelZero(s1_s, r7.xyz, r1.w).x;
    r4.z = r5.w + r4.z;
    r9.xyzw = float4(-0.641700029,0.479299992,0.479299992,-0.641700029) * r8.xyzw;
    r9.xy = r9.xz + r9.yw;
    r7.xy = r9.xy + r1.yz;
    r5.w = t0.SampleCmpLevelZero(s1_s, r7.xyz, r1.w).x;
    r4.z = r5.w + r4.z;
    r9.xyzw = float4(0.577899992,-0.816100001,-0.816100001,0.577899992) * r8.xyzw;
    r9.xy = r9.xz + r9.yw;
    r7.xy = r9.xy + r1.yz;
    r5.w = t0.SampleCmpLevelZero(s1_s, r7.xyz, r1.w).x;
    r4.z = r5.w + r4.z;
    r9.xyzw = float4(-0.540899992,-0.458799988,-0.458799988,0.540899992) * r8.xyzw;
    r9.xy = r9.xz + r9.yw;
    r7.xy = r9.xy + r1.yz;
    r5.w = t0.SampleCmpLevelZero(s1_s, r7.xyz, r1.w).x;
    r4.z = r5.w + r4.z;
    r9.xyzw = float4(0.704400003,-0.1919,-0.1919,0.704400003) * r8.xyzw;
    r9.xy = r9.xz + r9.yw;
    r7.xy = r9.xy + r1.yz;
    r5.w = t0.SampleCmpLevelZero(s1_s, r7.xyz, r1.w).x;
    r4.z = r5.w + r4.z;
    r9.xyzw = float4(0.105300002,-0.446399987,-0.446399987,0.105300002) * r8.xyzw;
    r9.xy = r9.xz + r9.yw;
    r7.xy = r9.xy + r1.yz;
    r5.w = t0.SampleCmpLevelZero(s1_s, r7.xyz, r1.w).x;
    r4.z = r5.w + r4.z;
    r8.xyzw = float4(-0.206599995,0.0661000013,0.0661000013,-0.206599995) * r8.xyzw;
    r8.xy = r8.xz + r8.yw;
    r7.xy = r8.xy + r1.yz;
    r1.y = t0.SampleCmpLevelZero(s1_s, r7.xyz, r1.w).x;
    r1.y = r4.z + r1.y;
    r1.y = cb2[25].x * r1.y;
    r1.z = 1 + -cb2[25].x;
    r1.y = r1.y * 0.0908999965 + r1.z;
    r1.z = cmp(0 >= r1.w);
    r1.w = cmp(r1.w >= 1);
    r1.z = (int)r1.w | (int)r1.z;
    r1.y = r1.z ? 1 : r1.y;
    r1.z = 1 + -r4.x;
    r1.z = cb3[40].y * r1.z + r4.x;
    r1.y = r1.y * r1.z;
    r1.z = saturate(r4.y * 2 + -1);
    r1.w = cb0[197].w * r1.z;
    r4.x = cmp(0.5 < cb3[37].x);
    r1.x = r1.z * r1.x + 1;
    r1.x = min(1, r1.x);
    r1.x = r4.x ? r1.x : 1;
    r1.x = r1.x * r1.y;
    r1.y = 1 + -cb4[136].z;
    r4.x = r1.x * cb4[136].z + r1.y;
    r1.y = cb4[136].z * r1.w;
    r1.z = -r1.w * cb4[136].z + 1;
    r4.y = r1.x * r1.y + r1.z;
  } else {
    r4.xy = float2(1,1);
  }
  r1.x = saturate(2.5 * cb3[1].w);
  r1.y = 1 + -r1.x;
  r1.x = r1.y * r4.y + r1.x;
  r1.yzw = float3(-0.800000012,-0.800000012,-0.800000012) + r5.xyz;
  r1.yzw = cb4[58].www * r1.yzw + float3(0.800000012,0.800000012,0.800000012);
  r4.yzw = r6.xyz * r4.www + -r3.xyz;
  r4.yzw = cb4[58].www * r4.yzw + r3.xyz;
  r1.x = -1 + r1.x;
  r1.x = cb4[58].w * r1.x + 1;
  r0.xyz = r0.www * -r0.xyz + r0.xyz;
  r0.xyz = float3(-1,-1,-1) + r0.xyz;
  r0.xyw = saturate(r2.www * r0.yzx + float3(1,1,1));
  r5.x = cmp(r0.x >= r0.y);
  r5.x = r5.x ? 1.000000 : 0;
  r6.xy = r0.yx;
  r6.zw = float2(-1,0.666666687);
  r7.xy = -r6.xy + r0.xy;
  r7.zw = float2(1,-1);
  r5.xyzw = r5.xxxx * r7.xyzw + r6.xyzw;
  r6.x = cmp(r0.w >= r5.x);
  r6.x = r6.x ? 1.000000 : 0;
  r0.xyz = r5.xyw;
  r5.xyw = r0.wyx;
  r5.xyzw = r5.xyzw + -r0.xyzw;
  r0.xyzw = r6.xxxx * r5.xyzw + r0.xyzw;
  r5.x = min(r0.w, r0.y);
  r5.x = -r5.x + r0.x;
  r0.y = r0.w + -r0.y;
  r0.w = r5.x * 6 + 9.99999975e-05;
  r0.y = r0.y / r0.w;
  r0.y = r0.z + r0.y;
  r0.z = 9.99999975e-05 + r0.x;
  r0.z = r5.x / r0.z;
  r0.w = r0.x + -r0.z;
  r0.w = 1 + r0.w;
  r0.w = r0.w * -0.200000003 + 0.800000012;
  r3.x = dot(r4.yzw, r3.xyz);
  r3.x = -0.280000001 + r3.x;
  r3.x = saturate(10.000001 * r3.x);
  r1.x = r3.x * r1.x;
  r1.x = r1.x * r4.x;
  r1.x = r1.x * 0.300000012 + 0.699999988;
  r0.w = r1.x * r0.w;
  r0.x = r0.w * r0.x;
  r3.xyz = float3(1,0.666666687,0.333333343) + abs(r0.yyy);
  r3.xyz = frac(r3.xyz);
  r3.xyz = r3.xyz * float3(6,6,6) + float3(-3,-3,-3);
  r3.xyz = saturate(float3(-1,-1,-1) + abs(r3.xyz));
  r3.xyz = float3(-1,-1,-1) + r3.xyz;
  r0.yzw = r0.zzz * r3.xyz + float3(1,1,1);
  r0.xyz = r0.xxx * r0.yzw;
  r2.xyz = r2.xyz * r0.xyz;
  r0.xyzw = cb4[58].xyzw * r2.xyzw;
  r2.xyz = cb0[18].xyz * r0.xyz;
  o0.w = r0.w;
  r0.w = max(r1.y, r1.z);
  r0.w = max(r0.w, r1.w);
  r1.x = cmp(1 < r0.w);
  r3.xyz = r1.yzw / r0.www;
  r1.xyz = r1.xxx ? r3.xyz : r1.yzw;
  r1.xyz = v8.xyz + r1.xyz;
  if (r3.w != 0) {
    r0.w = (int)cb3[2].z;
    r1.w = asint(cb0[196].x) + -1;
    r0.w = min((int)r1.w, (int)r0.w);
    r0.w = max(0, (int)r0.w);
    r3.x = t1[r0.w].val[112/4];
    r3.y = t1[r0.w].val[112/4+1];
    r3.z = t1[r0.w].val[112/4+2];
  } else {
    r3.xyz = float3(0,0,0);
  }
  r0.w = cmp(0.5 < cb0[23].y);
  r3.xyz = r0.www ? float3(0.0500000007,0.0500000007,0.0500000007) : r3.xyz;
  r0.xyz = r3.xyz * r0.xyz;
  r0.xyz = r2.xyz * r1.xyz + r0.xyz;
  r0.w = dot(r0.xyz, float3(0.298999995,0.587000012,0.114));
  r0.xyz = r0.xyz + -r0.www;
  r0.xyz = cb4[41].xxx * r0.xyz + r0.www;
  r0.xyz = float3(-0.5,-0.5,-0.5) + r0.xyz;
  r0.xyz = cb4[41].yyy * r0.xyz + float3(0.5,0.5,0.5);
  r0.xyz = max(float3(0,0,0), r0.xyz);
  r1.xyz = saturate(-cb0[20].zzz + r0.xyz);
  r0.w = dot(r1.xyz, float3(0.212672904,0.715152204,0.0721750036));
  r0.w = cmp(0 < r0.w);
  o1.w = r0.w ? 1.000000 : 0;
  o0.xyz = r0.xyz;
  o1.xyz = float3(0,0,0);
  o2.w = 0;
  o3.xyzw = float4(0.5,0.5,0.5,1);
  return;
}

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Generated by Microsoft (R) D3D Shader Disassembler
//
//   using 3Dmigoto v1.3.16 on Fri Jul 10 23:16:16 2026
//
//
// Input signature:
//
// Name                 Index   Mask Register SysValue  Format   Used
// -------------------- ----- ------ -------- -------- ------- ------
// SV_POSITION              0   xyzw        0      POS   float
// TEXCOORD                 0   xyzw        1     NONE   float   xyzw
// TEXCOORD                 1   xyzw        2     NONE   float   xyzw
// TEXCOORD                 5   xyz         3     NONE   float   xyz
// TEXCOORD                 6   xyzw        4     NONE   float
// TEXCOORD                 4   xyzw        5     NONE   float   xy
// TEXCOORD                 3   xyzw        6     NONE   float
// TEXCOORD                 7   xyzw        7     NONE   float   xyz
// TEXCOORD                 8   xyz         8     NONE   float   xyz
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
dcl_constantbuffer CB4[167], immediateIndexed
dcl_sampler s0, mode_default
dcl_sampler s1, mode_comparison
dcl_resource_texture2darray (float,float,float,float) t0
dcl_resource_structured t1, 128
dcl_resource_texture2d (float,float,float,float) t2
dcl_resource_texture2d (float,float,float,float) t3
dcl_resource_texture2d (float,float,float,float) t4
dcl_input_ps linear v1.xyzw
dcl_input_ps linear v2.xyzw
dcl_input_ps linear v3.xyz
dcl_input_ps linear v5.xy
dcl_input_ps linear v7.xyz
dcl_input_ps linear v8.xyz
dcl_output o0.xyzw
dcl_output o1.xyzw
dcl_output o2.xyzw
dcl_output o3.xyzw
dcl_temps 11
lt r0.x, l(0.500000), cb1[28].y
div r0.yz, v1.zzwz, v2.xxxx
div r1.xy, v2.yzyy, v2.wwww
add r0.yz, r0.yyzy, -r1.xxyx
mul r0.yz, r0.yyzy, l(0.000000, 0.500000, -0.500000, 0.000000)
and r0.xy, r0.yzyy, r0.xxxx
sqrt r0.zw, |r0.xxxy|
lt r1.xy, l(0.000000, 0.000000, 0.000000, 0.000000), r0.xyxx
lt r0.xy, r0.xyxx, l(0.000000, 0.000000, 0.000000, 0.000000)
iadd r0.xy, -r1.xyxx, r0.xyxx
itof r0.xy, r0.xyxx
mul r0.xy, r0.xyxx, r0.zwzz
mad o2.xy, r0.xyxx, l(0.500000, 0.500000, 0.000000, 0.000000), l(0.498039216, 0.498039216, 0.000000, 0.000000)
eq r0.x, cb3[40].x, l(1.000000)
ne r0.yz, l(0.000000, 0.000000, 0.000000, 0.000000), cb4[154].wwzw
ne r0.w, l(0.000000, 0.000000, 0.000000, 0.000000), cb4[155].x
bfi r0.xzw, l(1, 0, 1, 1), l(2, 0, 1, 3), r0.xxzw, l(0, 0, 0, 0)
iadd r0.x, r0.w, r0.x
iadd r0.x, r0.x, r0.z
bfi r0.x, l(1), l(0), r0.y, r0.x
utof r0.x, r0.x
mul o2.z, r0.x, l(0.00392156886)
lt r0.x, l(0.500000), cb4[150].y
lt r0.y, l(1.000000), v5.x
and r0.x, r0.y, r0.x
movc r0.xy, r0.xxxx, v5.xyxx, v1.xyxx
sample_b_indexable(texture2d)(float,float,float,float) r0.xyz, r0.xyxx, t2.xyzw, s0, cb0[199].x
mul r0.xyz, r0.xyzx, cb4[58].xyzx
sample_b_indexable(texture2d)(float,float,float,float) r1.xy, v1.xyxx, t3.xyzw, s0, cb0[199].x
mul r0.w, r1.y, cb4[140].z
lt r1.xyzw, r1.xxxx, l(0.200000, 0.400000, 0.600000, 0.800000)
movc r2.xyzw, r1.wwww, cb4[71].xyzw, cb4[70].xyzw
movc r2.xyzw, r1.zzzz, cb4[72].xyzw, r2.xyzw
movc r2.xyzw, r1.yyyy, cb4[73].xyzw, r2.xyzw
movc r2.xyzw, r1.xxxx, cb4[74].xyzw, r2.xyzw
dp3 r3.x, v7.xyzx, v7.xyzx
rsq r3.x, r3.x
mul r3.xyz, r3.xxxx, v7.xyzx
mul r4.xyz, r3.yyyy, cb0[118].xyzx
mad r3.xyw, cb0[117].xyxz, r3.xxxx, r4.xyxz
mad r3.xyz, cb0[119].xyzx, r3.zzzz, r3.xywx
mul r3.xyz, r3.xyzx, l(1.000000, 1.000000, -1.000000, 0.000000)
mul r4.xyz, r3.yyyy, cb0[122].xyzx
mad r3.xyw, cb0[121].xyxz, r3.xxxx, r4.xyxz
mad r3.xyz, cb0[123].xyzx, r3.zzzz, r3.xywx
dp3 r3.w, r3.xyzx, r3.xyzx
rsq r3.w, r3.w
mul r3.xyz, r3.wwww, r3.xyzx
ilt r3.w, l(0), cb0[196].x
if_nz r3.w
  ftoi r4.x, cb3[2].z
  iadd r4.y, cb0[196].x, l(-1)
  imin r4.x, r4.y, r4.x
  imax r4.x, r4.x, l(0)
  ld_structured_indexable(structured_buffer, stride=128)(mixed,mixed,mixed,mixed) r5.xyzw, r4.x, l(0), t1.xyzw
  ld_structured_indexable(structured_buffer, stride=128)(mixed,mixed,mixed,mixed) r6.xyzw, r4.x, l(16), t1.xyzw
  ld_structured_indexable(structured_buffer, stride=128)(mixed,mixed,mixed,mixed) r4.y, r4.x, l(32), t1.xxxx
  mov r4.x, r6.w
else
  mov r4.xy, l(0,0,0,0)
  mov r5.xyzw, l(0,0,0,0)
  mov r6.xyz, l(0,0,0,0)
endif
add r6.xyz, r6.xyzx, -v3.xyzx
dp3 r4.z, r6.xyzx, r6.xyzx
max r4.z, r4.z, l(1.175494351E-38)
rsq r4.w, r4.z
mul r5.w, r5.w, r5.w
div r4.z, r4.z, r5.w
add r4.z, -r4.z, l(1.000000)
max r4.z, r4.z, l(0.000000)
add r5.xyz, r5.xyzx, -cb0[197].xyzx
mad r5.xyz, r4.zzzz, r5.xyzx, cb0[197].xyzx
lt r4.z, l(0.500000), cb0[22].x
if_nz r4.z
  movc r1.w, r1.w, cb4[166].x, cb4[165].w
  movc r1.z, r1.z, cb4[166].y, r1.w
  movc r1.y, r1.y, cb4[166].z, r1.z
  movc r1.x, r1.x, cb4[166].w, r1.y
  mad r1.yzw, r3.xxyz, cb4[138].xxxx, v3.xxyz
  add r1.yzw, r1.yyzw, -cb3[39].xxyz
  mul r7.xyz, r1.zzzz, cb3[34].xyzx
  mad r7.xyz, cb3[33].xyzx, r1.yyyy, r7.xyzx
  mad r1.yzw, cb3[35].xxyz, r1.wwww, r7.xxyz
  add r1.yzw, r1.yyzw, cb3[36].xxyz
  mad r1.yz, r1.yyzy, cb3[38].xxyx, cb3[38].zzwz
  add r7.xy, r1.yzyy, -cb0[208].xyxx
  sample_c_lz_indexable(texture2d)(float,float,float,float) r4.z, r7.xyxx, t4.xxxx, s1, r1.w
  mad r7.xyzw, cb0[208].xyxy, l(-1.000000, 1.000000, 1.000000, -1.000000), r1.yzyz
  sample_c_lz_indexable(texture2d)(float,float,float,float) r5.w, r7.xyxx, t4.xxxx, s1, r1.w
  add r4.z, r4.z, r5.w
  sample_c_lz_indexable(texture2d)(float,float,float,float) r5.w, r7.zwzz, t4.xxxx, s1, r1.w
  add r4.z, r4.z, r5.w
  add r7.xy, r1.yzyy, cb0[208].xyxx
  sample_c_lz_indexable(texture2d)(float,float,float,float) r5.w, r7.xyxx, t4.xxxx, s1, r1.w
  add r4.z, r4.z, r5.w
  mad r7.xyzw, cb0[208].xyxy, l(-1.414213, 0.000000, 1.414213, 0.000000), r1.yzyz
  sample_c_lz_indexable(texture2d)(float,float,float,float) r5.w, r7.xyxx, t4.xxxx, s1, r1.w
  add r4.z, r4.z, r5.w
  sample_c_lz_indexable(texture2d)(float,float,float,float) r5.w, r7.zwzz, t4.xxxx, s1, r1.w
  add r4.z, r4.z, r5.w
  mad r7.xyzw, cb0[208].xyxy, l(0.000000, -1.414213, 0.000000, 1.414213), r1.yzyz
  sample_c_lz_indexable(texture2d)(float,float,float,float) r5.w, r7.xyxx, t4.xxxx, s1, r1.w
  add r4.z, r4.z, r5.w
  sample_c_lz_indexable(texture2d)(float,float,float,float) r5.w, r7.zwzz, t4.xxxx, s1, r1.w
  add r4.z, r4.z, r5.w
  sample_c_lz_indexable(texture2d)(float,float,float,float) r1.y, r1.yzyy, t4.xxxx, s1, r1.w
  add r1.y, r1.y, r4.z
  mad r1.y, r1.y, l(0.111100), l(-1.000000)
  mul r1.x, r1.y, r1.x
  mul r1.x, r1.x, cb3[37].x
  add r1.yzw, v3.xxyz, -cb2[20].xxyz
  add r7.xyz, v3.xyzx, -cb2[21].xyzx
  add r8.xyz, v3.xyzx, -cb2[22].xyzx
  add r9.xyz, v3.xyzx, -cb2[23].xyzx
  dp3 r10.x, r1.yzwy, r1.yzwy
  dp3 r10.y, r7.xyzx, r7.xyzx
  dp3 r10.z, r8.xyzx, r8.xyzx
  dp3 r10.w, r9.xyzx, r9.xyzx
  lt r7.xyzw, r10.xyzw, cb2[24].xyzw
  and r8.xyzw, r7.xyzw, l(0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000)
  movc r1.yzw, r7.xxyz, l(0,-1.000000,-1.000000,-1.000000), l(0,-0.000000,-0.000000,-0.000000)
  add r1.yzw, r1.yyzw, r8.yyzw
  max r8.yzw, r1.yyzw, l(0.000000, 0.000000, 0.000000, 0.000000)
  dp4 r1.y, r8.xyzw, l(4.000000, 3.000000, 2.000000, 1.000000)
  add r7.z, -r1.y, l(4.000000)
  ftou r1.y, r7.z
  ishl r1.y, r1.y, l(2)
  mul r8.xyz, v3.yyyy, cb2[r1.y + 1].xyzx
  mad r8.xyz, cb2[r1.y + 0].xyzx, v3.xxxx, r8.xyzx
  mad r8.xyz, cb2[r1.y + 2].xyzx, v3.zzzz, r8.xyzx
  add r1.yzw, r8.xxyz, cb2[r1.y + 3].xxyz
  mul r8.xy, r1.yzyy, l(1024.000000, 1024.000000, 0.000000, 0.000000)
  frc r8.xy, r8.xyxx
  dp2 r4.z, r8.xyxx, l(12.989800, 78.233002, 0.000000, 0.000000)
  sincos r4.z, null, r4.z
  mul r4.z, r4.z, l(43758.546875)
  frc r4.z, r4.z
  sincos r8.x, r9.x, r4.z
  mul r10.xyzw, cb2[26].xxyy, l(1.300000, 1.300000, 1.300000, 1.300000)
  mov r8.xz, r8.xxxx
  mov r8.yw, r9.xxxx
  mul r8.xyzw, r10.xyzw, r8.xyzw
  mul r9.xyzw, r8.xyzw, l(-0.172400, -0.978300, -0.978300, 0.172400)
  add r9.xy, r9.ywyy, r9.xzxx
  add r7.xy, r1.yzyy, r9.xyxx
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r4.z, r7.xyzx, t0.xxxx, s1, r1.w
  mul r9.xyzw, r8.xyzw, l(0.874700, 0.484600, 0.484600, -0.874700)
  add r9.xy, r9.ywyy, r9.xzxx
  add r7.xy, r1.yzyy, r9.xyxx
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r5.w, r7.xyzx, t0.xxxx, s1, r1.w
  add r4.z, r4.z, r5.w
  mul r9.xyzw, r8.xyzw, l(-0.968300, -0.037400, -0.037400, 0.968300)
  add r9.xy, r9.ywyy, r9.xzxx
  add r7.xy, r1.yzyy, r9.xyxx
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r5.w, r7.xyzx, t0.xxxx, s1, r1.w
  add r4.z, r4.z, r5.w
  mul r9.xyzw, r8.xyzw, l(0.278300, 0.419600, 0.419600, -0.278300)
  add r9.xy, r9.ywyy, r9.xzxx
  add r7.xy, r1.yzyy, r9.xyxx
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r5.w, r7.xyzx, t0.xxxx, s1, r1.w
  add r4.z, r4.z, r5.w
  mul r9.xyzw, r8.xyzw, l(-0.150700, 0.839100, 0.839100, -0.150700)
  add r9.xy, r9.ywyy, r9.xzxx
  add r7.xy, r1.yzyy, r9.xyxx
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r5.w, r7.xyzx, t0.xxxx, s1, r1.w
  add r4.z, r4.z, r5.w
  mul r9.xyzw, r8.xyzw, l(-0.641700, 0.479300, 0.479300, -0.641700)
  add r9.xy, r9.ywyy, r9.xzxx
  add r7.xy, r1.yzyy, r9.xyxx
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r5.w, r7.xyzx, t0.xxxx, s1, r1.w
  add r4.z, r4.z, r5.w
  mul r9.xyzw, r8.xyzw, l(0.577900, -0.816100, -0.816100, 0.577900)
  add r9.xy, r9.ywyy, r9.xzxx
  add r7.xy, r1.yzyy, r9.xyxx
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r5.w, r7.xyzx, t0.xxxx, s1, r1.w
  add r4.z, r4.z, r5.w
  mul r9.xyzw, r8.xyzw, l(-0.540900, -0.458800, -0.458800, 0.540900)
  add r9.xy, r9.ywyy, r9.xzxx
  add r7.xy, r1.yzyy, r9.xyxx
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r5.w, r7.xyzx, t0.xxxx, s1, r1.w
  add r4.z, r4.z, r5.w
  mul r9.xyzw, r8.xyzw, l(0.704400, -0.191900, -0.191900, 0.704400)
  add r9.xy, r9.ywyy, r9.xzxx
  add r7.xy, r1.yzyy, r9.xyxx
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r5.w, r7.xyzx, t0.xxxx, s1, r1.w
  add r4.z, r4.z, r5.w
  mul r9.xyzw, r8.xyzw, l(0.105300, -0.446400, -0.446400, 0.105300)
  add r9.xy, r9.ywyy, r9.xzxx
  add r7.xy, r1.yzyy, r9.xyxx
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r5.w, r7.xyzx, t0.xxxx, s1, r1.w
  add r4.z, r4.z, r5.w
  mul r8.xyzw, r8.xyzw, l(-0.206600, 0.066100, 0.066100, -0.206600)
  add r8.xy, r8.ywyy, r8.xzxx
  add r7.xy, r1.yzyy, r8.xyxx
  sample_c_lz_indexable(texture2darray)(float,float,float,float) r1.y, r7.xyzx, t0.xxxx, s1, r1.w
  add r1.y, r1.y, r4.z
  mul r1.y, r1.y, cb2[25].x
  add r1.z, -cb2[25].x, l(1.000000)
  mad r1.y, r1.y, l(0.090900), r1.z
  ge r1.z, l(0.000000), r1.w
  ge r1.w, r1.w, l(1.000000)
  or r1.z, r1.w, r1.z
  movc r1.y, r1.z, l(1.000000), r1.y
  add r1.z, -r4.x, l(1.000000)
  mad r1.z, cb3[40].y, r1.z, r4.x
  mul r1.y, r1.z, r1.y
  mad_sat r1.z, r4.y, l(2.000000), l(-1.000000)
  mul r1.w, r1.z, cb0[197].w
  lt r4.x, l(0.500000), cb3[37].x
  mad r1.x, r1.z, r1.x, l(1.000000)
  min r1.x, r1.x, l(1.000000)
  movc r1.x, r4.x, r1.x, l(1.000000)
  mul r1.x, r1.y, r1.x
  add r1.y, -cb4[136].z, l(1.000000)
  mad r4.x, r1.x, cb4[136].z, r1.y
  mul r1.y, r1.w, cb4[136].z
  mad r1.z, -r1.w, cb4[136].z, l(1.000000)
  mad r4.y, r1.x, r1.y, r1.z
else
  mov r4.xy, l(1.000000,1.000000,0,0)
endif
mul_sat r1.x, cb3[1].w, l(2.500000)
add r1.y, -r1.x, l(1.000000)
mad r1.x, r1.y, r4.y, r1.x
add r1.yzw, r5.xxyz, l(0.000000, -0.800000, -0.800000, -0.800000)
mad r1.yzw, cb4[58].wwww, r1.yyzw, l(0.000000, 0.800000, 0.800000, 0.800000)
mad r4.yzw, r6.xxyz, r4.wwww, -r3.xxyz
mad r4.yzw, cb4[58].wwww, r4.yyzw, r3.xxyz
add r1.x, r1.x, l(-1.000000)
mad r1.x, cb4[58].w, r1.x, l(1.000000)
mad r0.xyz, r0.wwww, -r0.xyzx, r0.xyzx
add r0.xyz, r0.xyzx, l(-1.000000, -1.000000, -1.000000, 0.000000)
mad_sat r0.xyw, r2.wwww, r0.yzyx, l(1.000000, 1.000000, 0.000000, 1.000000)
ge r5.x, r0.x, r0.y
and r5.x, r5.x, l(0x3f800000)
mov r6.xy, r0.yxyy
mov r6.zw, l(0,0,-1.000000,0.666666687)
add r7.xy, r0.xyxx, -r6.xyxx
mov r7.zw, l(0,0,1.000000,-1.000000)
mad r5.xyzw, r5.xxxx, r7.xyzw, r6.xyzw
ge r6.x, r0.w, r5.x
and r6.x, r6.x, l(0x3f800000)
mov r0.xyz, r5.xywx
mov r5.xyw, r0.wywx
add r5.xyzw, -r0.xyzw, r5.xyzw
mad r0.xyzw, r6.xxxx, r5.xyzw, r0.xyzw
min r5.x, r0.y, r0.w
add r5.x, r0.x, -r5.x
add r0.y, -r0.y, r0.w
mad r0.w, r5.x, l(6.000000), l(0.000100)
div r0.y, r0.y, r0.w
add r0.y, r0.y, r0.z
add r0.z, r0.x, l(0.000100)
div r0.z, r5.x, r0.z
add r0.w, -r0.z, r0.x
add r0.w, r0.w, l(1.000000)
mad r0.w, r0.w, l(-0.200000), l(0.800000)
dp3 r3.x, r4.yzwy, r3.xyzx
add r3.x, r3.x, l(-0.280000)
mul_sat r3.x, r3.x, l(10.000001)
mul r1.x, r1.x, r3.x
mul r1.x, r4.x, r1.x
mad r1.x, r1.x, l(0.300000), l(0.700000)
mul r0.w, r0.w, r1.x
mul r0.x, r0.x, r0.w
add r3.xyz, |r0.yyyy|, l(1.000000, 0.666666687, 0.333333343, 0.000000)
frc r3.xyz, r3.xyzx
mad r3.xyz, r3.xyzx, l(6.000000, 6.000000, 6.000000, 0.000000), l(-3.000000, -3.000000, -3.000000, 0.000000)
add_sat r3.xyz, |r3.xyzx|, l(-1.000000, -1.000000, -1.000000, 0.000000)
add r3.xyz, r3.xyzx, l(-1.000000, -1.000000, -1.000000, 0.000000)
mad r0.yzw, r0.zzzz, r3.xxyz, l(0.000000, 1.000000, 1.000000, 1.000000)
mul r0.xyz, r0.yzwy, r0.xxxx
mul r2.xyz, r0.xyzx, r2.xyzx
mul r0.xyzw, r2.xyzw, cb4[58].xyzw
mul r2.xyz, r0.xyzx, cb0[18].xyzx
mov o0.w, r0.w
max r0.w, r1.z, r1.y
max r0.w, r1.w, r0.w
lt r1.x, l(1.000000), r0.w
div r3.xyz, r1.yzwy, r0.wwww
movc r1.xyz, r1.xxxx, r3.xyzx, r1.yzwy
add r1.xyz, r1.xyzx, v8.xyzx
if_nz r3.w
  ftoi r0.w, cb3[2].z
  iadd r1.w, cb0[196].x, l(-1)
  imin r0.w, r0.w, r1.w
  imax r0.w, r0.w, l(0)
  ld_structured_indexable(structured_buffer, stride=128)(mixed,mixed,mixed,mixed) r3.xyz, r0.w, l(112), t1.xyzx
else
  mov r3.xyz, l(0,0,0,0)
endif
lt r0.w, l(0.500000), cb0[23].y
movc r3.xyz, r0.wwww, l(0.050000,0.050000,0.050000,0), r3.xyzx
mul r0.xyz, r0.xyzx, r3.xyzx
mad r0.xyz, r2.xyzx, r1.xyzx, r0.xyzx
dp3 r0.w, r0.xyzx, l(0.299000, 0.587000, 0.114000, 0.000000)
add r0.xyz, -r0.wwww, r0.xyzx
mad r0.xyz, cb4[41].xxxx, r0.xyzx, r0.wwww
add r0.xyz, r0.xyzx, l(-0.500000, -0.500000, -0.500000, 0.000000)
mad r0.xyz, cb4[41].yyyy, r0.xyzx, l(0.500000, 0.500000, 0.500000, 0.000000)
max r0.xyz, r0.xyzx, l(0.000000, 0.000000, 0.000000, 0.000000)
add_sat r1.xyz, r0.xyzx, -cb0[20].zzzz
dp3 r0.w, r1.xyzx, l(0.212672904, 0.715152204, 0.072175, 0.000000)
lt r0.w, l(0.000000), r0.w
and o1.w, r0.w, l(0x3f800000)
mov o0.xyz, r0.xyzx
mov o1.xyz, l(0,0,0,0)
mov o2.w, l(0)
mov o3.xyzw, l(0.500000,0.500000,0.500000,1.000000)
ret
// Approximately 0 instruction slots used

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
