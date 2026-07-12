#pragma once
// App-side helpers for Explorer DDS integration (thumbs + type + property schema).

#include <string>

namespace DdsThumbRegister {

struct IntegrationStatus {
    bool dllPresent = false;
    bool hklmThumbClsid = false;   // required for Explorer thumbnail host
    bool hklmPropClsid = false;    // property handler CLSID in HKLM
    bool extShellExBound = false; // .dds ShellEx → our thumb CLSID
    bool processElevated = false;  // current process has admin token
    bool fullyOk = false;          // ready for Explorer thumbs + props

    // Short English lines for UI (no localization yet)
    std::string dllLine;
    std::string thumbLine;
    std::string propLine;
    std::string elevLine;
    std::string summary;
};

// Path to RayVPaint_DdsThumb.dll next to this executable (bin/).
std::wstring DefaultDllPath();

// True if thumbs + extension binding look complete (HKLM + path match).
bool IsRegistered();

// Snapshot for Register UI (cheap registry reads).
IntegrationStatus QueryStatus();

// Register: HKCU bindings + elevate regsvr32 for HKLM if needed.
// forceElevate: always prompt UAC even if partially registered.
bool EnsureRegistered(const std::wstring& dllPath = {}, bool forceElevate = false);

// Unregister (best-effort; elevates to clear HKLM).
bool Unregister();

// True if this process token is elevated (Admin).
bool IsProcessElevated();

} // namespace DdsThumbRegister
