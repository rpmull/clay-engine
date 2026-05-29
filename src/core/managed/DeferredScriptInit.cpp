#include "DeferredScriptInit.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/managed/ScriptOrder.h"
#include "core/managed/ScriptSystem.h"
#include "core/debug/PrefabLog.h"
#include <algorithm>
#include <tuple>

namespace DeferredScriptInit {

// Global flag - default to false (immediate script init) for backwards compatibility
bool g_EnableDeferredScriptInit = false;

// Queued script initialization data
struct QueuedScriptInit {
    Scene* scene;
    EntityID entityId;
    size_t scriptIndex;
};

static std::mutex s_queueMutex;
static std::vector<QueuedScriptInit> s_pendingScripts;

void QueueScriptOnCreate(Scene* scene, EntityID entityId, size_t scriptIndex) {
    if (!g_EnableDeferredScriptInit) {
        // Deferred init disabled - call OnCreate immediately
        EntityData* data = scene->GetEntityData(entityId);
        if (data && scriptIndex < data->Scripts.size()) {
            auto& script = data->Scripts[scriptIndex];
            if (script.Instance) {
                Entity entity(entityId, scene);
                script.Instance->OnCreate(entity);
            }
        }
        return;
    }
    
    // Queue for deferred execution
    std::lock_guard<std::mutex> lock(s_queueMutex);
    s_pendingScripts.push_back({scene, entityId, scriptIndex});
}

size_t ProcessDeferredScripts(size_t maxToProcess) {
    std::vector<QueuedScriptInit> toProcess;
    
    {
        std::lock_guard<std::mutex> lock(s_queueMutex);
        toProcess = std::move(s_pendingScripts);
        s_pendingScripts.clear();
    }
    
    if (toProcess.empty()) {
        return 0;
    }
    
    // Sort by [Priority] then stable tie-breakers so OnCreate order is global and deterministic
    auto sortKey = [](const QueuedScriptInit& q) {
        EntityData* d = q.scene ? q.scene->GetEntityData(q.entityId) : nullptr;
        if (!d || q.scriptIndex >= d->Scripts.size())
            return std::make_tuple(0, uint64_t(0), q.entityId, q.scriptIndex);
        const std::string& cn = d->Scripts[q.scriptIndex].ClassName;
        return std::make_tuple(
            ScriptSystem::Instance().GetScriptPriority(cn),
            ScriptOrder::StableHashScriptClassName(cn),
            q.entityId,
            q.scriptIndex);
    };
    std::sort(toProcess.begin(), toProcess.end(), [&sortKey](const QueuedScriptInit& a, const QueuedScriptInit& b) {
        return sortKey(a) < sortKey(b);
    });
    
    PREFAB_LOG("Processing " << toProcess.size() << " deferred script initializations");
    
    size_t processed = 0;
    size_t limit = toProcess.size();
    if (maxToProcess > 0 && maxToProcess < limit) {
        limit = maxToProcess;
    }
    for (size_t i = 0; i < limit; ++i) {
        const auto& queued = toProcess[i];
        if (!queued.scene) continue;
        
        EntityData* data = queued.scene->GetEntityData(queued.entityId);
        if (!data || queued.scriptIndex >= data->Scripts.size()) continue;
        
        auto& script = data->Scripts[queued.scriptIndex];
        if (!script.Instance) continue;
        
        try {
            Entity entity(queued.entityId, queued.scene);
            script.Instance->OnCreate(entity);
            processed++;
        } catch (...) {
            PREFAB_LOG_ERROR("Exception in deferred OnCreate for script " << script.ClassName);
        }
    }
    
    if (limit < toProcess.size()) {
        std::lock_guard<std::mutex> lock(s_queueMutex);
        s_pendingScripts.insert(s_pendingScripts.end(), toProcess.begin() + limit, toProcess.end());
    }
    
    PREFAB_LOG("Processed " << processed << " deferred script OnCreate calls");
    
    return processed;
}

bool HasPendingScripts() {
    std::lock_guard<std::mutex> lock(s_queueMutex);
    return !s_pendingScripts.empty();
}

size_t GetPendingCount() {
    std::lock_guard<std::mutex> lock(s_queueMutex);
    return s_pendingScripts.size();
}

void ClearQueue() {
    std::lock_guard<std::mutex> lock(s_queueMutex);
    s_pendingScripts.clear();
}

} // namespace DeferredScriptInit
