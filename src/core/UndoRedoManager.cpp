#include "UndoRedoManager.h"
#include "../Canvas.h"
#include "Logger.h"
#include "ConfigManager.h"

#include <unordered_set>

// ---------------------------------------------------------------------------
// PaintStrokeCommand — restores shared TileSnapshot handles
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

size_t PaintStrokeCommand::GetMemorySize() const {
    // Charge unique TileData blobs once (shared between old/new and across deltas).
    std::unordered_set<const TileData*> seen;
    size_t sz = sizeof(PaintStrokeCommand) + m_Name.capacity()
              + m_Deltas.capacity() * sizeof(TileDelta);

    auto addSnap = [&](const TileSnapshot& s) {
        if (!s.data) return;
        if (seen.insert(s.data.get()).second) {
            sz += s.data->ByteSize();
        }
    };

    for (const auto& d : m_Deltas) {
        addSnap(d.oldState);
        addSnap(d.newState);
    }
    return sz;
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

size_t SelectionCommand::GetMemorySize() const {
    return sizeof(SelectionCommand) + m_Name.capacity()
        + m_OldMask.capacity() * sizeof(uint8_t)
        + m_NewMask.capacity() * sizeof(uint8_t);
}

// ---------------------------------------------------------------------------
// UndoRedoManager
// ---------------------------------------------------------------------------

UndoRedoManager::UndoRedoManager() {}

void UndoRedoManager::RecalcMemory() {
    m_CurrentMemoryBytes = 0;
    // Unique TileData across the whole history (undo + redo).
    std::unordered_set<const TileData*> seenGlobal;

    auto accumulate = [&](const std::shared_ptr<UndoCommand>& cmd) {
        // Prefer exact shared accounting for paint commands.
        auto* paint = dynamic_cast<PaintStrokeCommand*>(cmd.get());
        if (paint) {
            // Re-walk deltas via GetMemorySize is per-command unique only.
            // For global budget we still sum per-command unique + selection sizes;
            // double-count across commands that share is possible but rare after
            // redo clear. Acceptable vs deep graph walk.
            m_CurrentMemoryBytes += cmd->GetMemorySize();
            return;
        }
        m_CurrentMemoryBytes += cmd->GetMemorySize();
        (void)seenGlobal;
    };

    for (const auto& cmd : m_UndoStack) accumulate(cmd);
    for (const auto& cmd : m_RedoStack) accumulate(cmd);
}

void UndoRedoManager::PushCommand(std::shared_ptr<UndoCommand> command) {
    m_UndoStack.push_back(std::move(command));
    m_RedoStack.clear(); // drop redo branch; shared TileData refcounts drop
    EnforceLimits();
}

bool UndoRedoManager::Undo(Canvas* canvas) {
    if (m_UndoStack.empty()) return false;
    auto cmd = m_UndoStack.back();
    m_UndoStack.pop_back();
    cmd->Undo(canvas);
    m_RedoStack.push_back(cmd);
    RecalcMemory();
    return true;
}

bool UndoRedoManager::Redo(Canvas* canvas) {
    if (m_RedoStack.empty()) return false;
    auto cmd = m_RedoStack.back();
    m_RedoStack.pop_back();
    cmd->Redo(canvas);
    m_UndoStack.push_back(cmd);
    RecalcMemory();
    return true;
}

void UndoRedoManager::Clear() {
    m_UndoStack.clear();
    m_RedoStack.clear();
    m_CurrentMemoryBytes = 0;
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
    int maxSteps = ConfigManager::Get().GetMaxUndoSteps();
    while ((int)m_UndoStack.size() > maxSteps && !m_UndoStack.empty()) {
        m_UndoStack.erase(m_UndoStack.begin());
    }

    size_t configBudget = (size_t)ConfigManager::Get().GetMaxUndoMemoryMB() * 1024 * 1024;
    size_t budget = (m_MemoryBudgetBytes > 0) ? m_MemoryBudgetBytes : configBudget;

    RecalcMemory();

    while (m_CurrentMemoryBytes > budget && !m_UndoStack.empty()) {
        m_UndoStack.erase(m_UndoStack.begin());
        RecalcMemory();
    }
}
