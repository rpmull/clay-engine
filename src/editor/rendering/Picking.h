#pragma once
#include <glm/glm.hpp>
#include <vector>
#include "core/ecs/Scene.h"
#include "core/rendering/Camera.h"
#include "core/rendering/Mesh.h"

struct Ray {
    glm::vec3 Origin;
    glm::vec3 Direction;
};

class Picking {
public:
    struct PickRequest {
        float nx, ny; // Normalized coordinates
        bool PreferRootSelection = false;
    };

    // Original API (unchanged)
    static Ray ScreenPointToRay(float nx, float ny, Camera* cam);
    static int PickEntity(float nx, float ny, Scene& scene, Camera* cam);
    static int ResolveSelectionEntity(int pickedEntity, Scene& scene, bool preferRootSelection);

    // Additional API for queued picking
    static void QueuePick(float nx, float ny, bool preferRootSelection = false);
    static void QueueHoverPick(float nx, float ny);
    static void Process(Scene& scene, Camera* cam);
    static int GetLastPick();
    static int GetLastHoverPick();
    static bool LastPickPrefersRootSelection();
    static void ClearLastPick() { s_LastPick = -1; }
    static void ClearLastHoverPick() { s_LastHoverPick = -1; }
    static bool HadPickThisFrame();
    static bool HadHitThisFrame();

    // Intersection methods (public for drag-drop surface projection)
    static bool RayIntersectsAABB(const Ray& ray, const glm::vec3& min, const glm::vec3& max, float& t);
    static bool RayIntersectsOBB(const Ray& ray, const glm::mat4& transform, const glm::vec3& min, const glm::vec3& max, float& t);
    static bool RayIntersectsTriangle(const glm::vec3& origin, const glm::vec3& dir, const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, float& t);
    // closestT is returned in world-ray parametric units even though the test runs in local space.
    static bool RayIntersectsMesh(const Ray& ray, const Mesh& mesh, const glm::mat4& transform, float& closestT);

private:

    // Internal helpers
    static int PickEntityRay(const Ray& ray, Scene& scene);

    // Queue state
    static inline std::vector<PickRequest> s_PickQueue;
    static inline bool s_HoverQueued = false;
    static inline PickRequest s_HoverRequest{};
    static inline int s_LastPick = -1;
    static inline int s_LastHoverPick = -1;
    static inline bool s_LastPickPreferRootSelection = false;
    static inline bool s_ProcessedThisFrame = false;
    static inline bool s_AnyHitThisFrame = false;
};
