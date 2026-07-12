#pragma once
// Helpers for the main app: ensure Explorer DDS thumbnail handler is registered.

#include <string>

namespace DdsThumbRegister {

// Path to RayVPaint_DdsThumb.dll next to this executable (bin/).
std::wstring DefaultDllPath();

// True if HKCU already points ShellEx for .dds at our CLSID and InprocServer32 exists.
bool IsRegistered();

// Load the DLL and call RayV_RegisterDdsThumbnails (or write registry if export missing).
// Safe to call every startup — cheap if already registered with matching path.
bool EnsureRegistered(const std::wstring& dllPath = {});

// Unregister HKCU thumbnail handler.
bool Unregister();

} // namespace DdsThumbRegister
