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
bool VisualSlider(const char* id, float* value, ImVec2 size,
                  VisualSliderSkin skin = VisualSliderSkin::Plain,
                  const float rgb[3] = nullptr,
                  const char* tooltip = nullptr,
                  float defaultValue = -1.f, // <0 = no default reset
                  float snapStep = 0.f);

// Generic ImGui-style float slider with Backspace reset + Ctrl snap + animated thumb.
bool SmartSliderFloat(const char* label, float* v, float vMin, float vMax,
                      float defaultValue, float snapStep,
                      const char* format = "%.3f");

} // namespace Ui
