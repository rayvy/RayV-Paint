#pragma once
// Minimal Win32 + DX11 + Dear ImGui host for helper micro-apps.

#include <windows.h>
#include <d3d11.h>
#include <string>
#include <functional>

struct ImGuiContext;

namespace helpers {

class Dx11ImGuiApp {
public:
    bool Create(const wchar_t* title, int width, int height, HICON icon = nullptr);
    void Destroy();

    // Runs message + render loop until window closes.
    // drawFrame is called once per frame between ImGui::NewFrame and Render.
    int Run(const std::function<void()>& drawFrame);

    void RequestClose();
    HWND Hwnd() const { return hwnd_; }
    ID3D11Device* Device() const { return device_; }
    ID3D11DeviceContext* Context() const { return context_; }

    // Create SRV from RGBA8 (for atlas preview). Caller releases.
    ID3D11ShaderResourceView* CreateRgbaSrv(const uint8_t* rgba, int w, int h);

private:
    bool CreateDevice(HWND h);
    void CleanupDevice();
    void CreateRenderTarget();
    void CleanupRenderTarget();

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    HWND hwnd_ = nullptr;
    WNDCLASSEXW wc_{};
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGISwapChain* swap_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    bool running_ = false;
};

} // namespace helpers
