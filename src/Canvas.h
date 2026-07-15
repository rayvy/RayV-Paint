#pragma once
#include <imgui.h>

#include <d3d11.h>
#include <directxmath.h>
#include <vector>
#include <string>
#include <atomic>
#include <functional>
#include <memory>
#include <chrono>
#include <algorithm>
#include "core/TileCache.h"
#include "core/MaskTiles.h"
#include "core/PaintEngine.h"
#include "core/DdsHelper.h"
#include "core/UndoRedoManager.h"
#include "core/GpuStagingUpload.h"
#include "core/GpuTileStore.h"
#include "core/UpdateScheduler.h"
#include "core/GpuFxBlur.h"
#include "core/AsyncFilterJob.h"
#include "modio/ModTypes.h"
#include "layer/LayerTypes.h"
#include "texset/TextureSetTypes.h"
#include "texset/TextureSet.h"

// What the brush/smudge tools write into for the active layer (Photoshop-like).
enum class PaintTarget : uint8_t {
    LayerContent = 0, // paint RGB(A) into tileCache
    LayerMask    = 1  // paint grayscale into layer.mask (white=reveal, black=hide)
};

// Optional progress for long I/O (open image / .rayp). UI shows a bar; core stays free of ImGui.
// progress01 is in [0,1]; stage is a short stable tag e.g. "decode", "layers", "upload".
using LoadProgressFn = std::function<void(float progress01, const char* stage)>;

struct Layer {
    std::string name;
    bool visible = true;
    // Content / fill opacity. Does NOT multiply LayerStyles (shadow/outline use style.opacity).
    float opacity = 1.0f;
    BlendMode blendMode = BlendMode::Normal;

    // When true (default for new/open base layers): paint/import may overwrite alpha.
    // When false (default for imported decals): stamp A is morph strength for RGB only;
    // destination alpha is preserved.
    bool alphaRewrite = true;

    // Primary pixel data (replaces std::vector<float> pixels).
    // nullptr for group header layers (isGroup == true) and optional for Fill.
    std::unique_ptr<TileCache> tileCache;

    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    // Sparse GPU tiles (GpuTileStore). Non-zero ⇒ prefer tiled draw over full texture.
    // Full texture still used for small docs / fill / styles / fallback.
    uint32_t gpuSurfaceId = 0;
    // What is currently uploaded into GPU surface/texture: 0=raw, 1=filtered, 2=presentation.
    // Mismatch forces full re-upload (fixes filter toggle / FX ghosting).
    uint8_t gpuDisplayKind = 0xFF;
    bool needsUpload = false; // set true to force full re-upload (fallback)

    // UI list thumb (small RGB, A forced 255 when ignore-alpha / buffer view)
    ID3D11Texture2D* thumbTex = nullptr;
    ID3D11ShaderResourceView* thumbSRV = nullptr;
    bool thumbDirty = true;
    int thumbSize = 0;

    // Non-destructive pixel filters (Blur, Curves, …)
    std::vector<LayerFilter> filters;
    // Cache: content with filters applied (no styles). Rebuilt when filtersDirty=true.
    std::unique_ptr<TileCache> filteredCache;
    bool filtersDirty = true;

    // Layer styles (Shadow, Outline) — independent opacity from layer.opacity
    std::vector<LayerStyle> styles;
    // When styles present: baked presentation (styles + content*opacity) for single GPU draw at opacity=1.
    std::unique_ptr<TileCache> presentationCache;
    bool stylesDirty = true;
    bool presentationDirty = true;

    // Mask: single-channel, same canvas dimensions.
    // Prefer sparse MaskTiles (COW); flat `mask` is a cache for legacy / pack paths.
    std::vector<uint8_t> mask;
    std::unique_ptr<class MaskTiles> maskTiles;
    ID3D11Texture2D* maskTexture = nullptr;
    ID3D11ShaderResourceView* maskSRV = nullptr;
    bool hasMask = false;
    bool maskNeedsUpload = false;
    // Dirty rect for GPU upload (x1 < x0 ⇒ full upload). Keeps maskSRV pointer stable.
    int maskDirtyX0 = 0, maskDirtyY0 = 0, maskDirtyX1 = -1, maskDirtyY1 = -1;

    // Native-resolution map storage (import at map size ≠ document).
    // Viewport/paint use tileCache at document UV; export prefers nativeMapCache when valid.
    std::unique_ptr<TileCache> nativeMapCache;
    int nativeMapW = 0, nativeMapH = 0;
    texset::MapKind nativeMapKind = texset::MapKind::Diffuse;

    // Group support
    bool isGroup        = false; // group header — no pixel data
    int  parentGroupId  = -1;    // -1 = top-level
    bool groupExpanded  = true;

    // Layer type (Raster default; Group when isGroup; Fill; SmartObject/VectorSvg)
    enum class Type : uint8_t {
        Raster = 0,
        Group  = 1,
        SmartObject = 2,
        VectorSvg   = 3,
        Fill   = 4
    };
    Type type = Type::Raster;
    // Fill params (type == Fill)
    FillLayerParams fill;
    // GPU fill pattern (source-res texture). Interactive path samples this instead of
    // baking full document RGBA — kills Fill-texture lag.
    ID3D11Texture2D* fillPatternTex = nullptr;
    ID3D11ShaderResourceView* fillPatternSRV = nullptr;
    std::string fillPatternUploadedKey; // which asset key is currently on GPU
    // Source bytes for smart objects / SVG (empty for pure raster).
    std::vector<uint8_t> smartSourceBytes;
    std::string smartSourcePath;
    // Optional AssetStore key (proj:/user:/core:) for SmartObject source identity.
    std::string smartAssetKey;
    float smartScale = 1.0f;

    // Texture-set participation (Plan 0): which maps/roles this layer writes.
    // Default = Diffuse only (Simple-compatible). Fill layers update this from fill.target.
    texset::LayerWorkSpace workSpace;

    bool IsFill() const { return type == Type::Fill; }
    // SmartObject / VectorSvg: content paint locked until Rasterize (mask still OK).
    bool CanPaintContent() const {
        return !isGroup && type != Type::Fill && type != Type::Group
            && type != Type::SmartObject && type != Type::VectorSvg;
    }
    bool HasEnabledStyles() const { return LayerStyleListHasEnabled(styles); }

    // Sync workSpace from FillChannelTarget. Optional set resolves map from pack table.
    void SyncWorkSpaceFromFillTarget(const texset::TextureSet* set = nullptr);

    // Does this layer participate in the current map/role view?
    bool ParticipatesInView(texset::MapKind viewMap, bool roleIsolate,
                            texset::ChannelRole soloRole) const;
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
    friend class RasterizeCommand;
    friend class LayerMaskCommand;
    friend class LayerMaskPaintCommand;
    friend class LayerStackCommand;
    friend class LayerPropsCommand;
    friend class PaintStrokeCommand;

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

    enum class IccPreset : uint8_t { None = 0, sRGB = 1, DisplayP3 = 2, AdobeRGB = 3, Linear = 4 };
    static const char* IccPresetName(IccPreset p);
    static IccPreset IccPresetFromName(const std::string& name);
    void SetExportIccPreset(IccPreset p);
    IccPreset GetExportIccPreset() const { return m_ExportIccPreset; }
    // Embedded ICC bytes for current preset (empty for None). For UI diagnostics / export.
    static const std::vector<uint8_t>& GetIccPresetBytes(IccPreset p);

    // Project brush tip preference (persisted in .rayp). UI maps id → BrushPresets / custom.
    // ids: "procedural" | "soft_round" | "hard_round" | "pencil" | "airbrush" | "custom"
    void SetBrushTipId(const std::string& id) { m_BrushTipId = id; }
    const std::string& GetBrushTipId() const { return m_BrushTipId; }
    void SetCustomBrushTip(int size, const std::vector<uint8_t>& pixels);
    bool GetCustomBrushTip(int& outSize, std::vector<uint8_t>& outPixels) const;

    // Layer types / smart objects (Layer::Type)
    bool ImportSvgAsSmartObject(ID3D11Device* device, const std::string& filepath);
    // Convert raster (or baked display) layer into SmartObject; registers project asset.
    bool ConvertLayerToSmartObject(ID3D11Device* device, int layerIdx);
    // Replace SO source from an asset key (Texture kind). Re-uploads display from asset.
    bool ReplaceSmartObjectSource(ID3D11Device* device, int layerIdx, const std::string& assetKey);
    // Bake filters/styles/fill into raster tiles. Groups: flatten children into one raster.
    bool RasterizeLayer(ID3D11Device* device, int layerIdx);
    bool RasterizeGroup(ID3D11Device* device, int groupIdx);

    // Layer Management
    void CreateNewLayer(ID3D11Device* device, const std::string& name);
    // Capture layer for undo (tiles COW + mask tiles). Friend of LayerStackCommand.
    static LayerStackCommand::Snap CaptureLayerSnap(const Layer& L, int index, int docW, int docH,
                                                    CanvasPixelFormat fmt);
    // Capture non-pixel props for undo (FX / opacity / blend / fill params).
    static LayerPropsCommand::Props CaptureLayerProps(const Layer& L);
    // Push undo if before/after differ. Call after user finishes an edit session.
    void CommitLayerPropsEdit(int layerIdx, const LayerPropsCommand::Props& before,
                              const char* actionName);
    // Import image/DDS as a real paint layer bound to one map (LightMap/Normal/…).
    // Visible in Layers panel; participates only when that map is active in viewport.
    // Pixels are placed in document UV space (scaled to canvas size if needed).
    bool ImportImageAsMapLayer(ID3D11Device* device, const std::string& filepath,
                               texset::MapKind mapKind, const std::string& layerName = {});
    // Substance-like fill layer: full-canvas color, no content paint; mask paintable.
    void CreateFillLayer(ID3D11Device* device, const std::string& name,
                         const FillLayerParams& params = {});
    bool IsFillLayer(int layerIdx) const;
    bool CanPaintLayerContent(int layerIdx) const;
    // Load RGBA texture into Fill layer (or clear if path empty).
    // Imports file into Project session asset (proj:) — no long-term absolute path dependency.
    bool LoadFillTexture(int layerIdx, const std::string& filepath);
    // Bind Fill to an existing AssetManager key (Texture kind only). AddRefs key.
    bool BindFillTextureAsset(int layerIdx, const std::string& assetKey);
    // Load texture for outline style on layer.
    bool LoadOutlineTexture(int layerIdx, int styleIdx, const std::string& filepath);

    // Layer styles API
    int  AddLayerStyle(int layerIdx, StyleType type);
    void RemoveLayerStyle(int layerIdx, int styleIdx);
    void MarkLayerStylesDirty(int layerIdx);
    // Debounced style/presentation rebuild (call from UI while dragging FX params).
    void RequestPresentationRebuild(int layerIdx);

    // Interactive FX preview (styles + filters). OFF = paint raw content, no CPU bake.
    // Does not delete FX; re-enable rebuilds presentation. Export always bakes full FX.
    bool GetEffectsPreviewEnabled() const { return m_EffectsPreviewEnabled; }
    void SetEffectsPreviewEnabled(bool enabled);
    // True when this layer's styles/filters should run in the interactive path.
    bool LayerStylesPreviewActive(const Layer& layer) const;
    bool LayerFxPreviewActive(const Layer& layer) const;
    void DeleteLayer(int index);
    // Clone layer (or group header) inserted after source. Returns new index, or -1.
    int  DuplicateLayer(ID3D11Device* device, int index);
    // Duplicate multiple indices (sorted ascending; each clone placed after original).
    void DuplicateLayers(ID3D11Device* device, const std::vector<int>& indices);
    // Delete many (highest index first). Keeps at least one layer if possible.
    void DeleteLayers(const std::vector<int>& indices);
    // Merge upper layer into the one below (index-1), applying upper blendMode+opacity.
    // Returns new active index (the surviving lower layer), or -1 on failure.
    int MergeLayerDown(ID3D11Device* device, int upperIdx);
    // Merge all selected indices (bottommost survives). Empty/invalid → no-op, returns -1.
    int MergeLayers(ID3D11Device* device, const std::vector<int>& indices);
    void SetActiveLayerIndex(int idx);
    void ToggleLayerIsolation(int layerIdx);
    // Full-proxy recompose (layer stack / FX / format change).
    void MarkCompositeDirty() {
        m_CompositeDirty = true;
        m_CompositeDirtyFull = true;
        m_CompositeDirtyValid = false;
        m_ChannelPreviewDirty = true;
    }
    // Partial recompose: union document-space AABB (inclusive). Paint hot path.
    void InvalidateCompositeRect(int x0, int y0, int x1, int y1);
    // Force GPU re-upload of all layer/presentation tiles + recompose.
    // Call after FX toggle, undo glitches, or F5 — kills tile ghosting.
    void RefreshCanvas(ID3D11Device* device = nullptr);
    bool IsLayerIsolated(int layerIdx) const { return m_IsIsolatedMode && m_IsolatedLayerIdx == layerIdx; }
    bool IsInIsolationMode() const { return m_IsIsolatedMode; }
    int GetActiveLayerIndex() const { return m_ActiveLayerIdx; }
    std::vector<Layer>& GetLayers() { return m_Layers; }
    const std::vector<Layer>& GetLayers() const { return m_Layers; }
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
    // Local brush blur (Tool). Operator/Filter blur remain separate.
    void BlurToolOnActiveLayer(float x, float y, StrokePhase phase, const SmudgeSettings& s);

    // Clone Stamp (source point + offset resampling). Alt+click sets source.
    void StampSetSource(float canvasX, float canvasY);
    void StampClearSource();
    bool StampHasSource() const { return m_StampHasSource; }
    bool StampHasOffset() const { return m_StampHasOffset; }
    void StampGetSource(float& outX, float& outY) const { outX = m_StampSrcX; outY = m_StampSrcY; }
    // Call on first dab after source is set: locks clone offset (dest - source).
    void StampLockOffsetFromDab(float dabX, float dabY);
    void StampGetOffset(float& outOx, float& outOy) const { outOx = m_StampOffsetX; outOy = m_StampOffsetY; }
    // Content-Aware Fill (requires selection). Uses utilities/ContentAwareFill module.
    bool ApplyContentAwareFill(ID3D11Device* device = nullptr);

    // Destructive image adjustments (operate on active layer, respect selection mask)
    void SelectAll();                       // full canvas selection (with undo)
    void SelectOpaquePixels(int layerIdx = -1); // alpha-as-mask; -1 = active layer
    // Load layer mask into selection (white/high = selected). Alt+Click on mask thumb.
    void SelectFromLayerMask(int layerIdx = -1);
    void InvertSelection();
    void InvertAlpha();
    // Invert RGB (keeps alpha). Respects selection; uses existing mutation/undo path.
    void InvertColors();
    void ApplyBlur(float radius);
    void ApplyHSV(float dH, float dS, float dV);
    // lutRGB: 256 floats [0..1]; lutAlpha optional 256 floats (empty = leave A unchanged)
    void ApplyCurves(const std::vector<float>& lutRGB, const std::vector<float>& lutAlpha = {});
    void ApplyNoise(float strength, bool colorNoise);

    // Live destructive-adjust preview (active layer + selection). Matches Apply* result.
    // Begin snapshots base; Update* rewrite layer from base; Commit undoes to snapshot; Cancel restores.
    bool BeginAdjustPreview();
    void CancelAdjustPreview();
    void CommitAdjustPreview(const std::string& actionName);
    bool IsAdjustPreviewActive() const { return m_AdjustPreviewActive; }
    void UpdateAdjustPreviewHSV(float dH, float dS, float dV);
    void UpdateAdjustPreviewCurves(const std::vector<float>& lutRGB, const std::vector<float>& lutAlpha = {});
    void UpdateAdjustPreviewBlur(float radius);
    void UpdateAdjustPreviewNoise(float strength, bool colorNoise);

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
    // Quick Select (PS-like): progressive live selection under brush.
    // Begin → Stroke (live outline) → End (undo) · Alt = subtract.
    void BeginQuickSelectStroke();
    void StrokeQuickSelect(ID3D11Device* device, const std::vector<std::pair<int, int>>& points,
                           float radius, bool subtract);
    void EndQuickSelectStroke(ID3D11Device* device, bool subtract);
    // Abort in-progress quick-select stroke; restores selection from stroke start.
    void CancelQuickSelectStroke();
    bool IsQuickSelectStrokeActive() const { return m_QuickSelectStrokeActive; }
    bool IsSmartSelectInProgress() const { return m_SmartSelectInProgress.load(); }
    void CancelSmartSelect() { m_SmartSelectCancelled.store(true); }
    void ApplyBucketFill(int startX, int startY, float tolerance, const float color[4], bool contiguous);
    void ApplyGradient(int x1, int y1, int x2, int y2, const float startColor[4], const float endColor[4]);

    // Selection edit / clipboard (Photoshop-like)
    // Fill selected pixels with solid color (Backspace → secondary). Respects selection; undo.
    void FillSelection(const float rgba[4]);
    // Clear selected pixels (Delete → transparent). On mask target: paint selection black.
    void DeleteSelectionContent();
    // Copy: selection content if any, else full active layer. Also system CF_DIB when possible.
    bool CopyContentToClipboard();
    // Copy full layers (structure+pixels+mask) to internal layer clipboard.
    bool CopyLayersToClipboard(const std::vector<int>& indices);
    bool HasLayerClipboard() const { return m_LayerClipboardValid; }
    bool HasContentClipboard() const { return m_ContentClipboardValid; }
    // Paste layers from internal clipboard (inserted after active).
    bool PasteLayersFromClipboard(ID3D11Device* device);
    // Paste pixel content into active layer (or mask if paint target is mask). Blends at center.
    bool PasteContentIntoActive(ID3D11Device* device);
    // Paste as brand-new layer (always).
    bool PasteContentAsNewLayer(ID3D11Device* device, const std::string& name = "Pasted Layer");

    // Move Pixels operations
    bool IsMovingPixels() const { return m_IsMovingPixels; }
    void StartMovePixels(ID3D11Device* device);
    void UpdateMovePixels(ID3D11Device* device, int dx, int dy);
    void CommitMovePixels(ID3D11Device* device);
    void CancelMovePixels(ID3D11Device* device);
    // showHandles=false: Move tool (bbox only). true: Free Transform (scale/rotate handles).
    void DrawMoveGizmo(ImDrawList* dl, const std::function<ImVec2(float, float)>& canvasToScreen,
                       bool showHandles = true);

    // Perspective / mesh Warp operators (Ctrl+Shift+P / Ctrl+Shift+W via UI)
    enum class WarpOperatorMode : uint8_t { None = 0, Perspective = 1, Mesh = 2 };
    bool IsWarpOperatorActive() const { return m_WarpMode != WarpOperatorMode::None; }
    WarpOperatorMode GetWarpOperatorMode() const { return m_WarpMode; }
    void StartWarpOperator(ID3D11Device* device, WarpOperatorMode mode);
    void SetWarpControlPoint(int index, float canvasX, float canvasY);
    int  HitTestWarpControl(float canvasX, float canvasY, float threshPx = 10.f) const;
    int  GetWarpControlCount() const { return (int)m_WarpControls.size(); }
    bool GetWarpControl(int index, float& outX, float& outY) const;
    void PreviewWarpOperator(ID3D11Device* device); // rebuild floating tex from controls
    void CommitWarpOperator(ID3D11Device* device);
    void CancelWarpOperator(ID3D11Device* device);
    void DrawWarpGizmo(ImDrawList* dl, const std::function<ImVec2(float, float)>& canvasToScreen);
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

    // Channel solo controls (toggles mark composite dirty — buffer view depends on them)
    bool GetChannelR() const { return m_ChannelR; }
    bool GetChannelG() const { return m_ChannelG; }
    bool GetChannelB() const { return m_ChannelB; }
    bool GetChannelA() const { return m_ChannelA; }
    void SetChannelR(bool r);
    void SetChannelG(bool g);
    void SetChannelB(bool b);
    void SetChannelA(bool a);

    // Advanced+ viewport: which texture-set map is shown (default Diffuse).
    // Layers with workSpace not affecting this map are skipped in compose.
    texset::MapKind GetViewMapKind() const { return m_ViewMapKind; }
    void SetViewMapKind(texset::MapKind k);
    // Isolate a logical role as grayscale (false = full map RGBA with R/G/B/A toggles)
    bool GetViewRoleIsolate() const { return m_ViewRoleIsolate; }
    texset::ChannelRole GetViewSoloRole() const { return m_ViewSoloRole; }
    void SetViewRoleIsolate(bool on, texset::ChannelRole role = texset::ChannelRole::None);
    // Apply pack channel → auto-set R/G/B/A solo for role (uses map pack table)
    void ApplyViewRoleToChannelMasks(const texset::MapSlot* slot);

    // Packing tables of active texture set (for Fill ResolveForMap). UI/Project syncs this.
    void SetActiveSetMaps(const std::vector<texset::MapSlot>& maps);
    const std::vector<texset::MapSlot>& GetActiveSetMaps() const { return m_ActiveSetMaps; }

    // Non-Diffuse view underlay: imported map composite (LightMap/Normal/…) shown under layers.
    // Call when switching maps. Pass nullptr to clear.
    void SetViewMapUnderlay(ID3D11Device* device, const TileCache* cache);
    void ClearViewMapUnderlay();
    bool HasViewMapUnderlay() const { return m_ViewUnderlaySRV != nullptr; }

    ID3D11ShaderResourceView* GetCompositeSRV() const { return m_CompositeSRV; }

    // Channel preview thumbs (R/G/B/A as grayscale). Built from *layer tile data*
    // (not composite), so RGB is visible even when A=0 (buffer semantics).
    // Returns nullptr if unavailable. Caller does not own the SRV.
    enum class ChannelPreview : uint8_t { R = 0, G = 1, B = 2, A = 3 };
    ID3D11ShaderResourceView* GetChannelPreviewSRV(ID3D11Device* device, ChannelPreview ch);

    // Layer list thumb: always RGB with A=255 so ImGui doesn't hide A=0 buffers.
    // When forceOpaque is false and channel A is on, still keeps A=255 for list readability
    // in tech-art buffer mode (RGB must never vanish).
    ID3D11ShaderResourceView* GetLayerThumbSRV(ID3D11Device* device, int layerIdx, int size = 48);

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
    // Batch export: pack layers/fills into one map's RGBA using project packing table.
    // importedBase: optional imported TileCache for that map (underlay).
    bool ComposePackedMapRGBA8(texset::MapKind kind,
                               const std::vector<texset::MapSlot>& maps,
                               const TileCache* importedBase,
                               std::vector<uint8_t>& outRgba,
                               int& outW, int& outH) const;
    // Composite sample (float-accurate when document is F16/F32; U8 path quantizes).
    void SampleCompositePixel(int x, int y, float outColor[4]) const;
    // Raw active-layer pixel (no blend) — preferred for height/HDR diagnostics.
    void SampleActiveLayerPixel(int x, int y, float outColor[4]) const;
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
    // Drop all layers + GPU without undo (project setup / replace base)
    void ClearAllLayersNoUndo();
    bool IsDocumentModified() const { return m_IsDocumentModified; }
    void SetDocumentModified(bool modified) { m_IsDocumentModified = modified; }
    std::string GetCurrentProjectFilePath() const { return m_CurrentProjectFilePath; }
    void SetCurrentProjectFilePath(const std::string& path) { m_CurrentProjectFilePath = path; }

    // Hard export container — single source of truth (not inferred from path extension).
    enum class ExportContainer : uint8_t {
        PNG = 0, // standard image + ICC presets only
        DDS = 1  // texconv: format / mips / compression quality
    };
    ExportContainer GetExportContainer() const { return m_ExportContainer; }
    void SetExportContainer(ExportContainer c);
    // Align export path extension with container (.png / .dds). Returns updated path.
    std::string SyncExportPathExtension();
    // Write using container: DDS → SaveCanvasCompressed, PNG → SaveCanvasStandard(ICC).
    // Empty path → uses GetExportPath() or default export.dds / export.png.
    bool ExportWithProjectSettings(std::string* outPathUsed = nullptr);

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
        Advanced,
        AdvancedModMode  // XXMI/mod multi-map + optional 3D preview (meta in .rayp)
    };

    // Document color bit depth (Photoshop-like). Global working space for all channels.
    // Export remains free (any DDS/PNG packing target). Default U8 keeps current perf.
    enum class DocumentBitDepth : uint8_t {
        U8  = 0,  // 8-bit/channel → RGBA8 tiles (4 B/px)
        F16 = 1,  // 16-bit float → RGBA16F tiles (8 B/px)
        F32 = 2   // 32-bit float → RGBA32F tiles (16 B/px)
    };
    DocumentBitDepth GetDocumentBitDepth() const { return m_DocumentBitDepth; }
    void SetDocumentBitDepth(DocumentBitDepth d); // converts all layer tiles + GPU
    static CanvasPixelFormat FormatForBitDepth(DocumentBitDepth d);

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

    // ---- Advanced Mod Mode (optional 3D preview sources) ----
    // Paths are stored in .rayp; broken paths never crash — paint still works.
    const std::string& GetModIniPath() const { return m_ModIniPath; }
    void SetModIniPath(const std::string& p) { m_ModIniPath = p; m_IsDocumentModified = true; }
    const std::string& GetModDumpPath() const { return m_ModDumpPath; }
    void SetModDumpPath(const std::string& p) { m_ModDumpPath = p; m_IsDocumentModified = true; }

    // Re-parse INI → updates GetModScene(). Soft-fail (returns false + fills warnings).
    bool ApplyModIniParse();
    // Apply dump folder (vb0 layout headers) → overrides formats/roles (keeps manual locks).
    bool ApplyModDumpParse();
    const modio::ModScene& GetModScene() const { return m_ModScene; }
    modio::ModScene& GetModScene() { return m_ModScene; }
    const std::string& GetModParseSummary() const { return m_ModParseSummary; }
    bool IsModParseOk() const { return m_ModScene.ok; }

    // Native RAYP Format
    bool SaveCanvasRayp(const std::string& filepath);
    void SaveCanvasRaypAsync(const std::string& filepath, std::function<void(bool)> callback = nullptr);
    bool LoadCanvasRayp(const std::string& filepath, ID3D11Device* device, LoadProgressFn progress = nullptr);

    // Texture Set library meta (JSON) — owned by Project; mirrored here for .rayp I/O.
    // Call SetTextureSetsMetaJson before Save; read GetTextureSetsMetaJson after Load.
    void SetTextureSetsMetaJson(std::string json) { m_TextureSetsMetaJson = std::move(json); }
    const std::string& GetTextureSetsMetaJson() const { return m_TextureSetsMetaJson; }
    void ClearTextureSetsMetaJson() { m_TextureSetsMetaJson.clear(); }

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
        // w: 0=normal, 1=floating, 2=fillTexture (GPU pattern, no full-doc bake)
        DirectX::XMFLOAT4 transformParams; // x: scaleX, y: scaleY, z: rotation, w: mode
        // x,y: center; z: blendMode; w: alphaRewrite (1=overwrite A, 0=A is RGB strength only)
        DirectX::XMFLOAT4 centerParams;
        // Floating texture rect in document UV (only when isFloating):
        // xy = origin (bbox min / canvasSize), zw = size (bbox / canvasSize).
        // Zero size → legacy full-document floating UV.
        // Fill-texture mode: xy=texScale, zw=texOffset.
        DirectX::XMFLOAT4 floatRect;
        DirectX::XMFLOAT4 fillColor; // tint for fill-texture mode
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

    // Texture-set view (Advanced+). Simple mode ignores and always composes Diffuse.
    texset::MapKind m_ViewMapKind = texset::MapKind::Diffuse;
    bool m_ViewRoleIsolate = false;
    texset::ChannelRole m_ViewSoloRole = texset::ChannelRole::None;
    std::vector<texset::MapSlot> m_ActiveSetMaps; // soft labels + map sizes

    // Mask stroke undo: tiled COW snapshots of region touched this stroke
    std::vector<MaskTileSnapshot> m_MaskStrokeBackupTiles;
    bool m_MaskStrokeBackupValid = false;
    int m_MaskStrokeX0 = 0, m_MaskStrokeY0 = 0, m_MaskStrokeX1 = -1, m_MaskStrokeY1 = -1;

    // Optional underlay (legacy; prefer map layers)
    ID3D11Texture2D* m_ViewUnderlayTex = nullptr;
    ID3D11ShaderResourceView* m_ViewUnderlaySRV = nullptr;
    int m_ViewUnderlayW = 0, m_ViewUnderlayH = 0;

    std::vector<Layer> m_Layers;
    int m_ActiveLayerIdx = -1;



    // Layer Isolation State
    bool m_IsIsolatedMode = false;
    int m_IsolatedLayerIdx = -1;
    std::vector<bool> m_PreIsolationVisibility;

    // Direct3D 11 Resources
    ID3D11Buffer* m_VertexBuffer = nullptr;
    ID3D11Buffer* m_IndexBuffer = nullptr;
    ID3D11Buffer* m_TileQuadVB = nullptr; // DYNAMIC 4-vertex quad for sparse GPU tiles
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
    // Dirty-rect compose (document pixels, inclusive). Full when m_CompositeDirtyFull.
    bool m_CompositeDirtyFull = true;
    bool m_CompositeDirtyValid = false;
    int  m_CompositeDirtyX0 = 0, m_CompositeDirtyY0 = 0;
    int  m_CompositeDirtyX1 = -1, m_CompositeDirtyY1 = -1;
    // Staging ring for DEFAULT layer texture uploads (tile-sized).
    GpuStagingUpload m_TileStaging;
    // Sparse GPU tile surfaces for large documents (VRAM-friendly).
    GpuTileStore m_GpuTiles;
    static constexpr int kTiledGpuMinDim = 2048;
    // Phase C LOD stroke: cap GPU tile uploads per frame while painting.
    // Idle / stroke-end flush with a much higher budget.
    static constexpr int kLodStrokeUploadBudget = 8;
    static constexpr int kIdleUploadBudget = 256;
    bool UseTiledGpu() const { return std::max(m_Width, m_Height) > kTiledGpuMinDim; }
    // Styles need full-layer presentation bake → classic texture. Filters OK on tiles.
    bool UseTiledGpuForLayer(const Layer& layer) const {
        return UseTiledGpu() && layer.CanPaintContent() && !layer.HasEnabledStyles();
    }
    gpu_fx::GpuBlurContext m_GpuBlur;
    bool m_GpuBlurTried = false;
    async_fx::AsyncFilterQueue m_AsyncFilters;
    void EnsureGpuBlur(ID3D11Device* device, ID3D11DeviceContext* ctx);
    void SubmitAsyncFilterBake(int layerIdx);
    void PollAsyncFilterResults();
    // Phase C: deferred presentation / thumb jobs via UpdateScheduler.
    void ScheduleDeferredPresentation(int layerIdx);
    void ScheduleThumbJob(int layerIdx, int size);
    void DrawTiledLayer(ID3D11DeviceContext* context, Layer& layer,
                        float opacityMul, bool useMask, bool isFirst);
    ID3D11BlendState* m_LayerBlendState = nullptr;
    // RGB: SRC_ALPHA/INV_SRC_ALPHA; Alpha: ZERO/ONE — keeps dest A (Alpha Rewrite OFF)
    ID3D11BlendState* m_LayerBlendStateAlphaPreserve = nullptr;
    // Bottom layer init: ONE/ZERO full RGBA replace (RGB survives A=0)
    ID3D11BlendState* m_LayerBlendStateReplace = nullptr;
    // Per-channel write masks for Fill (unchecked channel = no overwrite). Index = channelMask 0..15
    ID3D11BlendState* m_FillWriteMaskBlend[16] = {};
    ID3D11BlendState* m_FillWriteMaskReplace[16] = {};
    ID3D11BlendState* GetFillWriteBlend(ID3D11Device* device, uint8_t channelMask, bool replace);
    ID3D11RasterizerState* m_RasterizerState = nullptr;
    ID3D11RasterizerState* m_RasterizerStateScissor = nullptr;

    // Upload one dirty tile via staging ring (falls back to UpdateSubresource).
    void UploadLayerTile(ID3D11DeviceContext* context, ID3D11Texture2D* dest,
                         int tx, int ty, const uint8_t* data, int bytesPerPixel,
                         int docW, int docH);
    // Upload into layer: GpuTileStore if surface active, else classic texture.
    void UploadLayerContentTile(ID3D11Device* device, ID3D11DeviceContext* context,
                                Layer& layer, int tx, int ty, const uint8_t* data, int bpp);

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
    // Sparse/tile-local filter refresh. onlyDirtyTiles: refilter tiles marked dirty on content
    // (paint hot path). Full rebuild when filtersDirty or filteredCache missing.
    void RefreshFilteredCache(Layer& layer, bool onlyDirtyTiles);
    // Rebuild presentation cache when styles/filters/fill require baked buffer for GPU.
    // fullQuality=true: document-res bake (export/rasterize). false: proxy preview.
    void RebuildLayerPresentation(Layer& layer, bool fullQuality = false);
    // Group: flatten children then apply group filters/styles → presentationCache + GPU tex.
    void RebuildGroupPresentation(int groupIdx, bool fullQuality = false);
    // Resolve raw content (tiles or fill solid) to float RGBA W×H.
    std::vector<float> ResolveLayerContentF(const Layer& layer) const;
    // Ensure Fill layer has a GPU texture (1×1 solid, GPU pattern, or full FX presentation).
    void EnsureFillLayerGpu(ID3D11Device* device, Layer& layer);
    // Upload fill pattern asset to layer.fillPattern* (source resolution, once per key).
    bool EnsureFillPatternGpu(ID3D11Device* device, Layer& layer);
    static void ReleaseFillPatternGpu(Layer& layer);
    // True if layer should be drawn as top-level (parentGroupId < 0 or parent missing).
    static bool IsTopLevelLayer(const Layer& layer);
    // True if layerIdx is under groupIdx (any depth).
    bool IsLayerUnderGroup(int layerIdx, int groupIdx) const;


    bool ExtractAndSetICCProfile(const std::string& pngPath);

    // Stroke state tracking
    bool m_IsStrokeActive = false;
    // When false: skip CPU style/filter bake in interactive path (paint/compose raw).
    bool m_EffectsPreviewEnabled = true;

    // Destructive Image-adjust modal session (HSV / Curves / Blur / Noise)
    bool m_AdjustPreviewActive = false;
    int  m_AdjustPreviewLayerIdx = -1;
    std::vector<float> m_AdjustPreviewBase; // RGBA32F snapshot of active layer at Begin
    uint32_t m_AdjustPreviewNoiseSeed = 1;
    float m_LastDabX = 0.0f;
    float m_LastDabY = 0.0f;
    float m_StrokeDistanceAccumulator = 0.0f;
    float m_PrevStabilizedX = 0.0f;
    float m_PrevStabilizedY = 0.0f;

    // Debounce style/presentation rebuild while dragging FX sliders (~80ms).
    std::chrono::steady_clock::time_point m_PresentationRebuildNotBefore{};
    bool m_PresentationRebuildDeferred = false;

    // Smudge / blur-tool state
    float m_SmudgePickup[4] = { 0.f, 0.f, 0.f, 0.f };
    bool  m_SmudgePickupValid = false;
    float m_SmudgeLastX = 0.f;
    float m_SmudgeLastY = 0.f;
    float m_SmudgeDistAcc = 0.f;
    // Finger buffer: per-pixel RGB(A) being dragged along stroke (true smudge)
    std::vector<float> m_SmudgeFinger; // radius*2+1 square, reused

    // Clone Stamp state (tool)
    bool  m_StampHasSource = false;
    bool  m_StampHasOffset = false;
    float m_StampSrcX = 0.f, m_StampSrcY = 0.f;
    float m_StampOffsetX = 0.f, m_StampOffsetY = 0.f;

    // Undo/Redo Engine
    UndoRedoManager m_UndoRedoManager;
    std::unordered_map<int, TileDelta> m_ActiveStrokeDeltas;
    bool m_IsDocumentModified = false;
    std::string m_CurrentProjectFilePath;

    std::string m_ExportPath = "";
    // Hard switch: PNG vs DDS (never infer from extension alone).
    ExportContainer m_ExportContainer = ExportContainer::DDS;
    // DDS preset string (texconv / UI combo labels)
    std::string m_ExportFormat = "BC7 (sRGB, DX 11+)";
    // Texture set library JSON (Project.textureSets.MetaToJson) for .rayp round-trip
    std::string m_TextureSetsMetaJson;
    bool m_ExportAdvancedMode = false;
    std::string m_ExportCompressionSpeed = "Medium";
    bool m_ExportGenerateMipMaps = true;
    std::string m_ExportMipFilter = "Bicubic";
    std::string m_ExportPngColorSpace = "sRGB";
    IccPreset m_ExportIccPreset = IccPreset::sRGB;
    std::string m_BrushTipId = "procedural";
    int m_CustomBrushTipSize = 0;
    std::vector<uint8_t> m_CustomBrushTipPixels;

    // Channel preview GPU thumbs (proxy size, R8)
    ID3D11Texture2D* m_ChannelPreviewTex[4] = {};
    ID3D11ShaderResourceView* m_ChannelPreviewSRV[4] = {};
    int m_ChannelPreviewW = 0, m_ChannelPreviewH = 0;
    bool m_ChannelPreviewDirty = true;
    void ReleaseChannelPreviewResources();
    void RebuildChannelPreviews(ID3D11Device* device);

    float m_RotationAngle = 0.0f;
    bool m_MirrorHorizontal = false;
    bool m_MirrorVertical = false;
    bool m_ViewportFlipH = false;
    bool m_ViewportFlipV = false;
    // Default Simple: open a lone DDS/PNG = paint+export that file.
    // Advanced is set explicitly by New Project wizard / multi-map import.
    ProjectType m_ProjectType = ProjectType::Simple;
    DocumentBitDepth m_DocumentBitDepth = DocumentBitDepth::U8;

    // Advanced Mod Mode — optional XXMI/3DMigoto binding (3D preview later)
    std::string m_ModIniPath;
    std::string m_ModDumpPath;
    modio::ModScene m_ModScene;
    std::string m_ModParseSummary;

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
    // Cached RGBA8 source for wand / quick-select (alpha matters — transparent ≠ black).
    std::vector<uint8_t> m_WandSourceRGBA;
    int m_WandSourceW = 0, m_WandSourceH = 0;
    int m_WandSourceLayerIdx = -1;

    // Quick select session
    bool m_QuickSelectStrokeActive = false;
    std::vector<uint8_t> m_QuickSelectMask;     // stroke contribution this drag
    std::vector<uint8_t> m_QuickSelectBaseMask; // document selection at Begin (for undo + progressive)
    bool m_QuickSelectBaseHas = false;
    bool m_QuickSelectSubtract = false;
    std::vector<uint8_t> m_QuickSelectEdge; // cached edge strength 0-255
    bool m_QuickSelectEdgeValid = false;

    // Internal content clipboard (pixels)
    bool m_ContentClipboardValid = false;
    int m_ContentClipW = 0, m_ContentClipH = 0;
    std::vector<float> m_ContentClipRGBA; // w*h*4

    // Internal layer clipboard
    bool m_LayerClipboardValid = false;
    struct LayerClipboardEntry {
        std::string name;
        bool isGroup = false;
        float opacity = 1.f;
        BlendMode blendMode = BlendMode::Normal;
        bool visible = true;
        std::vector<float> pixels; // full canvas size if !isGroup
        bool hasMask = false;
        std::vector<uint8_t> mask;
        std::vector<uint8_t> smartSourceBytes;
        std::string smartSourcePath;
        Layer::Type type = Layer::Type::Raster;
    };
    std::vector<LayerClipboardEntry> m_LayerClipboard;
    float m_QuickSelectLabMean[3] = {0,0,0};
    int   m_QuickSelectSampleCount = 0;

    // Move Pixels State
    bool m_IsMovingPixels = false;
    std::vector<float> m_FloatingPixels; // tight AABB (m_FloatingBufW x m_FloatingBufH), not full canvas
    // BBox of floating content in document space (origin of local floating buffer)
    int m_FloatingBBoxX = 0, m_FloatingBBoxY = 0, m_FloatingBBoxW = 0, m_FloatingBBoxH = 0;
    int m_FloatingBufW = 0, m_FloatingBufH = 0; // size of m_FloatingPixels layout (= bbox w/h)
    // Cached selection center (document pixels) — avoids O(W*H) scan every compose frame
    float m_FloatingCenterX = 0.f;
    float m_FloatingCenterY = 0.f;
    // Selection mask cropped to floating bbox (size = bufW*bufH). Empty → treat as fully selected.
    std::vector<uint8_t> m_OriginalSelectionMask;
    int m_FloatingOffsetX = 0;
    int m_FloatingOffsetY = 0;
    int m_StartActiveLayerIdx = -1;
    float m_FloatingScaleX = 1.0f;
    float m_FloatingScaleY = 1.0f;
    float m_FloatingRotation = 0.0f; // in radians

    // Backup tiles that intersect a document-space rect (inclusive).
    void BackupTilesInRect(int x0, int y0, int x1, int y1);

    ID3D11Texture2D* m_FloatingTexture = nullptr;
    ID3D11ShaderResourceView* m_FloatingSRV = nullptr;
    ID3D11Texture2D* m_FloatingMaskTexture = nullptr;
    ID3D11ShaderResourceView* m_FloatingMaskSRV = nullptr;

    // Perspective / mesh warp operator (uses floating content as source)
    WarpOperatorMode m_WarpMode = WarpOperatorMode::None;
    int m_WarpMeshN = 4; // control points per side for Mesh (NxN)
    std::vector<std::pair<float, float>> m_WarpControls; // dest control points (canvas space)
    std::vector<std::pair<float, float>> m_WarpSourceCorners; // original bbox TL,TR,BR,BL
    int m_WarpBBoxX = 0, m_WarpBBoxY = 0, m_WarpBBoxW = 0, m_WarpBBoxH = 0;
    std::vector<float> m_WarpSourcePixels; // frozen source at operator start
    void RebuildWarpPreviewTexture(ID3D11Device* device);
};
