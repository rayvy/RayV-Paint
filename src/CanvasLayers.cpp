#include "Canvas.h"
#include "core/Logger.h"
#include <algorithm>

// --- Helpers ---
static void EnsureLayerTileCache(Layer& layer, int w, int h, CanvasPixelFormat fmt) {
    if (!layer.tileCache) {
        layer.tileCache = std::make_unique<TileCache>();
        layer.tileCache->Init(w, h, fmt);
    }
}

static float SelU82F(uint8_t v) {
    return v / 255.f;
}

// ============================================================
// Layer Management
// ============================================================

void Canvas::CreateNewLayer(const std::string& name) {
    Layer newLayer;
    newLayer.name    = name;
    newLayer.visible = true;
    newLayer.opacity = 1.0f;

    // Initialise TileCache for this layer (no tiles allocated yet — truly lazy)
    newLayer.tileCache = std::make_unique<TileCache>();
    newLayer.tileCache->Init(m_Width, m_Height, m_CanvasFormat);

    newLayer.needsUpload = true;
    m_Layers.push_back(std::move(newLayer));
    m_ActiveLayerIdx = (int)m_Layers.size() - 1;
    m_CompositeDirty = true;
    Logger::Get().Info("Created new layer: " + name);
}

void Canvas::DeleteLayer(int index) {
    if (index < 0 || index >= m_Layers.size()) return;
    
    Logger::Get().Info("Deleted layer: " + m_Layers[index].name);

    m_Layers.erase(m_Layers.begin() + index);

    if (m_Layers.empty()) {
        m_ActiveLayerIdx = -1;
    } else {
        m_ActiveLayerIdx = std::clamp(m_ActiveLayerIdx, 0, static_cast<int>(m_Layers.size()) - 1);
    }

    m_CompositeDirty = true;
}

void Canvas::SetActiveLayerIndex(int idx) {
    if (idx >= 0 && idx < m_Layers.size()) {
        m_ActiveLayerIdx = idx;
    }
}

void Canvas::ToggleLayerIsolation(int layerIdx) {
    if (layerIdx < 0 || layerIdx >= static_cast<int>(m_Layers.size())) return;

    if (m_IsIsolatedMode && m_IsolatedLayerIdx == layerIdx) {
        // Turn off isolation: restore visibility states
        for (size_t i = 0; i < m_Layers.size(); ++i) {
            if (i < m_PreIsolationVisibility.size()) {
                m_Layers[i].visible = m_PreIsolationVisibility[i];
            } else {
                m_Layers[i].visible = true;
            }
        }
        m_IsIsolatedMode = false;
        m_IsolatedLayerIdx = -1;
        m_PreIsolationVisibility.clear();
    } else {
        // If already in isolated mode, first restore visibility before new isolation
        if (m_IsIsolatedMode) {
            for (size_t i = 0; i < m_Layers.size(); ++i) {
                if (i < m_PreIsolationVisibility.size()) {
                    m_Layers[i].visible = m_PreIsolationVisibility[i];
                }
            }
        }

        // Turn on isolation for layerIdx
        m_PreIsolationVisibility.resize(m_Layers.size());
        for (size_t i = 0; i < m_Layers.size(); ++i) {
            m_PreIsolationVisibility[i] = m_Layers[i].visible;
            m_Layers[i].visible = (static_cast<int>(i) == layerIdx);
        }
        m_IsIsolatedMode = true;
        m_IsolatedLayerIdx = layerIdx;
    }

    m_CompositeDirty = true;
}

// ============================================================
// Layer Mask operations
// ============================================================

void Canvas::CreateLayerMask(int index) {
    if (index < 0 || index >= m_Layers.size()) return;
    Layer& layer = m_Layers[index];
    
    layer.mask.assign((size_t)m_Width * m_Height, 255);
    layer.hasMask = true;
    layer.maskNeedsUpload = true;
    
    int tilesX = (m_Width + TILE_SIZE - 1) / TILE_SIZE;
    int tilesY = (m_Height + TILE_SIZE - 1) / TILE_SIZE;
    layer.maskDirtyTiles.assign(tilesX * tilesY, true);
    
    m_CompositeDirty = true;
    
    Logger::Get().Info("Created layer mask for layer: " + layer.name);
}

void Canvas::CreateLayerMaskFromSelection(int index) {
    if (index < 0 || index >= m_Layers.size()) return;
    Layer& layer = m_Layers[index];
    
    layer.mask.assign((size_t)m_Width * m_Height, 0);
    if (m_HasSelection) {
        std::copy(m_SelectionMask.begin(), m_SelectionMask.end(), layer.mask.begin());
    } else {
        layer.mask.assign((size_t)m_Width * m_Height, 255);
    }
    
    layer.hasMask = true;
    layer.maskNeedsUpload = true;
    
    int tilesX = (m_Width + TILE_SIZE - 1) / TILE_SIZE;
    int tilesY = (m_Height + TILE_SIZE - 1) / TILE_SIZE;
    layer.maskDirtyTiles.assign(tilesX * tilesY, true);
    
    m_CompositeDirty = true;
    
    Logger::Get().Info("Created layer mask from selection for layer: " + layer.name);
}

void Canvas::DeleteLayerMask(int index) {
    if (index < 0 || index >= m_Layers.size()) return;
    Layer& layer = m_Layers[index];
    
    layer.mask.clear();
    layer.maskDirtyTiles.clear();
    layer.hasMask = false;
    layer.maskNeedsUpload = false;
    m_CompositeDirty = true;
    
    Logger::Get().Info("Deleted layer mask for layer: " + layer.name);
}

void Canvas::ApplyLayerMask(int index) {
    if (index < 0 || index >= m_Layers.size()) return;
    Layer& layer = m_Layers[index];
    if (!layer.hasMask) return;
    
    int oldActive = m_ActiveLayerIdx;
    m_ActiveLayerIdx = index;
    int numTilesX = (m_Width + 255) / 256;
    int numTilesY = (m_Height + 255) / 256;
    for (int ty = 0; ty < numTilesY; ++ty) {
        for (int tx = 0; tx < numTilesX; ++tx) {
            BackupTile(tx, ty);
        }
    }
    
    if (layer.mask.size() != (size_t)m_Width * m_Height) {
        Logger::Get().Error("ApplyLayerMask: Mask size mismatch! Reallocating mask.");
        layer.mask.resize((size_t)m_Width * m_Height, 255);
    }

    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    for (int y = 0; y < m_Height; ++y) {
        for (int x = 0; x < m_Width; ++x) {
            float rgba[4];
            layer.tileCache->GetPixelF(x, y, rgba);
            rgba[3] *= SelU82F(layer.mask[(size_t)y * m_Width + x]);
            layer.tileCache->SetPixelF(x, y, rgba);
        }
    }
    layer.tileCache->MarkAllDirty();
    
    layer.needsUpload = true;
    layer.filtersDirty = true;
    DeleteLayerMask(index);
    
    if (!m_ActiveStrokeDeltas.empty()) {
        std::vector<TileDelta> deltas;
        for (auto& pair : m_ActiveStrokeDeltas) {
            auto& delta = pair.second;
            delta.newPixels = layer.tileCache
                ? layer.tileCache->SnapshotTile(delta.tileX, delta.tileY)
                : std::vector<uint8_t>{};
            deltas.push_back(std::move(delta));
        }
        m_UndoRedoManager.PushCommand(std::make_shared<PaintStrokeCommand>("Apply Mask", index, std::move(deltas)));
        m_ActiveStrokeDeltas.clear();
    }
    m_ActiveLayerIdx = oldActive;
    
    Logger::Get().Info("Applied layer mask to layer alpha: " + layer.name);
}

void Canvas::MarkLayerMaskDirty(int index) {
    if (index < 0 || index >= m_Layers.size()) return;
    Layer& layer = m_Layers[index];
    if (!layer.hasMask) return;
    layer.maskNeedsUpload = true;
    
    int tilesX = (m_Width + TILE_SIZE - 1) / TILE_SIZE;
    int tilesY = (m_Height + TILE_SIZE - 1) / TILE_SIZE;
    layer.maskDirtyTiles.assign(tilesX * tilesY, true);

    m_CompositeDirty = true;
}

// ============================================================
// Layer Group management
// ============================================================

void Canvas::CreateLayerGroup(const std::string& name) {
    Layer grp; grp.name=name; grp.isGroup=true; grp.visible=true; grp.opacity=1.f; grp.blendMode=BlendMode::Normal;
    m_Layers.push_back(std::move(grp));
    m_ActiveLayerIdx=(int)m_Layers.size()-1;
    m_CompositeDirty = true;
    Logger::Get().Info("Created layer group: "+name);
}

void Canvas::AddLayerToGroup(int layerIdx, int groupLayerIdx) {
    if (layerIdx<0||layerIdx>=(int)m_Layers.size()||groupLayerIdx<0||groupLayerIdx>=(int)m_Layers.size()) return;
    if (!m_Layers[groupLayerIdx].isGroup) return;
    m_Layers[layerIdx].parentGroupId=groupLayerIdx;
    m_CompositeDirty = true;
}

void Canvas::RemoveLayerFromGroup(int layerIdx) {
    if (layerIdx>=0&&layerIdx<(int)m_Layers.size()) {
        m_Layers[layerIdx].parentGroupId=-1;
        m_CompositeDirty = true;
    }
}

void Canvas::CreateLayerFromPixels(const std::string& name, const std::vector<float>& pixels, int width, int height) {
    if (pixels.empty() || width <= 0 || height <= 0) return;

    if (m_Layers.empty()) {
        m_Width = width;
        m_Height = height;
        MarkCompositeResourcesDirty();
    }

    Layer newLayer;
    newLayer.name = name;
    newLayer.visible = true;
    newLayer.opacity = 1.0f;
    newLayer.tileCache = std::make_unique<TileCache>();
    newLayer.tileCache->Init(m_Width, m_Height, m_CanvasFormat);

    if (width == m_Width && height == m_Height) {
        newLayer.tileCache->ImportRGBA32F(pixels.data(), width, height);
    } else {
        std::vector<float> resizedPixels((size_t)m_Width * m_Height * 4, 0.0f);
        int offsetX = (m_Width - width) / 2;
        int offsetY = (m_Height - height) / 2;
        for (int y = 0; y < height; ++y) {
            int targetY = y + offsetY;
            if (targetY < 0 || targetY >= m_Height) continue;
            for (int x = 0; x < width; ++x) {
                int targetX = x + offsetX;
                if (targetX < 0 || targetX >= m_Width) continue;

                int srcIdx = (y * width + x) * 4;
                int destIdx = (targetY * m_Width + targetX) * 4;
                std::memcpy(&resizedPixels[destIdx], &pixels[srcIdx], 4 * sizeof(float));
            }
        }
        newLayer.tileCache->ImportRGBA32F(resizedPixels.data(), m_Width, m_Height);
    }
    newLayer.tileCache->MarkAllDirty();
    newLayer.needsUpload = true;

    int insertIdx = m_ActiveLayerIdx + 1;
    if (insertIdx < 0 || insertIdx > static_cast<int>(m_Layers.size())) {
        insertIdx = static_cast<int>(m_Layers.size());
    }

    m_Layers.insert(m_Layers.begin() + insertIdx, std::move(newLayer));
    m_ActiveLayerIdx = insertIdx;
    m_CompositeDirty = true;

    ClearUndoHistory();
    m_IsDocumentModified = true;
    Logger::Get().Info("Created new layer from clipboard/drop: " + name);
}
