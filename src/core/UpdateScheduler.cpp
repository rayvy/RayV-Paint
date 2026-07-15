#include "UpdateScheduler.h"
#include "ThreadPool.h"
#include "Logger.h"

#include <algorithm>
#include <chrono>

UpdateScheduler& UpdateScheduler::Get() {
    static UpdateScheduler inst;
    return inst;
}

uint64_t UpdateScheduler::Submit(Key key, Priority prio, WorkFn work, ApplyFn apply) {
    const uint64_t gen = m_Gen.fetch_add(1);
    Job job;
    job.key = key;
    job.prio = prio;
    job.gen = gen;
    job.apply = std::move(apply);
    job.hasWorker = static_cast<bool>(work);

    if (work) {
        try {
            job.fut = ThreadPool::Get().Enqueue([w = std::move(work)]() mutable {
                try { w(); }
                catch (const std::exception& e) {
                    Logger::Get().WarnTag("sched", std::string("job work threw: ") + e.what());
                }
                catch (...) {
                    Logger::Get().WarnTag("sched", "job work threw unknown");
                }
            });
        } catch (...) {
            // Pool not ready — run work inline then apply on Poll
            try { work(); } catch (...) {}
            job.hasWorker = false;
            job.done = true;
        }
    } else {
        job.done = true;
    }

    std::lock_guard<std::mutex> lock(m_Mu);
    m_Jobs[key] = std::move(job);
    return gen;
}

void UpdateScheduler::Cancel(Key key) {
    std::lock_guard<std::mutex> lock(m_Mu);
    m_Jobs.erase(key);
}

void UpdateScheduler::CancelKind(Kind kind) {
    std::lock_guard<std::mutex> lock(m_Mu);
    for (auto it = m_Jobs.begin(); it != m_Jobs.end(); ) {
        if (it->first.kind == kind) it = m_Jobs.erase(it);
        else ++it;
    }
}

void UpdateScheduler::CancelAll() {
    std::lock_guard<std::mutex> lock(m_Mu);
    m_Jobs.clear();
}

bool UpdateScheduler::IsPending(Key key) const {
    std::lock_guard<std::mutex> lock(m_Mu);
    return m_Jobs.find(key) != m_Jobs.end();
}

size_t UpdateScheduler::PendingCount() const {
    std::lock_guard<std::mutex> lock(m_Mu);
    return m_Jobs.size();
}

int UpdateScheduler::Poll(double budgetMs) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    int applied = 0;

    std::vector<Job> ready;
    {
        std::lock_guard<std::mutex> lock(m_Mu);
        for (auto it = m_Jobs.begin(); it != m_Jobs.end(); ) {
            Job& j = it->second;
            if (j.hasWorker && !j.done) {
                if (j.fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    try { j.fut.get(); } catch (...) {}
                    j.done = true;
                }
            }
            if (j.done) {
                ready.push_back(std::move(j));
                it = m_Jobs.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::sort(ready.begin(), ready.end(), [](const Job& a, const Job& b) {
        if (a.prio != b.prio) return a.prio > b.prio;
        return a.gen < b.gen;
    });

    size_t i = 0;
    for (; i < ready.size(); ++i) {
        auto& j = ready[i];
        // Soft budget: always apply at least one job; stop before more if over budget
        // (except Critical which always runs).
        if (applied > 0 && j.prio < Priority::Critical) {
            double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
            if (ms >= budgetMs)
                break;
        }
        if (j.apply) {
            try { j.apply(); }
            catch (const std::exception& e) {
                Logger::Get().WarnTag("sched", std::string("apply threw: ") + e.what());
            }
            catch (...) {}
        }
        ++applied;
    }

    // Re-queue jobs we did not apply (budget cut) so they are not lost.
    if (i < ready.size()) {
        std::lock_guard<std::mutex> lock(m_Mu);
        for (; i < ready.size(); ++i) {
            // Newer Submit for same key wins — don't clobber.
            if (m_Jobs.find(ready[i].key) != m_Jobs.end())
                continue;
            m_Jobs[ready[i].key] = std::move(ready[i]);
        }
    }
    return applied;
}
