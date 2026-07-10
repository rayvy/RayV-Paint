#include "MaterialConfig.h"
#include "../core/Logger.h"
#include "../core/PathUtil.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace preview3d {
using json = nlohmann::json;
namespace fs = std::filesystem;

static ChannelSource Const(float v) {
    ChannelSource s;
    s.map = MapSet::Constant;
    s.swizzle = ChanSwizzle::One;
    s.constantValue = v;
    return s;
}

static ChannelSource From(MapSet m, ChanSwizzle c, float scale = 1.f, bool inv = false) {
    ChannelSource s;
    s.map = m;
    s.swizzle = c;
    s.scale = scale;
    s.invert = inv;
    return s;
}

const char* GameFamilyName(GameFamily f) {
    switch (f) {
    case GameFamily::ZZZ: return "ZZZ";
    case GameFamily::GI:  return "GI";
    default: return "Generic";
    }
}

const char* MapSetName(MapSet m) {
    switch (m) {
    case MapSet::Diffuse: return "Diffuse";
    case MapSet::Normal: return "Normal";
    case MapSet::LightMap: return "LightMap";
    case MapSet::MaterialMap: return "MaterialMap";
    case MapSet::Constant: return "Constant";
    default: return "?";
    }
}

const char* ChanSwizzleName(ChanSwizzle c) {
    switch (c) {
    case ChanSwizzle::R: return "R";
    case ChanSwizzle::G: return "G";
    case ChanSwizzle::B: return "B";
    case ChanSwizzle::A: return "A";
    case ChanSwizzle::RGB_Luma: return "Luma";
    case ChanSwizzle::One: return "1";
    case ChanSwizzle::Zero: return "0";
    default: return "?";
    }
}

void PreviewLighting::GetDirection(float outDir[3]) const {
    // yaw around Y, pitch elevation — light coming FROM this direction
    float cp = std::cos(pitch), sp = std::sin(pitch);
    float cy = std::cos(yaw), sy = std::sin(yaw);
    outDir[0] = sy * cp;
    outDir[1] = sp;
    outDir[2] = cy * cp;
    float len = std::sqrt(outDir[0]*outDir[0]+outDir[1]*outDir[1]+outDir[2]*outDir[2]);
    if (len > 1e-6f) { outDir[0]/=len; outDir[1]/=len; outDir[2]/=len; }
}

MaterialConfig MaterialConfig::MakeNeutral() {
    MaterialConfig m;
    m.id = "Generic-Neutral";
    m.displayName = "Generic Neutral";
    m.family = GameFamily::Generic;
    m.opacity = Const(1.f);
    m.shadowMask = Const(1.f);
    m.specular = Const(0.15f);
    m.metallic = Const(0.f);
    m.roughness = Const(0.6f);
    m.ao = Const(1.f);
    m.anisotropy = Const(0.f);
    m.sssMask = Const(0.f);
    m.glow = Const(0.f);
    m.rimMask = Const(1.f);
    m.useNormalMap = true;
    m.toonThreshold = 0.5f;
    m.toonSoftness = 0.12f;
    m.rimStrength = 0.15f;
    return m;
}

MaterialConfig MaterialConfig::MakeDefaultZZZ_Skin() {
    MaterialConfig m = MakeNeutral();
    m.id = "ZZZ-Skin";
    m.displayName = "ZZZ Skin / Body";
    m.family = GameFamily::ZZZ;
    // Community-common ZZZ packing (not guaranteed per-character — remappable in UI):
    // LightMap:  R=ShadowRamp/outline-ish  G=Metallic  B=Gloss
    // Material:  R=Transparency  G=GodKnows→None  B=Specular-ish
    // Normal:    R=N.x  G=N.y  B=Occlusion  (RG normal, B AO)
    // Vertex:    R=Outline thick  G=?  B=contact shadow?  A=neck?
    m.shadowMask = From(MapSet::LightMap, ChanSwizzle::R);
    m.metallic   = From(MapSet::LightMap, ChanSwizzle::G);
    m.roughness  = From(MapSet::LightMap, ChanSwizzle::B, 1.f, true); // gloss → rough via invert
    m.opacity    = From(MapSet::MaterialMap, ChanSwizzle::R);
    m.anisotropy = Const(0.f); // Material.G = God Knows → unused
    m.specular   = From(MapSet::MaterialMap, ChanSwizzle::B);
    m.ao         = From(MapSet::Normal, ChanSwizzle::B);
    m.sssMask    = Const(0.5f);
    m.glow       = Const(0.f);
    m.rimMask    = Const(1.f);
    m.normalRGOnly = true; // ZZZ normal often RG + AO in B
    m.sssStrength = 0.35f;
    m.toonThreshold = 0.38f;
    m.toonSoftness = 0.22f;
    m.toonShadowTint = 0.72f;
    m.rimStrength = 0.12f;
    m.anisoStrength = 0.f;
    m.outlineThickness = 0.0035f;
    m.outlineColorMul = 0.22f;
    m.alphaClip = false; // Transparency when enabled — user toggles
    return m;
}

MaterialConfig MaterialConfig::MakeDefaultZZZ_Cloth() {
    MaterialConfig m = MakeDefaultZZZ_Skin();
    m.id = "ZZZ-Cloth";
    m.displayName = "ZZZ Cloth / Coat";
    m.sssStrength = 0.f;
    m.sssMask = Const(0.f);
    m.toonThreshold = 0.45f;
    m.toonSoftness = 0.12f;
    m.toonShadowTint = 0.58f;
    m.rimStrength = 0.22f;
    return m;
}

MaterialConfig MaterialConfig::MakeDefaultZZZ_Face() {
    MaterialConfig m = MakeDefaultZZZ_Skin();
    m.id = "ZZZ-Face";
    m.displayName = "ZZZ Face";
    m.sssStrength = 0.55f;
    m.toonSoftness = 0.28f;
    m.toonShadowTint = 0.78f;
    m.rimStrength = 0.1f;
    return m;
}

MaterialConfig MaterialConfig::MakeDefaultZZZ_Hair() {
    MaterialConfig m = MakeDefaultZZZ_Cloth();
    m.id = "ZZZ-Hair";
    m.displayName = "ZZZ Hair";
    // Keep community maps; aniso unknown → off unless user remaps
    m.anisotropy = Const(0.f);
    m.anisoStrength = 0.f;
    m.toonSoftness = 0.1f;
    m.rimStrength = 0.28f;
    m.alphaClip = false;
    return m;
}

MaterialConfig MaterialConfig::MakeDefaultGI_Skin() {
    MaterialConfig m = MakeDefaultZZZ_Skin();
    m.id = "GI-Skin";
    m.displayName = "GI Skin";
    m.family = GameFamily::GI;
    m.normalRGOnly = true;
    // GI often packs differently — start conservative
    m.shadowMask = From(MapSet::LightMap, ChanSwizzle::R);
    m.specular = From(MapSet::LightMap, ChanSwizzle::G);
    m.metallic = Const(0.f);
    m.roughness = From(MapSet::LightMap, ChanSwizzle::B);
    m.ao = Const(1.f);
    m.sssStrength = 0.25f;
    m.toonThreshold = 0.5f;
    m.toonSoftness = 0.1f;
    return m;
}

MaterialConfig MaterialConfig::MakeDefaultGI_Cloth() {
    MaterialConfig m = MakeDefaultGI_Skin();
    m.id = "GI-Cloth";
    m.displayName = "GI Cloth";
    m.sssStrength = 0.f;
    m.metallic = From(MapSet::LightMap, ChanSwizzle::B);
    return m;
}

void PackChannel(const ChannelSource& src, GpuChannelPacked& out) {
    if (src.map == MapSet::Constant) {
        out.mapIdx = -1.f;
        out.swizzle = (float)ChanSwizzle::One;
        float v = src.constantValue;
        if (src.invert) v = 1.f - v;
        v = v * src.scale + src.bias;
        out.scale = v; // shader: if mapIdx<0 return scale as value
        out.bias = 0.f;
        return;
    }
    out.mapIdx = (float)(int)src.map;
    out.swizzle = (float)(int)src.swizzle;
    float sc = src.scale;
    float bi = src.bias;
    if (src.invert) {
        // out = 1 - (sample*scale + bias)  =>  sample*(-scale) + (1-bias)
        // simpler: sample then invert in shader if scale < 0? 
        // Use scale negative as invert flag: keep scale positive, bias stores invert in high bit via separate
        // Pack invert into swizzle + 10 if invert
        out.swizzle += src.invert ? 10.f : 0.f;
    }
    out.scale = sc;
    out.bias = bi;
}

void MaterialConfigToGpu(const MaterialConfig& m, int debugMode, GpuMaterialCB& out) {
    PackChannel(m.opacity, out.opacity);
    PackChannel(m.shadowMask, out.shadowMask);
    PackChannel(m.specular, out.specular);
    PackChannel(m.metallic, out.metallic);
    PackChannel(m.roughness, out.roughness);
    PackChannel(m.ao, out.ao);
    PackChannel(m.anisotropy, out.anisotropy);
    PackChannel(m.sssMask, out.sssMask);
    PackChannel(m.glow, out.glow);
    PackChannel(m.rimMask, out.rimMask);

    out.toonThreshold = m.toonThreshold;
    out.toonSoftness = m.toonSoftness;
    out.toonShadowTint = m.toonShadowTint;
    out.normalStrength = m.normalStrength;
    out.rimStrength = m.rimStrength;
    out.rimPower = m.rimPower;
    out.anisoStrength = m.anisoStrength;
    out.anisoSharpness = m.anisoSharpness;
    out.sssStrength = m.sssStrength;
    out.metalF0 = m.metalF0;
    out.glowStrength = m.glowStrength;
    out.alphaCutoff = m.alphaCutoff;

    float flags = 0.f;
    if (m.useNormalMap) flags += 1.f;
    if (m.normalRGOnly) flags += 2.f;
    if (m.alphaClip) flags += 4.f;
    out.flags = flags;
    out.debugMode = (float)debugMode;
    out._pad0 = out._pad1 = 0.f;
    out.constantOpacity = m.opacity.constantValue;
    out.constantShadow = m.shadowMask.constantValue;
    out.constantSpec = m.specular.constantValue;
    out.constantMetal = m.metallic.constantValue;
}

// ---- JSON ----
static json ChanToJson(const ChannelSource& s) {
    json j;
    j["map"] = MapSetName(s.map);
    j["swizzle"] = ChanSwizzleName(s.swizzle);
    j["scale"] = s.scale;
    j["bias"] = s.bias;
    j["invert"] = s.invert;
    j["constant"] = s.constantValue;
    return j;
}

static MapSet MapFromName(const std::string& n) {
    if (n == "Diffuse") return MapSet::Diffuse;
    if (n == "Normal") return MapSet::Normal;
    if (n == "LightMap") return MapSet::LightMap;
    if (n == "MaterialMap") return MapSet::MaterialMap;
    return MapSet::Constant;
}

static ChanSwizzle SwizFromName(const std::string& n) {
    if (n == "R") return ChanSwizzle::R;
    if (n == "G") return ChanSwizzle::G;
    if (n == "B") return ChanSwizzle::B;
    if (n == "A") return ChanSwizzle::A;
    if (n == "Luma") return ChanSwizzle::RGB_Luma;
    if (n == "0") return ChanSwizzle::Zero;
    return ChanSwizzle::One;
}

static void ChanFromJson(const json& j, ChannelSource& s) {
    if (!j.is_object()) return;
    s.map = MapFromName(j.value("map", "Constant"));
    s.swizzle = SwizFromName(j.value("swizzle", "1"));
    s.scale = j.value("scale", 1.f);
    s.bias = j.value("bias", 0.f);
    s.invert = j.value("invert", false);
    s.constantValue = j.value("constant", 0.f);
}

std::string ShaderPresetLibrary::ToJson(const MaterialConfig& m) const {
    json j;
    j["id"] = m.id;
    j["displayName"] = m.displayName;
    j["family"] = GameFamilyName(m.family);
    j["opacity"] = ChanToJson(m.opacity);
    j["shadowMask"] = ChanToJson(m.shadowMask);
    j["specular"] = ChanToJson(m.specular);
    j["metallic"] = ChanToJson(m.metallic);
    j["roughness"] = ChanToJson(m.roughness);
    j["ao"] = ChanToJson(m.ao);
    j["anisotropy"] = ChanToJson(m.anisotropy);
    j["sssMask"] = ChanToJson(m.sssMask);
    j["glow"] = ChanToJson(m.glow);
    j["rimMask"] = ChanToJson(m.rimMask);
    j["useNormalMap"] = m.useNormalMap;
    j["normalRGOnly"] = m.normalRGOnly;
    j["normalStrength"] = m.normalStrength;
    j["toonThreshold"] = m.toonThreshold;
    j["toonSoftness"] = m.toonSoftness;
    j["toonShadowTint"] = m.toonShadowTint;
    j["rimStrength"] = m.rimStrength;
    j["rimPower"] = m.rimPower;
    j["anisoStrength"] = m.anisoStrength;
    j["anisoSharpness"] = m.anisoSharpness;
    j["sssStrength"] = m.sssStrength;
    j["metalF0"] = m.metalF0;
    j["glowStrength"] = m.glowStrength;
    j["alphaCutoff"] = m.alphaCutoff;
    j["alphaClip"] = m.alphaClip;
    j["outlineEnable"] = m.outlineEnable;
    j["outlineThickness"] = m.outlineThickness;
    return j.dump(2);
}

bool ShaderPresetLibrary::FromJson(const std::string& str, MaterialConfig& m) const {
    try {
        json j = json::parse(str);
        m = MaterialConfig::MakeNeutral();
        m.id = j.value("id", m.id);
        m.displayName = j.value("displayName", m.id);
        std::string fam = j.value("family", "ZZZ");
        if (fam == "GI") m.family = GameFamily::GI;
        else if (fam == "ZZZ") m.family = GameFamily::ZZZ;
        else m.family = GameFamily::Generic;
        if (j.contains("opacity")) ChanFromJson(j["opacity"], m.opacity);
        if (j.contains("shadowMask")) ChanFromJson(j["shadowMask"], m.shadowMask);
        if (j.contains("specular")) ChanFromJson(j["specular"], m.specular);
        if (j.contains("metallic")) ChanFromJson(j["metallic"], m.metallic);
        if (j.contains("roughness")) ChanFromJson(j["roughness"], m.roughness);
        if (j.contains("ao")) ChanFromJson(j["ao"], m.ao);
        if (j.contains("anisotropy")) ChanFromJson(j["anisotropy"], m.anisotropy);
        if (j.contains("sssMask")) ChanFromJson(j["sssMask"], m.sssMask);
        if (j.contains("glow")) ChanFromJson(j["glow"], m.glow);
        if (j.contains("rimMask")) ChanFromJson(j["rimMask"], m.rimMask);
        m.useNormalMap = j.value("useNormalMap", true);
        m.normalRGOnly = j.value("normalRGOnly", false);
        m.normalStrength = j.value("normalStrength", 1.f);
        m.toonThreshold = j.value("toonThreshold", 0.45f);
        m.toonSoftness = j.value("toonSoftness", 0.08f);
        m.toonShadowTint = j.value("toonShadowTint", 0.55f);
        m.rimStrength = j.value("rimStrength", 0.25f);
        m.rimPower = j.value("rimPower", 3.f);
        m.anisoStrength = j.value("anisoStrength", 0.f);
        m.anisoSharpness = j.value("anisoSharpness", 64.f);
        m.sssStrength = j.value("sssStrength", 0.f);
        m.metalF0 = j.value("metalF0", 0.04f);
        m.glowStrength = j.value("glowStrength", 1.f);
        m.alphaCutoff = j.value("alphaCutoff", 0.5f);
        m.alphaClip = j.value("alphaClip", false);
        m.outlineEnable = j.value("outlineEnable", true);
        m.outlineThickness = j.value("outlineThickness", 0.012f);
        return true;
    } catch (...) {
        return false;
    }
}

ShaderPresetLibrary& ShaderPresetLibrary::Get() {
    static ShaderPresetLibrary inst;
    return inst;
}

void ShaderPresetLibrary::LoadBuiltins() {
    m_Presets.clear();
    m_Presets.push_back(MaterialConfig::MakeDefaultZZZ_Skin());
    m_Presets.push_back(MaterialConfig::MakeDefaultZZZ_Cloth());
    m_Presets.push_back(MaterialConfig::MakeDefaultZZZ_Face());
    m_Presets.push_back(MaterialConfig::MakeDefaultZZZ_Hair());
    m_Presets.push_back(MaterialConfig::MakeDefaultGI_Skin());
    m_Presets.push_back(MaterialConfig::MakeDefaultGI_Cloth());
    m_Presets.push_back(MaterialConfig::MakeNeutral());
    Logger::Get().Info("ShaderPresetLibrary: " + std::to_string(m_Presets.size()) + " builtins");
}

int ShaderPresetLibrary::LoadDirectory(const std::string& dirUtf8) {
    int n = 0;
    try {
        auto root = PathUtil::FromUtf8(dirUtf8);
        if (!fs::exists(root)) return 0;
        for (auto& ent : fs::directory_iterator(root)) {
            if (!ent.is_regular_file()) continue;
            auto ext = ent.path().extension().wstring();
            if (ext != L".json" && ext != L".JSON") continue;
            std::ifstream in(ent.path());
            if (!in) continue;
            std::string content((std::istreambuf_iterator<char>(in)), {});
            MaterialConfig m;
            if (!FromJson(content, m)) continue;
            // replace by id
            bool replaced = false;
            for (auto& p : m_Presets) {
                if (p.id == m.id) { p = m; replaced = true; break; }
            }
            if (!replaced) m_Presets.push_back(m);
            ++n;
        }
    } catch (...) {}
    if (n) Logger::Get().Info("ShaderPresetLibrary: loaded " + std::to_string(n) + " from " + dirUtf8);
    return n;
}

const MaterialConfig* ShaderPresetLibrary::Find(const std::string& id) const {
    for (const auto& p : m_Presets)
        if (p.id == id) return &p;
    return nullptr;
}

int ShaderPresetLibrary::IndexOf(const std::string& id) const {
    for (int i = 0; i < (int)m_Presets.size(); ++i)
        if (m_Presets[i].id == id) return i;
    return 0;
}

const MaterialConfig& ShaderPresetLibrary::At(int index) const {
    static MaterialConfig s_fallback = MaterialConfig::MakeNeutral();
    if (m_Presets.empty()) return s_fallback;
    return m_Presets[std::clamp(index, 0, (int)m_Presets.size() - 1)];
}

bool ShaderPresetLibrary::SavePreset(const MaterialConfig& cfg, const std::string& dirUtf8) {
    try {
        auto dir = PathUtil::FromUtf8(dirUtf8);
        fs::create_directories(dir);
        auto path = dir / (cfg.id + ".json");
        std::ofstream out(path);
        if (!out) return false;
        out << ToJson(cfg);
        // update memory
        bool replaced = false;
        for (auto& p : m_Presets) {
            if (p.id == cfg.id) { p = cfg; replaced = true; break; }
        }
        if (!replaced) m_Presets.push_back(cfg);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace preview3d
