// RayVPaint DDS Thumbnail Handler — COM in-proc server for Windows Explorer.
// Implements IInitializeWithStream + IThumbnailProvider.
//
// CLSID: {B5E8A1C2-4F3D-4A9E-9C1B-7D6E5F4A3B2C}
// ShellEx: {e357fccd-a995-4576-b01f-234630154e96}  (MS thumbnail provider)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <propkey.h>

#include "DdsThumbDecode.h"

#include <algorithm>
#include <atomic>
#include <new>
#include <string>
#include <vector>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "gdi32.lib")

// {B5E8A1C2-4F3D-4A9E-9C1B-7D6E5F4A3B2C}
static const CLSID CLSID_RayVDdsThumb =
    { 0xB5E8A1C2, 0x4F3D, 0x4A9E, { 0x9C, 0x1B, 0x7D, 0x6E, 0x5F, 0x4A, 0x3B, 0x2C } };

static const wchar_t* kClsidStr = L"{B5E8A1C2-4F3D-4A9E-9C1B-7D6E5F4A3B2C}";
// Microsoft thumbnail handler ShellEx GUID
static const wchar_t* kThumbShellEx = L"{e357fccd-a995-4576-b01f-234630154e96}";
static const wchar_t* kHandlerName = L"RayVPaint DDS Thumbnail Handler";

static HINSTANCE g_hInst = nullptr;
static std::atomic<long> g_serverLocks{0};
static std::atomic<long> g_objCount{0};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::wstring ModulePath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(g_hInst, buf, MAX_PATH);
    return buf;
}

static HRESULT ReadStreamAll(IStream* stream, std::vector<uint8_t>& out) {
    if (!stream) return E_INVALIDARG;
    out.clear();

    STATSTG st = {};
    HRESULT hr = stream->Stat(&st, STATFLAG_NONAME);
    if (SUCCEEDED(hr) && st.cbSize.QuadPart > 0 && st.cbSize.QuadPart < (ULONGLONG)512 * 1024 * 1024) {
        out.resize((size_t)st.cbSize.QuadPart);
        ULONG got = 0;
        LARGE_INTEGER zero = {};
        stream->Seek(zero, STREAM_SEEK_SET, nullptr);
        hr = stream->Read(out.data(), (ULONG)out.size(), &got);
        if (FAILED(hr)) return hr;
        out.resize(got);
        return out.empty() ? E_FAIL : S_OK;
    }

    // Fallback: chunked read
    const ULONG chunk = 1u << 20;
    std::vector<uint8_t> buf(chunk);
    LARGE_INTEGER zero = {};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    for (;;) {
        ULONG got = 0;
        hr = stream->Read(buf.data(), chunk, &got);
        if (FAILED(hr)) return hr;
        if (got == 0) break;
        out.insert(out.end(), buf.begin(), buf.begin() + got);
        if (out.size() > 512u * 1024u * 1024u)
            return E_OUTOFMEMORY;
    }
    return out.empty() ? E_FAIL : S_OK;
}

static HBITMAP RgbaToHBitmap(const uint8_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return nullptr;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP hbmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!hbmp || !bits) {
        if (hbmp) DeleteObject(hbmp);
        return nullptr;
    }

    // Windows DIB is BGRA
    auto* dst = static_cast<uint8_t*>(bits);
    const size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; ++i) {
        dst[i * 4 + 0] = rgba[i * 4 + 2]; // B
        dst[i * 4 + 1] = rgba[i * 4 + 1]; // G
        dst[i * 4 + 2] = rgba[i * 4 + 0]; // R
        dst[i * 4 + 3] = rgba[i * 4 + 3]; // A
    }
    return hbmp;
}

// ---------------------------------------------------------------------------
// Thumbnail provider
// ---------------------------------------------------------------------------

class DdsThumbnailProvider final :
    public IInitializeWithStream,
    public IInitializeWithFile,
    public IThumbnailProvider {
public:
    DdsThumbnailProvider() { g_objCount.fetch_add(1); }
    ~DdsThumbnailProvider() { g_objCount.fetch_sub(1); }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IInitializeWithStream) {
            *ppv = static_cast<IInitializeWithStream*>(this);
        } else if (riid == IID_IInitializeWithFile) {
            *ppv = static_cast<IInitializeWithFile*>(this);
        } else if (riid == IID_IThumbnailProvider) {
            *ppv = static_cast<IThumbnailProvider*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override {
        return (ULONG)m_ref.fetch_add(1) + 1;
    }
    IFACEMETHODIMP_(ULONG) Release() override {
        long r = m_ref.fetch_sub(1) - 1;
        if (r == 0) delete this;
        return (ULONG)r;
    }

    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream* pstream, DWORD /*grfMode*/) override {
        if (!pstream) return E_INVALIDARG;
        if (m_initialized) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        HRESULT hr = ReadStreamAll(pstream, m_data);
        if (FAILED(hr)) return hr;
        m_initialized = true;
        return S_OK;
    }

    // IInitializeWithFile — some Explorer paths use a path instead of a stream
    IFACEMETHODIMP Initialize(LPCWSTR pszFilePath, DWORD /*grfMode*/) override {
        if (!pszFilePath || !*pszFilePath)
            return E_INVALIDARG;
        if (m_initialized) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);

        HANDLE h = CreateFileW(pszFilePath, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return HRESULT_FROM_WIN32(GetLastError());

        LARGE_INTEGER li = {};
        if (!GetFileSizeEx(h, &li) || li.QuadPart <= 0 || li.QuadPart > (LONGLONG)512 * 1024 * 1024) {
            CloseHandle(h);
            return E_FAIL;
        }
        m_data.resize((size_t)li.QuadPart);
        DWORD got = 0;
        BOOL ok = ReadFile(h, m_data.data(), (DWORD)m_data.size(), &got, nullptr);
        CloseHandle(h);
        if (!ok || got == 0) {
            m_data.clear();
            return E_FAIL;
        }
        m_data.resize(got);
        m_initialized = true;
        return S_OK;
    }

    // IThumbnailProvider
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) override {
        if (!phbmp) return E_POINTER;
        *phbmp = nullptr;
        if (pdwAlpha) *pdwAlpha = WTSAT_ARGB;
        if (!m_initialized || m_data.empty()) return E_UNEXPECTED;
        if (cx == 0) cx = 256;
        if (cx > 1024) cx = 1024;

        ddsthumb::RgbaImage img;
        // Decode a mip near 2x requested size for sharper downscale
        const int target = (int)((std::min)(cx * 2u, 512u));
        if (!ddsthumb::DecodeDdsToRgba8(m_data.data(), m_data.size(), target, img))
            return E_FAIL;

        std::vector<uint8_t> scaled;
        // Keep aspect ratio inside cx x cx
        int tw = (int)cx, th = (int)cx;
        if (img.width > 0 && img.height > 0) {
            if (img.width >= img.height) {
                tw = (int)cx;
                th = (std::max)(1, (int)((int64_t)cx * img.height / img.width));
            } else {
                th = (int)cx;
                tw = (std::max)(1, (int)((int64_t)cx * img.width / img.height));
            }
        }
        ddsthumb::ScaleRgba8(img, tw, th, scaled);

        // Center on cx x cx canvas with transparent padding (Explorer expects square)
        std::vector<uint8_t> square((size_t)cx * (size_t)cx * 4u, 0);
        const int ox = ((int)cx - tw) / 2;
        const int oy = ((int)cx - th) / 2;
        for (int y = 0; y < th; ++y) {
            for (int x = 0; x < tw; ++x) {
                const size_t si = ((size_t)y * tw + x) * 4;
                const size_t di = ((size_t)(y + oy) * cx + (x + ox)) * 4;
                square[di+0] = scaled[si+0];
                square[di+1] = scaled[si+1];
                square[di+2] = scaled[si+2];
                square[di+3] = scaled[si+3];
            }
        }

        HBITMAP hbmp = RgbaToHBitmap(square.data(), (int)cx, (int)cx);
        if (!hbmp) return E_OUTOFMEMORY;
        *phbmp = hbmp;
        return S_OK;
    }

private:
    std::atomic<long> m_ref{1};
    bool m_initialized = false;
    std::vector<uint8_t> m_data;
};

// ---------------------------------------------------------------------------
// Class factory
// ---------------------------------------------------------------------------

class ClassFactory final : public IClassFactory {
public:
    ClassFactory() { g_objCount.fetch_add(1); }
    ~ClassFactory() { g_objCount.fetch_sub(1); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override {
        return (ULONG)m_ref.fetch_add(1) + 1;
    }
    IFACEMETHODIMP_(ULONG) Release() override {
        long r = m_ref.fetch_sub(1) - 1;
        if (r == 0) delete this;
        return (ULONG)r;
    }
    IFACEMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        auto* obj = new (std::nothrow) DdsThumbnailProvider();
        if (!obj) return E_OUTOFMEMORY;
        HRESULT hr = obj->QueryInterface(riid, ppv);
        obj->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL fLock) override {
        if (fLock) g_serverLocks.fetch_add(1);
        else g_serverLocks.fetch_sub(1);
        return S_OK;
    }

private:
    std::atomic<long> m_ref{1};
};

// ---------------------------------------------------------------------------
// Registry helpers
//
// CRITICAL (Win10/11 thumbnail host):
//   IShellItemImageFactory / Explorer only load thumbnail handler CLSIDs that
//   are registered under HKLM (like Paint.NET). HKCU-only CLSID → REGDB_E_CLASSNOTREG
//   → fallback to default-app icon.
//
//   Also: NEVER put ShellEx on Applications\YourApp.exe — UserChoice uses that
//   ProgId for every file type the app opens (dds+png share one handler).
// ---------------------------------------------------------------------------

static HRESULT SetRegSz(HKEY root, const wchar_t* sub, const wchar_t* name, const wchar_t* value) {
    HKEY key = nullptr;
    LONG rc = RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) return HRESULT_FROM_WIN32(rc);
    rc = RegSetValueExW(key, name, 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(value),
                        (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(rc);
}

static void DeleteTree(HKEY root, const wchar_t* sub) {
    RegDeleteTreeW(root, sub);
}

// Minimal Paint.NET-style CLSID registration under one hive.
static HRESULT RegisterClsidHive(HKEY root, const std::wstring& dll) {
    // root is HKEY_CURRENT_USER or HKEY_LOCAL_MACHINE
    // subkeys are relative to Software\Classes when root is HKCU, or Classes when HKLM via Software\Classes
    const wchar_t* prefix = (root == HKEY_LOCAL_MACHINE)
        ? L"SOFTWARE\\Classes\\"
        : L"Software\\Classes\\";

    std::wstring clsidKey = std::wstring(prefix) + L"CLSID\\" + kClsidStr;
    HRESULT hr = SetRegSz(root, clsidKey.c_str(), nullptr, kHandlerName);
    if (FAILED(hr)) return hr;

    std::wstring inproc = clsidKey + L"\\InprocServer32";
    hr = SetRegSz(root, inproc.c_str(), nullptr, dll.c_str());
    if (FAILED(hr)) return hr;
    return SetRegSz(root, inproc.c_str(), L"ThreadingModel", L"Apartment");
}

static HRESULT BindExtThumb(HKEY root, const wchar_t* extOrProg, const wchar_t* handlerClsid) {
    const wchar_t* prefix = (root == HKEY_LOCAL_MACHINE)
        ? L"SOFTWARE\\Classes\\"
        : L"Software\\Classes\\";
    std::wstring path = std::wstring(prefix) + extOrProg + L"\\ShellEx\\" + kThumbShellEx;
    return SetRegSz(root, path.c_str(), nullptr, handlerClsid);
}

// Windows Photo Thumbnail Provider — restores PNG/JPG after Google Drive hijack
static const wchar_t* kPhotoThumbClsid = L"{C7657C4A-9F68-40fa-A4DF-96BC08EB3551}";

static void RestoreStandardImageThumbs() {
    // HKCU overrides HKCR (DriveFS often overwrites .png ShellEx machine-wide)
    static const wchar_t* kExts[] = {
        L".png", L".jpg", L".jpeg", L".jfif", L".bmp", L".gif",
        L".tif", L".tiff", L".webp", L".ico", L".jpe"
    };
    for (const wchar_t* e : kExts)
        BindExtThumb(HKEY_CURRENT_USER, e, kPhotoThumbClsid);
}

static void RemoveApplicationsShellExPollution() {
    // Shared ProgId for every type opened by RayVPaint — must NOT carry a thumb handler
    static const wchar_t* kApps[] = {
        L"Applications\\RayVPaint_Core.exe",
        L"Applications\\RayVPaint.exe",
        L"Applications\\rayvpaint_core.exe",
        L"Applications\\rayvpaint.exe",
    };
    for (const wchar_t* a : kApps) {
        DeleteTree(HKEY_CURRENT_USER,
            (std::wstring(L"Software\\Classes\\") + a + L"\\ShellEx").c_str());
        DeleteTree(HKEY_LOCAL_MACHINE,
            (std::wstring(L"SOFTWARE\\Classes\\") + a + L"\\ShellEx").c_str());
    }
}

static HRESULT RegisterHandler() {
    const std::wstring dll = ModulePath();
    if (dll.empty()) return E_FAIL;

    // 1) Always write HKCU (extension bindings + fallback)
    HRESULT hrCu = RegisterClsidHive(HKEY_CURRENT_USER, dll);
    BindExtThumb(HKEY_CURRENT_USER, L".dds", kClsidStr);
    BindExtThumb(HKEY_CURRENT_USER, L"ddsfile", kClsidStr);
    BindExtThumb(HKEY_CURRENT_USER, L"RayVPaint.dds", kClsidStr);
    SetRegSz(HKEY_CURRENT_USER, L"Software\\Classes\\.dds", L"PerceivedType", L"image");
    SetRegSz(HKEY_CURRENT_USER, L"Software\\Classes\\.dds", L"Content Type", L"image/vnd-ms.dds");
    SetRegSz(HKEY_CURRENT_USER,
             L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\KindMap",
             L".dds", L"Picture");

    // 2) HKLM is REQUIRED for Explorer thumbnail host (needs admin once)
    HRESULT hrLm = RegisterClsidHive(HKEY_LOCAL_MACHINE, dll);
    if (SUCCEEDED(hrLm)) {
        BindExtThumb(HKEY_LOCAL_MACHINE, L".dds", kClsidStr);
        BindExtThumb(HKEY_LOCAL_MACHINE, L"ddsfile", kClsidStr);
    }

    // 3) Do not pollute Applications\*.exe ShellEx
    RemoveApplicationsShellExPollution();

    // 4) Fix PNG/JPG thumbs hijacked by Google Drive File Stream
    RestoreStandardImageThumbs();

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    // Success if HKLM ok (real Explorer path). HKCU-only still returns S_FALSE-ish via custom.
    if (FAILED(hrLm)) {
        // Still partially useful; caller should elevate and retry
        return SUCCEEDED(hrCu) ? HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) : hrCu;
    }
    return S_OK;
}

static HRESULT UnregisterHandler() {
    DeleteTree(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\CLSID\\") + kClsidStr).c_str());
    DeleteTree(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\AppID\\") + kClsidStr).c_str());
    DeleteTree(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\.dds\\ShellEx\\") + kThumbShellEx).c_str());
    DeleteTree(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\RayVPaint.dds\\ShellEx\\") + kThumbShellEx).c_str());
    DeleteTree(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\ddsfile\\ShellEx\\") + kThumbShellEx).c_str());
    DeleteTree(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsidStr).c_str());
    DeleteTree(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\.dds\\ShellEx\\") + kThumbShellEx).c_str());
    DeleteTree(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\ddsfile\\ShellEx\\") + kThumbShellEx).c_str());
    RemoveApplicationsShellExPollution();
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

// ---------------------------------------------------------------------------
// DLL exports
// ---------------------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

STDAPI DllCanUnloadNow() {
    if (g_serverLocks.load() == 0 && g_objCount.load() == 0)
        return S_OK;
    return S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (rclsid != CLSID_RayVDdsThumb)
        return CLASS_E_CLASSNOTAVAILABLE;
    auto* factory = new (std::nothrow) ClassFactory();
    if (!factory) return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllRegisterServer() {
    return RegisterHandler();
}

STDAPI DllUnregisterServer() {
    return UnregisterHandler();
}

// Optional: call from main app without regsvr32
extern "C" __declspec(dllexport) HRESULT __stdcall RayV_RegisterDdsThumbnails() {
    return RegisterHandler();
}
extern "C" __declspec(dllexport) HRESULT __stdcall RayV_UnregisterDdsThumbnails() {
    return UnregisterHandler();
}
