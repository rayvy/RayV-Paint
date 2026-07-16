#pragma once
// Main-thread job queue for Python plugins.
// CONTRACT (also in Documentation.MD §9):
//   - rayv.doc / rayv.ops that mutate the document MUST run on the host main thread.
//   - Worker threads may compute (HTTP, decode) then rayv.host.call_on_main(fn).
//   - Host drains the queue once per frame (before plugin on_ui).

#include <cstdint>
#include <functional>
#include <string>

namespace script {

void SetMainThread();
bool IsMainThread();

// Enqueue work for the next (or current) main-frame drain. Thread-safe.
uint64_t PostToMainThread(std::function<void()> fn);

// Drain queue. Call only from main thread each frame.
void PollMainThreadJobs(int maxJobs = 64);

// How many jobs waiting (debug).
size_t MainThreadJobCount();

} // namespace script
