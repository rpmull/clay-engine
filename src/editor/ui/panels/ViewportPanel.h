#pragma once
#include <bgfx/bgfx.h>
#include <glm/glm.hpp>
#include <string>
#include <utility>
#include <imgui.h>
#include <ImGuizmo.h>
#include <memory>
#include "core/ecs/Entity.h"
#include "EditorPanel.h"
#include "editor/ui/command/ConsoleOverlay.h"
#include "editor/rendering/Picking.h"
#include "editor/EditorSettings.h"

class Scene;

class ViewportPanel : public EditorPanel {
public:

    struct ViewportCameraState {
        glm::vec3 Target{ 0.0f };
        float Distance = 10.0f;
        float Yaw = 0.0f;
        float Pitch = 0.0f;
        float FieldOfView = 60.0f;
        float NearClip = 0.1f;
        float FarClip = 1000.0f;
    };


    ViewportPanel(Scene& scene, EntityID* selectedEntity, bool useInternalCamera = false)
        : m_SelectedEntity(selectedEntity), m_UseInternalCamera(useInternalCamera) {
       SetContext(&scene);
       if (m_UseInternalCamera) {
           m_Camera = std::make_unique<class Camera>(60.0f, 16.0f/9.0f, 0.1f, 100.0f);
       }
       // Embedded viewports should not stomp global UI input or queued picks
       m_ReportUIMouse = !m_UseInternalCamera;
       m_UseQueuedPicking = !m_UseInternalCamera;
        }

		void OnImGuiRender(bgfx::TextureHandle sceneTexture);
		// Render the viewport contents embedded inside the current ImGui window/child
		// without opening its own window. Useful for panels that host an internal viewport
		// such as the Prefab Editor.
		void OnImGuiRenderEmbedded(bgfx::TextureHandle sceneTexture, const char* idLabel = "EmbeddedViewport");
    void HandleCameraControls();
    class Camera* GetViewportCamera() const;
    ViewportCameraState GetCameraState() const;
    void ApplyCameraState(const ViewportCameraState& state);
    void ResetToDefaultCameraState();
    // Editor utility: frame the currently selected entity
    void FrameSelected(float durationSeconds = 0.35f);
    // Accessor for embedded camera (nullptr when using global camera)
    class Camera* GetPanelCamera() const { return m_Camera.get(); }
    // Optional render target size override (embedded viewports)
    void SetRenderTargetSize(uint32_t width, uint32_t height) { m_RenderTargetWidth = width; m_RenderTargetHeight = height; }
    std::pair<uint32_t, uint32_t> GetRenderTargetSize() const { return { m_RenderTargetWidth, m_RenderTargetHeight }; }

    // UI: allow parent layer to set the display scene name and dirty flag for window title
    void SetDisplaySceneTitle(const std::string& title) { m_DisplaySceneTitle = title; }
    // Control whether this viewport reports UI mouse positions to the renderer
    void SetReportUIMouse(bool enabled) { m_ReportUIMouse = enabled; }
    // Control whether this viewport uses queued (global) picking
    void SetQueuedPickingEnabled(bool enabled) { m_UseQueuedPicking = enabled; }

    // Focus/hover state (main viewport window only)
    bool IsWindowFocusedOrHovered() const { return m_WindowFocusedOrHovered; }
    bool IsViewportInteractionLocked() const { return IsGizmoUsing() || IsGizmoOver() || m_IsDraggingAsset; }
    bool IsViewportHovered() const;
    
    // Hover entity (for optional hover outline)
    EntityID GetHoveredEntity() const { return m_HoveredEntity; }

    // Gizmo operation control
    void SetOperation(ImGuizmo::OPERATION op) { m_CurrentOperation = op; }
    ImGuizmo::OPERATION GetCurrentOperation() const { return m_CurrentOperation; }
    void SetShowGizmos(bool enabled) { m_ShowGizmos = enabled; }
    bool GetShowGizmos() const { return m_ShowGizmos; }

    // Editor paint/draw tools block picking (set by UILayer before render)
    void SetSplineDrawModeActive(bool active) { m_SplineDrawModeActive = active; }
    void SetSoftbodyPaintModeActive(bool active) { m_SoftbodyPaintModeActive = active; }
    void SetRiverDrawModeActive(bool active) { m_RiverDrawModeActive = active; }

    // Picking API
    bool HasPickRequest() const { return m_ShouldPick; }
    std::pair<float, float> GetNormalizedPickCoords() const {
        return { m_NormalizedPickX, m_NormalizedPickY };
    }
    void ClearPickRequest() { m_ShouldPick = false; }

    void DrawUIRectOverlay(ImDrawList* dl, const ImVec2& viewportTL, const ImVec2& viewportSize, Scene* scene);

private:
    enum class KeyboardTransformAxis : int {
        None = -1,
        X = 0,
        Y = 1,
        Z = 2
    };

    struct KeyboardTransformSession {
        bool Active = false;
        EntityID Entity = INVALID_ENTITY_ID;
        ImGuizmo::OPERATION Operation = ImGuizmo::TRANSLATE;
        KeyboardTransformAxis Axis = KeyboardTransformAxis::None;
        ImGuizmo::OPERATION PreviousOperation = ImGuizmo::TRANSLATE;
        ImGuizmo::MODE PreviousMode = ImGuizmo::WORLD;
        TransformComponent BaselineTransform{};
        ImVec2 VirtualMousePos = ImVec2(0.0f, 0.0f);
        ImVec2 VirtualMouseDelta = ImVec2(0.0f, 0.0f);
        bool MouseCaptured = false;
    };

    void HandleEntityPicking();
    void Draw2DGrid();
    void HandleAssetDragDrop(const ImVec2& viewportPos);
    void DrawGizmo();
    void DrawUIGizmo();
    void DrawRadiusGizmo();
    void DrawConsoleOverlay(const ImVec2& viewportTL, const ImVec2& viewportSize);
    void UpdateGhostPosition(float mouseX, float mouseY);
    void DrawGhostPreview();
    bool IsGizmoOver() const;
    bool IsGizmoUsing() const;
    void DecomposeMatrix(const float* matrix, glm::vec3& pos, glm::vec3& rot, glm::quat& rotQ, glm::vec3& scale);
    bool SyncKeyboardTransformHotkeys();
    void RestoreKeyboardTransformBaseline();
    void BeginKeyboardTransformMouseCapture();
    void UpdateKeyboardTransformVirtualMouse();
    void EndKeyboardTransformMouseCapture();
    bool ShouldCommitKeyboardTransformSession() const;
    void EndKeyboardTransformSession(bool suppressMouseForFrame = false);
    void TweenCameraToState(const glm::vec3& target, float distance, float yaw, float pitch, float durationSeconds);
    void TweenAlignCameraToAxis(const glm::vec3& axisDirection, float durationSeconds);
    void EndTransformUndoAction();
    void EndEntityTransformUndoAction();
    void EndRadiusUndoAction();
    void EndUIUndoAction();
    void GatherEditorToolState(bool& terrainBrushActive,
                               bool& navLinkPaintActive,
                               bool& splineDrawActive,
                               bool& softbodyPaintActive,
                               bool& riverDrawActive) const;
    bool CanStartKeyboardTransformSession() const;
    bool CanContinueKeyboardTransformSession() const;

    void FinalizeAssetDrop();
    void EnsureAssetPreviewEntity();
    void UpdateAssetPreviewTransform();
    void CancelAssetPreview(bool commit);
    bool SupportsAssetPreview(const std::string& path) const;
    bool ProjectGhostOntoTerrain(const Ray& ray, glm::vec3& outPosition) const;
    bool RaycastToWorldPosition(const Ray& ray, glm::vec3& outPosition) const;

private:
    ImVec2 m_ViewportSize = { 0, 0 }; // actual drawn viewport image size (letterboxed)
    ImVec2 m_ViewportPos = { 0, 0 };  // screen-space top-left of the viewport image
    uint32_t m_RenderTargetWidth = 0;
    uint32_t m_RenderTargetHeight = 0;
    bool m_ShouldPick = false;
    // Track UI handle state to prevent scene picking/deselection when dragging UI elements
    bool m_UIHandleActive = false;
    bool m_UIHandleHovered = false;

    // Mouse coords for picking (normalized)
    float m_NormalizedPickX = 0.0f;
    float m_NormalizedPickY = 0.0f;

    // Orbit camera state
    float m_Yaw = 0.0f, m_Pitch = 0.0f, m_Distance = 10.0f;
    glm::vec3 m_Target = glm::vec3(0.0f);

    // Smooth zoom state
    float m_TargetDistance = 10.0f;  // Target distance for smooth interpolation
    float m_ZoomVelocity = 0.0f;     // Current zoom velocity for damping
    float m_LastScrollDelta = 0.0f;  // Track scroll delta for acceleration

    // Tween state for focus (editor-only)
    bool m_IsTweening = false;
    float m_TweenTime = 0.0f;
    float m_TweenDuration = 0.35f;
    float m_DistanceStart = 10.0f;
    float m_DistanceEnd = 10.0f;
    float m_YawStart = 0.0f;
    float m_YawEnd = 0.0f;
    float m_PitchStart = 0.0f;
    float m_PitchEnd = 0.0f;
    glm::vec3 m_TargetStart = glm::vec3(0.0f);
    glm::vec3 m_TargetEnd = glm::vec3(0.0f);
    
    // Hover entity tracking for outline preview
    EntityID m_HoveredEntity = INVALID_ENTITY_ID;
    
    // Throttle hover picking to reduce CPU cost (picking is expensive)
    float m_LastHoverPickTime = 0.0f;
    static constexpr float kHoverPickInterval = 0.05f; // 20 picks per second max

    bool m_ShowGizmos = true;
    bool m_GizmoOwnerThisFrame = false;
    bool m_GizmoOverThisFrame = false;
    bool m_GizmoUsingThisFrame = false;
    bool m_GizmoUsingLastFrame = false;
    bool m_IsDraggingAsset = false;
    std::string m_DraggedAssetPath;
    glm::vec3 m_GhostPosition;
    EntityID m_GhostEntityId = INVALID_ENTITY_ID;
    bool m_PreviewSceneWasDirty = false;
    std::string m_GhostAssetPath;
    bool m_CanPreviewDraggedAsset = false;
    float m_GridSize = 1.0f;

    EntityID* m_SelectedEntity = nullptr;

    ImGuizmo::OPERATION m_CurrentOperation = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE m_CurrentMode = ImGuizmo::WORLD;

    // Optional internal camera for fully-isolated embedded viewports
    bool m_UseInternalCamera = false;
    std::unique_ptr<class Camera> m_Camera;

    bool m_WindowFocusedOrHovered = false;

    // Window title display for main viewport (e.g., scene name + '*')
    std::string m_DisplaySceneTitle;

    // Embedded viewports should not report global UI mouse or use queued picking
    bool m_ReportUIMouse = true;
    bool m_UseQueuedPicking = true;

    bool m_SplineDrawModeActive = false;
    bool m_SoftbodyPaintModeActive = false;
    bool m_RiverDrawModeActive = false;
    bool m_SuppressMouseActionsThisFrame = false;
    KeyboardTransformSession m_KeyboardTransformSession{};
    bool m_TransformUndoActive = false;
    bool m_EntityTransformUndoActive = false;
    bool m_RadiusUndoActive = false;
    bool m_UIUndoActive = false;

    // Radius gizmo state
    bool m_IsDraggingRadius = false;
    int m_ActiveRadiusHandle = -1; // 0=X, 1=Y, 2=Z, -1=none
    float m_RadiusDragStart = 0.0f;
    glm::vec3 m_RadiusDragStartPos = glm::vec3(0.0f);
};
