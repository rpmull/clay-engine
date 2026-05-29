#pragma once
#include <bgfx/bgfx.h>

// Forward declarations
class Scene;

namespace cm { namespace deformation {

// ============================================================================
// ArmorWrapSystem - Runtime wrap deformation processor
// ============================================================================
// This system processes all entities with ArmorFitComponent each frame.
// It retrieves the body mesh's final positions (post-morph, post-skinning)
// and applies wrap deformation to armor meshes.
//
// The system supports both CPU and GPU (compute shader) execution paths.
// ============================================================================

class ArmorWrapSystem
{
public:
    // Initialize system resources (compute shader program, uniforms)
    static void Initialize();
    
    // Shutdown and release resources
    static void Shutdown();
    
    // Main update - processes all armor wrap components in the scene
    // Should be called AFTER SkinningSystem::Update and blendshape processing
    static void Update(Scene& scene);
    
    // Check if GPU compute path is available
    static bool IsComputeAvailable();
    
    // Force CPU fallback for debugging/testing
    static void SetForceGpuDisabled(bool disabled);
    
private:
    // CPU fallback path for wrap deformation
    static void UpdateCPU(Scene& scene);
    
    // GPU compute path for wrap deformation
    static void UpdateGPU(Scene& scene);
};

}} // namespace cm::deformation

