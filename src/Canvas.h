#pragma once
#include <imgui.h>

#include <d3d11.h>
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

// What the brush/smudge tools write into for the active layer (Photoshop-like).
enum class PaintTarget : uint8_t {
    LayerContent = 0, // paint RGB(A) into tileCache
    LayerMask    = 1  // paint grayscale into layer.mask (white=reveal, black=hide)
};

// Optional progress for long I/O (open image / .rayp). UI shows a bar; core stays free of ImGui.
// progress01 is in [0,1]; stage is a short stable tag e.g. "decode", "layers", "upload".
using LoadProgressFn = std::function<void(float progress01, const char* stage)>;

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

    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    bool needsUpload = false; // set true to force full re-upload (fallback)

    // Non-destructive filters
    std::vector<LayerFilter> filters;
    // Cache: tileCache with filters applied. Rebuilt when filtersDirty=true.
    std::unique_ptr<TileCache> filteredCache;
    bool filtersDirty = true;

    // Mask: single-channel, same canvas dimensions.
    // Values 0-255 (RGBA8 docs) or float reinterpreted as uint8 for GPU (R8_UNORM).
    std::vector<uint8_t> mask;
    ID3D11Texture2D* maskTexture = nullptr;
    ID3D11ShaderResourceView* maskSRV = nullptr;
    bool hasMask = false;
    bool maskNeedsUpload = false;

    // Group support
    bool isGroup        = false; // group header — no pixel data
    int  parentGroupId  = -1;    // -1 = top-level
    bool groupExpanded  = true;

    // Layer type (Raster default; Group when isGroup; SmartObject/VectorSvg for vectors)
    enum class Type : uint8_t { Raster = 0, Group = 1, SmartObject = 2, VectorSvg = 3 };
    Type type = Type::Raster;
    // Source bytes for smart objects / SVG (empty for pure raster).
    std::vector<uint8_t> smartSourceBytes;
    std::string smartSourcePath;
    float smartScale = 1.0f;
};


enum class StrokePhase {
    Begin,
    Update,
    End
};

class DocumentGeometryCommand;

class Canvas {
public:
    Canvas();
    ~Canvas();
    friend class DocumentGeometryCommand;

    bool Initialize(ID3D11Device* device);
    void Shutdown();

    // Updates pan/zoom parameters based on UI inputs
    void Update(float viewportWidth, float viewportHeight, bool isMouseOverCanvas, 
                float mouseX, float mouseY, bool isDragging, float dragDx, float dragDy, float wheelDelta);
    
    // Renders the canvas (checkerboard background + blended layers)
    void Render(ID3D11DeviceContext* context, float viewportWidth, float viewportHeight);

    void ResetView();

    float GetZoom() const { return m_Zoom; }
    void SetZoom(float zoom) { m_Zoom = zoom; }

    DirectX::XMFLOAT2 GetPan() const { return m_Pan; }
    void SetPan(DirectX::XMFLOAT2 pan) { m_Pan = pan; }

    int GetWidth() const { return m_Width; }
    int GetHeight() const { return m_Height; }

    // Resizes canvas and all layers (retaining existing pixel data where possible)
    // Extend mode: pad/crop content (current behaviour). No pixel scaling.
    void ResizeCanvas(ID3D11Device* device, int width, int height);

    enum class CanvasEditMode : uint8_t { Extend = 0, Resize = 1 };
    enum class ResampleFilter : uint8_t { Nearest = 0, Bilinear = 1, Lanczos = 2 };
    // Canvas Edit: Extend (content unscaled) or Resize (scale content with filter).
    // anchorX/Y in [0,1] for Extend placement of old content (0.5,0.5 = center).
    bool EditCanvas(ID3D11Device* device, CanvasEditMode mode, int newW, int newH,
                    ResampleFilter filter = ResampleFilter::Bilinear,
                    float anchorX = 0.5f, float anchorY = 0.5f);
    // Crop document to selection AABB (or given rect). Registers document undo.
    bool CropCanvasToSelection(ID3D11Device* device);
    bool CropCanvasToRect(ID3D11Device* device, int x, int y, int w, int h);

    enum class IccPreset : uint8_t { None = 0, sRGB = 1, DisplayP3 = 2, AdobeRGB = 3 };
    static const char* IccPresetName(IccPreset p);
    void SetExportIccPreset(IccPreset p);
    IccPreset GetExportIccPreset() const { return m_ExportIccPreset; }

    // Layer types / smart objects (Layer::Type)
    bool ImportSvgAsSmartObject(ID3D11Device* device, const std::string& filepath);
    bool RasterizeLayer(ID3D11Device* device, int layerIdx);

    // Layer Management
    void CreateNewLayer(ID3D11Device* device, const std::string& name);
    void DeleteLayer(int index);
    void SetActiveLayerIndex(int idx);
    void ToggleLayerIsolation(int layerIdx);
    void MarkCompositeDirty() { m_CompositeDirty = true; }
    bool IsLayerIsolated(int layerIdx) const { return m_IsIsolatedMode && m_IsolatedLayerIdx == layerIdx; }
    bool IsInIsolationMode() const { return m_IsIsolatedMode; }
    int GetActiveLayerIndex() const { return m_ActiveLayerIdx; }
    std::vector<Layer>& GetLayers() { return m_Layers; }
    size_t GetActiveLayerTileCount() const {
        if (m_ActiveLayerIdx < 0 || m_ActiveLayerIdx >= (int)m_Layers.size()) return 0;
        const auto& layer = m_Layers[m_ActiveLayerIdx];
        return (layer.tileCache) ? layer.tileCache->GetTileCount() : 0;
    }

    // Layer Mask operations (Photoshop-like: paint white=show, black=hide)
    void CreateLayerMask(ID3D11Device* device, int index);
    void CreateLayerMaskFromSelection(ID3D11Device* device, int index);
    void DeleteLayerMask(int index);
    void ApplyLayerMask(int index);
    void UpdateLayerMaskTexture(ID3D11Device* device, int index);
    bool ActiveLayerHasMask() const;
    // Switch brush target: content vs mask. Creating a mask auto-selects mask target.
    void SetPaintTarget(PaintTarget t);
    PaintTarget GetPaintTarget() const { return m_PaintTarget; }
    // True when mask is selected for painting (UI can show mask as grayscale preview).
    bool IsEditingLayerMask() const { return m_PaintTarget == PaintTarget::LayerMask; }

    // Paint Operation
    void PaintOnActiveLayer(float currRawX, float currRawY, StrokePhase phase, const BrushSettings& brush);
    bool IsStrokeActive() const { return m_IsStrokeActive; }
    void SmudgeOnActiveLayer(float x, float y, StrokePhase phase, const SmudgeSettings& s);

    // Destructive image adjustments (operate on active layer, respect selection mask)
    void SelectAll();                       // full canvas selection (with undo)
    void SelectOpaquePixels(int layerIdx = -1); // alpha-as-mask; -1 = active layer
    void InvertSelection();
    void InvertAlpha();
    void ApplyBlur(float radius);
    void ApplyHSV(float dH, float dS, float dV);
    // lutRGB: 256 floats [0..1]; lutAlpha optional 256 floats (empty = leave A unchanged)
    void ApplyCurves(const std::vector<float>& lutRGB, const std::vector<float>& lutAlpha = {});
    void ApplyNoise(float strength, bool colorNoise);

    // Layer group management
    void CreateLayerGroup(ID3D11Device* device, const std::string& name);
    void AddLayerToGroup(int layerIdx, int groupLayerIdx);
    void RemoveLayerFromGroup(int layerIdx);
    // Reorder + remaps parentGroupId. Returns new index of moved layer.
    int  ReorderLayer(int fromIdx, int toIdx);
    // Move layer into group (reparent + place under header). Returns new layer index.
    int  MoveLayerIntoGroup(int layerIdx, int groupIdx);

    // Selection System
    bool HasSelection() const { return m_HasSelection; }
    void ClearSelection();
    void SetSelectionMask(const std::vector<uint8_t>& mask);
    const std::vector<uint8_t>& GetSelectionMask() const { return m_SelectionMask; }
    void UpdateSelectionMaskTexture(ID3D11Device* device);
    void ApplyRectSelection(int x1, int y1, int x2, int y2, bool add, bool subtract);
    void ApplyEllipseSelection(int x1, int y1, int x2, int y2, bool add, bool subtract);
    void ApplyLassoSelection(const std::vector<std::pair<int, int>>& points, bool add, bool subtract);
    // Polygonal (straight) lasso — same fill as freehand; points are polygon vertices.
    void ApplyPolygonalLassoSelection(const std::vector<std::pair<int, int>>& points, bool add, bool subtract);
    void ApplyMagicWandSelection(ID3D11Device* device, int startX, int startY, float tolerance, bool add, bool subtract, bool contiguous);
    // Sticky wand seed (Photoshop-like): click sets seed; tolerance scrub re-previews without new click.
    bool HasWandSeed() const { return m_WandSeedValid; }
    void ClearWandSeed() { m_WandSeedValid = false; }
    void GetWandSeed(int& outX, int& outY) const { outX = m_WandSeedX; outY = m_WandSeedY; }
    // Re-run wand from last seed with new tolerance (no undo until Commit). Returns false if no seed.
    bool PreviewWandFromSeed(ID3D11Device* device, float tolerance, bool add, bool subtract, bool contiguous);
    void ApplySmartSelectSelection(ID3D11Device* device, const std::vector<std::pair<int, int>>& points, bool add, bool subtract);
    // Quick Selection (non-AI): brush seeds → region grow with edge stop.
    void BeginQuickSelectStroke();
    void StrokeQuickSelect(const std::vector<std::pair<int, int>>& points, float radius, bool subtract);
    void EndQuickSelectStroke(ID3D11Device* device, bool add, bool subtract);
    bool IsSmartSelectInProgress() const { return m_SmartSelectInProgress.load(); }
    void CancelSmartSelect() { m_SmartSelectCancelled.store(true); }
    void ApplyBucketFill(int startX, int startY, float tolerance, const float color[4], bool contiguous);
    void ApplyGradient(int x1, int y1, int x2, int y2, const float startColor[4], const float endColor[4]);

    // Move Pixels operations
    bool IsMovingPixels() const { return m_IsMovingPixels; }
    void StartMovePixels(ID3D11Device* device);
    void UpdateMovePixels(ID3D11Device* device, int dx, int dy);
    void CommitMovePixels(ID3D11Device* device);
    void CancelMovePixels(ID3D11Device* device);
    void DrawMoveGizmo(ImDrawList* dl, const std::function<ImVec2(float, float)>& canvasToScreen);
    float GetFloatingScaleX() const { return m_FloatingScaleX; }
    float GetFloatingScaleY() const { return m_FloatingScaleY; }
    float GetFloatingRotation() const { return m_FloatingRotation; }
    int GetFloatingOffsetX() const { return m_FloatingOffsetX; }
    int GetFloatingOffsetY() const { return m_FloatingOffsetY; }
    // Selection AABB of floating transform content (canvas space, before offset/scale/rot).
    bool GetFloatingBBox(int& outX, int& outY, int& outW, int& outH) const {
        if (!m_IsMovingPixels || m_FloatingBBoxW <= 0 || m_FloatingBBoxH <= 0) return false;
        outX = m_FloatingBBoxX; outY = m_FloatingBBoxY;
        outW = m_FloatingBBoxW; outH = m_FloatingBBoxH;
        return true;
    }
    void SetFloatingScaleX(float sx) { m_FloatingScaleX = sx; }
    void SetFloatingScaleY(float sy) { m_FloatingScaleY = sy; }
    void SetFloatingRotation(float rot) { m_FloatingRotation = rot; }

    // Channel solo controls
    bool GetChannelR() const { return m_ChannelR; }
    bool GetChannelG() const { return m_ChannelG; }
    bool GetChannelB() const { return m_ChannelB; }
    bool GetChannelA() const { return m_ChannelA; }
    void SetChannelR(bool r) { m_ChannelR = r; }
    void SetChannelG(bool g) { m_ChannelG = g; }
    void SetChannelB(bool b) { m_ChannelB = b; }
    void SetChannelA(bool a) { m_ChannelA = a; }

    ID3D11ShaderResourceView* GetCompositeSRV() const { return m_CompositeSRV; }

    // File Import / Export
    // Routes by extension: .rayp → LoadCanvasRayp, else LoadImageToLayer.
    // progress: optional; called on main/worker thread — UI should marshal to UI thread if needed.
    bool OpenDocument(ID3D11Device* device, const std::string& filepath, LoadProgressFn progress = nullptr);
    bool LoadImageToLayer(ID3D11Device* device, const std::string& filepath, LoadProgressFn progress = nullptr);
    bool SaveCanvas(const std::string& filepath, DdsFormat ddsFormat);
    // iccProfilePath: legacy path or empty. Prefer SetExportIccPreset + SaveCanvasStandard(path).
    bool SaveCanvasStandard(const std::string& filepath, const std::string& iccProfilePath = "");
    bool SaveCanvasStandard(const std::string& filepath, IccPreset preset);
    bool SaveCanvasCompressed(const std::string& filepath, const std::string& formatStr, bool generateMips, const std::string& mipFilter, const std::string& speed);
    std::vector<float> GetCompositePixels() const;
    void SampleCompositePixel(int x, int y, float outColor[4]) const;
    void CreateLayerFromPixels(ID3D11Device* device, const std::string& name, const std::vector<float>& pixels, int width, int height);

    // Pixel Transformations
    void FlipActiveLayerHorizontal(ID3D11Device* device);
    void FlipActiveLayerVertical(ID3D11Device* device);
    void RotateCanvas90(ID3D11Device* device, bool clockwise);
    void FlipCanvasHorizontal(ID3D11Device* device);
    void FlipCanvasVertical(ID3D11Device* device);
    void CommitTransformation(const std::string& actionName);

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
    bool LoadCanvasRayp(const std::string& filepath, ID3D11Device* device, LoadProgressFn progress = nullptr);

    std::vector<float> GetComposedPixels();

private:
    void BackupTile(int tileX, int tileY);
    // Snapshot every tile in the active layer grid (empty tiles get empty oldState).
    // Required before full-layer Import so undo covers all tiles (fixes partial-tile undo).
    void BackupAllActiveLayerTiles();
    // Snapshot newState for all backed-up tiles and push PaintStrokeCommand.
    void CommitActiveLayerMutation(const std::string& actionName);
    // Restore oldState from m_ActiveStrokeDeltas without pushing (cancel transform).
    void RestoreActiveLayerMutation();
    void RunMagicWand(ID3D11Device* device, int startX, int startY, float tolerance,
                      bool add, bool subtract, bool contiguous, bool pushUndo);
    bool GetSelectionBounds(int& outX, int& outY, int& outW, int& outH) const;
    void EnsureWandSourceCache();
    void InvalidateWandSourceCache();

    struct Vertex {
        DirectX::XMFLOAT2 pos;
        DirectX::XMFLOAT2 uv;
    };

    struct CanvasBuffer {
        DirectX::XMFLOAT4 viewportSizeAndZoom;
        DirectX::XMFLOAT4 offsetAndCanvasSize;
        DirectX::XMFLOAT4 channelMasks;
        DirectX::XMFLOAT4 viewportFlags; // x: flipH, y: flipV, zw: unused
    };

    struct LayerBuffer {
        DirectX::XMFLOAT4 layerParams;     // x: opacity, y: hasMask, zw: translation (uOff, vOff)
        DirectX::XMFLOAT4 transformParams; // x: scaleX, y: scaleY, z: rotation, w: isFloating
        DirectX::XMFLOAT4 centerParams;    // x: centerX, y: centerY, z: blendMode (as float uint), w: unused
    };


    void ComposeLayers(ID3D11DeviceContext* context);
    void CreateCompositeResources(ID3D11Device* device);
    void ReleaseCompositeResources();
    void RecreateLayerTexture(ID3D11Device* device, Layer& layer);

    int m_Width;
    int m_Height;
    float m_Zoom;
    DirectX::XMFLOAT2 m_Pan;

    // Document pixel format — set at canvas creation or auto-detected on file open.
    CanvasPixelFormat m_CanvasFormat = CanvasPixelFormat::RGBA8;
    // Corresponding DXGI format used for layer textures and composite target.
    DXGI_FORMAT GetLayerDxgiFormat() const;
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

    // Direct3D 11 Resources
    ID3D11Buffer* m_VertexBuffer = nullptr;
    ID3D11Buffer* m_IndexBuffer = nullptr;
    ID3D11Buffer* m_ConstantBuffer = nullptr;
    ID3D11Buffer* m_LayerConstantBuffer = nullptr;

    ID3D11VertexShader* m_VertexShader = nullptr;
    ID3D11VertexShader* m_LayerVertexShader = nullptr;
    ID3D11PixelShader* m_PixelShader = nullptr;
    ID3D11PixelShader* m_LayerBlendPixelShader = nullptr;
    ID3D11PixelShader* m_SelectionOutlinePixelShader = nullptr;
    ID3D11InputLayout* m_InputLayout = nullptr;
    ID3D11SamplerState* m_SamplerState = nullptr;

    // Layer Composition Target
    ID3D11Texture2D* m_CompositeTexture = nullptr;
    ID3D11RenderTargetView* m_CompositeRTV = nullptr;
    ID3D11ShaderResourceView* m_CompositeSRV = nullptr;
    // History copy for blend modes (cannot sample a texture bound as RTV).
    ID3D11Texture2D* m_CompositeHistoryTexture = nullptr;
    ID3D11ShaderResourceView* m_CompositeHistorySRV = nullptr;
    int m_CompositeWidth = 0;
    int m_CompositeHeight = 0;
    bool m_CompositeDirty = true;
    ID3D11BlendState* m_LayerBlendState = nullptr;
    ID3D11RasterizerState* m_RasterizerState = nullptr;

    PaintTarget m_PaintTarget = PaintTarget::LayerContent;
    void EnsureActiveLayerMaskAllocated();
    void PaintMaskStamp(float cx, float cy, const BrushSettings& brush);

    // Group Compositing: temp RT for rendering a group's children before blending into main
    ID3D11Texture2D* m_GroupCompositeTexture = nullptr;
    ID3D11RenderTargetView* m_GroupCompositeRTV = nullptr;
    ID3D11ShaderResourceView* m_GroupCompositeSRV = nullptr;
    void CreateGroupCompositeResources(ID3D11Device* device);
    void ReleaseGroupCompositeResources();

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
    IccPreset m_ExportIccPreset = IccPreset::sRGB;

    float m_RotationAngle = 0.0f;
    bool m_MirrorHorizontal = false;
    bool m_MirrorVertical = false;
    bool m_ViewportFlipH = false;
    bool m_ViewportFlipV = false;
    ProjectType m_ProjectType = ProjectType::Advanced;

    // Selection State
    bool m_HasSelection = false;
    std::vector<uint8_t> m_SelectionMask; // 0=not selected, 255=fully selected
    ID3D11Texture2D* m_SelectionMaskTexture = nullptr;
    ID3D11ShaderResourceView* m_SelectionMaskSRV = nullptr;
    float m_SelectionOutlineTime = 0.0f;
    bool m_SelectionMaskNeedsUpload = false;
    std::atomic<bool> m_SmartSelectInProgress{false};
    std::atomic<bool> m_SmartSelectCancelled{false};

    // Magic wand sticky seed
    bool m_WandSeedValid = false;
    int  m_WandSeedX = 0;
    int  m_WandSeedY = 0;
    // Cached RGB8 source for live tolerance scrub (invalidated on layer edit).
    std::vector<uint8_t> m_WandSourceRGB;
    int m_WandSourceW = 0, m_WandSourceH = 0;
    int m_WandSourceLayerIdx = -1;

    // Quick select session
    std::vector<uint8_t> m_QuickSelectMask; // working mask during stroke
    std::vector<uint8_t> m_QuickSelectEdge; // cached edge strength 0-255
    bool m_QuickSelectEdgeValid = false;
    float m_QuickSelectLabMean[3] = {0,0,0};
    int   m_QuickSelectSampleCount = 0;

    // Move Pixels State
    bool m_IsMovingPixels = false;
    std::vector<float> m_FloatingPixels;
    // BBox of floating content in document space (for perf; full buffer may still be used as fallback)
    int m_FloatingBBoxX = 0, m_FloatingBBoxY = 0, m_FloatingBBoxW = 0, m_FloatingBBoxH = 0;
    int m_FloatingBufW = 0, m_FloatingBufH = 0; // size of m_FloatingPixels layout
    std::vector<uint8_t> m_OriginalSelectionMask;
    int m_FloatingOffsetX = 0;
    int m_FloatingOffsetY = 0;
    int m_StartActiveLayerIdx = -1;
    float m_FloatingScaleX = 1.0f;
    float m_FloatingScaleY = 1.0f;
    float m_FloatingRotation = 0.0f; // in radians

    ID3D11Texture2D* m_FloatingTexture = nullptr;
    ID3D11ShaderResourceView* m_FloatingSRV = nullptr;
    ID3D11Texture2D* m_FloatingMaskTexture = nullptr;
    ID3D11ShaderResourceView* m_FloatingMaskSRV = nullptr;
};
