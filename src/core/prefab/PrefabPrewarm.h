#pragma once
#include "core/assets/AssetReference.h"

class Scene;

namespace prefab {

// Queue prefab prewarm requests based on scene script references.
void QueueScenePrefabs(Scene& scene);

// Queue a prefab GUID explicitly.
void QueuePrefabGuid(const ClaymoreGUID& guid);

// Process prewarm queue with a time budget (ms).
void Update(double budgetMs = 2.0);

// Enable/disable the prewarm system.
void SetEnabled(bool enabled);

// Clear pending queues and cached state.
void Clear();

} // namespace prefab

