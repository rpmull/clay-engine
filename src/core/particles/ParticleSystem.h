/*
 * Adapted from BGFX example particle system (copyright Branimir Karadzic).
 * Integrated into Claymore engine with modern particle system features.
 */
#pragma once

#include <bgfx/bgfx.h>
#include <bx/allocator.h>
#include <bx/bounds.h>
#include <bx/easing.h>
#include <bx/rng.h>
#include <cstdint>

// Forward declare pack rect types (implemented in packrect.h)
struct Pack2D;

namespace ps // short namespace for particle system
{
    struct EmitterHandle       { uint16_t idx; };
    struct EmitterSpriteHandle { uint16_t idx; };

    template<typename Ty>
    inline bool isValid(Ty _handle) { return _handle.idx != UINT16_MAX; }

    struct EmitterShape
    {
        enum Enum
        {
            Point = 0,      // Single point emission
            Sphere,         // Emit from sphere surface/volume
            Hemisphere,     // Emit from hemisphere
            Cone,           // Emit in a cone direction
            Box,            // Emit from box volume
            Circle,         // Emit from circle edge
            Disc,           // Emit from filled disc
            Edge,           // Emit from a line segment
            Rect,           // Emit from rectangle perimeter

            Count
        };
    };

    struct EmitterDirection
    {
        enum Enum
        {
            Up,
            Outward,
            Random,
            Directional,    // Use custom direction vector

            Count
        };
    };
    
    // Curve type for over-lifetime interpolation
    struct CurveType
    {
        enum Enum
        {
            Constant = 0,
            Linear,
            EaseIn,
            EaseOut,
            EaseInOut,
            
            Count
        };
    };

    struct EmitterUniforms
    {
        void reset();

        // ===== Transform =====
        float m_position[3];
        float m_angle[3];
        float m_direction[3];       // Custom emission direction (normalized)
        
        // ===== Shape Parameters =====
        float m_shapeRadius;        // Radius for sphere/cone/circle/disc
        float m_shapeRadiusThickness; // 0=surface, 1=volume
        float m_shapeAngle;         // Cone angle in radians
        float m_shapeArc;           // Arc angle for partial shapes (radians)
        float m_shapeScale[3];      // Scale for box shapes
        float m_shapeLength;        // Length for edge shape

        // ===== Legacy offset/scale (still used internally) =====
        float m_blendStart[2];
        float m_blendEnd[2];
        float m_offsetStart[2];
        float m_offsetEnd[2];
        float m_scaleStart[2];
        float m_scaleEnd[2];
        float m_lifeSpan[2];
        
        // ===== Start Parameters =====
        float m_startSpeedMin;
        float m_startSpeedMax;
        float m_startSizeMin;
        float m_startSizeMax;
        float m_startRotationMin;   // Radians
        float m_startRotationMax;
        
        // ===== Physics =====
        float m_gravityScale;
        float m_dragCoefficient;
        float m_inheritVelocity;
        float m_emitterVelocity[3];
        
        // ===== Velocity Over Lifetime =====
        float m_linearVelocity[3];
        float m_orbitalVelocity;
        float m_radialVelocity;
        
        // ===== Size Over Lifetime =====
        float m_sizeOverLifetimeStart;
        float m_sizeOverLifetimeEnd;
        CurveType::Enum m_sizeOverLifetimeCurve;
        
        // ===== Rotation Over Lifetime =====
        float m_angularVelocity;    // Radians per second
        bool m_alignToVelocity;     // Rotate particles to follow trajectory

        // ===== Color (RGBA gradient keys, up to 5 for compatibility) =====
        uint32_t m_rgba[5];
        float m_rgbaTime[5];        // Time keys for gradient (0-1)
        int m_rgbaKeyCount;         // Number of active gradient keys
        float m_startColorMin[4];   // Per-particle start color randomization range
        float m_startColorMax[4];
        bool m_startColorRandom;
        
        uint32_t m_particlesPerSecond;

        // ===== Rendering =====
        uint32_t m_blendMode;       // 0 = Alpha, 1 = Additive, 2 = Multiply
        int32_t m_renderOrder;      // Lower orders render first
        bool m_faceCamera;          // true = billboard, false = emitter-oriented quad
        
        // ===== Emission Mode =====
        bool m_burstMode;           // Burst vs continuous
        int m_burstCount;           // Particles per burst
        bool m_emitFromEdge;        // Emit from shape edge vs volume
        bool m_randomizeDirection;  // Randomize initial direction

        bx::Easing::Enum m_easePos;
        bx::Easing::Enum m_easeRgba;
        bx::Easing::Enum m_easeBlend;
        bx::Easing::Enum m_easeScale;

        EmitterSpriteHandle m_handle;
        
        // ===== Simulation Space =====
        bool m_localSpace;              // true = particles move with emitter, false = world space
        float m_worldMatrix[16];        // Emitter's world transform (for local space rendering)
    };

    // API functions (implemented in ParticleSystem.cpp)
    void init(uint16_t _maxEmitters = 64, bx::AllocatorI* _allocator = nullptr);
    void shutdown();

    EmitterSpriteHandle createSprite(uint16_t _width, uint16_t _height, const void* _data);
    void destroySprite(EmitterSpriteHandle _handle);

    EmitterHandle createEmitter(EmitterShape::Enum _shape, EmitterDirection::Enum _direction, uint32_t _maxParticles);
    void updateEmitter(EmitterHandle _handle, const EmitterUniforms* _uniforms = nullptr);
    void getAabb(EmitterHandle _handle, bx::Aabb& _outAabb);
    void destroyEmitter(EmitterHandle _handle);

    void update(float _dt);
    void updateEmitterParticles(EmitterHandle _handle, float _dt);

    // Renders all emitters using internal BGFX resources.
    // _view must be a valid BGFX view id, _mtxView is current view matrix, _eye is eye position in world space.
    void render(uint8_t _view, const float* _mtxView, const bx::Vec3& _eye);
    
    // Renders only emitters whose handle index is in the provided set.
    // Use this for scene-filtered rendering where only certain emitters should be drawn.
    void renderFiltered(uint8_t _view, const float* _mtxView, const bx::Vec3& _eye, 
                        const uint16_t* _handles, uint16_t _handleCount);

    // Validate handles against the live allocator (stronger than isValid()).
    bool IsEmitterAlive(EmitterHandle handle);
    bool IsSpriteAlive(EmitterSpriteHandle handle);

    // Get the current number of active particles in an emitter
    uint32_t GetEmitterParticleCount(EmitterHandle handle);
    
    // Sprite residency diagnostics
    uint16_t GetSpriteSlotCount();
    uint16_t GetSpriteSlotCapacity();
    // Legacy diagnostics kept for compatibility with existing callsites.
    uint16_t GetSpriteAtlasSize();
    uint16_t GetSpriteCount();
    uint16_t GetSpriteCapacity();

    bool GetSpriteUV(EmitterSpriteHandle sprite, float uv[4]);
    bool IsSpriteArrayBacked(EmitterSpriteHandle sprite);
    bgfx::TextureHandle GetSpriteTexture(EmitterSpriteHandle sprite);
    bgfx::TextureHandle GetTexture();

} // namespace ps
