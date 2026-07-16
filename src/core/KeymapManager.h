#pragma once
#include <string>
#include <unordered_map>
#include <vector>

struct KeyCombination {
    int key = 0;       // GLFW virtual key code (e.g. GLFW_KEY_Z); 0 or -1 = unbound
    int scancode = -1; // Platform physical scancode (resolved via glfwGetKeyScancode)
    bool ctrl = false;
    bool shift = false;
    bool alt = false;

    // Display / serialize. Unbound → "None" (never "Unknown").
    std::string ToString() const;
    static KeyCombination FromString(const std::string& str);
    bool IsUnbound() const { return key == 0 || key == -1; }
};

// KeymapManager stores id → KeyCombination only.
// Action metadata (label, category, scope) lives in core::ops::ActionCatalog.
// Dispatch: ProcessKeyEvent sets triggers; main uses core::ops::TryConsumeAction
// so AppContext poll drains blocked triggers without executing.
class KeymapManager {
public:
    static KeymapManager& Get();

    void Initialize();
    bool Load(const std::string& path = "");
    bool Save(const std::string& path = "");

    // Process a key event. Returns true if an action was matched.
    bool ProcessKeyEvent(int key, int scancode, int action, int mods);

    // Binds a key combination to an action
    void BindAction(const std::string& actionName, const KeyCombination& combo);

    // Consumes a singular trigger flag. Returns true if the action was triggered since last check.
    bool ConsumeActionTrigger(const std::string& actionName);

    // Peek without consume (for multi-tool same-key cycle resolution).
    bool PeekActionTrigger(const std::string& actionName) const;
    void ClearActionTrigger(const std::string& actionName);
    void SetActionTrigger(const std::string& actionName);
    // All action ids with a pending press trigger this frame.
    std::vector<std::string> ListTriggeredActions() const;

    // Returns if the action is currently held down
    bool IsActionActive(const std::string& actionName) const;

    // Formatted shortcut label (e.g. "Ctrl+Z" or "—")
    std::string GetActionShortcutString(const std::string& actionName) const;

    const std::unordered_map<std::string, KeyCombination>& GetBindings() const { return m_Bindings; }

    // Resolve all bindings' scancodes based on their virtual keys
    void ResolveScancodes();

    // Helper static lists for rebound GUI
    static const std::vector<std::pair<std::string, int>>& GetKeyList();
    static std::string GetKeyName(int key);

private:
    KeymapManager() = default;
    ~KeymapManager() = default;

    std::unordered_map<std::string, KeyCombination> m_Bindings;
    std::unordered_map<std::string, bool> m_TriggeredActions;
    std::unordered_map<std::string, bool> m_HeldActions;
};
