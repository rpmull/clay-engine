#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <optional>
#include <glm/glm.hpp>

#include "core/ecs/Entity.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/Scene.h"
#include "managed/interop/ScriptReflectionInterop.h" // SetManagedFieldPtr
#include "managed/interop/ManagedScriptComponent.h"

namespace cm {
namespace animation {

// Simple easing enumeration and helpers
enum class EasingType : int {
    Linear = 0,
    EaseInOutQuad = 1,
    EaseOutCubic = 2,
};

inline float Ease(EasingType e, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    switch (e) {
        case EasingType::EaseInOutQuad:
            return t < 0.5f ? 2.0f * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) * 0.5f;
        case EasingType::EaseOutCubic:
            return 1.0f - std::pow(1.0f - t, 3.0f);
        default:
            return t;
    }
}

struct ITween {
    virtual ~ITween() = default;
    // Returns true when tween completed
    virtual bool Update(Scene& scene, float dt) = 0;
};

class FloatTween : public ITween {
public:
    using Setter = std::function<void(float)>;

    FloatTween(float from, float to, float duration, EasingType easing, Setter setter)
        : m_From(from), m_To(to), m_Duration(std::max(0.0001f, duration)), m_Easing(easing), m_Setter(std::move(setter)) {}

    bool Update(Scene&, float dt) override {
        if (m_Elapsed == 0.0f) m_Setter(m_From);
        m_Elapsed += dt;
        float t = std::clamp(m_Elapsed / m_Duration, 0.0f, 1.0f);
        float v = m_From + (m_To - m_From) * Ease(m_Easing, t);
        m_Setter(v);
        return m_Elapsed >= m_Duration;
    }

private:
    float m_From;
    float m_To;
    float m_Duration;
    float m_Elapsed = 0.0f;
    EasingType m_Easing = EasingType::Linear;
    Setter m_Setter;
};

class Vec3Tween : public ITween {
public:
    using Setter = std::function<void(const glm::vec3&)>;

    Vec3Tween(const glm::vec3& from, const glm::vec3& to, float duration, EasingType easing, Setter setter)
        : m_From(from), m_To(to), m_Duration(std::max(0.0001f, duration)), m_Easing(easing), m_Setter(std::move(setter)) {}

    bool Update(Scene&, float dt) override {
        if (m_Elapsed == 0.0f) m_Setter(m_From);
        m_Elapsed += dt;
        float t = std::clamp(m_Elapsed / m_Duration, 0.0f, 1.0f);
        float k = Ease(m_Easing, t);
        glm::vec3 v = m_From + (m_To - m_From) * k;
        m_Setter(v);
        return m_Elapsed >= m_Duration;
    }

private:
    glm::vec3 m_From;
    glm::vec3 m_To;
    float m_Duration;
    float m_Elapsed = 0.0f;
    EasingType m_Easing = EasingType::Linear;
    Setter m_Setter;
};

// Header-only manager
class TweenManager {
public:
    static TweenManager& Get() {
        static TweenManager s;
        return s;
    }

    void Update(Scene& scene, float dt) {
        if (m_Tweens.empty()) return;
        size_t write = 0;
        for (size_t read = 0; read < m_Tweens.size(); ++read) {
            bool done = m_Tweens[read]->Update(scene, dt);
            if (!done) {
                if (write != read) m_Tweens[write] = std::move(m_Tweens[read]);
                ++write;
            }
        }
        m_Tweens.resize(write);
    }

    void ClearAll() { m_Tweens.clear(); }

    // Completion callback wiring (set by interop)
    using FinishedFn = void(*)(int, const char*);
    static void SetFinishedCallback(FinishedFn cb) { FinishedCallbackRef() = cb; }

    // Generic adders
    void Add(std::unique_ptr<ITween> tween) { m_Tweens.emplace_back(std::move(tween)); }

    // Add a tween that fires a completion callback tag for a given entity
    void AddTagged(std::unique_ptr<ITween> tween, int entityId, const char* tag) {
        struct CallbackTween : public ITween {
            std::unique_ptr<ITween> inner;
            int id;
            std::string tg;
            CallbackTween(std::unique_ptr<ITween> tw, int entity, const char* at)
                : inner(std::move(tw)), id(entity), tg(at ? at : "") {}
            bool Update(Scene& scene, float dt) override {
                if (!inner) return true;
                bool done = inner->Update(scene, dt);
                if (done) {
                    auto& cb = FinishedCallbackRef();
                    if (cb) cb(id, tg.c_str());
                }
                return done;
            }
        };
        m_Tweens.emplace_back(std::make_unique<CallbackTween>(std::move(tween), entityId, tag));
    }

    // Convenience: Transform tweens
    void TweenPosition(EntityID id, const glm::vec3& to, float duration, EasingType easing = EasingType::Linear) {
        Scene& sc = Scene::Get();
        auto* d = sc.GetEntityData(id);
        if (!d) return;
        glm::vec3 from = d->Transform.Position;
        AddTagged(std::make_unique<Vec3Tween>(from, to, duration, easing, [id](const glm::vec3& v){
            auto* dd = Scene::Get().GetEntityData(id);
            if (!dd) return;
            dd->Transform.Position = v;
            Scene::Get().MarkTransformDirty(id);
        }), id, "position");
    }

    void TweenRotationEuler(EntityID id, const glm::vec3& toDeg, float duration, EasingType easing = EasingType::Linear) {
        Scene& sc = Scene::Get();
        auto* d = sc.GetEntityData(id);
        if (!d) return;
        // Build start and end quaternions from Euler degrees (yaw, pitch, roll)
        glm::mat4 mFrom = glm::yawPitchRoll(glm::radians(d->Transform.Rotation.y), glm::radians(d->Transform.Rotation.x), glm::radians(d->Transform.Rotation.z));
        glm::quat qFrom = glm::normalize(glm::quat_cast(mFrom));
        glm::mat4 mTo = glm::yawPitchRoll(glm::radians(toDeg.y), glm::radians(toDeg.x), glm::radians(toDeg.z));
        glm::quat qTo = glm::normalize(glm::quat_cast(mTo));
        AddTagged(std::make_unique<FloatTween>(0.0f, 1.0f, duration, easing, [id, qFrom, qTo](float t){
            auto* dd = Scene::Get().GetEntityData(id);
            if (!dd) return;
            glm::quat q = glm::normalize(glm::slerp(qFrom, qTo, t));
            dd->Transform.RotationQ = q;
            // Keep Euler in degrees for UI display only
            glm::vec3 eulerRad = glm::eulerAngles(q);
            dd->Transform.Rotation = glm::degrees(eulerRad);
            dd->Transform.UseQuatRotation = true;
            Scene::Get().MarkTransformDirty(id);
        }), id, "rotation");
    }

    void TweenScale(EntityID id, const glm::vec3& to, float duration, EasingType easing = EasingType::Linear) {
        Scene& sc = Scene::Get();
        auto* d = sc.GetEntityData(id);
        if (!d) return;
        glm::vec3 from = d->Transform.Scale;
        AddTagged(std::make_unique<Vec3Tween>(from, to, duration, easing, [id](const glm::vec3& v){
            auto* dd = Scene::Get().GetEntityData(id);
            if (!dd) return;
            dd->Transform.Scale = v;
            Scene::Get().MarkTransformDirty(id);
        }), id, "scale");
    }

    void TweenLightIntensity(EntityID id, float to, float duration, EasingType easing = EasingType::Linear) {
        Scene& sc = Scene::Get();
        auto* d = sc.GetEntityData(id);
        if (!d || !d->Light) return;
        float from = d->Light->Intensity;
        AddTagged(std::make_unique<FloatTween>(from, to, duration, easing, [id](float v){
            auto* dd = Scene::Get().GetEntityData(id);
            if (!dd || !dd->Light) return;
            dd->Light->Intensity = v;
        }), id, "light");
    }

    // Managed property tweens
    void TweenManagedFloat(EntityID id, const char* classNameOrNull, const char* field, float to, float duration, EasingType easing = EasingType::Linear) {
        auto fromOpt = GetManagedFloat(id, classNameOrNull, field);
        if (!fromOpt.has_value()) return;
        float from = *fromOpt;
        std::string fieldCopy = field ? field : "";
        std::string classCopy = classNameOrNull ? classNameOrNull : "";
        std::string tag = std::string("mfloat:") + (classCopy.empty() ? std::string("?") : classCopy) + "." + fieldCopy;
        AddTagged(std::make_unique<FloatTween>(from, to, duration, easing, [id, fieldCopy, classCopy](float v){
            SetManagedFloat(id, classCopy.empty() ? nullptr : classCopy.c_str(), fieldCopy.c_str(), v);
        }), id, tag.c_str());
    }

    void TweenManagedVec3(EntityID id, const char* classNameOrNull, const char* field, const glm::vec3& to, float duration, EasingType easing = EasingType::Linear) {
        auto fromOpt = GetManagedVec3(id, classNameOrNull, field);
        if (!fromOpt.has_value()) return;
        glm::vec3 from = *fromOpt;
        std::string fieldCopy = field ? field : "";
        std::string classCopy = classNameOrNull ? classNameOrNull : "";
        std::string tag = std::string("mvec3:") + (classCopy.empty() ? std::string("?") : classCopy) + "." + fieldCopy;
        AddTagged(std::make_unique<Vec3Tween>(from, to, duration, easing, [id, fieldCopy, classCopy](const glm::vec3& v){
            SetManagedVec3(id, classCopy.empty() ? nullptr : classCopy.c_str(), fieldCopy.c_str(), v);
        }), id, tag.c_str());
    }

private:
    // Store finished-callback function pointer without ODR issues
    static FinishedFn& FinishedCallbackRef() { static FinishedFn fn = nullptr; return fn; }
    // Helpers for managed fields
    static void* FindManagedHandle(EntityID id, const char* classNameOrNull) {
        auto* d = Scene::Get().GetEntityData(id);
        if (!d) return nullptr;
        for (auto& s : d->Scripts) {
            if (!s.Instance) continue;
            if (s.Instance->GetBackend() != ScriptBackend::Managed) continue;
            if (classNameOrNull && s.ClassName != classNameOrNull) continue;
            auto managed = std::dynamic_pointer_cast<ManagedScriptComponent>(s.Instance);
            if (managed && managed->GetHandle()) return managed->GetHandle();
        }
        return nullptr;
    }

    static void SetManagedFloat(EntityID id, const char* classNameOrNull, const char* field, float value) {
        if (!SetManagedFieldPtr || !field) return;
        void* h = FindManagedHandle(id, classNameOrNull);
        if (!h) return;
        float tmp = value;
        SetManagedFieldPtr(h, field, &tmp);
    }

    struct V3Box { float X, Y, Z; };

    static void SetManagedVec3(EntityID id, const char* classNameOrNull, const char* field, const glm::vec3& v) {
        if (!SetManagedFieldPtr || !field) return;
        void* h = FindManagedHandle(id, classNameOrNull);
        if (!h) return;
        V3Box tmp{ v.x, v.y, v.z };
        SetManagedFieldPtr(h, field, &tmp);
    }

    static std::optional<float> GetManagedFloat(EntityID id, const char* classNameOrNull, const char* field) {
        // No generic getter; approximate by reading from reflection cache is non-trivial here.
        // Start from 0 to produce a deterministic tween if we can't inspect current value.
        (void)id; (void)classNameOrNull; (void)field;
        return 0.0f;
    }

    static std::optional<glm::vec3> GetManagedVec3(EntityID id, const char* classNameOrNull, const char* field) {
        (void)id; (void)classNameOrNull; (void)field;
        return glm::vec3(0.0f);
    }

private:
    std::vector<std::unique_ptr<ITween>> m_Tweens;
};

} // namespace animation
} // namespace cm


