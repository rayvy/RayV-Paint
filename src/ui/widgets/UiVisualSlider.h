#pragma once
#include <imgui.h>
#include <cstdint>

namespace Ui {

enum class VisualSliderSkin : uint8_t {
    Plain = 0,
    HueStrip,
    OpacityChecker,
};

// Base visual slider 0..1. Returns true if value changed.
// Backspace while hovered → reset to defaultValue (if >= 0 as flag use hasDefault).
// Ctrl+drag → snap to snapStep (0 = no snap).
// Double-click / Ctrl+click → exact text entry (safe parse).
bool VisualSlider(const char* id, float* value, ImVec2 size,
                  VisualSliderSkin skin = VisualSliderSkin::Plain,
                  const float rgb[3] = nullptr,
                  const char* tooltip = nullptr,
                  float defaultValue = -1.f, // <0 = no default reset
                  float snapStep = 0.f);

// Standard float slider for panels (Layer FX, opacity, Settings…).
// Interaction contract (all agents must keep this consistent):
//   • Drag — continuous
//   • Ctrl+drag — snap to snapStep
//   • Double-click OR Ctrl+click — exact text entry
//   • Backspace while hovered (not in text mode) — reset to defaultValue
//   • Text parse: invalid string → reject (no change); float for int slots → round
//   • While editing text / handling Backspace → AppContext::NotifyUiKeyboardCapture()
// Prefer this over raw ImGui::SliderFloat in new UI.
bool SmartSliderFloat(const char* label, float* v, float vMin, float vMax,
                      float defaultValue, float snapStep,
                      const char* format = "%.3f");

// Integer variant: text entry accepts float and rounds; rejects non-numeric.
bool SmartSliderInt(const char* label, int* v, int vMin, int vMax,
                    int defaultValue, int snapStep = 1);

// Thin wrapper: same as SmartSliderFloat with common defaults (no snap, mid default).
inline bool SliderFloat(const char* label, float* v, float vMin, float vMax,
                        const char* format = "%.3f") {
    float mid = (vMin + vMax) * 0.5f;
    return SmartSliderFloat(label, v, vMin, vMax, mid, 0.f, format);
}

} // namespace Ui
