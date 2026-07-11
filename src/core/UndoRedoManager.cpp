#include "UndoRedoManager.h"
#include "../Canvas.h"
#include "Logger.h"
#include "ConfigManager.h"
#include "MemoryStats.h"
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// UndoCommand helpers
// ---------------------------------------------------------------------------

size_t UndoCommand::GetMemorySize() const {
    std::unordered_set<const TileData*> seen;
    CollectTileData(seen);
    size_t bytes = GetOverheadBytes();
    for (const TileData* td : seen) {
        if (td) bytes += td->ByteSize();
    }
    return bytes;
}

// ---------------------------------------------------------------------------
// PaintStrokeCommand
// ---------------------------------------------------------------------------

PaintStrokeCommand::PaintStrokeCommand(const std::string& name, int layerIdx,
                                       std::vector<TileDelta> deltas)
    : m_Name(name), m_LayerIdx(layerIdx), m_Deltas(std::move(deltas)) {}

void PaintStrokeCommand::Undo(Canvas* canvas) {
    auto& layers = canvas->GetLayers();
    if (m_LayerIdx < 0 || m_LayerIdx >= (int)layers.size()) return;
    auto& layer = layers[m_LayerIdx];
    if (!layer.tileCache) return;

    for (const auto& delta : m_Deltas) {
        layer.tileCache->RestoreTile(delta.tileX, delta.tileY, delta.oldState);
    }
    layer.needsUpload  = true;
    layer.filtersDirty = true;
    canvas->MarkCompositeDirty();
}

void PaintStrokeCommand::Redo(Canvas* canvas) {
    auto& layers = canvas->GetLayers();
    if (m_LayerIdx < 0 || m_LayerIdx >= (int)layers.size()) return;
    auto& layer = layers[m_LayerIdx];
    if (!layer.tileCache) return;

    for (const auto& delta : m_Deltas) {
        layer.tileCache->RestoreTile(delta.tileX, delta.tileY, delta.newState);
    }
    layer.needsUpload  = true;
    layer.filtersDirty = true;
    canvas->MarkCompositeDirty();
}

size_t PaintStrokeCommand::GetOverheadBytes() const {
    return sizeof(PaintStrokeCommand)
         + m_Name.capacity()
         + m_Deltas.capacity() * sizeof(TileDelta);
}

void PaintStrokeCommand::CollectTileData(std::unordered_set<const TileData*>& seen) const {
    auto add = [&](const TileSnapshot& s) {
        if (s.data) seen.insert(s.data.get());
    };
    for (const auto& d : m_Deltas) {
        add(d.oldState);
        add(d.newState);
    }
}

// ---------------------------------------------------------------------------
// SelectionCommand
// ---------------------------------------------------------------------------

SelectionCommand::SelectionCommand(const std::string& name,
                                   std::vector<uint8_t> oldMask, bool oldHasSelection,
                                   std::vector<uint8_t> newMask, bool newHasSelection)
    : m_Name(name)
    , m_OldMask(std::move(oldMask))
    , m_OldHasSelection(oldHasSelection)
    , m_NewMask(std::move(newMask))
    , m_NewHasSelection(newHasSelection) {}

void SelectionCommand::Undo(Canvas* canvas) {
    canvas->SetSelectionMask(m_OldMask);
}

void SelectionCommand::Redo(Canvas* canvas) {
    canvas->SetSelectionMask(m_NewMask);
}

size_t SelectionCommand::GetOverheadBytes() const {
    return sizeof(SelectionCommand) + m_Name.capacity()
         + m_OldMask.capacity() * sizeof(uint8_t)
         + m_NewMask.capacity() * sizeof(uint8_t);
}

// ---------------------------------------------------------------------------
// DocumentGeometryCommand
// ---------------------------------------------------------------------------

DocumentGeometryCommand::DocumentGeometryCommand(const std::string& name, DocSnap oldSnap, DocSnap newSnap)
    : m_Name(name), m_Old(std::move(oldSnap)), m_New(std::move(newSnap)) {}

void DocumentGeometryCommand::Apply(Canvas* canvas, const DocSnap& snap) {
    if (!canvas) return;
    canvas->m_Width = snap.width;
    canvas->m_Height = snap.height;
    canvas->m_SelectionMask = snap.selection;
    canvas->m_HasSelection = snap.hasSelection;
    canvas->m_SelectionMaskNeedsUpload = true;

    auto& layers = canvas->GetLayers();
    for (const auto& lt : snap.layers) {
        if (lt.layerIdx < 0 || lt.layerIdx >= (int)layers.size()) continue;
        auto& layer = layers[lt.layerIdx];
        if (layer.isGroup) continue;
        if (!layer.tileCache) {
            layer.tileCache = std::make_unique<TileCache>();
        }
        layer.tileCache->Clear();
        layer.tileCache->Init(snap.width, snap.height, canvas->m_CanvasFormat);
        for (const auto& d : lt.tiles) {
            layer.tileCache->RestoreTile(d.tileX, d.tileY, d.newState);
        }
        layer.hasMask = lt.hasMask;
        layer.mask = lt.mask;
        layer.maskNeedsUpload = true;
        layer.needsUpload = true;
        layer.filtersDirty = true;
        // Drop GPU textures so ComposeLayers recreates at new document size.
        if (layer.texture) { layer.texture->Release(); layer.texture = nullptr; }
        if (layer.srv) { layer.srv->Release(); layer.srv = nullptr; }
        if (layer.maskTexture) { layer.maskTexture->Release(); layer.maskTexture = nullptr; }
        if (layer.maskSRV) { layer.maskSRV->Release(); layer.maskSRV = nullptr; }
    }
    // Force composite rebuild at new size on next frame (device-less path).
    canvas->ReleaseCompositeResources();
    if (canvas->m_SelectionMaskTexture) {
        canvas->m_SelectionMaskTexture->Release();
        canvas->m_SelectionMaskTexture = nullptr;
    }
    if (canvas->m_SelectionMaskSRV) {
        canvas->m_SelectionMaskSRV->Release();
        canvas->m_SelectionMaskSRV = nullptr;
    }
    canvas->m_CompositeDirty = true;
    canvas->m_IsDocumentModified = true;
}

void DocumentGeometryCommand::Undo(Canvas* canvas) { Apply(canvas, m_Old); }
void DocumentGeometryCommand::Redo(Canvas* canvas) { Apply(canvas, m_New); }

size_t DocumentGeometryCommand::GetOverheadBytes() const {
    size_t n = sizeof(DocumentGeometryCommand) + m_Name.capacity()
             + m_Old.selection.capacity() + m_New.selection.capacity();
    auto addLayers = [&](const DocSnap& s) {
        for (const auto& lt : s.layers) {
            n += lt.mask.capacity() + lt.tiles.capacity() * sizeof(TileDelta);
        }
    };
    addLayers(m_Old);
    addLayers(m_New);
    return n;
}

void DocumentGeometryCommand::CollectTileData(std::unordered_set<const TileData*>& seen) const {
    auto walk = [&](const DocSnap& s) {
        for (const auto& lt : s.layers) {
            for (const auto& d : lt.tiles) {
                if (d.oldState.data) seen.insert(d.oldState.data.get());
                if (d.newState.data) seen.insert(d.newState.data.get());
            }
        }
    };
    walk(m_Old);
    walk(m_New);
}

// ---------------------------------------------------------------------------
// UndoRedoManager — global unique TileData budget
// ---------------------------------------------------------------------------

UndoRedoManager::UndoRedoManager() {}

size_t UndoRedoManager::EffectiveBudget() const {
    // User-facing config is primary. Instance SetMemoryBudget overrides when
    // called explicitly with a non-default path: if config is 0, treat as
    // unlimited only when instance budget is also 0.
    const int configMB = ConfigManager::Get().GetMaxUndoMemoryMB();
    if (configMB > 0) {
        return (size_t)configMB * 1024ull * 1024ull;
    }
    // config 0 = "use code default / instance"
    return m_MemoryBudgetBytes; // default 256 MiB; SetMemoryBudget(0) => unlimited
}

void UndoRedoManager::RecalcMemory() {
    std::unordered_set<const TileData*> seen;
    m_OverheadBytes = 0;

    auto walk = [&](const std::vector<std::shared_ptr<UndoCommand>>& stack) {
        for (const auto& cmd : stack) {
            if (!cmd) continue;
            m_OverheadBytes += cmd->GetOverheadBytes();
            cmd->CollectTileData(seen);
        }
    };

    walk(m_UndoStack);
    walk(m_RedoStack);

    size_t tileBytes = 0;
    for (const TileData* td : seen) {
        if (td) tileBytes += td->ByteSize();
    }

    m_UniqueTileBlobCount = seen.size();
    m_CurrentMemoryBytes  = m_OverheadBytes + tileBytes;
}

void UndoRedoManager::PushCommand(std::shared_ptr<UndoCommand> command) {
    m_UndoStack.push_back(std::move(command));
    // New branch invalidates redo; shared TileData refcounts drop for orphaned history.
    m_RedoStack.clear();
    EnforceLimits();
}

bool UndoRedoManager::Undo(Canvas* canvas) {
    if (m_UndoStack.empty()) return false;
    auto cmd = m_UndoStack.back();
    m_UndoStack.pop_back();
    cmd->Undo(canvas);
    m_RedoStack.push_back(std::move(cmd));
    // Moving a command undo→redo does not free TileData (still referenced).
    RecalcMemory();
    return true;
}

bool UndoRedoManager::Redo(Canvas* canvas) {
    if (m_RedoStack.empty()) return false;
    auto cmd = m_RedoStack.back();
    m_RedoStack.pop_back();
    cmd->Redo(canvas);
    m_UndoStack.push_back(std::move(cmd));
    RecalcMemory();
    return true;
}

void UndoRedoManager::Clear() {
    m_UndoStack.clear();
    m_RedoStack.clear();
    m_CurrentMemoryBytes = 0;
    m_UniqueTileBlobCount = 0;
    m_OverheadBytes = 0;
}

bool UndoRedoManager::CanUndo() const { return !m_UndoStack.empty(); }
bool UndoRedoManager::CanRedo() const { return !m_RedoStack.empty(); }

std::string UndoRedoManager::GetUndoName() const {
    return m_UndoStack.empty() ? "" : m_UndoStack.back()->GetName();
}

std::string UndoRedoManager::GetRedoName() const {
    return m_RedoStack.empty() ? "" : m_RedoStack.back()->GetName();
}

void UndoRedoManager::EnforceLimits() {
    const int maxSteps = ConfigManager::Get().GetMaxUndoSteps();
    int droppedSteps = 0;

    // 1) Step cap — oldest undo first (front of stack).
    while (maxSteps > 0 && (int)m_UndoStack.size() > maxSteps) {
        m_UndoStack.erase(m_UndoStack.begin());
        ++droppedSteps;
    }

    // 2) Global unique TileData budget across undo + redo.
    const size_t budget = EffectiveBudget();
    RecalcMemory();

    if (budget > 0) {
        // Drop oldest undo first (Krita-like purge of deep history).
        while (m_CurrentMemoryBytes > budget && !m_UndoStack.empty()) {
            m_UndoStack.erase(m_UndoStack.begin());
            ++droppedSteps;
            RecalcMemory();
        }
        // If still over (e.g. huge redo after many undos), drop oldest redo.
        while (m_CurrentMemoryBytes > budget && !m_RedoStack.empty()) {
            m_RedoStack.erase(m_RedoStack.begin());
            ++droppedSteps;
            RecalcMemory();
        }
    }

    if (droppedSteps > 0) {
        Logger::Get().InfoTag("mem",
            "Undo history trimmed: dropped " + std::to_string(droppedSteps) +
            " step(s) | unique blobs=" + std::to_string(m_UniqueTileBlobCount) +
            " tiles=" + MemoryStats::FormatBytes(m_CurrentMemoryBytes - m_OverheadBytes) +
            " overhead=" + MemoryStats::FormatBytes(m_OverheadBytes) +
            " total=" + MemoryStats::FormatBytes(m_CurrentMemoryBytes) +
            (budget ? " budget=" + MemoryStats::FormatBytes(budget) : " budget=unlimited"));
    }
}

// ---------------------------------------------------------------------------
// RasterizeCommand
// ---------------------------------------------------------------------------

RasterizeCommand::RasterizeCommand(const std::string& name, int layerIdx,
                                   LayerMeta oldMeta, LayerMeta newMeta,
                                   std::vector<TileDelta> tileDeltas)
    : m_Name(name)
    , m_LayerIdx(layerIdx)
    , m_IsGroup(false)
    , m_OldMeta(std::move(oldMeta))
    , m_NewMeta(std::move(newMeta))
    , m_Tiles(std::move(tileDeltas)) {}

RasterizeCommand::RasterizeCommand(const std::string& name, int groupIdx,
                                   LayerMeta oldGroupMeta,
                                   std::vector<LayerMeta> removedChildren,
                                   LayerMeta newMeta,
                                   std::vector<TileDelta> tileDeltas)
    : m_Name(name)
    , m_LayerIdx(groupIdx)
    , m_IsGroup(true)
    , m_OldMeta(std::move(oldGroupMeta))
    , m_NewMeta(std::move(newMeta))
    , m_RemovedChildren(std::move(removedChildren))
    , m_Tiles(std::move(tileDeltas)) {}

void RasterizeCommand::ApplyMetaToLayer(Canvas* canvas, int layerIdx, const LayerMeta& meta) {
    if (!canvas || layerIdx < 0 || layerIdx >= (int)canvas->m_Layers.size()) return;
    auto& L = canvas->m_Layers[layerIdx];
    L.name = meta.name;
    L.type = static_cast<Layer::Type>(meta.type);
    L.isGroup = meta.isGroup;
    L.opacity = meta.opacity;
    L.blendMode = meta.blendMode;
    L.alphaRewrite = meta.alphaRewrite;
    L.parentGroupId = meta.parentGroupId;
    L.groupExpanded = meta.groupExpanded;
    L.hasMask = meta.hasMask;
    L.mask = meta.mask;
    L.maskNeedsUpload = meta.hasMask;
    L.smartSourcePath = meta.smartPath;
    L.smartSourceBytes = meta.smartBytes;
    L.smartScale = meta.smartScale;
    L.fill = meta.fill;
    L.filters = meta.filters;
    L.styles = meta.styles;
    L.filtersDirty = !L.filters.empty();
    L.stylesDirty = !L.styles.empty();
    L.presentationDirty = true;
    L.filteredCache.reset();
    L.presentationCache.reset();
    if (meta.pixelsValid && !meta.pixels.empty()) {
        if (!L.tileCache) {
            L.tileCache = std::make_unique<TileCache>();
            L.tileCache->Init(canvas->m_Width, canvas->m_Height, canvas->m_CanvasFormat);
        }
        L.tileCache->ImportRGBA32F(meta.pixels.data(), canvas->m_Width, canvas->m_Height);
        L.tileCache->MarkAllDirty();
    } else if (L.isGroup || L.IsFill()) {
        if (!meta.pixelsValid)
            L.tileCache.reset();
    }
    L.needsUpload = true;
    L.thumbDirty = true;
}

void RasterizeCommand::ApplyTiles(Canvas* canvas, int layerIdx, bool useNew) {
    if (!canvas || layerIdx < 0 || layerIdx >= (int)canvas->m_Layers.size()) return;
    auto& L = canvas->m_Layers[layerIdx];
    if (!L.tileCache) {
        L.tileCache = std::make_unique<TileCache>();
        L.tileCache->Init(canvas->m_Width, canvas->m_Height, canvas->m_CanvasFormat);
    }
    for (const auto& d : m_Tiles) {
        L.tileCache->RestoreTile(d.tileX, d.tileY, useNew ? d.newState : d.oldState);
    }
    L.needsUpload = true;
    L.filtersDirty = true;
    L.presentationDirty = true;
}

void RasterizeCommand::Undo(Canvas* canvas) {
    if (!canvas) return;
    if (m_IsGroup) {
        // Current state is flattened raster at m_LayerIdx � restore group header + children
        if (m_LayerIdx < 0 || m_LayerIdx >= (int)canvas->m_Layers.size()) return;
        ApplyMetaToLayer(canvas, m_LayerIdx, m_OldMeta);
        // Clear raster tiles on group header
        if (canvas->m_Layers[m_LayerIdx].tileCache)
            canvas->m_Layers[m_LayerIdx].tileCache->Clear();
        canvas->m_Layers[m_LayerIdx].tileCache.reset();
        canvas->m_Layers[m_LayerIdx].isGroup = true;
        canvas->m_Layers[m_LayerIdx].type = Layer::Type::Group;
        // Reinsert children high indices first so insertAt stays valid if sorted ascending
        std::vector<LayerMeta> kids = m_RemovedChildren;
        std::sort(kids.begin(), kids.end(), [](const LayerMeta& a, const LayerMeta& b) {
            return a.insertAt < b.insertAt;
        });
        for (const auto& ch : kids) {
            int at = std::clamp(ch.insertAt, 0, (int)canvas->m_Layers.size());
            Layer L;
            L.name = ch.name;
            L.type = static_cast<Layer::Type>(ch.type);
            L.isGroup = ch.isGroup;
            L.opacity = ch.opacity;
            L.blendMode = ch.blendMode;
            L.alphaRewrite = ch.alphaRewrite;
            L.parentGroupId = m_LayerIdx; // reparent to restored group
            L.groupExpanded = ch.groupExpanded;
            L.hasMask = ch.hasMask;
            L.mask = ch.mask;
            L.fill = ch.fill;
            L.filters = ch.filters;
            L.styles = ch.styles;
            L.smartSourcePath = ch.smartPath;
            L.smartSourceBytes = ch.smartBytes;
            L.smartScale = ch.smartScale;
            L.filtersDirty = !L.filters.empty();
            L.stylesDirty = !L.styles.empty();
            L.presentationDirty = true;
            if (ch.pixelsValid && !ch.pixels.empty()) {
                L.tileCache = std::make_unique<TileCache>();
                L.tileCache->Init(canvas->m_Width, canvas->m_Height, canvas->m_CanvasFormat);
                L.tileCache->ImportRGBA32F(ch.pixels.data(), canvas->m_Width, canvas->m_Height);
                L.tileCache->MarkAllDirty();
                L.needsUpload = true;
            }
            // Remap parent ids after insert
            for (auto& existing : canvas->m_Layers) {
                if (existing.parentGroupId >= at)
                    existing.parentGroupId++;
            }
            canvas->m_Layers.insert(canvas->m_Layers.begin() + at, std::move(L));
            if (m_LayerIdx >= at) m_LayerIdx++; // group index shifts
        }
        // Fix children parentGroupId to actual group index
        for (auto& L : canvas->m_Layers) {
            // already set to m_LayerIdx during insert � but m_LayerIdx may have shifted
        }
        // Re-find group by name
        for (int i = 0; i < (int)canvas->m_Layers.size(); ++i) {
            if (canvas->m_Layers[i].isGroup && canvas->m_Layers[i].name == m_OldMeta.name) {
                m_LayerIdx = i;
                break;
            }
        }
        for (auto& L : canvas->m_Layers) {
            if (!L.isGroup && L.parentGroupId >= 0) {
                // leave as set
            }
        }
        // Set parent of restored children to group
        for (int i = 0; i < (int)canvas->m_Layers.size(); ++i) {
            auto& L = canvas->m_Layers[i];
            if (L.isGroup) continue;
            // Children we just inserted have parentGroupId = previous m_LayerIdx during insert
            // Force: any layer that was in m_RemovedChildren by name
            for (const auto& ch : m_RemovedChildren) {
                if (L.name == ch.name) {
                    L.parentGroupId = m_LayerIdx;
                    break;
                }
            }
        }
        canvas->m_ActiveLayerIdx = m_LayerIdx;
    } else {
        ApplyMetaToLayer(canvas, m_LayerIdx, m_OldMeta);
        if (!m_OldMeta.pixelsValid)
            ApplyTiles(canvas, m_LayerIdx, /*useNew=*/false);
    }
    if (canvas->m_Layers[m_LayerIdx].texture) {
        canvas->m_Layers[m_LayerIdx].texture->Release();
        canvas->m_Layers[m_LayerIdx].texture = nullptr;
    }
    if (canvas->m_Layers[m_LayerIdx].srv) {
        canvas->m_Layers[m_LayerIdx].srv->Release();
        canvas->m_Layers[m_LayerIdx].srv = nullptr;
    }
    canvas->MarkCompositeDirty();
    canvas->m_IsDocumentModified = true;
}

void RasterizeCommand::Redo(Canvas* canvas) {
    if (!canvas) return;
    if (m_IsGroup) {
        // Delete children again and re-apply flatten
        // Find group
        int gIdx = -1;
        for (int i = 0; i < (int)canvas->m_Layers.size(); ++i) {
            if (canvas->m_Layers[i].isGroup && canvas->m_Layers[i].name == m_OldMeta.name) {
                gIdx = i; break;
            }
        }
        if (gIdx < 0) gIdx = m_LayerIdx;
        // Delete children under group high>low
        for (int i = (int)canvas->m_Layers.size() - 1; i >= 0; --i) {
            if (i == gIdx) continue;
            int p = canvas->m_Layers[i].parentGroupId;
            bool under = false;
            while (p >= 0 && p < (int)canvas->m_Layers.size()) {
                if (p == gIdx) { under = true; break; }
                p = canvas->m_Layers[p].parentGroupId;
            }
            if (under) {
                canvas->DeleteLayer(i);
                if (i < gIdx) gIdx--;
            }
        }
        m_LayerIdx = gIdx;
        ApplyMetaToLayer(canvas, m_LayerIdx, m_NewMeta);
        ApplyTiles(canvas, m_LayerIdx, /*useNew=*/true);
        canvas->m_Layers[m_LayerIdx].isGroup = false;
        canvas->m_Layers[m_LayerIdx].type = Layer::Type::Raster;
    } else {
        ApplyMetaToLayer(canvas, m_LayerIdx, m_NewMeta);
        ApplyTiles(canvas, m_LayerIdx, /*useNew=*/true);
    }
    if (canvas->m_Layers[m_LayerIdx].texture) {
        canvas->m_Layers[m_LayerIdx].texture->Release();
        canvas->m_Layers[m_LayerIdx].texture = nullptr;
    }
    if (canvas->m_Layers[m_LayerIdx].srv) {
        canvas->m_Layers[m_LayerIdx].srv->Release();
        canvas->m_Layers[m_LayerIdx].srv = nullptr;
    }
    canvas->MarkCompositeDirty();
    canvas->m_IsDocumentModified = true;
}

size_t RasterizeCommand::GetOverheadBytes() const {
    size_t n = sizeof(RasterizeCommand) + m_Name.capacity() + m_Tiles.capacity() * sizeof(TileDelta);
    auto addMeta = [&](const LayerMeta& m) {
        n += m.name.capacity() + m.mask.capacity() + m.smartBytes.capacity()
           + m.pixels.capacity() * sizeof(float) + m.filters.capacity() * 64
           + m.styles.capacity() * 128;
    };
    addMeta(m_OldMeta);
    addMeta(m_NewMeta);
    for (const auto& c : m_RemovedChildren) addMeta(c);
    return n;
}

void RasterizeCommand::CollectTileData(std::unordered_set<const TileData*>& seen) const {
    for (const auto& d : m_Tiles) {
        if (d.oldState.data) seen.insert(d.oldState.data.get());
        if (d.newState.data) seen.insert(d.newState.data.get());
    }
}
