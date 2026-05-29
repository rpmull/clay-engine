#include "PrefabPerfDiagnostics.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>

#include "core/ecs/EntityData.h"
#include "core/ecs/Scene.h"
#include "core/utils/Profiler.h"

namespace cm::debug {

namespace {

struct PrefabPerfSettings {
    bool DetailedTimingsFromEnv = false;
    bool ConsoleLogging = false;
    uint32_t ConsoleLogInterval = 120;
};

bool EnvIsTruthy(const char* value) {
    if (!value || *value == '\0') {
        return false;
    }

    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return normalized == "1" ||
           normalized == "true" ||
           normalized == "yes" ||
           normalized == "on";
}

uint32_t EnvToUint(const char* value, uint32_t fallbackValue) {
    if (!value || *value == '\0') {
        return fallbackValue;
    }

    const long parsed = std::strtol(value, nullptr, 10);
    if (parsed <= 0) {
        return fallbackValue;
    }
    return static_cast<uint32_t>(parsed);
}

const PrefabPerfSettings& GetPrefabPerfSettings() {
    static const PrefabPerfSettings settings = [] {
        PrefabPerfSettings loaded{};
        loaded.DetailedTimingsFromEnv = EnvIsTruthy(std::getenv("CLAYMORE_PREFAB_PERF_DEBUG"));
        loaded.ConsoleLogging = loaded.DetailedTimingsFromEnv;
        loaded.ConsoleLogInterval =
            EnvToUint(std::getenv("CLAYMORE_PREFAB_PERF_INTERVAL"), loaded.ConsoleLogInterval);
        return loaded;
    }();
    return settings;
}

bool HasPrefabIdentity(const EntityData& data) {
    return data.PrefabInstance != nullptr ||
           !data.PrefabSource.empty() ||
           data.PrefabGuid.high != 0 ||
           data.PrefabGuid.low != 0;
}

std::string LeafNameFromPath(const std::string& path) {
    if (path.empty()) {
        return {};
    }

    try {
        const std::filesystem::path fsPath(path);
        if (fsPath.has_filename()) {
            return fsPath.filename().string();
        }
    } catch (...) {
    }

    size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos || slash + 1 >= path.size()) {
        return path;
    }
    return path.substr(slash + 1);
}

std::string SanitizeProfilerToken(std::string value) {
    if (value.empty()) {
        return "prefab";
    }

    for (char& c : value) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '_' || c == '-' || c == '.') {
            continue;
        }
        c = '_';
    }

    return value;
}

} // namespace

bool PrefabPerfDetailedTimingsEnabled() {
    return GetPrefabPerfSettings().DetailedTimingsFromEnv;
}

bool PrefabPerfConsoleLoggingEnabled() {
    return GetPrefabPerfSettings().ConsoleLogging;
}

uint32_t PrefabPerfConsoleLogInterval() {
    return GetPrefabPerfSettings().ConsoleLogInterval;
}

EntityID ResolveOwningPrefabRoot(Scene& scene, EntityID entityId) {
    EntityID current = entityId;
    EntityID resolved = INVALID_ENTITY_ID;
    while (current != INVALID_ENTITY_ID) {
        EntityData* data = scene.GetEntityData(current);
        if (!data) {
            break;
        }
        if (HasPrefabIdentity(*data)) {
            resolved = current;
        }
        current = data->Parent;
    }
    return resolved;
}

PrefabPerfLabel DescribePrefabRoot(Scene& scene, EntityID prefabRootId) {
    PrefabPerfLabel label{};
    if (prefabRootId == INVALID_ENTITY_ID) {
        return label;
    }

    EntityData* data = scene.GetEntityData(prefabRootId);
    if (!data) {
        return label;
    }

    label.RootId = prefabRootId;
    label.RootName = data->Name;
    if (data->PrefabInstance) {
        label.PrefabPath = data->PrefabInstance->PrefabPath;
        label.OwnedEntityCount = data->PrefabInstance->OwnedEntityGuids.size();
    }
    if (label.PrefabPath.empty()) {
        label.PrefabPath = data->PrefabSource;
    }
    label.PrefabName = LeafNameFromPath(label.PrefabPath);
    if (label.PrefabName.empty()) {
        label.PrefabName = data->Name;
    }
    return label;
}

PrefabPerfLabel DescribeOwningPrefab(Scene& scene, EntityID entityId) {
    return DescribePrefabRoot(scene, ResolveOwningPrefabRoot(scene, entityId));
}

std::string MakePrefabProfilerSection(const char* prefix, const PrefabPerfLabel& label) {
    std::string section = prefix ? prefix : "Prefab";
    section += "/";
    section += SanitizeProfilerToken(label.PrefabName.empty() ? label.RootName : label.PrefabName);
    section += "::";
    section += SanitizeProfilerToken(label.RootName.empty() ? "root" : label.RootName);
    section += "#";
    section += std::to_string(label.RootId);
    return section;
}

std::string MakePrefabDebugLabel(const PrefabPerfLabel& label) {
    std::string description;
    if (!label.PrefabName.empty()) {
        description += label.PrefabName;
        description += " ";
    }

    description += "'";
    description += label.RootName.empty() ? std::string("Entity") : label.RootName;
    description += "'";
    description += " [";
    description += std::to_string(label.RootId);
    description += "]";

    if (label.OwnedEntityCount > 0) {
        description += " owned=";
        description += std::to_string(label.OwnedEntityCount);
    }
    if (!label.PrefabPath.empty()) {
        description += " src=";
        description += label.PrefabPath;
    }
    return description;
}

void RecordPrefabProfilerSample(const PrefabPerfLabel& label, const char* prefix, double durationMs) {
    if (!PrefabPerfDetailedTimingsEnabled() || !label.IsValid() || !prefix || *prefix == '\0') {
        return;
    }

    Profiler::Get().Record(MakePrefabProfilerSection(prefix, label), durationMs);
}

void RecordPrefabProfilerSample(Scene& scene, EntityID prefabRootId, const char* prefix, double durationMs) {
    RecordPrefabProfilerSample(DescribePrefabRoot(scene, prefabRootId), prefix, durationMs);
}

void LogPrefabPerfEvent(const char* category, const PrefabPerfLabel& label, double durationMs, const std::string& details) {
    if (!PrefabPerfConsoleLoggingEnabled() || !label.IsValid()) {
        return;
    }

    std::cout << "[PrefabPerf][" << (category ? category : "Setup") << "] "
              << MakePrefabDebugLabel(label)
              << " total=" << durationMs << "ms";
    if (!details.empty()) {
        std::cout << " " << details;
    }
    std::cout << std::endl;
}

void LogPrefabPerfEvent(const char* category, Scene& scene, EntityID prefabRootId, double durationMs, const std::string& details) {
    LogPrefabPerfEvent(category, DescribePrefabRoot(scene, prefabRootId), durationMs, details);
}

} // namespace cm::debug
