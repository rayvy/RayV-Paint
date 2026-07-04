#include "PaintEngine.h"
#include <cmath>
#include <algorithm>

void PaintEngine::DrawStamp(std::vector<float>& pixels, int width, int height, 
                          float cx, float cy, const BrushSettings& brush) {
    int startX = std::max(0, static_cast<int>(std::floor(cx - brush.radius)));
    int endX = std::min(width - 1, static_cast<int>(std::ceil(cx + brush.radius)));
    int startY = std::max(0, static_cast<int>(std::floor(cy - brush.radius)));
    int endY = std::min(height - 1, static_cast<int>(std::ceil(cy + brush.radius)));
    
    float r = brush.radius;
    float h = std::clamp(brush.hardness, 0.0f, 1.0f);
    float op = std::clamp(brush.opacity, 0.0f, 1.0f);
    
    for (int y = startY; y <= endY; ++y) {
        for (int x = startX; x <= endX; ++x) {
            float dx = x - cx;
            float dy = y - cy;
            float dist = std::sqrt(dx * dx + dy * dy);
            
            if (dist < r) {
                float intensity = 1.0f;
                if (h < 1.0f) {
                    float coreRadius = r * h;
                    if (dist > coreRadius) {
                        intensity = 1.0f - (dist - coreRadius) / (r - coreRadius);
                    }
                }
                
                float stampAlpha = brush.color[3] * op * intensity;
                if (stampAlpha <= 0.0f) continue;
                
                size_t idx = ((size_t)y * width + x) * 4;
                if (brush.erase) {
                    if (brush.writeR) pixels[idx + 0] *= (1.0f - stampAlpha);
                    if (brush.writeG) pixels[idx + 1] *= (1.0f - stampAlpha);
                    if (brush.writeB) pixels[idx + 2] *= (1.0f - stampAlpha);
                    if (brush.writeA) pixels[idx + 3] *= (1.0f - stampAlpha);
                } 
                else {
                    float destR = pixels[idx + 0];
                    float destG = pixels[idx + 1];
                    float destB = pixels[idx + 2];
                    float destA = pixels[idx + 3];
                    
                    float outA = stampAlpha + destA * (1.0f - stampAlpha);
                    if (outA > 0.0f) {
                        if (brush.writeR) pixels[idx + 0] = (brush.color[0] * stampAlpha + destR * destA * (1.0f - stampAlpha)) / outA;
                        if (brush.writeG) pixels[idx + 1] = (brush.color[1] * stampAlpha + destG * destA * (1.0f - stampAlpha)) / outA;
                        if (brush.writeB) pixels[idx + 2] = (brush.color[2] * stampAlpha + destB * destA * (1.0f - stampAlpha)) / outA;
                        if (brush.writeA) pixels[idx + 3] = outA;
                    }
                }
            }
        }
    }
}

void PaintEngine::DrawLine(std::vector<float>& pixels, int width, int height, 
                         float x0, float y0, float x1, float y1, const BrushSettings& brush) {
    float dx = x1 - x0;
    float dy = y1 - y0;
    float distance = std::sqrt(dx * dx + dy * dy);
    
    if (distance == 0.0f) {
        DrawStamp(pixels, width, height, x0, y0, brush);
        return;
    }
    
    // Draw stamps along the line. Use a step size of 10% of brush radius (minimum 1 pixel)
    float step = std::max(1.0f, brush.radius * 0.1f);
    int numSteps = static_cast<int>(std::ceil(distance / step));
    
    for (int i = 0; i <= numSteps; ++i) {
        float t = static_cast<float>(i) / numSteps;
        float cx = x0 + dx * t;
        float cy = y0 + dy * t;
        DrawStamp(pixels, width, height, cx, cy, brush);
    }
}

void PaintEngine::DrawStrokeSegment(std::vector<float>& pixels, int width, int height,
                                    float x0, float y0, float x1, float y1,
                                    const BrushSettings& brush, float& distanceAccumulator,
                                    float& lastDabX, float& lastDabY) {
    float dx = x1 - x0;
    float dy = y1 - y0;
    float segmentLength = std::sqrt(dx * dx + dy * dy);

    float spacingDistance = std::max(1.0f, brush.radius * 2.0f * brush.spacing);

    if (segmentLength == 0.0f) {
        return;
    }

    float dirX = dx / segmentLength;
    float dirY = dy / segmentLength;

    float traveled = 0.0f;
    while (traveled <= segmentLength) {
        float needed = spacingDistance - distanceAccumulator;
        
        if (traveled + needed <= segmentLength) {
            traveled += needed;
            float dabX = x0 + dirX * traveled;
            float dabY = y0 + dirY * traveled;
            DrawStamp(pixels, width, height, dabX, dabY, brush);
            lastDabX = dabX;
            lastDabY = dabY;
            distanceAccumulator = 0.0f;
        } else {
            float dxLast = x1 - lastDabX;
            float dyLast = y1 - lastDabY;
            distanceAccumulator = std::sqrt(dxLast * dxLast + dyLast * dyLast);
            break;
        }
    }
}
