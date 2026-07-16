#include "Notifications.h"

#include <chrono>
#include <ctime>

namespace core {

Notifications& Notifications::Get() {
    static Notifications instance;
    return instance;
}

std::string TruncateUtf8(const std::string& s, size_t maxChars) {
    if (maxChars == 0) return {};
    size_t i = 0;
    size_t count = 0;
    while (i < s.size() && count < maxChars) {
        unsigned char c = (unsigned char)s[i];
        size_t step = 1;
        if ((c & 0x80) == 0) step = 1;
        else if ((c & 0xE0) == 0xC0) step = 2;
        else if ((c & 0xF0) == 0xE0) step = 3;
        else if ((c & 0xF8) == 0xF0) step = 4;
        if (i + step > s.size()) break;
        i += step;
        ++count;
    }
    if (i >= s.size()) return s;
    // leave room for "…"
    if (maxChars <= 1) return "…";
    std::string head = TruncateUtf8(s, maxChars - 1);
    return head + "…";
}

void Notifications::Push(const std::string& message, NotifyLevel level) {
    if (message.empty()) return;
    Notification n;
    n.message = message;
    n.level = level;
    n.timeSec = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(m_Mu);
    m_Items.push_back(std::move(n));
    if (m_Items.size() > kMaxHistory)
        m_Items.erase(m_Items.begin(), m_Items.begin() + (m_Items.size() - kMaxHistory));
}

std::string Notifications::LatestPreview(size_t maxChars) const {
    std::lock_guard<std::mutex> lock(m_Mu);
    if (m_Items.empty()) return {};
    return TruncateUtf8(m_Items.back().message, maxChars);
}

bool Notifications::HasAny() const {
    std::lock_guard<std::mutex> lock(m_Mu);
    return !m_Items.empty();
}

NotifyLevel Notifications::LatestLevel() const {
    std::lock_guard<std::mutex> lock(m_Mu);
    if (m_Items.empty()) return NotifyLevel::Info;
    return m_Items.back().level;
}

std::vector<Notification> Notifications::History() const {
    std::lock_guard<std::mutex> lock(m_Mu);
    return m_Items;
}

void Notifications::Clear() {
    std::lock_guard<std::mutex> lock(m_Mu);
    m_Items.clear();
}

} // namespace core
