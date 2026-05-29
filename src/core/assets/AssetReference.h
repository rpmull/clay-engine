#pragma once
#include <string>
#include <cstdint>
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>

struct ClaymoreGUID {
    uint64_t high;
    uint64_t low;
    
    ClaymoreGUID() : high(0), low(0) {}
    ClaymoreGUID(uint64_t h, uint64_t l) : high(h), low(l) {}
    
    bool operator==(const ClaymoreGUID& other) const {
        return high == other.high && low == other.low;
    }
    
    bool operator!=(const ClaymoreGUID& other) const {
        return !(*this == other);
    }
    
    bool operator<(const ClaymoreGUID& other) const {
        if (high != other.high) return high < other.high;
        return low < other.low;
    }
    
    // Convert to string representation
    std::string ToString() const {
        std::stringstream ss;
        ss << std::hex << std::setfill('0') 
           << std::setw(16) << high 
           << std::setw(16) << low;
        return ss.str();
    }
    
    // Generate a new random GUID (thread-safe)
    static ClaymoreGUID Generate() {
        static thread_local std::mt19937_64 gen([]() {
            std::random_device rd;
            std::seed_seq seq{
                rd(), rd(), rd(), rd(),
                rd(), rd(), rd(), rd()
            };
            return std::mt19937_64(seq);
        }());
        static thread_local std::uniform_int_distribution<uint64_t> dis;
        
        return ClaymoreGUID(dis(gen), dis(gen));
    }
    
    // Create GUID from string (for loading from serialized data)
    static ClaymoreGUID FromString(const std::string& str) {
        if (str.length() != 32) return ClaymoreGUID();
        
        uint64_t h = 0, l = 0;
        std::stringstream ss(str.substr(0, 16));
        ss >> std::hex >> h;
        ss.clear();
        ss.str(str.substr(16, 16));
        ss >> std::hex >> l;
        
        return ClaymoreGUID(h, l);
    }
};

// Asset reference structure
struct AssetReference {
    ClaymoreGUID guid;
    int32_t fileID;  // Specific object within the asset file
    int32_t type;    // Asset type (3 = Mesh, 2 = Texture, etc.)
    
    enum class PrimitiveType : uint8_t {
        Cube = 0,
        Sphere = 1,
        Plane = 2,
        Capsule = 3,
        Unknown = 0xFF
    };

    AssetReference() : fileID(0), type(0) {}
    AssetReference(const ClaymoreGUID& g, int32_t fid = 0, int32_t t = 0) 
        : guid(g), fileID(fid), type(t) {}
    
    bool IsValid() const {
        return guid.high != 0 || guid.low != 0;
    }
    
    static PrimitiveType PrimitiveTypeFromName(const std::string& primitiveType) {
        if (primitiveType.empty()) return PrimitiveType::Unknown;
        std::string normalized;
        normalized.reserve(primitiveType.size());
        for (char c : primitiveType) {
            if (c == ' ' || c == '-' || c == '_') continue;
            normalized.push_back((char)std::tolower(static_cast<unsigned char>(c)));
        }
        if (normalized == "cube" || normalized == "debugcube") return PrimitiveType::Cube;
        if (normalized == "sphere") return PrimitiveType::Sphere;
        if (normalized == "plane" || normalized == "quad") return PrimitiveType::Plane;
        if (normalized == "capsule") return PrimitiveType::Capsule;
        return PrimitiveType::Unknown;
    }

    static const char* PrimitiveTypeToString(PrimitiveType type) {
        switch (type) {
        case PrimitiveType::Cube: return "Cube";
        case PrimitiveType::Sphere: return "Sphere";
        case PrimitiveType::Plane: return "Plane";
        case PrimitiveType::Capsule: return "Capsule";
        default: return "";
        }
    }

    static ClaymoreGUID PrimitiveGuidForType(PrimitiveType type) {
        switch (type) {
        case PrimitiveType::Cube:    return ClaymoreGUID::FromString("0000000000000000000000000000C001");
        case PrimitiveType::Sphere:  return ClaymoreGUID::FromString("0000000000000000000000000000C002");
        case PrimitiveType::Plane:   return ClaymoreGUID::FromString("0000000000000000000000000000C003");
        case PrimitiveType::Capsule: return ClaymoreGUID::FromString("0000000000000000000000000000C004");
        default:                     return ClaymoreGUID();
        }
    }

    static PrimitiveType PrimitiveTypeFromGuid(const ClaymoreGUID& guid) {
        if (guid == PrimitiveGuidForType(PrimitiveType::Cube)) return PrimitiveType::Cube;
        if (guid == PrimitiveGuidForType(PrimitiveType::Sphere)) return PrimitiveType::Sphere;
        if (guid == PrimitiveGuidForType(PrimitiveType::Plane)) return PrimitiveType::Plane;
        if (guid == PrimitiveGuidForType(PrimitiveType::Capsule)) return PrimitiveType::Capsule;
        return PrimitiveType::Unknown;
    }

    static bool IsPrimitiveGuid(const ClaymoreGUID& guid) {
        return PrimitiveTypeFromGuid(guid) != PrimitiveType::Unknown;
    }

    // For primitive types (cubes, spheres, etc.)
    static AssetReference CreatePrimitive(const std::string& primitiveType) {
        PrimitiveType typeEnum = PrimitiveTypeFromName(primitiveType);
        if (typeEnum == PrimitiveType::Unknown) return AssetReference();
        ClaymoreGUID guid = PrimitiveGuidForType(typeEnum);
        return AssetReference(guid, static_cast<int32_t>(typeEnum), 3);
    }
};

// JSON serialization for ClaymoreGUID and AssetReference
inline void to_json(nlohmann::json& j, const ClaymoreGUID& guid) {
    j = guid.ToString();
}

inline void from_json(const nlohmann::json& j, ClaymoreGUID& guid) {
    guid = ClaymoreGUID::FromString(j.get<std::string>());
}

inline void to_json(nlohmann::json& j, const AssetReference& ref) {
    j = {
        {"guid", ref.guid},
        {"fileID", ref.fileID},
        {"type", ref.type}
    };
}

inline void from_json(const nlohmann::json& j, AssetReference& ref) {
    j.at("guid").get_to(ref.guid);
    j.at("fileID").get_to(ref.fileID);
    j.at("type").get_to(ref.type);
}

// Hash function for ClaymoreGUID to work with std::unordered_map
namespace std {
    template<>
    struct hash<ClaymoreGUID> {
        std::size_t operator()(const ClaymoreGUID& guid) const {
            // Combine high and low parts for hashing
            return std::hash<uint64_t>{}(guid.high) ^ (std::hash<uint64_t>{}(guid.low) << 1);
        }
    };
} 