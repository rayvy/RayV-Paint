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
// Skins HueStrip / OpacityChecker fully painted in Stage 2a; Plain works now.
bool VisualSlider(const char* id, float* value, ImVec2 size,
                  VisualSliderSkin skin = VisualSliderSkin::Plain,
                  const float rgb[3] = nullptr, // for opacity skin
                  const char* tooltip = nullptr);

} // namespace Ui
