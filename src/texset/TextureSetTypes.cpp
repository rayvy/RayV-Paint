#include "TextureSetTypes.h"
#include <algorithm>
#include <cctype>

namespace texset {

static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

const char* MapExportCodecName(MapExportCodec c) {
    switch (c) {
    case MapExportCodec::PNG:            return "PNG";
    case MapExportCodec::BC7_UNORM_SRGB:  return "BC7_UNORM_SRGB";
    case MapExportCodec::BC7_UNORM:       return "BC7_UNORM";
    case MapExportCodec::BC5_UNORM:       return "BC5_UNORM";
    case MapExportCodec::R8G8_UNORM:      return "R8G8_UNORM";
    case MapExportCodec::R32_FLOAT:       return "R32_FLOAT";
    case MapExportCodec::RGBA8_UNORM:     return "RGBA8_UNORM";
    default: return "PNG";
    }
}
MapExportCodec MapExportCodecFromName(const std::string& n) {
    std::string s = ToLower(n);
    if (s == "bc7_unorm_srgb" || s == "bc7srgb") return MapExportCodec::BC7_UNORM_SRGB;
    if (s == "bc7_unorm" || s == "bc7") return MapExportCodec::BC7_UNORM;
    if (s == "bc5_unorm" || s == "bc5") return MapExportCodec::BC5_UNORM;
    if (s == "r8g8_unorm" || s == "r8g8") return MapExportCodec::R8G8_UNORM;
    if (s == "r32_float" || s == "r32f") return MapExportCodec::R32_FLOAT;
    if (s == "rgba8_unorm" || s == "rgba8") return MapExportCodec::RGBA8_UNORM;
    return MapExportCodec::PNG;
}
const char* MapColorSpaceName(MapColorSpace c) {
    return c == MapColorSpace::sRGB ? "sRGB" : "Linear";
}

const char* MapSlot::DisplayName() const {
    return displayName.empty() ? MapKindName(kind) : displayName.c_str();
}

const char* MapKindName(MapKind k) {
    switch (k) {
    case MapKind::Diffuse:     return "Diffuse";
    case MapKind::LightMap:    return "LightMap";
    case MapKind::MaterialMap: return "MaterialMap";
    case MapKind::NormalMap:    return "NormalMap";
    case MapKind::ExtraMap:     return "ExtraMap";
    case MapKind::GlowMap:      return "GlowMap";
    case MapKind::WengineFX:    return "WengineFX";
    default: return "?";
    }
}

const char* ChannelRoleName(ChannelRole r) {
    switch (r) {
    case ChannelRole::None:       return "None";
    case ChannelRole::None0:      return "None0";
    case ChannelRole::None1:      return "None1";
    case ChannelRole::None2:      return "None2";
    case ChannelRole::None3:      return "None3";
    case ChannelRole::None4:      return "None4";
    case ChannelRole::None5:      return "None5";
    case ChannelRole::None6:      return "None6";
    case ChannelRole::None7:      return "None7";
    case ChannelRole::BaseColor:  return "BaseColor";
    case ChannelRole::Opacity:    return "Opacity";
    case ChannelRole::ShadowRamp: return "ShadowRamp";
    case ChannelRole::Metallic:   return "Metallic";
    case ChannelRole::Roughness:  return "Roughness";
    case ChannelRole::Glossiness: return "Glossiness";
    case ChannelRole::Specular:   return "Specular";
    case ChannelRole::AO:         return "AO";
    case ChannelRole::Height:     return "Height";
    case ChannelRole::NormalX:    return "NormalX";
    case ChannelRole::NormalY:    return "NormalY";
    case ChannelRole::NormalZ:    return "NormalZ";
    case ChannelRole::Emission:   return "Emission";
    default: return "?";
    }
}

const char* ChannelRoleShortName(ChannelRole r) {
    switch (r) {
    case ChannelRole::None:       return "None";
    case ChannelRole::None0:      return "None0";
    case ChannelRole::None1:      return "None1";
    case ChannelRole::None2:      return "None2";
    case ChannelRole::None3:      return "None3";
    case ChannelRole::None4:      return "None4";
    case ChannelRole::None5:      return "None5";
    case ChannelRole::None6:      return "None6";
    case ChannelRole::None7:      return "None7";
    case ChannelRole::BaseColor:  return "BaseC";
    case ChannelRole::Opacity:    return "Trans";
    case ChannelRole::ShadowRamp: return "Shadw";
    case ChannelRole::Metallic:   return "Metal";
    case ChannelRole::Roughness:  return "Rough";
    case ChannelRole::Glossiness: return "Gloss";
    case ChannelRole::Specular:   return "Specl";
    case ChannelRole::AO:         return "AmbOc";
    case ChannelRole::Height:     return "Height";
    case ChannelRole::NormalX:    return "Normal";
    case ChannelRole::NormalY:    return "NormY";
    case ChannelRole::NormalZ:    return "NormZ";
    case ChannelRole::Emission:   return "Emiss";
    default: return "?";
    }
}

std::vector<ChannelRole> CollectProjectRoles(const std::vector<MapSlot>& maps) {
    bool seen[(int)ChannelRole::Count] = {};
    std::vector<ChannelRole> out;
    // Prefer order of appearance in packs across enabled maps
    for (const auto& m : maps) {
        if (!m.enabled) continue;
        for (int c = 0; c < 4; ++c) {
            ChannelRole r = m.pack[c].role;
            if (r == ChannelRole::None) continue;
            if ((int)r >= (int)ChannelRole::Count) continue;
            if (seen[(int)r]) continue;
            // Collapse NormalY/Z into Normal triad represented by NormalX for Fill UI
            if (r == ChannelRole::NormalY || r == ChannelRole::NormalZ) {
                if (!seen[(int)ChannelRole::NormalX]) {
                    seen[(int)ChannelRole::NormalX] = true;
                    out.push_back(ChannelRole::NormalX);
                }
                continue;
            }
            seen[(int)r] = true;
            out.push_back(r);
        }
    }
    // Always include BaseColor if Diffuse enabled
    if (const MapSlot* d = FindMap(maps, MapKind::Diffuse)) {
        if (d->enabled && !seen[(int)ChannelRole::BaseColor]) {
            out.insert(out.begin(), ChannelRole::BaseColor);
        }
    }
    return out;
}

bool IsNoneFamily(ChannelRole r) {
    return r == ChannelRole::None ||
           (r >= ChannelRole::None0 && r <= ChannelRole::None7);
}

bool Is3DSemanticRole(ChannelRole r) {
    return !IsNoneFamily(r) && r != ChannelRole::None;
}

bool IsSingleChannelRole(ChannelRole r) {
    if (r == ChannelRole::BaseColor) return false;
    if (r == ChannelRole::None) return false;
    return true;
}

// Ordered for UI combos
static const char* kRoleCombo[] = {
    "None", "None0", "None1", "None2", "None3", "None4", "None5", "None6", "None7",
    "BaseColor", "Opacity", "ShadowRamp", "Metallic", "Roughness", "Glossiness",
    "Specular", "AO", "Height", "NormalX", "NormalY", "NormalZ", "Emission"
};
static const ChannelRole kRoleComboEnum[] = {
    ChannelRole::None, ChannelRole::None0, ChannelRole::None1, ChannelRole::None2,
    ChannelRole::None3, ChannelRole::None4, ChannelRole::None5, ChannelRole::None6,
    ChannelRole::None7, ChannelRole::BaseColor, ChannelRole::Opacity, ChannelRole::ShadowRamp,
    ChannelRole::Metallic, ChannelRole::Roughness, ChannelRole::Glossiness, ChannelRole::Specular,
    ChannelRole::AO, ChannelRole::Height, ChannelRole::NormalX, ChannelRole::NormalY,
    ChannelRole::NormalZ, ChannelRole::Emission
};

int ChannelRoleComboCount() {
    return (int)(sizeof(kRoleCombo) / sizeof(kRoleCombo[0]));
}
const char* const* ChannelRoleComboNames() { return kRoleCombo; }

int ChannelRoleToComboIndex(ChannelRole r) {
    for (int i = 0; i < ChannelRoleComboCount(); ++i)
        if (kRoleComboEnum[i] == r) return i;
    return 0;
}
ChannelRole ChannelRoleFromComboIndex(int i) {
    if (i < 0 || i >= ChannelRoleComboCount()) return ChannelRole::None;
    return kRoleComboEnum[i];
}

MapKind MapKindFromName(const std::string& n) {
    std::string s = ToLower(n);
    if (s == "diffuse" || s == "albedo" || s == "basecolor") return MapKind::Diffuse;
    if (s == "lightmap" || s == "light") return MapKind::LightMap;
    if (s == "materialmap" || s == "material") return MapKind::MaterialMap;
    if (s == "normalmap" || s == "normal") return MapKind::NormalMap;
    if (s == "extramap" || s == "extra") return MapKind::ExtraMap;
    if (s == "glowmap" || s == "glow" || s == "emission") return MapKind::GlowMap;
    if (s == "wenginefx" || s == "wengine") return MapKind::WengineFX;
    return MapKind::Diffuse;
}

ChannelRole ChannelRoleFromName(const std::string& n) {
    std::string s = ToLower(n);
    if (s == "none" || s.empty()) return ChannelRole::None;
    if (s == "none0" || s == "custom0") return ChannelRole::None0;
    if (s == "none1" || s == "custom1") return ChannelRole::None1;
    if (s == "none2") return ChannelRole::None2;
    if (s == "none3") return ChannelRole::None3;
    if (s == "none4") return ChannelRole::None4;
    if (s == "none5") return ChannelRole::None5;
    if (s == "none6") return ChannelRole::None6;
    if (s == "none7") return ChannelRole::None7;
    if (s == "basecolor" || s == "albedo") return ChannelRole::BaseColor;
    if (s == "opacity" || s == "transparency" || s == "alpha") return ChannelRole::Opacity;
    if (s == "shadowramp" || s == "shadow") return ChannelRole::ShadowRamp;
    if (s == "metallic" || s == "metal") return ChannelRole::Metallic;
    if (s == "roughness" || s == "rough") return ChannelRole::Roughness;
    if (s == "glossiness" || s == "gloss") return ChannelRole::Glossiness;
    if (s == "specular" || s == "spec") return ChannelRole::Specular;
    if (s == "ao" || s == "occlusion") return ChannelRole::AO;
    if (s == "height") return ChannelRole::Height;
    if (s == "normalx") return ChannelRole::NormalX;
    if (s == "normaly") return ChannelRole::NormalY;
    if (s == "normalz") return ChannelRole::NormalZ;
    if (s == "emission" || s == "emissive") return ChannelRole::Emission;
    return ChannelRole::None;
}

ChannelRole RoleFromFillName(const std::string& name) {
    return ChannelRoleFromName(name);
}

static MapSlot MakeSlot(MapKind k, bool en) {
    MapSlot s;
    s.kind = k;
    s.enabled = en;
    return s;
}

static void Pack(MapSlot& s, Chan c, ChannelRole r, bool inv = false) {
    s.pack[(int)c].role = r;
    s.pack[(int)c].invert = inv;
}

SetTemplate Template_Default() {
    SetTemplate t;
    t.id = "Default";
    t.displayName = "Default (Diffuse only)";
    auto d = MakeSlot(MapKind::Diffuse, true);
    Pack(d, Chan::R, ChannelRole::BaseColor);
    Pack(d, Chan::G, ChannelRole::BaseColor);
    Pack(d, Chan::B, ChannelRole::BaseColor);
    Pack(d, Chan::A, ChannelRole::None);
    d.colorSpace = MapColorSpace::sRGB;
    d.exportCodec = MapExportCodec::BC7_UNORM_SRGB;
    d.nameSuffix = "_Diffuse";
    auto lm = MakeSlot(MapKind::LightMap, false);
    lm.colorSpace = MapColorSpace::Linear;
    lm.exportCodec = MapExportCodec::BC7_UNORM;
    lm.nameSuffix = "_LightMap";
    auto mat = MakeSlot(MapKind::MaterialMap, false);
    mat.colorSpace = MapColorSpace::Linear;
    mat.exportCodec = MapExportCodec::BC7_UNORM;
    mat.nameSuffix = "_Material";
    auto nrm = MakeSlot(MapKind::NormalMap, false);
    nrm.colorSpace = MapColorSpace::Linear;
    nrm.exportCodec = MapExportCodec::BC5_UNORM;
    nrm.nameSuffix = "_Normal";
    t.maps = {
        d, lm, mat, nrm,
        MakeSlot(MapKind::ExtraMap, false),
        MakeSlot(MapKind::GlowMap, false),
        MakeSlot(MapKind::WengineFX, false),
    };
    return t;
}

SetTemplate Template_ZZZ() {
    SetTemplate t;
    t.id = "ZZZ";
    t.displayName = "Zenless Zone Zero";
    auto d = MakeSlot(MapKind::Diffuse, true);
    Pack(d, Chan::R, ChannelRole::BaseColor);
    Pack(d, Chan::G, ChannelRole::BaseColor);
    Pack(d, Chan::B, ChannelRole::BaseColor);
    Pack(d, Chan::A, ChannelRole::None);
    d.colorSpace = MapColorSpace::sRGB;
    d.exportCodec = MapExportCodec::BC7_UNORM_SRGB;
    d.nameSuffix = "_Diffuse";

    auto lm = MakeSlot(MapKind::LightMap, true);
    Pack(lm, Chan::R, ChannelRole::ShadowRamp);
    Pack(lm, Chan::G, ChannelRole::Metallic);
    Pack(lm, Chan::B, ChannelRole::Glossiness);
    Pack(lm, Chan::A, ChannelRole::None);
    lm.colorSpace = MapColorSpace::Linear;
    lm.exportCodec = MapExportCodec::BC7_UNORM;
    lm.nameSuffix = "_LightMap";

    auto mat = MakeSlot(MapKind::MaterialMap, true);
    Pack(mat, Chan::R, ChannelRole::Opacity);
    Pack(mat, Chan::G, ChannelRole::None);
    Pack(mat, Chan::B, ChannelRole::Specular);
    Pack(mat, Chan::A, ChannelRole::None);
    mat.colorSpace = MapColorSpace::Linear;
    mat.exportCodec = MapExportCodec::BC7_UNORM;
    mat.nameSuffix = "_Material";

    auto nrm = MakeSlot(MapKind::NormalMap, true);
    Pack(nrm, Chan::R, ChannelRole::NormalX);
    Pack(nrm, Chan::G, ChannelRole::NormalY);
    Pack(nrm, Chan::B, ChannelRole::AO);
    Pack(nrm, Chan::A, ChannelRole::None);
    nrm.colorSpace = MapColorSpace::Linear;
    nrm.exportCodec = MapExportCodec::BC5_UNORM;
    nrm.nameSuffix = "_Normal";

    t.maps = {
        d, lm, mat, nrm,
        MakeSlot(MapKind::ExtraMap, false),
        MakeSlot(MapKind::GlowMap, false),
        MakeSlot(MapKind::WengineFX, false),
    };
    return t;
}

SetTemplate Template_GI() {
    SetTemplate t;
    t.id = "GI";
    t.displayName = "Genshin Impact";
    auto d = MakeSlot(MapKind::Diffuse, true);
    Pack(d, Chan::R, ChannelRole::BaseColor);
    Pack(d, Chan::G, ChannelRole::BaseColor);
    Pack(d, Chan::B, ChannelRole::BaseColor);
    Pack(d, Chan::A, ChannelRole::None);

    auto lm = MakeSlot(MapKind::LightMap, true);
    Pack(lm, Chan::R, ChannelRole::ShadowRamp);
    Pack(lm, Chan::G, ChannelRole::Specular);
    Pack(lm, Chan::B, ChannelRole::Metallic);
    Pack(lm, Chan::A, ChannelRole::None);

    auto nrm = MakeSlot(MapKind::NormalMap, true);
    Pack(nrm, Chan::R, ChannelRole::NormalX);
    Pack(nrm, Chan::G, ChannelRole::NormalY);
    Pack(nrm, Chan::B, ChannelRole::None);
    Pack(nrm, Chan::A, ChannelRole::None);

    t.maps = {
        d, lm,
        MakeSlot(MapKind::MaterialMap, false),
        nrm,
        MakeSlot(MapKind::ExtraMap, false),
        MakeSlot(MapKind::GlowMap, false),
        MakeSlot(MapKind::WengineFX, false),
    };
    return t;
}

void EnsureDiffuseSlot(std::vector<MapSlot>& maps) {
    if (!FindMap(maps, MapKind::Diffuse)) {
        MapSlot d = MakeSlot(MapKind::Diffuse, true);
        Pack(d, Chan::R, ChannelRole::BaseColor);
        Pack(d, Chan::G, ChannelRole::BaseColor);
        Pack(d, Chan::B, ChannelRole::BaseColor);
        maps.insert(maps.begin(), d);
    }
    if (auto* d = FindMap(maps, MapKind::Diffuse))
        d->enabled = true;
}

MapSlot* FindMap(std::vector<MapSlot>& maps, MapKind k) {
    for (auto& m : maps)
        if (m.kind == k) return &m;
    return nullptr;
}

const MapSlot* FindMap(const std::vector<MapSlot>& maps, MapKind k) {
    for (const auto& m : maps)
        if (m.kind == k) return &m;
    return nullptr;
}

void ApplyTemplate(std::vector<MapSlot>& maps, const SetTemplate& t) {
    std::vector<MapSlot> old = maps;
    maps = t.maps;
    for (auto& m : maps) {
        if (const MapSlot* prev = FindMap(old, m.kind)) {
            m.width = prev->width;
            m.height = prev->height;
            m.exportPath = prev->exportPath;
            m.sourcePath = prev->sourcePath;
            if (!prev->nameSuffix.empty()) m.nameSuffix = prev->nameSuffix;
            // Keep user codec overrides if previously customized
            m.colorSpace = prev->colorSpace;
            m.exportCodec = prev->exportCodec;
            m.exportMips = prev->exportMips;
        }
    }
    EnsureDiffuseSlot(maps);
}

int ResolveChannelForRole(const MapSlot& slot, ChannelRole role) {
    if (role == ChannelRole::None) return -1;
    for (int i = 0; i < 4; ++i)
        if (slot.pack[i].role == role) return i;
    return -1;
}

MapKind ResolveMapForRole(const std::vector<MapSlot>& maps, ChannelRole role) {
    if (role == ChannelRole::BaseColor) return MapKind::Diffuse;
    for (const auto& m : maps) {
        if (!m.enabled) continue;
        if (ResolveChannelForRole(m, role) >= 0) return m.kind;
    }
    // Fallbacks by convention
    switch (role) {
    case ChannelRole::ShadowRamp:
    case ChannelRole::Metallic:
    case ChannelRole::Roughness:
    case ChannelRole::Glossiness:
        return MapKind::LightMap;
    case ChannelRole::Opacity:
    case ChannelRole::Specular:
        return MapKind::MaterialMap;
    case ChannelRole::NormalX:
    case ChannelRole::NormalY:
    case ChannelRole::NormalZ:
    case ChannelRole::AO:
        return MapKind::NormalMap;
    case ChannelRole::Emission:
        return MapKind::GlowMap;
    case ChannelRole::Height:
        return MapKind::ExtraMap;
    default:
        return MapKind::Diffuse;
    }
}

} // namespace texset
