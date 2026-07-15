#pragma once
#include "../../assets/AssetTypes.h"
#include <d3d11.h>
#include <functional>
#include <string>

namespace Ui {

// Modal asset picker. Call every frame; opens when OpenAssetPicker is used.
void OpenAssetPicker(const assets::AssetFilter& filter, const char* title = "Choose Asset");
// Returns true once when user confirms; outKey receives selection. Resets after consume.
bool AssetPickerResult(std::string& outKey);
// Draw (call from RenderAll). device may be null (no thumbs).
void DrawAssetPicker(ID3D11Device* device);

bool IsAssetPickerOpen();

} // namespace Ui
