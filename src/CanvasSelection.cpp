#include "Canvas.h"
#include "core/Logger.h"
#include "core/ImageManager.h"
#include "core/ThreadPool.h"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <thread>
#include <queue>
#include <future>

// --- Helpers ---
static std::vector<float> ExportLayerF(const Layer& layer, int w, int h) {
    std::vector<float> out((size_t)w * h * 4, 0.f);
    if (layer.tileCache) layer.tileCache->ExportRGBA32F(out.data(), w, h);
    return out;
}

// ============================================================
// Selection System
// ============================================================

void Canvas::SelectAll() {
    m_SelectionMask.assign((size_t)m_Width * m_Height, 255);
    m_HasSelection = true;
    m_SelectionMaskNeedsUpload = true;
    Logger::Get().Info("SelectAll");
}

void Canvas::ClearSelection() {
    if (!m_HasSelection) return;
    std::vector<uint8_t> oldMask = m_SelectionMask;
    bool oldHasSelection = m_HasSelection;

    m_SelectionMask.assign(m_Width * m_Height, 0);
    m_HasSelection = false;
    m_SelectionMaskNeedsUpload = true;

    m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>("Deselect", std::move(oldMask), oldHasSelection, m_SelectionMask, m_HasSelection));
}

void Canvas::InvertSelection() {
    if (!m_HasSelection) { SelectAll(); return; }
    for (auto& v : m_SelectionMask) v = 255 - v;
    m_SelectionMaskNeedsUpload = true;
    Logger::Get().Info("InvertSelection");
}

void Canvas::SetSelectionMask(const std::vector<uint8_t>& mask) {
    if (mask.size() == (size_t)m_Width * m_Height) {
        m_SelectionMask = mask;
        m_HasSelection = false;
        for (uint8_t val : m_SelectionMask) {
            if (val > 0) {
                m_HasSelection = true;
                break;
            }
        }
        m_SelectionMaskNeedsUpload = true;
    }
}

void Canvas::MarkSelectionMaskDirty() {
    m_SelectionMaskNeedsUpload = true;
    m_CompositeDirty = true;
}

void Canvas::ApplyRectSelection(int x1, int y1, int x2, int y2, bool add, bool subtract) {
    std::vector<uint8_t> oldMask = m_SelectionMask;
    bool oldHasSelection = m_HasSelection;

    cv::Mat temp = cv::Mat::zeros(m_Height, m_Width, CV_8UC1);
    cv::rectangle(temp, cv::Point(x1, y1), cv::Point(x2, y2), cv::Scalar(255), -1);

    cv::Mat current(m_Height, m_Width, CV_8UC1);
    if (!m_SelectionMask.empty()) {
        std::memcpy(current.data, m_SelectionMask.data(), m_SelectionMask.size());
    } else {
        current = cv::Mat::zeros(m_Height, m_Width, CV_8UC1);
    }
    cv::Mat combined;
    if (add) {
        cv::bitwise_or(current, temp, combined);
    } else if (subtract) {
        cv::bitwise_and(current, ~temp, combined);
    } else {
        combined = temp.clone();
    }

    m_SelectionMask.resize((size_t)m_Width * m_Height);
    std::memcpy(m_SelectionMask.data(), combined.data, m_SelectionMask.size());
    m_HasSelection = false;
    for (uint8_t val : m_SelectionMask) {
        if (val > 0) {
            m_HasSelection = true;
            break;
        }
    }
    m_SelectionMaskNeedsUpload = true;

    m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>("Select", std::move(oldMask), oldHasSelection, m_SelectionMask, m_HasSelection));
}

void Canvas::ApplyEllipseSelection(int x1, int y1, int x2, int y2, bool add, bool subtract) {
    std::vector<uint8_t> oldMask = m_SelectionMask;
    bool oldHasSelection = m_HasSelection;

    cv::Mat temp = cv::Mat::zeros(m_Height, m_Width, CV_8UC1);
    cv::Point center((x1 + x2) / 2, (y1 + y2) / 2);
    cv::Size axes(std::abs(x2 - x1) / 2, std::abs(y2 - y1) / 2);
    if (axes.width > 0 && axes.height > 0) {
        cv::ellipse(temp, center, axes, 0.0, 0.0, 360.0, cv::Scalar(255), -1);
    }

    cv::Mat current(m_Height, m_Width, CV_8UC1);
    if (!m_SelectionMask.empty()) {
        std::memcpy(current.data, m_SelectionMask.data(), m_SelectionMask.size());
    } else {
        current = cv::Mat::zeros(m_Height, m_Width, CV_8UC1);
    }
    cv::Mat combined;
    if (add) {
        cv::bitwise_or(current, temp, combined);
    } else if (subtract) {
        cv::bitwise_and(current, ~temp, combined);
    } else {
        combined = temp.clone();
    }

    m_SelectionMask.resize((size_t)m_Width * m_Height);
    std::memcpy(m_SelectionMask.data(), combined.data, m_SelectionMask.size());
    m_HasSelection = false;
    for (uint8_t val : m_SelectionMask) {
        if (val > 0) {
            m_HasSelection = true;
            break;
        }
    }
    m_SelectionMaskNeedsUpload = true;

    m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>("Select", std::move(oldMask), oldHasSelection, m_SelectionMask, m_HasSelection));
}

void Canvas::ApplyLassoSelection(const std::vector<std::pair<int, int>>& points, bool add, bool subtract) {
    std::vector<uint8_t> oldMask = m_SelectionMask;
    bool oldHasSelection = m_HasSelection;

    cv::Mat temp = cv::Mat::zeros(m_Height, m_Width, CV_8UC1);
    if (points.size() >= 3) {
        std::vector<cv::Point> cvPoints;
        for (const auto& p : points) {
            cvPoints.push_back(cv::Point(p.first, p.second));
        }
        std::vector<std::vector<cv::Point>> polys = { cvPoints };
        cv::fillPoly(temp, polys, cv::Scalar(255));
    }

    cv::Mat current(m_Height, m_Width, CV_8UC1);
    if (!m_SelectionMask.empty()) {
        std::memcpy(current.data, m_SelectionMask.data(), m_SelectionMask.size());
    } else {
        current = cv::Mat::zeros(m_Height, m_Width, CV_8UC1);
    }
    cv::Mat combined;
    if (add) {
        cv::bitwise_or(current, temp, combined);
    } else if (subtract) {
        cv::bitwise_and(current, ~temp, combined);
    } else {
        combined = temp.clone();
    }

    m_SelectionMask.resize((size_t)m_Width * m_Height);
    std::memcpy(m_SelectionMask.data(), combined.data, m_SelectionMask.size());
    m_HasSelection = false;
    for (uint8_t val : m_SelectionMask) {
        if (val > 0) {
            m_HasSelection = true;
            break;
        }
    }
    m_SelectionMaskNeedsUpload = true;

    m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>("Select", std::move(oldMask), oldHasSelection, m_SelectionMask, m_HasSelection));
}

void Canvas::ApplyMagicWandSelection(int startX, int startY, float tolerance, bool add, bool subtract, bool contiguous) {
    if (startX < 0 || startX >= m_Width || startY < 0 || startY >= m_Height) return;

    m_SmartSelectInProgress.store(true);
    m_SmartSelectCancelled.store(false);

    // Get seed color on main thread (very fast)
    float seedColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (m_ActiveLayerIdx >= 0 && m_ActiveLayerIdx < (int)m_Layers.size()) {
        auto& layer = m_Layers[m_ActiveLayerIdx];
        if (layer.tileCache) {
            layer.tileCache->GetPixelF(startX, startY, seedColor);
        }
    } else {
        for (const auto& layer : m_Layers) {
            if (layer.visible && layer.tileCache) {
                float px[4] = {};
                layer.tileCache->GetPixelF(startX, startY, px);
                float srcA = px[3] * layer.opacity;
                float destA = seedColor[3];
                float outA = srcA + destA * (1.0f - srcA);
                if (outA > 0.0f) {
                    seedColor[0] = (px[0] * srcA + seedColor[0] * destA * (1.0f - srcA)) / outA;
                    seedColor[1] = (px[1] * srcA + seedColor[1] * destA * (1.0f - srcA)) / outA;
                    seedColor[2] = (px[2] * srcA + seedColor[2] * destA * (1.0f - srcA)) / outA;
                    seedColor[3] = outA;
                }
            }
        }
    }

    std::vector<uint8_t> oldMask = m_SelectionMask;
    bool oldHasSelection = m_HasSelection;

    ThreadPool::Get().Enqueue([this, startX, startY, tolerance, add, subtract, contiguous, seedColor, oldMask, oldHasSelection]() {
        std::vector<uint8_t> newMask(m_Width * m_Height, 0);

        if (contiguous) {
            MagicWandBFS_Tiled(newMask, startX, startY, seedColor, tolerance);
        } else {
            MagicWandGlobalThreshold(newMask, seedColor, tolerance);
        }

        if (m_SmartSelectCancelled.load()) {
            m_SmartSelectInProgress.store(false);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_SelectionMutex);
            cv::Mat current(m_Height, m_Width, CV_8UC1);
            if (!oldMask.empty()) {
                std::memcpy(current.data, oldMask.data(), oldMask.size());
            } else {
                current = cv::Mat::zeros(m_Height, m_Width, CV_8UC1);
            }

            cv::Mat temp(m_Height, m_Width, CV_8UC1, newMask.data());
            cv::Mat combined;
            if (add) {
                cv::bitwise_or(current, temp, combined);
            } else if (subtract) {
                cv::bitwise_and(current, ~temp, combined);
            } else {
                combined = temp.clone();
            }

            m_SelectionMask.resize((size_t)m_Width * m_Height);
            std::memcpy(m_SelectionMask.data(), combined.data, m_SelectionMask.size());
            
            m_HasSelection = false;
            for (uint8_t val : m_SelectionMask) {
                if (val > 0) {
                    m_HasSelection = true;
                    break;
                }
            }
            m_SelectionMaskNeedsUpload = true;
        }

        MarkSelectionMaskDirty();

        m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>("Select", std::move(oldMask), oldHasSelection, m_SelectionMask, m_HasSelection));
        m_SmartSelectInProgress.store(false);
    });
}

void Canvas::MagicWandBFS_Tiled(std::vector<uint8_t>& outMask, int startX, int startY, const float seedColor[4], float tolerance) {
    std::vector<uint8_t> visited(m_Width * m_Height, 0);
    std::queue<uint32_t> bfsQueue;

    bfsQueue.push(((uint32_t)startY << 16) | (uint32_t)startX);
    visited[(size_t)startY * m_Width + startX] = 1;

    int lastTx = -1, lastTy = -1;
    const uint8_t* lastTileData = nullptr;
    bool lastTileExists = false;

    const TileCache* activeTileCache = nullptr;
    if (m_ActiveLayerIdx >= 0 && m_ActiveLayerIdx < (int)m_Layers.size()) {
        activeTileCache = m_Layers[m_ActiveLayerIdx].tileCache.get();
    }
    CanvasPixelFormat format = activeTileCache ? activeTileCache->GetFormat() : CanvasPixelFormat::RGBA8;

    auto getPixelAt = [&](int x, int y, float rgba[4]) {
        if (activeTileCache) {
            int tx = x / TILE_SIZE;
            int ty = y / TILE_SIZE;
            int px = x % TILE_SIZE;
            int py = y % TILE_SIZE;

            if (tx != lastTx || ty != lastTy) {
                lastTx = tx;
                lastTy = ty;
                lastTileData = activeTileCache->GetTileData(tx, ty);
                lastTileExists = (lastTileData != nullptr);
            }

            if (!lastTileExists) {
                rgba[0] = 0.0f; rgba[1] = 0.0f; rgba[2] = 0.0f; rgba[3] = 0.0f;
                return;
            }

            if (format == CanvasPixelFormat::RGBA8) {
                const uint8_t* p = lastTileData + (py * TILE_SIZE + px) * 4;
                rgba[0] = p[0] / 255.0f;
                rgba[1] = p[1] / 255.0f;
                rgba[2] = p[2] / 255.0f;
                rgba[3] = p[3] / 255.0f;
            } else {
                const float* p = reinterpret_cast<const float*>(lastTileData + (py * TILE_SIZE + px) * 16);
                rgba[0] = p[0];
                rgba[1] = p[1];
                rgba[2] = p[2];
                rgba[3] = p[3];
            }
        } else {
            rgba[0] = 0.0f; rgba[1] = 0.0f; rgba[2] = 0.0f; rgba[3] = 0.0f;
            for (const auto& l : m_Layers) {
                if (l.visible && l.tileCache) {
                    float px[4] = {};
                    l.tileCache->GetPixelF(x, y, px);
                    float srcA = px[3] * l.opacity;
                    float destA = rgba[3];
                    float outA = srcA + destA * (1.0f - srcA);
                    if (outA > 0.0f) {
                        rgba[0] = (px[0] * srcA + rgba[0] * destA * (1.0f - srcA)) / outA;
                        rgba[1] = (px[1] * srcA + rgba[1] * destA * (1.0f - srcA)) / outA;
                        rgba[2] = (px[2] * srcA + rgba[2] * destA * (1.0f - srcA)) / outA;
                        rgba[3] = outA;
                    }
                }
            }
        }
    };

    auto colorMatch = [&](int x, int y) -> bool {
        float px[4] = {};
        getPixelAt(x, y, px);
        float diff = std::sqrt(
            std::pow(px[0] - seedColor[0], 2) +
            std::pow(px[1] - seedColor[1], 2) +
            std::pow(px[2] - seedColor[2], 2)
        );
        return diff <= tolerance * std::sqrt(3.0f);
    };

    const int dx[] = {1, -1, 0, 0};
    const int dy[] = {0, 0, 1, -1};

    size_t iterations = 0;
    while (!bfsQueue.empty()) {
        uint32_t val = bfsQueue.front();
        bfsQueue.pop();

        int x = val & 0xFFFF;
        int y = val >> 16;
        outMask[(size_t)y * m_Width + x] = 255;

        for (int d = 0; d < 4; ++d) {
            int nx = x + dx[d];
            int ny = y + dy[d];
            if (nx < 0 || ny < 0 || nx >= m_Width || ny >= m_Height) continue;
            size_t idx = (size_t)ny * m_Width + nx;
            if (visited[idx]) continue;
            visited[idx] = 1;
            if (colorMatch(nx, ny)) {
                bfsQueue.push(((uint32_t)ny << 16) | (uint32_t)nx);
            }
        }

        if (++iterations % 10000 == 0 && m_SmartSelectCancelled.load()) {
            break;
        }
    }
}

void Canvas::MagicWandGlobalThreshold(std::vector<uint8_t>& outMask, const float seedColor[4], float tolerance) {
    int tilesX = (m_Width + TILE_SIZE - 1) / TILE_SIZE;
    int tilesY = (m_Height + TILE_SIZE - 1) / TILE_SIZE;

    const TileCache* activeTileCache = nullptr;
    if (m_ActiveLayerIdx >= 0 && m_ActiveLayerIdx < (int)m_Layers.size()) {
        activeTileCache = m_Layers[m_ActiveLayerIdx].tileCache.get();
    }
    CanvasPixelFormat format = activeTileCache ? activeTileCache->GetFormat() : CanvasPixelFormat::RGBA8;

    std::vector<std::future<void>> futures;
    futures.reserve(tilesY);

    for (int ty = 0; ty < tilesY; ++ty) {
        futures.push_back(ThreadPool::Get().Enqueue([this, ty, tilesX, activeTileCache, format, seedColor, tolerance, &outMask]() {
            if (m_SmartSelectCancelled.load()) return;

            float toleranceLimit = tolerance * std::sqrt(3.0f);
            int startY = ty * TILE_SIZE;
            int endY = std::min((ty + 1) * TILE_SIZE, m_Height);

            for (int y = startY; y < endY; ++y) {
                if (m_SmartSelectCancelled.load()) break;
                for (int tx = 0; tx < tilesX; ++tx) {
                    int startX = tx * TILE_SIZE;
                    int endX = std::min((tx + 1) * TILE_SIZE, m_Width);

                    const uint8_t* tileData = activeTileCache ? activeTileCache->GetTileData(tx, ty) : nullptr;

                    if (activeTileCache && !tileData) {
                        float diff = std::sqrt(
                            std::pow(0.0f - seedColor[0], 2) +
                            std::pow(0.0f - seedColor[1], 2) +
                            std::pow(0.0f - seedColor[2], 2)
                        );
                        if (diff <= toleranceLimit) {
                            for (int x = startX; x < endX; ++x) {
                                outMask[(size_t)y * m_Width + x] = 255;
                            }
                        }
                    } else if (activeTileCache) {
                        if (format == CanvasPixelFormat::RGBA8) {
                            for (int x = startX; x < endX; ++x) {
                                int px = x % TILE_SIZE;
                                int py = y % TILE_SIZE;
                                const uint8_t* p = tileData + (py * TILE_SIZE + px) * 4;
                                float r = p[0] / 255.0f;
                                float g = p[1] / 255.0f;
                                float b = p[2] / 255.0f;
                                float diff = std::sqrt(
                                    std::pow(r - seedColor[0], 2) +
                                    std::pow(g - seedColor[1], 2) +
                                    std::pow(b - seedColor[2], 2)
                                );
                                if (diff <= toleranceLimit) {
                                    outMask[(size_t)y * m_Width + x] = 255;
                                }
                            }
                        } else {
                            for (int x = startX; x < endX; ++x) {
                                int px = x % TILE_SIZE;
                                int py = y % TILE_SIZE;
                                const float* p = reinterpret_cast<const float*>(tileData + (py * TILE_SIZE + px) * 16);
                                float diff = std::sqrt(
                                    std::pow(p[0] - seedColor[0], 2) +
                                    std::pow(p[1] - seedColor[1], 2) +
                                    std::pow(p[2] - seedColor[2], 2)
                                );
                                if (diff <= toleranceLimit) {
                                    outMask[(size_t)y * m_Width + x] = 255;
                                }
                            }
                        }
                    } else {
                        for (int x = startX; x < endX; ++x) {
                            float rgba[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                            for (const auto& l : m_Layers) {
                                if (l.visible && l.tileCache) {
                                    float px[4] = {};
                                    l.tileCache->GetPixelF(x, y, px);
                                    float srcA = px[3] * l.opacity;
                                    float destA = rgba[3];
                                    float outA = srcA + destA * (1.0f - srcA);
                                    if (outA > 0.0f) {
                                        rgba[0] = (px[0] * srcA + rgba[0] * destA * (1.0f - srcA)) / outA;
                                        rgba[1] = (px[1] * srcA + rgba[1] * destA * (1.0f - srcA)) / outA;
                                        rgba[2] = (px[2] * srcA + rgba[2] * destA * (1.0f - srcA)) / outA;
                                        rgba[3] = outA;
                                    }
                                }
                            }
                            float diff = std::sqrt(
                                std::pow(rgba[0] - seedColor[0], 2) +
                                std::pow(rgba[1] - seedColor[1], 2) +
                                std::pow(rgba[2] - seedColor[2], 2)
                            );
                            if (diff <= toleranceLimit) {
                                outMask[(size_t)y * m_Width + x] = 255;
                            }
                        }
                    }
                }
            }
        }));
    }

    for (auto& f : futures) {
        f.get();
    }
}

void Canvas::ApplySmartSelectSelection(const std::vector<std::pair<int, int>>& points, bool add, bool subtract) {
    if (points.empty()) return;

    m_SmartSelectInProgress.store(true);
    m_SmartSelectCancelled.store(false);

    std::vector<uint8_t> oldMask = m_SelectionMask;
    bool oldHasSelection = m_HasSelection;

    std::thread t([this, points, add, subtract, oldMask, oldHasSelection]() {
        std::vector<float> srcPixels;
        if (m_ActiveLayerIdx >= 0 && m_ActiveLayerIdx < (int)m_Layers.size()) {
            srcPixels = ExportLayerF(m_Layers[m_ActiveLayerIdx], m_Width, m_Height);
        } else {
            srcPixels = GetCompositePixels();
        }

        cv::Mat mat = ImageManager::PixelsToMat8UC3(srcPixels, m_Width, m_Height);

        // Find bounding box
        int minX = m_Width;
        int maxX = 0;
        int minY = m_Height;
        int maxY = 0;
        for (const auto& p : points) {
            minX = std::min(minX, p.first);
            maxX = std::max(maxX, p.first);
            minY = std::min(minY, p.second);
            maxY = std::max(maxY, p.second);
        }

        // Add 15px margin for GrabCut context
        const int margin = 15;
        minX = std::max(0, minX - margin);
        minY = std::max(0, minY - margin);
        maxX = std::min(m_Width - 1, maxX + margin);
        maxY = std::min(m_Height - 1, maxY + margin);

        int rectW = maxX - minX + 1;
        int rectH = maxY - minY + 1;

        if (rectW <= 2 || rectH <= 2) {
            m_SmartSelectInProgress.store(false);
            return;
        }

        cv::Rect roiRect(minX, minY, rectW, rectH);
        cv::Mat croppedMat = mat(roiRect).clone();
        cv::Mat croppedMask = cv::Mat::zeros(rectH, rectW, CV_8UC1); // Initialized to cv::GC_BGD

        // Shift points relative to ROI
        std::vector<cv::Point> shiftedPoints;
        for (const auto& p : points) {
            shiftedPoints.push_back(cv::Point(
                std::clamp(p.first - minX, 0, rectW - 1),
                std::clamp(p.second - minY, 0, rectH - 1)
            ));
        }

        // Fill probable foreground area (inside lasso)
        if (shiftedPoints.size() >= 3) {
            std::vector<std::vector<cv::Point>> polys = { shiftedPoints };
            cv::fillPoly(croppedMask, polys, cv::Scalar(cv::GC_PR_FGD));
        } else {
            // Draw a thick line if too few points
            for (size_t i = 0; i < shiftedPoints.size() - 1; ++i) {
                cv::line(croppedMask, shiftedPoints[i], shiftedPoints[i+1], cv::Scalar(cv::GC_PR_FGD), 5);
            }
        }

        cv::Mat bgdModel, fgdModel;
        try {
            if (!m_SmartSelectCancelled.load()) {
                cv::grabCut(croppedMat, croppedMask, cv::Rect(), bgdModel, fgdModel, 2, cv::GC_INIT_WITH_MASK);
            }
        } catch (...) {
            // Ignore errors
        }

        if (m_SmartSelectCancelled.load()) {
            m_SmartSelectInProgress.store(false);
            return;
        }

        // Map back to full selection mask
        cv::Mat temp = cv::Mat::zeros(m_Height, m_Width, CV_8UC1);
        for (int y = 0; y < rectH; ++y) {
            for (int x = 0; x < rectW; ++x) {
                uint8_t val = croppedMask.at<uint8_t>(y, x);
                if (val == cv::GC_PR_FGD || val == cv::GC_FGD) {
                    temp.at<uint8_t>(minY + y, minX + x) = 255;
                }
            }
        }

        cv::Mat current(m_Height, m_Width, CV_8UC1);
        if (!oldMask.empty()) {
            std::memcpy(current.data, oldMask.data(), oldMask.size());
        } else {
            current = cv::Mat::zeros(m_Height, m_Width, CV_8UC1);
        }
        cv::Mat combined;
        if (add) {
            cv::bitwise_or(current, temp, combined);
        } else if (subtract) {
            cv::bitwise_and(current, ~temp, combined);
        } else {
            combined = temp.clone();
        }

        m_SelectionMask.resize((size_t)m_Width * m_Height);
        std::memcpy(m_SelectionMask.data(), combined.data, m_SelectionMask.size());
        m_HasSelection = false;
        for (uint8_t val : m_SelectionMask) {
            if (val > 0) {
                m_HasSelection = true;
                break;
            }
        }
        m_SelectionMaskNeedsUpload = true;

        MarkSelectionMaskDirty();
        m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>("Smart Select", std::move(oldMask), oldHasSelection, m_SelectionMask, m_HasSelection));
        m_SmartSelectInProgress.store(false);
    });
    t.detach();
}
