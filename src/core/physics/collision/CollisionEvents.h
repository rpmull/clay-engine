#pragma once
#include "core/ecs/Entity.h"
#include <vector>
#include <mutex>
#include <cstdint>

namespace cm::physics {

enum class CollisionEventKind : uint8_t {
    Enter = 0,
    Exit  = 1
};

enum class CollisionBodyKind : uint8_t {
    Unknown             = 0,
    RigidBody           = 1,
    StaticBody          = 2,
    CharacterController = 3
};

struct CollisionEvent {
    CollisionEventKind Kind;
    EntityID SelfId;
    EntityID OtherId;
    CollisionBodyKind OtherKind;
};

struct CollisionEventBuffer {
    std::vector<CollisionEvent> Events;
    mutable std::mutex Mutex;

    void Clear() {
        std::lock_guard<std::mutex> lock(Mutex);
        Events.clear();
    }
    void Push(CollisionEvent e) {
        std::lock_guard<std::mutex> lock(Mutex);
        Events.emplace_back(e);
    }
    void SwapTo(std::vector<CollisionEvent>& out) {
        std::lock_guard<std::mutex> lock(Mutex);
        out.swap(Events);
    }
};

} // namespace cm::physics

