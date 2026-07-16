#pragma once
#include <imgui.h>

namespace Ui {

// Soft elevated panel chrome for dock windows (semi-transparent feel via style colors).
// Push at Begin, Pop after End. Returns same as ImGui::Begin.
// Prefer NoScrollbar on outer panels — only children that list content should scroll.
bool BeginDockPanel(const char* name, bool* open = nullptr, ImGuiWindowFlags flags = 0);
void EndDockPanel();

// Compact section label (muted)
void SectionLabel(const char* text);

// Hard clamp dock leaf size (no rubber-band overstretch). Call each frame after Begin.
// crossAxis: for vertical strips, clamp width; for horizontal strips, clamp height.
void ClampDockLeafCrossAxis(bool verticalStrip, float minPx, float maxPx);
// Clamp both axes into [minW,maxW] x [minH,maxH] for regular panels.
void ClampDockLeafBox(float minW, float maxW, float minH, float maxH);

} // namespace Ui
