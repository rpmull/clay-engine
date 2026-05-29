#include "EntityInterop.h"
#include "DotNetHost.h"
#include "ecs/Scene.h"
#include "physics/Physics.h"
#include <glm/glm.hpp>
#include <cstring>
#include <algorithm>
#include "ecs/AnimationComponents.h"
#include "rendering/PBRMaterial.h"
#include "ecs/Components.h"
#include "ComponentInterop.h" 
#include <serialization/Serializer.h>

GetEntityPosition_fn GetEntityPositionPtr = &GetEntityPosition;
SetEntityPosition_fn SetEntityPositionPtr = &SetEntityPosition;
FindEntityByName_fn  FindEntityByNamePtr = &FindEntityByName;
GetEntities_fn      GetEntitiesPtr = &GetEntities;
GetEntityCount_fn   GetEntityCountPtr = &GetEntityCount;
CreateEntity_fn  CreateEntityPtr = &CreateEntity;
DestroyEntity_fn DestroyEntityPtr = &DestroyEntity;
GetEntityByID_fn GetEntityByIDPtr = &GetEntityByID;
GetEntityRotation_fn GetEntityRotationPtr = &GetEntityRotation;
SetEntityRotation_fn SetEntityRotationPtr = &SetEntityRotation;
GetEntityRotationQuat_fn GetEntityRotationQuatPtr = &GetEntityRotationQuat;
SetEntityRotationQuat_fn SetEntityRotationQuatPtr = &SetEntityRotationQuat;
GetEntityScale_fn    GetEntityScalePtr    = &GetEntityScale;
SetEntityScale_fn    SetEntityScalePtr    = &SetEntityScale;
SetLinearVelocity_fn SetLinearVelocityPtr = &SetLinearVelocity;
SetAngularVelocity_fn SetAngularVelocityPtr = &SetAngularVelocity;
// New visibility/active function pointer exports
using SetEntityVisible_fn = void(*)(int, bool);
using GetEntityVisible_fn = bool(*)(int);
using SetEntityActive_fn = void(*)(int, bool);
using GetEntityActive_fn = bool(*)(int);
SetEntityVisible_fn SetEntityVisiblePtr = &SetEntityVisible;
GetEntityVisible_fn GetEntityVisiblePtr = &GetEntityVisible;
SetEntityActive_fn SetEntityActivePtr = &SetEntityActive;
GetEntityActive_fn GetEntityActivePtr = &GetEntityActive;

extern "C"
{
    __declspec(dllexport) void GetEntityPosition(int entityID, float* outX, float* outY, float* outZ)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if(!data)
        {
            *outX = *outY = *outZ = 0.0f;
            return;
        }
        auto pos = data->Transform.Position;
        *outX = pos.x;
        *outY = pos.y;
        *outZ = pos.z;
    }

    __declspec(dllexport) void SetEntityPosition(int entityID, float x, float y, float z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if(!data) return;
        data->Transform.Position = glm::vec3(x, y, z);
        Scene::Get().MarkTransformDirty(entityID);
    }

    __declspec(dllexport) void GetEntityLocalPosition(int entityID, float* outX, float* outY, float* outZ)
    {
        GetEntityPosition(entityID, outX, outY, outZ);
    }

    __declspec(dllexport) void SetEntityLocalPosition(int entityID, float x, float y, float z)
    {
        SetEntityPosition(entityID, x, y, z);
    }

    __declspec(dllexport) int FindEntityByName(const char* name)
    {
        for (const Entity& e : Scene::Get().GetEntities())
            if (e.GetName() == name)
                return e.GetID();
        return -1;
    }

    // ----------------------------------------------------------------------
    // Additional Interop Methods
    // ----------------------------------------------------------------------

    __declspec(dllexport) int CreateEntity(const char* name)
    {
        Entity entity = Scene::Get().CreateEntity(name ? name : "Entity");
        return entity.GetID();
    }

    __declspec(dllexport) void DestroyEntity(int entityID)
    {
       // Defer deletion to avoid mid-frame invalidation
       Scene::Get().QueueRemoveEntity(entityID);
    }

    __declspec(dllexport) int GetEntityByID(int entityID)
    {
        Entity entity = Scene::Get().FindEntityByID(entityID);
        return entity.GetID();
    }

    __declspec(dllexport) int* GetEntities()
       {
       // Use rotating thread-local buffers to avoid stale/overwritten pointers during
       // re-entrant interop calls while managed code enumerates entities.
       static constexpr int kNumBuffers = 4;
       thread_local std::vector<int> s_EntityIDs[kNumBuffers];
       thread_local int s_CurrentBuffer = 0;

       s_CurrentBuffer = (s_CurrentBuffer + 1) % kNumBuffers;
       auto& buffer = s_EntityIDs[s_CurrentBuffer];

       const auto& entities = Scene::Get().GetEntities();
       buffer.resize(entities.size());
       for (size_t i = 0; i < entities.size(); i++)
          buffer[i] = entities[i].GetID();
       return buffer.data();
       }

    __declspec(dllexport) int GetEntityCount()
    {
        return (int)Scene::Get().GetEntities().size();
    }

    // Rotation (Euler degrees)
    __declspec(dllexport) void GetEntityRotation(int entityID, float* outX, float* outY, float* outZ)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if(!data){ *outX = *outY = *outZ = 0.0f; return; }
        auto rot = data->Transform.Rotation;
        *outX = rot.x; *outY = rot.y; *outZ = rot.z;
    }

    __declspec(dllexport) void SetEntityRotation(int entityID, float x, float y, float z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if(!data) return;
        // Convert Euler degrees to quaternion and make quaternion authoritative
        glm::mat4 m = glm::yawPitchRoll(glm::radians(y), glm::radians(x), glm::radians(z));
        glm::quat q = glm::normalize(glm::quat_cast(m));
        data->Transform.RotationQ = q;
        // Keep Euler for inspector display only
        data->Transform.Rotation = glm::vec3(x, y, z);
        data->Transform.UseQuatRotation = true;
        Scene::Get().MarkTransformDirty(entityID);
    }

    __declspec(dllexport) void GetEntityRotationQuat(int entityID, float* outX, float* outY, float* outZ, float* outW)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if(!data){ *outX = *outY = *outZ = 0.0f; if(outW) *outW = 1.0f; return; }
        auto q = glm::normalize(data->Transform.RotationQ);
        *outX = q.x; *outY = q.y; *outZ = q.z; if(outW) *outW = q.w;
    }

    __declspec(dllexport) void SetEntityRotationQuat(int entityID, float x, float y, float z, float w)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if(!data) {
            return;
        }
        glm::quat q = glm::normalize(glm::quat(w, x, y, z));
        data->Transform.RotationQ = q;
        // Also update Euler for inspector display so UI reflects runtime changes
        glm::vec3 eulerRad = glm::eulerAngles(q); // returns radians (XYZ order)
        glm::vec3 eulerDegrees = glm::degrees(eulerRad);
        data->Transform.Rotation = eulerDegrees;
        data->Transform.UseQuatRotation = true;
        Scene::Get().MarkTransformDirty(entityID);
    }

    // Scale
    __declspec(dllexport) void GetEntityScale(int entityID, float* outX, float* outY, float* outZ)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if(!data){ *outX = *outY = *outZ = 0.0f; return; }
        auto scale = data->Transform.Scale;
        *outX = scale.x; *outY = scale.y; *outZ = scale.z;
    }

    __declspec(dllexport) void SetEntityScale(int entityID, float x, float y, float z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if(!data) return;
        data->Transform.Scale = glm::vec3(x, y, z);
        Scene::Get().MarkTransformDirty(entityID);
    }

    // Physics velocity setters
    __declspec(dllexport) void SetLinearVelocity(int entityID, float x, float y, float z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if(!data || !data->RigidBody) return;
        JPH::BodyID bodyID = data->RigidBody->BodyID;
        if(bodyID.IsInvalid()) return;
        // Keep component state in sync so kinematic update doesn't overwrite script-driven velocity
        data->RigidBody->LinearVelocity = glm::vec3(x, y, z);
        Physics::SetBodyLinearVelocity(bodyID, glm::vec3(x, y, z));
    }

    __declspec(dllexport) void SetAngularVelocity(int entityID, float x, float y, float z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if(!data || !data->RigidBody) return;
        JPH::BodyID bodyID = data->RigidBody->BodyID;
        if(bodyID.IsInvalid()) return;
        // Keep component state in sync so kinematic update doesn't overwrite script-driven angular velocity
        data->RigidBody->AngularVelocity = glm::vec3(x, y, z);
        Physics::SetBodyAngularVelocity(bodyID, glm::vec3(x, y, z));
    }

    // Visibility / Active
    __declspec(dllexport) void SetEntityVisible(int entityID, bool visible)
    {
        Scene::Get().SetEntityVisible(entityID, visible);
    }

    __declspec(dllexport) bool GetEntityVisible(int entityID)
    {
        if (auto* data = Scene::Get().GetEntityData(entityID)) {
            return data->Visible;
        }
        return false;
    }

    __declspec(dllexport) void SetEntityActive(int entityID, bool active)
    {
        if (auto* data = Scene::Get().GetEntityData(entityID)) {
            data->Active = active;
        }
    }

    __declspec(dllexport) bool GetEntityActive(int entityID)
    {
        if (auto* data = Scene::Get().GetEntityData(entityID)) {
            return data->Active;
        }
        return false;
    }

    // ---------------- Scene Interop ----------------
    __declspec(dllexport) bool Scene_LoadScene(const char* path)
    {
        return Scene_LoadSceneEx(path, true);
    }

    __declspec(dllexport) bool Scene_LoadSceneEx(const char* path, bool async)
    {
        if (!path) return false;
        Scene& scene = Scene::Get();
        if (async) {
            scene.RequestSceneLoad(std::string(path), true);
            return true;
        }
        return scene.LoadSceneImmediate(std::string(path), false);
    }

    __declspec(dllexport) float Scene_GetLoadProgress()
    {
        return Scene::Get().GetLoadProgress();
    }

    __declspec(dllexport) bool Scene_IsSceneLoading()
    {
        return Scene::Get().IsLoading();
    }

    __declspec(dllexport) bool Scene_IsSceneLoaded()
    {
        return Scene::Get().IsLoaded();
    }

    __declspec(dllexport) const char* Scene_GetCurrentScenePath()
    {
        const std::string& path = Scene::Get().GetScenePath();
        static thread_local std::string s_buffer;
        s_buffer = path;
        return s_buffer.empty() ? "" : s_buffer.c_str();
    }

    __declspec(dllexport) void Scene_UnloadScene()
    {
        // Remove all entities from the current scene
        // Copy ids first to avoid invalidation
        std::vector<int> ids;
        auto& ents = Scene::Get().GetEntities();
        ids.reserve(ents.size());
        for (auto& e : ents) ids.push_back(e.GetID());
        for (int id : ids) Scene::Get().QueueRemoveEntity(id);
    }

}

