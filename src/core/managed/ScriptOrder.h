#pragma once

#include "core/ecs/Entity.h"
#include <cstdint>
#include <string>

// Deterministic OnCreate ordering: priority (ascending = lower first), then
// scriptTypeStableId, entityId, scriptIndex. Used by Scene::RuntimeClone,
// prefab init, and DeferredScriptInit.

namespace ScriptOrder {

// FNV-1a 64-bit over class name. Stable across runs/platforms for same string.
inline uint64_t StableHashScriptClassName(const std::string& name) noexcept {
    constexpr uint64_t FNV_OFFSET = 14695981039346656037ull;
    constexpr uint64_t FNV_PRIME = 1099511628211ull;
    uint64_t h = FNV_OFFSET;
    for (unsigned char c : name) {
        h ^= static_cast<uint64_t>(c);
        h *= FNV_PRIME;
    }
    return h;
}

// Strict weak ordering: returns true iff a should run OnCreate before b.
// Primary: priority (lower first); then scriptTypeStableId; then entityId; then scriptIndex.
inline bool OrderLess(int priorityA, uint64_t scriptTypeStableIdA, EntityID entityA, size_t scriptIndexA,
                      int priorityB, uint64_t scriptTypeStableIdB, EntityID entityB, size_t scriptIndexB) {
    if (priorityA != priorityB) return priorityA < priorityB;
    if (scriptTypeStableIdA != scriptTypeStableIdB) return scriptTypeStableIdA < scriptTypeStableIdB;
    if (entityA != entityB) return entityA < entityB;
    return scriptIndexA < scriptIndexB;
}

} // namespace ScriptOrder
