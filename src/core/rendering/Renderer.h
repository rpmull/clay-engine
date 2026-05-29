#pragma once
#include <bgfx/bgfx.h>
#include <glm/glm.hpp>
#include <memory>
#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>

// Core includes
#include "Camera.h"
#include "Mesh.h"
#include "Material.h"
#include "TextRenderer.h"
#include "RenderContext.h"
#include "Instancing.h"
#include "core/ecs/Scene.h"
#include "core/ecs/Components.h"
#include "core/world/RuntimeWorld.h"
#include "core/physics/area/AreaComponent.h"

// Forward declarations for editor-only types
class DebugMaterial;
struct SkeletonComponent;
struct SkinningComponent;
struct BlendShapeComponent;

// Terrain chunk system (includes TerrainChunkSystem, ChunkLODBatch, ChunkSystemConfig)
#include "TerrainChunks.h"

// ImGui forward declarations (editor-only, guarded in implementation)
#ifdef CLAYMORE_EDITOR
#include "imgui.h"
#endif

struct LightData {
    LightType type;
    glm::vec3 color;
    glm::vec3 position;
    glm::vec3 direction;
    float range;           // For point lights
    float constant;        // Attenuation constant
    float linear;          // Attenuation linear
    float quadratic;       // Attenuation quadratic
};

class Renderer {
public:
    static constexpr int kMaxShaderLights = 5;      // 1 directional + up to 4 nearby point lights
    static constexpr int kMaxPointShadowLights = 4; // Budgeted point-light shadow casters
    static constexpr uint32_t kPresentationResetFlags = BGFX_RESET_NONE;

    struct RuntimeStatsFrame {
        uint64_t RenderedMeshObjects = 0;
        uint64_t CulledMeshObjects = 0;
        uint64_t RenderedSkinnedMeshObjects = 0;
        uint64_t CulledSkinnedMeshObjects = 0;
    };

    // Initializes bgfx and renderer resources for a given window size/handle.
    // Safe to call once; call Shutdown() on teardown.
    static Renderer& Get();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    // Initialization & lifecycle
    void Init(uint32_t width, uint32_t height, void* windowHandle);
    void Shutdown();
    void BeginFrame(float r, float g, float b);
    void EndFrame();
    uint32_t GetLastSubmittedFrame() const { return m_LastSubmittedFrame; }
    bool SupportsGpuSkinningAtlas() const { return m_GpuSkinningAtlasSupported; }
    bool CanUseGpuMorphTargets(const Mesh* mesh, const BlendShapeComponent* blendShapes) const;
    bool CanRenderGpuMorphTargets(const SkinningComponent* skinning, const Mesh* mesh, const BlendShapeComponent* blendShapes);
    bool ShouldUseGpuMorphTargets(const SkinningComponent* skinning, const Mesh* mesh, const BlendShapeComponent* blendShapes) const;
    bool CanUseGpuMaterializedSkinning(const SkinningComponent* skinning, const Mesh* mesh, const BlendShapeComponent* blendShapes) const;
    void Resize(uint32_t width, uint32_t height);
    void SetRenderToOffscreen(bool enable) { m_RenderToOffscreen = enable; }
    
    // Text rendering defaults
    void SetDefaultTextFont(const std::string& fontPath, float basePixelSize = 48.0f);

    // Scene rendering
    // Renders the active scene into the default or offscreen framebuffer.
    void RenderScene(Scene& scene);
    void RenderScene(Scene& scene, uint16_t viewId);
    
    // Renders a scene using the provided RenderContext.
    // This allows multiple independent viewports to render without interference.
    // The context contains all per-viewport state: scene, camera, view/proj matrices, etc.
    void RenderScene(const RenderContext& ctx);

    // Mesh submission
    void DrawMesh(const Mesh& mesh, const float* transform, const Material& material, const struct MaterialPropertyBlock* propertyBlock = nullptr);
    void DrawMesh(const Mesh& mesh, const float* transform, const Material& material, uint16_t viewId, const struct MaterialPropertyBlock* propertyBlock = nullptr);

    // Camera
    Camera* GetCamera() const {
        // Only fall back to a scene camera while the runtime scene is playing.
        if (Scene::CurrentScene && Scene::CurrentScene->m_IsPlaying) {
            if (Camera* cam = Scene::CurrentScene->GetActiveCamera()) {
                return cam;
            }
        }
        return m_RendererCamera;
    }
    // Get the editor camera directly, bypassing scene camera logic
    // Use this for editor tools (like ImGuizmo) that should always use the editor camera
    Camera* GetEditorCamera() const { return m_RendererCamera; }
    void SetCamera(Camera* cam) { m_RendererCamera = cam; }

    // Viewport info
    int GetWidth() const { return m_Width; }
    int GetHeight() const { return m_Height; }
    bgfx::TextureHandle GetSceneTexture() const { return m_SceneTexture; }
    // Render any scene into a temporary texture using a dedicated offscreen view.
    // The provided camera is used for this render only and does not affect the main viewport.
    bgfx::TextureHandle RenderSceneToTexture(Scene* scene, uint32_t width, uint32_t height, class Camera* camera,
        uint16_t viewIdBase = 220, bool showGrid = true, uint32_t clearColor = 0x202020ff, bool renderUIOverlay = true,
        const std::unordered_set<EntityID>* allowedEntities = nullptr, bool forceFogDisabled = false);
    bgfx::TextureHandle EnsureOffscreenTexture(uint16_t viewIdBase, uint32_t width, uint32_t height,
        bgfx::FrameBufferHandle* outFramebuffer = nullptr);
    // Release an offscreen target created by RenderSceneToTexture for a given viewId base
    void ReleaseOffscreenTarget(uint16_t viewIdBase);
    // Release all offscreen targets (call before bgfx shutdown)
    void ReleaseAllOffscreenTargets();

    // Debug utilities
	void InitGrid(float size, float step);
    void DrawGrid();
    void DrawGrid(uint16_t viewId);
    void DrawDebugRay(const glm::vec3& origin, const glm::vec3& dir, float length = 10.0f);
    void DrawDebugLineColored(const glm::vec3& from, const glm::vec3& to, uint32_t abgrColor);
    void DrawPhysicsDebugLines();  // Draw all queued physics debug visualization
    void DrawCollider(const ColliderComponent& collider, const TransformComponent& transform, const Mesh* mesh, const RigidBodyComponent* rigidBody, const StaticBodyComponent* staticBody);
    void DrawAreaCollider(const cm::physics::AreaComponent& area, const TransformComponent& transform, uint32_t abgrColor);
    void DrawCameraFrustum(const CameraComponent& camera, const TransformComponent& transform);
    void DrawRing(const glm::vec3& center, const glm::vec3& normal, float radius, uint32_t abgrColor);
    void DrawFilledCircle(const glm::vec3& center, const glm::vec3& normal, float radius, uint32_t abgrColor);
    void DrawCharacterController(const CharacterControllerComponent& cc, const TransformComponent& transform);
    // Hook to allow external debug overlays like navigation to draw after scene
    void AddOverlayCallback(void(*fn)(uint16_t));
    void RemoveOverlayCallback(void(*fn)(uint16_t));

    void UploadLightsToShader(const std::vector<LightData>& lights);
    void UploadEnvironmentToShader(const Environment& env, bool forceFogDisabled = false);

    // Shadows (directional)
    void InitShadowResources(uint32_t resolution);
    void ShutdownShadowResources();
    void RenderShadowMap(class Scene& scene, const Camera* camera);
    void BindShadowUniforms();
    
    // Re-bind the cached lighting uniforms (lights + environment) for external systems
    // Call this before each draw call that uses PBR shaders outside the main render loop
    void BindLightingUniforms();

    glm::vec3 ComputePrimarySunDirection(Scene& scene) const;
    void SetEditorLightingOverride(bool disabled);
    bool IsEditorLightingOverrideEnabled() const { return m_EditorLightingOverride; }

    std::vector<glm::mat4> ComputeFinalBoneMatrices(Entity entity, Scene& scene);

    // Editor helpers
    void DrawEntityOutline(Scene& scene, EntityID selectedEntity);
    void DrawEntityOutline(Scene& scene, EntityID entityId, const glm::vec4& color, float thickness);
    void DrawSceneOutline(Scene& scene);
    bool SupportsObjectIdPicking() const;
    bool RequestObjectIdPick(Scene& scene, float nx, float ny);
    bool ConsumeObjectIdPickResult(EntityID& outId, bool& outHadHit);
    bool IsObjectIdPickPending() const { return m_ObjectIdPickPending; }

    // Debug draw toggles
    void SetShowGrid(bool v) { m_ShowGrid = v; }
    bool GetShowGrid() const { return m_ShowGrid; }
    void SetShowColliders(bool v) { m_ShowColliders = v; }
    bool GetShowColliders() const { return m_ShowColliders; }
    void SetShowAABBs(bool v) { m_ShowAABBs = v; }
    bool GetShowAABBs() const { return m_ShowAABBs; }
    void SetShowCameraFrustums(bool v) { m_ShowCameraFrustums = v; }
    bool GetShowCameraFrustums() const { return m_ShowCameraFrustums; }
    void SetShowShadowDebugOverlay(bool v) { m_ShowShadowDebugOverlay = v; }
    bool GetShowShadowDebugOverlay() const { return m_ShowShadowDebugOverlay; }
    // Toggle CPU view-frustum culling for mesh submissions
    void SetFrustumCullingEnabled(bool enabled) { m_EnableFrustumCulling = enabled; }
    bool GetFrustumCullingEnabled() const { return m_EnableFrustumCulling; }
    // Allow debug rendering (grid/colliders/AABBs/etc.) while in play mode
    void SetDebugDrawInPlayMode(bool enabled);
    bool GetDebugDrawInPlayMode() const;
    void SetRuntimeStatsCaptureEnabled(bool enabled) {
        m_RuntimeStatsCaptureEnabled = enabled;
        if (!enabled) {
            m_LastRuntimeStatsFrame = {};
        }
    }
    bool GetRuntimeStatsCaptureEnabled() const { return m_RuntimeStatsCaptureEnabled; }
    const RuntimeStatsFrame& GetLastRuntimeStatsFrame() const { return m_LastRuntimeStatsFrame; }

    // Terrain texture arrays
    // Rebuild texture arrays from TerrainComponent layers (call when LayerTextureArraysDirty)
    void BuildTerrainTextureArrays(struct TerrainComponent& terrain);
    void DestroyTerrainTextureArrays();

private:
    struct SkinningBindCacheState {
        const SkinningComponent* skinning = nullptr;
        uint32_t atlasUploadSerial = 0;
        bool atlasGlobalsBound = false;
    };

    Renderer() = default;
    ~Renderer();

    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    Camera* m_RendererCamera = nullptr;
    uint32_t m_LastSubmittedFrame = 0;

    bgfx::FrameBufferHandle m_SceneFrameBuffer = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_SceneTexture = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_SceneDepthTexture = BGFX_INVALID_HANDLE;
    bgfx::TextureFormat::Enum m_SceneDepthFormat = bgfx::TextureFormat::D24S8;

    float m_view[16]{};
    float m_proj[16]{};

    // Multi-light uniforms (support up to 4 lights)
    bgfx::UniformHandle u_LightColors = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_LightPositions = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_LightParams = BGFX_INVALID_HANDLE;
	    bgfx::UniformHandle u_cameraPos = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_AmbientFog   = BGFX_INVALID_HANDLE; // xyz=color/intensity, w=flags
    bgfx::UniformHandle u_FogParams    = BGFX_INVALID_HANDLE; // x=fogDensity, y=unused
    // Sky uniforms
    bgfx::UniformHandle u_SkyParams    = BGFX_INVALID_HANDLE;  // x=eval procedural, y=sample cubemap, z=apply gamma
    bgfx::UniformHandle u_SkyTopColor  = BGFX_INVALID_HANDLE;  // Zenith color (rgb)
    bgfx::UniformHandle u_SkyHorizonColor = BGFX_INVALID_HANDLE; // Horizon color (rgb)
    bgfx::UniformHandle u_GroundColor  = BGFX_INVALID_HANDLE;  // Ground color (rgb)
    bgfx::UniformHandle u_SunDirection = BGFX_INVALID_HANDLE;  // Sun direction (xyz, normalized)
    bgfx::UniformHandle u_SkySunParams = BGFX_INVALID_HANDLE;  // x=SunSize, y=SunSizeConvergence, z=SunIntensity, w=unused
    bgfx::UniformHandle u_SkyAtmosphereParams = BGFX_INVALID_HANDLE;  // x=AtmosphereThickness, y=HorizonFade, z=SkyExposure, w=unused
    bgfx::UniformHandle u_SceneColorGrade = BGFX_INVALID_HANDLE; // x=exposure, y=contrast, z=saturation, w=tonemapEnabled
    bgfx::UniformHandle s_Skybox       = BGFX_INVALID_HANDLE;
    // Note: u_invViewProj is a predefined bgfx uniform (available in shaders automatically)
    bgfx::UniformHandle u_normalMat    = BGFX_INVALID_HANDLE; // CPU-provided normal matrix
     

    bgfx::ProgramHandle m_DebugLineProgram = BGFX_INVALID_HANDLE;
    std::shared_ptr<DebugMaterial> m_CachedDebugMaterial = nullptr;
    bgfx::UniformHandle u_DebugColor = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_OutlineProgram = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_outlineColor = BGFX_INVALID_HANDLE;

    // Outline/mask resources
    bgfx::TextureHandle m_VisMaskTex = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_OccMaskTex = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle m_VisMaskFB = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle m_OccMaskFB = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_TexelSize = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_OutlineColor = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_OutlineParams = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_SelectMaskProgram = BGFX_INVALID_HANDLE; // static mask (vs_pbr)
    bgfx::ProgramHandle m_SelectMaskProgramSkinned = BGFX_INVALID_HANDLE; // skinned mask (vs_pbr_skinned)
    bgfx::ProgramHandle m_PBRSkinnedInstancedProgram = BGFX_INVALID_HANDLE; // vs_pbr_skinned_instanced + fs_pbr_skinned
    bgfx::ProgramHandle m_PBRSkinnedMorphProgram = BGFX_INVALID_HANDLE; // vs_pbr_skinned_morph + fs_pbr_skinned
    bgfx::ProgramHandle m_PBRSkinnedMorphInstancedProgram = BGFX_INVALID_HANDLE; // vs_pbr_skinned_morph_instanced + fs_pbr_skinned
    bgfx::ProgramHandle m_OutlineCompositeProgram = BGFX_INVALID_HANDLE; // fullscreen (legacy masks)
    bgfx::UniformHandle s_MaskVis = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_MaskOcc = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_TintProgram = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_TintColor = BGFX_INVALID_HANDLE;

    // New screen-space outline pipeline: ObjectID -> Edge -> Composite
    bgfx::TextureHandle m_ObjectIdTex = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle m_ObjectIdFB = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_ObjectIdReadbackTex = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_EdgeMaskTex = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle m_EdgeMaskFB = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_ObjectIdProgram = BGFX_INVALID_HANDLE;            // vs_pbr + fs_object_id
    bgfx::ProgramHandle m_ObjectIdProgramSkinned = BGFX_INVALID_HANDLE;     // vs_pbr_skinned + fs_object_id
    bgfx::ProgramHandle m_ObjectIdProgramSkinnedInstanced = BGFX_INVALID_HANDLE; // vs_pbr_skinned_object_id_instanced + fs_object_id_instanced
    bgfx::ProgramHandle m_ObjectIdProgramSkinnedMorph = BGFX_INVALID_HANDLE; // vs_pbr_skinned_morph + fs_object_id
    bgfx::ProgramHandle m_ObjectIdProgramSkinnedMorphInstanced = BGFX_INVALID_HANDLE; // vs_pbr_skinned_morph_object_id_instanced + fs_object_id_instanced
    bgfx::ProgramHandle m_OutlineEdgeProgram = BGFX_INVALID_HANDLE;         // vs_fullscreen + fs_outline_edge
    bgfx::ProgramHandle m_OutlineCompositeProgram2 = BGFX_INVALID_HANDLE;   // vs_fullscreen + fs_outline_composite
    bgfx::UniformHandle u_ObjectIdPacked = BGFX_INVALID_HANDLE;             // per-draw ID (packed into rgb)
    bgfx::UniformHandle u_SelectedIdPacked = BGFX_INVALID_HANDLE;           // selected entity id (packed)
    bgfx::UniformHandle s_ObjectId = BGFX_INVALID_HANDLE;                   // sampler for ObjectIdTex
    bgfx::UniformHandle s_EdgeMask = BGFX_INVALID_HANDLE;                   // sampler for EdgeMaskTex
    bgfx::UniformHandle s_SceneColor = BGFX_INVALID_HANDLE;                 // sampler for scene color in composite
    bool m_ObjectIdPickPending = false;
    uint32_t m_ObjectIdPickPendingFrame = 0;
    std::array<uint8_t, 4> m_ObjectIdPickReadbackBuffer{};
    bool m_ObjectIdPickResultReady = false;
    bool m_ObjectIdPickResultHadHit = false;
    EntityID m_ObjectIdPickResult = INVALID_ENTITY_ID;
 

    // Terrain rendering resources
    bgfx::ProgramHandle m_TerrainProgram = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_TerrainDepthProgram = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_TerrainHeightTexture = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_TerrainSplatTexture = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_TerrainSplatTexture2 = BGFX_INVALID_HANDLE;  // Second splatmap for layers 4-7
    bgfx::UniformHandle s_TerrainHoleTexture = BGFX_INVALID_HANDLE;
    // Texture array samplers (all layers combined into arrays)
    bgfx::UniformHandle s_TerrainAlbedoArray = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_TerrainNormalArray = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_TerrainChunkParams = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_TerrainHeightParams = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_TerrainTexelSize = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_TerrainLayerTiling = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_TerrainLayerColor = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_TerrainMaterial = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_TerrainLayerCount = BGFX_INVALID_HANDLE;  // Number of active layers
    bgfx::TextureHandle m_TerrainFallbackAlbedo = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_TerrainFallbackNormal = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_TerrainFallbackHole = BGFX_INVALID_HANDLE;
    // Per-terrain texture arrays (rebuilt when layers/resolution change)
    bgfx::TextureHandle m_TerrainAlbedoArrayTex = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_TerrainNormalArrayTex = BGFX_INVALID_HANDLE;
    uint32_t m_TerrainArrayResolution = 0;  // Current resolution of texture arrays
    uint32_t m_TerrainArrayLayerCount = 0;  // Number of layers in arrays
    
    // Clipmap terrain rendering resources
    bgfx::ProgramHandle m_ClipmapProgram = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_ClipmapDepthProgram = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_ClipmapParams = BGFX_INVALID_HANDLE;   // (scale, morph, gridSize, level)
    bgfx::UniformHandle u_ClipmapOffset = BGFX_INVALID_HANDLE;   // (offsetX, offsetZ, terrainSizeX, terrainSizeZ)
    bgfx::UniformHandle u_TerrainOrigin = BGFX_INVALID_HANDLE;   // (originX, originY, originZ, unused) - terrain world position
    
    // Chunked terrain rendering resources (Skyrim-style cells)
    bgfx::ProgramHandle m_ChunkedTerrainProgram = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_ChunkedTerrainDepthProgram = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_ChunkedTerrainInstancedProgram = BGFX_INVALID_HANDLE;      // Instanced version (batched)
    bgfx::ProgramHandle m_ChunkedTerrainDepthInstancedProgram = BGFX_INVALID_HANDLE; // Instanced depth
    bgfx::UniformHandle u_ChunkParams = BGFX_INVALID_HANDLE;     // xy=UV offset, zw=UV scale
    bgfx::UniformHandle u_ChunkWorld = BGFX_INVALID_HANDLE;      // xy=world offset, zw=world extent
    bgfx::UniformHandle u_MorphParams = BGFX_INVALID_HANDLE;     // x=morph factor, y=LOD, z=grid size
    bgfx::UniformHandle u_NeighborLODs = BGFX_INVALID_HANDLE;    // x=N, y=E, z=S, w=W neighbor LODs
    bgfx::UniformHandle u_TerrainSize = BGFX_INVALID_HANDLE;     // xy=terrain world size
    std::unique_ptr<terrain::TerrainChunkSystem> m_ChunkSystem;  // Chunk management system
    std::array<terrain::ChunkLODBatch, terrain::ChunkSystemConfig::kMaxLODLevels> m_ChunkLODBatches; // Reusable per-frame

    bgfx::ProgramHandle m_SkyProgram = BGFX_INVALID_HANDLE;

    bgfx::VertexBufferHandle m_GridVB = BGFX_INVALID_HANDLE;
    uint32_t m_GridVertexCount = 0;

    // Text rendering state
    std::unique_ptr<TextRenderer> m_TextRenderer;

    // UI rendering
    bgfx::UniformHandle m_UISampler = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_UIProgram = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_UIWhiteTex = BGFX_INVALID_HANDLE;
    bool m_ShowUIOverlay = true;
    bool m_UIInputConsumed = false;
    bool m_ShowUIRects = false;
    EntityID m_UIDragSource = INVALID_ENTITY_ID;
    bool m_UIDragActive = false;
    bool m_UIPrevMouseDown = false;
    // Viewport-reported mouse position in scene framebuffer space (pixels)
    float m_UIMouseX = 0.0f;
    float m_UIMouseY = 0.0f;
    float m_UIMouseNX = 0.0f;
    float m_UIMouseNY = 0.0f;
    bool  m_UIMouseValid = false;
    bool  m_UIMouseNormalizedValid = false;

    struct UISceneCaptureState {
        Camera camera;
        EntityID lastTargetId = INVALID_ENTITY_ID;
        uint64_t lastTargetGuidHigh = 0;
        uint64_t lastTargetGuidLow = 0;
        uint32_t lastWidth = 0;
        uint32_t lastHeight = 0;
        bool initialized = false;
    };
    std::unordered_map<EntityID, UISceneCaptureState> m_UISceneCaptureStates;
    // Active UI offscreen target view IDs used by canvas scene-capture panels and world-space canvases.
    std::unordered_set<uint16_t> m_UISceneCaptureViewIds;
    std::unordered_map<EntityID, uint16_t> m_UIWorldCanvasViewIds;
    uint16_t m_NextUISceneCaptureViewId = 60;

    // When true, views 0/1/2 render to the offscreen scene framebuffer; otherwise, they render directly to the backbuffer
    bool m_RenderToOffscreen = true;
    // Toggle: when false, use legacy geometry-scaled outline; when true, use screen-space mask+dilate
    
    // Outline parameters (editor defaults)
    float m_OutlineThicknessPx = 3.0f;
    glm::vec4 m_OutlineColor = glm::vec4(1.0f, 0.6f, 0.0f, 1.0f);
 
public:
    void SetShowUIOverlay(bool v){ m_ShowUIOverlay = v; }
    bool WasUIInputConsumedThisFrame() const { return m_UIInputConsumed; }
    void SetUIMode(bool enabled){ m_ShowUIOverlay = enabled; }
    void SetShadowReceive(float receive);
    void SetUIMousePosition(float x, float y, float nx, float ny, bool valid){
        m_UIMouseX = x;
        m_UIMouseY = y;
        m_UIMouseNX = nx;
        m_UIMouseNY = ny;
        m_UIMouseValid = valid;
        m_UIMouseNormalizedValid = valid;
    }
    bool GetUIMouseNormalized(float& outX, float& outY) const { if (!m_UIMouseNormalizedValid) return false; outX = m_UIMouseNX; outY = m_UIMouseNY; return true; }
    bool GetUIMousePosition(float& outX, float& outY) const { if (!m_UIMouseValid) return false; outX = m_UIMouseX; outY = m_UIMouseY; return true; }
    void ConsumeUIInput(){ m_UIInputConsumed = true; }
    // Get scene framebuffer handle (for rendering overlays to the scene texture)
    bgfx::FrameBufferHandle GetSceneFrameBuffer() const { 
        if (m_RenderToOffscreen) return m_SceneFrameBuffer;
        bgfx::FrameBufferHandle invalid = BGFX_INVALID_HANDLE;
        return invalid;
    }
    void SetShowUIRects(bool v){ m_ShowUIRects = v; }
    bool GetShowUIRects() const { return m_ShowUIRects; }
    void SetOutlineThickness(float px){ m_OutlineThicknessPx = px; }
    void SetOutlineColor(const glm::vec4& color){ m_OutlineColor = color; }
 
 private:
    // Helper: draw world-space axis-aligned bounding box on a given view (default debug view 0)
    void DrawAABB(const glm::vec3& worldMin, const glm::vec3& worldMax, uint16_t viewId = 0);
    void ApplyDebugLineColor(const glm::vec4& color) const;
    void ApplyDefaultDebugLineColor() const;
    void RenderUIOverlay(Scene& scene,
        uint16_t viewId,
        uint32_t width,
        uint32_t height,
        bool allowInput,
        CanvasComponent::RenderSpace renderSpace = CanvasComponent::RenderSpace::ScreenSpace,
        EntityID targetCanvasEntity = INVALID_ENTITY_ID,
        std::unordered_set<uint16_t>* currentOffscreenViewIds = nullptr,
        bool releaseUnusedOffscreenTargets = true,
        bool updateMouseState = true);
    void RenderWorldSpaceCanvases(Scene& scene,
        Camera* renderCamera,
        const float* viewMtx,
        const float* projMtx,
        uint16_t worldViewId,
        bgfx::FrameBufferHandle framebuffer,
        uint32_t width,
        uint32_t height,
        bool allowInput,
        std::unordered_set<uint16_t>& currentOffscreenViewIds);
    bool PrepareGpuSkinningAtlases(Scene& scene, const std::vector<EntityID>& primaryEntities, const std::vector<EntityID>* secondaryEntities = nullptr);
    void PrepareGpuMaterializedSkinnedMeshes(Scene& scene, const std::vector<EntityID>& primaryEntities, const std::vector<EntityID>* secondaryEntities = nullptr);
    void BindGpuSkinningAtlasGlobals();
    void BindSkinningIfChanged(const SkinningComponent* skinning, SkinningBindCacheState& cache);
    void BindSkinningInstanceRecord(uint32_t recordIndex);
    glm::vec4 GetSkinningInstanceObjectIdPacked(uint32_t recordIndex) const;
    bgfx::VertexBufferHandle GetOrCreateGpuMorphVertexBuffer(const Mesh* mesh);
    bool TryGetGpuMaterializedSkinnedVertexBuffer(Scene& scene, EntityID entityId, const Mesh* mesh, bgfx::VertexBufferHandle& outVertexBuffer) const;
    bool ResolveGpuMaterializedSkinnedColorProgram(bgfx::ProgramHandle skinnedProgram, bgfx::ProgramHandle& outProgram) const;
    void DestroyGpuSkinningResources();
    
    // Helper: Compute and set normal matrix uniform from transform matrix
    // This handles non-uniform scaling correctly by computing transpose(inverse(mat3(model)))
    void SetNormalMatrixUniform(const float* transform);
    void RenderObjectIdScene(Scene& scene, uint16_t viewId);

    // Debug draw flags (editor)
    bool m_ShowGrid = true;
    bool m_ShowColliders = false;  // debug-only by default
    bool m_ShowAABBs = false;
    bool m_ShowCameraFrustums = true;
    bool m_EnableFrustumCulling = true;
    std::vector<void(*)(uint16_t)> m_OverlayCallbacks;
    bool m_DebugDrawInPlayMode = false;
    bool m_RuntimeStatsCaptureEnabled = false;
    glm::vec4 m_DefaultDebugLineColor = glm::vec4(0.5f, 0.5f, 0.5f, 0.35f);
    RuntimeStatsFrame m_LastRuntimeStatsFrame{};
    // Reusable fullscreen triangle buffers
    bgfx::VertexBufferHandle m_FullscreenVB = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle  m_FullscreenIB = BGFX_INVALID_HANDLE;
    bool m_EditorLightingOverride = false;

    bool ShouldApplyEditorLightingOverride(const Scene& scene) const;
    
    // Shadow uniforms
    bgfx::UniformHandle u_LightViewProj = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_LightViewProjCSM = BGFX_INVALID_HANDLE; // array[4]
    bgfx::UniformHandle u_ShadowParams = BGFX_INVALID_HANDLE;     // x=bias, y=normalBias, z=softness, w=strength
    bgfx::UniformHandle u_ShadowTexelSize = BGFX_INVALID_HANDLE;  // x=1/w, y=1/h, z=samples, w=originBottomLeft
    bgfx::UniformHandle s_ShadowMap = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_ShadowLightDir = BGFX_INVALID_HANDLE;   // xyz=dir, w=0
    bgfx::UniformHandle u_CascadeSplits = BGFX_INVALID_HANDLE;     // xyz = split distances, w = count
    bgfx::UniformHandle u_CascadeScaleBias = BGFX_INVALID_HANDLE;  // array[4], xy=scale, zw=bias
    bgfx::UniformHandle u_ShadowReceive = BGFX_INVALID_HANDLE;     // x = receive shadows (0/1)
    bgfx::UniformHandle s_PointShadowMap = BGFX_INVALID_HANDLE;    // point shadow atlas sampler
    bgfx::UniformHandle u_PointShadowMeta = BGFX_INVALID_HANDLE;   // array[kMaxPointShadowLights], x=enabled, y=lightSlot, z=range, w=bias
    bgfx::UniformHandle u_PointShadowLightPos = BGFX_INVALID_HANDLE; // array[kMaxPointShadowLights], xyz=point light position
    bgfx::UniformHandle u_PointShadowAtlas = BGFX_INVALID_HANDLE;  // x=tileCols, y=tileRows, z=1/atlasW, w=1/atlasH
    bgfx::UniformHandle s_ShadowDebug = BGFX_INVALID_HANDLE;       // shadow atlas debug sampler
    bgfx::UniformHandle u_ShadowDebugParams = BGFX_INVALID_HANDLE; // x=originBottomLeft, y=selectedCascade, z=tiles, w=unused
    bgfx::ProgramHandle m_ShadowDebugProgram = BGFX_INVALID_HANDLE;

    // GPU skinning atlas resources. We stream the authoritative skeleton
    // palette into a shared texture once per frame and bind only small per-draw
    // metadata for skinned meshes.
    bgfx::UniformHandle s_BoneAtlas = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_BoneRemapAtlas = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_SkinningInstanceAtlas = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_MorphVertexAtlas = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_MorphEntryAtlas = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle s_MorphActiveAtlas = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_SkinningBoneAtlasInfo = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_SkinningRemapAtlasInfo = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_SkinningInstanceAtlasInfo = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_MorphVertexAtlasInfo = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_MorphEntryAtlasInfo = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_MorphActiveAtlasInfo = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_SkinningParams = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_SkinningExtra = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_SkinningInstanceRecord = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_SkinningMeshFromSkeleton = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_SkinningMorphParams = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_SkinningBoneAtlasTex = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_SkinningRemapAtlasTex = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_SkinningInstanceAtlasTex = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_MorphVertexAtlasTex = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_MorphEntryAtlasTex = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_MorphActiveAtlasTex = BGFX_INVALID_HANDLE;
    std::vector<glm::vec4> m_SkinningBoneAtlasCpu;
    std::vector<uint16_t> m_SkinningRemapAtlasCpu;
    struct GpuSkinningInstanceRecord {
        glm::vec4 Params = glm::vec4(0.0f);
        glm::vec4 Extra = glm::vec4(0.0f);
        glm::mat4 MeshFromSkeleton = glm::mat4(1.0f);
        glm::vec4 Morph = glm::vec4(0.0f);
        glm::vec4 ObjectIdPacked = glm::vec4(0.0f);
    };
    std::vector<GpuSkinningInstanceRecord> m_SkinningInstanceRecords;
    std::vector<glm::vec4> m_SkinningInstanceAtlasCpu;
    std::vector<glm::vec4> m_MorphVertexAtlasCpu;
    std::vector<glm::vec4> m_MorphEntryAtlasCpu;
    std::vector<glm::vec4> m_MorphActiveAtlasCpu;
    struct CachedGpuMorphGeometry {
        std::vector<glm::vec4> VertexTexels;
        std::vector<glm::vec4> EntryTexels;
        uint32_t VertexCount = 0;
        uint32_t EntryCount = 0;
    };
    struct GpuMaterializedSkinningSourceBuffers {
        bgfx::DynamicVertexBufferHandle Positions = BGFX_INVALID_HANDLE;
        bgfx::DynamicVertexBufferHandle Normals = BGFX_INVALID_HANDLE;
        bgfx::DynamicVertexBufferHandle UVs = BGFX_INVALID_HANDLE;
        bgfx::DynamicVertexBufferHandle BoneIndices = BGFX_INVALID_HANDLE;
        bgfx::DynamicVertexBufferHandle BoneWeights = BGFX_INVALID_HANDLE;
        uint32_t VertexCount = 0;
        uint64_t SourceFingerprint = 0;
        bool UsesCpuBlendedMorphSource = false;
    };
    struct GpuMaterializedSkinnedMeshCache {
        const Mesh* SourceMesh = nullptr;
        bgfx::VertexBufferHandle Output = BGFX_INVALID_HANDLE;
        uint32_t VertexCount = 0;
        uint64_t LastFingerprint = 0;
        uint64_t LastTouchedFrame = 0;
    };
    uint16_t m_SkinningBoneAtlasWidth = 0;
    uint16_t m_SkinningBoneAtlasHeight = 0;
    uint16_t m_SkinningRemapAtlasWidth = 0;
    uint16_t m_SkinningRemapAtlasHeight = 0;
    uint16_t m_SkinningInstanceAtlasWidth = 0;
    uint16_t m_SkinningInstanceAtlasHeight = 0;
    uint16_t m_MorphVertexAtlasWidth = 0;
    uint16_t m_MorphVertexAtlasHeight = 0;
    uint16_t m_MorphEntryAtlasWidth = 0;
    uint16_t m_MorphEntryAtlasHeight = 0;
    uint16_t m_MorphActiveAtlasWidth = 0;
    uint16_t m_MorphActiveAtlasHeight = 0;
    uint64_t m_SkinningBoneAtlasLastFingerprint = 0;
    uint64_t m_SkinningRemapAtlasLastFingerprint = 0;
    uint64_t m_SkinningInstanceAtlasLastFingerprint = 0;
    uint64_t m_MorphVertexAtlasLastFingerprint = 0;
    uint64_t m_MorphEntryAtlasLastFingerprint = 0;
    uint64_t m_MorphActiveAtlasLastFingerprint = 0;
    uint32_t m_SkinningAtlasLastBoneTexelCount = 0;
    uint32_t m_SkinningAtlasLastRemapTexelCount = 0;
    uint32_t m_SkinningAtlasLastInstanceTexelCount = 0;
    uint32_t m_MorphAtlasLastVertexTexelCount = 0;
    uint32_t m_MorphAtlasLastEntryTexelCount = 0;
    uint32_t m_MorphAtlasLastActiveTexelCount = 0;
    uint32_t m_SkinningAtlasUploadSerial = 0;
    uint64_t m_SkinningAtlasFrameSerial = 0;
    uint64_t m_SkinningAtlasPreparedFrameSerial = 0;
    Scene* m_SkinningAtlasPreparedScene = nullptr;
    std::vector<EntityID> m_SkinningAtlasUnionEntities;
    std::unordered_set<EntityID> m_SkinningAtlasCoveredEntities;
    std::unordered_map<const Mesh*, bgfx::VertexBufferHandle> m_GpuMorphVertexBuffers;
    std::unordered_map<uint64_t, CachedGpuMorphGeometry> m_CachedGpuMorphGeometries;
    std::unordered_map<const Mesh*, GpuMaterializedSkinningSourceBuffers> m_GpuMaterializedSkinningSources;
    std::unordered_map<uint64_t, GpuMaterializedSkinnedMeshCache> m_GpuMaterializedSkinnedMeshes;
    std::unordered_set<uint64_t> m_GpuMaterializedSkinningCoveredInstances;
    uint64_t m_GpuMaterializedSkinningPreparedFrameSerial = 0;
    bgfx::ProgramHandle m_GpuMaterializedSkinningProgram = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_GpuMaterializedSkinningDispatch = BGFX_INVALID_HANDLE;
    bool m_GpuSkinningAtlasSupported = false;
    bool m_GpuSkinningAtlasReady = false;
    bool m_GpuMorphAtlasSupported = false;
    bool m_GpuMaterializedSkinningSupported = false;

    // Shadow resources
    uint16_t m_ShadowViewId = 50;
    // Dedicated point-shadow view range (6 faces * kMaxPointShadowLights).
    // Chosen to avoid scene/debug/outline ranges while still fitting maxViews=256.
    uint16_t m_PointShadowViewId = 224;
    bgfx::FrameBufferHandle m_ShadowFB = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_ShadowDepth = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_ShadowDebugColor = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle m_PointShadowFB = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_PointShadowDepth = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_PointShadowColor = BGFX_INVALID_HANDLE;
    glm::mat4 m_LightViewProj = glm::mat4(1.0f);
    uint32_t m_ShadowRes = 0;
    uint32_t m_PointShadowFaceRes = 0;
    uint32_t m_PointShadowAtlasWidth = 0;
    uint32_t m_PointShadowAtlasHeight = 0;
    int m_PointShadowCount = 0;
    std::array<int, kMaxPointShadowLights> m_PointShadowLightSlots{};
    std::array<glm::vec3, kMaxPointShadowLights> m_PointShadowLightPosWS{};
    std::array<float, kMaxPointShadowLights> m_PointShadowRanges{};
    glm::vec3 m_ShadowDirWS = glm::vec3(0.0f, -1.0f, 0.0f);
    bool m_ShowShadowDebugOverlay = false;
    // Current render context shadow scope (used to avoid cross-viewport leakage)
    Scene* m_ShadowContextScene = nullptr;
    bool m_ShadowContextEnabled = true;
    // Cascaded shadows: split distances and matrices
    std::array<glm::mat4, 4> m_CascadeMatrices{};
    std::array<glm::vec4, 4> m_CascadeScaleBias{}; // xy scale, zw bias for atlas rect
    int m_CascadeCount = 1;
    std::array<float, 4> m_CascadeSplits{}; // distances in world units from camera
    std::array<uint32_t, 4> m_ShadowCascadeSubmitCounts{}; // shadow submits per cascade (debug)

    // Simple plane and frustum utilities for CPU culling
    struct Plane { glm::vec4 p; }; // xyz = normal, w = -d
    struct Frustum { Plane planes[6]; };
    static inline float dotCoord(const glm::vec4& plane, const glm::vec3& pt) { return plane.x*pt.x + plane.y*pt.y + plane.z*pt.z + plane.w; }
    Frustum BuildFrustum(const float* view, const float* proj) const;
    bool AabbIntersectsFrustum(const Frustum& f, const glm::vec3& wmin, const glm::vec3& wmax) const;
    void DrawShadowDebugOverlay(const Camera* camera);
    void EnsureFullscreenTriangle();
    void DestroyFullscreenTriangle();

    // ========================================================================
    // PERFORMANCE: Scratch buffers for RenderScene to eliminate per-frame allocations
    // These persist across frames; .clear() only resets size without deallocation.
    // ========================================================================
    struct MaterialInstanceBinding {
        Material* MaterialPtr = nullptr;
        Material::PackedPropertyOverrides MeshOverrides;
        Material::PackedPropertyOverrides SlotOverrides;
        Material::PackedPropertyOverrides ProxyOverrides;
        bool HasPBRScalar1 = false;
        glm::vec4 PBRScalar1 = glm::vec4(0.0f);
        bool HasShadowReceive = false;
        float ShadowReceive = 1.0f;
        bool UnsupportedForInstancing = false;

        [[nodiscard]] bool HasPackedOverrides() const {
            return !MeshOverrides.Empty() ||
                   !SlotOverrides.Empty() ||
                   !ProxyOverrides.Empty();
        }

        [[nodiscard]] bool HasAnyOverrides() const {
            return HasPackedOverrides() ||
                   HasPBRScalar1 ||
                   HasShadowReceive;
        }
    };

    struct MaterialInstanceCacheKey {
        const Material* MaterialPtr = nullptr;
        const MaterialPropertyBlock* MeshBlock = nullptr;
        const MaterialPropertyBlock* SlotBlock = nullptr;
        const MaterialPropertyBlock* ProxyBlock = nullptr;

        bool operator==(const MaterialInstanceCacheKey& other) const {
            return MaterialPtr == other.MaterialPtr &&
                   MeshBlock == other.MeshBlock &&
                   SlotBlock == other.SlotBlock &&
                   ProxyBlock == other.ProxyBlock;
        }
    };

    struct MaterialInstanceCacheKeyHasher {
        size_t operator()(const MaterialInstanceCacheKey& key) const {
            size_t seed = std::hash<const void*>{}(key.MaterialPtr);
            seed ^= std::hash<const void*>{}(key.MeshBlock) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
            seed ^= std::hash<const void*>{}(key.SlotBlock) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
            seed ^= std::hash<const void*>{}(key.ProxyBlock) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
            return seed;
        }
    };

    struct MaterialInstanceCacheEntry {
        MaterialInstanceBinding Binding;
        uint64_t MeshVersion = 0;
        uint64_t SlotVersion = 0;
        uint64_t ProxyVersion = 0;
        uint64_t LastTouchedFrame = 0;
    };

    struct DrawItem {
        Mesh* mesh = nullptr;               // Raw ptr (guaranteed valid for frame)
        Material* material = nullptr;       // Raw ptr (guaranteed valid for frame)
        const MaterialInstanceBinding* materialInstance = nullptr;
        MaterialPropertyBlockStack propertyBlocks{};
        EntityID entityId = INVALID_ENTITY_ID;
        uint32_t indexStart = 0;
        uint32_t indexCount = 0;
        float transform[16]{};
        bool renderOnTop = false;
        int renderOrder = 0;
        bool showBackfaces = false;
        bool isTransparent = false;
        float sortDepth = 0.0f;
        bool alphaOverride = false;
        bool alphaEnable = false;
        bool alphaCutoutOverride = false;
        float alphaCutoutThreshold = 0.5f;
        bool depthWriteOverride = false;
        bool depthWriteEnable = true;
        bool depthTestOverride = false;
        bool depthTestEnable = true;
        bool receiveShadows = true;
        uint64_t resolvedStateFlags = 0;
        uint64_t materialBindingKey = 0;
        bool allowEquivalentBindCache = false;
        uint64_t instancingVariationHash = 0;
        bool instancingHasPBRScalar1 = false;
        glm::vec4 instancingPBRScalar1 = glm::vec4(0.0f);
        bool instancingHasShadowReceive = false;
        float instancingShadowReceive = 1.0f;
        BlendShapeComponent* blendShapes = nullptr;
        bool usesGpuMorphTargets = false;
        
        // Skinning: per-entity bone palette binding
        // Each skinned mesh needs its own bone palette (with invMeshWorld applied)
        // to support correct skinning when materials are shared between meshes.
        SkinningComponent* skinning = nullptr;
        
        // Skeleton tracking (kept for potential future batching optimization)
        SkeletonComponent* skeleton = nullptr;
        
        // PERF: Instancing flag - true if this item can be batched via GPU instancing
        bool canInstance = false;
        bool canSkinnedInstance = false;
    };

    enum class UIItemType { Panel, Text, LayoutGroup };

    struct UIDrawItem {
        int canvasOrder = 0;
        int hierarchyDepth = 0;  // Depth in entity hierarchy (for parent-before-child ordering)
        int z = 0;
        float canvasOpacity = 1.0f;
        UIItemType type = UIItemType::Panel;
        PanelComponent* panel = nullptr;
        TextRendererComponent* text = nullptr;
        EntityData* data = nullptr;
        EntityID entityId = INVALID_ENTITY_ID;
        CanvasComponent* canvas = nullptr;  // Reference to parent canvas for reference resolution scaling
        // Computed screen rect (x, y, width, height) - filled during layout pass
        glm::vec4 computedRect = {0, 0, 0, 0};
    };
    
    std::vector<LightData> m_ScratchLights;
    std::vector<EntityID> m_ScratchLightEntityIds;
    std::vector<EntityID> m_ScratchVisibleMeshIds;
    std::vector<EntityData*> m_ScratchVisibleMeshData;
    std::vector<cm::world::RuntimeEntityHandle> m_ScratchVisibleMeshHandles;
    std::vector<EntityID> m_ScratchTerrainEntityIds;
    std::vector<EntityData*> m_ScratchTerrainData;
    std::vector<cm::world::RuntimeEntityHandle> m_ScratchTerrainHandles;
    std::vector<EntityID> m_ScratchShadowMeshEntityIds;
    std::vector<EntityData*> m_ScratchShadowMeshData;
    std::vector<cm::world::RuntimeEntityHandle> m_ScratchShadowMeshHandles;
    std::vector<EntityID> m_ScratchShadowTerrainEntityIds;
    std::vector<EntityData*> m_ScratchShadowTerrainData;
    std::vector<cm::world::RuntimeEntityHandle> m_ScratchShadowTerrainHandles;
    std::vector<DrawItem> m_ScratchDraws;
    std::vector<DrawItem> m_ScratchOpaque;
    std::vector<DrawItem> m_ScratchTransparent;
    std::vector<UIDrawItem> m_ScratchUIItems;
    std::vector<size_t> m_ScratchUIDrawOrder;
    std::unordered_map<EntityID, size_t> m_ScratchUIIndex;
    
    // Cached lighting state for external systems (instancer, etc.)
    Environment m_CachedEnvironment;
    bool m_CachedEditorLightingOverride = false;
    
    // =========================================================================
    // PERF: GPU Instancing Manager for batching repeated mesh+material combos
    // =========================================================================
    cm::rendering::InstanceManager m_InstanceManager;
    cm::rendering::InstanceManager m_ObjectIdInstanceManager;
    std::array<cm::rendering::InstanceManager, 4> m_ShadowInstanceManagers;
    std::array<cm::rendering::InstanceManager, kMaxPointShadowLights * 6> m_PointShadowInstanceManagers;
    
    // Scratch buffer for non-instanced draw items
    std::vector<DrawItem> m_ScratchNonInstanced;
    std::unordered_map<MaterialInstanceCacheKey,
                       std::unique_ptr<MaterialInstanceCacheEntry>,
                       MaterialInstanceCacheKeyHasher> m_MaterialInstanceCache;
    struct StaticBoundsCacheEntry {
        Mesh* mesh = nullptr;
        float boundsPadding = 1.0f;
        float worldMatrix[16]{};
        glm::vec3 worldMin = glm::vec3(0.0f);
        glm::vec3 worldMax = glm::vec3(0.0f);
        bool valid = false;
    };
    std::unordered_map<EntityID, StaticBoundsCacheEntry> m_StaticBoundsCache;
    uint64_t m_StaticBoundsCacheSceneRevision = 0;
    struct TerrainChunkBatchCache {
        uint64_t token = 0;
        uint32_t totalVisible = 0;
        std::array<terrain::ChunkLODBatch, terrain::ChunkSystemConfig::kMaxLODLevels> batches;
    };
    struct TerrainChunkShadowCascadeBatchCache {
        uint32_t totalVisible = 0;
        float distanceLimit = 0.0f;
        std::array<terrain::ChunkLODBatch, terrain::ChunkSystemConfig::kMaxLODLevels> batches;
    };
    struct TerrainChunkShadowBatchCache {
        uint64_t token = 0;
        uint32_t totalVisible = 0;
        std::array<TerrainChunkShadowCascadeBatchCache, 4> cascades;
    };
    std::unordered_map<EntityID, TerrainChunkBatchCache> m_TerrainChunkBatchCache;
    std::unordered_map<EntityID, TerrainChunkShadowBatchCache> m_TerrainChunkShadowBatchCache;
    uint64_t m_TerrainChunkPrepToken = 1;

    struct SkinnedOcclusionQueryEntry {
        bgfx::OcclusionQueryHandle Handle = BGFX_INVALID_HANDLE;
        bool LastVisible = true;
        int32_t LastVisibleSamples = -1;
        uint8_t ConsecutiveInvisibleFrames = 0;
        uint64_t LastTouchedFrame = 0;
    };
    std::unordered_map<uint64_t, SkinnedOcclusionQueryEntry> m_SkinnedOcclusionQueries;
    uint64_t m_SkinnedOcclusionFrame = 0;
};
