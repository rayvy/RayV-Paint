#pragma once
#include <vector>

struct BrushSettings {
    float color[4] = { 1.0f, 0.0f, 0.0f, 1.0f }; // Red default
    float radius = 10.0f;
    float hardness = 0.5f; // 0 (soft) to 1 (hard)
    float opacity = 1.0f;
    bool erase = false;
    float spacing = 0.1f; // 10% spacing by default
    int stabilization = 1; // 1 to 50 smoothing strength

    // Channel Write Mask (RGBA)
    bool writeR = true;
    bool writeG = true;
    bool writeB = true;
    bool writeA = true;
};

class PaintEngine {
public:
    static void DrawStamp(std::vector<float>& pixels, int width, int height, 
                          float cx, float cy, const BrushSettings& brush);

    static void DrawLine(std::vector<float>& pixels, int width, int height, 
                         float x0, float y0, float x1, float y1, const BrushSettings& brush);

    static void DrawStrokeSegment(std::vector<float>& pixels, int width, int height,
                                  float x0, float y0, float x1, float y1,
                                  const BrushSettings& brush, float& distanceAccumulator,
                                  float& lastDabX, float& lastDabY);
};
