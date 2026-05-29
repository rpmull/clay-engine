#pragma once

#include <bgfx/bgfx.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <unordered_set>
#include "Camera.h"
#include "core/ecs/Entity.h"

// Forward declarations
class Scene;

/**
 * RenderContext encapsulates all per-viewport rendering state.
 * This allows multiple independent viewports (main scene, prefab editor, etc.)
 * to render without interfering with each other.
 */
struct RenderContext {
    // ===== Required =====
    Scene* scene = nullptr;
    Camera* camera = nullptr;
    uint16_t viewId = 0;
    // Optional UI overlay view id (screen-space UI) for this context
    uint16_t uiViewId = 2;
    uint32_t width = 0;
    uint32_t height = 0;
    
    // ===== Matrices (computed from camera via UpdateFromCamera()) =====
    float view[16] = {};
    float proj[16] = {};
    
    // ===== Per-viewport framebuffer =====
    // Set to BGFX_INVALID_HANDLE for main viewport (renders to backbuffer/main scene FB)
    bgfx::FrameBufferHandle framebuffer = BGFX_INVALID_HANDLE;
    
    // ===== Options =====
    bool showGrid = true;
    bool enableFrustumCulling = true;
    bool isOffscreen = false;  // True for secondary viewports (prefab editor, etc.)
    bool allowUIInput = true;  // If false, UI renders but won't consume input
    bool renderUIOverlay = true; // If false, skips UI overlay pass
    bool enableShadows = true; // If false, shadow uniforms are disabled for this context
    // Optional: restrict rendering to a subset of entities (meshes only)
    const std::unordered_set<EntityID>* allowedEntities = nullptr;
    // Optional: force fog disabled for this render context
    bool forceFogDisabled = false;
    uint16_t clearFlags = BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH;
    uint32_t clearColor = 0x202020ff; // RGBA
    float clearDepth = 1.0f;
    uint8_t clearStencil = 0;
    
    /**
     * Computes view and proj matrices from the current camera.
     * Call this after setting camera, width, and height.
     */
    void UpdateFromCamera() {
        if (!camera) return;
        
        glm::mat4 viewMat = camera->GetViewMatrix();
        glm::mat4 projMat = camera->GetProjectionMatrix();
        
        memcpy(view, glm::value_ptr(viewMat), sizeof(float) * 16);
        memcpy(proj, glm::value_ptr(projMat), sizeof(float) * 16);
    }
    
    /**
     * Returns the camera position as a glm::vec3.
     * Returns zero vector if camera is null.
     */
    glm::vec3 GetCameraPosition() const {
        if (!camera) return glm::vec3(0.0f);
        return camera->GetPosition();
    }
};

