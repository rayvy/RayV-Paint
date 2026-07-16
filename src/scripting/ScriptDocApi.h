#pragma once
// =============================================================================
// Script document surface — used by rayv.doc (Python O4+).
// RULES: call through ProjectManager active canvas; no ImGui; no new god-paths
// in main.cpp. Device is set once after D3D init (ScriptDocApi::SetDevice).
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>

struct ID3D11Device;

namespace script {

void SetDevice(ID3D11Device* device);
ID3D11Device* Device();

// --- Document ---
int  Width();
int  Height();
std::string BitDepth(); // "U8" | "F16" | "F32"
std::string ProjectPath();
bool IsModified();

bool Open(const std::string& path);          // image or .rayp
bool SaveRayp(const std::string& path);
bool SaveImage(const std::string& path);     // png/etc via standard export
bool NewBlank(int w, int h);                 // resize current blank / clear to size

// --- Layers ---
int  LayerCount();
int  ActiveLayerIndex();
bool SetActiveLayer(int index);
std::string LayerName(int index);
bool SetLayerName(int index, const std::string& name);
bool LayerVisible(int index);
bool SetLayerVisible(int index, bool visible);
float LayerOpacity(int index);
bool SetLayerOpacity(int index, float opacity);
bool LayerIsGroup(int index);
bool LayerHasMask(int index);
bool LayerCanPaintContent(int index);
int  CreateLayer(const std::string& name);
bool DeleteLayer(int index);

// --- Pixels (active document space) ---
// Float RGBA 0..1, length = w*h*4, row-major. Empty on error.
// Refuses absurd allocations (same spirit as composite RAM guards).
std::vector<float> GetPixels(int layer, int x, int y, int w, int h);
bool SetPixels(int layer, int x, int y, int w, int h, const std::vector<float>& rgba);

// Full-layer RGBA8 packed (w*h*4 bytes). Empty on error / too large.
std::vector<uint8_t> GetLayerRgba8(int layer);
bool SetLayerRgba8(int layer, int srcW, int srcH, const std::vector<uint8_t>& rgba,
                   int dstX = 0, int dstY = 0);

// Single pixel float RGBA
bool GetPixel(int layer, int x, int y, float outRgba[4]);
bool SetPixel(int layer, int x, int y, float r, float g, float b, float a);

// --- Mask (layer) grayscale 0..255, length w*h ---
std::vector<uint8_t> GetMask(int layer);
bool SetMask(int layer, const std::vector<uint8_t>& mask);
bool CreateMask(int layer); // white mask if missing

// --- Selection ---
bool HasSelection();
std::vector<uint8_t> GetSelection(); // w*h, empty if none
bool SetSelection(const std::vector<uint8_t>& mask); // size must be w*h

// Mark dirty after external edits (GPU re-upload + recompose)
void NotifyPixelsChanged(int layer);

// Tile stats
int  ActiveLayerTileCount();
int  TileSize(); // TILE_SIZE constant

// --- Composite tiled export (safe: validates and refuses bad params) ---
// mode:
//   "count" — tilesX/tilesY = number of tiles along width/height (1..64)
//   "size"  — tilesX/tilesY = tile size in pixels (>=1, <= max(w,h))
// outDir: existing or creatable directory
// namePattern: must contain {x} and {y} (e.g. "tile_{x}_{y}.png")
// Returns number of files written, or 0 on refusal (details in log + outError).
int ExportCompositeTiles(const std::string& outDir,
                         const std::string& namePattern,
                         int tilesX, int tilesY,
                         const std::string& mode,
                         std::string* outError = nullptr);

// Fill a rect on a layer with solid color (for user paint variant scripts).
// Validates bounds; returns false + log on invalid input.
bool FillRect(int layer, int x, int y, int w, int h,
              float r, float g, float b, float a = 1.f);

// --- Undoable pixel edit session (AI apply / generative tools) ---
// Must run on main thread. Pattern:
//   begin_edit(layer) → set_pixels / set_layer_rgba8 / fill_rect → end_edit("AI Generate")
// end_edit pushes one undo step; cancel_edit restores without undo.
bool BeginEdit(int layer);
bool EndEdit(const std::string& actionName);
bool CancelEdit();
bool IsEditActive();

// Selection AABB (x,y,w,h). Returns false if none.
bool SelectionBounds(int& x, int& y, int& w, int& h);

// Crop get_pixels to selection bbox if has selection; else full layer.
// outX/outY = origin of returned buffer in document space.
bool GetPixelsSelectionOrFull(int layer, std::vector<float>& outRgba,
                              int& outX, int& outY, int& outW, int& outH);

} // namespace script
