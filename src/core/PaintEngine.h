#pragma once
#include <vector>
#include <cstdint>
#include <string>

// Optional grayscale tip stamp (row-major, tipSize x tipSize, 0=transparent 255=solid).
// When tipPixels is null/empty, procedural soft circle is used (legacy path).
struct BrushTip {
    int size = 0;                      // width=height
    std::vector<uint8_t> pixels;       // size*size
    float spacingMul = 1.0f;
    const char* name = "Custom";
};

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
    // When true: stamp blends RGB as straight lerp (opacity×hardness), not premultiplied
    // src-over. A is still written when writeA is set (coverage / strength fade).
    // Used when Channels→Alpha is OFF, or layer Alpha Rewrite is OFF.
    bool rgbMorphOnly = false;

    bool pressureRadius   = false;
    bool pressureHardness = false;
    bool pressureOpacity  = false;

    // --- Stored in presets; paint engine may not implement yet ---
    float rotationDeg = 0.f;       // brush tip rotation (placeholder for engine)
    bool  pressureRotation = false;
    float scatter = 0.f;           // 0..1 positional scatter (placeholder)
    float angleJitter = 0.f;       // 0..1 form/angle dynamics (placeholder)
    std::string tipSourcePath;     // display path of custom texture if known

    // Optional custom tip (null = procedural circle, no regression)
    const BrushTip* tip = nullptr;
};

// Built-in brush tip presets (generated procedurally once).
namespace BrushPresets {
    const BrushTip& SoftRound();
    const BrushTip& HardRound();
    const BrushTip& Pencil();
    const BrushTip& Airbrush();
}

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
