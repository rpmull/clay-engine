#pragma once

#include "core/ecs/Entity.h"
#include <vector>
#include <mutex>
#include <functional>

class Scene;

/**
 * DeferredScriptInit - Queue for deferring script OnCreate calls
 * 
 * When CLAYMORE_DEFER_SCRIPT_INIT is defined, script OnCreate calls are queued
 * and processed on the first frame update instead of during prefab instantiation.
 * 
 * This can save 10-100ms for prefabs with complex script initialization, but
 * scripts won't be fully initialized until ProcessDeferredScripts() is called.
 * 
 * Usage:
 * 1. During prefab instantiation, call QueueScriptOnCreate() instead of OnCreate()
 * 2. In Scene::Update(), call ProcessDeferredScripts() at the start of the frame
 * 
 * Note: OnBind and OnValidate are NOT deferred - they must run during instantiation
 * for proper serialization and cross-script reference setup.
 */
namespace DeferredScriptInit {

/**
 * Queue a script's OnCreate call for deferred execution.
 * 
 * @param scene The scene containing the entity
 * @param entityId The entity ID with the script
 * @param scriptIndex Index of the script in the entity's Scripts vector
 */
void QueueScriptOnCreate(Scene* scene, EntityID entityId, size_t scriptIndex);

/**
 * Process all queued OnCreate calls.
 * Call this at the start of Scene::Update() on the first frame.
 * 
 * @return Number of scripts processed
 */
size_t ProcessDeferredScripts(size_t maxToProcess = 0);

/**
 * Check if there are pending script initializations.
 */
bool HasPendingScripts();

/**
 * Get the number of pending script initializations.
 */
size_t GetPendingCount();

/**
 * Clear the queue without processing (for cleanup on error/scene unload).
 */
void ClearQueue();

/**
 * Global flag to enable/disable deferred script initialization.
 * When false (default), QueueScriptOnCreate() calls OnCreate immediately.
 */
extern bool g_EnableDeferredScriptInit;

} // namespace DeferredScriptInit
