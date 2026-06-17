#include "DotNetHost.h"
#include "core/physics/Physics.h"
#include "core/vfs/FileSystem.h"
#include "core/managed/EntityInteropLayout.h"
#include "core/managed/ScriptInterop.h"
#include "core/managed/RuntimeInterop.h"
#include "core/ecs/Scene.h"
extern "C" {
#include "nethost.h"
   }
#include "coreclr_delegates.h"
#include "hostfxr.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#undef min
#undef max
#include <psapi.h>
#include <iostream>
#include "editor/ui/Logger.h"
#include <fstream>
#include "managed/interop/ScriptSystem.h"
#include "editor/pipeline/AssetPipeline.h"
#include "managed/interop/InputInterop.h"
#include "managed/interop/ScriptReflectionInterop.h"
#include "core/managed/ScriptReflection.h"
#include "core/navigation/NavInterop.h" // for Get_Nav_*_Ptr declarations
#include "managed/interop/ComponentInterop.h"
#include "managed/interop/ModuleComponentInterop.h"
#include "core/world/PortalInterop.h"
#include "core/world/WorldGraphInterop.h"
#include "managed/interop/ScriptableInterop.h"
#include "core/utils/PrefabPerfDiagnostics.h"
#include <chrono>
#include <filesystem>
#include <navigation/NavInterop.h>
#include "managed/interop/TweenInterop.h"
#include "managed/interop/NodeGraphInterop.h"
#include "managed/interop/AnimationEventInterop.h"
#include "managed/interop/AnimationStateInterop.h"
#include "managed/interop/ResourceInterop.h"
#include "managed/interop/DialogueInterop.h"
#include "managed/interop/QuestInterop.h"
#include "core/multiplayer/MultiplayerBridge.h"

// Forward declarations for module component interop functions
extern "C" bool HasModuleComponent_Native(int entityId, const char* typeName);
extern "C" void AddModuleComponent_Native(int entityId, const char* typeName);
extern "C" void RemoveModuleComponent_Native(int entityId, const char* typeName);
extern "C" bool GetModuleFieldBool_Native(int entityId, const char* typeName, const char* fieldName);
extern "C" int GetModuleFieldInt_Native(int entityId, const char* typeName, const char* fieldName);
extern "C" long long GetModuleFieldInt64_Native(int entityId, const char* typeName, const char* fieldName);
extern "C" float GetModuleFieldFloat_Native(int entityId, const char* typeName, const char* fieldName);
extern "C" double GetModuleFieldDouble_Native(int entityId, const char* typeName, const char* fieldName);
extern "C" const char* GetModuleFieldString_Native(int entityId, const char* typeName, const char* fieldName);
extern "C" void GetModuleFieldVec2_Native(int entityId, const char* typeName, const char* fieldName, float* x, float* y);
extern "C" void GetModuleFieldVec3_Native(int entityId, const char* typeName, const char* fieldName, float* x, float* y, float* z);
extern "C" void GetModuleFieldVec4_Native(int entityId, const char* typeName, const char* fieldName, float* x, float* y, float* z, float* w);
extern "C" void GetModuleFieldQuat_Native(int entityId, const char* typeName, const char* fieldName, float* x, float* y, float* z, float* w);
extern "C" void SetModuleFieldBool_Native(int entityId, const char* typeName, const char* fieldName, bool value);
extern "C" void SetModuleFieldInt_Native(int entityId, const char* typeName, const char* fieldName, int value);
extern "C" void SetModuleFieldInt64_Native(int entityId, const char* typeName, const char* fieldName, long long value);
extern "C" void SetModuleFieldFloat_Native(int entityId, const char* typeName, const char* fieldName, float value);
extern "C" void SetModuleFieldDouble_Native(int entityId, const char* typeName, const char* fieldName, double value);
extern "C" void SetModuleFieldString_Native(int entityId, const char* typeName, const char* fieldName, const char* value);
extern "C" void SetModuleFieldVec2_Native(int entityId, const char* typeName, const char* fieldName, float x, float y);
extern "C" void SetModuleFieldVec3_Native(int entityId, const char* typeName, const char* fieldName, float x, float y, float z);
extern "C" void SetModuleFieldVec4_Native(int entityId, const char* typeName, const char* fieldName, float x, float y, float z, float w);
extern "C" void SetModuleFieldQuat_Native(int entityId, const char* typeName, const char* fieldName, float x, float y, float z, float w);

#include "editor/Project.h"


// Area interop pointer getters (exported from physics/area/AreaInterop.cpp)
extern "C" void* Get_Area_SetOnBodyEntered_Ptr();
extern "C" void* Get_Area_SetOnBodyExited_Ptr();
extern "C" void* Get_Area_SetOnAreaEntered_Ptr();
extern "C" void* Get_Area_SetOnAreaExited_Ptr();

// Collision interop pointer getters (exported from physics/collision/CollisionInterop.cpp)
extern "C" void* Get_Collision_SetOnEnter_Ptr();
extern "C" void* Get_Collision_SetOnExit_Ptr();

// Ragdoll interop pointer getters (exported from physics/ragdoll/RagdollInterop.cpp)
extern "C" void* Get_Ragdoll_Create_Ptr();
extern "C" void* Get_Ragdoll_Destroy_Ptr();
extern "C" void* Get_Ragdoll_Has_Ptr();
extern "C" void* Get_Ragdoll_Activate_Ptr();
extern "C" void* Get_Ragdoll_Deactivate_Ptr();
extern "C" void* Get_Ragdoll_ApplyImpulse_Ptr();
extern "C" void* Get_Ragdoll_ApplyImpulseToAll_Ptr();
extern "C" void* Get_Ragdoll_SetPhysicsLayer_Ptr();
extern "C" void* Get_Ragdoll_GetOwnerFromBone_Ptr();

// --------------------------------------------------------------------------------------
// World position (Unity-style: transform.position = world position)
extern "C" void GetEntityWorldPosition(int entityID, float* outX, float* outY, float* outZ);
extern "C" void SetEntityWorldPosition(int entityID, float x, float y, float z);
// Local position (direct Transform.Position)
extern "C" void GetEntityLocalPosition(int entityID, float* outX, float* outY, float* outZ);
extern "C" void SetEntityLocalPosition(int entityID, float x, float y, float z);
extern "C" int FindEntityByName(const char* name);
// --------------------------------------------------------------------------------------

// Local position pointers are now defined in EntityInterop.cpp

#define STR(s) L##s
using RegisterAllScriptsFn = void(CORECLR_DELEGATE_CALLTYPE*)(void* fnPtr);

// Stored once after load
static RegisterAllScriptsFn g_RegisterAllScripts = nullptr;
// ----------------------------------------
// HostFXR function pointers
// ----------------------------------------
static hostfxr_initialize_for_runtime_config_fn init_fptr = nullptr;
static hostfxr_get_runtime_delegate_fn get_delegate_fptr = nullptr;
static hostfxr_close_fn close_fptr = nullptr;
static load_assembly_and_get_function_pointer_fn load_assembly_and_get_function_pointer = nullptr;


// ----------------------------------------
// Script interop function pointers
// ----------------------------------------
Script_Create_fn g_Script_Create = nullptr;
Script_Bind_fn g_Script_Bind = nullptr;
Script_OnCreate_fn g_Script_OnCreate = nullptr;
Script_OnUpdate_fn g_Script_OnUpdate = nullptr;
Script_OnDestroy_fn g_Script_OnDestroy = nullptr;
Script_Invoke_fn g_Script_Invoke = nullptr;
ReloadScripts_fn g_ReloadScripts = nullptr;
Script_Destroy_fn g_Script_Destroy = nullptr;
ClearComponentCaches_fn g_ClearComponentCaches = nullptr;

InstallSyncContext_fn InstallSyncContextPtr = nullptr;
// EnsureInstalledPtr is defined in core/managed/ScriptInterop.cpp for both editor and runtime

// Module Component Interop Function Pointers
HasModuleComponent_fn HasModuleComponentPtr = &HasModuleComponent_Native;
AddModuleComponent_fn AddModuleComponentPtr = &AddModuleComponent_Native;
RemoveModuleComponent_fn RemoveModuleComponentPtr = &RemoveModuleComponent_Native;
GetModuleComponent_fn GetModuleComponentPtr = &GetModuleComponent_Native;
GetModuleComponentByFullName_fn GetModuleComponentByFullNamePtr = &GetModuleComponentByFullName_Native;
GetModuleFieldBool_fn GetModuleFieldBoolPtr = &GetModuleFieldBool_Native;
GetModuleFieldInt_fn GetModuleFieldIntPtr = &GetModuleFieldInt_Native;
GetModuleFieldInt64_fn GetModuleFieldInt64Ptr = &GetModuleFieldInt64_Native;
GetModuleFieldFloat_fn GetModuleFieldFloatPtr = &GetModuleFieldFloat_Native;
GetModuleFieldDouble_fn GetModuleFieldDoublePtr = &GetModuleFieldDouble_Native;
GetModuleFieldString_fn GetModuleFieldStringPtr = &GetModuleFieldString_Native;
GetModuleFieldVec2_fn GetModuleFieldVec2Ptr = &GetModuleFieldVec2_Native;
GetModuleFieldVec3_fn GetModuleFieldVec3Ptr = &GetModuleFieldVec3_Native;
GetModuleFieldVec4_fn GetModuleFieldVec4Ptr = &GetModuleFieldVec4_Native;
GetModuleFieldQuat_fn GetModuleFieldQuatPtr = &GetModuleFieldQuat_Native;
SetModuleFieldBool_fn SetModuleFieldBoolPtr = &SetModuleFieldBool_Native;
SetModuleFieldInt_fn SetModuleFieldIntPtr = &SetModuleFieldInt_Native;
SetModuleFieldInt64_fn SetModuleFieldInt64Ptr = &SetModuleFieldInt64_Native;
SetModuleFieldFloat_fn SetModuleFieldFloatPtr = &SetModuleFieldFloat_Native;
SetModuleFieldDouble_fn SetModuleFieldDoublePtr = &SetModuleFieldDouble_Native;
SetModuleFieldString_fn SetModuleFieldStringPtr = &SetModuleFieldString_Native;
SetModuleFieldVec2_fn SetModuleFieldVec2Ptr = &SetModuleFieldVec2_Native;
SetModuleFieldVec3_fn SetModuleFieldVec3Ptr = &SetModuleFieldVec3_Native;
SetModuleFieldVec4_fn SetModuleFieldVec4Ptr = &SetModuleFieldVec4_Native;
SetModuleFieldQuat_fn SetModuleFieldQuatPtr = &SetModuleFieldQuat_Native;

// SyncContext control pointers
FlushSyncContext_fn FlushSyncContextPtr = nullptr;
ClearSyncContext_fn ClearSyncContextPtr = nullptr;

// Tween pointers (native side directly calls these exports; also forwarded to managed)
Tween_Position_fn Tween_PositionPtr = &Tween_Position;
Tween_RotationEuler_fn Tween_RotationEulerPtr = &Tween_RotationEuler;
Tween_Scale_fn Tween_ScalePtr = &Tween_Scale;
Tween_LightIntensity_fn Tween_LightIntensityPtr = &Tween_LightIntensity;
Tween_ManagedFloat_fn Tween_ManagedFloatPtr = &Tween_ManagedFloat;
Tween_ManagedVec3_fn Tween_ManagedVec3Ptr = &Tween_ManagedVec3;
Tween_SetFinishedCallback_fn Tween_SetFinishedCallbackPtr = (Tween_SetFinishedCallback_fn)&Tween_SetFinishedCallback;

// Script registration forward declarations
extern "C" __declspec(dllexport) void NativeRegisterScriptType(const char* className, int priority);
extern "C" __declspec(dllexport) void NativeRegisterScriptFlags(const char* className, uint32_t flags);

// Struct passed to managed side for script registration callbacks
struct ScriptRegistrationInterop {
    void (*RegisterScriptType)(const char*, int priority);
    void (*RegisterScriptFlags)(const char*, uint32_t flags);
    void (*RegisterScriptProperty)(const char*, const char*, int, void*, const char*);
    void (*RegisterScriptPropertyExtended)(const char*, const char*, int, void*, const char*, const char*, const char*, int, const char*, const char*, bool, bool);
    void (*ClearScriptProperties)(const char*);
};
static ScriptRegistrationInterop g_ScriptRegInterop = { 
    &NativeRegisterScriptType,
    &NativeRegisterScriptFlags,
    &RegisterScriptPropertyNative,
    &RegisterScriptPropertyExtended,
    &ClearScriptPropertiesNative
};

extern "C" void* Get_Prefab_InstantiateByGuid_Ptr();
extern "C" void* Get_Prefab_InstantiateByGuidBlocking_Ptr();
extern "C" void* Get_Prefab_InstantiateByGuidWithRoot_Ptr();
extern "C" void* Get_Prefab_GetAsyncStatus_Ptr();
extern "C" void* Get_Prefab_GetAssetNameByGuid_Ptr();
extern "C" void* Get_Prefab_PreloadByGuid_Ptr();
extern "C" void* Get_Mesh_InstantiateByGuid_Ptr();
extern "C" void* Get_Mesh_InstantiateByGuidWithRoot_Ptr();
extern "C" void* Get_Mesh_SetByGuid_Ptr();
extern "C" void* Get_Mesh_HasComponent_Ptr();
extern "C" void* Get_Mesh_GetReference_Ptr();
extern "C" void* Get_Mesh_GetVertexCount_Ptr();
extern "C" void* Get_Mesh_GetIndexCount_Ptr();
extern "C" void* Get_Mesh_GetSubmeshCount_Ptr();
extern "C" void* Get_Mesh_GetMaterialSlotCount_Ptr();
extern "C" void* Get_Mesh_GetMaterialSlotName_Ptr();
extern "C" void* Get_Mesh_GetBoundsMin_Ptr();
extern "C" void* Get_Mesh_GetBoundsMax_Ptr();
extern "C" void* Get_Mesh_GetBoundsPadding_Ptr();
extern "C" void* Get_Mesh_SetBoundsPadding_Ptr();
extern "C" void* Get_Mesh_GetRenderOnTop_Ptr();
extern "C" void* Get_Mesh_SetRenderOnTop_Ptr();
extern "C" void* Get_Mesh_GetShowBackfaces_Ptr();
extern "C" void* Get_Mesh_SetShowBackfaces_Ptr();
extern "C" void* Get_Mesh_GetSkipFrustumCulling_Ptr();
extern "C" void* Get_Mesh_SetSkipFrustumCulling_Ptr();
extern "C" void* Get_Mesh_GetRenderOrder_Ptr();
extern "C" void* Get_Mesh_SetRenderOrder_Ptr();
extern "C" void* Get_Mesh_GetUniqueMaterial_Ptr();
extern "C" void* Get_Mesh_SetUniqueMaterial_Ptr();
extern "C" void* Get_Mesh_HasSkinning_Ptr();
extern "C" void* Get_Mesh_GetName_Ptr();
extern "C" void* Get_Mesh_GetAssetNameByGuid_Ptr();
extern "C" void* Get_Audio_GetAssetNameByGuid_Ptr();
extern "C" void* Get_Audio_GetGuidFromPath_Ptr();
extern "C" void* Get_AudioSource_GetClipReference_Ptr();
extern "C" void* Get_AudioSource_SetClipReference_Ptr();
extern "C" void* Get_AudioSource_GetAudioPath_Ptr();
extern "C" void* Get_AudioSource_SetAudioPath_Ptr();
extern "C" void* Get_AudioSource_GetVolume_Ptr();
extern "C" void* Get_AudioSource_SetVolume_Ptr();
extern "C" void* Get_AudioSource_GetPitch_Ptr();
extern "C" void* Get_AudioSource_SetPitch_Ptr();
extern "C" void* Get_AudioSource_GetLoop_Ptr();
extern "C" void* Get_AudioSource_SetLoop_Ptr();
extern "C" void* Get_AudioSource_GetPlayOnAwake_Ptr();
extern "C" void* Get_AudioSource_SetPlayOnAwake_Ptr();
extern "C" void* Get_AudioSource_GetMute_Ptr();
extern "C" void* Get_AudioSource_SetMute_Ptr();
extern "C" void* Get_AudioSource_GetSpatial_Ptr();
extern "C" void* Get_AudioSource_SetSpatial_Ptr();
extern "C" void* Get_AudioSource_GetMinDistance_Ptr();
extern "C" void* Get_AudioSource_SetMinDistance_Ptr();
extern "C" void* Get_AudioSource_GetMaxDistance_Ptr();
extern "C" void* Get_AudioSource_SetMaxDistance_Ptr();
extern "C" void* Get_AudioSource_GetDopplerFactor_Ptr();
extern "C" void* Get_AudioSource_SetDopplerFactor_Ptr();
extern "C" void* Get_AudioSource_GetRolloff_Ptr();
extern "C" void* Get_AudioSource_SetRolloff_Ptr();
extern "C" void* Get_AudioSource_GetIsPlaying_Ptr();
extern "C" void* Get_AudioSource_GetIsPaused_Ptr();
extern "C" void* Get_AudioSource_Play_Ptr();
extern "C" void* Get_AudioSource_Stop_Ptr();
extern "C" void* Get_AudioSource_Pause_Ptr();
extern "C" void* Get_AudioSource_Resume_Ptr();

// Material interop function pointer getters
extern "C" void* Get_Material_SetVector4_Ptr();
extern "C" void* Get_Material_GetVector4_Ptr();
extern "C" void* Get_Material_HasProperty_Ptr();
extern "C" void* Get_Material_RemoveProperty_Ptr();
extern "C" void* Get_Material_ClearAll_Ptr();
extern "C" void* Get_Material_SetTexturePath_Ptr();
extern "C" void* Get_Material_SetVector4Slot_Ptr();
extern "C" void* Get_Material_GetVector4Slot_Ptr();
extern "C" void* Get_Material_HasPropertySlot_Ptr();
extern "C" void* Get_Material_RemovePropertySlot_Ptr();
extern "C" void* Get_Material_ClearSlot_Ptr();
extern "C" void* Get_Material_GetSlotCount_Ptr();
extern "C" void* Get_Material_SetTexturePathSlot_Ptr();
extern "C" void* Get_Material_GetMaterialTypeSlot_Ptr();
extern "C" void* Get_Material_GetMaterialNameSlot_Ptr();
extern "C" void* Get_Material_GetMaterialAssetPathSlot_Ptr();
extern "C" void* Get_Material_SetMaterialAssetPathSlot_Ptr();
extern "C" void* Get_Material_SetMaterialVector4Slot_Ptr();
extern "C" void* Get_Material_GetMaterialVector4Slot_Ptr();
extern "C" void* Get_Material_HasMaterialPropertySlot_Ptr();
extern "C" void* Get_Material_SetMaterialTexturePathSlot_Ptr();
extern "C" void* Get_Material_GetMaterialTexturePathSlot_Ptr();
extern "C" void* Get_Material_GetPbrScalarSlot_Ptr();
extern "C" void* Get_Material_SetPbrScalarSlot_Ptr();
extern "C" void* Get_Material_GetPbrEmissionColorSlot_Ptr();
extern "C" void* Get_Material_SetPbrEmissionColorSlot_Ptr();
extern "C" void* Get_Material_GetPbrUVTransformSlot_Ptr();
extern "C" void* Get_Material_SetPbrUVTransformSlot_Ptr();
extern "C" void* Get_Material_GetPbrReceiveShadowsOverrideSlot_Ptr();
extern "C" void* Get_Material_SetPbrReceiveShadowsOverrideSlot_Ptr();
extern "C" void* Get_Material_GetPbrReceiveShadowsSlot_Ptr();
extern "C" void* Get_Material_SetPbrReceiveShadowsSlot_Ptr();

// Particle interop function pointer getters (52 functions)
extern "C" void* Get_Particle_GetEnabled_Ptr();
extern "C" void* Get_Particle_SetEnabled_Ptr();
extern "C" void* Get_Particle_Play_Ptr();
extern "C" void* Get_Particle_Stop_Ptr();
extern "C" void* Get_Particle_Restart_Ptr();
extern "C" void* Get_Particle_IsPlaying_Ptr();
extern "C" void* Get_Particle_GetSimulationSpace_Ptr();
extern "C" void* Get_Particle_SetSimulationSpace_Ptr();
extern "C" void* Get_Particle_GetShape_Ptr();
extern "C" void* Get_Particle_SetShape_Ptr();
extern "C" void* Get_Particle_GetShapeRadius_Ptr();
extern "C" void* Get_Particle_SetShapeRadius_Ptr();
extern "C" void* Get_Particle_GetShapeAngle_Ptr();
extern "C" void* Get_Particle_SetShapeAngle_Ptr();
extern "C" void* Get_Particle_GetStartSpeed_Ptr();
extern "C" void* Get_Particle_SetStartSpeed_Ptr();
extern "C" void* Get_Particle_GetStartSize_Ptr();
extern "C" void* Get_Particle_SetStartSize_Ptr();
extern "C" void* Get_Particle_GetStartColor_Ptr();
extern "C" void* Get_Particle_SetStartColor_Ptr();
extern "C" void* Get_Particle_GetEmissionRate_Ptr();
extern "C" void* Get_Particle_SetEmissionRate_Ptr();
extern "C" void* Get_Particle_GetLooping_Ptr();
extern "C" void* Get_Particle_SetLooping_Ptr();
extern "C" void* Get_Particle_GetDuration_Ptr();
extern "C" void* Get_Particle_SetDuration_Ptr();
extern "C" void* Get_Particle_GetLifetime_Ptr();
extern "C" void* Get_Particle_SetLifetime_Ptr();
extern "C" void* Get_Particle_GetGravityModifier_Ptr();
extern "C" void* Get_Particle_SetGravityModifier_Ptr();
extern "C" void* Get_Particle_GetMaxParticles_Ptr();
extern "C" void* Get_Particle_SetMaxParticles_Ptr();
extern "C" void* Get_Particle_GetSizeOverLifetimeEnabled_Ptr();
extern "C" void* Get_Particle_SetSizeOverLifetimeEnabled_Ptr();
extern "C" void* Get_Particle_GetColorOverLifetimeEnabled_Ptr();
extern "C" void* Get_Particle_SetColorOverLifetimeEnabled_Ptr();
extern "C" void* Get_Particle_GetVelocityOverLifetimeEnabled_Ptr();
extern "C" void* Get_Particle_SetVelocityOverLifetimeEnabled_Ptr();
extern "C" void* Get_Particle_GetRotationOverLifetimeEnabled_Ptr();
extern "C" void* Get_Particle_SetRotationOverLifetimeEnabled_Ptr();
extern "C" void* Get_Particle_GetAlignWithTrajectory_Ptr();
extern "C" void* Get_Particle_SetAlignWithTrajectory_Ptr();
extern "C" void* Get_Particle_GetBurstEnabled_Ptr();
extern "C" void* Get_Particle_SetBurstEnabled_Ptr();
extern "C" void* Get_Particle_GetSizeOverLifetime_Ptr();
extern "C" void* Get_Particle_SetSizeOverLifetime_Ptr();
extern "C" void* Get_Particle_GetColorGradientKeyCount_Ptr();
extern "C" void* Get_Particle_GetColorGradientKey_Ptr();
extern "C" void* Get_Particle_SetColorGradientKey_Ptr();
extern "C" void* Get_Particle_ClearColorGradient_Ptr();
extern "C" void* Get_Particle_GetBurstCount_Ptr();
extern "C" void* Get_Particle_SetBurstCount_Ptr();

// Dialogue interop function pointer getters
extern "C" void* Get_Dialogue_LoadLibrary_Ptr();
extern "C" void* Get_Dialogue_UnloadLibrary_Ptr();
extern "C" void* Get_Dialogue_GetLibraryName_Ptr();
extern "C" void* Get_Dialogue_GetLibraryCharacterId_Ptr();
extern "C" void* Get_Dialogue_StartFromLibrary_Ptr();
extern "C" void* Get_Dialogue_StartFromText_Ptr();
extern "C" void* Get_Dialogue_Advance_Ptr();
extern "C" void* Get_Dialogue_SelectChoice_Ptr();
extern "C" void* Get_Dialogue_ForceEnd_Ptr();
extern "C" void* Get_Dialogue_Pause_Ptr();
extern "C" void* Get_Dialogue_Resume_Ptr();
extern "C" void* Get_Dialogue_IsActive_Ptr();
extern "C" void* Get_Dialogue_IsTyping_Ptr();
extern "C" void* Get_Dialogue_IsWaitingForChoice_Ptr();
extern "C" void* Get_Dialogue_GetState_Ptr();
extern "C" void* Get_Dialogue_GetDisplayedText_Ptr();
extern "C" void* Get_Dialogue_GetCurrentSpeaker_Ptr();
extern "C" void* Get_Dialogue_GetSpeakerEntityId_Ptr();
extern "C" void* Get_Dialogue_GetChoiceCount_Ptr();
extern "C" void* Get_Dialogue_GetChoiceText_Ptr();
extern "C" void* Get_Dialogue_GetConversationTitle_Ptr();
extern "C" void* Get_Dialogue_SetState_Ptr();
extern "C" void* Get_Dialogue_GetStateValue_Ptr();
extern "C" void* Get_Dialogue_ClearState_Ptr();
extern "C" void* Get_Dialogue_SetTextSpeed_Ptr();
extern "C" void* Get_Dialogue_SetMinDisplayTime_Ptr();
extern "C" void* Get_Dialogue_SetEndCooldown_Ptr();
extern "C" void* Get_Dialogue_SetOnStarted_Ptr();
extern "C" void* Get_Dialogue_SetOnEnded_Ptr();
extern "C" void* Get_Dialogue_SetOnLine_Ptr();
extern "C" void* Get_Dialogue_SetOnEmote_Ptr();
extern "C" void* Get_Dialogue_SetOnStateChange_Ptr();
extern "C" void* Get_Dialogue_SetOnEmotion_Ptr();
extern "C" void* Get_Dialogue_SetOnTypingComplete_Ptr();
extern "C" void* Get_Dialogue_SetOnCharTyped_Ptr();
extern "C" void* Get_Dialogue_SetOnGiveItem_Ptr();
extern "C" void* Get_Dialogue_SetOnTakeItem_Ptr();
extern "C" void* Get_Dialogue_SetOnStartQuest_Ptr();
extern "C" void* Get_Dialogue_SetOnCompleteStep_Ptr();
extern "C" void* Get_Dialogue_SetOnPlayAnim_Ptr();
extern "C" void* Get_Dialogue_SetOnPlaySound_Ptr();
extern "C" void* Get_Dialogue_SetOnWait_Ptr();
extern "C" void* Get_Dialogue_SetOnCamera_Ptr();
extern "C" void* Get_Dialogue_SetOnSetEvent_Ptr();

// Quest interop function pointer getters
extern "C" void* Get_Quest_LoadDatabase_Ptr();
extern "C" void* Get_Quest_UnloadDatabase_Ptr();
extern "C" void* Get_Quest_StartQuest_Ptr();
extern "C" void* Get_Quest_AdvanceStage_Ptr();
extern "C" void* Get_Quest_CompleteStage_Ptr();
extern "C" void* Get_Quest_FailQuest_Ptr();
extern "C" void* Get_Quest_AbandonQuest_Ptr();
extern "C" void* Get_Quest_UpdateObjectiveProgress_Ptr();
extern "C" void* Get_Quest_SetObjectiveProgress_Ptr();
extern "C" void* Get_Quest_GetQuestState_Ptr();
extern "C" void* Get_Quest_GetStageState_Ptr();
extern "C" void* Get_Quest_GetActiveStageId_Ptr();
extern "C" void* Get_Quest_GetObjectiveProgress_Ptr();
extern "C" void* Get_Quest_GetObjectiveTarget_Ptr();
extern "C" void* Get_Quest_IsObjectiveComplete_Ptr();
extern "C" void* Get_Quest_GetActiveQuestCount_Ptr();
extern "C" void* Get_Quest_GetActiveQuestIds_Ptr();
extern "C" void* Get_Quest_GetCompletedQuestCount_Ptr();
extern "C" void* Get_Quest_GetCompletedQuestIds_Ptr();
extern "C" void* Get_Quest_GetBranchHistoryCount_Ptr();
extern "C" void* Get_Quest_GetBranchHistory_Ptr();
extern "C" void* Get_Quest_CanStartQuest_Ptr();
extern "C" void* Get_Quest_AreStageConditionsMet_Ptr();
extern "C" void* Get_Quest_IsDialogueTriggerActive_Ptr();
extern "C" void* Get_Quest_SerializeState_Ptr();
extern "C" void* Get_Quest_DeserializeState_Ptr();
extern "C" void* Get_Quest_ResetAllState_Ptr();
extern "C" void* Get_Quest_SetOnQuestStarted_Ptr();
extern "C" void* Get_Quest_SetOnStageStarted_Ptr();
extern "C" void* Get_Quest_SetOnStageCompleted_Ptr();
extern "C" void* Get_Quest_SetOnQuestCompleted_Ptr();
extern "C" void* Get_Quest_SetOnQuestFailed_Ptr();
extern "C" void* Get_Quest_SetOnObjectiveProgress_Ptr();

// ----------------------------------------
// Managed script registration
// ----------------------------------------
// Vector defined in UILayer.cpp so the UI and interop use the same list
extern std::vector<std::string> g_RegisteredScriptNames;
 

// Global function pointer set from managed side
// Called by native to let C# perform registration
extern "C" __declspec(dllexport) void NativeRegisterScriptType(const char* className, int priority)
{
    if(std::find(g_RegisteredScriptNames.begin(), g_RegisteredScriptNames.end(), className) == g_RegisteredScriptNames.end())
        g_RegisteredScriptNames.emplace_back(className);
    std::cout << "[Interop] Registered script type: " << className << " priority=" << priority << std::endl;

    ScriptSystem::Instance().RegisterManaged(className);
    ScriptSystem::Instance().SetScriptPriority(className, priority);
}

extern "C" __declspec(dllexport) void NativeRegisterScriptFlags(const char* className, uint32_t flags)
{
    if (!className) return;
    ScriptSystem::Instance().SetScriptFlags(className, flags);
}



// ----------------------------------------
// Load HostFXR + CoreCLR
// ----------------------------------------
// Self-contained deployment: hostfxr.dll is bundled with the application
// Framework-dependent: uses system .NET installation via nethost.dll
// ----------------------------------------

// Track if we're using a private bundled .NET installation under <exe>/dotnet.
static bool g_UsingBundledDotnetRoot = false;

static std::filesystem::path GetBundledDotnetRoot(const std::filesystem::path& exeDir)
{
    return exeDir / L"dotnet";
}

static bool HasBundledDotnetRuntime(const std::filesystem::path& dotnetRoot)
{
    return std::filesystem::exists(dotnetRoot / L"host" / L"fxr") &&
           std::filesystem::exists(dotnetRoot / L"shared" / L"Microsoft.NETCore.App");
}

static bool HasLegacySelfContainedHost(const std::filesystem::path& exeDir)
{
    return std::filesystem::exists(exeDir / L"hostfxr.dll") ||
           std::filesystem::exists(exeDir / L"coreclr.dll") ||
           std::filesystem::exists(exeDir / L"hostpolicy.dll");
}

bool LoadHostFxr()
{
    g_UsingBundledDotnetRoot = false;

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    const std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
    const std::filesystem::path bundledDotnetRoot = GetBundledDotnetRoot(exeDir);

    HMODULE hostfxr = nullptr;
    HMODULE nethost = LoadLibraryW(L"nethost.dll");
    if (!nethost) {
        std::wcerr << L"[Interop] Failed to load nethost.dll\n";
        std::wcerr << L"[Interop] Either bundle a private .NET runtime under dotnet\\ or install .NET 10.\n";
        return false;
    }

    using get_hostfxr_path_fn = int(__cdecl*)(char_t*, size_t*, const get_hostfxr_parameters*);
    auto get_hostfxr_path = (get_hostfxr_path_fn)GetProcAddress(nethost, "get_hostfxr_path");
    if (!get_hostfxr_path) {
        std::cerr << "[Interop] Failed to resolve get_hostfxr_path\n";
        return false;
    }

    if (HasLegacySelfContainedHost(exeDir)) {
        std::wcerr << L"[Interop] Ignoring app-local hostfxr/coreclr files.\n";
        std::wcerr << L"[Interop] .NET component hosting expects nethost + runtimeconfig, not self-contained app layout.\n";
    }

    wchar_t buffer[MAX_PATH];
    size_t size = sizeof(buffer) / sizeof(wchar_t);
    if (HasBundledDotnetRuntime(bundledDotnetRoot)) {
        get_hostfxr_parameters params{};
        params.size = sizeof(get_hostfxr_parameters);
        params.dotnet_root = bundledDotnetRoot.c_str();

        int rc = get_hostfxr_path(buffer, &size, &params);
        if (rc == 0) {
            hostfxr = LoadLibraryW(buffer);
            if (hostfxr) {
                g_UsingBundledDotnetRoot = true;
                std::wcout << L"[Interop] Using bundled .NET runtime root: " << bundledDotnetRoot << std::endl;
            } else {
                std::wcerr << L"[Interop] Failed to load hostfxr.dll from bundled runtime root: " << buffer << std::endl;
            }
        } else {
            std::wcerr << L"[Interop] Failed to resolve hostfxr from bundled runtime root: " << bundledDotnetRoot << std::endl;
        }
    }

    if (!hostfxr) {
        size = sizeof(buffer) / sizeof(wchar_t);
        if (get_hostfxr_path(buffer, &size, nullptr) != 0) {
            std::wcerr << L"[Interop] get_hostfxr_path failed - .NET 10 runtime not installed?\n";
            std::wcerr << L"[Interop] Please install the .NET 10 Runtime or bundle a private runtime under dotnet\\.\n";
            return false;
        }

        hostfxr = LoadLibraryW(buffer);
        if (!hostfxr) {
            std::wcerr << L"[Interop] Failed to load hostfxr.dll from: " << buffer << std::endl;
            return false;
        }
        std::wcout << L"[Interop] Using system .NET runtime\n";
    }

    init_fptr = (hostfxr_initialize_for_runtime_config_fn)GetProcAddress(hostfxr, "hostfxr_initialize_for_runtime_config");
    get_delegate_fptr = (hostfxr_get_runtime_delegate_fn)GetProcAddress(hostfxr, "hostfxr_get_runtime_delegate");
    close_fptr = (hostfxr_close_fn)GetProcAddress(hostfxr, "hostfxr_close");

    return (init_fptr && get_delegate_fptr && close_fptr);
}

// ----------------------------------------
// Load .NET runtime and resolve entry point
// ----------------------------------------
bool LoadDotnetRuntime(const std::wstring& assemblyPath, const std::wstring& typeName, const std::wstring& methodName)
   {
   wprintf(L"[Interop] Starting .NET runtime load...\n");

   if (!LoadHostFxr())
      {
      wprintf(L"[Interop] LoadHostFxr() failed.\n");
      return false;
      }

   // Get the exe directory for finding runtimeconfig.json
   wchar_t exePath[MAX_PATH];
   GetModuleFileNameW(NULL, exePath, MAX_PATH);
   std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
   
   // Build absolute path to runtimeconfig.json (must be next to exe)
   std::filesystem::path runtimeConfigPath = exeDir / L"ClaymoreEngine.runtimeconfig.json";
   std::wstring runtimeConfigStr = runtimeConfigPath.wstring();
   
   if (!std::filesystem::exists(runtimeConfigPath)) {
      std::wcerr << L"[Interop] Runtime config not found: " << runtimeConfigStr << std::endl;
      return false;
   }
   
   std::ifstream configFile(runtimeConfigPath);
   if (configFile.is_open()) {
      std::string content((std::istreambuf_iterator<char>(configFile)),
                           std::istreambuf_iterator<char>());
      configFile.close();

      if (content.find("\"includedFrameworks\"") != std::string::npos) {
         std::wcerr << L"[Interop] Incompatible runtimeconfig detected: " << runtimeConfigStr << std::endl;
         std::wcerr << L"[Interop] ClaymoreEngine must use a framework-dependent runtimeconfig when hosted as a component.\n";
         std::wcerr << L"[Interop] Re-stage the managed output from bin\\<Config>\\net10.0 or use a bundled dotnet\\ runtime root.\n";
         return false;
      }
   }
   
   std::wcout << L"[Interop] Using runtime config: " << runtimeConfigStr << std::endl;

   hostfxr_handle handle = nullptr;
   int rc = init_fptr(runtimeConfigStr.c_str(), nullptr, &handle);
   if (rc != 0 || handle == nullptr)
      {
      wprintf(L"[Interop] init_fptr failed. HRESULT: 0x%08X\n", rc);
      
      if (rc == 0x80008093) {
         std::wcerr << L"[Interop] Error 0x80008093: component hosting rejected the staged runtime layout.\n";
         std::wcerr << L"[Interop] Ensure hostfxr/coreclr are not beside the exe and that ClaymoreEngine.runtimeconfig.json is framework-dependent.\n";
         std::wcerr << L"[Interop] If bundling the runtime, place it under a dotnet\\ folder instead.\n";
      }
      return false;
      }

   rc = get_delegate_fptr(handle, hdt_load_assembly_and_get_function_pointer, (void**)&load_assembly_and_get_function_pointer);
   close_fptr(handle);
   if (rc != 0 || !load_assembly_and_get_function_pointer)
      {
      wprintf(L"[Interop] get_delegate_fptr failed. HRESULT: 0x%08X\n", rc);
      return false;
      }

   std::filesystem::path fullPath = std::filesystem::absolute(assemblyPath);

   // Force load a known method to warm up the runtime
   void* dummy = nullptr;
   load_assembly_and_get_function_pointer(
      fullPath.c_str(),
      L"System.Object, System.Private.CoreLib",
      L"ToString",
      L"",
      nullptr,
      &dummy
   );

   // Load the managed engine entry point
   using component_entry_point_fn = int (CORECLR_DELEGATE_CALLTYPE*)(void*, int);
   component_entry_point_fn entryPoint = nullptr;

   rc = load_assembly_and_get_function_pointer(
      fullPath.c_str(),
      typeName.c_str(),
      methodName.c_str(),
      L"ClaymoreEngine.EntryPointDelegate, ClaymoreEngine",
      nullptr,
      (void**)&entryPoint
   );

   if (rc != 0 || !entryPoint)
      {
      wprintf(L"[Interop] Failed to get managed entry point. HRESULT: 0x%08X\n", rc);
      return false;
      }

   // Ensure GameScripts.dll is present and compiled BEFORE we invoke the managed entry point
   // (exeDir already computed above for runtimeconfig.json)
   
   // For standalone builds (pak mounted), look for GameScripts.dll next to exe
   // For editor mode, look in project .library folder
   bool isStandalone = FileSystem::Instance().IsPakMounted();
   std::filesystem::path gameScriptsDllPath;
   
   if (isStandalone) {
       // Standalone: GameScripts.dll is next to the exe
       gameScriptsDllPath = exeDir / "GameScripts.dll";
       if (std::filesystem::exists(gameScriptsDllPath)) {
           std::cout << "[Interop] Standalone mode: Using GameScripts.dll from exe directory.\n";
           AssetPipeline::Instance().SetScriptsCompiled(true);
       } else {
           std::cerr << "[Interop] Standalone mode: GameScripts.dll not found next to exe!\n";
       }
   } else {
       // Editor mode: look in project .library folder, compile if needed
       gameScriptsDllPath = std::filesystem::path(Project::GetProjectDirectory()) / ".library" / "GameScripts.dll";
       
       if (!std::filesystem::exists(gameScriptsDllPath) || !AssetPipeline::Instance().AreScriptsCompiled()) {
           std::wcerr << L"[Interop] GameScripts.dll missing or out-of-date. Attempting compilation...\n";
           AssetPipeline::Instance().CheckAndCompileScriptsAtStartup();
       }
       
       // If there are no scripts at all, treat as successful so engine can continue without managed game scripts.
       if (!AssetPipeline::Instance().AreScriptsCompiled() && AssetPipeline::Instance().HasAnyScripts()) {
           Logger::LogError("[Interop] C# scripts failed to compile. Continuing without user scripts � Play Mode disabled.");
       }
   }
   
   // Provide the managed side with the absolute path to the scripts DLL via environment variable
   {
       std::wstring dllW = std::filesystem::absolute(gameScriptsDllPath).wstring();
       SetEnvironmentVariableW(L"CLAYMORE_SCRIPTS_DLL", dllW.c_str());
   }

   std::cout << "[C++] Calling ManagedStart on thread: " << GetCurrentThreadId() << "\n";
   wprintf(L"[Interop] Managed entry point resolved. Invoking...\n");

   auto CallEntryPointSafe = [](component_entry_point_fn fn)->int {
#if defined(_MSC_VER) && !defined(CLAYMORE_FAST_SCRIPT_CALLS)
       __try {
           return fn(nullptr, 0);
       } __except(EXCEPTION_EXECUTE_HANDLER) {
           return -1;
       }
#else
       return fn(nullptr, 0);
#endif
   };

   int entryRc = CallEntryPointSafe(entryPoint);
   if(entryRc != 0) {
       Logger::LogError("[Interop] Managed entry point threw an exception � GameScripts.dll may be corrupted.");
       AssetPipeline::Instance().SetScriptsCompiled(false);
       wprintf(L"[Interop] Entry point failed � attempting to rebuild GameScripts.dll...\n");

       // Attempt to rebuild scripts and reload them if successful
       bool rebuilt = AssetPipeline::Instance().ForceRebuildScripts();
       if (rebuilt) {
           // Ask managed to reload the newly built assembly and re-register script types
           ReloadScripts();
           if (g_RegisterAllScripts) {
               g_RegisterAllScripts(reinterpret_cast<void*>(&g_ScriptRegInterop));
           }
           // Rebind delegates after reload (mirror startup path)
           bool runtimeInteropOk = cm::runtime::SetupAllInterop(load_assembly_and_get_function_pointer, fullPath.wstring());
           cm::multiplayer::InitializeManagedBridge(load_assembly_and_get_function_pointer, fullPath.wstring());
           if (runtimeInteropOk) {
               SetupReflectionInterop(fullPath);
           } else {
               SetupEntityInterop(fullPath);
               SetupInputInterop(fullPath);
               SetupReflectionInterop(fullPath);
               SetupPhysicsInterop(fullPath.wstring());
           }
           wprintf(L"[Interop] Rebuild completed and scripts reloaded.\n");
       } else {
           wprintf(L"[Interop] Rebuild failed � continuing without user scripts.\n");
       }
   } else {
       wprintf(L"[Interop] Entry point completed.\n");
   }

   // Load C# interop exports
   rc = load_assembly_and_get_function_pointer(
      fullPath.c_str(),
      L"ClaymoreEngine.InteropExports, ClaymoreEngine",
      L"Script_Create",
      L"ClaymoreEngine.Script_CreateDelegate, ClaymoreEngine",
      nullptr,
      (void**)&g_Script_Create
   );

   rc |= load_assembly_and_get_function_pointer(
      fullPath.c_str(),
      L"ClaymoreEngine.InteropExports, ClaymoreEngine",
      L"Script_Bind",
      L"ClaymoreEngine.Script_BindDelegate, ClaymoreEngine",
      nullptr,
      (void**)&g_Script_Bind
   );

   rc |= load_assembly_and_get_function_pointer(
      fullPath.c_str(),
      L"ClaymoreEngine.InteropExports, ClaymoreEngine",
      L"Script_OnCreate",
      L"ClaymoreEngine.Script_OnCreateDelegate, ClaymoreEngine",
      nullptr,
      (void**)&g_Script_OnCreate
   );

   {
      int localRc = load_assembly_and_get_function_pointer(
         fullPath.c_str(),
         L"ClaymoreEngine.InteropExports, ClaymoreEngine",
         L"Script_OnUpdate",
         L"ClaymoreEngine.Script_OnUpdateDelegate, ClaymoreEngine",
         nullptr,
         (void**)&g_Script_OnUpdate);
      if(localRc != 0 || (uintptr_t)g_Script_OnUpdate < 0x1000)
      {
         std::cerr << "[Interop] Failed to resolve Script_OnUpdate (HRESULT=" << std::hex << localRc << ")\n";
         g_Script_OnUpdate = nullptr;
      }
      rc |= localRc;
   }

   // Resolve Script_OnDestroy for cleanup when scripts are destroyed
   {
      void* fn = nullptr;
      int localRc = load_assembly_and_get_function_pointer(
         fullPath.c_str(),
         L"ClaymoreEngine.InteropExports, ClaymoreEngine",
         L"Script_OnDestroy",
         L"ClaymoreEngine.Script_OnDestroyDelegate, ClaymoreEngine",
         nullptr,
         &fn
      );
      if (localRc == 0 && fn)
      {
         g_Script_OnDestroy = reinterpret_cast<Script_OnDestroy_fn>(fn);
      }
      else
      {
         std::cerr << "[Interop] Failed to resolve Script_OnDestroy (HRESULT=" << std::hex << localRc << ")\n";
      }
   }

   // Resolve Script_Invoke for arbitrary method calls
   {
      void* fn = nullptr;
      int localRc = load_assembly_and_get_function_pointer(
         fullPath.c_str(),
         L"ClaymoreEngine.InteropExports, ClaymoreEngine",
         L"Script_Invoke",
         L"ClaymoreEngine.Script_InvokeDelegate, ClaymoreEngine",
         nullptr,
         &fn
      );
      if (localRc == 0 && fn)
      {
         g_Script_Invoke = reinterpret_cast<Script_Invoke_fn>(fn);
      }
      else
      {
         std::cerr << "[Interop] Failed to resolve Script_Invoke (HRESULT=" << std::hex << localRc << ")\n";
      }
   }

   // Resolve ClearComponentCaches for play mode cache clearing
   {
      void* fn = nullptr;
      int localRc = load_assembly_and_get_function_pointer(
         fullPath.c_str(),
         L"ClaymoreEngine.InteropExports, ClaymoreEngine",
         L"ClearComponentCaches",
         L"ClaymoreEngine.ClearComponentCachesDelegate, ClaymoreEngine",
         nullptr,
         &fn
      );
      if (localRc == 0 && fn)
      {
         g_ClearComponentCaches = reinterpret_cast<ClearComponentCaches_fn>(fn);
      }
      else
      {
         std::cerr << "[Interop] Failed to resolve ClearComponentCaches (HRESULT=" << std::hex << localRc << ")\n";
      }
   } 

   // Load FlushSyncContext from managed side
   rc |= load_assembly_and_get_function_pointer(
      fullPath.c_str(),
      L"ClaymoreEngine.EngineSyncContext, ClaymoreEngine",
      L"Flush",
      L"ClaymoreEngine.VoidDelegate, ClaymoreEngine",
      nullptr,
      (void**)&FlushSyncContextPtr
   );

   // Load Button.UpdateAll for managed button updates
   {
      void* fn = nullptr;
      int localRc = load_assembly_and_get_function_pointer(
         fullPath.c_str(),
         L"ClaymoreEngine.Button, ClaymoreEngine",
         L"UpdateAll",
         L"ClaymoreEngine.VoidDelegate, ClaymoreEngine",
         nullptr,
         &fn
      );
      if (localRc == 0 && fn)
      {
         cm::script::g_UpdateButtons = reinterpret_cast<cm::script::UpdateButtons_fn>(fn);
      }
      else
      {
         cm::script::g_UpdateButtons = nullptr;
         std::cerr << "[Interop] Failed to resolve Button.UpdateAll (HRESULT=" << std::hex << localRc << ")\n";
      }
   }

   // Load ManagedFrameBridge.UpdateExport so engine-owned systems can tick
   // managed frame services without depending on any user script OnUpdate.
   {
      void* fn = nullptr;
      int localRc = load_assembly_and_get_function_pointer(
         fullPath.c_str(),
         L"ClaymoreEngine.ManagedFrameBridge, ClaymoreEngine",
         L"UpdateExport",
         L"ClaymoreEngine.ManagedFrameUpdateDelegate, ClaymoreEngine",
         nullptr,
         &fn
      );
      if (localRc == 0 && fn)
      {
         cm::script::g_ManagedFrameUpdate = reinterpret_cast<cm::script::ManagedFrameUpdate_fn>(fn);
      }
      else
      {
         cm::script::g_ManagedFrameUpdate = nullptr;
         std::cerr << "[Interop] Failed to resolve ManagedFrameBridge.UpdateExport (HRESULT="
                   << std::hex << localRc << ")\n";
      }
   }

   // Load ClearSyncContext from managed side (optional safety)
   {
      void* fn = nullptr;
      int localRc = load_assembly_and_get_function_pointer(
         fullPath.c_str(),
         L"ClaymoreEngine.EngineSyncContext, ClaymoreEngine",
         L"Clear",
         L"ClaymoreEngine.VoidDelegate, ClaymoreEngine",
         nullptr,
         &fn
      );
      if (localRc == 0 && fn)
         ClearSyncContextPtr = reinterpret_cast<ClearSyncContext_fn>(fn);
   }

   // Resolve Script_Destroy so native can free managed GCHandles when scripts are destroyed
   {
      void* fn = nullptr;
      int localRc = load_assembly_and_get_function_pointer(
         fullPath.c_str(),
         L"ClaymoreEngine.InteropExports, ClaymoreEngine",
         L"Script_Destroy",
         L"ClaymoreEngine.Script_DestroyDelegate, ClaymoreEngine",
         nullptr,
         &fn
      );
      if (localRc == 0 && fn)
      {
         g_Script_Destroy = reinterpret_cast<Script_Destroy_fn>(fn);
      }
      else
      {
         std::cerr << "[Interop] Failed to resolve Script_Destroy (HRESULT=" << std::hex << localRc << ")\n";
      }
   }

   {
   void* fn = nullptr;
   int rc = load_assembly_and_get_function_pointer(
      fullPath.c_str(),
      L"ClaymoreEngine.EngineSyncContext, ClaymoreEngine",
      L"InstallFromNative",
      L"ClaymoreEngine.VoidDelegate, ClaymoreEngine", // same void() shape
      nullptr,
      &fn
   );
   if (rc == 0 && fn)
      InstallSyncContextPtr = reinterpret_cast<InstallSyncContext_fn>(fn);
   else
      std::cerr << "[Interop] Failed to resolve EngineSyncContext.InstallFromNative\n";
   }

   {
   void* fn = nullptr;
   int rc = load_assembly_and_get_function_pointer(
      fullPath.c_str(),
      L"ClaymoreEngine.EngineSyncContext, ClaymoreEngine",
      L"EnsureInstalledHereFromNative",
      L"ClaymoreEngine.VoidDelegate, ClaymoreEngine",
      nullptr,
      &fn
   );
   if (rc == 0 && fn)
      EnsureInstalledPtr = reinterpret_cast<EnsureInstalled_fn>(fn);
   else
      std::cerr << "[Interop] Failed to resolve EngineSyncContext.EnsureInstalledHereFromNative\n";
   }

   if (!g_Script_Create || !g_Script_OnCreate || !g_Script_OnUpdate)
   {
      std::cerr << "[Interop] One or more script interop function pointers are null. "
                << "Create=" << g_Script_Create << ", OnCreate=" << g_Script_OnCreate
                << ", OnUpdate=" << g_Script_OnUpdate << "\n";
      return false;
   }

   if (!FlushSyncContextPtr) {
	   std::cerr << "[Interop] Failed to resolve FlushSyncContext function.\n";
	   return false;
   }

   // Set the unified ScriptInterop global pointers so ManagedScriptComponent works
   // in both editor and runtime contexts
   cm::script::g_CreateInstance = g_Script_Create;
   cm::script::g_OnBind = g_Script_Bind;
   cm::script::g_OnCreate = g_Script_OnCreate;
   cm::script::g_OnUpdate = g_Script_OnUpdate;
   cm::script::g_OnDestroy = g_Script_OnDestroy;
   cm::script::g_Invoke = g_Script_Invoke;
   cm::script::g_Destroy = g_Script_Destroy;
   cm::script::g_FlushSyncContext = reinterpret_cast<cm::script::FlushSyncContext_fn>(FlushSyncContextPtr);
   cm::script::g_SetManagedField = SetManagedFieldPtr;
   cm::script::g_EnsureInstalled = EnsureInstalledPtr;
   std::cout << "[Interop] Set unified ScriptInterop function pointers\n";

   // Resolve RegisterAllScripts on startup
   if (!g_RegisterAllScripts)
      {
      void* fn = nullptr;
      int rc = load_assembly_and_get_function_pointer(
         fullPath.c_str(),
         L"ClaymoreEngine.InteropExports, ClaymoreEngine",
         L"RegisterAllScripts",
         L"ClaymoreEngine.RegisterAllScriptsDelegate, ClaymoreEngine",
         nullptr,
         &fn
      );

      if (rc == 0 && fn)
         g_RegisterAllScripts = reinterpret_cast<RegisterAllScriptsFn>(fn);
      }

   if (g_RegisterAllScripts)
   {
       // Pass struct of native callbacks so managed side can invoke them
       g_RegisterAllScripts(reinterpret_cast<void*>(&g_ScriptRegInterop));
   }

   bool runtimeInteropOk = cm::runtime::SetupAllInterop(load_assembly_and_get_function_pointer, fullPath.wstring());
   cm::multiplayer::InitializeManagedBridge(load_assembly_and_get_function_pointer, fullPath.wstring());
   if (runtimeInteropOk) {
       SetupReflectionInterop(fullPath);
   } else {
       SetupEntityInterop(fullPath);
       SetupInputInterop(fullPath);
       SetupReflectionInterop(fullPath);
       SetupPhysicsInterop(fullPath.wstring());
       SetupResourceInterop(fullPath);

   // Area interop bootstrap
   {
   void* args[4];
   args[0] = (void*)Get_Area_SetOnBodyEntered_Ptr();
   args[1] = (void*)Get_Area_SetOnBodyExited_Ptr();
   args[2] = (void*)Get_Area_SetOnAreaEntered_Ptr();
   args[3] = (void*)Get_Area_SetOnAreaExited_Ptr();

   using AreaInteropInitFn = void(*)(void**, int);
   AreaInteropInitFn initAreaFn = nullptr;
   int rcArea = load_assembly_and_get_function_pointer(
      fullPath.c_str(),
      L"ClaymoreEngine.AreaInterop, ClaymoreEngine",
      L"InitializeInteropExport",
      L"ClaymoreEngine.AreaInteropInitDelegate, ClaymoreEngine",
      nullptr,
      (void**)&initAreaFn
   );
   if (rcArea == 0 && initAreaFn) {
      initAreaFn(args, 4);
      }
   }

   // Portal interop bootstrap
   {
   void* args[1];
   args[0] = (void*)Get_Portal_SetOnCrossed_Ptr();

   using PortalInteropInitFn = void(*)(void**, int);
   PortalInteropInitFn initPortalFn = nullptr;
   int rcPortal = load_assembly_and_get_function_pointer(
      fullPath.c_str(),
      L"ClaymoreEngine.PortalInterop, ClaymoreEngine",
      L"InitializeInteropExport",
      L"ClaymoreEngine.PortalInteropInitDelegate, ClaymoreEngine",
      nullptr,
      (void**)&initPortalFn
   );
   if (rcPortal == 0 && initPortalFn) {
      initPortalFn(args, 1);
      }
   }

   // World graph interop bootstrap
   {
   void* args[27];
   args[0] = (void*)Get_WorldGraph_LoadProject_Ptr();
   args[1] = (void*)Get_WorldGraph_IsLoaded_Ptr();
   args[2] = (void*)Get_WorldGraph_GetPortalCount_Ptr();
   args[3] = (void*)Get_WorldGraph_GetPortalScenePath_Ptr();
   args[4] = (void*)Get_WorldGraph_GetPortalGuid_Ptr();
   args[5] = (void*)Get_WorldGraph_GetPortalTargetScenePath_Ptr();
   args[6] = (void*)Get_WorldGraph_GetPortalTargetGuid_Ptr();
   args[7] = (void*)Get_WorldGraph_GetPortalPath_Ptr();
   args[8] = (void*)Get_WorldGraph_GetPortalTargetPath_Ptr();
   args[9] = (void*)Get_WorldGraph_GetPortalEntryPosition_Ptr();
   args[10] = (void*)Get_WorldGraph_GetPortalExitPosition_Ptr();
   args[11] = (void*)Get_WorldGraph_IsPortalResolved_Ptr();
   args[12] = (void*)Get_WorldGraph_GetPortalDistance_Ptr();
   args[13] = (void*)Get_WorldGraph_FindPortalIndex_Ptr();
  args[14] = (void*)Get_WorldGraph_GetPoiCount_Ptr();
  args[15] = (void*)Get_WorldGraph_GetPoiScenePath_Ptr();
  args[16] = (void*)Get_WorldGraph_GetPoiGuid_Ptr();
  args[17] = (void*)Get_WorldGraph_GetPoiPath_Ptr();
  args[18] = (void*)Get_WorldGraph_GetPoiScriptClass_Ptr();
  args[19] = (void*)Get_WorldGraph_GetPoiNodeName_Ptr();
  args[20] = (void*)Get_WorldGraph_GetPoiNodeType_Ptr();
  args[21] = (void*)Get_WorldGraph_GetPoiIsPortal_Ptr();
  args[22] = (void*)Get_WorldGraph_GetPoiPosition_Ptr();
  args[23] = (void*)Get_WorldGraph_FindPoiIndex_Ptr();
  args[24] = (void*)Get_WorldGraph_GetPoiToPortalDistance_Ptr();
  args[25] = (void*)Get_WorldGraph_GetPortalToPoiDistance_Ptr();
  args[26] = (void*)Get_WorldGraph_GetPortalToPortalDistance_Ptr();

   using WorldGraphInteropInitFn = void(*)(void**, int);
   WorldGraphInteropInitFn initWorldGraphFn = nullptr;
   int rcWorldGraph = load_assembly_and_get_function_pointer(
      fullPath.c_str(),
      L"ClaymoreEngine.WorldGraphInterop, ClaymoreEngine",
      L"InitializeInteropExport",
      L"ClaymoreEngine.WorldGraphInteropInitDelegate, ClaymoreEngine",
      nullptr,
      (void**)&initWorldGraphFn
   );
   if (rcWorldGraph == 0 && initWorldGraphFn) {
      initWorldGraphFn(args, 27);
      }
   }

   // Collision interop bootstrap
   {
   void* args[2];
   args[0] = (void*)Get_Collision_SetOnEnter_Ptr();
   args[1] = (void*)Get_Collision_SetOnExit_Ptr();

   using CollisionInteropInitFn = void(*)(void**, int);
   CollisionInteropInitFn initCollisionFn = nullptr;
   int rcCollision = load_assembly_and_get_function_pointer(
      fullPath.c_str(),
      L"ClaymoreEngine.CollisionInterop, ClaymoreEngine",
      L"InitializeInteropExport",
      L"ClaymoreEngine.CollisionInteropInitDelegate, ClaymoreEngine",
      nullptr,
      (void**)&initCollisionFn
   );
   if (rcCollision == 0 && initCollisionFn) {
      initCollisionFn(args, 2);
      }
   }

   // Ragdoll interop bootstrap
   {
      void* ragdollArgs[9];
      ragdollArgs[0] = (void*)Get_Ragdoll_Create_Ptr();
      ragdollArgs[1] = (void*)Get_Ragdoll_Destroy_Ptr();
      ragdollArgs[2] = (void*)Get_Ragdoll_Has_Ptr();
      ragdollArgs[3] = (void*)Get_Ragdoll_Activate_Ptr();
      ragdollArgs[4] = (void*)Get_Ragdoll_Deactivate_Ptr();
      ragdollArgs[5] = (void*)Get_Ragdoll_ApplyImpulse_Ptr();
      ragdollArgs[6] = (void*)Get_Ragdoll_ApplyImpulseToAll_Ptr();
      ragdollArgs[7] = (void*)Get_Ragdoll_SetPhysicsLayer_Ptr();
      ragdollArgs[8] = (void*)Get_Ragdoll_GetOwnerFromBone_Ptr();

      using RagdollInteropInitFn = void(*)(void**, int);
      RagdollInteropInitFn initRagdollFn = nullptr;
      int rcRagdoll = load_assembly_and_get_function_pointer(
         fullPath.c_str(),
         L"ClaymoreEngine.Physics.Ragdoll, ClaymoreEngine",
         L"Initialize",
         L"ClaymoreEngine.Physics.RagdollInteropInitDelegate, ClaymoreEngine",
         nullptr,
         (void**)&initRagdollFn
      );
      if (rcRagdoll == 0 && initRagdollFn) {
         initRagdollFn(ragdollArgs, 9);
      }
   }

  // ScriptableObjects interop bootstrap
  {
      void* fn = nullptr;
      int rc = load_assembly_and_get_function_pointer(
          fullPath.c_str(),
          L"ClaymoreEngine.ScriptableInteropExports, ClaymoreEngine",
          L"GetScriptableAPI",
          L"ClaymoreEngine.ScriptableInteropExports+GetManagedScriptableAPIDelegate, ClaymoreEngine",
          nullptr,
          &fn
      );
      if (rc == 0 && fn) {
          GetManagedScriptableAPI getApi = reinterpret_cast<GetManagedScriptableAPI>(fn);
          NativeScriptableAPI native{};
          native.user = nullptr;
          native.RegisterType = &Scriptable_NativeRegisterType;
          native.SetField    = &Scriptable_NativeSetField;
          native.GetField    = &Scriptable_NativeGetField;
          native.MarkDirty   = &Scriptable_NativeMarkDirty;
          native.GetPathForGUID = &Scriptable_GetPathForGUID;
          native.IsTypeAssignable = &Scriptable_IsTypeAssignable;
          native.ReadFileContents = &Scriptable_ReadFileContents;
          native.InvalidateCache = &Scriptable_InvalidateCache;

          ManagedScriptableAPI managed{};
          getApi(&native, &managed);
          Scriptable_SetManagedAPI(managed);
          if (managed.EnumerateTypes) managed.EnumerateTypes(native.user);
      } else {
          std::cerr << "[Interop] Failed to init Scriptable interop (hr=0x" << std::hex << rc << ")\n";
      }
  }

  // Resolve ClayObjectCacheExports.InvalidateCache function pointer
  {
      void* fn = nullptr;
      int rc = load_assembly_and_get_function_pointer(
          fullPath.c_str(),
          L"ClaymoreEngine.ClayObjectCacheExports, ClaymoreEngine",
          L"InvalidateCache",
          L"ClaymoreEngine.InvalidateClayObjectCacheDelegate, ClaymoreEngine",
          nullptr,
          &fn
      );
      if (rc == 0 && fn) {
          g_InvalidateCache = reinterpret_cast<InvalidateCacheFn>(fn);
          std::cout << "[Interop] Resolved ClayObjectCacheExports.InvalidateCache" << std::endl;
      } else {
          std::cerr << "[Interop] Failed to resolve ClayObjectCacheExports.InvalidateCache (hr=0x" << std::hex << rc << ")\n";
      }
  }

  // Prefab interop bootstrap
  {
      
      void* args[6];
      args[0] = (void*)Get_Prefab_InstantiateByGuid_Ptr();
      args[1] = (void*)Get_Prefab_InstantiateByGuidBlocking_Ptr();
      args[2] = (void*)Get_Prefab_GetAsyncStatus_Ptr();
      args[3] = (void*)Get_Prefab_GetAssetNameByGuid_Ptr();
      args[4] = (void*)Get_Prefab_InstantiateByGuidWithRoot_Ptr();
      args[5] = (void*)Get_Prefab_PreloadByGuid_Ptr();

      using PrefabInteropInitFn = void(*)(void**, int);
      PrefabInteropInitFn initPrefabFn = nullptr;
      int rcPrefab = load_assembly_and_get_function_pointer(
          fullPath.c_str(),
          L"ClaymoreEngine.PrefabInterop, ClaymoreEngine",
          L"InitializeInteropExport",
          L"ClaymoreEngine.PrefabInteropInitDelegate, ClaymoreEngine",
          nullptr,
          (void**)&initPrefabFn
      );
      if (rcPrefab == 0 && initPrefabFn) {
          initPrefabFn(args, 6);
      }
  }

  // Mesh interop bootstrap
  {
      constexpr int kExpectedMeshInteropCount = 27;
      void* args[kExpectedMeshInteropCount];
      int i = 0;
      args[i++] = (void*)Get_Mesh_InstantiateByGuid_Ptr();
      args[i++] = (void*)Get_Mesh_SetByGuid_Ptr();
      args[i++] = (void*)Get_Mesh_HasComponent_Ptr();
      args[i++] = (void*)Get_Mesh_GetReference_Ptr();
      args[i++] = (void*)Get_Mesh_GetVertexCount_Ptr();
      args[i++] = (void*)Get_Mesh_GetIndexCount_Ptr();
      args[i++] = (void*)Get_Mesh_GetSubmeshCount_Ptr();
      args[i++] = (void*)Get_Mesh_GetMaterialSlotCount_Ptr();
      args[i++] = (void*)Get_Mesh_GetMaterialSlotName_Ptr();
      args[i++] = (void*)Get_Mesh_GetBoundsMin_Ptr();
      args[i++] = (void*)Get_Mesh_GetBoundsMax_Ptr();
      args[i++] = (void*)Get_Mesh_GetBoundsPadding_Ptr();
      args[i++] = (void*)Get_Mesh_SetBoundsPadding_Ptr();
      args[i++] = (void*)Get_Mesh_GetRenderOnTop_Ptr();
      args[i++] = (void*)Get_Mesh_SetRenderOnTop_Ptr();
      args[i++] = (void*)Get_Mesh_GetShowBackfaces_Ptr();
      args[i++] = (void*)Get_Mesh_SetShowBackfaces_Ptr();
      args[i++] = (void*)Get_Mesh_GetSkipFrustumCulling_Ptr();
      args[i++] = (void*)Get_Mesh_SetSkipFrustumCulling_Ptr();
      args[i++] = (void*)Get_Mesh_GetRenderOrder_Ptr();
      args[i++] = (void*)Get_Mesh_SetRenderOrder_Ptr();
      args[i++] = (void*)Get_Mesh_GetUniqueMaterial_Ptr();
      args[i++] = (void*)Get_Mesh_SetUniqueMaterial_Ptr();
      args[i++] = (void*)Get_Mesh_HasSkinning_Ptr();
      args[i++] = (void*)Get_Mesh_GetName_Ptr();
      args[i++] = (void*)Get_Mesh_GetAssetNameByGuid_Ptr();
      args[i++] = (void*)Get_Mesh_InstantiateByGuidWithRoot_Ptr();

      if (i != kExpectedMeshInteropCount) {
          Logger::LogError("[Interop] Mesh interop count mismatch during editor bootstrap.");
      } else {
          using MeshInteropInitFn = void(*)(void**, int);
          MeshInteropInitFn initMeshFn = nullptr;
          int rcMesh = load_assembly_and_get_function_pointer(
              fullPath.c_str(),
              L"ClaymoreEngine.MeshInterop, ClaymoreEngine",
              L"InitializeInteropExport",
              L"ClaymoreEngine.MeshInteropInitDelegate, ClaymoreEngine",
              nullptr,
              (void**)&initMeshFn
          );
          if (rcMesh == 0 && initMeshFn) {
              initMeshFn(args, kExpectedMeshInteropCount);
          }
      }
  }

  // Audio interop bootstrap
  {
      constexpr int kExpectedAudioInteropCount = 32;
      void* args[kExpectedAudioInteropCount];
      int i = 0;
      args[i++] = (void*)Get_Audio_GetAssetNameByGuid_Ptr();
      args[i++] = (void*)Get_Audio_GetGuidFromPath_Ptr();
      args[i++] = (void*)Get_AudioSource_GetClipReference_Ptr();
      args[i++] = (void*)Get_AudioSource_SetClipReference_Ptr();
      args[i++] = (void*)Get_AudioSource_GetAudioPath_Ptr();
      args[i++] = (void*)Get_AudioSource_SetAudioPath_Ptr();
      args[i++] = (void*)Get_AudioSource_GetVolume_Ptr();
      args[i++] = (void*)Get_AudioSource_SetVolume_Ptr();
      args[i++] = (void*)Get_AudioSource_GetPitch_Ptr();
      args[i++] = (void*)Get_AudioSource_SetPitch_Ptr();
      args[i++] = (void*)Get_AudioSource_GetLoop_Ptr();
      args[i++] = (void*)Get_AudioSource_SetLoop_Ptr();
      args[i++] = (void*)Get_AudioSource_GetPlayOnAwake_Ptr();
      args[i++] = (void*)Get_AudioSource_SetPlayOnAwake_Ptr();
      args[i++] = (void*)Get_AudioSource_GetMute_Ptr();
      args[i++] = (void*)Get_AudioSource_SetMute_Ptr();
      args[i++] = (void*)Get_AudioSource_GetSpatial_Ptr();
      args[i++] = (void*)Get_AudioSource_SetSpatial_Ptr();
      args[i++] = (void*)Get_AudioSource_GetMinDistance_Ptr();
      args[i++] = (void*)Get_AudioSource_SetMinDistance_Ptr();
      args[i++] = (void*)Get_AudioSource_GetMaxDistance_Ptr();
      args[i++] = (void*)Get_AudioSource_SetMaxDistance_Ptr();
      args[i++] = (void*)Get_AudioSource_GetDopplerFactor_Ptr();
      args[i++] = (void*)Get_AudioSource_SetDopplerFactor_Ptr();
      args[i++] = (void*)Get_AudioSource_GetRolloff_Ptr();
      args[i++] = (void*)Get_AudioSource_SetRolloff_Ptr();
      args[i++] = (void*)Get_AudioSource_GetIsPlaying_Ptr();
      args[i++] = (void*)Get_AudioSource_GetIsPaused_Ptr();
      args[i++] = (void*)Get_AudioSource_Play_Ptr();
      args[i++] = (void*)Get_AudioSource_Stop_Ptr();
      args[i++] = (void*)Get_AudioSource_Pause_Ptr();
      args[i++] = (void*)Get_AudioSource_Resume_Ptr();

      if (i != kExpectedAudioInteropCount) {
          Logger::LogError("[Interop] Audio interop count mismatch during editor bootstrap.");
      } else {
          using AudioInteropInitFn = void(*)(void**, int);
          AudioInteropInitFn initAudioFn = nullptr;
          int rcAudio = load_assembly_and_get_function_pointer(
              fullPath.c_str(),
              L"ClaymoreEngine.AudioInterop, ClaymoreEngine",
              L"InitializeInteropExport",
              L"ClaymoreEngine.AudioInteropInitDelegate, ClaymoreEngine",
              nullptr,
              (void**)&initAudioFn
          );
          if (rcAudio == 0 && initAudioFn) {
              initAudioFn(args, kExpectedAudioInteropCount);
          }
      }
  }

  // Material interop bootstrap
  {
      void* args[32];
      int i = 0;
      args[i++] = Get_Material_SetVector4_Ptr();
      args[i++] = Get_Material_GetVector4_Ptr();
      args[i++] = Get_Material_HasProperty_Ptr();
      args[i++] = Get_Material_RemoveProperty_Ptr();
      args[i++] = Get_Material_ClearAll_Ptr();
      args[i++] = Get_Material_SetTexturePath_Ptr();
      args[i++] = Get_Material_SetVector4Slot_Ptr();
      args[i++] = Get_Material_GetVector4Slot_Ptr();
      args[i++] = Get_Material_HasPropertySlot_Ptr();
      args[i++] = Get_Material_RemovePropertySlot_Ptr();
      args[i++] = Get_Material_ClearSlot_Ptr();
      args[i++] = Get_Material_GetSlotCount_Ptr();
      args[i++] = Get_Material_SetTexturePathSlot_Ptr();
      args[i++] = Get_Material_GetMaterialTypeSlot_Ptr();
      args[i++] = Get_Material_GetMaterialNameSlot_Ptr();
      args[i++] = Get_Material_GetMaterialAssetPathSlot_Ptr();
      args[i++] = Get_Material_SetMaterialAssetPathSlot_Ptr();
      args[i++] = Get_Material_SetMaterialVector4Slot_Ptr();
      args[i++] = Get_Material_GetMaterialVector4Slot_Ptr();
      args[i++] = Get_Material_HasMaterialPropertySlot_Ptr();
      args[i++] = Get_Material_SetMaterialTexturePathSlot_Ptr();
      args[i++] = Get_Material_GetMaterialTexturePathSlot_Ptr();
      args[i++] = Get_Material_GetPbrScalarSlot_Ptr();
      args[i++] = Get_Material_SetPbrScalarSlot_Ptr();
      args[i++] = Get_Material_GetPbrEmissionColorSlot_Ptr();
      args[i++] = Get_Material_SetPbrEmissionColorSlot_Ptr();
      args[i++] = Get_Material_GetPbrUVTransformSlot_Ptr();
      args[i++] = Get_Material_SetPbrUVTransformSlot_Ptr();
      args[i++] = Get_Material_GetPbrReceiveShadowsOverrideSlot_Ptr();
      args[i++] = Get_Material_SetPbrReceiveShadowsOverrideSlot_Ptr();
      args[i++] = Get_Material_GetPbrReceiveShadowsSlot_Ptr();
      args[i++] = Get_Material_SetPbrReceiveShadowsSlot_Ptr();

      using MaterialInteropInitFn = void(*)(void**, int);
      MaterialInteropInitFn initMaterialFn = nullptr;
      int rcMaterial = load_assembly_and_get_function_pointer(
          fullPath.c_str(),
          L"ClaymoreEngine.MaterialInterop, ClaymoreEngine",
          L"Initialize",
          L"ClaymoreEngine.MaterialInteropInitDelegate, ClaymoreEngine",
          nullptr,
          (void**)&initMaterialFn
      );
      if (rcMaterial == 0 && initMaterialFn) {
          initMaterialFn(args, i);
      }
  }

  // Particle interop bootstrap (52 functions)
  {
      void* args[52];
      int i = 0;
      
      // Core (6 functions)
      args[i++] = Get_Particle_GetEnabled_Ptr();
      args[i++] = Get_Particle_SetEnabled_Ptr();
      args[i++] = Get_Particle_Play_Ptr();
      args[i++] = Get_Particle_Stop_Ptr();
      args[i++] = Get_Particle_Restart_Ptr();
      args[i++] = Get_Particle_IsPlaying_Ptr();
      
      // Space & Shape (8 functions)
      args[i++] = Get_Particle_GetSimulationSpace_Ptr();
      args[i++] = Get_Particle_SetSimulationSpace_Ptr();
      args[i++] = Get_Particle_GetShape_Ptr();
      args[i++] = Get_Particle_SetShape_Ptr();
      args[i++] = Get_Particle_GetShapeRadius_Ptr();
      args[i++] = Get_Particle_SetShapeRadius_Ptr();
      args[i++] = Get_Particle_GetShapeAngle_Ptr();
      args[i++] = Get_Particle_SetShapeAngle_Ptr();
      
      // Start Values (6 functions)
      args[i++] = Get_Particle_GetStartSpeed_Ptr();
      args[i++] = Get_Particle_SetStartSpeed_Ptr();
      args[i++] = Get_Particle_GetStartSize_Ptr();
      args[i++] = Get_Particle_SetStartSize_Ptr();
      args[i++] = Get_Particle_GetStartColor_Ptr();
      args[i++] = Get_Particle_SetStartColor_Ptr();
      
      // Emission (8 functions)
      args[i++] = Get_Particle_GetEmissionRate_Ptr();
      args[i++] = Get_Particle_SetEmissionRate_Ptr();
      args[i++] = Get_Particle_GetLooping_Ptr();
      args[i++] = Get_Particle_SetLooping_Ptr();
      args[i++] = Get_Particle_GetDuration_Ptr();
      args[i++] = Get_Particle_SetDuration_Ptr();
      args[i++] = Get_Particle_GetLifetime_Ptr();
      args[i++] = Get_Particle_SetLifetime_Ptr();
      
      // Physics (4 functions)
      args[i++] = Get_Particle_GetGravityModifier_Ptr();
      args[i++] = Get_Particle_SetGravityModifier_Ptr();
      args[i++] = Get_Particle_GetMaxParticles_Ptr();
      args[i++] = Get_Particle_SetMaxParticles_Ptr();
      
      // Module Enables (12 functions)
      args[i++] = Get_Particle_GetSizeOverLifetimeEnabled_Ptr();
      args[i++] = Get_Particle_SetSizeOverLifetimeEnabled_Ptr();
      args[i++] = Get_Particle_GetColorOverLifetimeEnabled_Ptr();
      args[i++] = Get_Particle_SetColorOverLifetimeEnabled_Ptr();
      args[i++] = Get_Particle_GetVelocityOverLifetimeEnabled_Ptr();
      args[i++] = Get_Particle_SetVelocityOverLifetimeEnabled_Ptr();
      args[i++] = Get_Particle_GetRotationOverLifetimeEnabled_Ptr();
      args[i++] = Get_Particle_SetRotationOverLifetimeEnabled_Ptr();
      args[i++] = Get_Particle_GetAlignWithTrajectory_Ptr();
      args[i++] = Get_Particle_SetAlignWithTrajectory_Ptr();
      args[i++] = Get_Particle_GetBurstEnabled_Ptr();
      args[i++] = Get_Particle_SetBurstEnabled_Ptr();
      
      // Size Over Lifetime (2 functions)
      args[i++] = Get_Particle_GetSizeOverLifetime_Ptr();
      args[i++] = Get_Particle_SetSizeOverLifetime_Ptr();
      
      // Color Gradient (4 functions)
      args[i++] = Get_Particle_GetColorGradientKeyCount_Ptr();
      args[i++] = Get_Particle_GetColorGradientKey_Ptr();
      args[i++] = Get_Particle_SetColorGradientKey_Ptr();
      args[i++] = Get_Particle_ClearColorGradient_Ptr();
      
      // Burst (2 functions)
      args[i++] = Get_Particle_GetBurstCount_Ptr();
      args[i++] = Get_Particle_SetBurstCount_Ptr();

      using ParticleInteropInitFn = void(*)(void**, int);
      ParticleInteropInitFn initParticleFn = nullptr;
      int rcParticle = load_assembly_and_get_function_pointer(
          fullPath.c_str(),
          L"ClaymoreEngine.ParticleInterop, ClaymoreEngine",
          L"Initialize",
          L"ClaymoreEngine.ParticleInteropInitDelegate, ClaymoreEngine",
          nullptr,
          (void**)&initParticleFn
      );
      if (rcParticle == 0 && initParticleFn) {
          initParticleFn(args, i);
      }
  }

  // Dialogue interop bootstrap
  {
      void* args[50];
      int i = 0;
      // Library management (4)
      args[i++] = Get_Dialogue_LoadLibrary_Ptr();
      args[i++] = Get_Dialogue_UnloadLibrary_Ptr();
      args[i++] = Get_Dialogue_GetLibraryName_Ptr();
      args[i++] = Get_Dialogue_GetLibraryCharacterId_Ptr();
      // Playback (7)
      args[i++] = Get_Dialogue_StartFromLibrary_Ptr();
      args[i++] = Get_Dialogue_StartFromText_Ptr();
      args[i++] = Get_Dialogue_Advance_Ptr();
      args[i++] = Get_Dialogue_SelectChoice_Ptr();
      args[i++] = Get_Dialogue_ForceEnd_Ptr();
      args[i++] = Get_Dialogue_Pause_Ptr();
      args[i++] = Get_Dialogue_Resume_Ptr();
      // State queries (10)
      args[i++] = Get_Dialogue_IsActive_Ptr();
      args[i++] = Get_Dialogue_IsTyping_Ptr();
      args[i++] = Get_Dialogue_IsWaitingForChoice_Ptr();
      args[i++] = Get_Dialogue_GetState_Ptr();
      args[i++] = Get_Dialogue_GetDisplayedText_Ptr();
      args[i++] = Get_Dialogue_GetCurrentSpeaker_Ptr();
      args[i++] = Get_Dialogue_GetSpeakerEntityId_Ptr();
      args[i++] = Get_Dialogue_GetChoiceCount_Ptr();
      args[i++] = Get_Dialogue_GetChoiceText_Ptr();
      args[i++] = Get_Dialogue_GetConversationTitle_Ptr();
      // Game state (3)
      args[i++] = Get_Dialogue_SetState_Ptr();
      args[i++] = Get_Dialogue_GetStateValue_Ptr();
      args[i++] = Get_Dialogue_ClearState_Ptr();
      // Config (3)
      args[i++] = Get_Dialogue_SetTextSpeed_Ptr();
      args[i++] = Get_Dialogue_SetMinDisplayTime_Ptr();
      args[i++] = Get_Dialogue_SetEndCooldown_Ptr();
      // Callback setters (17)
      args[i++] = Get_Dialogue_SetOnStarted_Ptr();
      args[i++] = Get_Dialogue_SetOnEnded_Ptr();
      args[i++] = Get_Dialogue_SetOnLine_Ptr();
      args[i++] = Get_Dialogue_SetOnEmote_Ptr();
      args[i++] = Get_Dialogue_SetOnStateChange_Ptr();
      args[i++] = Get_Dialogue_SetOnEmotion_Ptr();
      args[i++] = Get_Dialogue_SetOnTypingComplete_Ptr();
      args[i++] = Get_Dialogue_SetOnCharTyped_Ptr();
      args[i++] = Get_Dialogue_SetOnGiveItem_Ptr();
      args[i++] = Get_Dialogue_SetOnTakeItem_Ptr();
      args[i++] = Get_Dialogue_SetOnStartQuest_Ptr();
      args[i++] = Get_Dialogue_SetOnCompleteStep_Ptr();
      args[i++] = Get_Dialogue_SetOnPlayAnim_Ptr();
      args[i++] = Get_Dialogue_SetOnPlaySound_Ptr();
      args[i++] = Get_Dialogue_SetOnWait_Ptr();
      args[i++] = Get_Dialogue_SetOnCamera_Ptr();
      args[i++] = Get_Dialogue_SetOnSetEvent_Ptr();

      using DialogueInteropInitFn = void(*)(void**, int);
      DialogueInteropInitFn initDialogueFn = nullptr;
      int rcDialogue = load_assembly_and_get_function_pointer(
          fullPath.c_str(),
          L"ClaymoreEngine.DialogueInterop, ClaymoreEngine",
          L"InitializeInteropExport",
          L"ClaymoreEngine.DialogueInteropInitDelegate, ClaymoreEngine",
          nullptr,
          (void**)&initDialogueFn
      );
      if (rcDialogue == 0 && initDialogueFn) {
          initDialogueFn(args, i);
      } else {
          std::cerr << "[Interop] Failed to init Dialogue interop (hr=0x" << std::hex << rcDialogue << ")\n";
      }
  }

  // Quest interop bootstrap
  {
      void* args[48];
      int i = 0;
      
      // Core functions
      args[i++] = Get_Quest_LoadDatabase_Ptr();
      args[i++] = Get_Quest_UnloadDatabase_Ptr();
      args[i++] = Get_Quest_StartQuest_Ptr();
      args[i++] = Get_Quest_AdvanceStage_Ptr();
      args[i++] = Get_Quest_CompleteStage_Ptr();
      args[i++] = Get_Quest_FailQuest_Ptr();
      args[i++] = Get_Quest_AbandonQuest_Ptr();
      args[i++] = Get_Quest_UpdateObjectiveProgress_Ptr();
      args[i++] = Get_Quest_SetObjectiveProgress_Ptr();
      
      // State queries
      args[i++] = Get_Quest_GetQuestState_Ptr();
      args[i++] = Get_Quest_GetStageState_Ptr();
      args[i++] = Get_Quest_GetActiveStageId_Ptr();
      args[i++] = Get_Quest_GetObjectiveProgress_Ptr();
      args[i++] = Get_Quest_GetObjectiveTarget_Ptr();
      args[i++] = Get_Quest_IsObjectiveComplete_Ptr();
      
      // Bulk queries
      args[i++] = Get_Quest_GetActiveQuestCount_Ptr();
      args[i++] = Get_Quest_GetActiveQuestIds_Ptr();
      args[i++] = Get_Quest_GetCompletedQuestCount_Ptr();
      args[i++] = Get_Quest_GetCompletedQuestIds_Ptr();
      args[i++] = Get_Quest_GetBranchHistoryCount_Ptr();
      args[i++] = Get_Quest_GetBranchHistory_Ptr();
      
      // Conditions
      args[i++] = Get_Quest_CanStartQuest_Ptr();
      args[i++] = Get_Quest_AreStageConditionsMet_Ptr();
      args[i++] = Get_Quest_IsDialogueTriggerActive_Ptr();
      
      // Save/Load
      args[i++] = Get_Quest_SerializeState_Ptr();
      args[i++] = Get_Quest_DeserializeState_Ptr();
      args[i++] = Get_Quest_ResetAllState_Ptr();
      
      // Callbacks
      args[i++] = Get_Quest_SetOnQuestStarted_Ptr();
      args[i++] = Get_Quest_SetOnStageStarted_Ptr();
      args[i++] = Get_Quest_SetOnStageCompleted_Ptr();
      args[i++] = Get_Quest_SetOnQuestCompleted_Ptr();
      args[i++] = Get_Quest_SetOnQuestFailed_Ptr();
      args[i++] = Get_Quest_SetOnObjectiveProgress_Ptr();

      using QuestInteropInitFn = void(*)(void**, int);
      QuestInteropInitFn initQuestFn = nullptr;
      int rcQuest = load_assembly_and_get_function_pointer(
          fullPath.c_str(),
          L"ClaymoreEngine.QuestInterop, ClaymoreEngine",
          L"InitializeInteropExport",
          L"ClaymoreEngine.QuestInteropInitDelegate, ClaymoreEngine",
          nullptr,
          (void**)&initQuestFn
      );
      if (rcQuest == 0 && initQuestFn) {
          initQuestFn(args, i);
      } else {
          std::cerr << "[Interop] Failed to init Quest interop (hr=0x" << std::hex << rcQuest << ")\n";
      }
  }

  // Node Graph interop bootstrap
  { 
      void* fn = nullptr;
      int rc = load_assembly_and_get_function_pointer(
          fullPath.c_str(),
          L"ClaymoreEngine.NodeGraphInteropExports, ClaymoreEngine",
          L"GetNodeGraphAPI",
          L"ClaymoreEngine.GetManagedNodeGraphAPIDelegate, ClaymoreEngine",
          nullptr,
          &fn
      ); 
      if (rc == 0 && fn) {
          GetManagedNodeGraphAPI getApi = reinterpret_cast<GetManagedNodeGraphAPI>(fn);
          NativeNodeGraphAPI native{};
          native.user = nullptr;
          native.RegisterNodeType = [](const NodeTypeDescNative* d){ if(d) NodeGraph_NativeRegisterType(*d); };

          ManagedNodeGraphAPI managed{};
          getApi(&native, &managed);
          NodeGraph_SetManagedAPI(managed);
          if (managed.EnumerateNodeTypes) managed.EnumerateNodeTypes(native.user);
      } else {
          std::cerr << "[Interop] Failed to init NodeGraph interop (hr=0x" << std::hex << rc << ")\n";
      }
  }


  // Navigation Interop
   {
       void* navArgs[22];
       navArgs[0] = (void*)Get_Nav_FindPath_Ptr();
       navArgs[1] = (void*)Get_Nav_Agent_SetDest_Ptr();
       navArgs[2] = (void*)Get_Nav_Agent_Stop_Ptr();
       navArgs[3] = (void*)Get_Nav_Agent_Warp_Ptr();
       navArgs[4] = (void*)Get_Nav_Agent_Remaining_Ptr();
       navArgs[5] = (void*)Get_Nav_SetOnPathComplete_Ptr();
       navArgs[6] = (void*)Get_Nav_Agent_IsStopped_Ptr();
       navArgs[7] = (void*)Get_Nav_Agent_IsMoving_Ptr();
       navArgs[8] = (void*)Get_Nav_Agent_HasPath_Ptr();
       navArgs[9] = (void*)Get_Nav_Agent_GetSpeed_Ptr();
       navArgs[10] = (void*)Get_Nav_Agent_SetSpeed_Ptr();
       navArgs[11] = (void*)Get_Nav_Agent_GetAccel_Ptr();
       navArgs[12] = (void*)Get_Nav_Agent_SetAccel_Ptr();
       navArgs[13] = (void*)Get_Nav_Agent_GetRadius_Ptr();
       navArgs[14] = (void*)Get_Nav_Agent_SetRadius_Ptr();
       navArgs[15] = (void*)Get_Nav_Agent_GetHeight_Ptr();
       navArgs[16] = (void*)Get_Nav_Agent_SetHeight_Ptr();
       navArgs[17] = (void*)Get_Nav_Agent_GetStopDist_Ptr();
       navArgs[18] = (void*)Get_Nav_Agent_SetStopDist_Ptr();
       navArgs[19] = (void*)Get_Nav_Agent_GetVelocityX_Ptr();
       navArgs[20] = (void*)Get_Nav_Agent_GetVelocityY_Ptr();
       navArgs[21] = (void*)Get_Nav_Agent_GetVelocityZ_Ptr();
       using NavInteropInitFn = void(*)(void**, int);
       NavInteropInitFn initNavFn = nullptr;
       int rcNav = load_assembly_and_get_function_pointer(
           fullPath.c_str(),
           L"ClaymoreEngine.NavigationInterop, ClaymoreEngine",
           L"InitializeInteropExport",
           L"ClaymoreEngine.NavigationInteropInitDelegate, ClaymoreEngine",
           nullptr,
           (void**)&initNavFn
       );
       if (rcNav == 0 && initNavFn) {
           initNavFn(navArgs, 22);
       }
   }

   // IK interop bootstrap
   {
       void* ikArgs[5];
       ikArgs[0] = (void*)Get_IK_SetWeight_Ptr();
       ikArgs[1] = (void*)Get_IK_SetTarget_Ptr();
       ikArgs[2] = (void*)Get_IK_SetPole_Ptr();
       ikArgs[3] = (void*)Get_IK_SetChain_Ptr();
       ikArgs[4] = (void*)Get_IK_GetErrorMeters_Ptr();

       using IKInteropInitFn = void(*)(void**, int);
       IKInteropInitFn initIkFn = nullptr;
       int rcIk = load_assembly_and_get_function_pointer(
           fullPath.c_str(),
           L"ClaymoreEngine.IKInterop, ClaymoreEngine",
           L"InitializeInteropExport",
           L"ClaymoreEngine.IKInteropInitDelegate, ClaymoreEngine",
           nullptr,
           (void**)&initIkFn
       );
       if (rcIk == 0 && initIkFn) {
           initIkFn(ikArgs, 5);
       }
   }

   // LookAt/Aim constraint interop bootstrap
   {
       void* lookAtArgs[11];
       lookAtArgs[0] = (void*)Get_LookAt_SetEnabled_Ptr();
       lookAtArgs[1] = (void*)Get_LookAt_SetWeight_Ptr();
       lookAtArgs[2] = (void*)Get_LookAt_SetTarget_Ptr();
       lookAtArgs[3] = (void*)Get_LookAt_SetSmoothingSpeed_Ptr();
       lookAtArgs[4] = (void*)Get_LookAt_SetMaxAngles_Ptr();
       lookAtArgs[5] = (void*)Get_LookAt_GetEnabled_Ptr();
       lookAtArgs[6] = (void*)Get_LookAt_GetWeight_Ptr();
       lookAtArgs[7] = (void*)Get_LookAt_SetMode_Ptr();
       lookAtArgs[8] = (void*)Get_LookAt_GetMode_Ptr();
       lookAtArgs[9] = (void*)Get_LookAt_SetTargetUsesNegativeZForward_Ptr();
       lookAtArgs[10] = (void*)Get_LookAt_GetTargetUsesNegativeZForward_Ptr();

       using LookAtInteropInitFn = void(*)(void**, int);
       LookAtInteropInitFn initLookAtFn = nullptr;
       int rcLookAt = load_assembly_and_get_function_pointer(
           fullPath.c_str(),
           L"ClaymoreEngine.LookAtInterop, ClaymoreEngine",
           L"InitializeInteropExport",
           L"ClaymoreEngine.LookAtInteropInitDelegate, ClaymoreEngine",
           nullptr,
           (void**)&initLookAtFn
       );
       if (rcLookAt == 0 && initLookAtFn) {
           initLookAtFn(lookAtArgs, 11);
       }
   }

   // Animation Event interop bootstrap
   {
       void* animEventArgs[4];
       animEventArgs[0] = (void*)Get_AnimEvent_SetCallback_Ptr();
       animEventArgs[1] = (void*)Get_AnimEvent_SetEntityCallback_Ptr();
       animEventArgs[2] = (void*)Get_AnimEvent_ClearEntityCallback_Ptr();
       animEventArgs[3] = (void*)Get_AnimEvent_ClearAllCallbacks_Ptr();

       using AnimEventInteropInitFn = void(*)(void**, int);
       AnimEventInteropInitFn initAnimEventFn = nullptr;
       int rcAnimEvent = load_assembly_and_get_function_pointer(
           fullPath.c_str(),
           L"ClaymoreEngine.AnimationEventInterop, ClaymoreEngine",
           L"Initialize",
           L"ClaymoreEngine.AnimationEventInteropInitDelegate, ClaymoreEngine",
           nullptr,
           (void**)&initAnimEventFn
       );
       if (rcAnimEvent == 0 && initAnimEventFn) {
           initAnimEventFn(animEventArgs, 4);
           std::cout << "[Interop] Animation Event interop initialized successfully\n";
       } else {
           std::cerr << "[Interop] Failed to initialize Animation Event interop (hr=0x" << std::hex << rcAnimEvent << ")\n";
       }
   }

   // Animation State interop bootstrap (OnStateEntered/OnStateExited callbacks)
   {
       void* animStateArgs[1];
       animStateArgs[0] = (void*)Get_AnimState_SetCallback_Ptr();

       using AnimStateInteropInitFn = void(*)(void**, int);
       AnimStateInteropInitFn initAnimStateFn = nullptr;
       int rcAnimState = load_assembly_and_get_function_pointer(
           fullPath.c_str(),
           L"ClaymoreEngine.AnimationStateInterop, ClaymoreEngine",
           L"Initialize",
           L"ClaymoreEngine.AnimationEventInteropInitDelegate, ClaymoreEngine",
           nullptr,
           (void**)&initAnimStateFn
       );
       if (rcAnimState == 0 && initAnimStateFn) {
           initAnimStateFn(animStateArgs, 1);
           std::cout << "[Interop] Animation State interop initialized successfully\n";
       } else {
           std::cerr << "[Interop] Failed to initialize Animation State interop (hr=0x" << std::hex << rcAnimState << ")\n";
       }
   }
   }

   return true;
   }

// ----------------------------------------
// Reload C# Scripts
// ----------------------------------------
   
    

void ReloadScripts()
   {
   if (!load_assembly_and_get_function_pointer) {
      std::cerr << "[Interop] .NET runtime not initialized; skipping ReloadScripts.\n";
      return;
   }
   std::filesystem::path scriptsDLL = std::filesystem::path(Project::GetProjectDirectory()) / ".library" / L"GameScripts.dll";
   std::wstring scriptsDllW = std::filesystem::absolute(scriptsDLL).wstring();
   wchar_t exePath[MAX_PATH];
   GetModuleFileNameW(NULL, exePath, MAX_PATH);
   std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
   std::filesystem::path engineDLL = exeDir / L"ClaymoreEngine.dll";

   if (!g_ReloadScripts)
      {
      void* fn = nullptr;
      int rc = load_assembly_and_get_function_pointer(
         engineDLL.wstring().c_str(),
         L"ClaymoreEngine.InteropProcessor, ClaymoreEngine",
         L"ReloadScripts",
         L"ClaymoreEngine.ReloadScriptsDelegate, ClaymoreEngine",
         nullptr,
         &fn
      );

      if (rc != 0 || !fn)
         {
         std::cerr << "[Interop] Failed to resolve ReloadScripts.\n";
         return;
         }

      g_ReloadScripts = reinterpret_cast<ReloadScripts_fn>(fn);
      }

   if (!g_ReloadScripts) {
        std::cerr << "[Interop] ReloadScripts function pointer not set.\n";
        return;
    }

    int rc = g_ReloadScripts(scriptsDllW.c_str());
    if (rc != 0)
    {
        std::cerr << "[Interop] ReloadScripts returned error.\n";
        return;
    }

   std::cout << "[Interop] Scripts reloaded.\n";

   // Drop stale reflection metadata so re-register captures current fields
   ScriptReflection::ClearAllScriptProperties();

   if (!g_RegisterAllScripts)
      {
      void* fn = nullptr;
      int rc = load_assembly_and_get_function_pointer(
         engineDLL.wstring().c_str(),
         L"ClaymoreEngine.InteropExports, ClaymoreEngine",
         L"RegisterAllScripts",
         L"ClaymoreEngine.RegisterAllScriptsDelegate, ClaymoreEngine",
         nullptr,
         &fn
      );

      if (rc != 0 || !fn)
         {
         std::cerr << "[Interop] Failed to resolve RegisterAllScripts.\n";
         return;
         }

      g_RegisterAllScripts = reinterpret_cast<RegisterAllScriptsFn>(fn);
      }

// Pass struct of callbacks
   g_RegisterAllScripts(reinterpret_cast<void*>(&g_ScriptRegInterop));
   
   // Recreate script instances so they use the new assembly types (both edit and play mode)
   if (Scene::CurrentScene) {
      std::cout << "[Interop] Recreating script instances after hot-reload...\n";
      Scene::CurrentScene->RecreateScriptInstances();
   }
   }

// ----------------------------------------
// Call OnValidate for all scripts after recompile
// ----------------------------------------
// Helper function to call OnValidate for scripts on a single entity
static void CallOnValidateForEntity(Scene& scene, EntityID entityId)
{
   if (!g_Script_Invoke || !SetManagedFieldPtr) return;
   
   auto* data = scene.GetEntityData(entityId);
   if (!data) return;
   
   for (auto& script : data->Scripts) {
      if (!script.Instance) continue;
      
      void* scriptHandle = nullptr;
      if (script.Instance->GetBackend() == ScriptBackend::Managed) {
         if (auto managed = std::dynamic_pointer_cast<ManagedScriptComponent>(script.Instance)) {
            scriptHandle = managed->GetHandle();
         }
      }
      
      if (!scriptHandle) continue;
      
      // DEBUG: Log all Values before OnValidate
      if (entityId == 200 && script.ClassName == "CharacterSheet") {
         std::cout << "[OnValidate] DEBUG: Before processing, script.Values for CharacterSheet on entity 200:" << std::endl;
         for (const auto& [key, val] : script.Values) {
            if (std::holds_alternative<int>(val)) {
               int v = std::get<int>(val);
               auto* entData = scene.GetEntityData(static_cast<EntityID>(v));
               std::string entName = entData ? entData->Name : "Unknown";
               std::cout << "  " << key << " = " << v << " (" << entName << ")" << std::endl;
            }
         }
      }
      
      // Populate all serialized fields from stored Values
      if (ScriptReflection::HasProperties(script.ClassName)) {
         auto& properties = ScriptReflection::GetScriptProperties(script.ClassName);
         for (auto& prop : properties) {
            auto it = script.Values.find(prop.name);
            if (it == script.Values.end()) {
               // Do not overwrite managed constructor/Bind-populated values (e.g. PopulateFromResources)
               // when no serialized value exists for this field.
               continue;
            }
            PropertyValue valueToSet = it->second;
            
            // DEBUG: Log entity reference values being set during OnValidate
            if ((prop.type == PropertyType::Entity || prop.type == PropertyType::ComponentRef || prop.type == PropertyType::ScriptRef) &&
                std::holds_alternative<int>(valueToSet)) {
               int refEntityId = std::get<int>(valueToSet);
               if (refEntityId > 0) {
                  auto* entData = scene.GetEntityData(static_cast<EntityID>(refEntityId));
                  std::string entName = entData ? entData->Name : "Unknown";
                  std::cout << "[OnValidate] Setting " << script.ClassName << "." << prop.name << " = " << refEntityId << " (" << entName << ") on script attached to entity " << entityId << " (" << data->Name << ")" << std::endl;
               }
            }
            
            // Set the field
            if (prop.type == PropertyType::List) {
               std::string listStr = ScriptReflection::PropertyValueToString(valueToSet);
               SetManagedFieldPtr(scriptHandle, prop.name.c_str(), (void*)listStr.c_str());
            } else {
               void* boxed = ScriptReflection::ValueToBox(valueToSet);
               SetManagedFieldPtr(scriptHandle, prop.name.c_str(), boxed);
            }
         }
      }
      
      // Call OnValidate
      g_Script_Invoke(scriptHandle, "OnValidate");
   }
}

void CallOnValidateForAllScripts()
{
   if (!Scene::CurrentScene || !g_Script_Invoke || !SetManagedFieldPtr) {
      return;
   }
   
   std::cout << "[Interop] Calling OnValidate for all scripts after recompile...\n";
   
   // Iterate all entities in the scene
   for (const auto& entity : Scene::CurrentScene->GetEntities()) {
      CallOnValidateForEntity(*Scene::CurrentScene, entity.GetID());
   }
   
   std::cout << "[Interop] OnValidate calls completed.\n";
}

// Call OnValidate for all scripts in a subtree (used for prefab instantiation)
void CallOnValidateForSubtree(Scene& scene, EntityID rootId)
{
   if (!g_Script_Invoke || !SetManagedFieldPtr) return;
   
   auto* rootData = scene.GetEntityData(rootId);
   std::string rootName = rootData ? rootData->Name : "Unknown";
   const auto prefabLabel = cm::debug::DescribePrefabRoot(scene, rootId);
   const auto startTime = std::chrono::high_resolution_clock::now();
   size_t processedEntityCount = 0;
   std::cout << "[CallOnValidateForSubtree] Starting from root entity " << rootId << " (" << rootName << ")" << std::endl;
   
   std::function<void(EntityID)> processEntity = [&](EntityID id) {
      ++processedEntityCount;
      CallOnValidateForEntity(scene, id);
      
      auto* data = scene.GetEntityData(id);
      if (data) {
         for (EntityID child : data->Children) {
            processEntity(child);
         }
      }
   };
   
   processEntity(rootId);
   std::cout << "[CallOnValidateForSubtree] Completed processing subtree from root " << rootId << std::endl;

   const auto endTime = std::chrono::high_resolution_clock::now();
   const double durationMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
   cm::debug::RecordPrefabProfilerSample(prefabLabel, "PrefabSetup/ScriptsOnValidate", durationMs);
   if (durationMs >= 0.25) {
      cm::debug::LogPrefabPerfEvent(
         "ScriptsOnValidate",
         prefabLabel,
         durationMs,
         "entities=" + std::to_string(processedEntityCount));
   }
}

// ----------------------------------------
// C++ Wrapper Utilities
// ----------------------------------------
void* CreateScriptInstance(const std::string& className)
   {
   return g_Script_Create ? g_Script_Create(className.c_str()) : nullptr;
   }


#if defined(_MSC_VER) && !defined(CLAYMORE_FAST_SCRIPT_CALLS)
static bool CallOnCreate_SEH(void* instance, int entityID)
{
    __try {
        g_Script_OnCreate(instance, entityID);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#endif

void CallOnCreate(void* instance, int entityID)
{
    if(!instance)
        return; // Skip if script handle invalid
    
    // Additional safety: check if pointer looks valid (not obviously corrupted)
    if (reinterpret_cast<uintptr_t>(instance) < 0x10000) {
        Logger::LogError("[Interop] CallOnCreate: Invalid instance pointer");
        return;
    }
    
    if (g_Script_OnCreate) {
#if defined(_MSC_VER) && !defined(CLAYMORE_FAST_SCRIPT_CALLS)
        // Debug/Development builds: use SEH to catch crashes gracefully
        if (!CallOnCreate_SEH(instance, entityID)) {
            Logger::LogError("[Interop] Script threw exception during OnCreate (entityID=" + std::to_string(entityID) + ") - check console for details");
        }
#else
        // Release builds with CLAYMORE_FAST_SCRIPT_CALLS: direct call for max performance
        g_Script_OnCreate(instance, entityID);
#endif
    }
}

void CallOnDestroy(void* instance)
{
    if(!instance)
        return;
    // Safety: Don't call into .NET if runtime is shutting down
    if (!IsDotnetRuntimeReady())
        return;
    // Additional safety: check for obviously invalid pointers
    if (reinterpret_cast<uintptr_t>(instance) < 0x10000)
        return;
    if (g_Script_OnDestroy)
        g_Script_OnDestroy(instance);
}

bool IsDotnetRuntimeReady()
{
   return load_assembly_and_get_function_pointer != nullptr;
}

// =========================================================================
// PERF: SEH wrapper for script calls
// SEH (Structured Exception Handling) has ~5-10 cycle overhead per call.
// In Release builds with CLAYMORE_FAST_SCRIPT_CALLS defined, we skip SEH
// for maximum performance. Define this only if you trust your scripts.
// =========================================================================
#if defined(_MSC_VER) && !defined(CLAYMORE_FAST_SCRIPT_CALLS)
static bool CallOnUpdate_SEH(void* instance, float dt)
{
    __try {
        g_Script_OnUpdate(instance, dt);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#endif

void CallOnUpdate(void* instance, float dt)
{
    if(!instance)
        return; // Skip if script handle invalid
    
    // Additional safety: check if pointer looks valid (not obviously corrupted)
    if (reinterpret_cast<uintptr_t>(instance) < 0x10000) {
        Logger::LogError("[Interop] CallOnUpdate: Invalid instance pointer");
        return;
    }

    if (g_Script_OnUpdate) {
#if defined(_MSC_VER) && !defined(CLAYMORE_FAST_SCRIPT_CALLS)
        // Debug/Development builds: use SEH to catch crashes gracefully
        if (!CallOnUpdate_SEH(instance, dt)) {
            Logger::LogError("[Interop] Script threw exception during OnUpdate - check console for details");
        }
#else
        // Release builds with CLAYMORE_FAST_SCRIPT_CALLS: direct call for max performance
        g_Script_OnUpdate(instance, dt);
#endif
    }
}

bool ResolveManagedDelegate(const std::wstring& assemblyPath,
                            const std::wstring& typeName,
                            const std::wstring& methodName,
                            const std::wstring& delegateType,
                            void** outFunction)
{
    if (!load_assembly_and_get_function_pointer) return false;
    void* fn = nullptr;
    int rc = load_assembly_and_get_function_pointer(
        assemblyPath.c_str(),
        typeName.c_str(),
        methodName.c_str(),
        delegateType.c_str(),
        nullptr,
        &fn
    );
    if (rc != 0 || !fn) return false;
    *outFunction = fn;
    return true;
}  

void SetupEntityInterop(std::filesystem::path fullPath)
{
    static bool s_EntityInteropInitialized = false;
    if (s_EntityInteropInitialized)
        return;

    cm::interop::EntityInteropLayout layout = cm::interop::BuildEntityInteropLayout();

        using EntityInteropInitFn = void(*)(void**, int);

        EntityInteropInitFn initInteropFn = nullptr;

        int rc = load_assembly_and_get_function_pointer(
            fullPath.c_str(),
            L"ClaymoreEngine.EntityInterop, ClaymoreEngine",
            L"InitializeInteropExport",
            L"ClaymoreEngine.EntityInteropInitDelegate, ClaymoreEngine",
            nullptr,
            (void**)&initInteropFn
        );


        if (rc != 0)
        {
            std::wcerr << L"[Interop] Failed to get delegate. HRESULT: 0x" << std::hex << rc << std::endl;
        }

        if (rc == 0 && initInteropFn)
        {
            const int totalCount = static_cast<int>(layout.args.size());
            const int expectedTotal = cm::interop::GetExpectedEntityInteropTotal(layout);
            const int expectedMinimum = layout.entityCoreCount + layout.tweenCount + layout.componentCount;
            if (totalCount != expectedTotal) {
                std::wcerr << L"[Interop] initArgs size (" << totalCount
                           << L") does not match expected total (" << expectedTotal << L")." << std::endl;
                return;
            }
            if (totalCount < expectedMinimum) {
                std::wcerr << L"[Interop] initArgs size (" << totalCount
                           << L") is smaller than expected layout minimum (" << expectedMinimum << L")." << std::endl;
                return;
            }
            initInteropFn(layout.args.data(), totalCount);
            s_EntityInteropInitialized = true;
        }
    }

void SetupInputInterop(std::filesystem::path fullPath)
{
    static bool s_InputInteropInitialized = false;
    if(s_InputInteropInitialized)
        return;

    // Ensure reflection setter is ready
    SetupReflectionInterop(fullPath);

    void* initArgs[] = {
        (void*)IsKeyHeldPtr,
        (void*)IsKeyDownPtr,
        (void*)IsMouseDownPtr,
        (void*)IsMouseHeldPtr,
        (void*)GetMouseDeltaPtr,
        (void*)GetScrollDeltaPtr,
        (void*)DebugLogPtr,
        (void*)SetMouseModePtr,
        // Gamepad functions
        (void*)IsGamepadConnectedPtr,
        (void*)IsGamepadButtonHeldPtr,
        (void*)IsGamepadButtonDownPtr,
        (void*)GetGamepadAxisPtr
    };

    using InputInteropInitFn = void(*)(void**, int);
    InputInteropInitFn initInteropFn = nullptr;

    int rc = load_assembly_and_get_function_pointer(
        fullPath.c_str(),
        L"ClaymoreEngine.InputInterop, ClaymoreEngine",
        L"InitializeInteropExport",
        L"ClaymoreEngine.InputInteropInitDelegate, ClaymoreEngine",
        nullptr,
        (void**)&initInteropFn);

    if(rc != 0)
    {
        std::wcerr << L"[Interop] Failed to get InputInterop delegate. HRESULT: 0x" << std::hex << rc << std::endl;
        return;
    }
    if(initInteropFn)
    {
        initInteropFn(initArgs, static_cast<int>(sizeof(initArgs)/sizeof(void*)));
        s_InputInteropInitialized = true;
    }
}

void SetupReflectionInterop(std::filesystem::path fullPath)
{
    if(SetManagedFieldPtr && GetManagedFieldPtr) return;

    // Load SetManagedField
    if (!SetManagedFieldPtr)
    {
        void* fn = nullptr;
        int rc = load_assembly_and_get_function_pointer(
            fullPath.c_str(),
            L"ClaymoreEngine.InteropExports, ClaymoreEngine",
            L"SetManagedField",
            L"ClaymoreEngine.InteropExports+SetFieldDelegate, ClaymoreEngine",
            nullptr,
            &fn); 
        if(rc==0 && fn)
            SetManagedFieldPtr = reinterpret_cast<cm::script::SetManagedField_fn>(fn);
    }

    // Load GetManagedField
    if (!GetManagedFieldPtr)
    {
        void* fn = nullptr;
        int rc = load_assembly_and_get_function_pointer(
            fullPath.c_str(),
            L"ClaymoreEngine.InteropExports, ClaymoreEngine",
            L"GetManagedField",
            L"ClaymoreEngine.InteropExports+GetFieldDelegate, ClaymoreEngine",
            nullptr,
            &fn);
        if(rc==0 && fn)
            GetManagedFieldPtr = reinterpret_cast<bool(*)(void*, const char*, int, void*)>(fn);
    }
}

void SetupResourceInterop(std::filesystem::path fullPath)
{
    static bool s_ResourceInteropInitialized = false;
    if (s_ResourceInteropInitialized) return;
    
    // Pass native resource function pointers to managed side
    // Order must match Resources.Initialize on managed side
    void* initArgs[] = {
        // Basic resource access (5)
        (void*)Resources_GetResourceCountPtr,
        (void*)Resources_GetResourceGUIDsPtr,
        (void*)Resources_GetResourceByNamePtr,
        (void*)Resources_GetResourceNamesPtr,
        (void*)Resources_IsInitializedPtr,
        // Change notification system (7)
        (void*)Resources_SubscribeToTypePtr,
        (void*)Resources_UnsubscribeFromTypePtr,
        (void*)Resources_HasResourcesFolderPtr,
        (void*)Resources_TryDiscoverFolderPtr,
        (void*)Resources_FlushPendingChangesPtr,
        (void*)Resources_GetPendingChangeCountPtr,
        (void*)Resources_GetPendingChangesPtr,
        // Asset loading helpers (3)
        (void*)Resources_GetGuidFromPathPtr,
        (void*)Resources_GetAssetCountByTypePtr,
        (void*)Resources_GetAssetGUIDsByTypePtr
    };
    
    using ResourceInteropInitFn = void(*)(void**, int);
    ResourceInteropInitFn initInteropFn = nullptr;
    
    int rc = load_assembly_and_get_function_pointer(
        fullPath.c_str(),
        L"ClaymoreEngine.Resources, ClaymoreEngine",
        L"Initialize",
        UNMANAGEDCALLERSONLY_METHOD,
        nullptr,
        (void**)&initInteropFn);
    
    if (rc != 0) {
        std::wcerr << L"[Interop] Failed to get ResourceInterop delegate. HRESULT: 0x" << std::hex << rc << std::endl;
        return;
    }
    
    if (initInteropFn) {
        initInteropFn(initArgs, static_cast<int>(sizeof(initArgs)/sizeof(void*)));
        s_ResourceInteropInitialized = true;
        std::cout << "[Interop] ResourceInterop initialized with " << (sizeof(initArgs)/sizeof(void*)) << " function pointers" << std::endl;
    }
}


