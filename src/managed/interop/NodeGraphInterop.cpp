#include "NodeGraphInterop.h"
#include <iostream>

static ManagedNodeGraphAPI g_ManagedNodeGraph{};
static NodeTypeRegistry g_GlobalNodeRegistry{};

namespace {
// Simple FNV-1a 32-bit hash for stable ids from names
static uint32_t HashName(const char* str)
{
    const uint32_t prime = 0x01000193u;
    uint32_t hash = 0x811C9DC5u;
    for (const unsigned char* s = (const unsigned char*)str; *s; ++s) { hash ^= *s; hash *= prime; }
    return hash ? hash : 1u;
}

static bool g_DefaultRegistered = false;
static void EnsureDefaultTypesRegistered()
{
    if (g_DefaultRegistered) return;
    g_DefaultRegistered = true;
    // "Empty" node: one event-like output named "Next"
    NodeType t{};
    t.name = "Empty";
    t.typeId = (NodeTypeId)HashName(t.name);
    t.Create = [](Graph& g, Node& n){
        // Ensure name and a single output pin
        if (n.name.empty()) n.name = "Empty";
        Pin out; out.id = g.NewPinId(); out.nodeId = n.id; out.kind = PinKind::Output; out.type = PinValueType::Event; out.name = "Next";
        g.pins.push_back(out); n.outputs.push_back(out.id);
    };
    g_GlobalNodeRegistry.Register(t);
}
}

bool NodeGraph_NativeRegisterType(const NodeTypeDescNative& desc)
{
    NodeType t{};
    t.typeId = desc.typeId;
    t.name = desc.name;
    // For now, leave Create/Serialize/Evaluate empty. Editor will still list the node type.
    g_GlobalNodeRegistry.Register(t);
    std::cout << "[NodeGraph] Registered managed node type: " << t.name << " (" << t.typeId << ")\n";
    return true;
}

void NodeGraph_SetManagedAPI(const ManagedNodeGraphAPI& api)
{
    g_ManagedNodeGraph = api;
}

const ManagedNodeGraphAPI* NodeGraph_GetManagedAPI()
{
    return g_ManagedNodeGraph.EnumerateNodeTypes ? &g_ManagedNodeGraph : nullptr;
}

// Simple accessor for panels to get registry (temporary global for MVP)
NodeTypeRegistry* GetGlobalNodeTypeRegistry()
{
    EnsureDefaultTypesRegistered();
    return &g_GlobalNodeRegistry;
}


