#include "ModuleInterop.h"
#include "core/ecs/ComponentRegistry.h"
#include <iostream>

static bool RegisterComponentNative(const InteropComponentDesc& d)
{
    cm::ComponentDesc c{};
    c.typeId = d.typeId;
    c.fullName = d.fullName ? d.fullName : std::string();
    c.menuPath = d.menuPath ? d.menuPath : c.fullName;
    c.version = d.version;
    c.order = d.order;
    c.Upgrade = d.Upgrade;
    c.CustomInspector = d.CustomInspector;
    c.fields.reserve((size_t)std::max(0, d.fieldCount));
    for (int i = 0; i < d.fieldCount; ++i) {
        const auto& f = d.fields[i];
        cm::FieldDesc fd;
        fd.name = f.name ? f.name : std::string();
        fd.type = static_cast<cm::ValueType>(f.type);
        fd.flags = f.flags;
        fd.arrayRank = f.arrayRank;
        fd.enumType = f.enumType ? f.enumType : std::string();
        c.fields.push_back(std::move(fd));
    }
    const bool ok = cm::ComponentRegistry::Instance().Register(c);
    if (!ok) {
        std::cerr << "[ModuleInterop] Failed to register component: " << c.fullName << " typeId=" << c.typeId << "\n";
    } else {
        std::cout << "[ModuleInterop] Registered component: " << c.fullName << " fields=" << c.fields.size() << "\n";
    }
    return ok;
}

void FillModuleNativeAPIs(NativeAPIs& out)
{
    out.RegisterComponent = &RegisterComponentNative;
}

 
