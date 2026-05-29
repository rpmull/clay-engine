#include "ScriptReflectionInterop.h"
#include <sstream>

// SetManagedFieldPtr is now defined in core/managed/ScriptInterop.cpp

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

    // Handle struct fields (structFieldsJson parsing would go here)
    // For now, struct fields are registered separately as nested properties
    
    // Mark if this field is auto-populated from resources
    info.populateFromResources = populateFromResources;
    
    // Mark if this ClayObject field should show as a dropdown selector
    info.selectFromResources = selectFromResources;

    ScriptReflection::RegisterScriptProperty(className, info);
}