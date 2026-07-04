#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <fstream>

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

    // Get copy of recent logs for in-app UI console
    std::vector<std::string> GetRecentLogs();
    void ClearRecentLogs();

    void SetMinLevel(LogLevel level);

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
