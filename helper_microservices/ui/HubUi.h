#pragma once
#include "Dx11ImGuiApp.h"

namespace helpers {

enum class AppMode {
    Hub,
    Convert,
    Atlas,
};

// Returns newly selected mode (or Hub if none).
AppMode DrawHubUi(Dx11ImGuiApp& app);

} // namespace helpers
