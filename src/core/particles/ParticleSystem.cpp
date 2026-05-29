/*
 * ParticleSystem.cpp - Integrated BGFX particle system for Claymore engine.
 * This file is largely based on the BGFX example particle system by Branimir Karadzic.
 * It has been adapted to fit Claymore's build system and code style.
 */

#include "ParticleSystem.h"

#include <bgfx/bgfx.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <bx/easing.h>
#include <bx/rng.h>
#include <bx/handlealloc.h>
#include <cfloat>
#include <bx/math.h>
#include <cmath>
#include "core/rendering/ShaderManager.h"
#include <atomic>
#include <iostream>
#include <array>
#include <unordered_set>
#include <vector>
#include "core/jobs/ParallelFor.h"
#include "core/jobs/Jobs.h"

namespace ps
{
    // --------------------------------------------------------------
    // Vertex layout for particles
    // --------------------------------------------------------------
    struct PosColorTexCoord0Vertex
    {
        float m_x;
        float m_y;
        float m_z;
        uint32_t m_abgr;
        float m_u;
        float m_v;
        float m_blend;
        float m_angle;

        static bgfx::VertexLayout ms_layout;
        static void init()
        {
            ms_layout
                .begin()
                .add(bgfx::Attrib::Position,  3, bgfx::AttribType::Float)
                .add(bgfx::Attrib::Color0,    4, bgfx::AttribType::Uint8, true)
                .add(bgfx::Attrib::TexCoord0, 4, bgfx::AttribType::Float)
                .end();
        }
    };

    bgfx::VertexLayout PosColorTexCoord0Vertex::ms_layout;

    // Helper conversions ------------------------------------------------------
    inline uint32_t toAbgr(float _rr, float _gg, float _bb, float _aa)
    {
        return 0
            | (uint8_t(bx::clamp(_rr, 0.0f, 1.0f)*255.0f)<< 0)
            | (uint8_t(bx::clamp(_gg, 0.0f, 1.0f)*255.0f)<< 8)
            | (uint8_t(bx::clamp(_bb, 0.0f, 1.0f)*255.0f)<<16)
            | (uint8_t(bx::clamp(_aa, 0.0f, 1.0f)*255.0f)<<24);
    }

    inline uint32_t toAbgr(const float* _rgba)
    {
        return toAbgr(_rgba[0], _rgba[1], _rgba[2], _rgba[3]);
    }

    inline void fromAbgr(uint32_t _abgr, float* _rgba)
    {
        _rgba[0] = float((_abgr >>  0) & 0xff) / 255.0f;
        _rgba[1] = float((_abgr >>  8) & 0xff) / 255.0f;
        _rgba[2] = float((_abgr >> 16) & 0xff) / 255.0f;
        _rgba[3] = float((_abgr >> 24) & 0xff) / 255.0f;
    }

    inline uint32_t modulateAbgr(uint32_t _abgr, const float* _rgbaMul)
    {
        float rgba[4];
        fromAbgr(_abgr, rgba);
        return toAbgr(
            rgba[0] * _rgbaMul[0],
            rgba[1] * _rgbaMul[1],
            rgba[2] * _rgbaMul[2],
            rgba[3] * _rgbaMul[3]);
    }

    // Emitter uniforms default ------------------------------------------------
    void EmitterUniforms::reset()
    {
        // Transform
        m_position[0] = m_position[1] = m_position[2] = 0.0f;
        m_angle[0] = m_angle[1] = m_angle[2] = 0.0f;
        m_direction[0] = 0.0f; m_direction[1] = 1.0f; m_direction[2] = 0.0f; // Up

        // Shape parameters
        m_shapeRadius = 1.0f;
        m_shapeRadiusThickness = 1.0f;
        m_shapeAngle = 0.436f;      // 25 degrees in radians
        m_shapeArc = 6.283f;        // 360 degrees in radians
        m_shapeScale[0] = m_shapeScale[1] = m_shapeScale[2] = 1.0f;
        m_shapeLength = 2.0f;

        m_particlesPerSecond = 100;

        // Legacy offset/scale
        m_offsetStart[0] = 0.0f; m_offsetStart[1] = 0.5f;
        m_offsetEnd[0]   = 1.0f; m_offsetEnd[1]   = 2.0f;

        // Start parameters
        m_startSpeedMin = 2.0f;
        m_startSpeedMax = 5.0f;
        m_startSizeMin = 0.1f;
        m_startSizeMax = 0.3f;
        m_startRotationMin = 0.0f;
        m_startRotationMax = 6.283f; // 360 degrees

        // Color gradient: fade in, solid, fade out
        m_rgba[0] = 0x00ffffff;  // Transparent white at start
        m_rgba[1] = 0xffffffff;  // Full white
        m_rgba[2] = 0xffffffff;  // Full white
        m_rgba[3] = 0xffffffff;  // Full white
        m_rgba[4] = 0x00ffffff;  // Transparent white at end

        m_rgbaTime[0] = 0.0f;
        m_rgbaTime[1] = 0.1f;
        m_rgbaTime[2] = 0.5f;
        m_rgbaTime[3] = 0.9f;
        m_rgbaTime[4] = 1.0f;
        m_rgbaKeyCount = 5;
        for (int i = 0; i < 4; ++i)
        {
            m_startColorMin[i] = 1.0f;
            m_startColorMax[i] = 1.0f;
        }
        m_startColorRandom = false;

        m_blendStart[0] = 0.8f; m_blendStart[1] = 1.0f;
        m_blendEnd[0]   = 0.0f; m_blendEnd[1]   = 0.2f;

        m_scaleStart[0] = 0.1f; m_scaleStart[1] = 0.3f;
        m_scaleEnd[0]   = 0.0f; m_scaleEnd[1]   = 0.1f;

        m_lifeSpan[0] = 3.0f; m_lifeSpan[1] = 5.0f;

        // Physics
        m_gravityScale = 0.0f;
        m_dragCoefficient = 0.0f;
        m_inheritVelocity = 0.0f;
        m_emitterVelocity[0] = m_emitterVelocity[1] = m_emitterVelocity[2] = 0.0f;
        
        // Velocity over lifetime
        m_linearVelocity[0] = m_linearVelocity[1] = m_linearVelocity[2] = 0.0f;
        m_orbitalVelocity = 0.0f;
        m_radialVelocity = 0.0f;
        
        // Size over lifetime
        m_sizeOverLifetimeStart = 1.0f;
        m_sizeOverLifetimeEnd = 0.0f;
        m_sizeOverLifetimeCurve = CurveType::Linear;
        
        // Rotation over lifetime
        m_angularVelocity = 0.0f;
        m_alignToVelocity = false;
        
        // Emission mode
        m_burstMode = false;
        m_burstCount = 10;
        m_emitFromEdge = false;
        m_randomizeDirection = false;

        m_easePos   = bx::Easing::Linear;
        m_easeRgba  = bx::Easing::Linear;
        m_easeBlend = bx::Easing::Linear;
        m_easeScale = bx::Easing::Linear;

        m_handle.idx = UINT16_MAX; // invalid
        m_blendMode = 1; // Additive by default for better particle visuals
        m_renderOrder = 0;
        m_faceCamera = true;
        
        // Simulation space
        m_localSpace = false;  // World space by default
        // Initialize world matrix to identity
        bx::mtxIdentity(m_worldMatrix);
    }

    // -------------------------------------------------------------------------
    // Forward declarations
    struct Emitter;

    static int32_t particleSortFn(const void* _lhs, const void* _rhs);

    // -------------------------------------------------------------------------
    // Sprite residency handling (hybrid: texture arrays + dedicated fallback)
    // -------------------------------------------------------------------------
    #define MAX_PARTICLE_SPRITES 1024
    #define PARTICLE_ARRAY_PREFERRED_LAYERS 32
    #define PARTICLE_ARRAY_MIN_LAYERS 4

    template<uint16_t MaxHandlesT = MAX_PARTICLE_SPRITES>
    struct SpriteT
    {
        enum class StorageKind : uint8_t
        {
            Invalid = 0,
            Dedicated2D,
            Array2D
        };

        struct SpriteEntry
        {
            StorageKind         kind{ StorageKind::Invalid };
            bgfx::TextureHandle texture{ BGFX_INVALID_HANDLE };
            uint16_t            arrayBucket{ UINT16_MAX };
            uint16_t            arrayLayer{ 0 };
        };

        struct ArrayBucket
        {
            bgfx::TextureHandle    texture{ BGFX_INVALID_HANDLE };
            uint16_t               width{ 0 };
            uint16_t               height{ 0 };
            uint16_t               maxLayers{ 0 };
            uint16_t               nextLayer{ 0 };
            uint16_t               activeSprites{ 0 };
            std::vector<uint16_t>  freeLayers;
            std::vector<uint16_t>  layerRefCounts;
        };

        SpriteT()
        {
            for (auto& entry : m_entries)
            {
                entry.kind = StorageKind::Invalid;
                entry.texture = BGFX_INVALID_HANDLE;
                entry.arrayBucket = UINT16_MAX;
                entry.arrayLayer = 0;
            }
        }

        void setArrayBackendEnabled(bool _enabled)
        {
            m_arrayBackendEnabled = _enabled;
        }

        EmitterSpriteHandle create(uint16_t _width, uint16_t _height, const void* _data)
        {
            EmitterSpriteHandle handle = { UINT16_MAX };
            if (!_data) return handle;
            if (m_handleAlloc.getNumHandles() >= m_handleAlloc.getMaxHandles())
            {
                return handle;
            }

            SpriteEntry entry;
            if (!(m_arrayBackendEnabled && tryCreateArraySprite(_width, _height, _data, entry)))
            {
                if (!tryCreateDedicatedSprite(_width, _height, _data, entry))
                {
                    return handle;
                }
            }

            handle.idx = m_handleAlloc.alloc();
            m_entries[handle.idx] = entry;
            return handle;
        }

        void destroy(EmitterSpriteHandle _sprite)
        {
            if (!isValid(_sprite)) return;
            releaseEntry(m_entries[_sprite.idx]);
            m_entries[_sprite.idx] = SpriteEntry{};
            m_handleAlloc.free(_sprite.idx);
        }

        void destroyAll()
        {
            // Destroy dedicated textures still referenced by live handles.
            for (uint16_t idx = 0; idx < MaxHandlesT; ++idx)
            {
                if (!m_handleAlloc.isValid(idx))
                    continue;
                SpriteEntry& entry = m_entries[idx];
                if (entry.kind == StorageKind::Dedicated2D && bgfx::isValid(entry.texture))
                {
                    bgfx::destroy(entry.texture);
                }
                entry = SpriteEntry{};
            }

            // Destroy all texture array buckets.
            for (ArrayBucket& bucket : m_arrayBuckets)
            {
                if (bgfx::isValid(bucket.texture))
                {
                    bgfx::destroy(bucket.texture);
                }
            }
            m_arrayBuckets.clear();
            m_arrayDisabledDimensions.clear();

            for (uint16_t idx = 0; idx < MaxHandlesT; ++idx)
            {
                if (m_handleAlloc.isValid(idx))
                {
                    m_handleAlloc.free(idx);
                }
            }
        }

        bgfx::TextureHandle getTexture(EmitterSpriteHandle _sprite) const
        {
            if (!isValid(_sprite))
                return BGFX_INVALID_HANDLE;
            return m_entries[_sprite.idx].texture;
        }

        bool isArrayBacked(EmitterSpriteHandle _sprite) const
        {
            if (!isValid(_sprite))
                return false;
            return m_entries[_sprite.idx].kind == StorageKind::Array2D;
        }

        float getArrayLayer(EmitterSpriteHandle _sprite) const
        {
            if (!isValid(_sprite))
                return 0.0f;
            const SpriteEntry& entry = m_entries[_sprite.idx];
            return (entry.kind == StorageKind::Array2D) ? float(entry.arrayLayer) : 0.0f;
        }

        bool isValid(EmitterSpriteHandle _sprite) const { return m_handleAlloc.isValid(_sprite.idx); }

        bx::HandleAllocT<MaxHandlesT> m_handleAlloc;
        std::array<SpriteEntry, MaxHandlesT> m_entries{ };
        std::vector<ArrayBucket>             m_arrayBuckets;
        std::unordered_set<uint32_t>         m_arrayDisabledDimensions;
        bool                                 m_arrayBackendEnabled{ true };

    private:
        static uint32_t makeDimKey(uint16_t _width, uint16_t _height)
        {
            return (uint32_t(_width) << 16) | uint32_t(_height);
        }

        bool tryCreateDedicatedSprite(uint16_t _width, uint16_t _height, const void* _data, SpriteEntry& _outEntry)
        {
            const uint32_t dataSize = uint32_t(_width) * uint32_t(_height) * 4u;
            const bgfx::Memory* mem = bgfx::copy(_data, dataSize);
            bgfx::TextureHandle texture = bgfx::createTexture2D(
                _width, _height, false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_NONE, mem);
            if (!bgfx::isValid(texture))
            {
                return false;
            }

            _outEntry.kind = StorageKind::Dedicated2D;
            _outEntry.texture = texture;
            _outEntry.arrayBucket = UINT16_MAX;
            _outEntry.arrayLayer = 0;
            return true;
        }

        bool tryCreateArraySprite(uint16_t _width, uint16_t _height, const void* _data, SpriteEntry& _outEntry)
        {
            const uint32_t dimKey = makeDimKey(_width, _height);
            if (m_arrayDisabledDimensions.find(dimKey) != m_arrayDisabledDimensions.end())
            {
                return false;
            }

            // Try existing buckets for this dimension first.
            for (uint16_t bucketIdx = 0; bucketIdx < uint16_t(m_arrayBuckets.size()); ++bucketIdx)
            {
                ArrayBucket& bucket = m_arrayBuckets[bucketIdx];
                if (!bgfx::isValid(bucket.texture) || bucket.width != _width || bucket.height != _height)
                {
                    continue;
                }

                const uint16_t layer = acquireLayer(bucket);
                if (layer == UINT16_MAX)
                {
                    continue;
                }

                uploadArrayLayer(bucket.texture, layer, _width, _height, _data);
                ++bucket.activeSprites;

                _outEntry.kind = StorageKind::Array2D;
                _outEntry.texture = bucket.texture;
                _outEntry.arrayBucket = bucketIdx;
                _outEntry.arrayLayer = layer;
                return true;
            }

            // Create a new bucket for this dimension.
            ArrayBucket newBucket;
            if (!createArrayBucket(_width, _height, newBucket))
            {
                m_arrayDisabledDimensions.insert(dimKey);
                return false;
            }

            const uint16_t layer = acquireLayer(newBucket);
            if (layer == UINT16_MAX)
            {
                if (bgfx::isValid(newBucket.texture))
                {
                    bgfx::destroy(newBucket.texture);
                }
                m_arrayDisabledDimensions.insert(dimKey);
                return false;
            }

            uploadArrayLayer(newBucket.texture, layer, _width, _height, _data);
            ++newBucket.activeSprites;
            m_arrayBuckets.push_back(std::move(newBucket));
            const uint16_t bucketIdx = uint16_t(m_arrayBuckets.size() - 1);

            _outEntry.kind = StorageKind::Array2D;
            _outEntry.texture = m_arrayBuckets[bucketIdx].texture;
            _outEntry.arrayBucket = bucketIdx;
            _outEntry.arrayLayer = layer;
            return true;
        }

        bool createArrayBucket(uint16_t _width, uint16_t _height, ArrayBucket& _outBucket)
        {
            uint16_t layers = PARTICLE_ARRAY_PREFERRED_LAYERS;
            if (layers < PARTICLE_ARRAY_MIN_LAYERS)
            {
                layers = PARTICLE_ARRAY_MIN_LAYERS;
            }

            bgfx::TextureHandle tex = bgfx::createTexture2D(
                _width, _height, false, layers, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_NONE);
            if (!bgfx::isValid(tex) && layers != PARTICLE_ARRAY_MIN_LAYERS)
            {
                layers = PARTICLE_ARRAY_MIN_LAYERS;
                tex = bgfx::createTexture2D(
                    _width, _height, false, layers, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_NONE);
            }
            if (!bgfx::isValid(tex))
            {
                return false;
            }

            _outBucket.texture = tex;
            _outBucket.width = _width;
            _outBucket.height = _height;
            _outBucket.maxLayers = layers;
            _outBucket.nextLayer = 0;
            _outBucket.activeSprites = 0;
            _outBucket.freeLayers.clear();
            _outBucket.layerRefCounts.assign(layers, 0);
            return true;
        }

        static void uploadArrayLayer(bgfx::TextureHandle _arrayTexture, uint16_t _layer, uint16_t _width, uint16_t _height, const void* _data)
        {
            const uint32_t dataSize = uint32_t(_width) * uint32_t(_height) * 4u;
            bgfx::updateTexture2D(_arrayTexture, _layer, 0, 0, 0, _width, _height, bgfx::copy(_data, dataSize));
        }

        static uint16_t acquireLayer(ArrayBucket& _bucket)
        {
            if (!_bucket.freeLayers.empty())
            {
                const uint16_t layer = _bucket.freeLayers.back();
                _bucket.freeLayers.pop_back();
                if (layer < _bucket.layerRefCounts.size())
                {
                    ++_bucket.layerRefCounts[layer];
                }
                return layer;
            }

            if (_bucket.nextLayer < _bucket.maxLayers)
            {
                const uint16_t layer = _bucket.nextLayer++;
                if (layer < _bucket.layerRefCounts.size())
                {
                    ++_bucket.layerRefCounts[layer];
                }
                return layer;
            }

            return UINT16_MAX;
        }

        void releaseEntry(SpriteEntry& _entry)
        {
            if (_entry.kind == StorageKind::Dedicated2D)
            {
                if (bgfx::isValid(_entry.texture))
                {
                    bgfx::destroy(_entry.texture);
                }
                return;
            }

            if (_entry.kind == StorageKind::Array2D)
            {
                if (_entry.arrayBucket < m_arrayBuckets.size())
                {
                    ArrayBucket& bucket = m_arrayBuckets[_entry.arrayBucket];
                    if (_entry.arrayLayer < bucket.layerRefCounts.size())
                    {
                        uint16_t& ref = bucket.layerRefCounts[_entry.arrayLayer];
                        if (ref > 0)
                        {
                            --ref;
                            if (ref == 0)
                            {
                                bucket.freeLayers.push_back(_entry.arrayLayer);
                            }
                        }
                    }
                    if (bucket.activeSprites > 0)
                    {
                        --bucket.activeSprites;
                    }
                }
                return;
            }
        }
    };

    // -------------------------------------------------------------------------
    // Emitter definition
    // -------------------------------------------------------------------------
    struct Particle
    {
        bx::Vec3 start;
        bx::Vec3 end[2];
        float blendStart;
        float blendEnd;
        float scaleStart;
        float scaleEnd;

        uint32_t rgba[5];
        float rgbaTime[5];
        int rgbaKeyCount;

        float life;     // progress 0..1 where 1 is dead
        float lifeSpan; // seconds
        float rotation; // radians
    };

    struct ParticleSort
    {
        float    dist;
        int32_t  renderOrder;
        uint32_t idx;
    };

    struct Emitter
    {
        void create(EmitterShape::Enum _shape, EmitterDirection::Enum _direction, uint32_t _maxParticles);
        void destroy();

        void reset();
        void update(float _dt);
        void spawn(float _dt);
        void spawnBurst(uint32_t _count);
        void spawnParticles(uint32_t _count, float _timeStep);

        uint32_t render(const float _uv[4], float _spriteLayer, const float* _mtxView, const bx::Vec3& _eye,
                         uint32_t _first, uint32_t _max, ParticleSort* _outSort,
                         PosColorTexCoord0Vertex* _outVertices);

        EmitterShape::Enum     m_shape{EmitterShape::Sphere};
        EmitterDirection::Enum m_direction{EmitterDirection::Up};

        float           m_dt{0.0f};
        bx::RngMwc      m_rng;
        EmitterUniforms m_uniforms;

        bx::Aabb        m_aabb;

        Particle*       m_particles{nullptr};
        uint32_t        m_num{0};
        uint32_t        m_max{0};
    };

    // -------------------------------------------------------------------------
    // ParticleSystem context
    // -------------------------------------------------------------------------
    struct ParticleSystem
    {
        void init(uint16_t _maxEmitters, bx::AllocatorI* _allocator);
        void shutdown();

        EmitterSpriteHandle createSprite(uint16_t _width, uint16_t _height, const void* _data);
        void destroySprite(EmitterSpriteHandle _handle);

        void update(float _dt);
        void updateEmitterParticles(EmitterHandle _handle, float _dt);
        void render(uint8_t _view, const float* _mtxView, const bx::Vec3& _eye);
        void renderFiltered(uint8_t _view, const float* _mtxView, const bx::Vec3& _eye,
                            const uint16_t* _handles, uint16_t _handleCount);

        EmitterHandle createEmitter(EmitterShape::Enum _shape, EmitterDirection::Enum _direction, uint32_t _maxParticles);
        void updateEmitter(EmitterHandle _handle, const EmitterUniforms* _uniforms);
        void getAabb(EmitterHandle _handle, bx::Aabb& _outAabb);
        void destroyEmitter(EmitterHandle _handle);
        bool isEmitterAlive(EmitterHandle _handle) const;
        bool isSpriteAlive(EmitterSpriteHandle _handle) const;
        uint32_t getEmitterParticleCount(EmitterHandle _handle) const;

        // members
        bx::AllocatorI*  m_allocator{nullptr};
        bx::HandleAlloc* m_emitterAlloc{nullptr};
        Emitter*         m_emitter{nullptr};

        typedef SpriteT<MAX_PARTICLE_SPRITES> Sprite;
        Sprite           m_sprite;

        // BGFX resources
        bgfx::UniformHandle s_texColor = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle s_texColorArray = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle s_particleParams = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle m_defaultWhiteTexture = BGFX_INVALID_HANDLE;
        bgfx::ProgramHandle m_program2D = BGFX_INVALID_HANDLE;
        bgfx::ProgramHandle m_programArray = BGFX_INVALID_HANDLE;

        uint32_t m_numParticles{0};
        
        // Pre-allocated render buffers (avoid per-frame allocations for performance)
        ParticleSort*        m_sortBuf{nullptr};
        uint32_t             m_sortBufCapacity{0};
        std::vector<uint8_t> m_modePerQuad;
        std::vector<uint16_t> m_spritePerQuad;
        std::vector<uint16_t> m_runIndices;
    };

    static ParticleSystem s_ctx; // global context

    // ----------------------------------------------------------------------------------------
    // Implementation details
    // ----------------------------------------------------------------------------------------

    void Emitter::reset()
    {
        m_dt = 0.0f;
        m_uniforms.reset();
        m_num = 0;
        bx::memSet(&m_aabb, 0, sizeof(bx::Aabb));
        m_rng.reset();
    }

    void Emitter::create(EmitterShape::Enum _shape, EmitterDirection::Enum _direction, uint32_t _maxParticles)
    {
        reset();
        m_shape     = _shape;
        m_direction = _direction;
        m_max       = _maxParticles;
        m_particles = (Particle*)bx::alloc(s_ctx.m_allocator, sizeof(Particle)*m_max);
    }

    void Emitter::destroy()
    {
        bx::free(s_ctx.m_allocator, m_particles);
        m_particles = nullptr;
    }

    // The majority of update, spawn, render functions are directly ported from BGFX sample.
    // For brevity and maintainability, please refer to the original source if you need to
    // modify the underlying behaviour.

    static inline bx::Vec3 aabbExpand(bx::Aabb& _aabb, const bx::Vec3& _point)
    {
        _aabb.min = bx::min(_aabb.min, _point);
        _aabb.max = bx::max(_aabb.max, _point);
        return _point;
    }

    // Helper to generate random point on/in cone
    static bx::Vec3 randCone(bx::RngMwc* _rng, float _angle, float _arc, bool _fromEdge)
    {
        // Generate random angle within arc
        float theta = bx::frnd(_rng) * _arc;
        // Generate random distance from center (0 for edge, random for volume)
        float r = _fromEdge ? 1.0f : bx::frnd(_rng);
        // Generate random height based on cone angle
        float h = bx::cos(_angle) + bx::frnd(_rng) * (1.0f - bx::cos(_angle));
        float sinH = bx::sqrt(1.0f - h * h);
        
        return { sinH * bx::cos(theta) * r, h, sinH * bx::sin(theta) * r };
    }
    
    // Helper to generate random point in box
    static bx::Vec3 randBox(bx::RngMwc* _rng, const float* _scale, bool _fromEdge)
    {
        if (_fromEdge)
        {
            // Pick a random face and point on it
            int face = (int)(bx::frnd(_rng) * 6.0f);
            float u = bx::frndh(_rng);
            float v = bx::frndh(_rng);
            switch (face)
            {
                case 0: return { _scale[0] * 0.5f, u * _scale[1], v * _scale[2] };
                case 1: return { -_scale[0] * 0.5f, u * _scale[1], v * _scale[2] };
                case 2: return { u * _scale[0], _scale[1] * 0.5f, v * _scale[2] };
                case 3: return { u * _scale[0], -_scale[1] * 0.5f, v * _scale[2] };
                case 4: return { u * _scale[0], v * _scale[1], _scale[2] * 0.5f };
                default: return { u * _scale[0], v * _scale[1], -_scale[2] * 0.5f };
            }
        }
        return { bx::frndh(_rng) * _scale[0], bx::frndh(_rng) * _scale[1], bx::frndh(_rng) * _scale[2] };
    }
    
    // Helper to generate random point on edge
    static bx::Vec3 randEdge(bx::RngMwc* _rng, float _length)
    {
        return { bx::frndh(_rng) * _length, 0.0f, 0.0f };
    }
    
    // Evaluate size over lifetime curve
    static float evalSizeCurve(CurveType::Enum _curve, float _t, float _start, float _end)
    {
        switch (_curve)
        {
            case CurveType::Constant: return _start;
            case CurveType::Linear: return bx::lerp(_start, _end, _t);
            case CurveType::EaseIn: return bx::lerp(_start, _end, _t * _t);
            case CurveType::EaseOut: { float it = 1.0f - _t; return bx::lerp(_start, _end, 1.0f - it * it); }
            case CurveType::EaseInOut: { float s = _t * _t * (3.0f - 2.0f * _t); return bx::lerp(_start, _end, s); }
            default: return _start;
        }
    }

    // Helper function to spawn new particles with modern emission shapes
    void Emitter::spawn(float _dt)
    {
        const float timePerParticle = 1.0f / bx::max<float>(1.0f, (float)m_uniforms.m_particlesPerSecond);
        m_dt += _dt;
        const uint32_t numParticlesToSpawn = (uint32_t)(m_dt / timePerParticle);
        m_dt -= numParticlesToSpawn * timePerParticle;

        spawnParticles(numParticlesToSpawn, timePerParticle);
    }

    void Emitter::spawnBurst(uint32_t _count)
    {
        spawnParticles(_count, 0.0f);
    }

    void Emitter::spawnParticles(uint32_t numParticlesToSpawn, float timePerParticle)
    {
        if (numParticlesToSpawn == 0) return;

        // Precompute emitter transform
        float mtx[16];
        bx::mtxSRT(mtx, 1.0f, 1.0f, 1.0f,
                   m_uniforms.m_angle[0], m_uniforms.m_angle[1], m_uniforms.m_angle[2],
                   m_uniforms.m_position[0], m_uniforms.m_position[1], m_uniforms.m_position[2]);

        auto rotateVector = [&mtx](const bx::Vec3& v) -> bx::Vec3 {
            return {
                v.x * mtx[0] + v.y * mtx[4] + v.z * mtx[8],
                v.x * mtx[1] + v.y * mtx[5] + v.z * mtx[9],
                v.x * mtx[2] + v.y * mtx[6] + v.z * mtx[10]
            };
        };

        auto inverseRotateVector = [&mtx](const bx::Vec3& v) -> bx::Vec3 {
            return {
                v.x * mtx[0] + v.y * mtx[1] + v.z * mtx[2],
                v.x * mtx[4] + v.y * mtx[5] + v.z * mtx[6],
                v.x * mtx[8] + v.y * mtx[9] + v.z * mtx[10]
            };
        };

        const bx::Vec3 up = { 0.0f, 1.0f, 0.0f };
        const bx::Vec3 customDir = { m_uniforms.m_direction[0], m_uniforms.m_direction[1], m_uniforms.m_direction[2] };
        const float shapeRadius = m_uniforms.m_shapeRadius;
        const float radiusThickness = m_uniforms.m_shapeRadiusThickness;
        const bool fromEdge = m_uniforms.m_emitFromEdge;

        auto cross = [](const bx::Vec3& a, const bx::Vec3& b) -> bx::Vec3 {
            return {
                a.y * b.z - a.z * b.y,
                a.z * b.x - a.x * b.z,
                a.x * b.y - a.y * b.x
            };
        };

        auto safeNormalize = [](const bx::Vec3& v, const bx::Vec3& fallback) -> bx::Vec3 {
            const float lenSq = v.x * v.x + v.y * v.y + v.z * v.z;
            if (lenSq <= 1e-6f)
            {
                return fallback;
            }
            return bx::mul(v, 1.0f / bx::sqrt(lenSq));
        };

        float emitTime = 0.0f;
        for (uint32_t ii = 0; ii < numParticlesToSpawn && m_num < m_max; ++ii)
        {
            Particle& p = m_particles[m_num++];

            // Random position depending on shape
            bx::Vec3 pos = { 0.0f, 0.0f, 0.0f };
            bx::Vec3 shapeDir = up;
            
            switch (m_shape)
            {
                case EmitterShape::Point:
                    pos = { 0.0f, 0.0f, 0.0f };
                    break;
                    
                case EmitterShape::Sphere:
                {
                    bx::Vec3 unitPos = bx::randUnitSphere(&m_rng);
                    float r = fromEdge ? 1.0f : (1.0f - radiusThickness + radiusThickness * bx::frnd(&m_rng));
                    pos = bx::mul(unitPos, shapeRadius * r);
                    shapeDir = bx::normalize(unitPos);
                    break;
                }
                    
                case EmitterShape::Hemisphere:
                {
                    bx::Vec3 unitPos = bx::randUnitHemisphere(&m_rng, up);
                    float r = fromEdge ? 1.0f : (1.0f - radiusThickness + radiusThickness * bx::frnd(&m_rng));
                    pos = bx::mul(unitPos, shapeRadius * r);
                    shapeDir = bx::normalize(unitPos);
                    break;
                }
                    
                case EmitterShape::Cone:
                {
                    bx::Vec3 conePos = randCone(&m_rng, m_uniforms.m_shapeAngle, m_uniforms.m_shapeArc, fromEdge);
                    pos = bx::mul(conePos, shapeRadius);
                    shapeDir = bx::normalize(conePos);
                    break;
                }
                    
                case EmitterShape::Box:
                {
                    pos = randBox(&m_rng, m_uniforms.m_shapeScale, fromEdge);
                    break;
                }
                    
                case EmitterShape::Circle:
                {
                    float theta = bx::frnd(&m_rng) * m_uniforms.m_shapeArc;
                    pos = { bx::cos(theta) * shapeRadius, 0.0f, bx::sin(theta) * shapeRadius };
                    break;
                }
                    
                case EmitterShape::Disc:
                {
                    bx::Vec3 tmp = bx::randUnitCircle(&m_rng);
                    float r = fromEdge ? 1.0f : bx::frnd(&m_rng);
                    pos = bx::mul(tmp, shapeRadius * r);
                    break;
                }
                    
                case EmitterShape::Edge:
                {
                    pos = randEdge(&m_rng, m_uniforms.m_shapeLength);
                    break;
                }
                    
                case EmitterShape::Rect:
                {
                    pos = { bx::frndh(&m_rng) * m_uniforms.m_shapeScale[0], 
                            0.0f, 
                            bx::frndh(&m_rng) * m_uniforms.m_shapeScale[2] };
                    break;
                }
                    
                default: 
                    break;
            }

            // Direction based on emission direction mode
            bx::Vec3 dir = up;
            switch (m_direction)
            {
                case EmitterDirection::Up:
                    dir = up;
                    break;
                case EmitterDirection::Outward:
                    dir = (bx::length(shapeDir) > 0.001f) ? shapeDir : up;
                    break;
                case EmitterDirection::Random:
                    dir = bx::randUnitSphere(&m_rng);
                    break;
                case EmitterDirection::Directional:
                    dir = bx::normalize(customDir);
                    break;
                default:
                    break;
            }
            
            // Apply randomize direction if enabled
            if (m_uniforms.m_randomizeDirection)
            {
                bx::Vec3 randDir = bx::randUnitSphere(&m_rng);
                dir = bx::normalize(bx::add(dir, bx::mul(randDir, 0.3f)));
            }

            // Calculate start speed
            const float startSpeed = bx::lerp(m_uniforms.m_startSpeedMin, m_uniforms.m_startSpeedMax, bx::frnd(&m_rng));
            
            // Lifetime
            p.lifeSpan = bx::lerp(m_uniforms.m_lifeSpan[0], m_uniforms.m_lifeSpan[1], bx::frnd(&m_rng));
            p.life = p.lifeSpan > 0.0001f ? emitTime / p.lifeSpan : 0.0f;
            p.rotation = bx::lerp(m_uniforms.m_startRotationMin, m_uniforms.m_startRotationMax, bx::frnd(&m_rng));

            // Calculate end position based on velocity
            const float dragFactor = 1.0f / bx::max(1.0f, 1.0f + bx::max(0.0f, m_uniforms.m_dragCoefficient) * p.lifeSpan);
            const float travelDist = startSpeed * p.lifeSpan;
            bx::Vec3 velocityDir = dir;
            if (!m_uniforms.m_localSpace)
            {
                velocityDir = rotateVector(velocityDir);
                const float lenSq =
                    velocityDir.x * velocityDir.x +
                    velocityDir.y * velocityDir.y +
                    velocityDir.z * velocityDir.z;
                if (lenSq > 1e-6f)
                {
                    velocityDir = bx::mul(velocityDir, 1.0f / bx::sqrt(lenSq));
                }
                else
                {
                    velocityDir = up;
                }
            }
            const bx::Vec3 velocity = bx::mul(velocityDir, travelDist * dragFactor);
            
            // Add gravity effect
            const bx::Vec3 gravity = { 0.0f, -9.81f * m_uniforms.m_gravityScale * bx::square(p.lifeSpan) * dragFactor, 0.0f };
            
            // Add linear velocity over lifetime
            const bx::Vec3 linearVel = { 
                m_uniforms.m_linearVelocity[0] * p.lifeSpan * dragFactor,
                m_uniforms.m_linearVelocity[1] * p.lifeSpan * dragFactor,
                m_uniforms.m_linearVelocity[2] * p.lifeSpan * dragFactor
            };
            const bx::Vec3 worldLinearVel = m_uniforms.m_localSpace ? linearVel : rotateVector(linearVel);

            const bx::Vec3 radialDirLocal = safeNormalize(pos, safeNormalize(shapeDir, dir));
            bx::Vec3 tangentDirLocal = safeNormalize(cross(up, radialDirLocal), { 1.0f, 0.0f, 0.0f });
            if (bx::length(tangentDirLocal) <= 0.001f)
            {
                tangentDirLocal = { 1.0f, 0.0f, 0.0f };
            }
            const bx::Vec3 radialVel = bx::mul(radialDirLocal, m_uniforms.m_radialVelocity * p.lifeSpan * dragFactor);
            const bx::Vec3 orbitalVel = bx::mul(tangentDirLocal, m_uniforms.m_orbitalVelocity * p.lifeSpan * dragFactor);
            const bx::Vec3 radialWorldVel = m_uniforms.m_localSpace ? radialVel : rotateVector(radialVel);
            const bx::Vec3 orbitalWorldVel = m_uniforms.m_localSpace ? orbitalVel : rotateVector(orbitalVel);

            const bx::Vec3 emitterVelocityWorld = {
                m_uniforms.m_emitterVelocity[0],
                m_uniforms.m_emitterVelocity[1],
                m_uniforms.m_emitterVelocity[2]
            };
            const bx::Vec3 inheritedWorldVel =
                bx::mul(emitterVelocityWorld, m_uniforms.m_inheritVelocity * p.lifeSpan * dragFactor);
            const bx::Vec3 inheritedLocalVel = inverseRotateVector(inheritedWorldVel);

            // For local space: keep positions relative to emitter origin
            // For world space: transform by emitter's world matrix
            if (m_uniforms.m_localSpace)
            {
                // Store in local space - will be transformed at render time
                p.start  = pos;
                p.end[0] = bx::add(p.start, velocity);
                p.end[0] = bx::add(p.end[0], linearVel);
                p.end[0] = bx::add(p.end[0], radialVel);
                p.end[0] = bx::add(p.end[0], orbitalVel);
                p.end[0] = bx::add(p.end[0], inheritedLocalVel);
                // Gravity is applied in world space even for local particles
                // (flames still rise up regardless of torch orientation)
                p.end[1] = bx::add(p.end[0], gravity);
            }
            else
            {
                // World space: transform to world coordinates immediately
                p.start  = bx::mul(pos, mtx);
                p.end[0] = bx::add(p.start, velocity);
                p.end[0] = bx::add(p.end[0], worldLinearVel);
                p.end[0] = bx::add(p.end[0], radialWorldVel);
                p.end[0] = bx::add(p.end[0], orbitalWorldVel);
                p.end[0] = bx::add(p.end[0], inheritedWorldVel);
                p.end[1] = bx::add(p.end[0], gravity);
            }

            // Copy color gradient and apply per-particle start color.
            float colorMul[4];
            for (int ci = 0; ci < 4; ++ci)
            {
                colorMul[ci] = bx::lerp(m_uniforms.m_startColorMin[ci], m_uniforms.m_startColorMax[ci], bx::frnd(&m_rng));
            }
            for (int ci = 0; ci < 5; ++ci)
            {
                p.rgba[ci] = modulateAbgr(m_uniforms.m_rgba[ci], colorMul);
                p.rgbaTime[ci] = m_uniforms.m_rgbaTime[ci];
            }
            p.rgbaKeyCount = bx::clamp(m_uniforms.m_rgbaKeyCount, 1, 5);

            p.blendStart = bx::lerp(m_uniforms.m_blendStart[0], m_uniforms.m_blendStart[1], bx::frnd(&m_rng));
            p.blendEnd   = bx::lerp(m_uniforms.m_blendEnd[0],   m_uniforms.m_blendEnd[1],   bx::frnd(&m_rng));

            // Use new start size parameters
            p.scaleStart = bx::lerp(m_uniforms.m_startSizeMin, m_uniforms.m_startSizeMax, bx::frnd(&m_rng));
            p.scaleEnd = p.scaleStart;

            emitTime += timePerParticle;
        }
    }

    void Emitter::update(float _dt)
    {
        // Update existing particles and remove dead ones
        for (uint32_t ii = 0; ii < m_num; )
        {
            Particle& p = m_particles[ii];
            p.life += _dt * (1.0f / p.lifeSpan);
            if (p.life > 1.0f)
            {
                // kill this particle by swap with last
                if (ii != m_num - 1)
                {
                    m_particles[ii] = m_particles[m_num - 1];
                }
                --m_num;
                continue; // don't increment ii, reprocess swapped
            }
            ++ii;
        }

        if (m_uniforms.m_burstMode && m_uniforms.m_burstCount > 0)
        {
            spawnBurst(static_cast<uint32_t>(m_uniforms.m_burstCount));
            m_uniforms.m_burstCount = 0;
        }

        // Spawn new ones if needed
        if (m_uniforms.m_particlesPerSecond > 0)
        {
            spawn(_dt);
        }
    }

    // Helper to interpolate ABGR color
    static uint32_t lerpColor(uint32_t _a, uint32_t _b, float _t)
    {
        const float invT = 1.0f - _t;
        uint8_t ra = (_a >>  0) & 0xff;
        uint8_t ga = (_a >>  8) & 0xff;
        uint8_t ba = (_a >> 16) & 0xff;
        uint8_t aa = (_a >> 24) & 0xff;
        uint8_t rb = (_b >>  0) & 0xff;
        uint8_t gb = (_b >>  8) & 0xff;
        uint8_t bb = (_b >> 16) & 0xff;
        uint8_t ab = (_b >> 24) & 0xff;
        
        uint8_t r = uint8_t(ra * invT + rb * _t);
        uint8_t g = uint8_t(ga * invT + gb * _t);
        uint8_t b = uint8_t(ba * invT + bb * _t);
        uint8_t a = uint8_t(aa * invT + ab * _t);
        
        return (a << 24) | (b << 16) | (g << 8) | r;
    }
    
    // Interpolate color through authored gradient keys.
    static uint32_t evalColorGradient(const uint32_t* _rgba, const float* _times, int _keyCount, float _t)
    {
        _keyCount = bx::clamp(_keyCount, 1, 5);

        // Clamp t to [0, 1]
        _t = bx::clamp(_t, 0.0f, 1.0f);

        if (_keyCount == 1 || _t <= _times[0])
        {
            return _rgba[0];
        }

        // Find the segment
        for (int i = 0; i < _keyCount - 1; ++i)
        {
            const float nextTime = bx::max(_times[i], _times[i + 1]);
            if (_t <= nextTime)
            {
                float segmentT = (nextTime - _times[i]) > 0.0001f
                    ? (_t - _times[i]) / (nextTime - _times[i])
                    : 0.0f;
                return lerpColor(_rgba[i], _rgba[i + 1], segmentT);
            }
        }
        return _rgba[_keyCount - 1];
    }

    // Render returns number of particles processed. Uses quadratic bezier for gravity and 
    // interpolates color over lifetime.
    uint32_t Emitter::render(const float _uv[4], float _spriteLayer, const float* _mtxView, const bx::Vec3& _eye,
                             uint32_t _first, uint32_t _max, ParticleSort* _outSort,
                             PosColorTexCoord0Vertex* _outVertices)
    {
        const uint32_t count = bx::uint32_min(m_num, _max - _first);
        bx::Vec3 rightDir = { _mtxView[0], _mtxView[4], _mtxView[8] };
        bx::Vec3 upDir = { _mtxView[1], _mtxView[5], _mtxView[9] };
        if (!m_uniforms.m_faceCamera)
        {
            const float* m = m_uniforms.m_worldMatrix;
            rightDir = { m[0], m[1], m[2] };
            upDir = { m[4], m[5], m[6] };
        }
        auto normalizeOr = [](const bx::Vec3& v, const bx::Vec3& fallback) -> bx::Vec3 {
            const float lenSq = v.x * v.x + v.y * v.y + v.z * v.z;
            return lenSq > 1e-6f ? bx::mul(v, 1.0f / bx::sqrt(lenSq)) : fallback;
        };
        rightDir = normalizeOr(rightDir, { 1.0f, 0.0f, 0.0f });
        upDir = normalizeOr(upDir, { 0.0f, 1.0f, 0.0f });
        for (uint32_t ii = 0; ii < count; ++ii)
        {
            const Particle& p = m_particles[ii];
            const float t = bx::clamp(p.life, 0.0f, 1.0f);
            
            // Quadratic bezier interpolation for gravity effect:
            // lerp(lerp(start, end[0], t), lerp(end[0], end[1], t), t)
            const bx::Vec3 p0p1 = bx::lerp(p.start, p.end[0], t);
            const bx::Vec3 p1p2 = bx::lerp(p.end[0], p.end[1], t);
            bx::Vec3 pos = bx::lerp(p0p1, p1p2, t);
            
            // For local space simulation: transform from emitter-local to world space
            // This makes particles move with the emitter (e.g., torch flames stay attached)
            if (m_uniforms.m_localSpace)
            {
                pos = bx::mul(pos, m_uniforms.m_worldMatrix);
            }

            // Size over lifetime with proper curve evaluation
            const float sizeT = evalSizeCurve(m_uniforms.m_sizeOverLifetimeCurve, t, 
                                              m_uniforms.m_sizeOverLifetimeStart, 
                                              m_uniforms.m_sizeOverLifetimeEnd);
            const float scale = p.scaleStart * sizeT;
            const float blend = bx::lerp(p.blendStart, p.blendEnd, t);
            
            // Interpolate color over lifetime
            const uint32_t color = evalColorGradient(p.rgba, p.rgbaTime, p.rgbaKeyCount, t);

            // Use view matrix columns (inverse rotation axes): right = col0, up = col1
            bx::Vec3 udir = bx::mul(rightDir, scale);
            bx::Vec3 vdir = bx::mul(upDir, scale);
            
            float particleAngle = p.rotation + m_uniforms.m_angularVelocity * (p.lifeSpan * t);
            if (m_uniforms.m_alignToVelocity)
            {
                const bx::Vec3 v0 = bx::sub(p.end[0], p.start);
                const bx::Vec3 v1 = bx::sub(p.end[1], p.end[0]);
                bx::Vec3 vel = bx::add(bx::mul(v0, 1.0f - t), bx::mul(v1, t));
                vel = bx::mul(vel, 2.0f);
                
                if (m_uniforms.m_localSpace)
                {
                    const float* m = m_uniforms.m_worldMatrix;
                    vel = {
                        vel.x * m[0] + vel.y * m[4] + vel.z * m[8],
                        vel.x * m[1] + vel.y * m[5] + vel.z * m[9],
                        vel.x * m[2] + vel.y * m[6] + vel.z * m[10]
                    };
                }
                
                const float lenSq = vel.x * vel.x + vel.y * vel.y + vel.z * vel.z;
                if (lenSq > 1e-6f)
                {
                    const float invLen = 1.0f / bx::sqrt(lenSq);
                    vel = bx::mul(vel, invLen);
                    const float vx = vel.x * rightDir.x + vel.y * rightDir.y + vel.z * rightDir.z;
                    const float vy = vel.x * upDir.x + vel.y * upDir.y + vel.z * upDir.z;
                    if (std::abs(vx) > 1e-6f || std::abs(vy) > 1e-6f)
                    {
                        particleAngle += std::atan2(vx, vy);
                    }
                }
            }
            
            if (particleAngle != 0.0f)
            {
                const float ca = bx::cos(particleAngle);
                const float sa = bx::sin(particleAngle);
                const bx::Vec3 urot = bx::add(bx::mul(udir, ca), bx::mul(vdir, -sa));
                const bx::Vec3 vrot = bx::add(bx::mul(udir, sa), bx::mul(vdir, ca));
                udir = urot;
                vdir = vrot;
            }

            PosColorTexCoord0Vertex* vertex = &_outVertices[(_first + ii)*4];

            const bx::Vec3 ul = bx::sub(bx::sub(pos, udir), vdir);
            bx::store(&vertex[0].m_x, ul);
            vertex[0].m_abgr = color;
            vertex[0].m_u = _uv[0]; vertex[0].m_v = _uv[1]; vertex[0].m_blend = blend; vertex[0].m_angle = _spriteLayer;
            const bx::Vec3 ur = bx::sub(bx::add(pos, udir), vdir);
            bx::store(&vertex[1].m_x, ur);
            vertex[1].m_abgr = color;
            vertex[1].m_u = _uv[2]; vertex[1].m_v = _uv[1]; vertex[1].m_blend = blend; vertex[1].m_angle = _spriteLayer;
            const bx::Vec3 br = bx::add(bx::add(pos, udir), vdir);
            bx::store(&vertex[2].m_x, br);
            vertex[2].m_abgr = color;
            vertex[2].m_u = _uv[2]; vertex[2].m_v = _uv[3]; vertex[2].m_blend = blend; vertex[2].m_angle = _spriteLayer;
            const bx::Vec3 bl = bx::add(bx::sub(pos, udir), vdir);
            bx::store(&vertex[3].m_x, bl);
            vertex[3].m_abgr = color;
            vertex[3].m_u = _uv[0]; vertex[3].m_v = _uv[3]; vertex[3].m_blend = blend; vertex[3].m_angle = _spriteLayer;

            // sort info
            const bx::Vec3 tmp = bx::sub(_eye, pos);
            _outSort[_first + ii].dist = bx::length(tmp);
            _outSort[_first + ii].renderOrder = m_uniforms.m_renderOrder;
            _outSort[_first + ii].idx  = _first + ii;
        }
        // update aabb (approx, expanded bounds)
        m_aabb.min = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        m_aabb.max = {  FLT_MAX,  FLT_MAX,  FLT_MAX };

        return count;
    }

    // -------------------------------------------------------------------------
    // Sorting function for qsort.
    static int32_t particleSortFn(const void* _lhs, const void* _rhs)
    {
        const ParticleSort& lhs = *(const ParticleSort*)_lhs;
        const ParticleSort& rhs = *(const ParticleSort*)_rhs;
        if (lhs.renderOrder != rhs.renderOrder)
        {
            return lhs.renderOrder < rhs.renderOrder ? -1 : 1;
        }
        return lhs.dist > rhs.dist ? -1 : 1;
    }

    // -------------------------------------------------------------------------
    // ParticleSystem methods
    void ParticleSystem::init(uint16_t _maxEmitters, bx::AllocatorI* _allocator)
    {
        m_allocator = _allocator;
        if (!m_allocator)
        {
            static bx::DefaultAllocator defaultAlloc;
            m_allocator = &defaultAlloc;
        }

        m_emitterAlloc = bx::createHandleAlloc(m_allocator, _maxEmitters);
        m_emitter      = (Emitter*)bx::alloc(m_allocator, sizeof(Emitter)*_maxEmitters);

        PosColorTexCoord0Vertex::init();

        // Uniforms and default fallback textures
        s_texColor = bgfx::createUniform("s_texColor", bgfx::UniformType::Sampler);
        s_texColorArray = bgfx::createUniform("s_texColorArray", bgfx::UniformType::Sampler);
        s_particleParams = bgfx::createUniform("u_particleParams", bgfx::UniformType::Vec4);
        {
            const uint32_t whitePixel = 0xffffffffu;
            m_defaultWhiteTexture = bgfx::createTexture2D(
                1, 1, false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_NONE,
                bgfx::copy(&whitePixel, sizeof(whitePixel)));
        }
        // Load split particle programs to avoid D3D11 SRV dimension mismatches.
        m_program2D = ShaderManager::Instance().LoadProgram("vs_particle", "fs_particle_2d");
        m_programArray = ShaderManager::Instance().LoadProgram("vs_particle", "fs_particle_array");
        m_sprite.setArrayBackendEnabled(bgfx::isValid(m_programArray));
        if (!bgfx::isValid(m_programArray))
        {
            std::cout << "[ParticleSystem] Array particle program unavailable. Using dedicated sprite textures only." << std::endl;
        }
        std::cout << "[ParticleSystem] Sprite residency mode: "
                  << (bgfx::isValid(m_programArray) ? "hybrid-array" : "dedicated-only")
                  << ", capacity=" << m_sprite.m_handleAlloc.getMaxHandles() << " sprites" << std::endl;

        // Ensure default blending-friendly render state is used in shader; we set at draw time.
    }

    void ParticleSystem::shutdown()
    {
        m_sprite.destroyAll();
        if (bgfx::isValid(m_programArray)) bgfx::destroy(m_programArray);
        if (bgfx::isValid(m_program2D)) bgfx::destroy(m_program2D);
        if (bgfx::isValid(m_defaultWhiteTexture)) bgfx::destroy(m_defaultWhiteTexture);
        if (bgfx::isValid(s_texColorArray)) bgfx::destroy(s_texColorArray);
        if (bgfx::isValid(s_texColor)) bgfx::destroy(s_texColor);
        if (bgfx::isValid(s_particleParams)) bgfx::destroy(s_particleParams);

        bx::destroyHandleAlloc(m_allocator, m_emitterAlloc);
        bx::free(m_allocator, m_emitter);
        
        // Free pre-allocated render buffers
        if (m_sortBuf) {
            bx::free(m_allocator, m_sortBuf);
            m_sortBuf = nullptr;
            m_sortBufCapacity = 0;
        }
        m_modePerQuad.clear();
        m_modePerQuad.shrink_to_fit();
        m_spritePerQuad.clear();
        m_spritePerQuad.shrink_to_fit();
        m_runIndices.clear();
        m_runIndices.shrink_to_fit();

        m_allocator = nullptr;
    }

    EmitterSpriteHandle ParticleSystem::createSprite(uint16_t _width, uint16_t _height, const void* _data)
    {
        return m_sprite.create(_width, _height, _data);
    }

    void ParticleSystem::destroySprite(EmitterSpriteHandle _handle)
    {
        m_sprite.destroy(_handle);
    }

    void ParticleSystem::update(float _dt)
    {
        if (m_emitterAlloc == nullptr)
        {
            m_numParticles = 0;
            return;
        }

        // Parallelize per-emitter updates; emitters are independent and safe to update concurrently.
        // Keep accumulation thread-safe and avoid any GPU calls here (render happens elsewhere).
        uint16_t nh = m_emitterAlloc->getNumHandles();
        if (nh == 0)
        {
            m_numParticles = 0;
            return;
        }

        // Defer to the engine job system when available; fall back to serial if not linked.
        // Local accumulation uses atomic to avoid false sharing across chunks.
        std::atomic<uint32_t> total{ 0u };

        // Parallel chunk size tuned to amortize job overhead while keeping cores busy.
        const size_t chunk = 64;
        parallel_for(Jobs(), size_t(0), size_t(nh), chunk, [&](size_t start, size_t count)
        {
            uint32_t localSum = 0;
            for (size_t i = 0; i < count; ++i)
            {
                uint16_t handleIndex = m_emitterAlloc->getHandleAt(static_cast<uint16_t>(start + i));
                m_emitter[handleIndex].update(_dt);
                localSum += m_emitter[handleIndex].m_num;
            }
            total.fetch_add(localSum, std::memory_order_relaxed);
        });

        m_numParticles = total.load(std::memory_order_relaxed);
        
    }

    void ParticleSystem::updateEmitterParticles(EmitterHandle _handle, float _dt)
    {
        if (!isEmitterAlive(_handle) || _dt <= 0.0f)
            return;

        Emitter& emitter = m_emitter[_handle.idx];
        emitter.update(_dt);

        uint32_t totalParticles = 0;
        for (uint16_t ii = 0, nh = m_emitterAlloc->getNumHandles(); ii < nh; ++ii)
        {
            uint16_t handleIndex = m_emitterAlloc->getHandleAt(ii);
            totalParticles += m_emitter[handleIndex].m_num;
        }
        m_numParticles = totalParticles;
    }

    void ParticleSystem::render(uint8_t _view, const float* _mtxView, const bx::Vec3& _eye)
    {
        if (m_numParticles == 0 || !bgfx::isValid(m_program2D))
            return;

        bgfx::TransientVertexBuffer tvb;

        const uint32_t availVB = bgfx::getAvailTransientVertexBuffer(m_numParticles*4, PosColorTexCoord0Vertex::ms_layout);
        const uint32_t availIB = bgfx::getAvailTransientIndexBuffer(m_numParticles * 6);
        const uint32_t maxDraw = bx::uint32_min(availVB/4, availIB/6);
        if (maxDraw == 0) return;

        bgfx::allocTransientVertexBuffer(&tvb, maxDraw*4, PosColorTexCoord0Vertex::ms_layout);

        PosColorTexCoord0Vertex* vertices = (PosColorTexCoord0Vertex*)tvb.data;
        
        // Use pre-allocated sort buffer - grow if needed (but don't shrink to avoid thrashing)
        if (maxDraw > m_sortBufCapacity) {
            if (m_sortBuf) bx::free(m_allocator, m_sortBuf);
            m_sortBufCapacity = maxDraw + maxDraw / 4; // Add 25% headroom
            m_sortBuf = (ParticleSort*)bx::alloc(m_allocator, sizeof(ParticleSort) * m_sortBufCapacity);
        }
        
        // Resize pre-allocated vectors (clear + resize is faster than recreating)
        m_modePerQuad.clear();
        m_modePerQuad.resize(maxDraw, 0);
        m_spritePerQuad.clear();
        m_spritePerQuad.resize(maxDraw, UINT16_MAX);
        m_runIndices.clear();
        m_runIndices.reserve(maxDraw * 6u);

        uint32_t pos = 0;
        for (uint16_t ii = 0, nh = m_emitterAlloc->getNumHandles(); ii < nh && pos < maxDraw; ++ii)
        {
            uint16_t idx = m_emitterAlloc->getHandleAt(ii);
            Emitter& emitter = m_emitter[idx];

            // Sprite backend is non-atlas: all sprite UVs use full range [0..1].
            const float uv[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
            const float spriteLayer = m_sprite.getArrayLayer(emitter.m_uniforms.m_handle);

            uint32_t start = pos;
            pos += emitter.render(uv, spriteLayer, _mtxView, _eye, pos, maxDraw, m_sortBuf, vertices);
            for (uint32_t q = start; q < pos; ++q)
            {
                m_modePerQuad[q] = (uint8_t)bx::uint32_min(emitter.m_uniforms.m_blendMode, 2u);
                m_spritePerQuad[q] = emitter.m_uniforms.m_handle.idx;
            }
        }

        if (pos == 0) return;

        // sort particles back-to-front
        qsort(m_sortBuf, pos, sizeof(ParticleSort), particleSortFn);
        float idMtx[16]; bx::mtxIdentity(idMtx);
        auto resolveBinding = [&](uint16_t spriteIdx, bool& outUseArray, bgfx::TextureHandle& outTexture)
        {
            EmitterSpriteHandle sprite{ spriteIdx };
            outUseArray = m_sprite.isArrayBacked(sprite);
            outTexture = m_sprite.getTexture(sprite);
            if (!bgfx::isValid(outTexture))
            {
                outUseArray = false;
                outTexture = m_defaultWhiteTexture;
            }
            if (outUseArray && !bgfx::isValid(m_programArray))
            {
                // Array sprites cannot be sampled by the 2D shader; render as white fallback.
                outUseArray = false;
                outTexture = m_defaultWhiteTexture;
            }
        };

        auto submitRun = [&](uint8_t mode, bool useArray, bgfx::TextureHandle runTexture)
        {
            if (m_runIndices.empty()) return;
            const uint32_t indexCount = (uint32_t)m_runIndices.size();
            if (bgfx::getAvailTransientIndexBuffer(indexCount) < indexCount) {
                m_runIndices.clear();
                return;
            }

            bgfx::TransientIndexBuffer tibLocal;
            bgfx::allocTransientIndexBuffer(&tibLocal, indexCount);
            bx::memCopy(tibLocal.data, m_runIndices.data(), indexCount * sizeof(uint16_t));

            uint64_t blendFlags = BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
            if (mode == 1) {
                blendFlags = BGFX_STATE_BLEND_ADD;
            } else if (mode == 2) {
                blendFlags = BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_DST_COLOR, BGFX_STATE_BLEND_ZERO);
            }

            bgfx::setTransform(idMtx);
            bgfx::setVertexBuffer(0, &tvb);
            bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
            if (useArray && bgfx::isValid(s_texColorArray) && bgfx::isValid(runTexture) && bgfx::isValid(m_programArray))
            {
                bgfx::setTexture(0, s_texColorArray, runTexture);
                program = m_programArray;
            }
            else
            {
                // Array sprites need an explicit 2D fallback to avoid submitting with slot 0 unbound.
                bgfx::TextureHandle tex2D = (!useArray && bgfx::isValid(runTexture)) ? runTexture : m_defaultWhiteTexture;
                if (!bgfx::isValid(s_texColor) || !bgfx::isValid(tex2D) || !bgfx::isValid(m_program2D))
                {
                    m_runIndices.clear();
                    return;
                }

                bgfx::setTexture(0, s_texColor, tex2D);
                program = m_program2D;
            }
            bgfx::setIndexBuffer(&tibLocal);
            if (bgfx::isValid(s_particleParams)) {
                const float params[4] = { float(mode), 0.0f, 0.0f, 0.0f };
                bgfx::setUniform(s_particleParams, params);
            }
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_LESS | blendFlags);
            if (bgfx::isValid(program))
            {
                bgfx::submit(_view, program);
            }
            m_runIndices.clear();
        };

        bool hasRun = false;
        uint8_t runMode = 0;
        bool runUseArray = false;
        bgfx::TextureHandle runTexture = BGFX_INVALID_HANDLE;
        for (uint32_t ii = 0; ii < pos; ++ii)
        {
            const ParticleSort& s = m_sortBuf[ii];
            const uint16_t qidx = (uint16_t)s.idx;
            const uint8_t mode = (qidx < m_modePerQuad.size()) ? m_modePerQuad[qidx] : 0;
            const uint16_t sprite = (qidx < m_spritePerQuad.size()) ? m_spritePerQuad[qidx] : UINT16_MAX;
            bool useArray = false;
            bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
            resolveBinding(sprite, useArray, texture);

            if (!hasRun) {
                hasRun = true;
                runMode = mode;
                runUseArray = useArray;
                runTexture = texture;
            } else if (mode != runMode || useArray != runUseArray || texture.idx != runTexture.idx) {
                submitRun(runMode, runUseArray, runTexture);
                runMode = mode;
                runUseArray = useArray;
                runTexture = texture;
            }

            const uint16_t base = (uint16_t)(qidx * 4);
            m_runIndices.push_back((uint16_t)(base + 0));
            m_runIndices.push_back((uint16_t)(base + 1));
            m_runIndices.push_back((uint16_t)(base + 2));
            m_runIndices.push_back((uint16_t)(base + 2));
            m_runIndices.push_back((uint16_t)(base + 3));
            m_runIndices.push_back((uint16_t)(base + 0));
        }
        if (hasRun) {
            submitRun(runMode, runUseArray, runTexture);
        }
    }

    void ParticleSystem::renderFiltered(uint8_t _view, const float* _mtxView, const bx::Vec3& _eye,
                                        const uint16_t* _handles, uint16_t _handleCount)
    {
        if (_handleCount == 0 || !bgfx::isValid(m_program2D))
            return;

        // Count particles for the filtered emitters
        uint32_t totalParticles = 0;
        for (uint16_t i = 0; i < _handleCount; ++i)
        {
            uint16_t idx = _handles[i];
            if (idx < m_emitterAlloc->getMaxHandles())
                totalParticles += m_emitter[idx].m_num;
        }
        
        if (totalParticles == 0) return;

        bgfx::TransientVertexBuffer tvb;

        const uint32_t availVB = bgfx::getAvailTransientVertexBuffer(totalParticles*4, PosColorTexCoord0Vertex::ms_layout);
        const uint32_t availIB = bgfx::getAvailTransientIndexBuffer(totalParticles*6);
        const uint32_t maxDraw = bx::uint32_min(availVB/4, availIB/6);
        if (maxDraw == 0) return;

        bgfx::allocTransientVertexBuffer(&tvb, maxDraw*4, PosColorTexCoord0Vertex::ms_layout);

        PosColorTexCoord0Vertex* vertices = (PosColorTexCoord0Vertex*)tvb.data;
        
        // Use pre-allocated sort buffer - grow if needed (but don't shrink to avoid thrashing)
        if (maxDraw > m_sortBufCapacity) {
            if (m_sortBuf) bx::free(m_allocator, m_sortBuf);
            m_sortBufCapacity = maxDraw + maxDraw / 4; // Add 25% headroom
            m_sortBuf = (ParticleSort*)bx::alloc(m_allocator, sizeof(ParticleSort) * m_sortBufCapacity);
        }
        
        // Resize pre-allocated vectors (clear + resize is faster than recreating)
        m_modePerQuad.clear();
        m_modePerQuad.resize(maxDraw, 0);
        m_spritePerQuad.clear();
        m_spritePerQuad.resize(maxDraw, UINT16_MAX);
        m_runIndices.clear();
        m_runIndices.reserve(maxDraw * 6u);

        uint32_t pos = 0;
        for (uint16_t i = 0; i < _handleCount && pos < maxDraw; ++i)
        {
            uint16_t idx = _handles[i];
            if (idx >= m_emitterAlloc->getMaxHandles()) continue;
            
            Emitter& emitter = m_emitter[idx];

            const float uv[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
            const float spriteLayer = m_sprite.getArrayLayer(emitter.m_uniforms.m_handle);

            uint32_t start = pos;
            pos += emitter.render(uv, spriteLayer, _mtxView, _eye, pos, maxDraw, m_sortBuf, vertices);
            for (uint32_t q = start; q < pos; ++q)
            {
                m_modePerQuad[q] = (uint8_t)bx::uint32_min(emitter.m_uniforms.m_blendMode, 2u);
                m_spritePerQuad[q] = emitter.m_uniforms.m_handle.idx;
            }
        }

        if (pos == 0) return;

        // sort particles back-to-front
        qsort(m_sortBuf, pos, sizeof(ParticleSort), particleSortFn);
        float idMtx[16]; bx::mtxIdentity(idMtx);
        auto resolveBinding = [&](uint16_t spriteIdx, bool& outUseArray, bgfx::TextureHandle& outTexture)
        {
            EmitterSpriteHandle sprite{ spriteIdx };
            outUseArray = m_sprite.isArrayBacked(sprite);
            outTexture = m_sprite.getTexture(sprite);
            if (!bgfx::isValid(outTexture))
            {
                outUseArray = false;
                outTexture = m_defaultWhiteTexture;
            }
            if (outUseArray && !bgfx::isValid(m_programArray))
            {
                // Array sprites cannot be sampled by the 2D shader; render as white fallback.
                outUseArray = false;
                outTexture = m_defaultWhiteTexture;
            }
        };

        auto submitRun = [&](uint8_t mode, bool useArray, bgfx::TextureHandle runTexture)
        {
            if (m_runIndices.empty()) return;
            const uint32_t indexCount = (uint32_t)m_runIndices.size();
            if (bgfx::getAvailTransientIndexBuffer(indexCount) < indexCount) {
                m_runIndices.clear();
                return;
            }

            bgfx::TransientIndexBuffer tibLocal;
            bgfx::allocTransientIndexBuffer(&tibLocal, indexCount);
            bx::memCopy(tibLocal.data, m_runIndices.data(), indexCount * sizeof(uint16_t));

            uint64_t blendFlags = BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_INV_SRC_ALPHA);
            if (mode == 1) {
                blendFlags = BGFX_STATE_BLEND_ADD;
            } else if (mode == 2) {
                blendFlags = BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_DST_COLOR, BGFX_STATE_BLEND_ZERO);
            }

            bgfx::setTransform(idMtx);
            bgfx::setVertexBuffer(0, &tvb);
            bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
            if (useArray && bgfx::isValid(s_texColorArray) && bgfx::isValid(runTexture) && bgfx::isValid(m_programArray))
            {
                bgfx::setTexture(0, s_texColorArray, runTexture);
                program = m_programArray;
            }
            else
            {
                bgfx::TextureHandle tex2D = (!useArray && bgfx::isValid(runTexture)) ? runTexture : m_defaultWhiteTexture;
                if (!bgfx::isValid(s_texColor) || !bgfx::isValid(tex2D) || !bgfx::isValid(m_program2D))
                {
                    m_runIndices.clear();
                    return;
                }

                bgfx::setTexture(0, s_texColor, tex2D);
                program = m_program2D;
            }
            bgfx::setIndexBuffer(&tibLocal);
            if (bgfx::isValid(s_particleParams)) {
                const float params[4] = { float(mode), 0.0f, 0.0f, 0.0f };
                bgfx::setUniform(s_particleParams, params);
            }
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_LESS | blendFlags);
            if (bgfx::isValid(program))
            {
                bgfx::submit(_view, program);
            }
            m_runIndices.clear();
        };

        bool hasRun = false;
        uint8_t runMode = 0;
        bool runUseArray = false;
        bgfx::TextureHandle runTexture = BGFX_INVALID_HANDLE;
        for (uint32_t ii = 0; ii < pos; ++ii)
        {
            const ParticleSort& s = m_sortBuf[ii];
            const uint16_t qidx = (uint16_t)s.idx;
            const uint8_t mode = (qidx < m_modePerQuad.size()) ? m_modePerQuad[qidx] : 0;
            const uint16_t sprite = (qidx < m_spritePerQuad.size()) ? m_spritePerQuad[qidx] : UINT16_MAX;
            bool useArray = false;
            bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
            resolveBinding(sprite, useArray, texture);

            if (!hasRun) {
                hasRun = true;
                runMode = mode;
                runUseArray = useArray;
                runTexture = texture;
            } else if (mode != runMode || useArray != runUseArray || texture.idx != runTexture.idx) {
                submitRun(runMode, runUseArray, runTexture);
                runMode = mode;
                runUseArray = useArray;
                runTexture = texture;
            }

            const uint16_t base = (uint16_t)(qidx * 4);
            m_runIndices.push_back((uint16_t)(base + 0));
            m_runIndices.push_back((uint16_t)(base + 1));
            m_runIndices.push_back((uint16_t)(base + 2));
            m_runIndices.push_back((uint16_t)(base + 2));
            m_runIndices.push_back((uint16_t)(base + 3));
            m_runIndices.push_back((uint16_t)(base + 0));
        }
        if (hasRun) {
            submitRun(runMode, runUseArray, runTexture);
        }
    }

    EmitterHandle ParticleSystem::createEmitter(EmitterShape::Enum _shape, EmitterDirection::Enum _direction, uint32_t _maxParticles)
    {
        EmitterHandle h = { m_emitterAlloc->alloc() };
        if (h.idx != UINT16_MAX) m_emitter[h.idx].create(_shape, _direction, _maxParticles);
        return h;
    }

    void ParticleSystem::updateEmitter(EmitterHandle _handle, const EmitterUniforms* _uniforms)
    {
        if (!isValid(_handle)) return;
        Emitter& e = m_emitter[_handle.idx];
        if (_uniforms)
            bx::memCopy(&e.m_uniforms, _uniforms, sizeof(EmitterUniforms));
        else
            e.reset();
    }

    void ParticleSystem::getAabb(EmitterHandle _handle, bx::Aabb& _outAabb)
    {
        if (!isValid(_handle)) return;
        _outAabb = m_emitter[_handle.idx].m_aabb;
    }

    void ParticleSystem::destroyEmitter(EmitterHandle _handle)
    {
        if (!isValid(_handle)) return;
        m_emitter[_handle.idx].destroy();
        m_emitterAlloc->free(_handle.idx);
    }

    bool ParticleSystem::isEmitterAlive(EmitterHandle _handle) const
    {
        return isValid(_handle) && m_emitterAlloc && m_emitterAlloc->isValid(_handle.idx);
    }

    bool ParticleSystem::isSpriteAlive(EmitterSpriteHandle _handle) const
    {
        return isValid(_handle) && m_sprite.isValid(_handle);
    }

    uint32_t ParticleSystem::getEmitterParticleCount(EmitterHandle _handle) const
    {
        if (!isValid(_handle) || !m_emitterAlloc || !m_emitterAlloc->isValid(_handle.idx))
            return 0;
        return m_emitter[_handle.idx].m_num;
    }

    // -------------------------------------------------------------------------
    // Public API wrappers

    void init(uint16_t _maxEmitters, bx::AllocatorI* _allocator)    { s_ctx.init(_maxEmitters, _allocator); }
    void shutdown()                                                { s_ctx.shutdown(); }
    EmitterSpriteHandle createSprite(uint16_t _width, uint16_t _height, const void* _data){ return s_ctx.createSprite(_width, _height, _data);}    
    void destroySprite(EmitterSpriteHandle _handle)                { s_ctx.destroySprite(_handle); }
    EmitterHandle createEmitter(EmitterShape::Enum _shape, EmitterDirection::Enum _direction, uint32_t _maxParticles){ return s_ctx.createEmitter(_shape, _direction, _maxParticles);}    
    void updateEmitter(EmitterHandle _handle, const EmitterUniforms* _uniforms){ s_ctx.updateEmitter(_handle, _uniforms);}    
    void getAabb(EmitterHandle _handle, bx::Aabb& _outAabb)       { s_ctx.getAabb(_handle, _outAabb);}    
    void destroyEmitter(EmitterHandle _handle)                     { s_ctx.destroyEmitter(_handle);}    
    void update(float _dt)                                         { s_ctx.update(_dt);}    
    void updateEmitterParticles(EmitterHandle _handle, float _dt)  { s_ctx.updateEmitterParticles(_handle, _dt); }
    void render(uint8_t _view, const float* _mtxView, const bx::Vec3& _eye) { s_ctx.render(_view, _mtxView, _eye);}
    void renderFiltered(uint8_t _view, const float* _mtxView, const bx::Vec3& _eye, 
                        const uint16_t* _handles, uint16_t _handleCount) { s_ctx.renderFiltered(_view, _mtxView, _eye, _handles, _handleCount); }    
    bool IsEmitterAlive(EmitterHandle _handle)                     { return s_ctx.isEmitterAlive(_handle); }
    bool IsSpriteAlive(EmitterSpriteHandle _handle)                { return s_ctx.isSpriteAlive(_handle); }
    uint32_t GetEmitterParticleCount(EmitterHandle _handle)        { return s_ctx.getEmitterParticleCount(_handle); }
    uint16_t GetSpriteSlotCount()                                  { return s_ctx.m_sprite.m_handleAlloc.getNumHandles(); }
    uint16_t GetSpriteSlotCapacity()                               { return s_ctx.m_sprite.m_handleAlloc.getMaxHandles(); }
    uint16_t GetSpriteAtlasSize()                                  { return 0; }
    uint16_t GetSpriteCount()                                      { return GetSpriteSlotCount(); }
    uint16_t GetSpriteCapacity()                                   { return GetSpriteSlotCapacity(); }

    bool GetSpriteUV(EmitterSpriteHandle sprite, float uv[4])
    {
        if (!isValid(sprite))
            return false;

        uv[0] = 0.0f;
        uv[1] = 0.0f;
        uv[2] = 1.0f;
        uv[3] = 1.0f;
        return true;
    }

    bool IsSpriteArrayBacked(EmitterSpriteHandle sprite)
    {
        if (!isValid(sprite))
            return false;

        return s_ctx.m_sprite.isArrayBacked(sprite);
    }

    bgfx::TextureHandle GetSpriteTexture(EmitterSpriteHandle sprite)
    {
        return s_ctx.m_sprite.getTexture(sprite);
    }

    bgfx::TextureHandle GetTexture()
    {
        return s_ctx.m_defaultWhiteTexture;
    }

} // namespace ps
