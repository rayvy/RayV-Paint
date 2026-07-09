#pragma once
#include <imgui.h>

namespace Ui {

// Icon toggle with press/bounce motion. Returns true if value changed.
bool IconToggle(const char* id, const char* iconLogicalName, bool* value,
                ImVec2 size, const char* tooltipOn, const char* tooltipOff = nullptr);

} // namespace Ui
