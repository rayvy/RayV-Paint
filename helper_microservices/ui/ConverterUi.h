#pragma once
#include <string>
#include <vector>

namespace helpers {

struct ConverterUiState {
    std::vector<std::string> files;
    int formatIndex = 0;
    bool generateMips = true;
    int speedIndex = 1; // 0 Fast 1 Medium 2 Slow
    bool busy = false;
    float progress = 0.f;
    std::string status;
    std::vector<std::string> logLines;
};

// Full-window converter UI. Returns true if user requested close via Done.
void DrawConverterUi(ConverterUiState& st, class Dx11ImGuiApp& app);

// Run convert using current options (blocking on UI thread; progress updated).
void RunConvert(ConverterUiState& st);

} // namespace helpers
