#pragma once
#include <imgui.h>
#include <cstdint>

namespace Ui {

enum ColorFieldFlags_ : int {
    ColorFieldFlags_None       = 0,
    ColorFieldFlags_NoAlpha    = 1 << 0, // RGB only
    ColorFieldFlags_AlphaBar   = 1 << 1, // show alpha bar in picker
    ColorFieldFlags_NoInputs   = 1 << 2, // swatch-only (ImGui ColorEdit NoInputs)
    ColorFieldFlags_Pipette    = 1 << 3, // show Pipette in custom popup (closes popup on arm)
    ColorFieldFlags_FullPicker = 1 << 4, // ColorPicker4 popup (for fill chips etc.)
};
typedef int ColorFieldFlags;

// Themed color field. Returns true if rgba changed this frame.
// If flags has Pipette and outPipetteClicked != null: set true when user arms pipette
// (caller should ArmFillPipette / set brush mode and close is already done).
bool ColorField(const char* id, float rgba[4], ColorFieldFlags flags = ColorFieldFlags_NoInputs,
                const char* label = nullptr, bool* outPipetteClicked = nullptr);

} // namespace Ui
