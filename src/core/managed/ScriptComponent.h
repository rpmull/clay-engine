#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include "core/ecs/Entity.h"
#include "ScriptReflection.h"

enum class ScriptBackend {
   None,
   Native,
	Managed
	};

class ScriptComponent {
public:
	virtual ~ScriptComponent() = default;
	// OnBind: Called first to register script in ScriptRegistry before any OnCreate
	// This enables GetScript<T>() to work during OnCreate for cross-script references
	virtual void OnBind(Entity entity) {
		m_Entity = entity;
		}
	virtual void OnCreate(Entity entity) {
		m_Entity = entity;
		}
	virtual void OnUpdate(float dt) {}

	virtual std::shared_ptr<ScriptComponent> Clone() const = 0;

	virtual ScriptBackend GetBackend() const { return ScriptBackend::Native; }

protected:
	Entity m_Entity;
	};

struct ScriptInstance {
	std::string ClassName;
	std::shared_ptr<ScriptComponent> Instance = nullptr;
	// Per-entity reflected property overrides (by field name)
	std::unordered_map<std::string, PropertyValue> Values;
    // Optional metadata for entity-like fields to preserve GUID/path information
    std::unordered_map<std::string, ScriptEntityRefMetadata> EntityRefMetadata;
};

