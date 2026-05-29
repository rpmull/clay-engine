using System;
using System.Numerics;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    // These delegates must match the C++ function pointer types in DotNetHost.h
    
    // Managed Logging (level: 0=Info, 1=Warning, 2=Error)
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void ManagedLogFn(int level, [MarshalAs(UnmanagedType.LPStr)] string message);

    // Component Lifetime
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate bool HasComponentFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string componentName);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void AddComponentFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string componentName);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void RemoveComponentFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string componentName);

    // LightComponent
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int GetLightTypeFn(int entityId);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void SetLightTypeFn(int entityId, int type);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void GetLightColorFn(int entityId, out float r, out float g, out float b);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void SetLightColorFn(int entityId, float r, float g, float b);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate float GetLightIntensityFn(int entityId);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void SetLightIntensityFn(int entityId, float intensity);

    // RigidBodyComponent
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate float GetRigidBodyMassFn(int entityId);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void SetRigidBodyMassFn(int entityId, float mass);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate bool GetRigidBodyIsKinematicFn(int entityId);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void SetRigidBodyIsKinematicFn(int entityId, bool isKinematic);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate bool GetRigidBodyUseGravityFn(int entityId);


    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void SetRigidBodyUseGravityFn(int entityId, bool useGravity);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate uint GetRigidBodyCollisionMaskFn(int entityId);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void SetRigidBodyCollisionMaskFn(int entityId, uint collisionMask);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void GetRigidBodyLinearVelocityFn(int entityId, out float x, out float y, out float z);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void SetRigidBodyLinearVelocityFn(int entityId, float x, float y, float z);
    
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void GetRigidBodyAngularVelocityFn(int entityId, out float x, out float y, out float z);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void SetRigidBodyAngularVelocityFn(int entityId, float x, float y, float z);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void ApplyRigidBodyForceFn(int entityId, float x, float y, float z);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void ApplyRigidBodyTorqueFn(int entityId, float x, float y, float z);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void ApplyRigidBodyImpulseFn(int entityId, float x, float y, float z);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void ApplyRigidBodyAngularImpulseFn(int entityId, float x, float y, float z);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate IntPtr RigidBody_GetDebugSummaryFn(int entityId);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void Collider_GetOffsetFn(int entityId, out float x, out float y, out float z);

    // CameraComponent
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate uint GetCameraLayerMaskFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetCameraLayerMaskFn(int entityId, uint mask);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void Camera_SetLayerMaskByNameFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName, [MarshalAs(UnmanagedType.I1)] bool enable);
    // Camera settings
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool GetCameraActiveFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetCameraActiveFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool active);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetCameraPriorityFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetCameraPriorityFn(int entityId, int priority);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float GetCameraFieldOfViewFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetCameraFieldOfViewFn(int entityId, float fov);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float GetCameraNearClipFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetCameraNearClipFn(int entityId, float nearClip);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float GetCameraFarClipFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetCameraFarClipFn(int entityId, float farClip);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool GetCameraIsPerspectiveFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetCameraIsPerspectiveFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool isPerspective);

    // BlendShapeComponent
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void SetBlendShapeWeightFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string shapeName, float weight);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate float GetBlendShapeWeightFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string shapeName);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate int GetBlendShapeCountFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate IntPtr GetBlendShapeNameFn(int entityId, int index);

    // UnifiedMorphComponent
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int UnifiedMorph_GetCountFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr UnifiedMorph_GetNameFn(int entityId, int index);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float UnifiedMorph_GetWeightFn(int entityId, int index);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UnifiedMorph_SetWeightFn(int entityId, int index, float weight);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UnifiedMorph_PropagateAllFn(int entityId);

    // TintMaskController
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool TintController_HasComponentFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr TintController_GetNamePatternFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TintController_SetNamePatternFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string pattern);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TintController_GetBaseTintFn(int entityId, out float r, out float g, out float b, out float a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TintController_SetBaseTintFn(int entityId, float r, float g, float b, float a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TintController_GetTintColorFn(int entityId, int channel, out float r, out float g, out float b, out float a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TintController_SetTintColorFn(int entityId, int channel, float r, float g, float b, float a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool TintController_GetUseTintMaskFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TintController_SetUseTintMaskFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool use);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool TintController_GetUsePbrOverridesFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TintController_SetUsePbrOverridesFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool use);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float TintController_GetPbrMetallicFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TintController_SetPbrMetallicFn(int entityId, float value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float TintController_GetPbrRoughnessFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TintController_SetPbrRoughnessFn(int entityId, float value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TintController_GetPbrEmissionColorFn(int entityId, out float r, out float g, out float b);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TintController_SetPbrEmissionColorFn(int entityId, float r, float g, float b);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float TintController_GetPbrEmissionStrengthFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TintController_SetPbrEmissionStrengthFn(int entityId, float value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int TintController_GetGlobalBlendModeFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TintController_SetGlobalBlendModeFn(int entityId, int blendMode);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool TintController_GetAutoIncludeParentedSkinnedMeshesFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TintController_SetAutoIncludeParentedSkinnedMeshesFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool enabled);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TintController_RefreshFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TintController_ClearTargetsFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TintController_RemoveTargetsForEntityFn(int entityId, int targetEntityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TintController_AddTargetFn(int entityId, int targetEntityId, int materialSlot, int blendMode, [MarshalAs(UnmanagedType.I1)] bool useTargetColor, float r, float g, float b, float a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int TintController_GetTrackedTargetCountFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int TintController_GetTrackedTargetEntityFn(int entityId, int index);

    // BoneAttachment
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool BoneAttachment_HasComponentFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool BoneAttachment_GetEnabledFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void BoneAttachment_SetEnabledFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool enabled);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr BoneAttachment_GetBoneNameFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void BoneAttachment_SetBoneNameFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string boneName);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void BoneAttachment_GetLocalPositionFn(int entityId, out float x, out float y, out float z);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void BoneAttachment_SetLocalPositionFn(int entityId, float x, float y, float z);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void BoneAttachment_GetLocalRotationFn(int entityId, out float x, out float y, out float z);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void BoneAttachment_SetLocalRotationFn(int entityId, float x, float y, float z);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void BoneAttachment_GetLocalScaleFn(int entityId, out float x, out float y, out float z);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void BoneAttachment_SetLocalScaleFn(int entityId, float x, float y, float z);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool BoneAttachment_GetInheritRotationFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void BoneAttachment_SetInheritRotationFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool inherit);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool BoneAttachment_GetInheritScaleFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void BoneAttachment_SetInheritScaleFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool inherit);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool BoneAttachment_IsResolvedFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void BoneAttachment_InvalidateResolutionFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int BoneAttachment_GetSkeletonEntityFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void BoneAttachment_SetSkeletonEntityFn(int entityId, int skeletonEntityId);

    // Animator parameter setters
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void Animator_SetBoolFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string name, [MarshalAs(UnmanagedType.I1)] bool value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void Animator_SetIntFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string name, int value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void Animator_SetFloatFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string name, float value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void Animator_SetTriggerFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string name);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void Animator_ResetTriggerFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string name);
    // Animator getters
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool  Animator_GetBoolFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string name);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int   Animator_GetIntFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string name);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float Animator_GetFloatFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string name);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool  Animator_GetTriggerFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string name);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr Animator_GetStateFn(int entityId);
    // Animator enable/disable (for ragdoll)
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool Animator_GetEnabledFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void Animator_SetEnabledFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool enabled);
    // Animator controller switching
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void Animator_SetControllerFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string controllerPath, float blendSeconds);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void Animator_SetOverrideFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string overridePath);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int Animator_GetCurrentClipRootMotionModeFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool NpcScalability_GetParticipatesFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int NpcScalability_GetTierFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int NpcScalability_GetRepresentationFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate uint NpcScalability_GetReasonFlagsFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float NpcScalability_GetCameraDistanceFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool NpcScalability_GetVisibleFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int NpcScalability_GetCrowdRankFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int NpcScalability_GetCrowdCountFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float NpcScalability_GetAnimationUpdateIntervalFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float NpcScalability_GetScriptUpdateIntervalFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float NpcScalability_GetNavigationRepathIntervalFn(int entityId);

    // UI Buttons
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_ButtonIsHoveredFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_ButtonIsPressedFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_ButtonWasClickedFn(int entityId);

    // UI Text
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_GetTextFn(int entityId, out IntPtr strPtr);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_SetTextFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string text);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float UI_Text_GetOpacityFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_SetOpacityFn(int entityId, float opacity);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Text_GetVisibleFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_SetVisibleFn(int entityId, bool visible);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_GetColorFn(int entityId, out float r, out float g, out float b, out float a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_SetColorFn(int entityId, float r, float g, float b, float a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float UI_Text_GetPixelSizeFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_SetPixelSizeFn(int entityId, float size);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int UI_Text_GetZOrderFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_SetZOrderFn(int entityId, int zOrder);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_GetFontPathFn(int entityId, out IntPtr strPtr);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_SetFontPathFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string path);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Text_GetAnchorEnabledFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_SetAnchorEnabledFn(int entityId, bool enabled);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int UI_Text_GetAnchorFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_SetAnchorFn(int entityId, int anchor);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_GetAnchorOffsetFn(int entityId, out float x, out float y);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_SetAnchorOffsetFn(int entityId, float x, float y);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Text_GetWordWrapFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_SetWordWrapFn(int entityId, bool wrap);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_GetRectSizeFn(int entityId, out float w, out float h);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_SetRectSizeFn(int entityId, float w, float h);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Text_GetWorldSpaceFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_SetWorldSpaceFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool worldSpace);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Text_GetBillboardFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_SetBillboardFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool billboard);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Text_GetOutlineEnabledFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_SetOutlineEnabledFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool enabled);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_GetOutlineColorFn(int entityId, out float r, out float g, out float b, out float a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_SetOutlineColorFn(int entityId, float r, float g, float b, float a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float UI_Text_GetOutlineThicknessFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_SetOutlineThicknessFn(int entityId, float thickness);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Text_GetShadowEnabledFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_SetShadowEnabledFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool enabled);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_GetShadowColorFn(int entityId, out float r, out float g, out float b, out float a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_SetShadowColorFn(int entityId, float r, float g, float b, float a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_GetShadowOffsetFn(int entityId, out float x, out float y);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_SetShadowOffsetFn(int entityId, float x, float y);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int UI_Text_GetAlignmentFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Text_SetAlignmentFn(int entityId, int alignment);

    // UI Panel (34 entries: 30 base + 2 texture + 2 child opacity)
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float UI_Panel_GetOpacityFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Panel_SetOpacityFn(int entityId, float opacity);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Panel_GetVisibleFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Panel_SetVisibleFn(int entityId, bool visible);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Panel_GetSizeFn(int entityId, out float w, out float h);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Panel_SetSizeFn(int entityId, float w, float h);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Panel_GetTintColorFn(int entityId, out float r, out float g, out float b, out float a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Panel_SetTintColorFn(int entityId, float r, float g, float b, float a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Panel_GetAnchorEnabledFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Panel_SetAnchorEnabledFn(int entityId, bool enabled);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Panel_GetAnchorToParentFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Panel_SetAnchorToParentFn(int entityId, bool enabled);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int UI_Panel_GetAnchorFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Panel_SetAnchorFn(int entityId, int anchor);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Panel_GetAnchorOffsetFn(int entityId, out float x, out float y);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Panel_SetAnchorOffsetFn(int entityId, float x, float y);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int UI_Panel_GetZOrderFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Panel_SetZOrderFn(int entityId, int z);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Panel_IsHoveredFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Panel_IsPressedFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Panel_IsDraggingFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Panel_DragStartedFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Panel_DragEndedFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Panel_WasDroppedFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int UI_Panel_GetDropSourceFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int UI_Panel_GetDropTargetFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Panel_GetAllowDragFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Panel_SetAllowDragFn(int entityId, bool allow);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Panel_GetAllowDropFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Panel_SetAllowDropFn(int entityId, bool allow);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Panel_GetDriveChildrenOpacityFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Panel_SetDriveChildrenOpacityFn(int entityId, bool enabled);
    // UI Rect (12 entries)
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Rect_GetAnchorToParentFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Rect_SetAnchorToParentFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool anchorToParent);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float UI_Rect_GetHorizontalAnchorFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Rect_SetHorizontalAnchorFn(int entityId, float anchor);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float UI_Rect_GetVerticalAnchorFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Rect_SetVerticalAnchorFn(int entityId, float anchor);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Rect_GetPivotFn(int entityId, out float x, out float y);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Rect_SetPivotFn(int entityId, float x, float y);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Rect_GetOffsetFn(int entityId, out float x, out float y);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Rect_SetOffsetFn(int entityId, float x, float y);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Rect_GetSizeFn(int entityId, out float w, out float h);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Rect_SetSizeFn(int entityId, float w, float h);
    // UI Canvas (6 entries)
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float UI_Canvas_GetOpacityFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Canvas_SetOpacityFn(int entityId, float opacity);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int UI_Canvas_GetRenderSpaceFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Canvas_SetRenderSpaceFn(int entityId, int renderSpace);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Canvas_GetBillboardFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Canvas_SetBillboardFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool billboard);

    // UI LayoutGroup (16 entries)
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int UI_LayoutGroup_GetDirectionFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_LayoutGroup_SetDirectionFn(int entityId, int direction);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_LayoutGroup_GetPaddingFn(int entityId, out float left, out float top, out float right, out float bottom);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_LayoutGroup_SetPaddingFn(int entityId, float left, float top, float right, float bottom);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float UI_LayoutGroup_GetSpacingFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_LayoutGroup_SetSpacingFn(int entityId, float spacing);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int UI_LayoutGroup_GetChildAlignmentFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_LayoutGroup_SetChildAlignmentFn(int entityId, int alignment);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int UI_LayoutGroup_GetCrossAlignmentFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_LayoutGroup_SetCrossAlignmentFn(int entityId, int alignment);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_LayoutGroup_GetControlChildWidthFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_LayoutGroup_SetControlChildWidthFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool control);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_LayoutGroup_GetControlChildHeightFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_LayoutGroup_SetControlChildHeightFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool control);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_LayoutGroup_GetReverseOrderFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_LayoutGroup_SetReverseOrderFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool reverse);

    // UI FitToContent (14 entries)
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_FitToContent_GetEnabledFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_FitToContent_SetEnabledFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool enabled);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_FitToContent_GetFitWidthFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_FitToContent_SetFitWidthFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool fit);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_FitToContent_GetFitHeightFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_FitToContent_SetFitHeightFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool fit);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_FitToContent_GetPaddingFn(int entityId, out float left, out float top, out float right, out float bottom);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_FitToContent_SetPaddingFn(int entityId, float left, float top, float right, float bottom);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_FitToContent_GetMinSizeFn(int entityId, out float w, out float h);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_FitToContent_SetMinSizeFn(int entityId, float w, float h);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_FitToContent_GetMaxSizeFn(int entityId, out float w, out float h);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_FitToContent_SetMaxSizeFn(int entityId, float w, float h);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_FitToContent_GetDirectChildrenOnlyFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_FitToContent_SetDirectChildrenOnlyFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool directOnly);

    // UI Scene Capture (28 entries)
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_SceneCapture_GetEnabledFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_SceneCapture_SetEnabledFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool enabled);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_SceneCapture_GetAutoFrameFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_SceneCapture_SetAutoFrameFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool autoFrame);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_SceneCapture_GetIncludeChildrenFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_SceneCapture_SetIncludeChildrenFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool includeChildren);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float UI_SceneCapture_GetBoundsPaddingFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_SceneCapture_SetBoundsPaddingFn(int entityId, float padding);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float UI_SceneCapture_GetFieldOfViewFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_SceneCapture_SetFieldOfViewFn(int entityId, float fov);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float UI_SceneCapture_GetNearClipFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_SceneCapture_SetNearClipFn(int entityId, float nearClip);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float UI_SceneCapture_GetFarClipFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_SceneCapture_SetFarClipFn(int entityId, float farClip);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_SceneCapture_GetViewDirectionFn(int entityId, out float x, out float y, out float z);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_SceneCapture_SetViewDirectionFn(int entityId, float x, float y, float z);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_SceneCapture_GetUpDirectionFn(int entityId, out float x, out float y, out float z);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_SceneCapture_SetUpDirectionFn(int entityId, float x, float y, float z);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_SceneCapture_GetFocusOffsetFn(int entityId, out float x, out float y, out float z);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_SceneCapture_SetFocusOffsetFn(int entityId, float x, float y, float z);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int UI_SceneCapture_GetTargetEntityFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_SceneCapture_SetTargetEntityFn(int entityId, int targetEntity);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_SceneCapture_GetRenderSizeFn(int entityId, out int w, out int h);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_SceneCapture_SetRenderSizeFn(int entityId, int w, int h);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_SceneCapture_GetClearColorFn(int entityId, out float r, out float g, out float b, out float a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_SceneCapture_SetClearColorFn(int entityId, float r, float g, float b, float a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_SceneCapture_GetShowGridFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_SceneCapture_SetShowGridFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool showGrid);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_SceneCapture_GetLockViewToTargetFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_SceneCapture_SetLockViewToTargetFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool lockView);

    // UI Slider (9 entries)
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float UI_Slider_GetValueFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Slider_SetValueFn(int entityId, float value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float UI_Slider_GetMinValueFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Slider_SetMinValueFn(int entityId, float value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float UI_Slider_GetMaxValueFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Slider_SetMaxValueFn(int entityId, float value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Slider_IsHoveredFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Slider_IsDraggingFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Slider_ValueChangedFn(int entityId);

    // UI ProgressBar (10 entries)
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float UI_ProgressBar_GetValueFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_ProgressBar_SetValueFn(int entityId, float value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float UI_ProgressBar_GetMinValueFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_ProgressBar_SetMinValueFn(int entityId, float value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float UI_ProgressBar_GetMaxValueFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_ProgressBar_SetMaxValueFn(int entityId, float value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float UI_ProgressBar_GetOpacityFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_ProgressBar_SetOpacityFn(int entityId, float opacity);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_ProgressBar_GetVisibleFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_ProgressBar_SetVisibleFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool visible);

    // UI Toggle (5 entries)
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Toggle_GetIsOnFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Toggle_SetIsOnFn(int entityId, bool isOn);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Toggle_IsHoveredFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Toggle_IsPressedFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Toggle_ValueChangedFn(int entityId);

    // UI ScrollView (8 entries)
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_ScrollView_GetContentOffsetFn(int entityId, out float x, out float y);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_ScrollView_SetContentOffsetFn(int entityId, float x, float y);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_ScrollView_GetContentSizeFn(int entityId, out float w, out float h);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_ScrollView_SetContentSizeFn(int entityId, float w, float h);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float UI_ScrollView_GetOpacityFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_ScrollView_SetOpacityFn(int entityId, float opacity);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_ScrollView_GetVisibleFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_ScrollView_SetVisibleFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool visible);

    // UI InputField (6 entries)
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_InputField_GetTextFn(int entityId, out IntPtr strPtr);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_InputField_SetTextFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string text);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_InputField_GetPlaceholderFn(int entityId, out IntPtr strPtr);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_InputField_SetPlaceholderFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string text);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_InputField_IsFocusedFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_InputField_TextChangedFn(int entityId);

    // UI Dropdown (9 entries)
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int UI_Dropdown_GetSelectedIndexFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Dropdown_SetSelectedIndexFn(int entityId, int index);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int UI_Dropdown_GetOptionCountFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Dropdown_GetOptionFn(int entityId, int index, out IntPtr strPtr);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Dropdown_SetOptionFn(int entityId, int index, [MarshalAs(UnmanagedType.LPStr)] string text);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Dropdown_AddOptionFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string text);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void UI_Dropdown_ClearOptionsFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Dropdown_IsOpenFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool UI_Dropdown_SelectionChangedFn(int entityId);

    // Animation Layers (28 entries)
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int AnimLayer_GetOrCreateFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName, int priority);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool AnimLayer_RemoveFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool AnimLayer_HasFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int AnimLayer_GetCountFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr AnimLayer_GetNameByIndexFn(int entityId, int index);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AnimLayer_SetAnimationFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName, [MarshalAs(UnmanagedType.LPStr)] string animPath);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr AnimLayer_GetAnimationFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AnimLayer_SetMaskFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName, int maskPreset);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int AnimLayer_GetMaskFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AnimLayer_SetBlendModeFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName, int blendMode);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int AnimLayer_GetBlendModeFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AnimLayer_SetWeightFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName, float weight);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float AnimLayer_GetWeightFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AnimLayer_BlendToFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName, float targetWeight, float blendSpeed);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AnimLayer_PlayFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName, [MarshalAs(UnmanagedType.I1)] bool loop);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AnimLayer_StopFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool AnimLayer_IsPlayingFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AnimLayer_SetSpeedFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName, float speed);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float AnimLayer_GetSpeedFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AnimLayer_SetTimeFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName, float time);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AnimLayer_SetNormalizedTimeFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName, float normalizedTime);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AnimLayer_SetStateFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName, int stateId, float normalizedTime);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AnimLayer_SetStateByNameFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName, [MarshalAs(UnmanagedType.LPStr)] string stateName, float normalizedTime, [MarshalAs(UnmanagedType.I1)] bool satisfyTransitionConditions);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AnimLayer_SetBlend2DFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName, float x, float y, [MarshalAs(UnmanagedType.I1)] bool clampToUnitRange);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float AnimLayer_GetTimeFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float AnimLayer_GetDurationFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AnimLayer_SetLoopingFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName, [MarshalAs(UnmanagedType.I1)] bool loop);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool AnimLayer_GetLoopingFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string layerName);

    // Character Controller
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void CC_SetDesiredVelocityFn(int entityId, float x, float y, float z);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void CC_GetDesiredVelocityFn(int entityId, out float x, out float y, out float z);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void CC_SetVerticalVelocityFn(int entityId, float v);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float CC_GetVerticalVelocityFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void CC_JumpFn(int entityId, float speed);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool CC_IsGroundedFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void CC_SetPositionFn(int entityId, float x, float y, float z);

    // TerrainComponent
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool Terrain_GetHeightAtWorldFn(int entityId, float worldX, float worldZ, out float height);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool Terrain_GetNormalAtWorldFn(int entityId, float worldX, float worldZ, out float x, out float y, out float z);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool Terrain_GetNearestPointFn(int entityId, float worldX, float worldZ, out float x, out float y, out float z);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool Terrain_RaycastFn(int entityId, float ox, float oy, float oz, float dx, float dy, float dz,
        out float hitX, out float hitY, out float hitZ, out float nx, out float ny, out float nz);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool Terrain_GetDominantLayerAtWorldFn(int entityId, float worldX, float worldZ, out int layerIndex, out float weight);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool Terrain_SetHeightAtWorldFn(int entityId, float worldX, float worldZ, float worldHeight);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool Terrain_ApplyHeightDeltaFn(int entityId, float worldX, float worldZ, float radius, float deltaHeight, float falloff);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int Terrain_GetInstancerLayerCountFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr Terrain_GetInstancerLayerNameFn(int entityId, int layerIndex);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool Terrain_SetInstancerLayerEnabledFn(int entityId, int layerIndex, [MarshalAs(UnmanagedType.I1)] bool enabled);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool Terrain_SetInstancerLayerDensityFn(int entityId, int layerIndex, float density);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool Terrain_RegenerateInstancersFn(int entityId);

    // SplineComponent
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int Spline_GetControlPointCountFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool Spline_GetControlPointFn(int entityId, int index, out float x, out float y, out float z);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int Spline_GetSampledPointCountFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool Spline_GetSampledPointFn(int entityId, int index, out float x, out float y, out float z);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool Spline_GetNearestPointFn(int entityId, float worldX, float worldY, float worldZ, out float outX, out float outY, out float outZ, out float outDistance);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool Spline_GetPointAtNormalizedFn(int entityId, float t, out float outX, out float outY, out float outZ);

    // PortalComponent
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool Portal_GetEnabledFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void Portal_SetEnabledFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool enabled);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr Portal_GetTargetScenePathFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void Portal_SetTargetScenePathFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string path);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr Portal_GetTargetPortalGuidFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void Portal_SetTargetPortalGuidFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string guid);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr Portal_GetTargetPortalPathFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void Portal_SetTargetPortalPathFn(int entityId, [MarshalAs(UnmanagedType.LPStr)] string path);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void Portal_GetVec3Fn(int entityId, out float x, out float y, out float z);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void Portal_SetVec3Fn(int entityId, float x, float y, float z);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate bool Portal_GetBoolFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void Portal_SetBoolFn(int entityId, [MarshalAs(UnmanagedType.I1)] bool value);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate float Portal_GetFloatFn(int entityId);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void Portal_SetFloatFn(int entityId, float value);

    public static unsafe class ComponentInterop
    {
        public const int InteropCount = 401;

        // --- Function pointers ---
        // Managed Logging (first so it's available early)
        public static ManagedLogFn ManagedLog;
        
        public static HasComponentFn HasComponent;
        public static AddComponentFn AddComponent;
        public static RemoveComponentFn RemoveComponent;

        // Light
        public static GetLightTypeFn GetLightType;
        public static SetLightTypeFn SetLightType;
        public static GetLightColorFn GetLightColor;
        public static SetLightColorFn SetLightColor;
        public static GetLightIntensityFn GetLightIntensity;
        public static SetLightIntensityFn SetLightIntensity;

        // RigidBody
        public static GetRigidBodyMassFn GetRigidBodyMass;
        public static SetRigidBodyMassFn SetRigidBodyMass;
        public static GetRigidBodyIsKinematicFn GetRigidBodyIsKinematic;
        public static SetRigidBodyIsKinematicFn SetRigidBodyIsKinematic;
        public static GetRigidBodyUseGravityFn GetRigidBodyUseGravity;
        public static SetRigidBodyUseGravityFn SetRigidBodyUseGravity;
        public static GetRigidBodyCollisionMaskFn GetRigidBodyCollisionMask;
        public static SetRigidBodyCollisionMaskFn SetRigidBodyCollisionMask;
        public static GetRigidBodyLinearVelocityFn GetRigidBodyLinearVelocity;
        public static SetRigidBodyLinearVelocityFn SetRigidBodyLinearVelocity;
        public static GetRigidBodyAngularVelocityFn GetRigidBodyAngularVelocity;
        public static SetRigidBodyAngularVelocityFn SetRigidBodyAngularVelocity;
        public static ApplyRigidBodyForceFn ApplyRigidBodyForce;
        public static ApplyRigidBodyTorqueFn ApplyRigidBodyTorque;
        public static ApplyRigidBodyImpulseFn ApplyRigidBodyImpulse;
        public static ApplyRigidBodyAngularImpulseFn ApplyRigidBodyAngularImpulse;
        public static RigidBody_GetDebugSummaryFn RigidBody_GetDebugSummaryInternal;
        public static Collider_GetOffsetFn Collider_GetOffset;

        // Terrain
        public static Terrain_GetHeightAtWorldFn Terrain_GetHeightAtWorld;
        public static Terrain_GetNormalAtWorldFn Terrain_GetNormalAtWorld;
        public static Terrain_GetNearestPointFn Terrain_GetNearestPoint;
        public static Terrain_RaycastFn Terrain_Raycast;
        public static Terrain_GetDominantLayerAtWorldFn Terrain_GetDominantLayerAtWorld;
        public static Terrain_SetHeightAtWorldFn Terrain_SetHeightAtWorld;
        public static Terrain_ApplyHeightDeltaFn Terrain_ApplyHeightDelta;
        public static Terrain_GetInstancerLayerCountFn Terrain_GetInstancerLayerCount;
        public static Terrain_GetInstancerLayerNameFn Terrain_GetInstancerLayerNameInternal;
        public static Terrain_SetInstancerLayerEnabledFn Terrain_SetInstancerLayerEnabled;
        public static Terrain_SetInstancerLayerDensityFn Terrain_SetInstancerLayerDensity;
        public static Terrain_RegenerateInstancersFn Terrain_RegenerateInstancers;

        // Spline
        public static Spline_GetControlPointCountFn Spline_GetControlPointCount;
        public static Spline_GetControlPointFn Spline_GetControlPoint;
        public static Spline_GetSampledPointCountFn Spline_GetSampledPointCount;
        public static Spline_GetSampledPointFn Spline_GetSampledPoint;
        public static Spline_GetNearestPointFn Spline_GetNearestPoint;
        public static Spline_GetPointAtNormalizedFn Spline_GetPointAtNormalized;

        // Portal
        public static Portal_GetEnabledFn Portal_GetEnabled;
        public static Portal_SetEnabledFn Portal_SetEnabled;
        public static Portal_GetTargetScenePathFn Portal_GetTargetScenePathInternal;
        public static Portal_SetTargetScenePathFn Portal_SetTargetScenePath;
        public static Portal_GetTargetPortalGuidFn Portal_GetTargetPortalGuidInternal;
        public static Portal_SetTargetPortalGuidFn Portal_SetTargetPortalGuid;
        public static Portal_GetTargetPortalPathFn Portal_GetTargetPortalPathInternal;
        public static Portal_SetTargetPortalPathFn Portal_SetTargetPortalPath;
        public static Portal_GetVec3Fn Portal_GetEntryOffset;
        public static Portal_SetVec3Fn Portal_SetEntryOffset;
        public static Portal_GetVec3Fn Portal_GetExitOffset;
        public static Portal_SetVec3Fn Portal_SetExitOffset;
        public static Portal_GetBoolFn Portal_GetAutoDetect;
        public static Portal_SetBoolFn Portal_SetAutoDetect;
        public static Portal_GetFloatFn Portal_GetTriggerRadius;
        public static Portal_SetFloatFn Portal_SetTriggerRadius;
        public static Portal_GetBoolFn Portal_GetFireExitEvents;
        public static Portal_SetBoolFn Portal_SetFireExitEvents;

        // Camera
        public static GetCameraLayerMaskFn GetCameraLayerMask;
        public static SetCameraLayerMaskFn SetCameraLayerMask;
        public static Camera_SetLayerMaskByNameFn Camera_SetLayerMaskByName;
        // Camera settings (12 entries)
        public static GetCameraActiveFn GetCameraActive;
        public static SetCameraActiveFn SetCameraActive;
        public static GetCameraPriorityFn GetCameraPriority;
        public static SetCameraPriorityFn SetCameraPriority;
        public static GetCameraFieldOfViewFn GetCameraFieldOfView;
        public static SetCameraFieldOfViewFn SetCameraFieldOfView;
        public static GetCameraNearClipFn GetCameraNearClip;
        public static SetCameraNearClipFn SetCameraNearClip;
        public static GetCameraFarClipFn GetCameraFarClip;
        public static SetCameraFarClipFn SetCameraFarClip;
        public static GetCameraIsPerspectiveFn GetCameraIsPerspective;
        public static SetCameraIsPerspectiveFn SetCameraIsPerspective;

        // BlendShapes
        public static SetBlendShapeWeightFn SetBlendShapeWeight;
        public static GetBlendShapeWeightFn GetBlendShapeWeight;
        public static GetBlendShapeCountFn GetBlendShapeCount;
        public static GetBlendShapeNameFn GetBlendShapeNameInternal;

        // UnifiedMorphs
        public static UnifiedMorph_GetCountFn UnifiedMorph_GetCount;
        public static UnifiedMorph_GetNameFn UnifiedMorph_GetNameInternal;
        public static UnifiedMorph_GetWeightFn UnifiedMorph_GetWeight;
        public static UnifiedMorph_SetWeightFn UnifiedMorph_SetWeight;
        public static UnifiedMorph_PropagateAllFn UnifiedMorph_PropagateAll;

        // TintMaskController
        public static TintController_HasComponentFn TintController_HasComponent;
        public static TintController_GetNamePatternFn TintController_GetNamePatternInternal;
        public static TintController_SetNamePatternFn TintController_SetNamePattern;
        public static TintController_GetBaseTintFn TintController_GetBaseTint;
        public static TintController_SetBaseTintFn TintController_SetBaseTint;
        public static TintController_GetTintColorFn TintController_GetTintColor;
        public static TintController_SetTintColorFn TintController_SetTintColor;
        public static TintController_GetUseTintMaskFn TintController_GetUseTintMask;
        public static TintController_SetUseTintMaskFn TintController_SetUseTintMask;
        public static TintController_GetUsePbrOverridesFn TintController_GetUsePbrOverrides;
        public static TintController_SetUsePbrOverridesFn TintController_SetUsePbrOverrides;
        public static TintController_GetPbrMetallicFn TintController_GetPbrMetallic;
        public static TintController_SetPbrMetallicFn TintController_SetPbrMetallic;
        public static TintController_GetPbrRoughnessFn TintController_GetPbrRoughness;
        public static TintController_SetPbrRoughnessFn TintController_SetPbrRoughness;
        public static TintController_GetPbrEmissionColorFn TintController_GetPbrEmissionColor;
        public static TintController_SetPbrEmissionColorFn TintController_SetPbrEmissionColor;
        public static TintController_GetPbrEmissionStrengthFn TintController_GetPbrEmissionStrength;
        public static TintController_SetPbrEmissionStrengthFn TintController_SetPbrEmissionStrength;
        public static TintController_GetGlobalBlendModeFn TintController_GetGlobalBlendMode;
        public static TintController_SetGlobalBlendModeFn TintController_SetGlobalBlendMode;
        public static TintController_GetAutoIncludeParentedSkinnedMeshesFn TintController_GetAutoIncludeParentedSkinnedMeshes;
        public static TintController_SetAutoIncludeParentedSkinnedMeshesFn TintController_SetAutoIncludeParentedSkinnedMeshes;
        public static TintController_RefreshFn TintController_Refresh;
        public static TintController_ClearTargetsFn TintController_ClearTargets;
        public static TintController_RemoveTargetsForEntityFn TintController_RemoveTargetsForEntity;
        public static TintController_AddTargetFn TintController_AddTarget;
        public static TintController_GetTrackedTargetCountFn TintController_GetTrackedTargetCount;
        public static TintController_GetTrackedTargetEntityFn TintController_GetTrackedTargetEntity;

        // BoneAttachment
        public static BoneAttachment_HasComponentFn BoneAttachment_HasComponent;
        public static BoneAttachment_GetEnabledFn BoneAttachment_GetEnabled;
        public static BoneAttachment_SetEnabledFn BoneAttachment_SetEnabled;
        public static BoneAttachment_GetBoneNameFn BoneAttachment_GetBoneNameInternal;
        public static BoneAttachment_SetBoneNameFn BoneAttachment_SetBoneName;
        public static BoneAttachment_GetLocalPositionFn BoneAttachment_GetLocalPosition;
        public static BoneAttachment_SetLocalPositionFn BoneAttachment_SetLocalPosition;
        public static BoneAttachment_GetLocalRotationFn BoneAttachment_GetLocalRotation;
        public static BoneAttachment_SetLocalRotationFn BoneAttachment_SetLocalRotation;
        public static BoneAttachment_GetLocalScaleFn BoneAttachment_GetLocalScale;
        public static BoneAttachment_SetLocalScaleFn BoneAttachment_SetLocalScale;
        public static BoneAttachment_GetInheritRotationFn BoneAttachment_GetInheritRotation;
        public static BoneAttachment_SetInheritRotationFn BoneAttachment_SetInheritRotation;
        public static BoneAttachment_GetInheritScaleFn BoneAttachment_GetInheritScale;
        public static BoneAttachment_SetInheritScaleFn BoneAttachment_SetInheritScale;
        public static BoneAttachment_IsResolvedFn BoneAttachment_IsResolved;
        public static BoneAttachment_InvalidateResolutionFn BoneAttachment_InvalidateResolution;
        public static BoneAttachment_GetSkeletonEntityFn BoneAttachment_GetSkeletonEntity;
        public static BoneAttachment_SetSkeletonEntityFn BoneAttachment_SetSkeletonEntity;

        // Animator
        public static Animator_SetBoolFn Animator_SetBool;
        public static Animator_SetIntFn Animator_SetInt;
        public static Animator_SetFloatFn Animator_SetFloat;
        public static Animator_SetTriggerFn Animator_SetTrigger;
        public static Animator_ResetTriggerFn Animator_ResetTrigger;
        public static Animator_GetBoolFn Animator_GetBool;
        public static Animator_GetIntFn Animator_GetInt;
        public static Animator_GetFloatFn Animator_GetFloat;
        public static Animator_GetTriggerFn Animator_GetTrigger;
        public static Animator_GetStateFn Animator_GetStateInternal;
        public static Animator_GetEnabledFn Animator_GetEnabled;
        public static Animator_SetEnabledFn Animator_SetEnabled;
        public static Animator_SetControllerFn Animator_SetController;
        public static Animator_SetOverrideFn Animator_SetOverride;
        public static Animator_GetCurrentClipRootMotionModeFn Animator_GetCurrentClipRootMotionMode;
        public static NpcScalability_GetParticipatesFn NpcScalability_GetParticipates;
        public static NpcScalability_GetTierFn NpcScalability_GetTier;
        public static NpcScalability_GetRepresentationFn NpcScalability_GetRepresentation;
        public static NpcScalability_GetReasonFlagsFn NpcScalability_GetReasonFlags;
        public static NpcScalability_GetCameraDistanceFn NpcScalability_GetCameraDistance;
        public static NpcScalability_GetVisibleFn NpcScalability_GetVisible;
        public static NpcScalability_GetCrowdRankFn NpcScalability_GetCrowdRank;
        public static NpcScalability_GetCrowdCountFn NpcScalability_GetCrowdCount;
        public static NpcScalability_GetAnimationUpdateIntervalFn NpcScalability_GetAnimationUpdateInterval;
        public static NpcScalability_GetScriptUpdateIntervalFn NpcScalability_GetScriptUpdateInterval;
        public static NpcScalability_GetNavigationRepathIntervalFn NpcScalability_GetNavigationRepathInterval;

        // UI Buttons
        public static UI_ButtonIsHoveredFn UI_ButtonIsHovered;
        public static UI_ButtonIsPressedFn UI_ButtonIsPressed;
        public static UI_ButtonWasClickedFn UI_ButtonWasClicked;

        // UI Text
        public static UI_Text_GetTextFn UI_Text_GetText;
        public static UI_Text_SetTextFn UI_Text_SetText;
        public static UI_Text_GetOpacityFn UI_Text_GetOpacity;
        public static UI_Text_SetOpacityFn UI_Text_SetOpacity;
        public static UI_Text_GetVisibleFn UI_Text_GetVisible;
        public static UI_Text_SetVisibleFn UI_Text_SetVisible;
        public static UI_Text_GetColorFn UI_Text_GetColor;
        public static UI_Text_SetColorFn UI_Text_SetColor;
        public static UI_Text_GetPixelSizeFn UI_Text_GetPixelSize;
        public static UI_Text_SetPixelSizeFn UI_Text_SetPixelSize;
        public static UI_Text_GetZOrderFn UI_Text_GetZOrder;
        public static UI_Text_SetZOrderFn UI_Text_SetZOrder;
        public static UI_Text_GetFontPathFn UI_Text_GetFontPathInternal;
        public static UI_Text_SetFontPathFn UI_Text_SetFontPath;
        public static UI_Text_GetAnchorEnabledFn UI_Text_GetAnchorEnabled;
        public static UI_Text_SetAnchorEnabledFn UI_Text_SetAnchorEnabled;
        public static UI_Text_GetAnchorFn UI_Text_GetAnchor;
        public static UI_Text_SetAnchorFn UI_Text_SetAnchor;
        public static UI_Text_GetAnchorOffsetFn UI_Text_GetAnchorOffset;
        public static UI_Text_SetAnchorOffsetFn UI_Text_SetAnchorOffset;
        public static UI_Text_GetWordWrapFn UI_Text_GetWordWrap;
        public static UI_Text_SetWordWrapFn UI_Text_SetWordWrap;
        public static UI_Text_GetRectSizeFn UI_Text_GetRectSize;
        public static UI_Text_SetRectSizeFn UI_Text_SetRectSize;
        public static UI_Text_GetWorldSpaceFn UI_Text_GetWorldSpace;
        public static UI_Text_SetWorldSpaceFn UI_Text_SetWorldSpace;
        public static UI_Text_GetBillboardFn UI_Text_GetBillboard;
        public static UI_Text_SetBillboardFn UI_Text_SetBillboard;
        public static UI_Text_GetOutlineEnabledFn UI_Text_GetOutlineEnabled;
        public static UI_Text_SetOutlineEnabledFn UI_Text_SetOutlineEnabled;
        public static UI_Text_GetOutlineColorFn UI_Text_GetOutlineColor;
        public static UI_Text_SetOutlineColorFn UI_Text_SetOutlineColor;
        public static UI_Text_GetOutlineThicknessFn UI_Text_GetOutlineThickness;
        public static UI_Text_SetOutlineThicknessFn UI_Text_SetOutlineThickness;
        public static UI_Text_GetShadowEnabledFn UI_Text_GetShadowEnabled;
        public static UI_Text_SetShadowEnabledFn UI_Text_SetShadowEnabled;
        public static UI_Text_GetShadowColorFn UI_Text_GetShadowColor;
        public static UI_Text_SetShadowColorFn UI_Text_SetShadowColor;
        public static UI_Text_GetShadowOffsetFn UI_Text_GetShadowOffset;
        public static UI_Text_SetShadowOffsetFn UI_Text_SetShadowOffset;
        public static UI_Text_GetAlignmentFn UI_Text_GetAlignment;
        public static UI_Text_SetAlignmentFn UI_Text_SetAlignment;
        // UI Panel (34 entries: 30 base + 2 texture + 2 child opacity)
        public static UI_Panel_GetOpacityFn UI_Panel_GetOpacity;
        public static UI_Panel_SetOpacityFn UI_Panel_SetOpacity;
        public static UI_Panel_GetVisibleFn UI_Panel_GetVisible;
        public static UI_Panel_SetVisibleFn UI_Panel_SetVisible;
        public static UI_Panel_GetSizeFn UI_Panel_GetSize;
        public static UI_Panel_SetSizeFn UI_Panel_SetSize;
        public static UI_Panel_GetTintColorFn UI_Panel_GetTintColor;
        public static UI_Panel_SetTintColorFn UI_Panel_SetTintColor;
        public static UI_Panel_GetAnchorEnabledFn UI_Panel_GetAnchorEnabled;
        public static UI_Panel_SetAnchorEnabledFn UI_Panel_SetAnchorEnabled;
        public static UI_Panel_GetAnchorFn UI_Panel_GetAnchor;
        public static UI_Panel_SetAnchorFn UI_Panel_SetAnchor;
        public static UI_Panel_GetAnchorOffsetFn UI_Panel_GetAnchorOffset;
        public static UI_Panel_SetAnchorOffsetFn UI_Panel_SetAnchorOffset;
        public static UI_Panel_GetAnchorToParentFn UI_Panel_GetAnchorToParent;
        public static UI_Panel_SetAnchorToParentFn UI_Panel_SetAnchorToParent;
        public static UI_Panel_GetZOrderFn UI_Panel_GetZOrder;
        public static UI_Panel_SetZOrderFn UI_Panel_SetZOrder;
        public static UI_Panel_IsHoveredFn UI_Panel_IsHovered;
        public static UI_Panel_IsPressedFn UI_Panel_IsPressed;
        public static UI_Panel_IsDraggingFn UI_Panel_IsDragging;
        public static UI_Panel_DragStartedFn UI_Panel_DragStarted;
        public static UI_Panel_DragEndedFn UI_Panel_DragEnded;
        public static UI_Panel_WasDroppedFn UI_Panel_WasDropped;
        public static UI_Panel_GetDropSourceFn UI_Panel_GetDropSource;
        public static UI_Panel_GetDropTargetFn UI_Panel_GetDropTarget;
        public static UI_Panel_GetAllowDragFn UI_Panel_GetAllowDrag;
        public static UI_Panel_SetAllowDragFn UI_Panel_SetAllowDrag;
        public static UI_Panel_GetAllowDropFn UI_Panel_GetAllowDrop;
        public static UI_Panel_SetAllowDropFn UI_Panel_SetAllowDrop;
        public static UI_Panel_GetDriveChildrenOpacityFn UI_Panel_GetDriveChildrenOpacity;
        public static UI_Panel_SetDriveChildrenOpacityFn UI_Panel_SetDriveChildrenOpacity;
        // UI Rect (12 entries)
        public static UI_Rect_GetAnchorToParentFn UI_Rect_GetAnchorToParent;
        public static UI_Rect_SetAnchorToParentFn UI_Rect_SetAnchorToParent;
        public static UI_Rect_GetHorizontalAnchorFn UI_Rect_GetHorizontalAnchor;
        public static UI_Rect_SetHorizontalAnchorFn UI_Rect_SetHorizontalAnchor;
        public static UI_Rect_GetVerticalAnchorFn UI_Rect_GetVerticalAnchor;
        public static UI_Rect_SetVerticalAnchorFn UI_Rect_SetVerticalAnchor;
        public static UI_Rect_GetPivotFn UI_Rect_GetPivot;
        public static UI_Rect_SetPivotFn UI_Rect_SetPivot;
        public static UI_Rect_GetOffsetFn UI_Rect_GetOffset;
        public static UI_Rect_SetOffsetFn UI_Rect_SetOffset;
        public static UI_Rect_GetSizeFn UI_Rect_GetSize;
        public static UI_Rect_SetSizeFn UI_Rect_SetSize;
        // UI Canvas
        public static UI_Canvas_GetOpacityFn UI_Canvas_GetOpacity;
        public static UI_Canvas_SetOpacityFn UI_Canvas_SetOpacity;
        public static UI_Canvas_GetRenderSpaceFn UI_Canvas_GetRenderSpace;
        public static UI_Canvas_SetRenderSpaceFn UI_Canvas_SetRenderSpace;
        public static UI_Canvas_GetBillboardFn UI_Canvas_GetBillboard;
        public static UI_Canvas_SetBillboardFn UI_Canvas_SetBillboard;

        // UI LayoutGroup
        public static UI_LayoutGroup_GetDirectionFn UI_LayoutGroup_GetDirection;
        public static UI_LayoutGroup_SetDirectionFn UI_LayoutGroup_SetDirection;
        public static UI_LayoutGroup_GetPaddingFn UI_LayoutGroup_GetPadding;
        public static UI_LayoutGroup_SetPaddingFn UI_LayoutGroup_SetPadding;
        public static UI_LayoutGroup_GetSpacingFn UI_LayoutGroup_GetSpacing;
        public static UI_LayoutGroup_SetSpacingFn UI_LayoutGroup_SetSpacing;
        public static UI_LayoutGroup_GetChildAlignmentFn UI_LayoutGroup_GetChildAlignment;
        public static UI_LayoutGroup_SetChildAlignmentFn UI_LayoutGroup_SetChildAlignment;
        public static UI_LayoutGroup_GetCrossAlignmentFn UI_LayoutGroup_GetCrossAlignment;
        public static UI_LayoutGroup_SetCrossAlignmentFn UI_LayoutGroup_SetCrossAlignment;
        public static UI_LayoutGroup_GetControlChildWidthFn UI_LayoutGroup_GetControlChildWidth;
        public static UI_LayoutGroup_SetControlChildWidthFn UI_LayoutGroup_SetControlChildWidth;
        public static UI_LayoutGroup_GetControlChildHeightFn UI_LayoutGroup_GetControlChildHeight;
        public static UI_LayoutGroup_SetControlChildHeightFn UI_LayoutGroup_SetControlChildHeight;
        public static UI_LayoutGroup_GetReverseOrderFn UI_LayoutGroup_GetReverseOrder;
        public static UI_LayoutGroup_SetReverseOrderFn UI_LayoutGroup_SetReverseOrder;

        // UI FitToContent
        public static UI_FitToContent_GetEnabledFn UI_FitToContent_GetEnabled;
        public static UI_FitToContent_SetEnabledFn UI_FitToContent_SetEnabled;
        public static UI_FitToContent_GetFitWidthFn UI_FitToContent_GetFitWidth;
        public static UI_FitToContent_SetFitWidthFn UI_FitToContent_SetFitWidth;
        public static UI_FitToContent_GetFitHeightFn UI_FitToContent_GetFitHeight;
        public static UI_FitToContent_SetFitHeightFn UI_FitToContent_SetFitHeight;
        public static UI_FitToContent_GetPaddingFn UI_FitToContent_GetPadding;
        public static UI_FitToContent_SetPaddingFn UI_FitToContent_SetPadding;
        public static UI_FitToContent_GetMinSizeFn UI_FitToContent_GetMinSize;
        public static UI_FitToContent_SetMinSizeFn UI_FitToContent_SetMinSize;
        public static UI_FitToContent_GetMaxSizeFn UI_FitToContent_GetMaxSize;
        public static UI_FitToContent_SetMaxSizeFn UI_FitToContent_SetMaxSize;
        public static UI_FitToContent_GetDirectChildrenOnlyFn UI_FitToContent_GetDirectChildrenOnly;
        public static UI_FitToContent_SetDirectChildrenOnlyFn UI_FitToContent_SetDirectChildrenOnly;

        // UI Scene Capture
        public static UI_SceneCapture_GetEnabledFn UI_SceneCapture_GetEnabled;
        public static UI_SceneCapture_SetEnabledFn UI_SceneCapture_SetEnabled;
        public static UI_SceneCapture_GetAutoFrameFn UI_SceneCapture_GetAutoFrame;
        public static UI_SceneCapture_SetAutoFrameFn UI_SceneCapture_SetAutoFrame;
        public static UI_SceneCapture_GetIncludeChildrenFn UI_SceneCapture_GetIncludeChildren;
        public static UI_SceneCapture_SetIncludeChildrenFn UI_SceneCapture_SetIncludeChildren;
        public static UI_SceneCapture_GetBoundsPaddingFn UI_SceneCapture_GetBoundsPadding;
        public static UI_SceneCapture_SetBoundsPaddingFn UI_SceneCapture_SetBoundsPadding;
        public static UI_SceneCapture_GetFieldOfViewFn UI_SceneCapture_GetFieldOfView;
        public static UI_SceneCapture_SetFieldOfViewFn UI_SceneCapture_SetFieldOfView;
        public static UI_SceneCapture_GetNearClipFn UI_SceneCapture_GetNearClip;
        public static UI_SceneCapture_SetNearClipFn UI_SceneCapture_SetNearClip;
        public static UI_SceneCapture_GetFarClipFn UI_SceneCapture_GetFarClip;
        public static UI_SceneCapture_SetFarClipFn UI_SceneCapture_SetFarClip;
        public static UI_SceneCapture_GetViewDirectionFn UI_SceneCapture_GetViewDirection;
        public static UI_SceneCapture_SetViewDirectionFn UI_SceneCapture_SetViewDirection;
        public static UI_SceneCapture_GetUpDirectionFn UI_SceneCapture_GetUpDirection;
        public static UI_SceneCapture_SetUpDirectionFn UI_SceneCapture_SetUpDirection;
        public static UI_SceneCapture_GetFocusOffsetFn UI_SceneCapture_GetFocusOffset;
        public static UI_SceneCapture_SetFocusOffsetFn UI_SceneCapture_SetFocusOffset;
        public static UI_SceneCapture_GetTargetEntityFn UI_SceneCapture_GetTargetEntity;
        public static UI_SceneCapture_SetTargetEntityFn UI_SceneCapture_SetTargetEntity;
        public static UI_SceneCapture_GetRenderSizeFn UI_SceneCapture_GetRenderSize;
        public static UI_SceneCapture_SetRenderSizeFn UI_SceneCapture_SetRenderSize;
        public static UI_SceneCapture_GetClearColorFn UI_SceneCapture_GetClearColor;
        public static UI_SceneCapture_SetClearColorFn UI_SceneCapture_SetClearColor;
        public static UI_SceneCapture_GetShowGridFn UI_SceneCapture_GetShowGrid;
        public static UI_SceneCapture_SetShowGridFn UI_SceneCapture_SetShowGrid;
        public static UI_SceneCapture_GetLockViewToTargetFn UI_SceneCapture_GetLockViewToTarget;
        public static UI_SceneCapture_SetLockViewToTargetFn UI_SceneCapture_SetLockViewToTarget;

        // UI Slider
        public static UI_Slider_GetValueFn UI_Slider_GetValue;
        public static UI_Slider_SetValueFn UI_Slider_SetValue;
        public static UI_Slider_GetMinValueFn UI_Slider_GetMinValue;
        public static UI_Slider_SetMinValueFn UI_Slider_SetMinValue;
        public static UI_Slider_GetMaxValueFn UI_Slider_GetMaxValue;
        public static UI_Slider_SetMaxValueFn UI_Slider_SetMaxValue;
        public static UI_Slider_IsHoveredFn UI_Slider_IsHovered;
        public static UI_Slider_IsDraggingFn UI_Slider_IsDragging;
        public static UI_Slider_ValueChangedFn UI_Slider_ValueChanged;

        // UI ProgressBar
        public static UI_ProgressBar_GetValueFn UI_ProgressBar_GetValue;
        public static UI_ProgressBar_SetValueFn UI_ProgressBar_SetValue;
        public static UI_ProgressBar_GetMinValueFn UI_ProgressBar_GetMinValue;
        public static UI_ProgressBar_SetMinValueFn UI_ProgressBar_SetMinValue;
        public static UI_ProgressBar_GetMaxValueFn UI_ProgressBar_GetMaxValue;
        public static UI_ProgressBar_SetMaxValueFn UI_ProgressBar_SetMaxValue;
        public static UI_ProgressBar_GetOpacityFn UI_ProgressBar_GetOpacity;
        public static UI_ProgressBar_SetOpacityFn UI_ProgressBar_SetOpacity;
        public static UI_ProgressBar_GetVisibleFn UI_ProgressBar_GetVisible;
        public static UI_ProgressBar_SetVisibleFn UI_ProgressBar_SetVisible;

        // UI Toggle
        public static UI_Toggle_GetIsOnFn UI_Toggle_GetIsOn;
        public static UI_Toggle_SetIsOnFn UI_Toggle_SetIsOn;
        public static UI_Toggle_IsHoveredFn UI_Toggle_IsHovered;
        public static UI_Toggle_IsPressedFn UI_Toggle_IsPressed;
        public static UI_Toggle_ValueChangedFn UI_Toggle_ValueChanged;

        // UI ScrollView
        public static UI_ScrollView_GetContentOffsetFn UI_ScrollView_GetContentOffset;
        public static UI_ScrollView_SetContentOffsetFn UI_ScrollView_SetContentOffset;
        public static UI_ScrollView_GetContentSizeFn UI_ScrollView_GetContentSize;
        public static UI_ScrollView_SetContentSizeFn UI_ScrollView_SetContentSize;
        public static UI_ScrollView_GetOpacityFn UI_ScrollView_GetOpacity;
        public static UI_ScrollView_SetOpacityFn UI_ScrollView_SetOpacity;
        public static UI_ScrollView_GetVisibleFn UI_ScrollView_GetVisible;
        public static UI_ScrollView_SetVisibleFn UI_ScrollView_SetVisible;

        // UI InputField
        public static UI_InputField_GetTextFn UI_InputField_GetTextInternal;
        public static UI_InputField_SetTextFn UI_InputField_SetText;
        public static UI_InputField_GetPlaceholderFn UI_InputField_GetPlaceholderInternal;
        public static UI_InputField_SetPlaceholderFn UI_InputField_SetPlaceholder;
        public static UI_InputField_IsFocusedFn UI_InputField_IsFocused;
        public static UI_InputField_TextChangedFn UI_InputField_TextChanged;

        // UI Dropdown
        public static UI_Dropdown_GetSelectedIndexFn UI_Dropdown_GetSelectedIndex;
        public static UI_Dropdown_SetSelectedIndexFn UI_Dropdown_SetSelectedIndex;
        public static UI_Dropdown_GetOptionCountFn UI_Dropdown_GetOptionCount;
        public static UI_Dropdown_GetOptionFn UI_Dropdown_GetOptionInternal;
        public static UI_Dropdown_SetOptionFn UI_Dropdown_SetOption;
        public static UI_Dropdown_AddOptionFn UI_Dropdown_AddOption;
        public static UI_Dropdown_ClearOptionsFn UI_Dropdown_ClearOptions;
        public static UI_Dropdown_IsOpenFn UI_Dropdown_IsOpen;
        public static UI_Dropdown_SelectionChangedFn UI_Dropdown_SelectionChanged;

        // Character Controller
        public static CC_SetDesiredVelocityFn CC_SetDesiredVelocity;
        public static CC_GetDesiredVelocityFn CC_GetDesiredVelocity;
        public static CC_SetVerticalVelocityFn CC_SetVerticalVelocity;
        public static CC_GetVerticalVelocityFn CC_GetVerticalVelocity;
        public static CC_JumpFn CC_Jump;
        public static CC_IsGroundedFn CC_IsGrounded;
        public static CC_SetPositionFn CC_SetPosition;

        // Animation Layers
        public static AnimLayer_GetOrCreateFn AnimLayer_GetOrCreate;
        public static AnimLayer_RemoveFn AnimLayer_Remove;
        public static AnimLayer_HasFn AnimLayer_Has;
        public static AnimLayer_GetCountFn AnimLayer_GetCount;
        public static AnimLayer_GetNameByIndexFn AnimLayer_GetNameByIndexInternal;
        public static AnimLayer_SetAnimationFn AnimLayer_SetAnimation;
        public static AnimLayer_GetAnimationFn AnimLayer_GetAnimationInternal;
        public static AnimLayer_SetMaskFn AnimLayer_SetMask;
        public static AnimLayer_GetMaskFn AnimLayer_GetMask;
        public static AnimLayer_SetBlendModeFn AnimLayer_SetBlendMode;
        public static AnimLayer_GetBlendModeFn AnimLayer_GetBlendMode;
        public static AnimLayer_SetWeightFn AnimLayer_SetWeight;
        public static AnimLayer_GetWeightFn AnimLayer_GetWeight;
        public static AnimLayer_BlendToFn AnimLayer_BlendTo;
        public static AnimLayer_PlayFn AnimLayer_Play;
        public static AnimLayer_StopFn AnimLayer_Stop;
        public static AnimLayer_IsPlayingFn AnimLayer_IsPlaying;
        public static AnimLayer_SetSpeedFn AnimLayer_SetSpeed;
        public static AnimLayer_GetSpeedFn AnimLayer_GetSpeed;
        public static AnimLayer_SetTimeFn AnimLayer_SetTime;
        public static AnimLayer_SetNormalizedTimeFn AnimLayer_SetNormalizedTime;
        public static AnimLayer_SetStateFn AnimLayer_SetState;
        public static AnimLayer_SetStateByNameFn AnimLayer_SetStateByName;
        public static AnimLayer_SetBlend2DFn AnimLayer_SetBlend2D;
        public static AnimLayer_GetTimeFn AnimLayer_GetTime;
        public static AnimLayer_GetDurationFn AnimLayer_GetDuration;
        public static AnimLayer_SetLoopingFn AnimLayer_SetLooping;
        public static AnimLayer_GetLoopingFn AnimLayer_GetLooping;

        // Helper methods for string returns
        public static string AnimLayer_GetNameByIndex(int entityId, int index)
        {
            if (AnimLayer_GetNameByIndexInternal == null) return string.Empty;
            IntPtr ptr = AnimLayer_GetNameByIndexInternal(entityId, index);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) : string.Empty;
        }

        public static string AnimLayer_GetAnimation(int entityId, string layerName)
        {
            if (AnimLayer_GetAnimationInternal == null) return string.Empty;
            IntPtr ptr = AnimLayer_GetAnimationInternal(entityId, layerName);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) : string.Empty;
        }

        public static string GetBlendShapeName(int entityId, int index)
        {
            IntPtr ptr = GetBlendShapeNameInternal(entityId, index);
            return Marshal.PtrToStringAnsi(ptr);
        }

        public static string UnifiedMorph_GetName(int entityId, int index)
        {
            IntPtr ptr = UnifiedMorph_GetNameInternal(entityId, index);
            return Marshal.PtrToStringAnsi(ptr);
        }

        public static string TintController_GetNamePattern(int entityId)
        {
            IntPtr ptr = TintController_GetNamePatternInternal(entityId);
            return Marshal.PtrToStringAnsi(ptr);
        }

        public static string BoneAttachment_GetBoneName(int entityId)
        {
            if (BoneAttachment_GetBoneNameInternal == null) return string.Empty;
            IntPtr ptr = BoneAttachment_GetBoneNameInternal(entityId);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) : string.Empty;
        }

        public static string Animator_GetState(int entityId)
        {
            if (Animator_GetStateInternal == null) return string.Empty;
            IntPtr ptr = Animator_GetStateInternal(entityId);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) : string.Empty;
        }

        public static string UI_Text_GetFontPath(int entityId)
        {
            UI_Text_GetFontPathInternal(entityId, out IntPtr ptr);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) : string.Empty;
        }

        public static string UI_InputField_GetText(int entityId)
        {
            if (UI_InputField_GetTextInternal == null) return string.Empty;
            UI_InputField_GetTextInternal(entityId, out IntPtr ptr);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) : string.Empty;
        }

        public static string UI_InputField_GetPlaceholder(int entityId)
        {
            if (UI_InputField_GetPlaceholderInternal == null) return string.Empty;
            UI_InputField_GetPlaceholderInternal(entityId, out IntPtr ptr);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) : string.Empty;
        }

        public static string UI_Dropdown_GetOption(int entityId, int index)
        {
            if (UI_Dropdown_GetOptionInternal == null) return string.Empty;
            UI_Dropdown_GetOptionInternal(entityId, index, out IntPtr ptr);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) : string.Empty;
        }

        public static string Portal_GetTargetScenePath(int entityId)
        {
            if (Portal_GetTargetScenePathInternal == null) return string.Empty;
            IntPtr ptr = Portal_GetTargetScenePathInternal(entityId);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) : string.Empty;
        }

        public static string Portal_GetTargetPortalGuid(int entityId)
        {
            if (Portal_GetTargetPortalGuidInternal == null) return string.Empty;
            IntPtr ptr = Portal_GetTargetPortalGuidInternal(entityId);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) : string.Empty;
        }

        public static string Portal_GetTargetPortalPath(int entityId)
        {
            if (Portal_GetTargetPortalPathInternal == null) return string.Empty;
            IntPtr ptr = Portal_GetTargetPortalPathInternal(entityId);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) : string.Empty;
        }

        public static string RigidBody_GetDebugSummary(int entityId)
        {
            if (RigidBody_GetDebugSummaryInternal == null) return string.Empty;
            IntPtr ptr = RigidBody_GetDebugSummaryInternal(entityId);
            return ptr != IntPtr.Zero ? Marshal.PtrToStringAnsi(ptr) : string.Empty;
        }

        private static T SafeGetDelegate<T>(IntPtr ptr, int index, string name) where T : Delegate
        {
            if (ptr == IntPtr.Zero)
            {
                Console.WriteLine($"[ComponentInterop] WARNING: Null pointer at index {index} for {name}");
                return null;
            }
            try
            {
                return Marshal.GetDelegateForFunctionPointer<T>(ptr);
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[ComponentInterop] ERROR creating delegate at index {index} for {name}: {ex.Message}");
                return null;
            }
        }

        public static void Initialize(void** ptrs, int count)
        {
            Console.WriteLine($"[ComponentInterop] Initialize called with count={count}");
            
            if (count < InteropCount)
            {
                Console.WriteLine($"[ComponentInterop] Expected at least {InteropCount} function pointers, but got {count}.");
                return;
            }

            int i = 0;
            // ManagedLog first so it's available for logging during initialization
            ManagedLog = SafeGetDelegate<ManagedLogFn>((IntPtr)ptrs[i], i, "ManagedLog"); i++;
            
            HasComponent = SafeGetDelegate<HasComponentFn>((IntPtr)ptrs[i], i, "HasComponent"); i++;
            AddComponent = SafeGetDelegate<AddComponentFn>((IntPtr)ptrs[i], i, "AddComponent"); i++;
            RemoveComponent = SafeGetDelegate<RemoveComponentFn>((IntPtr)ptrs[i], i, "RemoveComponent"); i++;
            Console.WriteLine($"[ComponentInterop] After core component funcs i={i}");
            
            GetLightType = SafeGetDelegate<GetLightTypeFn>((IntPtr)ptrs[i], i, "GetLightType"); i++;
            SetLightType = SafeGetDelegate<SetLightTypeFn>((IntPtr)ptrs[i], i, "SetLightType"); i++;
            GetLightColor = SafeGetDelegate<GetLightColorFn>((IntPtr)ptrs[i], i, "GetLightColor"); i++;
            SetLightColor = SafeGetDelegate<SetLightColorFn>((IntPtr)ptrs[i], i, "SetLightColor"); i++;
            GetLightIntensity = SafeGetDelegate<GetLightIntensityFn>((IntPtr)ptrs[i], i, "GetLightIntensity"); i++;
            SetLightIntensity = SafeGetDelegate<SetLightIntensityFn>((IntPtr)ptrs[i], i, "SetLightIntensity"); i++;
            Console.WriteLine($"[ComponentInterop] After light funcs i={i}");

            GetRigidBodyMass = SafeGetDelegate<GetRigidBodyMassFn>((IntPtr)ptrs[i], i, "GetRigidBodyMass"); i++;
            SetRigidBodyMass = SafeGetDelegate<SetRigidBodyMassFn>((IntPtr)ptrs[i], i, "SetRigidBodyMass"); i++;
            GetRigidBodyIsKinematic = SafeGetDelegate<GetRigidBodyIsKinematicFn>((IntPtr)ptrs[i], i, "GetRigidBodyIsKinematic"); i++;
            SetRigidBodyIsKinematic = SafeGetDelegate<SetRigidBodyIsKinematicFn>((IntPtr)ptrs[i], i, "SetRigidBodyIsKinematic"); i++;
            GetRigidBodyUseGravity = SafeGetDelegate<GetRigidBodyUseGravityFn>((IntPtr)ptrs[i], i, "GetRigidBodyUseGravity"); i++;
            SetRigidBodyUseGravity = SafeGetDelegate<SetRigidBodyUseGravityFn>((IntPtr)ptrs[i], i, "SetRigidBodyUseGravity"); i++;
            GetRigidBodyCollisionMask = SafeGetDelegate<GetRigidBodyCollisionMaskFn>((IntPtr)ptrs[i], i, "GetRigidBodyCollisionMask"); i++;
            SetRigidBodyCollisionMask = SafeGetDelegate<SetRigidBodyCollisionMaskFn>((IntPtr)ptrs[i], i, "SetRigidBodyCollisionMask"); i++;
            GetRigidBodyLinearVelocity = SafeGetDelegate<GetRigidBodyLinearVelocityFn>((IntPtr)ptrs[i], i, "GetRigidBodyLinearVelocity"); i++;
            SetRigidBodyLinearVelocity = SafeGetDelegate<SetRigidBodyLinearVelocityFn>((IntPtr)ptrs[i], i, "SetRigidBodyLinearVelocity"); i++;
            GetRigidBodyAngularVelocity = SafeGetDelegate<GetRigidBodyAngularVelocityFn>((IntPtr)ptrs[i], i, "GetRigidBodyAngularVelocity"); i++;
            SetRigidBodyAngularVelocity = SafeGetDelegate<SetRigidBodyAngularVelocityFn>((IntPtr)ptrs[i], i, "SetRigidBodyAngularVelocity"); i++;
            ApplyRigidBodyForce = SafeGetDelegate<ApplyRigidBodyForceFn>((IntPtr)ptrs[i], i, "ApplyRigidBodyForce"); i++;
            ApplyRigidBodyTorque = SafeGetDelegate<ApplyRigidBodyTorqueFn>((IntPtr)ptrs[i], i, "ApplyRigidBodyTorque"); i++;
            ApplyRigidBodyImpulse = SafeGetDelegate<ApplyRigidBodyImpulseFn>((IntPtr)ptrs[i], i, "ApplyRigidBodyImpulse"); i++;
            ApplyRigidBodyAngularImpulse = SafeGetDelegate<ApplyRigidBodyAngularImpulseFn>((IntPtr)ptrs[i], i, "ApplyRigidBodyAngularImpulse"); i++;
            RigidBody_GetDebugSummaryInternal = SafeGetDelegate<RigidBody_GetDebugSummaryFn>((IntPtr)ptrs[i], i, "RigidBody_GetDebugSummary"); i++;
            Collider_GetOffset = SafeGetDelegate<Collider_GetOffsetFn>((IntPtr)ptrs[i], i, "Collider_GetOffset"); i++;
            Console.WriteLine($"[ComponentInterop] After rigidbody funcs i={i}");
            
            // Camera layer mask
            GetCameraLayerMask = SafeGetDelegate<GetCameraLayerMaskFn>((IntPtr)ptrs[i], i, "GetCameraLayerMask"); i++;
            SetCameraLayerMask = SafeGetDelegate<SetCameraLayerMaskFn>((IntPtr)ptrs[i], i, "SetCameraLayerMask"); i++;
            Camera_SetLayerMaskByName = SafeGetDelegate<Camera_SetLayerMaskByNameFn>((IntPtr)ptrs[i], i, "Camera_SetLayerMaskByName"); i++;
            // Camera settings (12 entries)
            GetCameraActive = SafeGetDelegate<GetCameraActiveFn>((IntPtr)ptrs[i], i, "GetCameraActive"); i++;
            SetCameraActive = SafeGetDelegate<SetCameraActiveFn>((IntPtr)ptrs[i], i, "SetCameraActive"); i++;
            GetCameraPriority = SafeGetDelegate<GetCameraPriorityFn>((IntPtr)ptrs[i], i, "GetCameraPriority"); i++;
            SetCameraPriority = SafeGetDelegate<SetCameraPriorityFn>((IntPtr)ptrs[i], i, "SetCameraPriority"); i++;
            GetCameraFieldOfView = SafeGetDelegate<GetCameraFieldOfViewFn>((IntPtr)ptrs[i], i, "GetCameraFieldOfView"); i++;
            SetCameraFieldOfView = SafeGetDelegate<SetCameraFieldOfViewFn>((IntPtr)ptrs[i], i, "SetCameraFieldOfView"); i++;
            GetCameraNearClip = SafeGetDelegate<GetCameraNearClipFn>((IntPtr)ptrs[i], i, "GetCameraNearClip"); i++;
            SetCameraNearClip = SafeGetDelegate<SetCameraNearClipFn>((IntPtr)ptrs[i], i, "SetCameraNearClip"); i++;
            GetCameraFarClip = SafeGetDelegate<GetCameraFarClipFn>((IntPtr)ptrs[i], i, "GetCameraFarClip"); i++;
            SetCameraFarClip = SafeGetDelegate<SetCameraFarClipFn>((IntPtr)ptrs[i], i, "SetCameraFarClip"); i++;
            GetCameraIsPerspective = SafeGetDelegate<GetCameraIsPerspectiveFn>((IntPtr)ptrs[i], i, "GetCameraIsPerspective"); i++;
            SetCameraIsPerspective = SafeGetDelegate<SetCameraIsPerspectiveFn>((IntPtr)ptrs[i], i, "SetCameraIsPerspective"); i++;
            Console.WriteLine($"[ComponentInterop] After camera funcs i={i}");

            SetBlendShapeWeight = SafeGetDelegate<SetBlendShapeWeightFn>((IntPtr)ptrs[i], i, "SetBlendShapeWeight"); i++;
            GetBlendShapeWeight = SafeGetDelegate<GetBlendShapeWeightFn>((IntPtr)ptrs[i], i, "GetBlendShapeWeight"); i++;
            GetBlendShapeCount = SafeGetDelegate<GetBlendShapeCountFn>((IntPtr)ptrs[i], i, "GetBlendShapeCount"); i++;
            GetBlendShapeNameInternal = SafeGetDelegate<GetBlendShapeNameFn>((IntPtr)ptrs[i], i, "GetBlendShapeNameInternal"); i++;
            Console.WriteLine($"[ComponentInterop] After blendshape funcs i={i}");

            Animator_SetBool = SafeGetDelegate<Animator_SetBoolFn>((IntPtr)ptrs[i], i, "Animator_SetBool"); i++;
            Animator_SetInt = SafeGetDelegate<Animator_SetIntFn>((IntPtr)ptrs[i], i, "Animator_SetInt"); i++;
            Animator_SetFloat = SafeGetDelegate<Animator_SetFloatFn>((IntPtr)ptrs[i], i, "Animator_SetFloat"); i++;
            Animator_SetTrigger = SafeGetDelegate<Animator_SetTriggerFn>((IntPtr)ptrs[i], i, "Animator_SetTrigger"); i++;
            Animator_ResetTrigger = SafeGetDelegate<Animator_ResetTriggerFn>((IntPtr)ptrs[i], i, "Animator_ResetTrigger"); i++;
            Animator_GetBool = SafeGetDelegate<Animator_GetBoolFn>((IntPtr)ptrs[i], i, "Animator_GetBool"); i++;
            Animator_GetInt = SafeGetDelegate<Animator_GetIntFn>((IntPtr)ptrs[i], i, "Animator_GetInt"); i++;
            Animator_GetFloat = SafeGetDelegate<Animator_GetFloatFn>((IntPtr)ptrs[i], i, "Animator_GetFloat"); i++;
            Animator_GetTrigger = SafeGetDelegate<Animator_GetTriggerFn>((IntPtr)ptrs[i], i, "Animator_GetTrigger"); i++;
            Animator_GetStateInternal = SafeGetDelegate<Animator_GetStateFn>((IntPtr)ptrs[i], i, "Animator_GetState"); i++;
            Animator_GetEnabled = SafeGetDelegate<Animator_GetEnabledFn>((IntPtr)ptrs[i], i, "Animator_GetEnabled"); i++;
            Animator_SetEnabled = SafeGetDelegate<Animator_SetEnabledFn>((IntPtr)ptrs[i], i, "Animator_SetEnabled"); i++;
            Animator_SetController = SafeGetDelegate<Animator_SetControllerFn>((IntPtr)ptrs[i], i, "Animator_SetController"); i++;
            Animator_SetOverride = SafeGetDelegate<Animator_SetOverrideFn>((IntPtr)ptrs[i], i, "Animator_SetOverride"); i++;
            Animator_GetCurrentClipRootMotionMode = SafeGetDelegate<Animator_GetCurrentClipRootMotionModeFn>((IntPtr)ptrs[i], i, "Animator_GetCurrentClipRootMotionMode"); i++;
            NpcScalability_GetParticipates = SafeGetDelegate<NpcScalability_GetParticipatesFn>((IntPtr)ptrs[i], i, "NpcScalability_GetParticipates"); i++;
            NpcScalability_GetTier = SafeGetDelegate<NpcScalability_GetTierFn>((IntPtr)ptrs[i], i, "NpcScalability_GetTier"); i++;
            NpcScalability_GetRepresentation = SafeGetDelegate<NpcScalability_GetRepresentationFn>((IntPtr)ptrs[i], i, "NpcScalability_GetRepresentation"); i++;
            NpcScalability_GetReasonFlags = SafeGetDelegate<NpcScalability_GetReasonFlagsFn>((IntPtr)ptrs[i], i, "NpcScalability_GetReasonFlags"); i++;
            NpcScalability_GetCameraDistance = SafeGetDelegate<NpcScalability_GetCameraDistanceFn>((IntPtr)ptrs[i], i, "NpcScalability_GetCameraDistance"); i++;
            NpcScalability_GetVisible = SafeGetDelegate<NpcScalability_GetVisibleFn>((IntPtr)ptrs[i], i, "NpcScalability_GetVisible"); i++;
            NpcScalability_GetCrowdRank = SafeGetDelegate<NpcScalability_GetCrowdRankFn>((IntPtr)ptrs[i], i, "NpcScalability_GetCrowdRank"); i++;
            NpcScalability_GetCrowdCount = SafeGetDelegate<NpcScalability_GetCrowdCountFn>((IntPtr)ptrs[i], i, "NpcScalability_GetCrowdCount"); i++;
            NpcScalability_GetAnimationUpdateInterval = SafeGetDelegate<NpcScalability_GetAnimationUpdateIntervalFn>((IntPtr)ptrs[i], i, "NpcScalability_GetAnimationUpdateInterval"); i++;
            NpcScalability_GetScriptUpdateInterval = SafeGetDelegate<NpcScalability_GetScriptUpdateIntervalFn>((IntPtr)ptrs[i], i, "NpcScalability_GetScriptUpdateInterval"); i++;
            NpcScalability_GetNavigationRepathInterval = SafeGetDelegate<NpcScalability_GetNavigationRepathIntervalFn>((IntPtr)ptrs[i], i, "NpcScalability_GetNavigationRepathInterval"); i++;
            Console.WriteLine($"[ComponentInterop] After animator funcs i={i}");

            // UI Buttons
            UI_ButtonIsHovered = SafeGetDelegate<UI_ButtonIsHoveredFn>((IntPtr)ptrs[i], i, "UI_ButtonIsHovered"); i++;
            UI_ButtonIsPressed = SafeGetDelegate<UI_ButtonIsPressedFn>((IntPtr)ptrs[i], i, "UI_ButtonIsPressed"); i++;
            UI_ButtonWasClicked = SafeGetDelegate<UI_ButtonWasClickedFn>((IntPtr)ptrs[i], i, "UI_ButtonWasClicked"); i++;
            Console.WriteLine($"[ComponentInterop] After UI button funcs i={i}");

            // UnifiedMorphs
            UnifiedMorph_GetCount = SafeGetDelegate<UnifiedMorph_GetCountFn>((IntPtr)ptrs[i], i, "UnifiedMorph_GetCount"); i++;
            UnifiedMorph_GetNameInternal = SafeGetDelegate<UnifiedMorph_GetNameFn>((IntPtr)ptrs[i], i, "UnifiedMorph_GetNameInternal"); i++;
            UnifiedMorph_GetWeight = SafeGetDelegate<UnifiedMorph_GetWeightFn>((IntPtr)ptrs[i], i, "UnifiedMorph_GetWeight"); i++;
            UnifiedMorph_SetWeight = SafeGetDelegate<UnifiedMorph_SetWeightFn>((IntPtr)ptrs[i], i, "UnifiedMorph_SetWeight"); i++;
            UnifiedMorph_PropagateAll = SafeGetDelegate<UnifiedMorph_PropagateAllFn>((IntPtr)ptrs[i], i, "UnifiedMorph_PropagateAll"); i++;
            Console.WriteLine($"[ComponentInterop] After UnifiedMorph funcs i={i}");

            // TintMaskController
            TintController_HasComponent = SafeGetDelegate<TintController_HasComponentFn>((IntPtr)ptrs[i], i, "TintController_HasComponent"); i++;
            TintController_GetNamePatternInternal = SafeGetDelegate<TintController_GetNamePatternFn>((IntPtr)ptrs[i], i, "TintController_GetNamePattern"); i++;
            TintController_SetNamePattern = SafeGetDelegate<TintController_SetNamePatternFn>((IntPtr)ptrs[i], i, "TintController_SetNamePattern"); i++;
            TintController_GetBaseTint = SafeGetDelegate<TintController_GetBaseTintFn>((IntPtr)ptrs[i], i, "TintController_GetBaseTint"); i++;
            TintController_SetBaseTint = SafeGetDelegate<TintController_SetBaseTintFn>((IntPtr)ptrs[i], i, "TintController_SetBaseTint"); i++;
            TintController_GetTintColor = SafeGetDelegate<TintController_GetTintColorFn>((IntPtr)ptrs[i], i, "TintController_GetTintColor"); i++;
            TintController_SetTintColor = SafeGetDelegate<TintController_SetTintColorFn>((IntPtr)ptrs[i], i, "TintController_SetTintColor"); i++;
            TintController_GetUseTintMask = SafeGetDelegate<TintController_GetUseTintMaskFn>((IntPtr)ptrs[i], i, "TintController_GetUseTintMask"); i++;
            TintController_SetUseTintMask = SafeGetDelegate<TintController_SetUseTintMaskFn>((IntPtr)ptrs[i], i, "TintController_SetUseTintMask"); i++;
            TintController_GetUsePbrOverrides = SafeGetDelegate<TintController_GetUsePbrOverridesFn>((IntPtr)ptrs[i], i, "TintController_GetUsePbrOverrides"); i++;
            TintController_SetUsePbrOverrides = SafeGetDelegate<TintController_SetUsePbrOverridesFn>((IntPtr)ptrs[i], i, "TintController_SetUsePbrOverrides"); i++;
            TintController_GetPbrMetallic = SafeGetDelegate<TintController_GetPbrMetallicFn>((IntPtr)ptrs[i], i, "TintController_GetPbrMetallic"); i++;
            TintController_SetPbrMetallic = SafeGetDelegate<TintController_SetPbrMetallicFn>((IntPtr)ptrs[i], i, "TintController_SetPbrMetallic"); i++;
            TintController_GetPbrRoughness = SafeGetDelegate<TintController_GetPbrRoughnessFn>((IntPtr)ptrs[i], i, "TintController_GetPbrRoughness"); i++;
            TintController_SetPbrRoughness = SafeGetDelegate<TintController_SetPbrRoughnessFn>((IntPtr)ptrs[i], i, "TintController_SetPbrRoughness"); i++;
            TintController_GetPbrEmissionColor = SafeGetDelegate<TintController_GetPbrEmissionColorFn>((IntPtr)ptrs[i], i, "TintController_GetPbrEmissionColor"); i++;
            TintController_SetPbrEmissionColor = SafeGetDelegate<TintController_SetPbrEmissionColorFn>((IntPtr)ptrs[i], i, "TintController_SetPbrEmissionColor"); i++;
            TintController_GetPbrEmissionStrength = SafeGetDelegate<TintController_GetPbrEmissionStrengthFn>((IntPtr)ptrs[i], i, "TintController_GetPbrEmissionStrength"); i++;
            TintController_SetPbrEmissionStrength = SafeGetDelegate<TintController_SetPbrEmissionStrengthFn>((IntPtr)ptrs[i], i, "TintController_SetPbrEmissionStrength"); i++;
            TintController_GetGlobalBlendMode = SafeGetDelegate<TintController_GetGlobalBlendModeFn>((IntPtr)ptrs[i], i, "TintController_GetGlobalBlendMode"); i++;
            TintController_SetGlobalBlendMode = SafeGetDelegate<TintController_SetGlobalBlendModeFn>((IntPtr)ptrs[i], i, "TintController_SetGlobalBlendMode"); i++;
            TintController_GetAutoIncludeParentedSkinnedMeshes = SafeGetDelegate<TintController_GetAutoIncludeParentedSkinnedMeshesFn>((IntPtr)ptrs[i], i, "TintController_GetAutoIncludeParentedSkinnedMeshes"); i++;
            TintController_SetAutoIncludeParentedSkinnedMeshes = SafeGetDelegate<TintController_SetAutoIncludeParentedSkinnedMeshesFn>((IntPtr)ptrs[i], i, "TintController_SetAutoIncludeParentedSkinnedMeshes"); i++;
            TintController_Refresh = SafeGetDelegate<TintController_RefreshFn>((IntPtr)ptrs[i], i, "TintController_Refresh"); i++;
            TintController_ClearTargets = SafeGetDelegate<TintController_ClearTargetsFn>((IntPtr)ptrs[i], i, "TintController_ClearTargets"); i++;
            TintController_RemoveTargetsForEntity = SafeGetDelegate<TintController_RemoveTargetsForEntityFn>((IntPtr)ptrs[i], i, "TintController_RemoveTargetsForEntity"); i++;
            TintController_AddTarget = SafeGetDelegate<TintController_AddTargetFn>((IntPtr)ptrs[i], i, "TintController_AddTarget"); i++;
            TintController_GetTrackedTargetCount = SafeGetDelegate<TintController_GetTrackedTargetCountFn>((IntPtr)ptrs[i], i, "TintController_GetTrackedTargetCount"); i++;
            TintController_GetTrackedTargetEntity = SafeGetDelegate<TintController_GetTrackedTargetEntityFn>((IntPtr)ptrs[i], i, "TintController_GetTrackedTargetEntity"); i++;
            Console.WriteLine($"[ComponentInterop] After TintController funcs i={i}");

            // BoneAttachment (19 entries)
            BoneAttachment_HasComponent = SafeGetDelegate<BoneAttachment_HasComponentFn>((IntPtr)ptrs[i], i, "BoneAttachment_HasComponent"); i++;
            BoneAttachment_GetEnabled = SafeGetDelegate<BoneAttachment_GetEnabledFn>((IntPtr)ptrs[i], i, "BoneAttachment_GetEnabled"); i++;
            BoneAttachment_SetEnabled = SafeGetDelegate<BoneAttachment_SetEnabledFn>((IntPtr)ptrs[i], i, "BoneAttachment_SetEnabled"); i++;
            BoneAttachment_GetBoneNameInternal = SafeGetDelegate<BoneAttachment_GetBoneNameFn>((IntPtr)ptrs[i], i, "BoneAttachment_GetBoneName"); i++;
            BoneAttachment_SetBoneName = SafeGetDelegate<BoneAttachment_SetBoneNameFn>((IntPtr)ptrs[i], i, "BoneAttachment_SetBoneName"); i++;
            BoneAttachment_GetLocalPosition = SafeGetDelegate<BoneAttachment_GetLocalPositionFn>((IntPtr)ptrs[i], i, "BoneAttachment_GetLocalPosition"); i++;
            BoneAttachment_SetLocalPosition = SafeGetDelegate<BoneAttachment_SetLocalPositionFn>((IntPtr)ptrs[i], i, "BoneAttachment_SetLocalPosition"); i++;
            BoneAttachment_GetLocalRotation = SafeGetDelegate<BoneAttachment_GetLocalRotationFn>((IntPtr)ptrs[i], i, "BoneAttachment_GetLocalRotation"); i++;
            BoneAttachment_SetLocalRotation = SafeGetDelegate<BoneAttachment_SetLocalRotationFn>((IntPtr)ptrs[i], i, "BoneAttachment_SetLocalRotation"); i++;
            BoneAttachment_GetLocalScale = SafeGetDelegate<BoneAttachment_GetLocalScaleFn>((IntPtr)ptrs[i], i, "BoneAttachment_GetLocalScale"); i++;
            BoneAttachment_SetLocalScale = SafeGetDelegate<BoneAttachment_SetLocalScaleFn>((IntPtr)ptrs[i], i, "BoneAttachment_SetLocalScale"); i++;
            BoneAttachment_GetInheritRotation = SafeGetDelegate<BoneAttachment_GetInheritRotationFn>((IntPtr)ptrs[i], i, "BoneAttachment_GetInheritRotation"); i++;
            BoneAttachment_SetInheritRotation = SafeGetDelegate<BoneAttachment_SetInheritRotationFn>((IntPtr)ptrs[i], i, "BoneAttachment_SetInheritRotation"); i++;
            BoneAttachment_GetInheritScale = SafeGetDelegate<BoneAttachment_GetInheritScaleFn>((IntPtr)ptrs[i], i, "BoneAttachment_GetInheritScale"); i++;
            BoneAttachment_SetInheritScale = SafeGetDelegate<BoneAttachment_SetInheritScaleFn>((IntPtr)ptrs[i], i, "BoneAttachment_SetInheritScale"); i++;
            BoneAttachment_IsResolved = SafeGetDelegate<BoneAttachment_IsResolvedFn>((IntPtr)ptrs[i], i, "BoneAttachment_IsResolved"); i++;
            BoneAttachment_InvalidateResolution = SafeGetDelegate<BoneAttachment_InvalidateResolutionFn>((IntPtr)ptrs[i], i, "BoneAttachment_InvalidateResolution"); i++;
            BoneAttachment_GetSkeletonEntity = SafeGetDelegate<BoneAttachment_GetSkeletonEntityFn>((IntPtr)ptrs[i], i, "BoneAttachment_GetSkeletonEntity"); i++;
            BoneAttachment_SetSkeletonEntity = SafeGetDelegate<BoneAttachment_SetSkeletonEntityFn>((IntPtr)ptrs[i], i, "BoneAttachment_SetSkeletonEntity"); i++;
            Console.WriteLine($"[ComponentInterop] After BoneAttachment funcs i={i}");

            // Character Controller (7 entries)
            CC_SetDesiredVelocity = SafeGetDelegate<CC_SetDesiredVelocityFn>((IntPtr)ptrs[i], i, "CC_SetDesiredVelocity"); i++;
            CC_GetDesiredVelocity = SafeGetDelegate<CC_GetDesiredVelocityFn>((IntPtr)ptrs[i], i, "CC_GetDesiredVelocity"); i++;
            CC_SetVerticalVelocity = SafeGetDelegate<CC_SetVerticalVelocityFn>((IntPtr)ptrs[i], i, "CC_SetVerticalVelocity"); i++;
            CC_GetVerticalVelocity = SafeGetDelegate<CC_GetVerticalVelocityFn>((IntPtr)ptrs[i], i, "CC_GetVerticalVelocity"); i++;
            CC_Jump = SafeGetDelegate<CC_JumpFn>((IntPtr)ptrs[i], i, "CC_Jump"); i++;
            CC_IsGrounded = SafeGetDelegate<CC_IsGroundedFn>((IntPtr)ptrs[i], i, "CC_IsGrounded"); i++;
            CC_SetPosition = SafeGetDelegate<CC_SetPositionFn>((IntPtr)ptrs[i], i, "CC_SetPosition"); i++;
            Console.WriteLine($"[ComponentInterop] After CharacterController funcs i={i}");

            // UI Text functions (42 entries)
            if (i + 42 <= count)
            {
                UI_Text_GetText = SafeGetDelegate<UI_Text_GetTextFn>((IntPtr)ptrs[i], i, "UI_Text_GetText"); i++;
                UI_Text_SetText = SafeGetDelegate<UI_Text_SetTextFn>((IntPtr)ptrs[i], i, "UI_Text_SetText"); i++;
                UI_Text_GetOpacity = SafeGetDelegate<UI_Text_GetOpacityFn>((IntPtr)ptrs[i], i, "UI_Text_GetOpacity"); i++;
                UI_Text_SetOpacity = SafeGetDelegate<UI_Text_SetOpacityFn>((IntPtr)ptrs[i], i, "UI_Text_SetOpacity"); i++;
                UI_Text_GetVisible = SafeGetDelegate<UI_Text_GetVisibleFn>((IntPtr)ptrs[i], i, "UI_Text_GetVisible"); i++;
                UI_Text_SetVisible = SafeGetDelegate<UI_Text_SetVisibleFn>((IntPtr)ptrs[i], i, "UI_Text_SetVisible"); i++;
                UI_Text_GetColor = SafeGetDelegate<UI_Text_GetColorFn>((IntPtr)ptrs[i], i, "UI_Text_GetColor"); i++;
                UI_Text_SetColor = SafeGetDelegate<UI_Text_SetColorFn>((IntPtr)ptrs[i], i, "UI_Text_SetColor"); i++;
                UI_Text_GetPixelSize = SafeGetDelegate<UI_Text_GetPixelSizeFn>((IntPtr)ptrs[i], i, "UI_Text_GetPixelSize"); i++;
                UI_Text_SetPixelSize = SafeGetDelegate<UI_Text_SetPixelSizeFn>((IntPtr)ptrs[i], i, "UI_Text_SetPixelSize"); i++;
                UI_Text_GetZOrder = SafeGetDelegate<UI_Text_GetZOrderFn>((IntPtr)ptrs[i], i, "UI_Text_GetZOrder"); i++;
                UI_Text_SetZOrder = SafeGetDelegate<UI_Text_SetZOrderFn>((IntPtr)ptrs[i], i, "UI_Text_SetZOrder"); i++;
                UI_Text_GetFontPathInternal = SafeGetDelegate<UI_Text_GetFontPathFn>((IntPtr)ptrs[i], i, "UI_Text_GetFontPath"); i++;
                UI_Text_SetFontPath = SafeGetDelegate<UI_Text_SetFontPathFn>((IntPtr)ptrs[i], i, "UI_Text_SetFontPath"); i++;
                UI_Text_GetAnchorEnabled = SafeGetDelegate<UI_Text_GetAnchorEnabledFn>((IntPtr)ptrs[i], i, "UI_Text_GetAnchorEnabled"); i++;
                UI_Text_SetAnchorEnabled = SafeGetDelegate<UI_Text_SetAnchorEnabledFn>((IntPtr)ptrs[i], i, "UI_Text_SetAnchorEnabled"); i++;
                UI_Text_GetAnchor = SafeGetDelegate<UI_Text_GetAnchorFn>((IntPtr)ptrs[i], i, "UI_Text_GetAnchor"); i++;
                UI_Text_SetAnchor = SafeGetDelegate<UI_Text_SetAnchorFn>((IntPtr)ptrs[i], i, "UI_Text_SetAnchor"); i++;
                UI_Text_GetAnchorOffset = SafeGetDelegate<UI_Text_GetAnchorOffsetFn>((IntPtr)ptrs[i], i, "UI_Text_GetAnchorOffset"); i++;
                UI_Text_SetAnchorOffset = SafeGetDelegate<UI_Text_SetAnchorOffsetFn>((IntPtr)ptrs[i], i, "UI_Text_SetAnchorOffset"); i++;
                UI_Text_GetWordWrap = SafeGetDelegate<UI_Text_GetWordWrapFn>((IntPtr)ptrs[i], i, "UI_Text_GetWordWrap"); i++;
                UI_Text_SetWordWrap = SafeGetDelegate<UI_Text_SetWordWrapFn>((IntPtr)ptrs[i], i, "UI_Text_SetWordWrap"); i++;
                UI_Text_GetRectSize = SafeGetDelegate<UI_Text_GetRectSizeFn>((IntPtr)ptrs[i], i, "UI_Text_GetRectSize"); i++;
                UI_Text_SetRectSize = SafeGetDelegate<UI_Text_SetRectSizeFn>((IntPtr)ptrs[i], i, "UI_Text_SetRectSize"); i++;
                UI_Text_GetWorldSpace = SafeGetDelegate<UI_Text_GetWorldSpaceFn>((IntPtr)ptrs[i], i, "UI_Text_GetWorldSpace"); i++;
                UI_Text_SetWorldSpace = SafeGetDelegate<UI_Text_SetWorldSpaceFn>((IntPtr)ptrs[i], i, "UI_Text_SetWorldSpace"); i++;
                UI_Text_GetBillboard = SafeGetDelegate<UI_Text_GetBillboardFn>((IntPtr)ptrs[i], i, "UI_Text_GetBillboard"); i++;
                UI_Text_SetBillboard = SafeGetDelegate<UI_Text_SetBillboardFn>((IntPtr)ptrs[i], i, "UI_Text_SetBillboard"); i++;
                UI_Text_GetOutlineEnabled = SafeGetDelegate<UI_Text_GetOutlineEnabledFn>((IntPtr)ptrs[i], i, "UI_Text_GetOutlineEnabled"); i++;
                UI_Text_SetOutlineEnabled = SafeGetDelegate<UI_Text_SetOutlineEnabledFn>((IntPtr)ptrs[i], i, "UI_Text_SetOutlineEnabled"); i++;
                UI_Text_GetOutlineColor = SafeGetDelegate<UI_Text_GetOutlineColorFn>((IntPtr)ptrs[i], i, "UI_Text_GetOutlineColor"); i++;
                UI_Text_SetOutlineColor = SafeGetDelegate<UI_Text_SetOutlineColorFn>((IntPtr)ptrs[i], i, "UI_Text_SetOutlineColor"); i++;
                UI_Text_GetOutlineThickness = SafeGetDelegate<UI_Text_GetOutlineThicknessFn>((IntPtr)ptrs[i], i, "UI_Text_GetOutlineThickness"); i++;
                UI_Text_SetOutlineThickness = SafeGetDelegate<UI_Text_SetOutlineThicknessFn>((IntPtr)ptrs[i], i, "UI_Text_SetOutlineThickness"); i++;
                UI_Text_GetShadowEnabled = SafeGetDelegate<UI_Text_GetShadowEnabledFn>((IntPtr)ptrs[i], i, "UI_Text_GetShadowEnabled"); i++;
                UI_Text_SetShadowEnabled = SafeGetDelegate<UI_Text_SetShadowEnabledFn>((IntPtr)ptrs[i], i, "UI_Text_SetShadowEnabled"); i++;
                UI_Text_GetShadowColor = SafeGetDelegate<UI_Text_GetShadowColorFn>((IntPtr)ptrs[i], i, "UI_Text_GetShadowColor"); i++;
                UI_Text_SetShadowColor = SafeGetDelegate<UI_Text_SetShadowColorFn>((IntPtr)ptrs[i], i, "UI_Text_SetShadowColor"); i++;
                UI_Text_GetShadowOffset = SafeGetDelegate<UI_Text_GetShadowOffsetFn>((IntPtr)ptrs[i], i, "UI_Text_GetShadowOffset"); i++;
                UI_Text_SetShadowOffset = SafeGetDelegate<UI_Text_SetShadowOffsetFn>((IntPtr)ptrs[i], i, "UI_Text_SetShadowOffset"); i++;
                UI_Text_GetAlignment = SafeGetDelegate<UI_Text_GetAlignmentFn>((IntPtr)ptrs[i], i, "UI_Text_GetAlignment"); i++;
                UI_Text_SetAlignment = SafeGetDelegate<UI_Text_SetAlignmentFn>((IntPtr)ptrs[i], i, "UI_Text_SetAlignment"); i++;
                Console.WriteLine($"[ComponentInterop] After UI text funcs i={i}");
            }
            // UI Panel functions (34 entries: 30 base + 2 texture + 2 child opacity)
            if (i + 34 <= count)
            {
                UI_Panel_GetOpacity = SafeGetDelegate<UI_Panel_GetOpacityFn>((IntPtr)ptrs[i], i, "UI_Panel_GetOpacity"); i++;
                UI_Panel_SetOpacity = SafeGetDelegate<UI_Panel_SetOpacityFn>((IntPtr)ptrs[i], i, "UI_Panel_SetOpacity"); i++;
                UI_Panel_GetVisible = SafeGetDelegate<UI_Panel_GetVisibleFn>((IntPtr)ptrs[i], i, "UI_Panel_GetVisible"); i++;
                UI_Panel_SetVisible = SafeGetDelegate<UI_Panel_SetVisibleFn>((IntPtr)ptrs[i], i, "UI_Panel_SetVisible"); i++;
                UI_Panel_GetSize = SafeGetDelegate<UI_Panel_GetSizeFn>((IntPtr)ptrs[i], i, "UI_Panel_GetSize"); i++;
                UI_Panel_SetSize = SafeGetDelegate<UI_Panel_SetSizeFn>((IntPtr)ptrs[i], i, "UI_Panel_SetSize"); i++;
                UI_Panel_GetTintColor = SafeGetDelegate<UI_Panel_GetTintColorFn>((IntPtr)ptrs[i], i, "UI_Panel_GetTintColor"); i++;
                UI_Panel_SetTintColor = SafeGetDelegate<UI_Panel_SetTintColorFn>((IntPtr)ptrs[i], i, "UI_Panel_SetTintColor"); i++;
                UI_Panel_GetAnchorEnabled = SafeGetDelegate<UI_Panel_GetAnchorEnabledFn>((IntPtr)ptrs[i], i, "UI_Panel_GetAnchorEnabled"); i++;
                UI_Panel_SetAnchorEnabled = SafeGetDelegate<UI_Panel_SetAnchorEnabledFn>((IntPtr)ptrs[i], i, "UI_Panel_SetAnchorEnabled"); i++;
                UI_Panel_GetAnchor = SafeGetDelegate<UI_Panel_GetAnchorFn>((IntPtr)ptrs[i], i, "UI_Panel_GetAnchor"); i++;
                UI_Panel_SetAnchor = SafeGetDelegate<UI_Panel_SetAnchorFn>((IntPtr)ptrs[i], i, "UI_Panel_SetAnchor"); i++;
                UI_Panel_GetAnchorOffset = SafeGetDelegate<UI_Panel_GetAnchorOffsetFn>((IntPtr)ptrs[i], i, "UI_Panel_GetAnchorOffset"); i++;
                UI_Panel_SetAnchorOffset = SafeGetDelegate<UI_Panel_SetAnchorOffsetFn>((IntPtr)ptrs[i], i, "UI_Panel_SetAnchorOffset"); i++;
                UI_Panel_GetAnchorToParent = SafeGetDelegate<UI_Panel_GetAnchorToParentFn>((IntPtr)ptrs[i], i, "UI_Panel_GetAnchorToParent"); i++;
                UI_Panel_SetAnchorToParent = SafeGetDelegate<UI_Panel_SetAnchorToParentFn>((IntPtr)ptrs[i], i, "UI_Panel_SetAnchorToParent"); i++;
                UI_Panel_GetZOrder = SafeGetDelegate<UI_Panel_GetZOrderFn>((IntPtr)ptrs[i], i, "UI_Panel_GetZOrder"); i++;
                UI_Panel_SetZOrder = SafeGetDelegate<UI_Panel_SetZOrderFn>((IntPtr)ptrs[i], i, "UI_Panel_SetZOrder"); i++;
                UI_Panel_IsHovered = SafeGetDelegate<UI_Panel_IsHoveredFn>((IntPtr)ptrs[i], i, "UI_Panel_IsHovered"); i++;
                UI_Panel_IsPressed = SafeGetDelegate<UI_Panel_IsPressedFn>((IntPtr)ptrs[i], i, "UI_Panel_IsPressed"); i++;
                UI_Panel_IsDragging = SafeGetDelegate<UI_Panel_IsDraggingFn>((IntPtr)ptrs[i], i, "UI_Panel_IsDragging"); i++;
                UI_Panel_DragStarted = SafeGetDelegate<UI_Panel_DragStartedFn>((IntPtr)ptrs[i], i, "UI_Panel_DragStarted"); i++;
                UI_Panel_DragEnded = SafeGetDelegate<UI_Panel_DragEndedFn>((IntPtr)ptrs[i], i, "UI_Panel_DragEnded"); i++;
                UI_Panel_WasDropped = SafeGetDelegate<UI_Panel_WasDroppedFn>((IntPtr)ptrs[i], i, "UI_Panel_WasDropped"); i++;
                UI_Panel_GetDropSource = SafeGetDelegate<UI_Panel_GetDropSourceFn>((IntPtr)ptrs[i], i, "UI_Panel_GetDropSource"); i++;
                UI_Panel_GetDropTarget = SafeGetDelegate<UI_Panel_GetDropTargetFn>((IntPtr)ptrs[i], i, "UI_Panel_GetDropTarget"); i++;
                UI_Panel_GetAllowDrag = SafeGetDelegate<UI_Panel_GetAllowDragFn>((IntPtr)ptrs[i], i, "UI_Panel_GetAllowDrag"); i++;
                UI_Panel_SetAllowDrag = SafeGetDelegate<UI_Panel_SetAllowDragFn>((IntPtr)ptrs[i], i, "UI_Panel_SetAllowDrag"); i++;
                UI_Panel_GetAllowDrop = SafeGetDelegate<UI_Panel_GetAllowDropFn>((IntPtr)ptrs[i], i, "UI_Panel_GetAllowDrop"); i++;
                UI_Panel_SetAllowDrop = SafeGetDelegate<UI_Panel_SetAllowDropFn>((IntPtr)ptrs[i], i, "UI_Panel_SetAllowDrop"); i++;
                // Texture functions (2 entries)
                TextureInterop.Initialize(
                    IntPtr.Zero, // GetAssetNameByGuid - not exposed yet
                    IntPtr.Zero, // GetGuidFromPath - not exposed yet
                    (IntPtr)ptrs[i], // UI_Panel_SetTexture
                    i + 1 < count ? (IntPtr)ptrs[i + 1] : IntPtr.Zero // UI_Panel_GetTexture
                );
                i += 2;
                UI_Panel_GetDriveChildrenOpacity = SafeGetDelegate<UI_Panel_GetDriveChildrenOpacityFn>((IntPtr)ptrs[i], i, "UI_Panel_GetDriveChildrenOpacity"); i++;
                UI_Panel_SetDriveChildrenOpacity = SafeGetDelegate<UI_Panel_SetDriveChildrenOpacityFn>((IntPtr)ptrs[i], i, "UI_Panel_SetDriveChildrenOpacity"); i++;
            }
            // UI Rect functions (12 entries)
            if (i + 12 <= count)
            {
                UI_Rect_GetAnchorToParent = SafeGetDelegate<UI_Rect_GetAnchorToParentFn>((IntPtr)ptrs[i], i, "UI_Rect_GetAnchorToParent"); i++;
                UI_Rect_SetAnchorToParent = SafeGetDelegate<UI_Rect_SetAnchorToParentFn>((IntPtr)ptrs[i], i, "UI_Rect_SetAnchorToParent"); i++;
                UI_Rect_GetHorizontalAnchor = SafeGetDelegate<UI_Rect_GetHorizontalAnchorFn>((IntPtr)ptrs[i], i, "UI_Rect_GetHorizontalAnchor"); i++;
                UI_Rect_SetHorizontalAnchor = SafeGetDelegate<UI_Rect_SetHorizontalAnchorFn>((IntPtr)ptrs[i], i, "UI_Rect_SetHorizontalAnchor"); i++;
                UI_Rect_GetVerticalAnchor = SafeGetDelegate<UI_Rect_GetVerticalAnchorFn>((IntPtr)ptrs[i], i, "UI_Rect_GetVerticalAnchor"); i++;
                UI_Rect_SetVerticalAnchor = SafeGetDelegate<UI_Rect_SetVerticalAnchorFn>((IntPtr)ptrs[i], i, "UI_Rect_SetVerticalAnchor"); i++;
                UI_Rect_GetPivot = SafeGetDelegate<UI_Rect_GetPivotFn>((IntPtr)ptrs[i], i, "UI_Rect_GetPivot"); i++;
                UI_Rect_SetPivot = SafeGetDelegate<UI_Rect_SetPivotFn>((IntPtr)ptrs[i], i, "UI_Rect_SetPivot"); i++;
                UI_Rect_GetOffset = SafeGetDelegate<UI_Rect_GetOffsetFn>((IntPtr)ptrs[i], i, "UI_Rect_GetOffset"); i++;
                UI_Rect_SetOffset = SafeGetDelegate<UI_Rect_SetOffsetFn>((IntPtr)ptrs[i], i, "UI_Rect_SetOffset"); i++;
                UI_Rect_GetSize = SafeGetDelegate<UI_Rect_GetSizeFn>((IntPtr)ptrs[i], i, "UI_Rect_GetSize"); i++;
                UI_Rect_SetSize = SafeGetDelegate<UI_Rect_SetSizeFn>((IntPtr)ptrs[i], i, "UI_Rect_SetSize"); i++;
            }
            // UI Canvas functions (6 entries)
            if (i + 6 <= count)
            {
                UI_Canvas_GetOpacity = SafeGetDelegate<UI_Canvas_GetOpacityFn>((IntPtr)ptrs[i], i, "UI_Canvas_GetOpacity"); i++;
                UI_Canvas_SetOpacity = SafeGetDelegate<UI_Canvas_SetOpacityFn>((IntPtr)ptrs[i], i, "UI_Canvas_SetOpacity"); i++;
                UI_Canvas_GetRenderSpace = SafeGetDelegate<UI_Canvas_GetRenderSpaceFn>((IntPtr)ptrs[i], i, "UI_Canvas_GetRenderSpace"); i++;
                UI_Canvas_SetRenderSpace = SafeGetDelegate<UI_Canvas_SetRenderSpaceFn>((IntPtr)ptrs[i], i, "UI_Canvas_SetRenderSpace"); i++;
                UI_Canvas_GetBillboard = SafeGetDelegate<UI_Canvas_GetBillboardFn>((IntPtr)ptrs[i], i, "UI_Canvas_GetBillboard"); i++;
                UI_Canvas_SetBillboard = SafeGetDelegate<UI_Canvas_SetBillboardFn>((IntPtr)ptrs[i], i, "UI_Canvas_SetBillboard"); i++;
            }

            // UI LayoutGroup functions (16 entries)
            if (i + 16 <= count)
            {
                UI_LayoutGroup_GetDirection = SafeGetDelegate<UI_LayoutGroup_GetDirectionFn>((IntPtr)ptrs[i], i, "UI_LayoutGroup_GetDirection"); i++;
                UI_LayoutGroup_SetDirection = SafeGetDelegate<UI_LayoutGroup_SetDirectionFn>((IntPtr)ptrs[i], i, "UI_LayoutGroup_SetDirection"); i++;
                UI_LayoutGroup_GetPadding = SafeGetDelegate<UI_LayoutGroup_GetPaddingFn>((IntPtr)ptrs[i], i, "UI_LayoutGroup_GetPadding"); i++;
                UI_LayoutGroup_SetPadding = SafeGetDelegate<UI_LayoutGroup_SetPaddingFn>((IntPtr)ptrs[i], i, "UI_LayoutGroup_SetPadding"); i++;
                UI_LayoutGroup_GetSpacing = SafeGetDelegate<UI_LayoutGroup_GetSpacingFn>((IntPtr)ptrs[i], i, "UI_LayoutGroup_GetSpacing"); i++;
                UI_LayoutGroup_SetSpacing = SafeGetDelegate<UI_LayoutGroup_SetSpacingFn>((IntPtr)ptrs[i], i, "UI_LayoutGroup_SetSpacing"); i++;
                UI_LayoutGroup_GetChildAlignment = SafeGetDelegate<UI_LayoutGroup_GetChildAlignmentFn>((IntPtr)ptrs[i], i, "UI_LayoutGroup_GetChildAlignment"); i++;
                UI_LayoutGroup_SetChildAlignment = SafeGetDelegate<UI_LayoutGroup_SetChildAlignmentFn>((IntPtr)ptrs[i], i, "UI_LayoutGroup_SetChildAlignment"); i++;
                UI_LayoutGroup_GetCrossAlignment = SafeGetDelegate<UI_LayoutGroup_GetCrossAlignmentFn>((IntPtr)ptrs[i], i, "UI_LayoutGroup_GetCrossAlignment"); i++;
                UI_LayoutGroup_SetCrossAlignment = SafeGetDelegate<UI_LayoutGroup_SetCrossAlignmentFn>((IntPtr)ptrs[i], i, "UI_LayoutGroup_SetCrossAlignment"); i++;
                UI_LayoutGroup_GetControlChildWidth = SafeGetDelegate<UI_LayoutGroup_GetControlChildWidthFn>((IntPtr)ptrs[i], i, "UI_LayoutGroup_GetControlChildWidth"); i++;
                UI_LayoutGroup_SetControlChildWidth = SafeGetDelegate<UI_LayoutGroup_SetControlChildWidthFn>((IntPtr)ptrs[i], i, "UI_LayoutGroup_SetControlChildWidth"); i++;
                UI_LayoutGroup_GetControlChildHeight = SafeGetDelegate<UI_LayoutGroup_GetControlChildHeightFn>((IntPtr)ptrs[i], i, "UI_LayoutGroup_GetControlChildHeight"); i++;
                UI_LayoutGroup_SetControlChildHeight = SafeGetDelegate<UI_LayoutGroup_SetControlChildHeightFn>((IntPtr)ptrs[i], i, "UI_LayoutGroup_SetControlChildHeight"); i++;
                UI_LayoutGroup_GetReverseOrder = SafeGetDelegate<UI_LayoutGroup_GetReverseOrderFn>((IntPtr)ptrs[i], i, "UI_LayoutGroup_GetReverseOrder"); i++;
                UI_LayoutGroup_SetReverseOrder = SafeGetDelegate<UI_LayoutGroup_SetReverseOrderFn>((IntPtr)ptrs[i], i, "UI_LayoutGroup_SetReverseOrder"); i++;
            }

            // UI FitToContent functions (14 entries)
            if (i + 14 <= count)
            {
                UI_FitToContent_GetEnabled = SafeGetDelegate<UI_FitToContent_GetEnabledFn>((IntPtr)ptrs[i], i, "UI_FitToContent_GetEnabled"); i++;
                UI_FitToContent_SetEnabled = SafeGetDelegate<UI_FitToContent_SetEnabledFn>((IntPtr)ptrs[i], i, "UI_FitToContent_SetEnabled"); i++;
                UI_FitToContent_GetFitWidth = SafeGetDelegate<UI_FitToContent_GetFitWidthFn>((IntPtr)ptrs[i], i, "UI_FitToContent_GetFitWidth"); i++;
                UI_FitToContent_SetFitWidth = SafeGetDelegate<UI_FitToContent_SetFitWidthFn>((IntPtr)ptrs[i], i, "UI_FitToContent_SetFitWidth"); i++;
                UI_FitToContent_GetFitHeight = SafeGetDelegate<UI_FitToContent_GetFitHeightFn>((IntPtr)ptrs[i], i, "UI_FitToContent_GetFitHeight"); i++;
                UI_FitToContent_SetFitHeight = SafeGetDelegate<UI_FitToContent_SetFitHeightFn>((IntPtr)ptrs[i], i, "UI_FitToContent_SetFitHeight"); i++;
                UI_FitToContent_GetPadding = SafeGetDelegate<UI_FitToContent_GetPaddingFn>((IntPtr)ptrs[i], i, "UI_FitToContent_GetPadding"); i++;
                UI_FitToContent_SetPadding = SafeGetDelegate<UI_FitToContent_SetPaddingFn>((IntPtr)ptrs[i], i, "UI_FitToContent_SetPadding"); i++;
                UI_FitToContent_GetMinSize = SafeGetDelegate<UI_FitToContent_GetMinSizeFn>((IntPtr)ptrs[i], i, "UI_FitToContent_GetMinSize"); i++;
                UI_FitToContent_SetMinSize = SafeGetDelegate<UI_FitToContent_SetMinSizeFn>((IntPtr)ptrs[i], i, "UI_FitToContent_SetMinSize"); i++;
                UI_FitToContent_GetMaxSize = SafeGetDelegate<UI_FitToContent_GetMaxSizeFn>((IntPtr)ptrs[i], i, "UI_FitToContent_GetMaxSize"); i++;
                UI_FitToContent_SetMaxSize = SafeGetDelegate<UI_FitToContent_SetMaxSizeFn>((IntPtr)ptrs[i], i, "UI_FitToContent_SetMaxSize"); i++;
                UI_FitToContent_GetDirectChildrenOnly = SafeGetDelegate<UI_FitToContent_GetDirectChildrenOnlyFn>((IntPtr)ptrs[i], i, "UI_FitToContent_GetDirectChildrenOnly"); i++;
                UI_FitToContent_SetDirectChildrenOnly = SafeGetDelegate<UI_FitToContent_SetDirectChildrenOnlyFn>((IntPtr)ptrs[i], i, "UI_FitToContent_SetDirectChildrenOnly"); i++;
            }

            // UI Scene Capture functions (30 entries)
            if (i + 30 <= count)
            {
                UI_SceneCapture_GetEnabled = SafeGetDelegate<UI_SceneCapture_GetEnabledFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_GetEnabled"); i++;
                UI_SceneCapture_SetEnabled = SafeGetDelegate<UI_SceneCapture_SetEnabledFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_SetEnabled"); i++;
                UI_SceneCapture_GetAutoFrame = SafeGetDelegate<UI_SceneCapture_GetAutoFrameFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_GetAutoFrame"); i++;
                UI_SceneCapture_SetAutoFrame = SafeGetDelegate<UI_SceneCapture_SetAutoFrameFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_SetAutoFrame"); i++;
                UI_SceneCapture_GetIncludeChildren = SafeGetDelegate<UI_SceneCapture_GetIncludeChildrenFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_GetIncludeChildren"); i++;
                UI_SceneCapture_SetIncludeChildren = SafeGetDelegate<UI_SceneCapture_SetIncludeChildrenFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_SetIncludeChildren"); i++;
                UI_SceneCapture_GetBoundsPadding = SafeGetDelegate<UI_SceneCapture_GetBoundsPaddingFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_GetBoundsPadding"); i++;
                UI_SceneCapture_SetBoundsPadding = SafeGetDelegate<UI_SceneCapture_SetBoundsPaddingFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_SetBoundsPadding"); i++;
                UI_SceneCapture_GetFieldOfView = SafeGetDelegate<UI_SceneCapture_GetFieldOfViewFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_GetFieldOfView"); i++;
                UI_SceneCapture_SetFieldOfView = SafeGetDelegate<UI_SceneCapture_SetFieldOfViewFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_SetFieldOfView"); i++;
                UI_SceneCapture_GetNearClip = SafeGetDelegate<UI_SceneCapture_GetNearClipFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_GetNearClip"); i++;
                UI_SceneCapture_SetNearClip = SafeGetDelegate<UI_SceneCapture_SetNearClipFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_SetNearClip"); i++;
                UI_SceneCapture_GetFarClip = SafeGetDelegate<UI_SceneCapture_GetFarClipFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_GetFarClip"); i++;
                UI_SceneCapture_SetFarClip = SafeGetDelegate<UI_SceneCapture_SetFarClipFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_SetFarClip"); i++;
                UI_SceneCapture_GetViewDirection = SafeGetDelegate<UI_SceneCapture_GetViewDirectionFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_GetViewDirection"); i++;
                UI_SceneCapture_SetViewDirection = SafeGetDelegate<UI_SceneCapture_SetViewDirectionFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_SetViewDirection"); i++;
                UI_SceneCapture_GetUpDirection = SafeGetDelegate<UI_SceneCapture_GetUpDirectionFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_GetUpDirection"); i++;
                UI_SceneCapture_SetUpDirection = SafeGetDelegate<UI_SceneCapture_SetUpDirectionFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_SetUpDirection"); i++;
                UI_SceneCapture_GetFocusOffset = SafeGetDelegate<UI_SceneCapture_GetFocusOffsetFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_GetFocusOffset"); i++;
                UI_SceneCapture_SetFocusOffset = SafeGetDelegate<UI_SceneCapture_SetFocusOffsetFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_SetFocusOffset"); i++;
                UI_SceneCapture_GetTargetEntity = SafeGetDelegate<UI_SceneCapture_GetTargetEntityFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_GetTargetEntity"); i++;
                UI_SceneCapture_SetTargetEntity = SafeGetDelegate<UI_SceneCapture_SetTargetEntityFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_SetTargetEntity"); i++;
                UI_SceneCapture_GetRenderSize = SafeGetDelegate<UI_SceneCapture_GetRenderSizeFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_GetRenderSize"); i++;
                UI_SceneCapture_SetRenderSize = SafeGetDelegate<UI_SceneCapture_SetRenderSizeFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_SetRenderSize"); i++;
                UI_SceneCapture_GetClearColor = SafeGetDelegate<UI_SceneCapture_GetClearColorFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_GetClearColor"); i++;
                UI_SceneCapture_SetClearColor = SafeGetDelegate<UI_SceneCapture_SetClearColorFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_SetClearColor"); i++;
                UI_SceneCapture_GetShowGrid = SafeGetDelegate<UI_SceneCapture_GetShowGridFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_GetShowGrid"); i++;
                UI_SceneCapture_SetShowGrid = SafeGetDelegate<UI_SceneCapture_SetShowGridFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_SetShowGrid"); i++;
                UI_SceneCapture_GetLockViewToTarget = SafeGetDelegate<UI_SceneCapture_GetLockViewToTargetFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_GetLockViewToTarget"); i++;
                UI_SceneCapture_SetLockViewToTarget = SafeGetDelegate<UI_SceneCapture_SetLockViewToTargetFn>((IntPtr)ptrs[i], i, "UI_SceneCapture_SetLockViewToTarget"); i++;
            }

            // UI Slider functions (9 entries)
            if (i + 9 <= count)
            {
                UI_Slider_GetValue = SafeGetDelegate<UI_Slider_GetValueFn>((IntPtr)ptrs[i], i, "UI_Slider_GetValue"); i++;
                UI_Slider_SetValue = SafeGetDelegate<UI_Slider_SetValueFn>((IntPtr)ptrs[i], i, "UI_Slider_SetValue"); i++;
                UI_Slider_GetMinValue = SafeGetDelegate<UI_Slider_GetMinValueFn>((IntPtr)ptrs[i], i, "UI_Slider_GetMinValue"); i++;
                UI_Slider_SetMinValue = SafeGetDelegate<UI_Slider_SetMinValueFn>((IntPtr)ptrs[i], i, "UI_Slider_SetMinValue"); i++;
                UI_Slider_GetMaxValue = SafeGetDelegate<UI_Slider_GetMaxValueFn>((IntPtr)ptrs[i], i, "UI_Slider_GetMaxValue"); i++;
                UI_Slider_SetMaxValue = SafeGetDelegate<UI_Slider_SetMaxValueFn>((IntPtr)ptrs[i], i, "UI_Slider_SetMaxValue"); i++;
                UI_Slider_IsHovered = SafeGetDelegate<UI_Slider_IsHoveredFn>((IntPtr)ptrs[i], i, "UI_Slider_IsHovered"); i++;
                UI_Slider_IsDragging = SafeGetDelegate<UI_Slider_IsDraggingFn>((IntPtr)ptrs[i], i, "UI_Slider_IsDragging"); i++;
                UI_Slider_ValueChanged = SafeGetDelegate<UI_Slider_ValueChangedFn>((IntPtr)ptrs[i], i, "UI_Slider_ValueChanged"); i++;
                Console.WriteLine($"[ComponentInterop] After UI slider funcs i={i}");
            }

            // UI ProgressBar functions (10 entries)
            if (i + 10 <= count)
            {
                UI_ProgressBar_GetValue = SafeGetDelegate<UI_ProgressBar_GetValueFn>((IntPtr)ptrs[i], i, "UI_ProgressBar_GetValue"); i++;
                UI_ProgressBar_SetValue = SafeGetDelegate<UI_ProgressBar_SetValueFn>((IntPtr)ptrs[i], i, "UI_ProgressBar_SetValue"); i++;
                UI_ProgressBar_GetMinValue = SafeGetDelegate<UI_ProgressBar_GetMinValueFn>((IntPtr)ptrs[i], i, "UI_ProgressBar_GetMinValue"); i++;
                UI_ProgressBar_SetMinValue = SafeGetDelegate<UI_ProgressBar_SetMinValueFn>((IntPtr)ptrs[i], i, "UI_ProgressBar_SetMinValue"); i++;
                UI_ProgressBar_GetMaxValue = SafeGetDelegate<UI_ProgressBar_GetMaxValueFn>((IntPtr)ptrs[i], i, "UI_ProgressBar_GetMaxValue"); i++;
                UI_ProgressBar_SetMaxValue = SafeGetDelegate<UI_ProgressBar_SetMaxValueFn>((IntPtr)ptrs[i], i, "UI_ProgressBar_SetMaxValue"); i++;
                UI_ProgressBar_GetOpacity = SafeGetDelegate<UI_ProgressBar_GetOpacityFn>((IntPtr)ptrs[i], i, "UI_ProgressBar_GetOpacity"); i++;
                UI_ProgressBar_SetOpacity = SafeGetDelegate<UI_ProgressBar_SetOpacityFn>((IntPtr)ptrs[i], i, "UI_ProgressBar_SetOpacity"); i++;
                UI_ProgressBar_GetVisible = SafeGetDelegate<UI_ProgressBar_GetVisibleFn>((IntPtr)ptrs[i], i, "UI_ProgressBar_GetVisible"); i++;
                UI_ProgressBar_SetVisible = SafeGetDelegate<UI_ProgressBar_SetVisibleFn>((IntPtr)ptrs[i], i, "UI_ProgressBar_SetVisible"); i++;
                Console.WriteLine($"[ComponentInterop] After UI progressbar funcs i={i}");
            }

            // UI Toggle functions (5 entries)
            if (i + 5 <= count)
            {
                UI_Toggle_GetIsOn = SafeGetDelegate<UI_Toggle_GetIsOnFn>((IntPtr)ptrs[i], i, "UI_Toggle_GetIsOn"); i++;
                UI_Toggle_SetIsOn = SafeGetDelegate<UI_Toggle_SetIsOnFn>((IntPtr)ptrs[i], i, "UI_Toggle_SetIsOn"); i++;
                UI_Toggle_IsHovered = SafeGetDelegate<UI_Toggle_IsHoveredFn>((IntPtr)ptrs[i], i, "UI_Toggle_IsHovered"); i++;
                UI_Toggle_IsPressed = SafeGetDelegate<UI_Toggle_IsPressedFn>((IntPtr)ptrs[i], i, "UI_Toggle_IsPressed"); i++;
                UI_Toggle_ValueChanged = SafeGetDelegate<UI_Toggle_ValueChangedFn>((IntPtr)ptrs[i], i, "UI_Toggle_ValueChanged"); i++;
                Console.WriteLine($"[ComponentInterop] After UI toggle funcs i={i}");
            }

            // UI ScrollView functions (8 entries)
            if (i + 8 <= count)
            {
                UI_ScrollView_GetContentOffset = SafeGetDelegate<UI_ScrollView_GetContentOffsetFn>((IntPtr)ptrs[i], i, "UI_ScrollView_GetContentOffset"); i++;
                UI_ScrollView_SetContentOffset = SafeGetDelegate<UI_ScrollView_SetContentOffsetFn>((IntPtr)ptrs[i], i, "UI_ScrollView_SetContentOffset"); i++;
                UI_ScrollView_GetContentSize = SafeGetDelegate<UI_ScrollView_GetContentSizeFn>((IntPtr)ptrs[i], i, "UI_ScrollView_GetContentSize"); i++;
                UI_ScrollView_SetContentSize = SafeGetDelegate<UI_ScrollView_SetContentSizeFn>((IntPtr)ptrs[i], i, "UI_ScrollView_SetContentSize"); i++;
                UI_ScrollView_GetOpacity = SafeGetDelegate<UI_ScrollView_GetOpacityFn>((IntPtr)ptrs[i], i, "UI_ScrollView_GetOpacity"); i++;
                UI_ScrollView_SetOpacity = SafeGetDelegate<UI_ScrollView_SetOpacityFn>((IntPtr)ptrs[i], i, "UI_ScrollView_SetOpacity"); i++;
                UI_ScrollView_GetVisible = SafeGetDelegate<UI_ScrollView_GetVisibleFn>((IntPtr)ptrs[i], i, "UI_ScrollView_GetVisible"); i++;
                UI_ScrollView_SetVisible = SafeGetDelegate<UI_ScrollView_SetVisibleFn>((IntPtr)ptrs[i], i, "UI_ScrollView_SetVisible"); i++;
                Console.WriteLine($"[ComponentInterop] After UI scrollview funcs i={i}");
            }

            // UI InputField functions (6 entries)
            if (i + 6 <= count)
            {
                UI_InputField_GetTextInternal = SafeGetDelegate<UI_InputField_GetTextFn>((IntPtr)ptrs[i], i, "UI_InputField_GetText"); i++;
                UI_InputField_SetText = SafeGetDelegate<UI_InputField_SetTextFn>((IntPtr)ptrs[i], i, "UI_InputField_SetText"); i++;
                UI_InputField_GetPlaceholderInternal = SafeGetDelegate<UI_InputField_GetPlaceholderFn>((IntPtr)ptrs[i], i, "UI_InputField_GetPlaceholder"); i++;
                UI_InputField_SetPlaceholder = SafeGetDelegate<UI_InputField_SetPlaceholderFn>((IntPtr)ptrs[i], i, "UI_InputField_SetPlaceholder"); i++;
                UI_InputField_IsFocused = SafeGetDelegate<UI_InputField_IsFocusedFn>((IntPtr)ptrs[i], i, "UI_InputField_IsFocused"); i++;
                UI_InputField_TextChanged = SafeGetDelegate<UI_InputField_TextChangedFn>((IntPtr)ptrs[i], i, "UI_InputField_TextChanged"); i++;
                Console.WriteLine($"[ComponentInterop] After UI inputfield funcs i={i}");
            }

            // UI Dropdown functions (9 entries)
            if (i + 9 <= count)
            {
                UI_Dropdown_GetSelectedIndex = SafeGetDelegate<UI_Dropdown_GetSelectedIndexFn>((IntPtr)ptrs[i], i, "UI_Dropdown_GetSelectedIndex"); i++;
                UI_Dropdown_SetSelectedIndex = SafeGetDelegate<UI_Dropdown_SetSelectedIndexFn>((IntPtr)ptrs[i], i, "UI_Dropdown_SetSelectedIndex"); i++;
                UI_Dropdown_GetOptionCount = SafeGetDelegate<UI_Dropdown_GetOptionCountFn>((IntPtr)ptrs[i], i, "UI_Dropdown_GetOptionCount"); i++;
                UI_Dropdown_GetOptionInternal = SafeGetDelegate<UI_Dropdown_GetOptionFn>((IntPtr)ptrs[i], i, "UI_Dropdown_GetOption"); i++;
                UI_Dropdown_SetOption = SafeGetDelegate<UI_Dropdown_SetOptionFn>((IntPtr)ptrs[i], i, "UI_Dropdown_SetOption"); i++;
                UI_Dropdown_AddOption = SafeGetDelegate<UI_Dropdown_AddOptionFn>((IntPtr)ptrs[i], i, "UI_Dropdown_AddOption"); i++;
                UI_Dropdown_ClearOptions = SafeGetDelegate<UI_Dropdown_ClearOptionsFn>((IntPtr)ptrs[i], i, "UI_Dropdown_ClearOptions"); i++;
                UI_Dropdown_IsOpen = SafeGetDelegate<UI_Dropdown_IsOpenFn>((IntPtr)ptrs[i], i, "UI_Dropdown_IsOpen"); i++;
                UI_Dropdown_SelectionChanged = SafeGetDelegate<UI_Dropdown_SelectionChangedFn>((IntPtr)ptrs[i], i, "UI_Dropdown_SelectionChanged"); i++;
                Console.WriteLine($"[ComponentInterop] After UI dropdown funcs i={i}");
            }

            // Animation Layer functions (28 entries)
            if (i + 28 <= count)
            {
                AnimLayer_GetOrCreate = SafeGetDelegate<AnimLayer_GetOrCreateFn>((IntPtr)ptrs[i], i, "AnimLayer_GetOrCreate"); i++;
                AnimLayer_Remove = SafeGetDelegate<AnimLayer_RemoveFn>((IntPtr)ptrs[i], i, "AnimLayer_Remove"); i++;
                AnimLayer_Has = SafeGetDelegate<AnimLayer_HasFn>((IntPtr)ptrs[i], i, "AnimLayer_Has"); i++;
                AnimLayer_GetCount = SafeGetDelegate<AnimLayer_GetCountFn>((IntPtr)ptrs[i], i, "AnimLayer_GetCount"); i++;
                AnimLayer_GetNameByIndexInternal = SafeGetDelegate<AnimLayer_GetNameByIndexFn>((IntPtr)ptrs[i], i, "AnimLayer_GetNameByIndex"); i++;
                AnimLayer_SetAnimation = SafeGetDelegate<AnimLayer_SetAnimationFn>((IntPtr)ptrs[i], i, "AnimLayer_SetAnimation"); i++;
                AnimLayer_GetAnimationInternal = SafeGetDelegate<AnimLayer_GetAnimationFn>((IntPtr)ptrs[i], i, "AnimLayer_GetAnimation"); i++;
                AnimLayer_SetMask = SafeGetDelegate<AnimLayer_SetMaskFn>((IntPtr)ptrs[i], i, "AnimLayer_SetMask"); i++;
                AnimLayer_GetMask = SafeGetDelegate<AnimLayer_GetMaskFn>((IntPtr)ptrs[i], i, "AnimLayer_GetMask"); i++;
                AnimLayer_SetBlendMode = SafeGetDelegate<AnimLayer_SetBlendModeFn>((IntPtr)ptrs[i], i, "AnimLayer_SetBlendMode"); i++;
                AnimLayer_GetBlendMode = SafeGetDelegate<AnimLayer_GetBlendModeFn>((IntPtr)ptrs[i], i, "AnimLayer_GetBlendMode"); i++;
                AnimLayer_SetWeight = SafeGetDelegate<AnimLayer_SetWeightFn>((IntPtr)ptrs[i], i, "AnimLayer_SetWeight"); i++;
                AnimLayer_GetWeight = SafeGetDelegate<AnimLayer_GetWeightFn>((IntPtr)ptrs[i], i, "AnimLayer_GetWeight"); i++;
                AnimLayer_BlendTo = SafeGetDelegate<AnimLayer_BlendToFn>((IntPtr)ptrs[i], i, "AnimLayer_BlendTo"); i++;
                AnimLayer_Play = SafeGetDelegate<AnimLayer_PlayFn>((IntPtr)ptrs[i], i, "AnimLayer_Play"); i++;
                AnimLayer_Stop = SafeGetDelegate<AnimLayer_StopFn>((IntPtr)ptrs[i], i, "AnimLayer_Stop"); i++;
                AnimLayer_IsPlaying = SafeGetDelegate<AnimLayer_IsPlayingFn>((IntPtr)ptrs[i], i, "AnimLayer_IsPlaying"); i++;
                AnimLayer_SetSpeed = SafeGetDelegate<AnimLayer_SetSpeedFn>((IntPtr)ptrs[i], i, "AnimLayer_SetSpeed"); i++;
                AnimLayer_GetSpeed = SafeGetDelegate<AnimLayer_GetSpeedFn>((IntPtr)ptrs[i], i, "AnimLayer_GetSpeed"); i++;
                AnimLayer_SetTime = SafeGetDelegate<AnimLayer_SetTimeFn>((IntPtr)ptrs[i], i, "AnimLayer_SetTime"); i++;
                AnimLayer_SetNormalizedTime = SafeGetDelegate<AnimLayer_SetNormalizedTimeFn>((IntPtr)ptrs[i], i, "AnimLayer_SetNormalizedTime"); i++;
                AnimLayer_SetState = SafeGetDelegate<AnimLayer_SetStateFn>((IntPtr)ptrs[i], i, "AnimLayer_SetState"); i++;
                AnimLayer_SetStateByName = SafeGetDelegate<AnimLayer_SetStateByNameFn>((IntPtr)ptrs[i], i, "AnimLayer_SetStateByName"); i++;
                AnimLayer_SetBlend2D = SafeGetDelegate<AnimLayer_SetBlend2DFn>((IntPtr)ptrs[i], i, "AnimLayer_SetBlend2D"); i++;
                AnimLayer_GetTime = SafeGetDelegate<AnimLayer_GetTimeFn>((IntPtr)ptrs[i], i, "AnimLayer_GetTime"); i++;
                AnimLayer_GetDuration = SafeGetDelegate<AnimLayer_GetDurationFn>((IntPtr)ptrs[i], i, "AnimLayer_GetDuration"); i++;
                AnimLayer_SetLooping = SafeGetDelegate<AnimLayer_SetLoopingFn>((IntPtr)ptrs[i], i, "AnimLayer_SetLooping"); i++;
                AnimLayer_GetLooping = SafeGetDelegate<AnimLayer_GetLoopingFn>((IntPtr)ptrs[i], i, "AnimLayer_GetLooping"); i++;
                Console.WriteLine($"[ComponentInterop] After AnimLayer funcs i={i}");
            }

            // Terrain functions (12 entries)
            if (i + 12 <= count)
            {
                Terrain_GetHeightAtWorld = SafeGetDelegate<Terrain_GetHeightAtWorldFn>((IntPtr)ptrs[i], i, "Terrain_GetHeightAtWorld"); i++;
                Terrain_GetNormalAtWorld = SafeGetDelegate<Terrain_GetNormalAtWorldFn>((IntPtr)ptrs[i], i, "Terrain_GetNormalAtWorld"); i++;
                Terrain_GetNearestPoint = SafeGetDelegate<Terrain_GetNearestPointFn>((IntPtr)ptrs[i], i, "Terrain_GetNearestPoint"); i++;
                Terrain_Raycast = SafeGetDelegate<Terrain_RaycastFn>((IntPtr)ptrs[i], i, "Terrain_Raycast"); i++;
                Terrain_GetDominantLayerAtWorld = SafeGetDelegate<Terrain_GetDominantLayerAtWorldFn>((IntPtr)ptrs[i], i, "Terrain_GetDominantLayerAtWorld"); i++;
                Terrain_SetHeightAtWorld = SafeGetDelegate<Terrain_SetHeightAtWorldFn>((IntPtr)ptrs[i], i, "Terrain_SetHeightAtWorld"); i++;
                Terrain_ApplyHeightDelta = SafeGetDelegate<Terrain_ApplyHeightDeltaFn>((IntPtr)ptrs[i], i, "Terrain_ApplyHeightDelta"); i++;
                Terrain_GetInstancerLayerCount = SafeGetDelegate<Terrain_GetInstancerLayerCountFn>((IntPtr)ptrs[i], i, "Terrain_GetInstancerLayerCount"); i++;
                Terrain_GetInstancerLayerNameInternal = SafeGetDelegate<Terrain_GetInstancerLayerNameFn>((IntPtr)ptrs[i], i, "Terrain_GetInstancerLayerName"); i++;
                Terrain_SetInstancerLayerEnabled = SafeGetDelegate<Terrain_SetInstancerLayerEnabledFn>((IntPtr)ptrs[i], i, "Terrain_SetInstancerLayerEnabled"); i++;
                Terrain_SetInstancerLayerDensity = SafeGetDelegate<Terrain_SetInstancerLayerDensityFn>((IntPtr)ptrs[i], i, "Terrain_SetInstancerLayerDensity"); i++;
                Terrain_RegenerateInstancers = SafeGetDelegate<Terrain_RegenerateInstancersFn>((IntPtr)ptrs[i], i, "Terrain_RegenerateInstancers"); i++;
                Console.WriteLine($"[ComponentInterop] After Terrain funcs i={i}");
            }

            // Spline functions (5 entries)
            if (i + 5 <= count)
            {
                Spline_GetControlPointCount = SafeGetDelegate<Spline_GetControlPointCountFn>((IntPtr)ptrs[i], i, "Spline_GetControlPointCount"); i++;
                Spline_GetControlPoint = SafeGetDelegate<Spline_GetControlPointFn>((IntPtr)ptrs[i], i, "Spline_GetControlPoint"); i++;
                Spline_GetSampledPointCount = SafeGetDelegate<Spline_GetSampledPointCountFn>((IntPtr)ptrs[i], i, "Spline_GetSampledPointCount"); i++;
                Spline_GetSampledPoint = SafeGetDelegate<Spline_GetSampledPointFn>((IntPtr)ptrs[i], i, "Spline_GetSampledPoint"); i++;
                Spline_GetNearestPoint = SafeGetDelegate<Spline_GetNearestPointFn>((IntPtr)ptrs[i], i, "Spline_GetNearestPoint"); i++;
                Spline_GetPointAtNormalized = SafeGetDelegate<Spline_GetPointAtNormalizedFn>((IntPtr)ptrs[i], i, "Spline_GetPointAtNormalized"); i++;
            }

            // Portal functions (18 entries)
            if (i + 18 <= count)
            {
                Portal_GetEnabled = SafeGetDelegate<Portal_GetEnabledFn>((IntPtr)ptrs[i], i, "Portal_GetEnabled"); i++;
                Portal_SetEnabled = SafeGetDelegate<Portal_SetEnabledFn>((IntPtr)ptrs[i], i, "Portal_SetEnabled"); i++;
                Portal_GetTargetScenePathInternal = SafeGetDelegate<Portal_GetTargetScenePathFn>((IntPtr)ptrs[i], i, "Portal_GetTargetScenePath"); i++;
                Portal_SetTargetScenePath = SafeGetDelegate<Portal_SetTargetScenePathFn>((IntPtr)ptrs[i], i, "Portal_SetTargetScenePath"); i++;
                Portal_GetTargetPortalGuidInternal = SafeGetDelegate<Portal_GetTargetPortalGuidFn>((IntPtr)ptrs[i], i, "Portal_GetTargetPortalGuid"); i++;
                Portal_SetTargetPortalGuid = SafeGetDelegate<Portal_SetTargetPortalGuidFn>((IntPtr)ptrs[i], i, "Portal_SetTargetPortalGuid"); i++;
                Portal_GetTargetPortalPathInternal = SafeGetDelegate<Portal_GetTargetPortalPathFn>((IntPtr)ptrs[i], i, "Portal_GetTargetPortalPath"); i++;
                Portal_SetTargetPortalPath = SafeGetDelegate<Portal_SetTargetPortalPathFn>((IntPtr)ptrs[i], i, "Portal_SetTargetPortalPath"); i++;
                Portal_GetEntryOffset = SafeGetDelegate<Portal_GetVec3Fn>((IntPtr)ptrs[i], i, "Portal_GetEntryOffset"); i++;
                Portal_SetEntryOffset = SafeGetDelegate<Portal_SetVec3Fn>((IntPtr)ptrs[i], i, "Portal_SetEntryOffset"); i++;
                Portal_GetExitOffset = SafeGetDelegate<Portal_GetVec3Fn>((IntPtr)ptrs[i], i, "Portal_GetExitOffset"); i++;
                Portal_SetExitOffset = SafeGetDelegate<Portal_SetVec3Fn>((IntPtr)ptrs[i], i, "Portal_SetExitOffset"); i++;
                Portal_GetAutoDetect = SafeGetDelegate<Portal_GetBoolFn>((IntPtr)ptrs[i], i, "Portal_GetAutoDetect"); i++;
                Portal_SetAutoDetect = SafeGetDelegate<Portal_SetBoolFn>((IntPtr)ptrs[i], i, "Portal_SetAutoDetect"); i++;
                Portal_GetTriggerRadius = SafeGetDelegate<Portal_GetFloatFn>((IntPtr)ptrs[i], i, "Portal_GetTriggerRadius"); i++;
                Portal_SetTriggerRadius = SafeGetDelegate<Portal_SetFloatFn>((IntPtr)ptrs[i], i, "Portal_SetTriggerRadius"); i++;
                Portal_GetFireExitEvents = SafeGetDelegate<Portal_GetBoolFn>((IntPtr)ptrs[i], i, "Portal_GetFireExitEvents"); i++;
                Portal_SetFireExitEvents = SafeGetDelegate<Portal_SetBoolFn>((IntPtr)ptrs[i], i, "Portal_SetFireExitEvents"); i++;
                Console.WriteLine($"[ComponentInterop] After Portal funcs i={i}");
            }

            Console.WriteLine($"[ComponentInterop] Initialize complete i={i}");
        }
    }
}

