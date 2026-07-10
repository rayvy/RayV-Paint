#include "ModTypes.h"
#include <algorithm>
#include <cctype>

namespace modio {

static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

int ModScene::PartCount() const {
    int n = 0;
    for (const auto& c : components) n += (int)c.parts.size();
    n += (int)orphanParts.size();
    return n;
}

int ModScene::DrawCount() const {
    int n = 0;
    auto count = [&](const std::vector<ModPart>& parts) {
        for (const auto& p : parts)
            n += (int)p.draws.size();
    };
    for (const auto& c : components) count(c.parts);
    count(orphanParts);
    return n;
}

int ModScene::TextureBindCount() const {
    int n = 0;
    auto count = [&](const std::vector<ModPart>& parts) {
        for (const auto& p : parts)
            n += (int)p.textures.size();
    };
    for (const auto& c : components) count(c.parts);
    count(orphanParts);
    return n;
}

const char* MaterialSlotName(MaterialSlot s) {
    switch (s) {
    case MaterialSlot::Diffuse:     return "Diffuse";
    case MaterialSlot::NormalMap:   return "NormalMap";
    case MaterialSlot::LightMap:    return "LightMap";
    case MaterialSlot::MaterialMap: return "MaterialMap";
    case MaterialSlot::Opacity:     return "Opacity";
    case MaterialSlot::Glow:        return "Glow";
    case MaterialSlot::Custom:      return "Custom";
    default:                        return "Unknown";
    }
}

MaterialSlot MaterialSlotFromApiToken(const std::string& token) {
    std::string t = ToLower(token);
    // strip path-like Resource\ZZMI\Diffuse → Diffuse
    size_t slash = t.find_last_of("\\/");
    if (slash != std::string::npos)
        t = t.substr(slash + 1);

    if (t == "diffuse" || t == "albedo" || t == "basecolor" || t == "base_color")
        return MaterialSlot::Diffuse;
    if (t == "normalmap" || t == "normal" || t == "normal_map")
        return MaterialSlot::NormalMap;
    if (t == "lightmap" || t == "light" || t == "light_map")
        return MaterialSlot::LightMap;
    if (t == "materialmap" || t == "material" || t == "material_map")
        return MaterialSlot::MaterialMap;
    if (t == "opacity" || t == "alpha" || t == "transparency")
        return MaterialSlot::Opacity;
    if (t == "glow" || t == "emission" || t == "emissive")
        return MaterialSlot::Glow;
    return MaterialSlot::Unknown;
}

MaterialSlot MaterialSlotFromResourceName(const std::string& resourceOrFile) {
    std::string t = ToLower(resourceOrFile);
    // Prefer longer tokens first
    if (t.find("materialmap") != std::string::npos || t.find("material_map") != std::string::npos)
        return MaterialSlot::MaterialMap;
    if (t.find("normalmap") != std::string::npos || t.find("normal_map") != std::string::npos)
        return MaterialSlot::NormalMap;
    if (t.find("lightmap") != std::string::npos || t.find("light_map") != std::string::npos)
        return MaterialSlot::LightMap;
    if (t.find("diffuse") != std::string::npos || t.find("albedo") != std::string::npos)
        return MaterialSlot::Diffuse;
    if (t.find("opacity") != std::string::npos)
        return MaterialSlot::Opacity;
    if (t.find("glow") != std::string::npos || t.find("emiss") != std::string::npos)
        return MaterialSlot::Glow;
    // GI often names ResourceXxxNormalMap / Diffuse at end without "Map"
    if (t.size() >= 6 && t.compare(t.size() - 6, 6, "normal") == 0)
        return MaterialSlot::NormalMap;
    return MaterialSlot::Unknown;
}

BufferKind GuessBufferKind(const std::string& sectionName, int stride, const std::string& format) {
    std::string n = ToLower(sectionName);
    std::string f = ToLower(format);

    if (n.find("position") != std::string::npos) return BufferKind::Position;
    if (n.find("blend") != std::string::npos) return BufferKind::Blend;
    if (n.find("texcoord") != std::string::npos || n.find("tex_coord") != std::string::npos)
        return BufferKind::Texcoord;
    if (n.size() >= 2 && (n.compare(n.size() - 2, 2, "ib") == 0 || n.find(".ib") != std::string::npos))
        return BufferKind::Index;
    if (f.find("r32_uint") != std::string::npos || f.find("r16_uint") != std::string::npos)
        return BufferKind::Index;

    if (stride == 40) return BufferKind::Position;
    if (stride == 32) return BufferKind::Blend;
    if (stride == 20 || stride == 24) return BufferKind::Texcoord;

    // Texture resources usually have no stride / dds filename
    if (n.find("diffuse") != std::string::npos || n.find("normal") != std::string::npos ||
        n.find("light") != std::string::npos || n.find("material") != std::string::npos)
        return BufferKind::Texture;

    return BufferKind::Unknown;
}

} // namespace modio
