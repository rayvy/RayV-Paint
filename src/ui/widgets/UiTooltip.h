#pragma once
#include <imgui.h>

namespace Ui {

// Delayed tooltip (default 1.0s hover, no click/hold). Soft ease-in + slight squash.
// Call once per frame after widgets; auto-tracks hovered item via ImGui.
void TooltipSetDelay(float seconds); // default ~0.4s (tokens)
void Tooltip(const char* text);      // show if current item hovered long enough (multi-line OK)

// Process end-of-frame (optional — Tooltip() is self-contained)
void TooltipEndFrame();

} // namespace Ui
