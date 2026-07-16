#pragma once
// Background / long-running editor jobs (export, open, smart-select, …).
// Footer shows progress + cancel; document-lock jobs block edits via AppContext.

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace core {

enum class JobState : uint8_t {
    Running = 0,
    Cancelling,
    Succeeded,
    Failed,
    Cancelled
};

struct JobSnapshot {
    uint64_t id = 0;
    std::string name;
    std::string status;     // short status line
    float progress = -1.f;  // 0..1, or <0 indeterminate
    JobState state = JobState::Running;
    bool cancellable = false;
    bool locksDocument = false;
};

class JobManager {
public:
    static JobManager& Get();

    // Begin a tracked job. Returns id (0 = failed).
    // cancelFn optional; called from UI thread when user hits Cancel.
    uint64_t Begin(const std::string& name,
                   bool locksDocument = false,
                   bool cancellable = false,
                   std::function<void()> cancelFn = {});

    void SetProgress(uint64_t id, float progress01, const std::string& status = {});
    void SetStatus(uint64_t id, const std::string& status);

    bool IsCancelRequested(uint64_t id) const;
    void RequestCancel(uint64_t id);

    // Finish job (removes from active list after a short retention for footer flash).
    // notify=false: log only (folder index etc. — avoid notification spam).
    void Complete(uint64_t id, bool ok, const std::string& message = {}, bool notify = true);

    bool IsDocumentLocked() const;
    bool HasActiveJobs() const;

    // Active + recently finished (for footer).
    std::vector<JobSnapshot> Snapshot() const;

    // Drop finished jobs older than keepMs.
    void PruneFinished(double keepMs = 2500.0);

private:
    JobManager() = default;

    struct Entry {
        uint64_t id = 0;
        std::string name;
        std::string status;
        float progress = -1.f;
        JobState state = JobState::Running;
        bool cancellable = false;
        bool locksDocument = false;
        bool cancelRequested = false; // guarded by m_Mu
        std::function<void()> cancelFn;
        double finishedAtMs = 0.0; // 0 while running
    };

    mutable std::mutex m_Mu;
    std::vector<Entry> m_Jobs;
    uint64_t m_NextId = 1;

    static double NowMs();
    Entry* FindUnlocked(uint64_t id);
    const Entry* FindUnlocked(uint64_t id) const;
};

} // namespace core
