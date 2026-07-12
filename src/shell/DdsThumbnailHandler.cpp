// RayVPaint DDS Shell COM server (Explorer):
//   Thumbnail:  IThumbnailProvider     CLSID {B5E8A1C2-4F3D-4A9E-9C1B-7D6E5F4A3B2C}
//   Properties: IPropertyStore         CLSID {D4A1B2C3-5E6F-4789-A012-3456789ABCDE}
// Also registers ProgID, KindMap, InfoTip, propdesc schema — teaches Windows about .dds.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <thumbcache.h>
#include <propkey.h>
#include <propsys.h>

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
#pragma comment(lib, "propsys.lib")

// {B5E8A1C2-4F3D-4A9E-9C1B-7D6E5F4A3B2C}
static const CLSID CLSID_RayVDdsThumb =
    { 0xB5E8A1C2, 0x4F3D, 0x4A9E, { 0x9C, 0x1B, 0x7D, 0x6E, 0x5F, 0x4A, 0x3B, 0x2C } };
// {D4A1B2C3-5E6F-4789-A012-3456789ABCDE}
extern const CLSID CLSID_RayVDdsProps;

static const wchar_t* kClsidThumbStr = L"{B5E8A1C2-4F3D-4A9E-9C1B-7D6E5F4A3B2C}";
static const wchar_t* kClsidPropsStr = L"{D4A1B2C3-5E6F-4789-A012-3456789ABCDE}";
// Microsoft thumbnail handler ShellEx GUID
static const wchar_t* kThumbShellEx = L"{e357fccd-a995-4576-b01f-234630154e96}";
static const wchar_t* kHandlerName = L"RayVPaint DDS Thumbnail Handler";
static const wchar_t* kPropsName = L"RayVPaint DDS Property Handler";
static const wchar_t* kProgId = L"RayVPaint.dds";

static HINSTANCE g_hInst = nullptr;
// Shared with DdsPropertyHandler.cpp
std::atomic<long> g_shellServerLocks{0};
std::atomic<long> g_shellObjCount{0};

// legacy aliases used in this TU
#define g_serverLocks g_shellServerLocks
#define g_objCount g_shellObjCount

// Forward: property handler factory (DdsPropertyHandler.cpp)
HRESULT CreateDdsPropsClassFactory(REFIID riid, void** ppv);

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

static const wchar_t* ClassesPrefix(HKEY root) {
    return (root == HKEY_LOCAL_MACHINE) ? L"SOFTWARE\\Classes\\" : L"Software\\Classes\\";
}

// Minimal Paint.NET-style CLSID registration under one hive.
static HRESULT RegisterClsidHive(HKEY root, const std::wstring& dll,
                                 const wchar_t* clsidStr, const wchar_t* name) {
    const wchar_t* prefix = ClassesPrefix(root);
    std::wstring clsidKey = std::wstring(prefix) + L"CLSID\\" + clsidStr;
    HRESULT hr = SetRegSz(root, clsidKey.c_str(), nullptr, name);
    if (FAILED(hr)) return hr;
    std::wstring inproc = clsidKey + L"\\InprocServer32";
    hr = SetRegSz(root, inproc.c_str(), nullptr, dll.c_str());
    if (FAILED(hr)) return hr;
    return SetRegSz(root, inproc.c_str(), L"ThreadingModel", L"Apartment");
}

static HRESULT BindExtThumb(HKEY root, const wchar_t* extOrProg, const wchar_t* handlerClsid) {
    std::wstring path = std::wstring(ClassesPrefix(root)) + extOrProg +
                        L"\\ShellEx\\" + kThumbShellEx;
    return SetRegSz(root, path.c_str(), nullptr, handlerClsid);
}

static std::wstring SiblingExePath() {
    // Prefer RayVPaint.exe one level up from bin\, else Core in same folder
    std::wstring dll = ModulePath();
    size_t slash = dll.find_last_of(L"\\/");
    std::wstring dir = (slash == std::wstring::npos) ? L"." : dll.substr(0, slash);
    std::wstring core = dir + L"\\RayVPaint_Core.exe";
    std::wstring launcher = dir + L"\\..\\RayVPaint.exe";
    DWORD a = GetFileAttributesW(launcher.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY))
        return launcher;
    return core;
}

static void RegisterPropdescSchema(const std::wstring& dllDir) {
    std::wstring propdesc = dllDir + L"\\RayVPaint.Dds.propdesc";
    if (GetFileAttributesW(propdesc.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;
    HRESULT hr = PSRegisterPropertySchema(propdesc.c_str());
    // S_OK or already registered — both fine. Log only hard failures.
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
        // Can't use app Logger from shell DLL reliably — silent
        (void)hr;
    }
}

static void UnregisterPropdescSchema(const std::wstring& dllDir) {
    std::wstring propdesc = dllDir + L"\\RayVPaint.Dds.propdesc";
    PSUnregisterPropertySchema(propdesc.c_str());
}

// Teach Windows that .dds is a real image type with our ProgID + details columns.
static void RegisterDdsFileType(HKEY root, const std::wstring& dll) {
    const wchar_t* p = ClassesPrefix(root);
    std::wstring exe = SiblingExePath();
    std::wstring icon = L"\"" + exe + L"\",0";
    std::wstring openCmd = L"\"" + exe + L"\" \"%1\"";

    // .dds → RayVPaint.dds
    SetRegSz(root, (std::wstring(p) + L".dds").c_str(), nullptr, kProgId);
    SetRegSz(root, (std::wstring(p) + L".dds").c_str(), L"PerceivedType", L"image");
    SetRegSz(root, (std::wstring(p) + L".dds").c_str(), L"Content Type", L"image/vnd-ms.dds");
    // OpenWithProgids entry
    SetRegSz(root, (std::wstring(p) + L".dds\\OpenWithProgids").c_str(), kProgId, L"");

    // ProgID
    std::wstring pid = std::wstring(p) + kProgId;
    SetRegSz(root, pid.c_str(), nullptr, L"DDS Texture");
    SetRegSz(root, pid.c_str(), L"FriendlyTypeName", L"DDS Texture");
    SetRegSz(root, (pid + L"\\DefaultIcon").c_str(), nullptr, icon.c_str());
    SetRegSz(root, (pid + L"\\shell\\open\\command").c_str(), nullptr, openCmd.c_str());

    // Tooltip + Details pane (system image props + our custom ones)
    SetRegSz(root, pid.c_str(), L"InfoTip",
             L"prop:System.ItemType;System.Image.Dimensions;RayV.Dds.Format;RayV.Dds.Dxgi;"
             L"RayV.Dds.MipCount;RayV.Dds.Flags;System.Size");
    SetRegSz(root, pid.c_str(), L"FullDetails",
             L"prop:System.PropGroup.Image;System.Image.Dimensions;System.Image.HorizontalSize;"
             L"System.Image.VerticalSize;System.Image.BitDepth;RayV.Dds.Format;RayV.Dds.Dxgi;"
             L"RayV.Dds.MipCount;RayV.Dds.Flags;System.PropGroup.FileSystem;System.ItemNameDisplay;"
             L"System.ItemType;System.ItemFolderPathDisplay;System.DateCreated;System.DateModified;"
             L"System.Size");
    SetRegSz(root, pid.c_str(), L"PreviewDetails",
             L"prop:System.Image.Dimensions;RayV.Dds.Format;RayV.Dds.Dxgi;RayV.Dds.MipCount;System.Size");
    SetRegSz(root, pid.c_str(), L"ExtendedTileInfo",
             L"prop:System.ItemType;System.Image.Dimensions;RayV.Dds.Format;*System.Size");

    // Thumbnail on ProgID (extension binding also set)
    BindExtThumb(root, kProgId, kClsidThumbStr);

    // Property handler for .dds (system list under PropertySystem)
    // HKLM path preferred; also try Classes\.dds shell extension style under HKCU
    if (root == HKEY_LOCAL_MACHINE) {
        SetRegSz(HKEY_LOCAL_MACHINE,
                 L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertyHandlers\\.dds",
                 nullptr, kClsidPropsStr);
    } else {
        // HKCU override (works for current user when HKLM blocked)
        SetRegSz(HKEY_CURRENT_USER,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertyHandlers\\.dds",
                 nullptr, kClsidPropsStr);
    }

    // KindMap → Picture (Explorer "Pictures" filters / photo chrome)
    if (root == HKEY_LOCAL_MACHINE) {
        SetRegSz(HKEY_LOCAL_MACHINE,
                 L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\KindMap",
                 L".dds", L"Picture");
    } else {
        SetRegSz(HKEY_CURRENT_USER,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\KindMap",
                 L".dds", L"Picture");
    }

    // Also keep legacy ddsfile ProgID informed (some systems still use it)
    SetRegSz(root, (std::wstring(p) + L"ddsfile").c_str(), nullptr, L"DDS Texture");
    SetRegSz(root, (std::wstring(p) + L"ddsfile\\DefaultIcon").c_str(), nullptr, icon.c_str());
    BindExtThumb(root, L"ddsfile", kClsidThumbStr);

    // SystemFileAssociations\.dds — Explorer often reads columns/InfoTip from here
    // even when UserChoice ProgId is Applications\RayVPaint_Core.exe
    const wchar_t* sfaPrefix = (root == HKEY_LOCAL_MACHINE)
        ? L"SOFTWARE\\Classes\\SystemFileAssociations\\.dds"
        : L"Software\\Classes\\SystemFileAssociations\\.dds";
    SetRegSz(root, sfaPrefix, L"InfoTip",
             L"prop:System.ItemType;System.Image.Dimensions;RayV.Dds.Format;RayV.Dds.Dxgi;"
             L"RayV.Dds.MipCount;RayV.Dds.Flags;System.Size");
    SetRegSz(root, sfaPrefix, L"FullDetails",
             L"prop:System.PropGroup.Image;System.Image.Dimensions;System.Image.HorizontalSize;"
             L"System.Image.VerticalSize;RayV.Dds.Format;RayV.Dds.Dxgi;RayV.Dds.MipCount;"
             L"RayV.Dds.Flags;System.PropGroup.FileSystem;System.ItemNameDisplay;System.Size");
    SetRegSz(root, sfaPrefix, L"PreviewDetails",
             L"prop:System.Image.Dimensions;RayV.Dds.Format;RayV.Dds.Dxgi;RayV.Dds.MipCount;System.Size");
    SetRegSz(root, sfaPrefix, L"ExtendedTileInfo",
             L"prop:System.ItemType;System.Image.Dimensions;RayV.Dds.Format;*System.Size");
    SetRegSz(root, sfaPrefix, L"TileInfo",
             L"prop:System.ItemType;System.Image.Dimensions;RayV.Dds.Format");

    // UserChoice default app ProgId (no ShellEx — would break multi-type) but Details columns yes
    static const wchar_t* kAppProgIds[] = {
        L"Applications\\RayVPaint_Core.exe",
        L"Applications\\RayVPaint.exe",
    };
    for (const wchar_t* ap : kAppProgIds) {
        std::wstring base = std::wstring(p) + ap;
        SetRegSz(root, base.c_str(), L"FriendlyTypeName", L"DDS Texture");
        SetRegSz(root, base.c_str(), L"InfoTip",
                 L"prop:System.ItemType;System.Image.Dimensions;RayV.Dds.Format;RayV.Dds.Dxgi;"
                 L"RayV.Dds.MipCount;System.Size");
        SetRegSz(root, base.c_str(), L"FullDetails",
                 L"prop:System.PropGroup.Image;System.Image.Dimensions;RayV.Dds.Format;"
                 L"RayV.Dds.Dxgi;RayV.Dds.MipCount;RayV.Dds.Flags;"
                 L"System.PropGroup.FileSystem;System.ItemNameDisplay;System.Size");
        SetRegSz(root, base.c_str(), L"PreviewDetails",
                 L"prop:System.Image.Dimensions;RayV.Dds.Format;RayV.Dds.Dxgi;System.Size");
    }

    (void)dll;
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
    size_t slash = dll.find_last_of(L"\\/");
    std::wstring dllDir = (slash == std::wstring::npos) ? L"." : dll.substr(0, slash);

    // 1) COM CLSIDs — thumb + property (HKCU always)
    HRESULT hrCu = RegisterClsidHive(HKEY_CURRENT_USER, dll, kClsidThumbStr, kHandlerName);
    RegisterClsidHive(HKEY_CURRENT_USER, dll, kClsidPropsStr, kPropsName);
    BindExtThumb(HKEY_CURRENT_USER, L".dds", kClsidThumbStr);
    RegisterDdsFileType(HKEY_CURRENT_USER, dll);
    RegisterPropdescSchema(dllDir);

    // 2) HKLM required for thumbnail host + machine-wide property handler
    HRESULT hrLm = RegisterClsidHive(HKEY_LOCAL_MACHINE, dll, kClsidThumbStr, kHandlerName);
    if (SUCCEEDED(hrLm)) {
        RegisterClsidHive(HKEY_LOCAL_MACHINE, dll, kClsidPropsStr, kPropsName);
        BindExtThumb(HKEY_LOCAL_MACHINE, L".dds", kClsidThumbStr);
        RegisterDdsFileType(HKEY_LOCAL_MACHINE, dll);
        RegisterPropdescSchema(dllDir);
    }

    // 3) Never put ShellEx on Applications\RayVPaint*.exe
    RemoveApplicationsShellExPollution();

    // 4) Fix PNG/JPG if Google Drive stole them
    RestoreStandardImageThumbs();

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    if (FAILED(hrLm))
        return SUCCEEDED(hrCu) ? HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) : hrCu;
    return S_OK;
}

static HRESULT UnregisterHandler() {
    std::wstring dll = ModulePath();
    size_t slash = dll.find_last_of(L"\\/");
    std::wstring dllDir = (slash == std::wstring::npos) ? L"." : dll.substr(0, slash);
    UnregisterPropdescSchema(dllDir);

    auto wipe = [](HKEY root, const wchar_t* prefix) {
        DeleteTree(root, (std::wstring(prefix) + L"CLSID\\" + kClsidThumbStr).c_str());
        DeleteTree(root, (std::wstring(prefix) + L"CLSID\\" + kClsidPropsStr).c_str());
        DeleteTree(root, (std::wstring(prefix) + L".dds\\ShellEx\\" + kThumbShellEx).c_str());
        DeleteTree(root, (std::wstring(prefix) + L"ddsfile\\ShellEx\\" + kThumbShellEx).c_str());
        DeleteTree(root, (std::wstring(prefix) + kProgId).c_str());
    };
    wipe(HKEY_CURRENT_USER, L"Software\\Classes\\");
    wipe(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\");
    DeleteTree(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertyHandlers\\.dds");
    DeleteTree(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertyHandlers\\.dds");
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
    if (rclsid == CLSID_RayVDdsThumb) {
        auto* factory = new (std::nothrow) ClassFactory();
        if (!factory) return E_OUTOFMEMORY;
        HRESULT hr = factory->QueryInterface(riid, ppv);
        factory->Release();
        return hr;
    }
    if (rclsid == CLSID_RayVDdsProps)
        return CreateDdsPropsClassFactory(riid, ppv);
    return CLASS_E_CLASSNOTAVAILABLE;
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
