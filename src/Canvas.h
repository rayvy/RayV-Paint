#pragma once

#include <d3d11.h>
#include <directxmath.h>
#include <vector>
#include <string>
#include <functional>
#include "core/PaintEngine.h"
#include "core/DdsHelper.h"
#include "core/UndoRedoManager.h"

struct Layer {
    std::string name;
    bool visible = true;
    float opacity = 1.0f;
    std::vector<float> pixels; // CPU pixel data: RGBA (32-bit float per channel, size width * height * 4)
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    bool needsUpload = false;
};

enum class StrokePhase {
    Begin,
    Update,
    End
};

class Canvas {
public:
    Canvas();
    ~Canvas();

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
    void ResizeCanvas(ID3D11Device* device, int width, int height);

    // Layer Management
    void CreateNewLayer(ID3D11Device* device, const std::string& name);
    void DeleteLayer(int index);
    void SetActiveLayerIndex(int idx);
    int GetActiveLayerIndex() const { return m_ActiveLayerIdx; }
    std::vector<Layer>& GetLayers() { return m_Layers; }

    // Paint Operation
    void PaintOnActiveLayer(float currRawX, float currRawY, StrokePhase phase, const BrushSettings& brush);

    // File Import / Export
    bool LoadImageToLayer(ID3D11Device* device, const std::string& filepath);
    bool SaveCanvas(const std::string& filepath, DdsFormat ddsFormat);
    bool SaveCanvasStandard(const std::string& filepath, const std::string& iccProfilePath = "");

    // Visualization Mode
    int GetVisualizationMode() const { return m_VisMode; }
    void SetVisualizationMode(int mode) { m_VisMode = mode; }
    
    float* GetAlphaMaskColor() { return m_AlphaMaskColor; }

    // Undo / Redo Interface
    bool Undo();
    bool Redo();
    bool CanUndo() const;
    bool CanRedo() const;
    std::string GetUndoName() const;
    std::string GetRedoName() const;
    void ClearUndoHistory();
    bool IsDocumentModified() const { return m_IsDocumentModified; }
    void SetDocumentModified(bool modified) { m_IsDocumentModified = modified; }

    // Native RAYP Format
    bool SaveCanvasRayp(const std::string& filepath);
    void SaveCanvasRaypAsync(const std::string& filepath, std::function<void(bool)> callback = nullptr);
    bool LoadCanvasRayp(const std::string& filepath, ID3D11Device* device);

private:
    void BackupTile(int tileX, int tileY);

    struct Vertex {
        DirectX::XMFLOAT2 pos;
        DirectX::XMFLOAT2 uv;
    };

    struct CanvasBuffer {
        DirectX::XMFLOAT4 viewportSizeAndZoom;
        // xy: Pan/Offset, z: Canvas Width, w: Canvas Height
        DirectX::XMFLOAT4 offsetAndCanvasSize;
        // x: Visualization Mode, yzw: Alpha Mask Color (RGB)
        DirectX::XMFLOAT4 visModeAndMaskColor;
    };

    struct LayerBuffer {
        DirectX::XMFLOAT4 layerParams; // x: opacity, yzw: unused
    };

    void ComposeLayers(ID3D11DeviceContext* context);
    void CreateCompositeResources(ID3D11Device* device);
    void ReleaseCompositeResources();
    void RecreateLayerTexture(ID3D11Device* device, Layer& layer);

    int m_Width;
    int m_Height;
    float m_Zoom;
    DirectX::XMFLOAT2 m_Pan;

    // Visualization Mode: 0 = RGBA, 1 = RGB, 2 = Alpha, 3 = Alpha Mask
    int m_VisMode = 0;
    float m_AlphaMaskColor[3] = { 0.0f, 1.0f, 0.0f }; // Green default

    // Layer resources
    std::vector<Layer> m_Layers;
    int m_ActiveLayerIdx = -1;

    // Direct3D 11 Resources
    ID3D11Buffer* m_VertexBuffer = nullptr;
    ID3D11Buffer* m_IndexBuffer = nullptr;
    ID3D11Buffer* m_ConstantBuffer = nullptr;
    ID3D11Buffer* m_LayerConstantBuffer = nullptr;

    ID3D11VertexShader* m_VertexShader = nullptr;
    ID3D11VertexShader* m_LayerVertexShader = nullptr;
    ID3D11PixelShader* m_PixelShader = nullptr;
    ID3D11PixelShader* m_LayerBlendPixelShader = nullptr;
    ID3D11InputLayout* m_InputLayout = nullptr;
    ID3D11SamplerState* m_SamplerState = nullptr;

    // Layer Composition Target
    ID3D11Texture2D* m_CompositeTexture = nullptr;
    ID3D11RenderTargetView* m_CompositeRTV = nullptr;
    ID3D11ShaderResourceView* m_CompositeSRV = nullptr;
    ID3D11BlendState* m_LayerBlendState = nullptr;

    // Stroke state tracking
    bool m_IsStrokeActive = false;
    float m_LastDabX = 0.0f;
    float m_LastDabY = 0.0f;
    float m_StrokeDistanceAccumulator = 0.0f;
    float m_PrevStabilizedX = 0.0f;
    float m_PrevStabilizedY = 0.0f;

    // Undo/Redo Engine
    UndoRedoManager m_UndoRedoManager;
    std::unordered_map<int, TileDelta> m_ActiveStrokeDeltas;
    bool m_IsDocumentModified = false;
};
