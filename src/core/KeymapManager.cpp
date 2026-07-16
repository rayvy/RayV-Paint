#include "KeymapManager.h"
#include "ops/ActionCatalog.h"
#include "ConfigManager.h"
#include "Logger.h"
#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <algorithm>

using json = nlohmann::json;

// Key Name Mapping Table
static const std::vector<std::pair<std::string, int>> s_Keys = {
    {"A", GLFW_KEY_A}, {"B", GLFW_KEY_B}, {"C", GLFW_KEY_C}, {"D", GLFW_KEY_D},
    {"E", GLFW_KEY_E}, {"F", GLFW_KEY_F}, {"G", GLFW_KEY_G}, {"H", GLFW_KEY_H},
    {"I", GLFW_KEY_I}, {"J", GLFW_KEY_J}, {"K", GLFW_KEY_K}, {"L", GLFW_KEY_L},
    {"M", GLFW_KEY_M}, {"N", GLFW_KEY_N}, {"O", GLFW_KEY_O}, {"P", GLFW_KEY_P},
    {"Q", GLFW_KEY_Q}, {"R", GLFW_KEY_R}, {"S", GLFW_KEY_S}, {"T", GLFW_KEY_T},
    {"U", GLFW_KEY_U}, {"V", GLFW_KEY_V}, {"W", GLFW_KEY_W}, {"X", GLFW_KEY_X},
    {"Y", GLFW_KEY_Y}, {"Z", GLFW_KEY_Z},
    {"0", GLFW_KEY_0}, {"1", GLFW_KEY_1}, {"2", GLFW_KEY_2}, {"3", GLFW_KEY_3},
    {"4", GLFW_KEY_4}, {"5", GLFW_KEY_5}, {"6", GLFW_KEY_6}, {"7", GLFW_KEY_7},
    {"8", GLFW_KEY_8}, {"9", GLFW_KEY_9},
    {"F1", GLFW_KEY_F1}, {"F2", GLFW_KEY_F2}, {"F3", GLFW_KEY_F3}, {"F4", GLFW_KEY_F4},
    {"F5", GLFW_KEY_F5}, {"F6", GLFW_KEY_F6}, {"F7", GLFW_KEY_F7}, {"F8", GLFW_KEY_F8},
    {"F9", GLFW_KEY_F9}, {"F10", GLFW_KEY_F10}, {"F11", GLFW_KEY_F11}, {"F12", GLFW_KEY_F12},
    {"Space", GLFW_KEY_SPACE}, {"Enter", GLFW_KEY_ENTER}, {"Escape", GLFW_KEY_ESCAPE},
    {"Tab", GLFW_KEY_TAB}, {"Backspace", GLFW_KEY_BACKSPACE}, {"Insert", GLFW_KEY_INSERT},
    {"Delete", GLFW_KEY_DELETE}, {"Right", GLFW_KEY_RIGHT}, {"Left", GLFW_KEY_LEFT},
    {"Down", GLFW_KEY_DOWN}, {"Up", GLFW_KEY_UP},
    {"Comma", GLFW_KEY_COMMA}, {"Period", GLFW_KEY_PERIOD}, {"Slash", GLFW_KEY_SLASH},
    {"Semicolon", GLFW_KEY_SEMICOLON}, {"Equal", GLFW_KEY_EQUAL}, {"Minus", GLFW_KEY_MINUS},
    {"LeftBracket", GLFW_KEY_LEFT_BRACKET}, {"RightBracket", GLFW_KEY_RIGHT_BRACKET},
    {"Backslash", GLFW_KEY_BACKSLASH}, {"Grave", GLFW_KEY_GRAVE_ACCENT}
};

const std::vector<std::pair<std::string, int>>& KeymapManager::GetKeyList() {
    return s_Keys;
}

std::string KeymapManager::GetKeyName(int key) {
    if (key == 0 || key == -1) return "None";
    for (const auto& pair : s_Keys) {
        if (pair.second == key) return pair.first;
    }
    return "None";
}

KeyCombination KeyCombination::FromString(const std::string& str) {
    KeyCombination combo;
    // Unbound sentinels (catalog + old "Unknown" display)
    if (str.empty() || str == "None" || str == "—" || str == "-" || str == "Unknown" ||
        str == "none" || str == "unbound") {
        combo.key = 0;
        combo.scancode = -1;
        return combo;
    }

    std::stringstream ss(str);
    std::string token;
    std::vector<std::string> tokens;

    while (std::getline(ss, token, '+')) {
        tokens.push_back(token);
    }

    for (size_t i = 0; i < tokens.size(); ++i) {
        std::string t = tokens[i];
        t.erase(std::remove_if(t.begin(), t.end(), ::isspace), t.end());

        if (t == "Ctrl") combo.ctrl = true;
        else if (t == "Shift") combo.shift = true;
        else if (t == "Alt") combo.alt = true;
        else {
            for (const auto& pair : s_Keys) {
                if (pair.first == t) {
                    combo.key = pair.second;
                    break;
                }
            }
        }
    }
    if (combo.key == 0 && !combo.ctrl && !combo.shift && !combo.alt) {
        // "Ctrl+" alone or garbage → unbound
        combo.key = 0;
    }
    return combo;
}

std::string KeyCombination::ToString() const {
    if (IsUnbound()) return "None";
    std::string res;
    if (ctrl) res += "Ctrl+";
    if (shift) res += "Shift+";
    if (alt) res += "Alt+";
    std::string kn = KeymapManager::GetKeyName(key);
    if (kn == "None" || kn.empty()) return "None";
    res += kn;
    return res;
}

KeymapManager& KeymapManager::Get() {
    static KeymapManager instance;
    return instance;
}

void KeymapManager::Initialize() {
    // Single source of truth: ActionCatalog (not a hand-maintained parallel list).
    m_Bindings.clear();
    core::ops::ActionCatalog::ApplyDefaultsTo(m_Bindings);
    ResolveScancodes();
    Logger::Get().Info("KeymapManager: loaded " + std::to_string(m_Bindings.size()) +
                       " default actions from ActionCatalog");
}

void KeymapManager::ResolveScancodes() {
    for (auto& pair : m_Bindings) {
        if (!pair.second.IsUnbound() && pair.second.key != 0) {
            pair.second.scancode = glfwGetKeyScancode(pair.second.key);
        } else {
            pair.second.scancode = -1;
        }
    }
}

bool KeymapManager::Load(const std::string& path) {
    std::string filePath = path;
    if (filePath.empty()) {
        filePath = ConfigManager::GetUserSubdirectory("user") + "/keymap.json";
    }

    std::ifstream in(filePath);
    if (!in.is_open()) {
        Logger::Get().Warn("Failed to open keymap config: " + filePath + ". Using defaults.");
        return false;
    }

    try {
        json data;
        in >> data;
        for (auto& el : data.items()) {
            std::string actionName = el.key();
            std::string comboStr = el.value().get<std::string>();
            // Only apply known catalog / existing bindings; ignore orphans silently.
            if (m_Bindings.count(actionName) || core::ops::ActionCatalog::Find(actionName)) {
                m_Bindings[actionName] = KeyCombination::FromString(comboStr);
            }
        }
        ResolveScancodes();
        Logger::Get().Info("Loaded custom keymap configuration from " + filePath);
        return true;
    }
    catch (const std::exception& e) {
        Logger::Get().Error("Failed to parse keymap configuration: " + std::string(e.what()));
        return false;
    }
}

bool KeymapManager::Save(const std::string& path) {
    std::string filePath = path;
    if (filePath.empty()) {
        filePath = ConfigManager::GetUserSubdirectory("user") + "/keymap.json";
    }

    std::ofstream out(filePath);
    if (!out.is_open()) {
        Logger::Get().Error("Failed to open keymap config for saving: " + filePath);
        return false;
    }

    try {
        json data;
        for (const auto& pair : m_Bindings) {
            data[pair.first] = pair.second.ToString();
        }
        out << data.dump(4);
        Logger::Get().Info("Saved custom keymap configuration to " + filePath);
        return true;
    }
    catch (const std::exception& e) {
        Logger::Get().Error("Failed to serialize keymap configuration: " + std::string(e.what()));
        return false;
    }
}

bool KeymapManager::ProcessKeyEvent(int key, int scancode, int action, int mods) {
    bool ctrlPressed = (mods & GLFW_MOD_CONTROL) != 0;
    bool shiftPressed = (mods & GLFW_MOD_SHIFT) != 0;
    bool altPressed = (mods & GLFW_MOD_ALT) != 0;

    bool matched = false;
    for (const auto& pair : m_Bindings) {
        const auto& combo = pair.second;
        if (combo.IsUnbound()) continue;

        bool keyMatches = false;
        if (combo.scancode != -1 && scancode != -1) {
            keyMatches = (combo.scancode == scancode);
        } else {
            keyMatches = (combo.key == key);
        }

        if (keyMatches && combo.ctrl == ctrlPressed && combo.shift == shiftPressed && combo.alt == altPressed) {
            matched = true;
            if (action == GLFW_PRESS) {
                m_TriggeredActions[pair.first] = true;
                m_HeldActions[pair.first] = true;
            } else if (action == GLFW_RELEASE) {
                m_HeldActions[pair.first] = false;
            }
        }
    }
    return matched;
}

void KeymapManager::BindAction(const std::string& actionName, const KeyCombination& combo) {
    m_Bindings[actionName] = combo;
    if (!combo.IsUnbound() && combo.key != 0) {
        m_Bindings[actionName].scancode = glfwGetKeyScancode(combo.key);
    } else {
        m_Bindings[actionName].scancode = -1;
        m_Bindings[actionName].key = 0;
    }
}

bool KeymapManager::ConsumeActionTrigger(const std::string& actionName) {
    auto it = m_TriggeredActions.find(actionName);
    if (it != m_TriggeredActions.end() && it->second) {
        it->second = false;
        return true;
    }
    return false;
}

bool KeymapManager::IsActionActive(const std::string& actionName) const {
    auto it = m_HeldActions.find(actionName);
    if (it != m_HeldActions.end()) {
        return it->second;
    }
    return false;
}

std::string KeymapManager::GetActionShortcutString(const std::string& actionName) const {
    auto it = m_Bindings.find(actionName);
    if (it != m_Bindings.end()) {
        // Prefer em-dash for UI readability; ToString uses "None" for JSON.
        if (it->second.IsUnbound()) {
            // Group members: show parent group shortcut as hint
            if (const auto* def = core::ops::ActionCatalog::Find(actionName)) {
                if (def->role == core::ops::ActionRole::GroupMember && def->groupId) {
                    auto git = m_Bindings.find(def->groupId);
                    if (git != m_Bindings.end() && !git->second.IsUnbound()) {
                        return "via " + git->second.ToString();
                    }
                }
            }
            return "—";
        }
        return it->second.ToString();
    }
    return "—";
}
