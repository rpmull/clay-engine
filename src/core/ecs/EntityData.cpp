#include "EntityData.h"
#include "Components.h"
#include "core/ecs/Scene.h"
#include "core/deformation/ArmorFitComponent.h"
#include "core/resourcelayer/ResourceLayerTypes.h"

///----------------------------------------------------------------------
/// DeepCopy: copy this entity data, assigning a new ID and
/// optionally a new scene
///----------------------------------------------------------------------
EntityData EntityData::DeepCopy(EntityID ID, Scene* newScene) const {
   
   EntityData copy;
   
   copy.Name = Name;
   copy.Transform = Transform;
   // Recompute LocalMatrix from Position/Rotation/Scale to ensure it's correct
   // (the copied LocalMatrix might be stale if source scene transforms weren't updated)
   copy.Transform.LocalMatrix = copy.Transform.CalculateLocalMatrix();
   copy.Layer = Layer;
   copy.Tag = Tag;
   copy.Groups = Groups;
   copy.Visible = Visible;
   copy.PresentationHidden = PresentationHidden;
   copy.Active = Active;
   copy.PrefabAsyncPending = PrefabAsyncPending;
   copy.PrefabAsyncFailed = PrefabAsyncFailed;
   copy.Parent = Parent;
   copy.Children = Children;
   // Assign a fresh GUID so duplicated entities don't share identity
   copy.EntityGuid = ClaymoreGUID::Generate();
   copy.PrefabGuid = PrefabGuid;
   copy.PrefabSource = PrefabSource;
   copy.Extra = Extra;
   copy.ModelAssetGuid = ModelAssetGuid;
   copy.DeletedModelNodes = DeletedModelNodes;
   
   //--------------------------------------------------
   // Dynamic sidecar components: deep copy by value 
   //--------------------------------------------------
   copy.Dynamic.clear();
   for (const auto& kv : Dynamic) {
      const cm::TypeId& tid = kv.first;
      const cm::ModuleComponent& src = kv.second;
      
      cm::ModuleComponent dst(tid, src.GetVersion());
      // Recreate field layout and copy values in declared order
      
      std::vector<cm::FieldDesc> defs; defs.reserve(src.Fields().size());
      
      for (const auto& f : src.Fields()) {
         cm::FieldDesc d; d.name = f.name; d.type = f.data.type; d.flags = 0; d.arrayRank = 0; d.enumType = "";
         defs.push_back(std::move(d));
      }
      
      dst.DefineFields(defs);
      for (const auto& f : src.Fields()) {
         dst.Set(f.name, f.data);
      }
      copy.Dynamic.emplace(tid, std::move(dst));
   }

   //----------------------------------------------
   // Deep copy Native Components
   //----------------------------------------------
   if (Mesh)
      copy.Mesh = std::make_unique<MeshComponent>(*Mesh);

   // Deep copy RenderOverridesComponent (alpha cutout/blend, shadows, etc.)
   if (RenderOverrides)
      copy.RenderOverrides = std::make_unique<RenderOverridesComponent>(*RenderOverrides);

   // Deep copy Collider (already being rebuilt, but safe to copy config)
   if (Collider)
      copy.Collider = std::make_unique<ColliderComponent>(*Collider);

   // Deep copy LightComponent
   if (Light)
      copy.Light = std::make_unique<LightComponent>(*Light);

   // Deep copy CameraComponent
   if (Camera)
      copy.Camera = std::make_unique<CameraComponent>(*Camera);

   // Deep copy RigidBodyComponent
   if (RigidBody) {
      copy.RigidBody = std::make_unique<RigidBodyComponent>(*RigidBody);
      // Runtime-only: reset body handle so play mode creates a fresh body
      copy.RigidBody->BodyID = JPH::BodyID();
      copy.RigidBody->PendingForce = glm::vec3(0.0f);
      copy.RigidBody->PendingTorque = glm::vec3(0.0f);
      copy.RigidBody->PendingImpulse = glm::vec3(0.0f);
      copy.RigidBody->PendingAngularImpulse = glm::vec3(0.0f);
      copy.RigidBody->_LastAppliedGravityFactor = -1.0f;
   }

   // Deep copy StaticBodyComponent
   if (StaticBody) {
      copy.StaticBody = std::make_unique<StaticBodyComponent>(*StaticBody);
      // Runtime-only: reset body handle so play mode creates a fresh body
      copy.StaticBody->BodyID = JPH::BodyID();
   }

   // Deep copy SoftbodyComponent (authoring data only; runtime cleared)
   if (Softbody) {
      copy.Softbody = std::make_unique<SoftbodyComponent>(*Softbody);
      copy.Softbody->BodyID = JPH::BodyID();
      copy.Softbody->RuntimeSharedSettings = nullptr;
      copy.Softbody->RuntimeMesh.reset();
      copy.Softbody->SourceMesh.reset();
      copy.Softbody->RuntimeNormals.clear();
      copy.Softbody->ScratchVertices.clear();
      copy.Softbody->ScratchSkinnedVertices.clear();
   }

   // Deep copy CharacterControllerComponent (authoring params only; runtime cleared)
   if (CharacterController) {
      copy.CharacterController = std::make_unique<CharacterControllerComponent>(*CharacterController);
      copy.CharacterController->Character = nullptr;
      copy.CharacterController->IsGrounded = false;
      copy.CharacterController->DesiredVelocity = glm::vec3(0.0f);
      copy.CharacterController->VerticalVelocity = 0.0f;
      copy.CharacterController->JumpRequested = false;
   }

   // Deep copy TerrainComponent
   if (Terrain)
   {
      copy.Terrain = std::make_unique<TerrainComponent>(*Terrain);
      copy.Terrain->ResetAssetIdentity();
   }

   // Deep copy ResourceLayerComponent (settings only, runtime regenerated)
   if (ResourceLayers)
   {
      copy.ResourceLayers = std::make_unique<cm::resourcelayer::ResourceLayerComponent>();
      copy.ResourceLayers->Climate = ResourceLayers->Climate;
      copy.ResourceLayers->UseClimateGradients = ResourceLayers->UseClimateGradients;
      copy.ResourceLayers->Layers = ResourceLayers->Layers;
      copy.ResourceLayers->GlobalSeed = ResourceLayers->GlobalSeed;
      copy.ResourceLayers->GlobalDensityMultiplier = ResourceLayers->GlobalDensityMultiplier;
      copy.ResourceLayers->GlobalSwapDistance = ResourceLayers->GlobalSwapDistance;
      copy.ResourceLayers->SwapHysteresis = ResourceLayers->SwapHysteresis;
      copy.ResourceLayers->MaxActivePrefabs = ResourceLayers->MaxActivePrefabs;
      copy.ResourceLayers->Regions = ResourceLayers->Regions;
      copy.ResourceLayers->Roads = ResourceLayers->Roads;
      copy.ResourceLayers->Persistent = ResourceLayers->Persistent;
      copy.ResourceLayers->NeedsFullRegeneration = true;
   }

   // Deep copy SplineComponent (control points and settings)
   if (Spline)
      copy.Spline = std::make_unique<SplineComponent>(*Spline);

   // Deep copy InstancerComponent (settings only, runtime regenerated)
   if (Instancer)
   {
      copy.Instancer = std::make_unique<cm::instancer::InstancerComponent>();
      // Asset references
      copy.Instancer->MeshAsset = Instancer->MeshAsset;
      copy.Instancer->MeshPath = Instancer->MeshPath;
      copy.Instancer->PrefabAsset = Instancer->PrefabAsset;
      copy.Instancer->PrefabPath = Instancer->PrefabPath;
      copy.Instancer->SurfaceEntity = Instancer->SurfaceEntity;
      // Distribution settings
      copy.Instancer->Distribution = Instancer->Distribution;
      copy.Instancer->DistributionRadius = Instancer->DistributionRadius;
      copy.Instancer->DistributionAreaMin = Instancer->DistributionAreaMin;
      copy.Instancer->DistributionAreaMax = Instancer->DistributionAreaMax;
      copy.Instancer->UseRadiusMode = Instancer->UseRadiusMode;
      copy.Instancer->UseManualPoints = Instancer->UseManualPoints;
      copy.Instancer->ManualPoints = Instancer->ManualPoints;
      // Swap settings
      copy.Instancer->Swap = Instancer->Swap;
      // Flags
      copy.Instancer->Enabled = Instancer->Enabled;
      copy.Instancer->PreviewColor = Instancer->PreviewColor;
      copy.Instancer->ShowDebugMarkers = Instancer->ShowDebugMarkers;
      copy.Instancer->ShowBounds = Instancer->ShowBounds;
      // Rendering options
      copy.Instancer->UseAlphaCutout = Instancer->UseAlphaCutout;
      copy.Instancer->AlphaCutoutThreshold = Instancer->AlphaCutoutThreshold;
      // Persistent state (destroyed/modified instances)
      copy.Instancer->Persistent = Instancer->Persistent;
      // Mark for regeneration in new scene
      copy.Instancer->NeedsRegeneration = true;
      copy.Instancer->NeedsMeshReload = true;
   }

   // Deep copy ParticleEmitterComponent
   if (Emitter) {
      copy.Emitter = std::make_unique<ParticleEmitterComponent>(*Emitter);
      // CRITICAL: Reset emitter HANDLE for fresh creation in the new scene.
      // Without this, play mode particles don't work because copied handles
      // belong to the editor scene's particle system registration.
      copy.Emitter->Handle = { uint16_t{UINT16_MAX} };
      
      // CRITICAL: Do not carry sprite handles into the runtime clone.
      // Sprite handles are ref-counted; copying the handle without a matching
      // Acquire/Release will eventually invalidate the atlas entry across play sessions.
      copy.Emitter->SpriteHandle = { uint16_t{UINT16_MAX} };
      copy.Emitter->Uniforms.m_handle = { uint16_t{UINT16_MAX} };
      
      copy.Emitter->IsPlaying = false;
      copy.Emitter->HasEmitted = false;
      copy.Emitter->ElapsedTime = 0.0f;
      copy.Emitter->BurstCyclesRemaining = copy.Emitter->BurstCycles;
      copy.Emitter->NextBurstTime = copy.Emitter->BurstTime;
      copy.Emitter->JustCreated = false; // Will be set to true when emitter is created in runtime scene
    }

  // Deep copy Area (Area3D)
  if (Area)
     copy.Area = std::make_unique<cm::physics::AreaComponent>(*Area);

   // Deep copy TextRendererComponent
   if (Text)
      copy.Text = std::make_unique<TextRendererComponent>(*Text);

   // Deep copy UI components
   if (Canvas)
      copy.Canvas = std::make_unique<CanvasComponent>(*Canvas);
   if (Panel)
      copy.Panel = std::make_unique<PanelComponent>(*Panel);
   if (copy.Panel) {
      copy.Panel->Hovered = false;
      copy.Panel->Pressed = false;
      copy.Panel->Dragging = false;
      copy.Panel->DragStarted = false;
      copy.Panel->DragEnded = false;
      copy.Panel->Dropped = false;
      copy.Panel->DropSourceEntity = -1;
      copy.Panel->DropTargetEntity = -1;
      copy.Panel->UseExternalTexture = false;
      copy.Panel->ExternalTextureHandle = BGFX_INVALID_HANDLE;
   }
   if (Button)
      copy.Button = std::make_unique<ButtonComponent>(*Button);
   if (Slider)
      copy.Slider = std::make_unique<SliderComponent>(*Slider);
   if (ProgressBar)
      copy.ProgressBar = std::make_unique<ProgressBarComponent>(*ProgressBar);
   if (Toggle)
      copy.Toggle = std::make_unique<ToggleComponent>(*Toggle);
   if (ScrollView)
      copy.ScrollView = std::make_unique<ScrollViewComponent>(*ScrollView);
   if (LayoutGroup)
      copy.LayoutGroup = std::make_unique<LayoutGroupComponent>(*LayoutGroup);
   if (InputField)
      copy.InputField = std::make_unique<InputFieldComponent>(*InputField);
   if (Dropdown)
      copy.Dropdown = std::make_unique<DropdownComponent>(*Dropdown);
   if (UIRect) {
      copy.UIRect = std::make_unique<UIRectComponent>(*UIRect);
      // Reset runtime state
      copy.UIRect->_ComputedRect = glm::vec4(0.0f);
      copy.UIRect->_RectDirty = true;
   }
   if (FitToContent) {
      copy.FitToContent = std::make_unique<FitToContentComponent>(*FitToContent);
      // Reset runtime state
      copy.FitToContent->_CachedChildrenBounds = glm::vec4(0.0f);
      copy.FitToContent->_BoundsDirty = true;
   }
   if (UISceneCapture) {
      copy.UISceneCapture = std::make_unique<UISceneCaptureComponent>(*UISceneCapture);
      // Reset runtime state so a new view ID is allocated per clone
      copy.UISceneCapture->_ViewIdBase = 0;
   }

   // Deep copy Audio components
   if (AudioSource) {
      copy.AudioSource = std::make_unique<AudioSourceComponent>(*AudioSource);
      // Reset runtime state for fresh playback in play mode
      copy.AudioSource->SoundHandle = INVALID_AUDIO_HANDLE;
      copy.AudioSource->IsPlaying = false;
      copy.AudioSource->IsPaused = false;
      copy.AudioSource->Initialized = false;
      copy.AudioSource->PlayRequested = false;
      copy.AudioSource->StopRequested = false;
      copy.AudioSource->PauseRequested = false;
      copy.AudioSource->ResumeRequested = false;
      copy.AudioSource->LastPosition = glm::vec3(0.0f);
   }
   if (AudioListener) {
      copy.AudioListener = std::make_unique<AudioListenerComponent>(*AudioListener);
      // Reset runtime state
      copy.AudioListener->LastPosition = glm::vec3(0.0f);
      copy.AudioListener->Velocity = glm::vec3(0.0f);
      copy.AudioListener->WasActive = false;
   }

   if (BlendShapes)
      copy.BlendShapes = std::make_unique<BlendShapeComponent>(*BlendShapes);
   copy.PendingBlendShapeWeights = PendingBlendShapeWeights;
   if (UnifiedMorph)
      copy.UnifiedMorph = std::make_unique<UnifiedMorphComponent>(*UnifiedMorph);
   copy.PendingUnifiedMorphWeights = PendingUnifiedMorphWeights;
   if (TintController)
      copy.TintController = std::make_unique<TintMaskController>(*TintController);
   
   // Keep MeshComponent's raw pointer in sync with the copied BlendShapes so sliders work in Play Mode
   if (copy.Mesh && copy.BlendShapes)
      copy.Mesh->BlendShapes = copy.BlendShapes.get();
   
   if (Skeleton) {
      copy.Skeleton = std::make_unique<SkeletonComponent>();
      copy.Skeleton->InverseBindPoses = Skeleton->InverseBindPoses;
      copy.Skeleton->BoneEntities     = Skeleton->BoneEntities;
      copy.Skeleton->BoneNameToIndex  = Skeleton->BoneNameToIndex;
      copy.Skeleton->BoneNames        = Skeleton->BoneNames;  // Required for bone remapping in UseParentSkeleton
      copy.Skeleton->BoneParents      = Skeleton->BoneParents;
      copy.Skeleton->BindPoseGlobals  = Skeleton->BindPoseGlobals;
      copy.Skeleton->BindPoseLocals   = Skeleton->BindPoseLocals;
      copy.Skeleton->BindPoseLocalTranslations = Skeleton->BindPoseLocalTranslations;
      copy.Skeleton->BindPoseLocalRotations = Skeleton->BindPoseLocalRotations;
      copy.Skeleton->BindPoseLocalScales = Skeleton->BindPoseLocalScales;
      copy.Skeleton->SkeletonGuid     = Skeleton->SkeletonGuid;
      copy.Skeleton->JointGuids       = Skeleton->JointGuids;
      if (Skeleton->Avatar) {
         copy.Skeleton->Avatar = std::make_unique<cm::animation::AvatarDefinition>(*Skeleton->Avatar);
      }
   }
   
   if (Skinning) {
      copy.Skinning = std::make_unique<SkinningComponent>();
      copy.Skinning->SkeletonRoot = Skinning->SkeletonRoot;
      copy.Skinning->UseParentSkeleton = Skinning->UseParentSkeleton;
      copy.Skinning->SkeletonOverrideGuid = Skinning->SkeletonOverrideGuid;
      copy.Skinning->ResolvedSkeletonRoot = INVALID_ENTITY_ID;
      copy.Skinning->ResetGpuSharedSkeletonSource();
      // Copy skeleton retargeting data
      copy.Skinning->OriginalBoneNames = Skinning->OriginalBoneNames;
      copy.Skinning->OriginalInverseBindPoses = Skinning->OriginalInverseBindPoses;
      // BoneRemap, RemapBuiltForSkeleton are runtime-only
      // ResolvedSkeletonRoot is runtime-only, resolved by SkinningSystem
   }
   
   // Deep copy BoneAttachmentComponent
   if (BoneAttachment) {
      copy.BoneAttachment = std::make_unique<BoneAttachmentComponent>();
      copy.BoneAttachment->TargetBoneName = BoneAttachment->TargetBoneName;
      copy.BoneAttachment->LocalPosition = BoneAttachment->LocalPosition;
      copy.BoneAttachment->LocalRotation = BoneAttachment->LocalRotation;
      copy.BoneAttachment->LocalScale = BoneAttachment->LocalScale;
      copy.BoneAttachment->SkeletonEntity = BoneAttachment->SkeletonEntity;
      copy.BoneAttachment->InheritRotation = BoneAttachment->InheritRotation;
      copy.BoneAttachment->InheritScale = BoneAttachment->InheritScale;
      copy.BoneAttachment->Enabled = BoneAttachment->Enabled;
      // Runtime resolution state is not copied - will be resolved on use
   }

   // Deep copy ArmorFitComponent
   if (ArmorFit) {
      copy.ArmorFit = std::make_unique<cm::deformation::ArmorFitComponent>();
      copy.ArmorFit->BodyEntity = ArmorFit->BodyEntity;
      copy.ArmorFit->GlobalWrapWeight = ArmorFit->GlobalWrapWeight;
      copy.ArmorFit->BoneWeightOverrides = ArmorFit->BoneWeightOverrides;
      copy.ArmorFit->WrapData = ArmorFit->WrapData; // Shared wrap data
      copy.ArmorFit->WrapBinPath = ArmorFit->WrapBinPath;
      copy.ArmorFit->UseGPU = ArmorFit->UseGPU;
      // GPU buffers are runtime-only, recreated on demand
   }

   // Deep copy AnimationPlayerComponent to preserve default playback setup
   if (AnimationPlayer) {
      copy.AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>(*AnimationPlayer);
   }

   // Deep copy Navigation components (authoring + runtime refs)
   if (Navigation) {
      copy.Navigation = std::make_unique<nav::NavMeshComponent>();
      copy.Navigation->Enabled = Navigation->Enabled;
      copy.Navigation->Bake = Navigation->Bake;
      copy.Navigation->AABB = Navigation->AABB;
      copy.Navigation->BakeHash = Navigation->BakeHash;
      copy.Navigation->Baking.store(false);
      copy.Navigation->BakingProgress.store(0.0f);
      copy.Navigation->BakingCancel.store(false);
      
      // New persistence fields - CRITICAL for play mode navmesh loading
      copy.Navigation->NavMeshDataGuid = Navigation->NavMeshDataGuid;
      copy.Navigation->AssetPath = Navigation->AssetPath;
      copy.Navigation->AssetDirty = Navigation->AssetDirty;
      
      // Terrain settings
      copy.Navigation->TerrainSampleStep = Navigation->TerrainSampleStep;
      copy.Navigation->GeometryIncludeRegexEnabled = Navigation->GeometryIncludeRegexEnabled;
      copy.Navigation->GeometryIncludeRegexPattern = Navigation->GeometryIncludeRegexPattern;
      copy.Navigation->BakeVisibleChunksOnly = Navigation->BakeVisibleChunksOnly;
      copy.Navigation->BakeVisibleChunkPadding = Navigation->BakeVisibleChunkPadding;
      copy.Navigation->BakeMissingChunksOnly = Navigation->BakeMissingChunksOnly;
      copy.Navigation->ChunkedNavEnabled = Navigation->ChunkedNavEnabled;
      copy.Navigation->ChunkingMode = Navigation->ChunkingMode;
      copy.Navigation->ChunkWorldSize = Navigation->ChunkWorldSize;
      copy.Navigation->ChunkBakePadding = Navigation->ChunkBakePadding;
      copy.Navigation->ChunkStreamRadius = Navigation->ChunkStreamRadius;
      copy.Navigation->NavPackPath = Navigation->NavPackPath;
      copy.Navigation->EnableStitching = Navigation->EnableStitching;
      copy.Navigation->StitchEpsilon = Navigation->StitchEpsilon;
      copy.Navigation->StitchMaxNormalAngleDeg = Navigation->StitchMaxNormalAngleDeg;
      copy.Navigation->StitchMaxHeight = Navigation->StitchMaxHeight;
      copy.Navigation->StitchMaxXZ = Navigation->StitchMaxXZ;
      copy.Navigation->DomainId = Navigation->DomainId;
      copy.Navigation->DomainPriority = Navigation->DomainPriority;
      copy.Navigation->AutoPortalEnabled = Navigation->AutoPortalEnabled;
      copy.Navigation->AutoPortalMaxXZ = Navigation->AutoPortalMaxXZ;
      copy.Navigation->AutoPortalMaxHeight = Navigation->AutoPortalMaxHeight;
      copy.Navigation->LoadAttempted = false;
      copy.Navigation->DebugDrawOffset = Navigation->DebugDrawOffset;
      copy.Navigation->AgentPlacementOffset = Navigation->AgentPlacementOffset;
      copy.Navigation->CostAwareSmoothing = Navigation->CostAwareSmoothing;

      // Share the runtime data (if already loaded)
      copy.Navigation->Runtime = Navigation->Runtime;
   }

   if (NavAgent) {
      copy.NavAgent = std::make_unique<nav::NavAgentComponent>();
      copy.NavAgent->Enabled = NavAgent->Enabled;
      copy.NavAgent->NavMeshEntity = NavAgent->NavMeshEntity;
      copy.NavAgent->Params = NavAgent->Params;
      copy.NavAgent->ArriveThreshold = NavAgent->ArriveThreshold;
      copy.NavAgent->RepathInterval = NavAgent->RepathInterval;
      copy.NavAgent->AutoRepath = NavAgent->AutoRepath;
      copy.NavAgent->AvoidanceRadiusMul = NavAgent->AvoidanceRadiusMul;
      copy.NavAgent->CurrentPath = {};
      copy.NavAgent->PathCursor = 0;
      copy.NavAgent->RepathTimer = 0.0f;
      copy.NavAgent->HasDestination = false;
      copy.NavAgent->PathRequested = false;
      copy.NavAgent->ManagedHandle = 0;
   }

   if (NavLink) {
      copy.NavLink = std::make_unique<nav::NavLinkComponent>(*NavLink);
   }

   if (Portal) {
      copy.Portal = std::make_unique<PortalComponent>(*Portal);
      copy.Portal->ResetRuntime();
   }

   //----------------------------------------------
   // Scripts: clone and rebind context
   //----------------------------------------------
   copy.Scripts.clear();
   for (const auto& script : Scripts) {
      ScriptInstance instance;
      instance.ClassName = script.ClassName;

      // Create a new script instance using the registered factory
      auto created = ScriptSystem::Instance().Create(instance.ClassName);
      if (created) {
         instance.Instance = created;
         // Preserve per-entity reflected property overrides (e.g., entity references)
         // so RuntimeClone can apply them to the managed instance before OnCreate.
         instance.Values = script.Values;
         copy.Scripts.push_back(instance);
         }
      else {
         std::cerr << "[ScriptSystem] Failed to create script of type '"
            << instance.ClassName << "'\n";
         }
      }

   //----------------------------------------------
   // IK components: shallow copy authoring, 
   // reset runtime
   //----------------------------------------------
   copy.IKs = IKs;
   for (auto& k : copy.IKs) {
      k.WasValidLastFrame = false;
      k.Skeleton = nullptr;
      k.ManagedHandle = 0;
   }

   //----------------------------------------------
   // LookAt constraints: shallow copy authoring,
   // reset runtime state
   //----------------------------------------------
   copy.LookAtConstraints = LookAtConstraints;
   for (auto& lac : copy.LookAtConstraints) {
      lac.WasValidLastFrame = false;
      lac.SmoothedYaw = 0.0f;
      lac.SmoothedPitch = 0.0f;
      lac.SmoothedRoll = 0.0f;
      lac.NormalizedWeights.clear();
      lac.ManagedHandle = 0;
   }

   return copy;
} 
