#pragma once
#include <imgui.h>

#include <directxmath.h>
#include <vector>
#include <string>
#include <atomic>
#include <functional>
#include "core/TileCache.h"
#include "core/PaintEngine.h"
#include "core/DdsHelper.h"
#include "core/UndoRedoManager.h"

// ---- Blend Modes ----
enum class BlendMode : uint8_t {
    Normal = 0,
    Multiply,
    Screen,
    Overlay,
    Add,
    Subtract,
    Darken,
    Lighten,
    HardLight,
    SoftLight
};

// ---- Non-destructive Layer Filters ----
enum class FilterType : uint8_t {
    Blur = 0,
    HSV,
    Curves,
    AlphaInvert,
    Noise
};

struct LayerFilter {
    FilterType type = FilterType::Blur;
    bool enabled    = true;
    float p[4]      = {};  // generic params: blur=p[0] radius; hsv=p[0..2] H/S/V; noise=p[0] strength,p[1] colorNoise
    std::vector<float> lut; // 256 floats [0..1] for Curves (same LUT applied to R,G,B)
};

struct Layer {
    std::string name;
    bool visible = true;
    float opacity = 1.0f;
    BlendMode blendMode = BlendMode::Normal;

    // Primary pixel data (replaces std::vector<float> pixels).
    // nullptr for group header layers (isGroup == true).
    std::unique_ptr<TileCache> tileCache;

    bool needsUpload = false; // set true when renderer must upload changed layer data

    // Non-destructive filters
    std::vector<LayerFilter> filters;
    // Cache: tileCache with filters applied. Rebuilt when filtersDirty=true.
    std::unique_ptr<TileCache> filteredCache;
    bool filtersDirty = true;

    // Mask: single-channel, same canvas dimensions.
    // Values 0-255 (RGBA8 docs) or float reinterpreted as uint8 for GPU (R8_UNORM).
    std::vector<uint8_t> mask;
    bool hasMask = false;
    bool maskNeedsUpload = false;
    std::vector<bool> maskDirtyTiles;  // [tileY * tilesX + tileX] = dirty

    // Group support
    bool isGroup        = false; // group header — no pixel data
    int  parentGroupId  = -1;    // -1 = top-level
    bool groupExpanded  = true;
};


enum class StrokePhase {
    Begin,
    Update,
    End
};

class Canvas {
public:
    Canvas();
    ~Canvas();

    bool Initialize();
    void Shutdown();

    // Updates pan/zoom parameters based on UI inputs
    void Update(float viewportWidth, float viewportHeight, bool isMouseOverCanvas, 
                float mouseX, float mouseY, bool isDragging, float dragDx, float dragDy, float wheelDelta);
    
    void ResetView();

    float GetZoom() const { return m_Zoom; }
    void SetZoom(float zoom) { m_Zoom = zoom; }

    DirectX::XMFLOAT2 GetPan() const { return m_Pan; }
    void SetPan(DirectX::XMFLOAT2 pan) { m_Pan = pan; }

    int GetWidth() const { return m_Width; }
    int GetHeight() const { return m_Height; }

    // Resizes canvas and all layers (retaining existing pixel data where possible)
    void ResizeCanvas(int width, int height);

    // Layer Management
    void CreateNewLayer(const std::string& name);
    void DeleteLayer(int index);
    void SetActiveLayerIndex(int idx);
    void ToggleLayerIsolation(int layerIdx);
    void MarkCompositeDirty() { m_CompositeDirty = true; }
    bool IsLayerIsolated(int layerIdx) const { return m_IsIsolatedMode && m_IsolatedLayerIdx == layerIdx; }
    bool IsInIsolationMode() const { return m_IsIsolatedMode; }
    int GetActiveLayerIndex() const { return m_ActiveLayerIdx; }
    std::vector<Layer>& GetLayers() { return m_Layers; }

    // Layer Mask operations
    void CreateLayerMask(int index);
    void CreateLayerMaskFromSelection(int index);
    void DeleteLayerMask(int index);
    void ApplyLayerMask(int index);
    void MarkLayerMaskDirty(int index);

    // Paint Operation
    void PaintOnActiveLayer(float currRawX, float currRawY, StrokePhase phase, const BrushSettings& brush);
    bool IsStrokeActive() const { return m_IsStrokeActive; }
    void SmudgeOnActiveLayer(float x, float y, StrokePhase phase, const SmudgeSettings& s);

    // Destructive image adjustments (operate on active layer, respect selection mask)
    void SelectAll();
    void InvertSelection();
    void InvertAlpha();
    void ApplyBlur(float radius);
    void ApplyHSV(float dH, float dS, float dV);
    void ApplyCurves(const std::vector<float>& lut256); // 256-element LUT [0..1] applied to R,G,B
    void ApplyNoise(float strength, bool colorNoise);

    // Layer group management
    void CreateLayerGroup(const std::string& name);
    void AddLayerToGroup(int layerIdx, int groupLayerIdx);
    void RemoveLayerFromGroup(int layerIdx);

    // Selection System
    bool HasSelection() const { return m_HasSelection; }
    void ClearSelection();
    void SetSelectionMask(const std::vector<uint8_t>& mask);
    const std::vector<uint8_t>& GetSelectionMask() const { return m_SelectionMask; }
    void MarkSelectionMaskDirty();
    void ApplyRectSelection(int x1, int y1, int x2, int y2, bool add, bool subtract);
    void ApplyEllipseSelection(int x1, int y1, int x2, int y2, bool add, bool subtract);
    void ApplyLassoSelection(const std::vector<std::pair<int, int>>& points, bool add, bool subtract);
    void ApplyMagicWandSelection(int startX, int startY, float tolerance, bool add, bool subtract, bool contiguous);
    void ApplySmartSelectSelection(const std::vector<std::pair<int, int>>& points, bool add, bool subtract);
    bool IsSmartSelectInProgress() const { return m_SmartSelectInProgress.load(); }
    void CancelSmartSelect() { m_SmartSelectCancelled.store(true); }
    void ApplyBucketFill(int startX, int startY, float tolerance, const float color[4], bool contiguous);
    void ApplyGradient(int x1, int y1, int x2, int y2, const float startColor[4], const float endColor[4]);

    // Move Pixels operations
    bool IsMovingPixels() const { return m_IsMovingPixels; }
    void StartMovePixels();
    void UpdateMovePixels(int dx, int dy);
    void CommitMovePixels();
    void CancelMovePixels();
    void DrawMoveGizmo(ImDrawList* dl, const std::function<ImVec2(float, float)>& canvasToScreen);
    float GetFloatingScaleX() const { return m_FloatingScaleX; }
    float GetFloatingScaleY() const { return m_FloatingScaleY; }
    float GetFloatingRotation() const { return m_FloatingRotation; }
    void SetFloatingScaleX(float sx) { m_FloatingScaleX = sx; }
    void SetFloatingScaleY(float sy) { m_FloatingScaleY = sy; }
    void SetFloatingRotation(float rot) { m_FloatingRotation = rot; }

    // File Import / Export
    bool LoadImageToLayer(const std::string& filepath);
    bool SaveCanvas(const std::string& filepath, DdsFormat ddsFormat);
    bool SaveCanvasStandard(const std::string& filepath, const std::string& iccProfilePath = "");
    bool SaveCanvasCompressed(const std::string& filepath, const std::string& formatStr, bool generateMips, const std::string& mipFilter, const std::string& speed);
    std::vector<float> GetCompositePixels() const;
    void CreateLayerFromPixels(const std::string& name, const std::vector<float>& pixels, int width, int height);

    // Pixel Transformations
    void FlipActiveLayerHorizontal();
    void FlipActiveLayerVertical();
    void RotateCanvas90(bool clockwise);
    void FlipCanvasHorizontal();
    void FlipCanvasVertical();
    void CommitTransformation(const std::string& actionName);



    bool GetChannelR() const { return m_ChannelR; }
    void SetChannelR(bool v) { m_ChannelR = v; }
    bool GetChannelG() const { return m_ChannelG; }
    void SetChannelG(bool v) { m_ChannelG = v; }
    bool GetChannelB() const { return m_ChannelB; }
    void SetChannelB(bool v) { m_ChannelB = v; }
    bool GetChannelA() const { return m_ChannelA; }
    void SetChannelA(bool v) { m_ChannelA = v; }

    bool Undo();
    bool Redo();
    bool CanUndo() const;
    bool CanRedo() const;
    std::string GetUndoName() const;
    std::string GetRedoName() const;
    void ClearUndoHistory();
    bool IsDocumentModified() const { return m_IsDocumentModified; }
    void SetDocumentModified(bool modified) { m_IsDocumentModified = modified; }
    std::string GetCurrentProjectFilePath() const { return m_CurrentProjectFilePath; }
    void SetCurrentProjectFilePath(const std::string& path) { m_CurrentProjectFilePath = path; }

    std::string GetExportPath() const { return m_ExportPath; }
    void SetExportPath(const std::string& path) { m_ExportPath = path; }
    std::string GetExportFormat() const { return m_ExportFormat; }
    void SetExportFormat(const std::string& format) { m_ExportFormat = format; }
    bool GetExportAdvancedMode() const { return m_ExportAdvancedMode; }
    void SetExportAdvancedMode(bool v) { m_ExportAdvancedMode = v; }
    std::string GetExportCompressionSpeed() const { return m_ExportCompressionSpeed; }
    void SetExportCompressionSpeed(const std::string& speed) { m_ExportCompressionSpeed = speed; }
    bool GetExportGenerateMipMaps() const { return m_ExportGenerateMipMaps; }
    void SetExportGenerateMipMaps(bool v) { m_ExportGenerateMipMaps = v; }
    std::string GetExportMipFilter() const { return m_ExportMipFilter; }
    void SetExportMipFilter(const std::string& filter) { m_ExportMipFilter = filter; }
    std::string GetExportPngColorSpace() const { return m_ExportPngColorSpace; }
    void SetExportPngColorSpace(const std::string& cs) { m_ExportPngColorSpace = cs; }

    enum class ProjectType {
        Simple,
        Advanced
    };

    float GetRotationAngle() const { return m_RotationAngle; }
    void SetRotationAngle(float angle) { m_RotationAngle = angle; }
    bool GetMirrorHorizontal() const { return m_MirrorHorizontal; }
    void SetMirrorHorizontal(bool v) { m_MirrorHorizontal = v; }
    bool GetMirrorVertical() const { return m_MirrorVertical; }
    void SetMirrorVertical(bool v) { m_MirrorVertical = v; }

    bool GetViewportFlipH() const { return m_ViewportFlipH; }
    void SetViewportFlipH(bool v) { m_ViewportFlipH = v; }
    bool GetViewportFlipV() const { return m_ViewportFlipV; }
    void SetViewportFlipV(bool v) { m_ViewportFlipV = v; }

    ProjectType GetProjectType() const { return m_ProjectType; }
    void SetProjectType(ProjectType type) { m_ProjectType = type; }
    bool SaveProjectAuto();

    // Native RAYP Format
    bool SaveCanvasRayp(const std::string& filepath);
    void SaveCanvasRaypAsync(const std::string& filepath, std::function<void(bool)> callback = nullptr);
    bool LoadCanvasRayp(const std::string& filepath);

    std::vector<float> GetComposedPixels();

    // Move Pixels Accessors
    const TileCache* GetFloatingTileCache() const { return m_FloatingTileCache.get(); }
    TileCache* GetFloatingTileCache() { return m_FloatingTileCache.get(); }
    int GetFloatingOffsetX() const { return m_FloatingOffsetX; }
    int GetFloatingOffsetY() const { return m_FloatingOffsetY; }
    int GetStartActiveLayerIdx() const { return m_StartActiveLayerIdx; }
    const std::vector<uint8_t>& GetOriginalSelectionMask() const { return m_OriginalSelectionMask; }

private:
    void BackupTile(int tileX, int tileY);

    void MarkCompositeResourcesDirty();

    int m_Width;
    int m_Height;
    float m_Zoom;
    DirectX::XMFLOAT2 m_Pan;

    // Document pixel format — set at canvas creation or auto-detected on file open.
    CanvasPixelFormat m_CanvasFormat = CanvasPixelFormat::RGBA8;
    bool m_ChannelR = true;
    bool m_ChannelG = true;
    bool m_ChannelB = true;
    bool m_ChannelA = true;

    std::vector<Layer> m_Layers;
    int m_ActiveLayerIdx = -1;



    // Layer Isolation State
    bool m_IsIsolatedMode = false;
    int m_IsolatedLayerIdx = -1;
    std::vector<bool> m_PreIsolationVisibility;

    int m_CompositeWidth = 0;
    int m_CompositeHeight = 0;
    bool m_CompositeDirty = true;

    // Applies filters to layer.pixels → layer.filteredPixels (rebuilds if filtersDirty)
    void RebuildFilteredPixels(Layer& layer);


    bool ExtractAndSetICCProfile(const std::string& pngPath);

    // Stroke state tracking
    bool m_IsStrokeActive = false;
    float m_LastDabX = 0.0f;
    float m_LastDabY = 0.0f;
    float m_StrokeDistanceAccumulator = 0.0f;
    float m_PrevStabilizedX = 0.0f;
    float m_PrevStabilizedY = 0.0f;

    // Smudge state
    float m_SmudgePickup[4] = { 0.f, 0.f, 0.f, 0.f };
    bool  m_SmudgePickupValid = false;
    float m_SmudgeLastX = 0.f;
    float m_SmudgeLastY = 0.f;
    float m_SmudgeDistAcc = 0.f;

    // Undo/Redo Engine
    UndoRedoManager m_UndoRedoManager;
    std::unordered_map<int, TileDelta> m_ActiveStrokeDeltas;
    bool m_IsDocumentModified = false;
    std::string m_CurrentProjectFilePath;

    std::string m_ExportPath = "";
    std::string m_ExportFormat = "BC7_UNORM_SRGB";
    bool m_ExportAdvancedMode = false;
    std::string m_ExportCompressionSpeed = "Medium";
    bool m_ExportGenerateMipMaps = true;
    std::string m_ExportMipFilter = "Bicubic";
    std::string m_ExportPngColorSpace = "sRGB";

    float m_RotationAngle = 0.0f;
    bool m_MirrorHorizontal = false;
    bool m_MirrorVertical = false;
    bool m_ViewportFlipH = false;
    bool m_ViewportFlipV = false;
    ProjectType m_ProjectType = ProjectType::Advanced;

    // Selection State
    bool m_HasSelection = false;
    std::vector<uint8_t> m_SelectionMask; // 0=not selected, 255=fully selected
    float m_SelectionOutlineTime = 0.0f;
    bool m_SelectionMaskNeedsUpload = false;
    std::atomic<bool> m_SmartSelectInProgress{false};
    std::atomic<bool> m_SmartSelectCancelled{false};

    // Move Pixels State
    bool m_IsMovingPixels = false;
    std::unique_ptr<TileCache> m_FloatingTileCache;
    std::vector<uint8_t> m_OriginalSelectionMask;
    int m_FloatingOffsetX = 0;
    int m_FloatingOffsetY = 0;
    int m_StartActiveLayerIdx = -1;
    float m_FloatingScaleX = 1.0f;
    float m_FloatingScaleY = 1.0f;
    float m_FloatingRotation = 0.0f; // in radians

};
