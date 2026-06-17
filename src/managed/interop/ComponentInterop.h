#pragma once
#include <cstdint>
#include "EntityInterop.h"

#ifdef __cplusplus
extern "C" {
#endif

    // --- Managed Logging ---
    // Log levels: 0=Info, 1=Warning, 2=Error
    __declspec(dllexport) void ManagedLog(int level, const char* message);

    // --- Component Lifetime ---
    __declspec(dllexport) bool HasComponent(int entityID, const char* componentName);
    __declspec(dllexport) void AddComponent(int entityID, const char* componentName);
    __declspec(dllexport) void RemoveComponent(int entityID, const char* componentName);
    __declspec(dllexport) void AddScript(int entityID, const char* className);

    // --- LightComponent ---
    __declspec(dllexport) int GetLightType(int entityID);
    __declspec(dllexport) void SetLightType(int entityID, int type);
    __declspec(dllexport) void GetLightColor(int entityID, float* r, float* g, float* b);
    __declspec(dllexport) void SetLightColor(int entityID, float r, float g, float b);
    __declspec(dllexport) float GetLightIntensity(int entityID);
    __declspec(dllexport) void SetLightIntensity(int entityID, float intensity);

    // --- RigidBodyComponent ---
    __declspec(dllexport) float GetRigidBodyMass(int entityID);
    __declspec(dllexport) void SetRigidBodyMass(int entityID, float mass);
    __declspec(dllexport) bool GetRigidBodyIsKinematic(int entityID);
    __declspec(dllexport) void SetRigidBodyIsKinematic(int entityID, bool isKinematic);
    __declspec(dllexport) bool GetRigidBodyUseGravity(int entityID);
    __declspec(dllexport) void SetRigidBodyUseGravity(int entityID, bool useGravity);
    __declspec(dllexport) uint32_t GetRigidBodyCollisionMask(int entityID);
    __declspec(dllexport) void SetRigidBodyCollisionMask(int entityID, uint32_t collisionMask);
    __declspec(dllexport) bool SetRigidBodyPhysicsLayer(int entityID, const char* layerName);
    __declspec(dllexport) void ApplyRigidBodyForce(int entityID, float x, float y, float z);
    __declspec(dllexport) void ApplyRigidBodyTorque(int entityID, float x, float y, float z);
    __declspec(dllexport) void ApplyRigidBodyImpulse(int entityID, float x, float y, float z);
    __declspec(dllexport) void ApplyRigidBodyAngularImpulse(int entityID, float x, float y, float z);
    __declspec(dllexport) const char* RigidBody_GetDebugSummary(int entityID);
    __declspec(dllexport) void Collider_GetOffset(int entityID, float* x, float* y, float* z);

    // Character Controller (virtual)
    __declspec(dllexport) void CC_SetDesiredVelocity(int entityID, float x, float y, float z);
    __declspec(dllexport) void CC_GetDesiredVelocity(int entityID, float* x, float* y, float* z);
    __declspec(dllexport) void CC_SetVerticalVelocity(int entityID, float v);
    __declspec(dllexport) float CC_GetVerticalVelocity(int entityID);
    __declspec(dllexport) void CC_Jump(int entityID, float speed);
    __declspec(dllexport) bool CC_IsGrounded(int entityID);
    __declspec(dllexport) void CC_SetPosition(int entityID, float x, float y, float z);
    __declspec(dllexport) uint32_t CC_GetCollisionMask(int entityID);
    __declspec(dllexport) void CC_SetCollisionMask(int entityID, uint32_t collisionMask);
    __declspec(dllexport) void GetRigidBodyLinearVelocity(int entityID, float* x, float* y, float* z);
    __declspec(dllexport) void SetRigidBodyLinearVelocity(int entityID, float x, float y, float z);
    __declspec(dllexport) void GetRigidBodyAngularVelocity(int entityID, float* x, float* y, float* z);
    __declspec(dllexport) void SetRigidBodyAngularVelocity(int entityID, float x, float y, float z);

    // --- TerrainComponent ---
    __declspec(dllexport) bool Terrain_GetHeightAtWorld(int entityID, float worldX, float worldZ, float* outHeight);
    __declspec(dllexport) bool Terrain_GetNormalAtWorld(int entityID, float worldX, float worldZ, float* outX, float* outY, float* outZ);
    __declspec(dllexport) bool Terrain_GetNearestPoint(int entityID, float worldX, float worldZ, float* outX, float* outY, float* outZ);
    __declspec(dllexport) bool Terrain_Raycast(int entityID, float ox, float oy, float oz, float dx, float dy, float dz,
                                               float* outX, float* outY, float* outZ, float* outNx, float* outNy, float* outNz);
    __declspec(dllexport) bool Terrain_GetDominantLayerAtWorld(int entityID, float worldX, float worldZ, int* outLayerIndex, float* outWeight);
    __declspec(dllexport) bool Terrain_SetHeightAtWorld(int entityID, float worldX, float worldZ, float worldHeight);
    __declspec(dllexport) bool Terrain_ApplyHeightDelta(int entityID, float worldX, float worldZ, float radius, float deltaHeight, float falloff);
    __declspec(dllexport) int Terrain_GetInstancerLayerCount(int entityID);
    __declspec(dllexport) const char* Terrain_GetInstancerLayerName(int entityID, int layerIndex);
    __declspec(dllexport) bool Terrain_SetInstancerLayerEnabled(int entityID, int layerIndex, bool enabled);
    __declspec(dllexport) bool Terrain_SetInstancerLayerDensity(int entityID, int layerIndex, float density);
    __declspec(dllexport) bool Terrain_RegenerateInstancers(int entityID);

    // --- SplineComponent ---
    __declspec(dllexport) int Spline_GetControlPointCount(int entityID);
    __declspec(dllexport) bool Spline_GetControlPoint(int entityID, int index, float* outX, float* outY, float* outZ);
    __declspec(dllexport) int Spline_GetSampledPointCount(int entityID);
    __declspec(dllexport) bool Spline_GetSampledPoint(int entityID, int index, float* outX, float* outY, float* outZ);
    __declspec(dllexport) bool Spline_GetNearestPoint(int entityID, float worldX, float worldY, float worldZ,
                                                      float* outX, float* outY, float* outZ, float* outDistance);
    __declspec(dllexport) bool Spline_GetPointAtNormalized(int entityID, float t, float* outX, float* outY, float* outZ);

    // --- PortalComponent ---
    __declspec(dllexport) bool Portal_GetEnabled(int entityID);
    __declspec(dllexport) void Portal_SetEnabled(int entityID, bool enabled);
    __declspec(dllexport) const char* Portal_GetTargetScenePath(int entityID);
    __declspec(dllexport) void Portal_SetTargetScenePath(int entityID, const char* path);
    __declspec(dllexport) const char* Portal_GetTargetPortalGuid(int entityID);
    __declspec(dllexport) void Portal_SetTargetPortalGuid(int entityID, const char* guid);
    __declspec(dllexport) const char* Portal_GetTargetPortalPath(int entityID);
    __declspec(dllexport) void Portal_SetTargetPortalPath(int entityID, const char* path);
    __declspec(dllexport) void Portal_GetEntryOffset(int entityID, float* x, float* y, float* z);
    __declspec(dllexport) void Portal_SetEntryOffset(int entityID, float x, float y, float z);
    __declspec(dllexport) void Portal_GetExitOffset(int entityID, float* x, float* y, float* z);
    __declspec(dllexport) void Portal_SetExitOffset(int entityID, float x, float y, float z);
    __declspec(dllexport) bool Portal_GetAutoDetect(int entityID);
    __declspec(dllexport) void Portal_SetAutoDetect(int entityID, bool value);
    __declspec(dllexport) float Portal_GetTriggerRadius(int entityID);
    __declspec(dllexport) void Portal_SetTriggerRadius(int entityID, float value);
    __declspec(dllexport) bool Portal_GetFireExitEvents(int entityID);
    __declspec(dllexport) void Portal_SetFireExitEvents(int entityID, bool value);

    // --- CameraComponent ---
    // Get/Set the camera layer mask for culling (bit i corresponds to EntityData::Layer == i)
    __declspec(dllexport) unsigned int GetCameraLayerMask(int entityID);
    __declspec(dllexport) void SetCameraLayerMask(int entityID, unsigned int mask);
    // Enable/disable a single camera mask bit by project layer name
    __declspec(dllexport) void Camera_SetLayerMaskByName(int entityID, const char* layerName, bool enable);
    // Camera settings (Active, Priority, FOV, Near/Far clip, Perspective)
    __declspec(dllexport) bool GetCameraActive(int entityID);
    __declspec(dllexport) void SetCameraActive(int entityID, bool active);
    __declspec(dllexport) int GetCameraPriority(int entityID);
    __declspec(dllexport) void SetCameraPriority(int entityID, int priority);
    __declspec(dllexport) float GetCameraFieldOfView(int entityID);
    __declspec(dllexport) void SetCameraFieldOfView(int entityID, float fov);
    __declspec(dllexport) float GetCameraNearClip(int entityID);
    __declspec(dllexport) void SetCameraNearClip(int entityID, float nearClip);
    __declspec(dllexport) float GetCameraFarClip(int entityID);
    __declspec(dllexport) void SetCameraFarClip(int entityID, float farClip);
    __declspec(dllexport) bool GetCameraIsPerspective(int entityID);
    __declspec(dllexport) void SetCameraIsPerspective(int entityID, bool isPerspective);

    // --- Mesh property block (tint) ---
    __declspec(dllexport) void Mesh_SetColorTint(int entityID, float r, float g, float b, float a);
    __declspec(dllexport) void Mesh_GetColorTint(int entityID, float* r, float* g, float* b, float* a);

    // --- BlendShapeComponent ---
    __declspec(dllexport) void SetBlendShapeWeight(int entityID, const char* shapeName, float weight);
    __declspec(dllexport) float GetBlendShapeWeight(int entityID, const char* shapeName);
    __declspec(dllexport) int GetBlendShapeCount(int entityID);
    __declspec(dllexport) const char* GetBlendShapeName(int entityID, int index);

    // --- UnifiedMorphComponent (attached to skeleton/model root) ---
    __declspec(dllexport) int UnifiedMorph_GetCount(int entityID);
    __declspec(dllexport) const char* UnifiedMorph_GetName(int entityID, int index);
    __declspec(dllexport) float UnifiedMorph_GetWeight(int entityID, int index);
    __declspec(dllexport) void UnifiedMorph_SetWeight(int entityID, int index, float weight);
    __declspec(dllexport) void UnifiedMorph_PropagateAll(int entityID);

    // --- TintMaskController (controls tints on child meshes by name pattern) ---
    __declspec(dllexport) bool TintController_HasComponent(int entityID);
    __declspec(dllexport) const char* TintController_GetNamePattern(int entityID);
    __declspec(dllexport) void TintController_SetNamePattern(int entityID, const char* pattern);
    __declspec(dllexport) void TintController_GetBaseTint(int entityID, float* r, float* g, float* b, float* a);
    __declspec(dllexport) void TintController_SetBaseTint(int entityID, float r, float g, float b, float a);
    __declspec(dllexport) void TintController_GetTintColor(int entityID, int channel, float* r, float* g, float* b, float* a);
    __declspec(dllexport) void TintController_SetTintColor(int entityID, int channel, float r, float g, float b, float a);
    __declspec(dllexport) bool TintController_GetUseTintMask(int entityID);
    __declspec(dllexport) void TintController_SetUseTintMask(int entityID, bool use);
    __declspec(dllexport) bool TintController_GetUsePbrOverrides(int entityID);
    __declspec(dllexport) void TintController_SetUsePbrOverrides(int entityID, bool use);
    __declspec(dllexport) float TintController_GetPbrMetallic(int entityID);
    __declspec(dllexport) void TintController_SetPbrMetallic(int entityID, float value);
    __declspec(dllexport) float TintController_GetPbrRoughness(int entityID);
    __declspec(dllexport) void TintController_SetPbrRoughness(int entityID, float value);
    __declspec(dllexport) void TintController_GetPbrEmissionColor(int entityID, float* r, float* g, float* b);
    __declspec(dllexport) void TintController_SetPbrEmissionColor(int entityID, float r, float g, float b);
    __declspec(dllexport) float TintController_GetPbrEmissionStrength(int entityID);
    __declspec(dllexport) void TintController_SetPbrEmissionStrength(int entityID, float value);
    __declspec(dllexport) int TintController_GetGlobalBlendMode(int entityID);
    __declspec(dllexport) void TintController_SetGlobalBlendMode(int entityID, int blendMode);
    __declspec(dllexport) bool TintController_GetAutoIncludeParentedSkinnedMeshes(int entityID);
    __declspec(dllexport) void TintController_SetAutoIncludeParentedSkinnedMeshes(int entityID, bool enabled);
    __declspec(dllexport) void TintController_Refresh(int entityID);
    __declspec(dllexport) void TintController_ClearTargets(int entityID);
    __declspec(dllexport) void TintController_RemoveTargetsForEntity(int entityID, int targetEntityID);
    __declspec(dllexport) void TintController_AddTarget(int entityID, int targetEntityID, int materialSlot, int blendMode, bool useTargetColor, float r, float g, float b, float a);
    __declspec(dllexport) int TintController_GetTrackedTargetCount(int entityID);
    __declspec(dllexport) int TintController_GetTrackedTargetEntity(int entityID, int index);

    // --- BoneAttachment (attach entity to skeleton bone) ---
    __declspec(dllexport) bool BoneAttachment_HasComponent(int entityID);
    __declspec(dllexport) bool BoneAttachment_GetEnabled(int entityID);
    __declspec(dllexport) void BoneAttachment_SetEnabled(int entityID, bool enabled);
    __declspec(dllexport) const char* BoneAttachment_GetBoneName(int entityID);
    __declspec(dllexport) void BoneAttachment_SetBoneName(int entityID, const char* boneName);
    __declspec(dllexport) void BoneAttachment_GetLocalPosition(int entityID, float* x, float* y, float* z);
    __declspec(dllexport) void BoneAttachment_SetLocalPosition(int entityID, float x, float y, float z);
    __declspec(dllexport) void BoneAttachment_GetLocalRotation(int entityID, float* x, float* y, float* z);
    __declspec(dllexport) void BoneAttachment_SetLocalRotation(int entityID, float x, float y, float z);
    __declspec(dllexport) void BoneAttachment_GetLocalScale(int entityID, float* x, float* y, float* z);
    __declspec(dllexport) void BoneAttachment_SetLocalScale(int entityID, float x, float y, float z);
    __declspec(dllexport) bool BoneAttachment_GetInheritRotation(int entityID);
    __declspec(dllexport) void BoneAttachment_SetInheritRotation(int entityID, bool inherit);
    __declspec(dllexport) bool BoneAttachment_GetInheritScale(int entityID);
    __declspec(dllexport) void BoneAttachment_SetInheritScale(int entityID, bool inherit);
    __declspec(dllexport) bool BoneAttachment_IsResolved(int entityID);
    __declspec(dllexport) void BoneAttachment_InvalidateResolution(int entityID);
    __declspec(dllexport) int BoneAttachment_GetSkeletonEntity(int entityID);
    __declspec(dllexport) void BoneAttachment_SetSkeletonEntity(int entityID, int skeletonEntityID);

    // --- Animator / AnimationPlayer ---
    __declspec(dllexport) void Animator_SetBool(int entityID, const char* name, bool value);
    __declspec(dllexport) void Animator_SetInt(int entityID, const char* name, int value);
    __declspec(dllexport) void Animator_SetFloat(int entityID, const char* name, float value);
    __declspec(dllexport) void Animator_SetTrigger(int entityID, const char* name);
    __declspec(dllexport) void Animator_ResetTrigger(int entityID, const char* name);
    // Animator parameter getters
    __declspec(dllexport) bool  Animator_GetBool(int entityID, const char* name);
    __declspec(dllexport) int   Animator_GetInt(int entityID, const char* name);
    __declspec(dllexport) float Animator_GetFloat(int entityID, const char* name);
    __declspec(dllexport) bool  Animator_GetTrigger(int entityID, const char* name);
    // Single-clip controls and queries
    __declspec(dllexport) void AnimationPlayer_Play(int entityID);
    __declspec(dllexport) void AnimationPlayer_Stop(int entityID);
    __declspec(dllexport) bool AnimationPlayer_IsPlaying(int entityID);
    __declspec(dllexport) void AnimationPlayer_SetLoop(int entityID, bool loop);
    __declspec(dllexport) void AnimationPlayer_SetSpeed(int entityID, float speed);
    __declspec(dllexport) const char* AnimationPlayer_GetCurrentClipName(int entityID);
    __declspec(dllexport) const char* Animator_GetCurrentStateName(int entityID);
    __declspec(dllexport) const char* Animator_GetPreviousStateName(int entityID);
    // Predicted next base-layer state given current state + parameters/triggers.
    // Returns the current (self) state name when no transition currently qualifies.
    __declspec(dllexport) const char* Animator_GetNextStateName(int entityID);
    __declspec(dllexport) bool Animator_IsPlaying(int entityID);
    // Animator enable/disable (for ragdoll)
    __declspec(dllexport) bool Animator_GetEnabled(int entityID);
    __declspec(dllexport) void Animator_SetEnabled(int entityID, bool enabled);
    // Animator controller switching
    __declspec(dllexport) void Animator_SetController(int entityID, const char* controllerPath, float blendSeconds);
    __declspec(dllexport) void Animator_SetOverride(int entityID, const char* overridePath);
    __declspec(dllexport) int Animator_GetCurrentClipRootMotionMode(int entityID);

    // --- Runtime NPC scalability / significance ---
    __declspec(dllexport) bool NpcScalability_GetParticipates(int entityID);
    __declspec(dllexport) int NpcScalability_GetTier(int entityID);
    __declspec(dllexport) int NpcScalability_GetRepresentation(int entityID);
    __declspec(dllexport) uint32_t NpcScalability_GetReasonFlags(int entityID);
    __declspec(dllexport) float NpcScalability_GetCameraDistance(int entityID);
    __declspec(dllexport) bool NpcScalability_GetVisible(int entityID);
    __declspec(dllexport) int NpcScalability_GetCrowdRank(int entityID);
    __declspec(dllexport) int NpcScalability_GetCrowdCount(int entityID);
    __declspec(dllexport) float NpcScalability_GetAnimationUpdateInterval(int entityID);
    __declspec(dllexport) float NpcScalability_GetScriptUpdateInterval(int entityID);
    __declspec(dllexport) float NpcScalability_GetNavigationRepathInterval(int entityID);

    // --- Animation Layers ---
    __declspec(dllexport) int AnimLayer_GetOrCreate(int entityID, const char* layerName, int priority);
    __declspec(dllexport) bool AnimLayer_Remove(int entityID, const char* layerName);
    __declspec(dllexport) bool AnimLayer_Has(int entityID, const char* layerName);
    __declspec(dllexport) int AnimLayer_GetCount(int entityID);
    __declspec(dllexport) const char* AnimLayer_GetNameByIndex(int entityID, int index);
    __declspec(dllexport) void AnimLayer_SetAnimation(int entityID, const char* layerName, const char* animPath);
    __declspec(dllexport) const char* AnimLayer_GetAnimation(int entityID, const char* layerName);
    __declspec(dllexport) void AnimLayer_SetMask(int entityID, const char* layerName, int maskPreset);
    __declspec(dllexport) int AnimLayer_GetMask(int entityID, const char* layerName);
    __declspec(dllexport) void AnimLayer_SetBlendMode(int entityID, const char* layerName, int blendMode);
    __declspec(dllexport) int AnimLayer_GetBlendMode(int entityID, const char* layerName);
    __declspec(dllexport) void AnimLayer_SetWeight(int entityID, const char* layerName, float weight);
    __declspec(dllexport) float AnimLayer_GetWeight(int entityID, const char* layerName);
    __declspec(dllexport) void AnimLayer_BlendTo(int entityID, const char* layerName, float targetWeight, float blendSpeed);
    __declspec(dllexport) void AnimLayer_Play(int entityID, const char* layerName, bool loop);
    __declspec(dllexport) void AnimLayer_Stop(int entityID, const char* layerName);
    __declspec(dllexport) bool AnimLayer_IsPlaying(int entityID, const char* layerName);
    __declspec(dllexport) void AnimLayer_SetSpeed(int entityID, const char* layerName, float speed);
    __declspec(dllexport) float AnimLayer_GetSpeed(int entityID, const char* layerName);
    __declspec(dllexport) void AnimLayer_SetTime(int entityID, const char* layerName, float time);
    __declspec(dllexport) void AnimLayer_SetNormalizedTime(int entityID, const char* layerName, float normalizedTime);
    __declspec(dllexport) void AnimLayer_SetState(int entityID, const char* layerName, int stateId, float normalizedTime);
    __declspec(dllexport) void AnimLayer_SetStateByName(int entityID, const char* layerName, const char* stateName, float normalizedTime, bool satisfyTransitionConditions);
    __declspec(dllexport) void AnimLayer_SetBlend2D(int entityID, const char* layerName, float x, float y, bool clampToUnitRange);
    __declspec(dllexport) float AnimLayer_GetTime(int entityID, const char* layerName);
    __declspec(dllexport) float AnimLayer_GetDuration(int entityID, const char* layerName);
    __declspec(dllexport) void AnimLayer_SetLooping(int entityID, const char* layerName, bool loop);
    __declspec(dllexport) bool AnimLayer_GetLooping(int entityID, const char* layerName);

    // --- UI Button state ---
    __declspec(dllexport) bool UI_ButtonIsHovered(int entityID);
    __declspec(dllexport) bool UI_ButtonIsPressed(int entityID);
    __declspec(dllexport) bool UI_ButtonWasClicked(int entityID);

    // --- UI Slider ---
    __declspec(dllexport) float UI_Slider_GetValue(int entityID);
    __declspec(dllexport) void UI_Slider_SetValue(int entityID, float value);
    __declspec(dllexport) float UI_Slider_GetMinValue(int entityID);
    __declspec(dllexport) void UI_Slider_SetMinValue(int entityID, float value);
    __declspec(dllexport) float UI_Slider_GetMaxValue(int entityID);
    __declspec(dllexport) void UI_Slider_SetMaxValue(int entityID, float value);
    __declspec(dllexport) bool UI_Slider_IsHovered(int entityID);
    __declspec(dllexport) bool UI_Slider_IsDragging(int entityID);
    __declspec(dllexport) bool UI_Slider_ValueChanged(int entityID);

    // --- UI ProgressBar ---
    __declspec(dllexport) float UI_ProgressBar_GetValue(int entityID);
    __declspec(dllexport) void UI_ProgressBar_SetValue(int entityID, float value);
    __declspec(dllexport) float UI_ProgressBar_GetMinValue(int entityID);
    __declspec(dllexport) void UI_ProgressBar_SetMinValue(int entityID, float value);
    __declspec(dllexport) float UI_ProgressBar_GetMaxValue(int entityID);
    __declspec(dllexport) void UI_ProgressBar_SetMaxValue(int entityID, float value);
    __declspec(dllexport) float UI_ProgressBar_GetOpacity(int entityID);
    __declspec(dllexport) void UI_ProgressBar_SetOpacity(int entityID, float opacity);
    __declspec(dllexport) bool UI_ProgressBar_GetVisible(int entityID);
    __declspec(dllexport) void UI_ProgressBar_SetVisible(int entityID, bool visible);

    // --- UI Toggle ---
    __declspec(dllexport) bool UI_Toggle_GetIsOn(int entityID);
    __declspec(dllexport) void UI_Toggle_SetIsOn(int entityID, bool isOn);
    __declspec(dllexport) bool UI_Toggle_IsHovered(int entityID);
    __declspec(dllexport) bool UI_Toggle_IsPressed(int entityID);
    __declspec(dllexport) bool UI_Toggle_ValueChanged(int entityID);

    // --- UI Panel interaction ---
    __declspec(dllexport) bool UI_Panel_IsHovered(int entityID);
    __declspec(dllexport) bool UI_Panel_IsPressed(int entityID);
    __declspec(dllexport) bool UI_Panel_IsDragging(int entityID);
    __declspec(dllexport) bool UI_Panel_DragStarted(int entityID);
    __declspec(dllexport) bool UI_Panel_DragEnded(int entityID);
    __declspec(dllexport) bool UI_Panel_WasDropped(int entityID);
    __declspec(dllexport) int UI_Panel_GetDropSource(int entityID);
    __declspec(dllexport) int UI_Panel_GetDropTarget(int entityID);
    __declspec(dllexport) bool UI_Panel_GetAllowDrag(int entityID);
    __declspec(dllexport) void UI_Panel_SetAllowDrag(int entityID, bool allow);
    __declspec(dllexport) bool UI_Panel_GetAllowDrop(int entityID);
    __declspec(dllexport) void UI_Panel_SetAllowDrop(int entityID, bool allow);

    // --- UI ScrollView ---
    __declspec(dllexport) void UI_ScrollView_GetContentOffset(int entityID, float* x, float* y);
    __declspec(dllexport) void UI_ScrollView_SetContentOffset(int entityID, float x, float y);
    __declspec(dllexport) void UI_ScrollView_GetContentSize(int entityID, float* w, float* h);
    __declspec(dllexport) void UI_ScrollView_SetContentSize(int entityID, float w, float h);
    __declspec(dllexport) float UI_ScrollView_GetOpacity(int entityID);
    __declspec(dllexport) void UI_ScrollView_SetOpacity(int entityID, float opacity);
    __declspec(dllexport) bool UI_ScrollView_GetVisible(int entityID);
    __declspec(dllexport) void UI_ScrollView_SetVisible(int entityID, bool visible);

    // --- UI InputField ---
    __declspec(dllexport) void UI_InputField_GetText(int entityID, const char** outText);
    __declspec(dllexport) void UI_InputField_SetText(int entityID, const char* text);
    __declspec(dllexport) void UI_InputField_GetPlaceholder(int entityID, const char** outText);
    __declspec(dllexport) void UI_InputField_SetPlaceholder(int entityID, const char* text);
    __declspec(dllexport) bool UI_InputField_IsFocused(int entityID);
    __declspec(dllexport) bool UI_InputField_TextChanged(int entityID);

    // --- UI Dropdown ---
    __declspec(dllexport) int UI_Dropdown_GetSelectedIndex(int entityID);
    __declspec(dllexport) void UI_Dropdown_SetSelectedIndex(int entityID, int index);
    __declspec(dllexport) int UI_Dropdown_GetOptionCount(int entityID);
    __declspec(dllexport) void UI_Dropdown_GetOption(int entityID, int index, const char** outText);
    __declspec(dllexport) void UI_Dropdown_SetOption(int entityID, int index, const char* text);
    __declspec(dllexport) void UI_Dropdown_AddOption(int entityID, const char* text);
    __declspec(dllexport) void UI_Dropdown_ClearOptions(int entityID);
    __declspec(dllexport) bool UI_Dropdown_IsOpen(int entityID);
    __declspec(dllexport) bool UI_Dropdown_SelectionChanged(int entityID);

    // --- Material Property Block ---
    // Material functions are exposed via function pointers (see Material_*Ptr below)
    // Function pointer getters are exported for interop initialization
    __declspec(dllexport) void* Get_Material_SetVector4_Ptr();
    __declspec(dllexport) void* Get_Material_GetVector4_Ptr();
    __declspec(dllexport) void* Get_Material_HasProperty_Ptr();
    __declspec(dllexport) void* Get_Material_RemoveProperty_Ptr();
    __declspec(dllexport) void* Get_Material_ClearAll_Ptr();
    __declspec(dllexport) void* Get_Material_SetTexturePath_Ptr();
    __declspec(dllexport) void* Get_Material_SetVector4Slot_Ptr();
    __declspec(dllexport) void* Get_Material_GetVector4Slot_Ptr();
    __declspec(dllexport) void* Get_Material_HasPropertySlot_Ptr();
    __declspec(dllexport) void* Get_Material_RemovePropertySlot_Ptr();
    __declspec(dllexport) void* Get_Material_ClearSlot_Ptr();
    __declspec(dllexport) void* Get_Material_GetSlotCount_Ptr();
    __declspec(dllexport) void* Get_Material_SetTexturePathSlot_Ptr();
    __declspec(dllexport) void* Get_Material_GetMaterialTypeSlot_Ptr();
    __declspec(dllexport) void* Get_Material_GetMaterialNameSlot_Ptr();
    __declspec(dllexport) void* Get_Material_GetMaterialAssetPathSlot_Ptr();
    __declspec(dllexport) void* Get_Material_SetMaterialAssetPathSlot_Ptr();
    __declspec(dllexport) void* Get_Material_SetMaterialVector4Slot_Ptr();
    __declspec(dllexport) void* Get_Material_GetMaterialVector4Slot_Ptr();
    __declspec(dllexport) void* Get_Material_HasMaterialPropertySlot_Ptr();
    __declspec(dllexport) void* Get_Material_SetMaterialTexturePathSlot_Ptr();
    __declspec(dllexport) void* Get_Material_GetMaterialTexturePathSlot_Ptr();
    __declspec(dllexport) void* Get_Material_GetPbrScalarSlot_Ptr();
    __declspec(dllexport) void* Get_Material_SetPbrScalarSlot_Ptr();
    __declspec(dllexport) void* Get_Material_GetPbrEmissionColorSlot_Ptr();
    __declspec(dllexport) void* Get_Material_SetPbrEmissionColorSlot_Ptr();
    __declspec(dllexport) void* Get_Material_GetPbrUVTransformSlot_Ptr();
    __declspec(dllexport) void* Get_Material_SetPbrUVTransformSlot_Ptr();
    __declspec(dllexport) void* Get_Material_GetPbrReceiveShadowsOverrideSlot_Ptr();
    __declspec(dllexport) void* Get_Material_SetPbrReceiveShadowsOverrideSlot_Ptr();
    __declspec(dllexport) void* Get_Material_GetPbrReceiveShadowsSlot_Ptr();
    __declspec(dllexport) void* Get_Material_SetPbrReceiveShadowsSlot_Ptr();

    // --- Particle Emitter ---
    // Particle functions are exposed via function pointers (see Particle_*Ptr below)
    // Function pointer getters are exported for interop initialization (50 functions)
    __declspec(dllexport) void* Get_Particle_GetEnabled_Ptr();
    __declspec(dllexport) void* Get_Particle_SetEnabled_Ptr();
    __declspec(dllexport) void* Get_Particle_Play_Ptr();
    __declspec(dllexport) void* Get_Particle_Stop_Ptr();
    __declspec(dllexport) void* Get_Particle_Restart_Ptr();
    __declspec(dllexport) void* Get_Particle_IsPlaying_Ptr();
    __declspec(dllexport) void* Get_Particle_GetSimulationSpace_Ptr();
    __declspec(dllexport) void* Get_Particle_SetSimulationSpace_Ptr();
    __declspec(dllexport) void* Get_Particle_GetShape_Ptr();
    __declspec(dllexport) void* Get_Particle_SetShape_Ptr();
    __declspec(dllexport) void* Get_Particle_GetShapeRadius_Ptr();
    __declspec(dllexport) void* Get_Particle_SetShapeRadius_Ptr();
    __declspec(dllexport) void* Get_Particle_GetShapeAngle_Ptr();
    __declspec(dllexport) void* Get_Particle_SetShapeAngle_Ptr();
    __declspec(dllexport) void* Get_Particle_GetStartSpeed_Ptr();
    __declspec(dllexport) void* Get_Particle_SetStartSpeed_Ptr();
    __declspec(dllexport) void* Get_Particle_GetStartSize_Ptr();
    __declspec(dllexport) void* Get_Particle_SetStartSize_Ptr();
    __declspec(dllexport) void* Get_Particle_GetStartColor_Ptr();
    __declspec(dllexport) void* Get_Particle_SetStartColor_Ptr();
    __declspec(dllexport) void* Get_Particle_GetEmissionRate_Ptr();
    __declspec(dllexport) void* Get_Particle_SetEmissionRate_Ptr();
    __declspec(dllexport) void* Get_Particle_GetLooping_Ptr();
    __declspec(dllexport) void* Get_Particle_SetLooping_Ptr();
    __declspec(dllexport) void* Get_Particle_GetDuration_Ptr();
    __declspec(dllexport) void* Get_Particle_SetDuration_Ptr();
    __declspec(dllexport) void* Get_Particle_GetLifetime_Ptr();
    __declspec(dllexport) void* Get_Particle_SetLifetime_Ptr();
    __declspec(dllexport) void* Get_Particle_GetGravityModifier_Ptr();
    __declspec(dllexport) void* Get_Particle_SetGravityModifier_Ptr();
    __declspec(dllexport) void* Get_Particle_GetMaxParticles_Ptr();
    __declspec(dllexport) void* Get_Particle_SetMaxParticles_Ptr();
    __declspec(dllexport) void* Get_Particle_GetSizeOverLifetimeEnabled_Ptr();
    __declspec(dllexport) void* Get_Particle_SetSizeOverLifetimeEnabled_Ptr();
    __declspec(dllexport) void* Get_Particle_GetColorOverLifetimeEnabled_Ptr();
    __declspec(dllexport) void* Get_Particle_SetColorOverLifetimeEnabled_Ptr();
    __declspec(dllexport) void* Get_Particle_GetVelocityOverLifetimeEnabled_Ptr();
    __declspec(dllexport) void* Get_Particle_SetVelocityOverLifetimeEnabled_Ptr();
    __declspec(dllexport) void* Get_Particle_GetRotationOverLifetimeEnabled_Ptr();
    __declspec(dllexport) void* Get_Particle_SetRotationOverLifetimeEnabled_Ptr();
    __declspec(dllexport) void* Get_Particle_GetBurstEnabled_Ptr();
    __declspec(dllexport) void* Get_Particle_SetBurstEnabled_Ptr();
    __declspec(dllexport) void* Get_Particle_GetSizeOverLifetime_Ptr();
    __declspec(dllexport) void* Get_Particle_SetSizeOverLifetime_Ptr();
    __declspec(dllexport) void* Get_Particle_GetColorGradientKeyCount_Ptr();
    __declspec(dllexport) void* Get_Particle_GetColorGradientKey_Ptr();
    __declspec(dllexport) void* Get_Particle_SetColorGradientKey_Ptr();
    __declspec(dllexport) void* Get_Particle_ClearColorGradient_Ptr();
    __declspec(dllexport) void* Get_Particle_GetBurstCount_Ptr();
    __declspec(dllexport) void* Get_Particle_SetBurstCount_Ptr();

#ifdef __cplusplus
}
#endif

// UI Text function pointer declarations (for cross-TU linkage)
extern void (*UI_Text_GetTextPtr)(int, const char**);
extern void (*UI_Text_SetTextPtr)(int, const char*);
extern float (*UI_Text_GetOpacityPtr)(int);
extern void (*UI_Text_SetOpacityPtr)(int, float);
extern bool (*UI_Text_GetVisiblePtr)(int);
extern void (*UI_Text_SetVisiblePtr)(int, bool);
extern void (*UI_Text_GetColorPtr)(int, float*, float*, float*, float*);
extern void (*UI_Text_SetColorPtr)(int, float, float, float, float);
extern float (*UI_Text_GetPixelSizePtr)(int);
extern void (*UI_Text_SetPixelSizePtr)(int, float);
extern int (*UI_Text_GetZOrderPtr)(int);
extern void (*UI_Text_SetZOrderPtr)(int, int);
extern void (*UI_Text_GetFontPathPtr)(int, const char**);
extern void (*UI_Text_SetFontPathPtr)(int, const char*);
extern bool (*UI_Text_GetAnchorEnabledPtr)(int);
extern void (*UI_Text_SetAnchorEnabledPtr)(int, bool);
extern int (*UI_Text_GetAnchorPtr)(int);
extern void (*UI_Text_SetAnchorPtr)(int, int);
extern void (*UI_Text_GetAnchorOffsetPtr)(int, float*, float*);
extern void (*UI_Text_SetAnchorOffsetPtr)(int, float, float);
extern bool (*UI_Text_GetWordWrapPtr)(int);
extern void (*UI_Text_SetWordWrapPtr)(int, bool);
extern void (*UI_Text_GetRectSizePtr)(int, float*, float*);
extern void (*UI_Text_SetRectSizePtr)(int, float, float);
extern bool (*UI_Text_GetWorldSpacePtr)(int);
extern void (*UI_Text_SetWorldSpacePtr)(int, bool);
extern bool (*UI_Text_GetBillboardPtr)(int);
extern void (*UI_Text_SetBillboardPtr)(int, bool);
extern bool (*UI_Text_GetOutlineEnabledPtr)(int);
extern void (*UI_Text_SetOutlineEnabledPtr)(int, bool);
extern void (*UI_Text_GetOutlineColorPtr)(int, float*, float*, float*, float*);
extern void (*UI_Text_SetOutlineColorPtr)(int, float, float, float, float);
extern float (*UI_Text_GetOutlineThicknessPtr)(int);
extern void (*UI_Text_SetOutlineThicknessPtr)(int, float);
extern bool (*UI_Text_GetShadowEnabledPtr)(int);
extern void (*UI_Text_SetShadowEnabledPtr)(int, bool);
extern void (*UI_Text_GetShadowColorPtr)(int, float*, float*, float*, float*);
extern void (*UI_Text_SetShadowColorPtr)(int, float, float, float, float);
extern void (*UI_Text_GetShadowOffsetPtr)(int, float*, float*);
extern void (*UI_Text_SetShadowOffsetPtr)(int, float, float);
extern int (*UI_Text_GetAlignmentPtr)(int);
extern void (*UI_Text_SetAlignmentPtr)(int, int);

// UI Panel / Canvas function pointer declarations
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
// UI Rect (parent-relative)
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
extern int (*UI_Canvas_GetRenderSpacePtr)(int);
extern void (*UI_Canvas_SetRenderSpacePtr)(int, int);
extern bool (*UI_Canvas_GetBillboardPtr)(int);
extern void (*UI_Canvas_SetBillboardPtr)(int, bool);

// UI LayoutGroup function pointer declarations
extern int (*UI_LayoutGroup_GetDirectionPtr)(int);
extern void (*UI_LayoutGroup_SetDirectionPtr)(int, int);
extern void (*UI_LayoutGroup_GetPaddingPtr)(int, float*, float*, float*, float*);
extern void (*UI_LayoutGroup_SetPaddingPtr)(int, float, float, float, float);
extern float (*UI_LayoutGroup_GetSpacingPtr)(int);
extern void (*UI_LayoutGroup_SetSpacingPtr)(int, float);
extern int (*UI_LayoutGroup_GetChildAlignmentPtr)(int);
extern void (*UI_LayoutGroup_SetChildAlignmentPtr)(int, int);
extern int (*UI_LayoutGroup_GetCrossAlignmentPtr)(int);
extern void (*UI_LayoutGroup_SetCrossAlignmentPtr)(int, int);
extern bool (*UI_LayoutGroup_GetControlChildWidthPtr)(int);
extern void (*UI_LayoutGroup_SetControlChildWidthPtr)(int, bool);
extern bool (*UI_LayoutGroup_GetControlChildHeightPtr)(int);
extern void (*UI_LayoutGroup_SetControlChildHeightPtr)(int, bool);
extern bool (*UI_LayoutGroup_GetReverseOrderPtr)(int);
extern void (*UI_LayoutGroup_SetReverseOrderPtr)(int, bool);

// UI FitToContent function pointer declarations
extern bool (*UI_FitToContent_GetEnabledPtr)(int);
extern void (*UI_FitToContent_SetEnabledPtr)(int, bool);
extern bool (*UI_FitToContent_GetFitWidthPtr)(int);
extern void (*UI_FitToContent_SetFitWidthPtr)(int, bool);
extern bool (*UI_FitToContent_GetFitHeightPtr)(int);
extern void (*UI_FitToContent_SetFitHeightPtr)(int, bool);
extern void (*UI_FitToContent_GetPaddingPtr)(int, float*, float*, float*, float*);
extern void (*UI_FitToContent_SetPaddingPtr)(int, float, float, float, float);
extern void (*UI_FitToContent_GetMinSizePtr)(int, float*, float*);
extern void (*UI_FitToContent_SetMinSizePtr)(int, float, float);
extern void (*UI_FitToContent_GetMaxSizePtr)(int, float*, float*);
extern void (*UI_FitToContent_SetMaxSizePtr)(int, float, float);
extern bool (*UI_FitToContent_GetDirectChildrenOnlyPtr)(int);
extern void (*UI_FitToContent_SetDirectChildrenOnlyPtr)(int, bool);

// UI Scene Capture function pointer declarations
extern bool (*UI_SceneCapture_GetEnabledPtr)(int);
extern void (*UI_SceneCapture_SetEnabledPtr)(int, bool);
extern bool (*UI_SceneCapture_GetAutoFramePtr)(int);
extern void (*UI_SceneCapture_SetAutoFramePtr)(int, bool);
extern bool (*UI_SceneCapture_GetIncludeChildrenPtr)(int);
extern void (*UI_SceneCapture_SetIncludeChildrenPtr)(int, bool);
extern float (*UI_SceneCapture_GetBoundsPaddingPtr)(int);
extern void (*UI_SceneCapture_SetBoundsPaddingPtr)(int, float);
extern float (*UI_SceneCapture_GetFieldOfViewPtr)(int);
extern void (*UI_SceneCapture_SetFieldOfViewPtr)(int, float);
extern float (*UI_SceneCapture_GetNearClipPtr)(int);
extern void (*UI_SceneCapture_SetNearClipPtr)(int, float);
extern float (*UI_SceneCapture_GetFarClipPtr)(int);
extern void (*UI_SceneCapture_SetFarClipPtr)(int, float);
extern void (*UI_SceneCapture_GetViewDirectionPtr)(int, float*, float*, float*);
extern void (*UI_SceneCapture_SetViewDirectionPtr)(int, float, float, float);
extern void (*UI_SceneCapture_GetUpDirectionPtr)(int, float*, float*, float*);
extern void (*UI_SceneCapture_SetUpDirectionPtr)(int, float, float, float);
extern void (*UI_SceneCapture_GetFocusOffsetPtr)(int, float*, float*, float*);
extern void (*UI_SceneCapture_SetFocusOffsetPtr)(int, float, float, float);
extern int (*UI_SceneCapture_GetTargetEntityPtr)(int);
extern void (*UI_SceneCapture_SetTargetEntityPtr)(int, int);
extern void (*UI_SceneCapture_GetRenderSizePtr)(int, int*, int*);
extern void (*UI_SceneCapture_SetRenderSizePtr)(int, int, int);
extern void (*UI_SceneCapture_GetClearColorPtr)(int, float*, float*, float*, float*);
extern void (*UI_SceneCapture_SetClearColorPtr)(int, float, float, float, float);
extern bool (*UI_SceneCapture_GetShowGridPtr)(int);
extern void (*UI_SceneCapture_SetShowGridPtr)(int, bool);
extern bool (*UI_SceneCapture_GetLockViewToTargetPtr)(int);
extern void (*UI_SceneCapture_SetLockViewToTargetPtr)(int, bool);

// UI Slider function pointer declarations
extern float (*UI_Slider_GetValuePtr)(int);
extern void (*UI_Slider_SetValuePtr)(int, float);
extern float (*UI_Slider_GetMinValuePtr)(int);
extern void (*UI_Slider_SetMinValuePtr)(int, float);
extern float (*UI_Slider_GetMaxValuePtr)(int);
extern void (*UI_Slider_SetMaxValuePtr)(int, float);
extern bool (*UI_Slider_IsHoveredPtr)(int);
extern bool (*UI_Slider_IsDraggingPtr)(int);
extern bool (*UI_Slider_ValueChangedPtr)(int);

// UI ProgressBar function pointer declarations
extern float (*UI_ProgressBar_GetValuePtr)(int);
extern void (*UI_ProgressBar_SetValuePtr)(int, float);
extern float (*UI_ProgressBar_GetMinValuePtr)(int);
extern void (*UI_ProgressBar_SetMinValuePtr)(int, float);
extern float (*UI_ProgressBar_GetMaxValuePtr)(int);
extern void (*UI_ProgressBar_SetMaxValuePtr)(int, float);
extern float (*UI_ProgressBar_GetOpacityPtr)(int);
extern void (*UI_ProgressBar_SetOpacityPtr)(int, float);
extern bool (*UI_ProgressBar_GetVisiblePtr)(int);
extern void (*UI_ProgressBar_SetVisiblePtr)(int, bool);

// UI Toggle function pointer declarations
extern bool (*UI_Toggle_GetIsOnPtr)(int);
extern void (*UI_Toggle_SetIsOnPtr)(int, bool);
extern bool (*UI_Toggle_IsHoveredPtr)(int);
extern bool (*UI_Toggle_IsPressedPtr)(int);
extern bool (*UI_Toggle_ValueChangedPtr)(int);

// UI ScrollView function pointer declarations
extern void (*UI_ScrollView_GetContentOffsetPtr)(int, float*, float*);
extern void (*UI_ScrollView_SetContentOffsetPtr)(int, float, float);
extern void (*UI_ScrollView_GetContentSizePtr)(int, float*, float*);
extern void (*UI_ScrollView_SetContentSizePtr)(int, float, float);
extern float (*UI_ScrollView_GetOpacityPtr)(int);
extern void (*UI_ScrollView_SetOpacityPtr)(int, float);
extern bool (*UI_ScrollView_GetVisiblePtr)(int);
extern void (*UI_ScrollView_SetVisiblePtr)(int, bool);

// UI InputField function pointer declarations
extern void (*UI_InputField_GetTextPtr)(int, const char**);
extern void (*UI_InputField_SetTextPtr)(int, const char*);
extern void (*UI_InputField_GetPlaceholderPtr)(int, const char**);
extern void (*UI_InputField_SetPlaceholderPtr)(int, const char*);
extern bool (*UI_InputField_IsFocusedPtr)(int);
extern bool (*UI_InputField_TextChangedPtr)(int);

// UI Dropdown function pointer declarations
extern int (*UI_Dropdown_GetSelectedIndexPtr)(int);
extern void (*UI_Dropdown_SetSelectedIndexPtr)(int, int);
extern int (*UI_Dropdown_GetOptionCountPtr)(int);
extern void (*UI_Dropdown_GetOptionPtr)(int, int, const char**);
extern void (*UI_Dropdown_SetOptionPtr)(int, int, const char*);
extern void (*UI_Dropdown_AddOptionPtr)(int, const char*);
extern void (*UI_Dropdown_ClearOptionsPtr)(int);
extern bool (*UI_Dropdown_IsOpenPtr)(int);
extern bool (*UI_Dropdown_SelectionChangedPtr)(int);

// Material Property Block function pointer declarations
extern void (*Material_SetVector4Ptr)(int, const char*, float, float, float, float);
extern void (*Material_GetVector4Ptr)(int, const char*, float*, float*, float*, float*);
extern bool (*Material_HasPropertyPtr)(int, const char*);
extern void (*Material_RemovePropertyPtr)(int, const char*);
extern void (*Material_ClearAllPtr)(int);
extern void (*Material_SetTexturePathPtr)(int, const char*, const char*);
extern void (*Material_SetVector4SlotPtr)(int, int, const char*, float, float, float, float);
extern void (*Material_GetVector4SlotPtr)(int, int, const char*, float*, float*, float*, float*);
extern bool (*Material_HasPropertySlotPtr)(int, int, const char*);
extern void (*Material_RemovePropertySlotPtr)(int, int, const char*);
extern void (*Material_ClearSlotPtr)(int, int);
extern int (*Material_GetSlotCountPtr)(int);
extern void (*Material_SetTexturePathSlotPtr)(int, int, const char*, const char*);
extern int (*Material_GetMaterialTypeSlotPtr)(int, int);
extern const char* (*Material_GetMaterialNameSlotPtr)(int, int);
extern const char* (*Material_GetMaterialAssetPathSlotPtr)(int, int);
extern bool (*Material_SetMaterialAssetPathSlotPtr)(int, int, const char*);
extern void (*Material_SetMaterialVector4SlotPtr)(int, int, const char*, float, float, float, float);
extern void (*Material_GetMaterialVector4SlotPtr)(int, int, const char*, float*, float*, float*, float*);
extern bool (*Material_HasMaterialPropertySlotPtr)(int, int, const char*);
extern void (*Material_SetMaterialTexturePathSlotPtr)(int, int, const char*, const char*);
extern const char* (*Material_GetMaterialTexturePathSlotPtr)(int, int, const char*);
extern float (*Material_GetPbrScalarSlotPtr)(int, int, int);
extern void (*Material_SetPbrScalarSlotPtr)(int, int, int, float);
extern void (*Material_GetPbrEmissionColorSlotPtr)(int, int, float*, float*, float*);
extern void (*Material_SetPbrEmissionColorSlotPtr)(int, int, float, float, float);
extern void (*Material_GetPbrUVTransformSlotPtr)(int, int, float*, float*, float*, float*);
extern void (*Material_SetPbrUVTransformSlotPtr)(int, int, float, float, float, float);
extern bool (*Material_GetPbrReceiveShadowsOverrideSlotPtr)(int, int);
extern void (*Material_SetPbrReceiveShadowsOverrideSlotPtr)(int, int, bool);
extern bool (*Material_GetPbrReceiveShadowsSlotPtr)(int, int);
extern void (*Material_SetPbrReceiveShadowsSlotPtr)(int, int, bool);

// Particle Emitter function pointer declarations (50 functions)
extern bool (*Particle_GetEnabledPtr)(int);
extern void (*Particle_SetEnabledPtr)(int, bool);
extern void (*Particle_PlayPtr)(int);
extern void (*Particle_StopPtr)(int);
extern void (*Particle_RestartPtr)(int);
extern bool (*Particle_IsPlayingPtr)(int);
extern int (*Particle_GetSimulationSpacePtr)(int);
extern void (*Particle_SetSimulationSpacePtr)(int, int);
extern int (*Particle_GetShapePtr)(int);
extern void (*Particle_SetShapePtr)(int, int);
extern float (*Particle_GetShapeRadiusPtr)(int);
extern void (*Particle_SetShapeRadiusPtr)(int, float);
extern float (*Particle_GetShapeAnglePtr)(int);
extern void (*Particle_SetShapeAnglePtr)(int, float);
extern void (*Particle_GetStartSpeedPtr)(int, float*, float*);
extern void (*Particle_SetStartSpeedPtr)(int, float, float);
extern void (*Particle_GetStartSizePtr)(int, float*, float*);
extern void (*Particle_SetStartSizePtr)(int, float, float);
extern void (*Particle_GetStartColorPtr)(int, float*, float*, float*, float*);
extern void (*Particle_SetStartColorPtr)(int, float, float, float, float);
extern float (*Particle_GetEmissionRatePtr)(int);
extern void (*Particle_SetEmissionRatePtr)(int, float);
extern bool (*Particle_GetLoopingPtr)(int);
extern void (*Particle_SetLoopingPtr)(int, bool);
extern float (*Particle_GetDurationPtr)(int);
extern void (*Particle_SetDurationPtr)(int, float);
extern void (*Particle_GetLifetimePtr)(int, float*, float*);
extern void (*Particle_SetLifetimePtr)(int, float, float);
extern float (*Particle_GetGravityModifierPtr)(int);
extern void (*Particle_SetGravityModifierPtr)(int, float);
extern int (*Particle_GetMaxParticlesPtr)(int);
extern void (*Particle_SetMaxParticlesPtr)(int, int);
extern bool (*Particle_GetSizeOverLifetimeEnabledPtr)(int);
extern void (*Particle_SetSizeOverLifetimeEnabledPtr)(int, bool);
extern bool (*Particle_GetColorOverLifetimeEnabledPtr)(int);
extern void (*Particle_SetColorOverLifetimeEnabledPtr)(int, bool);
extern bool (*Particle_GetVelocityOverLifetimeEnabledPtr)(int);
extern void (*Particle_SetVelocityOverLifetimeEnabledPtr)(int, bool);
extern bool (*Particle_GetRotationOverLifetimeEnabledPtr)(int);
extern void (*Particle_SetRotationOverLifetimeEnabledPtr)(int, bool);
extern bool (*Particle_GetBurstEnabledPtr)(int);
extern void (*Particle_SetBurstEnabledPtr)(int, bool);
extern void (*Particle_GetSizeOverLifetimePtr)(int, float*, float*, int*);
extern void (*Particle_SetSizeOverLifetimePtr)(int, float, float, int);
extern int (*Particle_GetColorGradientKeyCountPtr)(int);
extern void (*Particle_GetColorGradientKeyPtr)(int, int, float*, float*, float*, float*, float*);
extern void (*Particle_SetColorGradientKeyPtr)(int, int, float, float, float, float, float);
extern void (*Particle_ClearColorGradientPtr)(int);
extern int (*Particle_GetBurstCountPtr)(int);
extern void (*Particle_SetBurstCountPtr)(int, int);
