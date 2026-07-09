#pragma once
#include <imgui.h>
#include <cstddef>

namespace Ui {

// Label above; single field with path text + browse button integrated on the right.
// browseFilter: Win32 double-null filter string. Returns true if path text changed or browse confirmed.
bool PathField(const char* id, const char* label, char* pathBuf, size_t pathBufSize,
               bool (*browseFn)(char*, size_t, const char*), const char* browseFilter,
               const char* tooltip = nullptr);

} // namespace Ui
