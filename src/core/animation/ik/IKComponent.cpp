// IKComponent.cpp
#include "core/animation/ik/IKComponent.h"
#include "core/ecs/AnimationComponents.h"
#include <algorithm>

namespace cm { namespace animation { namespace ik {

bool IKComponent::ValidateChain(const SkeletonComponent& skeleton) const {
    if (Chain.size() < 2) return false;
    // Each bone id must be valid and parent->child order must hold
    for (size_t i = 0; i < Chain.size(); ++i) {
        int bi = Chain[i];
        if (bi < 0 || bi >= (int)skeleton.BoneParents.size()) return false;
        if (i > 0) {
            int parent = skeleton.BoneParents[Chain[i]];
            if (parent != Chain[i-1]) return false;
        }
    }
    // Constraints size check (optional)
    if (!Constraints.empty() && Constraints.size() != Chain.size() - 1) return false;
    return true;
}
 
void IKComponent::SetChain(const std::vector<BoneId>& ids) {
    Chain = ids;
    if (Chain.size() > (size_t)kMaxChainLen) Chain.resize(kMaxChainLen);
    // Reset caches as topology changed
    WasValidLastFrame = false;
    if (!Chain.empty()) {
        ChainRootHint = Chain.front();
        ChainEffectorHint = (Chain.size() >= 2) ? Chain.back() : -1;
    } else {
        ChainRootHint = -1;
        ChainEffectorHint = -1;
    }
    if (Chain.size() < 2) {
        ChainEffectorHint = -1;
    }
}

void IKComponent::SetChain(const BoneId* ids, size_t count) {
    Chain.assign(ids, ids + count);
    if (Chain.size() > (size_t)kMaxChainLen) Chain.resize(kMaxChainLen);
    // Reset caches as topology changed
    WasValidLastFrame = false;
    if (!Chain.empty()) {
        ChainRootHint = Chain.front();
        ChainEffectorHint = (Chain.size() >= 2) ? Chain.back() : -1;
    } else {
        ChainRootHint = -1;
        ChainEffectorHint = -1;
    }
    if (Chain.size() < 2) {
        ChainEffectorHint = -1;
    }
}

} } }


