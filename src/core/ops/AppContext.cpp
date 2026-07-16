#include "AppContext.h"
#include "../KeymapManager.h"
#include "../../ui/EditorPanels.h"
#include <imgui.h>
#include <sstream>

namespace core::ops {

bool AppContext::s_uiKeyboardCaptureSticky = false;

AppContext& AppContext::Get() {
    static AppContext s;
    return s;
}

void AppContext::BeginFrame() {
    s_uiKeyboardCaptureSticky = false;
}

void AppContext::NotifyUiKeyboardCapture() {
    s_uiKeyboardCaptureSticky = true;
    Get().uiKeyboardCapture = true;
}

void AppContext::UpdateFromFrame(const ImGuiIO& io,
                                 const UI::UIState& ui,
                                 bool viewportHovered,
                                 bool layersHovered,
                                 bool hasDocument,
                                 int canvasW, int canvasH,
                                 int activeLayerIndex,
                                 bool hasSelection,
                                 const char* activeToolLabel) {
    auto& c = Get();
    c.wantTextInput = io.WantTextInput;
    c.wantCaptureKeyboard = io.WantCaptureKeyboard;
    c.wantCaptureMouse = io.WantCaptureMouse;
    c.uiKeyboardCapture = s_uiKeyboardCaptureSticky;
    c.rebindingHotkey = ui.listeningForKey;

    c.fileExplorerOpen = ui.fileExplorer.open;
    // openSettingsModal is a one-shot OpenPopup trigger — query live popup state.
    c.settingsOpen = ImGui::IsPopupOpen("Settings");
    c.layerEffectsOpen = ui.showLayerEffects;

    c.blockingModalOpen =
        ui.openAboutModal ||
        ui.openNewProjectWizard ||
        ui.showProjectSetup ||
        ImGui::IsPopupOpen("Blur##modal") ||
        ImGui::IsPopupOpen("HSV Adjust##modal") ||
        ImGui::IsPopupOpen("Add Noise##modal") ||
        ImGui::IsPopupOpen("Curves##modal") ||
        ImGui::IsPopupOpen("Canvas Edit") ||
        ImGui::IsPopupOpen("Restore Auto-Saved Session?") ||
        ImGui::IsPopupOpen("About RayV-Paint##about") ||
        ImGui::IsPopupOpen("Export DDS") ||
        ImGui::IsPopupOpen("Export Standard Image") ||
        ImGui::IsPopupOpen("Close Project?##dirty") ||
        ImGui::IsPopupOpen("Loading Document...");

    c.viewportHovered = viewportHovered;
    c.layersHovered = layersHovered;
    c.hasDocument = hasDocument;
    c.canvasW = canvasW;
    c.canvasH = canvasH;
    c.activeLayerIndex = activeLayerIndex;
    c.hasSelection = hasSelection;
    c.activeToolLabel = activeToolLabel ? activeToolLabel : "";

    c.keyboardOwnedByText = c.wantTextInput || c.uiKeyboardCapture;

    // Document ops: blocked when typing, FE open, settings, adjust modals, rebind listen.
    c.blocksDocumentOps =
        c.keyboardOwnedByText ||
        c.fileExplorerOpen ||
        c.settingsOpen ||
        c.blockingModalOpen ||
        c.rebindingHotkey;

    // Canvas pointer interaction: FE / settings / blocking modal own the stage.
    // Layer Effects is a dock panel — does NOT block paint (only text fields inside
    // will set WantTextInput / uiKeyboardCapture).
    c.blocksCanvasInteraction =
        c.fileExplorerOpen ||
        c.settingsOpen ||
        c.blockingModalOpen ||
        c.rebindingHotkey;

    if (c.fileExplorerOpen)
        c.focusRegion = FocusRegion::FileExplorer;
    else if (c.blockingModalOpen || c.settingsOpen)
        c.focusRegion = FocusRegion::Modal;
    else if (c.wantTextInput)
        c.focusRegion = FocusRegion::TextField;
    else if (viewportHovered)
        c.focusRegion = FocusRegion::Viewport;
    else if (layersHovered)
        c.focusRegion = FocusRegion::Layers;
    else
        c.focusRegion = FocusRegion::Other;

    c.hoverRegion = viewportHovered ? FocusRegion::Viewport
                   : layersHovered ? FocusRegion::Layers
                   : FocusRegion::Other;
}

bool AppContext::Allows(ActionScope scope) const {
    switch (scope) {
    case ActionScope::Global:
        // Only UI keyboard capture (slider text) hard-blocks Global.
        return !uiKeyboardCapture && !rebindingHotkey;
    case ActionScope::EditHistory:
        // Undo/Redo while typing path would be surprising; block text + FE + rebind.
        return !keyboardOwnedByText && !fileExplorerOpen && !rebindingHotkey && !settingsOpen;
    case ActionScope::Document:
    case ActionScope::ToolSwitch:
        return !blocksDocumentOps;
    default:
        return !blocksDocumentOps;
    }
}

bool AppContext::PollAction(std::string_view actionId) const {
    const ActionDef* def = ActionCatalog::Find(actionId);
    ActionScope scope = def ? def->scope : ActionScope::Document;
    if (Allows(scope)) return true;
    return false;
}

void AppContext::NoteInvoke(std::string_view actionId) {
    lastInvokedAction = std::string(actionId);
    lastBlockedAction.clear();
    lastBlockedReason.clear();
}

void AppContext::NoteBlocked(std::string_view actionId, std::string_view reason) {
    lastBlockedAction = std::string(actionId);
    lastBlockedReason = std::string(reason);
}

static const char* RegionName(FocusRegion r) {
    switch (r) {
    case FocusRegion::Viewport: return "Viewport";
    case FocusRegion::Layers: return "Layers";
    case FocusRegion::Channels: return "Channels";
    case FocusRegion::Modal: return "Modal";
    case FocusRegion::FileExplorer: return "FileExplorer";
    case FocusRegion::TextField: return "TextField";
    case FocusRegion::ToolSettings: return "ToolSettings";
    default: return "Other";
    }
}

void AppContext::AppendDebugLines(std::vector<std::string>& out) const {
    auto yn = [](bool b) { return b ? "yes" : "no"; };
    out.push_back(std::string("Focus: ") + RegionName(focusRegion) +
                  "  Hover: " + RegionName(hoverRegion));
    out.push_back(std::string("WantTextInput: ") + yn(wantTextInput) +
                  "  UiKeyCapture: " + yn(uiKeyboardCapture) +
                  "  Rebind: " + yn(rebindingHotkey));
    out.push_back(std::string("FileExplorer: ") + yn(fileExplorerOpen) +
                  "  Settings: " + yn(settingsOpen) +
                  "  BlockingModal: " + yn(blockingModalOpen));
    out.push_back(std::string("blocksDocumentOps: ") + yn(blocksDocumentOps) +
                  "  blocksCanvas: " + yn(blocksCanvasInteraction));
    out.push_back("Tool: " + activeToolLabel +
                  "  Doc: " + std::to_string(canvasW) + "x" + std::to_string(canvasH) +
                  "  Layer: " + std::to_string(activeLayerIndex) +
                  "  Sel: " + yn(hasSelection));
    if (!lastInvokedAction.empty())
        out.push_back("Last invoke: " + lastInvokedAction);
    if (!lastBlockedAction.empty())
        out.push_back("Last blocked: " + lastBlockedAction + " (" + lastBlockedReason + ")");
}

bool TryConsumeAction(std::string_view actionId) {
    std::string id(actionId);
    if (!KeymapManager::Get().ConsumeActionTrigger(id))
        return false;

    auto& ctx = AppContext::Get();
    const ActionDef* def = ActionCatalog::Find(actionId);
    ActionScope scope = def ? def->scope : ActionScope::Document;

    if (!ctx.Allows(scope)) {
        const char* why = "blocked by context";
        if (ctx.wantTextInput || ctx.uiKeyboardCapture) why = "text/UI owns keyboard";
        else if (ctx.fileExplorerOpen) why = "File Explorer open";
        else if (ctx.settingsOpen) why = "Settings open";
        else if (ctx.blockingModalOpen) why = "blocking modal open";
        else if (ctx.rebindingHotkey) why = "rebinding hotkey";
        ctx.NoteBlocked(actionId, why);
        return false;
    }
    ctx.NoteInvoke(actionId);
    return true;
}

} // namespace core::ops
