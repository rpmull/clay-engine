#include "ViewportPanel.h"
#include "core/rendering/Renderer.h"
#include "core/rendering/Camera.h"
#include "editor/rendering/Picking.h" // New system for ray logic
#include "editor/EditorSettings.h"
#include <imgui.h>
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <cctype>
#include <limits>
#include "core/ecs/EntityData.h"
#include <utils/Time.h>
#include <cfloat>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include "editor/import/ModelLoader.h"
#include "core/rendering/Terrain.h"
#include <imgui_internal.h>
#include "editor/pipeline/AssetPipeline.h"
#include "editor/application.h"
#include "core/input/Input.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/Components.h"
#include "core/physics/Physics.h"
#include "core/utils/TerrainPainter.h"
#include "editor/tools/NavLinkPainter.h"
#include "core/physics/Physics.h"
#include "editor/tools/ResourceLayerPanel.h"
#include "editor/undo/EditorSceneUndoStack.h"
#include "core/rendering/TextureLoader.h"

namespace {
struct GizmoOwnerState {
    int frame = -1;
    ImGuiID owner = 0;
};

static GizmoOwnerState s_GizmoOwner;

constexpr float kCameraAxisAlignPitchLimit = 89.9f;
constexpr float kCameraAxisAlignDefaultDuration = 0.2f;

static bool BeginViewportGizmo(ImGuiID viewportId, bool wantsGizmo, const ImVec2& rectMin, const ImVec2& rectMax) {
    const int frame = ImGui::GetFrameCount();
    if (s_GizmoOwner.frame != frame) {
        s_GizmoOwner.frame = frame;
        s_GizmoOwner.owner = 0;
    }

    if (wantsGizmo) {
        if (s_GizmoOwner.owner == 0 || s_GizmoOwner.owner == viewportId) {
            s_GizmoOwner.owner = viewportId;
        }
    }

    if (s_GizmoOwner.owner == viewportId) {
        ImGuizmo::BeginFrame();
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect(rectMin.x, rectMin.y, rectMax.x - rectMin.x, rectMax.y - rectMin.y);
        return true;
    }

    return false;
}

static float NormalizeAngleDegrees(float angle) {
    while (angle > 180.0f) {
        angle -= 360.0f;
    }
    while (angle <= -180.0f) {
        angle += 360.0f;
    }
    return angle;
}

static float LerpAngleDegrees(float from, float to, float t) {
    const float delta = NormalizeAngleDegrees(to - from);
    return NormalizeAngleDegrees(from + delta * t);
}
} // namespace

// =============================
// Main Viewport Render
// =============================

// =============================================================
// RENDER VIEWPORT PANEL
// =============================================================
void ViewportPanel::OnImGuiRender(bgfx::TextureHandle sceneTexture) {
    const char* base = "Viewport";
    if (!m_DisplaySceneTitle.empty()) {
        std::string title = m_DisplaySceneTitle + " - " + base;
        std::string unique = title + std::string("###Viewport");
        ImGui::Begin(unique.c_str());
    } else {
        ImGui::Begin(base);
    }
    m_WindowFocusedOrHovered = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
                               ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    if (!m_UseInternalCamera) {
        SetRenderTargetSize(0, 0);
    }
    // Reuse the same rendering path as embedded panels so behavior is identical
    OnImGuiRenderEmbedded(sceneTexture, "MainViewportEmbedded");
    ImGui::End();
}

// Render the viewport inside an existing window/child instead of opening its own window
void ViewportPanel::OnImGuiRenderEmbedded(bgfx::TextureHandle sceneTexture, const char* idLabel) {
    m_WindowFocusedOrHovered = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
                               ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    // Compute letterboxed viewport to preserve aspect ratio
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float targetAspect = 16.0f / 9.0f; // default aspect if renderer size is unavailable
    uint32_t rw = m_RenderTargetWidth > 0 ? m_RenderTargetWidth : (uint32_t)Renderer::Get().GetWidth();
    uint32_t rh = m_RenderTargetHeight > 0 ? m_RenderTargetHeight : (uint32_t)Renderer::Get().GetHeight();
    if (rw > 0 && rh > 0) targetAspect = (float)rw / (float)rh;

    float availAspect = (avail.y > 0.0f) ? (avail.x / avail.y) : targetAspect;
    ImVec2 drawSize = avail;
    if (availAspect > targetAspect) {
        // Too wide: pillarbox
        drawSize.x = avail.y * targetAspect;
        drawSize.y = avail.y;
    } else {
        // Too tall: letterbox
        drawSize.x = avail.x;
        drawSize.y = avail.x / targetAspect;
    }

    // If no drawable size yet (e.g., hidden/zero-sized region), skip drawing to avoid ImGui assert
    if (drawSize.x <= 0.0f || drawSize.y <= 0.0f) {
        // Still update cached size/pos for consistency
        m_ViewportSize = ImVec2(0, 0);
        m_ViewportPos = ImGui::GetCursorScreenPos();
        return;
    }

    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImRect viewportFrame(cursor, ImVec2(cursor.x + avail.x, cursor.y + avail.y));
    ImDrawList* viewportDrawList = ImGui::GetWindowDrawList();
    const ImVec4 matteColor = ImLerp(ImGui::GetStyleColorVec4(ImGuiCol_ChildBg), ImGui::GetStyleColorVec4(ImGuiCol_WindowBg), 0.45f);
    viewportDrawList->AddRectFilled(viewportFrame.Min, viewportFrame.Max, ImGui::GetColorU32(matteColor), 8.0f);

    // Center the image within available region
    ImVec2 offset = ImVec2((avail.x - drawSize.x) * 0.5f, (avail.y - drawSize.y) * 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(cursor.x + offset.x, cursor.y + offset.y));

    // Reset gizmo ownership each frame; will be set only for the active viewport
    m_GizmoOwnerThisFrame = false;
    m_GizmoOverThisFrame = false;
    m_GizmoUsingThisFrame = false;
    m_SuppressMouseActionsThisFrame = false;

    // Draw scene texture
    if (bgfx::isValid(sceneTexture)) {
        ImTextureID texId = TextureLoader::ToImGuiTextureID(sceneTexture);
        ImGui::PushID(idLabel);
        ImGui::SetNextItemAllowOverlap();
        ImGui::Image(texId, drawSize, ImVec2(0, 0), ImVec2(1, 1));
        bool viewportImageVisible = ImGui::IsItemVisible();
        const ImVec2 imageMin = ImGui::GetItemRectMin();
        const ImVec2 imageMax = ImGui::GetItemRectMax();
        
        // Store size/pos for input mapping IMMEDIATELY after Image
        m_ViewportSize = drawSize;
        m_ViewportPos = imageMin;
        
        // Handle drag-drop IMMEDIATELY after Image while it's still the last item
        // This ensures BeginDragDropTarget() targets the viewport image
        HandleAssetDragDrop(ImGui::GetWindowPos());
        
        // Allow gizmo to receive clicks even though the Image is an item

        // Skip heavy work when the viewport image isn't visible (clipped/hidden)
        if (!viewportImageVisible) {
            ImGui::PopID();
            return;
        }

        ImVec2 min = imageMin;
        ImVec2 max = imageMax;
        ImVec2 mousePos = ImGui::GetMousePos();
        const bool mouseInViewport = mousePos.x >= min.x && mousePos.x <= max.x &&
                                     mousePos.y >= min.y && mousePos.y <= max.y;
        bool viewportHovered =
            ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                                   ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            mouseInViewport;
        const bool wantsGizmo = m_ShowGizmos && (viewportHovered || m_GizmoUsingLastFrame);
        m_GizmoOwnerThisFrame = BeginViewportGizmo(ImGui::GetID(this), wantsGizmo, min, max);

        // Prevent the viewport Image from capturing mouse clicks that should go to ImGuizmo
        // We need to clear active ID aggressively because:
        // 1. ImGuizmo::IsOver() uses previous frame's state (before Manipulate() is called)
        // 2. The Image widget will capture clicks before ImGuizmo can process them
        // Solution: Clear active ID only when the actual viewport image is hovered and
        // clicked, so overlapping floating windows don't lose focus just because they
        // happen to sit on top of the viewport's screen rectangle.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            // Release any stale viewport-owned active item so ImGuizmo can claim the drag.
            if (viewportHovered &&
                mouseInViewport &&
                !m_GizmoUsingLastFrame &&
                !m_IsDraggingAsset &&
                !m_UIHandleHovered &&
                !m_UIHandleActive &&
                !m_IsDraggingRadius) {
                ImGui::ClearActiveID();
            }
        }
        ImGui::PopID();
    }
    else {
        ImGui::Text("Invalid scene texture!");
        m_ViewportSize = drawSize;
        m_ViewportPos = ImGui::GetCursorScreenPos();
    }    

    // Viewport-scoped Delete key: remove selected entity when hovered and not typing
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)
        && !ImGui::IsAnyItemActive()
        && m_SelectedEntity && *m_SelectedEntity != -1
        && Input::WasKeyPressedThisFrame(KeyCode::Delete))
    {
        EditorSceneUndoStack::Get().RequestDeferredCommit(m_Context, "Delete Entity");
        if (m_Context) m_Context->QueueRemoveModelChild(*m_SelectedEntity);
        *m_SelectedEntity = -1;
    }

    DrawGizmo();
    DrawRadiusGizmo();
    DrawUIGizmo();
    m_GizmoUsingLastFrame = IsGizmoUsing();

    HandleCameraControls();

    // Draw command console overlay when in play mode
    DrawConsoleOverlay(m_ViewportPos, m_ViewportSize);

    // Draw ghost preview if dragging
    if (m_IsDraggingAsset) {
        DrawGhostPreview();
    }

    if (m_IsDraggingAsset && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
       FinalizeAssetDrop();
       m_IsDraggingAsset = false;
       }

    // Character controller debug draw is now handled in the world debug draw path
    // Draw all UI rect overlays if enabled (editor only)
    if (!m_Context->m_IsPlaying && Renderer::Get().GetShowUIRects()) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        DrawUIRectOverlay(dl, m_ViewportPos, m_ViewportSize, m_Context);
    }
}

Camera* ViewportPanel::GetViewportCamera() const
{
    if (m_UseInternalCamera && m_Camera)
        return m_Camera.get();

    if (Application::Get().IsPlaying()) {
        if (Camera* gameCam = Renderer::Get().GetCamera()) {
            return gameCam;
        }
    }

    // In edit mode, always use the editor camera for viewport operations.
    // This ensures gizmos, picking, and camera controls all use the same camera.
    Camera* editorCam = Renderer::Get().GetEditorCamera();
    return editorCam ? editorCam : Renderer::Get().GetCamera();
}

ViewportPanel::ViewportCameraState ViewportPanel::GetCameraState() const {
    ViewportCameraState state;
    state.Target = m_Target;
    state.Distance = m_Distance;
    state.Yaw = m_Yaw;
    state.Pitch = m_Pitch;
    if (Camera* cam = GetViewportCamera()) {
        state.FieldOfView = cam->GetFieldOfView();
        state.NearClip = cam->GetNearClip();
        state.FarClip = cam->GetFarClip();
    }
    return state;
}

void ViewportPanel::ApplyCameraState(const ViewportCameraState& state) {
    m_Target = state.Target;
    m_Distance = std::max(0.1f, state.Distance);
    m_Yaw = state.Yaw;
    m_Pitch = glm::clamp(state.Pitch, -89.0f, 89.0f);
    if (Camera* cam = GetViewportCamera()) {
        cam->SetFieldOfView(state.FieldOfView);
        cam->SetNearClip(std::max(0.001f, state.NearClip));
        float minFar = cam->GetNearClip() + 0.01f;
        cam->SetFarClip(std::max(state.FarClip, minFar));
        glm::vec3 dir;
        dir.x = cos(glm::radians(m_Yaw)) * cos(glm::radians(m_Pitch));
        dir.y = sin(glm::radians(m_Pitch));
        dir.z = sin(glm::radians(m_Yaw)) * cos(glm::radians(m_Pitch));
        dir = glm::normalize(dir);
        glm::vec3 camPos = m_Target - dir * m_Distance;
        cam->SetPosition(camPos);
        cam->LookAt(m_Target);
    }
}

void ViewportPanel::ResetToDefaultCameraState() {
    // Reset to the default values defined in ViewportCameraState
    ViewportCameraState defaults;
    ApplyCameraState(defaults);
}

void ViewportPanel::GatherEditorToolState(bool& terrainBrushActive,
                                          bool& navLinkPaintActive,
                                          bool& splineDrawActive,
                                          bool& softbodyPaintActive,
                                          bool& riverDrawActive) const {
    terrainBrushActive = TerrainPainter::IsBrushModeEnabled();
    navLinkPaintActive = NavLinkPainter::IsPaintModeEnabled();
    splineDrawActive = m_SplineDrawModeActive;
    softbodyPaintActive = m_SoftbodyPaintModeActive;
    riverDrawActive = m_RiverDrawModeActive;
}

bool ViewportPanel::CanStartKeyboardTransformSession() const {
    const bool isPlaying = Application::Get().IsPlaying() || (m_Context && m_Context->m_IsPlaying);
    if (!m_GizmoOwnerThisFrame || !m_ShowGizmos || !m_Context || !m_SelectedEntity) {
        return false;
    }
    if (*m_SelectedEntity == INVALID_ENTITY_ID || isPlaying || Input::IsBlocked()) {
        return false;
    }
    if (!(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
          ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows))) {
        return false;
    }
    if (ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive()) {
        return false;
    }
    if (m_IsDraggingAsset || m_IsDraggingRadius || m_UIHandleHovered || m_UIHandleActive) {
        return false;
    }

    bool terrainBrushActive = false;
    bool navLinkPaintActive = false;
    bool splineDrawActive = false;
    bool softbodyPaintActive = false;
    bool riverDrawActive = false;
    GatherEditorToolState(
        terrainBrushActive,
        navLinkPaintActive,
        splineDrawActive,
        softbodyPaintActive,
        riverDrawActive);
    return !terrainBrushActive && !navLinkPaintActive && !splineDrawActive && !softbodyPaintActive && !riverDrawActive;
}

bool ViewportPanel::CanContinueKeyboardTransformSession() const {
    const bool isPlaying = Application::Get().IsPlaying() || (m_Context && m_Context->m_IsPlaying);
    if (!m_KeyboardTransformSession.Active) {
        return false;
    }
    if (!m_GizmoOwnerThisFrame || !m_ShowGizmos || !m_Context || !m_SelectedEntity) {
        return false;
    }
    if (*m_SelectedEntity == INVALID_ENTITY_ID ||
        *m_SelectedEntity != m_KeyboardTransformSession.Entity ||
        isPlaying ||
        Input::IsBlocked()) {
        return false;
    }
    if (m_IsDraggingAsset || m_IsDraggingRadius || m_UIHandleHovered || m_UIHandleActive) {
        return false;
    }

    bool terrainBrushActive = false;
    bool navLinkPaintActive = false;
    bool splineDrawActive = false;
    bool softbodyPaintActive = false;
    bool riverDrawActive = false;
    GatherEditorToolState(
        terrainBrushActive,
        navLinkPaintActive,
        splineDrawActive,
        softbodyPaintActive,
        riverDrawActive);
    return !terrainBrushActive && !navLinkPaintActive && !splineDrawActive && !softbodyPaintActive && !riverDrawActive;
}

void ViewportPanel::EndKeyboardTransformSession(bool suppressMouseForFrame) {
    if (m_KeyboardTransformSession.Active) {
        m_CurrentOperation = m_KeyboardTransformSession.PreviousOperation;
        m_CurrentMode = m_KeyboardTransformSession.PreviousMode;
    }
    EndKeyboardTransformMouseCapture();
    ImGuizmo::EndKeyboardManipulation();
    m_KeyboardTransformSession = {};
    if (suppressMouseForFrame) {
        m_SuppressMouseActionsThisFrame = true;
    }
}

void ViewportPanel::RestoreKeyboardTransformBaseline() {
    if (!m_KeyboardTransformSession.Active || !m_Context) {
        return;
    }

    EntityData* data = m_Context->GetEntityData(m_KeyboardTransformSession.Entity);
    if (!data) {
        return;
    }

    data->Transform = m_KeyboardTransformSession.BaselineTransform;
    m_Context->MarkTransformDirty(m_KeyboardTransformSession.Entity);
    m_Context->MarkDirty();
}

void ViewportPanel::BeginKeyboardTransformMouseCapture() {
    if (!m_KeyboardTransformSession.Active ||
        m_KeyboardTransformSession.MouseCaptured ||
        !Application::HasInstance()) {
        return;
    }

    Application& app = Application::Get();
    if (app.IsPlaying()) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mouse = io.MousePos;
    if (!std::isfinite(mouse.x) ||
        !std::isfinite(mouse.y) ||
        mouse.x <= -FLT_MAX * 0.5f ||
        mouse.y <= -FLT_MAX * 0.5f) {
        mouse = ImVec2(
            m_ViewportPos.x + m_ViewportSize.x * 0.5f,
            m_ViewportPos.y + m_ViewportSize.y * 0.5f);
    }

    m_KeyboardTransformSession.VirtualMousePos = mouse;
    m_KeyboardTransformSession.VirtualMouseDelta = ImVec2(0.0f, 0.0f);
    app.SetEditModeMouseCaptureAllowed(true);
    app.SetMouseCaptured(true);
    m_KeyboardTransformSession.MouseCaptured = true;
    io.MousePos = mouse;
    io.MouseDelta = ImVec2(0.0f, 0.0f);
    io.WantCaptureMouse = true;
    io.WantCaptureKeyboard = true;
}

void ViewportPanel::UpdateKeyboardTransformVirtualMouse() {
    if (!m_KeyboardTransformSession.Active || !m_KeyboardTransformSession.MouseCaptured) {
        return;
    }

    auto delta = Input::GetMouseDelta();
    m_KeyboardTransformSession.VirtualMouseDelta = ImVec2(delta.first, delta.second);
    m_KeyboardTransformSession.VirtualMousePos.x += delta.first;
    m_KeyboardTransformSession.VirtualMousePos.y += delta.second;

    ImGuiIO& io = ImGui::GetIO();
    io.MousePos = m_KeyboardTransformSession.VirtualMousePos;
    io.MouseDelta = m_KeyboardTransformSession.VirtualMouseDelta;
    io.WantCaptureMouse = true;
    io.WantCaptureKeyboard = true;
}

void ViewportPanel::EndKeyboardTransformMouseCapture() {
    if (!Application::HasInstance()) {
        return;
    }

    Application& app = Application::Get();
    if (m_KeyboardTransformSession.MouseCaptured) {
        app.SetMouseCaptured(false);
    }
    app.SetEditModeMouseCaptureAllowed(false);
    m_KeyboardTransformSession.MouseCaptured = false;
}

bool ViewportPanel::ShouldCommitKeyboardTransformSession() const {
    return m_KeyboardTransformSession.Active &&
           (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
            Input::WasMouseButtonPressedThisFrame(MouseButton::Left));
}

void ViewportPanel::TweenCameraToState(const glm::vec3& target, float distance, float yaw, float pitch, float durationSeconds) {
    const float clampedDistance = std::max(0.1f, distance);
    const float clampedPitch = glm::clamp(pitch, -kCameraAxisAlignPitchLimit, kCameraAxisAlignPitchLimit);
    const float normalizedYaw = NormalizeAngleDegrees(yaw);
    const float clampedDuration = std::max(0.0f, durationSeconds);

    m_TargetStart = m_Target;
    m_TargetEnd = target;
    m_DistanceStart = m_Distance;
    m_DistanceEnd = clampedDistance;
    m_YawStart = NormalizeAngleDegrees(m_Yaw);
    m_YawEnd = normalizedYaw;
    m_PitchStart = m_Pitch;
    m_PitchEnd = clampedPitch;
    m_TweenDuration = clampedDuration;
    m_TweenTime = 0.0f;
    m_TargetDistance = clampedDistance;

    if (clampedDuration <= 0.0f) {
        m_Target = m_TargetEnd;
        m_Distance = clampedDistance;
        m_TargetDistance = clampedDistance;
        m_Yaw = normalizedYaw;
        m_Pitch = clampedPitch;
        m_IsTweening = false;
        return;
    }

    m_IsTweening = true;
}

void ViewportPanel::TweenAlignCameraToAxis(const glm::vec3& axisDirection, float durationSeconds) {
    const glm::vec3 normalizedAxis = glm::normalize(axisDirection);

    float targetYaw = 0.0f;
    float targetPitch = 0.0f;

    // Snap to camera positions on the major axes while keeping a stable screen orientation.
    if (std::abs(normalizedAxis.x) > 0.5f) {
        targetYaw = normalizedAxis.x > 0.0f ? 180.0f : 0.0f;
        targetPitch = 0.0f;
    } else if (std::abs(normalizedAxis.y) > 0.5f) {
        targetYaw = 90.0f;
        targetPitch = normalizedAxis.y > 0.0f ? -kCameraAxisAlignPitchLimit : kCameraAxisAlignPitchLimit;
    } else {
        targetYaw = normalizedAxis.z > 0.0f ? -90.0f : 90.0f;
        targetPitch = 0.0f;
    }

    TweenCameraToState(m_Target, m_Distance, targetYaw, targetPitch, durationSeconds);
}

void ViewportPanel::EndTransformUndoAction() {
    if (!m_TransformUndoActive) {
        return;
    }
    EditorSceneUndoStack::Get().EndScopedAction(m_Context);
    m_TransformUndoActive = false;
}

void ViewportPanel::EndEntityTransformUndoAction() {
    if (!m_EntityTransformUndoActive) {
        return;
    }
    EditorSceneUndoStack::Get().EndEntityTransformAction(m_Context);
    m_EntityTransformUndoActive = false;
}

void ViewportPanel::EndRadiusUndoAction() {
    if (!m_RadiusUndoActive) {
        return;
    }
    EditorSceneUndoStack::Get().EndScopedAction(m_Context);
    m_RadiusUndoActive = false;
}

void ViewportPanel::EndUIUndoAction() {
    if (!m_UIUndoActive) {
        return;
    }
    EditorSceneUndoStack::Get().EndScopedAction(m_Context);
    m_UIUndoActive = false;
}

bool ViewportPanel::SyncKeyboardTransformHotkeys() {
    const ImGuiIO& io = ImGui::GetIO();
    const bool hasCommandModifier = io.KeyCtrl || io.KeyAlt || io.KeySuper;

    auto readOperationShortcut = [](ImGuizmo::OPERATION& outOperation) -> bool {
        if (Input::WasKeyPressedThisFrame(KeyCode::G)) {
            outOperation = ImGuizmo::TRANSLATE;
            return true;
        }
        if (Input::WasKeyPressedThisFrame(KeyCode::R)) {
            outOperation = ImGuizmo::ROTATE;
            return true;
        }
        if (Input::WasKeyPressedThisFrame(KeyCode::S)) {
            outOperation = ImGuizmo::SCALE;
            return true;
        }
        return false;
    };

    auto readAxisShortcut = [](KeyboardTransformAxis& outAxis) -> bool {
        if (Input::WasKeyPressedThisFrame(KeyCode::X)) {
            outAxis = KeyboardTransformAxis::X;
            return true;
        }
        if (Input::WasKeyPressedThisFrame(KeyCode::Y)) {
            outAxis = KeyboardTransformAxis::Y;
            return true;
        }
        if (Input::WasKeyPressedThisFrame(KeyCode::Z)) {
            outAxis = KeyboardTransformAxis::Z;
            return true;
        }
        return false;
    };

    if (m_KeyboardTransformSession.Active && !CanContinueKeyboardTransformSession()) {
        EndKeyboardTransformSession();
    }

    ImGuizmo::OPERATION requestedOperation = ImGuizmo::TRANSLATE;
    KeyboardTransformAxis requestedAxis = KeyboardTransformAxis::None;
    const bool operationPressed = !hasCommandModifier && readOperationShortcut(requestedOperation);
    const bool axisPressed = !hasCommandModifier && readAxisShortcut(requestedAxis);

    if (m_KeyboardTransformSession.Active) {
        m_CurrentMode = ImGuizmo::WORLD;
        m_CurrentOperation = m_KeyboardTransformSession.Operation;
        BeginKeyboardTransformMouseCapture();
        if (operationPressed) {
            RestoreKeyboardTransformBaseline();
            ImGuizmo::EndKeyboardManipulation();
            m_CurrentMode = ImGuizmo::WORLD;
            m_CurrentOperation = requestedOperation;
            m_KeyboardTransformSession.Entity = *m_SelectedEntity;
            m_KeyboardTransformSession.Operation = requestedOperation;
            m_KeyboardTransformSession.Axis = KeyboardTransformAxis::None;
            return true;
        }

        if (axisPressed && m_KeyboardTransformSession.Axis != requestedAxis) {
            RestoreKeyboardTransformBaseline();
            ImGuizmo::EndKeyboardManipulation();
            m_CurrentMode = ImGuizmo::WORLD;
            m_KeyboardTransformSession.Axis = requestedAxis;
            return true;
        }

        return false;
    }

    if (!operationPressed || !CanStartKeyboardTransformSession()) {
        return false;
    }

    m_KeyboardTransformSession.Active = true;
    m_KeyboardTransformSession.Entity = *m_SelectedEntity;
    m_KeyboardTransformSession.Operation = requestedOperation;
    m_KeyboardTransformSession.Axis = KeyboardTransformAxis::None;
    m_KeyboardTransformSession.PreviousOperation = m_CurrentOperation;
    m_KeyboardTransformSession.PreviousMode = m_CurrentMode;
    if (EntityData* data = m_Context ? m_Context->GetEntityData(*m_SelectedEntity) : nullptr) {
        m_KeyboardTransformSession.BaselineTransform = data->Transform;
    }
    BeginKeyboardTransformMouseCapture();
    m_CurrentMode = ImGuizmo::WORLD;
    m_CurrentOperation = requestedOperation;
    return true;
}

// =============================================================
// CAMERA CONTROL (Orbit + Zoom) - Enhanced with adaptive zoom and smooth damping
// =============================================================
void ViewportPanel::HandleCameraControls() {
    Camera* cam = GetViewportCamera();
    if (!cam || m_ViewportSize.x <= 0.0f || m_ViewportSize.y <= 0.0f) return;

    const EditorSettings& settings = EditorSettings::Get();
    float deltaTime = (float)Time::GetDeltaTime();

    const bool isPlaying = Application::Get().IsPlaying() || (m_Context && m_Context->m_IsPlaying);

    // Shortcut: Frame selected (Editor-only)
    // Allow F key when viewport is focused OR hovered, so user can select in hierarchy then press F
    if (!m_KeyboardTransformSession.Active &&
        !m_SuppressMouseActionsThisFrame &&
        !isPlaying &&
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows))) {
        // GLFW F key = 70
        if (Input::WasKeyPressedThisFrame(70) && m_SelectedEntity && *m_SelectedEntity != -1) {
            FrameSelected(settings.FocusDuration);
        }
    }

    ImGuiIO& io = ImGui::GetIO();

    if (IsGizmoOver() || IsGizmoUsing()) {
        // Disable mouse/keyboard capture when using gizmo
        io.WantCaptureMouse = true;
        io.WantCaptureKeyboard = true;
        ImGuizmo::Enable(true);
    }

    bool terrainBrushActive = false;
    bool navLinkPaintActive = false;
    bool splineDrawActive = false;
    bool softbodyPaintActive = false;
    bool riverDrawActive = false;
    GatherEditorToolState(
        terrainBrushActive,
        navLinkPaintActive,
        splineDrawActive,
        softbodyPaintActive,
        riverDrawActive);

    if (m_KeyboardTransformSession.Active) {
        io.WantCaptureMouse = true;
        io.WantCaptureKeyboard = true;
    }

    if (m_SuppressMouseActionsThisFrame) {
        io.WantCaptureMouse = true;
        io.WantCaptureKeyboard = true;
        return;
    }

    // Ctrl+LeftClick: Create Empty entity at world position (editor only, never at runtime)
    if (!isPlaying && m_Context && m_SelectedEntity && IsViewportHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::GetIO().KeyCtrl &&
        !terrainBrushActive && !navLinkPaintActive && !splineDrawActive && !softbodyPaintActive && !riverDrawActive &&
        !m_IsDraggingRadius &&
        !IsGizmoOver() && !IsGizmoUsing() && !m_UIHandleHovered && !m_UIHandleActive && cam) {
        ImVec2 mousePos = ImGui::GetMousePos();
        float nx = (mousePos.x - m_ViewportPos.x) / m_ViewportSize.x;
        float ny = (mousePos.y - m_ViewportPos.y) / m_ViewportSize.y;
        nx = glm::clamp(nx, 0.0f, 1.0f);
        ny = glm::clamp(ny, 0.0f, 1.0f);
        Ray ray = Picking::ScreenPointToRay(nx, ny, cam);
        glm::vec3 hitPos;
        if (RaycastToWorldPosition(ray, hitPos)) {
            Entity e = m_Context->CreateEntity("Empty Entity");
            if (auto* d = m_Context->GetEntityData(e.GetID())) {
                d->Transform.Position = hitPos;
            }
            *m_SelectedEntity = e.GetID();
        }
    }
    else
    // Handle Picking
    if (!isPlaying && !terrainBrushActive && !navLinkPaintActive && !splineDrawActive && !softbodyPaintActive && !riverDrawActive &&
        IsViewportHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !m_IsDraggingRadius &&
        !IsGizmoOver() && !IsGizmoUsing() && !m_UIHandleHovered && !m_UIHandleActive) {
        ImVec2 mousePos = ImGui::GetMousePos();
        // Convert to normalized coordinates inside the letterboxed image
        float nx = (mousePos.x - m_ViewportPos.x) / m_ViewportSize.x;
        float ny = (mousePos.y - m_ViewportPos.y) / m_ViewportSize.y;

        nx = glm::clamp(nx, 0.0f, 1.0f);
        ny = glm::clamp(ny, 0.0f, 1.0f);

        const bool preferRootSelection = ImGui::GetIO().KeyShift;
        if (m_UseQueuedPicking) {
            Picking::QueuePick(nx, ny, preferRootSelection);
        } else if (m_Context && m_SelectedEntity && cam) {
            int pickedEntity = Picking::PickEntity(nx, ny, *m_Context, cam);
            if (pickedEntity != -1) {
                *m_SelectedEntity = Picking::ResolveSelectionEntity(pickedEntity, *m_Context, preferRootSelection);
            } else if (EditorSettings::Get().DeselectOnEmptyClick) {
                *m_SelectedEntity = -1;
            }
        }
    }

    // Update hover entity tracking for potential hover outline
    // Throttled to reduce CPU cost - picking involves ray-AABB/mesh intersection
    if (!isPlaying && settings.ShowHoverOutline && IsViewportHovered() &&
        !m_IsDraggingRadius &&
        !IsGizmoOver() && !IsGizmoUsing() &&
        !terrainBrushActive && !navLinkPaintActive && !splineDrawActive && !softbodyPaintActive && !riverDrawActive) {
        if (m_UseQueuedPicking) {
            m_HoveredEntity = Picking::GetLastHoverPick();
        }
        float currentTime = Time::GetTotalTime();
        if (currentTime - m_LastHoverPickTime >= kHoverPickInterval) {
            m_LastHoverPickTime = currentTime;
            ImVec2 mousePos = ImGui::GetMousePos();
            float nx = (mousePos.x - m_ViewportPos.x) / m_ViewportSize.x;
            float ny = (mousePos.y - m_ViewportPos.y) / m_ViewportSize.y;
            if (nx >= 0.0f && nx <= 1.0f && ny >= 0.0f && ny <= 1.0f) {
                if (m_UseQueuedPicking) {
                    Picking::QueueHoverPick(nx, ny);
                } else {
                    m_HoveredEntity = Picking::PickEntity(nx, ny, *m_Context, cam);
                }
            } else {
                if (m_UseQueuedPicking) {
                    Picking::ClearLastHoverPick();
                }
                m_HoveredEntity = INVALID_ENTITY_ID;
            }
        }
        // Keep current hover entity between picks for smooth feel
    } else {
        if (m_UseQueuedPicking) {
            Picking::ClearLastHoverPick();
        }
        m_HoveredEntity = INVALID_ENTITY_ID;
    }

    // Report UI mouse position (in framebuffer pixel coords) to renderer every frame
    if (m_ReportUIMouse) {
        ImVec2 mouse = ImGui::GetMousePos();
        float mx = (mouse.x - m_ViewportPos.x);
        float my = (mouse.y - m_ViewportPos.y);
        bool inside = (mx >= 0 && my >= 0 && mx <= m_ViewportSize.x && my <= m_ViewportSize.y);
        if (inside) {
            float renderW = (m_RenderTargetWidth > 0) ? (float)m_RenderTargetWidth : (float)Renderer::Get().GetWidth();
            float renderH = (m_RenderTargetHeight > 0) ? (float)m_RenderTargetHeight : (float)Renderer::Get().GetHeight();
            float fbX = mx * (renderW / m_ViewportSize.x);
            float fbY = my * (renderH / m_ViewportSize.y);
            float normX = (m_ViewportSize.x > 0.0f) ? (mx / m_ViewportSize.x) : 0.0f;
            float normY = (m_ViewportSize.y > 0.0f) ? (my / m_ViewportSize.y) : 0.0f;
            Renderer::Get().SetUIMousePosition(fbX, fbY, normX, normY, true);
        } else {
            Renderer::Get().SetUIMousePosition(0, 0, 0, 0, false);
        }
    }

    // Skip editor camera controls (orbit, pan, zoom) during play mode - the game controls the camera
    if (isPlaying) {
        return;
    }

    if (!m_KeyboardTransformSession.Active &&
        !Input::IsBlocked() &&
        !io.WantTextInput &&
        !ImGui::IsAnyItemActive() &&
        !m_IsDraggingAsset &&
        !m_IsDraggingRadius &&
        !m_UIHandleHovered &&
        !m_UIHandleActive &&
        !IsGizmoOver() &&
        !IsGizmoUsing() &&
        !terrainBrushActive &&
        !navLinkPaintActive &&
        !splineDrawActive &&
        !softbodyPaintActive &&
        !riverDrawActive &&
        (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || IsViewportHovered())) {
        const bool hasUnsupportedModifier = io.KeyAlt || io.KeySuper;
        const bool flipDirection = io.KeyCtrl;
        glm::vec3 axisDirection(0.0f);
        bool shouldAlign = false;

        auto tryAxisShortcut = [&](KeyCode mainKey, KeyCode keypadKey, const glm::vec3& axis) -> bool {
            if (Input::WasKeyPressedThisFrame(mainKey) || Input::WasKeyPressedThisFrame(keypadKey)) {
                axisDirection = flipDirection ? -axis : axis;
                shouldAlign = true;
                return true;
            }
            return false;
        };

        if (!hasUnsupportedModifier) {
            tryAxisShortcut(KeyCode::Digit1, KeyCode::KP_1, glm::vec3(1.0f, 0.0f, 0.0f)) ||
            tryAxisShortcut(KeyCode::Digit2, KeyCode::KP_2, glm::vec3(0.0f, 1.0f, 0.0f)) ||
            tryAxisShortcut(KeyCode::Digit3, KeyCode::KP_3, glm::vec3(0.0f, 0.0f, 1.0f));
        }

        if (shouldAlign) {
            io.WantCaptureKeyboard = true;
            TweenAlignCameraToAxis(axisDirection, std::min(settings.FocusDuration, kCameraAxisAlignDefaultDuration));
        }
    }

    // Camera controls (editor mode only)
    // Orbit with right-mouse drag
    if (IsViewportHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Right) &&
        !m_IsDraggingRadius &&
        !IsGizmoOver() && !IsGizmoUsing()) {
        io.WantCaptureMouse = false;
        io.WantCaptureKeyboard = false;

        // If mouse is captured (relative mode), use engine input deltas; otherwise use ImGui's
        float dx = 0.0f, dy = 0.0f;
        if (Input::IsRelativeMode()) {
            auto d = Input::GetMouseDelta();
            dx = d.first; dy = d.second;
        } else {
            dx = io.MouseDelta.x; dy = io.MouseDelta.y;
        }
        m_Yaw += dx * settings.OrbitSensitivity;
        m_Pitch -= dy * settings.OrbitSensitivity;
        m_Pitch = glm::clamp(m_Pitch, -89.0f, 89.0f);
    }

    // Adaptive scroll-wheel zoom with distance-based scaling
    if (IsViewportHovered() && !m_IsDraggingRadius && !IsGizmoOver() && !IsGizmoUsing()) {
        float scroll = io.MouseWheel;
        if (std::abs(scroll) > 0.001f) {
            // Compute adaptive zoom speed based on:
            // 1. Current distance (zoom faster when far, slower when close)
            // 2. Scroll magnitude (fast scrolls = acceleration)
            float distanceFactor = std::max(0.1f, m_Distance * 0.1f);
            
            // Acceleration for fast scrolls: detect large/quick scrolls
            float scrollMagnitude = std::abs(scroll);
            float acceleration = 1.0f + (scrollMagnitude - 1.0f) * settings.ZoomAcceleration;
            acceleration = std::max(1.0f, acceleration);
            
            float zoomDelta = scroll * distanceFactor * settings.ZoomBaseSpeed * acceleration;
            
            if (settings.SmoothZoomEnabled) {
                // Add to target distance for smooth interpolation
                m_TargetDistance -= zoomDelta;
                m_TargetDistance = glm::clamp(m_TargetDistance, settings.ZoomMinDistance, settings.ZoomMaxDistance);
            } else {
                // Instant zoom
                m_Distance -= zoomDelta;
                m_Distance = glm::clamp(m_Distance, settings.ZoomMinDistance, settings.ZoomMaxDistance);
                m_TargetDistance = m_Distance;
            }
            
            m_LastScrollDelta = scroll;
        }
    }

    // Apply smooth zoom damping
    if (settings.SmoothZoomEnabled && !m_IsTweening) {
        float zoomDiff = m_TargetDistance - m_Distance;
        if (std::abs(zoomDiff) > 0.001f) {
            // Exponential decay towards target
            float smoothFactor = 1.0f - std::pow(settings.ZoomSmoothness, deltaTime * 60.0f);
            m_Distance += zoomDiff * smoothFactor;
            m_Distance = glm::clamp(m_Distance, settings.ZoomMinDistance, settings.ZoomMaxDistance);
        } else {
            m_Distance = m_TargetDistance;
        }
    }

    // Middle-mouse panning: translate target along camera right/up vectors
    if (IsViewportHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Middle) &&
        !m_IsDraggingRadius &&
        !IsGizmoOver() && !IsGizmoUsing()) {
        io.WantCaptureMouse = false;
        io.WantCaptureKeyboard = false;

        ImVec2 delta = io.MouseDelta;

        // Compute camera basis from current yaw/pitch
        glm::vec3 forward;
        forward.x = cos(glm::radians(m_Yaw)) * cos(glm::radians(m_Pitch));
        forward.y = sin(glm::radians(m_Pitch));
        forward.z = sin(glm::radians(m_Yaw)) * cos(glm::radians(m_Pitch));
        forward = glm::normalize(forward);

        const glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
        glm::vec3 right = -1.0f * glm::normalize(glm::cross(forward, worldUp));
        glm::vec3 up = glm::normalize(glm::cross(right, forward));

        // Scale pan with distance so it feels consistent at different zoom levels
        float panSpeed = std::max(0.001f, m_Distance * settings.PanSpeedFactor);

        // Drag right -> move camera right; drag up -> move camera up
        m_Target += (right * delta.x - up * delta.y) * panSpeed;
    }

    // Apply active tween towards target/distance/orientation (editor-only camera moves)
    if (m_IsTweening) {
        m_TweenTime += deltaTime;
        float t = (m_TweenDuration > 0.0f)
            ? glm::clamp(m_TweenTime / m_TweenDuration, 0.0f, 1.0f)
            : 1.0f;
        // Smoothstep easing for natural feel
        t = t * t * (3.0f - 2.0f * t);
        m_Target = glm::mix(m_TargetStart, m_TargetEnd, t);
        m_Distance = glm::mix(m_DistanceStart, m_DistanceEnd, t);
        m_Yaw = LerpAngleDegrees(m_YawStart, m_YawEnd, t);
        m_Pitch = glm::mix(m_PitchStart, m_PitchEnd, t);
        m_TargetDistance = m_Distance; // Keep smooth zoom in sync
        if (t >= 1.0f) m_IsTweening = false;
    }

    glm::vec3 dir;
    dir.x = cos(glm::radians(m_Yaw)) * cos(glm::radians(m_Pitch));
    dir.y = sin(glm::radians(m_Pitch));
    dir.z = sin(glm::radians(m_Yaw)) * cos(glm::radians(m_Pitch));
    dir = glm::normalize(dir);

    glm::vec3 camPos = m_Target - dir * m_Distance;
    cam->SetPosition(camPos);
    cam->LookAt(m_Target);
}

void ViewportPanel::FrameSelected(float durationSeconds)
{
    if (!m_Context || !m_SelectedEntity || *m_SelectedEntity == -1)
        return;

    auto* data = m_Context->GetEntityData(*m_SelectedEntity);
    if (!data)
        return;

    const EditorSettings& settings = EditorSettings::Get();

    // Compute world-space center and bounding radius for the selected entity
    glm::vec3 worldCenter = glm::vec3(data->Transform.WorldMatrix * glm::vec4(0, 0, 0, 1));
    float radius = 0.0f;
    bool hasBounds = false;

    // Try to get bounds from mesh component
    if (data->Mesh && data->Mesh->mesh) {
        glm::vec3 lmin = data->Mesh->mesh->BoundsMin;
        glm::vec3 lmax = data->Mesh->mesh->BoundsMax;
        
        // Validate bounds (some meshes may have invalid/empty bounds)
        if (lmax.x > lmin.x && lmax.y > lmin.y && lmax.z > lmin.z) {
            glm::vec3 corners[8] = {
                {lmin.x, lmin.y, lmin.z}, {lmax.x, lmin.y, lmin.z},
                {lmin.x, lmax.y, lmin.z}, {lmax.x, lmax.y, lmin.z},
                {lmin.x, lmin.y, lmax.z}, {lmax.x, lmin.y, lmax.z},
                {lmin.x, lmax.y, lmax.z}, {lmax.x, lmax.y, lmax.z}
            };
            glm::vec3 wmin(FLT_MAX), wmax(-FLT_MAX);
            for (int i = 0; i < 8; i++) {
                glm::vec3 wp = glm::vec3(data->Transform.WorldMatrix * glm::vec4(corners[i], 1.0f));
                wmin = glm::min(wmin, wp);
                wmax = glm::max(wmax, wp);
            }
            worldCenter = (wmin + wmax) * 0.5f;
            glm::vec3 extents = (wmax - wmin) * 0.5f;
            radius = glm::length(extents);
            hasBounds = true;
        }
    }

    // Try collider bounds as fallback
    if (!hasBounds && data->Collider) {
        glm::vec3 colliderExtent(0.0f);
        switch (data->Collider->ShapeType) {
            case ColliderShape::Box:
                colliderExtent = data->Collider->Size * 0.5f;
                break;
            case ColliderShape::Sphere:
                colliderExtent = glm::vec3(data->Collider->Radius);
                break;
            case ColliderShape::Capsule:
                colliderExtent = glm::vec3(data->Collider->Radius, data->Collider->Height * 0.5f, data->Collider->Radius);
                break;
            default:
                break;
        }
        if (glm::length(colliderExtent) > 0.001f) {
            // Apply entity scale to collider extent
            glm::vec3 scale = data->Transform.Scale;
            colliderExtent *= scale;
            radius = glm::length(colliderExtent);
            hasBounds = true;
        }
    }

    // Try terrain bounds
    if (!hasBounds && data->Terrain) {
        float terrainSize = std::max(data->Terrain->WorldSize.x, data->Terrain->WorldSize.y) * 0.5f;
        if (terrainSize > 0.0f) {
            radius = terrainSize;
            hasBounds = true;
        }
    }

    // Use default distance if no bounds found
    if (!hasBounds || radius < 0.001f) {
        radius = settings.FocusDefaultDistance * 0.5f;
    }

    // Ensure minimum radius to prevent extreme zoom
    radius = std::max(radius, 0.01f);

    // Compute distance based on camera FOV to properly frame the object
    Camera* cam = GetViewportCamera();
    float fovDeg = cam ? cam->GetFieldOfView() : 60.0f;
    float fovRad = glm::radians(fovDeg);
    
    // Use vertical FOV to compute distance that frames the bounding sphere
    float dist = radius / std::tan(fovRad * 0.5f);
    
    // Apply padding multiplier from settings
    dist *= settings.FocusDistancePadding;
    
    // Clamp to reasonable range
    dist = glm::clamp(dist, settings.ZoomMinDistance, settings.ZoomMaxDistance);

    TweenCameraToState(worldCenter, dist, m_Yaw, m_Pitch, durationSeconds);
}
// =============================
// Picking
// =============================
void ViewportPanel::HandleEntityPicking() {
    m_ShouldPick = true;

    ImVec2 mousePos = ImGui::GetMousePos();
    ImVec2 windowPos = ImGui::GetWindowPos();

    float localX = mousePos.x - windowPos.x;
    float localY = mousePos.y - windowPos.y;

    // Normalize coordinates [0..1]
    if (m_ViewportSize.x > 0 && m_ViewportSize.y > 0) {
        m_NormalizedPickX = localX / m_ViewportSize.x;
        m_NormalizedPickY = localY / m_ViewportSize.y;
    }
}


// =============================
// Draw Overlay Grid
// =============================
void ViewportPanel::Draw2DGrid() {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 winPos = ImGui::GetWindowPos();
    ImVec2 size = m_ViewportSize;

    const float spacing = 32.0f;
    const ImU32 color = IM_COL32(80, 80, 80, 80);

    for (float x = winPos.x; x < winPos.x + size.x; x += spacing)
        drawList->AddLine(ImVec2(x, winPos.y), ImVec2(x, winPos.y + size.y), color);
    for (float y = winPos.y; y < winPos.y + size.y; y += spacing)
        drawList->AddLine(ImVec2(winPos.x, y), ImVec2(winPos.x + size.x, y), color);
}

// =============================
// Drag-Drop Handling
// =============================
void ViewportPanel::HandleAssetDragDrop(const ImVec2& viewportPos) {
    if (!m_Context) return;
    (void)viewportPos;
    bool wasDragging = m_IsDraggingAsset;
    bool payloadAccepted = false;
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
            m_IsDraggingAsset = true;
            m_DraggedAssetPath = (const char*)payload->Data;
            m_CanPreviewDraggedAsset = SupportsAssetPreview(m_DraggedAssetPath);
            payloadAccepted = true;

            ImVec2 mousePos = ImGui::GetMousePos();
            UpdateGhostPosition(mousePos.x, mousePos.y);
            if (m_CanPreviewDraggedAsset) {
                EnsureAssetPreviewEntity();
            } else {
                CancelAssetPreview(false);
            }

            // Tooltip
            ImGui::BeginTooltip();
            ImGui::Text("Placing: %s", m_DraggedAssetPath.c_str());
            ImGui::EndTooltip();
        }
        if (ImGui::IsDragDropPayloadBeingAccepted()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        ImGui::EndDragDropTarget();
    }
    else {
        m_IsDraggingAsset = false;
    }

    if (m_IsDraggingAsset && !payloadAccepted) {
        ImVec2 mousePos = ImGui::GetMousePos();
        UpdateGhostPosition(mousePos.x, mousePos.y);
        UpdateAssetPreviewTransform();
    }

    if (!m_IsDraggingAsset && (wasDragging || m_GhostEntityId != INVALID_ENTITY_ID)) {
        CancelAssetPreview(false);
        m_DraggedAssetPath.clear();
        m_CanPreviewDraggedAsset = false;
    }
}

// =============================
// Update Ghost Position in World
// =============================
void ViewportPanel::UpdateGhostPosition(float mouseX, float mouseY) {
    // Convert screen coords to normalized within letterboxed viewport
    float nx = (mouseX - m_ViewportPos.x) / m_ViewportSize.x;
    float ny = (mouseY - m_ViewportPos.y) / m_ViewportSize.y;
    Camera* cam = m_UseInternalCamera ? m_Camera.get() : Renderer::Get().GetEditorCamera();
    if (!cam) cam = Renderer::Get().GetCamera();
    Ray ray = Picking::ScreenPointToRay(nx, ny, cam);

    // Priority 1: Physics raycast against scene colliders
    // This provides Unity-like projection onto any surface with a collider
    Physics::RaycastHit physicsHit;
    if (Physics::Raycast(ray.Origin, ray.Direction, 1000.0f, physicsHit)) {
        // Skip if we hit the ghost preview entity itself
        if (static_cast<EntityID>(physicsHit.entityId) != m_GhostEntityId) {
            glm::vec3 hitPos = physicsHit.point;
            // Snap to grid
            hitPos.x = round(hitPos.x / m_GridSize) * m_GridSize;
            hitPos.z = round(hitPos.z / m_GridSize) * m_GridSize;
            m_GhostPosition = hitPos;
            return;
        }
    }

    // Priority 2: Terrain raycast (for terrains that may not have physics colliders)
    glm::vec3 terrainHit;
    if (ProjectGhostOntoTerrain(ray, terrainHit)) {
        terrainHit.x = round(terrainHit.x / m_GridSize) * m_GridSize;
        terrainHit.z = round(terrainHit.z / m_GridSize) * m_GridSize;
        m_GhostPosition = terrainHit;
        return;
    }

    // Priority 3: Scene mesh intersection (for visual meshes without colliders)
    if (m_Context) {
        float closestWorldDist = std::numeric_limits<float>::max();
        bool anyMeshHit = false;
        glm::vec3 meshHitPos;
        
        for (auto& entity : m_Context->GetEntities()) {
            EntityID id = entity.GetID();
            // Skip the ghost preview entity
            if (id == m_GhostEntityId) continue;
            
            auto* data = m_Context->GetEntityData(id);
            if (!data || !data->Visible || !data->Active) continue;
            if (!data->Mesh || !data->Mesh->mesh) continue;
            
            float hitT = 0.0f;
            if (Picking::RayIntersectsMesh(ray, *data->Mesh->mesh, data->Transform.WorldMatrix, hitT)) {
                if (hitT > 0.0f) {
                    glm::vec3 worldHit = ray.Origin + ray.Direction * hitT;
                    if (hitT < closestWorldDist) {
                        closestWorldDist = hitT;
                        meshHitPos = worldHit;
                        anyMeshHit = true;
                    }
                }
            }
        }
        
        if (anyMeshHit) {
            meshHitPos.x = round(meshHitPos.x / m_GridSize) * m_GridSize;
            meshHitPos.z = round(meshHitPos.z / m_GridSize) * m_GridSize;
            m_GhostPosition = meshHitPos;
            return;
        }
    }

    // Priority 4: Fallback to ground plane (y=0)
    if (fabs(ray.Direction.y) > 1e-6f) {
        float t = -ray.Origin.y / ray.Direction.y;
        if (t > 0.0f && t < 1000.0f) {  // Reasonable distance limit
            glm::vec3 hit = ray.Origin + ray.Direction * t;

            // Snap to grid
            hit.x = round(hit.x / m_GridSize) * m_GridSize;
            hit.z = round(hit.z / m_GridSize) * m_GridSize;
            m_GhostPosition = hit;
            return;
        }
    }
    
    // Priority 5: Ultimate fallback - project a fixed distance along ray
    // This ensures we always have a valid drop position even with unusual camera angles
    {
        float defaultDistance = 10.0f;  // Default placement distance from camera
        glm::vec3 hit = ray.Origin + ray.Direction * defaultDistance;
        
        // Snap to grid
        hit.x = round(hit.x / m_GridSize) * m_GridSize;
        hit.z = round(hit.z / m_GridSize) * m_GridSize;
        // Keep Y at grid level if looking up or in weird direction
        if (hit.y < 0.0f || hit.y > 100.0f) {
            hit.y = 0.0f;
        }
        m_GhostPosition = hit;
    }
}

bool ViewportPanel::RaycastToWorldPosition(const Ray& ray, glm::vec3& outPosition) const {
    if (!m_Context) return false;
    Physics::RaycastHit physicsHit;
    if (Physics::Raycast(ray.Origin, ray.Direction, 1000.0f, physicsHit)) {
        outPosition = physicsHit.point;
        return true;
    }
    if (ProjectGhostOntoTerrain(ray, outPosition)) return true;
    float closestWorldDist = std::numeric_limits<float>::max();
    bool anyMeshHit = false;
    glm::vec3 meshHitPos;
    for (auto& entity : m_Context->GetEntities()) {
        auto* data = m_Context->GetEntityData(entity.GetID());
        if (!data || !data->Visible || !data->Active || !data->Mesh || !data->Mesh->mesh) continue;
        float hitT = 0.0f;
        if (Picking::RayIntersectsMesh(ray, *data->Mesh->mesh, data->Transform.WorldMatrix, hitT) && hitT > 0.0f) {
            glm::vec3 worldHit = ray.Origin + ray.Direction * hitT;
            if (hitT < closestWorldDist) {
                closestWorldDist = hitT;
                meshHitPos = worldHit;
                anyMeshHit = true;
            }
        }
    }
    if (anyMeshHit) {
        outPosition = meshHitPos;
        return true;
    }
    if (std::abs(ray.Direction.y) > 1e-6f) {
        float t = -ray.Origin.y / ray.Direction.y;
        if (t > 0.0f && t < 1000.0f) {
            outPosition = ray.Origin + ray.Direction * t;
            return true;
        }
    }
    return false;
}

bool ViewportPanel::ProjectGhostOntoTerrain(const Ray& ray, glm::vec3& outPosition) const {
    if (!m_Context) return false;
    float closest = std::numeric_limits<float>::max();
    bool hit = false;
    for (const auto& entity : m_Context->GetEntities()) {
        EntityID id = entity.GetID();
        auto* data = m_Context->GetEntityData(id);
        if (!data || !data->Terrain) continue;
        glm::vec3 worldPos;
        if (Terrain::Raycast(data->Transform, *data->Terrain, ray.Origin, ray.Direction, &worldPos, nullptr, nullptr, nullptr)) {
            float dist = glm::length(worldPos - ray.Origin);
            if (dist < closest) {
                closest = dist;
                outPosition = worldPos;
                hit = true;
            }
        }
    }
    return hit;
}

// =============================
// Draw Ghost Preview (Optional UI)
// =============================
void ViewportPanel::DrawGhostPreview() {
    if (m_GhostEntityId != INVALID_ENTITY_ID) {
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 mp = ImGui::GetMousePos();
    dl->AddCircleFilled(mp, 8.0f, IM_COL32(200, 200, 200, 120));
}

// =============================
// ImGuizmo for Transform
// =============================
void ViewportPanel::DrawGizmo() {
    if (!m_GizmoOwnerThisFrame) {
        EndTransformUndoAction();
        EndEntityTransformUndoAction();
        if (m_KeyboardTransformSession.Active) {
            EndKeyboardTransformSession();
        }
        m_GizmoOverThisFrame = false;
        m_GizmoUsingThisFrame = false;
        return;
    }
    // Always reset ImGuizmo to enabled state at the start of each frame
    // This prevents Enable(false) from persisting across selection changes
    ImGuizmo::Enable(true);
    
    if (*m_SelectedEntity < 0 || !m_Context || !m_ShowGizmos) {
        EndTransformUndoAction();
        EndEntityTransformUndoAction();
        if (m_KeyboardTransformSession.Active) {
            EndKeyboardTransformSession();
        }
        m_GizmoOverThisFrame = false;
        m_GizmoUsingThisFrame = false;
        return;
    }

    // Do not allow gizmo in play mode
    const bool isPlaying = Application::Get().IsPlaying() || m_Context->m_IsPlaying;
    if (isPlaying) {
        EndTransformUndoAction();
        EndEntityTransformUndoAction();
        if (m_KeyboardTransformSession.Active) {
            EndKeyboardTransformSession();
        }
        ImGuizmo::Enable(false);
        m_GizmoOverThisFrame = false;
        m_GizmoUsingThisFrame = false;
        return;
    }

    // Disable transform gizmo when dragging radius gizmo
    if (m_IsDraggingRadius) {
        EndTransformUndoAction();
        EndEntityTransformUndoAction();
        if (m_KeyboardTransformSession.Active) {
            EndKeyboardTransformSession();
        }
        ImGuizmo::Enable(false);
        m_GizmoOverThisFrame = false;
        m_GizmoUsingThisFrame = false;
        return;
    }

    auto* data = m_Context->GetEntityData(*m_SelectedEntity);
    if (!data) {
        EndTransformUndoAction();
        EndEntityTransformUndoAction();
        if (m_KeyboardTransformSession.Active) {
            EndKeyboardTransformSession();
        }
        m_GizmoOverThisFrame = false;
        m_GizmoUsingThisFrame = false;
        return;
    }

    // Check if we should show radius gizmo instead (will be handled in DrawRadiusGizmo)
    bool hasSphereCollider = (data->Collider && data->Collider->ShapeType == ColliderShape::Sphere);
    bool hasSphereMesh = (data->Mesh && data->Mesh->MeshName == "Sphere");
    
    // If we have sphere collider/mesh and scale operation is selected, prefer radius gizmo
    if ((hasSphereCollider || hasSphereMesh) && m_CurrentOperation == ImGuizmo::SCALE) {
        // Still draw transform gizmo but radius gizmo will handle interaction
    }

    // If selected is a UI element under a screen-space canvas, skip 3D gizmo
    auto* ed = m_Context->GetEntityData(*m_SelectedEntity);
    if (ed) {
        const bool isUI = (
            ed->Canvas ||
            ed->Panel ||
            ed->Button ||
            ed->Slider ||
            ed->ProgressBar ||
            ed->Toggle ||
            ed->ScrollView ||
            ed->LayoutGroup ||
            ed->InputField ||
            ed->Dropdown ||
            ed->UIRect ||
            ed->FitToContent ||
            ed->UISceneCapture ||
            (ed->Text && !ed->Text->WorldSpace));
        if (isUI) {
            // climb to check if under screen-space canvas
            EntityID cur = *m_SelectedEntity;
            while (cur != INVALID_ENTITY_ID) {
                auto* d2 = m_Context->GetEntityData(cur);
                if (!d2) break;
                if (d2->Canvas && d2->Canvas->Space == CanvasComponent::RenderSpace::ScreenSpace) {
                    EndTransformUndoAction();
                    EndEntityTransformUndoAction();
                    if (m_KeyboardTransformSession.Active) {
                        EndKeyboardTransformSession();
                    }
                    ImGuizmo::Enable(false);
                    m_GizmoOverThisFrame = false;
                    m_GizmoUsingThisFrame = false;
                    return;
                }
                cur = d2->Parent;
            }
        }
    }

    const bool restartKeyboardManipulation = SyncKeyboardTransformHotkeys();
    if (m_KeyboardTransformSession.Active) {
        if (Input::WasKeyPressedThisFrame(KeyCode::Escape) ||
            Input::WasMouseButtonPressedThisFrame(MouseButton::Right)) {
            RestoreKeyboardTransformBaseline();
            EndTransformUndoAction();
            EndEntityTransformUndoAction();
            EndKeyboardTransformSession(true);
            return;
        }
        UpdateKeyboardTransformVirtualMouse();
    }

    ImGuizmo::SetOrthographic(false);

    // Always use editor camera for gizmo in edit mode
    // Entity cameras should only affect rendering during play mode
    Camera* cam = m_UseInternalCamera ? m_Camera.get() : Renderer::Get().GetEditorCamera();
    if (!cam) {
        // Fallback to renderer camera if editor camera not available
        cam = Renderer::Get().GetCamera();
    }
    if (!cam) {
        EndTransformUndoAction();
        EndEntityTransformUndoAction();
        if (m_KeyboardTransformSession.Active) {
            EndKeyboardTransformSession();
        }
        m_GizmoOverThisFrame = false;
        m_GizmoUsingThisFrame = false;
        return;
    }

    auto resolveWorldCanvasRenderDimension = [](int explicitSize, int referenceSize, uint32_t fallbackSize) -> uint32_t {
        if (explicitSize > 0)
            return static_cast<uint32_t>(explicitSize);
        if (referenceSize > 0)
            return static_cast<uint32_t>(referenceSize);
        return std::max<uint32_t>(fallbackSize, 1u);
    };

    auto buildCanvasBillboardMatrix = [](const glm::mat4& worldMatrix, const glm::mat4& viewMatrix) -> glm::mat4 {
        glm::vec3 position = glm::vec3(worldMatrix[3]);
        glm::vec3 scale(
            glm::max(glm::length(glm::vec3(worldMatrix[0])), 0.0001f),
            glm::max(glm::length(glm::vec3(worldMatrix[1])), 0.0001f),
            glm::max(glm::length(glm::vec3(worldMatrix[2])), 0.0001f));

        glm::mat4 cameraWorld = glm::inverse(viewMatrix);
        glm::mat3 cameraRotation(cameraWorld);

        glm::mat4 billboard(1.0f);
        billboard[0] = glm::vec4(cameraRotation[0] * scale.x, 0.0f);
        billboard[1] = glm::vec4(cameraRotation[1] * scale.y, 0.0f);
        billboard[2] = glm::vec4(cameraRotation[2] * scale.z, 0.0f);
        billboard[3] = glm::vec4(position, 1.0f);
        return billboard;
    };

    auto buildGizmoFrame = [](const glm::mat4& referenceMatrix, const glm::vec3& position) -> glm::mat4 {
        glm::mat4 gizmo(1.0f);
        for (int axis = 0; axis < 3; axis++) {
            glm::vec3 axisVector = glm::vec3(referenceMatrix[axis]);
            float axisLength = glm::length(axisVector);
            if (axisLength > 1e-5f)
                axisVector /= axisLength;
            else
                axisVector = glm::vec3(axis == 0, axis == 1, axis == 2);

            gizmo[axis] = glm::vec4(axisVector, 0.0f);
        }
        gizmo[3] = glm::vec4(position, 1.0f);
        return gizmo;
    };

    auto getCanvasScale = [](const CanvasComponent* canvas, float renderWidth, float renderHeight) -> glm::vec2 {
        if (!canvas || canvas->ReferenceWidth <= 0 || canvas->ReferenceHeight <= 0)
            return glm::vec2(1.0f, 1.0f);

        const float refW = static_cast<float>(canvas->ReferenceWidth);
        const float refH = static_cast<float>(canvas->ReferenceHeight);
        const float scaleX = renderWidth / refW;
        const float scaleY = renderHeight / refH;

        switch (canvas->ReferenceScaleMode) {
        case CanvasComponent::ScaleMode::ConstantPixelSize:
            return glm::vec2(1.0f, 1.0f);
        case CanvasComponent::ScaleMode::ScaleWithWidth:
            return glm::vec2(scaleX, scaleX);
        case CanvasComponent::ScaleMode::ScaleWithHeight:
            return glm::vec2(scaleY, scaleY);
        case CanvasComponent::ScaleMode::ScaleWithSmallest:
        {
            const float minScale = std::min(scaleX, scaleY);
            return glm::vec2(minScale, minScale);
        }
        case CanvasComponent::ScaleMode::ScaleWithLargest:
        {
            const float maxScale = std::max(scaleX, scaleY);
            return glm::vec2(maxScale, maxScale);
        }
        case CanvasComponent::ScaleMode::Expand:
            return glm::vec2(scaleX, scaleY);
        default:
            return glm::vec2(1.0f, 1.0f);
        }
    };

    auto hasResolvedUIRect = [](const UIRectComponent* rect) -> bool {
        return rect && !rect->_RectDirty;
    };

    auto resolveCanvasAnchorPoint = [](UIAnchorPreset anchor, float width, float height) -> glm::vec2 {
        switch (anchor) {
        case UIAnchorPreset::TopLeft:     return { 0.0f, 0.0f };
        case UIAnchorPreset::Top:         return { width * 0.5f, 0.0f };
        case UIAnchorPreset::TopRight:    return { width, 0.0f };
        case UIAnchorPreset::Left:        return { 0.0f, height * 0.5f };
        case UIAnchorPreset::Center:      return { width * 0.5f, height * 0.5f };
        case UIAnchorPreset::Right:       return { width, height * 0.5f };
        case UIAnchorPreset::BottomLeft:  return { 0.0f, height };
        case UIAnchorPreset::Bottom:      return { width * 0.5f, height };
        case UIAnchorPreset::BottomRight: return { width, height };
        default:                          return { 0.0f, 0.0f };
        }
    };

    auto isCanvasBackedUIEntity = [](const EntityData* entityData) -> bool {
        return entityData && (
            entityData->Canvas ||
            entityData->Panel ||
            entityData->Button ||
            entityData->Slider ||
            entityData->ProgressBar ||
            entityData->Toggle ||
            entityData->ScrollView ||
            entityData->LayoutGroup ||
            entityData->InputField ||
            entityData->Dropdown ||
            entityData->UIRect ||
            entityData->FitToContent ||
            entityData->UISceneCapture ||
            (entityData->Text && !entityData->Text->WorldSpace));
    };

    const EditorSettings& settings = EditorSettings::Get();

    auto setAdaptiveGizmoSize = [&](const glm::vec3& worldPosition) {
        if (settings.GizmoAutoScale) {
            glm::vec3 entityPos = worldPosition;
            glm::vec3 camPos = cam->GetPosition();
            float distToEntity = glm::length(entityPos - camPos);
            distToEntity = std::max(distToEntity, 0.0001f);

            float fov = glm::radians(cam->GetFieldOfView());
            float viewHeight = 2.0f * distToEntity * std::tan(fov * 0.5f);
            float screenFraction = (settings.GizmoMinScreenSize + settings.GizmoMaxScreenSize) * 0.5f / m_ViewportSize.y;
            float desiredWorldSize = viewHeight * screenFraction;
            float clipSize = glm::clamp(desiredWorldSize / distToEntity * 0.5f, 0.05f, 0.3f);

            ImGuizmo::SetGizmoSizeClipSpace(clipSize);
        } else {
            ImGuizmo::SetGizmoSizeClipSpace(0.1f * settings.GizmoBaseScale);
        }
    };

    const float* view = glm::value_ptr(cam->GetViewMatrix());
    const float* proj = glm::value_ptr(cam->GetProjectionMatrix());

    auto tryDrawWorldSpaceUIGizmo = [&]() -> bool {
        if (!ed || !isCanvasBackedUIEntity(ed))
            return false;

        EntityID canvasEntityId = INVALID_ENTITY_ID;
        EntityData* canvasData = nullptr;
        CanvasComponent* canvas = nullptr;
        EntityID current = *m_SelectedEntity;
        while (current != INVALID_ENTITY_ID) {
            auto* currentData = m_Context->GetEntityData(current);
            if (!currentData)
                break;

            if (currentData->Canvas) {
                if (currentData->Canvas->Space == CanvasComponent::RenderSpace::WorldSpace) {
                    canvasEntityId = current;
                    canvasData = currentData;
                    canvas = currentData->Canvas.get();
                }
                break;
            }

            current = currentData->Parent;
        }

        if (!canvas || !canvasData || canvasEntityId == *m_SelectedEntity)
            return false;

        auto suppressGizmo = [&]() -> bool {
            m_GizmoOverThisFrame = false;
            m_GizmoUsingThisFrame = false;
            return true;
        };

        if (m_CurrentOperation != ImGuizmo::TRANSLATE) {
            if (m_KeyboardTransformSession.Active) {
                EndKeyboardTransformSession();
            }
            return suppressGizmo();
        }

        uint32_t fallbackWidth = m_RenderTargetWidth > 0 ? static_cast<uint32_t>(m_RenderTargetWidth) : static_cast<uint32_t>(Renderer::Get().GetWidth());
        uint32_t fallbackHeight = m_RenderTargetHeight > 0 ? static_cast<uint32_t>(m_RenderTargetHeight) : static_cast<uint32_t>(Renderer::Get().GetHeight());
        float renderWidth = static_cast<float>(resolveWorldCanvasRenderDimension(canvas->Width, canvas->ReferenceWidth, fallbackWidth));
        float renderHeight = static_cast<float>(resolveWorldCanvasRenderDimension(canvas->Height, canvas->ReferenceHeight, fallbackHeight));
        if (renderWidth <= 0.0f || renderHeight <= 0.0f)
            return suppressGizmo();

        const glm::vec2 canvasScale = getCanvasScale(canvas, renderWidth, renderHeight);
        const float safeScaleX = std::max(canvasScale.x, 1e-5f);
        const float safeScaleY = std::max(canvasScale.y, 1e-5f);
        const glm::vec4 canvasRect(0.0f, 0.0f, renderWidth, renderHeight);

        auto tryResolveEntityUIRect = [&](auto&& self, EntityID entityId, glm::vec4& outRect) -> bool {
            auto* entityData = m_Context->GetEntityData(entityId);
            if (!entityData)
                return false;

            if (hasResolvedUIRect(entityData->UIRect.get())) {
                outRect = entityData->UIRect->_ComputedRect;
                return true;
            }

            glm::vec4 parentRect = canvasRect;
            bool hasUIParent = false;
            for (EntityID parent = entityData->Parent; parent != INVALID_ENTITY_ID && parent != canvasEntityId;) {
                auto* parentData = m_Context->GetEntityData(parent);
                if (!parentData)
                    break;

                if (parentData->Panel || parentData->LayoutGroup) {
                    if (!self(self, parent, parentRect))
                        return false;
                    hasUIParent = true;
                    break;
                }

                parent = parentData->Parent;
            }

            if (entityData->Panel) {
                PanelComponent& panel = *entityData->Panel;
                float x = 0.0f;
                float y = 0.0f;
                const float width = panel.Size.x * panel.Scale.x * safeScaleX;
                const float height = panel.Size.y * panel.Scale.y * safeScaleY;

                if (entityData->UIRect && entityData->UIRect->AnchorToParent) {
                    UIRectComponent& rect = *entityData->UIRect;
                    const float anchorX = parentRect.x + parentRect.z * rect.HorizontalAnchor;
                    const float anchorY = parentRect.y + parentRect.w * rect.VerticalAnchor;
                    x = anchorX - width * rect.Pivot.x + rect.Offset.x * safeScaleX;
                    y = anchorY - height * rect.Pivot.y + rect.Offset.y * safeScaleY;
                } else if (panel.AnchorEnabled) {
                    const glm::vec4 anchorRect = panel.AnchorToParentUI && hasUIParent ? parentRect : canvasRect;
                    glm::vec2 anchorPoint = resolveCanvasAnchorPoint(panel.Anchor, anchorRect.z, anchorRect.w);
                    anchorPoint.x += anchorRect.x + panel.AnchorOffset.x * safeScaleX;
                    anchorPoint.y += anchorRect.y + panel.AnchorOffset.y * safeScaleY;
                    x = anchorPoint.x - width * panel.Pivot.x;
                    y = anchorPoint.y - height * panel.Pivot.y;
                } else {
                    x = (hasUIParent ? parentRect.x : 0.0f) + panel.Position.x * safeScaleX;
                    y = (hasUIParent ? parentRect.y : 0.0f) + panel.Position.y * safeScaleY;
                }

                outRect = glm::vec4(x, y, width, height);
                return true;
            }

            if (entityData->Text && !entityData->Text->WorldSpace) {
                TextRendererComponent& text = *entityData->Text;
                const float scaledPixelSize = text.PixelSize * safeScaleX;
                const float textAscent = scaledPixelSize * 0.8f;
                const float textDescent = scaledPixelSize * 0.2f;
                const float textHeight = textAscent + textDescent;
                const float textWidth = scaledPixelSize * text.Text.length() * 0.5f;

                const bool wantsParentAnchor = text.AnchorToParentUI ||
                    (entityData->UIRect && entityData->UIRect->AnchorToParent);
                if (wantsParentAnchor) {
                    UIRectComponent defaultRect;
                    defaultRect.AnchorToParent = true;
                    UIRectComponent& rect = entityData->UIRect ? *entityData->UIRect : defaultRect;
                    const glm::vec4 anchorRect = hasUIParent ? parentRect : canvasRect;
                    const float anchorX = anchorRect.x + anchorRect.z * rect.HorizontalAnchor;
                    const float anchorY = anchorRect.y + anchorRect.w * rect.VerticalAnchor;
                    const float boundsW = rect.Size.x > 0.0f ? rect.Size.x * safeScaleX : textWidth;
                    const float boundsH = rect.Size.y > 0.0f ? rect.Size.y * safeScaleY : textHeight;
                    float boxLeft = anchorX - boundsW * rect.Pivot.x
                        + rect.Offset.x * safeScaleX
                        + text.AnchorOffset.x * safeScaleX;
                    const float boxTop = anchorY - boundsH * rect.Pivot.y
                        + rect.Offset.y * safeScaleY
                        + text.AnchorOffset.y * safeScaleY;
                    if (textWidth < boundsW)
                        boxLeft += (boundsW - textWidth) * rect.Pivot.x;

                    const float rectTop = boxTop + (boundsH - textHeight) * 0.5f;
                    outRect = glm::vec4(boxLeft, rectTop, textWidth, textHeight);
                    return true;
                }

                if (text.AnchorEnabled) {
                    glm::vec2 anchorPoint = resolveCanvasAnchorPoint(text.Anchor, canvasRect.z, canvasRect.w);
                    anchorPoint.x += text.AnchorOffset.x * safeScaleX;
                    anchorPoint.y += text.AnchorOffset.y * safeScaleY;
                    outRect = glm::vec4(anchorPoint.x, anchorPoint.y - textAscent, textWidth, textHeight);
                    return true;
                }

                const float topX = (hasUIParent ? parentRect.x : 0.0f) + entityData->Transform.Position.x * safeScaleX;
                const float topY = (hasUIParent ? parentRect.y : 0.0f) + entityData->Transform.Position.y * safeScaleY;
                outRect = glm::vec4(topX, topY, textWidth, textHeight);
                return true;
            }

            if (entityData->LayoutGroup || entityData->UIRect) {
                float x = 0.0f;
                float y = 0.0f;
                float width = 0.0f;
                float height = 0.0f;

                if (entityData->UIRect && entityData->UIRect->AnchorToParent) {
                    UIRectComponent& rect = *entityData->UIRect;
                    const float anchorX = parentRect.x + parentRect.z * rect.HorizontalAnchor;
                    const float anchorY = parentRect.y + parentRect.w * rect.VerticalAnchor;
                    width = rect.Size.x * safeScaleX;
                    height = rect.Size.y * safeScaleY;
                    x = anchorX - width * rect.Pivot.x + rect.Offset.x * safeScaleX;
                    y = anchorY - height * rect.Pivot.y + rect.Offset.y * safeScaleY;
                } else {
                    x = (hasUIParent ? parentRect.x : 0.0f) + entityData->Transform.Position.x * safeScaleX;
                    y = (hasUIParent ? parentRect.y : 0.0f) + entityData->Transform.Position.y * safeScaleY;
                    if (entityData->UIRect) {
                        width = entityData->UIRect->Size.x * safeScaleX;
                        height = entityData->UIRect->Size.y * safeScaleY;
                    }
                }

                outRect = glm::vec4(x, y, width, height);
                return true;
            }

            return false;
        };

        enum class UIPositionTarget {
            None,
            PanelPosition,
            PanelAnchorOffset,
            TextAnchorOffset,
            TransformPosition,
            UIRectOffset
        };

        UIPositionTarget positionTarget = UIPositionTarget::None;
        if (ed->UIRect && ed->UIRect->AnchorToParent) {
            positionTarget = UIPositionTarget::UIRectOffset;
        } else if (ed->Panel) {
            positionTarget = ed->Panel->AnchorEnabled
                ? UIPositionTarget::PanelAnchorOffset
                : UIPositionTarget::PanelPosition;
        } else if (ed->Text && !ed->Text->WorldSpace) {
            positionTarget = ed->Text->AnchorEnabled
                ? UIPositionTarget::TextAnchorOffset
                : UIPositionTarget::TransformPosition;
        } else if (ed->UIRect) {
            positionTarget = ed->UIRect->AnchorToParent
                ? UIPositionTarget::UIRectOffset
                : UIPositionTarget::TransformPosition;
        }

        if (positionTarget == UIPositionTarget::None)
            return suppressGizmo();

        glm::vec4 resolvedRect(0.0f);
        if (!tryResolveEntityUIRect(tryResolveEntityUIRect, *m_SelectedEntity, resolvedRect))
            return suppressGizmo();

        glm::vec2 scrollOffset(0.0f, 0.0f);
        for (EntityID ancestor = ed->Parent; ancestor != INVALID_ENTITY_ID && ancestor != canvasEntityId;) {
            auto* ancestorData = m_Context->GetEntityData(ancestor);
            if (!ancestorData)
                break;

            if (ancestorData->ScrollView && ancestorData->ScrollView->Visible)
                scrollOffset += ancestorData->ScrollView->ContentOffset;

            ancestor = ancestorData->Parent;
        }
        resolvedRect.x -= scrollOffset.x;
        resolvedRect.y -= scrollOffset.y;

        float logicalX = resolvedRect.x;
        float logicalY = resolvedRect.y;
        bool updatesTransform = false;

        glm::mat4 canvasModel = canvas->Billboard
            ? buildCanvasBillboardMatrix(canvasData->Transform.WorldMatrix, cam->GetViewMatrix())
            : canvasData->Transform.WorldMatrix;
        glm::vec3 localPoint(
            (logicalX - renderWidth * 0.5f) * 0.01f,
            (renderHeight * 0.5f - logicalY) * 0.01f,
            0.0f);
        glm::vec3 worldPoint = glm::vec3(canvasModel * glm::vec4(localPoint, 1.0f));
        glm::mat4 gizmoWorld = buildGizmoFrame(canvasModel, worldPoint);

        setAdaptiveGizmoSize(worldPoint);

        float matrix[16];
        memcpy(matrix, glm::value_ptr(gizmoWorld), sizeof(matrix));

        ImGuizmo::PushID(*m_SelectedEntity);
        if (restartKeyboardManipulation) {
            const int axisIndex = m_KeyboardTransformSession.Axis == KeyboardTransformAxis::None
                ? -1
                : static_cast<int>(m_KeyboardTransformSession.Axis);
            ImGuizmo::BeginKeyboardManipulation(m_KeyboardTransformSession.Operation, axisIndex);
        }
        ImGuizmo::Manipulate(view, proj, ImGuizmo::TRANSLATE, m_CurrentMode, matrix);
        m_GizmoOverThisFrame = ImGuizmo::IsOver();
        m_GizmoUsingThisFrame = ImGuizmo::IsUsing();
        ImGuizmo::PopID();

        if (m_GizmoUsingThisFrame && !m_TransformUndoActive) {
            EditorSceneUndoStack::Get().BeginScopedAction(m_Context, "Transform Entity");
            m_TransformUndoActive = EditorSceneUndoStack::Get().IsBoundTo(m_Context);
        } else if (!m_GizmoUsingThisFrame) {
            EndTransformUndoAction();
        }

        if (m_GizmoOverThisFrame || m_GizmoUsingThisFrame) {
            ImGui::SetNextFrameWantCaptureMouse(true);
            Renderer::Get().ConsumeUIInput();
        }

        if (m_GizmoUsingThisFrame) {
            glm::mat4 editedWorld = glm::make_mat4(matrix);
            glm::vec3 editedWorldPosition = glm::vec3(editedWorld[3]);
            glm::vec3 editedLocal = glm::vec3(glm::inverse(canvasModel) * glm::vec4(editedWorldPosition, 1.0f));
            float newLogicalX = editedLocal.x / 0.01f + renderWidth * 0.5f;
            float newLogicalY = renderHeight * 0.5f - editedLocal.y / 0.01f;
            const float deltaX = (newLogicalX - logicalX) / safeScaleX;
            const float deltaY = (newLogicalY - logicalY) / safeScaleY;

            switch (positionTarget) {
            case UIPositionTarget::PanelPosition:
                ed->Panel->Position.x += deltaX;
                ed->Panel->Position.y += deltaY;
                break;
            case UIPositionTarget::PanelAnchorOffset:
                ed->Panel->AnchorOffset.x += deltaX;
                ed->Panel->AnchorOffset.y += deltaY;
                break;
            case UIPositionTarget::TextAnchorOffset:
                ed->Text->AnchorOffset.x += deltaX;
                ed->Text->AnchorOffset.y += deltaY;
                break;
            case UIPositionTarget::UIRectOffset:
                ed->UIRect->Offset.x += deltaX;
                ed->UIRect->Offset.y += deltaY;
                ed->UIRect->_RectDirty = true;
                break;
            case UIPositionTarget::TransformPosition:
                ed->Transform.Position.x += deltaX;
                ed->Transform.Position.y += deltaY;
                updatesTransform = true;
                if (ed->UIRect)
                    ed->UIRect->_RectDirty = true;
                break;
            default:
                break;
            }

            if (updatesTransform)
                m_Context->MarkTransformDirty(*m_SelectedEntity);
            m_Context->MarkDirty();
        }

        if (ShouldCommitKeyboardTransformSession()) {
            EndKeyboardTransformSession(true);
        }

        return true;
    };

    if (tryDrawWorldSpaceUIGizmo()) {
        EndEntityTransformUndoAction();
        return;
    }

    // Apply adaptive gizmo scaling based on camera distance if enabled
    {
        glm::vec3 entityPos = glm::vec3(data->Transform.WorldMatrix[3]);
        setAdaptiveGizmoSize(entityPos);
    }

    // Use world matrix so gizmo appears at the correct spot for children
    // and reflects parent transforms. We'll convert the edited world back
    // into a local matrix relative to the parent below.
    glm::mat4 worldBefore = data->Transform.WorldMatrix;

    float matrix[16];
    memcpy(matrix, glm::value_ptr(worldBefore), sizeof(matrix));
    float deltaMatrix[16];
    for (int i = 0; i < 16; ++i) {
        deltaMatrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }

    ImGuizmo::PushID(*m_SelectedEntity);
    if (restartKeyboardManipulation) {
        const int axisIndex = m_KeyboardTransformSession.Axis == KeyboardTransformAxis::None
            ? -1
            : static_cast<int>(m_KeyboardTransformSession.Axis);
        ImGuizmo::BeginKeyboardManipulation(m_KeyboardTransformSession.Operation, axisIndex);
    }
    ImGuizmo::Manipulate(view, proj, m_CurrentOperation, m_CurrentMode, matrix, deltaMatrix);
    m_GizmoOverThisFrame = ImGuizmo::IsOver();
    m_GizmoUsingThisFrame = ImGuizmo::IsUsing();
    ImGuizmo::PopID();

    if (m_GizmoUsingThisFrame && !m_EntityTransformUndoActive) {
        m_EntityTransformUndoActive =
            EditorSceneUndoStack::Get().BeginEntityTransformAction(m_Context, *m_SelectedEntity, "Transform Entity");
    } else if (!m_GizmoUsingThisFrame) {
        EndEntityTransformUndoAction();
    }

    if (m_GizmoOverThisFrame || m_GizmoUsingThisFrame) {
        ImGui::SetNextFrameWantCaptureMouse(true);
        Renderer::Get().ConsumeUIInput();
    }

    if (m_GizmoUsingThisFrame) {
        // Convert the edited world matrix back into local space
        glm::mat4 editedWorld = glm::make_mat4(matrix);
        bool handledKeyboardAxisScale = false;
        if (m_KeyboardTransformSession.Active &&
            m_KeyboardTransformSession.Operation == ImGuizmo::SCALE &&
            m_KeyboardTransformSession.Axis != KeyboardTransformAxis::None) {
            const int axisIndex = static_cast<int>(m_KeyboardTransformSession.Axis);
            float factor = deltaMatrix[axisIndex * 5];
            if (!std::isfinite(factor) || factor <= 0.0f) {
                factor = 1.0f;
            }
            factor = std::max(factor, 0.0001f);

            const TransformComponent& baseline = m_KeyboardTransformSession.BaselineTransform;
            glm::vec3 scale = baseline.Scale;
            scale[axisIndex] = baseline.Scale[axisIndex] * factor;
            if (std::abs(scale[axisIndex]) < 0.0001f) {
                scale[axisIndex] = (scale[axisIndex] < 0.0f) ? -0.0001f : 0.0001f;
            }

            data->Transform.Position = baseline.Position;
            data->Transform.Rotation = baseline.Rotation;
            data->Transform.RotationQ = baseline.RotationQ;
            data->Transform.UseQuatRotation = baseline.UseQuatRotation;
            data->Transform.Scale = scale;
            handledKeyboardAxisScale = true;
        }

        if (!handledKeyboardAxisScale) {
            glm::mat4 parentWorld = glm::mat4(1.0f);
            if (data->Parent != -1) {
                if (auto* parentData = m_Context->GetEntityData(data->Parent))
                    parentWorld = parentData->Transform.WorldMatrix;
            }

            glm::mat4 newLocal = glm::inverse(parentWorld) * editedWorld;

            glm::vec3 pos, rot, scale;
            glm::quat rotQ;
            DecomposeMatrix(glm::value_ptr(newLocal), pos, rot, rotQ, scale);
            data->Transform.Position = pos;
            data->Transform.Rotation = rot;
            data->Transform.RotationQ = glm::normalize(rotQ);
            data->Transform.Scale = scale;
        }

        // Ensure transform updates propagate to children
        m_Context->MarkTransformDirty(*m_SelectedEntity);
        // Mark owning scene as dirty for serialization tracking
        if (m_Context) { m_Context->MarkDirty(); }
    }

    if (ShouldCommitKeyboardTransformSession()) {
        EndKeyboardTransformSession(true);
    }
}

// Radius gizmo for sphere colliders and sphere meshes
void ViewportPanel::DrawRadiusGizmo() {
    // Reset dragging state if conditions aren't met (prevents stuck state on selection change)
    if (*m_SelectedEntity < 0 || !m_Context || !m_ShowGizmos) {
        EndRadiusUndoAction();
        m_IsDraggingRadius = false;
        m_ActiveRadiusHandle = -1;
        return;
    }
    if (m_Context->m_IsPlaying) {
        EndRadiusUndoAction();
        m_IsDraggingRadius = false;
        m_ActiveRadiusHandle = -1;
        return;
    }

    auto* data = m_Context->GetEntityData(*m_SelectedEntity);
    if (!data) {
        EndRadiusUndoAction();
        m_IsDraggingRadius = false;
        m_ActiveRadiusHandle = -1;
        return;
    }

    // Check if we have a sphere collider or sphere mesh
    bool hasSphereCollider = false;
    bool hasSphereMesh = false;
    float currentRadius = 0.5f;
    float* radiusPtr = nullptr;

    if (data->Collider && data->Collider->ShapeType == ColliderShape::Sphere) {
        hasSphereCollider = true;
        currentRadius = data->Collider->Radius;
        radiusPtr = &data->Collider->Radius;
    }

    if (data->Mesh && data->Mesh->MeshName == "Sphere") {
        hasSphereMesh = true;
        // For sphere mesh, use scale to determine radius
        glm::vec3 scale = data->Transform.Scale;
        currentRadius = (scale.x + scale.y + scale.z) / 3.0f * 0.5f; // Unit sphere radius is 0.5
    }

    if (!hasSphereCollider && !hasSphereMesh) {
        EndRadiusUndoAction();
        m_IsDraggingRadius = false;
        m_ActiveRadiusHandle = -1;
        return;
    }

    // Always use editor camera for gizmo in edit mode
    Camera* cam = m_UseInternalCamera ? m_Camera.get() : Renderer::Get().GetEditorCamera();
    if (!cam) {
        cam = Renderer::Get().GetCamera(); // Fallback
    }
    if (!cam) {
        EndRadiusUndoAction();
        m_IsDraggingRadius = false;
        m_ActiveRadiusHandle = -1;
        return;
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (!drawList) {
        EndRadiusUndoAction();
        m_IsDraggingRadius = false;
        m_ActiveRadiusHandle = -1;
        return;
    }

    glm::mat4 worldMatrix = data->Transform.WorldMatrix;
    glm::vec3 worldPos = glm::vec3(worldMatrix[3]);
    
    const float* view = glm::value_ptr(cam->GetViewMatrix());
    const float* proj = glm::value_ptr(cam->GetProjectionMatrix());

    // Project world position to screen
    glm::vec4 clipPos = cam->GetProjectionMatrix() * cam->GetViewMatrix() * glm::vec4(worldPos, 1.0f);
    if (clipPos.w <= 0.0f) {
        // Behind camera - reset drag state in case camera moved during drag
        EndRadiusUndoAction();
        m_IsDraggingRadius = false;
        m_ActiveRadiusHandle = -1;
        return;
    }
    glm::vec2 screenPos = glm::vec2(clipPos.x / clipPos.w, clipPos.y / clipPos.w);
    screenPos = (screenPos + 1.0f) * 0.5f;
    screenPos.x = m_ViewportPos.x + screenPos.x * m_ViewportSize.x;
    screenPos.y = m_ViewportPos.y + (1.0f - screenPos.y) * m_ViewportSize.y;

    // Get camera forward direction to determine which circle to highlight
    glm::vec3 camForward = glm::normalize(glm::vec3(cam->GetViewMatrix()[2]));
    glm::vec3 camRight = glm::normalize(glm::vec3(cam->GetViewMatrix()[0]));
    glm::vec3 camUp = glm::normalize(glm::vec3(cam->GetViewMatrix()[1]));

    // Calculate world-space radius (accounting for scale)
    glm::vec3 scale = data->Transform.Scale;
    float worldRadius = currentRadius * std::max(scale.x, std::max(scale.y, scale.z));

    // Draw three orthogonal circles
    const int numSegments = 64;
    const float handleSize = 8.0f;
    const ImU32 colorX = IM_COL32(255, 80, 80, 255);
    const ImU32 colorY = IM_COL32(80, 255, 80, 255);
    const ImU32 colorZ = IM_COL32(80, 80, 255, 255);
    const ImU32 colorHover = IM_COL32(255, 255, 0, 255);

    // Get mouse position in world space
    ImVec2 mousePos = ImGui::GetMousePos();
    float nx = (mousePos.x - m_ViewportPos.x) / m_ViewportSize.x;
    float ny = (mousePos.y - m_ViewportPos.y) / m_ViewportSize.y;
    nx = glm::clamp(nx, 0.0f, 1.0f);
    ny = glm::clamp(ny, 0.0f, 1.0f);
    Ray mouseRay = Picking::ScreenPointToRay(nx, ny, cam);

    int hoveredHandle = -1;
    float minDist = FLT_MAX;
    const float pickThreshold = 0.05f; // World-space threshold for picking handles

    // Draw and check each circle
    for (int axis = 0; axis < 3; ++axis) {
        glm::vec3 axisDir, perp1, perp2;
        ImU32 color;
        if (axis == 0) { // X-axis circle (YZ plane)
            axisDir = glm::vec3(1, 0, 0);
            perp1 = glm::vec3(0, 1, 0);
            perp2 = glm::vec3(0, 0, 1);
            color = colorX;
        } else if (axis == 1) { // Y-axis circle (XZ plane)
            axisDir = glm::vec3(0, 1, 0);
            perp1 = glm::vec3(1, 0, 0);
            perp2 = glm::vec3(0, 0, 1);
            color = colorY;
        } else { // Z-axis circle (XY plane)
            axisDir = glm::vec3(0, 0, 1);
            perp1 = glm::vec3(1, 0, 0);
            perp2 = glm::vec3(0, 1, 0);
            color = colorZ;
        }

        // Transform axes to world space
        glm::mat3 rotMatrix = glm::mat3(worldMatrix);
        axisDir = glm::normalize(rotMatrix * axisDir);
        perp1 = glm::normalize(rotMatrix * perp1);
        perp2 = glm::normalize(rotMatrix * perp2);

        // Check if mouse ray intersects with this circle plane
        float denom = glm::dot(mouseRay.Direction, axisDir);
        if (fabs(denom) > 1e-6f) {
            float t = glm::dot(worldPos - mouseRay.Origin, axisDir) / denom;
            if (t > 0.0f) {
                glm::vec3 hitPoint = mouseRay.Origin + mouseRay.Direction * t;
                glm::vec3 toHit = hitPoint - worldPos;
                float distFromCenter = glm::length(toHit);
                
                // Check if hit is near the circle
                if (fabs(distFromCenter - worldRadius) < pickThreshold * worldRadius) {
                    float dist = fabs(distFromCenter - worldRadius);
                    if (dist < minDist) {
                        minDist = dist;
                        hoveredHandle = axis;
                    }
                }
            }
        }

        // Draw circle on screen
        std::vector<ImVec2> screenPoints;
        screenPoints.reserve(numSegments + 1);
        for (int i = 0; i <= numSegments; ++i) {
            float angle = (float)i / numSegments * 2.0f * 3.14159f;
            glm::vec3 point = worldPos + (perp1 * cos(angle) + perp2 * sin(angle)) * worldRadius;
            glm::vec4 clip = cam->GetProjectionMatrix() * cam->GetViewMatrix() * glm::vec4(point, 1.0f);
            if (clip.w > 0.0f) {
                glm::vec2 screen = glm::vec2(clip.x / clip.w, clip.y / clip.w);
                screen = (screen + 1.0f) * 0.5f;
                screen.x = m_ViewportPos.x + screen.x * m_ViewportSize.x;
                screen.y = m_ViewportPos.y + (1.0f - screen.y) * m_ViewportSize.y;
                screenPoints.push_back(ImVec2(screen.x, screen.y));
            }
        }

        if (screenPoints.size() > 1) {
            ImU32 drawColor = (hoveredHandle == axis || m_ActiveRadiusHandle == axis) ? colorHover : color;
            for (size_t i = 0; i < screenPoints.size() - 1; ++i) {
                drawList->AddLine(screenPoints[i], screenPoints[i + 1], drawColor, 2.0f);
            }
        }
    }

    // Handle mouse interaction
    bool isMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    
    // Don't interfere with ImGuizmo
    if (IsGizmoOver() || IsGizmoUsing()) {
        if (m_IsDraggingRadius) {
            EndRadiusUndoAction();
            m_IsDraggingRadius = false;
            m_ActiveRadiusHandle = -1;
        }
        return;
    }

    if (!m_IsDraggingRadius && hoveredHandle >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        EditorSceneUndoStack::Get().BeginScopedAction(m_Context, "Resize Sphere");
        m_RadiusUndoActive = EditorSceneUndoStack::Get().IsBoundTo(m_Context);
        m_IsDraggingRadius = true;
        m_ActiveRadiusHandle = hoveredHandle;
        m_RadiusDragStart = currentRadius;
        // Store initial mouse ray intersection point
        glm::vec3 axisDir = glm::normalize(glm::vec3(worldMatrix[m_ActiveRadiusHandle]));
        float denom = glm::dot(mouseRay.Direction, axisDir);
        if (fabs(denom) > 1e-6f) {
            float t = glm::dot(worldPos - mouseRay.Origin, axisDir) / denom;
            m_RadiusDragStartPos = mouseRay.Origin + mouseRay.Direction * t;
        }
    }

    if (m_IsDraggingRadius) {
        if (isMouseDown) {
            // Calculate new radius based on mouse movement along the active circle
            glm::vec3 axisDir = glm::normalize(glm::vec3(worldMatrix[m_ActiveRadiusHandle]));
            float denom = glm::dot(mouseRay.Direction, axisDir);
            if (fabs(denom) > 1e-6f) {
                float t = glm::dot(worldPos - mouseRay.Origin, axisDir) / denom;
                glm::vec3 hitPoint = mouseRay.Origin + mouseRay.Direction * t;
                float newWorldRadius = glm::length(hitPoint - worldPos);
                
                // Get initial world radius
                glm::vec3 scale = data->Transform.Scale;
                float avgScale = std::max(scale.x, std::max(scale.y, scale.z));
                float startWorldRadius = m_RadiusDragStart * avgScale;
                
                // Calculate scale factor
                float radiusScale = (startWorldRadius > 1e-6f) ? (newWorldRadius / startWorldRadius) : 1.0f;
                float newRadius = m_RadiusDragStart * radiusScale;
                
                // Clamp to reasonable values
                newRadius = glm::clamp(newRadius, 0.01f, 100.0f);
                
                if (hasSphereCollider && radiusPtr) {
                    *radiusPtr = newRadius;
                    // Rebuild shape
                    data->Collider->BuildShape(nullptr, scale);
                    if (m_Context) m_Context->MarkDirty();
                } else if (hasSphereMesh) {
                    // Update scale uniformly - unit sphere has radius 0.5, so scale = radius * 2
                    float newScale = newRadius * 2.0f;
                    data->Transform.Scale = glm::vec3(newScale);
                    data->Transform.TransformDirty = true;
                    m_Context->MarkTransformDirty(*m_SelectedEntity);
                    if (m_Context) m_Context->MarkDirty();
                }
            }
        } else {
            EndRadiusUndoAction();
            m_IsDraggingRadius = false;
            m_ActiveRadiusHandle = -1;
        }
    }
}

// 2D UI gizmo: draw a draggable handle at panel/text screen position
void ViewportPanel::DrawUIGizmo() {
    // Reset UI handle state at start of each frame when conditions aren't met
    // This prevents flags from getting stuck if selection changes mid-drag
    if (!m_ShowGizmos || !m_Context || *m_SelectedEntity < 0) {
        EndUIUndoAction();
        m_UIHandleActive = false;
        m_UIHandleHovered = false;
        return;
    }
    auto* ed = m_Context->GetEntityData(*m_SelectedEntity);
    if (!ed) {
        EndUIUndoAction();
        m_UIHandleActive = false;
        m_UIHandleHovered = false;
        return;
    }
    bool isScreenUI = false;
    {
        EntityID cur = *m_SelectedEntity;
        while (cur != -1) {
            auto* d2 = m_Context->GetEntityData(cur);
            if (!d2) break;
            if (d2->Canvas && d2->Canvas->Space == CanvasComponent::RenderSpace::ScreenSpace) { isScreenUI = true; break; }
            cur = d2->Parent;
        }
    }
    if (!isScreenUI) {
        EndUIUndoAction();
        m_UIHandleActive = false;
        m_UIHandleHovered = false;
        return;
    }

    // Determine viewport image top-left to convert screen coords to overlay coords
    ImVec2 viewportTL = m_ViewportPos;
    float renderW = (m_RenderTargetWidth > 0) ? (float)m_RenderTargetWidth : (float)Renderer::Get().GetWidth();
    float renderH = (m_RenderTargetHeight > 0) ? (float)m_RenderTargetHeight : (float)Renderer::Get().GetHeight();
    // Convert stored backbuffer-space values to overlay coordinates using viewport scale
    float sx = (m_ViewportSize.x > 0.0f) ? (renderW / m_ViewportSize.x) : 1.0f;
    float sy = (m_ViewportSize.y > 0.0f) ? (renderH / m_ViewportSize.y) : 1.0f;
    // Map element position to viewport overlay space: use Panel.Position or Text Transform.Position
    ImVec2 p = viewportTL;
    if (ed->Panel) {
        if (ed->Panel->AnchorEnabled) {
            p.x += ed->Panel->AnchorOffset.x / sx;
            p.y += ed->Panel->AnchorOffset.y / sy;
        } else {
            p.x += ed->Panel->Position.x / sx;
            p.y += ed->Panel->Position.y / sy;
        }
    } else if (ed->Text && !ed->Text->WorldSpace) {
        p.x += ed->Transform.Position.x / sx;
        p.y += ed->Transform.Position.y / sy;
    } else if (ed->Canvas) {
        // Canvas origin at (0,0) in backbuffer space
    } else return;

    // Draw handle
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 col = IM_COL32(255, 180, 0, 200);
    float r = 6.0f;
    dl->AddCircleFilled(ImVec2(p.x, p.y), r, col);

    // Drag to move
    ImGui::SetCursorScreenPos(ImVec2(p.x - r, p.y - r));
    ImGui::InvisibleButton("ui_drag", ImVec2(r*2, r*2));
    m_UIHandleHovered = ImGui::IsItemHovered();
    if (m_UIHandleHovered && m_ReportUIMouse) {
        // Mark UI input as consumed to prevent scene picking
        Renderer::Get().SetUIMousePosition(0,0,0,0,true); // keep valid flag
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        if (!m_UIUndoActive) {
            EditorSceneUndoStack::Get().BeginScopedAction(m_Context, "Transform Entity");
            m_UIUndoActive = EditorSceneUndoStack::Get().IsBoundTo(m_Context);
        }
        m_UIHandleActive = true;
        ImVec2 delta = ImGui::GetIO().MouseDelta;
        // Convert overlay pixel delta to framebuffer pixel delta (normalize by letterboxed viewport)
        float sx2 = (m_ViewportSize.x > 0.0f) ? (renderW / m_ViewportSize.x) : 1.0f;
        float sy2 = (m_ViewportSize.y > 0.0f) ? (renderH / m_ViewportSize.y) : 1.0f;
        delta.x *= sx2; delta.y *= sy2;
        if (ed->Panel) {
            if (ed->Panel->AnchorEnabled) {
                ed->Panel->AnchorOffset.x += delta.x;
                ed->Panel->AnchorOffset.y += delta.y;
            } else {
                ed->Panel->Position.x += delta.x;
                ed->Panel->Position.y += delta.y;
            }
        } else if (ed->Text && !ed->Text->WorldSpace) {
            ed->Transform.Position.x += delta.x;
            ed->Transform.Position.y += delta.y;
        }
    } else {
        EndUIUndoAction();
        m_UIHandleActive = false;
    }
}

void ViewportPanel::DrawConsoleOverlay(const ImVec2& viewportTL, const ImVec2& viewportSize) {
    Scene* ctx = m_Context;
    bool isPlay = (ctx && ctx->m_IsPlaying);
    // Only render inside main viewport window; rely on Renderer::Get() for input consumption via ImGui flags
    CommandConsole::Instance().RenderInViewport(viewportTL, viewportSize, isPlay);
}

// Helper: draw all UI rects as an editor overlay
void ViewportPanel::DrawUIRectOverlay(ImDrawList* dl, const ImVec2& viewportTL, const ImVec2& viewportSize, Scene* scene) {
    if (!scene) return;
    float renderW = (m_RenderTargetWidth > 0) ? (float)m_RenderTargetWidth : (float)Renderer::Get().GetWidth();
    float renderH = (m_RenderTargetHeight > 0) ? (float)m_RenderTargetHeight : (float)Renderer::Get().GetHeight();
    float sx = (viewportSize.x > 0.0f) ? (renderW / viewportSize.x) : 1.0f;
    float sy = (viewportSize.y > 0.0f) ? (renderH / viewportSize.y) : 1.0f;
    auto toOverlay = [&](float x, float y){ return ImVec2(viewportTL.x + x / sx, viewportTL.y + y / sy); };
    for (auto& e : scene->GetEntities()) {
        auto* d = scene->GetEntityData(e.GetID()); if (!d || !d->Visible) continue;
        // Check if under a screen-space canvas
        bool underScreen = false;
        {
            EntityID cur = e.GetID();
            while (cur != -1) { auto* d2 = scene->GetEntityData(cur); if (!d2) break; if (d2->Canvas && d2->Canvas->Space == CanvasComponent::RenderSpace::ScreenSpace) { underScreen = true; break; } cur = d2->Parent; }
        }
        if (!underScreen) continue;
        // Panel rect
        if (d->Panel && d->Panel->Visible) {
            float ax = d->Panel->AnchorEnabled ? 0.0f : d->Panel->Position.x;
            float ay = d->Panel->AnchorEnabled ? 0.0f : d->Panel->Position.y;
            if (d->Panel->AnchorEnabled) {
                // Use renderer's backbuffer size for anchoring
                float W = renderW;
                float H = renderH;
                switch (d->Panel->Anchor) {
                    case UIAnchorPreset::TopLeft:    break;
                    case UIAnchorPreset::Top:        ax = W * 0.5f; break;
                    case UIAnchorPreset::TopRight:   ax = W; break;
                    case UIAnchorPreset::Left:       ay = H * 0.5f; break;
                    case UIAnchorPreset::Center:     ax = W * 0.5f; ay = H * 0.5f; break;
                    case UIAnchorPreset::Right:      ax = W; ay = H * 0.5f; break;
                    case UIAnchorPreset::BottomLeft: ay = H; break;
                    case UIAnchorPreset::Bottom:     ax = W * 0.5f; ay = H; break;
                    case UIAnchorPreset::BottomRight:ax = W; ay = H; break;
                }
                ax += d->Panel->AnchorOffset.x; ay += d->Panel->AnchorOffset.y;
            }
            float x0 = ax;
            float y0 = ay;
            float x1 = x0 + d->Panel->Size.x * d->Panel->Scale.x;
            float y1 = y0 + d->Panel->Size.y * d->Panel->Scale.y;
            ImU32 col = IM_COL32(255,136,0,160);
            dl->AddRect(toOverlay(x0,y0), toOverlay(x1,y1), col, 0.0f, 0, 1.5f);
        }
        // Text rect: show wrapping rect if set; otherwise approximate baseline rect height using pixel size
        if (d->Text && !d->Text->WorldSpace && d->Text->Visible) {
            float sxp = d->Transform.Position.x; float syp = d->Transform.Position.y;
            if (d->Text->AnchorEnabled) {
                float W = renderW;
                float H = renderH;
                switch (d->Text->Anchor) {
                    case UIAnchorPreset::TopLeft:    break;
                    case UIAnchorPreset::Top:        sxp = W * 0.5f; break;
                    case UIAnchorPreset::TopRight:   sxp = W; break;
                    case UIAnchorPreset::Left:       syp = H * 0.5f; break;
                    case UIAnchorPreset::Center:     sxp = W * 0.5f; syp = H * 0.5f; break;
                    case UIAnchorPreset::Right:      sxp = W; syp = H * 0.5f; break;
                    case UIAnchorPreset::BottomLeft: syp = H; break;
                    case UIAnchorPreset::Bottom:     sxp = W * 0.5f; syp = H; break;
                    case UIAnchorPreset::BottomRight:sxp = W; syp = H; break;
                }
                sxp += d->Text->AnchorOffset.x; syp += d->Text->AnchorOffset.y;
            }
            float rw = d->Text->RectSize.x;
            float rh = d->Text->RectSize.y;
            if (rw <= 0.0f || rh <= 0.0f) {
                // Initialize to an approximate AABB for current text (one line height)
                float lineH = d->Text->PixelSize; // approx
                rh = lineH * 1.2f;
                // crude width estimate: characters * 0.6 * pixelSize
                float est = std::max<size_t>(1, d->Text->Text.size());
                rw = std::min(600.0f, est * (d->Text->PixelSize * 0.6f));
            }
            ImU32 col = IM_COL32(0,200,255,160);
            dl->AddRect(toOverlay(sxp, syp), toOverlay(sxp + rw, syp + rh), col, 0.0f, 0, 1.5f);
        }
    }
}

void ViewportPanel::DecomposeMatrix(const float* matrix, glm::vec3& pos, glm::vec3& rot, glm::quat& rotQ, glm::vec3& scale) {
    glm::mat4 m = glm::make_mat4(matrix);
    glm::vec3 skew;
    glm::vec4 perspective;
    glm::quat orientation;
    
    // Initialize output parameters
    pos = glm::vec3(0.0f);
    rot = glm::vec3(0.0f);
    rotQ = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    scale = glm::vec3(1.0f);
    
    if (!glm::decompose(m, scale, orientation, pos, skew, perspective)) {
        return;
    }

    rotQ = glm::normalize(orientation);
    rot = glm::degrees(glm::eulerAngles(rotQ));
}

void ViewportPanel::FinalizeAssetDrop() {
    if (!m_Context || m_DraggedAssetPath.empty()) return;

    if (m_GhostEntityId != INVALID_ENTITY_ID) {
        UpdateAssetPreviewTransform();
        if (m_SelectedEntity) {
            *m_SelectedEntity = m_GhostEntityId;
        }
        CancelAssetPreview(true);
        EditorSceneUndoStack::Get().CommitSceneState(m_Context, "Add Entity");
        m_DraggedAssetPath.clear();
        m_CanPreviewDraggedAsset = false;
        return;
    }

    std::string path = m_DraggedAssetPath;
    // Models: enqueue background import then hot-swap; others: old path
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb") {
        // Create placeholder
        Entity placeholder = m_Context->CreateEntity("Importing ...");
        EntityID placeholderID = placeholder.GetID();
        if (auto* ed = m_Context->GetEntityData(placeholderID)) {
            ed->Transform.Position = m_GhostPosition;
        }
        AssetPipeline::ImportRequest req;
        req.sourcePath = path;
        req.preferredVPath = "assets/models";
        req.onReady = [this, placeholderID, pos = m_GhostPosition](const BuiltModelPaths& built){
            // If build failed, keep placeholder but rename
            if (built.metaPath.empty()) {
                auto* ed = m_Context->GetEntityData(placeholderID);
                if (ed) ed->Name = "Import failed (see console)";
                return;
            }
            // Replace placeholder
            m_Context->RemoveEntity(placeholderID);
            EntityID id = m_Context->InstantiateModelFast(built.metaPath, pos, /*synchronous*/ false);
            if (id != -1) *m_SelectedEntity = id;
        };
        if (auto* pipeline = Application::Get().GetAssetPipeline()) {
            pipeline->EnqueueModelImport(req);
        }
    } else {
        // Use the last computed ghost position for placement when available
        EntityID entityID = m_Context->InstantiateAsset(path, m_GhostPosition);
        if (entityID == -1) {
            std::cerr << "[ViewportPanel] Failed to instantiate asset: " << path << std::endl;
        } else {
            *m_SelectedEntity = entityID;
            EditorSceneUndoStack::Get().CommitSceneState(m_Context, "Add Entity");
        }
    }

    m_DraggedAssetPath.clear();
    m_CanPreviewDraggedAsset = false;
}

bool ViewportPanel::SupportsAssetPreview(const std::string& path) const {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb" ||
           ext == ".meta" || ext == ".prefab" || ext == ".json";
}

void ViewportPanel::EnsureAssetPreviewEntity() {
    if (!m_Context) return;
    if (!m_CanPreviewDraggedAsset) {
        CancelAssetPreview(false);
        return;
    }
    if (m_GhostEntityId != INVALID_ENTITY_ID) {
        if (m_GhostAssetPath == m_DraggedAssetPath) {
            UpdateAssetPreviewTransform();
            return;
        }
        CancelAssetPreview(false);
    }
    m_PreviewSceneWasDirty = m_Context->IsDirty();
    EditorSceneUndoStack::Get().SetAutoCaptureSuppressed(m_Context, true);
    EntityID id = m_Context->InstantiateAsset(m_DraggedAssetPath, m_GhostPosition);
    if (id == INVALID_ENTITY_ID) {
        EditorSceneUndoStack::Get().SetAutoCaptureSuppressed(m_Context, false);
        m_GhostAssetPath.clear();
        return;
    }
    m_GhostEntityId = id;
    m_GhostAssetPath = m_DraggedAssetPath;
    UpdateAssetPreviewTransform();
}

void ViewportPanel::UpdateAssetPreviewTransform() {
    if (!m_Context) return;
    if (m_GhostEntityId == INVALID_ENTITY_ID) return;
    auto* data = m_Context->GetEntityData(m_GhostEntityId);
    if (!data) return;
    if (glm::distance(data->Transform.Position, m_GhostPosition) < 0.001f) return;
    data->Transform.Position = m_GhostPosition;
    m_Context->MarkTransformDirty(m_GhostEntityId);
}

void ViewportPanel::CancelAssetPreview(bool commit) {
    if (!m_Context) return;
    if (m_GhostEntityId == INVALID_ENTITY_ID) {
        EditorSceneUndoStack::Get().SetAutoCaptureSuppressed(m_Context, false);
        m_GhostAssetPath.clear();
        return;
    }
    if (!commit) {
        m_Context->RemoveEntity(m_GhostEntityId);
        if (m_PreviewSceneWasDirty) {
            m_Context->MarkDirty();
        } else {
            m_Context->ClearDirty();
        }
    }
    m_GhostEntityId = INVALID_ENTITY_ID;
    m_GhostAssetPath.clear();
    m_PreviewSceneWasDirty = false;
    EditorSceneUndoStack::Get().SetAutoCaptureSuppressed(m_Context, false);
}

bool ViewportPanel::IsViewportHovered() const {
    if (!m_WindowFocusedOrHovered) return false;
    
    // Check if mouse is actually over the viewport image bounds
    ImVec2 mouse = ImGui::GetMousePos();
    float mx = (mouse.x - m_ViewportPos.x);
    float my = (mouse.y - m_ViewportPos.y);
    return (mx >= 0 && my >= 0 && mx <= m_ViewportSize.x && my <= m_ViewportSize.y);
}

bool ViewportPanel::IsGizmoOver() const {
    return m_GizmoOwnerThisFrame && m_GizmoOverThisFrame;
}

bool ViewportPanel::IsGizmoUsing() const {
    return m_GizmoOwnerThisFrame && m_GizmoUsingThisFrame;
}

