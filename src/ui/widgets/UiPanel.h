#pragma once
#include <imgui.h>

namespace Ui {

// Soft elevated panel chrome for dock windows (semi-transparent feel via style colors).
// Push at Begin, Pop after End. Returns same as ImGui::Begin.
bool BeginDockPanel(const char* name, bool* open = nullptr, ImGuiWindowFlags flags = 0);
void EndDockPanel();

// Compact section label (muted)
void SectionLabel(const char* text);

} // namespace Ui
