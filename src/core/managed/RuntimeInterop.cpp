// ============================================================================
// RuntimeInterop.cpp - Complete interop setup for exported games
// ============================================================================
// This file mirrors the interop setup from DotNetHost.cpp for runtime builds.
// It sets up all the function pointers that managed code needs to interact
// with native systems (entities, physics, input, etc.)
// ============================================================================

#include "RuntimeHost.h"
#include "EntityInteropLayout.h"
#include "ScriptInterop.h"
#include "ScriptSystem.h"
#include "ScriptReflection.h"

// Include DotNetHost.h which has all the correct function pointer typedefs and extern declarations
#include "managed/interop/DotNetHost.h"
#include "managed/interop/InputInterop.h"
#include "managed/interop/PhysicsInterop.h"
#include "managed/interop/ComponentInterop.h"
#include "managed/interop/ModuleComponentInterop.h"

#include "core/physics/Physics.h"
#include "core/navigation/NavInterop.h"
#include "core/world/PortalInterop.h"
#include "core/world/WorldGraphInterop.h"
#include "core/animation/ik/IKInterop.h"
#include "core/animation/lookat/LookAtInterop.h"
#include "managed/interop/ScriptableInterop.h"
#include "managed/interop/ResourceInterop.h"
#include "managed/interop/DialogueInterop.h"
#include "managed/interop/AnimationEventInterop.h"
#include "core/multiplayer/NetworkingInterop.h"

#include <iostream>
#include <filesystem>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// hostfxr for load_assembly_and_get_function_pointer
extern "C" {
#include "nethost.h"
}
#include "coreclr_delegates.h"
#include "hostfxr.h"

// ============================================================================
// All function pointer declarations come from the included headers:
// - DotNetHost.h (includes EntityInterop.h, ComponentInterop.h, TweenInterop.h)
// - InputInterop.h
// - PhysicsInterop.h
// - NavInterop.h, IKInterop.h, LookAtInterop.h
// ============================================================================

// Area interop functions (defined in AreaInterop.cpp but not in header)
extern "C" void* Get_Area_SetOnBodyEntered_Ptr();
extern "C" void* Get_Area_SetOnBodyExited_Ptr();
extern "C" void* Get_Area_SetOnAreaEntered_Ptr();
extern "C" void* Get_Area_SetOnAreaExited_Ptr();

// Collision interop functions (defined in CollisionInterop.cpp)
extern "C" void* Get_Collision_SetOnEnter_Ptr();
extern "C" void* Get_Collision_SetOnExit_Ptr();

// Ragdoll interop functions (defined in RagdollInterop.cpp)
extern "C" void* Get_Ragdoll_Create_Ptr();
extern "C" void* Get_Ragdoll_Destroy_Ptr();
extern "C" void* Get_Ragdoll_Has_Ptr();
extern "C" void* Get_Ragdoll_Activate_Ptr();
extern "C" void* Get_Ragdoll_Deactivate_Ptr();
extern "C" void* Get_Ragdoll_ApplyImpulse_Ptr();
extern "C" void* Get_Ragdoll_ApplyImpulseToAll_Ptr();
extern "C" void* Get_Ragdoll_SetPhysicsLayer_Ptr();
extern "C" void* Get_Ragdoll_GetOwnerFromBone_Ptr();

// Prefab interop functions (defined in PrefabInterop.cpp)
extern "C" void* Get_Prefab_InstantiateByGuid_Ptr();
extern "C" void* Get_Prefab_InstantiateByGuidBlocking_Ptr();
extern "C" void* Get_Prefab_InstantiateByGuidWithRoot_Ptr();
extern "C" void* Get_Prefab_GetAsyncStatus_Ptr();
extern "C" void* Get_Prefab_GetAssetNameByGuid_Ptr();

// Mesh interop functions (defined in MeshInterop.cpp)
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

// Audio interop functions (defined in AudioInterop.cpp)
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

// Material interop functions (defined in ComponentInterop.cpp)
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

// Particle interop functions (defined in ComponentInterop.cpp) - 52 functions
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

// Dialogue interop functions (defined in DialogueInterop.cpp)
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

// ============================================================================
// Runtime Interop Namespace
// ============================================================================
namespace cm {
namespace runtime {

// ============================================================================
// Interop Setup Functions
// ============================================================================

static load_assembly_and_get_function_pointer_fn s_loader = nullptr;
static std::wstring s_engineDllPath;

void SetupRuntimeInteropLoader(load_assembly_and_get_function_pointer_fn loader, const std::wstring& engineDll) {
    s_loader = loader;
    s_engineDllPath = engineDll;
}

bool SetupEntityInterop() {
    if (!s_loader) {
        std::cerr << "[RuntimeInterop] Loader not set!\n";
        return false;
    }
    
    std::cout << "[RuntimeInterop] Setting up entity interop...\n";
    
    cm::interop::EntityInteropLayout layout = cm::interop::BuildEntityInteropLayout();

    using EntityInteropInitFn = void(*)(void**, int);
    EntityInteropInitFn initInteropFn = nullptr;

    int rc = s_loader(
        s_engineDllPath.c_str(),
        L"ClaymoreEngine.EntityInterop, ClaymoreEngine",
        L"InitializeInteropExport",
        L"ClaymoreEngine.EntityInteropInitDelegate, ClaymoreEngine",
        nullptr,
        (void**)&initInteropFn
    );

    if (rc != 0) {
        std::cerr << "[RuntimeInterop] Failed to get EntityInterop delegate. HRESULT: 0x" << std::hex << rc << std::endl;
        return false;
    }

    if (initInteropFn) {
        const int totalCount = static_cast<int>(layout.args.size());
        const int expectedTotal = cm::interop::GetExpectedEntityInteropTotal(layout);
        const int expectedMinimum = layout.entityCoreCount + layout.tweenCount + layout.componentCount;
        if (totalCount != expectedTotal) {
            std::cerr << "[RuntimeInterop] initArgs size (" << totalCount
                      << ") does not match expected total (" << expectedTotal << ").\n";
            return false;
        }
        if (totalCount < expectedMinimum) {
            std::cerr << "[RuntimeInterop] initArgs size (" << totalCount
                      << ") is smaller than expected layout minimum (" << expectedMinimum << ").\n";
            return false;
        }
        initInteropFn(layout.args.data(), totalCount);
        std::cout << "[RuntimeInterop] Entity interop initialized (" << totalCount << " functions)\n";
        return true;
    }
    
    return false;
}

bool SetupInputInterop() {
    if (!s_loader) return false;
    
    std::cout << "[RuntimeInterop] Setting up input interop...\n";

    void* initArgs[] = {
        (void*)::IsKeyHeldPtr,
        (void*)::IsKeyDownPtr,
        (void*)::IsMouseDownPtr,
        (void*)::IsMouseHeldPtr,
        (void*)::GetMouseDeltaPtr,
        (void*)::GetScrollDeltaPtr,
        (void*)::DebugLogPtr,
        (void*)::SetMouseModePtr,
        (void*)::IsGamepadConnectedPtr,
        (void*)::IsGamepadButtonHeldPtr,
        (void*)::IsGamepadButtonDownPtr,
        (void*)::GetGamepadAxisPtr
    };

    using InputInteropInitFn = void(*)(void**, int);
    InputInteropInitFn initInteropFn = nullptr;

    int rc = s_loader(
        s_engineDllPath.c_str(),
        L"ClaymoreEngine.InputInterop, ClaymoreEngine",
        L"InitializeInteropExport",
        L"ClaymoreEngine.InputInteropInitDelegate, ClaymoreEngine",
        nullptr,
        (void**)&initInteropFn
    );

    if (rc != 0) {
        std::cerr << "[RuntimeInterop] Failed to get InputInterop delegate. HRESULT: 0x" << std::hex << rc << std::endl;
        return false;
    }

    if (initInteropFn) {
        initInteropFn(initArgs, static_cast<int>(sizeof(initArgs) / sizeof(void*)));
        std::cout << "[RuntimeInterop] Input interop initialized\n";
        return true;
    }
    
    return false;
}

bool SetupNetworkingInterop() {
    if (!s_loader) {
        return false;
    }

    std::cout << "[RuntimeInterop] Setting up networking interop...\n";

    void* initArgs[] = {
        (void*)::Networking_GetEntityGuidPtr,
        (void*)::Networking_FindEntityByGuidPtr
    };

    using NetworkingInteropInitFn = void(*)(void**, int);
    NetworkingInteropInitFn initInteropFn = nullptr;

    int rc = s_loader(
        s_engineDllPath.c_str(),
        L"ClaymoreEngine.Networking.NetworkingInterop, ClaymoreEngine",
        L"InitializeInteropExport",
        L"ClaymoreEngine.Networking.NetworkingInteropInitDelegate, ClaymoreEngine",
        nullptr,
        (void**)&initInteropFn
    );

    if (rc != 0) {
        std::cerr << "[RuntimeInterop] Networking interop not available (rc=0x"
                  << std::hex << rc << std::dec << ")\n";
        return false;
    }

    if (initInteropFn) {
        initInteropFn(initArgs, static_cast<int>(sizeof(initArgs) / sizeof(void*)));
        std::cout << "[RuntimeInterop] Networking interop initialized\n";
        return true;
    }

    return false;
}

bool SetupPhysicsInterop() {
    if (!s_loader) return false;
    
    std::cout << "[RuntimeInterop] Setting up physics interop...\n";

    void* initArgs[] = {
        (void*)::Physics_SetGravityPtr,
        (void*)::Physics_GetGravityPtr,
        (void*)::Physics_RaycastPtr,
        (void*)::Physics_RaycastPointsPtr,
        (void*)::Physics_RegisterLayerPtr,
        (void*)::Physics_GetLayerIndexPtr,
        (void*)::Physics_GetLayerMaskPtr,
        (void*)::Physics_GetLayerCountPtr
    };

    using PhysicsInteropInitFn = void(*)(void**, int);
    PhysicsInteropInitFn initInteropFn = nullptr;

    int rc = s_loader(
        s_engineDllPath.c_str(),
        L"ClaymoreEngine.Physics.Physics, ClaymoreEngine",
        L"InitializeInterop",
        L"ClaymoreEngine.Physics.PhysicsInteropInitDelegate, ClaymoreEngine",
        nullptr,
        (void**)&initInteropFn
    );

    if (rc == 0 && initInteropFn) {
        initInteropFn(initArgs, static_cast<int>(sizeof(initArgs) / sizeof(void*)));
        std::cout << "[RuntimeInterop] Physics interop initialized\n";
        return true;
    }
    
    // Physics interop is optional - some games may not use it
    std::cerr << "[RuntimeInterop] Physics interop not available (rc=0x" << std::hex << rc << ")\n";
    return false;
}

bool SetupAreaInterop() {
    if (!s_loader) return false;
    
    std::cout << "[RuntimeInterop] Setting up area interop...\n";

    void* args[4];
    args[0] = (void*)Get_Area_SetOnBodyEntered_Ptr();
    args[1] = (void*)Get_Area_SetOnBodyExited_Ptr();
    args[2] = (void*)Get_Area_SetOnAreaEntered_Ptr();
    args[3] = (void*)Get_Area_SetOnAreaExited_Ptr();

    using AreaInteropInitFn = void(*)(void**, int);
    AreaInteropInitFn initAreaFn = nullptr;
    
    int rc = s_loader(
        s_engineDllPath.c_str(),
        L"ClaymoreEngine.AreaInterop, ClaymoreEngine",
        L"InitializeInteropExport",
        L"ClaymoreEngine.AreaInteropInitDelegate, ClaymoreEngine",
        nullptr,
        (void**)&initAreaFn
    );
    
    if (rc == 0 && initAreaFn) {
        initAreaFn(args, 4);
        std::cout << "[RuntimeInterop] Area interop initialized\n";
        return true;
    }
    
    return false;
}

bool SetupPortalInterop() {
    if (!s_loader) return false;
    
    std::cout << "[RuntimeInterop] Setting up portal interop...\n";

    void* args[1];
    args[0] = (void*)Get_Portal_SetOnCrossed_Ptr();

    using PortalInteropInitFn = void(*)(void**, int);
    PortalInteropInitFn initPortalFn = nullptr;
    
    int rc = s_loader(
        s_engineDllPath.c_str(),
        L"ClaymoreEngine.PortalInterop, ClaymoreEngine",
        L"InitializeInteropExport",
        L"ClaymoreEngine.PortalInteropInitDelegate, ClaymoreEngine",
        nullptr,
        (void**)&initPortalFn
    );
    
    if (rc == 0 && initPortalFn) {
        initPortalFn(args, 1);
        std::cout << "[RuntimeInterop] Portal interop initialized\n";
        return true;
    }
    
    return false;
}

bool SetupCollisionInterop() {
    if (!s_loader) return false;
    
    std::cout << "[RuntimeInterop] Setting up collision interop...\n";

    void* args[2];
    args[0] = (void*)Get_Collision_SetOnEnter_Ptr();
    args[1] = (void*)Get_Collision_SetOnExit_Ptr();

    using CollisionInteropInitFn = void(*)(void**, int);
    CollisionInteropInitFn initCollisionFn = nullptr;
    
    int rc = s_loader(
        s_engineDllPath.c_str(),
        L"ClaymoreEngine.CollisionInterop, ClaymoreEngine",
        L"InitializeInteropExport",
        L"ClaymoreEngine.CollisionInteropInitDelegate, ClaymoreEngine",
        nullptr,
        (void**)&initCollisionFn
    );
    
    if (rc == 0 && initCollisionFn) {
        initCollisionFn(args, 2);
        std::cout << "[RuntimeInterop] Collision interop initialized\n";
        return true;
    }
    
    return false;
}

bool SetupNavigationInterop() {
    if (!s_loader) return false;
    
    std::cout << "[RuntimeInterop] Setting up navigation interop...\n";

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
    
    int rc = s_loader(
        s_engineDllPath.c_str(),
        L"ClaymoreEngine.NavigationInterop, ClaymoreEngine",
        L"InitializeInteropExport",
        L"ClaymoreEngine.NavigationInteropInitDelegate, ClaymoreEngine",
        nullptr,
        (void**)&initNavFn
    );
    
    if (rc == 0 && initNavFn) {
        initNavFn(navArgs, 22);
        std::cout << "[RuntimeInterop] Navigation interop initialized\n";
        return true;
    }
    
    return false;
}

bool SetupWorldGraphInterop() {
    if (!s_loader) return false;
    
    std::cout << "[RuntimeInterop] Setting up world graph interop...\n";

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
    
    int rc = s_loader(
        s_engineDllPath.c_str(),
        L"ClaymoreEngine.WorldGraphInterop, ClaymoreEngine",
        L"InitializeInteropExport",
        L"ClaymoreEngine.WorldGraphInteropInitDelegate, ClaymoreEngine",
        nullptr,
        (void**)&initWorldGraphFn
    );
    
    if (rc == 0 && initWorldGraphFn) {
        initWorldGraphFn(args, 27);
        std::cout << "[RuntimeInterop] World graph interop initialized\n";
        return true;
    }
    
    return false;
}

bool SetupIKInterop() {
    if (!s_loader) return false;
    
    std::cout << "[RuntimeInterop] Setting up IK interop...\n";

    void* ikArgs[5];
    ikArgs[0] = (void*)Get_IK_SetWeight_Ptr();
    ikArgs[1] = (void*)Get_IK_SetTarget_Ptr();
    ikArgs[2] = (void*)Get_IK_SetPole_Ptr();
    ikArgs[3] = (void*)Get_IK_SetChain_Ptr();
    ikArgs[4] = (void*)Get_IK_GetErrorMeters_Ptr();

    using IKInteropInitFn = void(*)(void**, int);
    IKInteropInitFn initIkFn = nullptr;
    
    int rc = s_loader(
        s_engineDllPath.c_str(),
        L"ClaymoreEngine.IKInterop, ClaymoreEngine",
        L"InitializeInteropExport",
        L"ClaymoreEngine.IKInteropInitDelegate, ClaymoreEngine",
        nullptr,
        (void**)&initIkFn
    );
    
    if (rc == 0 && initIkFn) {
        initIkFn(ikArgs, 5);
        std::cout << "[RuntimeInterop] IK interop initialized\n";
        return true;
    }
    
    return false;
}

bool SetupLookAtInterop() {
    if (!s_loader) return false;
    
    std::cout << "[RuntimeInterop] Setting up LookAt interop...\n";

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
    
    int rc = s_loader(
        s_engineDllPath.c_str(),
        L"ClaymoreEngine.LookAtInterop, ClaymoreEngine",
        L"InitializeInteropExport",
        L"ClaymoreEngine.LookAtInteropInitDelegate, ClaymoreEngine",
        nullptr,
        (void**)&initLookAtFn
    );
    
    if (rc == 0 && initLookAtFn) {
        initLookAtFn(lookAtArgs, 11);
        std::cout << "[RuntimeInterop] LookAt interop initialized\n";
        return true;
    }
    
    return false;
}

bool SetupRagdollInterop() {
    if (!s_loader) return false;
    
    std::cout << "[RuntimeInterop] Setting up Ragdoll interop...\n";

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
    
    int rc = s_loader(
        s_engineDllPath.c_str(),
        L"ClaymoreEngine.Physics.Ragdoll, ClaymoreEngine",
        L"Initialize",
        L"ClaymoreEngine.Physics.RagdollInteropInitDelegate, ClaymoreEngine",
        nullptr,
        (void**)&initRagdollFn
    );
    
    if (rc == 0 && initRagdollFn) {
        initRagdollFn(ragdollArgs, 9);
        std::cout << "[RuntimeInterop] Ragdoll interop initialized\n";
        return true;
    }
    
    return false;
}

bool SetupScriptableInterop() {
    if (!s_loader) return false;
    
    std::cout << "[RuntimeInterop] Setting up Scriptable interop...\n";
    
    // Get the GetScriptableAPI export from managed code
    GetManagedScriptableAPI getApi = nullptr;
    
    int rc = s_loader(
        s_engineDllPath.c_str(),
        L"ClaymoreEngine.ScriptableInteropExports, ClaymoreEngine",
        L"GetScriptableAPI",
        L"ClaymoreEngine.ScriptableInteropExports+GetManagedScriptableAPIDelegate, ClaymoreEngine",
        nullptr,
        (void**)&getApi
    );
    
    if (rc == 0 && getApi) {
        // Fill the native API struct with function pointers
        NativeScriptableAPI native{};
        native.user = nullptr;
        native.RegisterType = &Scriptable_NativeRegisterType;
        native.SetField = &Scriptable_NativeSetField;
        native.GetField = &Scriptable_NativeGetField;
        native.MarkDirty = &Scriptable_NativeMarkDirty;
        native.GetPathForGUID = &Scriptable_GetPathForGUID;
        native.IsTypeAssignable = &Scriptable_IsTypeAssignable;
        native.ReadFileContents = &Scriptable_ReadFileContents;
        native.InvalidateCache = &Scriptable_InvalidateCache;
        
        ManagedScriptableAPI managed{};
        getApi(&native, &managed);
        Scriptable_SetManagedAPI(managed);
        if (managed.EnumerateTypes) {
            managed.EnumerateTypes(native.user);
        }
        
        std::cout << "[RuntimeInterop] Scriptable interop initialized\n";
        return true;
    }
    
    std::cerr << "[RuntimeInterop] Scriptable interop not available (rc=0x" << std::hex << rc << std::dec << ")\n";
    return false;
}

bool SetupClayObjectCacheInterop() {
    if (!s_loader) return false;

    std::cout << "[RuntimeInterop] Setting up ClayObject cache interop...\n";

    void* fn = nullptr;
    int rc = s_loader(
        s_engineDllPath.c_str(),
        L"ClaymoreEngine.ClayObjectCacheExports, ClaymoreEngine",
        L"InvalidateCache",
        L"ClaymoreEngine.InvalidateClayObjectCacheDelegate, ClaymoreEngine",
        nullptr,
        &fn
    );

    if (rc == 0 && fn) {
        g_InvalidateCache = reinterpret_cast<InvalidateCacheFn>(fn);
        std::cout << "[RuntimeInterop] ClayObject cache interop initialized\n";
        return true;
    }

    std::cerr << "[RuntimeInterop] ClayObject cache interop not available (rc=0x" << std::hex << rc << std::dec << ")\n";
    return false;
}

bool SetupPrefabInterop() {
    if (!s_loader) return false;
    
    std::cout << "[RuntimeInterop] Setting up Prefab interop...\n";
    
    // CRITICAL: Must match editor's SetupPrefabInterop - 5 functions
    void* args[5];
    args[0] = (void*)Get_Prefab_InstantiateByGuid_Ptr();
    args[1] = (void*)Get_Prefab_InstantiateByGuidBlocking_Ptr();
    args[2] = (void*)Get_Prefab_GetAsyncStatus_Ptr();
    args[3] = (void*)Get_Prefab_GetAssetNameByGuid_Ptr();
    args[4] = (void*)Get_Prefab_InstantiateByGuidWithRoot_Ptr();
    
    using PrefabInteropInitFn = void(*)(void**, int);
    PrefabInteropInitFn initPrefabFn = nullptr;
    
    int rc = s_loader(
        s_engineDllPath.c_str(),
        L"ClaymoreEngine.PrefabInterop, ClaymoreEngine",
        L"InitializeInteropExport",
        L"ClaymoreEngine.PrefabInteropInitDelegate, ClaymoreEngine",
        nullptr,
        (void**)&initPrefabFn
    );
    
    if (rc == 0 && initPrefabFn) {
        initPrefabFn(args, 5);
        std::cout << "[RuntimeInterop] Prefab interop initialized (5 functions)\n";
        return true;
    }
    
    std::cerr << "[RuntimeInterop] Prefab interop not available (rc=0x" << std::hex << rc << std::dec << ")\n";
    return false;
}

bool SetupMeshInterop() {
    if (!s_loader) return false;
    
    std::cout << "[RuntimeInterop] Setting up Mesh interop...\n";
    
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
        std::cerr << "[RuntimeInterop] Mesh interop count mismatch. Expected "
                  << kExpectedMeshInteropCount << " functions but prepared " << i << ".\n";
        return false;
    }
    
    using MeshInteropInitFn = void(*)(void**, int);
    MeshInteropInitFn initMeshFn = nullptr;
    
    int rc = s_loader(
        s_engineDllPath.c_str(),
        L"ClaymoreEngine.MeshInterop, ClaymoreEngine",
        L"InitializeInteropExport",
        L"ClaymoreEngine.MeshInteropInitDelegate, ClaymoreEngine",
        nullptr,
        (void**)&initMeshFn
    );
    
    if (rc == 0 && initMeshFn) {
        initMeshFn(args, kExpectedMeshInteropCount);
        std::cout << "[RuntimeInterop] Mesh interop initialized (" << kExpectedMeshInteropCount << " functions)\n";
        return true;
    }
    
    std::cerr << "[RuntimeInterop] Mesh interop not available (rc=0x" << std::hex << rc << std::dec << ")\n";
    return false;
}

bool SetupAudioInterop() {
    if (!s_loader) return false;

    std::cout << "[RuntimeInterop] Setting up Audio interop...\n";

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
        std::cerr << "[RuntimeInterop] Audio interop count mismatch. Expected "
                  << kExpectedAudioInteropCount << " functions but prepared " << i << ".\n";
        return false;
    }

    using AudioInteropInitFn = void(*)(void**, int);
    AudioInteropInitFn initAudioFn = nullptr;

    int rc = s_loader(
        s_engineDllPath.c_str(),
        L"ClaymoreEngine.AudioInterop, ClaymoreEngine",
        L"InitializeInteropExport",
        L"ClaymoreEngine.AudioInteropInitDelegate, ClaymoreEngine",
        nullptr,
        (void**)&initAudioFn
    );

    if (rc == 0 && initAudioFn) {
        initAudioFn(args, kExpectedAudioInteropCount);
        std::cout << "[RuntimeInterop] Audio interop initialized (" << kExpectedAudioInteropCount << " functions)\n";
        return true;
    }

    std::cerr << "[RuntimeInterop] Audio interop not available (rc=0x" << std::hex << rc << std::dec << ")\n";
    return false;
}

bool SetupMaterialInterop() {
    if (!s_loader) return false;
    
    std::cout << "[RuntimeInterop] Setting up Material interop...\n";
    
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
    
    int rc = s_loader(
        s_engineDllPath.c_str(),
        L"ClaymoreEngine.MaterialInterop, ClaymoreEngine",
        L"Initialize",
        L"ClaymoreEngine.MaterialInteropInitDelegate, ClaymoreEngine",
        nullptr,
        (void**)&initMaterialFn
    );
    
    if (rc == 0 && initMaterialFn) {
        initMaterialFn(args, i);
        std::cout << "[RuntimeInterop] Material interop initialized (" << i << " functions)\n";
        return true;
    }
    
    std::cerr << "[RuntimeInterop] Material interop not available (rc=0x" << std::hex << rc << std::dec << ")\n";
    return false;
}

bool SetupParticleInterop() {
    if (!s_loader) return false;
    
    std::cout << "[RuntimeInterop] Setting up Particle interop...\n";
    
    void* args[52]; // Buffer for 52 pointers
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
    
    int rc = s_loader(
        s_engineDllPath.c_str(),
        L"ClaymoreEngine.ParticleInterop, ClaymoreEngine",
        L"Initialize",
        L"ClaymoreEngine.ParticleInteropInitDelegate, ClaymoreEngine",
        nullptr,
        (void**)&initParticleFn
    );
    
    if (rc == 0 && initParticleFn) {
        initParticleFn(args, i);
        std::cout << "[RuntimeInterop] Particle interop initialized (" << i << " functions)\n";
        return true;
    }
    
    std::cerr << "[RuntimeInterop] Particle interop not available (rc=0x" << std::hex << rc << std::dec << ")\n";
    return false;
}

bool SetupResourceInterop() {
    if (!s_loader) return false;
    
    std::cout << "[RuntimeInterop] Setting up Resource interop...\n";
    
    // Pass native resource function pointers to managed side
    // Order must match Resources.Initialize on managed side (15 functions)
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
    
    int rc = s_loader(
        s_engineDllPath.c_str(),
        L"ClaymoreEngine.Resources, ClaymoreEngine",
        L"Initialize",
        UNMANAGEDCALLERSONLY_METHOD,
        nullptr,
        (void**)&initInteropFn
    );
    
    if (rc == 0 && initInteropFn) {
        initInteropFn(initArgs, static_cast<int>(sizeof(initArgs) / sizeof(void*)));
        std::cout << "[RuntimeInterop] Resource interop initialized (15 functions)\n";
        return true;
    }
    
    std::cerr << "[RuntimeInterop] Resource interop not available (rc=0x" << std::hex << rc << std::dec << ")\n";
    return false;
}

bool SetupDialogueInterop() {
    if (!s_loader) return false;
    
    std::cout << "[RuntimeInterop] Setting up Dialogue interop...\n";
    
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

    int rc = s_loader(
        s_engineDllPath.c_str(),
        L"ClaymoreEngine.DialogueInterop, ClaymoreEngine",
        L"InitializeInteropExport",
        L"ClaymoreEngine.DialogueInteropInitDelegate, ClaymoreEngine",
        nullptr,
        (void**)&initDialogueFn
    );
    
    if (rc == 0 && initDialogueFn) {
        initDialogueFn(args, i);
        std::cout << "[RuntimeInterop] Dialogue interop initialized (" << i << " functions)\n";
        return true;
    }
    
    std::cerr << "[RuntimeInterop] Dialogue interop not available (rc=0x" << std::hex << rc << std::dec << ")\n";
    return false;
}

bool SetupAnimationEventInterop() {
    if (!s_loader) return false;
    
    std::cout << "[RuntimeInterop] Setting up Animation Event interop...\n";
    
    void* animEventArgs[4];
    animEventArgs[0] = (void*)Get_AnimEvent_SetCallback_Ptr();
    animEventArgs[1] = (void*)Get_AnimEvent_SetEntityCallback_Ptr();
    animEventArgs[2] = (void*)Get_AnimEvent_ClearEntityCallback_Ptr();
    animEventArgs[3] = (void*)Get_AnimEvent_ClearAllCallbacks_Ptr();
    
    using AnimEventInteropInitFn = void(*)(void**, int);
    AnimEventInteropInitFn initAnimEventFn = nullptr;
    
    int rc = s_loader(
        s_engineDllPath.c_str(),
        L"ClaymoreEngine.AnimationEventInterop, ClaymoreEngine",
        L"Initialize",
        L"ClaymoreEngine.AnimationEventInteropInitDelegate, ClaymoreEngine",
        nullptr,
        (void**)&initAnimEventFn
    );
    
    if (rc == 0 && initAnimEventFn) {
        initAnimEventFn(animEventArgs, 4);
        std::cout << "[RuntimeInterop] Animation Event interop initialized\n";
        return true;
    }
    
    std::cerr << "[RuntimeInterop] Animation Event interop not available (rc=0x" << std::hex << rc << std::dec << ")\n";
    return false;
}

bool SetupQuestInterop() {
    if (!s_loader) return false;
    
    std::cout << "[RuntimeInterop] Setting up Quest interop...\n";
    
    void* args[48];
    int i = 0;
    
    // Core functions (order must match managed side - 33 total)
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
    
    int rc = s_loader(
        s_engineDllPath.c_str(),
        L"ClaymoreEngine.QuestInterop, ClaymoreEngine",
        L"InitializeInteropExport",
        L"ClaymoreEngine.QuestInteropInitDelegate, ClaymoreEngine",
        nullptr,
        (void**)&initQuestFn
    );
    
    if (rc == 0 && initQuestFn) {
        initQuestFn(args, i);
        std::cout << "[RuntimeInterop] Quest interop initialized (" << i << " functions)\n";
        return true;
    }
    
    std::cerr << "[RuntimeInterop] Quest interop not available (rc=0x" << std::hex << rc << std::dec << ")\n";
    return false;
}

// ============================================================================
// Master Interop Setup - Call this after .NET runtime is initialized
// ============================================================================
bool SetupAllInterop(load_assembly_and_get_function_pointer_fn loader, const std::wstring& engineDll) {
    SetupRuntimeInteropLoader(loader, engineDll);
    
    std::cout << "[RuntimeInterop] ============================================\n";
    std::cout << "[RuntimeInterop] Setting up all interop systems\n";
    std::cout << "[RuntimeInterop] ============================================\n";
    
    bool success = true;
    
    // Entity interop is critical - fail if it doesn't work
    if (!SetupEntityInterop()) {
        std::cerr << "[RuntimeInterop] CRITICAL: Entity interop failed!\n";
        success = false;
    }
    
    // Input interop is critical for gameplay
    if (!SetupInputInterop()) {
        std::cerr << "[RuntimeInterop] CRITICAL: Input interop failed!\n";
        success = false;
    }

    SetupNetworkingInterop();
    
    // Physics interop - important but not always required
    SetupPhysicsInterop();
    
    // Resource - for [PopulateFromResources] and Resources API
    SetupResourceInterop();
    
    // Area interop - for trigger volumes
    SetupAreaInterop();

    // Portal interop - for portal crossing callbacks
    SetupPortalInterop();
    
    // Collision interop - rigidbody collision callbacks
    SetupCollisionInterop();
    
    // Ragdoll - for physics ragdolls
    SetupRagdollInterop();
    
    // Scriptable - for ClayScriptableObject and Resources API (VFS support)
    SetupScriptableInterop();
    
    // ClayObject cache interop - for Scriptable cache invalidation
    SetupClayObjectCacheInterop();
    
    // Prefab - for runtime prefab instantiation
    SetupPrefabInterop();
    
    // Mesh - for runtime mesh instantiation and access
    SetupMeshInterop();

    // Audio - for runtime audio asset references and source control
    SetupAudioInterop();
    
    // Material - for runtime material property manipulation
    SetupMaterialInterop();
    
    // Particle - for runtime particle emitter control
    SetupParticleInterop();
    
    // Dialogue - for NPC conversations and dialogue system
    SetupDialogueInterop();
    
    // Quest - for quest/objective tracking system
    SetupQuestInterop();
    
    // Navigation - for AI pathfinding
    SetupNavigationInterop();

    // World graph - for cross-scene traversal queries
    SetupWorldGraphInterop();
    
    // IK - for procedural animation
    SetupIKInterop();
    
    // LookAt - for aim/look constraints
    SetupLookAtInterop();
    
    // Animation Events - for animation event callbacks
    SetupAnimationEventInterop();
    
    std::cout << "[RuntimeInterop] ============================================\n";
    std::cout << "[RuntimeInterop] Interop setup " << (success ? "complete" : "FAILED") << "\n";
    std::cout << "[RuntimeInterop] ============================================\n";
    
    return success;
}

} // namespace runtime
} // namespace cm

