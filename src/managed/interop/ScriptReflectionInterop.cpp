#include "ScriptReflectionInterop.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <sstream>

// SetManagedFieldPtr is now defined in core/managed/ScriptInterop.cpp

namespace {

using json = nlohmann::json;

static PropertyInfo ParseStructFieldMetadata(const json& fieldJson)
{
    PropertyInfo info;
    info.name = fieldJson.value("name", std::string());
    info.type = static_cast<PropertyType>(fieldJson.value("type", 0));
    info.defaultValue = ScriptReflection::BoxToValue(nullptr, info.type);
    info.currentValue = info.defaultValue;

    if (fieldJson.contains("aux") && fieldJson["aux"].is_string()) {
        info.auxTypeName = fieldJson["aux"].get<std::string>();
    }

    if (fieldJson.contains("enumNames") && fieldJson["enumNames"].is_array()) {
        for (const auto& name : fieldJson["enumNames"]) {
            if (name.is_string()) {
                info.enumMeta.names.push_back(name.get<std::string>());
            }
        }
    }

    if (fieldJson.contains("enumValues") && fieldJson["enumValues"].is_array()) {
        for (const auto& value : fieldJson["enumValues"]) {
            if (value.is_number_integer()) {
                info.enumMeta.values.push_back(value.get<int>());
            }
        }
    }

    if (info.type == PropertyType::List) {
        info.listElementType = static_cast<PropertyType>(fieldJson.value("listElementType", 0));
        if (fieldJson.contains("listElementTypeName") && fieldJson["listElementTypeName"].is_string()) {
            info.listElementTypeName = fieldJson["listElementTypeName"].get<std::string>();
        }
    }

    info.populateFromResources = fieldJson.value("populateFromResources", false);
    info.selectFromResources = fieldJson.value("selectFromResources", false);

    if (fieldJson.contains("structFields") && fieldJson["structFields"].is_array()) {
        for (const auto& nested : fieldJson["structFields"]) {
            info.structFields.push_back(ParseStructFieldMetadata(nested));
        }
    }

    return info;
}

static void ApplyStructFieldMetadata(PropertyInfo& info, const char* structFieldsJson)
{
    if (!structFieldsJson || !*structFieldsJson) {
        return;
    }

    try {
        json parsed = json::parse(structFieldsJson);
        if (!parsed.is_array()) {
            return;
        }

        for (const auto& fieldJson : parsed) {
            info.structFields.push_back(ParseStructFieldMetadata(fieldJson));
        }
    } catch (const std::exception& ex) {
        std::cerr << "[ScriptReflectionInterop] Failed to parse structFieldsJson for "
                  << info.name << ": " << ex.what() << std::endl;
    }
}

} // namespace

// Helper to split a pipe-separated string
static std::vector<std::string> SplitPipe(const char* str) {
    std::vector<std::string> result;
    if (!str) return result;
    std::string s(str);
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, '|')) {
        result.push_back(item);
    }
    return result;
}

// Helper to parse pipe-separated integers
static std::vector<int> SplitPipeInt(const char* str) {
    std::vector<int> result;
    if (!str) return result;
    std::string s(str);
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, '|')) {
        try { result.push_back(std::stoi(item)); }
        catch (...) { result.push_back(0); }
    }
    return result;
}

extern "C" __declspec(dllexport)
void ClearScriptPropertiesNative(const char* className)
{
    if (!className) return;
    ScriptReflection::ClearScriptPropertiesForClass(className);
}

extern "C" __declspec(dllexport)
void RegisterScriptPropertyNative(const char* className,
                                  const char* fieldName,
                                  int propType,
                                  void* boxedDefault,
                                  const char* auxTypeFullName)
{
    PropertyInfo info;
    info.name = fieldName;
    info.type = static_cast<PropertyType>(propType);
    info.currentValue = ScriptReflection::BoxToValue(boxedDefault, info.type);
    // defaultValue can mirror currentValue for now
    info.defaultValue = info.currentValue;
    if (auxTypeFullName) info.auxTypeName = auxTypeFullName;

    ScriptReflection::RegisterScriptProperty(className, info);
}

extern "C" __declspec(dllexport)
void RegisterScriptPropertyExtended(const char* className,
                                    const char* fieldName,
                                    int propType,
                                    void* boxedDefault,
                                    const char* auxTypeFullName,
                                    const char* enumNames,
                                    const char* enumValues,
                                    int listElementType,
                                    const char* listElementTypeName,
                                    const char* structFieldsJson,
                                    bool populateFromResources,
                                    bool selectFromResources)
{
    PropertyInfo info;
    info.name = fieldName;
    info.type = static_cast<PropertyType>(propType);
    info.currentValue = ScriptReflection::BoxToValue(boxedDefault, info.type);
    info.defaultValue = info.currentValue;
    if (auxTypeFullName) info.auxTypeName = auxTypeFullName;

    // Handle enum metadata - store for both direct enum properties AND list elements that are enums
    if (enumNames && enumValues) {
        info.enumMeta.names = SplitPipe(enumNames);
        info.enumMeta.values = SplitPipeInt(enumValues);
    }

    // Handle list metadata
    if (info.type == PropertyType::List) {
        info.listElementType = static_cast<PropertyType>(listElementType);
        if (listElementTypeName) info.listElementTypeName = listElementTypeName;
        
        // If list element type is Enum, enum metadata should already be set above
        // The inspector will use property.enumMeta for rendering enum list elements
    }

    ApplyStructFieldMetadata(info, structFieldsJson);
    
    // Mark if this field is auto-populated from resources
    info.populateFromResources = populateFromResources;
    
    // Mark if this ClayObject field should show as a dropdown selector
    info.selectFromResources = selectFromResources;

    ScriptReflection::RegisterScriptProperty(className, info);
}
