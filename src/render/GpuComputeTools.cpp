#include "GpuComputeTools.h"
#include "../core/Logger.h"
#include <d3dcompiler.h>
#include <fstream>
#include <sstream>
#include <algorithm>

bool GpuComputeTools::Initialize(
    ID3D12Device* device,
    AllocateSrvFn allocFn,
    FreeSrvFn freeFn
) {
    m_Device = device;
    m_AllocFn = allocFn;
    m_FreeFn = freeFn;

    if (!CreateRootSignature()) {
        Logger::Get().Error("Failed to create GpuComputeTools Root Signature.");
        return false;
    }

    if (!CreatePipelineStates()) {
        Logger::Get().Error("Failed to create GpuComputeTools Pipeline States.");
        return false;
    }

    Logger::Get().Info("GpuComputeTools initialized successfully.");
    return true;
}

void GpuComputeTools::Shutdown() {
    m_PsoInit.Reset();
    m_PsoMain.Reset();
    m_PsoGlobalThreshold.Reset();
    m_RootSignature.Reset();
    m_Device = nullptr;
}

bool GpuComputeTools::CreateRootSignature() {
    D3D12_ROOT_PARAMETER rootParams[3] = {};

    // 0: Root Constants (12 constants = 48 bytes, register b0)
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = 12;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // 1: Table SRV (t0)
    D3D12_DESCRIPTOR_RANGE rangesT0 = {};
    rangesT0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rangesT0.NumDescriptors = 1;
    rangesT0.BaseShaderRegister = 0;
    rangesT0.RegisterSpace = 0;
    rangesT0.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &rangesT0;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // 2: Table UAV (u0)
    D3D12_DESCRIPTOR_RANGE rangesU0 = {};
    rangesU0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    rangesU0.NumDescriptors = 1;
    rangesU0.BaseShaderRegister = 0;
    rangesU0.RegisterSpace = 0;
    rangesU0.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges = &rangesU0;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 3;
    rootSigDesc.pParameters = rootParams;
    rootSigDesc.NumStaticSamplers = 0;
    rootSigDesc.pStaticSamplers = nullptr;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3DBlob> serializedRootSig;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    if (FAILED(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serializedRootSig, &errorBlob))) {
        if (errorBlob) {
            Logger::Get().Error("Compute Root signature serialization error: " + std::string(reinterpret_cast<const char*>(errorBlob->GetBufferPointer())));
        }
        return false;
    }

    if (FAILED(m_Device->CreateRootSignature(0, serializedRootSig->GetBufferPointer(), serializedRootSig->GetBufferSize(), IID_PPV_ARGS(&m_RootSignature)))) {
        return false;
    }

    return true;
}

bool GpuComputeTools::CreatePipelineStates() {
    Microsoft::WRL::ComPtr<ID3DBlob> csInitBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> csMainBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> csGlobalThresholdBlob;

    if (!CompileShaders(csInitBlob, csMainBlob, csGlobalThresholdBlob)) {
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = m_RootSignature.Get();

    desc.CS = { csInitBlob->GetBufferPointer(), csInitBlob->GetBufferSize() };
    if (FAILED(m_Device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&m_PsoInit)))) {
        Logger::Get().Error("Failed to create CSInit Compute PSO");
        return false;
    }

    desc.CS = { csMainBlob->GetBufferPointer(), csMainBlob->GetBufferSize() };
    if (FAILED(m_Device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&m_PsoMain)))) {
        Logger::Get().Error("Failed to create CSMain Compute PSO");
        return false;
    }

    desc.CS = { csGlobalThresholdBlob->GetBufferPointer(), csGlobalThresholdBlob->GetBufferSize() };
    if (FAILED(m_Device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&m_PsoGlobalThreshold)))) {
        Logger::Get().Error("Failed to create CSGlobalThreshold Compute PSO");
        return false;
    }

    return true;
}

bool GpuComputeTools::CompileShaders(
    Microsoft::WRL::ComPtr<ID3DBlob>& csInit,
    Microsoft::WRL::ComPtr<ID3DBlob>& csMain,
    Microsoft::WRL::ComPtr<ID3DBlob>& csGlobalThreshold
) {
    // Try to load precompiled .cso files
    std::vector<uint8_t> csoInit = LoadCsoFile("shaders/FloodFill_CSInit.cso");
    std::vector<uint8_t> csoMain = LoadCsoFile("shaders/FloodFill_CSMain.cso");
    std::vector<uint8_t> csoGlobalThreshold = LoadCsoFile("shaders/FloodFill_CSGlobalThreshold.cso");

    auto makeBlobFromBytes = [](const std::vector<uint8_t>& bytes, Microsoft::WRL::ComPtr<ID3DBlob>& blob) -> bool {
        if (bytes.empty()) return false;
        HRESULT hr = D3DCreateBlob(bytes.size(), &blob);
        if (FAILED(hr)) return false;
        std::memcpy(blob->GetBufferPointer(), bytes.data(), bytes.size());
        return true;
    };

    if (!csoInit.empty() && !csoMain.empty() && !csoGlobalThreshold.empty()) {
        if (makeBlobFromBytes(csoInit, csInit) &&
            makeBlobFromBytes(csoMain, csMain) &&
            makeBlobFromBytes(csoGlobalThreshold, csGlobalThreshold)) {
            Logger::Get().Info("Compute shaders loaded from .cso files successfully.");
            return true;
        }
    }

    // Fallback to runtime compiler
    std::string hlslSource;
    std::ifstream file("shaders/FloodFill.hlsl");
    if (!file.is_open()) file.open("FloodFill.hlsl");
    if (file.is_open()) {
        std::stringstream ss; ss << file.rdbuf();
        hlslSource = ss.str();
        Logger::Get().Info("Loaded FloodFill.hlsl from disk for runtime compile.");
    } else {
        hlslSource = GetEmbeddedFloodFillHLSL();
        Logger::Get().Info("Using embedded FloodFill.hlsl fallback.");
    }

    auto compile = [&](const char* entry, Microsoft::WRL::ComPtr<ID3DBlob>& dest) -> bool {
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        Microsoft::WRL::ComPtr<ID3DBlob> err;
        HRESULT hr = D3DCompile(hlslSource.data(), hlslSource.size(),
            nullptr, nullptr, nullptr, entry, "cs_5_0", flags, 0, &dest, &err);
        if (FAILED(hr)) {
            if (err) {
                Logger::Get().Error("Compute shader compile error (" + std::string(entry) + "): " +
                    std::string(reinterpret_cast<const char*>(err->GetBufferPointer())));
            }
            return false;
        }
        return true;
    };

    if (!compile("CSInit", csInit)) return false;
    if (!compile("CSMain", csMain)) return false;
    if (!compile("CSGlobalThreshold", csGlobalThreshold)) return false;

    return true;
}

bool GpuComputeTools::RunFloodFill(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* albedoRes,
    DXGI_FORMAT albedoFormat,
    ID3D12Resource* maskRes,
    int width,
    int height,
    int startX,
    int startY,
    const float fillColor[4],
    float tolerance,
    bool contiguous
) {
    if (!m_Device || !albedoRes || !maskRes) return false;

    // 1. Allocate descriptor handles
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = {};
    if (!m_AllocFn(&srvCpu, &srvGpu)) return false;

    D3D12_CPU_DESCRIPTOR_HANDLE uavCpu = {};
    D3D12_GPU_DESCRIPTOR_HANDLE uavGpu = {};
    if (!m_AllocFn(&uavCpu, &uavGpu)) {
        m_FreeFn(srvCpu, srvGpu);
        return false;
    }

    // 2. Create views
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = albedoFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    m_Device->CreateShaderResourceView(albedoRes, &srvDesc, srvCpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_R8_UNORM;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;
    m_Device->CreateUnorderedAccessView(maskRes, nullptr, &uavDesc, uavCpu);

    // 3. Transition resources to compute states
    TransitionResource(cmdList, albedoRes, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TransitionResource(cmdList, maskRes, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // 4. Set state & Bind resources
    cmdList->SetComputeRootSignature(m_RootSignature.Get());
    cmdList->SetComputeRootDescriptorTable(1, srvGpu);
    cmdList->SetComputeRootDescriptorTable(2, uavGpu);

    FloodFillParams params = {};
    params.seedColor = DirectX::XMFLOAT4(fillColor[0], fillColor[1], fillColor[2], fillColor[3]);
    params.tolerance = tolerance;
    params.seedPos = DirectX::XMINT2(startX, startY);
    params.canvasSize = DirectX::XMINT2(width, height);

    UINT groupCountX = (width + 15) / 16;
    UINT groupCountY = (height + 15) / 16;

    if (contiguous) {
        // Initialize mask with seed
        cmdList->SetPipelineState(m_PsoInit.Get());
        cmdList->SetComputeRoot32BitConstants(0, 12, &params, 0);
        cmdList->Dispatch(groupCountX, groupCountY, 1);

        D3D12_RESOURCE_BARRIER uavBarrier = {};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = maskRes;
        cmdList->ResourceBarrier(1, &uavBarrier);

        // Propagate flood fill
        cmdList->SetPipelineState(m_PsoMain.Get());
        
        // Cap propagation at 512 passes for performance/deadlock prevention
        int passCount = std::min(512, std::max(width, height));
        for (int i = 0; i < passCount; ++i) {
            params.passIndex = i;
            cmdList->SetComputeRoot32BitConstants(0, 12, &params, 0);
            cmdList->Dispatch(groupCountX, groupCountY, 1);
            cmdList->ResourceBarrier(1, &uavBarrier);
        }
    } else {
        // Global Threshold Matching (Non-contiguous)
        cmdList->SetPipelineState(m_PsoGlobalThreshold.Get());
        cmdList->SetComputeRoot32BitConstants(0, 12, &params, 0);
        cmdList->Dispatch(groupCountX, groupCountY, 1);
    }

    // 5. Restore transitions
    TransitionResource(cmdList, albedoRes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    TransitionResource(cmdList, maskRes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // 6. Free descriptors (safe to free immediately since we're done building command list commands,
    // and the descriptors are referenced in the GPU-visible heap during dispatch recording)
    m_FreeFn(srvCpu, srvGpu);
    m_FreeFn(uavCpu, uavGpu);

    return true;
}

void GpuComputeTools::TransitionResource(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES stateBefore,
    D3D12_RESOURCE_STATES stateAfter
) {
    if (stateBefore == stateAfter) return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = stateBefore;
    barrier.Transition.StateAfter = stateAfter;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);
}

std::vector<uint8_t> GpuComputeTools::LoadCsoFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

std::string GpuComputeTools::GetEmbeddedFloodFillHLSL() const {
    return R"(
Texture2D<float4> gInputAlbedo : register(t0);
RWTexture2D<float> gOutputMask : register(u0);

cbuffer FloodFillParams : register(b0) {
    float4 seedColor;
    float tolerance;
    int2 seedPos;
    int2 canvasSize;
    int passIndex;
};

[numthreads(16, 16, 1)]
void CSInit(uint3 id : SV_DispatchThreadID) {
    int2 pos = int2(id.xy);
    if (pos.x >= canvasSize.x || pos.y >= canvasSize.y) return;

    if (pos.x == seedPos.x && pos.y == seedPos.y) {
        gOutputMask[pos] = 1.0f;
    } else {
        gOutputMask[pos] = 0.0f;
    }
}

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    int2 pos = int2(id.xy);
    if (pos.x >= canvasSize.x || pos.y >= canvasSize.y) return;

    float currentMask = gOutputMask[pos];
    if (currentMask > 0.0f) return;

    int2 neighbors[4] = {
        pos + int2(1, 0),
        pos + int2(-1, 0),
        pos + int2(0, 1),
        pos + int2(0, -1)
    };

    bool hasFilledNeighbor = false;
    for (int i = 0; i < 4; ++i) {
        int2 n = neighbors[i];
        if (n.x >= 0 && n.x < canvasSize.x && n.y >= 0 && n.y < canvasSize.y) {
            if (gOutputMask[n] > 0.0f) {
                hasFilledNeighbor = true;
                break;
            }
        }
    }

    if (hasFilledNeighbor) {
        float4 color = gInputAlbedo[pos];
        float diff = sqrt(
            pow(color.r - seedColor.r, 2) +
            pow(color.g - seedColor.g, 2) +
            pow(color.b - seedColor.b, 2)
        );
        if (diff <= tolerance * sqrt(3.0f)) {
            gOutputMask[pos] = 1.0f;
        }
    }
}

[numthreads(16, 16, 1)]
void CSGlobalThreshold(uint3 id : SV_DispatchThreadID) {
    int2 pos = int2(id.xy);
    if (pos.x >= canvasSize.x || pos.y >= canvasSize.y) return;

    float4 color = gInputAlbedo[pos];
    float diff = sqrt(
        pow(color.r - seedColor.r, 2) +
        pow(color.g - seedColor.g, 2) +
        pow(color.b - seedColor.b, 2)
    );
    if (diff <= tolerance * sqrt(3.0f)) {
        gOutputMask[pos] = 1.0f;
    } else {
        gOutputMask[pos] = 0.0f;
    }
}
)";
}
