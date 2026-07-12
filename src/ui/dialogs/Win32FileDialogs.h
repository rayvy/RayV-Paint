#pragma once
// Shared Win32 open/save dialogs for UI (not core). Prefer FileExplorer modes when possible.
#include <cstddef>

namespace Ui {

// filter: double-null terminated Win32 filter string. Returns true if user picked a path.
bool ShowOpenFile(char* outPath, size_t maxLen,
                  const char* filter = "All Files (*.*)\0*.*\0");
bool ShowSaveFile(char* outPath, size_t maxLen,
                  const char* filter = "All Files (*.*)\0*.*\0");

} // namespace Ui
