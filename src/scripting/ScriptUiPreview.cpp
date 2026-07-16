#include "ScriptUiPreview.h"
#include "../core/Logger.h"
#include <d3d11.h>
#include <imgui.h>
#include <unordered_map>
#include <vector>
#include <cstring>

namespace script {
namespace {

struct PreviewTex {
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;
};

ID3D11Device* g_Dev = nullptr;
ID3D11DeviceContext* g_Ctx = nullptr;
std::unordered_map<std::string, PreviewTex> g_Map;

void Release(PreviewTex& p) {
    if (p.srv) { p.srv->Release(); p.srv = nullptr; }
    if (p.tex) { p.tex->Release(); p.tex = nullptr; }
    p.w = p.h = 0;
}

} // namespace

void UiPreviewSetDevice(ID3D11Device* device, ID3D11DeviceContext* ctx) {
    g_Dev = device;
    g_Ctx = ctx;
}

bool UiPreviewSetRgba8(const std::string& key, const uint8_t* rgba, int w, int h) {
    if (!g_Dev || !rgba || w < 1 || h < 1 || key.empty()) {
        Logger::Get().ErrorTag("script.ui", "image set refused: bad args/device");
        return false;
    }
    if (w > 8192 || h > 8192) {
        Logger::Get().ErrorTag("script.ui", "image set refused: max 8192 per side");
        return false;
    }

    auto& slot = g_Map[key];
    if (slot.tex && (slot.w != w || slot.h != h)) {
        Release(slot);
    }

    if (!slot.tex) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)w;
        td.Height = (UINT)h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = rgba;
        init.SysMemPitch = (UINT)(w * 4);

        HRESULT hr = g_Dev->CreateTexture2D(&td, &init, &slot.tex);
        if (FAILED(hr) || !slot.tex) {
            Logger::Get().ErrorTag("script.ui", "CreateTexture2D failed");
            Release(slot);
            return false;
        }
        hr = g_Dev->CreateShaderResourceView(slot.tex, nullptr, &slot.srv);
        if (FAILED(hr) || !slot.srv) {
            Logger::Get().ErrorTag("script.ui", "CreateSRV failed");
            Release(slot);
            return false;
        }
        slot.w = w;
        slot.h = h;
        return true;
    }

    // Update existing
    if (!g_Ctx) return false;
    g_Ctx->UpdateSubresource(slot.tex, 0, nullptr, rgba, (UINT)(w * 4), 0);
    return true;
}

bool UiPreviewDraw(const std::string& key, float displayW, float displayH) {
    auto it = g_Map.find(key);
    if (it == g_Map.end() || !it->second.srv) {
        Logger::Get().ErrorTag("script.ui", "image draw: unknown key '" + key + "'");
        return false;
    }
    float dw = displayW > 1.f ? displayW : (float)it->second.w;
    float dh = displayH > 1.f ? displayH : (float)it->second.h;
    // Fit max side if only one dimension requested via 0
    ImGui::Image((ImTextureID)it->second.srv, ImVec2(dw, dh));
    return true;
}

void UiPreviewDestroy(const std::string& key) {
    auto it = g_Map.find(key);
    if (it == g_Map.end()) return;
    Release(it->second);
    g_Map.erase(it);
}

void UiPreviewClearAll() {
    for (auto& kv : g_Map) Release(kv.second);
    g_Map.clear();
}

} // namespace script
