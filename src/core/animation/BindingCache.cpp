#include "core/animation/BindingCache.h"
#include "core/ecs/AnimationComponents.h"
#include "core/ecs/Scene.h"

#include <functional>

namespace cm {
namespace animation {

namespace {

int ResolveBoneByNameSlow(const SkeletonComponent& skeleton, const std::string& name)
{
    int idx = skeleton.GetBoneIndex(name);
    if (idx >= 0) {
        return idx;
    }

    size_t pos = name.find_last_of(':');
    if (pos != std::string::npos && pos + 1 < name.size()) {
        idx = skeleton.GetBoneIndex(name.substr(pos + 1));
        if (idx >= 0) return idx;
    }
    pos = name.find_last_of('|');
    if (pos != std::string::npos && pos + 1 < name.size()) {
        idx = skeleton.GetBoneIndex(name.substr(pos + 1));
        if (idx >= 0) return idx;
    }
    pos = name.find_last_of('.');
    if (pos != std::string::npos && pos + 1 < name.size()) {
        idx = skeleton.GetBoneIndex(name.substr(pos + 1));
        if (idx >= 0) return idx;
    }

    for (const auto& kv : skeleton.BoneNameToIndex) {
        const std::string& skName = kv.first;
        if (skName.size() >= name.size()) {
            if (skName.compare(skName.size() - name.size(), name.size(), name) == 0) {
                return kv.second;
            }
        } else if (name.compare(name.size() - skName.size(), skName.size(), skName) == 0) {
            return kv.second;
        }
    }

    return -1;
}

} // namespace

void BindingCache::SetSkeleton(const SkeletonComponent* skeleton)
{
    const std::uint64_t topologyKey =
        (skeleton != nullptr) ? skeleton->GetCachedTopologySignature() : 0ull;
    if (m_Skeleton == skeleton &&
        m_SkeletonTopologyKey == topologyKey) {
        return;
    }

    if (m_SkeletonTopologyKey != topologyKey) {
        m_TrackBoneNameToIndex.clear();
        m_SkeletonTopologyKey = topologyKey;
    }

    m_Skeleton = skeleton;
}

std::uint64_t BindingCache::ResolveProperty(const std::string& path) const
{
    // Very simple hash-based id for now; editor will hold the full path
    // and runtime writeback will route via systems using the id.
    auto it = m_PropertyPathToId.find(path);
    if (it != m_PropertyPathToId.end()) return it->second;
    std::hash<std::string> h; // non-stable across runs, but acceptable for MVP editor preview
    std::uint64_t id = static_cast<std::uint64_t>(h(path));
    m_PropertyPathToId.emplace(path, id);
    return id;
}

int BindingCache::ResolveBoneByName(const std::string& name) const
{
    if (!m_Skeleton) return -1;
    return ResolveBoneByNameSlow(*m_Skeleton, name);
}

int BindingCache::ResolveBoneByTrackName(const std::string* name) const
{
    if (!m_Skeleton || name == nullptr) {
        return -1;
    }

    auto it = m_TrackBoneNameToIndex.find(name);
    if (it != m_TrackBoneNameToIndex.end()) {
        return it->second;
    }

    const int resolvedIndex = ResolveBoneByNameSlow(*m_Skeleton, *name);
    m_TrackBoneNameToIndex.emplace(name, resolvedIndex);
    return resolvedIndex;
}

void BindingCache::Clear()
{
    m_PropertyPathToId.clear();
    m_TrackBoneNameToIndex.clear();
    m_Skeleton = nullptr;
    m_SkeletonTopologyKey = 0;
}

} // namespace animation
} // namespace cm



