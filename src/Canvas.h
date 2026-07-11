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
#include "core/TileCache.h"
#include "core/PaintEngine.h"
#include "core/DdsHelper.h"
#include "core/UndoRedoManager.h"
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
    // Values 0-255 (RGBA8 docs) or float reinterpreted as uint8 for GPU (R8_UNORM).
    std::vector<uint8_t> mask;
    ID3D11Texture2D* maskTexture = nullptr;
    ID3D11ShaderResourceView* maskSRV = nullptr;
    bool hasMask = false;
    bool maskNeedsUpload = false;
    // Dirty rect for GPU upload (x1 < x0 ⇒ full upload). Keeps maskSRV pointer stable.
    int maskDirtyX0 = 0, maskDirtyY0 = 0, maskDirtyX1 = -1, maskDirtyY1 = -1;

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
    // Source bytes for smart objects / SVG (empty for pure raster).
    std::vector<uint8_t> smartSourceBytes;
    std::string smartSourcePath;
    float smartScale = 1.0f;

    // Texture-set participation (Plan 0): which maps/roles this layer writes.
    // Default = Diffuse only (Simple-compatible). Fill layers update this from fill.target.
    texset::LayerWorkSpace workSpace;

    bool IsFill() const { return type == Type::Fill; }
    bool CanPaintContent() const {
        return !isGroup && type != Type::Fill && type != Type::Group;
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
    // Bake filters/styles/fill into raster tiles. Groups: flatten children into one raster.
    bool RasterizeLayer(ID3D11Device* device, int layerIdx);
    bool RasterizeGroup(ID3D11Device* device, int groupIdx);

    // Layer Management
    void CreateNewLayer(ID3D11Device* device, const std::string& name);
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
    // Load RGBA texture into Fill layer (or clear if path empty). Returns false on failure.
    bool LoadFillTexture(int layerIdx, const std::string& filepath);
    // Load texture for outline style on layer.
    bool LoadOutlineTexture(int layerIdx, int styleIdx, const std::string& filepath);

    // Layer styles API
    int  AddLayerStyle(int layerIdx, StyleType type);
    void RemoveLayerStyle(int layerIdx, int styleIdx);
    void MarkLayerStylesDirty(int layerIdx);
    // Debounced style/presentation rebuild (call from UI while dragging FX params).
    void RequestPresentationRebuild(int layerIdx);
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
    void MarkCompositeDirty() { m_CompositeDirty = true; m_ChannelPreviewDirty = true; }
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
    // Abort in-progress quick-select stroke without modifying document selection / undo.
    void CancelQuickSelectStroke();
    bool IsQuickSelectStrokeActive() const { return !m_QuickSelectMask.empty(); }
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
        DirectX::XMFLOAT4 transformParams; // x: scaleX, y: scaleY, z: rotation, w: isFloating
        // x,y: center; z: blendMode; w: alphaRewrite (1=overwrite A, 0=A is RGB strength only)
        DirectX::XMFLOAT4 centerParams;
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

    // Mask stroke undo buffer
    std::vector<uint8_t> m_MaskStrokeBackup;
    bool m_MaskStrokeBackupValid = false;

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
    // RGB: SRC_ALPHA/INV_SRC_ALPHA; Alpha: ZERO/ONE — keeps dest A (Alpha Rewrite OFF)
    ID3D11BlendState* m_LayerBlendStateAlphaPreserve = nullptr;
    // Bottom layer init: ONE/ZERO full RGBA replace (RGB survives A=0)
    ID3D11BlendState* m_LayerBlendStateReplace = nullptr;
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
    // Rebuild presentation cache when styles/filters/fill require baked buffer for GPU.
    // fullQuality=true: document-res bake (export/rasterize). false: proxy preview.
    void RebuildLayerPresentation(Layer& layer, bool fullQuality = false);
    // Group: flatten children then apply group filters/styles → presentationCache + GPU tex.
    void RebuildGroupPresentation(int groupIdx, bool fullQuality = false);
    // Resolve raw content (tiles or fill solid) to float RGBA W×H.
    std::vector<float> ResolveLayerContentF(const Layer& layer) const;
    // Ensure Fill layer has a GPU texture (1×1 solid or full presentation).
    void EnsureFillLayerGpu(ID3D11Device* device, Layer& layer);
    // True if layer should be drawn as top-level (parentGroupId < 0 or parent missing).
    static bool IsTopLevelLayer(const Layer& layer);
    // True if layerIdx is under groupIdx (any depth).
    bool IsLayerUnderGroup(int layerIdx, int groupIdx) const;


    bool ExtractAndSetICCProfile(const std::string& pngPath);

    // Stroke state tracking
    bool m_IsStrokeActive = false;
    float m_LastDabX = 0.0f;
    float m_LastDabY = 0.0f;
    float m_StrokeDistanceAccumulator = 0.0f;
    float m_PrevStabilizedX = 0.0f;
    float m_PrevStabilizedY = 0.0f;

    // Debounce style/presentation rebuild while dragging FX sliders (~80ms).
    std::chrono::steady_clock::time_point m_PresentationRebuildNotBefore{};
    bool m_PresentationRebuildDeferred = false;

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
    ProjectType m_ProjectType = ProjectType::Advanced;
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
    std::vector<uint8_t> m_QuickSelectMask; // working mask during stroke
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
