#pragma once
// Footer notification bar: last message (32 chars) + expandable history.
// Separate from Logger console, but Logger Warn/Error can feed here.

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace core {

enum class NotifyLevel : uint8_t {
    Info = 0,
    Warning,
    Error
};

struct Notification {
    std::string message;
    NotifyLevel level = NotifyLevel::Info;
    double timeSec = 0.0; // wall clock for display
};

class Notifications {
public:
    static Notifications& Get();

    void Push(const std::string& message, NotifyLevel level = NotifyLevel::Info);

    // Last notification text truncated to maxChars (default 32 for footer).
    std::string LatestPreview(size_t maxChars = 32) const;
    bool HasAny() const;
    NotifyLevel LatestLevel() const;

    std::vector<Notification> History() const;
    void Clear();

    static constexpr size_t kMaxHistory = 200;

private:
    Notifications() = default;
    mutable std::mutex m_Mu;
    std::vector<Notification> m_Items;
};

// UTF-8 safe truncate with ellipsis when needed.
std::string TruncateUtf8(const std::string& s, size_t maxChars);

} // namespace core
