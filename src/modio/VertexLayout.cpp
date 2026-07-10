#include "VertexLayout.h"
#include "../core/PathUtil.h"
#include "../core/Logger.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace modio {
using json = nlohmann::json;

static std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static std::string Trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
    size_t b = s.size();
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

int AttrFormatSizeBytes(AttrFormat f) {
    switch (f) {
    case AttrFormat::R32G32B32_FLOAT:    return 12;
    case AttrFormat::R32G32B32A32_FLOAT: return 16;
    case AttrFormat::R32G32_FLOAT:       return 8;
    case AttrFormat::R32G32B32A32_UINT:  return 16;
    case AttrFormat::R16G16_FLOAT:       return 4;
    case AttrFormat::R16G16B16A16_FLOAT: return 8;
    case AttrFormat::R8G8B8A8_UNORM:     return 4;
    case AttrFormat::R8G8B8A8_UINT:      return 4;
    case AttrFormat::R32_UINT:           return 4;
    case AttrFormat::R16_UINT:           return 2;
    default: return 0;
    }
}

const char* AttrFormatName(AttrFormat f) {
    switch (f) {
    case AttrFormat::R32G32B32_FLOAT:    return "R32G32B32_FLOAT";
    case AttrFormat::R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
    case AttrFormat::R32G32_FLOAT:       return "R32G32_FLOAT";
    case AttrFormat::R32G32B32A32_UINT:  return "R32G32B32A32_UINT";
    case AttrFormat::R16G16_FLOAT:       return "R16G16_FLOAT";
    case AttrFormat::R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case AttrFormat::R8G8B8A8_UNORM:     return "R8G8B8A8_UNORM";
    case AttrFormat::R8G8B8A8_UINT:      return "R8G8B8A8_UINT";
    case AttrFormat::R32_UINT:           return "R32_UINT";
    case AttrFormat::R16_UINT:           return "R16_UINT";
    default: return "Unknown";
    }
}

AttrFormat AttrFormatFromDxgiName(const std::string& name) {
    std::string n = ToLower(name);
    // strip DXGI_FORMAT_
    const char* pfx = "dxgi_format_";
    if (n.rfind(pfx, 0) == 0) n = n.substr(std::strlen(pfx));

    if (n == "r32g32b32_float") return AttrFormat::R32G32B32_FLOAT;
    if (n == "r32g32b32a32_float") return AttrFormat::R32G32B32A32_FLOAT;
    if (n == "r32g32_float") return AttrFormat::R32G32_FLOAT;
    if (n == "r32g32b32a32_uint") return AttrFormat::R32G32B32A32_UINT;
    if (n == "r16g16_float") return AttrFormat::R16G16_FLOAT;
    if (n == "r16g16b16a16_float") return AttrFormat::R16G16B16A16_FLOAT;
    if (n == "r8g8b8a8_unorm") return AttrFormat::R8G8B8A8_UNORM;
    if (n == "r8g8b8a8_uint") return AttrFormat::R8G8B8A8_UINT;
    if (n == "r32_uint") return AttrFormat::R32_UINT;
    if (n == "r16_uint") return AttrFormat::R16_UINT;
    return AttrFormat::Unknown;
}

static const char* kRoleNames[] = {
    "None",
    "Position",
    "Normal",
    "Tangent",
    "BlendWeight",
    "BlendIndex",
    "VertexColor",
    "UV0",
    "UV_LightMap",
    "UV_Outline",
    "UV_Backface",
    "UV_Extra0",
    "UV_Extra1",
    "Custom",
};

const char* AttrRoleName(AttrRole r) {
    int i = (int)r;
    if (i < 0 || i >= (int)(sizeof(kRoleNames) / sizeof(kRoleNames[0])))
        return "None";
    return kRoleNames[i];
}

const char* const* AttrRoleNameTable(int& outCount) {
    outCount = (int)(sizeof(kRoleNames) / sizeof(kRoleNames[0]));
    return kRoleNames;
}

AttrRole AttrRoleFromName(const std::string& name) {
    std::string n = ToLower(name);
    int count = 0;
    const char* const* table = AttrRoleNameTable(count);
    for (int i = 0; i < count; ++i) {
        if (ToLower(table[i]) == n)
            return static_cast<AttrRole>(i);
    }
    return AttrRole::None;
}

AttrRole SuggestRoleFromDump(const std::string& semanticName, int semanticIndex,
                             GameLayoutHint game) {
    std::string s = ToLower(semanticName);
    if (s == "position") return AttrRole::Position;
    if (s == "normal") return AttrRole::Normal;
    if (s == "tangent") return AttrRole::Tangent;
    if (s == "blendweights" || s == "blendweight") return AttrRole::BlendWeight;
    if (s == "blendindices" || s == "blendindex") return AttrRole::BlendIndex;
    if (s == "color") return AttrRole::VertexColor;

    if (s == "texcoord") {
        // Defaults — user must override when wrong.
        if (semanticIndex == 0) return AttrRole::UV0;
        if (game == GameLayoutHint::ZZZ) {
            // Research: TEXCOORD1 = outline projection packing (float2), NOT a UV sample
            if (semanticIndex == 1) return AttrRole::UV_Outline;
            if (semanticIndex == 2) return AttrRole::UV_LightMap;
            if (semanticIndex == 3) return AttrRole::UV_Extra0;
        } else if (game == GameLayoutHint::GI) {
            // GI often: UV0 base, UV1 light / special; outline may live in TANGENT
            if (semanticIndex == 1) return AttrRole::UV_LightMap;
            if (semanticIndex == 2) return AttrRole::UV_Extra0;
        } else {
            if (semanticIndex == 1) return AttrRole::UV_LightMap;
            if (semanticIndex == 2) return AttrRole::UV_Extra0;
        }
        return AttrRole::Custom;
    }
    return AttrRole::None;
}

const LayoutElement* BufferLayout::FindByRole(AttrRole role) const {
    for (const auto& e : elements)
        if (e.role == role) return &e;
    return nullptr;
}

bool BufferLayout::HasRole(AttrRole role) const {
    return FindByRole(role) != nullptr;
}

void AssignSuggestedRoles(BufferLayout& layout, GameLayoutHint game) {
    for (auto& e : layout.elements) {
        if (e.roleManual) continue;
        e.role = SuggestRoleFromDump(e.dumpSemanticName, e.dumpSemanticIndex, game);
    }
}

bool SetElementRole(BufferLayout& layout, int elementIndex, AttrRole role) {
    for (auto& e : layout.elements) {
        if (e.index == elementIndex) {
            e.role = role;
            e.roleManual = true;
            layout.source = LayoutSource::Manual;
            return true;
        }
    }
    if (elementIndex >= 0 && elementIndex < (int)layout.elements.size()) {
        layout.elements[elementIndex].role = role;
        layout.elements[elementIndex].roleManual = true;
        layout.source = LayoutSource::Manual;
        return true;
    }
    return false;
}

static LayoutElement MakeEl(int idx, const char* name, int semIdx, AttrFormat fmt, int off,
                            AttrRole role) {
    LayoutElement e;
    e.index = idx;
    e.dumpSemanticName = name;
    e.dumpSemanticIndex = semIdx;
    e.format = fmt;
    e.offset = off;
    e.sizeBytes = AttrFormatSizeBytes(fmt);
    e.role = role;
    e.inputSlot = 0;
    return e;
}

BufferLayout Preset_ZZZ_Position40() {
    BufferLayout L;
    L.name = "ZZZ/XXMI Position stride=40";
    L.kind = LayoutBufferKind::Position;
    L.stride = 40;
    L.source = LayoutSource::Preset;
    L.valid = true;
    L.elements = {
        MakeEl(0, "POSITION", 0, AttrFormat::R32G32B32_FLOAT, 0, AttrRole::Position),
        MakeEl(1, "NORMAL", 0, AttrFormat::R32G32B32_FLOAT, 12, AttrRole::Normal),
        MakeEl(2, "TANGENT", 0, AttrFormat::R32G32B32A32_FLOAT, 24, AttrRole::Tangent),
    };
    return L;
}

BufferLayout Preset_ZZZ_Texcoord20() {
    // color(4) + uv0 half2(4) + outline float2(8) + uv2 half2(4) = 20
    BufferLayout L;
    L.name = "ZZZ/XXMI Texcoord stride=20";
    L.kind = LayoutBufferKind::Texcoord;
    L.stride = 20;
    L.source = LayoutSource::Preset;
    L.valid = true;
    L.elements = {
        MakeEl(0, "COLOR", 0, AttrFormat::R8G8B8A8_UNORM, 0, AttrRole::VertexColor),
        MakeEl(1, "TEXCOORD", 0, AttrFormat::R16G16_FLOAT, 4, AttrRole::UV0),
        MakeEl(2, "TEXCOORD", 1, AttrFormat::R32G32_FLOAT, 8, AttrRole::UV_Outline),
        MakeEl(3, "TEXCOORD", 2, AttrFormat::R16G16_FLOAT, 16, AttrRole::UV_LightMap),
    };
    return L;
}

BufferLayout Preset_ZZZ_Texcoord24() {
    // + uv3 half2
    BufferLayout L = Preset_ZZZ_Texcoord20();
    L.name = "ZZZ/XXMI Texcoord stride=24";
    L.stride = 24;
    LayoutElement e = MakeEl(4, "TEXCOORD", 3, AttrFormat::R16G16_FLOAT, 20, AttrRole::UV_Extra0);
    L.elements.push_back(e);
    return L;
}

BufferLayout PresetForPositionStride(int stride, GameLayoutHint game) {
    (void)game;
    if (stride == 40) return Preset_ZZZ_Position40();
    BufferLayout L;
    L.name = "Unknown Position stride=" + std::to_string(stride);
    L.kind = LayoutBufferKind::Position;
    L.stride = stride;
    L.source = LayoutSource::Preset;
    L.valid = false;
    L.error = "No preset for position stride " + std::to_string(stride) + " — provide dump or manual layout";
    return L;
}

BufferLayout PresetForTexcoordStride(int stride, GameLayoutHint game) {
    (void)game;
    if (stride == 20) return Preset_ZZZ_Texcoord20();
    if (stride == 24) return Preset_ZZZ_Texcoord24();
    BufferLayout L;
    L.name = "Unknown Texcoord stride=" + std::to_string(stride);
    L.kind = LayoutBufferKind::Texcoord;
    L.stride = stride;
    L.source = LayoutSource::Preset;
    L.valid = false;
    L.error = "No preset for texcoord stride " + std::to_string(stride) +
              " — dump required (half vs float UV cannot be guessed)";
    return L;
}

BufferLayout ParseDumpVbLayoutHeader(const std::string& dumpFilePath) {
    BufferLayout L;
    L.source = LayoutSource::DumpFile;
    L.sourcePath = dumpFilePath;
    L.kind = LayoutBufferKind::InterleavedFull;
    L.name = "Dump: " + dumpFilePath;

    const std::string path = PathUtil::NormalizeToUtf8Path(dumpFilePath);
#ifdef _WIN32
    std::ifstream in(PathUtil::FromUtf8(path));
#else
    std::ifstream in(path);
#endif
    if (!in) {
        L.valid = false;
        L.error = "Cannot open dump: " + path;
        return L;
    }

    std::string line;
    LayoutElement cur;
    bool inElement = false;
    int elementCount = 0;

    auto flushElement = [&]() {
        if (!inElement) return;
        cur.sizeBytes = AttrFormatSizeBytes(cur.format);
        cur.index = elementCount++;
        L.elements.push_back(cur);
        cur = LayoutElement{};
        inElement = false;
    };

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = Trim(line);
        if (t.empty()) continue;

        // Stop before huge vertex body
        if (ToLower(t).rfind("vertex-data", 0) == 0 ||
            ToLower(t).rfind("vb0[", 0) == 0) {
            flushElement();
            break;
        }

        if (ToLower(t).rfind("stride:", 0) == 0) {
            L.stride = std::atoi(Trim(t.substr(7)).c_str());
            continue;
        }

        if (ToLower(t).rfind("element[", 0) == 0) {
            flushElement();
            inElement = true;
            cur = LayoutElement{};
            // element[N]:
            size_t lb = t.find('[');
            size_t rb = t.find(']');
            if (lb != std::string::npos && rb != std::string::npos)
                cur.index = std::atoi(t.substr(lb + 1, rb - lb - 1).c_str());
            continue;
        }

        if (!inElement) continue;

        auto takeKey = [&](const char* key) -> std::string {
            std::string low = ToLower(t);
            std::string k = std::string(key) + ":";
            if (low.rfind(ToLower(k), 0) != 0) return {};
            return Trim(t.substr(std::strlen(key) + 1));
        };

        if (auto v = takeKey("SemanticName"); !v.empty()) {
            cur.dumpSemanticName = v;
            continue;
        }
        if (auto v = takeKey("SemanticIndex"); !v.empty()) {
            cur.dumpSemanticIndex = std::atoi(v.c_str());
            continue;
        }
        if (auto v = takeKey("Format"); !v.empty()) {
            cur.format = AttrFormatFromDxgiName(v);
            continue;
        }
        if (auto v = takeKey("AlignedByteOffset"); !v.empty()) {
            cur.offset = std::atoi(v.c_str());
            continue;
        }
        if (auto v = takeKey("InputSlot"); !v.empty()) {
            cur.inputSlot = std::atoi(v.c_str());
            continue;
        }
    }
    flushElement();

    if (L.elements.empty()) {
        L.valid = false;
        L.error = "No elements parsed from dump header";
        return L;
    }

    // If stride missing, compute from max offset+size
    if (L.stride <= 0) {
        int maxEnd = 0;
        for (const auto& e : L.elements)
            maxEnd = std::max(maxEnd, e.offset + e.sizeBytes);
        L.stride = maxEnd;
    }

    AssignSuggestedRoles(L, GameLayoutHint::ZZZ);
    L.valid = true;
    Logger::Get().Info("Parsed dump layout: " + path +
                       " elements=" + std::to_string(L.elements.size()) +
                       " stride=" + std::to_string(L.stride));
    return L;
}

void SplitDumpLayoutByInputSlot(const BufferLayout& full,
                                BufferLayout& outSlot0,
                                BufferLayout& outSlot1) {
    outSlot0 = BufferLayout{};
    outSlot1 = BufferLayout{};
    outSlot0.source = full.source;
    outSlot1.source = full.source;
    outSlot0.sourcePath = full.sourcePath;
    outSlot1.sourcePath = full.sourcePath;
    outSlot0.kind = LayoutBufferKind::Position;
    outSlot1.kind = LayoutBufferKind::Texcoord;
    outSlot0.name = full.name + " [slot0]";
    outSlot1.name = full.name + " [slot1]";

    int max0 = 0, max1 = 0;
    for (const auto& e : full.elements) {
        LayoutElement c = e;
        if (e.inputSlot <= 0) {
            // offset already within slot0 for interleaved? In full vb0 dump offsets are global.
            // For interleaved full buffer, InputSlot is often all 0 with global offsets.
            // Only rebase when multiple slots present with per-slot offsets (split dump).
            outSlot0.elements.push_back(c);
            max0 = std::max(max0, c.offset + c.sizeBytes);
        } else if (e.inputSlot == 1) {
            outSlot1.elements.push_back(c);
            max1 = std::max(max1, c.offset + c.sizeBytes);
        } else {
            // higher slots → attach to texcoord as extras
            outSlot1.elements.push_back(c);
            max1 = std::max(max1, c.offset + c.sizeBytes);
        }
    }

    // Detect interleaved single-slot full dump (all inputSlot 0, stride ~92)
    bool multiSlot = false;
    for (const auto& e : full.elements)
        if (e.inputSlot > 0) multiSlot = true;

    if (!multiSlot) {
        // Split by semantic groups into Position vs Texcoord streams (XXMI export style)
        outSlot0.elements.clear();
        outSlot1.elements.clear();
        max0 = max1 = 0;
        int off0 = 0, off1 = 0;
        for (const auto& e : full.elements) {
            LayoutElement c = e;
            std::string sn = ToLower(e.dumpSemanticName);
            bool isPosStream = (sn == "position" || sn == "normal" || sn == "tangent" ||
                                sn == "blendweights" || sn == "blendindices");
            if (isPosStream) {
                // Skip blend in position preview layout but keep offsets consistent with XXMI Position.buf
                if (sn == "blendweights" || sn == "blendindices")
                    continue; // Position.buf is only pos+nrm+tan (40)
                c.offset = off0;
                c.sizeBytes = AttrFormatSizeBytes(c.format);
                off0 += c.sizeBytes;
                outSlot0.elements.push_back(c);
                max0 = off0;
            } else {
                c.offset = off1;
                c.sizeBytes = AttrFormatSizeBytes(c.format);
                off1 += c.sizeBytes;
                outSlot1.elements.push_back(c);
                max1 = off1;
            }
        }
        // Re-index
        for (int i = 0; i < (int)outSlot0.elements.size(); ++i) outSlot0.elements[i].index = i;
        for (int i = 0; i < (int)outSlot1.elements.size(); ++i) outSlot1.elements[i].index = i;
    }

    outSlot0.stride = max0;
    outSlot1.stride = max1;
    // Snap to known XXMI strides when close
    if (outSlot0.stride > 0 && outSlot0.stride <= 40) outSlot0.stride = 40;
    outSlot0.valid = !outSlot0.elements.empty();
    outSlot1.valid = !outSlot1.elements.empty();
    if (outSlot0.valid) AssignSuggestedRoles(outSlot0, GameLayoutHint::ZZZ);
    if (outSlot1.valid) AssignSuggestedRoles(outSlot1, GameLayoutHint::ZZZ);
}

std::string BufferLayoutToJson(const BufferLayout& layout) {
    json j;
    j["name"] = layout.name;
    j["kind"] = (int)layout.kind;
    j["stride"] = layout.stride;
    j["source"] = (int)layout.source;
    j["sourcePath"] = layout.sourcePath;
    j["valid"] = layout.valid;
    json arr = json::array();
    for (const auto& e : layout.elements) {
        json ej;
        ej["index"] = e.index;
        ej["semantic"] = e.dumpSemanticName;
        ej["semanticIndex"] = e.dumpSemanticIndex;
        ej["format"] = AttrFormatName(e.format);
        ej["offset"] = e.offset;
        ej["inputSlot"] = e.inputSlot;
        ej["role"] = AttrRoleName(e.role);
        ej["roleManual"] = e.roleManual;
        arr.push_back(ej);
    }
    j["elements"] = arr;
    return j.dump();
}

bool BufferLayoutFromJson(const std::string& jsonStr, BufferLayout& out) {
    try {
        json j = json::parse(jsonStr);
        out = BufferLayout{};
        out.name = j.value("name", "");
        out.kind = static_cast<LayoutBufferKind>(j.value("kind", 0));
        out.stride = j.value("stride", 0);
        out.source = static_cast<LayoutSource>(j.value("source", 0));
        out.sourcePath = j.value("sourcePath", "");
        out.valid = j.value("valid", false);
        if (j.contains("elements") && j["elements"].is_array()) {
            for (const auto& ej : j["elements"]) {
                LayoutElement e;
                e.index = ej.value("index", 0);
                e.dumpSemanticName = ej.value("semantic", "");
                e.dumpSemanticIndex = ej.value("semanticIndex", 0);
                e.format = AttrFormatFromDxgiName(ej.value("format", ""));
                e.offset = ej.value("offset", 0);
                e.inputSlot = ej.value("inputSlot", 0);
                e.sizeBytes = AttrFormatSizeBytes(e.format);
                e.role = AttrRoleFromName(ej.value("role", "None"));
                e.roleManual = ej.value("roleManual", false);
                out.elements.push_back(e);
            }
        }
        out.valid = out.valid || !out.elements.empty();
        return true;
    } catch (...) {
        return false;
    }
}

std::string FormatLayoutSummary(const BufferLayout& layout) {
    std::ostringstream o;
    o << layout.name << "  stride=" << layout.stride
      << "  source=" << (int)layout.source
      << "  elements=" << layout.elements.size()
      << (layout.valid ? " OK" : " INVALID");
    if (!layout.error.empty()) o << "  err=" << layout.error;
    o << "\n";
    for (const auto& e : layout.elements) {
        o << "  [" << e.index << "] +" << e.offset << " "
          << e.dumpSemanticName;
        if (e.dumpSemanticIndex > 0) o << e.dumpSemanticIndex;
        o << " " << AttrFormatName(e.format)
          << " → " << AttrRoleName(e.role)
          << (e.roleManual ? " (manual)" : "")
          << "\n";
    }
    return o.str();
}

float HalfToFloat(uint16_t h) {
    // IEEE half → float
    uint32_t sign = (h >> 15) & 1;
    uint32_t exp  = (h >> 10) & 0x1f;
    uint32_t mant = h & 0x3ff;
    uint32_t f;
    if (exp == 0) {
        if (mant == 0) {
            f = sign << 31;
        } else {
            exp = 127 - 15 + 1;
            while ((mant & 0x400) == 0) { mant <<= 1; --exp; }
            mant &= 0x3ff;
            f = (sign << 31) | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        f = (sign << 31) | 0x7f800000 | (mant << 13);
    } else {
        f = (sign << 31) | ((exp + (127 - 15)) << 23) | (mant << 13);
    }
    float out;
    std::memcpy(&out, &f, sizeof(out));
    return out;
}

bool DecodeAttrToFloat4(const uint8_t* vertexBase, int stride, const LayoutElement& el,
                        float out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0.f;
    if (!vertexBase || el.offset < 0 || el.sizeBytes <= 0) return false;
    if (el.offset + el.sizeBytes > stride) return false;
    const uint8_t* p = vertexBase + el.offset;

    switch (el.format) {
    case AttrFormat::R32G32B32_FLOAT: {
        const float* f = reinterpret_cast<const float*>(p);
        out[0] = f[0]; out[1] = f[1]; out[2] = f[2]; out[3] = 1.f;
        return true;
    }
    case AttrFormat::R32G32B32A32_FLOAT: {
        const float* f = reinterpret_cast<const float*>(p);
        out[0] = f[0]; out[1] = f[1]; out[2] = f[2]; out[3] = f[3];
        return true;
    }
    case AttrFormat::R32G32_FLOAT: {
        const float* f = reinterpret_cast<const float*>(p);
        out[0] = f[0]; out[1] = f[1]; out[2] = 0.f; out[3] = 0.f;
        return true;
    }
    case AttrFormat::R16G16_FLOAT: {
        const uint16_t* h = reinterpret_cast<const uint16_t*>(p);
        out[0] = HalfToFloat(h[0]);
        out[1] = HalfToFloat(h[1]);
        return true;
    }
    case AttrFormat::R16G16B16A16_FLOAT: {
        const uint16_t* h = reinterpret_cast<const uint16_t*>(p);
        out[0] = HalfToFloat(h[0]); out[1] = HalfToFloat(h[1]);
        out[2] = HalfToFloat(h[2]); out[3] = HalfToFloat(h[3]);
        return true;
    }
    case AttrFormat::R8G8B8A8_UNORM: {
        out[0] = p[0] / 255.f; out[1] = p[1] / 255.f;
        out[2] = p[2] / 255.f; out[3] = p[3] / 255.f;
        return true;
    }
    case AttrFormat::R32G32B32A32_UINT: {
        const uint32_t* u = reinterpret_cast<const uint32_t*>(p);
        out[0] = (float)u[0]; out[1] = (float)u[1];
        out[2] = (float)u[2]; out[3] = (float)u[3];
        return true;
    }
    default:
        return false;
    }
}

} // namespace modio
