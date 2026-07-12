#include "UiColorField.h"
#include "UiTooltip.h"
#include "../style/UiTokens.h"
#include <cstring>

namespace Ui {

bool ColorField(const char* id, float rgba[4], ColorFieldFlags flags,
                const char* label, bool* outPipetteClicked) {
    if (!rgba || !id) return false;
    if (outPipetteClicked) *outPipetteClicked = false;

    ImGuiColorEditFlags editFlags = ImGuiColorEditFlags_None;
    if (flags & ColorFieldFlags_NoAlpha)
        editFlags |= ImGuiColorEditFlags_NoAlpha;
    if (flags & ColorFieldFlags_AlphaBar)
        editFlags |= ImGuiColorEditFlags_AlphaBar;
    if (flags & ColorFieldFlags_NoInputs)
        editFlags |= ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreview;

    bool changed = false;

    // Full custom popup (Fill chips, places that need explicit pipette arm)
    if (flags & ColorFieldFlags_FullPicker) {
        ImGui::PushID(id);
        if (label && label[0]) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SameLine();
        }
        ImVec4 col(rgba[0], rgba[1], rgba[2], rgba[3]);
        if (ImGui::ColorButton("##sw", col,
                ImGuiColorEditFlags_AlphaPreview | ImGuiColorEditFlags_NoTooltip,
                ImVec2(22, 22))) {
            ImGui::OpenPopup("##colorpop");
        }
        if (ImGui::BeginPopup("##colorpop")) {
            float before[4] = { rgba[0], rgba[1], rgba[2], rgba[3] };
            ImGuiColorEditFlags pf = ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_NoSidePreview;
            if (flags & ColorFieldFlags_AlphaBar)
                pf |= ImGuiColorEditFlags_AlphaBar;
            if (flags & ColorFieldFlags_NoAlpha)
                pf |= ImGuiColorEditFlags_NoAlpha;
            ImGui::ColorPicker4("##picker", rgba, pf);
            if (ImGui::IsItemEdited()) {
                if (before[0] != rgba[0] || before[1] != rgba[1] ||
                    before[2] != rgba[2] || before[3] != rgba[3])
                    changed = true;
            }
            if (flags & ColorFieldFlags_Pipette) {
                if (ImGui::Button("Pipette", ImVec2(-1, 0))) {
                    if (outPipetteClicked) *outPipetteClicked = true;
                    // Close so next canvas click is not swallowed by ImGui
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::IsItemHovered())
                    Tooltip("Click canvas to sample (active Channels map)");
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
        return changed;
    }

    // Compact path: themed ColorEdit (swatch / optional inputs)
    if (label && label[0])
        changed = ImGui::ColorEdit4(id, rgba, editFlags) || changed;
    else {
        // id-only: still need a visible label for ImGui — use ##-only id
        changed = ImGui::ColorEdit4(id, rgba, editFlags) || changed;
    }
    // Optional pipette beside compact edit (arms without custom popup)
    if (flags & ColorFieldFlags_Pipette) {
        ImGui::SameLine(0, 4);
        ImGui::PushID(id);
        if (ImGui::SmallButton("Pip")) {
            if (outPipetteClicked) *outPipetteClicked = true;
        }
        if (ImGui::IsItemHovered())
            Tooltip("Pipette — click canvas to sample");
        ImGui::PopID();
    }
    (void)Tokens(); // keep theme touch-point for future chrome
    return changed;
}

} // namespace Ui
