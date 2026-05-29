#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace cm { namespace animation { struct AvatarDefinition; } }

struct BuiltModelPaths {
    std::string metaPath;     // assets/models/foo.meta
    std::string skelPath;     // assets/models/foo.skelbin
    std::string meshPath;     // assets/models/foo.meshbin
    std::vector<std::string> animPaths; // 0..N
};

// Returns true if cache exists and is up-to-date. Does NOT build anything.
bool HasModelCache(const std::string& sourceModelPath, BuiltModelPaths& out);

// Returns true if cache exists and is up-to-date (by source timestamp/version). If not, attempts to build it.
bool EnsureModelCache(const std::string& sourceModelPath, BuiltModelPaths& out);

// One-shot build (blocking); you’ll call this inside a background Job.
bool BuildModelCacheBlocking(const std::string& sourceModelPath, BuiltModelPaths& out);

// Variant that force-slices the model into humanoid body regions before caching.
bool BuildHumanoidSlicedModelCacheBlocking(const std::string& sourceModelPath,
                                           const cm::animation::AvatarDefinition& avatar,
                                           BuiltModelPaths& out);



