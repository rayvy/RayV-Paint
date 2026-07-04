#include "UndoRedoManager.h"
#include "../Canvas.h"
#include "Logger.h"
#include "ConfigManager.h"

PaintStrokeCommand::PaintStrokeCommand(const std::string& name, int layerIdx, std::vector<TileDelta> deltas)
    : m_Name(name), m_LayerIdx(layerIdx), m_Deltas(std::move(deltas)) {}

void PaintStrokeCommand::Undo(Canvas* canvas) {
    auto& layers = canvas->GetLayers();
    if (m_LayerIdx < 0 || m_LayerIdx >= static_cast<int>(layers.size())) return;
    auto& layer = layers[m_LayerIdx];
    int canvasWidth = canvas->GetWidth();
    int canvasHeight = canvas->GetHeight();

    for (const auto& delta : m_Deltas) {
        int startX = delta.tileX * 256;
        int startY = delta.tileY * 256;
        
        for (int y = 0; y < 256; ++y) {
            int canvasY = startY + y;
            if (canvasY >= canvasHeight) break;
            
            for (int x = 0; x < 256; ++x) {
                int canvasX = startX + x;
                if (canvasX >= canvasWidth) break;
                
                int tileOffset = (y * 256 + x) * 4;
                int canvasOffset = (canvasY * canvasWidth + canvasX) * 4;
                
                layer.pixels[canvasOffset + 0] = delta.oldPixels[tileOffset + 0];
                layer.pixels[canvasOffset + 1] = delta.oldPixels[tileOffset + 1];
                layer.pixels[canvasOffset + 2] = delta.oldPixels[tileOffset + 2];
                layer.pixels[canvasOffset + 3] = delta.oldPixels[tileOffset + 3];
            }
        }
    }
    layer.needsUpload = true;
}

void PaintStrokeCommand::Redo(Canvas* canvas) {
    auto& layers = canvas->GetLayers();
    if (m_LayerIdx < 0 || m_LayerIdx >= static_cast<int>(layers.size())) return;
    auto& layer = layers[m_LayerIdx];
    int canvasWidth = canvas->GetWidth();
    int canvasHeight = canvas->GetHeight();

    for (const auto& delta : m_Deltas) {
        int startX = delta.tileX * 256;
        int startY = delta.tileY * 256;
        
        for (int y = 0; y < 256; ++y) {
            int canvasY = startY + y;
            if (canvasY >= canvasHeight) break;
            
            for (int x = 0; x < 256; ++x) {
                int canvasX = startX + x;
                if (canvasX >= canvasWidth) break;
                
                int tileOffset = (y * 256 + x) * 4;
                int canvasOffset = (canvasY * canvasWidth + canvasX) * 4;
                
                layer.pixels[canvasOffset + 0] = delta.newPixels[tileOffset + 0];
                layer.pixels[canvasOffset + 1] = delta.newPixels[tileOffset + 1];
                layer.pixels[canvasOffset + 2] = delta.newPixels[tileOffset + 2];
                layer.pixels[canvasOffset + 3] = delta.newPixels[tileOffset + 3];
            }
        }
    }
    layer.needsUpload = true;
}

size_t PaintStrokeCommand::GetMemorySize() const {
    size_t size = sizeof(PaintStrokeCommand) + m_Name.capacity();
    for (const auto& delta : m_Deltas) {
        size += sizeof(TileDelta) + 
                delta.oldPixels.capacity() * sizeof(float) + 
                delta.newPixels.capacity() * sizeof(float);
    }
    return size;
}

UndoRedoManager::UndoRedoManager() {}

void UndoRedoManager::PushCommand(std::shared_ptr<UndoCommand> command) {
    m_UndoStack.push_back(command);
    m_RedoStack.clear();
    EnforceLimits();
}

bool UndoRedoManager::Undo(Canvas* canvas) {
    if (m_UndoStack.empty()) return false;
    auto cmd = m_UndoStack.back();
    m_UndoStack.pop_back();
    cmd->Undo(canvas);
    m_RedoStack.push_back(cmd);
    EnforceLimits();
    return true;
}

bool UndoRedoManager::Redo(Canvas* canvas) {
    if (m_RedoStack.empty()) return false;
    auto cmd = m_RedoStack.back();
    m_RedoStack.pop_back();
    cmd->Redo(canvas);
    m_UndoStack.push_back(cmd);
    EnforceLimits();
    return true;
}

void UndoRedoManager::Clear() {
    m_UndoStack.clear();
    m_RedoStack.clear();
}

bool UndoRedoManager::CanUndo() const {
    return !m_UndoStack.empty();
}

bool UndoRedoManager::CanRedo() const {
    return !m_RedoStack.empty();
}

std::string UndoRedoManager::GetUndoName() const {
    if (m_UndoStack.empty()) return "";
    return m_UndoStack.back()->GetName();
}

std::string UndoRedoManager::GetRedoName() const {
    if (m_RedoStack.empty()) return "";
    return m_RedoStack.back()->GetName();
}

void UndoRedoManager::EnforceLimits() {
    int maxSteps = ConfigManager::Get().GetMaxUndoSteps();
    size_t maxMemoryBytes = static_cast<size_t>(ConfigManager::Get().GetMaxUndoMemoryMB()) * 1024 * 1024;

    while (static_cast<int>(m_UndoStack.size()) > maxSteps) {
        m_UndoStack.erase(m_UndoStack.begin());
    }

    while (true) {
        size_t totalBytes = 0;
        for (const auto& cmd : m_UndoStack) {
            totalBytes += cmd->GetMemorySize();
        }
        for (const auto& cmd : m_RedoStack) {
            totalBytes += cmd->GetMemorySize();
        }

        if (totalBytes <= maxMemoryBytes || m_UndoStack.empty()) {
            break;
        }

        m_UndoStack.erase(m_UndoStack.begin());
    }
}
