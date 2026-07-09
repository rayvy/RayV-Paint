#pragma once

#include "TileCache.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

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
    virtual size_t GetMemorySize() const = 0;
};

class PaintStrokeCommand : public UndoCommand {
public:
    PaintStrokeCommand(const std::string& name, int layerIdx, std::vector<TileDelta> deltas);
    std::string GetName() const override { return m_Name; }
    void Undo(Canvas* canvas) override;
    void Redo(Canvas* canvas) override;
    size_t GetMemorySize() const override;

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
    size_t GetMemorySize() const override;

private:
    std::string m_Name;
    std::vector<uint8_t> m_OldMask;
    bool m_OldHasSelection;
    std::vector<uint8_t> m_NewMask;
    bool m_NewHasSelection;
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
    size_t GetCurrentMemoryUsage() const{ return m_CurrentMemoryBytes; }

private:
    void EnforceLimits();
    void RecalcMemory();

    static constexpr size_t kDefaultMemoryBudget = 256ull * 1024 * 1024;

    std::vector<std::shared_ptr<UndoCommand>> m_UndoStack;
    std::vector<std::shared_ptr<UndoCommand>> m_RedoStack;
    size_t m_MemoryBudgetBytes  = kDefaultMemoryBudget;
    size_t m_CurrentMemoryBytes = 0;
};
