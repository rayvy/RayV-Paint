#include "UndoRedoManager.h"
#include "../Canvas.h"
#include "Logger.h"
#include "ConfigManager.h"
#include "MemoryStats.h"

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
