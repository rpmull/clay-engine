#pragma once
#include <glm/glm.hpp>

// Editor-wide settings for viewport interaction, camera behavior, and visual aids.
// These are runtime-only; persistence can be added via JSON serialization later.
struct EditorSettings {
    // ===== Camera Navigation =====
    // Base zoom speed multiplier (applied to scroll delta)
    float ZoomBaseSpeed = 1.0f;
    // Zoom acceleration factor for fast scrolls (higher = more aggressive acceleration)
    float ZoomAcceleration = 0.15f;
    // Minimum zoom distance (prevents clipping through geometry)
    float ZoomMinDistance = 0.1f;
    // Maximum zoom distance
    float ZoomMaxDistance = 5000.0f;
    // Enable smooth damped zooming
    bool SmoothZoomEnabled = true;
    // Smoothing factor for zoom (0 = instant, 1 = very slow)
    float ZoomSmoothness = 0.15f;
    
    // Orbit sensitivity (degrees per pixel)
    float OrbitSensitivity = 0.2f;
    // Pan speed multiplier (relative to distance)
    float PanSpeedFactor = 0.01f;
    
    // ===== Focus / Frame Selected =====
    // Default duration for focus animation (seconds)
    float FocusDuration = 0.35f;
    // Distance padding multiplier when framing objects
    float FocusDistancePadding = 1.5f;
    // Default distance when focusing on objects without bounds
    float FocusDefaultDistance = 5.0f;
    
    // ===== Entity Picking =====
    // Whether clicking on empty space deselects the current entity
    bool DeselectOnEmptyClick = true;
    // Screen-space tolerance for picking (in normalized coords, e.g., 0.005 = 0.5% of viewport)
    float PickingTolerance = 0.0f;
    // Skip picking for hidden entities
    bool PickingSkipHidden = true;
    // Skip picking for locked entities  
    bool PickingSkipLocked = true;
    
    // ===== Grid Settings =====
    // Major axis line colors (X = Red, Y = Green, Z = Blue)
    glm::vec4 GridAxisColorX = glm::vec4(0.8f, 0.2f, 0.2f, 0.7f);
    glm::vec4 GridAxisColorY = glm::vec4(0.2f, 0.8f, 0.2f, 0.7f);
    glm::vec4 GridAxisColorZ = glm::vec4(0.2f, 0.2f, 0.8f, 0.7f);
    // Regular grid line color
    glm::vec4 GridColor = glm::vec4(0.5f, 0.5f, 0.5f, 0.35f);
    // Major grid line color (every N units)
    glm::vec4 GridMajorColor = glm::vec4(0.6f, 0.6f, 0.6f, 0.5f);
    // Major grid line interval (draw thicker lines every N units)
    int GridMajorLineInterval = 10;
    // Enable colored axis lines on grid
    bool GridShowAxisLines = true;
    // Grid plane orientation: 0 = XZ (default), 1 = XY, 2 = YZ
    int GridPlaneOrientation = 0;
    
    // ===== Gizmo Interaction =====
    // Base gizmo scale factor
    float GizmoBaseScale = 1.0f;
    // Enable automatic gizmo scaling based on camera distance
    bool GizmoAutoScale = true;
    // Minimum gizmo screen size (prevents too-small gizmos)
    float GizmoMinScreenSize = 80.0f;
    // Maximum gizmo screen size (prevents too-large gizmos)
    float GizmoMaxScreenSize = 200.0f;
    
    // ===== Selection Feedback =====
    // Show outline on entity under cursor (hover highlight)
    bool ShowHoverOutline = false;
    // Hover outline color
    glm::vec4 HoverOutlineColor = glm::vec4(1.0f, 0.8f, 0.2f, 0.6f);
    // Hover outline thickness (pixels)
    float HoverOutlineThickness = 2.0f;
    
    // ===== Singleton Access =====
    static EditorSettings& Get() {
        static EditorSettings instance;
        return instance;
    }
    
private:
    EditorSettings() = default;
};




