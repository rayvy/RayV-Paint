#pragma once
// =============================================================================
// AppContext — per-frame snapshot of "who owns input / what is open".
//
// RULES FOR FUTURE AGENTS:
// 1. Before running any document-mutating hotkey or starting canvas paint from
//    input, call AppContext::Get().Allows(scope) / BlocksCanvasInteraction().
// 2. Do NOT gate only on ImGuiIO::WantTextInput — File Explorer, settings, and
//    blocking modals must set flags via UIState → UpdateFromUi (below).
// 3. Widgets that temporarily own keys (slider text edit, rebind listener)
//    MUST call NotifyUiKeyboardCapture() so document ops (Backspace fill) do
//    not steal keys.
// 4. Prefer reading AppContext over new globals like g_IsViewportHovered for
//    "should I run?" decisions. Filling context FROM existing flags is fine.
// =============================================================================

#include "ActionCatalog.h"
#include <string>
#include <vector>

struct ImGuiIO;

namespace UI {
struct UIState;
}

namespace core::ops {

enum class FocusRegion : uint8_t {
    Other = 0,
    Viewport,
    Layers,
    Channels,
    Modal,
    FileExplorer,
    TextField,
    ToolSettings,
};

struct AppContext {
    // --- Input ownership ---
    bool wantTextInput = false;
    bool wantCaptureKeyboard = false;
    bool wantCaptureMouse = false;
    bool uiKeyboardCapture = false;   // slider text / custom widget key eat
    bool rebindingHotkey = false;

    // --- Blocking chrome ---
    bool fileExplorerOpen = false;
    bool settingsOpen = false;
    bool blockingModalOpen = false;   // adjust ops, about, recovery, wizard…
    bool layerEffectsOpen = false;

    // --- Focus / hover (best-effort) ---
    FocusRegion focusRegion = FocusRegion::Other;
    FocusRegion hoverRegion = FocusRegion::Other;
    bool viewportHovered = false;
    bool layersHovered = false;

    // --- Document / tool (debug) ---
    bool hasDocument = false;
    int  canvasW = 0, canvasH = 0;
    int  activeLayerIndex = -1;
    bool hasSelection = false;
    std::string activeToolLabel;
    std::string lastBlockedAction;
    std::string lastBlockedReason;
    std::string lastInvokedAction;

    // --- Derived policy ---
    bool keyboardOwnedByText = false;     // text field or UI capture
    bool blocksDocumentOps = false;       // FE / modal / text / rebind
    bool blocksCanvasInteraction = false; // FE / blocking modal / settings

    // Call once at the start of each frame (clears one-shot UI capture).
    static void BeginFrame();

    // Widgets: call when handling keys locally (slider exact entry, etc.).
    static void NotifyUiKeyboardCapture();

    // After UI::RenderAll — build snapshot from ImGui + UIState + canvas facts.
    static void UpdateFromFrame(const ImGuiIO& io,
                                const UI::UIState& ui,
                                bool viewportHovered,
                                bool layersHovered,
                                bool hasDocument,
                                int canvasW, int canvasH,
                                int activeLayerIndex,
                                bool hasSelection,
                                const char* activeToolLabel);

    static AppContext& Get();
    static const AppContext& CGet() { return Get(); }

    // Scope gate used by hotkey dispatch.
    bool Allows(ActionScope scope) const;
    bool BlocksCanvasInteraction() const { return blocksCanvasInteraction; }

    // Consume path helper: records why poll failed (Context debug panel).
    bool PollAction(std::string_view actionId) const;
    void NoteInvoke(std::string_view actionId);
    void NoteBlocked(std::string_view actionId, std::string_view reason);

    // Human-readable dump for Context panel.
    void AppendDebugLines(std::vector<std::string>& out) const;

private:
    static bool s_uiKeyboardCaptureSticky;
};

// Drain trigger only if present; execute only if PollAction. Always consumes.
// Usage: if (TryConsumeAction("FillSecondary")) { ... }
bool TryConsumeAction(std::string_view actionId);

} // namespace core::ops
