#include "SingleInstance.h"
#include "PathUtil.h"
#include "Logger.h"

#include <cstring>

namespace SingleInstance {

namespace {
constexpr wchar_t kMutexName[] = L"Local\\RayVPaint_SingleInstance_v1";
constexpr wchar_t kPropName[] = L"RayVPaintMainWindow";
constexpr wchar_t kMsgName[] = L"RayVPaint_OpenPath_v1";
// COPYDATA magic
constexpr DWORD kCopyDataMagic = 0x52565031; // 'RVP1'

struct EnumFindData {
    HWND found = nullptr;
};

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lParam) {
    auto* data = reinterpret_cast<EnumFindData*>(lParam);
    HANDLE prop = GetPropW(hwnd, kPropName);
    if (prop) {
        data->found = hwnd;
        return FALSE; // stop
    }
    return TRUE;
}

HWND FindPrimaryWindow() {
    EnumFindData data;
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&data));
    return data.found;
}
} // namespace

unsigned int GetOpenPathMessage() {
    static unsigned int s_msg = 0;
    if (!s_msg)
        s_msg = RegisterWindowMessageW(kMsgName);
    return s_msg;
}

const wchar_t* GetWindowPropName() { return kPropName; }
const wchar_t* GetMutexName() { return kMutexName; }

bool TryBecomePrimary(HANDLE& outMutex, HWND& existingHwnd) {
    outMutex = nullptr;
    existingHwnd = nullptr;

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!mutex) {
        return true; // fail open — allow run
    }

    const DWORD err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        outMutex = nullptr;
        // Primary may still be creating the window — retry briefly.
        for (int i = 0; i < 50; ++i) {
            existingHwnd = FindPrimaryWindow();
            if (existingHwnd) break;
            Sleep(20);
        }
        return false;
    }

    outMutex = mutex;
    return true;
}

void RegisterMainWindow(HWND hwnd) {
    if (!hwnd) return;
    SetPropW(hwnd, kPropName, (HANDLE)1);
}

void UnregisterMainWindow(HWND hwnd) {
    if (!hwnd) return;
    RemovePropW(hwnd, kPropName);
}

bool SendOpenPath(HWND target, const std::string& utf8Path) {
    if (!target || utf8Path.empty()) return false;

    COPYDATASTRUCT cds{};
    cds.dwData = kCopyDataMagic;
    cds.cbData = static_cast<DWORD>(utf8Path.size() + 1);
    cds.lpData = (void*)utf8Path.data();

    // SendMessage is synchronous — primary copies the string before return.
    LRESULT lr = SendMessageW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds));
    return lr != 0;
}

void FocusWindow(HWND hwnd) {
    if (!hwnd) return;
    if (IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
}

std::string ParseCopyData(const COPYDATASTRUCT* cds) {
    if (!cds || cds->dwData != kCopyDataMagic || !cds->lpData || cds->cbData == 0)
        return {};
    const char* p = static_cast<const char*>(cds->lpData);
    size_t n = cds->cbData;
    // Exclude trailing null if present
    if (n > 0 && p[n - 1] == '\0')
        --n;
    return std::string(p, p + n);
}

bool GuardStartup(int argc, char** argv, bool allowMulti, HANDLE& outMutex) {
    outMutex = nullptr;
    if (allowMulti)
        return true;

    HWND existing = nullptr;
    if (TryBecomePrimary(outMutex, existing))
        return true;

    // Secondary instance: forward file arguments, focus primary, exit.
    if (!existing) {
        // Primary not ready — still exit to avoid double device; user can retry.
        return false;
    }

    FocusWindow(existing);

    bool sentAny = false;
    for (int i = 1; i < argc; ++i) {
        if (!argv[i] || argv[i][0] == '-') continue;
        // skip known flag values that look like paths already consumed by primary CLI —
        // only bare paths are documents.
        std::string path = PathUtil::NormalizeToUtf8Path(argv[i]);
        if (path.empty()) continue;
        if (SendOpenPath(existing, path))
            sentAny = true;
    }

    // No path: just focus the existing editor (double-click shortcut).
    (void)sentAny;
    return false;
}

} // namespace SingleInstance
