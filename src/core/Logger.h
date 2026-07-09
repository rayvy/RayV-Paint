#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <fstream>
#include <chrono>

enum class LogLevel {
    LogLevel_Debug,
    LogLevel_Info,
    LogLevel_Warning,
    LogLevel_Error
};

class Logger {
public:
    static Logger& Get();

    void Init(const std::string& logFilePath);
    void Shutdown();

    void Log(LogLevel level, const std::string& message);

    // Convenience functions
    void Debug(const std::string& msg) { Log(LogLevel::LogLevel_Debug, msg); }
    void Info(const std::string& msg)  { Log(LogLevel::LogLevel_Info, msg); }
    void Warn(const std::string& msg)  { Log(LogLevel::LogLevel_Warning, msg); }
    void Error(const std::string& msg) { Log(LogLevel::LogLevel_Error, msg); }

    // Tagged helpers: message is prefixed with [tag]
    void DebugTag(const char* tag, const std::string& msg) { Debug(std::string("[") + tag + "] " + msg); }
    void InfoTag(const char* tag, const std::string& msg)  { Info(std::string("[") + tag + "] " + msg); }
    void WarnTag(const char* tag, const std::string& msg)  { Warn(std::string("[") + tag + "] " + msg); }
    void ErrorTag(const char* tag, const std::string& msg) { Error(std::string("[") + tag + "] " + msg); }

    // Get copy of recent logs for in-app UI console
    std::vector<std::string> GetRecentLogs();
    void ClearRecentLogs();

    void SetMinLevel(LogLevel level);
    void Flush();

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::mutex m_Mutex;
    std::ofstream m_FileStream;
    std::vector<std::string> m_RecentLogs;
    LogLevel m_MinLevel = LogLevel::LogLevel_Info;
    bool m_Initialized = false;
};

// RAII timer: logs duration on destruction under [perf]
class ScopedTimer {
public:
    explicit ScopedTimer(const std::string& name, LogLevel level = LogLevel::LogLevel_Info)
        : m_Name(name), m_Level(level), m_Start(std::chrono::high_resolution_clock::now()) {}

    ~ScopedTimer() {
        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - m_Start).count();
        std::string msg = "[perf] " + m_Name + " took " + std::to_string(ms) + " ms";
        Logger::Get().Log(m_Level, msg);
    }

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    std::string m_Name;
    LogLevel m_Level;
    std::chrono::high_resolution_clock::time_point m_Start;
};

#define RAYV_PERF(name) ScopedTimer _rayv_perf_##__LINE__(name)
