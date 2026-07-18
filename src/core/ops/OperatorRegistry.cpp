#include "OperatorRegistry.h"
#include "OperatorHost.h"
#include "ActionCatalog.h"
#include "../KeymapManager.h"
#include <imgui.h>
#include <map>
#include <algorithm>

namespace core::ops {

// When several ToolSwitch actions share one key (e.g. Fill+F Gradient), only one
// must fire — cycle through them instead of last-wins.
static void ResolveSharedKeyToolCycles() {
    auto& km = KeymapManager::Get();
    auto triggered = km.ListTriggeredActions();
    if (triggered.size() < 2) return;

    const auto& binds = km.GetBindings();
    // Group triggered actions by binding signature
    std::map<std::string, std::vector<std::string>> groups;
    for (const auto& id : triggered) {
        auto it = binds.find(id);
        if (it == binds.end() || it->second.IsUnbound()) continue;
        const ActionDef* def = ActionCatalog::Find(id);
        if (!def || def->scope != ActionScope::ToolSwitch) continue;
        groups[it->second.ToString()].push_back(id);
    }

    // Stable cycle order (matches toolbar kAllTools)
    static const char* kCycleOrder[] = {
        "BrushTool", "EraserTool", "StampTool", "BucketFillTool", "GradientTool",
        "SmudgeTool", "BlurTool", "PipetteTool",
        "RectSelectTool", "EllipseSelectTool", "SelectToolGroup",
        "LassoSelectTool", "PolygonalLassoTool", "LassoToolGroup",
        "MagicWandTool", "QuickSelectTool", "SmartSelectTool", "WandToolGroup",
        "TransformTool", "PanTool", "RotateTool",
        "VectorSelectTool", "VectorEditTool", "VectorPenTool",
        "VectorRectTool", "VectorEllipseTool", "VectorLineTool",
        "VectorFreehandTool", "VectorPolygonTool",
    };
    auto cycleRank = [&](const std::string& id) -> int {
        for (int i = 0; i < (int)(sizeof(kCycleOrder) / sizeof(kCycleOrder[0])); ++i)
            if (id == kCycleOrder[i]) return i;
        return 1000;
    };

    ActiveTool* cur = GetOperatorHost().activeTool;
    BrushSettings* brush = GetOperatorHost().brush;

    for (auto& kv : groups) {
        auto& ids = kv.second;
        if (ids.size() < 2) continue;
        std::sort(ids.begin(), ids.end(), [&](const std::string& a, const std::string& b) {
            return cycleRank(a) < cycleRank(b);
        });

        // Which candidate matches the currently active tool?
        int curIdx = -1;
        if (cur) {
            for (int i = 0; i < (int)ids.size(); ++i) {
                const std::string& id = ids[i];
                bool match = false;
                if (id == "BrushTool") match = (*cur == ActiveTool::Brush && brush && !brush->erase);
                else if (id == "EraserTool") match = (*cur == ActiveTool::Eraser) ||
                    (*cur == ActiveTool::Brush && brush && brush->erase);
                else if (id == "StampTool") match = (*cur == ActiveTool::Stamp);
                else if (id == "BucketFillTool") match = (*cur == ActiveTool::BucketFill);
                else if (id == "GradientTool") match = (*cur == ActiveTool::Gradient);
                else if (id == "SmudgeTool") match = (*cur == ActiveTool::Smudge);
                else if (id == "BlurTool") match = (*cur == ActiveTool::BlurTool);
                else if (id == "PipetteTool") match = (*cur == ActiveTool::Pipette);
                else if (id == "RectSelectTool") match = (*cur == ActiveTool::RectSelect);
                else if (id == "EllipseSelectTool") match = (*cur == ActiveTool::EllipseSelect);
                else if (id == "LassoSelectTool") match = (*cur == ActiveTool::LassoSelect);
                else if (id == "PolygonalLassoTool") match = (*cur == ActiveTool::PolygonalLasso);
                else if (id == "MagicWandTool") match = (*cur == ActiveTool::MagicWand);
                else if (id == "QuickSelectTool") match = (*cur == ActiveTool::QuickSelect);
                else if (id == "SmartSelectTool") match = (*cur == ActiveTool::SmartSelect);
                else if (id == "TransformTool") match = (*cur == ActiveTool::MovePixels);
                else if (id == "PanTool" || id == "RotateTool") match = (*cur == ActiveTool::Pan);
                else if (id == "SelectToolGroup")
                    match = (*cur == ActiveTool::RectSelect || *cur == ActiveTool::EllipseSelect);
                else if (id == "LassoToolGroup")
                    match = (*cur == ActiveTool::LassoSelect || *cur == ActiveTool::PolygonalLasso);
                else if (id == "WandToolGroup")
                    match = (*cur == ActiveTool::MagicWand || *cur == ActiveTool::QuickSelect ||
                             *cur == ActiveTool::SmartSelect);
                if (match) { curIdx = i; break; }
            }
        }
        // Next in cycle (or first if none currently active from this group)
        int next = (curIdx < 0) ? 0 : (curIdx + 1) % (int)ids.size();
        for (int i = 0; i < (int)ids.size(); ++i)
            km.ClearActionTrigger(ids[i]);
        km.SetActionTrigger(ids[next]);
    }
}

OperatorRegistry& OperatorRegistry::Get() {
    static OperatorRegistry s;
    return s;
}

void OperatorRegistry::Clear() {
    m_Ops.clear();
}

void OperatorRegistry::Register(std::string_view id, std::function<OperatorResult()> execute) {
    OperatorEntry e;
    e.id = std::string(id);
    e.execute = std::move(execute);
    m_Ops[e.id] = std::move(e);
}

const OperatorEntry* OperatorRegistry::Find(std::string_view id) const {
    auto it = m_Ops.find(std::string(id));
    if (it == m_Ops.end()) return nullptr;
    return &it->second;
}

std::vector<const OperatorEntry*> OperatorRegistry::List() const {
    std::vector<const OperatorEntry*> out;
    out.reserve(m_Ops.size());
    for (const auto& kv : m_Ops)
        out.push_back(&kv.second);
    return out;
}

bool OperatorRegistry::HasExecute(std::string_view id) const {
    auto it = m_Ops.find(std::string(id));
    return it != m_Ops.end() && static_cast<bool>(it->second.execute);
}

const char* OperatorRegistry::ResultName(OperatorResult r) {
    switch (r) {
    case OperatorResult::Finished: return "finished";
    case OperatorResult::Cancelled: return "cancelled";
    case OperatorResult::PassThrough: return "pass_through";
    case OperatorResult::Blocked: return "blocked";
    default: return "unknown";
    }
}

OperatorResult OperatorRegistry::Invoke(std::string_view id) {
    return InvokeForScript(id, /*force=*/false);
}

OperatorResult OperatorRegistry::InvokeForScript(std::string_view id, bool force) {
    auto& ctx = AppContext::Get();
    const ActionDef* def = ActionCatalog::Find(id);
    ActionScope scope = def ? def->scope : ActionScope::Document;

    if (!force && !ctx.Allows(scope)) {
        const char* why = "blocked by context";
        if (ctx.wantTextInput || ctx.uiKeyboardCapture) why = "text/UI owns keyboard";
        else if (ctx.fileExplorerOpen) why = "File Explorer open";
        else if (ctx.settingsOpen) why = "Settings open";
        else if (ctx.blockingModalOpen) why = "blocking modal open";
        else if (ctx.rebindingHotkey) why = "rebinding hotkey";
        ctx.NoteBlocked(id, why);
        return OperatorResult::Blocked;
    }

    auto it = m_Ops.find(std::string(id));
    if (it == m_Ops.end() || !it->second.execute) {
        return OperatorResult::PassThrough;
    }

    ctx.NoteInvoke(id);
    return it->second.execute();
}

int OperatorRegistry::DispatchKeymapFrame() {
    // Shared hotkeys among tools → cycle (Fill+F Gradient must not last-wins).
    ResolveSharedKeyToolCycles();

    int ran = 0;
    // Snapshot ids — exec must not re-enter registration mid-loop
    std::vector<std::string> ids;
    ids.reserve(m_Ops.size());
    for (const auto& kv : m_Ops)
        ids.push_back(kv.first);

    for (const auto& id : ids) {
        // TryConsumeAction: drain trigger + poll scope
        if (!TryConsumeAction(id))
            continue;
        auto it = m_Ops.find(id);
        if (it == m_Ops.end() || !it->second.execute)
            continue;
        // Poll already succeeded inside TryConsumeAction (NoteInvoke already called).
        it->second.execute();
        ++ran;
    }
    return ran;
}

bool MenuAction(std::string_view id, bool enabled) {
    const ActionDef* def = ActionCatalog::Find(id);
    const char* label = (def && def->label) ? def->label : id.data();
    std::string shortcut = KeymapManager::Get().GetActionShortcutString(std::string(id));
    const char* sc = (shortcut == "—" || shortcut == "None") ? nullptr : shortcut.c_str();

    // ImGui::MenuItem needs stable string for label; shortcut can be temporary.
    if (!ImGui::MenuItem(label, sc, false, enabled))
        return false;

    OperatorResult r = OperatorRegistry::Get().Invoke(id);
    return r == OperatorResult::Finished;
}

} // namespace core::ops
