#pragma once
// Core Texture Set model (Substance-like).
// Shared by Advanced + Advanced Mod Mode. Free of D3D/ImGui.

#include <cstdint>
#include <string>
#include <vector>

namespace texset {

// ---- Map kinds (file / slot categories) ----
// At most one of each kind per TextureSet.
enum class MapKind : uint8_t {
    Diffuse = 0,     // RGB BaseColor always; A optional
    LightMap,
    MaterialMap,
    NormalMap,
    ExtraMap,
    GlowMap,
    WengineFX,
    Count
};

// Logical channel roles (tools paint roles; maps pack them into RGBA).
// None* family: paintable / packable but ignored by 3D preview (unspecified cases).
enum class ChannelRole : uint8_t {
    None = 0,        // pack empty / unused — not a paint target
    None0,           // free paint lanes (no 3D semantic)
    None1,
    None2,
    None3,
    None4,
    None5,
    None6,
    None7,
    BaseColor,       // Diffuse RGB special-cased
    Opacity,
    ShadowRamp,
    Metallic,
    Roughness,
    Glossiness,
    Specular,
    AO,
    Height,
    NormalX,
    NormalY,
    NormalZ,
    Emission,
    Count
};

enum class Chan : uint8_t { R = 0, G = 1, B = 2, A = 3 };

// Which source channel of a map packs a role (or None)
struct ChannelPackEntry {
    ChannelRole role = ChannelRole::None;
    bool invert = false;
    float scale = 1.f;
    float bias = 0.f;
};

// Export encoding for a map file (Substance-like per-map settings)
enum class MapColorSpace : uint8_t {
    sRGB = 0,
    Linear = 1
};
enum class MapExportCodec : uint8_t {
    PNG = 0,           // lossless intermediate / default quick export
    BC7_UNORM_SRGB,    // color maps
    BC7_UNORM,         // linear data
    BC5_UNORM,         // normal RG
    R8G8_UNORM,        // 2-channel
    R32_FLOAT,         // height / HDR scalar
    RGBA8_UNORM,       // raw
    Count
};
const char* MapExportCodecName(MapExportCodec c);
MapExportCodec MapExportCodecFromName(const std::string& n);
const char* MapColorSpaceName(MapColorSpace c);

// One map file / GPU composite target inside a set
struct MapSlot {
    MapKind kind = MapKind::Diffuse;
    bool enabled = false;
    int width = 0;
    int height = 0;
    // User-facing name (not hard-locked to kind). Empty → MapKindName(kind).
    std::string displayName;
    std::string exportPath;      // full path or relative; empty → pattern
    std::string sourcePath;      // last imported path
    std::string nameSuffix;      // auto-import / export suffix e.g. "_LightMap"
    // Soft labels for 3D / hints only — NOT required for paint/export core
    ChannelPackEntry pack[4] = {};
    // Per-map export encoding
    MapColorSpace colorSpace = MapColorSpace::Linear;
    MapExportCodec exportCodec = MapExportCodec::PNG;
    bool exportMips = true;

    bool IsValidSize() const { return width > 0 && height > 0; }
    // Empty displayName → MapKindName(kind). Defined in .cpp
    const char* DisplayName() const;
};

// Layer participation: which maps + roles this layer writes
struct LayerWorkSpace {
    // bit i set => MapKind(i) participates
    uint32_t mapMask = 1u << (uint32_t)MapKind::Diffuse;
    // bit i set => ChannelRole(i); 0 = all roles implied by maps (full Diffuse RGB paint)
    uint32_t roleMask = 0;
    // Physical write mask on the active map packing (bit0=R..bit3=A). 0xF = all.
    uint8_t channelWriteMask = 0xF;

    bool AffectsMap(MapKind k) const {
        return (mapMask & (1u << (uint32_t)k)) != 0;
    }
    void SetMap(MapKind k, bool on) {
        uint32_t b = 1u << (uint32_t)k;
        if (on) mapMask |= b; else mapMask &= ~b;
    }
    bool AffectsRole(ChannelRole r) const {
        if (roleMask == 0) return true;
        if (r == ChannelRole::None) return false;
        return (roleMask & (1u << (uint32_t)r)) != 0;
    }
    void SetRole(ChannelRole r, bool on) {
        if (r == ChannelRole::None) return;
        uint32_t b = 1u << (uint32_t)r;
        if (on) roleMask |= b; else roleMask &= ~b;
    }
    bool WritesChan(Chan c) const {
        return (channelWriteMask & (1u << (uint32_t)c)) != 0;
    }
    void SetWriteChan(Chan c, bool on) {
        uint8_t b = (uint8_t)(1u << (uint32_t)c);
        if (on) channelWriteMask |= b; else channelWriteMask = (uint8_t)(channelWriteMask & ~b);
    }
};

// Named template for enabling slots + default packing (Default / ZZZ / GI)
struct SetTemplate {
    std::string id;
    std::string displayName;
    std::vector<MapSlot> maps; // enabled flags + pack tables
};

// ---- Names / queries ----
const char* MapKindName(MapKind k);
const char* ChannelRoleName(ChannelRole r);
const char* ChannelRoleShortName(ChannelRole r); // UI: BaseC, Metal, Rough…
MapKind MapKindFromName(const std::string& n);
ChannelRole ChannelRoleFromName(const std::string& n);

// Roles actually used by enabled maps' packing (project-scoped attribute list)
std::vector<ChannelRole> CollectProjectRoles(const std::vector<MapSlot>& maps);

// None / None0..None7 — not used by 3D preview
bool IsNoneFamily(ChannelRole r);
// Roles that drive 3D (false for None family and plain None)
bool Is3DSemanticRole(ChannelRole r);
// Single-channel style paint (grayscale)
bool IsSingleChannelRole(ChannelRole r);

// All roles for UI combo (includes None, None0.., BaseColor, …)
int ChannelRoleComboCount();
const char* const* ChannelRoleComboNames(); // static array, size = ChannelRoleComboCount()
int ChannelRoleToComboIndex(ChannelRole r);
ChannelRole ChannelRoleFromComboIndex(int i);

// Built-in templates
SetTemplate Template_Default();   // Diffuse only
SetTemplate Template_ZZZ();       // Diffuse+Light+Material+Normal with community packing
SetTemplate Template_GI();

// Apply template into map slot list (preserves sizes/paths if kinds match)
void ApplyTemplate(std::vector<MapSlot>& maps, const SetTemplate& t);

// Ensure Diffuse exists and is enabled
void EnsureDiffuseSlot(std::vector<MapSlot>& maps);

MapSlot* FindMap(std::vector<MapSlot>& maps, MapKind k);
const MapSlot* FindMap(const std::vector<MapSlot>& maps, MapKind k);

// First enabled map that packs role; Diffuse if BaseColor; Diffuse fallback
MapKind ResolveMapForRole(const std::vector<MapSlot>& maps, ChannelRole role);
int ResolveChannelForRole(const MapSlot& slot, ChannelRole role); // 0-3 or -1

// FillChannelTarget bridge helpers live near LayerTypes; role from fill name:
ChannelRole RoleFromFillName(const std::string& name);

} // namespace texset
