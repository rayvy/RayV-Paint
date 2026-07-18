#include "CrashGuard.h"
#include "Logger.h"
#include "PathUtil.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <new>
#include <string>

#pragma comment(lib, "dbghelp.lib")

namespace CrashGuard {
namespace {

std::mutex g_Mutex;
std::wstring g_CrashDirW;
std::string g_CrashDirUtf8;
std::string g_ExtraCrashLogUtf8;
bool g_SilentMode = false;
volatile long g_Handling = 0;
char g_LastCheckpoint[256] = "early_startup";

std::wstring Utf8ToWide(const std::string& u8) {
    if (u8.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), w.data(), n);
    return w;
}

void WriteTextFile(const std::wstring& path, const std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
}

void AppendAppLog(const std::string& line) {
    // Best-effort — Logger may be dead or locked during crash.
    try {
        Logger::Get().ErrorTag("CRASH", line);
        Logger::Get().Flush();
    } catch (...) {
    }
    // Always try raw file append (does not depend on Logger mutex state)
    try {
        if (!g_CrashDirUtf8.empty()) {
            std::ofstream f(g_CrashDirUtf8 + "/crash_breadcrumbs.log", std::ios::app);
            if (f) {
                f << line << "\n";
                f.flush();
            }
        }
    } catch (...) {
    }
}

std::string ExceptionCodeName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_DATATYPE_MISALIGNMENT:    return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSTRUCTION";
    case 0xE06D7363:                         return "CPP_EXCEPTION"; // MSVC C++ EH
    default: {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%08X", (unsigned)code);
        return buf;
    }
    }
}

void WriteMinidump(EXCEPTION_POINTERS* ep, const std::wstring& path) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;

    // MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo
    const MINIDUMP_TYPE type = (MINIDUMP_TYPE)(
        MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory |
        MiniDumpWithThreadInfo | MiniDumpWithHandleData);

    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, type,
                      ep ? &mei : nullptr, nullptr, nullptr);
    FlushFileBuffers(hFile);
    CloseHandle(hFile);
}

void WriteCrashReport(const char* kind, const char* detail, EXCEPTION_POINTERS* ep) {
    if (InterlockedCompareExchange(&g_Handling, 1, 0) != 0) {
        // Nested crash while handling — hard exit
        TerminateProcess(GetCurrentProcess(), 0xDEAD);
        return;
    }

    char timeBuf[64] = {};
    {
        time_t t = time(nullptr);
        struct tm tm_b{};
        localtime_s(&tm_b, &t);
        std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tm_b);
    }

    std::string report;
    report.reserve(2048);
    report += "=== RayV Paint CRASH ===\n";
    report += "time: ";
    report += timeBuf;
    report += "\nkind: ";
    report += kind ? kind : "unknown";
    report += "\ncheckpoint: ";
    report += g_LastCheckpoint;
    report += "\n";
    if (detail && detail[0]) {
        report += "detail: ";
        report += detail;
        report += "\n";
    }
    if (ep && ep->ExceptionRecord) {
        report += "exception: ";
        report += ExceptionCodeName(ep->ExceptionRecord->ExceptionCode);
        report += "\naddress: 0x";
        char abuf[32];
        std::snprintf(abuf, sizeof(abuf), "%p", ep->ExceptionRecord->ExceptionAddress);
        report += abuf;
        report += "\n";
        if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            ep->ExceptionRecord->NumberParameters >= 2) {
            report += "access: ";
            report += (ep->ExceptionRecord->ExceptionInformation[0] == 0) ? "read" :
                      (ep->ExceptionRecord->ExceptionInformation[0] == 1) ? "write" : "dep";
            report += " at 0x";
            std::snprintf(abuf, sizeof(abuf), "%llX",
                          (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
            report += abuf;
            report += "\n";
        }
    }
    report += "pid: ";
    report += std::to_string(GetCurrentProcessId());
    report += " tid: ";
    report += std::to_string(GetCurrentThreadId());
    report += "\n";

    std::wstring dir = g_CrashDirW;
    if (dir.empty())
        dir = Utf8ToWide(g_CrashDirUtf8);
    if (!dir.empty()) {
        CreateDirectoryW(dir.c_str(), nullptr);
        WriteTextFile(dir + L"\\crash_last.log", report);

        wchar_t dumpName[128];
        std::swprintf(dumpName, 128, L"\\crash_%u_%u.dmp",
                      (unsigned)GetCurrentProcessId(), (unsigned)GetTickCount());
        WriteMinidump(ep, dir + dumpName);
    }

    // Also try user log path (append)
    AppendAppLog(std::string("FATAL ") + kind + " | " + (detail ? detail : "") +
                 " | checkpoint=" + g_LastCheckpoint);

    // Stress journal / agent log: append full report so session file ends with CRASH
    if (!g_ExtraCrashLogUtf8.empty()) {
        try {
            HANDLE h = CreateFileW(Utf8ToWide(g_ExtraCrashLogUtf8).c_str(),
                                  FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                std::string block = "\n########## CRASH (CrashGuard) ##########\n";
                block += report;
                block += "########## END CRASH ##########\n";
                DWORD written = 0;
                WriteFile(h, block.data(), (DWORD)block.size(), &written, nullptr);
                FlushFileBuffers(h);
                CloseHandle(h);
            }
        } catch (...) {
        }
    }

    // Interactive: MessageBox so user notices. Stress/CI/headless: silent exit
    // (blocking dialog made agents/users wait minutes thinking the suite was "working").
    if (!g_SilentMode) {
        std::string box = report + "\nWritten to:\n" + g_CrashDirUtf8 + "\\crash_last.log";
        if (!g_ExtraCrashLogUtf8.empty()) {
            box += "\n";
            box += g_ExtraCrashLogUtf8;
        }
        MessageBoxA(nullptr, box.c_str(), "RayV Paint — Crash", MB_OK | MB_ICONERROR | MB_TOPMOST);
    }
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) {
    WriteCrashReport("UnhandledException", ExceptionCodeName(
        ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0).c_str(), ep);
    return EXCEPTION_EXECUTE_HANDLER;
}

LONG WINAPI VectoredHandler(EXCEPTION_POINTERS* ep) {
    // Stack overflow often never reaches UnhandledExceptionFilter — capture here.
    // Other faults: continue search so filter can run once (avoid double MessageBox).
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        WriteCrashReport("STACK_OVERFLOW", "vectored", ep);
        return EXCEPTION_EXECUTE_HANDLER;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void OnTerminate() {
    WriteCrashReport("std::terminate", "uncaught C++ exception or terminate()", nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
}

void OnPureCall() {
    WriteCrashReport("purecall", "pure virtual function call", nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
}

void OnInvalidParameter(const wchar_t* expression, const wchar_t* function,
                        const wchar_t* file, unsigned int line, uintptr_t) {
    char detail[512];
    std::snprintf(detail, sizeof(detail), "CRT invalid parameter line=%u", line);
    (void)expression; (void)function; (void)file;
    WriteCrashReport("CRT_InvalidParameter", detail, nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
}

void OnSigAbort(int) {
    WriteCrashReport("SIGABRT", "abort() / assertion", nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
}

void OnSigSegv(int) {
    WriteCrashReport("SIGSEGV", "segmentation fault signal", nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
}

void OnNewHandler() {
    WriteCrashReport("operator_new", "out of memory (new failed)", nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
}

} // namespace

void Install(const std::string& crashDirUtf8) {
    g_CrashDirUtf8 = crashDirUtf8;
    g_CrashDirW = Utf8ToWide(crashDirUtf8);
    if (!g_CrashDirW.empty())
        CreateDirectoryW(g_CrashDirW.c_str(), nullptr);

    SetUnhandledExceptionFilter(UnhandledFilter);
    AddVectoredExceptionHandler(1, VectoredHandler);

    std::set_terminate(OnTerminate);
    _set_purecall_handler(OnPureCall);
    _set_invalid_parameter_handler(OnInvalidParameter);
    std::signal(SIGABRT, OnSigAbort);
    std::signal(SIGSEGV, OnSigSegv);
    std::set_new_handler(OnNewHandler);

    // Disable Windows Error Reporting dialog that swallows dumps without our log
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    NoteCheckpoint("CrashGuard::Install");
}

void SetSilentMode(bool silent) { g_SilentMode = silent; }
bool IsSilentMode() { return g_SilentMode; }

void SetExtraCrashLogPath(const std::string& utf8Path) { g_ExtraCrashLogUtf8 = utf8Path; }
std::string ExtraCrashLogPath() { return g_ExtraCrashLogUtf8; }

void NoteCheckpoint(const char* where) {
    if (!where) return;
    // no lock — crash path must stay async-signal-ish
    std::strncpy(g_LastCheckpoint, where, sizeof(g_LastCheckpoint) - 1);
    g_LastCheckpoint[sizeof(g_LastCheckpoint) - 1] = 0;
}

void ReportHandledException(const char* where, const char* detail) {
    std::string msg = std::string(where ? where : "?") + ": " + (detail ? detail : "");
    AppendAppLog(msg);
    try {
        Logger::Get().ErrorTag("exception", msg);
        Logger::Get().Flush();
    } catch (...) {
    }
}

namespace {
void SehCall(void (*fn)(void*), void* ctx, int* okOut) {
    __try {
        fn(ctx);
        *okOut = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *okOut = 0;
    }
}
} // namespace

bool RunUnderSeh(void (*fn)(void* ctx), void* ctx, const char* whereLabel) {
    if (!fn) return false;
    int ok = 0;
    SehCall(fn, ctx, &ok);
    if (!ok) {
        ReportHandledException(whereLabel ? whereLabel : "RunUnderSeh",
                               "SEH hard fault caught (process continues)");
    }
    return ok != 0;
}

} // namespace CrashGuard
