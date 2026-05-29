#include "core/animation/AnimationPreloader.h"

#include "core/animation/AnimationPlayerComponent.h"
#include "core/animation/AnimationSerializer.h"
#include "core/animation/AnimationAssetCache.h"
#include "core/animation/AnimatorController.h"
#include "core/animation/AnimatorControllerIO.h"
#include "core/animation/AnimatorControllerOverride.h"
#include "core/animation/AnimatorControllerOverrideIO.h"
#include "core/animation/AnimationTypes.h"
#include <memory>

namespace cm {
namespace animation {

namespace {

std::shared_ptr<AnimationAsset> LoadAssetShared(const std::string& path) {
    if (path.empty()) return nullptr;
    auto asset = LoadAnimationAssetCached(path, true);
    if (!asset || asset->tracks.empty()) return nullptr;
    return asset;
}

std::shared_ptr<AnimationClip> LoadClipShared(const std::string& path) {
    if (path.empty()) return nullptr;
    AnimationClip clip = LoadAnimationClip(path);
    if (clip.BoneTracks.empty() && clip.HumanoidTracks.empty()) return nullptr;
    return std::make_shared<AnimationClip>(std::move(clip));
}

struct ResolvedPreloadBinding {
    std::string assetPath;
    std::string clipPath;
};

ResolvedPreloadBinding ResolvePreloadBinding(const AnimationPlayerComponent& component,
                                             const std::string& assetPath,
                                             const std::string& clipPath) {
    if (component.ControllerOverride &&
        component.ControllerOverride->MatchesController(component.ControllerPath)) {
        if (!assetPath.empty()) {
            if (std::string overridePath = component.ControllerOverride->Resolve(assetPath); !overridePath.empty()) {
                return { overridePath, {} };
            }
        }
        if (!clipPath.empty()) {
            if (std::string overridePath = component.ControllerOverride->Resolve(clipPath); !overridePath.empty()) {
                return { overridePath, {} };
            }
        }
    }

    return { assetPath, clipPath };
}

} // namespace

void PreloadAnimatorComponent(AnimationPlayerComponent& component) {
    if (component.ActiveStates.empty()) component.ActiveStates.push_back({});

    if (!component.ControllerOverride && !component.ControllerOverridePath.empty()) {
        component.ControllerOverride = LoadAnimatorControllerOverrideFromFile(component.ControllerOverridePath);
    }

    // If controller path is set, load the controller file
    if (!component.Controller && !component.ControllerPath.empty()) {
        component.Controller = LoadAnimatorControllerFromFile(component.ControllerPath);
        if (component.Controller) {
            component._AutoControllerGenerated = false;
            component.SyncRuntimeControllerState();
        }
    }
    
    // If no controller but SingleClipPath is set, preload that asset for quick play
    // (An auto-generated controller will be created at runtime in AnimationSystem)
    if (!component.Controller && component.ControllerPath.empty() && !component.SingleClipPath.empty()) {
        auto asset = LoadAssetShared(component.SingleClipPath);
        if (asset) {
            component.CachedAssets[0] = asset;
            component.ActiveStates.front().Asset = asset.get();
            component.ActiveStates.front().LegacyClip = nullptr;
        }
        return;
    }

    if (!component.Controller) {
        return;
    }

    auto warmEntry = [&](int cacheKey, const std::string& assetPath, const std::string& clipPath) {
        const ResolvedPreloadBinding resolved = ResolvePreloadBinding(component, assetPath, clipPath);
        if (!resolved.assetPath.empty()) {
            if (component.CachedAssets.find(cacheKey) == component.CachedAssets.end()) {
                if (auto asset = LoadAssetShared(resolved.assetPath)) {
                    component.CachedAssets[cacheKey] = asset;
                }
            }
        } else if (!resolved.clipPath.empty()) {
            if (component.CachedClips.find(cacheKey) == component.CachedClips.end()) {
                if (auto clip = LoadClipShared(resolved.clipPath)) {
                    component.CachedClips[cacheKey] = clip;
                }
            }
        }
    };

    auto warmState = [&](const AnimatorState& state, int layerIdx) {
        const bool isOverlay = layerIdx > 0;
        const int baseKey = isOverlay ? (layerIdx * 10000 + state.Id) : state.Id;
        warmEntry(baseKey, state.AnimationAssetPath, state.ClipPath);

        if (state.Kind == AnimatorStateKind::Blend1D) {
            for (size_t i = 0; i < state.Blend1DEntries.size(); ++i) {
                const auto& entry = state.Blend1DEntries[i];
                const int key = isOverlay
                    ? (layerIdx * 10000 + state.Id * 100 + static_cast<int>(i))
                    : (state.Id * 1000 + static_cast<int>(i));
                warmEntry(key, entry.AssetPath, entry.ClipPath);
            }
        } else if (state.Kind == AnimatorStateKind::Blend2D) {
            for (size_t i = 0; i < state.Blend2DEntries.size(); ++i) {
                const auto& entry = state.Blend2DEntries[i];
                const int key = isOverlay
                    ? (layerIdx * 10000 + state.Id * 100 + static_cast<int>(i))
                    : (state.Id * 1000 + static_cast<int>(i));
                warmEntry(key, entry.AssetPath, entry.ClipPath);
            }
        }
    };

    if (component.Controller->HasLayers()) {
        for (const auto& layer : component.Controller->Layers) {
            for (const auto& state : layer.States) {
                warmState(state, layer.Index);
            }
        }
    } else {
        for (const auto& state : component.Controller->States) {
            warmState(state, 0);
        }
    }

    const int defaultStateId = component.ResolveBaseDefaultStateId();
    if (defaultStateId >= 0) {
        auto it = component.CachedAssets.find(defaultStateId);
        if (it != component.CachedAssets.end()) {
            component.ActiveStates.front().Asset = it->second.get();
            component.ActiveStates.front().LegacyClip = nullptr;
        } else {
            auto itClip = component.CachedClips.find(defaultStateId);
            if (itClip != component.CachedClips.end()) {
                component.ActiveStates.front().Asset = nullptr;
                component.ActiveStates.front().LegacyClip = itClip->second.get();
            }
        }
    }
}

} // namespace animation
} // namespace cm


