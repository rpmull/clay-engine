#pragma once

#include <string>
#include <vector>

namespace editorui {

enum class EditorNotificationLevel {
    Info,
    Success,
    Warning,
    Error
};

class EditorNotifications {
public:
    void Push(EditorNotificationLevel level, std::string message, float lifetimeSeconds = 4.5f);
    void Render();

private:
    struct Toast {
        uint64_t Id = 0;
        EditorNotificationLevel Level = EditorNotificationLevel::Info;
        std::string Message;
        double CreatedAt = 0.0;
        float LifetimeSeconds = 0.0f;
        int Count = 1;
    };

private:
    std::vector<Toast> m_Toasts;
    uint64_t m_NextId = 1;
};

} // namespace editorui
