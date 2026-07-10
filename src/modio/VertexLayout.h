#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Vertex attribute layout + *shader roles* (what RayV unified preview shader consumes).
//
// IMPORTANT:
// - Dump TEXCOORD ≠ always a texture UV.
// - TEXCOORD1 may be outline packing (ZZZ), not UV.
// - COLOR may be vertex color, unused, or special.
// - Formats (half vs float) come from dump or user override — never guess from stride alone.
// - User can remap any element → AttrRole::None or another role when auto is wrong.

namespace modio {

enum class AttrFormat : uint8_t {
    Unknown = 0,
    R32G32B32_FLOAT,      // 12
    R32G32B32A32_FLOAT,   // 16
    R32G32_FLOAT,         // 8
    R32G32B32A32_UINT,    // 16
    R16G16_FLOAT,         // 4
    R16G16B16A16_FLOAT,   // 8
    R8G8B8A8_UNORM,       // 4
    R8G8B8A8_UINT,
    R32_UINT,
    R16_UINT,
};

// What the unified RayV mesh/shader pipeline will use this field for.
enum class AttrRole : uint8_t {
    None = 0,             // ignore completely

    Position,
    Normal,
    Tangent,              // xyz + sign w
    BlendWeight,          // ignored by preview mesh
    BlendIndex,           // ignored by preview mesh

    VertexColor,
    UV0,                  // primary diffuse UV
    UV_LightMap,          // secondary lightmap UV
    UV_Outline,           // outline projection / packed data — NOT a tex sample UV
    UV_Backface,          // dual-sided / backface param
    UV_Extra0,
    UV_Extra1,
    Custom
};

enum class LayoutSource : uint8_t {
    Unknown = 0,
    Preset,
    DumpFile,
    Manual
};

enum class LayoutBufferKind : uint8_t {
    Unknown = 0,
    Position,
    Blend,
    Texcoord,
    InterleavedFull       // pre-split full vb0 dump
};

enum class GameLayoutHint : uint8_t {
    Generic = 0,
    ZZZ,
    GI,
    HSR
};

struct LayoutElement {
    int index = 0;
    std::string dumpSemanticName;   // POSITION, TEXCOORD, COLOR…
    int dumpSemanticIndex = 0;
    AttrFormat format = AttrFormat::Unknown;
    int offset = 0;                 // within this buffer
    int inputSlot = 0;              // dump InputSlot (0/1…)
    int sizeBytes = 0;

    AttrRole role = AttrRole::None; // shader mapping — user editable
    bool roleManual = false;        // lock against re-auto
};

struct BufferLayout {
    std::string name;
    LayoutBufferKind kind = LayoutBufferKind::Unknown;
    int stride = 0;
    LayoutSource source = LayoutSource::Unknown;
    std::string sourcePath;
    std::vector<LayoutElement> elements;
    bool valid = false;
    std::string error;

    const LayoutElement* FindByRole(AttrRole role) const;
    bool HasRole(AttrRole role) const;
};

// ---- Format / role names ----
int AttrFormatSizeBytes(AttrFormat f);
const char* AttrFormatName(AttrFormat f);
AttrFormat AttrFormatFromDxgiName(const std::string& name);

const char* AttrRoleName(AttrRole r);
AttrRole AttrRoleFromName(const std::string& name);
// Null-terminated list for ImGui combo
const char* const* AttrRoleNameTable(int& outCount);

AttrRole SuggestRoleFromDump(const std::string& semanticName, int semanticIndex,
                             GameLayoutHint game = GameLayoutHint::ZZZ);

// Built-in presets (XXMI split exports)
BufferLayout Preset_ZZZ_Position40();
BufferLayout Preset_ZZZ_Texcoord20();
BufferLayout Preset_ZZZ_Texcoord24();
BufferLayout PresetForPositionStride(int stride, GameLayoutHint game = GameLayoutHint::ZZZ);
BufferLayout PresetForTexcoordStride(int stride, GameLayoutHint game = GameLayoutHint::ZZZ);

// Parse 3DMigoto vb*.txt header only (stops at "vertex-data:")
BufferLayout ParseDumpVbLayoutHeader(const std::string& dumpFilePath);

// Split multi-slot dump into per-buffer layouts (offsets rebased per slot)
void SplitDumpLayoutByInputSlot(const BufferLayout& full,
                                BufferLayout& outSlot0,
                                BufferLayout& outSlot1);

void AssignSuggestedRoles(BufferLayout& layout, GameLayoutHint game = GameLayoutHint::ZZZ);
bool SetElementRole(BufferLayout& layout, int elementIndex, AttrRole role);

std::string BufferLayoutToJson(const BufferLayout& layout);
bool BufferLayoutFromJson(const std::string& jsonStr, BufferLayout& out);
std::string FormatLayoutSummary(const BufferLayout& layout);

// Decode one attribute of one vertex into float4
bool DecodeAttrToFloat4(const uint8_t* vertexBase, int stride, const LayoutElement& el,
                        float out[4]);
float HalfToFloat(uint16_t h);

} // namespace modio
