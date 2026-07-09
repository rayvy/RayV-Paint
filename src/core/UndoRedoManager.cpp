#include "UndoRedoManager.h"
#include "../Canvas.h"
#include "Logger.h"
#include "ConfigManager.h"
#include "TileCache.h"

// ---------------------------------------------------------------------------
// PaintStrokeCommand — now restores tiles via TileCache::RestoreTile
// ---------------------------------------------------------------------------

PaintStrokeCommand::PaintStrokeCommand(const std::string& name, int layerIdx,
                                       std::vector<TileDelta> deltas)
    : m_Name(name), m_LayerIdx(layerIdx), m_Deltas(std::move(deltas)) {}

void PaintStrokeCommand::Undo(Canvas* canvas) {
    auto& layers = canvas->GetLayers();
    if (m_LayerIdx < 0 || m_LayerIdx >= (int)layers.size()) return;
    auto& layer = layers[m_LayerIdx];
    if (!layer.tileCache) return;

    // Restore only touched tiles (Krita-style memento granularity).
    // RestoreTile marks dirty or queues a GPU clear for erased tiles.
    for (const auto& delta : m_Deltas) {
        layer.tileCache->RestoreTile(delta.tileX, delta.tileY, delta.oldPixels);
    }
    layer.needsUpload  = true;
    layer.filtersDirty = true; // filteredCache is stale after tile restore
    canvas->MarkCompositeDirty();
}

void PaintStrokeCommand::Redo(Canvas* canvas) {
    auto& layers = canvas->GetLayers();
    if (m_LayerIdx < 0 || m_LayerIdx >= (int)layers.size()) return;
    auto& layer = layers[m_LayerIdx];
    if (!layer.tileCache) return;

    for (const auto& delta : m_Deltas) {
        layer.tileCache->RestoreTile(delta.tileX, delta.tileY, delta.newPixels);
    }
    layer.needsUpload  = true;
    layer.filtersDirty = true;
    canvas->MarkCompositeDirty();
}

size_t PaintStrokeCommand::GetMemorySize() const {
    size_t sz = sizeof(PaintStrokeCommand) + m_Name.capacity();
    for (const auto& d : m_Deltas) {
        sz += sizeof(TileDelta)
            + d.oldPixels.capacity()
            + d.newPixels.capacity(); // uint8_t — 1 byte each
    }
    return sz;
}

// ---------------------------------------------------------------------------
// SelectionCommand — mask is now vector<uint8_t>
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

void UndoRedoManager::PushCommand(std::shared_ptr<UndoCommand> command) {
    m_CurrentMemoryBytes += command->GetMemorySize();
    m_UndoStack.push_back(std::move(command));
    m_RedoStack.clear();
    EnforceLimits();
}

bool UndoRedoManager::Undo(Canvas* canvas) {
    if (m_UndoStack.empty()) return false;
    auto cmd = m_UndoStack.back();
    m_UndoStack.pop_back();
    cmd->Undo(canvas);
    m_RedoStack.push_back(cmd);
    return true;
}

bool UndoRedoManager::Redo(Canvas* canvas) {
    if (m_RedoStack.empty()) return false;
    auto cmd = m_RedoStack.back();
    m_RedoStack.pop_back();
    cmd->Redo(canvas);
    m_UndoStack.push_back(cmd);
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
    // Step limit from config
    int maxSteps = ConfigManager::Get().GetMaxUndoSteps();
    while ((int)m_UndoStack.size() > maxSteps && !m_UndoStack.empty()) {
        m_CurrentMemoryBytes -= m_UndoStack.front()->GetMemorySize();
        m_UndoStack.erase(m_UndoStack.begin());
    }

    // Memory budget: use instance budget (set via SetMemoryBudget) OR config value
    size_t configBudget = (size_t)ConfigManager::Get().GetMaxUndoMemoryMB() * 1024 * 1024;
    size_t budget = (m_MemoryBudgetBytes > 0) ? m_MemoryBudgetBytes : configBudget;

    // Recompute total (keeps m_CurrentMemoryBytes accurate)
    m_CurrentMemoryBytes = 0;
    for (const auto& cmd : m_UndoStack)  m_CurrentMemoryBytes += cmd->GetMemorySize();
    for (const auto& cmd : m_RedoStack)  m_CurrentMemoryBytes += cmd->GetMemorySize();

    while (m_CurrentMemoryBytes > budget && !m_UndoStack.empty()) {
        m_CurrentMemoryBytes -= m_UndoStack.front()->GetMemorySize();
        m_UndoStack.erase(m_UndoStack.begin());
    }
}
