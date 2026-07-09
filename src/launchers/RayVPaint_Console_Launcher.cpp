#include <windows.h>
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
    
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    si.cb = sizeof(si);
    
    // Append the --console flag so the core executable knows it should attach/allocate a console
    std::wstring cmd = GetCommandLineW();
    cmd += L" --console";
    
    if (CreateProcessW(
        target.c_str(),
        const_cast<wchar_t*>(cmd.c_str()),
        NULL, NULL, FALSE, 0, NULL,
        NULL, // Inherit calling process's working directory
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
