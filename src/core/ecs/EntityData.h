#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <nlohmann/json.hpp>

// Core includes (within core library)
#include "Entity.h"
#include "Components.h"
#include "UIComponents.h"
#include "ModuleComponent.h"
#include "RenderOverridesComponent.h"
#include "core/animation/AnimationPlayerComponent.h"
#include "core/animation/ik/IKComponent.h"
#include "core/animation/lookat/LookAtConstraintComponent.h"
#include "core/navigation/NavMesh.h"
#include "core/navigation/NavAgent.h"
#include "core/physics/area/AreaComponent.h"
#include "core/assets/AssetReference.h"
#include "core/deformation/ArmorFitComponent.h"
#include "core/ecs/NpcScalability.h"
#include "core/resourcelayer/ResourceLayerTypes.h"
#include "core/prefab/PrefabInstanceComponent.h"
#include "InstancerComponent.h"

// Managed interop includes
#include "managed/interop/ScriptSystem.h"
#include "managed/interop/ScriptComponent.h"

// Forward declaration
class Scene;

//----------------------------------------------------------------------
// Entity Data
//----------------------------------------------------------------------
struct EntityData {

   EntityData() = default;
   EntityData(const EntityData&) = delete;
   EntityData& operator=(const EntityData&) = delete;
   EntityData(EntityData&&) = default;
   EntityData& operator=(EntityData&&) = default;
   std::string Name = "Entity";

   //----------------------------------------------------------------------
   // Basic properties and Identifiers
   //----------------------------------------------------------------------
   int Layer = 0;

   std::string Tag;

   std::vector<std::string> Groups; // Arbitrary groups for filtering/searching

   // Global visibility toggle for the whole entity (affects rendering and lights)
   bool Visible = true;

   // Runtime-only presentation suppression used for hide-until-ready flows.
   // This must not overwrite authored Visible state.
   bool PresentationHidden = false;

   // Active toggle: when false, scripts should not run and entity should not render
   bool Active = true;

   // Runtime-only async prefab readiness tracking for managed/native awaiters.
   bool PrefabAsyncPending = false;
   bool PrefabAsyncFailed = false;

   // Runtime-only persistence for play mode scene loads
   bool PersistAcrossScenes = false;

   EntityID Parent = INVALID_ENTITY_ID;

   std::vector<EntityID> Children;

   // Stable identity for forward/backward compatibility across saves
   ClaymoreGUID EntityGuid = ClaymoreGUID::Generate();


   //----------------------------------------------------------------------
   // Components
   //----------------------------------------------------------------------
   TransformComponent Transform;
   
   std::unique_ptr<MeshComponent> Mesh;
   std::unique_ptr<MeshProxyComponent> MeshProxy;

   std::unique_ptr<LightComponent> Light;
   
   std::unique_ptr<BlendShapeComponent> BlendShapes;
   // Pending blend shape weights to apply when BlendShapes is populated (restored from scene/prefab)
   std::unordered_map<std::string, float> PendingBlendShapeWeights;
   
   std::unique_ptr<UnifiedMorphComponent> UnifiedMorph;
   // Pending unified morph weights to apply when UnifiedMorph is populated (restored from scene/prefab)
   std::unordered_map<std::string, float> PendingUnifiedMorphWeights;
   
   std::unique_ptr<TintMaskController> TintController;
   
   std::unique_ptr<SkeletonComponent> Skeleton;
   
   std::unique_ptr<SkinningComponent> Skinning;
   
   // Bone attachment (for attaching non-skinned meshes to bones without parenting)
   std::unique_ptr<BoneAttachmentComponent> BoneAttachment;
   
   // Armor wrap deformation (for armor meshes that conform to body morphs)
   std::unique_ptr<cm::deformation::ArmorFitComponent> ArmorFit;
   
   std::unique_ptr<ColliderComponent> Collider;
   
   std::unique_ptr<CameraComponent> Camera; 
   
   std::unique_ptr<RigidBodyComponent> RigidBody;
   
   std::unique_ptr<StaticBodyComponent> StaticBody;

   std::unique_ptr<SoftbodyComponent> Softbody;
   
   std::unique_ptr<CharacterControllerComponent> CharacterController;
   
   std::unique_ptr<GrassDeformerComponent> GrassDeformer;
   
   std::unique_ptr<TerrainComponent> Terrain;
   
   std::unique_ptr<RiverComponent> River;
   
   // Spline path (for scripting, instancer distribution, etc.)
   std::unique_ptr<SplineComponent> Spline;
   
   // Resource Layer system (procedural resource distribution with eligibility maps)
   std::unique_ptr<cm::resourcelayer::ResourceLayerComponent> ResourceLayers;
   
   // Instancer system (optimized instanced rendering with prefab hot-swap)
   std::unique_ptr<cm::instancer::InstancerComponent> Instancer;
   
   std::unique_ptr<ParticleEmitterComponent> Emitter;

   // Areas (Area3D)
   std::unique_ptr<cm::physics::AreaComponent> Area;

   // Navigation
   std::unique_ptr<nav::NavMeshComponent> Navigation;
   
   // Nav Agent
   std::unique_ptr<nav::NavAgentComponent> NavAgent; 

   // Nav Link (off-mesh navigation link)
   std::unique_ptr<nav::NavLinkComponent> NavLink;

   // Portals (cross-scene traversal)
   std::unique_ptr<PortalComponent> Portal;

   // Text rendering
   std::unique_ptr<TextRendererComponent> Text;

   // UI Components
   std::unique_ptr<CanvasComponent> Canvas;
   
   // UI Panel
   std::unique_ptr<PanelComponent> Panel;
   
   // UI Button
   std::unique_ptr<ButtonComponent> Button;
   
   // UI Slider
   std::unique_ptr<SliderComponent> Slider;
   
   // UI Progress Bar
   std::unique_ptr<ProgressBarComponent> ProgressBar;
   
   // UI Toggle (checkbox)
   std::unique_ptr<ToggleComponent> Toggle;
   
   // UI Scroll View
   std::unique_ptr<ScrollViewComponent> ScrollView;
   
   // UI Layout Group
   std::unique_ptr<LayoutGroupComponent> LayoutGroup;
   
   // UI Input Field
   std::unique_ptr<InputFieldComponent> InputField;
   
   // UI Dropdown
   std::unique_ptr<DropdownComponent> Dropdown;
   
   // UI Rect (parent-relative positioning)
   std::unique_ptr<UIRectComponent> UIRect;
   
   // Fit To Content (auto-size panel to children)
   std::unique_ptr<FitToContentComponent> FitToContent;
   
   // UI Scene Capture (render scene view into a panel)
   std::unique_ptr<UISceneCaptureComponent> UISceneCapture;

   // Audio
   std::unique_ptr<AudioSourceComponent> AudioSource;
   std::unique_ptr<AudioListenerComponent> AudioListener;

   // Animation Player
   std::unique_ptr<cm::animation::AnimationPlayerComponent> AnimationPlayer;
   
   //  per-entity render overrides 
   // (alpha, depth, show on top, shader override)
   std::unique_ptr<RenderOverridesComponent> RenderOverrides;

   // IK components
   std::vector<cm::animation::ik::IKComponent> IKs;
   
   // LookAt/Aim constraints (pre-IK rotation layer)
   std::vector<cm::animation::lookat::LookAtConstraintComponent> LookAtConstraints;

   // Attached scripts (by class name)
   std::vector<ScriptInstance> Scripts;
   
   // =========================================================================
   // PERF: Script LOD - reduce update frequency for distant entities
   // =========================================================================
   float ScriptLodAccumulatedTime = 0.0f;  // Accumulated time since last script update
   float ScriptLodLastDistance = 0.0f;      // Distance from camera (cached for LOD)
   bool  ScriptLodEnabled = false;          // Whether this entity uses script LOD
   bool  ScriptLodForceDisabled = false;    // Runtime override to keep scripts full-rate
   cm::npc::ScalabilityState NpcScalability; // Runtime-only shared NPC/crowd significance state

   //----------------------------------------------------------------------
   // Prefab Instance (new unified system)
   //----------------------------------------------------------------------
   // If this entity is the root of a prefab instance, this component tracks
   // the source prefab, owned entities, and per-instance overrides.
   std::unique_ptr<PrefabInstanceComponent> PrefabInstance;
   
   //----------------------------------------------------------------------
   // Legacy prefab fields (kept for migration compatibility)
   // These are deprecated - use PrefabInstance instead
   //----------------------------------------------------------------------
   // Prefab linkage: GUID of the prefab this entity instance originates from (zero if not from a prefab)
   ClaymoreGUID PrefabGuid = {};
   
   // Record prefab source vpath used to instantiate this entity (advisory)
   std::string PrefabSource;
   
   // Model asset identity: GUID of the model asset this entity was instantiated from.
   // Set by Scene::InstantiateModel. Used by Editor for prefab serialization.
   // Core only stores the GUID; path resolution happens in Editor layer.
   ClaymoreGUID ModelAssetGuid = {};
   
   // Model node deletions: paths of model child nodes intentionally deleted by user.
   // These paths are relative to the model root (e.g., "Armature/Hips/LeftLeg").
   // On scene load or hot reload, nodes in this list are NOT recreated from the model file.
   // This enables stable user deletions that persist across reimports.
   std::vector<std::string> DeletedModelNodes;
   
   // Preserve unknown JSON so forward-compat fields survive round-trips
   nlohmann::json Extra; // keys not recognized by current serializer

   // Dynamic, module-defined components (sidecar storage by TypeId)
   std::unordered_map<cm::TypeId, cm::ModuleComponent, cm::TypeIdHasher> Dynamic;

   
   //----------------------------------------------------------------------
   // Deep Copy: copy this entity data, assigning a new ID and 
   // optionally a new scene
   //----------------------------------------------------------------------
   EntityData DeepCopy(EntityID ID, Scene* newScene) const;
};
