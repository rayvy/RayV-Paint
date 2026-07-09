#pragma once
#include "../icons/SvgIconCache.h"
#include "../style/UiTokens.h"
#include <imgui.h>

namespace Ui {

struct IconButtonResult {
    bool clicked = false;
    bool held = false;
    bool hovered = false;
};

// Animated icon button: press scale (ease-out) → release bounce (EaseOutBack).
IconButtonResult IconButton(const char* id, const char* iconLogicalName,
                            ImVec2 size, const char* tooltip = nullptr,
                            bool enabled = true, bool active = false);

// Same but with explicit icon pointer (already resolved)
IconButtonResult IconButton(const char* id, const SvgIcon* icon,
                            ImVec2 size, const char* tooltip = nullptr,
                            bool enabled = true, bool active = false);

} // namespace Ui
