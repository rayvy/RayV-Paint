#include "ActionCatalog.h"
#include "../KeymapManager.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstring>
#include <iterator>

namespace core::ops {

// ---------------------------------------------------------------------------
// Catalog table — add new actions here only.
// key=0 means unbound by default.
// ---------------------------------------------------------------------------
static const ActionDef kActions[] = {
    // --- Edit ---
    { "Undo", "Undo", ActionCategory::Edit, ActionScope::EditHistory, ActionRole::Direct,
      nullptr, GLFW_KEY_Z, true, false, false, true, nullptr },
    { "Redo", "Redo", ActionCategory::Edit, ActionScope::EditHistory, ActionRole::Direct,
      nullptr, GLFW_KEY_Y, true, false, false, true, nullptr },
    { "Copy", "Copy", ActionCategory::Edit, ActionScope::Document, ActionRole::Direct,
      nullptr, GLFW_KEY_C, true, false, false, true, nullptr },
    { "CopyLayers", "Copy Layers", ActionCategory::Edit, ActionScope::Document, ActionRole::Direct,
      nullptr, GLFW_KEY_C, true, true, false, true, "Ctrl+Shift+C" },
    { "Paste", "Paste", ActionCategory::Edit, ActionScope::Document, ActionRole::Direct,
      nullptr, GLFW_KEY_V, true, false, false, true, nullptr },
    { "PasteAsNewLayer", "Paste as New Layer", ActionCategory::Edit, ActionScope::Document, ActionRole::Direct,
      nullptr, GLFW_KEY_V, true, true, false, true, nullptr },
    { "DuplicateLayer", "Duplicate Layer", ActionCategory::Edit, ActionScope::Document, ActionRole::Direct,
      nullptr, GLFW_KEY_J, true, false, false, true, nullptr },
    { "FillSecondary", "Fill with Secondary Color", ActionCategory::Edit, ActionScope::Document, ActionRole::Direct,
      nullptr, GLFW_KEY_BACKSPACE, false, false, false, true,
      "Blocked while typing or File Explorer is open" },
    { "DeleteContent", "Delete Content", ActionCategory::Edit, ActionScope::Document, ActionRole::Direct,
      nullptr, GLFW_KEY_DELETE, false, false, false, true, nullptr },

    // --- File ---
    { "NewProject", "New Project", ActionCategory::File, ActionScope::Document, ActionRole::Direct,
      nullptr, GLFW_KEY_N, true, false, false, true, nullptr },
    { "OpenProject", "Open Project", ActionCategory::File, ActionScope::Document, ActionRole::Direct,
      nullptr, GLFW_KEY_O, true, false, false, true, nullptr },
    { "SaveProject", "Save Project", ActionCategory::File, ActionScope::Document, ActionRole::Direct,
      nullptr, GLFW_KEY_S, true, false, false, true, nullptr },
    { "QuickExport", "Quick Export", ActionCategory::File, ActionScope::Document, ActionRole::Direct,
      nullptr, GLFW_KEY_E, true, false, false, true, nullptr },
    { "AdvancedExport", "Advanced Export", ActionCategory::File, ActionScope::Document, ActionRole::Direct,
      nullptr, GLFW_KEY_E, true, true, false, true, nullptr },

    // --- Tools (direct) ---
    { "BrushTool", "Brush Tool", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::Direct,
      nullptr, GLFW_KEY_B, false, false, false, true, nullptr },
    { "EraserTool", "Eraser Tool", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::Direct,
      nullptr, GLFW_KEY_E, false, false, false, true, nullptr },
    { "BrushPopup", "Brush Preset Popup", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::Direct,
      nullptr, 0, false, false, false, true, "Also via RMB on viewport" },
    { "PanTool", "Hand / Pan Tool", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::Direct,
      nullptr, GLFW_KEY_H, false, false, false, true, nullptr },
    { "RotateTool", "Rotate View (Hand)", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::Direct,
      nullptr, GLFW_KEY_R, false, false, false, true, "Activates Hand tool" },
    { "TransformTool", "Move Tool", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::Direct,
      nullptr, GLFW_KEY_V, false, false, false, true, nullptr },
    { "FreeTransform", "Free Transform", ActionCategory::Tool, ActionScope::Document, ActionRole::Direct,
      nullptr, GLFW_KEY_T, true, false, false, true, nullptr },
    { "BucketFillTool", "Bucket Fill", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::Direct,
      nullptr, GLFW_KEY_F, false, false, false, true, nullptr },
    { "GradientTool", "Gradient Tool", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::Direct,
      nullptr, GLFW_KEY_G, false, false, false, true, nullptr },
    { "PipetteTool", "Eyedropper", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::Direct,
      nullptr, GLFW_KEY_I, false, false, false, true, nullptr },
    { "SmudgeTool", "Smudge Tool", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::Direct,
      nullptr, GLFW_KEY_Y, false, false, false, true, nullptr },
    { "BlurTool", "Blur Tool", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::Direct,
      nullptr, 0, false, false, false, true, nullptr },
    { "StampTool", "Clone Stamp", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::Direct,
      nullptr, 0, false, false, false, true, nullptr },

    // --- Vector tools ---
    { "VectorSelectTool", "Vector Select", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::Direct,
      nullptr, GLFW_KEY_A, false, false, false, true, "Select / move vector shapes" },
    { "VectorEditTool", "Vector Edit", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::Direct,
      nullptr, 0, false, false, false, true, "Edit path nodes" },
    { "VectorPenTool", "Vector Pen", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::Direct,
      nullptr, GLFW_KEY_P, false, false, false, true, "Bezier pen" },
    { "VectorRectTool", "Vector Rectangle", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::Direct,
      nullptr, 0, false, false, false, true, nullptr },
    { "VectorEllipseTool", "Vector Ellipse", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::Direct,
      nullptr, 0, false, false, false, true, nullptr },
    { "VectorLineTool", "Vector Line", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::Direct,
      nullptr, 0, false, false, false, true, nullptr },
    { "VectorFreehandTool", "Vector Freehand", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::Direct,
      nullptr, 0, false, false, false, true, "Draw freehand path (smoothed)" },
    { "VectorPolygonTool", "Vector Polygon", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::Direct,
      nullptr, 0, false, false, false, true, "Click vertices · Enter finish" },

    // --- Tool groups ---
    { "SelectToolGroup", "Select Tools (cycle)", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::ToolGroup,
      nullptr, GLFW_KEY_S, false, false, false, true, "Cycles Rect / Ellipse" },
    { "LassoToolGroup", "Lasso Tools (cycle)", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::ToolGroup,
      nullptr, GLFW_KEY_L, false, false, false, true, "Cycles Freehand / Polygonal" },
    { "WandToolGroup", "Wand Tools (cycle)", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::ToolGroup,
      nullptr, GLFW_KEY_W, false, false, false, true, "Cycles Wand / Smart / Quick" },

    // --- Group members (default unbound; activated via group key or toolbar) ---
    { "RectSelectTool", "Rectangular Select", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::GroupMember,
      "SelectToolGroup", 0, false, false, false, true, "Default: cycle with S" },
    { "EllipseSelectTool", "Elliptical Select", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::GroupMember,
      "SelectToolGroup", 0, false, false, false, true, "Default: cycle with S" },
    { "LassoSelectTool", "Freehand Lasso", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::GroupMember,
      "LassoToolGroup", 0, false, false, false, true, "Default: cycle with L" },
    { "PolygonalLassoTool", "Polygonal Lasso", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::GroupMember,
      "LassoToolGroup", 0, false, false, false, true, "Default: cycle with L" },
    { "MagicWandTool", "Magic Wand", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::GroupMember,
      "WandToolGroup", 0, false, false, false, true, "Default: cycle with W" },
    { "SmartSelectTool", "Smart Select", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::GroupMember,
      "WandToolGroup", 0, false, false, false, true, "Default: cycle with W" },
    { "QuickSelectTool", "Quick Select", ActionCategory::Tool, ActionScope::ToolSwitch, ActionRole::GroupMember,
      "WandToolGroup", 0, false, false, false, true, "Default: cycle with W" },

    // --- Selection ---
    { "SelectAll", "Select All", ActionCategory::Selection, ActionScope::Document, ActionRole::Direct,
      nullptr, GLFW_KEY_A, true, false, false, true, nullptr },
    { "Deselect", "Deselect", ActionCategory::Selection, ActionScope::Document, ActionRole::Direct,
      nullptr, GLFW_KEY_D, true, false, false, true, "Ctrl+D — clear selection" },
    { "InvertSelection", "Invert Selection", ActionCategory::Selection, ActionScope::Document, ActionRole::Direct,
      nullptr, GLFW_KEY_I, true, true, false, true, nullptr },
    { "CropToSelection", "Crop Canvas to Selection", ActionCategory::Selection, ActionScope::Document, ActionRole::Direct,
      nullptr, GLFW_KEY_X, true, false, false, true, nullptr },
    // --- Colors (edit) ---
    { "SwapColors", "Swap Primary / Secondary", ActionCategory::Edit, ActionScope::Document, ActionRole::Direct,
      nullptr, GLFW_KEY_X, false, false, false, true, "X — swap brush primary/secondary" },

    // --- Image ---
    { "InvertColors", "Invert Colors", ActionCategory::Image, ActionScope::Document, ActionRole::Direct,
      nullptr, GLFW_KEY_I, true, false, false, true, nullptr },
    { "InvertAlpha", "Invert Alpha", ActionCategory::Image, ActionScope::Document, ActionRole::Direct,
      nullptr, GLFW_KEY_I, true, false, true, true, nullptr },
    { "AdjustHSV", "Hue / Saturation…", ActionCategory::Image, ActionScope::Document, ActionRole::Direct,
      nullptr, GLFW_KEY_U, true, false, false, true, nullptr },
    { "AdjustCurves", "Curves…", ActionCategory::Image, ActionScope::Document, ActionRole::Direct,
      nullptr, GLFW_KEY_M, true, false, false, true, nullptr },
    { "AdjustBlur", "Blur…", ActionCategory::Image, ActionScope::Document, ActionRole::Direct,
      nullptr, GLFW_KEY_B, true, true, false, true, nullptr },
    { "AdjustNoise", "Add Noise…", ActionCategory::Image, ActionScope::Document, ActionRole::Direct,
      nullptr, 0, false, false, false, true, "Unbound by default — assign in Keybindings" },
    { "ContentAwareFill", "Content-Aware Fill", ActionCategory::Image, ActionScope::Document, ActionRole::Direct,
      nullptr, 0, false, false, false, true, nullptr },
    { "PerspectiveWarp", "Perspective Warp", ActionCategory::Image, ActionScope::Document, ActionRole::Direct,
      nullptr, GLFW_KEY_P, true, true, false, true, nullptr },
    { "MeshWarp", "Mesh Warp", ActionCategory::Image, ActionScope::Document, ActionRole::Direct,
      nullptr, GLFW_KEY_W, true, true, false, true, nullptr },

    // --- View ---
    { "RefreshCanvas", "Refresh Canvas", ActionCategory::View, ActionScope::Global, ActionRole::Direct,
      nullptr, GLFW_KEY_F5, false, false, false, true, "F5 — force GPU re-upload" },
};

static std::vector<ActionDef> g_All;

static void EnsureInit() {
    if (!g_All.empty()) return;
    g_All.assign(std::begin(kActions), std::end(kActions));
}

const std::vector<ActionDef>& ActionCatalog::All() {
    EnsureInit();
    return g_All;
}

const ActionDef* ActionCatalog::Find(std::string_view id) {
    EnsureInit();
    for (const auto& a : g_All) {
        if (id == a.id) return &a;
    }
    return nullptr;
}

const char* ActionCatalog::CategoryLabel(ActionCategory c) {
    switch (c) {
    case ActionCategory::Edit: return "Edit";
    case ActionCategory::File: return "File";
    case ActionCategory::Tool: return "Tools";
    case ActionCategory::Selection: return "Selection";
    case ActionCategory::Image: return "Image";
    case ActionCategory::View: return "View";
    case ActionCategory::Debug: return "Debug";
    default: return "Other";
    }
}

KeyCombination ActionCatalog::DefaultChord(const ActionDef& def) {
    KeyCombination c;
    c.key = def.defaultKey;
    c.scancode = -1;
    c.ctrl = def.defaultCtrl;
    c.shift = def.defaultShift;
    c.alt = def.defaultAlt;
    return c;
}

void ActionCatalog::ApplyDefaultsTo(std::unordered_map<std::string, KeyCombination>& bindings) {
    EnsureInit();
    for (const auto& a : g_All) {
        bindings[a.id] = DefaultChord(a);
    }
}

std::vector<const ActionDef*> ActionCatalog::ListForKeybindUi() {
    EnsureInit();
    std::vector<const ActionDef*> out;
    out.reserve(g_All.size());
    for (const auto& a : g_All) out.push_back(&a);
    std::stable_sort(out.begin(), out.end(), [](const ActionDef* a, const ActionDef* b) {
        if (a->category != b->category) return (int)a->category < (int)b->category;
        // Groups before members; members under their group
        if (a->role != b->role) {
            if (a->role == ActionRole::ToolGroup) return true;
            if (b->role == ActionRole::ToolGroup) return false;
            if (a->role == ActionRole::GroupMember && b->role != ActionRole::GroupMember) return false;
            if (b->role == ActionRole::GroupMember && a->role != ActionRole::GroupMember) return true;
        }
        if (a->role == ActionRole::GroupMember && b->role == ActionRole::GroupMember) {
            int cmp = std::strcmp(a->groupId ? a->groupId : "", b->groupId ? b->groupId : "");
            if (cmp != 0) return cmp < 0;
        }
        return std::strcmp(a->label, b->label) < 0;
    });
    return out;
}

bool ChordIsUnbound(const KeyCombination& combo) {
    return combo.key == 0 || combo.key == -1;
}

std::string FormatChord(const KeyCombination& combo) {
    if (ChordIsUnbound(combo)) return "—";
    std::string res;
    if (combo.ctrl) res += "Ctrl+";
    if (combo.shift) res += "Shift+";
    if (combo.alt) res += "Alt+";
    std::string kn = KeymapManager::GetKeyName(combo.key);
    if (kn == "Unknown" || kn.empty()) return "—";
    res += kn;
    return res;
}

} // namespace core::ops
