#include "PropertiesPanel.h"
#include "../EditorPanels.h"
#include "../widgets/UiPanel.h"
#include "../widgets/UiDropdown.h"
#include "../widgets/UiPathField.h"
#include "../widgets/UiTooltip.h"
#include "../dialogs/Win32FileDialogs.h"
#include "../../Canvas.h"
#include "../../core/TileCache.h"
#include <imgui.h>
#include <cstring>
#include <string>
#include <algorithm>
#include <cctype>

namespace UI {

static bool DrawIccPresetCombo(Canvas& canvas, const char* label = "ICC Profile") {
    static const Canvas::IccPreset kPresets[] = {
        Canvas::IccPreset::None,
        Canvas::IccPreset::sRGB,
        Canvas::IccPreset::DisplayP3,
        Canvas::IccPreset::AdobeRGB,
        Canvas::IccPreset::Linear
    };
    int cur = 1;
    Canvas::IccPreset p = canvas.GetExportIccPreset();
    for (int i = 0; i < IM_ARRAYSIZE(kPresets); ++i) {
        if (kPresets[i] == p) { cur = i; break; }
    }
    const char* names[] = {
        Canvas::IccPresetName(Canvas::IccPreset::None),
        Canvas::IccPresetName(Canvas::IccPreset::sRGB),
        Canvas::IccPresetName(Canvas::IccPreset::DisplayP3),
        Canvas::IccPresetName(Canvas::IccPreset::AdobeRGB),
        Canvas::IccPresetName(Canvas::IccPreset::Linear)
    };
    if (Ui::Combo("##icc_preset_prop", &cur, names, IM_ARRAYSIZE(names), label)) {
        canvas.SetExportIccPreset(kPresets[cur]);
        return true;
    }
    return false;
}

void DrawPropertiesPanel(UIState& state, Canvas& canvas) {
    if (!state.showProperties) return;

    Ui::BeginDockPanel("Properties", &state.showProperties);

    ImGui::Text("Project Properties:");
    int pType =
        (canvas.GetProjectType() == Canvas::ProjectType::Simple) ? 0 :
        (canvas.GetProjectType() == Canvas::ProjectType::AdvancedModMode) ? 2 : 1;
    const char* pTypeNames[] = {
        "Simple Project",
        "Advanced Project (.rayp)",
        "Advanced Mod Mode (.rayp)"
    };
    if (Ui::Combo("##cmb_pType", &pType, pTypeNames, IM_ARRAYSIZE(pTypeNames), "Project Type")) {
        if (pType == 0) canvas.SetProjectType(Canvas::ProjectType::Simple);
        else if (pType == 2) canvas.SetProjectType(Canvas::ProjectType::AdvancedModMode);
        else canvas.SetProjectType(Canvas::ProjectType::Advanced);
    }

    ImGui::Text("Document Bit Depth:");
    int bd = (int)canvas.GetDocumentBitDepth();
    const char* bdNames[] = {
        "8-bit (U8) — default / diffuse",
        "16-bit float (F16) — HDR mid",
        "32-bit float (F32) — height / full float"
    };
    if (Ui::Combo("##cmb_bitDepth", &bd, bdNames, IM_ARRAYSIZE(bdNames),
            "Working space for paint storage. Export format stays free.")) {
        canvas.SetDocumentBitDepth(static_cast<Canvas::DocumentBitDepth>(bd));
    }
    ImGui::TextDisabled("Canvas %d x %d · storage %d B/px",
        canvas.GetWidth(), canvas.GetHeight(),
        BytesPerPixel(Canvas::FormatForBitDepth(canvas.GetDocumentBitDepth())));

    char propProjPath[512] = "";
    std::strncpy(propProjPath, canvas.GetCurrentProjectFilePath().c_str(), sizeof(propProjPath));
    if (Ui::PathField("##projpath", "Project Path", propProjPath, sizeof(propProjPath),
            Ui::ShowOpenFile, "RayP Projects (*.rayp)\0*.rayp\0All Files (*.*)\0*.*\0")) {
        canvas.SetCurrentProjectFilePath(propProjPath);
    }

    if (canvas.GetProjectType() == Canvas::ProjectType::AdvancedModMode) {
        ImGui::NewLine();
        ImGui::Separator();
        ImGui::Text("Advanced Mod");
        if (ImGui::Button("Mod Setup…##prop_mod"))
            state.showModSetup = true;
        ImGui::SameLine();
        if (ImGui::Button("3D Preview##prop_3d")) {
            state.showPreview3D = true;
            state.preview3DNeedReload = true;
        }
        if (canvas.IsModParseOk())
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.f), "INI parsed OK");
        else
            ImGui::TextDisabled("INI not applied — open Mod Setup");
    }

    ImGui::NewLine();
    ImGui::Separator();
    ImGui::Text("Project Output Format (DDS ↔ PNG):");
    ImGui::TextDisabled("Hard container switch — batch + quick export both use this.");

    char propExportPath[512] = "";
    std::strncpy(propExportPath, canvas.GetExportPath().c_str(), sizeof(propExportPath));

    int outFmt = (canvas.GetExportContainer() == Canvas::ExportContainer::DDS) ? 1 : 0;
    const char* outFmtNames[] = { "PNG / Standard image", "DDS (compressed)" };
    if (Ui::Combo("##cmb_outFmt", &outFmt, outFmtNames, IM_ARRAYSIZE(outFmtNames), "Output Type")) {
        canvas.SetExportContainer(outFmt == 1
            ? Canvas::ExportContainer::DDS
            : Canvas::ExportContainer::PNG);
        std::string synced = canvas.GetExportPath();
        std::strncpy(propExportPath, synced.c_str(), sizeof(propExportPath) - 1);
        propExportPath[sizeof(propExportPath) - 1] = '\0';
    }

    if (Ui::PathField("##exppath", "Export Path", propExportPath, sizeof(propExportPath),
            Ui::ShowSaveFile, "PNG (*.png)\0*.png\0DDS (*.dds)\0*.dds\0All Files (*.*)\0*.*\0")
        || std::string(propExportPath) != canvas.GetExportPath()) {
        canvas.SetExportPath(propExportPath);
        std::string pathStr = propExportPath;
        size_t d = pathStr.find_last_of('.');
        if (d != std::string::npos) {
            std::string e = pathStr.substr(d + 1);
            std::transform(e.begin(), e.end(), e.begin(), ::tolower);
            if (e == "dds")
                canvas.SetExportContainer(Canvas::ExportContainer::DDS);
            else if (e == "png")
                canvas.SetExportContainer(Canvas::ExportContainer::PNG);
        }
        canvas.SyncExportPathExtension();
        std::string synced = canvas.GetExportPath();
        std::strncpy(propExportPath, synced.c_str(), sizeof(propExportPath) - 1);
        propExportPath[sizeof(propExportPath) - 1] = '\0';
    }

    if (canvas.GetExportContainer() == Canvas::ExportContainer::DDS) {
        static const char* formats[] = {
            "BC7 (sRGB, DX 11+)", "BC7 (Linear, DX 11+)",
            "BC3 (Linear, DXT5)", "BC1 (Linear, DXT1)",
            "BC5 (Linear, Unsigned)", "R8G8B8A8 (Linear, A8B8G8R8)",
            "RGBA16_FLOAT", "RGBA32_FLOAT", "R32 (Linear, Float)"
        };
        int currentFormatIdx = 0;
        std::string currentFmt = canvas.GetExportFormat();
        for (int i = 0; i < IM_ARRAYSIZE(formats); ++i) {
            if (currentFmt == formats[i]) currentFormatIdx = i;
        }
        if (Ui::Combo("##cmb_currentFormatIdx", &currentFormatIdx, formats, IM_ARRAYSIZE(formats), "DDS Preset")) {
            canvas.SetExportFormat(formats[currentFormatIdx]);
        }
        bool mips = canvas.GetExportGenerateMipMaps();
        if (ImGui::Checkbox("Mipmaps", &mips)) canvas.SetExportGenerateMipMaps(mips);
        if (mips) {
            const char* filters[] = { "Point", "Box", "Linear", "Cubic" };
            int currentFilterIdx = 3;
            std::string currentFilter = canvas.GetExportMipFilter();
            for (int i = 0; i < IM_ARRAYSIZE(filters); ++i) {
                if (currentFilter == filters[i]) currentFilterIdx = i;
            }
            if (Ui::Combo("##cmb_currentFilterIdx", &currentFilterIdx, filters, IM_ARRAYSIZE(filters), "Mip Filter")) {
                canvas.SetExportMipFilter(filters[currentFilterIdx]);
            }
        }
        const char* speeds[] = { "Fast", "Medium", "Slow", "Best" };
        int si = 1;
        std::string cs = canvas.GetExportCompressionSpeed();
        for (int i = 0; i < 4; ++i) if (cs == speeds[i]) si = i;
        if (Ui::Combo("##cmb_compSpeed", &si, speeds, 4, "Quality"))
            canvas.SetExportCompressionSpeed(speeds[si]);
    } else {
        DrawIccPresetCombo(canvas, "ICC Profile");
    }

    if (ImGui::Button("Quick Export (project format)", ImVec2(-1, 0))) {
        state.openQuickExportTrigger = true;
    }
    if (ImGui::IsItemHovered()) Ui::Tooltip("Export using the path/format above (same as Ctrl+E)");

    Ui::EndDockPanel();
}

} // namespace UI
