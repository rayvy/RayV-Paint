#include "HelperShellRegister.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>

#include <string>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace HelperShellRegister {
namespace {

// SystemFileAssociations\.png works for multi-select without owning the extension.
static const wchar_t* kBase =
    L"Software\\Classes\\SystemFileAssociations\\.png\\shell";
static const wchar_t* kConvert = L"RayVPaint.ConvertToDds";
static const wchar_t* kAtlas = L"RayVPaint.CreateAtlas";

static std::wstring ModuleDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p.resize(slash);
    return p;
}

static bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static LONG SetSz(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(value.c_str()),
                          (DWORD)((value.size() + 1) * sizeof(wchar_t)));
}

static bool WriteVerb(const wchar_t* verbKey, const std::wstring& label,
                      const std::wstring& helpersExe, const std::wstring& iconExe,
                      const wchar_t* modeFlag) {
    std::wstring path = std::wstring(kBase) + L"\\" + verbKey;
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
                        KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;

    SetSz(key, nullptr, label);
    SetSz(key, L"Icon", iconExe + L",0");
    // One process receives all selected files as separate argv entries.
    SetSz(key, L"MultiSelectModel", L"Player");
    RegCloseKey(key);

    std::wstring cmdPath = path + L"\\command";
    HKEY cmd = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, cmdPath.c_str(), 0, nullptr, 0,
                        KEY_WRITE | KEY_WOW64_64KEY, nullptr, &cmd, nullptr) != ERROR_SUCCESS)
        return false;

    // "%1" is first file; Explorer appends remaining selected paths when MultiSelectModel=Player.
    // Use --mode + -- so paths with leading dashes stay safe.
    std::wstring cmdLine = L"\"" + helpersExe + L"\" --mode " + modeFlag + L" -- \"%1\"";
    LONG rc = SetSz(cmd, nullptr, cmdLine);
    RegCloseKey(cmd);
    return rc == ERROR_SUCCESS;
}

static bool DeleteTree(const wchar_t* verbKey) {
    std::wstring path = std::wstring(kBase) + L"\\" + verbKey;
    // SHDeleteKeyW removes subkeys (command)
    return SHDeleteKeyW(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS ||
           GetLastError() == ERROR_FILE_NOT_FOUND;
}

static std::wstring ReadDefault(const wchar_t* verbKey) {
    std::wstring path = std::wstring(kBase) + L"\\" + verbKey + L"\\command";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0,
                      KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return {};
    wchar_t val[2048] = {};
    DWORD type = 0, cb = sizeof(val);
    LONG rc = RegQueryValueExW(key, nullptr, nullptr, &type,
                               reinterpret_cast<LPBYTE>(val), &cb);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_SZ) return {};
    return val;
}

static bool VerbLooksOk(const wchar_t* verbKey, const std::wstring& helpersExe) {
    std::wstring cmd = ReadDefault(verbKey);
    if (cmd.empty()) return false;
    if (cmd.find(helpersExe) != std::wstring::npos) return true;
    // Case-insensitive path match
    std::wstring lowerCmd = cmd, lowerExe = helpersExe;
    for (auto& c : lowerCmd) c = (wchar_t)towlower(c);
    for (auto& c : lowerExe) c = (wchar_t)towlower(c);
    return lowerCmd.find(lowerExe) != std::wstring::npos;
}

} // namespace

std::wstring DefaultHelpersExePath() {
    return ModuleDir() + L"\\RayVHelpers.exe";
}

Status QueryStatus(const std::wstring& helpersExeIn) {
    Status s;
    std::wstring exe = helpersExeIn.empty() ? DefaultHelpersExePath() : helpersExeIn;
    s.exePresent = FileExists(exe);
    s.exeLine = s.exePresent
        ? ("Helpers EXE: found (" + WideToUtf8(exe) + ")")
        : "Helpers EXE: MISSING (build RayVHelpers)";
    s.convertVerb = VerbLooksOk(kConvert, exe);
    s.atlasVerb = VerbLooksOk(kAtlas, exe);
    s.convertLine = s.convertVerb ? "Context: Convert to DDS OK" : "Context: Convert to DDS missing";
    s.atlasLine = s.atlasVerb ? "Context: Atlas creator OK" : "Context: Atlas creator missing";
    s.fullyOk = s.exePresent && s.convertVerb && s.atlasVerb;
    if (s.fullyOk)
        s.summary = "PNG helpers: registered";
    else if (!s.exePresent)
        s.summary = "Build RayVHelpers.exe next to Core, then Register";
    else
        s.summary = "Click Register helpers (PNG context menu)";
    return s;
}

bool EnsureRegistered(const std::wstring& helpersExeIn, const std::wstring& iconExeIn) {
    std::wstring helpers = helpersExeIn.empty() ? DefaultHelpersExePath() : helpersExeIn;
    if (!FileExists(helpers))
        return false;
    std::wstring icon = iconExeIn.empty() ? helpers : iconExeIn;
    if (!FileExists(icon))
        icon = helpers;

    bool ok = true;
    ok = WriteVerb(kConvert, L"Convert to DDS (RayVPaint)", helpers, icon, L"convert") && ok;
    ok = WriteVerb(kAtlas, L"Create Texture Atlas (RayVPaint)", helpers, icon, L"atlas") && ok;

    // Notify Explorer of association change
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok && IsRegistered();
}

bool Unregister() {
    bool a = DeleteTree(kConvert);
    bool b = DeleteTree(kAtlas);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return a || b;
}

bool IsRegistered() {
    return QueryStatus().fullyOk;
}

} // namespace HelperShellRegister
