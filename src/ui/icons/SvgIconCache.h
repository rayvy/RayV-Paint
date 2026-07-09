#pragma once
#include <d3d11.h>
#include <imgui.h>
#include <string>
#include <unordered_map>

namespace Ui {

struct SvgIcon {
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0;
    int h = 0;
    bool valid() const { return srv != nullptr; }
};

// Loads monochrome SVG paths → white RGBA texture; draw with tint.
class SvgIconCache {
public:
    static SvgIconCache& Get();

    void SetDevice(ID3D11Device* device);
    void Clear();

    // Logical name without path, e.g. "tool_brush" or "brush" — tries known roots + .svg
    const SvgIcon* Get(const char* logicalName, int pixelSize = 64);

    // Draw icon fitted in rect with theme tint (or custom color)
    void Draw(const SvgIcon* icon, ImVec2 min, ImVec2 max, ImU32 tint) const;
    void DrawCentered(const SvgIcon* icon, ImVec2 center, float side, ImU32 tint) const;

private:
    SvgIconCache() = default;
    bool LoadFile(const std::string& path, int pixelSize, SvgIcon& out);
    std::string ResolvePath(const char* logicalName) const;

    ID3D11Device* m_Device = nullptr;
    // key = name#size
    std::unordered_map<std::string, SvgIcon> m_Cache;
};

} // namespace Ui
