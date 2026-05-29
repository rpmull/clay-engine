#include "ScriptableRegistry.h"
#include <string>
#include <algorithm>

static uint64_t Rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static void Hash128_Simple(const std::string& s, uint64_t& outHigh, uint64_t& outLow) {
    // Fast non-crypto hash using two lanes
    const uint64_t p1 = 11400714785074694791ull;
    const uint64_t p2 = 14029467366897019727ull;
    uint64_t h1 = 0x9E3779B97F4A7C15ull;
    uint64_t h2 = 0xC2B2AE3D27D4EB4Full;
    for (unsigned char c : s) {
        h1 ^= c; h1 *= p1; h1 = Rotl64(h1, 27);
        h2 ^= (uint64_t)c << 1; h2 *= p2; h2 = Rotl64(h2, 31);
    }
    outLow = h1 ^ (h2 << 1);
    outHigh = h2 ^ (h1 << 1);
}

TypeId ComputeTypeIdFromName(const std::string& fullName) {
    TypeId t{}; Hash128_Simple(fullName, t.high, t.low); return t;
}

bool ScriptableTypeRegistry::Register(const ScriptableTypeDesc& desc) {
    std::lock_guard<std::mutex> lk(m_Mutex);
    if (!desc.id.IsValid()) return false;
    auto it = m_ById.find(desc.id);
    if (it != m_ById.end()) {
        // Allow hot-reload / recompilation to update field descriptors and metadata.
        // m_AllOrder stores pointers to map values; overwriting the value keeps pointers valid.
        it->second = desc;
        m_ByName[desc.fullName] = desc.id;
        std::sort(m_AllOrder.begin(), m_AllOrder.end(), [](auto a, auto b){
            if (a->order != b->order) return a->order < b->order;
            return a->fullName < b->fullName;
        });
        return true;
    }

    m_ById[desc.id] = desc;
    m_ByName[desc.fullName] = desc.id;
    m_AllOrder.push_back(&m_ById[desc.id]);
    std::sort(m_AllOrder.begin(), m_AllOrder.end(), [](auto a, auto b){
        if (a->order != b->order) return a->order < b->order;
        return a->fullName < b->fullName;
    });
    return true;
}

const ScriptableTypeDesc* ScriptableTypeRegistry::Find(const TypeId& id) const {
    std::lock_guard<std::mutex> lk(m_Mutex);
    auto it = m_ById.find(id);
    return it == m_ById.end() ? nullptr : &it->second;
}

const ScriptableTypeDesc* ScriptableTypeRegistry::FindByName(const std::string& fullName) const {
    std::lock_guard<std::mutex> lk(m_Mutex);
    auto it = m_ByName.find(fullName);
    if (it == m_ByName.end()) return nullptr;
    auto it2 = m_ById.find(it->second);
    return it2 == m_ById.end() ? nullptr : &it2->second;
}


