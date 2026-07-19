#pragma once

#include "TileCache.h"
#include "MaskTiles.h"
#include "../layer/LayerTypes.h"
#include "../texset/TextureSetTypes.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>

// Tile-level history delta. Snapshots share TileData (COW) — no full copies
// until the live cache mutates a shared blob.
struct TileDelta {
    int layerIdx = 0;
    int tileX = 0;
    int tileY = 0;
    TileSnapshot oldState; // empty => tile was absent
    TileSnapshot newState; // empty => tile becomes absent
};

class Canvas;

class UndoCommand {
public:
    virtual ~UndoCommand() = default;
    virtual std::string GetName() const = 0;
    virtual void Undo(Canvas* canvas) = 0;
    virtual void Redo(Canvas* canvas) = 0;

    // Private / non-shared overhead (structs, names, selection masks, etc.).
    // Tile pixel blobs are reported via CollectTileData instead.
    virtual size_t GetOverheadBytes() const = 0;

    // Visit every TileData pointer held by this command (may repeat; manager dedupes).
    virtual void CollectTileData(std::unordered_set<const TileData*>& seen) const {}

    // Convenience: unique blobs *inside this command only* + overhead.
    size_t GetMemorySize() const;
};

class PaintStrokeCommand : public UndoCommand {
public:
    PaintStrokeCommand(const std::string& name, int layerIdx, std::vector<TileDelta> deltas);
    std::string GetName() const override { return m_Name; }
    void Undo(Canvas* canvas) override;
    void Redo(Canvas* canvas) override;
    size_t GetOverheadBytes() const override;
    void CollectTileData(std::unordered_set<const TileData*>& seen) const override;

private:
    std::string m_Name;
    int m_LayerIdx;
    std::vector<TileDelta> m_Deltas;
};

class SelectionCommand : public UndoCommand {
public:
    SelectionCommand(const std::string& name,
                     std::vector<uint8_t> oldMask, bool oldHasSelection,
                     std::vector<uint8_t> newMask, bool newHasSelection);
    std::string GetName() const override { return m_Name; }
    void Undo(Canvas* canvas) override;
    void Redo(Canvas* canvas) override;
    size_t GetOverheadBytes() const override;

private:
    std::string m_Name;
    std::vector<uint8_t> m_OldMask;
    bool m_OldHasSelection;
    std::vector<uint8_t> m_NewMask;
    bool m_NewHasSelection;
};

// Layer mask create / delete (full mask only when structure changes)
class LayerMaskCommand : public UndoCommand {
public:
    LayerMaskCommand(const std::string& name, int layerIdx,
                     bool oldHasMask, std::vector<uint8_t> oldMask,
                     bool newHasMask, std::vector<uint8_t> newMask);
    // Prefer tiled snap when available (smaller for mostly-white masks)
    LayerMaskCommand(const std::string& name, int layerIdx,
                     bool oldHasMask, std::vector<MaskTileSnapshot> oldTiles,
                     bool newHasMask, std::vector<MaskTileSnapshot> newTiles,
                     int maskW, int maskH);
    std::string GetName() const override { return m_Name; }
    void Undo(Canvas* canvas) override;
    void Redo(Canvas* canvas) override;
    size_t GetOverheadBytes() const override;
private:
    void Apply(Canvas* canvas, bool hasMask, const std::vector<uint8_t>& mask,
               const std::vector<MaskTileSnapshot>* tiles);
    std::string m_Name;
    int m_LayerIdx = -1;
    bool m_OldHas = false, m_NewHas = false;
    bool m_UseTiles = false;
    int m_MaskW = 0, m_MaskH = 0;
    std::vector<uint8_t> m_OldMask, m_NewMask;
    std::vector<MaskTileSnapshot> m_OldTiles, m_NewTiles;
};

// Mask paint/erase: only dirty-rect tiles (COW), not full 2× document mask
class LayerMaskPaintCommand : public UndoCommand {
public:
    LayerMaskPaintCommand(const std::string& name, int layerIdx,
                          std::vector<MaskTileSnapshot> oldTiles,
                          std::vector<MaskTileSnapshot> newTiles);
    std::string GetName() const override { return m_Name; }
    void Undo(Canvas* canvas) override;
    void Redo(Canvas* canvas) override;
    size_t GetOverheadBytes() const override;
private:
    void Apply(Canvas* canvas, const std::vector<MaskTileSnapshot>& tiles);
    std::string m_Name;
    int m_LayerIdx = -1;
    std::vector<MaskTileSnapshot> m_OldTiles, m_NewTiles;
};

// Non-destructive layer props: opacity, blend, filters, styles, visibility, name, fill params.
// Used for FX panel edits and layer header changes that previously had no undo.
class LayerPropsCommand : public UndoCommand {
public:
    struct Props {
        std::string name;
        bool visible = true;
        float opacity = 1.f;
        BlendMode blendMode = BlendMode::Normal;
        bool alphaRewrite = true;
        std::vector<LayerFilter> filters;
        std::vector<LayerStyle> styles;
        FillLayerParams fill{};
        bool isFill = false;
    };

    LayerPropsCommand(const std::string& name, int layerIdx, Props oldProps, Props newProps);
    std::string GetName() const override { return m_Name; }
    void Undo(Canvas* canvas) override;
    void Redo(Canvas* canvas) override;
    size_t GetOverheadBytes() const override;

private:
    void Apply(Canvas* canvas, const Props& p);
    std::string m_Name;
    int m_LayerIdx = -1;
    Props m_Old;
    Props m_New;
};

// Layer insert / remove (create, delete, duplicate)
class LayerStackCommand : public UndoCommand {
public:
    enum class Kind : uint8_t { Insert = 0, Remove = 1 };

    struct Snap {
        int index = 0;
        std::string name;
        uint8_t type = 0;
        bool isGroup = false;
        bool visible = true;
        float opacity = 1.f;
        BlendMode blendMode = BlendMode::Normal;
        bool alphaRewrite = true;
        int parentGroupId = -1;
        bool groupExpanded = true;
        bool hasMask = false;
        std::vector<uint8_t> maskFlat;
        std::vector<MaskTileSnapshot> maskTiles;
        int maskW = 0, maskH = 0;
        FillLayerParams fill;
        std::vector<LayerFilter> filters;
        std::vector<LayerStyle> styles;
        std::string smartPath;
        std::vector<uint8_t> smartBytes;
        float smartScale = 1.f;
        // Vector geometry JSON (empty if not a vector layer)
        std::string vectorJson;
        texset::LayerWorkSpace workSpace{};
        // Content tiles (newState = content; old empty for insert capture)
        std::vector<TileDelta> tiles;
        // Native map storage
        bool hasNative = false;
        int nativeW = 0, nativeH = 0;
        texset::MapKind nativeKind = texset::MapKind::Diffuse;
        std::vector<TileDelta> nativeTiles;
    };

    LayerStackCommand(const std::string& name, Kind kind, Snap snap);
    std::string GetName() const override { return m_Name; }
    void Undo(Canvas* canvas) override;
    void Redo(Canvas* canvas) override;
    size_t GetOverheadBytes() const override;
    void CollectTileData(std::unordered_set<const TileData*>& seen) const override;

private:
    void InsertSnap(Canvas* canvas);
    void RemoveAt(Canvas* canvas, int index);
    std::string m_Name;
    Kind m_Kind = Kind::Insert;
    Snap m_Snap;
};

// Full document geometry change (crop / canvas edit): stores per-layer tile maps + size.
class DocumentGeometryCommand : public UndoCommand {
public:
    struct LayerTiles {
        int layerIdx = 0;
        bool hasMask = false;
        std::vector<uint8_t> mask;
        // Sparse tiles for this layer at the associated document size
        std::vector<TileDelta> tiles;
    };
    struct DocSnap {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> selection;
        bool hasSelection = false;
        std::vector<LayerTiles> layers;
    };

    DocumentGeometryCommand(const std::string& name, DocSnap oldSnap, DocSnap newSnap);
    std::string GetName() const override { return m_Name; }
    void Undo(Canvas* canvas) override;
    void Redo(Canvas* canvas) override;
    size_t GetOverheadBytes() const override;
    void CollectTileData(std::unordered_set<const TileData*>& seen) const override;

private:
    void Apply(Canvas* canvas, const DocSnap& snap);
    std::string m_Name;
    DocSnap m_Old;
    DocSnap m_New;
};

// Vector geometry edit (before/after JSON of vec::Document).
class VectorEditCommand : public UndoCommand {
public:
    VectorEditCommand(const std::string& name, int layerIdx,
                      std::string beforeJson, std::string afterJson);
    std::string GetName() const override { return m_Name; }
    void Undo(Canvas* canvas) override;
    void Redo(Canvas* canvas) override;
    size_t GetOverheadBytes() const override;

private:
    void Apply(Canvas* canvas, const std::string& json);
    std::string m_Name;
    int m_LayerIdx = -1;
    std::string m_Before;
    std::string m_After;
};

// Rasterize bake undo: restores layer type/filters/styles/fill + tile pixels.
// For groups: also restores deleted children (full float RGBA + meta).
class RasterizeCommand : public UndoCommand {
public:
    struct LayerMeta {
        std::string name;
        uint8_t type = 0; // Layer::Type as uint8
        bool isGroup = false;
        float opacity = 1.f;
        BlendMode blendMode = BlendMode::Normal;
        bool alphaRewrite = true;
        int parentGroupId = -1;
        bool groupExpanded = true;
        bool hasMask = false;
        std::vector<uint8_t> mask;
        std::string smartPath;
        std::vector<uint8_t> smartBytes;
        float smartScale = 1.f;
        FillLayerParams fill;
        std::vector<LayerFilter> filters;
        std::vector<LayerStyle> styles;
        // Full pixels for fill/children restore (W*H*4). Empty → tile deltas only.
        std::vector<float> pixels;
        bool pixelsValid = false;
        int insertAt = -1; // original index for reinsertion
    };

    // Single-layer rasterize
    RasterizeCommand(const std::string& name, int layerIdx,
                     LayerMeta oldMeta, LayerMeta newMeta,
                     std::vector<TileDelta> tileDeltas);

    // Group flatten
    RasterizeCommand(const std::string& name, int groupIdx,
                     LayerMeta oldGroupMeta,
                     std::vector<LayerMeta> removedChildren,
                     LayerMeta newMeta,
                     std::vector<TileDelta> tileDeltas);

    std::string GetName() const override { return m_Name; }
    void Undo(Canvas* canvas) override;
    void Redo(Canvas* canvas) override;
    size_t GetOverheadBytes() const override;
    void CollectTileData(std::unordered_set<const TileData*>& seen) const override;

private:
    void ApplyMetaToLayer(Canvas* canvas, int layerIdx, const LayerMeta& meta);
    void ApplyTiles(Canvas* canvas, int layerIdx, bool useNew);
    std::string m_Name;
    int m_LayerIdx = -1;
    bool m_IsGroup = false;
    LayerMeta m_OldMeta;
    LayerMeta m_NewMeta;
    std::vector<LayerMeta> m_RemovedChildren;
    std::vector<TileDelta> m_Tiles;
};

class UndoRedoManager {
public:
    UndoRedoManager();
    ~UndoRedoManager() = default;

    void PushCommand(std::shared_ptr<UndoCommand> command);
    bool Undo(Canvas* canvas);
    bool Redo(Canvas* canvas);

    void Clear();
    bool CanUndo() const;
    bool CanRedo() const;

    std::string GetUndoName() const;
    std::string GetRedoName() const;

    void  SetMemoryBudget(size_t bytes) { m_MemoryBudgetBytes = bytes; }
    size_t GetMemoryBudget() const      { return m_MemoryBudgetBytes; }

    // Global unique TileData accounting across undo + redo stacks.
    size_t GetCurrentMemoryUsage() const { return m_CurrentMemoryBytes; }
    size_t GetUniqueTileBlobCount() const { return m_UniqueTileBlobCount; }
    size_t GetOverheadBytes() const       { return m_OverheadBytes; }

    // Soft-memory ladder for dormant / pressure: drop history rather than crash.
    // soft: clear redo + enforce half budget. extreme: keep at most last undo step.
    // Returns number of steps removed.
    int TrimForPressure(bool extreme);

private:
    void EnforceLimits();
    void RecalcMemory();
    size_t EffectiveBudget() const;

    static constexpr size_t kDefaultMemoryBudget = 256ull * 1024 * 1024;

    std::vector<std::shared_ptr<UndoCommand>> m_UndoStack;
    std::vector<std::shared_ptr<UndoCommand>> m_RedoStack;
    size_t m_MemoryBudgetBytes  = kDefaultMemoryBudget;
    size_t m_CurrentMemoryBytes = 0;
    size_t m_UniqueTileBlobCount = 0;
    size_t m_OverheadBytes = 0;
};
