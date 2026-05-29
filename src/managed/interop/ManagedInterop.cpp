#include "ScriptReflection.h"
#include <cstring>
#include <iostream>

extern "C"
{
    __declspec(dllexport) void RegisterScriptProperty(
        const char* scriptClass,
        const char* propName,
        int propertyType,
        const char* defaultValue)
    {
        if(!scriptClass || !propName) return;

        PropertyType type = static_cast<PropertyType>(propertyType);
        PropertyValue defVal;

        // Parse defaultValue string according to type
        std::string ddl = defaultValue ? defaultValue : "";
        defVal = ScriptReflection::StringToPropertyValue(ddl, type);

        PropertyInfo info;
        info.name = propName;
        info.type = type;
        info.defaultValue = defVal;
        info.currentValue = defVal;
        info.auxTypeName = std::string();
        info.enumMeta = {};
        info.listElementType = PropertyType::Int;  // Default; only used for List types
        info.listElementTypeName = std::string();
        info.structFields = {};
        info.getter = nullptr;
        info.setter = nullptr;
        ScriptReflection::RegisterScriptProperty(scriptClass, info);

        std::cout << "[Interop] Registered property '" << propName << "' for script " << scriptClass << std::endl;
    }
}
