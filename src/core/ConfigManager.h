#pragma once
#include <string>
#include <mutex>

class ConfigManager {
public:
    static ConfigManager& Get();

    bool Load(const std::string& configFilePath = "config.json");
    bool Save(const std::string& configFilePath = "config.json");

    // Getters & Setters (thread-safe)
    int GetDefaultWidth() const;
    void SetDefaultWidth(int width);

    int GetDefaultHeight() const;
    void SetDefaultHeight(int height);

    std::string GetLogLevel() const;
    void SetLogLevel(const std::string& level);

    float GetZoomSpeed() const;
    void SetZoomSpeed(float speed);

    std::string GetLogFilePath() const;
    void SetLogFilePath(const std::string& path);

private:
    ConfigManager() = default;
    ~ConfigManager() = default;
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    mutable std::mutex m_Mutex;
    
    // Configurations
    int m_DefaultWidth = 1024;
    int m_DefaultHeight = 1024;
    std::string m_LogLevel = "info";
    float m_ZoomSpeed = 1.15f;
    std::string m_LogFilePath = "rayv_paint.log";
};
