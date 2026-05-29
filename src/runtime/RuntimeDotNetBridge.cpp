#include "core/managed/RuntimeHost.h"

#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/managed/ManagedScriptComponent.h"
#include "core/managed/ScriptInterop.h"
#include "core/managed/ScriptReflection.h"

#include <functional>
#include <memory>

namespace {

void CallOnValidateForEntity(Scene& scene, EntityID entityId) {
    const auto invoke = cm::runtime::GetScriptInvoke();
    if (!invoke || !SetManagedFieldPtr) {
        return;
    }

    EntityData* data = scene.GetEntityData(entityId);
    if (!data) {
        return;
    }

    for (auto& script : data->Scripts) {
        if (!script.Instance || script.Instance->GetBackend() != ScriptBackend::Managed) {
            continue;
        }

        auto managed = std::dynamic_pointer_cast<ManagedScriptComponent>(script.Instance);
        if (!managed) {
            continue;
        }

        void* scriptHandle = managed->GetHandle();
        if (!scriptHandle) {
            continue;
        }

        if (ScriptReflection::HasProperties(script.ClassName)) {
            auto& properties = ScriptReflection::GetScriptProperties(script.ClassName);
            for (auto& property : properties) {
                auto it = script.Values.find(property.name);
                if (it == script.Values.end()) {
                    continue;
                }

                const PropertyValue& value = it->second;
                if (property.type == PropertyType::List) {
                    const std::string listValue = ScriptReflection::PropertyValueToString(value);
                    SetManagedFieldPtr(scriptHandle, property.name.c_str(), (void*)listValue.c_str());
                } else {
                    void* boxed = ScriptReflection::ValueToBox(value);
                    SetManagedFieldPtr(scriptHandle, property.name.c_str(), boxed);
                }
            }
        }

        invoke(scriptHandle, "OnValidate");
    }
}

} // namespace

void CallOnValidateForAllScripts() {
    if (!Scene::CurrentScene) {
        return;
    }

    for (const auto& entity : Scene::CurrentScene->GetEntities()) {
        CallOnValidateForEntity(*Scene::CurrentScene, entity.GetID());
    }
}

void CallOnValidateForSubtree(Scene& scene, EntityID rootId) {
    std::function<void(EntityID)> visit = [&](EntityID entityId) {
        CallOnValidateForEntity(scene, entityId);

        EntityData* data = scene.GetEntityData(entityId);
        if (!data) {
            return;
        }

        for (EntityID childId : data->Children) {
            visit(childId);
        }
    };

    visit(rootId);
}

bool IsDotnetRuntimeReady() {
    return cm::runtime::IsDotNetReady();
}
