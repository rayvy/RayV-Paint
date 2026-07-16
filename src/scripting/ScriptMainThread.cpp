#include "ScriptMainThread.h"
#include "../core/Logger.h"
#include <atomic>
#include <mutex>
#include <queue>
#include <thread>

namespace script {
namespace {

std::mutex g_QMutex;
std::queue<std::function<void()>> g_Q;
std::atomic<uint64_t> g_NextId{1};
std::thread::id g_MainId{};
std::atomic<bool> g_MainSet{false};

} // namespace

void SetMainThread() {
    g_MainId = std::this_thread::get_id();
    g_MainSet = true;
}

bool IsMainThread() {
    if (!g_MainSet.load()) return true; // before init: treat as main
    return std::this_thread::get_id() == g_MainId;
}

uint64_t PostToMainThread(std::function<void()> fn) {
    if (!fn) return 0;
    uint64_t id = g_NextId.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(g_QMutex);
        g_Q.push(std::move(fn));
    }
    return id;
}

void PollMainThreadJobs(int maxJobs) {
    if (!IsMainThread()) {
        Logger::Get().ErrorTag("script", "PollMainThreadJobs called off main thread — refused");
        return;
    }
    int n = 0;
    while (n < maxJobs) {
        std::function<void()> job;
        {
            std::lock_guard<std::mutex> lock(g_QMutex);
            if (g_Q.empty()) break;
            job = std::move(g_Q.front());
            g_Q.pop();
        }
        try {
            if (job) job();
        } catch (const std::exception& e) {
            Logger::Get().ErrorTag("script", std::string("main-thread job exception: ") + e.what());
        } catch (...) {
            Logger::Get().ErrorTag("script", "main-thread job unknown exception");
        }
        ++n;
    }
}

size_t MainThreadJobCount() {
    std::lock_guard<std::mutex> lock(g_QMutex);
    return g_Q.size();
}

} // namespace script
