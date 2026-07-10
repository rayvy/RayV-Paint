#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <string>
#include <vector>

// Photoshop-style single process: second launch forwards paths and exits.
namespace SingleInstance {

// Custom window message id for open-path IPC (RegisterWindowMessage).
unsigned int GetOpenPathMessage();

// Property name set on main HWND so second instance can find us.
const wchar_t* GetWindowPropName();

// Mutex name (local session).
const wchar_t* GetMutexName();

// Returns true if this process is the primary instance (owns the mutex).
// If false, |existingHwnd| is the other instance's main window (may be null if not ready).
bool TryBecomePrimary(HANDLE& outMutex, HWND& existingHwnd);

// Mark primary HWND for discovery (call after window create).
void RegisterMainWindow(HWND hwnd);

void UnregisterMainWindow(HWND hwnd);

// Send one UTF-8 path to the primary instance. Returns true if posted.
bool SendOpenPath(HWND target, const std::string& utf8Path);

// Bring primary window to foreground.
void FocusWindow(HWND hwnd);

// Parse WM_COPYDATA payload written by SendOpenPath into UTF-8 path.
// Returns empty on failure.
std::string ParseCopyData(const COPYDATASTRUCT* cds);

// Convenience: if secondary, forward all file args and return false (caller should exit).
// If primary, return true (caller continues startup).
// |allowMulti| skips single-instance (tests / --allow-multi-instance).
bool GuardStartup(int argc, char** argv, bool allowMulti, HANDLE& outMutex);

} // namespace SingleInstance
