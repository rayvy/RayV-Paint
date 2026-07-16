#pragma once
// Viewport transform snapshot for Python plugins (doc ↔ screen, selection HUD).
// Host publishes once per frame after the canvas image is laid out in ImGui.

namespace script {

struct ViewSnapshot {
    bool valid = false;
    float imageMinX = 0.f;
    float imageMinY = 0.f;
    float viewportW = 0.f;
    float viewportH = 0.f;
    float panX = 0.f;
    float panY = 0.f;
    float zoom = 1.f;
    float rotationRad = 0.f;
    bool flipH = false;
    bool flipV = false;
    int docW = 0;
    int docH = 0;
};

// Call from main thread after ImGui::Image for the canvas (same math as mouse map).
void PublishViewSnapshot(const ViewSnapshot& s);
void InvalidateViewSnapshot();

const ViewSnapshot& GetViewSnapshot();
bool IsViewValid();

// Document pixel → absolute ImGui/screen coordinates.
// Returns false if view invalid or document empty.
bool DocToScreen(float docX, float docY, float& outSx, float& outSy);

// Absolute screen → document pixel (float). Inverse of DocToScreen.
bool ScreenToDoc(float sx, float sy, float& outDocX, float& outDocY);

// Canvas image rect in screen space: (x, y, w, h).
bool ViewportScreenRect(float& outX, float& outY, float& outW, float& outH);

// Axis-aligned screen rect covering the selection AABB (maps 4 corners).
// Returns false if no selection / empty / view invalid.
bool SelectionScreenRect(float& outX, float& outY, float& outW, float& outH);

float ViewZoom();
float ViewPanX();
float ViewPanY();
float ViewRotationRad();

} // namespace script
