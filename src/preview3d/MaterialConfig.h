#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Configurable material for the **one** RayV uber preview shader.
// Game dumps are reference only — never ship 1:1 hashes/names from research HLSL.
//
// Diffuse RGB is always BaseColor.
// Every other graphic parameter is either a constant or a channel remap
// (map set + R/G/B/A + scale/bias/invert). Presets are saveable parameter packs
// (per-game families: ZZZ / GI / …).

namespace preview3d {

enum class GameFamily : uint8_t {
    Generic = 0,
    ZZZ,
    GI
};

// Which texture set to sample (bound as t0..t3)
enum class MapSet : uint8_t {
    Diffuse = 0,     // always RGB basecolor; channels still usable for opacity etc.
    Normal = 1,
    LightMap = 2,
    MaterialMap = 3,
    Constant = 255   // use constantValue, ignore map
};

enum class ChanSwizzle : uint8_t {
    R = 0, G = 1, B = 2, A = 3,
    RGB_Luma = 4,    // approx 0.299R+0.587G+0.114B
    One = 5,
    Zero = 6
};

// One graphic parameter ← texture channel or constant
struct ChannelSource {
    MapSet map = MapSet::Constant;
    ChanSwizzle swizzle = ChanSwizzle::One;
    float scale = 1.f;
    float bias = 0.f;
    bool invert = false;
    float constantValue = 0.f; // when map == Constant
};

// Runtime material instance (per part, or from preset)
struct MaterialConfig {
    std::string id;           // "ZZZ-Skin"
    std::string displayName;
    GameFamily family = GameFamily::ZZZ;

    // --- Channel graph (what feeds the uber shader) ---
    // Diffuse RGB is fixed BaseColor (no remap for RGB).
    ChannelSource opacity;        // alpha clip / blend strength
    ChannelSource shadowMask;     // toon shadow / lightmap ramp
    ChannelSource specular;       // spec intensity
    ChannelSource metallic;
    ChannelSource roughness;      // if glossiness workflow → invert in preset
    ChannelSource ao;
    ChannelSource anisotropy;
    ChannelSource sssMask;
    ChannelSource glow;
    ChannelSource rimMask;        // optional rim multipiler from map

    // Normal options
    bool useNormalMap = true;
    bool normalRGOnly = false;    // GI-style: reconstruct Z from RG
    float normalStrength = 1.f;

    // Lighting / style knobs (same shader, different looks)
    float toonThreshold = 0.45f;
    float toonSoftness = 0.08f;
    float toonShadowTint = 0.55f; // multiply albedo in shadow
    float rimStrength = 0.25f;
    float rimPower = 3.0f;
    float anisoStrength = 0.f;    // 0 = off (skin), hair uses >0
    float anisoSharpness = 64.f;
    float sssStrength = 0.f;      // soft wrap for skin/face
    float metalF0 = 0.04f;
    float glowStrength = 1.f;
    float alphaCutoff = 0.5f;
    bool  alphaClip = false;

    // Outline (preview stub params — multi-pass later)
    bool  outlineEnable = true;
    float outlineThickness = 0.012f;
    float outlineColorMul = 0.15f;

    static MaterialConfig MakeDefaultZZZ_Skin();
    static MaterialConfig MakeDefaultZZZ_Cloth();
    static MaterialConfig MakeDefaultZZZ_Face();
    static MaterialConfig MakeDefaultZZZ_Hair();
    static MaterialConfig MakeDefaultGI_Skin();
    static MaterialConfig MakeDefaultGI_Cloth();
    static MaterialConfig MakeNeutral();
};

// Lighting controlled from 3D UI (not game CB)
struct PreviewLighting {
    float yaw = 0.6f;       // radians around Y
    float pitch = 0.45f;    // elevation
    float intensity = 0.9f;
    float ambient = 0.28f;
    float ambientTint[3] = { 0.9f, 0.92f, 1.0f };
    bool followCamera = false;

    // World-space light direction (from surface toward light is -dir in shader)
    void GetDirection(float outDir[3]) const;
};

// GPU packing for cbuffer (must match HLSL)
// Each ChannelSource → float4: x=mapIndex(0-3 or -1 const), y=swizzle(0-6),
//   z=scale, w=bias  ; invert: scale negated and bias adjusted in CPU pack
struct GpuChannelPacked {
    float mapIdx, swizzle, scale, bias;
};

struct GpuMaterialCB {
    // 12 x float4 channel packs (order fixed — see MaterialConfigToGpu)
    GpuChannelPacked opacity;
    GpuChannelPacked shadowMask;
    GpuChannelPacked specular;
    GpuChannelPacked metallic;
    GpuChannelPacked roughness;
    GpuChannelPacked ao;
    GpuChannelPacked anisotropy;
    GpuChannelPacked sssMask;
    GpuChannelPacked glow;
    GpuChannelPacked rimMask;
    // constants / style
    float toonThreshold, toonSoftness, toonShadowTint, normalStrength;
    float rimStrength, rimPower, anisoStrength, anisoSharpness;
    float sssStrength, metalF0, glowStrength, alphaCutoff;
    float flags;          // bit0 normal, bit1 normalRGOnly, bit2 alphaClip
    float debugMode;
    float _pad0, _pad1;
    float constantOpacity, constantShadow, constantSpec, constantMetal;
    // more constants if channel is Constant — stored in packed as mapIdx=-1 and const in scale field...
    // Actually for Constant we put value in bias and mapIdx=-1, swizzle=One
};

void PackChannel(const ChannelSource& src, GpuChannelPacked& out);
void MaterialConfigToGpu(const MaterialConfig& m, int debugMode, GpuMaterialCB& out);

// Preset registry (file + builtins)
class ShaderPresetLibrary {
public:
    static ShaderPresetLibrary& Get();

    void LoadBuiltins();
    // Load *.json from directory (user or install presets/)
    int LoadDirectory(const std::string& dirUtf8);

    const std::vector<MaterialConfig>& All() const { return m_Presets; }
    const MaterialConfig* Find(const std::string& id) const;
    // index for UI combo
    int IndexOf(const std::string& id) const;
    const MaterialConfig& At(int index) const;

    bool SavePreset(const MaterialConfig& cfg, const std::string& dirUtf8);
    std::string ToJson(const MaterialConfig& cfg) const;
    bool FromJson(const std::string& json, MaterialConfig& out) const;

private:
    std::vector<MaterialConfig> m_Presets;
};

const char* GameFamilyName(GameFamily f);
const char* MapSetName(MapSet m);
const char* ChanSwizzleName(ChanSwizzle c);

} // namespace preview3d
