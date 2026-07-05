#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

struct TileDelta {
    int layerIdx;
    int tileX;
    int tileY;
    std::vector<float> oldPixels; // 256 * 256 * 4 floats
    std::vector<float> newPixels;
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
                     std::vector<float> oldMask, bool oldHasSelection,
                     std::vector<float> newMask, bool newHasSelection);
    std::string GetName() const override { return m_Name; }
    void Undo(Canvas* canvas) override;
    void Redo(Canvas* canvas) override;
    size_t GetMemorySize() const override;

private:
    std::string m_Name;
    std::vector<float> m_OldMask;
    bool m_OldHasSelection;
    std::vector<float> m_NewMask;
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

private:
    void EnforceLimits();

    std::vector<std::shared_ptr<UndoCommand>> m_UndoStack;
    std::vector<std::shared_ptr<UndoCommand>> m_RedoStack;
};
