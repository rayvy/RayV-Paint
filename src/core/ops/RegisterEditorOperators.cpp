#include "OperatorHost.h"
#include "OperatorRegistry.h"
#include "../../Canvas.h"
#include "../../core/Logger.h"
#include "../../core/ProjectManager.h"
#include "../../core/ClipboardHelper.h"
#include <utility>

namespace core::ops {

static OperatorHost g_Host;

static OperatorResult Ok() { return OperatorResult::Finished; }

void BindOperatorHostFrame(Canvas* canvas, ID3D11Device* device) {
    g_Host.canvas = canvas;
    g_Host.device = device;
}

void RegisterEditorOperators(OperatorHost host) {
    g_Host = host;
    auto& R = OperatorRegistry::Get();
    R.Clear();

    auto H = []() -> OperatorHost& { return g_Host; };

    // --- Edit ---
    R.Register("Undo", [H] {
        if (H().canvas) H().canvas->Undo();
        return Ok();
    });
    R.Register("Redo", [H] {
        if (H().canvas) H().canvas->Redo();
        return Ok();
    });
    R.Register("FillSecondary", [H] {
        if (!H().canvas || !H().secondaryColor) return OperatorResult::Cancelled;
        if (H().canvas->HasSelection() || H().canvas->GetActiveLayerIndex() >= 0)
            H().canvas->FillSelection(H().secondaryColor);
        return Ok();
    });
    R.Register("DeleteContent", [H] {
        if (H().canvas) H().canvas->DeleteSelectionContent();
        return Ok();
    });
    R.Register("Copy", [H] {
        if (!H().canvas) return OperatorResult::Cancelled;
        if (!H().canvas->CopyContentToClipboard()) {
            std::vector<float> composite = H().canvas->GetCompositePixels();
            ClipboardHelper::CopyImageToClipboard(composite, H().canvas->GetWidth(), H().canvas->GetHeight());
        }
        return Ok();
    });
    R.Register("CopyLayers", [H] {
        if (!H().canvas || !H().ui) return OperatorResult::Cancelled;
        std::vector<int> idxs = H().ui->selectedLayers;
        if (idxs.empty() && H().canvas->GetActiveLayerIndex() >= 0)
            idxs.push_back(H().canvas->GetActiveLayerIndex());
        H().canvas->CopyLayersToClipboard(idxs);
        return Ok();
    });
    R.Register("PasteAsNewLayer", [H] {
        if (!H().canvas || !H().device) return OperatorResult::Cancelled;
        if (!H().canvas->PasteContentAsNewLayer(H().device, "Pasted Layer"))
            Logger::Get().Warn("PasteAsNewLayer: no image on clipboard");
        return Ok();
    });
    // Paste policy: external system image wins when newer than last internal copy;
    // else layer clipboard; else paste content into active (or new layer fallback).
    R.Register("Paste", [H] {
        if (!H().canvas || !H().device) return OperatorResult::Cancelled;
        const bool externalImage =
            ClipboardHelper::HasClipboardImage() &&
            ClipboardHelper::IsSystemClipboardNewerThanLastCopy();
        if (externalImage) {
            if (H().canvas->IsEditingLayerMask()) {
                if (!H().canvas->PasteContentIntoActive(H().device))
                    Logger::Get().Warn("Paste: failed to paste into mask");
            } else if (!H().canvas->PasteContentAsNewLayer(H().device, "Pasted Layer")) {
                Logger::Get().Warn("Paste: failed to paste system image as layer");
            }
        } else if (H().canvas->HasLayerClipboard()) {
            H().canvas->PasteLayersFromClipboard(H().device);
        } else if (!H().canvas->PasteContentIntoActive(H().device)) {
            if (!H().canvas->PasteContentAsNewLayer(H().device, "Pasted Layer"))
                Logger::Get().Warn("Paste: clipboard has no pasteable image");
        }
        return Ok();
    });
    R.Register("SwapColors", [H] {
        if (!H().brush || !H().secondaryColor) return OperatorResult::Cancelled;
        for (int i = 0; i < 4; ++i)
            std::swap(H().brush->color[i], H().secondaryColor[i]);
        if (H().colorSwapPending) *H().colorSwapPending = true;
        return Ok();
    });
    R.Register("DuplicateLayer", [H] {
        if (!H().canvas || !H().device || !H().ui) return OperatorResult::Cancelled;
        if (!H().ui->selectedLayers.empty()) {
            H().canvas->DuplicateLayers(H().device, H().ui->selectedLayers);
            H().ui->selectedLayers.clear();
            if (H().canvas->GetActiveLayerIndex() >= 0)
                H().ui->selectedLayers.push_back(H().canvas->GetActiveLayerIndex());
        } else if (H().canvas->GetActiveLayerIndex() >= 0) {
            int neu = H().canvas->DuplicateLayer(H().device, H().canvas->GetActiveLayerIndex());
            H().ui->selectedLayers.clear();
            if (neu >= 0) H().ui->selectedLayers.push_back(neu);
        }
        return Ok();
    });

    // --- File ---
    R.Register("NewProject", [H] {
        if (H().ui) H().ui->openNewProjectWizard = true;
        return Ok();
    });
    R.Register("OpenProject", [H] {
        if (H().ui) UI::FileExplorerOpen(H().ui->fileExplorer, UI::FileExplorerMode::OpenProject);
        return Ok();
    });
    R.Register("SaveProject", [H] {
        if (H().ui) UI::FileExplorerOpen(H().ui->fileExplorer, UI::FileExplorerMode::SaveProject);
        return Ok();
    });
    R.Register("QuickExport", [H] {
        if (!H().canvas) return OperatorResult::Cancelled;
        const bool advanced = H().canvas->GetProjectType() != Canvas::ProjectType::Simple;
        if (advanced) {
            if (Project* proj = ProjectManager::Get().ActiveProject()) {
                int n = proj->QuickExportAllMaps();
                if (n > 0)
                    Logger::Get().Info("Batch export: " + std::to_string(n) + " map(s) written");
                else
                    Logger::Get().Error("Batch export: no maps written");
            }
        } else {
            // Background encode/write; document locked via JobManager while running.
            if (!H().canvas->ExportWithProjectSettingsAsync())
                Logger::Get().Error("Quick export failed to start (busy or empty?)");
        }
        return Ok();
    });
    R.Register("AdvancedExport", [H] {
        if (!H().ui || !H().canvas) return OperatorResult::Cancelled;
        const bool advanced = H().canvas->GetProjectType() != Canvas::ProjectType::Simple;
        if (advanced)
            UI::FileExplorerOpen(H().ui->fileExplorer, UI::FileExplorerMode::ExportTemplate);
        else
            UI::FileExplorerOpen(H().ui->fileExplorer, UI::FileExplorerMode::AdvancedExport);
        return Ok();
    });

    // --- Tools ---
    R.Register("BrushTool", [H] {
        if (H().activeTool) *H().activeTool = ActiveTool::Brush;
        if (H().brush) H().brush->erase = false;
        return Ok();
    });
    R.Register("EraserTool", [H] {
        if (H().activeTool) *H().activeTool = ActiveTool::Eraser;
        if (H().brush) H().brush->erase = true;
        return Ok();
    });
    R.Register("BucketFillTool", [H] {
        if (H().activeTool) *H().activeTool = ActiveTool::BucketFill;
        return Ok();
    });
    R.Register("GradientTool", [H] {
        if (H().activeTool) *H().activeTool = ActiveTool::Gradient;
        return Ok();
    });
    R.Register("PipetteTool", [H] {
        if (H().activeTool) *H().activeTool = ActiveTool::Pipette;
        return Ok();
    });
    R.Register("PanTool", [H] {
        if (H().activeTool) *H().activeTool = ActiveTool::Pan;
        return Ok();
    });
    R.Register("RotateTool", [H] {
        if (H().activeTool) *H().activeTool = ActiveTool::Pan;
        return Ok();
    });
    R.Register("TransformTool", [H] {
        if (H().freeTransformMode) *H().freeTransformMode = false;
        if (H().activeTool) *H().activeTool = ActiveTool::MovePixels;
        return Ok();
    });
    R.Register("SmudgeTool", [H] {
        if (H().activeTool) *H().activeTool = ActiveTool::Smudge;
        return Ok();
    });
    R.Register("BlurTool", [H] {
        if (H().activeTool) *H().activeTool = ActiveTool::BlurTool;
        return Ok();
    });
    R.Register("StampTool", [H] {
        if (H().activeTool) *H().activeTool = ActiveTool::Stamp;
        return Ok();
    });

    R.Register("SelectToolGroup", [H] {
        if (!H().activeTool || !H().lastSelectTool) return OperatorResult::Cancelled;
        if (UI::IsSelectTool(*H().activeTool))
            *H().activeTool = UI::CycleSelectTool(*H().activeTool);
        else
            *H().activeTool = *H().lastSelectTool;
        *H().lastSelectTool = *H().activeTool;
        return Ok();
    });
    R.Register("LassoToolGroup", [H] {
        if (!H().activeTool || !H().lastLassoTool) return OperatorResult::Cancelled;
        if (UI::IsLassoTool(*H().activeTool))
            *H().activeTool = UI::CycleLassoTool(*H().activeTool);
        else
            *H().activeTool = *H().lastLassoTool;
        *H().lastLassoTool = *H().activeTool;
        return Ok();
    });
    R.Register("WandToolGroup", [H] {
        if (!H().activeTool || !H().lastWandTool) return OperatorResult::Cancelled;
        if (UI::IsWandTool(*H().activeTool))
            *H().activeTool = UI::CycleWandTool(*H().activeTool);
        else
            *H().activeTool = *H().lastWandTool;
        *H().lastWandTool = *H().activeTool;
        return Ok();
    });

    R.Register("RectSelectTool", [H] {
        if (H().activeTool) *H().activeTool = ActiveTool::RectSelect;
        return Ok();
    });
    R.Register("EllipseSelectTool", [H] {
        if (H().activeTool) *H().activeTool = ActiveTool::EllipseSelect;
        return Ok();
    });
    R.Register("LassoSelectTool", [H] {
        if (H().activeTool) *H().activeTool = ActiveTool::LassoSelect;
        if (H().lastLassoTool) *H().lastLassoTool = ActiveTool::LassoSelect;
        return Ok();
    });
    R.Register("PolygonalLassoTool", [H] {
        if (H().activeTool) *H().activeTool = ActiveTool::PolygonalLasso;
        if (H().lastLassoTool) *H().lastLassoTool = ActiveTool::PolygonalLasso;
        return Ok();
    });
    R.Register("MagicWandTool", [H] {
        if (H().activeTool) *H().activeTool = ActiveTool::MagicWand;
        if (H().lastWandTool) *H().lastWandTool = ActiveTool::MagicWand;
        return Ok();
    });
    R.Register("SmartSelectTool", [H] {
        if (H().activeTool) *H().activeTool = ActiveTool::SmartSelect;
        if (H().lastWandTool) *H().lastWandTool = ActiveTool::SmartSelect;
        return Ok();
    });
    R.Register("QuickSelectTool", [H] {
        if (H().activeTool) *H().activeTool = ActiveTool::QuickSelect;
        if (H().lastWandTool) *H().lastWandTool = ActiveTool::QuickSelect;
        return Ok();
    });

    R.Register("FreeTransform", [H] {
        if (!H().canvas || !H().device || !H().activeTool || !H().freeTransformMode)
            return OperatorResult::Cancelled;
        if (!*H().freeTransformMode) {
            if (H().toolBeforeFreeTransform)
                *H().toolBeforeFreeTransform = *H().activeTool;
            if (H().toolBeforeFreeTransform && *H().toolBeforeFreeTransform == ActiveTool::MovePixels)
                *H().toolBeforeFreeTransform = ActiveTool::Brush;
        }
        *H().freeTransformMode = true;
        *H().activeTool = ActiveTool::MovePixels;
        if (!H().canvas->IsMovingPixels())
            H().canvas->StartMovePixels(H().device);
        return Ok();
    });

    // --- Selection / Image ---
    R.Register("SelectAll", [H] {
        if (H().canvas) H().canvas->SelectAll();
        return Ok();
    });
    R.Register("Deselect", [H] {
        if (!H().canvas || !H().device) return OperatorResult::Cancelled;
        H().canvas->ClearSelection();
        H().canvas->UpdateSelectionMaskTexture(H().device);
        return Ok();
    });
    R.Register("InvertSelection", [H] {
        if (H().canvas) H().canvas->InvertSelection();
        return Ok();
    });
    R.Register("CropToSelection", [H] {
        if (H().canvas && H().device && H().canvas->HasSelection())
            H().canvas->CropCanvasToSelection(H().device);
        return Ok();
    });
    R.Register("InvertColors", [H] {
        if (H().canvas) H().canvas->InvertColors();
        return Ok();
    });
    R.Register("InvertAlpha", [H] {
        if (H().canvas) H().canvas->InvertAlpha();
        return Ok();
    });
    R.Register("AdjustHSV", [H] {
        if (H().ui) H().ui->showHSVModal = true;
        return Ok();
    });
    R.Register("AdjustCurves", [H] {
        if (H().ui) H().ui->showCurvesModal = true;
        return Ok();
    });
    R.Register("AdjustBlur", [H] {
        if (H().ui) H().ui->showBlurModal = true;
        return Ok();
    });
    R.Register("AdjustNoise", [H] {
        if (H().ui) H().ui->showNoiseModal = true;
        return Ok();
    });
    R.Register("ContentAwareFill", [H] {
        if (H().canvas && H().device)
            H().canvas->ApplyContentAwareFill(H().device);
        return Ok();
    });
    R.Register("PerspectiveWarp", [H] {
        if (!H().canvas || !H().device || !H().activeTool) return OperatorResult::Cancelled;
        if (H().toolBeforeWarp) *H().toolBeforeWarp = *H().activeTool;
        H().canvas->StartWarpOperator(H().device, Canvas::WarpOperatorMode::Perspective);
        if (H().warpDragIndex) *H().warpDragIndex = -1;
        return Ok();
    });
    R.Register("MeshWarp", [H] {
        if (!H().canvas || !H().device || !H().activeTool) return OperatorResult::Cancelled;
        if (H().toolBeforeWarp) *H().toolBeforeWarp = *H().activeTool;
        H().canvas->StartWarpOperator(H().device, Canvas::WarpOperatorMode::Mesh);
        if (H().warpDragIndex) *H().warpDragIndex = -1;
        return Ok();
    });
    R.Register("RefreshCanvas", [H] {
        if (H().canvas) H().canvas->RefreshCanvas(H().device);
        return Ok();
    });

    Logger::Get().Info("OperatorRegistry: registered " +
        std::to_string(OperatorRegistry::Get().List().size()) + " operators");
}

} // namespace core::ops
