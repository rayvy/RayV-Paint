#include "Canvas.h"
#include "core/Logger.h"
#include "core/PaintEngine.h"
#include "core/ImageManager.h"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cstring>
#include <cmath>

extern float g_PenPressure;

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

static uint8_t SelF2U8(float v) {
    return (uint8_t)(std::clamp(v, 0.f, 1.f) * 255.f + .5f);
}

static float SelU82F(uint8_t v) {
    return v / 255.f;
}

static float GetSelWeight(const std::vector<uint8_t>& mask, int w, int x, int y, bool hasSel) {
    if (!hasSel || mask.empty()) return 1.f;
    return SelU82F(mask[(size_t)y * w + x]);
}

static float sampleBilinearChannel(const std::vector<float>& pixels, int width, int height, float fx, float fy, int channel) {
    int x1 = (int)std::floor(fx);
    int y1 = (int)std::floor(fy);
    int x2 = x1 + 1;
    int y2 = y1 + 1;
    
    float tx = fx - x1;
    float ty = fy - y1;
    
    x1 = std::clamp(x1, 0, width - 1);
    x2 = std::clamp(x2, 0, width - 1);
    y1 = std::clamp(y1, 0, height - 1);
    y2 = std::clamp(y2, 0, height - 1);
    
    float c00 = pixels[(y1 * width + x1) * 4 + channel];
    float c10 = pixels[(y1 * width + x2) * 4 + channel];
    float c01 = pixels[(y2 * width + x1) * 4 + channel];
    float c11 = pixels[(y2 * width + x2) * 4 + channel];
    
    float r1 = c00 * (1.0f - tx) + c10 * tx;
    float r2 = c01 * (1.0f - tx) + c11 * tx;
    return r1 * (1.0f - ty) + r2 * ty;
}

static float sampleBilinearMask(const std::vector<uint8_t>& mask, int width, int height, float fx, float fy) {
    int x1 = (int)std::floor(fx);
    int y1 = (int)std::floor(fy);
    int x2 = x1 + 1;
    int y2 = y1 + 1;
    
    float tx = fx - x1;
    float ty = fy - y1;
    
    x1 = std::clamp(x1, 0, width - 1);
    x2 = std::clamp(x2, 0, width - 1);
    y1 = std::clamp(y1, 0, height - 1);
    y2 = std::clamp(y2, 0, height - 1);
    
    float c00 = SelU82F(mask[y1 * width + x1]);
    float c10 = SelU82F(mask[y1 * width + x2]);
    float c01 = SelU82F(mask[y2 * width + x1]);
    float c11 = SelU82F(mask[y2 * width + x2]);
    
    float r1 = c00 * (1.0f - tx) + c10 * tx;
    float r2 = c01 * (1.0f - tx) + c11 * tx;
    return r1 * (1.0f - ty) + r2 * ty;
}

// ============================================================
// Paint Operation
// ============================================================

void Canvas::PaintOnActiveLayer(float currRawX, float currRawY, StrokePhase phase, const BrushSettings& brush) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= static_cast<int>(m_Layers.size())) return;

    auto& layer = m_Layers[m_ActiveLayerIdx];

    BrushSettings activeBrush = brush;
    activeBrush.writeR = m_ChannelR;
    activeBrush.writeG = m_ChannelG;
    activeBrush.writeB = m_ChannelB;
    activeBrush.writeA = m_ChannelA;
    if (brush.pressureRadius) {
        activeBrush.radius = brush.radius * g_PenPressure;
        if (activeBrush.radius < 1.0f) activeBrush.radius = 1.0f;
    }
    if (brush.pressureHardness) {
        activeBrush.hardness = brush.hardness * g_PenPressure;
    }
    if (brush.pressureOpacity) {
        activeBrush.opacity = brush.opacity * g_PenPressure;
    }

    int numTilesX = (m_Width + 255) / 256;
    int numTilesY = (m_Height + 255) / 256;

    auto backupSymmetricTiles = [&](float cx, float cy, float radius) {
        float minX = cx - radius;
        float maxX = cx + radius;
        float minY = cy - radius;
        float maxY = cy + radius;
        
        int minTileX = std::max(0, static_cast<int>(minX) / 256);
        int maxTileX = std::max(0, static_cast<int>(maxX) / 256);
        int minTileY = std::max(0, static_cast<int>(minY) / 256);
        int maxTileY = std::max(0, static_cast<int>(maxY) / 256);

        for (int ty = minTileY; ty <= std::min(maxTileY, numTilesY - 1); ++ty) {
            for (int tx = minTileX; tx <= std::min(maxTileX, numTilesX - 1); ++tx) {
                BackupTile(tx, ty);
            }
        }
    };

    if (phase == StrokePhase::Begin) {
        m_IsStrokeActive = true;
        m_StrokeDistanceAccumulator = 0.0f;
        m_LastDabX = currRawX;
        m_LastDabY = currRawY;
        m_PrevStabilizedX = currRawX;
        m_PrevStabilizedY = currRawY;
        m_ActiveStrokeDeltas.clear();

        // Backup tiles covered by the first stamp (and its symmetries)
        backupSymmetricTiles(currRawX, currRawY, activeBrush.radius);
        if (m_MirrorHorizontal) {
            backupSymmetricTiles(static_cast<float>(m_Width) - currRawX, currRawY, activeBrush.radius);
        }
        if (m_MirrorVertical) {
            backupSymmetricTiles(currRawX, static_cast<float>(m_Height) - currRawY, activeBrush.radius);
        }
        if (m_MirrorHorizontal && m_MirrorVertical) {
            backupSymmetricTiles(static_cast<float>(m_Width) - currRawX, static_cast<float>(m_Height) - currRawY, activeBrush.radius);
        }

        // Lazy-allocate TileCache on first paint
        if (!m_Layers[m_ActiveLayerIdx].tileCache) {
            m_Layers[m_ActiveLayerIdx].tileCache = std::make_unique<TileCache>();
            m_Layers[m_ActiveLayerIdx].tileCache->Init(m_Width, m_Height, m_CanvasFormat);
        }

        // Place the very first stamp immediately
        PaintEngine::DrawStamp(*m_Layers[m_ActiveLayerIdx].tileCache,
                               currRawX, currRawY, activeBrush,
                               m_MirrorHorizontal, m_MirrorVertical,
                               m_HasSelection ? m_SelectionMask : std::vector<uint8_t>{});
        m_Layers[m_ActiveLayerIdx].needsUpload = true;
        m_Layers[m_ActiveLayerIdx].filtersDirty = true;
    }
    else if (phase == StrokePhase::Update && m_IsStrokeActive) {
        // Apply stabilization
        float weight = 1.0f / static_cast<float>(std::max(1, activeBrush.stabilization));
        float stabilizedX = m_PrevStabilizedX + weight * (currRawX - m_PrevStabilizedX);
        float stabilizedY = m_PrevStabilizedY + weight * (currRawY - m_PrevStabilizedY);

        // Backup tiles covered by the stroke segment (and its symmetries)
        auto backupSegment = [&](float x0, float y0, float x1, float y1) {
            float minX = std::min(x0, x1) - activeBrush.radius;
            float maxX = std::max(x0, x1) + activeBrush.radius;
            float minY = std::min(y0, y1) - activeBrush.radius;
            float maxY = std::max(y0, y1) + activeBrush.radius;

            int minTileX = std::max(0, static_cast<int>(minX) / 256);
            int maxTileX = std::max(0, static_cast<int>(maxX) / 256);
            int minTileY = std::max(0, static_cast<int>(minY) / 256);
            int maxTileY = std::max(0, static_cast<int>(maxY) / 256);

            for (int ty = minTileY; ty <= std::min(maxTileY, numTilesY - 1); ++ty) {
                for (int tx = minTileX; tx <= std::min(maxTileX, numTilesX - 1); ++tx) {
                    BackupTile(tx, ty);
                }
            }
        };

        backupSegment(m_PrevStabilizedX, m_PrevStabilizedY, stabilizedX, stabilizedY);
        if (m_MirrorHorizontal) {
            backupSegment(static_cast<float>(m_Width) - m_PrevStabilizedX, m_PrevStabilizedY,
                          static_cast<float>(m_Width) - stabilizedX, stabilizedY);
        }
        if (m_MirrorVertical) {
            backupSegment(m_PrevStabilizedX, static_cast<float>(m_Height) - m_PrevStabilizedY,
                          stabilizedX, static_cast<float>(m_Height) - stabilizedY);
        }
        if (m_MirrorHorizontal && m_MirrorVertical) {
            backupSegment(static_cast<float>(m_Width) - m_PrevStabilizedX, static_cast<float>(m_Height) - m_PrevStabilizedY,
                          static_cast<float>(m_Width) - stabilizedX, static_cast<float>(m_Height) - stabilizedY);
        }

        // Draw stroke segment
        PaintEngine::DrawStrokeSegment(*m_Layers[m_ActiveLayerIdx].tileCache,
                                       m_PrevStabilizedX, m_PrevStabilizedY,
                                       stabilizedX, stabilizedY,
                                       activeBrush, m_StrokeDistanceAccumulator,
                                       m_LastDabX, m_LastDabY,
                                       m_MirrorHorizontal, m_MirrorVertical,
                                       m_HasSelection ? m_SelectionMask : std::vector<uint8_t>{});

        m_PrevStabilizedX = stabilizedX;
        m_PrevStabilizedY = stabilizedY;
        m_Layers[m_ActiveLayerIdx].needsUpload = true;
        m_Layers[m_ActiveLayerIdx].filtersDirty = true;
    }
    else if (phase == StrokePhase::End) {
        m_IsStrokeActive = false;
        if (!m_ActiveStrokeDeltas.empty()) {
            auto& layer = m_Layers[m_ActiveLayerIdx];
            std::vector<TileDelta> deltas;
            deltas.reserve(m_ActiveStrokeDeltas.size());

            for (auto& pair : m_ActiveStrokeDeltas) {
                auto& delta = pair.second;
                // Snapshot the tile AFTER the stroke (newPixels)
                delta.newPixels = layer.tileCache
                    ? layer.tileCache->SnapshotTile(delta.tileX, delta.tileY)
                    : std::vector<uint8_t>{};
                deltas.push_back(std::move(delta));
            }

            auto cmd = std::make_shared<PaintStrokeCommand>(
                brush.erase ? "Eraser Stroke" : "Brush Stroke",
                m_ActiveLayerIdx,
                std::move(deltas)
            );
            m_UndoRedoManager.PushCommand(cmd);
            m_ActiveStrokeDeltas.clear();
            m_IsDocumentModified = true;
        }
    }
}

void Canvas::SmudgeOnActiveLayer(float x, float y, StrokePhase phase, const SmudgeSettings& s) {
    if (m_ActiveLayerIdx<0||m_ActiveLayerIdx>=(int)m_Layers.size()) return;
    Layer& layer=m_Layers[m_ActiveLayerIdx];
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    int r=std::max(1,(int)s.radius);

    auto pickupAt=[&](float cx, float cy){
        float acc[4]={0,0,0,0}; int cnt=0;
        for (int dy=-r;dy<=r;++dy) for (int dx=-r;dx<=r;++dx) {
            if (dx*dx+dy*dy>r*r) continue;
            int px=std::clamp((int)cx+dx,0,m_Width-1);
            int py=std::clamp((int)cy+dy,0,m_Height-1);
            float rgba[4];
            layer.tileCache->GetPixelF(px, py, rgba);
            for(int c=0;c<4;++c) acc[c]+=rgba[c]; ++cnt;
        }
        if (cnt>0) for(int c=0;c<4;++c) m_SmudgePickup[c]=acc[c]/(float)cnt;
    };

    if (phase==StrokePhase::Begin) {
        m_SmudgePickupValid=false; m_SmudgeLastX=x; m_SmudgeLastY=y; m_SmudgeDistAcc=0.f;
        pickupAt(x,y); m_SmudgePickupValid=true; return;
    }
    if (phase==StrokePhase::End) { m_SmudgePickupValid=false; return; }
    if (!m_SmudgePickupValid) return;

    float ddx=x-m_SmudgeLastX, ddy=y-m_SmudgeLastY;
    float dist=sqrtf(ddx*ddx+ddy*ddy);
    m_SmudgeDistAcc+=dist;
    float minDist=s.radius*s.spacing;
    if (m_SmudgeDistAcc<minDist) return;
    m_SmudgeDistAcc=0.f; m_SmudgeLastX=x; m_SmudgeLastY=y;

    for (int ky=-r;ky<=r;++ky) for (int kx=-r;kx<=r;++kx) {
        float d2=sqrtf((float)(kx*kx+ky*ky));
        if (d2>(float)r) continue;
        float falloff=1.f-d2/(float)r;
        float blend=s.strength*falloff;
        int px=std::clamp((int)x+kx,0,m_Width-1);
        int py=std::clamp((int)y+ky,0,m_Height-1);
        float sel = GetSelWeight(m_SelectionMask, m_Width, px, py, m_HasSelection);
        if (sel<1e-4f) continue;
        float rgba[4];
        layer.tileCache->GetPixelF(px, py, rgba);
        for(int c=0;c<4;++c) rgba[c]=rgba[c]*(1.f-blend*sel)+m_SmudgePickup[c]*blend*sel;
        layer.tileCache->SetPixelF(px, py, rgba);
    }
    pickupAt(x,y);
    layer.needsUpload=true;
    layer.filtersDirty=true;
}

void Canvas::ApplyBucketFill(int startX, int startY, float tolerance, const float color[4], bool contiguous) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;
    if (startX < 0 || startX >= m_Width || startY < 0 || startY >= m_Height) return;

    auto& layer = m_Layers[m_ActiveLayerIdx];
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);
    std::vector<float> layerPixels = ExportLayerF(layer, m_Width, m_Height);

    cv::Mat mat = ImageManager::PixelsToMat8UC3(layerPixels, m_Width, m_Height);
    cv::Mat temp = cv::Mat::zeros(m_Height, m_Width, CV_8UC1);

    if (contiguous) {
        cv::Mat mask = cv::Mat::zeros(m_Height + 2, m_Width + 2, CV_8UC1);
        cv::Scalar loDiff(tolerance * 255.0f, tolerance * 255.0f, tolerance * 255.0f);
        cv::Scalar upDiff(tolerance * 255.0f, tolerance * 255.0f, tolerance * 255.0f);
        cv::floodFill(mat, mask, cv::Point(startX, startY), cv::Scalar(255), nullptr, loDiff, upDiff, 4 | cv::FLOODFILL_MASK_ONLY | (255 << 8));
        temp = mask(cv::Range(1, m_Height + 1), cv::Range(1, m_Width + 1)).clone();
    } else {
        cv::Vec3b seedColor = mat.at<cv::Vec3b>(startY, startX);
        for (int y = 0; y < m_Height; ++y) {
            for (int x = 0; x < m_Width; ++x) {
                cv::Vec3b colorVal = mat.at<cv::Vec3b>(y, x);
                float diff = std::sqrt(
                    std::pow(static_cast<float>(colorVal[0]) - seedColor[0], 2) +
                    std::pow(static_cast<float>(colorVal[1]) - seedColor[1], 2) +
                    std::pow(static_cast<float>(colorVal[2]) - seedColor[2], 2)
                ) / 255.0f;
                if (diff <= tolerance * std::sqrt(3.0f)) {
                    temp.at<uint8_t>(y, x) = 255;
                }
            }
        }
    }

    for (int ty = 0; ty < (m_Height + 255) / 256; ++ty) {
        for (int tx = 0; tx < (m_Width + 255) / 256; ++tx) {
            bool tileAffected = false;
            for (int y = ty * 256; y < std::min((ty + 1) * 256, m_Height); ++y) {
                for (int x = tx * 256; x < std::min((tx + 1) * 256, m_Width); ++x) {
                    if (temp.at<uint8_t>(y, x) > 0) {
                        tileAffected = true;
                        break;
                    }
                }
                if (tileAffected) break;
            }
            if (tileAffected) {
                BackupTile(tx, ty);
            }
        }
    }

    for (int y = 0; y < m_Height; ++y) {
        for (int x = 0; x < m_Width; ++x) {
            float selectionVal = m_HasSelection ? SelU82F(m_SelectionMask[(size_t)y * m_Width + x]) : 1.0f;
            float maskVal = temp.at<uint8_t>(y, x) / 255.0f;
            float fillAlpha = maskVal * selectionVal * color[3];

            if (fillAlpha > 0.0f) {
                float dest[4];
                layer.tileCache->GetPixelF(x, y, dest);
                float outA = fillAlpha + dest[3] * (1.0f - fillAlpha);
                if (outA > 0.0f) {
                    float out[4];
                    out[0] = (color[0] * fillAlpha + dest[0] * dest[3] * (1.0f - fillAlpha)) / outA;
                    out[1] = (color[1] * fillAlpha + dest[1] * dest[3] * (1.0f - fillAlpha)) / outA;
                    out[2] = (color[2] * fillAlpha + dest[2] * dest[3] * (1.0f - fillAlpha)) / outA;
                    out[3] = outA;
                    layer.tileCache->SetPixelF(x, y, out);
                }
            }
        }
    }

    layer.needsUpload = true;
    layer.filtersDirty = true;
    m_IsDocumentModified = true;

    if (!m_ActiveStrokeDeltas.empty()) {
        std::vector<TileDelta> deltas;
        for (auto& pair : m_ActiveStrokeDeltas) {
            auto& delta = pair.second;
            delta.newPixels = layer.tileCache
                ? layer.tileCache->SnapshotTile(delta.tileX, delta.tileY)
                : std::vector<uint8_t>{};
            deltas.push_back(std::move(delta));
        }
        m_UndoRedoManager.PushCommand(std::make_shared<PaintStrokeCommand>("Bucket Fill", m_ActiveLayerIdx, std::move(deltas)));
        m_ActiveStrokeDeltas.clear();
    }
}

void Canvas::ApplyGradient(int x1, int y1, int x2, int y2, const float startColor[4], const float endColor[4]) {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;

    auto& layer = m_Layers[m_ActiveLayerIdx];
    EnsureLayerTileCache(layer, m_Width, m_Height, m_CanvasFormat);

    float dx = (float)(x2 - x1);
    float dy = (float)(y2 - y1);
    float lenSq = dx * dx + dy * dy;

    for (int ty = 0; ty < (m_Height + 255) / 256; ++ty) {
        for (int tx = 0; tx < (m_Width + 255) / 256; ++tx) {
            BackupTile(tx, ty);
        }
    }

    for (int y = 0; y < m_Height; ++y) {
        for (int x = 0; x < m_Width; ++x) {
            float t = 0.0f;
            if (lenSq > 0.001f) {
                float vx = (float)(x - x1);
                float vy = (float)(y - y1);
                t = (vx * dx + vy * dy) / lenSq;
                t = std::clamp(t, 0.0f, 1.0f);
            }

            float lerpColor[4];
            lerpColor[0] = startColor[0] * (1.0f - t) + endColor[0] * t;
            lerpColor[1] = startColor[1] * (1.0f - t) + endColor[1] * t;
            lerpColor[2] = startColor[2] * (1.0f - t) + endColor[2] * t;
            lerpColor[3] = startColor[3] * (1.0f - t) + endColor[3] * t;

            float selectionVal = m_HasSelection ? SelU82F(m_SelectionMask[(size_t)y * m_Width + x]) : 1.0f;
            float alphaVal = lerpColor[3] * selectionVal;

            if (alphaVal > 0.0f) {
                float dest[4];
                layer.tileCache->GetPixelF(x, y, dest);
                float outA = alphaVal + dest[3] * (1.0f - alphaVal);
                if (outA > 0.0f) {
                    float out[4];
                    out[0] = (lerpColor[0] * alphaVal + dest[0] * dest[3] * (1.0f - alphaVal)) / outA;
                    out[1] = (lerpColor[1] * alphaVal + dest[1] * dest[3] * (1.0f - alphaVal)) / outA;
                    out[2] = (lerpColor[2] * alphaVal + dest[2] * dest[3] * (1.0f - alphaVal)) / outA;
                    out[3] = outA;
                    layer.tileCache->SetPixelF(x, y, out);
                }
            }
        }
    }

    layer.needsUpload = true;
    layer.filtersDirty = true;
    m_IsDocumentModified = true;

    if (!m_ActiveStrokeDeltas.empty()) {
        std::vector<TileDelta> deltas;
        for (auto& pair : m_ActiveStrokeDeltas) {
            auto& delta = pair.second;
            delta.newPixels = layer.tileCache
                ? layer.tileCache->SnapshotTile(delta.tileX, delta.tileY)
                : std::vector<uint8_t>{};
            deltas.push_back(std::move(delta));
        }
        m_UndoRedoManager.PushCommand(std::make_shared<PaintStrokeCommand>("Gradient", m_ActiveLayerIdx, std::move(deltas)));
        m_ActiveStrokeDeltas.clear();
    }
}

// ============================================================
// Move Pixels operations
// ============================================================

void Canvas::StartMovePixels() {
    if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return;
    if (m_IsMovingPixels) {
        CommitMovePixels();
    }
    
    auto& layer = m_Layers[m_ActiveLayerIdx];
    if (!layer.tileCache) return;
    
    m_StartActiveLayerIdx = m_ActiveLayerIdx;
    
    // Backup all tiles for Undo
    int tilesX = (m_Width + TILE_SIZE - 1) / TILE_SIZE;
    int tilesY = (m_Height + TILE_SIZE - 1) / TILE_SIZE;
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            BackupTile(tx, ty);
        }
    }
    
    // Create floating TileCache with the same format
    m_FloatingTileCache = std::make_unique<TileCache>();
    m_FloatingTileCache->Init(m_Width, m_Height, layer.tileCache->GetFormat());
    
    m_OriginalSelectionMask.assign(m_Width * m_Height, 255);
    if (m_HasSelection) {
        std::copy(m_SelectionMask.begin(), m_SelectionMask.end(), m_OriginalSelectionMask.begin());
    }
    
    // If there is selection — copy only selected pixels
    if (m_HasSelection) {
        for (int ty = 0; ty < tilesY; ++ty) {
            for (int tx = 0; tx < tilesX; ++tx) {
                const uint8_t* srcTile = layer.tileCache->GetTileData(tx, ty);
                if (!srcTile) continue;
                
                // Check if there is selection in this tile
                bool hasMaskInTile = false;
                for (int py = 0; py < TILE_SIZE && !hasMaskInTile; ++py) {
                    int canvasY = ty * TILE_SIZE + py;
                    if (canvasY >= m_Height) break;
                    for (int px = 0; px < TILE_SIZE; ++px) {
                        int canvasX = tx * TILE_SIZE + px;
                        if (canvasX >= m_Width) break;
                        if (m_SelectionMask[(size_t)canvasY * m_Width + canvasX] > 0) {
                            hasMaskInTile = true;
                            break;
                        }
                    }
                }
                if (!hasMaskInTile) continue;
                
                // Copy tile to floating cache
                uint8_t* dstTile = m_FloatingTileCache->LockTile(tx, ty);
                std::memcpy(dstTile, srcTile, (size_t)TILE_SIZE * TILE_SIZE * layer.tileCache->GetBytesPerPixel());
                
                // Clear source in selection zone (cut)
                uint8_t* srcMutable = layer.tileCache->LockTile(tx, ty);
                for (int py = 0; py < TILE_SIZE; ++py) {
                    int canvasY = ty * TILE_SIZE + py;
                    if (canvasY >= m_Height) break;
                    for (int px = 0; px < TILE_SIZE; ++px) {
                        int canvasX = tx * TILE_SIZE + px;
                        if (canvasX >= m_Width) break;
                        float weight = SelU82F(m_SelectionMask[(size_t)canvasY * m_Width + canvasX]);
                        if (weight > 0.0f) {
                            int bpp = layer.tileCache->GetBytesPerPixel();
                            int pixOff = (py * TILE_SIZE + px) * bpp;
                            
                            if (layer.tileCache->GetFormat() == CanvasPixelFormat::RGBA8) {
                                uint8_t* dp = srcMutable + pixOff;
                                dp[0] = (uint8_t)(dp[0] * (1.0f - weight));
                                dp[1] = (uint8_t)(dp[1] * (1.0f - weight));
                                dp[2] = (uint8_t)(dp[2] * (1.0f - weight));
                                dp[3] = (uint8_t)(dp[3] * (1.0f - weight));
                            } else {
                                float* dp = reinterpret_cast<float*>(srcMutable + pixOff);
                                dp[0] *= (1.0f - weight);
                                dp[1] *= (1.0f - weight);
                                dp[2] *= (1.0f - weight);
                                dp[3] *= (1.0f - weight);
                            }
                        }
                    }
                }
                layer.tileCache->MarkDirty(tx, ty);
            }
        }
    } else {
        // No selection — floating = whole layer (sparse copy)
        m_FloatingTileCache->CopyFrom(*layer.tileCache, 0, 0, 0, 0, m_Width, m_Height);
        layer.tileCache->Clear();
        layer.tileCache->MarkAllDirty();
    }
    
    m_FloatingOffsetX = 0;
    m_FloatingOffsetY = 0;
    m_FloatingScaleX = 1.0f;
    m_FloatingScaleY = 1.0f;
    m_FloatingRotation = 0.0f;
    m_IsMovingPixels = true;
    
    if (layer.hasMask) {
        if (layer.maskDirtyTiles.size() != (size_t)tilesX * tilesY) {
            layer.maskDirtyTiles.assign(tilesX * tilesY, false);
        }
        for (int y = 0; y < m_Height; ++y) {
            for (int x = 0; x < m_Width; ++x) {
                size_t idx = (size_t)y * m_Width + x;
                if (m_OriginalSelectionMask[idx] > 0) {
                    layer.mask[idx] = 255;
                    int tx = x / TILE_SIZE;
                    int ty = y / TILE_SIZE;
                    int tileIdx = ty * tilesX + tx;
                    if (tileIdx < (int)layer.maskDirtyTiles.size()) {
                        layer.maskDirtyTiles[tileIdx] = true;
                    }
                }
            }
        }
        layer.maskNeedsUpload = true;
    }
    
    layer.needsUpload = true;
    layer.filtersDirty = true;
    MarkCompositeResourcesDirty();
}

void Canvas::UpdateMovePixels(int dx, int dy) {
    if (!m_IsMovingPixels) return;
    m_FloatingOffsetX = dx;
    m_FloatingOffsetY = dy;
}

void Canvas::CommitMovePixels() {
    if (!m_IsMovingPixels || !m_FloatingTileCache) return;
    
    if (m_StartActiveLayerIdx >= 0 && m_StartActiveLayerIdx < (int)m_Layers.size()) {
        Layer& layer = m_Layers[m_StartActiveLayerIdx];
        if (layer.tileCache) {
            // Apply standard translation offset to destination TileCache
            layer.tileCache->CopyFrom(
                *m_FloatingTileCache,
                0, 0,
                m_FloatingOffsetX,
                m_FloatingOffsetY,
                m_Width, m_Height
            );
            layer.tileCache->MarkAllDirty();
            layer.needsUpload = true;
            layer.filtersDirty = true;
        }
        
        // Bounding box center for mask transformation
        int minX = m_Width, maxX = 0, minY = m_Height, maxY = 0;
        bool hasPixels = false;
        for (int y = 0; y < m_Height; ++y) {
            for (int x = 0; x < m_Width; ++x) {
                if (m_OriginalSelectionMask[(size_t)y * m_Width + x] > 0) {
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                    if (y < minY) minY = y;
                    if (y > maxY) maxY = y;
                    hasPixels = true;
                }
            }
        }
        float cx = hasPixels ? (minX + maxX) * 0.5f : m_Width * 0.5f;
        float cy = hasPixels ? (minY + maxY) * 0.5f : m_Height * 0.5f;

        // Shift the layer mask
        if (layer.hasMask) {
            std::vector<uint8_t> finalMask = layer.mask;
            int tilesX = (m_Width + TILE_SIZE - 1) / TILE_SIZE;
            int tilesY = (m_Height + TILE_SIZE - 1) / TILE_SIZE;
            if (layer.maskDirtyTiles.size() != (size_t)tilesX * tilesY) {
                layer.maskDirtyTiles.assign(tilesX * tilesY, false);
            }
            for (int y = 0; y < m_Height; ++y) {
                for (int x = 0; x < m_Width; ++x) {
                    float px = (float)x - cx;
                    float py = (float)y - cy;
                    px -= (float)m_FloatingOffsetX;
                    py -= (float)m_FloatingOffsetY;
                    
                    float angle = -m_FloatingRotation;
                    float cosA = std::cos(angle);
                    float sinA = std::sin(angle);
                    float rx = px * cosA - py * sinA;
                    float ry = px * sinA + py * cosA;
                    
                    float sx = rx;
                    float sy = ry;
                    if (m_FloatingScaleX > 0.0001f) sx /= m_FloatingScaleX;
                    if (m_FloatingScaleY > 0.0001f) sy /= m_FloatingScaleY;
                    
                    float srcX = sx + cx;
                    float srcY = sy + cy;
                    
                    if (srcX >= 0.0f && srcX <= (float)m_Width - 1.0f && srcY >= 0.0f && srcY <= (float)m_Height - 1.0f) {
                        float maskWeight = sampleBilinearMask(m_OriginalSelectionMask, m_Width, m_Height, srcX, srcY);
                        if (maskWeight > 0.0f) {
                            size_t destIdx = (size_t)y * m_Width + x;
                            uint8_t newVal = SelF2U8(maskWeight);
                            if (finalMask[destIdx] != newVal) {
                                finalMask[destIdx] = newVal;
                                int tx = x / TILE_SIZE;
                                int ty = y / TILE_SIZE;
                                int tileIdx = ty * tilesX + tx;
                                if (tileIdx < (int)layer.maskDirtyTiles.size()) {
                                    layer.maskDirtyTiles[tileIdx] = true;
                                }
                            }
                        }
                    }
                }
            }
            layer.mask = finalMask;
            layer.maskNeedsUpload = true;
        }

        // Shift selection mask
        if (m_HasSelection) {
            std::vector<uint8_t> shiftedSelection(m_Width * m_Height, 0);
            for (int y = 0; y < m_Height; ++y) {
                for (int x = 0; x < m_Width; ++x) {
                    float px = (float)x - cx;
                    float py = (float)y - cy;
                    px -= (float)m_FloatingOffsetX;
                    py -= (float)m_FloatingOffsetY;
                    
                    float angle = -m_FloatingRotation;
                    float cosA = std::cos(angle);
                    float sinA = std::sin(angle);
                    float rx = px * cosA - py * sinA;
                    float ry = px * sinA + py * cosA;
                    
                    float sx = rx;
                    float sy = ry;
                    if (m_FloatingScaleX > 0.0001f) sx /= m_FloatingScaleX;
                    if (m_FloatingScaleY > 0.0001f) sy /= m_FloatingScaleY;
                    
                    float srcX = sx + cx;
                    float srcY = sy + cy;
                    
                    if (srcX >= 0.0f && srcX <= (float)m_Width - 1.0f && srcY >= 0.0f && srcY <= (float)m_Height - 1.0f) {
                        float maskWeight = sampleBilinearMask(m_OriginalSelectionMask, m_Width, m_Height, srcX, srcY);
                        shiftedSelection[(size_t)y * m_Width + x] = SelF2U8(maskWeight);
                    }
                }
            }
            m_SelectionMask = shiftedSelection;
            MarkSelectionMaskDirty();
        }

        // Push undo state for the move operation
        if (!m_ActiveStrokeDeltas.empty()) {
            std::vector<TileDelta> deltas;
            for (auto& pair : m_ActiveStrokeDeltas) {
                auto& delta = pair.second;
                delta.newPixels = layer.tileCache
                    ? layer.tileCache->SnapshotTile(delta.tileX, delta.tileY)
                    : std::vector<uint8_t>{};
                deltas.push_back(std::move(delta));
            }
            m_UndoRedoManager.PushCommand(std::make_shared<PaintStrokeCommand>("Move Pixels", m_StartActiveLayerIdx, std::move(deltas)));
            m_ActiveStrokeDeltas.clear();
        }
    }
    
    m_FloatingTileCache.reset();
    m_OriginalSelectionMask.clear();
    m_FloatingOffsetX = 0;
    m_FloatingOffsetY = 0;
    m_IsMovingPixels = false;
    
    m_IsDocumentModified = true;
    MarkCompositeResourcesDirty();
}

void Canvas::CancelMovePixels() {
    if (!m_IsMovingPixels || !m_FloatingTileCache) return;
    
    if (m_StartActiveLayerIdx >= 0 && m_StartActiveLayerIdx < (int)m_Layers.size()) {
        Layer& layer = m_Layers[m_StartActiveLayerIdx];
        if (layer.tileCache) {
            // Restore original pixels from floating (without offset)
            layer.tileCache->CopyFrom(*m_FloatingTileCache, 0, 0, 0, 0, m_Width, m_Height);
            layer.tileCache->MarkAllDirty();
            layer.needsUpload = true;
            layer.filtersDirty = true;
        }
    }
    
    m_SelectionMask = m_OriginalSelectionMask;
    MarkSelectionMaskDirty();
    
    m_FloatingTileCache.reset();
    m_OriginalSelectionMask.clear();
    m_FloatingOffsetX = 0;
    m_FloatingOffsetY = 0;
    m_IsMovingPixels = false;
    
    m_ActiveStrokeDeltas.clear();
    
    MarkCompositeResourcesDirty();
}

void Canvas::DrawMoveGizmo(ImDrawList* dl, const std::function<ImVec2(float, float)>& canvasToScreen) {
    if (!m_IsMovingPixels) return;
    
    int minX = m_Width, maxX = 0, minY = m_Height, maxY = 0;
    bool hasPixels = false;
    for (int y = 0; y < m_Height; ++y) {
        for (int x = 0; x < m_Width; ++x) {
            if (m_OriginalSelectionMask[y * m_Width + x] > 0) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
                hasPixels = true;
            }
        }
    }
    
    if (hasPixels) {
        float cx = (minX + maxX) * 0.5f;
        float cy = (minY + maxY) * 0.5f;
        float cosA = std::cos(m_FloatingRotation);
        float sinA = std::sin(m_FloatingRotation);
        
        // Forward transform: scale then rotate around bounding box center, then translate
        auto transformCorner = [&](float px, float py) -> ImVec2 {
            float rx = (px - cx) * m_FloatingScaleX;
            float ry = (py - cy) * m_FloatingScaleY;
            float tx = rx * cosA - ry * sinA + cx + (float)m_FloatingOffsetX;
            float ty = rx * sinA + ry * cosA + cy + (float)m_FloatingOffsetY;
            return canvasToScreen(tx, ty);
        };
        
        ImVec2 p1 = transformCorner((float)minX, (float)minY); // TL
        ImVec2 p2 = transformCorner((float)maxX, (float)minY); // TR
        ImVec2 p3 = transformCorner((float)maxX, (float)maxY); // BR
        ImVec2 p4 = transformCorner((float)minX, (float)maxY); // BL
        
        // Midpoints for edge handles
        ImVec2 mT = { (p1.x + p2.x) * 0.5f, (p1.y + p2.y) * 0.5f };
        ImVec2 mR = { (p2.x + p3.x) * 0.5f, (p2.y + p3.y) * 0.5f };
        ImVec2 mB = { (p3.x + p4.x) * 0.5f, (p3.y + p4.y) * 0.5f };
        ImVec2 mL = { (p4.x + p1.x) * 0.5f, (p4.y + p1.y) * 0.5f };
        
        ImU32 gizmoCol  = IM_COL32(0, 120, 215, 255);
        ImU32 shadowCol = IM_COL32(0, 0, 0, 120);
        float thickness = 2.0f;

        // Shadow
        dl->AddLine(ImVec2(p1.x+1,p1.y+1), ImVec2(p2.x+1,p2.y+1), shadowCol, thickness);
        dl->AddLine(ImVec2(p2.x+1,p2.y+1), ImVec2(p3.x+1,p3.y+1), shadowCol, thickness);
        dl->AddLine(ImVec2(p3.x+1,p3.y+1), ImVec2(p4.x+1,p4.y+1), shadowCol, thickness);
        dl->AddLine(ImVec2(p4.x+1,p4.y+1), ImVec2(p1.x+1,p1.y+1), shadowCol, thickness);
        // Border
        dl->AddLine(p1, p2, gizmoCol, thickness);
        dl->AddLine(p2, p3, gizmoCol, thickness);
        dl->AddLine(p3, p4, gizmoCol, thickness);
        dl->AddLine(p4, p1, gizmoCol, thickness);
        
        float hs = 5.0f;
        auto drawHandle = [&](ImVec2 p) {
            dl->AddRectFilled(ImVec2(p.x - hs, p.y - hs), ImVec2(p.x + hs, p.y + hs), IM_COL32(255, 255, 255, 230));
            dl->AddRect(ImVec2(p.x - hs, p.y - hs), ImVec2(p.x + hs, p.y + hs), gizmoCol, 0.0f, 0, 1.5f);
        };
        auto drawEdgeHandle = [&](ImVec2 p) {
            float hs2 = 4.0f;
            dl->AddRectFilled(ImVec2(p.x - hs2, p.y - hs2), ImVec2(p.x + hs2, p.y + hs2), IM_COL32(200, 220, 255, 200));
            dl->AddRect(ImVec2(p.x - hs2, p.y - hs2), ImVec2(p.x + hs2, p.y + hs2), gizmoCol, 0.0f, 0, 1.0f);
        };
        // Corner handles
        drawHandle(p1); drawHandle(p2); drawHandle(p3); drawHandle(p4);
        // Edge handles
        drawEdgeHandle(mT); drawEdgeHandle(mR); drawEdgeHandle(mB); drawEdgeHandle(mL);
    }
}
