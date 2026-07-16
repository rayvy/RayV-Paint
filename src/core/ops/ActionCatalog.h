#pragma once
// =============================================================================
// ActionCatalog — single source of truth for bindable editor actions.
//
// RULES FOR FUTURE AGENTS:
// 1. New hotkey / menu command → add an entry HERE first (id, label, category,
//    default chord, scope, role). Do NOT invent free strings only in main.cpp.
// 2. KeymapManager defaults, Settings → Keybindings UI, and menus read labels
//    / categories from this catalog.
// 3. Runtime execute still lives in main (or thin operator façade later);
//    catalog owns identity + policy metadata, not paint math.
// 4. Prefer ActionScope so AppContext poll can block document ops during text
//    fields / File Explorer / modal UI without ad-hoc ifs.
// =============================================================================

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <cstdint>

struct KeyCombination;

namespace core::ops {

enum class ActionCategory : uint8_t {
    Edit = 0,
    File,
    Tool,
    Selection,
    Image,
    View,
    Debug,
    COUNT
};

// Who may run this action given AppContext.
enum class ActionScope : uint8_t {
    // Always eligible (rare). Prefer Document for canvas-mutating work.
    Global = 0,
    // Undo/redo — allowed unless pure text field owns keyboard (FE path typing).
    // Still blocked by explicit UI keyboard capture (slider text edit).
    EditHistory,
    // Needs document interaction free: blocked by text, FE, blocking modals.
    Document,
    // Tool switch — same as Document for input ownership.
    ToolSwitch,
};

enum class ActionRole : uint8_t {
    Direct = 0,   // one action ↔ one hotkey
    ToolGroup,    // cycles variants (e.g. LassoToolGroup → L)
    GroupMember,  // variant; default unbound; shown under group in UI
};

struct ActionDef {
    const char* id = nullptr;          // stable string (e.g. "FillSecondary")
    const char* label = nullptr;       // human ("Fill Secondary Color")
    ActionCategory category = ActionCategory::Edit;
    ActionScope scope = ActionScope::Document;
    ActionRole role = ActionRole::Direct;
    const char* groupId = nullptr;     // parent group id when GroupMember
    // Default chord: key=0 or key=-1 means unbound.
    int defaultKey = 0;
    bool defaultCtrl = false;
    bool defaultShift = false;
    bool defaultAlt = false;
    bool userRebindable = true;
    const char* note = nullptr;        // optional UI hint
};

class ActionCatalog {
public:
    static const std::vector<ActionDef>& All();
    static const ActionDef* Find(std::string_view id);
    static const char* CategoryLabel(ActionCategory c);
    static KeyCombination DefaultChord(const ActionDef& def);
    // Apply every catalog default into a bindings map (used by KeymapManager::Initialize).
    static void ApplyDefaultsTo(std::unordered_map<std::string, KeyCombination>& bindings);
    // Ordered ids for UI: category order, then label.
    static std::vector<const ActionDef*> ListForKeybindUi();
};

// Display helper: never "Unknown" for unbound.
std::string FormatChord(const KeyCombination& combo);
bool ChordIsUnbound(const KeyCombination& combo);

} // namespace core::ops
