#pragma once
#include <string>
#include <memory>
#include <nlohmann/json.hpp>
#include "core/ecs/Scene.h"
#include "core/ecs/Entity.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/UIComponents.h"
#include "core/ecs/RenderOverridesComponent.h"
#include "core/rendering/MaterialManager.h"
#include "core/rendering/StandardMeshManager.h"

using json = nlohmann::json;

// Forward declarations to avoid heavy includes
namespace cm { namespace animation { struct AnimationPlayerComponent; } }
namespace cm { namespace resourcelayer { struct ResourceLayerComponent; } }
namespace cm { namespace instancer { struct InstancerComponent; } }
 
class Serializer {
public:
    // Scene serialization - THE SINGLE SOURCE OF TRUTH
    // When rootFilter is set, only entities under that root are serialized (used for prefabs)
    static json SerializeScene(Scene& scene, EntityID rootFilter = INVALID_ENTITY_ID);
    static bool DeserializeScene(const json& data, Scene& scene);
    static bool SaveSceneToFile(Scene& scene, const std::string& filepath);
    static bool LoadSceneFromFile(const std::string& filepath, Scene& scene);

    // Legacy prefab serialization (deprecated) — kept temporarily for backward compatibility only
    static json SerializePrefab(const EntityData& entityData, Scene& scene);
    static bool DeserializePrefab(const json& data, EntityData& entityData, Scene& scene);
    static bool SavePrefabToFile(const EntityData& entityData, Scene& scene, const std::string& filepath);
    static bool LoadPrefabFromFile(const std::string& filepath, EntityData& entityData, Scene& scene);

    // DEPRECATED: Use SerializeScene(scene, rootId) instead - same code path for scenes and prefabs
    static json SerializePrefabSubtree(EntityID rootId, Scene& scene);
    static bool SavePrefabSubtreeToFile(Scene& scene, EntityID rootId, const std::string& filepath);
    // Legacy subtree loader removed. Use InstantiatePrefabFromPath for .prefab authoring JSON

    // Individual component serialization
    static json SerializeTransform(const TransformComponent& transform);
    static void DeserializeTransform(const json& data, TransformComponent& transform);
    
    static json SerializeMesh(const MeshComponent& mesh);
    static void DeserializeMesh(const json& data, MeshComponent& mesh);
    static void DeserializeMesh(const json& data, MeshComponent& mesh, std::unique_ptr<RenderOverridesComponent>& renderOverrides);
    static json SerializeMeshProxy(const MeshProxyComponent& proxy, Scene& scene);
    static void DeserializeMeshProxy(const json& data, MeshProxyComponent& proxy, Scene& scene);
    
    static json SerializeLight(const LightComponent& light);
    static void DeserializeLight(const json& data, LightComponent& light);

    static json SerializeAudioSource(const AudioSourceComponent& source);
    static void DeserializeAudioSource(const json& data, AudioSourceComponent& source);
    static json SerializeAudioListener(const AudioListenerComponent& listener);
    static void DeserializeAudioListener(const json& data, AudioListenerComponent& listener);

    // Animation-related components
    static json SerializeSkeleton(const SkeletonComponent& skeleton);
    static void DeserializeSkeleton(const json& data, SkeletonComponent& skeleton);
    static json SerializeSkinning(const SkinningComponent& skinning);
    static void DeserializeSkinning(const json& data, SkinningComponent& skinning);
    
    // Bone attachment (for attaching non-skinned meshes to bones)
    static json SerializeBoneAttachment(const struct BoneAttachmentComponent& ba);
    static void DeserializeBoneAttachment(const json& data, struct BoneAttachmentComponent& ba);

    // Blend shape weights (only weights, geometry comes from mesh file)
    static json SerializeBlendShapeWeights(const struct BlendShapeComponent& blendShapes);
    static void DeserializeBlendShapeWeights(const json& data, struct BlendShapeComponent& blendShapes);

    // Unified morph weights (aggregated blend shapes at model root)
    static json SerializeUnifiedMorphWeights(const struct UnifiedMorphComponent& unifiedMorph);
    static void DeserializeUnifiedMorphWeights(const json& data, struct UnifiedMorphComponent& unifiedMorph);

    // TintMaskController (tint colors for child meshes)
    static json SerializeTintController(const struct TintMaskController& tint);
    static void DeserializeTintController(const json& data, struct TintMaskController& tint);
    
    static json SerializeCollider(const ColliderComponent& collider);
    static void DeserializeCollider(const json& data, ColliderComponent& collider);

    static json SerializeRigidBody(const RigidBodyComponent& rigidbody);
    static void DeserializeRigidBody(const json& data, RigidBodyComponent& rigidbody);

    static json SerializeStaticBody(const StaticBodyComponent& staticbody);
    static void DeserializeStaticBody(const json& data, StaticBodyComponent& staticbody);

    static json SerializeSoftbody(const SoftbodyComponent& softbody);
    static void DeserializeSoftbody(const json& data, SoftbodyComponent& softbody);

     // Camera
     static json SerializeCamera(const CameraComponent& camera);
     static void DeserializeCamera(const json& data, CameraComponent& camera);

     // Terrain
     static json SerializeTerrain(const TerrainComponent& terrain);
     static void DeserializeTerrain(const json& data, TerrainComponent& terrain);

     // River
     static json SerializeRiver(const RiverComponent& river);
     static void DeserializeRiver(const json& data, RiverComponent& river);

     // Spline
     static json SerializeSpline(const SplineComponent& spline);
     static void DeserializeSpline(const json& data, SplineComponent& spline);

     // Resource Layers (procedural resource distribution)
     static json SerializeResourceLayers(const cm::resourcelayer::ResourceLayerComponent& layers);
     static void DeserializeResourceLayers(const json& data, cm::resourcelayer::ResourceLayerComponent& layers);
     
     // Instancer (optimized instanced rendering with prefab hot-swap)
     static json SerializeInstancer(const cm::instancer::InstancerComponent& instancer);
     static void DeserializeInstancer(const json& data, cm::instancer::InstancerComponent& instancer);

     // Particle Emitter
     static json SerializeParticleEmitter(const ParticleEmitterComponent& emitter);
     static void DeserializeParticleEmitter(const json& data, ParticleEmitterComponent& emitter);

     // Area (physics area/trigger)
     static json SerializeArea(const cm::physics::AreaComponent& area);
     static void DeserializeArea(const json& data, cm::physics::AreaComponent& area);

     // UI components
     static json SerializeCanvas(const CanvasComponent& canvas);
     static void DeserializeCanvas(const json& data, CanvasComponent& canvas);
     static json SerializePanel(const PanelComponent& panel);
     static void DeserializePanel(const json& data, PanelComponent& panel);
     static json SerializeButton(const ButtonComponent& button);
     static void DeserializeButton(const json& data, ButtonComponent& button);
     static json SerializeSlider(const SliderComponent& slider);
     static void DeserializeSlider(const json& data, SliderComponent& slider);
     static json SerializeProgressBar(const ProgressBarComponent& bar);
     static void DeserializeProgressBar(const json& data, ProgressBarComponent& bar);
     static json SerializeToggle(const ToggleComponent& toggle);
     static void DeserializeToggle(const json& data, ToggleComponent& toggle);
     static json SerializeScrollView(const ScrollViewComponent& scrollView);
     static void DeserializeScrollView(const json& data, ScrollViewComponent& scrollView);
     static json SerializeLayoutGroup(const LayoutGroupComponent& layout);
     static void DeserializeLayoutGroup(const json& data, LayoutGroupComponent& layout);
     static json SerializeInputField(const InputFieldComponent& input);
     static void DeserializeInputField(const json& data, InputFieldComponent& input);
     static json SerializeDropdown(const DropdownComponent& dropdown);
     static void DeserializeDropdown(const json& data, DropdownComponent& dropdown);
     static json SerializeUIRect(const UIRectComponent& rect);
     static void DeserializeUIRect(const json& data, UIRectComponent& rect);
     static json SerializeFitToContent(const FitToContentComponent& ftc);
     static void DeserializeFitToContent(const json& data, FitToContentComponent& ftc);
    static json SerializeUISceneCapture(const UISceneCaptureComponent& capture);
    static void DeserializeUISceneCapture(const json& data, UISceneCaptureComponent& capture);
     // Render overrides
     static json SerializeRenderOverrides(const RenderOverridesComponent& ro);
     static void DeserializeRenderOverrides(const json& data, RenderOverridesComponent& ro);

     // TextRenderer
     static json SerializeText(const TextRendererComponent& text);
     static void DeserializeText(const json& data, TextRendererComponent& text);

      // Animator / AnimationPlayerComponent
      static json SerializeAnimator(const cm::animation::AnimationPlayerComponent& animator);
      static void DeserializeAnimator(const json& data, cm::animation::AnimationPlayerComponent& animator);

    // Script serialization (scene-aware so Entity refs persist via GUID)
    static json SerializeScripts(Scene& scene, const std::vector<ScriptInstance>& scripts);
    static void DeserializeScripts(const json& data, std::vector<ScriptInstance>& scripts, Scene& scene, bool createInstances = true);

    // Entity serialization
    static json SerializeEntity(EntityID id, Scene& scene);
    static EntityID DeserializeEntity(const json& data, Scene& scene);
    // Apply component data from JSON to an existing entity (for prefab refresh/revert)
    static void DeserializeEntityData(const json& data, EntityData* entityData, Scene& scene, bool createScriptInstances = true);
    // Dynamic module components (values only; schemas come from ComponentRegistry)
    static json SerializeDynamic(const std::unordered_map<cm::TypeId, cm::ModuleComponent, cm::TypeIdHasher>& dyn);
    static void DeserializeDynamic(const json& arr, std::unordered_map<cm::TypeId, cm::ModuleComponent, cm::TypeIdHasher>& dyn);

    // Navigation components
    static json SerializeNavMesh(const nav::NavMeshComponent& navmesh);
    static void DeserializeNavMesh(const json& data, nav::NavMeshComponent& navmesh);
    static json SerializeNavAgent(const nav::NavAgentComponent& agent);
    static void DeserializeNavAgent(const json& data, nav::NavAgentComponent& agent);
    static json SerializeNavLink(const nav::NavLinkComponent& link);
    static void DeserializeNavLink(const json& data, nav::NavLinkComponent& link);

    // Portal components
    static json SerializePortal(const PortalComponent& portal);
    static void DeserializePortal(const json& data, PortalComponent& portal);

    // Prefab instance component (unified prefab system)
    static json SerializePrefabInstance(const PrefabInstanceComponent& instance);
    static void DeserializePrefabInstance(const json& data, PrefabInstanceComponent& instance);

    // Resource cleanup
    static void ReleasePBTextures(Scene& scene);

    static void DeserializeCharacterController(const nlohmann::json& j, CharacterControllerComponent& cc);
    static void DeserializeGrassDeformer(const nlohmann::json& j, GrassDeformerComponent& deformer);

    static EntityID LoadPrefabToScene(const std::string& filepath, Scene& scene);
    // Helper functions
    static json SerializeVec3(const glm::vec3& vec);
    static glm::vec3 DeserializeVec3(const json& data);

    static json SerializeMat4(const glm::mat4& mat);
    static glm::mat4 DeserializeMat4(const json& data);

};
