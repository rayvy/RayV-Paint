#include "Canvas.h"
#include "core/TileCache.h"
#include "core/Logger.h"
#include <algorithm>
#include <cstring>
#include <cmath>

static std::vector<float> ExportLayerF(const Layer& layer, int w, int h) {
    std::vector<float> out((size_t)w * h * 4, 0.f);
    if (layer.tileCache) layer.tileCache->ExportRGBA32F(out.data(), w, h);
    return out;
}

static bool LayerHasPixels(const Layer& layer) {
    return layer.tileCache && !layer.tileCache->IsEmpty();
}

static std::vector<float> ComposeVisibleLayers(const std::vector<Layer>& layers, int w, int h) {
    std::vector<float> composite((size_t)w * h * 4, 0.f);
    int firstVisibleIdx = -1;
    for (int l = 0; l < (int)layers.size(); ++l) {
        if (layers[l].visible) { firstVisibleIdx = l; break; }
    }
    if (firstVisibleIdx == -1) return composite;

    const auto& baseLayer = layers[firstVisibleIdx];
    if (LayerHasPixels(baseLayer)) {
        auto basePx = ExportLayerF(baseLayer, w, h);
        std::memcpy(composite.data(), basePx.data(), basePx.size() * sizeof(float));
        if (baseLayer.opacity < 1.f) {
            for (size_t i = 0; i < (size_t)w * h; ++i) {
                composite[i * 4 + 3] *= baseLayer.opacity;
            }
        }
    }

    for (int l = firstVisibleIdx + 1; l < (int)layers.size(); ++l) {
        const auto& layer = layers[l];
        if (!layer.visible || !LayerHasPixels(layer)) continue;
        auto layerPx = ExportLayerF(layer, w, h);
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            size_t base = i * 4;
            float srcR = layerPx[base + 0];
            float srcG = layerPx[base + 1];
            float srcB = layerPx[base + 2];
            float srcA = layerPx[base + 3] * layer.opacity;
            if (srcA <= 0.f) continue;
            float destR = composite[base + 0];
            float destG = composite[base + 1];
            float destB = composite[base + 2];
            float destA = composite[base + 3];
            float outA = srcA + destA * (1.f - srcA);
            if (outA > 0.f) {
                composite[base + 0] = (srcR * srcA + destR * destA * (1.f - srcA)) / outA;
                composite[base + 1] = (srcG * srcA + destG * destA * (1.f - srcA)) / outA;
                composite[base + 2] = (srcB * srcA + destB * destA * (1.f - srcA)) / outA;
                composite[base + 3] = outA;
            }
        }
    }
    return composite;
}

static void ComputeCompositePreviewSize(int canvasW, int canvasH, int& outW, int& outH) {
    constexpr int kProxyThreshold = 4096;
    constexpr int kProxyMaxDim = 2048;

    int maxDim = std::max(canvasW, canvasH);
    if (maxDim <= kProxyThreshold) {
        outW = std::max(1, canvasW);
        outH = std::max(1, canvasH);
        return;
    }

    float scale = (float)kProxyMaxDim / (float)maxDim;
    outW = std::max(1, (int)std::round(canvasW * scale));
    outH = std::max(1, (int)std::round(canvasH * scale));
}

Canvas::Canvas()
    : m_Width(0)
    , m_Height(0)
    , m_Zoom(1.0f)
    , m_Pan(0.0f, 0.0f) {
    ResetView();
}

Canvas::~Canvas() {
    Shutdown();
}

void Canvas::ResetView() {
    m_Zoom = 1.0f;
    m_Pan.x = -m_Width * 0.5f * m_Zoom;
    m_Pan.y = -m_Height * 0.5f * m_Zoom;
    m_RotationAngle = 0.0f;
    m_ViewportFlipH = false;
    m_ViewportFlipV = false;
}

bool Canvas::Initialize() {
    if (m_Layers.empty()) {
        CreateNewLayer("Background");
    }
    m_CompositeDirty = true;
    m_IsStrokeActive = false;
    m_IsMovingPixels = false;
    m_SmartSelectInProgress.store(false);
    m_SmartSelectCancelled.store(false);
    return true;
}

void Canvas::Shutdown() {
    m_Layers.clear();
}

void Canvas::MarkCompositeResourcesDirty() {
    ComputeCompositePreviewSize(m_Width, m_Height, m_CompositeWidth, m_CompositeHeight);
    m_CompositeDirty = true;
}

void Canvas::ResizeCanvas(int width, int height) {
    int oldW = m_Width;
    int oldH = m_Height;
    
    m_Width = std::max(1, std::min(width, 16384));
    m_Height = std::max(1, std::min(height, 16384));
    m_RendererInvalidated = true;

    if (m_Width == oldW && m_Height == oldH) return;

    Logger::Get().Info("Resizing canvas from " + std::to_string(oldW) + "x" + std::to_string(oldH) +
                       " to " + std::to_string(m_Width) + "x" + std::to_string(m_Height));

    // Resize selection mask (uint8_t, single-channel)
    {
        std::vector<uint8_t> oldSel = std::move(m_SelectionMask);
        m_SelectionMask.assign((size_t)m_Width * m_Height, 0);
        int copyW = std::min(oldW, m_Width);
        int copyH = std::min(oldH, m_Height);
        for (int y = 0; y < copyH; ++y) {
            for (int x = 0; x < copyW; ++x) {
                uint8_t v = oldSel.empty() ? 0 : oldSel[(size_t)y * oldW + x];
                m_SelectionMask[(size_t)y * m_Width + x] = v;
                if (v > 0) m_HasSelection = true;
            }
        }
    }

    MarkCompositeResourcesDirty();

    // Resize each layer's TileCache and mark renderer upload debt.
    for (size_t i = 0; i < m_Layers.size(); ++i) {
        auto& layer = m_Layers[i];
        if (layer.isGroup) continue;

        if (layer.tileCache) {
            layer.tileCache->Resize(m_Width, m_Height);
            layer.tileCache->MarkAllDirty();
        }

        layer.needsUpload = true;

        // Resize mask
        if (layer.hasMask && !layer.mask.empty()) {
            std::vector<uint8_t> oldMask = std::move(layer.mask);
            layer.mask.assign((size_t)m_Width * m_Height, 255);
            int copyW = std::min(oldW, m_Width);
            int copyH = std::min(oldH, m_Height);
            for (int y = 0; y < copyH; ++y) {
                for (int x = 0; x < copyW; ++x) {
                    layer.mask[(size_t)y * m_Width + x] = oldMask[(size_t)y * oldW + x];
                }
            }
            layer.maskNeedsUpload = true;
            int tilesX = (m_Width + TILE_SIZE - 1) / TILE_SIZE;
            int tilesY = (m_Height + TILE_SIZE - 1) / TILE_SIZE;
            layer.maskDirtyTiles.assign(tilesX * tilesY, true);
        }
    }

    m_CompositeDirty = true;
}

void Canvas::Update(float viewportWidth, float viewportHeight, bool isMouseOverCanvas, 
                    float mouseX, float mouseY, bool isDragging, float dragDx, float dragDy, float wheelDelta) {
    if (isDragging) {
        m_Pan.x += dragDx;
        m_Pan.y += dragDy;
    }

    if (isMouseOverCanvas && wheelDelta != 0.0f) {
        float zoomFactor = (wheelDelta > 0.0f) ? 1.15f : 0.85f;
        float oldZoom = m_Zoom;
        m_Zoom = std::clamp(m_Zoom * zoomFactor, 0.05f, 64.0f);

        float originX = std::floor(m_Pan.x + viewportWidth * 0.5f);
        float originY = std::floor(m_Pan.y + viewportHeight * 0.5f);

        float mouseInCanvasX = (mouseX - originX) / oldZoom;
        float mouseInCanvasY = (mouseY - originY) / oldZoom;

        m_Pan.x = mouseX - mouseInCanvasX * m_Zoom - viewportWidth * 0.5f;
        m_Pan.y = mouseY - mouseInCanvasY * m_Zoom - viewportHeight * 0.5f;
    }
}

std::vector<float> Canvas::GetComposedPixels() {
    return ComposeVisibleLayers(m_Layers, m_Width, m_Height);
}
