#include "JobManager.h"
#include "Logger.h"
#include "Notifications.h"

#include <chrono>

namespace core {

JobManager& JobManager::Get() {
    static JobManager instance;
    return instance;
}

double JobManager::NowMs() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

JobManager::Entry* JobManager::FindUnlocked(uint64_t id) {
    for (auto& e : m_Jobs)
        if (e.id == id) return &e;
    return nullptr;
}

const JobManager::Entry* JobManager::FindUnlocked(uint64_t id) const {
    for (const auto& e : m_Jobs)
        if (e.id == id) return &e;
    return nullptr;
}

uint64_t JobManager::Begin(const std::string& name,
                           bool locksDocument,
                           bool cancellable,
                           std::function<void()> cancelFn) {
    std::lock_guard<std::mutex> lock(m_Mu);
    Entry e;
    e.id = m_NextId++;
    e.name = name;
    e.status = "Starting…";
    e.progress = -1.f;
    e.state = JobState::Running;
    e.cancellable = cancellable;
    e.locksDocument = locksDocument;
    e.cancelFn = std::move(cancelFn);
    e.finishedAtMs = 0.0;
    const uint64_t id = e.id;
    m_Jobs.push_back(std::move(e));
    Logger::Get().InfoTag("job", "Begin #" + std::to_string(id) + " " + name);
    return id;
}

void JobManager::SetProgress(uint64_t id, float progress01, const std::string& status) {
    std::lock_guard<std::mutex> lock(m_Mu);
    Entry* e = FindUnlocked(id);
    if (!e || (e->state != JobState::Running && e->state != JobState::Cancelling)) return;
    e->progress = progress01;
    if (!status.empty()) e->status = status;
}

void JobManager::SetStatus(uint64_t id, const std::string& status) {
    std::lock_guard<std::mutex> lock(m_Mu);
    Entry* e = FindUnlocked(id);
    if (!e) return;
    e->status = status;
}

bool JobManager::IsCancelRequested(uint64_t id) const {
    std::lock_guard<std::mutex> lock(m_Mu);
    const Entry* e = FindUnlocked(id);
    return e && e->cancelRequested;
}

void JobManager::RequestCancel(uint64_t id) {
    std::function<void()> fn;
    {
        std::lock_guard<std::mutex> lock(m_Mu);
        Entry* e = FindUnlocked(id);
        if (!e || (e->state != JobState::Running && e->state != JobState::Cancelling))
            return;
        if (!e->cancellable) return;
        e->cancelRequested = true;
        e->state = JobState::Cancelling;
        e->status = "Cancelling…";
        fn = e->cancelFn;
    }
    Logger::Get().InfoTag("job", "Cancel requested #" + std::to_string(id));
    if (fn) {
        try { fn(); } catch (...) {}
    }
}

void JobManager::Complete(uint64_t id, bool ok, const std::string& message, bool notify) {
    std::string name;
    JobState finalState = ok ? JobState::Succeeded : JobState::Failed;
    {
        std::lock_guard<std::mutex> lock(m_Mu);
        Entry* e = FindUnlocked(id);
        if (!e) return;
        if (e->cancelRequested && !ok)
            finalState = JobState::Cancelled;
        e->state = finalState;
        e->progress = (finalState == JobState::Succeeded) ? 1.f : e->progress;
        if (!message.empty())
            e->status = message;
        else if (finalState == JobState::Succeeded)
            e->status = "Done";
        else if (finalState == JobState::Cancelled)
            e->status = "Cancelled";
        else
            e->status = "Failed";
        e->finishedAtMs = NowMs();
        name = e->name;
    }

    const char* tag = "job";
    std::string msg = name + ": " + (message.empty() ? (ok ? "done" : "failed") : message);
    if (finalState == JobState::Succeeded) {
        Logger::Get().InfoTag(tag, msg);
        if (notify)
            Notifications::Get().Push(msg, NotifyLevel::Info);
    } else if (finalState == JobState::Cancelled) {
        Logger::Get().WarnTag(tag, msg); // Logger → notification bar
    } else {
        Logger::Get().ErrorTag(tag, msg); // Logger → notification bar
    }
}

bool JobManager::IsDocumentLocked() const {
    std::lock_guard<std::mutex> lock(m_Mu);
    for (const auto& e : m_Jobs) {
        if (e.locksDocument &&
            (e.state == JobState::Running || e.state == JobState::Cancelling))
            return true;
    }
    return false;
}

bool JobManager::HasActiveJobs() const {
    std::lock_guard<std::mutex> lock(m_Mu);
    for (const auto& e : m_Jobs) {
        if (e.state == JobState::Running || e.state == JobState::Cancelling)
            return true;
    }
    return false;
}

std::vector<JobSnapshot> JobManager::Snapshot() const {
    std::lock_guard<std::mutex> lock(m_Mu);
    std::vector<JobSnapshot> out;
    out.reserve(m_Jobs.size());
    for (const auto& e : m_Jobs) {
        JobSnapshot s;
        s.id = e.id;
        s.name = e.name;
        s.status = e.status;
        s.progress = e.progress;
        s.state = e.state;
        s.cancellable = e.cancellable;
        s.locksDocument = e.locksDocument;
        out.push_back(std::move(s));
    }
    return out;
}

void JobManager::PruneFinished(double keepMs) {
    const double now = NowMs();
    std::lock_guard<std::mutex> lock(m_Mu);
    for (auto it = m_Jobs.begin(); it != m_Jobs.end(); ) {
        if (it->state == JobState::Running || it->state == JobState::Cancelling) {
            ++it;
            continue;
        }
        if (it->finishedAtMs > 0.0 && (now - it->finishedAtMs) > keepMs)
            it = m_Jobs.erase(it);
        else
            ++it;
    }
}

} // namespace core
