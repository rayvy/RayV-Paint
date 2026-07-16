#pragma once
// GPU preview textures for rayv.ui.image_* (main-thread ImGui only).

#include <cstdint>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace script {

void UiPreviewSetDevice(ID3D11Device* device, ID3D11DeviceContext* ctx);

// Upload/replace RGBA8 texture keyed by name. Returns false on failure.
bool UiPreviewSetRgba8(const std::string& key, const uint8_t* rgba, int w, int h);

// Draw ImGui image for key at display size (pixels). Returns false if missing.
bool UiPreviewDraw(const std::string& key, float displayW, float displayH);

void UiPreviewDestroy(const std::string& key);
void UiPreviewClearAll();

} // namespace script
