#pragma once

#include <cstdint>
#include <string>

#include "core/ecs/Entity.h"

class Scene;

namespace cm::debug {

struct PrefabPerfLabel {
    EntityID RootId = INVALID_ENTITY_ID;
    std::string RootName;
    std::string PrefabName;
    std::string PrefabPath;
    size_t OwnedEntityCount = 0;

    [[nodiscard]] bool IsValid() const { return RootId != INVALID_ENTITY_ID; }
};

bool PrefabPerfDetailedTimingsEnabled();
bool PrefabPerfConsoleLoggingEnabled();
uint32_t PrefabPerfConsoleLogInterval();

EntityID ResolveOwningPrefabRoot(Scene& scene, EntityID entityId);
PrefabPerfLabel DescribePrefabRoot(Scene& scene, EntityID prefabRootId);
PrefabPerfLabel DescribeOwningPrefab(Scene& scene, EntityID entityId);

std::string MakePrefabProfilerSection(const char* prefix, const PrefabPerfLabel& label);
std::string MakePrefabDebugLabel(const PrefabPerfLabel& label);
void RecordPrefabProfilerSample(const PrefabPerfLabel& label, const char* prefix, double durationMs);
void RecordPrefabProfilerSample(Scene& scene, EntityID prefabRootId, const char* prefix, double durationMs);
void LogPrefabPerfEvent(const char* category, const PrefabPerfLabel& label, double durationMs, const std::string& details = {});
void LogPrefabPerfEvent(const char* category, Scene& scene, EntityID prefabRootId, double durationMs, const std::string& details = {});

} // namespace cm::debug
