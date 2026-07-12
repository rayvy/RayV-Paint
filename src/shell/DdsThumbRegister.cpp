#include "DdsThumbRegister.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <string>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace DdsThumbRegister {
namespace {

static const wchar_t* kClsidStr = L"{B5E8A1C2-4F3D-4A9E-9C1B-7D6E5F4A3B2C}";
static const wchar_t* kThumbShellEx = L"{e357fccd-a995-4576-b01f-234630154e96}";

using RegisterFn = HRESULT(__stdcall*)();

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        p.resize(slash);
    return p;
}

static std::wstring ReadRegSz(HKEY root, const wchar_t* sub, const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return {};
    wchar_t val[1024] = {};
    DWORD type = 0, cb = sizeof(val);
    LONG rc = RegQueryValueExW(key, name, nullptr, &type,
                               reinterpret_cast<LPBYTE>(val), &cb);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_SZ)
        return {};
    return val;
}

static bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Thumbnail host only loads handlers whose CLSID is under HKLM (verified Win11).
static bool IsHklmClsidOk(const std::wstring& wantDll) {
    const std::wstring inproc =
        std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsidStr + L"\\InprocServer32";
    std::wstring dll = ReadRegSz(HKEY_LOCAL_MACHINE, inproc.c_str(), nullptr);
    if (dll.empty() || !FileExists(dll))
        return false;
    if (!wantDll.empty() && _wcsicmp(dll.c_str(), wantDll.c_str()) != 0)
        return false;
    return true;
}

static bool IsExtBound(HKEY root, const wchar_t* prefix) {
    std::wstring shellex =
        std::wstring(prefix) + L".dds\\ShellEx\\" + kThumbShellEx;
    std::wstring clsid = ReadRegSz(root, shellex.c_str(), nullptr);
    return !clsid.empty() && _wcsicmp(clsid.c_str(), kClsidStr) == 0;
}

static bool CallDllRegister(const std::wstring& dll, bool unregister) {
    HMODULE mod = LoadLibraryW(dll.c_str());
    if (!mod) return false;
    auto fn = reinterpret_cast<RegisterFn>(
        GetProcAddress(mod, unregister ? "RayV_UnregisterDdsThumbnails"
                                       : "RayV_RegisterDdsThumbnails"));
    HRESULT hr = E_FAIL;
    if (fn) {
        hr = fn();
    } else {
        using DllReg = HRESULT(STDAPICALLTYPE*)();
        auto reg = reinterpret_cast<DllReg>(GetProcAddress(mod,
            unregister ? "DllUnregisterServer" : "DllRegisterServer"));
        if (reg) hr = reg();
    }
    FreeLibrary(mod);
    // ACCESS_DENIED means HKCU ok but HKLM needs elevation — still partial
    return SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
}

// Elevate once: regsvr32 the DLL (writes HKLM when elevated).
static bool ElevateRegsvr32(const std::wstring& dll) {
    // Avoid infinite UAC loops: only auto-elevate if never succeeded HKLM
    wchar_t localApp[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring flag = std::wstring(localApp) + L"\\RayVPaint\\thumb_hklm_ok.flag";
        if (FileExists(flag) && IsHklmClsidOk(dll))
            return true;
    }

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = L"regsvr32.exe";
    std::wstring params = L"/s \"" + dll + L"\"";
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei))
        return false;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 15000);
        CloseHandle(sei.hProcess);
    }
    bool ok = IsHklmClsidOk(dll);
    if (ok) {
        wchar_t appdata[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", appdata, MAX_PATH) > 0) {
            std::wstring dir = std::wstring(appdata) + L"\\RayVPaint";
            CreateDirectoryW(dir.c_str(), nullptr);
            std::wstring flag = dir + L"\\thumb_hklm_ok.flag";
            HANDLE h = CreateFileW(flag.c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
    }
    return ok;
}

} // namespace

std::wstring DefaultDllPath() {
    return ExeDir() + L"\\RayVPaint_DdsThumb.dll";
}

bool IsRegistered() {
    std::wstring want = DefaultDllPath();
    if (!FileExists(want))
        return false;
    // Must have HKLM CLSID (thumbnail host requirement)
    if (!IsHklmClsidOk(want))
        return false;
    // Extension binding either HKCU or HKLM
    if (!IsExtBound(HKEY_CURRENT_USER, L"Software\\Classes\\") &&
        !IsExtBound(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\"))
        return false;
    return true;
}

bool EnsureRegistered(const std::wstring& dllPathIn) {
    std::wstring dll = dllPathIn.empty() ? DefaultDllPath() : dllPathIn;
    if (!FileExists(dll))
        return false;

    if (IsRegistered())
        return true;

    // Non-elevated: writes HKCU + tries HKLM (may get ACCESS_DENIED)
    CallDllRegister(dll, false);

    if (IsHklmClsidOk(dll)) {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        return true;
    }

    // Elevate regsvr32 once so Explorer thumbnail host can CoCreate the CLSID
    if (ElevateRegsvr32(dll)) {
        // Re-run to bind HKCU extension + restore PNG/JPG (no admin needed for that)
        CallDllRegister(dll, false);
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        return IsRegistered();
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return IsRegistered();
}

bool Unregister() {
    std::wstring dll = DefaultDllPath();
    if (!FileExists(dll))
        return false;
    bool ok = CallDllRegister(dll, true);
    // Best-effort elevated cleanup of HKLM
    if (IsHklmClsidOk({})) {
        SHELLEXECUTEINFOW sei = {};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas";
        sei.lpFile = L"regsvr32.exe";
        std::wstring params = L"/u /s \"" + dll + L"\"";
        sei.lpParameters = params.c_str();
        sei.nShow = SW_HIDE;
        if (ShellExecuteExW(&sei) && sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 15000);
            CloseHandle(sei.hProcess);
        }
    }
    return ok;
}

} // namespace DdsThumbRegister
