#include "Dx11ImGuiApp.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <dxgi.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace helpers {

bool Dx11ImGuiApp::Create(const wchar_t* title, int width, int height, HICON icon) {
    wc_ = { sizeof(WNDCLASSEXW), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandleW(nullptr),
            icon ? icon : LoadIcon(nullptr, IDI_APPLICATION),
            LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"RayVHelpersClass", icon };
    RegisterClassExW(&wc_);

    hwnd_ = CreateWindowW(wc_.lpszClassName, title, WS_OVERLAPPEDWINDOW,
                          100, 100, width, height, nullptr, nullptr, wc_.hInstance, this);
    if (!hwnd_) return false;

    if (!CreateDevice(hwnd_)) {
        CleanupDevice();
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }

    ShowWindow(hwnd_, SW_SHOWDEFAULT);
    UpdateWindow(hwnd_);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // no imgui.ini noise next to tools

    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6.f;
    st.FrameRounding = 4.f;
    st.GrabRounding = 3.f;

    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(device_, context_);
    return true;
}

void Dx11ImGuiApp::Destroy() {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    if (ImGui::GetCurrentContext())
        ImGui::DestroyContext();
    CleanupDevice();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    UnregisterClassW(wc_.lpszClassName, wc_.hInstance);
}

void Dx11ImGuiApp::RequestClose() {
    if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

int Dx11ImGuiApp::Run(const std::function<void()>& drawFrame) {
    running_ = true;
    MSG msg{};
    while (running_) {
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                running_ = false;
        }
        if (!running_) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (drawFrame) drawFrame();

        ImGui::Render();
        const float clear[4] = { 0.10f, 0.10f, 0.12f, 1.f };
        context_->OMSetRenderTargets(1, &rtv_, nullptr);
        context_->ClearRenderTargetView(rtv_, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        swap_->Present(1, 0);
    }
    return 0;
}

ID3D11ShaderResourceView* Dx11ImGuiApp::CreateRgbaSrv(const uint8_t* rgba, int w, int h) {
    if (!device_ || !rgba || w <= 0 || h <= 0) return nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = (UINT)w;
    desc.Height = (UINT)h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sub{};
    sub.pSysMem = rgba;
    sub.SysMemPitch = (UINT)(w * 4);

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(device_->CreateTexture2D(&desc, &sub, &tex))) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = desc.Format;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr;
    device_->CreateShaderResourceView(tex, &srvd, &srv);
    tex->Release();
    return srv;
}

bool Dx11ImGuiApp::CreateDevice(HWND h) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = h;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = 0;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
        D3D11_SDK_VERSION, &sd, &swap_, &device_, &fl, &context_);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 2,
            D3D11_SDK_VERSION, &sd, &swap_, &device_, &fl, &context_);
        if (FAILED(hr)) return false;
    }
    CreateRenderTarget();
    return true;
}

void Dx11ImGuiApp::CreateRenderTarget() {
    ID3D11Texture2D* bb = nullptr;
    swap_->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (bb) {
        device_->CreateRenderTargetView(bb, nullptr, &rtv_);
        bb->Release();
    }
}

void Dx11ImGuiApp::CleanupRenderTarget() {
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
}

void Dx11ImGuiApp::CleanupDevice() {
    CleanupRenderTarget();
    if (swap_) { swap_->Release(); swap_ = nullptr; }
    if (context_) { context_->Release(); context_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
}

LRESULT CALLBACK Dx11ImGuiApp::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    Dx11ImGuiApp* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<Dx11ImGuiApp*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = reinterpret_cast<Dx11ImGuiApp*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    switch (msg) {
    case WM_SIZE:
        if (self && self->device_ && wParam != SIZE_MINIMIZED) {
            self->CleanupRenderTarget();
            self->swap_->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam),
                                       DXGI_FORMAT_UNKNOWN, 0);
            self->CreateRenderTarget();
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // disable ALT menu beep
            return 0;
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace helpers
