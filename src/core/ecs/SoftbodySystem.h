#pragma once

#include <string>

class Scene;
struct EntityData;
struct MeshComponent;

class SoftbodySystem {
public:
   static bool SupportsMesh(const EntityData& data, std::string* outReason = nullptr);
   static bool EnsureAuthoringData(EntityData& data);

   static void PrePhysics(Scene& scene, float dt);
   static void PostPhysics(Scene& scene);

   static void ReleaseRuntime(EntityData& data, bool destroyBody = false);
};
