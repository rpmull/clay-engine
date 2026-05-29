#pragma once
#include "ScriptReflection.h"
#include <glm/glm.hpp>

// This file demonstrates how script reflection would be set up
// In a real implementation, this would be called from the managed code interop

// Helper to create a simple PropertyInfo with default extended fields
inline PropertyInfo MakeSimpleProperty(const std::string& name, PropertyType type, const PropertyValue& defaultVal) {
    PropertyInfo info;
    info.name = name;
    info.type = type;
    info.defaultValue = defaultVal;
    info.currentValue = defaultVal;
    info.auxTypeName = std::string();
    info.enumMeta = {};
    info.listElementType = PropertyType::Int;
    info.listElementTypeName = std::string();
    info.structFields = {};
    info.getter = nullptr;
    info.setter = nullptr;
    return info;
}

inline void RegisterSampleScriptProperties() {
    // Example: PlayerController script with common properties
    ScriptReflection::RegisterScriptProperty("PlayerController", MakeSimpleProperty("Speed", PropertyType::Float, 5.0f));
    
    ScriptReflection::RegisterScriptProperty("PlayerController", MakeSimpleProperty("JumpHeight", PropertyType::Float, 2.0f));
    
    ScriptReflection::RegisterScriptProperty("PlayerController", MakeSimpleProperty("CanDoubleJump", PropertyType::Bool, true));

    // Example: EnemyAI script
    ScriptReflection::RegisterScriptProperty("EnemyAI", MakeSimpleProperty("PatrolRadius", PropertyType::Float, 10.0f));
    
    ScriptReflection::RegisterScriptProperty("EnemyAI", MakeSimpleProperty("DetectionRange", PropertyType::Float, 5.0f));
    
    ScriptReflection::RegisterScriptProperty("EnemyAI", MakeSimpleProperty("PatrolPoints", PropertyType::Int, 4));

    // Example: Transform Rotator script
    ScriptReflection::RegisterScriptProperty("TransformRotator", MakeSimpleProperty("RotationSpeed", PropertyType::Vector3, glm::vec3(0, 90, 0)));
    
    ScriptReflection::RegisterScriptProperty("TransformRotator", MakeSimpleProperty("RotateInLocalSpace", PropertyType::Bool, true));

    // Example: UI Text Display script
    ScriptReflection::RegisterScriptProperty("UITextDisplay", MakeSimpleProperty("DisplayText", PropertyType::String, std::string("Hello World")));
    
    ScriptReflection::RegisterScriptProperty("UITextDisplay", MakeSimpleProperty("UpdateInterval", PropertyType::Float, 1.0f));
}