#include "ScriptableSerialization.h"
#include "ScriptableRegistry.h"
#include <fstream>
#include <iostream>

using json = nlohmann::json;

namespace scriptableio {

static bool ParseTypeId(const std::string& hex, TypeId& out) {
    // Expect 32-hex chars possibly with dashes; strip non-hex
    std::string s; s.reserve(32);
    for (char c : hex) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) s.push_back((char)tolower(c));
    }
    if (s.size() < 16) return false;
    auto hexTo64 = [](const std::string& h)->uint64_t{
        uint64_t v = 0; for (char c : h) { v <<= 4; if (c>='0'&&c<='9') v|=(c-'0'); else if (c>='a'&&c<='f') v|=(c-'a'+10); }
        return v;
    };
    out.high = hexTo64(s.substr(0, 16));
    out.low  = hexTo64(s.substr(16, 16));
    return true;
}

static std::string TypeIdToHex(const TypeId& id) {
    auto toHex64 = [](uint64_t v){ char buf[17]; snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)v); return std::string(buf); };
    return toHex64(id.high) + toHex64(id.low);
}

ScriptableObject* LoadFromFile(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        std::cerr << "[ScriptableIO] Failed to open asset file: " << path << std::endl;
        return nullptr;
    }
    json j; try { in >> j; } catch(...) { std::cerr << "[ScriptableIO] JSON parse failed: " << path << std::endl; return nullptr; }
    TypeId type{}; std::string typeName; uint32_t version=1; ClaymoreGUID guid{};
    try {
        if (j.contains("typeId")) ParseTypeId(j.value("typeId", std::string()), type);
        typeName = j.value("typeName", std::string());
        version = j.value("version", 1u);
        if (j.contains("guid")) guid = ClaymoreGUID::FromString(j.value("guid", std::string()));
    } catch(...) {}
    const ScriptableTypeDesc* td = nullptr;
    if (type.IsValid()) td = ScriptableTypeRegistry::Get().Find(type);
    if (!td && !typeName.empty()) td = ScriptableTypeRegistry::Get().FindByName(typeName);
    if (!td) {
        std::cerr << "[ScriptableIO] Unknown type for .asset load (path=" << path << ", name=" << typeName << ")\n";
        return nullptr;
    }
    ScriptableObject* obj = reinterpret_cast<ScriptableObject*>(td->CreateNative ? td->CreateNative() : new ScriptableObject());
    obj->__SetType(td->id, td->fullName);
    obj->__SetGuid(guid);
    if (!obj->Deserialize(j, version)) {
        std::cerr << "[ScriptableIO] Failed to deserialize fields for " << td->fullName << " (" << path << ")\n";
    }
    return obj;
}

bool SaveToFile(const ScriptableObject& obj, const std::string& path, uint32_t version) {
    json j;
    j["typeId"] = TypeIdToHex(obj.GetTypeId());
    j["typeName"] = obj.GetTypeName();
    j["version"] = version;
    j["guid"] = obj.GetGuid().ToString();
    if (!obj.Serialize(j)) return false;
    std::ofstream out(path);
    if (!out.is_open()) return false;
    out << j.dump(4);
    return true;
}

}


