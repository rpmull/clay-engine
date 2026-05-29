#pragma once

#include <bgfx/bgfx.h>
#include <unordered_map>
#include "core/particles/ParticleSystem.h"

// Forward declarations
class Scene;

namespace ecs
{
    class ParticleEmitterSystem
    {
    public:
        static ParticleEmitterSystem& Get();

        void Init();
        void Shutdown();

        // Tick emitters and underlying particle system.
        void Update(Scene& scene, float dt);
        
        // Only sync emitter uniforms from a scene without stepping simulation.
        // Use this for secondary viewports (prefab editor) to avoid double-updating.
        void SyncEmittersOnly(Scene& scene);

        // Submit draw calls for all emitters.
        void Render(uint8_t viewId, const float* mtxView, const bx::Vec3& eye);
        
        // Submit draw calls only for emitters belonging to the specified scene.
        // If filterScene is nullptr, renders all emitters (backwards compatible).
        void Render(uint8_t viewId, const float* mtxView, const bx::Vec3& eye, Scene* filterScene);
        
        // Render all particles without any scene filtering (for isolated snapshot renders)
        void RenderAllUnfiltered(uint8_t viewId, const float* mtxView, const bx::Vec3& eye);
        
        // Track emitter ownership when created
        void RegisterEmitterOwnership(ps::EmitterHandle handle, Scene* scene);
        
        // Remove ownership tracking when emitter is destroyed
        void UnregisterEmitterOwnership(ps::EmitterHandle handle);
        
        // Clear all emitters owned by a specific scene (call when scene is destroyed)
        void ClearSceneEmitters(Scene* scene);
        
    private:
        bool m_Initialized = false;
        
        // Maps emitter handle index to owning scene
        std::unordered_map<uint16_t, Scene*> m_EmitterOwnership;
    };
}
