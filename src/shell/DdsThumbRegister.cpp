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

static const wchar_t* kClsidThumb = L"{B5E8A1C2-4F3D-4A9E-9C1B-7D6E5F4A3B2C}";
static const wchar_t* kClsidProps = L"{D4A1B2C3-5E6F-4789-A012-3456789ABCDE}";
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

static bool IsHklmClsidPathOk(const wchar_t* clsid, const std::wstring& wantDll) {
    const std::wstring inproc =
        std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + clsid + L"\\InprocServer32";
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
    return !clsid.empty() && _wcsicmp(clsid.c_str(), kClsidThumb) == 0;
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
    return SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
}

static bool ElevateRegsvr32(const std::wstring& dll, bool unregister) {
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = L"regsvr32.exe";
    std::wstring params = unregister
        ? (L"/u /s \"" + dll + L"\"")
        : (L"/s \"" + dll + L"\"");
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei))
        return false;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 30000);
        CloseHandle(sei.hProcess);
    }
    if (unregister)
        return !IsHklmClsidPathOk(kClsidThumb, {});
    return IsHklmClsidPathOk(kClsidThumb, dll);
}

static void WriteHklmOkFlag(bool ok) {
    wchar_t appdata[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", appdata, MAX_PATH) <= 0)
        return;
    std::wstring dir = std::wstring(appdata) + L"\\RayVPaint";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring flag = dir + L"\\thumb_hklm_ok.flag";
    if (ok) {
        HANDLE h = CreateFileW(flag.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    } else {
        DeleteFileW(flag.c_str());
    }
}

} // namespace

std::wstring DefaultDllPath() {
    return ExeDir() + L"\\RayVPaint_DdsThumb.dll";
}

bool IsProcessElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        return false;
    TOKEN_ELEVATION elev = {};
    DWORD cb = 0;
    BOOL ok = GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &cb);
    CloseHandle(tok);
    return ok && elev.TokenIsElevated;
}

IntegrationStatus QueryStatus() {
    IntegrationStatus s;
    s.processElevated = IsProcessElevated();
    s.elevLine = s.processElevated
        ? "Process: elevated (Admin)"
        : "Process: not elevated (UAC needed for full register)";

    std::wstring dll = DefaultDllPath();
    s.dllPresent = FileExists(dll);
    s.dllLine = s.dllPresent
        ? "DLL: found (RayVPaint_DdsThumb.dll)"
        : "DLL: MISSING next to exe";

    s.hklmThumbClsid = IsHklmClsidPathOk(kClsidThumb, dll);
    s.thumbLine = s.hklmThumbClsid
        ? "DDS thumbs: HKLM CLSID OK"
        : "DDS thumbs: HKLM CLSID missing (register as Admin)";

    s.hklmPropClsid = IsHklmClsidPathOk(kClsidProps, dll);
    s.propLine = s.hklmPropClsid
        ? "DDS properties: HKLM CLSID OK"
        : "DDS properties: HKLM CLSID missing";

    s.extShellExBound =
        IsExtBound(HKEY_CURRENT_USER, L"Software\\Classes\\") ||
        IsExtBound(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\");

    s.fullyOk = s.dllPresent && s.hklmThumbClsid && s.extShellExBound;
    if (s.fullyOk && s.hklmPropClsid)
        s.summary = "Explorer integration: ready";
    else if (s.fullyOk)
        s.summary = "Thumbs OK; properties partial — re-register";
    else if (!s.dllPresent)
        s.summary = "Build/copy RayVPaint_DdsThumb.dll next to Core";
    else
        s.summary = "Click Register (accept UAC once)";
    return s;
}

bool IsRegistered() {
    return QueryStatus().fullyOk;
}

bool EnsureRegistered(const std::wstring& dllPathIn, bool forceElevate) {
    std::wstring dll = dllPathIn.empty() ? DefaultDllPath() : dllPathIn;
    if (!FileExists(dll))
        return false;

    if (!forceElevate && IsRegistered())
        return true;

    // Non-elevated pass: HKCU + try HKLM
    CallDllRegister(dll, false);

    if (IsHklmClsidPathOk(kClsidThumb, dll) && IsHklmClsidPathOk(kClsidProps, dll)) {
        WriteHklmOkFlag(true);
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        return true;
    }

    // Elevate for HKLM (required by Explorer thumbnail host)
    if (ElevateRegsvr32(dll, false)) {
        CallDllRegister(dll, false); // HKCU + PNG restore
        WriteHklmOkFlag(true);
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
    CallDllRegister(dll, true);
    ElevateRegsvr32(dll, true);
    WriteHklmOkFlag(false);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

} // namespace DdsThumbRegister
