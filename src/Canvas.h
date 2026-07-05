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
    void ToggleLayerIsolation(int layerIdx);
    bool IsLayerIsolated(int layerIdx) const { return m_IsIsolatedMode && m_IsolatedLayerIdx == layerIdx; }
    bool IsInIsolationMode() const { return m_IsIsolatedMode; }
    int GetActiveLayerIndex() const { return m_ActiveLayerIdx; }
    std::vector<Layer>& GetLayers() { return m_Layers; }

    // Paint Operation
    void PaintOnActiveLayer(float currRawX, float currRawY, StrokePhase phase, const BrushSettings& brush);

    // File Import / Export
    bool LoadImageToLayer(ID3D11Device* device, const std::string& filepath);
    bool SaveCanvas(const std::string& filepath, DdsFormat ddsFormat);
    bool SaveCanvasStandard(const std::string& filepath, const std::string& iccProfilePath = "");

    // Pixel Transformations
    void FlipActiveLayerHorizontal(ID3D11Device* device);
    void FlipActiveLayerVertical(ID3D11Device* device);
    void RotateCanvas90(ID3D11Device* device, bool clockwise);
    void FlipCanvasHorizontal(ID3D11Device* device);
    void FlipCanvasVertical(ID3D11Device* device);
    void CommitTransformation(const std::string& actionName);



    bool GetChannelR() const { return m_ChannelR; }
    void SetChannelR(bool v) { m_ChannelR = v; }
    bool GetChannelG() const { return m_ChannelG; }
    void SetChannelG(bool v) { m_ChannelG = v; }
    bool GetChannelB() const { return m_ChannelB; }
    void SetChannelB(bool v) { m_ChannelB = v; }
    bool GetChannelA() const { return m_ChannelA; }
    void SetChannelA(bool v) { m_ChannelA = v; }

    bool Undo();
    bool Redo();
    bool CanUndo() const;
    bool CanRedo() const;
    std::string GetUndoName() const;
    std::string GetRedoName() const;
    void ClearUndoHistory();
    bool IsDocumentModified() const { return m_IsDocumentModified; }
    void SetDocumentModified(bool modified) { m_IsDocumentModified = modified; }
    std::string GetCurrentProjectFilePath() const { return m_CurrentProjectFilePath; }
    void SetCurrentProjectFilePath(const std::string& path) { m_CurrentProjectFilePath = path; }

    std::string GetExportPath() const { return m_ExportPath; }
    void SetExportPath(const std::string& path) { m_ExportPath = path; }
    std::string GetExportFormat() const { return m_ExportFormat; }
    void SetExportFormat(const std::string& format) { m_ExportFormat = format; }
    bool GetExportAdvancedMode() const { return m_ExportAdvancedMode; }
    void SetExportAdvancedMode(bool v) { m_ExportAdvancedMode = v; }
    std::string GetExportCompressionSpeed() const { return m_ExportCompressionSpeed; }
    void SetExportCompressionSpeed(const std::string& speed) { m_ExportCompressionSpeed = speed; }
    bool GetExportGenerateMipMaps() const { return m_ExportGenerateMipMaps; }
    void SetExportGenerateMipMaps(bool v) { m_ExportGenerateMipMaps = v; }
    std::string GetExportMipFilter() const { return m_ExportMipFilter; }
    void SetExportMipFilter(const std::string& filter) { m_ExportMipFilter = filter; }
    std::string GetExportPngColorSpace() const { return m_ExportPngColorSpace; }
    void SetExportPngColorSpace(const std::string& cs) { m_ExportPngColorSpace = cs; }

    float GetRotationAngle() const { return m_RotationAngle; }
    void SetRotationAngle(float angle) { m_RotationAngle = angle; }
    bool GetMirrorHorizontal() const { return m_MirrorHorizontal; }
    void SetMirrorHorizontal(bool v) { m_MirrorHorizontal = v; }
    bool GetMirrorVertical() const { return m_MirrorVertical; }
    void SetMirrorVertical(bool v) { m_MirrorVertical = v; }

    // Native RAYP Format
    bool SaveCanvasRayp(const std::string& filepath);
    void SaveCanvasRaypAsync(const std::string& filepath, std::function<void(bool)> callback = nullptr);
    bool LoadCanvasRayp(const std::string& filepath, ID3D11Device* device);

    std::vector<float> GetComposedPixels();

private:
    void BackupTile(int tileX, int tileY);

    struct Vertex {
        DirectX::XMFLOAT2 pos;
        DirectX::XMFLOAT2 uv;
    };

    struct CanvasBuffer {
        DirectX::XMFLOAT4 viewportSizeAndZoom;
        DirectX::XMFLOAT4 offsetAndCanvasSize;
        DirectX::XMFLOAT4 channelMasks;
    };

    struct LayerBuffer {
        DirectX::XMFLOAT4 layerParams;
    };

    void ComposeLayers(ID3D11DeviceContext* context);
    void CreateCompositeResources(ID3D11Device* device);
    void ReleaseCompositeResources();
    void RecreateLayerTexture(ID3D11Device* device, Layer& layer);

    int m_Width;
    int m_Height;
    float m_Zoom;
    DirectX::XMFLOAT2 m_Pan;

    bool m_ChannelR = true;
    bool m_ChannelG = true;
    bool m_ChannelB = true;
    bool m_ChannelA = true;

    std::vector<Layer> m_Layers;
    int m_ActiveLayerIdx = -1;



    // Layer Isolation State
    bool m_IsIsolatedMode = false;
    int m_IsolatedLayerIdx = -1;
    std::vector<bool> m_PreIsolationVisibility;

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
    std::string m_CurrentProjectFilePath;

    std::string m_ExportPath = "";
    std::string m_ExportFormat = "BC7_UNORM_SRGB";
    bool m_ExportAdvancedMode = false;
    std::string m_ExportCompressionSpeed = "Medium";
    bool m_ExportGenerateMipMaps = true;
    std::string m_ExportMipFilter = "Bicubic";
    std::string m_ExportPngColorSpace = "sRGB";

    float m_RotationAngle = 0.0f;
    bool m_MirrorHorizontal = false;
    bool m_MirrorVertical = false;
};
