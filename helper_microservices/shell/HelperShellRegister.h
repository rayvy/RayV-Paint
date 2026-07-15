#pragma once
// Windows Explorer context-menu registration for RayVHelpers.
// Safe to compile into RayVPaint_Core (register button only) and into RayVHelpers itself.

#include <string>

namespace HelperShellRegister {

struct Status {
    bool exePresent = false;
    bool convertVerb = false;
    bool atlasVerb = false;
    bool fullyOk = false;
    std::string summary; // English UI line
    std::string exeLine;
    std::string convertLine;
    std::string atlasLine;
};

// Path to RayVHelpers.exe next to the calling module (Core or Helpers).
std::wstring DefaultHelpersExePath();

Status QueryStatus(const std::wstring& helpersExe = {});

// Register PNG multi-select context verbs (HKCU — no Admin required).
// helpersExe: full path to RayVHelpers.exe; empty = DefaultHelpersExePath().
// iconExe: optional icon source (defaults to helpersExe).
bool EnsureRegistered(const std::wstring& helpersExe = {},
                      const std::wstring& iconExe = {});

bool Unregister();

bool IsRegistered();

} // namespace HelperShellRegister
