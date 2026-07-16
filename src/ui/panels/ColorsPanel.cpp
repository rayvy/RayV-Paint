#include "ColorsPanel.h"
#include "../EditorPanels.h"
#include "../widgets/UiPanel.h"
#include "../widgets/UiTooltip.h"
#include "../widgets/UiVisualSlider.h"
#include "../style/UiTokens.h"
#include "../style/UiMotion.h"
#include "../../core/PaintEngine.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

extern float g_SecondaryColor[4];
extern bool g_ColorSwapPending;
extern float g_ColorSwapAnim;

namespace UI {

void DrawColorsPanel(UIState& state, Canvas& canvas, BrushSettings& brush) {
if (!state.showColors) return;

    Ui::BeginDockPanel("Colors", &state.showColors);
    Ui::ClampDockLeafBox(180.f, 420.f, 220.f, 900.f);

    static bool s_EditSecondary = false;
    float* editCol = s_EditSecondary ? g_SecondaryColor : brush.color;
    const bool floatDoc = (canvas.GetDocumentBitDepth() != Canvas::DocumentBitDepth::U8);

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float dispR = std::clamp(editCol[0], 0.f, 1.f);
    float dispG = std::clamp(editCol[1], 0.f, 1.f);
    float dispB = std::clamp(editCol[2], 0.f, 1.f);
    float h, s, v;
    ImGui::ColorConvertRGBtoHSV(dispR, dispG, dispB, h, s, v);

    float stripH = 20.f;
    float chipBlock = 52.f;
    float fieldsH = floatDoc ? 88.f : 56.f;
    float svW = std::max(80.f, avail.x - 4.f);
    // Fit without outer scroll: SV takes remainder
    float reserved = stripH * 2.f + chipBlock + fieldsH + 28.f;
    float svH = std::clamp(avail.y - reserved, 72.f, 280.f);

    ImVec2 sqPos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##svsq", ImVec2(svW, svH));
    bool svActive = ImGui::IsItemActive();
    bool svHovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto& tok = Ui::Tokens();

    // Smooth SV field: row-wise multi-color (not blocky 28×28 cells)
    const int rows = std::max(32, (int)(svH * 0.75f));
    for (int yi = 0; yi < rows; ++yi) {
        float t0 = (float)yi / (float)rows;
        float t1 = (float)(yi + 1) / (float)rows;
        float vv0 = 1.f - t0;
        float vv1 = 1.f - t1;
        float rL0, gL0, bL0, rR0, gR0, bR0;
        float rL1, gL1, bL1, rR1, gR1, bR1;
        ImGui::ColorConvertHSVtoRGB(h, 0.f, vv0, rL0, gL0, bL0);
        ImGui::ColorConvertHSVtoRGB(h, 1.f, vv0, rR0, gR0, bR0);
        ImGui::ColorConvertHSVtoRGB(h, 0.f, vv1, rL1, gL1, bL1);
        ImGui::ColorConvertHSVtoRGB(h, 1.f, vv1, rR1, gR1, bR1);
        ImVec2 a(sqPos.x, sqPos.y + t0 * svH);
        ImVec2 b(sqPos.x + svW, sqPos.y + t1 * svH);
        dl->AddRectFilledMultiColor(a, b,
            IM_COL32((int)(rL0*255),(int)(gL0*255),(int)(bL0*255),255),
            IM_COL32((int)(rR0*255),(int)(gR0*255),(int)(bR0*255),255),
            IM_COL32((int)(rR1*255),(int)(gR1*255),(int)(bR1*255),255),
            IM_COL32((int)(rL1*255),(int)(gL1*255),(int)(bL1*255),255));
    }
    dl->AddRect(sqPos, ImVec2(sqPos.x + svW, sqPos.y + svH), tok.ColU32(tok.strokeHairline), tok.rSm);
    ImVec2 cur(sqPos.x + s * svW, sqPos.y + (1.f - v) * svH);
    dl->AddCircle(cur, 6.f, IM_COL32(0,0,0,200), 16, 2.f);
    dl->AddCircle(cur, 6.f, IM_COL32(255,255,255,230), 16, 1.5f);

    if (svActive || (svHovered && ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
        ImVec2 mp = ImGui::GetIO().MousePos;
        s = std::clamp((mp.x - sqPos.x) / svW, 0.f, 1.f);
        v = std::clamp(1.f - (mp.y - sqPos.y) / svH, 0.f, 1.f);
        ImGui::ColorConvertHSVtoRGB(h, s, v, editCol[0], editCol[1], editCol[2]);
    }

    ImGui::Spacing();
    if (Ui::VisualSlider("##huevis", &h, ImVec2(svW, stripH), Ui::VisualSliderSkin::HueStrip, nullptr, "Hue")) {
        ImGui::ColorConvertHSVtoRGB(h, s, v, editCol[0], editCol[1], editCol[2]);
    }
    {
        float aDisp = std::clamp(editCol[3], 0.f, 1.f);
        float rgbDisp[4] = {
            std::clamp(editCol[0], 0.f, 1.f),
            std::clamp(editCol[1], 0.f, 1.f),
            std::clamp(editCol[2], 0.f, 1.f),
            aDisp
        };
        if (Ui::VisualSlider("##alphavis", &aDisp, ImVec2(svW, stripH),
                Ui::VisualSliderSkin::OpacityChecker, rgbDisp, "Opacity")) {
            editCol[3] = aDisp;
        }
    }

    // RGB + HEX compact
    {
        auto toU8 = [](float x) -> int {
            return (int)std::lround(std::clamp(x, 0.f, 1.f) * 255.f);
        };
        auto fromU8 = [](int x) -> float { return std::clamp(x, 0, 255) / 255.f; };
        int rgb[4] = { toU8(editCol[0]), toU8(editCol[1]), toU8(editCol[2]), toU8(editCol[3]) };
        ImGui::SetNextItemWidth(svW);
        if (ImGui::DragInt4("##rgb255", rgb, 1.f, 0, 255, "%d")) {
            editCol[0] = fromU8(rgb[0]);
            editCol[1] = fromU8(rgb[1]);
            editCol[2] = fromU8(rgb[2]);
            editCol[3] = fromU8(rgb[3]);
            ImGui::ColorConvertRGBtoHSV(
                std::clamp(editCol[0], 0.f, 1.f),
                std::clamp(editCol[1], 0.f, 1.f),
                std::clamp(editCol[2], 0.f, 1.f), h, s, v);
        }
        if (ImGui::IsItemHovered())
            Ui::Tooltip("RGBA 0–255");

        static char s_HexBuf[16] = "#FFFFFF";
        static float s_LastHexCol[4] = { -1.f, -1.f, -1.f, -1.f };
        static bool s_HexWasActive = false;
        const bool colChanged =
            s_LastHexCol[0] != editCol[0] || s_LastHexCol[1] != editCol[1] ||
            s_LastHexCol[2] != editCol[2] || s_LastHexCol[3] != editCol[3];
        int r = toU8(editCol[0]), g = toU8(editCol[1]), b = toU8(editCol[2]), a = toU8(editCol[3]);
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
        ImGui::SetNextItemWidth(std::max(90.f, svW - 52.f));
        if (ImGui::InputText("##hex", s_HexBuf, sizeof(s_HexBuf),
                ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_AutoSelectAll)) {
            char raw[16] = {};
            const char* p = s_HexBuf;
            if (*p == '#') ++p;
            size_t n = 0;
            while (p[n] && n < 8) {
                char c = p[n];
                if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))
                    raw[n++] = c;
                else break;
            }
            auto hexNibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return 0;
            };
            auto hexByte = [&](const char* s) { return (hexNibble(s[0]) << 4) | hexNibble(s[1]); };
            if (n == 3 || n == 4) {
                editCol[0] = fromU8(hexNibble(raw[0]) * 17);
                editCol[1] = fromU8(hexNibble(raw[1]) * 17);
                editCol[2] = fromU8(hexNibble(raw[2]) * 17);
                if (n == 4) editCol[3] = fromU8(hexNibble(raw[3]) * 17);
            } else if (n == 6 || n == 8) {
                editCol[0] = fromU8(hexByte(raw + 0));
                editCol[1] = fromU8(hexByte(raw + 2));
                editCol[2] = fromU8(hexByte(raw + 4));
                if (n == 8) editCol[3] = fromU8(hexByte(raw + 6));
            }
            ImGui::ColorConvertRGBtoHSV(
                std::clamp(editCol[0], 0.f, 1.f),
                std::clamp(editCol[1], 0.f, 1.f),
                std::clamp(editCol[2], 0.f, 1.f), h, s, v);
            s_LastHexCol[0] = editCol[0]; s_LastHexCol[1] = editCol[1];
            s_LastHexCol[2] = editCol[2]; s_LastHexCol[3] = editCol[3];
        }
        s_HexWasActive = ImGui::IsItemActive();
        ImGui::SameLine(0, 4);
        if (ImGui::SmallButton("##cpy")) ImGui::SetClipboardText(s_HexBuf);
        if (ImGui::IsItemHovered()) Ui::Tooltip("Copy HEX");
    }

    if (floatDoc) {
        ImGui::SetNextItemWidth(svW);
        ImGui::DragFloat4("##hdr_rgba", editCol, 0.01f, 0.f, 0.f, "%.3f");
        if (ImGui::IsItemHovered()) Ui::Tooltip("Float RGBA (HDR / height)");
    }

    // Primary / Secondary chips — same swap animation as toolbar (shared g_ColorSwapPending)
    {
        auto clampCol = [](const float* c) {
            return ImVec4(std::clamp(c[0], 0.f, 1.f), std::clamp(c[1], 0.f, 1.f),
                          std::clamp(c[2], 0.f, 1.f), std::clamp(c[3], 0.f, 1.f));
        };
        ImVec2 base = ImGui::GetCursorScreenPos();
        const float priSz = 36.f, secSz = 28.f;
        // Hit targets (stable)
        ImGui::SetCursorScreenPos(ImVec2(base.x + 14.f, base.y + 14.f));
        if (ImGui::InvisibleButton("##sec_hit", ImVec2(secSz, secSz)))
            s_EditSecondary = true;
        if (ImGui::IsItemHovered()) Ui::Tooltip("Secondary · click edit · X swap");
        bool secHov = ImGui::IsItemHovered();

        ImGui::SetCursorScreenPos(base);
        if (ImGui::InvisibleButton("##pri_hit", ImVec2(priSz, priSz)))
            s_EditSecondary = false;
        if (ImGui::IsItemHovered()) Ui::Tooltip("Primary · click edit · X swap");
        bool priHov = ImGui::IsItemHovered();

        // Drive shared animation (toolbar may also set pending)
        static Ui::AnimFloat s_swapT;
        if (g_ColorSwapPending) {
            s_swapT.Snap(1.f);
            s_swapT.SetTarget(0.f, Ui::Tokens().durMed * 1.15f, Ui::EaseKind::EaseOutCubic);
            // Don't clear pending here if toolbar also needs it — main/toolbar clears.
            // Colors panel: if we only draw, still animate from shared flag.
        }
        // Local mirror: when pending was consumed by toolbar same frame, still animate via g_ColorSwapAnim
        s_swapT.Update(Ui::DeltaTime());
        float useT = std::max(s_swapT.value, g_ColorSwapAnim);

        ImVec2 pPri = base;
        ImVec2 pSec(base.x + 14.f, base.y + 14.f);
        ImVec2 a0(pPri.x + (pSec.x - pPri.x) * useT, pPri.y + (pSec.y - pPri.y) * useT);
        ImVec2 b0(pSec.x + (pPri.x - pSec.x) * useT, pSec.y + (pPri.y - pSec.y) * useT);

        ImU32 colSec = ImGui::ColorConvertFloat4ToU32(clampCol(g_SecondaryColor));
        ImU32 colPri = ImGui::ColorConvertFloat4ToU32(clampCol(brush.color));
        dl->AddRectFilled(b0, ImVec2(b0.x + secSz, b0.y + secSz), colSec, tok.rSm);
        dl->AddRect(b0, ImVec2(b0.x + secSz, b0.y + secSz),
            secHov || s_EditSecondary ? tok.ColU32(tok.strokeActive) : tok.ColU32(tok.strokeHairline),
            tok.rSm, 0, 1.f);
        dl->AddRectFilled(a0, ImVec2(a0.x + priSz, a0.y + priSz), colPri, tok.rSm);
        dl->AddRect(a0, ImVec2(a0.x + priSz, a0.y + priSz),
            priHov || !s_EditSecondary ? tok.ColU32(tok.strokeActive) : tok.ColU32(tok.strokeHairline),
            tok.rSm, 0, 1.25f);

        ImGui::SetCursorScreenPos(ImVec2(base.x + 56.f, base.y + 8.f));
        if (ImGui::SmallButton("X##sw")) {
            std::swap(brush.color[0], g_SecondaryColor[0]);
            std::swap(brush.color[1], g_SecondaryColor[1]);
            std::swap(brush.color[2], g_SecondaryColor[2]);
            std::swap(brush.color[3], g_SecondaryColor[3]);
            g_ColorSwapPending = true;
            s_swapT.Snap(1.f);
            s_swapT.SetTarget(0.f, Ui::Tokens().durMed * 1.15f, Ui::EaseKind::EaseOutCubic);
        }
        if (ImGui::IsItemHovered()) Ui::Tooltip("Swap primary ↔ secondary (X)");
        ImGui::Dummy(ImVec2(1, chipBlock - 8.f));
    }

    Ui::EndDockPanel();
}

} // namespace UI
