#include "Logger.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>

Logger& Logger::Get() {
    static Logger instance;
    return instance;
}

Logger::~Logger() {
    Shutdown();
}

void Logger::Init(const std::string& logFilePath) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Initialized) return;

    if (!logFilePath.empty()) {
        m_FileStream.open(logFilePath, std::ios::out | std::ios::app);
        if (!m_FileStream.is_open()) {
            std::cerr << "[Logger] Failed to open log file: " << logFilePath << std::endl;
        }
    }
    m_Initialized = true;
}

void Logger::Shutdown() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Initialized) return;

    if (m_FileStream.is_open()) {
        m_FileStream.close();
    }
    m_Initialized = false;
}

void Logger::Log(LogLevel level, const std::string& message) {
    if (level < m_MinLevel) return;

    std::lock_guard<std::mutex> lock(m_Mutex);

    // 1. Get timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm_buf;
    localtime_s(&tm_buf, &time_t_now);

    std::stringstream timeStr;
    timeStr << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();

    // 2. Map level to prefix
    std::string prefix;
    switch (level) {
        case LogLevel::LogLevel_Debug:   prefix = "[DEBUG]"; break;
        case LogLevel::LogLevel_Info:    prefix = "[INFO] "; break;
        case LogLevel::LogLevel_Warning: prefix = "[WARN] "; break;
        case LogLevel::LogLevel_Error:   prefix = "[ERROR]"; break;
    }

    // 3. Format message
    std::stringstream formattedMsg;
    formattedMsg << "[" << timeStr.str() << "] " << prefix << " " << message;
    std::string fullLogLine = formattedMsg.str();

    // 4. Output to console
    if (level == LogLevel::LogLevel_Error) {
        std::cerr << fullLogLine << std::endl;
    } else {
        std::cout << fullLogLine << std::endl;
    }

    // 5. Output to file
    if (m_FileStream.is_open()) {
        m_FileStream << fullLogLine << std::endl;
    }

    // 6. Output to recent log memory buffer (cap at 1000 lines)
    if (m_RecentLogs.size() >= 1000) {
        m_RecentLogs.erase(m_RecentLogs.begin());
    }
    m_RecentLogs.push_back(fullLogLine);
}

std::vector<std::string> Logger::GetRecentLogs() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_RecentLogs;
}

void Logger::ClearRecentLogs() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_RecentLogs.clear();
}

void Logger::SetMinLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_MinLevel = level;
}
