#include "TextureSet.h"
#include "../core/Logger.h"
#include <nlohmann/json.hpp>
#include <algorithm>

namespace texset {
using json = nlohmann::json;

int TextureSet::LogicalWidth() const {
    if (const MapSlot* d = GetMap(MapKind::Diffuse))
        if (d->width > 0) return d->width;
    for (const auto& m : maps)
        if (m.enabled && m.width > 0) return m.width;
    return 0;
}

int TextureSet::LogicalHeight() const {
    if (const MapSlot* d = GetMap(MapKind::Diffuse))
        if (d->height > 0) return d->height;
    for (const auto& m : maps)
        if (m.enabled && m.height > 0) return m.height;
    return 0;
}

TextureSet TextureSet::CreateDefault(const std::string& name) {
    TextureSet s;
    s.name = name;
    s.templateId = "Default";
    s.maps = Template_Default().maps;
    s.activeMap = MapKind::Diffuse;
    return s;
}

TextureSet TextureSet::CreateFromTemplate(const std::string& name, const SetTemplate& t) {
    TextureSet s;
    s.name = name;
    s.templateId = t.id;
    s.maps = t.maps;
    EnsureDiffuseSlot(s.maps);
    s.activeMap = MapKind::Diffuse;
    return s;
}

void TextureSet::ApplySetTemplate(const SetTemplate& t) {
    templateId = t.id;
    ApplyTemplate(maps, t);
    if (!GetMap(activeMap) || !GetMap(activeMap)->enabled)
        activeMap = MapKind::Diffuse;
}

bool TextureSet::EnableMap(MapKind k, int w, int h, const std::string& sourcePath) {
    MapSlot* slot = GetMap(k);
    if (!slot) {
        MapSlot ns;
        ns.kind = k;
        maps.push_back(ns);
        slot = GetMap(k);
    }
    if (!slot) return false;
    slot->enabled = true;
    if (w > 0 && h > 0) {
        slot->width = w;
        slot->height = h;
    }
    if (!sourcePath.empty())
        slot->sourcePath = sourcePath;
    return true;
}

void TextureSet::DisableMap(MapKind k) {
    // Diffuse may also be disabled (user request) — paint layers still hold pixels
    if (MapSlot* s = GetMap(k))
        s->enabled = false;
    if (activeMap == k) {
        activeMap = MapKind::Diffuse;
        for (const auto& m : maps)
            if (m.enabled) { activeMap = m.kind; break; }
    }
}

static json PackToJson(const ChannelPackEntry& e) {
    json j;
    j["role"] = ChannelRoleName(e.role);
    j["invert"] = e.invert;
    j["scale"] = e.scale;
    j["bias"] = e.bias;
    return j;
}

static ChannelPackEntry PackFromJson(const json& j) {
    ChannelPackEntry e;
    if (!j.is_object()) return e;
    e.role = ChannelRoleFromName(j.value("role", "None"));
    e.invert = j.value("invert", false);
    e.scale = j.value("scale", 1.f);
    e.bias = j.value("bias", 0.f);
    return e;
}

std::string TextureSet::MetaToJson() const {
    json j;
    j["id"] = id;
    j["name"] = name;
    j["templateId"] = templateId;
    j["activeMap"] = MapKindName(activeMap);
    json arr = json::array();
    for (const auto& m : maps) {
        json mj;
        mj["kind"] = MapKindName(m.kind);
        mj["enabled"] = m.enabled;
        mj["width"] = m.width;
        mj["height"] = m.height;
        mj["displayName"] = m.displayName;
        mj["exportPath"] = m.exportPath;
        mj["sourcePath"] = m.sourcePath;
        mj["nameSuffix"] = m.nameSuffix;
        mj["colorSpace"] = MapColorSpaceName(m.colorSpace);
        mj["exportCodec"] = MapExportCodecName(m.exportCodec);
        mj["exportMips"] = m.exportMips;
        json pack = json::array();
        for (int i = 0; i < 4; ++i)
            pack.push_back(PackToJson(m.pack[i]));
        mj["pack"] = pack;
        arr.push_back(mj);
    }
    j["maps"] = arr;
    return j.dump();
}

bool TextureSet::MetaFromJson(const std::string& str, TextureSet& out) {
    try {
        json j = json::parse(str);
        out = TextureSet{}; // move-assign empty (non-copyable)
        out.name = j.value("name", "Texture Set");
        out.templateId = j.value("templateId", "Default");
        out.id = j.value("id", 0);
        out.activeMap = MapKindFromName(j.value("activeMap", "Diffuse"));
        out.maps.clear();
        if (j.contains("maps") && j["maps"].is_array()) {
            for (const auto& mj : j["maps"]) {
                MapSlot m;
                m.kind = MapKindFromName(mj.value("kind", "Diffuse"));
                m.enabled = mj.value("enabled", m.kind == MapKind::Diffuse);
                m.width = mj.value("width", 0);
                m.height = mj.value("height", 0);
                m.displayName = mj.value("displayName", "");
                m.exportPath = mj.value("exportPath", "");
                m.sourcePath = mj.value("sourcePath", "");
                m.nameSuffix = mj.value("nameSuffix", "");
                {
                    std::string cs = mj.value("colorSpace", "Linear");
                    m.colorSpace = (cs == "sRGB" || cs == "SRGB") ? MapColorSpace::sRGB
                                                                  : MapColorSpace::Linear;
                }
                m.exportCodec = MapExportCodecFromName(mj.value("exportCodec", "PNG"));
                m.exportMips = mj.value("exportMips", true);
                if (mj.contains("pack") && mj["pack"].is_array()) {
                    int i = 0;
                    for (const auto& pj : mj["pack"]) {
                        if (i >= 4) break;
                        m.pack[i++] = PackFromJson(pj);
                    }
                }
                out.maps.push_back(m);
            }
        } else {
            out.maps = Template_Default().maps;
        }
        EnsureDiffuseSlot(out.maps);
        return true;
    } catch (...) {
        return false;
    }
}

// ---- Library ----

TextureSet* TextureSetLibrary::Active() { return Find(activeSetId); }
const TextureSet* TextureSetLibrary::Active() const { return Find(activeSetId); }

TextureSet* TextureSetLibrary::Find(int id) {
    for (auto& s : sets)
        if (s.id == id) return &s;
    return nullptr;
}

const TextureSet* TextureSetLibrary::Find(int id) const {
    for (const auto& s : sets)
        if (s.id == id) return &s;
    return nullptr;
}

int TextureSetLibrary::AddSet(TextureSet set) {
    set.id = nextId++;
    int id = set.id;
    sets.push_back(std::move(set));
    if (activeSetId < 0)
        activeSetId = id;
    return id;
}

bool TextureSetLibrary::RemoveSet(int id) {
    if (sets.size() <= 1) return false;
    auto it = std::remove_if(sets.begin(), sets.end(),
                             [id](const TextureSet& s) { return s.id == id; });
    if (it == sets.end()) return false;
    sets.erase(it, sets.end());
    if (activeSetId == id)
        activeSetId = sets.empty() ? -1 : sets.front().id;
    return true;
}

bool TextureSetLibrary::SetActive(int id) {
    if (!Find(id)) return false;
    activeSetId = id;
    return true;
}

void TextureSetLibrary::EnsureSimpleDefault() {
    if (sets.empty()) {
        auto s = TextureSet::CreateDefault("Document");
        AddSet(std::move(s));
    }
    if (activeSetId < 0 && !sets.empty())
        activeSetId = sets.front().id;
}

std::string TextureSetLibrary::MetaToJson() const {
    json j;
    j["activeSetId"] = activeSetId;
    j["nextId"] = nextId;
    json arr = json::array();
    for (const auto& s : sets)
        arr.push_back(json::parse(s.MetaToJson()));
    j["sets"] = arr;
    return j.dump();
}

bool TextureSetLibrary::MetaFromJson(const std::string& str, TextureSetLibrary& out) {
    try {
        json j = json::parse(str);
        // Rebuild in place (library is move-only / non-copyable)
        out.sets.clear();
        out.activeSetId = j.value("activeSetId", -1);
        out.nextId = j.value("nextId", 1);
        if (j.contains("sets") && j["sets"].is_array()) {
            for (const auto& sj : j["sets"]) {
                TextureSet s;
                if (TextureSet::MetaFromJson(sj.dump(), s))
                    out.sets.push_back(std::move(s));
            }
        }
        out.EnsureSimpleDefault();
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace texset
