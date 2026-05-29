#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

#include "editor/nodegraph/NodeRegsitry.h"

struct NodeFieldDesc {
    std::string name;
    PinValueType type;
    float minValue{0.0f};
    float maxValue{0.0f};
    std::vector<std::string> options; // for enums
};

struct NodePinDesc {
    std::string name;
    PinKind kind;
    PinValueType type;
};

struct NodeTypeDescNative {
    NodeTypeId typeId;
    const char* name;
    std::vector<NodeFieldDesc> fields;
    std::vector<NodePinDesc> pins;
};

// Managed API descriptors
struct ManagedNodeGraphAPI {
    // Managed will enumerate all node types on demand
    void (*EnumerateNodeTypes)(void* user);
};

struct NativeNodeGraphAPI {
    void* user{nullptr};
    // Register a node type defined on managed side
    void (*RegisterNodeType)(const NodeTypeDescNative* desc);
};

// C API for bootstrap
typedef void (*GetManagedNodeGraphAPI)(const NativeNodeGraphAPI* nativeApi, ManagedNodeGraphAPI* outManagedApi);

// Entry points
void NodeGraph_SetManagedAPI(const ManagedNodeGraphAPI& api);
bool NodeGraph_NativeRegisterType(const NodeTypeDescNative& desc);
const ManagedNodeGraphAPI* NodeGraph_GetManagedAPI();


