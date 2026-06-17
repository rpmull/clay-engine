#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include "core/ecs/Entity.h"

bool LoadHostFxr();
bool LoadDotnetRuntime(const std::wstring& assemblyPath, const std::wstring& typeName, const std::wstring& methodName);

// Resolve a managed function pointer using load_assembly_and_get_function_pointer
bool ResolveManagedDelegate(const std::wstring& assemblyPath,
                            const std::wstring& typeName,
                            const std::wstring& methodName,
                            const std::wstring& delegateType,
                            void** outFunction);


#include "managed/interop/ComponentInterop.h"
#include "managed/interop/EntityInterop.h"
#include "managed/interop/TweenInterop.h"

// Script interop function pointer types
using Script_Create_fn = void* (*)(const char* className);
using Script_Bind_fn = void (*)(void* handle, int entityID);
using Script_OnCreate_fn = void (*)(void* handle, int entityID);
using Script_OnUpdate_fn = void (*)(void* handle, float dt);
using Script_OnDestroy_fn = void (*)(void* handle);
using Script_Invoke_fn = void (*)(void* handle, const char* methodName);
using Script_Destroy_fn = void (*)(void* handle);
using ReloadScripts_fn = int (*)(const wchar_t*);
using ClearComponentCaches_fn = void (*)();

// These are resolved at runtime
extern Script_Create_fn g_Script_Create;
extern Script_Bind_fn g_Script_Bind;
extern Script_OnCreate_fn g_Script_OnCreate;
extern Script_OnUpdate_fn g_Script_OnUpdate;
extern Script_OnDestroy_fn g_Script_OnDestroy;
extern Script_Invoke_fn g_Script_Invoke;
extern Script_Destroy_fn g_Script_Destroy;
extern ReloadScripts_fn g_ReloadScripts;
extern ClearComponentCaches_fn g_ClearComponentCaches;
// Creates and initializes the managed script instance for an entity
void CallOnCreate(void* instance, int entityID);
void CallOnUpdate(void* instance, float dt);
void CallOnDestroy(void* instance);
void ReloadScripts();
void CallOnValidateForAllScripts();
void CallOnValidateForSubtree(Scene& scene, EntityID rootId);

// Returns true if the .NET runtime has been initialized and delegates can be resolved
bool IsDotnetRuntimeReady();
void SetupEntityInterop(std::filesystem::path fullPath);
void SetupInputInterop(std::filesystem::path fullPath);
void SetupReflectionInterop(std::filesystem::path fullPath);
void SetupPhysicsInterop(const std::wstring& assemblyPath);
void SetupResourceInterop(std::filesystem::path fullPath);

// SetField pointer
#include "managed/interop/ScriptReflectionInterop.h"

// Creates a managed script instance (returns GCHandle pointer held as void*)
void* CreateScriptInstance(const std::string& className);

// Function pointer types
// World position (Unity-style: transform.position = world position)
using GetEntityWorldPosition_fn = void(*)(int entityID, float* outX, float* outY, float* outZ);
using SetEntityWorldPosition_fn = void(*)(int entityID, float x, float y, float z);
// Local position (direct Transform.Position)
using GetEntityLocalPosition_fn = void(*)(int entityID, float* outX, float* outY, float* outZ);
using SetEntityLocalPosition_fn = void(*)(int entityID, float x, float y, float z);
using FindEntityByName_fn      = int (*)(const char* name);
using GetEntityName_fn         = const char* (*)(int entityID);
using GetEntities_fn           = int* (*)();
using GetEntityCount_fn        = int (*)();
// Entity management
using CreateEntity_fn          = int (*)(const char* name);
using DestroyEntity_fn         = void(*)(int entityID);
// Rotation & Scale 
using GetEntityRotation_fn     = void(*)(int entityID, float* outX, float* outY, float* outZ);
using SetEntityRotation_fn     = void(*)(int entityID, float x, float y, float z);
using GetEntityRotationQuat_fn = void(*)(int entityID, float* outX, float* outY, float* outZ, float* outW);
using SetEntityRotationQuat_fn = void(*)(int entityID, float x, float y, float z, float w);
using GetEntityScale_fn        = void(*)(int entityID, float* outX, float* outY, float* outZ);
using SetEntityScale_fn        = void(*)(int entityID, float x, float y, float z);
// Physics
using SetLinearVelocity_fn     = void(*)(int entityID, float x, float y, float z);
using SetAngularVelocity_fn    = void(*)(int entityID, float x, float y, float z);
// Visibility/Active
using SetEntityVisible_fn      = void(*)(int, bool);
using GetEntityVisible_fn      = bool(*)(int);
using SetEntityPresentationHidden_fn = void(*)(int, bool);
using GetEntityPresentationHidden_fn = bool(*)(int);
using SetEntityActive_fn       = void(*)(int, bool);
using GetEntityActive_fn       = bool(*)(int);

// --- Component Interop Function Pointer Types ---
using HasComponent_fn = bool (*)(int, const char*);
using AddComponent_fn = void (*)(int, const char*);
using RemoveComponent_fn = void (*)(int, const char*);
using AddScript_fn = void (*)(int, const char*);
using GetLightType_fn = int (*)(int);
using SetLightType_fn = void (*)(int, int);
using GetLightColor_fn = void (*)(int, float*, float*, float*);
using SetLightColor_fn = void (*)(int, float, float, float);
using GetLightIntensity_fn = float (*)(int);
using SetLightIntensity_fn = void (*)(int, float);
using GetRigidBodyMass_fn = float (*)(int);
using SetRigidBodyMass_fn = void (*)(int, float);
using GetRigidBodyIsKinematic_fn = bool (*)(int);
using SetRigidBodyIsKinematic_fn = void (*)(int, bool);
using GetRigidBodyUseGravity_fn = bool (*)(int);
using SetRigidBodyUseGravity_fn = void (*)(int, bool);
using GetRigidBodyCollisionMask_fn = uint32_t (*)(int);
using SetRigidBodyCollisionMask_fn = void (*)(int, uint32_t);
using SetRigidBodyPhysicsLayer_fn = bool (*)(int, const char*);
using GetRigidBodyLinearVelocity_fn = void (*)(int, float*, float*, float*);
using SetRigidBodyLinearVelocity_fn = void (*)(int, float, float, float);
using GetRigidBodyAngularVelocity_fn = void (*)(int, float*, float*, float*);
using SetRigidBodyAngularVelocity_fn = void (*)(int, float, float, float);
using ApplyRigidBodyForce_fn = void (*)(int, float, float, float);
using ApplyRigidBodyTorque_fn = void (*)(int, float, float, float);
using ApplyRigidBodyImpulse_fn = void (*)(int, float, float, float);
using ApplyRigidBodyAngularImpulse_fn = void (*)(int, float, float, float);
using RigidBody_GetDebugSummary_fn = const char* (*)(int);
using Collider_GetOffset_fn = void (*)(int, float*, float*, float*);

// Terrain component interop
using Terrain_GetHeightAtWorld_fn = bool (*)(int, float, float, float*);
using Terrain_GetNormalAtWorld_fn = bool (*)(int, float, float, float*, float*, float*);
using Terrain_GetNearestPoint_fn = bool (*)(int, float, float, float*, float*, float*);
using Terrain_Raycast_fn = bool (*)(int, float, float, float, float, float, float, float*, float*, float*, float*, float*, float*);
using Terrain_GetDominantLayerAtWorld_fn = bool (*)(int, float, float, int*, float*);
using Terrain_SetHeightAtWorld_fn = bool (*)(int, float, float, float);
using Terrain_ApplyHeightDelta_fn = bool (*)(int, float, float, float, float, float);
using Terrain_GetInstancerLayerCount_fn = int (*)(int);
using Terrain_GetInstancerLayerName_fn = const char* (*)(int, int);
using Terrain_SetInstancerLayerEnabled_fn = bool (*)(int, int, bool);
using Terrain_SetInstancerLayerDensity_fn = bool (*)(int, int, float);
using Terrain_RegenerateInstancers_fn = bool (*)(int);

using Spline_GetControlPointCount_fn = int (*)(int);
using Spline_GetControlPoint_fn = bool (*)(int, int, float*, float*, float*);
using Spline_GetSampledPointCount_fn = int (*)(int);
using Spline_GetSampledPoint_fn = bool (*)(int, int, float*, float*, float*);
using Spline_GetNearestPoint_fn = bool (*)(int, float, float, float, float*, float*, float*, float*);
using Spline_GetPointAtNormalized_fn = bool (*)(int, float, float*, float*, float*);

// PortalComponent
using Portal_GetEnabled_fn = bool (*)(int);
using Portal_SetEnabled_fn = void (*)(int, bool);
using Portal_GetTargetScenePath_fn = const char* (*)(int);
using Portal_SetTargetScenePath_fn = void (*)(int, const char*);
using Portal_GetTargetPortalGuid_fn = const char* (*)(int);
using Portal_SetTargetPortalGuid_fn = void (*)(int, const char*);
using Portal_GetTargetPortalPath_fn = const char* (*)(int);
using Portal_SetTargetPortalPath_fn = void (*)(int, const char*);
using Portal_GetVec3_fn = void (*)(int, float*, float*, float*);
using Portal_SetVec3_fn = void (*)(int, float, float, float);
using Portal_GetBool_fn = bool (*)(int);
using Portal_SetBool_fn = void (*)(int, bool);
using Portal_GetFloat_fn = float (*)(int);
using Portal_SetFloat_fn = void (*)(int, float);
// Camera layer mask interop
using GetCameraLayerMask_fn = unsigned int (*)(int);
using SetCameraLayerMask_fn = void (*)(int, unsigned int);
using Camera_SetLayerMaskByName_fn = void (*)(int, const char*, bool);
// Camera settings interop (12 functions)
using GetCameraActive_fn = bool (*)(int);
using SetCameraActive_fn = void (*)(int, bool);
using GetCameraPriority_fn = int (*)(int);
using SetCameraPriority_fn = void (*)(int, int);
using GetCameraFieldOfView_fn = float (*)(int);
using SetCameraFieldOfView_fn = void (*)(int, float);
using GetCameraNearClip_fn = float (*)(int);
using SetCameraNearClip_fn = void (*)(int, float);
using GetCameraFarClip_fn = float (*)(int);
using SetCameraFarClip_fn = void (*)(int, float);
using GetCameraIsPerspective_fn = bool (*)(int);
using SetCameraIsPerspective_fn = void (*)(int, bool);
// Character Controller
using CC_SetDesiredVelocity_fn = void (*)(int, float, float, float);
using CC_GetDesiredVelocity_fn = void (*)(int, float*, float*, float*);
using CC_SetVerticalVelocity_fn = void (*)(int, float);
using CC_GetVerticalVelocity_fn = float (*)(int);
using CC_Jump_fn = void (*)(int, float);
using CC_IsGrounded_fn = bool (*)(int);
using CC_SetPosition_fn = void (*)(int, float, float, float);
using CC_GetCollisionMask_fn = uint32_t (*)(int);
using CC_SetCollisionMask_fn = void (*)(int, uint32_t);
using SetBlendShapeWeight_fn = void(*)(int, const char*, float);
using GetBlendShapeWeight_fn = float(*)(int, const char*);
using GetBlendShapeCount_fn = int(*)(int);
using GetBlendShapeName_fn = const char*(*)(int, int);
// Unified Morph interop
using UnifiedMorph_GetCount_fn = int(*)(int);
using UnifiedMorph_GetName_fn = const char*(*)(int, int);
using UnifiedMorph_GetWeight_fn = float(*)(int, int);
using UnifiedMorph_SetWeight_fn = void(*)(int, int, float);
using UnifiedMorph_PropagateAll_fn = void(*)(int);
// TintMaskController interop
using TintController_HasComponent_fn = bool(*)(int);
using TintController_GetNamePattern_fn = const char*(*)(int);
using TintController_SetNamePattern_fn = void(*)(int, const char*);
using TintController_GetBaseTint_fn = void(*)(int, float*, float*, float*, float*);
using TintController_SetBaseTint_fn = void(*)(int, float, float, float, float);
using TintController_GetTintColor_fn = void(*)(int, int, float*, float*, float*, float*);
using TintController_SetTintColor_fn = void(*)(int, int, float, float, float, float);
using TintController_GetUseTintMask_fn = bool(*)(int);
using TintController_SetUseTintMask_fn = void(*)(int, bool);
using TintController_GetUsePbrOverrides_fn = bool(*)(int);
using TintController_SetUsePbrOverrides_fn = void(*)(int, bool);
using TintController_GetPbrMetallic_fn = float(*)(int);
using TintController_SetPbrMetallic_fn = void(*)(int, float);
using TintController_GetPbrRoughness_fn = float(*)(int);
using TintController_SetPbrRoughness_fn = void(*)(int, float);
using TintController_GetPbrEmissionColor_fn = void(*)(int, float*, float*, float*);
using TintController_SetPbrEmissionColor_fn = void(*)(int, float, float, float);
using TintController_GetPbrEmissionStrength_fn = float(*)(int);
using TintController_SetPbrEmissionStrength_fn = void(*)(int, float);
using TintController_GetGlobalBlendMode_fn = int(*)(int);
using TintController_SetGlobalBlendMode_fn = void(*)(int, int);
using TintController_GetAutoIncludeParentedSkinnedMeshes_fn = bool(*)(int);
using TintController_SetAutoIncludeParentedSkinnedMeshes_fn = void(*)(int, bool);
using TintController_Refresh_fn = void(*)(int);
using TintController_ClearTargets_fn = void(*)(int);
using TintController_RemoveTargetsForEntity_fn = void(*)(int, int);
using TintController_AddTarget_fn = void(*)(int, int, int, int, bool, float, float, float, float);
using TintController_GetTrackedTargetCount_fn = int(*)(int);
using TintController_GetTrackedTargetEntity_fn = int(*)(int, int);
// BoneAttachment interop
using BoneAttachment_HasComponent_fn = bool(*)(int);
using BoneAttachment_GetEnabled_fn = bool(*)(int);
using BoneAttachment_SetEnabled_fn = void(*)(int, bool);
using BoneAttachment_GetBoneName_fn = const char*(*)(int);
using BoneAttachment_SetBoneName_fn = void(*)(int, const char*);
using BoneAttachment_GetLocalPosition_fn = void(*)(int, float*, float*, float*);
using BoneAttachment_SetLocalPosition_fn = void(*)(int, float, float, float);
using BoneAttachment_GetLocalRotation_fn = void(*)(int, float*, float*, float*);
using BoneAttachment_SetLocalRotation_fn = void(*)(int, float, float, float);
using BoneAttachment_GetLocalScale_fn = void(*)(int, float*, float*, float*);
using BoneAttachment_SetLocalScale_fn = void(*)(int, float, float, float);
using BoneAttachment_GetInheritRotation_fn = bool(*)(int);
using BoneAttachment_SetInheritRotation_fn = void(*)(int, bool);
using BoneAttachment_GetInheritScale_fn = bool(*)(int);
using BoneAttachment_SetInheritScale_fn = void(*)(int, bool);
using BoneAttachment_IsResolved_fn = bool(*)(int);
using BoneAttachment_InvalidateResolution_fn = void(*)(int);
using BoneAttachment_GetSkeletonEntity_fn = int(*)(int);
using BoneAttachment_SetSkeletonEntity_fn = void(*)(int, int);
using GetEntityByID_fn = int (*)(int);

// SyncContext controls
using FlushSyncContext_fn      = void(__stdcall*)();
using ClearSyncContext_fn      = void(__stdcall*)();
using RegisterScriptCallbackFn = void(*)(const char*, int priority);

using InstallSyncContext_fn = void(__stdcall*)();
using EnsureInstalled_fn = void(__stdcall*)();

// Pointers to assign in Claymore main.cpp or initialization
extern GetEntityWorldPosition_fn GetEntityWorldPositionPtr;
extern SetEntityWorldPosition_fn SetEntityWorldPositionPtr;
extern GetEntityLocalPosition_fn GetEntityLocalPositionPtr;
extern SetEntityLocalPosition_fn SetEntityLocalPositionPtr;
extern FindEntityByName_fn    FindEntityByNamePtr;
extern GetEntityName_fn       GetEntityNamePtr;
extern GetEntities_fn         GetEntitiesPtr;
extern GetEntityCount_fn      GetEntityCountPtr;
extern CreateEntity_fn        CreateEntityPtr;
extern DestroyEntity_fn       DestroyEntityPtr;
extern GetEntityByID_fn       GetEntityByIDPtr;
extern GetEntityRotation_fn   GetEntityRotationPtr;
extern SetEntityRotation_fn   SetEntityRotationPtr;
extern GetEntityRotationQuat_fn GetEntityRotationQuatPtr;
extern SetEntityRotationQuat_fn SetEntityRotationQuatPtr;
extern GetEntityScale_fn      GetEntityScalePtr;
extern SetEntityScale_fn      SetEntityScalePtr;
extern SetLinearVelocity_fn   SetLinearVelocityPtr;
extern SetAngularVelocity_fn  SetAngularVelocityPtr;
extern SetEntityVisible_fn    SetEntityVisiblePtr;
extern GetEntityVisible_fn    GetEntityVisiblePtr;
extern SetEntityPresentationHidden_fn SetEntityPresentationHiddenPtr;
extern GetEntityPresentationHidden_fn GetEntityPresentationHiddenPtr;
extern SetEntityActive_fn     SetEntityActivePtr;
extern GetEntityActive_fn     GetEntityActivePtr;
using IsSceneBeingDestroyed_fn = bool(*)();
extern IsSceneBeingDestroyed_fn IsSceneBeingDestroyedPtr;

// Parenting interop
using SetEntityParent_fn = void(*)(int, int, bool);
using GetEntityParent_fn = int(*)(int);
using GetEntityChildren_fn = int*(*)(int);
using GetEntityChildCount_fn = int(*)(int);
using FindChildByName_fn = int(*)(int, const char*);
using FindDescendantByName_fn = int(*)(int, const char*);
using CreateEntityWithParent_fn = int(*)(const char*, int);
extern SetEntityParent_fn SetEntityParentPtr;
extern GetEntityParent_fn GetEntityParentPtr;
extern GetEntityChildren_fn GetEntityChildrenPtr;
extern GetEntityChildCount_fn GetEntityChildCountPtr;
extern FindChildByName_fn FindChildByNamePtr;
extern FindDescendantByName_fn FindDescendantByNamePtr;
extern CreateEntityWithParent_fn CreateEntityWithParentPtr;
// Duplication and UI mouse position
using DuplicateEntity_fn = int(*)(int);
using GetUIMousePosition_fn = bool(*)(float*, float*);
extern DuplicateEntity_fn DuplicateEntityPtr;
extern GetUIMousePosition_fn GetUIMousePositionPtr;

// --- Managed Logging ---
using ManagedLog_fn = void(*)(int, const char*);
extern ManagedLog_fn ManagedLogPtr;

// --- Component Interop Function Pointers ---
extern HasComponent_fn HasComponentPtr;
extern AddComponent_fn AddComponentPtr;
extern RemoveComponent_fn RemoveComponentPtr;
extern AddScript_fn AddScriptPtr;
extern GetLightType_fn GetLightTypePtr;
extern SetLightType_fn SetLightTypePtr;
extern GetLightColor_fn GetLightColorPtr;
extern SetLightColor_fn SetLightColorPtr;
extern GetLightIntensity_fn GetLightIntensityPtr;
extern SetLightIntensity_fn SetLightIntensityPtr;
extern GetRigidBodyMass_fn GetRigidBodyMassPtr;
extern SetRigidBodyMass_fn SetRigidBodyMassPtr;
extern GetRigidBodyIsKinematic_fn GetRigidBodyIsKinematicPtr;
extern SetRigidBodyIsKinematic_fn SetRigidBodyIsKinematicPtr;
extern GetRigidBodyUseGravity_fn GetRigidBodyUseGravityPtr;
extern SetRigidBodyUseGravity_fn SetRigidBodyUseGravityPtr;
extern GetRigidBodyCollisionMask_fn GetRigidBodyCollisionMaskPtr;
extern SetRigidBodyCollisionMask_fn SetRigidBodyCollisionMaskPtr;
extern SetRigidBodyPhysicsLayer_fn SetRigidBodyPhysicsLayerPtr;
extern GetRigidBodyLinearVelocity_fn GetRigidBodyLinearVelocityPtr;
extern SetRigidBodyLinearVelocity_fn SetRigidBodyLinearVelocityPtr;
extern GetRigidBodyAngularVelocity_fn GetRigidBodyAngularVelocityPtr;
extern SetRigidBodyAngularVelocity_fn SetRigidBodyAngularVelocityPtr;
extern ApplyRigidBodyForce_fn ApplyRigidBodyForcePtr;
extern ApplyRigidBodyTorque_fn ApplyRigidBodyTorquePtr;
extern ApplyRigidBodyImpulse_fn ApplyRigidBodyImpulsePtr;
extern ApplyRigidBodyAngularImpulse_fn ApplyRigidBodyAngularImpulsePtr;
extern RigidBody_GetDebugSummary_fn RigidBody_GetDebugSummaryPtr;
extern Collider_GetOffset_fn Collider_GetOffsetPtr;

extern Terrain_GetHeightAtWorld_fn Terrain_GetHeightAtWorldPtr;
extern Terrain_GetNormalAtWorld_fn Terrain_GetNormalAtWorldPtr;
extern Terrain_GetNearestPoint_fn Terrain_GetNearestPointPtr;
extern Terrain_Raycast_fn Terrain_RaycastPtr;
extern Terrain_GetDominantLayerAtWorld_fn Terrain_GetDominantLayerAtWorldPtr;
extern Terrain_SetHeightAtWorld_fn Terrain_SetHeightAtWorldPtr;
extern Terrain_ApplyHeightDelta_fn Terrain_ApplyHeightDeltaPtr;
extern Terrain_GetInstancerLayerCount_fn Terrain_GetInstancerLayerCountPtr;
extern Terrain_GetInstancerLayerName_fn Terrain_GetInstancerLayerNamePtr;
extern Terrain_SetInstancerLayerEnabled_fn Terrain_SetInstancerLayerEnabledPtr;
extern Terrain_SetInstancerLayerDensity_fn Terrain_SetInstancerLayerDensityPtr;
extern Terrain_RegenerateInstancers_fn Terrain_RegenerateInstancersPtr;
extern Spline_GetControlPointCount_fn Spline_GetControlPointCountPtr;
extern Spline_GetControlPoint_fn Spline_GetControlPointPtr;
extern Spline_GetSampledPointCount_fn Spline_GetSampledPointCountPtr;
extern Spline_GetSampledPoint_fn Spline_GetSampledPointPtr;
extern Spline_GetNearestPoint_fn Spline_GetNearestPointPtr;
extern Spline_GetPointAtNormalized_fn Spline_GetPointAtNormalizedPtr;
// Portal
extern Portal_GetEnabled_fn Portal_GetEnabledPtr;
extern Portal_SetEnabled_fn Portal_SetEnabledPtr;
extern Portal_GetTargetScenePath_fn Portal_GetTargetScenePathPtr;
extern Portal_SetTargetScenePath_fn Portal_SetTargetScenePathPtr;
extern Portal_GetTargetPortalGuid_fn Portal_GetTargetPortalGuidPtr;
extern Portal_SetTargetPortalGuid_fn Portal_SetTargetPortalGuidPtr;
extern Portal_GetTargetPortalPath_fn Portal_GetTargetPortalPathPtr;
extern Portal_SetTargetPortalPath_fn Portal_SetTargetPortalPathPtr;
extern Portal_GetVec3_fn Portal_GetEntryOffsetPtr;
extern Portal_SetVec3_fn Portal_SetEntryOffsetPtr;
extern Portal_GetVec3_fn Portal_GetExitOffsetPtr;
extern Portal_SetVec3_fn Portal_SetExitOffsetPtr;
extern Portal_GetBool_fn Portal_GetAutoDetectPtr;
extern Portal_SetBool_fn Portal_SetAutoDetectPtr;
extern Portal_GetFloat_fn Portal_GetTriggerRadiusPtr;
extern Portal_SetFloat_fn Portal_SetTriggerRadiusPtr;
extern Portal_GetBool_fn Portal_GetFireExitEventsPtr;
extern Portal_SetBool_fn Portal_SetFireExitEventsPtr;
extern CC_SetDesiredVelocity_fn CC_SetDesiredVelocityPtr;
extern CC_GetDesiredVelocity_fn CC_GetDesiredVelocityPtr;
extern CC_SetVerticalVelocity_fn CC_SetVerticalVelocityPtr;
extern CC_GetVerticalVelocity_fn CC_GetVerticalVelocityPtr;
extern CC_Jump_fn CC_JumpPtr;
extern CC_IsGrounded_fn CC_IsGroundedPtr;
extern CC_SetPosition_fn CC_SetPositionPtr;
extern CC_GetCollisionMask_fn CC_GetCollisionMaskPtr;
extern CC_SetCollisionMask_fn CC_SetCollisionMaskPtr;
// Camera settings pointers (12 entries)
extern GetCameraActive_fn GetCameraActivePtr;
extern SetCameraActive_fn SetCameraActivePtr;
extern GetCameraPriority_fn GetCameraPriorityPtr;
extern SetCameraPriority_fn SetCameraPriorityPtr;
extern GetCameraFieldOfView_fn GetCameraFieldOfViewPtr;
extern SetCameraFieldOfView_fn SetCameraFieldOfViewPtr;
extern GetCameraNearClip_fn GetCameraNearClipPtr;
extern SetCameraNearClip_fn SetCameraNearClipPtr;
extern GetCameraFarClip_fn GetCameraFarClipPtr;
extern SetCameraFarClip_fn SetCameraFarClipPtr;
extern GetCameraIsPerspective_fn GetCameraIsPerspectivePtr;
extern SetCameraIsPerspective_fn SetCameraIsPerspectivePtr;
extern SetBlendShapeWeight_fn SetBlendShapeWeightPtr;
extern GetBlendShapeWeight_fn GetBlendShapeWeightPtr;
extern GetBlendShapeCount_fn GetBlendShapeCountPtr;
extern GetBlendShapeName_fn GetBlendShapeNamePtr;
extern UnifiedMorph_GetCount_fn UnifiedMorph_GetCountPtr;
extern UnifiedMorph_GetName_fn UnifiedMorph_GetNamePtr;
extern UnifiedMorph_GetWeight_fn UnifiedMorph_GetWeightPtr;
extern UnifiedMorph_SetWeight_fn UnifiedMorph_SetWeightPtr;
extern UnifiedMorph_PropagateAll_fn UnifiedMorph_PropagateAllPtr;
// TintMaskController
extern TintController_HasComponent_fn TintController_HasComponentPtr;
extern TintController_GetNamePattern_fn TintController_GetNamePatternPtr;
extern TintController_SetNamePattern_fn TintController_SetNamePatternPtr;
extern TintController_GetBaseTint_fn TintController_GetBaseTintPtr;
extern TintController_SetBaseTint_fn TintController_SetBaseTintPtr;
extern TintController_GetTintColor_fn TintController_GetTintColorPtr;
extern TintController_SetTintColor_fn TintController_SetTintColorPtr;
extern TintController_GetUseTintMask_fn TintController_GetUseTintMaskPtr;
extern TintController_SetUseTintMask_fn TintController_SetUseTintMaskPtr;
extern TintController_GetUsePbrOverrides_fn TintController_GetUsePbrOverridesPtr;
extern TintController_SetUsePbrOverrides_fn TintController_SetUsePbrOverridesPtr;
extern TintController_GetPbrMetallic_fn TintController_GetPbrMetallicPtr;
extern TintController_SetPbrMetallic_fn TintController_SetPbrMetallicPtr;
extern TintController_GetPbrRoughness_fn TintController_GetPbrRoughnessPtr;
extern TintController_SetPbrRoughness_fn TintController_SetPbrRoughnessPtr;
extern TintController_GetPbrEmissionColor_fn TintController_GetPbrEmissionColorPtr;
extern TintController_SetPbrEmissionColor_fn TintController_SetPbrEmissionColorPtr;
extern TintController_GetPbrEmissionStrength_fn TintController_GetPbrEmissionStrengthPtr;
extern TintController_SetPbrEmissionStrength_fn TintController_SetPbrEmissionStrengthPtr;
extern TintController_GetGlobalBlendMode_fn TintController_GetGlobalBlendModePtr;
extern TintController_SetGlobalBlendMode_fn TintController_SetGlobalBlendModePtr;
extern TintController_GetAutoIncludeParentedSkinnedMeshes_fn TintController_GetAutoIncludeParentedSkinnedMeshesPtr;
extern TintController_SetAutoIncludeParentedSkinnedMeshes_fn TintController_SetAutoIncludeParentedSkinnedMeshesPtr;
extern TintController_Refresh_fn TintController_RefreshPtr;
extern TintController_ClearTargets_fn TintController_ClearTargetsPtr;
extern TintController_RemoveTargetsForEntity_fn TintController_RemoveTargetsForEntityPtr;
extern TintController_AddTarget_fn TintController_AddTargetPtr;
extern TintController_GetTrackedTargetCount_fn TintController_GetTrackedTargetCountPtr;
extern TintController_GetTrackedTargetEntity_fn TintController_GetTrackedTargetEntityPtr;

// BoneAttachment
extern BoneAttachment_HasComponent_fn BoneAttachment_HasComponentPtr;
extern BoneAttachment_GetEnabled_fn BoneAttachment_GetEnabledPtr;
extern BoneAttachment_SetEnabled_fn BoneAttachment_SetEnabledPtr;
extern BoneAttachment_GetBoneName_fn BoneAttachment_GetBoneNamePtr;
extern BoneAttachment_SetBoneName_fn BoneAttachment_SetBoneNamePtr;
extern BoneAttachment_GetLocalPosition_fn BoneAttachment_GetLocalPositionPtr;
extern BoneAttachment_SetLocalPosition_fn BoneAttachment_SetLocalPositionPtr;
extern BoneAttachment_GetLocalRotation_fn BoneAttachment_GetLocalRotationPtr;
extern BoneAttachment_SetLocalRotation_fn BoneAttachment_SetLocalRotationPtr;
extern BoneAttachment_GetLocalScale_fn BoneAttachment_GetLocalScalePtr;
extern BoneAttachment_SetLocalScale_fn BoneAttachment_SetLocalScalePtr;
extern BoneAttachment_GetInheritRotation_fn BoneAttachment_GetInheritRotationPtr;
extern BoneAttachment_SetInheritRotation_fn BoneAttachment_SetInheritRotationPtr;
extern BoneAttachment_GetInheritScale_fn BoneAttachment_GetInheritScalePtr;
extern BoneAttachment_SetInheritScale_fn BoneAttachment_SetInheritScalePtr;
extern BoneAttachment_IsResolved_fn BoneAttachment_IsResolvedPtr;
extern BoneAttachment_InvalidateResolution_fn BoneAttachment_InvalidateResolutionPtr;
extern BoneAttachment_GetSkeletonEntity_fn BoneAttachment_GetSkeletonEntityPtr;
extern BoneAttachment_SetSkeletonEntity_fn BoneAttachment_SetSkeletonEntityPtr;

// Optional UI text/panel/canvas interop (appended at the end of component table)
extern void (*UI_Text_GetTextPtr)(int, const char**);
extern void (*UI_Text_SetTextPtr)(int, const char*);
extern float (*UI_Text_GetOpacityPtr)(int);
extern void (*UI_Text_SetOpacityPtr)(int, float);
extern int (*UI_Text_GetAlignmentPtr)(int);
extern void (*UI_Text_SetAlignmentPtr)(int, int);
extern float (*UI_Panel_GetOpacityPtr)(int);
extern void (*UI_Panel_SetOpacityPtr)(int, float);
extern bool (*UI_Panel_GetVisiblePtr)(int);
extern void (*UI_Panel_SetVisiblePtr)(int, bool);
extern void (*UI_Panel_GetSizePtr)(int, float*, float*);
extern void (*UI_Panel_SetSizePtr)(int, float, float);
extern void (*UI_Panel_GetTintColorPtr)(int, float*, float*, float*, float*);
extern void (*UI_Panel_SetTintColorPtr)(int, float, float, float, float);
extern bool (*UI_Panel_GetAnchorEnabledPtr)(int);
extern void (*UI_Panel_SetAnchorEnabledPtr)(int, bool);
extern int (*UI_Panel_GetAnchorPtr)(int);
extern void (*UI_Panel_SetAnchorPtr)(int, int);
extern void (*UI_Panel_GetAnchorOffsetPtr)(int, float*, float*);
extern void (*UI_Panel_SetAnchorOffsetPtr)(int, float, float);
extern bool (*UI_Panel_GetAnchorToParentPtr)(int);
extern void (*UI_Panel_SetAnchorToParentPtr)(int, bool);
extern int (*UI_Panel_GetZOrderPtr)(int);
extern void (*UI_Panel_SetZOrderPtr)(int, int);
extern bool (*UI_Panel_IsHoveredPtr)(int);
extern bool (*UI_Panel_IsPressedPtr)(int);
extern bool (*UI_Panel_IsDraggingPtr)(int);
extern bool (*UI_Panel_DragStartedPtr)(int);
extern bool (*UI_Panel_DragEndedPtr)(int);
extern bool (*UI_Panel_WasDroppedPtr)(int);
extern int (*UI_Panel_GetDropSourcePtr)(int);
extern int (*UI_Panel_GetDropTargetPtr)(int);
extern bool (*UI_Panel_GetAllowDragPtr)(int);
extern void (*UI_Panel_SetAllowDragPtr)(int, bool);
extern bool (*UI_Panel_GetAllowDropPtr)(int);
extern void (*UI_Panel_SetAllowDropPtr)(int, bool);
extern void (*UI_Panel_SetTexturePtr)(int, uint64_t, uint64_t, int);
extern void (*UI_Panel_GetTexturePtr)(int, uint64_t*, uint64_t*, int*);
extern bool (*UI_Panel_GetDriveChildrenOpacityPtr)(int);
extern void (*UI_Panel_SetDriveChildrenOpacityPtr)(int, bool);
// UI Rect
extern bool (*UI_Rect_GetAnchorToParentPtr)(int);
extern void (*UI_Rect_SetAnchorToParentPtr)(int, bool);
extern float (*UI_Rect_GetHorizontalAnchorPtr)(int);
extern void (*UI_Rect_SetHorizontalAnchorPtr)(int, float);
extern float (*UI_Rect_GetVerticalAnchorPtr)(int);
extern void (*UI_Rect_SetVerticalAnchorPtr)(int, float);
extern void (*UI_Rect_GetPivotPtr)(int, float*, float*);
extern void (*UI_Rect_SetPivotPtr)(int, float, float);
extern void (*UI_Rect_GetOffsetPtr)(int, float*, float*);
extern void (*UI_Rect_SetOffsetPtr)(int, float, float);
extern void (*UI_Rect_GetSizePtr)(int, float*, float*);
extern void (*UI_Rect_SetSizePtr)(int, float, float);
extern float (*UI_Canvas_GetOpacityPtr)(int);
extern void (*UI_Canvas_SetOpacityPtr)(int, float);

extern FlushSyncContext_fn   FlushSyncContextPtr;
extern ClearSyncContext_fn   ClearSyncContextPtr;

extern InstallSyncContext_fn InstallSyncContextPtr;
extern EnsureInstalled_fn    EnsureInstalledPtr;

// Tween interop function pointer types
using Tween_Position_fn = void(*)(int, float, float, float, float, int);
using Tween_RotationEuler_fn = void(*)(int, float, float, float, float, int);
using Tween_Scale_fn = void(*)(int, float, float, float, float, int);
using Tween_LightIntensity_fn = void(*)(int, float, float, int);
using Tween_ManagedFloat_fn = void(*)(int, const char*, const char*, float, float, int);
using Tween_ManagedVec3_fn = void(*)(int, const char*, const char*, float, float, float, float, int);
using Tween_SetFinishedCallback_fn = void(*)(void*);

extern Tween_Position_fn Tween_PositionPtr;
extern Tween_RotationEuler_fn Tween_RotationEulerPtr;
extern Tween_Scale_fn Tween_ScalePtr;
extern Tween_LightIntensity_fn Tween_LightIntensityPtr;
extern Tween_ManagedFloat_fn Tween_ManagedFloatPtr;
extern Tween_ManagedVec3_fn Tween_ManagedVec3Ptr;
extern Tween_SetFinishedCallback_fn Tween_SetFinishedCallbackPtr;

// Navigation interop raw pointer getters (resolved from NavInterop.cpp)
extern "C" void* Get_Nav_FindPath_Ptr();
extern "C" void* Get_Nav_Agent_SetDest_Ptr();
extern "C" void* Get_Nav_Agent_Stop_Ptr();
extern "C" void* Get_Nav_Agent_Warp_Ptr();
extern "C" void* Get_Nav_Agent_Remaining_Ptr();
extern "C" void* Get_Nav_SetOnPathComplete_Ptr();
extern "C" void* Get_Nav_Agent_IsStopped_Ptr();
extern "C" void* Get_Nav_Agent_IsMoving_Ptr();
extern "C" void* Get_Nav_Agent_HasPath_Ptr();
extern "C" void* Get_Nav_Agent_GetSpeed_Ptr();
extern "C" void* Get_Nav_Agent_SetSpeed_Ptr();
extern "C" void* Get_Nav_Agent_GetAccel_Ptr();
extern "C" void* Get_Nav_Agent_SetAccel_Ptr();
extern "C" void* Get_Nav_Agent_GetRadius_Ptr();
extern "C" void* Get_Nav_Agent_SetRadius_Ptr();
extern "C" void* Get_Nav_Agent_GetHeight_Ptr();
extern "C" void* Get_Nav_Agent_SetHeight_Ptr();
extern "C" void* Get_Nav_Agent_GetStopDist_Ptr();
extern "C" void* Get_Nav_Agent_SetStopDist_Ptr();
extern "C" void* Get_Nav_Agent_GetVelocityX_Ptr();
extern "C" void* Get_Nav_Agent_GetVelocityY_Ptr();
extern "C" void* Get_Nav_Agent_GetVelocityZ_Ptr();

// IK interop raw pointer getters (resolved from IKInterop.cpp)
extern "C" void* Get_IK_SetWeight_Ptr();
extern "C" void* Get_IK_SetTarget_Ptr();
extern "C" void* Get_IK_SetPole_Ptr();
extern "C" void* Get_IK_SetChain_Ptr();
extern "C" void* Get_IK_GetErrorMeters_Ptr();

// LookAt/Aim constraint interop raw pointer getters (resolved from LookAtInterop.cpp)
extern "C" void* Get_LookAt_SetEnabled_Ptr();
extern "C" void* Get_LookAt_SetWeight_Ptr();
extern "C" void* Get_LookAt_SetTarget_Ptr();
extern "C" void* Get_LookAt_SetSmoothingSpeed_Ptr();
extern "C" void* Get_LookAt_SetMaxAngles_Ptr();
extern "C" void* Get_LookAt_GetEnabled_Ptr();
extern "C" void* Get_LookAt_GetWeight_Ptr();
extern "C" void* Get_LookAt_SetMode_Ptr();
extern "C" void* Get_LookAt_GetMode_Ptr();
extern "C" void* Get_LookAt_SetTargetUsesNegativeZForward_Ptr();
extern "C" void* Get_LookAt_GetTargetUsesNegativeZForward_Ptr();

// Module Component Interop Function Pointer Types
using HasModuleComponent_fn = bool (*)(int, const char*);
using AddModuleComponent_fn = void (*)(int, const char*);
using RemoveModuleComponent_fn = void (*)(int, const char*);
using GetModuleComponent_fn = void* (*)(int, const char*);
using GetModuleComponentByFullName_fn = void* (*)(int, const char*);
using GetModuleFieldBool_fn = bool (*)(int, const char*, const char*);
using GetModuleFieldInt_fn = int (*)(int, const char*, const char*);
using GetModuleFieldInt64_fn = long long (*)(int, const char*, const char*);
using GetModuleFieldFloat_fn = float (*)(int, const char*, const char*);
using GetModuleFieldDouble_fn = double (*)(int, const char*, const char*);
using GetModuleFieldString_fn = const char* (*)(int, const char*, const char*);
using GetModuleFieldVec2_fn = void (*)(int, const char*, const char*, float*, float*);
using GetModuleFieldVec3_fn = void (*)(int, const char*, const char*, float*, float*, float*);
using GetModuleFieldVec4_fn = void (*)(int, const char*, const char*, float*, float*, float*, float*);
using GetModuleFieldQuat_fn = void (*)(int, const char*, const char*, float*, float*, float*, float*);
using SetModuleFieldBool_fn = void (*)(int, const char*, const char*, bool);
using SetModuleFieldInt_fn = void (*)(int, const char*, const char*, int);
using SetModuleFieldInt64_fn = void (*)(int, const char*, const char*, long long);
using SetModuleFieldFloat_fn = void (*)(int, const char*, const char*, float);
using SetModuleFieldDouble_fn = void (*)(int, const char*, const char*, double);
using SetModuleFieldString_fn = void (*)(int, const char*, const char*, const char*);
using SetModuleFieldVec2_fn = void (*)(int, const char*, const char*, float, float);
using SetModuleFieldVec3_fn = void (*)(int, const char*, const char*, float, float, float);
using SetModuleFieldVec4_fn = void (*)(int, const char*, const char*, float, float, float, float);
using SetModuleFieldQuat_fn = void (*)(int, const char*, const char*, float, float, float, float);

// Module Component Interop Function Pointers
extern HasModuleComponent_fn HasModuleComponentPtr;
extern AddModuleComponent_fn AddModuleComponentPtr;
extern RemoveModuleComponent_fn RemoveModuleComponentPtr;
extern GetModuleComponent_fn GetModuleComponentPtr;
extern GetModuleComponentByFullName_fn GetModuleComponentByFullNamePtr;
extern GetModuleFieldBool_fn GetModuleFieldBoolPtr;
extern GetModuleFieldInt_fn GetModuleFieldIntPtr;
extern GetModuleFieldInt64_fn GetModuleFieldInt64Ptr;
extern GetModuleFieldFloat_fn GetModuleFieldFloatPtr;
extern GetModuleFieldDouble_fn GetModuleFieldDoublePtr;
extern GetModuleFieldString_fn GetModuleFieldStringPtr;
extern GetModuleFieldVec2_fn GetModuleFieldVec2Ptr;
extern GetModuleFieldVec3_fn GetModuleFieldVec3Ptr;
extern GetModuleFieldVec4_fn GetModuleFieldVec4Ptr;
extern GetModuleFieldQuat_fn GetModuleFieldQuatPtr;
extern SetModuleFieldBool_fn SetModuleFieldBoolPtr;
extern SetModuleFieldInt_fn SetModuleFieldIntPtr;
extern SetModuleFieldInt64_fn SetModuleFieldInt64Ptr;
extern SetModuleFieldFloat_fn SetModuleFieldFloatPtr;
extern SetModuleFieldDouble_fn SetModuleFieldDoublePtr;
extern SetModuleFieldString_fn SetModuleFieldStringPtr;
extern SetModuleFieldVec2_fn SetModuleFieldVec2Ptr;
extern SetModuleFieldVec3_fn SetModuleFieldVec3Ptr;
extern SetModuleFieldVec4_fn SetModuleFieldVec4Ptr;
extern SetModuleFieldQuat_fn SetModuleFieldQuatPtr;
