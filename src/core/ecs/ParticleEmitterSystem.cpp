#include "ParticleEmitterSystem.h"
#include "Scene.h"
#include "Components.h"
#include "Entity.h"
#include <filesystem>
#include <algorithm>
#include <vector>
#include <iostream>
#include <glm/gtc/type_ptr.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#ifndef CLAYMORE_RUNTIME
#include "editor/EnginePaths.h"
#include "editor/Project.h"
#endif
#include "core/particles/SpriteLoader.h"
#include "core/world/RuntimeWorld.h"

namespace ecs 
{
    namespace
    {
        template <typename Fn>
        void ForEachEmitterEntity(Scene& scene, Fn&& fn)
        {
            if (const auto* runtimeWorld = scene.GetRuntimeWorld())
            {
                for (EntityID id : runtimeWorld->GetEmitterSceneEntities())
                {
                    fn(id);
                }
                return;
            }

            for (const auto& entity : scene.GetEntities())
            {
                fn(entity.GetID());
            }
        }
    }

    ParticleEmitterSystem& ParticleEmitterSystem::Get()
    {
        static ParticleEmitterSystem s_Instance;
        return s_Instance;
    }

    void ParticleEmitterSystem::Init()
    {
        if (!m_Initialized)
        {
            ps::init(256); // Allow up to 256 emitters by default.
            m_Initialized = true;
            std::cout << "[ParticleEmitterSystem] Initialized - max 256 emitters" << std::endl;
        }
    }

    void ParticleEmitterSystem::Shutdown()
    {
        if (m_Initialized)
        {
            m_EmitterOwnership.clear();
            particles::ClearSpriteCache(); // Clear sprite cache before shutdown
            ps::shutdown();
            m_Initialized = false;
        }
    }
    
    // Convert component shape enum to ps::EmitterShape
    static ps::EmitterShape::Enum ConvertShape(ParticleEmissionShape shape)
    {
        switch (shape)
        {
            case ParticleEmissionShape::Point:      return ps::EmitterShape::Point;
            case ParticleEmissionShape::Sphere:     return ps::EmitterShape::Sphere;
            case ParticleEmissionShape::Hemisphere: return ps::EmitterShape::Hemisphere;
            case ParticleEmissionShape::Cone:       return ps::EmitterShape::Cone;
            case ParticleEmissionShape::Box:        return ps::EmitterShape::Box;
            case ParticleEmissionShape::Circle:     return ps::EmitterShape::Circle;
            case ParticleEmissionShape::Disc:       return ps::EmitterShape::Disc;
            case ParticleEmissionShape::Edge:       return ps::EmitterShape::Edge;
            case ParticleEmissionShape::Rectangle:  return ps::EmitterShape::Rect;
            default:                                return ps::EmitterShape::Cone;
        }
    }
    
    // Convert component blend mode to uniform value
    static uint32_t ConvertBlendMode(ParticleBlendMode mode)
    {
        return static_cast<uint32_t>(mode);
    }
    
    // Pack RGBA color to uint32 (ABGR format for BGFX)
    static uint32_t PackColorABGR(const glm::vec4& color)
    {
        uint8_t r = static_cast<uint8_t>(glm::clamp(color.r, 0.0f, 1.0f) * 255.0f);
        uint8_t g = static_cast<uint8_t>(glm::clamp(color.g, 0.0f, 1.0f) * 255.0f);
        uint8_t b = static_cast<uint8_t>(glm::clamp(color.b, 0.0f, 1.0f) * 255.0f);
        uint8_t a = static_cast<uint8_t>(glm::clamp(color.a, 0.0f, 1.0f) * 255.0f);
        return (a << 24) | (b << 16) | (g << 8) | r;
    }
    
    static glm::vec3 ResolveEmitterVelocity(
        ParticleEmitterComponent& comp,
        const TransformComponent& transform,
        const RigidBodyComponent* rigidBody,
        float dt)
    {
        const glm::vec3 worldPos = glm::vec3(transform.WorldMatrix[3]);
        glm::vec3 emitterVelocity(0.0f);

        if (rigidBody != nullptr)
        {
            emitterVelocity = rigidBody->LinearVelocity;
        }
        else if (comp.HasPreviousWorldPosition && dt > 0.0001f)
        {
            emitterVelocity = (worldPos - comp.PreviousWorldPosition) / dt;
        }

        comp.PreviousWorldPosition = worldPos;
        comp.HasPreviousWorldPosition = true;
        return emitterVelocity;
    }

    static void PrewarmEmitter(ParticleEmitterComponent& comp)
    {
        if (!comp.Prewarm || comp.HasPrewarmed || !comp.IsPlaying || !ps::isValid(comp.Handle))
            return;

        const float prewarmSeconds = glm::max(0.0f, comp.Duration);
        if (prewarmSeconds <= 0.0f)
        {
            comp.HasPrewarmed = true;
            return;
        }

        constexpr float kStep = 1.0f / 60.0f;
        float remaining = prewarmSeconds;
        while (remaining > 0.0f)
        {
            const float step = glm::min(kStep, remaining);
            ps::updateEmitter(comp.Handle, &comp.Uniforms);
            ps::updateEmitterParticles(comp.Handle, step);
            remaining -= step;
        }
        comp.ElapsedTime = comp.Looping ? 0.0f : glm::min(comp.ElapsedTime, comp.Duration);
        comp.HasEmitted = true;
        comp.HasPrewarmed = true;
    }

    // Sync component properties to uniforms
    static void SyncComponentToUniforms(
        ParticleEmitterComponent& comp,
        const TransformComponent& transform,
        const glm::vec3& emitterVelocity = glm::vec3(0.0f))
    {
        auto& u = comp.Uniforms;
        
        // Use WORLD position from WorldMatrix so parented emitters work correctly
        // WorldMatrix is computed during Scene::UpdateTransforms() and includes parent hierarchy
        const glm::mat4& world = transform.WorldMatrix;
        glm::vec3 worldPos = glm::vec3(world[3]);
        u.m_position[0] = worldPos.x;
        u.m_position[1] = worldPos.y;
        u.m_position[2] = worldPos.z;
        
        glm::vec3 emitterRotation(0.0f);
        if (comp.SimulationSpace == ParticleSimulationSpace::Local)
        {
            // Local-space particles should fully inherit the emitter hierarchy.
            glm::vec3 scale, skew;
            glm::vec4 perspective;
            glm::quat rotationQuat;
            glm::vec3 translation;
            glm::decompose(
                world,
                scale,
                rotationQuat,
                translation,
                skew,
                perspective);
            emitterRotation = glm::eulerAngles(glm::normalize(rotationQuat));
        }
        else
        {
            // World-space particles should spawn from the emitter's world position,
            // but their authored emission direction should not be re-steered by an
            // attached parent or bone attachment.
            const glm::quat localRotation = transform.UseQuatRotation
                ? glm::normalize(transform.RotationQ)
                : glm::quat_cast(glm::yawPitchRoll(
                    glm::radians(transform.Rotation.y),
                    glm::radians(transform.Rotation.x),
                    glm::radians(transform.Rotation.z)));
            emitterRotation = glm::eulerAngles(localRotation);
        }
        u.m_angle[0] = emitterRotation.x;
        u.m_angle[1] = emitterRotation.y;
        u.m_angle[2] = emitterRotation.z;
        
        // Default direction (up)
        u.m_direction[0] = 0.0f;
        u.m_direction[1] = 1.0f;
        u.m_direction[2] = 0.0f;
        
        // Shape parameters
        u.m_shapeRadius = comp.ShapeRadius;
        u.m_shapeRadiusThickness = comp.ShapeRadiusThickness;
        u.m_shapeAngle = glm::radians(comp.ShapeAngle);
        u.m_shapeArc = glm::radians(comp.ShapeArc);
        u.m_shapeScale[0] = comp.ShapeScale.x;
        u.m_shapeScale[1] = comp.ShapeScale.y;
        u.m_shapeScale[2] = comp.ShapeScale.z;
        u.m_shapeLength = comp.ShapeLength;
        u.m_emitFromEdge = comp.ShapeEmitFromEdge;
        u.m_randomizeDirection = comp.ShapeRandomizeDirection;
        
        // Emission rate - only emit if playing
        if (comp.IsPlaying && !comp.BurstEnabled)
        {
            u.m_particlesPerSecond = static_cast<uint32_t>(comp.EmissionRate);
        }
        else
        {
            u.m_particlesPerSecond = 0;
        }
        u.m_burstMode = comp.BurstEnabled;
        u.m_burstCount = 0;
        
        // Lifetime
        u.m_lifeSpan[0] = comp.Lifetime.Min;
        u.m_lifeSpan[1] = comp.Lifetime.Max;
        
        // Start parameters
        u.m_startSpeedMin = comp.StartSpeed.Min;
        u.m_startSpeedMax = comp.StartSpeed.Max;
        u.m_startSizeMin = comp.StartSize.Min;
        u.m_startSizeMax = comp.StartSize.Max;
        u.m_startRotationMin = glm::radians(comp.StartRotation.Min);
        u.m_startRotationMax = glm::radians(comp.StartRotation.Max);
        
        // Physics
        u.m_gravityScale = comp.GravityModifier;
        u.m_dragCoefficient = comp.DragCoefficient;
        u.m_inheritVelocity = comp.InheritVelocity;
        u.m_emitterVelocity[0] = emitterVelocity.x;
        u.m_emitterVelocity[1] = emitterVelocity.y;
        u.m_emitterVelocity[2] = emitterVelocity.z;
        
        // Velocity over lifetime
        if (comp.VelocityOverLifetimeEnabled)
        {
            u.m_linearVelocity[0] = comp.LinearVelocity.x;
            u.m_linearVelocity[1] = comp.LinearVelocity.y;
            u.m_linearVelocity[2] = comp.LinearVelocity.z;
            u.m_orbitalVelocity = comp.OrbitalVelocity;
            u.m_radialVelocity = comp.RadialVelocity;
        }
        else
        {
            u.m_linearVelocity[0] = u.m_linearVelocity[1] = u.m_linearVelocity[2] = 0.0f;
            u.m_orbitalVelocity = 0.0f;
            u.m_radialVelocity = 0.0f;
        }
        
        // Size over lifetime
        if (comp.SizeOverLifetimeEnabled)
        {
            u.m_sizeOverLifetimeStart = comp.SizeOverLifetime.StartValue;
            u.m_sizeOverLifetimeEnd = comp.SizeOverLifetime.EndValue;
            u.m_sizeOverLifetimeCurve = static_cast<ps::CurveType::Enum>(static_cast<int>(comp.SizeOverLifetime.CurveType));
        }
        else
        {
            u.m_sizeOverLifetimeStart = 1.0f;
            u.m_sizeOverLifetimeEnd = 1.0f;
            u.m_sizeOverLifetimeCurve = ps::CurveType::Constant;
        }
        
        // Rotation over lifetime
        u.m_angularVelocity = comp.RotationOverLifetimeEnabled ? glm::radians(comp.AngularVelocity) : 0.0f;
        u.m_alignToVelocity = comp.AlignWithTrajectory;
        
        const glm::vec4 startColorMin = comp.StartColorRandom ? comp.StartColorMin : comp.StartColor;
        const glm::vec4 startColorMax = comp.StartColorRandom ? comp.StartColorMax : comp.StartColor;
        u.m_startColorRandom = comp.StartColorRandom;
        u.m_startColorMin[0] = startColorMin.r;
        u.m_startColorMin[1] = startColorMin.g;
        u.m_startColorMin[2] = startColorMin.b;
        u.m_startColorMin[3] = startColorMin.a;
        u.m_startColorMax[0] = startColorMax.r;
        u.m_startColorMax[1] = startColorMax.g;
        u.m_startColorMax[2] = startColorMax.b;
        u.m_startColorMax[3] = startColorMax.a;

        // Color gradient - convert to ABGR packed format. Keep authored key
        // timing intact and fall back to a constant white gradient when the
        // module is disabled, leaving start color/randomization to the spawn step.
        std::vector<ParticleColorKey> colorKeys;
        if (comp.ColorOverLifetimeEnabled && !comp.ColorGradient.empty())
        {
            colorKeys = comp.ColorGradient;
            std::sort(colorKeys.begin(), colorKeys.end(),
                [](const ParticleColorKey& a, const ParticleColorKey& b) { return a.Time < b.Time; });
        }
        else
        {
            colorKeys.push_back({ 0.0f, glm::vec4(1.0f) });
        }

        int keyCount = std::min(5, static_cast<int>(colorKeys.size()));
        for (int i = 0; i < keyCount; ++i)
        {
            const auto& key = colorKeys[i];
            u.m_rgba[i] = PackColorABGR(key.Color);
            u.m_rgbaTime[i] = glm::clamp(key.Time, 0.0f, 1.0f);
        }
        // Fill remaining keys with last color if needed
        for (int i = keyCount; i < 5; ++i)
        {
            u.m_rgba[i] = keyCount > 0 ? u.m_rgba[keyCount - 1] : 0xffffffff;
            u.m_rgbaTime[i] = 1.0f;
        }
        u.m_rgbaKeyCount = keyCount;
        
        // Blend mode
        u.m_blendMode = ConvertBlendMode(comp.BlendMode);
        u.m_renderOrder = comp.RenderOrder;
        u.m_faceCamera = comp.FaceCamera;
        
        // Sprite handle
        u.m_handle = comp.SpriteHandle;
        
        // Simulation space - determines whether particles follow emitter or stay in world
        u.m_localSpace = (comp.SimulationSpace == ParticleSimulationSpace::Local);
        
        // Copy world matrix for local space rendering
        // Particles in local space need to be transformed by current world matrix at render time
        const float* worldPtr = glm::value_ptr(world);
        for (int i = 0; i < 16; ++i)
        {
            u.m_worldMatrix[i] = worldPtr[i];
        }
    }

    void ParticleEmitterSystem::Update(Scene& scene, float dt)
    {
        if (!m_Initialized)
            Init();

        if (!scene.GetRuntimeWorld() ||
            (scene.HasPendingRuntimeWorldStructuralSyncWork() && !scene.IsRuntimeWorldFrameSyncLocked())) {
            scene.SyncRuntimeWorld(false);
        }
        
            
        // Collect entities to destroy after iteration
        std::vector<EntityID> entitiesToDestroy;
            
        int foundEmitters = 0;
        ForEachEmitterEntity(scene, [&](EntityID id)
        {
            auto* dataPtr = scene.GetEntityData(id);
            if (!dataPtr || !dataPtr->Emitter) return;
            auto& emitterComp = *dataPtr->Emitter;

            // Recover from stale handles (e.g., after particle system reset)
            if (ps::isValid(emitterComp.Handle) && !ps::IsEmitterAlive(emitterComp.Handle))
            {
                std::cout << "[ParticleEmitterSystem] Detected stale emitter handle " << emitterComp.Handle.idx 
                          << " for entity " << id << " - clearing" << std::endl;
                UnregisterEmitterOwnership(emitterComp.Handle);
                emitterComp.Handle = { uint16_t{UINT16_MAX} };
            }
            if (ps::isValid(emitterComp.SpriteHandle) && !ps::IsSpriteAlive(emitterComp.SpriteHandle))
            {
                std::cout << "[ParticleEmitterSystem] Detected stale sprite handle " << emitterComp.SpriteHandle.idx 
                          << " for entity " << id << " - clearing" << std::endl;
                emitterComp.SpriteHandle = { uint16_t{UINT16_MAX} };
                emitterComp.Uniforms.m_handle = { uint16_t{UINT16_MAX} };
            }
            
            // Handle disabled emitters or invisible entities:
            // Destroy the underlying emitter so particles stop rendering immediately
            bool shouldBeActive = emitterComp.Enabled && dataPtr->Visible &&
                !dataPtr->PresentationHidden;
            if (!shouldBeActive)
            {
                if (ps::isValid(emitterComp.Handle))
                {
                    // Destroy the emitter to stop all particles immediately
                    std::cout << "[ParticleEmitterSystem] Destroying emitter handle " << emitterComp.Handle.idx 
                              << " for entity " << id << " (disabled or invisible)" << std::endl;
                    UnregisterEmitterOwnership(emitterComp.Handle);
                    ps::destroyEmitter(emitterComp.Handle);
                    emitterComp.Handle = { uint16_t{UINT16_MAX} };
                }
                // Reset play state so it can re-trigger PlayOnAwake when re-enabled
                emitterComp.IsPlaying = false;
                emitterComp.HasEmitted = false;
                emitterComp.HasPrewarmed = false;
                emitterComp.HasPreviousWorldPosition = false;
                emitterComp.PreviousWorldPosition = glm::vec3(0.0f);
                return;
            }
            
            // Check if shape or max particles changed - if so, recreate the emitter
            bool needsRecreate = false;
            bool wasJustCreated = false;
            if (ps::isValid(emitterComp.Handle))
            {
                if (emitterComp.Shape != emitterComp.CachedShape ||
                    emitterComp.MaxParticles != emitterComp.CachedMaxParticles ||
                    emitterComp.ShapeEmitFromEdge != emitterComp.CachedShapeEmitFromEdge)
                {
                    // Destroy old emitter
                    UnregisterEmitterOwnership(emitterComp.Handle);
                    ps::destroyEmitter(emitterComp.Handle);
                    emitterComp.Handle = { uint16_t{UINT16_MAX} };
                    needsRecreate = true;
                }
            }

            // Create emitter lazily or after recreation
            if (!ps::isValid(emitterComp.Handle))
            {
                ps::EmitterShape::Enum psShape = ConvertShape(emitterComp.Shape);
                ps::EmitterDirection::Enum psDir = emitterComp.ShapeEmitFromEdge 
                    ? ps::EmitterDirection::Outward 
                    : ps::EmitterDirection::Up;
                    
                emitterComp.Handle = ps::createEmitter(psShape, psDir, emitterComp.MaxParticles);
                
                if (!ps::isValid(emitterComp.Handle))
                {
                    std::cout << "[ParticleEmitterSystem] WARNING: Failed to create emitter for entity " << id 
                              << " - emitter pool exhausted (max 256 emitters)" << std::endl;
                }
                else
                {
                    std::cout << "[ParticleEmitterSystem] Created emitter handle " << emitterComp.Handle.idx 
                              << " for entity " << id << " (max particles: " << emitterComp.MaxParticles << ")" << std::endl;
                }
                
                // Cache the shape and max particles for change detection
                emitterComp.CachedShape = emitterComp.Shape;
                emitterComp.CachedMaxParticles = emitterComp.MaxParticles;
                emitterComp.CachedShapeEmitFromEdge = emitterComp.ShapeEmitFromEdge;
                
                // Register ownership for scene-filtered rendering
                RegisterEmitterOwnership(emitterComp.Handle, &scene);
                
                // Mark as just created - we'll sync transforms before allowing emission
                wasJustCreated = true;
                emitterComp.JustCreated = true;
            }
            else
            {
                // Ensure ownership stays registered in case the map was cleared.
                RegisterEmitterOwnership(emitterComp.Handle, &scene);
            }

            uint32_t burstCountThisFrame = 0;
            const RigidBodyComponent* rigidBody =
                dataPtr->RigidBody ? &*dataPtr->RigidBody : nullptr;
            const glm::vec3 emitterVelocity =
                ResolveEmitterVelocity(emitterComp, dataPtr->Transform, rigidBody, dt);
            
            // CRITICAL: Sync transforms BEFORE checking PlayOnAwake or updating elapsed time
            // This ensures particles are emitted at the correct position, especially on the first frame
            SyncComponentToUniforms(emitterComp, dataPtr->Transform, emitterVelocity);
            
            // Update emitter with synced uniforms immediately after creation to set correct position
            if (wasJustCreated && ps::isValid(emitterComp.Handle))
            {
                // Ensure sprite handle is synced to uniforms
                emitterComp.Uniforms.m_handle = emitterComp.SpriteHandle;
                ps::updateEmitter(emitterComp.Handle, &emitterComp.Uniforms);
            }
            
            // Auto-play on awake (now that transforms are synced)
            // CRITICAL: Skip emission on the first frame after creation to ensure transforms are fully synced
            // This prevents particles from being emitted at the wrong position when play mode starts
            if (emitterComp.JustCreated)
            {
                // On first frame after creation, just sync transforms and mark that we're ready
                // Don't trigger PlayOnAwake yet - wait until next frame
                emitterComp.JustCreated = false;
            }
            else if (emitterComp.PlayOnAwake && !emitterComp.HasEmitted && !emitterComp.IsPlaying)
            {
                // Trigger PlayOnAwake on second frame (after transforms are synced)
                emitterComp.Play();
            }
            
            // Continuously try to load sprite from path if SpriteHandle is invalid but SpritePath is set
            // This handles cases where the sprite couldn't be loaded at creation time (path resolution, file not ready, etc.)
            if (!ps::isValid(emitterComp.SpriteHandle) && !emitterComp.SpritePath.empty()) {
                // Try loading with the path as-is first
                auto loadedSprite = particles::AcquireSprite(emitterComp.SpritePath);
                
                // If that fails, try resolving relative to project directory
                if (!ps::isValid(loadedSprite)) {
#ifndef CLAYMORE_RUNTIME
                    std::filesystem::path projectDir = Project::GetProjectDirectory();
                    if (!projectDir.empty()) {
                        std::filesystem::path resolvedPath = projectDir / emitterComp.SpritePath;
                        if (std::filesystem::exists(resolvedPath)) {
                            loadedSprite = particles::AcquireSprite(resolvedPath.string());
                        }
                    }
#endif
                }
                
                if (ps::isValid(loadedSprite)) {
                    emitterComp.SpriteHandle = loadedSprite;
                    std::cout << "[ParticleEmitterSystem] Loaded sprite handle " << loadedSprite.idx 
                              << " for entity " << id << " (path: " << emitterComp.SpritePath << ")" << std::endl;
                }
                else
                {
                    std::cout << "[ParticleEmitterSystem] WARNING: Failed to load sprite for entity " << id 
                              << " (path: " << emitterComp.SpritePath
                              << ") - missing file or sprite allocation failed (see [SpriteLoader] details)" << std::endl;
                }
            }
            
            // Only use default sprite if no custom SpritePath is specified
            if (!ps::isValid(emitterComp.SpriteHandle) && emitterComp.SpritePath.empty()) {
#ifndef CLAYMORE_RUNTIME
                static bool s_DefaultResolved = false;
                static ps::EmitterSpriteHandle s_DefaultSprite{ uint16_t{UINT16_MAX} };
                if (ps::isValid(s_DefaultSprite) && !ps::IsSpriteAlive(s_DefaultSprite)) {
                    s_DefaultSprite = { uint16_t{UINT16_MAX} };
                    s_DefaultResolved = false;
                }
                if (!s_DefaultResolved) {
                    std::filesystem::path exeAssets = EnginePaths::GetEngineAssetPath();
                    std::filesystem::path particlesDir = exeAssets / "particles";
                    if (std::filesystem::exists(particlesDir)) {
                        for (auto& entry : std::filesystem::directory_iterator(particlesDir)) {
                            if (!entry.is_regular_file()) continue;
                            auto ext = entry.path().extension().string();
                            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") {
                                s_DefaultSprite = particles::AcquireSprite(entry.path().string());
                                break;
                            }
                        }
                    }
                    s_DefaultResolved = true;
                }
                if (ps::isValid(s_DefaultSprite)) {
                    emitterComp.SpriteHandle = s_DefaultSprite;
                }
#endif
                // In runtime, particle sprites should be pre-loaded from PAK
            }
            
            // Update elapsed time (keep ticking after emission ends if we need to auto-destroy)
            const bool allowPostLifetime =
                emitterComp.DestroyOnComplete &&
                !emitterComp.Looping &&
                emitterComp.HasEmitted &&
                emitterComp.ElapsedTime >= emitterComp.Duration;

            if (emitterComp.IsPlaying || allowPostLifetime)
            {
                emitterComp.ElapsedTime += dt;
            }
            if (emitterComp.IsPlaying)
            {
                emitterComp.HasEmitted = true;
                
                // Handle burst emission
                if (emitterComp.BurstEnabled && emitterComp.ElapsedTime >= emitterComp.NextBurstTime)
                {
                    if (emitterComp.BurstCyclesRemaining > 0 || (emitterComp.Looping && emitterComp.BurstCycles == 0))
                    {
                        burstCountThisFrame = static_cast<uint32_t>(glm::max(0, emitterComp.BurstCount));
                        
                        emitterComp.NextBurstTime = emitterComp.ElapsedTime + emitterComp.BurstInterval;
                        if (emitterComp.BurstCyclesRemaining > 0)
                            emitterComp.BurstCyclesRemaining--;
                    }
                }
                
                // Loop reset
                if (emitterComp.Looping && emitterComp.ElapsedTime >= emitterComp.Duration)
                {
                    emitterComp.ElapsedTime = 0.0f;
                    emitterComp.BurstCyclesRemaining = emitterComp.BurstCycles;
                    emitterComp.NextBurstTime = emitterComp.BurstTime;
                }
            }

            // Check duration/completion regardless of playing state so DestroyOnComplete can finish
            if (!emitterComp.Looping && emitterComp.ElapsedTime >= emitterComp.Duration)
            {
                if (emitterComp.IsPlaying && emitterComp.StopEmittingOnComplete)
                {
                    emitterComp.Stop();
                }
                
                // Check for auto-destroy (wait for particles to die first)
                if (emitterComp.DestroyOnComplete)
                {
                    // Wait additional time for particles to fade
                    float maxLifetime = emitterComp.Lifetime.Max;
                    if (emitterComp.ElapsedTime >= emitterComp.Duration + maxLifetime)
                    {
                        entitiesToDestroy.push_back(id);
                    }
                }
            }

            // Auto-free emitter handles when finished emitting and all particles are dead
            // This prevents exhausting the emitter handle pool (256 max) when spawning many systems
            if (ps::isValid(emitterComp.Handle) && !emitterComp.Looping && !emitterComp.IsPlaying)
            {
                // Check if emission has finished and enough time has passed for particles to die
                float maxLifetime = emitterComp.Lifetime.Max;
                bool emissionFinished = emitterComp.HasEmitted && emitterComp.ElapsedTime >= emitterComp.Duration;
                bool particlesShouldBeDead = emitterComp.ElapsedTime >= emitterComp.Duration + maxLifetime;
                
                if (emissionFinished && particlesShouldBeDead)
                {
                    // Check if there are actually no particles left
                    uint32_t particleCount = ps::GetEmitterParticleCount(emitterComp.Handle);
                    if (particleCount == 0)
                    {
                        // Free the emitter handle to make it available for new systems
                        std::cout << "[ParticleEmitterSystem] Auto-freeing emitter handle " << emitterComp.Handle.idx 
                                  << " for entity " << id << " (finished emitting, all particles dead)" << std::endl;
                        
                        // Release sprite handle if it exists (ref-counted, will free atlas space when ref count reaches 0)
                        if (!emitterComp.SpritePath.empty() && ps::isValid(emitterComp.SpriteHandle))
                        {
                            particles::ReleaseSprite(emitterComp.SpriteHandle);
                            emitterComp.SpriteHandle = { uint16_t{UINT16_MAX} };
                            emitterComp.Uniforms.m_handle = { uint16_t{UINT16_MAX} };
                            std::cout << "[ParticleEmitterSystem] Released sprite handle for entity " << id << std::endl;
                        }
                        
                        UnregisterEmitterOwnership(emitterComp.Handle);
                        ps::destroyEmitter(emitterComp.Handle);
                        emitterComp.Handle = { uint16_t{UINT16_MAX} };
                        // Reset state so emitter can be restarted later
                        emitterComp.HasEmitted = false;
                        emitterComp.ElapsedTime = 0.0f;
                        emitterComp.HasPreviousWorldPosition = false;
                        emitterComp.PreviousWorldPosition = glm::vec3(0.0f);
                    }
                }
            }

            // Sync all component properties to uniforms (already done above for newly created emitters)
            // Only sync again if emitter wasn't just created (to avoid double-sync)
            if (!wasJustCreated)
            {
                SyncComponentToUniforms(emitterComp, dataPtr->Transform, emitterVelocity);
            }
            if (burstCountThisFrame > 0)
            {
                emitterComp.Uniforms.m_burstMode = true;
                emitterComp.Uniforms.m_burstCount = static_cast<int>(burstCountThisFrame);
                emitterComp.Uniforms.m_particlesPerSecond = 0;
            }

            // Only update emitter if it still has a valid handle
            // CRITICAL: If createEmitter failed (pool exhausted), Handle will be invalid and we skip update
            if (ps::isValid(emitterComp.Handle))
            {
                // Ensure sprite handle is synced to uniforms (in case sprite was reloaded or emitter was recreated)
                emitterComp.Uniforms.m_handle = emitterComp.SpriteHandle;
                ps::updateEmitter(emitterComp.Handle, &emitterComp.Uniforms);
                PrewarmEmitter(emitterComp);
            }
            foundEmitters++;
        });
        
        
        // Destroy entities marked for auto-destroy
        for (EntityID id : entitiesToDestroy)
        {
            scene.DestroyEntity(id);
        }

        // Step particle simulation once per frame.
        ps::update(dt);
    }

    void ParticleEmitterSystem::SyncEmittersOnly(Scene& scene)
    {
        if (!m_Initialized)
            Init();
        if (!scene.GetRuntimeWorld() ||
            (scene.HasPendingRuntimeWorldStructuralSyncWork() && !scene.IsRuntimeWorldFrameSyncLocked())) {
            scene.SyncRuntimeWorld(false);
        }
        // Iterate all emitter entities and sync uniforms with transform, but don't step simulation.
        ForEachEmitterEntity(scene, [&](EntityID id)
        {
            auto* dataPtr = scene.GetEntityData(id);
            if (!dataPtr || !dataPtr->Emitter) return;
            auto& emitterComp = *dataPtr->Emitter;

            // Recover from stale handles (e.g., after particle system reset)
            if (ps::isValid(emitterComp.Handle) && !ps::IsEmitterAlive(emitterComp.Handle))
            {
                UnregisterEmitterOwnership(emitterComp.Handle);
                emitterComp.Handle = { uint16_t{UINT16_MAX} };
            }
            if (ps::isValid(emitterComp.SpriteHandle) && !ps::IsSpriteAlive(emitterComp.SpriteHandle))
            {
                emitterComp.SpriteHandle = { uint16_t{UINT16_MAX} };
                emitterComp.Uniforms.m_handle = { uint16_t{UINT16_MAX} };
            }
            
            // Handle disabled emitters or invisible entities:
            // Destroy the underlying emitter so particles stop rendering immediately
            bool shouldBeActive = emitterComp.Enabled && dataPtr->Visible &&
                !dataPtr->PresentationHidden;
            if (!shouldBeActive)
            {
                if (ps::isValid(emitterComp.Handle))
                {
                    UnregisterEmitterOwnership(emitterComp.Handle);
                    ps::destroyEmitter(emitterComp.Handle);
                    emitterComp.Handle = { uint16_t{UINT16_MAX} };
                }
                emitterComp.IsPlaying = false;
                emitterComp.HasEmitted = false;
                emitterComp.HasPrewarmed = false;
                return;
            }

            // Check if shape or max particles changed - if so, recreate the emitter
            if (ps::isValid(emitterComp.Handle))
            {
                if (emitterComp.Shape != emitterComp.CachedShape ||
                    emitterComp.MaxParticles != emitterComp.CachedMaxParticles ||
                    emitterComp.ShapeEmitFromEdge != emitterComp.CachedShapeEmitFromEdge)
                {
                    UnregisterEmitterOwnership(emitterComp.Handle);
                    ps::destroyEmitter(emitterComp.Handle);
                    emitterComp.Handle = { uint16_t{UINT16_MAX} };
                }
            }
            
            // Create emitter lazily if needed
            if (!ps::isValid(emitterComp.Handle))
            {
                ps::EmitterShape::Enum psShape = ConvertShape(emitterComp.Shape);
                ps::EmitterDirection::Enum psDir = emitterComp.ShapeEmitFromEdge
                    ? ps::EmitterDirection::Outward
                    : ps::EmitterDirection::Up;
                emitterComp.Handle = ps::createEmitter(psShape, psDir, emitterComp.MaxParticles);
                
                // Cache the shape and max particles for change detection
                emitterComp.CachedShape = emitterComp.Shape;
                emitterComp.CachedMaxParticles = emitterComp.MaxParticles;
                emitterComp.CachedShapeEmitFromEdge = emitterComp.ShapeEmitFromEdge;
                
                // Register ownership for scene-filtered rendering
                RegisterEmitterOwnership(emitterComp.Handle, &scene);
            }
            else
            {
                // Ensure ownership stays registered in case the map was cleared.
                RegisterEmitterOwnership(emitterComp.Handle, &scene);
            }
            
            // Continuously try to load sprite from path if SpriteHandle is invalid but SpritePath is set
            if (!ps::isValid(emitterComp.SpriteHandle) && !emitterComp.SpritePath.empty()) {
                auto loadedSprite = particles::AcquireSprite(emitterComp.SpritePath);
                
                // If that fails, try resolving relative to project directory
                if (!ps::isValid(loadedSprite)) {
#ifndef CLAYMORE_RUNTIME
                    std::filesystem::path projectDir = Project::GetProjectDirectory();
                    if (!projectDir.empty()) {
                        std::filesystem::path resolvedPath = projectDir / emitterComp.SpritePath;
                        if (std::filesystem::exists(resolvedPath)) {
                            loadedSprite = particles::AcquireSprite(resolvedPath.string());
                        }
                    }
#endif
                }
                
                if (ps::isValid(loadedSprite)) {
                    emitterComp.SpriteHandle = loadedSprite;
                }
            }
            
            emitterComp.Uniforms.m_handle = emitterComp.SpriteHandle;

            // Sync all component properties
            SyncComponentToUniforms(emitterComp, dataPtr->Transform);

            ps::updateEmitter(emitterComp.Handle, &emitterComp.Uniforms);
        });
        // NOTE: Don't call ps::update() - let the main scene's Update() handle that
    }

    void ParticleEmitterSystem::Render(uint8_t viewId, const float* mtxView, const bx::Vec3& eye)
    {
        if (!m_Initialized) return;
        ps::render(viewId, mtxView, eye);
    }
    
    void ParticleEmitterSystem::RenderAllUnfiltered(uint8_t viewId, const float* mtxView, const bx::Vec3& eye)
    {
        if (!m_Initialized) return;
        ps::render(viewId, mtxView, eye);
    }
    
    void ParticleEmitterSystem::Render(uint8_t viewId, const float* mtxView, const bx::Vec3& eye, Scene* filterScene)
    {
        if (!m_Initialized) return;
        
        // If no filter scene, render all (backwards compatible)
        if (!filterScene)
        {
            ps::render(viewId, mtxView, eye);
            return;
        }
        
        // Collect handles belonging to this scene
        std::vector<uint16_t> sceneHandles;
        sceneHandles.reserve(m_EmitterOwnership.size());
        
        for (const auto& [handleIdx, ownerScene] : m_EmitterOwnership)
        {
            if (ownerScene == filterScene)
            {
                sceneHandles.push_back(handleIdx);
            }
        }
        
        if (sceneHandles.empty()) return;
        
        ps::renderFiltered(viewId, mtxView, eye, sceneHandles.data(), static_cast<uint16_t>(sceneHandles.size()));
    }
    
    void ParticleEmitterSystem::RegisterEmitterOwnership(ps::EmitterHandle handle, Scene* scene)
    {
        if (!ps::isValid(handle)) return;
        m_EmitterOwnership[handle.idx] = scene;
    }
    
    void ParticleEmitterSystem::UnregisterEmitterOwnership(ps::EmitterHandle handle)
    {
        if (!ps::isValid(handle)) return;
        m_EmitterOwnership.erase(handle.idx);
    }
    
    void ParticleEmitterSystem::ClearSceneEmitters(Scene* scene)
    {
        if (!scene) return;
        
        // Remove all entries for this scene
        for (auto it = m_EmitterOwnership.begin(); it != m_EmitterOwnership.end(); )
        {
            if (it->second == scene)
            {
                it = m_EmitterOwnership.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}
