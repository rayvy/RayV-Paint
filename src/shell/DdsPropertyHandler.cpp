// RayVPaint DDS Property Handler — IPropertyStore + IInitializeWithStream/File
// CLSID: {D4A1B2C3-5E6F-4789-A012-3456789ABCDE}

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlwapi.h>

#include "DdsHeaderInfo.h"

#include <atomic>
#include <new>
#include <string>
#include <vector>

#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")

// {D4A1B2C3-5E6F-4789-A012-3456789ABCDE}
// Not file-static: DllGetClassObject in DdsThumbnailHandler.cpp references this.
extern const CLSID CLSID_RayVDdsProps =
    { 0xD4A1B2C3, 0x5E6F, 0x4789, { 0xA0, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE } };

// Custom property formatID (matches RayVPaint.Dds.propdesc)
// {8F4C2A10-9B7E-4D61-A5E2-3C1D0F9E8B7A}
static const PROPERTYKEY PKEY_RayV_Dds_Format =
    { { 0x8F4C2A10, 0x9B7E, 0x4D61, { 0xA5, 0xE2, 0x3C, 0x1D, 0x0F, 0x9E, 0x8B, 0x7A } }, 2 };
static const PROPERTYKEY PKEY_RayV_Dds_MipCount =
    { { 0x8F4C2A10, 0x9B7E, 0x4D61, { 0xA5, 0xE2, 0x3C, 0x1D, 0x0F, 0x9E, 0x8B, 0x7A } }, 3 };
static const PROPERTYKEY PKEY_RayV_Dds_Flags =
    { { 0x8F4C2A10, 0x9B7E, 0x4D61, { 0xA5, 0xE2, 0x3C, 0x1D, 0x0F, 0x9E, 0x8B, 0x7A } }, 4 };
static const PROPERTYKEY PKEY_RayV_Dds_Dxgi =
    { { 0x8F4C2A10, 0x9B7E, 0x4D61, { 0xA5, 0xE2, 0x3C, 0x1D, 0x0F, 0x9E, 0x8B, 0x7A } }, 5 };

extern std::atomic<long> g_shellObjCount;
extern std::atomic<long> g_shellServerLocks;

static bool KeyEq(const PROPERTYKEY& a, const PROPERTYKEY& b) {
    return a.pid == b.pid && IsEqualIID(a.fmtid, b.fmtid);
}

static HRESULT ReadStreamHead(IStream* stream, std::vector<uint8_t>& out) {
    out.clear();
    if (!stream) return E_INVALIDARG;
    LARGE_INTEGER zero = {};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    out.resize(256);
    ULONG got = 0;
    HRESULT hr = stream->Read(out.data(), (ULONG)out.size(), &got);
    if (FAILED(hr)) return hr;
    out.resize(got);
    return out.size() >= 128 ? S_OK : E_FAIL;
}

// ---------------------------------------------------------------------------

class DdsPropertyStore final :
    public IInitializeWithStream,
    public IInitializeWithFile,
    public IPropertyStore,
    public IPropertyStoreCapabilities {
public:
    DdsPropertyStore() { g_shellObjCount.fetch_add(1); }
    ~DdsPropertyStore() { g_shellObjCount.fetch_sub(1); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IInitializeWithStream)
            *ppv = static_cast<IInitializeWithStream*>(this);
        else if (riid == IID_IInitializeWithFile)
            *ppv = static_cast<IInitializeWithFile*>(this);
        else if (riid == IID_IPropertyStore)
            *ppv = static_cast<IPropertyStore*>(this);
        else if (riid == IID_IPropertyStoreCapabilities)
            *ppv = static_cast<IPropertyStoreCapabilities*>(this);
        else
            return E_NOINTERFACE;
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

    IFACEMETHODIMP Initialize(IStream* pstream, DWORD /*grfMode*/) override {
        if (m_ready) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        std::vector<uint8_t> buf;
        HRESULT hr = ReadStreamHead(pstream, buf);
        if (FAILED(hr)) return hr;
        if (!ddsinfo::Parse(buf.data(), buf.size(), m_info))
            return E_FAIL;
        m_ready = true;
        return S_OK;
    }

    IFACEMETHODIMP Initialize(LPCWSTR pszFilePath, DWORD /*grfMode*/) override {
        if (m_ready) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        if (!pszFilePath || !*pszFilePath) return E_INVALIDARG;
        if (!ddsinfo::ParseFile(pszFilePath, m_info))
            return E_FAIL;
        m_ready = true;
        return S_OK;
    }

    IFACEMETHODIMP GetCount(DWORD* cProps) override {
        if (!cProps) return E_POINTER;
        *cProps = m_ready ? (DWORD)kKeysCount : 0;
        return S_OK;
    }

    IFACEMETHODIMP GetAt(DWORD iProp, PROPERTYKEY* pkey) override {
        if (!pkey) return E_POINTER;
        if (!m_ready || iProp >= kKeysCount) return E_INVALIDARG;
        *pkey = kKeys[iProp];
        return S_OK;
    }

    IFACEMETHODIMP GetValue(REFPROPERTYKEY key, PROPVARIANT* pv) override {
        if (!pv) return E_POINTER;
        PropVariantInit(pv);
        if (!m_ready) return E_UNEXPECTED;

        if (KeyEq(key, PKEY_Image_HorizontalSize))
            return InitPropVariantFromUInt32((ULONG)m_info.width, pv);
        if (KeyEq(key, PKEY_Image_VerticalSize))
            return InitPropVariantFromUInt32((ULONG)m_info.height, pv);
        if (KeyEq(key, PKEY_Image_Dimensions)) {
            wchar_t buf[64];
            swprintf_s(buf, L"%d x %d", m_info.width, m_info.height);
            return InitPropVariantFromString(buf, pv);
        }
        if (KeyEq(key, PKEY_Image_BitDepth) && m_info.bitDepth > 0)
            return InitPropVariantFromUInt32((ULONG)m_info.bitDepth, pv);

        if (KeyEq(key, PKEY_RayV_Dds_Format)) {
            std::wstring w(m_info.formatLabel.begin(), m_info.formatLabel.end());
            return InitPropVariantFromString(w.c_str(), pv);
        }
        if (KeyEq(key, PKEY_RayV_Dds_MipCount))
            return InitPropVariantFromUInt32((ULONG)m_info.mipCount, pv);
        if (KeyEq(key, PKEY_RayV_Dds_Flags)) {
            std::wstring f;
            if (m_info.srgb) f += L"sRGB";
            if (m_info.isCube) {
                if (!f.empty()) f += L", ";
                f += L"Cube";
            }
            if (m_info.isVolume) {
                if (!f.empty()) f += L", ";
                f += L"Volume";
            }
            if (m_info.arraySize > 1) {
                if (!f.empty()) f += L", ";
                f += L"Array x" + std::to_wstring(m_info.arraySize);
            }
            if (m_info.isDx10) {
                if (!f.empty()) f += L", ";
                f += L"DX10";
            }
            if (f.empty()) f = L"—";
            return InitPropVariantFromString(f.c_str(), pv);
        }
        if (KeyEq(key, PKEY_RayV_Dds_Dxgi) && m_info.dxgiFormat != 0)
            return InitPropVariantFromUInt32((ULONG)m_info.dxgiFormat, pv);

        // Not our key — empty (not an error)
        return S_OK;
    }

    IFACEMETHODIMP SetValue(REFPROPERTYKEY /*key*/, REFPROPVARIANT /*propvar*/) override {
        return STG_E_ACCESSDENIED;
    }
    IFACEMETHODIMP Commit() override { return S_OK; }

    IFACEMETHODIMP IsPropertyWritable(REFPROPERTYKEY /*key*/) override {
        return S_FALSE; // read-only
    }

private:
    static constexpr DWORD kKeysCount = 8;
    static const PROPERTYKEY kKeys[kKeysCount];

    std::atomic<long> m_ref{1};
    bool m_ready = false;
    ddsinfo::Info m_info;
};

const PROPERTYKEY DdsPropertyStore::kKeys[DdsPropertyStore::kKeysCount] = {
    PKEY_Image_HorizontalSize,
    PKEY_Image_VerticalSize,
    PKEY_Image_Dimensions,
    PKEY_Image_BitDepth,
    PKEY_RayV_Dds_Format,
    PKEY_RayV_Dds_MipCount,
    PKEY_RayV_Dds_Flags,
    PKEY_RayV_Dds_Dxgi,
};

// Factory used by DllGetClassObject
class DdsPropsClassFactory final : public IClassFactory {
public:
    DdsPropsClassFactory() { g_shellObjCount.fetch_add(1); }
    ~DdsPropsClassFactory() { g_shellObjCount.fetch_sub(1); }

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
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        auto* obj = new (std::nothrow) DdsPropertyStore();
        if (!obj) return E_OUTOFMEMORY;
        HRESULT hr = obj->QueryInterface(riid, ppv);
        obj->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL fLock) override {
        if (fLock) g_shellServerLocks.fetch_add(1);
        else g_shellServerLocks.fetch_sub(1);
        return S_OK;
    }

private:
    std::atomic<long> m_ref{1};
};

// Called from DllGetClassObject
HRESULT CreateDdsPropsClassFactory(REFIID riid, void** ppv) {
    auto* f = new (std::nothrow) DdsPropsClassFactory();
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}
