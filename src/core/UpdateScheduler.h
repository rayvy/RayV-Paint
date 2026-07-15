#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// UpdateScheduler — prioritized background jobs for the 2D engine.
//
// Worker work runs on ThreadPool. Completions are applied on the main thread
// via Poll() (call once per frame from Compose / main loop).
//
// Consumer-agnostic: jobs are free functions + opaque keys. Canvas uses it for
// filter bake, presentation bake, thumb rebuild — not for 3D.
// ---------------------------------------------------------------------------
class UpdateScheduler {
public:
    enum class Priority : uint8_t {
        Low = 0,
        Normal = 1,
        High = 2,
        Critical = 3
    };

    enum class Kind : uint8_t {
        Custom = 0,
        FilterBake = 1,
        Presentation = 2,
        Thumb = 3,
        LodResolve = 4
    };

    struct Key {
        Kind kind = Kind::Custom;
        int  a = -1; // typically layer index
        int  b = 0;  // optional sub-id
        bool operator==(const Key& o) const {
            return kind == o.kind && a == o.a && b == o.b;
        }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            return (size_t)k.kind * 2654435761u
                 ^ (size_t)(uint32_t)k.a * 40503u
                 ^ (size_t)(uint32_t)k.b;
        }
    };

    using WorkFn = std::function<void()>;          // runs on worker (may be empty)
    using ApplyFn = std::function<void()>;         // runs on main in Poll()

    static UpdateScheduler& Get();

    // Submit/replace job for key. Newer submit cancels older generation for same key.
    // If work is empty, only apply runs on main (still ordered via Poll).
    uint64_t Submit(Key key, Priority prio, WorkFn work, ApplyFn apply);

    void Cancel(Key key);
    void CancelKind(Kind kind);
    void CancelAll();

    // Main thread: apply finished jobs highest-priority first. budgetMs soft limit.
    int Poll(double budgetMs = 4.0);

    bool IsPending(Key key) const;
    size_t PendingCount() const;

private:
    UpdateScheduler() = default;

    struct Job {
        Key key;
        Priority prio = Priority::Normal;
        uint64_t gen = 0;
        WorkFn work;
        ApplyFn apply;
        std::future<void> fut;
        bool hasWorker = false;
        bool done = false;
    };

    mutable std::mutex m_Mu;
    std::unordered_map<Key, Job, KeyHash> m_Jobs;
    std::atomic<uint64_t> m_Gen{1};
};
