#include <windows.h>
#include <string>

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Get full path of the launcher
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring ws(exePath);
    size_t pos = ws.find_last_of(L"\\/");
    std::wstring dir = ws.substr(0, pos);
    
    // Target executable path is inside bin/RayVPaint_Core.exe
    std::wstring target = dir + L"\\bin\\RayVPaint_Core.exe";
    
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    si.cb = sizeof(si);
    
    // Create the process, passing the original command line arguments
    if (CreateProcessW(
        target.c_str(),
        const_cast<wchar_t*>(GetCommandLineW()),
        NULL, NULL, FALSE, 0, NULL, 
        NULL, // Inherit calling process's working directory
        &si, &pi)) {
        // Wait for the subprocess to complete to return the correct exit code
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return exitCode;
    }
    return 1;
}
