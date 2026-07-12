#include "ColorsPanel.h"
#include "../EditorPanels.h"
#include "../widgets/UiPanel.h"
#include "../widgets/UiTooltip.h"
#include "../widgets/UiVisualSlider.h"
#include "../style/UiTokens.h"
#include "../../core/PaintEngine.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

extern float g_SecondaryColor[4];
extern bool g_ColorSwapPending;

namespace UI {

void DrawColorsPanel(UIState& state, Canvas& canvas, BrushSettings& brush) {
// 10. Colors — adaptive SV + Hue/Alpha; primary & secondary
if (state.showColors) {
    Ui::BeginDockPanel("Colors", &state.showColors);

    static bool s_EditSecondary = false;
    float* editCol = s_EditSecondary ? g_SecondaryColor : brush.color;
    const bool floatDoc = (canvas.GetDocumentBitDepth() != Canvas::DocumentBitDepth::U8);

    ImVec2 avail = ImGui::GetContentRegionAvail();
    // HSV picker uses clamped display RGB (HDR channels edited via float fields below).
    float dispR = std::clamp(editCol[0], 0.f, 1.f);
    float dispG = std::clamp(editCol[1], 0.f, 1.f);
    float dispB = std::clamp(editCol[2], 0.f, 1.f);
    float h, s, v;
    ImGui::ColorConvertRGBtoHSV(dispR, dispG, dispB, h, s, v);

    float stripH = 22.f;
    float svW = std::max(80.f, avail.x - 4.f);
    float svH = std::clamp(avail.y - stripH * 2.f - 100.f, 100.f, std::max(120.f, avail.y * 0.55f));
    if (avail.x > avail.y)
        svH = std::clamp(svW * 0.65f, 100.f, avail.y - stripH * 2.f - 90.f);

    ImVec2 sqPos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##svsq", ImVec2(svW, svH));
    bool svActive = ImGui::IsItemActive();
    bool svHovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto& tok = Ui::Tokens();

    const int steps = 28;
    for (int yi = 0; yi < steps; ++yi) {
        for (int xi = 0; xi < steps; ++xi) {
            float ss = (xi + 0.5f) / steps;
            float vv = 1.f - (yi + 0.5f) / steps;
            float rr, gg, bb;
            ImGui::ColorConvertHSVtoRGB(h, ss, vv, rr, gg, bb);
            ImU32 col = IM_COL32((int)(rr*255),(int)(gg*255),(int)(bb*255),255);
            float x0 = sqPos.x + xi * (svW / steps);
            float y0 = sqPos.y + yi * (svH / steps);
            float x1 = sqPos.x + (xi + 1) * (svW / steps);
            float y1 = sqPos.y + (yi + 1) * (svH / steps);
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col);
        }
    }
    dl->AddRect(sqPos, ImVec2(sqPos.x + svW, sqPos.y + svH), tok.ColU32(tok.strokeHairline), tok.rSm);
    ImVec2 cur(sqPos.x + s * svW, sqPos.y + (1.f - v) * svH);
    dl->AddCircle(cur, 6.f, IM_COL32(0,0,0,200), 16, 2.f);
    dl->AddCircle(cur, 6.f, IM_COL32(255,255,255,230), 16, 1.5f);

    if (svActive || (svHovered && ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
        ImVec2 mp = ImGui::GetIO().MousePos;
        s = std::clamp((mp.x - sqPos.x) / svW, 0.f, 1.f);
        v = std::clamp(1.f - (mp.y - sqPos.y) / svH, 0.f, 1.f);
        // HSV sets 0..1 RGB; preserves HDR only if user uses float fields after.
        ImGui::ColorConvertHSVtoRGB(h, s, v, editCol[0], editCol[1], editCol[2]);
    }

    ImGui::Spacing();
    if (Ui::VisualSlider("##huevis", &h, ImVec2(svW, stripH), Ui::VisualSliderSkin::HueStrip, nullptr, "Hue")) {
        ImGui::ColorConvertHSVtoRGB(h, s, v, editCol[0], editCol[1], editCol[2]);
    }
    ImGui::Spacing();
    {
        float aDisp = std::clamp(editCol[3], 0.f, 1.f);
        float rgbDisp[4] = {
            std::clamp(editCol[0], 0.f, 1.f),
            std::clamp(editCol[1], 0.f, 1.f),
            std::clamp(editCol[2], 0.f, 1.f),
            aDisp
        };
        if (Ui::VisualSlider("##alphavis", &aDisp, ImVec2(svW, stripH),
                Ui::VisualSliderSkin::OpacityChecker, rgbDisp, "Opacity / Alpha")) {
            editCol[3] = aDisp;
        }
    }

    // ---- Exact color control: RGB 0..255 + HEX (seam matching across texture sets) ----
    {
        ImGui::Spacing();
        auto toU8 = [](float v) -> int {
            return (int)std::lround(std::clamp(v, 0.f, 1.f) * 255.f);
        };
        auto fromU8 = [](int v) -> float {
            return std::clamp(v, 0, 255) / 255.f;
        };

        int rgb[4] = { toU8(editCol[0]), toU8(editCol[1]), toU8(editCol[2]), toU8(editCol[3]) };
        ImGui::SetNextItemWidth(svW);
        if (ImGui::DragInt4("##rgb255", rgb, 1.f, 0, 255, "%d")) {
            editCol[0] = fromU8(rgb[0]);
            editCol[1] = fromU8(rgb[1]);
            editCol[2] = fromU8(rgb[2]);
            editCol[3] = fromU8(rgb[3]);
            // Refresh HSV from new RGB
            ImGui::ColorConvertRGBtoHSV(
                std::clamp(editCol[0], 0.f, 1.f),
                std::clamp(editCol[1], 0.f, 1.f),
                std::clamp(editCol[2], 0.f, 1.f), h, s, v);
        }
        if (ImGui::IsItemHovered())
            Ui::Tooltip("RGBA 0–255\nExact values for matching seams across texture sets");

        // HEX field — keep buffer in sync when color changes from picker/sliders
        static char s_HexBuf[16] = "#FFFFFF";
        static float s_LastHexCol[4] = { -1.f, -1.f, -1.f, -1.f };
        const bool colChanged =
            s_LastHexCol[0] != editCol[0] || s_LastHexCol[1] != editCol[1] ||
            s_LastHexCol[2] != editCol[2] || s_LastHexCol[3] != editCol[3];
        if (colChanged && !ImGui::IsItemActive()) {
            // Don't overwrite while user is typing in HEX field (checked below).
        }
        // Rebuild HEX when color changed and HEX input is not focused
        bool hexFocused = false;
        {
            // Format without alpha if fully opaque (shorter, PS-like); with AA if needed
            int r = toU8(editCol[0]), g = toU8(editCol[1]), b = toU8(editCol[2]), a = toU8(editCol[3]);
            // Defer write until we know if ##hex is active — use previous frame flag
            static bool s_HexWasActive = false;
            if (colChanged && !s_HexWasActive) {
                if (a >= 255)
                    std::snprintf(s_HexBuf, sizeof(s_HexBuf), "#%02X%02X%02X", r, g, b);
                else
                    std::snprintf(s_HexBuf, sizeof(s_HexBuf), "#%02X%02X%02X%02X", r, g, b, a);
                s_LastHexCol[0] = editCol[0];
                s_LastHexCol[1] = editCol[1];
                s_LastHexCol[2] = editCol[2];
                s_LastHexCol[3] = editCol[3];
            }

            ImGui::SetNextItemWidth(std::max(90.f, svW - 70.f));
            // Allow '#' + hex digits (CharsHexadecimal would block '#').
            if (ImGui::InputText("##hex", s_HexBuf, sizeof(s_HexBuf),
                    ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_AutoSelectAll)) {
                // Parse #RGB #RRGGBB #RRGGBBAA (optional leading #)
                char raw[16] = {};
                const char* p = s_HexBuf;
                if (*p == '#') ++p;
                size_t n = 0;
                while (p[n] && n < 8) {
                    char c = p[n];
                    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))
                        raw[n++] = c;
                    else
                        break;
                }
                raw[n] = 0;
                auto hexNibble = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    return 0;
                };
                auto hexByte = [&](const char* s) -> int {
                    return (hexNibble(s[0]) << 4) | hexNibble(s[1]);
                };
                if (n == 3 || n == 4) {
                    // #RGB or #RGBA short form
                    int rr = hexNibble(raw[0]) * 17;
                    int gg = hexNibble(raw[1]) * 17;
                    int bb = hexNibble(raw[2]) * 17;
                    int aa = (n == 4) ? hexNibble(raw[3]) * 17 : 255;
                    editCol[0] = fromU8(rr);
                    editCol[1] = fromU8(gg);
                    editCol[2] = fromU8(bb);
                    editCol[3] = fromU8(aa);
                } else if (n == 6 || n == 8) {
                    editCol[0] = fromU8(hexByte(raw + 0));
                    editCol[1] = fromU8(hexByte(raw + 2));
                    editCol[2] = fromU8(hexByte(raw + 4));
                    editCol[3] = (n == 8) ? fromU8(hexByte(raw + 6)) : editCol[3];
                    if (n == 6) { /* keep existing alpha */ }
                }
                ImGui::ColorConvertRGBtoHSV(
                    std::clamp(editCol[0], 0.f, 1.f),
                    std::clamp(editCol[1], 0.f, 1.f),
                    std::clamp(editCol[2], 0.f, 1.f), h, s, v);
                s_LastHexCol[0] = editCol[0];
                s_LastHexCol[1] = editCol[1];
                s_LastHexCol[2] = editCol[2];
                s_LastHexCol[3] = editCol[3];
            }
            s_HexWasActive = ImGui::IsItemActive();
            hexFocused = s_HexWasActive;
            if (ImGui::IsItemHovered())
                Ui::Tooltip("HEX color\n#RGB  #RRGGBB  #RRGGBBAA\nMatch exact seam colors across sets");

            ImGui::SameLine(0, 6);
            if (ImGui::SmallButton("Copy##hex")) {
                ImGui::SetClipboardText(s_HexBuf);
            }
            if (ImGui::IsItemHovered()) Ui::Tooltip("Copy HEX to clipboard");
            (void)hexFocused;
        }
    }

    if (floatDoc) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.35f, 0.85f, 1.0f, 1.0f), "Float color (no 0..1 clamp)");
        ImGui::SetNextItemWidth(svW);
        // v_min == v_max → ImGui does not clamp (HDR / height values).
        ImGui::DragFloat4("##hdr_rgba", editCol, 0.01f, 0.f, 0.f, "%.4f");
        if (ImGui::IsItemHovered())
            Ui::Tooltip("RGBA linear values for F16/F32 documents.\nHeight maps often use R only with values outside 0..1.");
        if (ImGui::SmallButton("Mono R→RGB")) {
            editCol[1] = editCol[0];
            editCol[2] = editCol[0];
        }
        if (ImGui::IsItemHovered()) Ui::Tooltip("Copy R into G and B (height visualization)");
        ImGui::SameLine();
        if (ImGui::SmallButton("R-only paint")) {
            brush.writeR = true; brush.writeG = false;
            brush.writeB = false; brush.writeA = false;
        }
        if (ImGui::IsItemHovered()) Ui::Tooltip("Channel mask: write R only (height channel)");
    }

    ImGui::Spacing();
    // Primary (front) + Secondary (offset) like PS
    {
        auto clampCol = [](const float* c) {
            return ImVec4(std::clamp(c[0], 0.f, 1.f), std::clamp(c[1], 0.f, 1.f),
                          std::clamp(c[2], 0.f, 1.f), std::clamp(c[3], 0.f, 1.f));
        };
        ImVec2 base = ImGui::GetCursorScreenPos();
        // Secondary drawn first (behind / offset)
        ImGui::SetCursorScreenPos(ImVec2(base.x + 14.f, base.y + 14.f));
        if (ImGui::ColorButton("##sec", clampCol(g_SecondaryColor),
                ImGuiColorEditFlags_AlphaPreview | (s_EditSecondary ? ImGuiColorEditFlags_None : 0), ImVec2(28, 28))) {
            s_EditSecondary = true;
        }
        if (ImGui::IsItemHovered()) Ui::Tooltip("Secondary\nClick to edit · X: swap");

        ImGui::SetCursorScreenPos(base);
        if (ImGui::ColorButton("##pri", clampCol(brush.color),
                ImGuiColorEditFlags_AlphaPreview, ImVec2(36, 36))) {
            s_EditSecondary = false;
        }
        if (ImGui::IsItemHovered()) Ui::Tooltip(floatDoc
            ? "Primary (display clamped 0..1)\nUse float RGBA fields for HDR values"
            : "Primary\nClick to edit · HEX/RGB fields for exact match");

        ImGui::SetCursorScreenPos(ImVec2(base.x + 56.f, base.y + 8.f));
        if (ImGui::SmallButton("X##sw")) {
            std::swap(brush.color[0], g_SecondaryColor[0]);
            std::swap(brush.color[1], g_SecondaryColor[1]);
            std::swap(brush.color[2], g_SecondaryColor[2]);
            std::swap(brush.color[3], g_SecondaryColor[3]);
            g_ColorSwapPending = true;
        }
        if (ImGui::IsItemHovered()) Ui::Tooltip("Swap primary ↔ secondary (X)");
        ImGui::SameLine();
        ImGui::TextDisabled(s_EditSecondary ? "Editing secondary" : "Editing primary");
        ImGui::Dummy(ImVec2(1, 28));
    }

    ImGui::Spacing();
    static const ImVec4 paletteColors[] = {
        ImVec4(0,0,0,1), ImVec4(1,1,1,1), ImVec4(0.5f,0.5f,0.5f,1), ImVec4(0.75f,0.75f,0.75f,1),
        ImVec4(1,0,0,1), ImVec4(1,1,0,1), ImVec4(0,1,0,1), ImVec4(0,1,1,1),
        ImVec4(0,0,1,1), ImVec4(1,0,1,1), ImVec4(0.5f,0,0,1), ImVec4(0.5f,0.5f,0,1),
        ImVec4(0,0.5f,0,1), ImVec4(0,0.5f,0.5f,1), ImVec4(0,0,0.5f,1), ImVec4(0.5f,0,0.5f,1)
    };
    float cell = 18.f;
    int perRow = std::max(4, (int)(ImGui::GetContentRegionAvail().x / (cell + 4.f)));
    for (int i = 0; i < IM_ARRAYSIZE(paletteColors); ++i) {
        ImGui::PushID(i);
        if (i > 0 && (i % perRow) != 0) ImGui::SameLine(0, 3);
        if (ImGui::ColorButton("##pal", paletteColors[i], ImGuiColorEditFlags_NoTooltip, ImVec2(cell, cell))) {
            editCol[0] = paletteColors[i].x;
            editCol[1] = paletteColors[i].y;
            editCol[2] = paletteColors[i].z;
            editCol[3] = paletteColors[i].w;
        }
        ImGui::PopID();
    }

    Ui::EndDockPanel();
    }
}

} // namespace UI
