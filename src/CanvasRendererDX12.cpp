#include "CanvasRendererDX12.h"
#include "Logger.h"
#include <d3dcompiler.h>
#include <sstream>
#include <fstream>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <cassert>

bool CanvasRendererDX12::Initialize(
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    AllocateSrvFn allocFn,
    FreeSrvFn freeFn
) {
    m_Device = device;
    m_CommandQueue = queue;
    m_AllocFn = allocFn;
    m_FreeFn = freeFn;

    if (!CreateRootSignatures()) return false;
    if (!CreatePipelineStates()) return false;
    if (!CreateConstantBuffers()) return false;
    if (!CreateDummyResources()) return false;
    if (!CreateQuadVertexBuffer()) return false;

    if (FAILED(m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_UploadFence)))) {
        Logger::Get().Error("Failed to create upload fence");
        return false;
    }
    m_UploadFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_UploadFenceEvent) {
        Logger::Get().Error("Failed to create upload fence event");
        return false;
    }

    if (m_AsyncUploader.Initialize(m_Device, m_CommandQueue)) {
        m_AsyncUploaderReady = true;
        Logger::Get().Info("Async upload queue initialized.");
    } else {
        Logger::Get().Info("Async upload unavailable — using sync path.");
    }

    if (!m_ComputeTools.Initialize(device, allocFn, freeFn)) {
        Logger::Get().Error("Failed to initialize GpuComputeTools");
        return false;
    }

    return true;
}

void CanvasRendererDX12::Shutdown() {
    if (m_AsyncUploaderReady) {
        m_AsyncUploader.Shutdown();
        m_AsyncUploaderReady = false;
    }

    for (auto& [cache, gpuRes] : m_GpuLayers) {
        for (auto& [key, tile] : gpuRes.albedoTiles) {
            if (tile.srvCpuHandle.ptr != 0) {
                m_FreeFn(tile.srvCpuHandle, tile.srvGpuHandle);
            }
        }
        for (auto& [key, tile] : gpuRes.maskTiles) {
            if (tile.srvCpuHandle.ptr != 0) {
                m_FreeFn(tile.srvCpuHandle, tile.srvGpuHandle);
            }
        }
    }
    m_GpuLayers.clear();

    if (m_SelectionMaskSrvCpu.ptr != 0) {
        m_FreeFn(m_SelectionMaskSrvCpu, m_SelectionMaskSrvGpu);
        m_SelectionMaskSrvCpu = {};
    }
    m_SelectionMaskTex.Reset();

    for (int i = 0; i < 2; ++i) {
        if (m_CompositeSrvCpus[i].ptr != 0) {
            m_FreeFn(m_CompositeSrvCpus[i], m_CompositeSrvGpus[i]);
            m_CompositeSrvCpus[i] = {};
        }
        m_CompositeRTs[i].Reset();
    }
    m_CompositeRtvHeap.Reset();

    if (m_DummyWhiteSrvCpu.ptr != 0) m_FreeFn(m_DummyWhiteSrvCpu, m_DummyWhiteSrvGpu);
    if (m_DummyBlackSrvCpu.ptr != 0) m_FreeFn(m_DummyBlackSrvCpu, m_DummyBlackSrvGpu);

    m_DummyWhiteTex.Reset();
    m_DummyBlackTex.Reset();

    if (m_ConstantBufferUpload) {
        m_ConstantBufferUpload->Unmap(0, nullptr);
        m_CbMappedData = nullptr;
    }
    m_ConstantBufferUpload.Reset();

    m_QuadVertexBuffer.Reset();
    m_PsoCheckerboard.Reset();
    m_PsoLayerBlend.Reset();
    m_PsoSelectionOutline.Reset();
    m_RootSignature.Reset();

    if (m_UploadFence) {
        m_UploadFenceValue++;
        m_CommandQueue->Signal(m_UploadFence.Get(), m_UploadFenceValue);
        if (m_UploadFence->GetCompletedValue() < m_UploadFenceValue) {
            m_UploadFence->SetEventOnCompletion(m_UploadFenceValue, m_UploadFenceEvent);
            WaitForSingleObject(m_UploadFenceEvent, INFINITE);
        }
    }
    for (auto& pool : m_StagingPools) {
        pool.resources.clear();
    }
    if (m_UploadFenceEvent) {
        CloseHandle(m_UploadFenceEvent);
        m_UploadFenceEvent = nullptr;
    }
    m_UploadFence.Reset();
    m_ComputeTools.Shutdown();
}

bool CanvasRendererDX12::Render(
    ID3D12GraphicsCommandList* cmdList,
    Canvas& canvas,
    ID3D12Resource* viewportRT,
    D3D12_CPU_DESCRIPTOR_HANDLE viewportRtv,
    int viewportWidth,
    int viewportHeight
) {
    m_AccessCounter++;
    if (m_AsyncUploaderReady) {
        m_AsyncUploader.FlushCompleted(0);
    }
    m_CurrentFrameIdx = (m_CurrentFrameIdx + 1) % kMaxFramesInFlight;
    auto& currentPool = m_StagingPools[m_CurrentFrameIdx];

    if (currentPool.fenceValue > 0 &&
        m_UploadFence->GetCompletedValue() < currentPool.fenceValue) {
        m_UploadFence->SetEventOnCompletion(currentPool.fenceValue, m_UploadFenceEvent);
        WaitForSingleObject(m_UploadFenceEvent, INFINITE);
    }
    currentPool.resources.clear();

    m_CbOffset = 0;             // Reset dynamic constant buffer ring allocator

    int canvasWidth = canvas.GetWidth();
    int canvasHeight = canvas.GetHeight();
    if (canvasWidth <= 0 || canvasHeight <= 0) return true;

    // 1. Maintain Composite Ping-pong RT sizes
    if (!m_CompositeRTs[0] || m_CompositeWidth != canvasWidth || m_CompositeHeight != canvasHeight) {
        m_CompositeWidth = canvasWidth;
        m_CompositeHeight = canvasHeight;

        for (int i = 0; i < 2; ++i) {
            if (m_CompositeSrvCpus[i].ptr != 0) {
                m_FreeFn(m_CompositeSrvCpus[i], m_CompositeSrvGpus[i]);
                m_CompositeSrvCpus[i] = {};
            }
            m_CompositeRTs[i].Reset();
        }

        for (int i = 0; i < 2; ++i) {
            m_CompositeRTs[i] = CreateTextureResource(
                canvasWidth, canvasHeight,
                DXGI_FORMAT_R8G8B8A8_UNORM,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
            );

            m_AllocFn(&m_CompositeSrvCpus[i], &m_CompositeSrvGpus[i]);

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            m_Device->CreateShaderResourceView(m_CompositeRTs[i].Get(), &srvDesc, m_CompositeSrvCpus[i]);
        }

        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.NumDescriptors = 2;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(m_Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_CompositeRtvHeap)))) {
            return false;
        }

        UINT rtvSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtvStart = m_CompositeRtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (int i = 0; i < 2; ++i) {
            m_CompositeRtvs[i].ptr = rtvStart.ptr + static_cast<SIZE_T>(i) * rtvSize;
            m_Device->CreateRenderTargetView(m_CompositeRTs[i].Get(), nullptr, m_CompositeRtvs[i]);
        }
    }

    // Garbage collect CPU-deleted layer caches
    GarbageCollectGpuLayers(canvas);

    // 2. Clear first composite target to clear base
    TransitionResource(cmdList, m_CompositeRTs[0].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    cmdList->ClearRenderTargetView(m_CompositeRtvs[0], clearColor, 0, nullptr);
    TransitionResource(cmdList, m_CompositeRTs[0].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // 3. Process Layers & Composite
    auto& layers = canvas.GetLayers();
    int currentCompositeIdx = 0;

    // Set common pipeline states
    cmdList->SetGraphicsRootSignature(m_RootSignature.Get());
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->IASetVertexBuffers(0, 1, &m_QuadVertexBufferView);

    // Update global constant buffer data
    UpdateCanvasBuffer(cmdList, canvas, viewportWidth, viewportHeight);

    for (size_t layerIdx = 0; layerIdx < layers.size(); ++layerIdx) {
        auto& layer = layers[layerIdx];
        if (!layer.visible || layer.isGroup) continue;

        TileCache* activeCache = layer.tileCache.get();
        if (!layer.filters.empty() && layer.filteredCache) {
            activeCache = layer.filteredCache.get();
        }
        if (!activeCache) continue;

        LayerGpuResources& gpuRes = m_GpuLayers[activeCache];

        // Upload dirty tiles to albedo
        UploadDirtyTiles(cmdList, *activeCache, gpuRes, false, {}, {}, 0, 0, currentPool);

        // Upload masks to corresponding albedo tile keys
        if (layer.hasMask && !layer.mask.empty()) {
            if (layer.maskNeedsUpload) {
                UploadDirtyTiles(cmdList, *activeCache, gpuRes, true, layer.mask, layer.maskDirtyTiles, canvasWidth, canvasHeight, currentPool);
                layer.maskNeedsUpload = false;
                std::fill(layer.maskDirtyTiles.begin(), layer.maskDirtyTiles.end(), false);
            }
        }

        layer.needsUpload = false;

        // Perform Ping-Pong blend
        int nextCompositeIdx = 1 - currentCompositeIdx;

        // Copy previous composite accumulation into the base of next composite target
        TransitionResource(cmdList, m_CompositeRTs[currentCompositeIdx].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        TransitionResource(cmdList, m_CompositeRTs[nextCompositeIdx].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

        cmdList->CopyResource(m_CompositeRTs[nextCompositeIdx].Get(), m_CompositeRTs[currentCompositeIdx].Get());

        TransitionResource(cmdList, m_CompositeRTs[currentCompositeIdx].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        TransitionResource(cmdList, m_CompositeRTs[nextCompositeIdx].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);

        cmdList->OMSetRenderTargets(1, &m_CompositeRtvs[nextCompositeIdx], FALSE, nullptr);
        cmdList->SetPipelineState(m_PsoLayerBlend.Get());

        D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(canvasWidth), static_cast<float>(canvasHeight), 0.0f, 1.0f };
        D3D12_RECT scissor = { 0, 0, canvasWidth, canvasHeight };
        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissor);

        // Render each active tile onto the composite
        for (auto& [tileKey, albedoTile] : gpuRes.albedoTiles) {
            int tx = tileKey & 0xFFFF;
            int ty = tileKey >> 16;

            if (m_AsyncUploaderReady && albedoTile.uploadFenceValue > 0) {
                uint64_t completedFence = m_AsyncUploader.GetCompletedFenceValue();
                if (albedoTile.uploadFenceValue > completedFence) {
                    m_AsyncUploader.FlushCompleted(INFINITE);
                }
                TransitionResource(cmdList, albedoTile.resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                albedoTile.uploadFenceValue = 0;
            }

            UpdateLayerBuffer(
                cmdList,
                layer.opacity,
                layer.hasMask,
                0.0f, 0.0f,  // Translation offset placeholders
                1.0f, 1.0f,  // Scale
                0.0f,        // Rotation
                false,       // IsFloating
                0.0f, 0.0f,  // Center pivot
                layer.blendMode
            );

            // Set Tile coordinates
            TileParamsData tileData = {};
            tileData.tileParams = DirectX::XMFLOAT4(static_cast<float>(tx), static_cast<float>(ty), static_cast<float>(canvasWidth), static_cast<float>(canvasHeight));
            uint32_t tileCbOffset = AllocateConstantBufferSpace(&tileData, sizeof(tileData));

            cmdList->SetGraphicsRootConstantBufferView(2, m_ConstantBufferUpload->GetGPUVirtualAddress() + tileCbOffset);

            // Bind resources
            cmdList->SetGraphicsRootDescriptorTable(3, albedoTile.srvGpuHandle);

            if (layer.hasMask && gpuRes.maskTiles.count(tileKey)) {
                auto& maskTile = gpuRes.maskTiles[tileKey];
                if (m_AsyncUploaderReady && maskTile.uploadFenceValue > 0) {
                    uint64_t completedFence = m_AsyncUploader.GetCompletedFenceValue();
                    if (maskTile.uploadFenceValue > completedFence) {
                        m_AsyncUploader.FlushCompleted(INFINITE);
                    }
                    TransitionResource(cmdList, maskTile.resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    maskTile.uploadFenceValue = 0;
                }
                cmdList->SetGraphicsRootDescriptorTable(4, maskTile.srvGpuHandle);
            } else {
                cmdList->SetGraphicsRootDescriptorTable(4, m_DummyWhiteSrvGpu);
            }

            cmdList->SetGraphicsRootDescriptorTable(5, m_CompositeSrvGpus[currentCompositeIdx]);

            cmdList->DrawInstanced(6, 1, 0, 0);
        }

        // If this is the active layer and we are moving pixels, render the floating layer on top of it
        if ((int)layerIdx == canvas.GetStartActiveLayerIdx() && canvas.IsMovingPixels() && canvas.GetFloatingTileCache()) {
            TileCache* floatCache = canvas.GetFloatingTileCache();
            LayerGpuResources& floatGpuRes = m_GpuLayers[floatCache];

            // 1. Upload dirty tiles of floating cache
            UploadDirtyTiles(cmdList, *floatCache, floatGpuRes, false, {}, {}, 0, 0, currentPool);

            // Compute selection bounding box center (pivot)
            int minX = canvasWidth, maxX = 0, minY = canvasHeight, maxY = 0;
            bool hasPixels = false;
            const auto& origMask = canvas.GetOriginalSelectionMask();
            if (!origMask.empty()) {
                for (int y = 0; y < canvasHeight; ++y) {
                    for (int x = 0; x < canvasWidth; ++x) {
                        if (origMask[(size_t)y * canvasWidth + x] > 0) {
                            if (x < minX) minX = x;
                            if (x > maxX) maxX = x;
                            if (y < minY) minY = y;
                            if (y > maxY) maxY = y;
                            hasPixels = true;
                        }
                    }
                }
            }
            float cx = hasPixels ? (minX + maxX) * 0.5f : canvasWidth * 0.5f;
            float cy = hasPixels ? (minY + maxY) * 0.5f : canvasHeight * 0.5f;

            // Normalized pivot coordinates for the shader [0..1]
            float normCx = cx / static_cast<float>(canvasWidth);
            float normCy = cy / static_cast<float>(canvasHeight);

            // Normalized offsets [0..1]
            float normOffX = static_cast<float>(canvas.GetFloatingOffsetX()) / static_cast<float>(canvasWidth);
            float normOffY = static_cast<float>(canvas.GetFloatingOffsetY()) / static_cast<float>(canvasHeight);

            for (auto& [tileKey, albedoTile] : floatGpuRes.albedoTiles) {
                int tx = tileKey & 0xFFFF;
                int ty = tileKey >> 16;

                if (m_AsyncUploaderReady && albedoTile.uploadFenceValue > 0) {
                    uint64_t completedFence = m_AsyncUploader.GetCompletedFenceValue();
                    if (albedoTile.uploadFenceValue > completedFence) {
                        m_AsyncUploader.FlushCompleted(INFINITE);
                    }
                    TransitionResource(cmdList, albedoTile.resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    albedoTile.uploadFenceValue = 0;
                }

                UpdateLayerBuffer(
                    cmdList,
                    layer.opacity,
                    false, // Floating cache does not have its own separate layer mask during move
                    normOffX, normOffY,
                    canvas.GetFloatingScaleX(), canvas.GetFloatingScaleY(),
                    canvas.GetFloatingRotation(),
                    true, // isFloating
                    normCx, normCy,
                    layer.blendMode
                );

                TileParamsData tileData = {};
                tileData.tileParams = DirectX::XMFLOAT4(static_cast<float>(tx), static_cast<float>(ty), static_cast<float>(canvasWidth), static_cast<float>(canvasHeight));
                uint32_t tileCbOffset = AllocateConstantBufferSpace(&tileData, sizeof(tileData));

                cmdList->SetGraphicsRootConstantBufferView(2, m_ConstantBufferUpload->GetGPUVirtualAddress() + tileCbOffset);

                cmdList->SetGraphicsRootDescriptorTable(3, albedoTile.srvGpuHandle);
                cmdList->SetGraphicsRootDescriptorTable(4, m_DummyWhiteSrvGpu); // No mask
                cmdList->SetGraphicsRootDescriptorTable(5, m_CompositeSrvGpus[currentCompositeIdx]);

                cmdList->DrawInstanced(6, 1, 0, 0);
            }
        }

        TransitionResource(cmdList, m_CompositeRTs[nextCompositeIdx].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        currentCompositeIdx = nextCompositeIdx;
    }

    // 4. Present viewport with checkerboard + albedo composite
    TransitionResource(cmdList, viewportRT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmdList->OMSetRenderTargets(1, &viewportRtv, FALSE, nullptr);

    D3D12_VIEWPORT vPort = { 0.0f, 0.0f, static_cast<float>(viewportWidth), static_cast<float>(viewportHeight), 0.0f, 1.0f };
    D3D12_RECT sRect = { 0, 0, viewportWidth, viewportHeight };
    cmdList->RSSetViewports(1, &vPort);
    cmdList->RSSetScissorRects(1, &sRect);

    cmdList->SetPipelineState(m_PsoCheckerboard.Get());

    // Root parameters
    cmdList->SetGraphicsRootDescriptorTable(3, m_CompositeSrvGpus[currentCompositeIdx]);
    cmdList->SetGraphicsRootDescriptorTable(4, m_DummyWhiteSrvGpu);
    cmdList->SetGraphicsRootDescriptorTable(5, m_DummyWhiteSrvGpu);

    cmdList->DrawInstanced(6, 1, 0, 0);

    // 5. Draw selection outline if present
    if (canvas.HasSelection()) {
        const auto& selMask = canvas.GetSelectionMask();
        int sW = canvas.GetWidth();
        int sH = canvas.GetHeight();

        if (!m_SelectionMaskTex || m_SelectionMaskWidth != sW || m_SelectionMaskHeight != sH) {
            if (m_SelectionMaskTex && m_SelectionMaskSrvCpu.ptr != 0) {
                m_FreeFn(m_SelectionMaskSrvCpu, m_SelectionMaskSrvGpu);
            }
            m_SelectionMaskTex = CreateTextureResource(sW, sH, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            m_SelectionMaskWidth = sW;
            m_SelectionMaskHeight = sH;
            m_AllocFn(&m_SelectionMaskSrvCpu, &m_SelectionMaskSrvGpu);

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_R8_UNORM;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            m_Device->CreateShaderResourceView(m_SelectionMaskTex.Get(), &srvDesc, m_SelectionMaskSrvCpu);
        }

        // Row pitch must be aligned to 256 bytes for staging transfers
        int rowPitch = (sW + 255) & ~255;
        int totalBytes = sH * rowPitch;

        auto staging = CreateStagingResource(totalBytes);
        currentPool.resources.push_back(staging);

        void* mapped = nullptr;
        if (SUCCEEDED(staging->Map(0, nullptr, &mapped))) {
            uint8_t* dst = reinterpret_cast<uint8_t*>(mapped);
            for (int y = 0; y < sH; ++y) {
                std::memcpy(dst + y * rowPitch, selMask.data() + y * sW, sW);
            }
            staging->Unmap(0, nullptr);
        }

        TransitionResource(cmdList, m_SelectionMaskTex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

        D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
        srcLocation.pResource = staging.Get();
        srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLocation.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UNORM;
        srcLocation.PlacedFootprint.Footprint.Width = sW;
        srcLocation.PlacedFootprint.Footprint.Height = sH;
        srcLocation.PlacedFootprint.Footprint.Depth = 1;
        srcLocation.PlacedFootprint.Footprint.RowPitch = rowPitch;

        D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
        dstLocation.pResource = m_SelectionMaskTex.Get();
        dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLocation.SubresourceIndex = 0;

        cmdList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
        TransitionResource(cmdList, m_SelectionMaskTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // Render outline
        cmdList->SetPipelineState(m_PsoSelectionOutline.Get());
        cmdList->SetGraphicsRootDescriptorTable(3, m_DummyWhiteSrvGpu);
        cmdList->SetGraphicsRootDescriptorTable(4, m_SelectionMaskSrvGpu);
        cmdList->SetGraphicsRootDescriptorTable(5, m_DummyWhiteSrvGpu);

        cmdList->DrawInstanced(6, 1, 0, 0);
    }

    TransitionResource(cmdList, viewportRT, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // Signal fence
    m_UploadFenceValue++;
    m_CommandQueue->Signal(m_UploadFence.Get(), m_UploadFenceValue);
    currentPool.fenceValue = m_UploadFenceValue;

    return true;
}

void CanvasRendererDX12::UploadDirtyTiles(
    ID3D12GraphicsCommandList* cmdList,
    const TileCache& tileCache,
    LayerGpuResources& gpuRes,
    bool isMask,
    const std::vector<uint8_t>& rawMaskData,
    const std::vector<bool>& maskDirtyTiles,
    int canvasWidth,
    int canvasHeight,
    FrameStagingPool& pool
) {
    int bytesPerPixel = isMask ? 1 : BytesPerPixel(tileCache.GetFormat());
    DXGI_FORMAT format = isMask ? DXGI_FORMAT_R8_UNORM :
        ((tileCache.GetFormat() == CanvasPixelFormat::RGBA8) ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R32G32B32A32_FLOAT);

    int rowPitch = TILE_SIZE * bytesPerPixel;
    int totalBytes = TILE_SIZE * rowPitch;

    auto processUpload = [&](int tx, int ty, const uint8_t* srcData) {
        uint32_t key = (ty << 16) | tx;
        GpuTile& tile = isMask ? gpuRes.maskTiles[key] : gpuRes.albedoTiles[key];

        if (!tile.resource) {
            tile.resource = CreateTextureResource(TILE_SIZE, TILE_SIZE, format);
            m_AllocFn(&tile.srvCpuHandle, &tile.srvGpuHandle);

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = format;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            m_Device->CreateShaderResourceView(tile.resource.Get(), &srvDesc, tile.srvCpuHandle);
        }

        if (m_AsyncUploaderReady) {
            // Async path: copy data and queue upload
            TransitionResource(cmdList, tile.resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

            UploadRequest req;
            req.data.assign(srcData, srcData + totalBytes);
            req.dstResource = tile.resource.Get();
            req.format = format;
            req.width = TILE_SIZE;
            req.height = TILE_SIZE;
            req.onComplete = [&tile, this]() {
                tile.lastAccess = m_AccessCounter;
            };

            tile.uploadFenceValue = m_AsyncUploader.EnqueueUpload(std::move(req));
        } else {
            // Sync path
            auto staging = CreateStagingResource(totalBytes);
            pool.resources.push_back(staging);

            void* mapped = nullptr;
            if (SUCCEEDED(staging->Map(0, nullptr, &mapped))) {
                std::memcpy(mapped, srcData, totalBytes);
                staging->Unmap(0, nullptr);
            }

            TransitionResource(cmdList, tile.resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

            D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
            srcLocation.pResource = staging.Get();
            srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            srcLocation.PlacedFootprint.Footprint.Format = format;
            srcLocation.PlacedFootprint.Footprint.Width = TILE_SIZE;
            srcLocation.PlacedFootprint.Footprint.Height = TILE_SIZE;
            srcLocation.PlacedFootprint.Footprint.Depth = 1;
            srcLocation.PlacedFootprint.Footprint.RowPitch = rowPitch;

            D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
            dstLocation.pResource = tile.resource.Get();
            dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dstLocation.SubresourceIndex = 0;

            cmdList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
            TransitionResource(cmdList, tile.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

            tile.lastAccess = m_AccessCounter;
            tile.uploadFenceValue = 0;
        }
    };

    if (isMask) {
        // Since CPU mask is stored as a flat vector rather than a sparse TileCache, 
        // we synthesize dirty tiles matching the populated albedo list.
        std::vector<uint8_t> localBuffer(TILE_SIZE * TILE_SIZE, 0);

        int tilesX = (canvasWidth + TILE_SIZE - 1) / TILE_SIZE;

        for (auto& [tileKey, albedoTile] : gpuRes.albedoTiles) {
            int tx = tileKey & 0xFFFF;
            int ty = tileKey >> 16;

            int tileIdx = ty * tilesX + tx;
            bool isDirty = maskDirtyTiles.empty() || 
                           (tileIdx >= (int)maskDirtyTiles.size()) || 
                           maskDirtyTiles[tileIdx];
            if (!isDirty) {
                continue;
            }

            for (int y = 0; y < TILE_SIZE; ++y) {
                int srcY = ty * TILE_SIZE + y;
                if (srcY >= canvasHeight) {
                    std::memset(localBuffer.data() + y * TILE_SIZE, 0, TILE_SIZE);
                    continue;
                }
                for (int x = 0; x < TILE_SIZE; ++x) {
                    int srcX = tx * TILE_SIZE + x;
                    if (srcX >= canvasWidth) {
                        localBuffer[y * TILE_SIZE + x] = 0;
                    } else {
                        localBuffer[y * TILE_SIZE + x] = rawMaskData[srcY * canvasWidth + srcX];
                    }
                }
            }
            processUpload(tx, ty, localBuffer.data());
        }
    } else {
        tileCache.ForEachDirtyTile([&](int tx, int ty, const uint8_t* data, int pitch) {
            processUpload(tx, ty, data);
        });
        const_cast<TileCache&>(tileCache).ClearAllDirty();
    }
}

void CanvasRendererDX12::GarbageCollectGpuLayers(const Canvas& canvas) {
    std::unordered_set<const TileCache*> activeCaches;

    for (const auto& layer : const_cast<Canvas&>(canvas).GetLayers()) {
        if (layer.tileCache) activeCaches.insert(layer.tileCache.get());
        if (layer.filteredCache) activeCaches.insert(layer.filteredCache.get());
    }

    if (canvas.IsMovingPixels() && canvas.GetFloatingTileCache()) {
        activeCaches.insert(canvas.GetFloatingTileCache());
    }

    for (auto it = m_GpuLayers.begin(); it != m_GpuLayers.end(); ) {
        if (activeCaches.count(it->first) == 0) {
            for (auto& [key, tile] : it->second.albedoTiles) {
                if (m_AsyncUploaderReady && tile.uploadFenceValue > 0) {
                    uint64_t completedFence = m_AsyncUploader.GetCompletedFenceValue();
                    if (tile.uploadFenceValue > completedFence) {
                        m_AsyncUploader.FlushCompleted(INFINITE);
                    }
                }
                if (tile.srvCpuHandle.ptr != 0) m_FreeFn(tile.srvCpuHandle, tile.srvGpuHandle);
            }
            for (auto& [key, tile] : it->second.maskTiles) {
                if (m_AsyncUploaderReady && tile.uploadFenceValue > 0) {
                    uint64_t completedFence = m_AsyncUploader.GetCompletedFenceValue();
                    if (tile.uploadFenceValue > completedFence) {
                        m_AsyncUploader.FlushCompleted(INFINITE);
                    }
                }
                if (tile.srvCpuHandle.ptr != 0) m_FreeFn(tile.srvCpuHandle, tile.srvGpuHandle);
            }
            it = m_GpuLayers.erase(it);
        } else {
            // Trim individual tiles not present in CPU TileCache
            const TileCache* tc = it->first;
            for (auto tileIt = it->second.albedoTiles.begin(); tileIt != it->second.albedoTiles.end(); ) {
                int tx = tileIt->first & 0xFFFF;
                int ty = tileIt->first >> 16;
                if (!tc->HasTile(tx, ty)) {
                    if (m_AsyncUploaderReady && tileIt->second.uploadFenceValue > 0) {
                        uint64_t completedFence = m_AsyncUploader.GetCompletedFenceValue();
                        if (tileIt->second.uploadFenceValue > completedFence) {
                            m_AsyncUploader.FlushCompleted(INFINITE);
                        }
                    }
                    if (tileIt->second.srvCpuHandle.ptr != 0) m_FreeFn(tileIt->second.srvCpuHandle, tileIt->second.srvGpuHandle);
                    tileIt = it->second.albedoTiles.erase(tileIt);
                } else {
                    ++tileIt;
                }
            }
            ++it;
        }
    }
}

bool CanvasRendererDX12::CreateRootSignatures() {
    D3D12_ROOT_PARAMETER rootParams[6] = {};

    // 0: CBV CanvasBuffer (b0)
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // 1: CBV LayerBuffer (b1)
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 1;
    rootParams[1].Descriptor.RegisterSpace = 0;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // 2: CBV TileParams (b2)
    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[2].Descriptor.ShaderRegister = 2;
    rootParams[2].Descriptor.RegisterSpace = 0;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    // 3: Table SRV Texture (t0)
    D3D12_DESCRIPTOR_RANGE rangesT0 = {};
    rangesT0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rangesT0.NumDescriptors = 1;
    rangesT0.BaseShaderRegister = 0;
    rangesT0.RegisterSpace = 0;
    rangesT0.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[3].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[3].DescriptorTable.pDescriptorRanges = &rangesT0;
    rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // 4: Table SRV LayerMask (t1)
    D3D12_DESCRIPTOR_RANGE rangesT1 = rangesT0;
    rangesT1.BaseShaderRegister = 1;

    rootParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[4].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[4].DescriptorTable.pDescriptorRanges = &rangesT1;
    rootParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // 5: Table SRV Composite (t2)
    D3D12_DESCRIPTOR_RANGE rangesT2 = rangesT0;
    rangesT2.BaseShaderRegister = 2;

    rootParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[5].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[5].DescriptorTable.pDescriptorRanges = &rangesT2;
    rootParams[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Sampler
    D3D12_STATIC_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplerDesc.MipLODBias = 0;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    samplerDesc.MinLOD = 0.0f;
    samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
    samplerDesc.ShaderRegister = 0;
    samplerDesc.RegisterSpace = 0;
    samplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = 6;
    rootSigDesc.pParameters = rootParams;
    rootSigDesc.NumStaticSamplers = 1;
    rootSigDesc.pStaticSamplers = &samplerDesc;
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> serializedRootSig;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    if (FAILED(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serializedRootSig, &errorBlob))) {
        if (errorBlob) {
            Logger::Get().Error("Root signature serialization error: " + std::string(reinterpret_cast<const char*>(errorBlob->GetBufferPointer())));
        }
        return false;
    }

    if (FAILED(m_Device->CreateRootSignature(0, serializedRootSig->GetBufferPointer(), serializedRootSig->GetBufferSize(), IID_PPV_ARGS(&m_RootSignature)))) {
        return false;
    }

    return true;
}

bool CanvasRendererDX12::CreatePipelineStates() {
    Microsoft::WRL::ComPtr<ID3DBlob> vsMainBlob, psMainBlob, vsLayerBlob, psLayerBlob, psOutlineBlob, vsTileBlob, psTileBlob;
    if (!CompileShaders(vsMainBlob, psMainBlob, vsLayerBlob, psLayerBlob, psOutlineBlob, vsTileBlob, psTileBlob)) {
        return false;
    }

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { layout, 2 };
    psoDesc.pRootSignature = m_RootSignature.Get();
    psoDesc.RasterizerState = {};
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.BlendState = {};
    psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    // 1. Viewport / Checkerboard PSO
    psoDesc.VS = { vsMainBlob->GetBufferPointer(), vsMainBlob->GetBufferSize() };
    psoDesc.PS = { psMainBlob->GetBufferPointer(), psMainBlob->GetBufferSize() };
    if (FAILED(m_Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_PsoCheckerboard)))) return false;

    // 2. Layer Blend PSO with hardware Alpha Blending enabled
    psoDesc.VS = { vsTileBlob->GetBufferPointer(), vsTileBlob->GetBufferSize() };
    psoDesc.PS = { psTileBlob->GetBufferPointer(), psTileBlob->GetBufferSize() };
    psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    if (FAILED(m_Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_PsoLayerBlend)))) return false;

    // 3. Selection Outline PSO
    psoDesc.VS = { vsMainBlob->GetBufferPointer(), vsMainBlob->GetBufferSize() };
    psoDesc.PS = { psOutlineBlob->GetBufferPointer(), psOutlineBlob->GetBufferSize() };
    if (FAILED(m_Device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_PsoSelectionOutline)))) return false;

    return true;
}

bool CanvasRendererDX12::CompileShaders(
    Microsoft::WRL::ComPtr<ID3DBlob>& vsMain,
    Microsoft::WRL::ComPtr<ID3DBlob>& psMain,
    Microsoft::WRL::ComPtr<ID3DBlob>& vsLayer,
    Microsoft::WRL::ComPtr<ID3DBlob>& psLayerBlend,
    Microsoft::WRL::ComPtr<ID3DBlob>& psSelectionOutline,
    Microsoft::WRL::ComPtr<ID3DBlob>& vsTile,
    Microsoft::WRL::ComPtr<ID3DBlob>& psTileBlend
) {
    // Helper: wrap raw bytes into an ID3DBlob
    auto makeBlobFromBytes = [](const std::vector<uint8_t>& bytes,
                                Microsoft::WRL::ComPtr<ID3DBlob>& dest) -> bool {
        if (bytes.empty()) return false;
        if (FAILED(D3DCreateBlob(bytes.size(), &dest))) return false;
        std::memcpy(dest->GetBufferPointer(), bytes.data(), bytes.size());
        return true;
    };

    // Try shaders/ subdir first, then working dir
    auto tryCso = [&](const char* name) -> std::vector<uint8_t> {
        auto b = LoadCsoFile(std::string("shaders/") + name);
        if (b.empty()) b = LoadCsoFile(name);
        return b;
    };

    // --- Attempt 1: load precompiled .cso ---
    auto csoVSMain    = tryCso("Canvas_VSMain.cso");
    auto csoPSMain    = tryCso("Canvas_PSMain.cso");
    auto csoVSLayer   = tryCso("Canvas_VSLayerMain.cso");
    auto csoPSLayer   = tryCso("Canvas_PSLayerBlend.cso");
    auto csoPSOutline = tryCso("Canvas_PSSelectionOutline.cso");
    auto csoVSTile    = tryCso("Canvas_VSTileMain.cso");
    auto csoPSTile    = tryCso("Canvas_PSTileBlend.cso");

    bool allFound = !csoVSMain.empty() && !csoPSMain.empty() &&
                    !csoVSLayer.empty() && !csoPSLayer.empty() &&
                    !csoPSOutline.empty() && !csoVSTile.empty() && !csoPSTile.empty();

    if (allFound) {
        Logger::Get().Info("Loading precompiled shader .cso files.");
        if (makeBlobFromBytes(csoVSMain,    vsMain)              &&
            makeBlobFromBytes(csoPSMain,    psMain)              &&
            makeBlobFromBytes(csoVSLayer,   vsLayer)             &&
            makeBlobFromBytes(csoPSLayer,   psLayerBlend)        &&
            makeBlobFromBytes(csoPSOutline, psSelectionOutline)  &&
            makeBlobFromBytes(csoVSTile,    vsTile)              &&
            makeBlobFromBytes(csoPSTile,    psTileBlend)) {
            Logger::Get().Info("All precompiled shaders loaded successfully.");
            return true;
        }
        Logger::Get().Error(".cso blob wrap failed — falling back to runtime compile.");
    } else {
        Logger::Get().Info("Precompiled .cso not found — runtime compile fallback.");
    }

    // --- Attempt 2: Runtime D3DCompile ---
    std::string canvasHlsl;
    std::ifstream canvasFile("shaders/Canvas.hlsl");
    if (!canvasFile.is_open()) canvasFile.open("Canvas.hlsl");
    if (canvasFile.is_open()) {
        std::stringstream ss; ss << canvasFile.rdbuf();
        canvasHlsl = ss.str();
        Logger::Get().Info("Loaded Canvas.hlsl from disk for runtime compile.");
    } else {
        canvasHlsl = GetEmbeddedHLSL();
        Logger::Get().Info("Using embedded Canvas.hlsl fallback.");
    }

    std::string tileHlsl;
    std::ifstream tileFile("shaders/CanvasTiles.hlsl");
    if (!tileFile.is_open()) tileFile.open("CanvasTiles.hlsl");
    if (tileFile.is_open()) {
        std::stringstream ss; ss << tileFile.rdbuf();
        tileHlsl = ss.str();
        Logger::Get().Info("Loaded CanvasTiles.hlsl from disk for runtime compile.");
    } else {
        tileHlsl = GetTileShadersHLSL();
        Logger::Get().Info("Using embedded CanvasTiles fallback.");
    }

    std::string combined = canvasHlsl + "\n\n" + tileHlsl;

    auto compile = [&](const char* entry, const char* target,
                       Microsoft::WRL::ComPtr<ID3DBlob>& dest) -> bool {
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        Microsoft::WRL::ComPtr<ID3DBlob> err;
        HRESULT hr = D3DCompile(combined.data(), combined.size(),
            nullptr, nullptr, nullptr, entry, target, flags, 0, &dest, &err);
        if (FAILED(hr)) {
            if (err) Logger::Get().Error("Shader compile error (" + std::string(entry) + "): " +
                std::string(reinterpret_cast<const char*>(err->GetBufferPointer())));
            return false;
        }
        return true;
    };

    if (!compile("VSMain",             "vs_5_0", vsMain))            return false;
    if (!compile("PSMain",             "ps_5_0", psMain))            return false;
    if (!compile("VSLayerMain",        "vs_5_0", vsLayer))           return false;
    if (!compile("PSLayerBlend",       "ps_5_0", psLayerBlend))      return false;
    if (!compile("PSSelectionOutline", "ps_5_0", psSelectionOutline)) return false;
    if (!compile("VSTileMain",         "vs_5_0", vsTile))            return false;
    if (!compile("PSTileBlend",        "ps_5_0", psTileBlend))       return false;

    return true;
}

std::vector<uint8_t> CanvasRendererDX12::LoadCsoFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return {};
    auto size = f.tellg();
    if (size <= 0) return {};
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    if (!f.read(reinterpret_cast<char*>(buf.data()), size)) return {};
    return buf;
}


bool CanvasRendererDX12::CreateConstantBuffers() {
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = MAX_CB_SIZE;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(m_Device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_ConstantBufferUpload)
    ))) {
        return false;
    }

    if (FAILED(m_ConstantBufferUpload->Map(0, nullptr, reinterpret_cast<void**>(&m_CbMappedData)))) {
        return false;
    }

    return true;
}

bool CanvasRendererDX12::CreateDummyResources() {
    m_DummyWhiteTex = CreateTextureResource(1, 1, DXGI_FORMAT_R8G8B8A8_UNORM);
    m_AllocFn(&m_DummyWhiteSrvCpu, &m_DummyWhiteSrvGpu);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    m_Device->CreateShaderResourceView(m_DummyWhiteTex.Get(), &srvDesc, m_DummyWhiteSrvCpu);

    m_DummyBlackTex = CreateTextureResource(1, 1, DXGI_FORMAT_R8G8B8A8_UNORM);
    m_AllocFn(&m_DummyBlackSrvCpu, &m_DummyBlackSrvGpu);
    m_Device->CreateShaderResourceView(m_DummyBlackTex.Get(), &srvDesc, m_DummyBlackSrvCpu);

    // Initialise resources using temporary graphics execution pass
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmdList;
    if (FAILED(m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) return false;
    if (FAILED(m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&cmdList)))) return false;

    // Staging buffer: 2 × 256-byte aligned slots (DX12 min row pitch = 256)
    auto staging = CreateStagingResource(512);
    void* mapped = nullptr;
    if (SUCCEEDED(staging->Map(0, nullptr, &mapped))) {
        uint8_t* buf = reinterpret_cast<uint8_t*>(mapped);
        // White pixel at aligned offset 0
        uint32_t white = 0xFFFFFFFF;
        std::memcpy(buf, &white, 4);
        // Black pixel at aligned offset 256
        uint32_t black = 0xFF000000;
        std::memcpy(buf + 256, &black, 4);
        staging->Unmap(0, nullptr);
    }

    TransitionResource(cmdList.Get(), m_DummyWhiteTex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    TransitionResource(cmdList.Get(), m_DummyBlackTex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = staging.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srcLoc.PlacedFootprint.Footprint.Width = 1;
    srcLoc.PlacedFootprint.Footprint.Height = 1;
    srcLoc.PlacedFootprint.Footprint.Depth = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch = 256; // Standard DX12 copy pitch constraint

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = m_DummyWhiteTex.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    srcLoc.PlacedFootprint.Offset = 256; // Next aligned segment
    dstLoc.pResource = m_DummyBlackTex.Get();
    cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    TransitionResource(cmdList.Get(), m_DummyWhiteTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    TransitionResource(cmdList.Get(), m_DummyBlackTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList.Get() };
    m_CommandQueue->ExecuteCommandLists(1, lists);

    // Command Queue Fence synchronisation
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE fEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    m_CommandQueue->Signal(fence.Get(), 1);
    fence->SetEventOnCompletion(1, fEvent);
    WaitForSingleObject(fEvent, INFINITE);
    CloseHandle(fEvent);

    return true;
}

bool CanvasRendererDX12::CreateQuadVertexBuffer() {
    Vertex vertices[] = {
        { { 0.0f, 0.0f }, { 0.0f, 0.0f } },
        { { 1.0f, 0.0f }, { 1.0f, 0.0f } },
        { { 0.0f, 1.0f }, { 0.0f, 1.0f } },
        { { 0.0f, 1.0f }, { 0.0f, 1.0f } },
        { { 1.0f, 0.0f }, { 1.0f, 0.0f } },
        { { 1.0f, 1.0f }, { 1.0f, 1.0f } },
    };

    size_t size = sizeof(vertices);
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(m_Device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_QuadVertexBuffer)
    ))) {
        return false;
    }

    auto staging = CreateStagingResource(size);
    void* mapped = nullptr;
    if (SUCCEEDED(staging->Map(0, nullptr, &mapped))) {
        std::memcpy(mapped, vertices, size);
        staging->Unmap(0, nullptr);
    }

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cmdList;
    if (FAILED(m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) return false;
    if (FAILED(m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&cmdList)))) return false;

    cmdList->CopyBufferRegion(m_QuadVertexBuffer.Get(), 0, staging.Get(), 0, size);
    TransitionResource(cmdList.Get(), m_QuadVertexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList.Get() };
    m_CommandQueue->ExecuteCommandLists(1, lists);

    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE fEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    m_CommandQueue->Signal(fence.Get(), 1);
    fence->SetEventOnCompletion(1, fEvent);
    WaitForSingleObject(fEvent, INFINITE);
    CloseHandle(fEvent);

    m_QuadVertexBufferView.BufferLocation = m_QuadVertexBuffer->GetGPUVirtualAddress();
    m_QuadVertexBufferView.SizeInBytes = static_cast<UINT>(size);
    m_QuadVertexBufferView.StrideInBytes = sizeof(Vertex);

    return true;
}

Microsoft::WRL::ComPtr<ID3D12Resource> CanvasRendererDX12::CreateTextureResource(
    int width,
    int height,
    DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initialState
) {
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = static_cast<UINT64>(width);
    desc.Height = static_cast<UINT>(height);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = format;
    clearValue.Color[0] = 0.0f;
    clearValue.Color[1] = 0.0f;
    clearValue.Color[2] = 0.0f;
    clearValue.Color[3] = 0.0f;

    Microsoft::WRL::ComPtr<ID3D12Resource> res;
    m_Device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        initialState,
        (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ? &clearValue : nullptr,
        IID_PPV_ARGS(&res)
    );
    return res;
}

Microsoft::WRL::ComPtr<ID3D12Resource> CanvasRendererDX12::CreateStagingResource(size_t size) {
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<ID3D12Resource> res;
    m_Device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&res)
    );
    return res;
}

void CanvasRendererDX12::TransitionResource(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES stateBefore,
    D3D12_RESOURCE_STATES stateAfter
) {
    if (stateBefore == stateAfter || !resource) return;
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = stateBefore;
    barrier.Transition.StateAfter = stateAfter;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);
}

uint32_t CanvasRendererDX12::AllocateConstantBufferSpace(const void* data, uint32_t size) {
    uint32_t alignedOffset = (m_CbOffset + 255) & ~255;
    uint32_t requiredEnd = alignedOffset + size;

    if (requiredEnd > MAX_CB_SIZE) {
        // Ring wrap: reset to beginning
        Logger::Get().Error("CB ring overflow — wrapping. Consider increasing MAX_CB_SIZE.");
        alignedOffset = 0;
        requiredEnd = size;
        if (requiredEnd > MAX_CB_SIZE) {
            Logger::Get().Error("Single CB allocation exceeds MAX_CB_SIZE — skipping.");
            return 0;
        }
    }

    assert((alignedOffset % 256) == 0 && "CBV must be 256-byte aligned");

    std::memcpy(m_CbMappedData + alignedOffset, data, size);
    m_CbOffset = requiredEnd;
    return alignedOffset;
}

uint32_t CanvasRendererDX12::UpdateCanvasBuffer(
    ID3D12GraphicsCommandList* cmdList,
    const Canvas& canvas,
    int viewportWidth,
    int viewportHeight
) {
    CanvasBufferData data = {};
    data.viewportSizeAndZoom = DirectX::XMFLOAT4(
        static_cast<float>(viewportWidth), static_cast<float>(viewportHeight),
        canvas.GetZoom(), canvas.GetRotationAngle()
    );
    data.offsetAndCanvasSize = DirectX::XMFLOAT4(
        canvas.GetPan().x, canvas.GetPan().y,
        static_cast<float>(canvas.GetWidth()), static_cast<float>(canvas.GetHeight())
    );
    data.channelMasksAndFlags = DirectX::XMFLOAT4(
        canvas.GetChannelR() ? 1.0f : 0.0f,
        canvas.GetChannelG() ? 1.0f : 0.0f,
        canvas.GetChannelB() ? 1.0f : 0.0f,
        canvas.GetChannelA() ? 1.0f : 0.0f
    );
    data.viewportFlags = DirectX::XMFLOAT4(
        canvas.GetViewportFlipH() ? 1.0f : 0.0f,
        canvas.GetViewportFlipV() ? 1.0f : 0.0f,
        static_cast<float>(ImGui::GetTime()),
        0.0f
    );
    uint32_t offset = AllocateConstantBufferSpace(&data, sizeof(data));
    cmdList->SetGraphicsRootConstantBufferView(0, m_ConstantBufferUpload->GetGPUVirtualAddress() + offset);
    return offset;
}

uint32_t CanvasRendererDX12::UpdateLayerBuffer(
    ID3D12GraphicsCommandList* cmdList,
    float opacity,
    bool hasMask,
    float uOff, float vOff,
    float scaleX, float scaleY,
    float rotation,
    bool isFloating,
    float centerX, float centerY,
    BlendMode blendMode
) {
    LayerBufferData data = {};
    data.layerParams    = DirectX::XMFLOAT4(opacity, hasMask ? 1.0f : 0.0f, uOff, vOff);
    data.transformParams= DirectX::XMFLOAT4(scaleX, scaleY, rotation, isFloating ? 1.0f : 0.0f);
    data.centerParams   = DirectX::XMFLOAT4(centerX, centerY, static_cast<float>(blendMode), 0.0f);
    uint32_t offset = AllocateConstantBufferSpace(&data, sizeof(data));
    cmdList->SetGraphicsRootConstantBufferView(1, m_ConstantBufferUpload->GetGPUVirtualAddress() + offset);
    return offset;
}

// Inline Fallbacks of complete Canvas.hlsl resource to prevent application startup crashes if asset missing.
std::string CanvasRendererDX12::GetEmbeddedHLSL() const {
    return R"hlsl(
struct VS_INPUT
{
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
    float2 screenPos : TEXCOORD1;
};

cbuffer CanvasBuffer : register(b0)
{
    float4 u_ViewportSizeAndZoom; // xy: Viewport size, z: Zoom, w: rotation
    float4 u_OffsetAndCanvasSize; // xy: Offset/Pan, zw: Canvas size
    float4 u_ChannelMasksAndFlags; // x:R, y:G, z:B, w:A
    float4 u_ViewportFlags; // x: flipH, y: flipV, z: outlineTime, w: unused
};

cbuffer LayerBuffer : register(b1)
{
    float4 u_LayerParams;     // x: opacity, y: hasMask, zw: translation
    float4 u_TransformParams; // x: scaleX, y: scaleY, z: rotation, w: isFloating
    float4 u_CenterParams;    // x: centerX, y: centerY, z: blendMode, w: unused
};

Texture2D g_Texture : register(t0);
SamplerState g_Sampler : register(s0);

PS_INPUT VSMain(VS_INPUT input)
{
    PS_INPUT output;
    float2 viewportSize = u_ViewportSizeAndZoom.xy;
    float zoom = u_ViewportSizeAndZoom.z;
    float rotation = u_ViewportSizeAndZoom.w;
    float2 panOffset = u_OffsetAndCanvasSize.xy;
    float2 canvasSize = u_OffsetAndCanvasSize.zw;
    
    float2 canvasPixelPos = input.pos * canvasSize;
    if (u_ViewportFlags.x > 0.5f) canvasPixelPos.x = canvasSize.x - canvasPixelPos.x;
    if (u_ViewportFlags.y > 0.5f) canvasPixelPos.y = canvasSize.y - canvasPixelPos.y;
    
    float2 center = canvasSize * 0.5f;
    float2 rel = canvasPixelPos - center;
    float cosA = cos(rotation);
    float sinA = sin(rotation);
    float2 rotatedPixelPos;
    rotatedPixelPos.x = rel.x * cosA - rel.y * sinA;
    rotatedPixelPos.y = rel.x * sinA + rel.y * cosA;
    rotatedPixelPos += center;
    
    float2 screenOrigin = floor(panOffset + viewportSize * 0.5f);
    float2 screenPixelPos = floor(rotatedPixelPos * zoom) + screenOrigin;
    
    float2 ndcPos = (screenPixelPos / viewportSize) * 2.0f - 1.0f;
    ndcPos.y = -ndcPos.y;
    
    output.pos = float4(ndcPos, 0.0f, 1.0f);
    output.uv = input.uv;
    output.screenPos = screenPixelPos;
    return output;
}

PS_INPUT VSLayerMain(VS_INPUT input)
{
    PS_INPUT output;
    float2 ndcPos;
    ndcPos.x = input.pos.x * 2.0f - 1.0f;
    ndcPos.y = 1.0f - input.pos.y * 2.0f;
    output.pos = float4(ndcPos, 0.0f, 1.0f);
    output.uv = input.uv;
    output.screenPos = float2(0.0f, 0.0f);
    return output;
}

float4 PSMain(PS_INPUT input) : SV_TARGET
{
    float2 canvasCoords = input.uv * u_OffsetAndCanvasSize.zw;
    float cellSize = 16.0f;
    int2 cellIndex = (int2)floor(canvasCoords / cellSize);
    int sum = cellIndex.x + cellIndex.y;
    if (sum < 0) sum = -sum;
    
    float check = (sum % 2 == 0) ? 1.0f : 0.0f;
    float3 color1 = float3(0.18f, 0.18f, 0.18f);
    float3 color2 = float3(0.24f, 0.24f, 0.24f);
    float3 checkColor = lerp(color1, color2, check);
    
    float4 texCol = g_Texture.Sample(g_Sampler, input.uv);
    bool r = u_ChannelMasksAndFlags.x > 0.5f;
    bool g = u_ChannelMasksAndFlags.y > 0.5f;
    bool b = u_ChannelMasksAndFlags.z > 0.5f;
    bool a = u_ChannelMasksAndFlags.w > 0.5f;
    
    float3 finalColor = checkColor;
    int activeCount = (r ? 1 : 0) + (g ? 1 : 0) + (b ? 1 : 0) + (a ? 1 : 0);
    
    if (activeCount == 1) {
        float val = r ? texCol.r : (g ? texCol.g : (b ? texCol.b : texCol.a));
        finalColor = float3(val, val, val);
    } else {
        float3 rgb = float3(r ? texCol.r : 0.0f, g ? texCol.g : 0.0f, b ? texCol.b : 0.0f);
        finalColor = a ? lerp(checkColor, rgb, texCol.a) : rgb;
    }
    
    float2 pixelDist = min(input.uv, 1.0f - input.uv) * u_OffsetAndCanvasSize.zw;
    float distToEdge = min(pixelDist.x, pixelDist.y);
    float borderThreshold = 1.0f / u_ViewportSizeAndZoom.z;
    if (distToEdge < borderThreshold) finalColor = float3(0.5f, 0.5f, 0.5f);
    
    return float4(finalColor, 1.0f);
}

Texture2D g_LayerMask : register(t1);
Texture2D g_Composite : register(t2);

float4 PSLayerBlend(PS_INPUT input) : SV_TARGET
{
    float2 uv = input.uv;
    if (u_TransformParams.w > 0.5f) {
        float2 center = u_CenterParams.xy;
        float2 rel = uv - center;
        rel -= u_LayerParams.zw;
        float angle = -u_TransformParams.z;
        float cosA = cos(angle);
        float sinA = sin(angle);
        float2 rotated = float2(rel.x * cosA - rel.y * sinA, rel.x * sinA + rel.y * cosA);
        float2 scale = u_TransformParams.xy;
        if (scale.x > 0.0001f && scale.y > 0.0001f) rotated /= scale;
        uv = rotated + center;
    } else {
        uv -= u_LayerParams.zw;
    }
    if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) {
        discard;
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    
    float4 col = g_Texture.Sample(g_Sampler, uv);
    if (u_ChannelMasksAndFlags.w < 0.5f) col.a = 1.0f;
    col.a *= u_LayerParams.x;
    
    if (u_LayerParams.y > 0.5f) {
        col.a *= g_LayerMask.Sample(g_Sampler, uv).r;
    }
    return col;
}

Texture2D g_SelectionMask : register(t1);

float4 PSSelectionOutline(PS_INPUT input) : SV_TARGET
{
    float mask = g_SelectionMask.Sample(g_Sampler, input.uv).r;
    float2 canvasSize = u_OffsetAndCanvasSize.zw;
    float zoom = u_ViewportSizeAndZoom.z;
    float2 uvStep = 1.0f / (canvasSize * zoom);
    
    float mLeft  = g_SelectionMask.Sample(g_Sampler, input.uv - float2(uvStep.x, 0.0f)).r;
    float mRight = g_SelectionMask.Sample(g_Sampler, input.uv + float2(uvStep.x, 0.0f)).r;
    float mUp    = g_SelectionMask.Sample(g_Sampler, input.uv - float2(0.0f, uvStep.y)).r;
    float mDown  = g_SelectionMask.Sample(g_Sampler, input.uv + float2(0.0f, uvStep.y)).r;
    
    if (abs(mask - mLeft) > 0.1f || abs(mask - mRight) > 0.1f || abs(mask - mUp) > 0.1f || abs(mask - mDown) > 0.1f) {
        float time = u_ViewportFlags.z;
        float dash = (input.screenPos.x + input.screenPos.y - time * 30.0f) % 10.0f;
        if (dash < 0.0f) dash += 10.0f;
        float3 col = (dash < 5.0f) ? float3(0.0f, 0.0f, 0.0f) : float3(1.0f, 1.0f, 1.0f);
        return float4(col, 1.0f);
    }
    discard;
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
}
)hlsl";
}

std::string CanvasRendererDX12::GetTileShadersHLSL() const {
    return R"hlsl(
cbuffer TileParams : register(b2)
{
    float4 u_TileParams; // x: tileX, y: tileY, z: canvasWidth, w: canvasHeight
};

PS_INPUT VSTileMain(VS_INPUT input)
{
    PS_INPUT output;
    float tileX = u_TileParams.x;
    float tileY = u_TileParams.y;
    float canvasWidth = u_TileParams.z;
    float canvasHeight = u_TileParams.w;
    
    // Position quad vertices in direct pixel coordinates mapping to the canvas bounds
    float2 pixelPos = float2(tileX * 256.0f + input.pos.x * 256.0f,
                             tileY * 256.0f + input.pos.y * 256.0f);
                             
    // If this is a floating layer, apply rotation, scale, and translation around the center pivot (u_CenterParams.xy)
    if (u_TransformParams.w > 0.5f) {
        float2 center = u_CenterParams.xy * float2(canvasWidth, canvasHeight);
        float2 rel = pixelPos - center;
        
        // 1. Scale
        rel *= u_TransformParams.xy;
        
        // 2. Rotate
        float angle = u_TransformParams.z;
        float cosA = cos(angle);
        float sinA = sin(angle);
        float2 rotated;
        rotated.x = rel.x * cosA - rel.y * sinA;
        rotated.y = rel.x * sinA + rel.y * cosA;
        
        // 3. Translate (u_LayerParams.zw is normalized translation offset)
        float2 translation = u_LayerParams.zw * float2(canvasWidth, canvasHeight);
        
        pixelPos = rotated + center + translation;
    }

    float2 ndc = (pixelPos / float2(canvasWidth, canvasHeight)) * 2.0f - 1.0f;
    ndc.y = -ndc.y; // DirectX Y inversion
    
    output.pos = float4(ndc, 0.0f, 1.0f);
    output.uv = input.uv;
    output.screenPos = pixelPos;
    return output;
}

float4 PSTileBlend(PS_INPUT input) : SV_TARGET
{
    // input.uv maps perfectly to the 256x256 GPU albedo tile texture [0..1]
    // Translate screenPos back to composite UV coordinates to read background composite albedo
    float2 canvasUV = input.screenPos / u_OffsetAndCanvasSize.zw;
    
    float4 col = g_Texture.Sample(g_Sampler, input.uv);
    if (u_ChannelMasksAndFlags.w < 0.5f) {
        col.a = 1.0f;
    }
    col.a *= u_LayerParams.x; // Multiply active alpha by layer opacity
    
    if (u_LayerParams.y > 0.5f) {
        float maskVal = g_LayerMask.Sample(g_Sampler, input.uv).r;
        col.a *= maskVal;
    }
    
    uint blendMode = (uint)(u_CenterParams.z + 0.5f);
    if (blendMode > 0u) {
        float4 dst = g_Composite.Sample(g_Sampler, canvasUV);
        float3 s = col.rgb;
        float3 d = dst.rgb;
        float3 result = s;
        
        if (blendMode == 1u)      result = s * d;
        else if (blendMode == 2u) result = 1.0f - (1.0f - s) * (1.0f - d);
        else if (blendMode == 3u) {
            result.r = (d.r < 0.5f) ? 2.0f*s.r*d.r : 1.0f - 2.0f*(1.0f-s.r)*(1.0f-d.r);
            result.g = (d.g < 0.5f) ? 2.0f*s.g*d.g : 1.0f - 2.0f*(1.0f-s.g)*(1.0f-d.g);
            result.b = (d.b < 0.5f) ? 2.0f*s.b*d.b : 1.0f - 2.0f*(1.0f-s.b)*(1.0f-d.b);
        }
        else if (blendMode == 4u) result = min(s + d, 1.0f);
        else if (blendMode == 5u) result = max(d - s, 0.0f);
        else if (blendMode == 6u) result = min(s, d);
        else if (blendMode == 7u) result = max(s, d);
        else if (blendMode == 8u) {
            result.r = (s.r < 0.5f) ? 2.0f*s.r*d.r : 1.0f - 2.0f*(1.0f-s.r)*(1.0f-d.r);
            result.g = (s.g < 0.5f) ? 2.0f*s.g*d.g : 1.0f - 2.0f*(1.0f-s.g)*(1.0f-d.g);
            result.b = (s.b < 0.5f) ? 2.0f*s.b*d.b : 1.0f - 2.0f*(1.0f-s.b)*(1.0f-d.b);
        }
        else if (blendMode == 9u) {
            result.r = (s.r < 0.5f) ? d.r - (1.0f-2.0f*s.r)*d.r*(1.0f-d.r) : d.r + (2.0f*s.r-1.0f)*(sqrt(d.r)-d.r);
            result.g = (s.g < 0.5f) ? d.g - (1.0f-2.0f*s.g)*d.g*(1.0f-d.g) : d.g + (2.0f*s.g-1.0f)*(sqrt(d.g)-d.g);
            result.b = (s.b < 0.5f) ? d.b - (1.0f-2.0f*s.b)*d.b*(1.0f-d.b) : d.b + (2.0f*s.b-1.0f)*(sqrt(d.b)-d.b);
        }
        col.rgb = result;
    }
    return col;
}
)hlsl";
}