#pragma once

#include "ModTypes.h"
#include <string>

namespace modio {

// Parse XXMI / 3DMigoto character .ini into a ModScene.
// - Does not load binary .buf/.ib contents (paths only)
// - Soft-fail: missing files → warnings, still returns partial scene when possible
// - Target: ZZZ (Resource\ZZMI\*) + GI (ps-tN) patterns
class ModIniParser {
public:
    // Absolute or relative path to .ini
    static ModScene ParseFile(const std::string& iniPath);

    // Parse from already-loaded text (unit tests). iniDir used for path resolve.
    static ModScene ParseText(const std::string& iniText, const std::string& iniPathForMeta,
                              const std::string& iniDirectory);

    // Optional: re-check file existence on disk for an existing scene
    static void RefreshExistence(ModScene& scene);
};

// Human-readable dump for console / UI diagnostics
std::string FormatSceneSummary(const ModScene& scene);

} // namespace modio
