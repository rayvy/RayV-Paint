#include "Dx12AsyncUploader.h"
#include "Logger.h"
#include <algorithm>
#include <cassert>

bool Dx12AsyncUploader::Initialize(ID3D12Device* device, ID3D12CommandQueue* mainQueue) {
    m_Device = device;
    m_MainQueue = mainQueue;

    // Create Copy Command Queue
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;

    HRESULT hr = m_Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_CopyQueue));
    if (FAILED(hr)) {
        Logger::Get().Error("Dx12AsyncUploader: Failed to create copy command queue.");
        return false;
    }

    // Create Command Allocator for COPY
    hr = m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&m_CopyAllocator));
    if (FAILED(hr)) {
        Logger::Get().Error("Dx12AsyncUploader: Failed to create copy command allocator.");
        return false;
    }

    // Create Graphics Command List for COPY
    hr = m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, m_CopyAllocator.Get(), nullptr, IID_PPV_ARGS(&m_CopyList));
    if (FAILED(hr)) {
        Logger::Get().Error("Dx12AsyncUploader: Failed to create copy command list.");
        return false;
    }
    m_CopyList->Close(); // Close initially

    // Create fence
    hr = m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_UploadFence));
    if (FAILED(hr)) {
        Logger::Get().Error("Dx12AsyncUploader: Failed to create fence.");
        return false;
    }

    m_FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_FenceEvent) {
        Logger::Get().Error("Dx12AsyncUploader: Failed to create fence event.");
        return false;
    }

    m_Running = true;
    m_UploadThread = std::thread(&Dx12AsyncUploader::UploadThreadFunc, this);

    Logger::Get().Info("Dx12AsyncUploader initialized successfully.");
    return true;
}

void Dx12AsyncUploader::Shutdown() {
    if (m_Running) {
        m_Running = false;
        m_QueueCV.notify_all();
        if (m_UploadThread.joinable()) {
            m_UploadThread.join();
        }
    }

    if (m_FenceEvent) {
        CloseHandle(m_FenceEvent);
        m_FenceEvent = nullptr;
    }

    m_CopyList.Reset();
    m_CopyAllocator.Reset();
    m_CopyQueue.Reset();
    m_UploadFence.Reset();

    m_Device = nullptr;
    m_MainQueue = nullptr;
}

uint64_t Dx12AsyncUploader::EnqueueUpload(UploadRequest request) {
    if (!m_Running) return 0;
    std::lock_guard<std::mutex> lock(m_QueueMutex);
    m_UploadFenceValue++;
    request.fenceValue = m_UploadFenceValue.load();
    m_PendingRequests.push(std::move(request));
    m_QueueCV.notify_one();
    return request.fenceValue;
}

uint32_t Dx12AsyncUploader::FlushCompleted(uint32_t timeoutMs) {
    if (timeoutMs > 0 && m_UploadFence) {
        uint64_t targetFence = m_UploadFenceValue.load();
        if (m_UploadFence->GetCompletedValue() < targetFence) {
            HANDLE tempEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (tempEvent) {
                m_UploadFence->SetEventOnCompletion(targetFence, tempEvent);
                WaitForSingleObject(tempEvent, timeoutMs);
                CloseHandle(tempEvent);
            }
        }
    }

    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(m_CompletedMutex);
        callbacks = std::move(m_CompletedCallbacks);
        m_CompletedCallbacks.clear();
    }

    for (auto& cb : callbacks) {
        if (cb) {
            cb();
        }
    }

    return static_cast<uint32_t>(callbacks.size());
}

void Dx12AsyncUploader::UploadThreadFunc() {
    while (m_Running) {
        std::vector<UploadRequest> batch;
        {
            std::unique_lock<std::mutex> lock(m_QueueMutex);
            m_QueueCV.wait(lock, [this]() { return !m_Running || !m_PendingRequests.empty(); });
            if (!m_Running && m_PendingRequests.empty()) {
                break;
            }
            while (!m_PendingRequests.empty()) {
                batch.push_back(std::move(m_PendingRequests.front()));
                m_PendingRequests.pop();
            }
        }

        if (!batch.empty()) {
            ExecuteUploadBatch(batch);
        }
    }
}

void Dx12AsyncUploader::ExecuteUploadBatch(std::vector<UploadRequest>& batch) {
    if (batch.empty()) return;

    // Reset allocator and command list
    m_CopyAllocator->Reset();
    m_CopyList->Reset(m_CopyAllocator.Get(), nullptr);

    // List of staging resources to keep alive until GPU is done
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> stagingResources;
    stagingResources.reserve(batch.size());

    uint64_t batchFenceValue = 0;

    for (auto& req : batch) {
        if (!req.dstResource) continue;

        int bytesPerPixel = 1;
        if (req.format == DXGI_FORMAT_R8G8B8A8_UNORM) {
            bytesPerPixel = 4;
        } else if (req.format == DXGI_FORMAT_R32G32B32A32_FLOAT) {
            bytesPerPixel = 16;
        } else if (req.format == DXGI_FORMAT_R8_UNORM) {
            bytesPerPixel = 1;
        }

        uint32_t rowPitch = req.width * bytesPerPixel;
        uint32_t totalBytes = req.height * rowPitch;

        // Create staging resource
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = totalBytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        Microsoft::WRL::ComPtr<ID3D12Resource> staging;
        HRESULT hr = m_Device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&staging)
        );

        if (FAILED(hr)) {
            Logger::Get().Error("Dx12AsyncUploader: Failed to create staging resource.");
            continue;
        }

        void* mapped = nullptr;
        if (SUCCEEDED(staging->Map(0, nullptr, &mapped))) {
            std::memcpy(mapped, req.data.data(), std::min(static_cast<size_t>(totalBytes), req.data.size()));
            staging->Unmap(0, nullptr);
        }

        stagingResources.push_back(staging);

        // Transition: COMMON -> COPY_DEST
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = req.dstResource;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_CopyList->ResourceBarrier(1, &barrier);

        D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
        srcLocation.pResource = staging.Get();
        srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLocation.PlacedFootprint.Footprint.Format = req.format;
        srcLocation.PlacedFootprint.Footprint.Width = req.width;
        srcLocation.PlacedFootprint.Footprint.Height = req.height;
        srcLocation.PlacedFootprint.Footprint.Depth = 1;
        srcLocation.PlacedFootprint.Footprint.RowPitch = rowPitch;

        D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
        dstLocation.pResource = req.dstResource;
        dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLocation.SubresourceIndex = 0;

        m_CopyList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);

        // Transition: COPY_DEST -> COMMON
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        m_CopyList->ResourceBarrier(1, &barrier);

        if (req.fenceValue > batchFenceValue) {
            batchFenceValue = req.fenceValue;
        }
    }

    m_CopyList->Close();

    // Execute
    ID3D12CommandList* lists[] = { m_CopyList.Get() };
    m_CopyQueue->ExecuteCommandLists(1, lists);

    // Signal fence
    m_CopyQueue->Signal(m_UploadFence.Get(), batchFenceValue);

    // Wait on fence before freeing staging buffers and calling completed callbacks
    if (m_UploadFence->GetCompletedValue() < batchFenceValue) {
        m_UploadFence->SetEventOnCompletion(batchFenceValue, m_FenceEvent);
        WaitForSingleObject(m_FenceEvent, INFINITE);
    }

    // Now copy has finished on GPU. Move callbacks to completed callbacks list
    std::lock_guard<std::mutex> lock(m_CompletedMutex);
    for (auto& req : batch) {
        if (req.onComplete) {
            m_CompletedCallbacks.push_back(std::move(req.onComplete));
        }
    }
}
