#include "EntityInterop.h"
#include "DotNetHost.h"
#include "core/ecs/Scene.h"
#include "core/physics/Physics.h"
#include <glm/glm.hpp>
#include <cstring>
#include <algorithm>
#include <functional>
#include <unordered_set>
#include "core/ecs/AnimationComponents.h"
#include "core/rendering/PBRMaterial.h"
#include "core/ecs/Components.h"
#include "core/rendering/Renderer.h"
#include "core/utils/PrefabPerfDiagnostics.h"
#include <iostream>

#include "ComponentInterop.h" 

#if !defined(CLAYMORE_RUNTIME)
#include <serialization/Serializer.h>
#else
#include "core/serialization/EntityBinaryLoader.h"
#endif

#include <chrono>

// Forward declarations for world position functions
extern "C" {
    __declspec(dllexport) void GetEntityWorldPosition(int entityID, float* outX, float* outY, float* outZ);
    __declspec(dllexport) void SetEntityWorldPosition(int entityID, float x, float y, float z);
}

// Function pointer typedefs for world position
using GetEntityWorldPosition_fn = void(*)(int, float*, float*, float*);
using SetEntityWorldPosition_fn = void(*)(int, float, float, float);

GetEntityWorldPosition_fn GetEntityWorldPositionPtr = &GetEntityWorldPosition;
SetEntityWorldPosition_fn SetEntityWorldPositionPtr = &SetEntityWorldPosition;
GetEntityLocalPosition_fn GetEntityLocalPositionPtr = &GetEntityLocalPosition;
SetEntityLocalPosition_fn SetEntityLocalPositionPtr = &SetEntityLocalPosition;
FindEntityByName_fn  FindEntityByNamePtr = &FindEntityByName;
GetEntityName_fn     GetEntityNamePtr = &GetEntityName;
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
using SetEntityPresentationHidden_fn = void(*)(int, bool);
using GetEntityPresentationHidden_fn = bool(*)(int);
using SetEntityActive_fn = void(*)(int, bool);
using GetEntityActive_fn = bool(*)(int);
using IsSceneBeingDestroyed_fn = bool(*)();

// Forward declarations for functions defined later in extern "C" block
extern "C" __declspec(dllexport) bool IsSceneBeingDestroyed();

SetEntityVisible_fn SetEntityVisiblePtr = &SetEntityVisible;
GetEntityVisible_fn GetEntityVisiblePtr = &GetEntityVisible;
SetEntityPresentationHidden_fn SetEntityPresentationHiddenPtr = &SetEntityPresentationHidden;
GetEntityPresentationHidden_fn GetEntityPresentationHiddenPtr = &GetEntityPresentationHidden;
SetEntityActive_fn SetEntityActivePtr = &SetEntityActive;
GetEntityActive_fn GetEntityActivePtr = &GetEntityActive;
IsSceneBeingDestroyed_fn IsSceneBeingDestroyedPtr = &IsSceneBeingDestroyed;

// Parenting function pointer exports - forward declarations
extern "C" {
    __declspec(dllexport) void SetEntityParent(int childID, int parentID, bool preserveWorldTransform);
    __declspec(dllexport) int GetEntityParent(int entityID);
    __declspec(dllexport) int* GetEntityChildren(int entityID);
    __declspec(dllexport) int GetEntityChildCount(int entityID);
    __declspec(dllexport) int FindChildByName(int parentID, const char* name);
    __declspec(dllexport) int FindDescendantByName(int rootID, const char* name);
    __declspec(dllexport) int CreateEntityWithParent(const char* name, int parentID);
}
using SetEntityParent_fn = void(*)(int, int, bool);
using GetEntityParent_fn = int(*)(int);
using GetEntityChildren_fn = int*(*)(int);
using GetEntityChildCount_fn = int(*)(int);
using FindChildByName_fn = int(*)(int, const char*);
using FindDescendantByName_fn = int(*)(int, const char*);
using CreateEntityWithParent_fn = int(*)(const char*, int);
SetEntityParent_fn SetEntityParentPtr = &SetEntityParent;
GetEntityParent_fn GetEntityParentPtr = &GetEntityParent;
GetEntityChildren_fn GetEntityChildrenPtr = &GetEntityChildren;
GetEntityChildCount_fn GetEntityChildCountPtr = &GetEntityChildCount;
FindChildByName_fn FindChildByNamePtr = &FindChildByName;
FindDescendantByName_fn FindDescendantByNamePtr = &FindDescendantByName;
CreateEntityWithParent_fn CreateEntityWithParentPtr = &CreateEntityWithParent;

// Duplication and UI mouse position function pointer exports
extern "C" {
    __declspec(dllexport) int DuplicateEntity(int entityID);
    __declspec(dllexport) bool GetUIMousePosition(float* outX, float* outY);
}
using DuplicateEntity_fn = int(*)(int);
using GetUIMousePosition_fn = bool(*)(float*, float*);
DuplicateEntity_fn DuplicateEntityPtr = &DuplicateEntity;
GetUIMousePosition_fn GetUIMousePositionPtr = &GetUIMousePosition;

extern "C"
{
    //--------------------------------------------------------------------------
    // World Position API (Unity-style: transform.position = world position)
    //--------------------------------------------------------------------------
    
    __declspec(dllexport) void GetEntityWorldPosition(int entityID, float* outX, float* outY, float* outZ)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if(!data)
        {
            *outX = *outY = *outZ = 0.0f;
            return;
        }
        // For bone entities, resolve the world position from the skeleton's animated
        // pose buffer when available. Bones are pose-driven (their AoS transform is
        // not animated per-frame in play mode), so this yields the actual animated
        // world position and decouples script bone reads from the generic transform
        // propagation. Falls back to the AoS WorldMatrix for non-bones / no palette.
        glm::mat4 boneWorld;
        if (data->BoneIndex >= 0 && Scene::Get().TryGetBoneWorldMatrix(entityID, boneWorld))
        {
            *outX = boneWorld[3].x;
            *outY = boneWorld[3].y;
            *outZ = boneWorld[3].z;
            return;
        }
        // Extract world position from the 4th column of WorldMatrix
        // This is the actual world-space position after parent transforms are applied
        glm::vec3 worldPos = glm::vec3(data->Transform.WorldMatrix[3]);
        *outX = worldPos.x;
        *outY = worldPos.y;
        *outZ = worldPos.z;
    }

    __declspec(dllexport) void SetEntityWorldPosition(int entityID, float x, float y, float z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if(!data) return;
        
        glm::vec3 worldPos(x, y, z);
        
        // If entity has a parent, convert world position to local
        if (data->Parent != INVALID_ENTITY_ID)
        {
            auto* parentData = Scene::Get().GetEntityData(data->Parent);
            if (parentData)
            {
                // Get parent's world matrix and invert it
                glm::mat4 parentWorldInv = glm::inverse(parentData->Transform.WorldMatrix);
                // Transform world position to parent's local space
                glm::vec4 localPos4 = parentWorldInv * glm::vec4(worldPos, 1.0f);
                data->Transform.Position = glm::vec3(localPos4);
            }
            else
            {
                // Parent data missing, fall back to direct assignment
                data->Transform.Position = worldPos;
            }
        }
        else
        {
            // No parent, world position == local position
            data->Transform.Position = worldPos;
        }
        
        Scene::Get().MarkTransformDirty(entityID);
    }

    //--------------------------------------------------------------------------
    // Local Position API (direct read/write of Transform.Position)
    //--------------------------------------------------------------------------
    
    __declspec(dllexport) void GetEntityLocalPosition(int entityID, float* outX, float* outY, float* outZ)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if(!data)
        {
            *outX = *outY = *outZ = 0.0f;
            return;
        }
        // Position in TransformComponent IS the local position
        auto pos = data->Transform.Position;
        *outX = pos.x;
        *outY = pos.y;
        *outZ = pos.z;
    }

    __declspec(dllexport) void SetEntityLocalPosition(int entityID, float x, float y, float z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if(!data) return;
        // Position in TransformComponent IS the local position
        data->Transform.Position = glm::vec3(x, y, z);
        Scene::Get().MarkTransformDirty(entityID);
    }

    __declspec(dllexport) int FindEntityByName(const char* name)
    {
        for (const Entity& e : Scene::Get().GetEntities())
            if (e.GetName() == name)
                return e.GetID();
        return -1;
    }

    __declspec(dllexport) const char* GetEntityName(int entityID)
    {
        Entity entity = Scene::Get().FindEntityByID(entityID);
        if (!entity.IsValid())
            return "";
        static thread_local std::string s_nameBuffer;
        s_nameBuffer = entity.GetName();
        return s_nameBuffer.c_str();
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
       if (Scene::Get().IsBeingDestroyed()) {
          return;
       }
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
        // Use rotating thread-local buffers to handle re-entrancy safely
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

    __declspec(dllexport) void SetEntityPresentationHidden(int entityID, bool hidden)
    {
        Scene::Get().SetEntityPresentationHidden(entityID, hidden);
    }

    __declspec(dllexport) bool GetEntityPresentationHidden(int entityID)
    {
        if (auto* data = Scene::Get().GetEntityData(entityID)) {
            return data->PresentationHidden;
        }
        return false;
    }

    __declspec(dllexport) void SetEntityActive(int entityID, bool active)
    {
        Scene::Get().SetEntityActive(entityID, active);
    }

    __declspec(dllexport) bool GetEntityActive(int entityID)
    {
        if (auto* data = Scene::Get().GetEntityData(entityID)) {
            return data->Active;
        }
        return false;
    }

    __declspec(dllexport) bool IsSceneBeingDestroyed()
    {
        return Scene::Get().IsBeingDestroyed();
    }

    // ---------------- Parenting Interop ----------------
    __declspec(dllexport) void SetEntityParent(int childID, int parentID, bool preserveWorldTransform)
    {
        std::cout << "[EntityInterop] SetEntityParent: child=" << childID << ", parent=" << parentID 
                  << ", preserveWorld=" << (preserveWorldTransform ? "true" : "false") << std::endl;
        
        auto* childData = Scene::Get().GetEntityData(childID);
        const bool clearParent = parentID <= 0 || static_cast<EntityID>(parentID) == INVALID_ENTITY_ID;
        const EntityID resolvedParentID = clearParent ? INVALID_ENTITY_ID : static_cast<EntityID>(parentID);
        
        if (!childData) {
            std::cerr << "[EntityInterop] ERROR: Child entity " << childID << " not found!" << std::endl;
            return;
        }

        if (clearParent) {
            const auto prefabLabel = cm::debug::DescribeOwningPrefab(
                Scene::Get(),
                static_cast<EntityID>(childID));
            const auto perfStart = std::chrono::high_resolution_clock::now();

            Scene::Get().SetParent(static_cast<EntityID>(childID), INVALID_ENTITY_ID, preserveWorldTransform);

            const auto perfEnd = std::chrono::high_resolution_clock::now();
            const double durationMs = std::chrono::duration<double, std::milli>(perfEnd - perfStart).count();
            cm::debug::RecordPrefabProfilerSample(prefabLabel, "PrefabSetup/SetEntityParent", durationMs);
            if (durationMs >= 0.25) {
                cm::debug::LogPrefabPerfEvent(
                    "SetEntityParent",
                    prefabLabel,
                    durationMs,
                    "child=" + childData->Name +
                        " parent=<scene root>" +
                        " preserveWorld=" + std::string(preserveWorldTransform ? "true" : "false"));
            }
            return;
        }

        auto* parentData = Scene::Get().GetEntityData(resolvedParentID);
        if (!parentData) {
            std::cerr << "[EntityInterop] ERROR: Parent entity " << parentID << " not found!" << std::endl;
            return;
        }
        
        const auto prefabLabel = cm::debug::DescribeOwningPrefab(Scene::Get(), resolvedParentID);
        const auto perfStart = std::chrono::high_resolution_clock::now();
        const std::string parentName = parentData->Name;
        std::cout << "[EntityInterop] Child '" << childData->Name << "' setting parent to '"
                  << parentName << "'" << std::endl;
        
        // Check if child (or any of its descendants) has armor skinning that needs parent skeleton
        bool hasArmorSkinning = false;
        std::function<void(EntityID)> checkArmorSkinning = [&](EntityID entityId) {
            auto* data = Scene::Get().GetEntityData(entityId);
            if (!data) return;
            if (data->Skinning && data->Skinning->UseParentSkeleton) {
                hasArmorSkinning = true;
                std::cout << "[EntityInterop] Found UseParentSkeleton=true on '" << data->Name << "'" << std::endl;
            }
            for (EntityID c : data->Children) {
                checkArmorSkinning(c);
            }
        };
        checkArmorSkinning(childID);
        
        if (hasArmorSkinning) {
            std::cout << "[EntityInterop] Armor skinning detected, searching for skeleton in parent hierarchy..." << std::endl;
            
            // Walk up hierarchy to find skeleton
            EntityID ancestorId = resolvedParentID;
            bool foundSkeleton = false;
            while (ancestorId != INVALID_ENTITY_ID) {
                auto* ancestorData = Scene::Get().GetEntityData(ancestorId);
                if (!ancestorData) break;
                
                if (ancestorData->Skeleton) {
                    std::cout << "[EntityInterop] Found SkeletonComponent on ancestor '" << ancestorData->Name 
                              << "' (ID=" << ancestorId << ") with " << ancestorData->Skeleton->BoneEntities.size() << " bones" << std::endl;
                    foundSkeleton = true;
                    break;
                }
                ancestorId = ancestorData->Parent;
            }
            
            if (!foundSkeleton) {
                std::cerr << "[EntityInterop] WARNING: No SkeletonComponent found in parent hierarchy! "
                          << "Armor skinning may not work. Parent the armor under an entity with a skeleton." << std::endl;
            }
        }
        
        Scene::Get().SetParent(static_cast<EntityID>(childID), resolvedParentID, preserveWorldTransform);
        
        // Auto-wire armor blendshapes to parent skeleton's UnifiedMorphComponent
        // This allows armor morphs to be controlled from the character's unified morph controller
        if (hasArmorSkinning) {
            // Collect all meshes from the armor hierarchy (for tint controller wiring)
            std::vector<EntityID> armorMeshesForTint;
            // Collect all blendshapes from the armor hierarchy
            std::vector<EntityID> armorMeshesWithBlendShapes;
            std::function<void(EntityID)> collectBlendShapeMeshes = [&](EntityID entityId) {
                auto* data = Scene::Get().GetEntityData(entityId);
                if (!data) return;
                if (data->Mesh) {
                    armorMeshesForTint.push_back(entityId);
                }
                if (data->BlendShapes && !data->BlendShapes->Shapes.empty()) {
                    armorMeshesWithBlendShapes.push_back(entityId);
                }
                for (EntityID c : data->Children) {
                    collectBlendShapeMeshes(c);
                }
            };
            collectBlendShapeMeshes(childID);

            // Auto-wire armor meshes to parent skeleton's TintController
            if (!armorMeshesForTint.empty()) {
                TintMaskController* tintController = nullptr;
                EntityID cur = resolvedParentID;
                while (cur != INVALID_ENTITY_ID && !tintController) {
                    auto* data = Scene::Get().GetEntityData(cur);
                    if (!data) break;
                    if (data->TintController) {
                        tintController = data->TintController.get();
                    }
                    cur = data->Parent;
                }

                if (tintController && tintController->AutoIncludeParentedSkinnedMeshes) {
                    std::cout << "[EntityInterop] Found TintController, wiring armor meshes..." << std::endl;
                    for (EntityID armorMeshId : armorMeshesForTint) {
                        auto* armorData = Scene::Get().GetEntityData(armorMeshId);
                        if (!armorData || !armorData->Mesh) continue;
                        bool alreadyTarget = false;
                        for (const auto& target : tintController->Targets) {
                            if (target.TargetEntity == armorMeshId && target.MaterialSlot == -1) {
                                alreadyTarget = true;
                                break;
                            }
                        }
                        if (!alreadyTarget) {
                            TintTarget newTarget;
                            newTarget.TargetEntity = armorMeshId;
                            newTarget.MaterialSlot = -1; // all slots, tint is still manually adjustable
                            newTarget.BlendMode = tintController->GlobalBlendMode;
                            newTarget.UseTargetColor = false;
                            tintController->Targets.push_back(newTarget);
                            tintController->MarkDirty();
                            std::cout << "[EntityInterop] Added armor mesh (ID=" << armorMeshId
                                      << ") to TintController" << std::endl;
                        }
                    }
                } else if (!tintController) {
                    std::cout << "[EntityInterop] No TintController found in parent hierarchy for armor meshes" << std::endl;
                } else {
                    std::cout << "[EntityInterop] TintController auto-inclusion disabled, skipping armor mesh wiring" << std::endl;
                }
            }
            
            if (!armorMeshesWithBlendShapes.empty()) {
                // Find UnifiedMorphComponent in parent hierarchy
                EntityID unifiedMorphEntity = INVALID_ENTITY_ID;
                UnifiedMorphComponent* unifiedMorph = nullptr;
                EntityID cur = resolvedParentID;
                while (cur != INVALID_ENTITY_ID && !unifiedMorph) {
                    auto* data = Scene::Get().GetEntityData(cur);
                    if (!data) break;
                    if (data->UnifiedMorph && !data->UnifiedMorph->Names.empty()) {
                        unifiedMorphEntity = cur;
                        unifiedMorph = data->UnifiedMorph.get();
                    }
                    cur = data->Parent;
                }
                
                if (unifiedMorph) {
                    std::cout << "[EntityInterop] Found UnifiedMorphComponent, wiring armor blendshapes..." << std::endl;
                    
                    // Add armor mesh entities to the UnifiedMorphComponent's member list
                    for (EntityID armorMeshId : armorMeshesWithBlendShapes) {
                        // Check if already in the list
                        bool alreadyMember = false;
                        for (EntityID existing : unifiedMorph->MemberMeshes) {
                            if (existing == armorMeshId) { alreadyMember = true; break; }
                        }
                        if (!alreadyMember) {
                            unifiedMorph->MemberMeshes.push_back(armorMeshId);
                            std::cout << "[EntityInterop] Added armor mesh (ID=" << armorMeshId << ") to UnifiedMorphComponent" << std::endl;
                        }
                        
                        // Add any new morph names from the armor that aren't in the unified list
                        auto* armorData = Scene::Get().GetEntityData(armorMeshId);
                        if (armorData && armorData->BlendShapes) {
                            for (const auto& shape : armorData->BlendShapes->Shapes) {
                                bool found = false;
                                for (const std::string& existingName : unifiedMorph->Names) {
                                    if (existingName == shape.Name) { found = true; break; }
                                }
                                if (!found) {
                                    unifiedMorph->Names.push_back(shape.Name);
                                    unifiedMorph->Weights.push_back(0.0f);
                                    unifiedMorph->NameIndexDirty = true;
                                    std::cout << "[EntityInterop] Added morph '" << shape.Name << "' to UnifiedMorphComponent" << std::endl;
                                }
                            }
                        }
                    }
                    
                    // Propagate current unified morph weights to the newly added armor meshes
                    Scene::Get().PropagateUnifiedMorphWeights(unifiedMorphEntity);
                } else {
                    std::cout << "[EntityInterop] No UnifiedMorphComponent found in parent hierarchy for armor blendshapes" << std::endl;
                }
            }
        }
        
        // Verify
        std::cout << "[EntityInterop] After SetParent, child's parent is now: " << childData->Parent << std::endl;
        
        // Log transform info
        std::cout << "[EntityInterop] Child transform: pos=(" 
                  << childData->Transform.Position.x << "," 
                  << childData->Transform.Position.y << "," 
                  << childData->Transform.Position.z << ")"
                  << " Visible=" << (childData->Visible ? "true" : "false")
                  << " Active=" << (childData->Active ? "true" : "false")
                  << std::endl;

        const auto perfEnd = std::chrono::high_resolution_clock::now();
        const double durationMs = std::chrono::duration<double, std::milli>(perfEnd - perfStart).count();
        cm::debug::RecordPrefabProfilerSample(prefabLabel, "PrefabSetup/SetEntityParent", durationMs);
        if (durationMs >= 0.25) {
            cm::debug::LogPrefabPerfEvent(
                "SetEntityParent",
                prefabLabel,
                durationMs,
                "child=" + childData->Name +
                    " parent=" + parentName +
                    " preserveWorld=" + std::string(preserveWorldTransform ? "true" : "false"));
        }
    }

    __declspec(dllexport) int GetEntityParent(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data) return -1;
        return static_cast<int>(data->Parent);
    }

    __declspec(dllexport) int* GetEntityChildren(int entityID)
    {
        // Use rotating thread-local buffers to handle re-entrancy safely
        static constexpr int kNumBuffers = 4;
        thread_local std::vector<int> s_ChildIDs[kNumBuffers];
        thread_local int s_CurrentBuffer = 0;
        
        s_CurrentBuffer = (s_CurrentBuffer + 1) % kNumBuffers;
        auto& buffer = s_ChildIDs[s_CurrentBuffer];
        buffer.clear();
        
        auto* data = Scene::Get().GetEntityData(entityID);
        if (data) {
            buffer.reserve(data->Children.size());
            for (EntityID childId : data->Children) {
                buffer.push_back(static_cast<int>(childId));
            }
        }
        return buffer.data();
    }

    __declspec(dllexport) int GetEntityChildCount(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data) return 0;
        return static_cast<int>(data->Children.size());
    }

    __declspec(dllexport) int FindChildByName(int parentID, const char* name)
    {
        if (!name) return -1;
        
        auto* parentData = Scene::Get().GetEntityData(parentID);
        if (!parentData) return -1;
        
        std::string searchName(name);
        for (EntityID childId : parentData->Children) {
            auto* childData = Scene::Get().GetEntityData(childId);
            if (childData && childData->Name == searchName) {
                return static_cast<int>(childId);
            }
        }
        return -1;
    }

    __declspec(dllexport) int FindDescendantByName(int rootID, const char* name)
    {
        if (!name) return -1;
        
        std::string searchName(name);
        std::function<int(int)> searchRecursive = [&](int entityID) -> int {
            auto* data = Scene::Get().GetEntityData(entityID);
            if (!data) return -1;
            
            // Check this entity's children
            for (EntityID childId : data->Children) {
                auto* childData = Scene::Get().GetEntityData(childId);
                if (childData) {
                    if (childData->Name == searchName) {
                        return static_cast<int>(childId);
                    }
                    // Recurse into child
                    int found = searchRecursive(static_cast<int>(childId));
                    if (found != -1) return found;
                }
            }
            return -1;
        };
        
        return searchRecursive(rootID);
    }

    __declspec(dllexport) int CreateEntityWithParent(const char* name, int parentID)
    {
        const char* resolvedName = name ? name : "Entity";
        Entity entity = parentID > 0
            ? Scene::Get().CreateEntityExact(resolvedName)
            : Scene::Get().CreateEntity(resolvedName);
        int entityID = entity.GetID();
        
        if (parentID > 0) {
            Scene::Get().SetParent(entityID, parentID, false);
        }
        
        return entityID;
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
        Scene& scene = Scene::Get();
        std::unordered_set<EntityID> persistent;
        if (scene.m_IsPlaying) {
            persistent = scene.CollectPersistentEntities();
        }

        // Remove all non-persistent entities from the current scene
        // Copy ids first to avoid invalidation
        std::vector<int> ids;
        auto& ents = scene.GetEntities();
        ids.reserve(ents.size());
        for (auto& e : ents) {
            EntityID id = e.GetID();
            if (!persistent.empty() && persistent.find(id) != persistent.end()) continue;
            ids.push_back(id);
        }
        for (int id : ids) scene.QueueRemoveEntity(id);
    }

    __declspec(dllexport) int DuplicateEntity(int entityID)
    {
        if (entityID <= 0) return -1;
        EntityID duplicatedId = Scene::Get().DuplicateEntity(entityID);
        return static_cast<int>(duplicatedId);
    }

    __declspec(dllexport) bool GetUIMousePosition(float* outX, float* outY)
    {
        if (!outX || !outY) return false;
        
        // Get UI mouse position from Renderer
        float x, y;
        bool valid = Renderer::Get().GetUIMousePosition(x, y);
        if (valid) {
            *outX = x;
            *outY = y;
        }
        return valid;
    }

}
