#pragma once

#include "VertexLayout.h"

#include <cstdint>
#include <string>
#include <vector>

// Pure data model for XXMI / 3DMigoto character mods (ZZZ + GI first).
// No D3D — safe for unit tests and headless parse.

namespace modio {

enum class BufferKind : uint8_t {
    Unknown = 0,
    Position,
    Blend,      // ignored by preview
    Texcoord,
    Index,
    Texture
};

enum class MaterialSlot : uint8_t {
    Unknown = 0,
    Diffuse,       // always RGB BaseColor
    NormalMap,
    LightMap,
    MaterialMap,
    Opacity,       // e.g. ps-t69 custom API
    Glow,          // e.g. ps-t70 RabbitFX
    Custom
};

enum class BindSource : uint8_t {
    ApiNamed = 0,  // Resource\ZZMI\Diffuse = ref ResourceX  (priority)
    PsSlot,        // ps-tN = ResourceX
    FilenameHint,  // inferred from Resource name / file
    Manual         // user override later
};

struct ModResource {
    std::string sectionName;   // e.g. ResourceBelleBodyPosition
    std::string filename;      // relative to ini dir
    std::string absolutePath;  // resolved
    int stride = 0;
    std::string format;        // DXGI_FORMAT_… for IB
    BufferKind kind = BufferKind::Unknown;
    bool exists = false;
};

struct TextureBind {
    MaterialSlot slot = MaterialSlot::Unknown;
    BindSource source = BindSource::ApiNamed;
    std::string apiName;       // "Diffuse", "NormalMap", …
    int psSlot = -1;           // for PsSlot binds
    std::string resourceName;  // Resource section
    std::string absolutePath;
    bool exists = false;
};

struct DrawIndexed {
    int indexCount = 0;
    int indexStart = 0;
    int baseVertex = 0;        // 3rd arg; always 0 in XXMI exports
    std::string commentLabel;  // from preceding ; comment if any
    bool visible = true;
};

// One material group inside a part: textures active for one or more drawindexed.
// Belle BodyA: jacket maps → several draws, then BodyDiffuse maps → base mesh draw.
struct DrawBatch {
    std::string name;          // comment label or "batchN"
    std::vector<TextureBind> textures;
    std::vector<DrawIndexed> draws;
    bool visible = true;
};

struct ModPart {
    std::string sectionName;   // TextureOverrideBelleBodyA
    std::string name;          // BodyA / Part0 / synthetic
    std::string ibResource;
    std::string ibAbsolutePath;
    int matchFirstIndex = -1;
    std::string hash;
    std::vector<DrawBatch> batches; // multi-texture sequential groups
    bool visible = true;
    bool hasGeometry = true;   // false for texture-only overrides

    // Flattened views (filled after parse) for stats / simple UIs
    int TotalDraws() const;
    int TotalTextureBinds() const;
};

struct ModComponent {
    std::string name;          // Hair, Body, Legs, …
    std::string positionResource;
    std::string texcoordResource;
    std::string blendResource; // stored but unused for preview
    std::string positionPath;
    std::string texcoordPath;
    int positionStride = 40;
    int texcoordStride = 20;
    int vertexCountHint = 0;   // from draw= N,0 on blend section
    std::vector<ModPart> parts;
    bool visible = true;

    // Vertex layouts — preset / dump / manual roles (UV_Outline ≠ UV0, etc.)
    BufferLayout positionLayout;
    BufferLayout texcoordLayout;
};

struct ParseWarning {
    std::string message;
};

struct ModScene {
    std::string iniPath;
    std::string iniDirectory;
    std::string dumpPath;              // frame analysis / component dump folder
    GameLayoutHint gameHint = GameLayoutHint::ZZZ;

    std::vector<ModResource> resources;
    std::vector<ModComponent> components;
    std::vector<ModPart> orphanParts; // texture overrides that couldn't attach to a component
    std::vector<ParseWarning> warnings;

    // Global default layouts (applied to components without overrides)
    BufferLayout defaultPositionLayout;
    BufferLayout defaultTexcoordLayout;
    // Last dump parse (full interleaved + split)
    BufferLayout dumpFullLayout;
    BufferLayout dumpPositionLayout;
    BufferLayout dumpTexcoordLayout;

    bool ok = false;
    std::string error;

    // Stats helpers
    int PartCount() const;
    int DrawCount() const;
    int TextureBindCount() const;
};

// Special ps-tN roles (RabbitFX / custom APIs) — data-driven table.
struct SpecialSlotHint {
    int psSlot = -1;
    MaterialSlot role = MaterialSlot::Custom;
    const char* note = "";
};

// Built-in defaults; can extend later via config.
inline const SpecialSlotHint* FindSpecialSlot(int psSlot) {
    static const SpecialSlotHint kHints[] = {
        { 69, MaterialSlot::Opacity, "custom opacity (common API)" },
        { 70, MaterialSlot::Glow,    "glow / RabbitFX-style" },
    };
    for (const auto& h : kHints)
        if (h.psSlot == psSlot) return &h;
    return nullptr;
}

const char* MaterialSlotName(MaterialSlot s);
MaterialSlot MaterialSlotFromApiToken(const std::string& token);
MaterialSlot MaterialSlotFromResourceName(const std::string& resourceOrFile);
BufferKind GuessBufferKind(const std::string& sectionName, int stride, const std::string& format);

} // namespace modio
