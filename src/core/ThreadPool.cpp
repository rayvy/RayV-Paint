#include "ThreadPool.h"
#include "Logger.h"

ThreadPool& ThreadPool::Get() {
    static ThreadPool instance;
    return instance;
}

ThreadPool::~ThreadPool() {
    Shutdown();
}

void ThreadPool::Init(size_t threads) {
    std::unique_lock<std::mutex> lock(m_QueueMutex);
    if (m_Initialized) return;

    m_Stop = false;
    Logger::Get().Info("Initializing ThreadPool with " + std::to_string(threads) + " worker threads.");

    for (size_t i = 0; i < threads; ++i) {
        m_Workers.emplace_back([this](std::stop_token st) {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(this->m_QueueMutex);
                    this->m_Condition.wait(lock, st, [this] {
                        return this->m_Stop || !this->m_Tasks.empty();
                    });
                    
                    if ((this->m_Stop || st.stop_requested()) && this->m_Tasks.empty()) {
                        return;
                    }
                    
                    task = std::move(this->m_Tasks.front());
                    this->m_Tasks.pop();
                }
                task();
            }
        });
    }
    m_Initialized = true;
}

void ThreadPool::Shutdown() {
    {
        std::unique_lock<std::mutex> lock(m_QueueMutex);
        if (!m_Initialized) return;
        m_Stop = true;
    }
    
    m_Condition.notify_all();

    for (auto& worker : m_Workers) {
        worker.request_stop();
    }
    m_Condition.notify_all();

    m_Workers.clear();
    m_Initialized = false;
    m_Stop = false;
    Logger::Get().Info("ThreadPool shut down successfully.");
}
