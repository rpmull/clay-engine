#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>

// Binary format definitions (from BinaryFormats.h)
constexpr uint32_t PREFAB_MAGIC = 'P' | ('R' << 8) | ('F' << 16) | ('B' << 24);   // 'PRFB'

struct BinaryAssetHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t reserved;
};

struct PrefabBinaryHeader {
    BinaryAssetHeader base;
    uint32_t entityCount;
    uint32_t rootEntityIndex;
    uint32_t componentTableOffset;
    uint32_t stringTableOffset;
    uint32_t assetRefTableOffset;
    uint32_t modelDeltaTableOffset;
    uint32_t modelDeltaCount;
};

struct EntityHeader {
    uint64_t guidHigh;
    uint64_t guidLow;
    uint64_t parentGuidHigh;
    uint64_t parentGuidLow;
    uint32_t entityId;
    uint32_t nameIndex;
    uint8_t  flags;
    uint8_t  padding[3];
    int32_t  layer;
    uint32_t tagIndex;
    uint64_t modelGuidHigh;
    uint64_t modelGuidLow;
    uint32_t componentCount;
    uint32_t componentOffset;
};

enum class ComponentTypeId : uint16_t {
    None = 0,
    Transform = 1,
    Mesh = 2,
    MeshProxy = 3,
    Light = 4,
    Camera = 5,
    Skeleton = 6,
    Skinning = 7,
    BlendShape = 8,
    Collider = 9,
    RigidBody = 10,
    StaticBody = 11,
    CharacterController = 12,
    Terrain = 13,
    ParticleEmitter = 14,
    Canvas = 15,
    Panel = 16,
    Button = 17,
    Text = 18,
    NavMesh = 19,
    NavAgent = 20,
    AnimationPlayer = 21,
    RenderOverrides = 22,
    Area = 23,
    Script = 24,
    Module = 25,
    BoneAttachment = 26,
    UnifiedMorph = 27,
    TintController = 28,
    GrassDeformer = 29,
    River = 30,
    Slider = 31,
    ProgressBar = 32,
    Toggle = 33,
    ScrollView = 34,
    LayoutGroup = 35,
    InputField = 36,
    Dropdown = 37,
    IKConstraint = 38,
    LookAtConstraint = 39,
    Instancer = 40,
    UIRect = 41,
    FitToContent = 42,
    ResourceLayers = 43,
    MAX_COMPONENT_TYPE = 44
};

struct ComponentEntry {
    ComponentTypeId typeId;
    uint16_t flags;
    uint32_t dataSize;
    uint32_t dataOffset;
};

const char* ComponentTypeName(ComponentTypeId id) {
    switch (id) {
        case ComponentTypeId::Transform: return "Transform";
        case ComponentTypeId::Mesh: return "Mesh";
        case ComponentTypeId::Panel: return "Panel";
        case ComponentTypeId::Button: return "Button";
        case ComponentTypeId::Text: return "Text";
        case ComponentTypeId::Canvas: return "Canvas";
        default: return "Unknown";
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: debug_prefab_binary <prefab_file>" << std::endl;
        return 1;
    }

    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        std::cerr << "Failed to open file: " << argv[1] << std::endl;
        return 1;
    }

    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(fileSize);
    file.read(reinterpret_cast<char*>(data.data()), fileSize);

    if (data.size() < sizeof(PrefabBinaryHeader)) {
        std::cerr << "File too small" << std::endl;
        return 1;
    }

    PrefabBinaryHeader header;
    std::memcpy(&header, data.data(), sizeof(PrefabBinaryHeader));

    std::cout << "=== Prefab Binary Analysis ===" << std::endl;
    std::cout << "File size: " << fileSize << " bytes" << std::endl;
    std::cout << "Magic: 0x" << std::hex << header.base.magic << std::dec;
    if (header.base.magic == PREFAB_MAGIC) {
        std::cout << " (PRFB - OK)" << std::endl;
    } else {
        std::cout << " (INVALID!)" << std::endl;
        return 1;
    }
    std::cout << "Version: " << header.base.version << std::endl;
    std::cout << "Entity count: " << header.entityCount << std::endl;
    std::cout << "Root entity index: " << header.rootEntityIndex << std::endl;
    std::cout << "Component table offset: " << header.componentTableOffset << std::endl;
    std::cout << "String table offset: " << header.stringTableOffset << std::endl;
    std::cout << std::endl;

    // Read entity headers
    size_t headerSize = sizeof(PrefabBinaryHeader);
    size_t prefabGuidSize = 16;  // GUID high + low
    size_t prefabNameSize = 4;   // Name index
    size_t entityTableOffset = headerSize + prefabGuidSize + prefabNameSize;

    std::cout << "=== Entity Headers ===" << std::endl;
    for (uint32_t i = 0; i < header.entityCount; ++i) {
        size_t offset = entityTableOffset + i * sizeof(EntityHeader);
        if (offset + sizeof(EntityHeader) > data.size()) {
            std::cerr << "Entity header " << i << " out of bounds" << std::endl;
            break;
        }

        EntityHeader eh;
        std::memcpy(&eh, data.data() + offset, sizeof(EntityHeader));

        std::cout << "Entity " << i << ":" << std::endl;
        std::cout << "  Component count: " << eh.componentCount << std::endl;
        std::cout << "  Component offset: " << eh.componentOffset << std::endl;
        std::cout << "  Name index: " << eh.nameIndex << std::endl;

        // Read components for this entity
        if (eh.componentOffset < data.size() && eh.componentCount > 0) {
            std::cout << "  Components:" << std::endl;
            size_t compOffset = header.componentTableOffset + eh.componentOffset;
            
            for (uint32_t c = 0; c < eh.componentCount; ++c) {
                if (compOffset + sizeof(ComponentEntry) > data.size()) {
                    std::cerr << "    Component entry " << c << " out of bounds" << std::endl;
                    break;
                }

                ComponentEntry entry;
                std::memcpy(&entry, data.data() + compOffset, sizeof(ComponentEntry));

                std::cout << "    [" << c << "] Type: " << static_cast<int>(entry.typeId) 
                          << " (" << ComponentTypeName(entry.typeId) << ")"
                          << ", Size: " << entry.dataSize
                          << ", DataOffset: " << entry.dataOffset << std::endl;

                // Check if this is Panel
                if (entry.typeId == ComponentTypeId::Panel) {
                    std::cout << "      *** PANEL COMPONENT FOUND ***" << std::endl;
                    size_t panelDataOffset = header.componentTableOffset + eh.componentOffset + entry.dataOffset;
                    if (panelDataOffset + entry.dataSize <= data.size()) {
                        std::cout << "      Panel data at offset: " << panelDataOffset << std::endl;
                        std::cout << "      Panel data size: " << entry.dataSize << " bytes" << std::endl;
                    } else {
                        std::cerr << "      ERROR: Panel data out of bounds!" << std::endl;
                    }
                }

                compOffset += sizeof(ComponentEntry);
            }
        }
        std::cout << std::endl;
    }

    return 0;
}
