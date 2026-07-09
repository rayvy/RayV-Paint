#include "UiIconToggle.h"
#include "UiIconButton.h"

namespace Ui {

bool IconToggle(const char* id, const char* iconLogicalName, bool* value,
                ImVec2 size, const char* tooltipOn, const char* tooltipOff) {
    if (!value) return false;
    const char* tip = *value ? (tooltipOn ? tooltipOn : "") : (tooltipOff ? tooltipOff : tooltipOn);
    auto r = IconButton(id, iconLogicalName, size, tip, true, *value);
    if (r.clicked) {
        *value = !*value;
        return true;
    }
    return false;
}

} // namespace Ui
