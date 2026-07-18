#pragma once
// Process-wide crash / abort capture for RayV Paint.
// Install as early as possible (before other subsystems). Writes:
//   - Documents/RayVPaint/user/crash_last.log  (human text)
//   - Documents/RayVPaint/user/crash_*.dmp     (minidump, if dbghelp loads)
//   - also appends a line to the normal app log if Logger is alive

#include <string>

namespace CrashGuard {

// Call once at process entry (wWinMain / main), before heavy work.
void Install(const std::string& crashDirUtf8);

// Force flush of any pending crash note (also used before risky ops).
void NoteCheckpoint(const char* where);

// Optional: write a non-fatal diagnostic without terminating.
void ReportHandledException(const char* where, const char* detail);

// Run fn(ctx) under SEH. Returns false if a hard fault was caught (and reported).
// fn must not require C++ unwinding across the SEH boundary (keep it thin).
bool RunUnderSeh(void (*fn)(void* ctx), void* ctx, const char* whereLabel);

// Stress / CI / headless: no blocking MessageBox — process exits so scripts continue.
// Default false (interactive crash dialog for humans).
void SetSilentMode(bool silent);
bool IsSilentMode();

// Extra UTF-8 path: on crash, append the same report here (flushed).
// Used by --stress-16k journal so the session file ends with CRASH even if
// the user never opens crash_last.log.
void SetExtraCrashLogPath(const std::string& utf8Path);
std::string ExtraCrashLogPath();

} // namespace CrashGuard
