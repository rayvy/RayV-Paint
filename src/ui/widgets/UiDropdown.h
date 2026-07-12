#pragma once
#include <imgui.h>
#include <vector>
#include <string>

namespace Ui {

enum DropdownFlags_ : int {
    DropdownFlags_None = 0,
    DropdownFlags_ClickOnly = 1 << 0,
    DropdownFlags_HoldOnly  = 1 << 1,
    DropdownFlags_ClickAndHold = 1 << 2, // default if none set
};
typedef int DropdownFlags;

// Trigger is an icon button. Returns true if selection index changed.
// items: display labels. selected: in/out index.
// active: soft fill under icon (outline owned by floating toolbar accent).
bool DropdownIcon(const char* id, const char* iconLogicalName, ImVec2 triggerSize,
                  const char* const* items, int itemCount, int* selected,
                  const char* tooltip = nullptr, DropdownFlags flags = DropdownFlags_ClickAndHold,
                  bool active = false);

// Text/chip trigger variant
bool DropdownChip(const char* id, const char* previewLabel,
                  const char* const* items, int itemCount, int* selected,
                  DropdownFlags flags = DropdownFlags_ClickAndHold);

// Standard themed combo (replaces raw ImGui::Combo and local UiCombo helpers).
// Optional label drawn to the left. Returns true if selection changed.
bool Combo(const char* id, int* idx, const char* const* items, int count,
           const char* label = nullptr);

} // namespace Ui
