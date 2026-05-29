#pragma once
#include "core/ecs/Entity.h"
#include <vector>
#include <mutex>
#include <cstdint>

namespace cm::physics {

enum class AreaEventKind : uint8_t {
    BodyEntered,
    BodyExited,
    AreaEntered,
    AreaExited
};

struct AreaEvent {
    AreaEventKind Kind;
    EntityID AreaId;
    EntityID OtherId;
};

struct AreaEventBuffer {
    std::vector<AreaEvent> Events;
    mutable std::mutex Mutex;

    void Clear() {
        std::lock_guard<std::mutex> lock(Mutex);
        Events.clear();
    }
    void Push(AreaEvent e) {
        std::lock_guard<std::mutex> lock(Mutex);
        Events.emplace_back(e);
    }
    void SwapTo(std::vector<AreaEvent>& out) {
        std::lock_guard<std::mutex> lock(Mutex);
        out.swap(Events);
    }
};

} // namespace cm::physics


