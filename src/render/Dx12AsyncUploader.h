#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <queue>

struct UploadRequest {
    // Данные для загрузки
    std::vector<uint8_t> data;    // копия данных тайла
    
    // Целевой ресурс
    ID3D12Resource* dstResource = nullptr;  // raw ptr — lifetime гарантирован вызывающим
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
    
    // Callback после завершения upload
    std::function<void()> onComplete;

    // Внутреннее поле для отслеживания готовности
    uint64_t fenceValue = 0;
};

class Dx12AsyncUploader {
public:
    Dx12AsyncUploader() = default;
    ~Dx12AsyncUploader() { Shutdown(); }

    Dx12AsyncUploader(const Dx12AsyncUploader&) = delete;
    Dx12AsyncUploader& operator=(const Dx12AsyncUploader&) = delete;

    [[nodiscard]] bool Initialize(ID3D12Device* device, ID3D12CommandQueue* mainQueue);
    void Shutdown();

    // Поставить в очередь загрузку тайла. Thread-safe.
    // dstResource должен быть переведен в D3D12_RESOURCE_STATE_COMMON до вызова.
    // Возвращает fence value, сигнализирующий о завершении копирования.
    uint64_t EnqueueUpload(UploadRequest request);

    // Вызывать из main thread каждый кадр.
    // Ждёт завершения uploads нужных для текущего кадра (с timeout).
    // Возвращает количество завершённых uploads с прошлого кадра.
    uint32_t FlushCompleted(uint32_t timeoutMs = 0);

    // GPU fence value который сигнализирует завершение всех текущих uploads
    uint64_t GetCompletedFenceValue() const { 
        return m_UploadFence ? m_UploadFence->GetCompletedValue() : 0; 
    }

private:
    void UploadThreadFunc();
    void ExecuteUploadBatch(std::vector<UploadRequest>& batch);

    ID3D12Device* m_Device = nullptr;
    ID3D12CommandQueue* m_MainQueue = nullptr;

    // Copy command queue (приоритет ниже чем Direct)
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>       m_CopyQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>   m_CopyAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_CopyList;  // D3D12_COMMAND_LIST_TYPE_COPY

    // Cross-queue fence для синхронизации
    Microsoft::WRL::ComPtr<ID3D12Fence> m_UploadFence;
    std::atomic<uint64_t> m_UploadFenceValue{0};
    HANDLE m_FenceEvent = nullptr;

    // Upload thread
    std::thread m_UploadThread;
    std::atomic<bool> m_Running{false};

    // Request queue
    std::mutex m_QueueMutex;
    std::condition_variable m_QueueCV;
    std::queue<UploadRequest> m_PendingRequests;

    // Completed callbacks (main thread collects and executes these)
    std::mutex m_CompletedMutex;
    std::vector<std::function<void()>> m_CompletedCallbacks;
};
