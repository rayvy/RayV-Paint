#include "PngIo.h"
#include "../common/Utf8.h"

#include <windows.h>
#include <wincodec.h>
#include <shobjidl.h>
#include <vector>
#include <comdef.h>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace helpers {
namespace {

struct ComInit {
    HRESULT hr;
    ComInit() { hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ~ComInit() { if (SUCCEEDED(hr)) CoUninitialize(); }
    bool ok() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

template <typename T>
void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

} // namespace

bool LoadPng(const std::string& utf8Path, RgbaImage& out, std::string* err) {
    out = {};
    ComInit com;
    if (!com.ok()) {
        if (err) *err = "COM init failed";
        return false;
    }

    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        if (err) *err = "WIC factory failed";
        return false;
    }

    std::wstring path = Utf8ToWide(utf8Path);
    IWICBitmapDecoder* decoder = nullptr;
    hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr) || !decoder) {
        SafeRelease(factory);
        if (err) *err = "Cannot open image";
        return false;
    }

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) {
        SafeRelease(decoder);
        SafeRelease(factory);
        if (err) *err = "Cannot read frame";
        return false;
    }

    IWICFormatConverter* conv = nullptr;
    hr = factory->CreateFormatConverter(&conv);
    if (FAILED(hr) || !conv) {
        SafeRelease(frame);
        SafeRelease(decoder);
        SafeRelease(factory);
        if (err) *err = "Format converter failed";
        return false;
    }

    hr = conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                          WICBitmapDitherTypeNone, nullptr, 0.0,
                          WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        SafeRelease(conv);
        SafeRelease(frame);
        SafeRelease(decoder);
        SafeRelease(factory);
        if (err) *err = "Convert to RGBA failed";
        return false;
    }

    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (w == 0 || h == 0 || w > 16384 || h > 16384) {
        SafeRelease(conv);
        SafeRelease(frame);
        SafeRelease(decoder);
        SafeRelease(factory);
        if (err) *err = "Invalid image size";
        return false;
    }

    out.width = (int)w;
    out.height = (int)h;
    out.rgba.resize((size_t)w * h * 4);
    hr = conv->CopyPixels(nullptr, w * 4, (UINT)out.rgba.size(), out.rgba.data());

    SafeRelease(conv);
    SafeRelease(frame);
    SafeRelease(decoder);
    SafeRelease(factory);

    if (FAILED(hr)) {
        out = {};
        if (err) *err = "CopyPixels failed";
        return false;
    }
    return true;
}

bool SavePng(const std::string& utf8Path, const RgbaImage& img, std::string* err) {
    if (img.width <= 0 || img.height <= 0 ||
        img.rgba.size() < (size_t)img.width * img.height * 4) {
        if (err) *err = "Invalid image";
        return false;
    }

    ComInit com;
    if (!com.ok()) {
        if (err) *err = "COM init failed";
        return false;
    }

    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        if (err) *err = "WIC factory failed";
        return false;
    }

    std::wstring path = Utf8ToWide(utf8Path);
    IWICStream* stream = nullptr;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr) || !stream) {
        SafeRelease(factory);
        if (err) *err = "Stream failed";
        return false;
    }
    hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) {
        SafeRelease(stream);
        SafeRelease(factory);
        if (err) *err = "Cannot create file";
        return false;
    }

    IWICBitmapEncoder* encoder = nullptr;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr) || !encoder) {
        SafeRelease(stream);
        SafeRelease(factory);
        if (err) *err = "PNG encoder failed";
        return false;
    }
    hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);

    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    hr = encoder->CreateNewFrame(&frame, &props);
    if (FAILED(hr) || !frame) {
        SafeRelease(props);
        SafeRelease(encoder);
        SafeRelease(stream);
        SafeRelease(factory);
        if (err) *err = "Frame encode failed";
        return false;
    }
    SafeRelease(props);

    hr = frame->Initialize(nullptr);
    hr = frame->SetSize((UINT)img.width, (UINT)img.height);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    hr = frame->SetPixelFormat(&fmt);
    // If encoder rewrites format, convert — for PNG, RGBA usually sticks.
    hr = frame->WritePixels((UINT)img.height, (UINT)img.width * 4,
                            (UINT)img.rgba.size(), const_cast<BYTE*>(img.rgba.data()));
    hr = frame->Commit();
    hr = encoder->Commit();

    SafeRelease(frame);
    SafeRelease(encoder);
    SafeRelease(stream);
    SafeRelease(factory);

    if (FAILED(hr)) {
        if (err) *err = "Write PNG failed";
        return false;
    }
    return true;
}

std::vector<std::string> BrowseOpenPngFiles(const std::string& startDirUtf8) {
    std::vector<std::string> result;
    ComInit com;
    if (!com.ok()) return result;

    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dlg));
    if (FAILED(hr) || !dlg) return result;

    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);

    COMDLG_FILTERSPEC filters[] = {
        { L"PNG images", L"*.png" },
        { L"All images", L"*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.tif;*.tiff" },
        { L"All files", L"*.*" },
    };
    dlg->SetFileTypes(3, filters);
    dlg->SetTitle(L"Select images");

    if (!startDirUtf8.empty()) {
        IShellItem* folder = nullptr;
        std::wstring w = Utf8ToWide(startDirUtf8);
        if (SUCCEEDED(SHCreateItemFromParsingName(w.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    }

    hr = dlg->Show(nullptr);
    if (hr == S_OK) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items)) && items) {
            DWORD count = 0;
            items->GetCount(&count);
            for (DWORD i = 0; i < count; ++i) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &item)) && item) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                        result.push_back(WideToUtf8(path));
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            items->Release();
        }
    }
    dlg->Release();
    return result;
}

std::string BrowseSavePngFile(const std::string& startDirUtf8, const std::string& defaultName) {
    ComInit com;
    if (!com.ok()) return {};

    IFileSaveDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dlg));
    if (FAILED(hr) || !dlg) return {};

    COMDLG_FILTERSPEC filters[] = {
        { L"PNG image", L"*.png" },
        { L"All files", L"*.*" },
    };
    dlg->SetFileTypes(2, filters);
    dlg->SetDefaultExtension(L"png");
    dlg->SetTitle(L"Save atlas PNG");
    if (!defaultName.empty())
        dlg->SetFileName(Utf8ToWide(defaultName).c_str());

    if (!startDirUtf8.empty()) {
        IShellItem* folder = nullptr;
        std::wstring w = Utf8ToWide(startDirUtf8);
        if (SUCCEEDED(SHCreateItemFromParsingName(w.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    }

    std::string result;
    if (dlg->Show(nullptr) == S_OK) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                result = WideToUtf8(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

} // namespace helpers
