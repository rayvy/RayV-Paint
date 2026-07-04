#pragma once
#include <string>
#include <unordered_map>
#include <vector>

struct KeyCombination {
    int key = 0;       // GLFW virtual key code (e.g. GLFW_KEY_Z)
    int scancode = -1; // Platform physical scancode (resolved via glfwGetKeyScancode)
    bool ctrl = false;
    bool shift = false;
    bool alt = false;

    std::string ToString() const;
    static KeyCombination FromString(const std::string& str);
};

class KeymapManager {
public:
    static KeymapManager& Get();

    void Initialize();
    bool Load(const std::string& path = "keymap.json");
    bool Save(const std::string& path = "keymap.json");

    // Process a key event. Returns true if an action was matched.
    bool ProcessKeyEvent(int key, int scancode, int action, int mods);

    // Binds a key combination to an action
    void BindAction(const std::string& actionName, const KeyCombination& combo);

    // Consumes a singular trigger flag. Returns true if the action was triggered since last check.
    bool ConsumeActionTrigger(const std::string& actionName);

    // Returns if the action is currently held down
    bool IsActionActive(const std::string& actionName) const;

    // Get formatted shortcut label (e.g. "Ctrl+Z")
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
