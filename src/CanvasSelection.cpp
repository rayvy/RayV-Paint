#include "Canvas.h"
#include "core/Logger.h"
#include "core/ImageManager.h"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <thread>

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

    std::vector<uint8_t> oldMask = m_SelectionMask;
    bool oldHasSelection = m_HasSelection;

    std::vector<float> srcPixels;
    if (m_ActiveLayerIdx >= 0 && m_ActiveLayerIdx < (int)m_Layers.size()) {
        srcPixels = ExportLayerF(m_Layers[m_ActiveLayerIdx], m_Width, m_Height);
    } else {
        srcPixels = GetCompositePixels();
    }

    cv::Mat mat = ImageManager::PixelsToMat8UC3(srcPixels, m_Width, m_Height);
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
                cv::Vec3b color = mat.at<cv::Vec3b>(y, x);
                float diff = std::sqrt(
                    std::pow(static_cast<float>(color[0]) - seedColor[0], 2) +
                    std::pow(static_cast<float>(color[1]) - seedColor[1], 2) +
                    std::pow(static_cast<float>(color[2]) - seedColor[2], 2)
                ) / 255.0f;
                temp.at<uint8_t>(y, x) = (diff <= tolerance * std::sqrt(3.0f)) ? 255 : 0;
            }
        }
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

    MarkSelectionMaskDirty();

    m_UndoRedoManager.PushCommand(std::make_shared<SelectionCommand>("Select", std::move(oldMask), oldHasSelection, m_SelectionMask, m_HasSelection));
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
