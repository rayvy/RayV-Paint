#pragma once
// Collects multi-file Explorer launches (one process per file) into a single UI instance.

#include <string>
#include <vector>

namespace helpers {

// If this process should show the UI, returns true and fills `files` with the full set.
// If another instance is the leader, returns false (caller should exit quietly).
// modeTag: "convert" or "atlas" — separate queues.
bool CollectBatchFiles(const char* modeTag, std::vector<std::string> seedFiles,
                       std::vector<std::string>& outFiles,
                       int waitMs = 450);

} // namespace helpers
