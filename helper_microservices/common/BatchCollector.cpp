#include "BatchCollector.h"
#include "Utf8.h"

#include <windows.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>
#include <chrono>

namespace helpers {
namespace {

std::filesystem::path QueuePath(const char* modeTag) {
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring name = L"RayVHelpers_batch_";
    for (const char* p = modeTag; *p; ++p)
        name.push_back((wchar_t)(unsigned char)*p);
    name += L".txt";
    return std::filesystem::path(tmp) / name;
}

std::wstring MutexName(const char* modeTag) {
    std::wstring n = L"Local\\RayVHelpers_batch_";
    for (const char* p = modeTag; *p; ++p)
        n.push_back((wchar_t)(unsigned char)*p);
    return n;
}

void AppendPaths(const std::filesystem::path& q, const std::vector<std::string>& files) {
    // Open exclusive-ish append
    HANDLE h = CreateFileW(q.wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    for (const auto& f : files) {
        std::string line = f + "\n";
        DWORD written = 0;
        WriteFile(h, line.data(), (DWORD)line.size(), &written, nullptr);
    }
    CloseHandle(h);
}

std::vector<std::string> ReadAll(const std::filesystem::path& q) {
    std::vector<std::string> out;
    std::ifstream in(q);
    if (!in) return out;
    std::string line;
    std::set<std::string> seen;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        if (seen.insert(line).second) out.push_back(line);
    }
    return out;
}

} // namespace

bool CollectBatchFiles(const char* modeTag, std::vector<std::string> seedFiles,
                       std::vector<std::string>& outFiles, int waitMs) {
    outFiles.clear();
    auto q = QueuePath(modeTag);
    auto mtxName = MutexName(modeTag);

    HANDLE mtx = CreateMutexW(nullptr, FALSE, mtxName.c_str());
    if (!mtx) {
        outFiles = std::move(seedFiles);
        return true;
    }

    // Always contribute our seeds
    if (!seedFiles.empty())
        AppendPaths(q, seedFiles);

    DWORD wait = WaitForSingleObject(mtx, 0);
    if (wait == WAIT_TIMEOUT) {
        // Leader already running — our paths are queued; exit
        CloseHandle(mtx);
        return false;
    }

    // We are the leader. Clear stale queue first? Keep what was just written.
    // Wait for sibling processes to append.
    if (waitMs > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));

    outFiles = ReadAll(q);
    if (outFiles.empty())
        outFiles = std::move(seedFiles);

    // Reset queue for next session
    DeleteFileW(q.wstring().c_str());

    // Hold mutex until process ends so late arrivals still append before next leader.
    // Release at exit is fine (OS closes handle).
    // Keep mutex owned for a short extra window is already done via sleep.
    ReleaseMutex(mtx);
    CloseHandle(mtx);
    return true;
}

} // namespace helpers
