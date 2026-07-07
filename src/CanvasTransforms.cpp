#include "Canvas.h"
#include "core/Logger.h"
#include <algorithm>
#include <vector>

// --- Helpers ---
static std::vector<float> ExportLayerF(const Layer& layer, int w, int h) {
    std::vector<float> out((size_t)w * h * 4, 0.f);
    if (layer.tileCache) layer.tileCache->ExportRGBA32F(out.data(), w, h);
    return out;
}

static void SetLayerPixelsF(Layer& layer, const std::vector<float>& pixels, int w, int h, CanvasPixelFormat fmt) {
    if (!layer.tileCache) {
        layer.tileCache = std::make_unique<TileCache>();
        layer.tileCache->Init(w, h, fmt);
    }
    layer.tileCache->ImportRGBA32F(pixels.data(), w, h);
    layer.tileCache->MarkAllDirty();
    layer.needsUpload = true;
    layer.filtersDirty = true;
}

static void EnsureLayerTileCache(Layer& layer, int w, int h, CanvasPixelFormat fmt) {
    if (!layer.tileCache) {
        layer.tileCache = std::make_unique<TileCache>();
        layer.tileCache->Init(w, h, fmt);
    }
}

static bool LayerHasPixels(const Layer& layer) {
    return layer.tileCache && !layer.tileCache->IsEmpty();
}

// ============================================================
// Pixel Transformations
// ============================================================

void Canvas::CommitTransformation(const std::string& actionName) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= static_cast<int>(m_Layers.size())) return;
    auto& layer = m_Layers[m_ActiveLayerIdx];
    std::vector<TileDelta> deltas;
    deltas.reserve(m_ActiveStrokeDeltas.size());

    for (auto& pair : m_ActiveStrokeDeltas) {
        auto& delta = pair.second;
        delta.newPixels = layer.tileCache
            ? layer.tileCache->SnapshotTile(delta.tileX, delta.tileY)
            : std::vector<uint8_t>{};
        deltas.push_back(std::move(delta));
    }

    auto cmd = std::make_shared<PaintStrokeCommand>(
        layer.name + " " + actionName, m_ActiveLayerIdx, std::move(deltas)
    );
    m_UndoRedoManager.PushCommand(cmd);
    m_ActiveStrokeDeltas.clear();
    m_IsDocumentModified = true;
    layer.needsUpload = true;
}

void Canvas::FlipActiveLayerHorizontal() {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= static_cast<int>(m_Layers.size())) return;
    auto& layer = m_Layers[m_ActiveLayerIdx];
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    auto pixels = ExportLayerF(layer, m_Width, m_Height);

    // Backup all tiles
    m_ActiveStrokeDeltas.clear();
    int numTilesX = (m_Width + 255) / 256;
    int numTilesY = (m_Height + 255) / 256;
    for (int ty = 0; ty < numTilesY; ++ty) {
        for (int tx = 0; tx < numTilesX; ++tx) {
            BackupTile(tx, ty);
        }
    }

    // Perform horizontal flip
    for (int y = 0; y < m_Height; ++y) {
        for (int x = 0; x < m_Width / 2; ++x) {
            int leftIdx = (y * m_Width + x) * 4;
            int rightIdx = (y * m_Width + (m_Width - 1 - x)) * 4;
            for (int c = 0; c < 4; ++c) {
                std::swap(pixels[leftIdx + c], pixels[rightIdx + c]);
            }
        }
    }
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);

    CommitTransformation("Horizontal Flip");
}

void Canvas::FlipActiveLayerVertical() {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= static_cast<int>(m_Layers.size())) return;
    auto& layer = m_Layers[m_ActiveLayerIdx];
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    auto pixels = ExportLayerF(layer, m_Width, m_Height);

    // Backup all tiles
    m_ActiveStrokeDeltas.clear();
    int numTilesX = (m_Width + 255) / 256;
    int numTilesY = (m_Height + 255) / 256;
    for (int ty = 0; ty < numTilesY; ++ty) {
        for (int tx = 0; tx < numTilesX; ++tx) {
            BackupTile(tx, ty);
        }
    }

    // Perform vertical flip
    for (int y = 0; y < m_Height / 2; ++y) {
        int targetY = m_Height - 1 - y;
        for (int x = 0; x < m_Width; ++x) {
            int topIdx = (y * m_Width + x) * 4;
            int bottomIdx = (targetY * m_Width + x) * 4;
            for (int c = 0; c < 4; ++c) {
                std::swap(pixels[topIdx + c], pixels[bottomIdx + c]);
            }
        }
    }
    SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);

    CommitTransformation("Vertical Flip");
}

void Canvas::RotateCanvas90(bool clockwise) {
    int oldW = m_Width;
    int oldH = m_Height;
    int newW = oldH;
    int newH = oldW;

    // Clear undo history because resizing/rotating the entire canvas changes layout dimensions
    ClearUndoHistory();

    for (auto& layer : m_Layers) {
        auto pixels = ExportLayerF(layer, oldW, oldH);
        std::vector<float> rotated((size_t)newW * newH * 4, 0.0f);
        for (int y = 0; y < oldH; ++y) {
            for (int x = 0; x < oldW; ++x) {
                int dx = clockwise ? (oldH - 1 - y) : y;
                int dy = clockwise ? x : (oldW - 1 - x);
                int srcIdx = (y * oldW + x) * 4;
                int destIdx = (dy * newW + dx) * 4;
                for (int c = 0; c < 4; ++c) {
                    rotated[destIdx + c] = pixels[srcIdx + c];
                }
            }
        }
        if (!layer.tileCache) {
            layer.tileCache = std::make_unique<TileCache>();
        }
        layer.tileCache->Init(newW, newH, m_CanvasFormat);
        layer.tileCache->ImportRGBA32F(rotated.data(), newW, newH);
        layer.tileCache->MarkAllDirty();
        layer.needsUpload = true;
    }

    m_Width = newW;
    m_Height = newH;

    MarkCompositeResourcesDirty();

    m_IsDocumentModified = true;
    Logger::Get().Info("Rotated canvas 90 degrees " + std::string(clockwise ? "CW" : "CCW"));
}

void Canvas::FlipCanvasHorizontal() {
    ClearUndoHistory();
    for (auto& layer : m_Layers) {
        if (!LayerHasPixels(layer)) continue;
        auto pixels = ExportLayerF(layer, m_Width, m_Height);
        for (int y = 0; y < m_Height; ++y) {
            for (int x = 0; x < m_Width / 2; ++x) {
                int leftIdx = (y * m_Width + x) * 4;
                int rightIdx = (y * m_Width + (m_Width - 1 - x)) * 4;
                for (int c = 0; c < 4; ++c) {
                    std::swap(pixels[leftIdx + c], pixels[rightIdx + c]);
                }
            }
        }
        SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);
    }
    m_IsDocumentModified = true;
    Logger::Get().Info("Flipped canvas horizontally");
}

void Canvas::FlipCanvasVertical() {
    ClearUndoHistory();
    for (auto& layer : m_Layers) {
        if (!LayerHasPixels(layer)) continue;
        auto pixels = ExportLayerF(layer, m_Width, m_Height);
        for (int y = 0; y < m_Height / 2; ++y) {
            int targetY = m_Height - 1 - y;
            for (int x = 0; x < m_Width; ++x) {
                int topIdx = (y * m_Width + x) * 4;
                int bottomIdx = (targetY * m_Width + x) * 4;
                for (int c = 0; c < 4; ++c) {
                    std::swap(pixels[topIdx + c], pixels[bottomIdx + c]);
                }
            }
        }
        SetLayerPixelsF(layer, pixels, m_Width, m_Height, m_CanvasFormat);
    }
    m_IsDocumentModified = true;
    Logger::Get().Info("Flipped canvas vertically");
}
