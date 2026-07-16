#include "OperatorRegistry.h"
#include "../KeymapManager.h"
#include <imgui.h>

namespace core::ops {

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

OperatorResult OperatorRegistry::Invoke(std::string_view id) {
    auto& ctx = AppContext::Get();
    const ActionDef* def = ActionCatalog::Find(id);
    ActionScope scope = def ? def->scope : ActionScope::Document;

    if (!ctx.Allows(scope)) {
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
