#pragma once
// =============================================================================
// OperatorRegistry — thin execute façade on top of ActionCatalog + AppContext.
//
// RULES FOR FUTURE AGENTS:
// 1. ActionCatalog owns id / label / category / scope / default key.
// 2. OperatorRegistry owns optional execute() — one-liner into Canvas/UI/services.
//    ZERO paint math / file decode here.
// 3. Hotkeys: OperatorRegistry::DispatchKeymapFrame() after AppContext::UpdateFromFrame.
//    Do not grow main.cpp with new if (TryConsumeAction) chains for catalog ops.
// 4. Menus / buttons: core::ops::MenuAction(id) or Invoke(id) — same poll as hotkeys.
// 5. Register handlers in RegisterEditorOperators() (called once at startup).
// =============================================================================

#include "ActionCatalog.h"
#include "AppContext.h"
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace core::ops {

enum class OperatorResult : uint8_t {
    Finished = 0,
    Cancelled,
    PassThrough, // not handled (no exec registered)
    Blocked,     // poll failed
};

struct OperatorEntry {
    std::string id;
    std::function<OperatorResult()> execute; // may be empty → PassThrough
};

class OperatorRegistry {
public:
    static OperatorRegistry& Get();

    void Clear();
    void Register(std::string_view id, std::function<OperatorResult()> execute);

    const OperatorEntry* Find(std::string_view id) const;
    std::vector<const OperatorEntry*> List() const;

    // Poll AppContext + run execute (no keymap). For menus / Python later.
    OperatorResult Invoke(std::string_view id);

    // For each registered op: if keymap trigger pending → TryConsume + exec.
    // Call once per frame after AppContext::UpdateFromFrame.
    int DispatchKeymapFrame();

    // True if this id has an execute handler (catalog may list more ids).
    bool HasExecute(std::string_view id) const;

private:
    std::unordered_map<std::string, OperatorEntry> m_Ops;
};

// ImGui menu line: label+shortcut from catalog/keymap, Invoke on click.
// Returns true if the operator ran this frame.
bool MenuAction(std::string_view id, bool enabled = true);

// Toolbar/helper: same as Invoke, ignores keymap.
inline OperatorResult Invoke(std::string_view id) {
    return OperatorRegistry::Get().Invoke(id);
}

} // namespace core::ops
