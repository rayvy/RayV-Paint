#pragma once
#include <vector>
#include <cstdint>

struct BrushSettings {
    float color[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    float radius    = 10.0f;
    float hardness  = 0.5f;
    float opacity   = 1.0f;
    bool  erase     = false;
    float spacing   = 0.1f;
    int   stabilization = 1;

    bool writeR = true;
    bool writeG = true;
    bool writeB = true;
    bool writeA = false;

    bool pressureRadius   = false;
    bool pressureHardness = false;
    bool pressureOpacity  = false;
};

struct SmudgeSettings {
    float radius   = 20.0f;
    float strength = 0.6f;
    float spacing  = 0.12f;
};

class TileCache;

class PaintEngine {
public:
    // selectionMask: per-pixel uint8 (0=not selected, 255=fully selected).
    // Empty vector = no selection (full canvas active).
    static void DrawStamp(TileCache& cache,
                          float cx, float cy, const BrushSettings& brush,
                          bool mirrorH = false, bool mirrorV = false,
                          const std::vector<uint8_t>& selectionMask = {});

    static void DrawLine(TileCache& cache,
                         float x0, float y0, float x1, float y1,
                         const BrushSettings& brush,
                         bool mirrorH = false, bool mirrorV = false,
                         const std::vector<uint8_t>& selectionMask = {});

    static void DrawStrokeSegment(TileCache& cache,
                                  float x0, float y0, float x1, float y1,
                                  const BrushSettings& brush,
                                  float& distanceAccumulator,
                                  float& lastDabX, float& lastDabY,
                                  bool mirrorH = false, bool mirrorV = false,
                                  const std::vector<uint8_t>& selectionMask = {});
};
