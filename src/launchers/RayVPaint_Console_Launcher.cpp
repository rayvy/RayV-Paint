#include <windows.h>
#include <shellapi.h>
#include <string>
#include <iostream>

int main() {
    // Get full path of the launcher
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring ws(exePath);
    size_t pos = ws.find_last_of(L"\\/");
    std::wstring dir = ws.substr(0, pos);
    
    std::wstring target = dir + L"\\bin\\RayVPaint_Core.exe";

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring cmd = L"\"" + target + L"\"";
    for (int i = 1; i < argc; ++i) {
        cmd += L" ";
        cmd += L"\"";
        cmd += argv[i];
        cmd += L"\"";
    }
    cmd += L" --console";
    if (argv) {
        LocalFree(argv);
    }
    
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    si.cb = sizeof(si);

    std::wstring mutableCmd = cmd;
    if (CreateProcessW(
        target.c_str(),
        mutableCmd.data(),
        NULL, NULL, FALSE, 0, NULL,
        dir.c_str(), // Set working directory to the parent directory
        &si, &pi)) {
        // Wait for the subprocess to complete
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return exitCode;
    }
    return 1;
}
