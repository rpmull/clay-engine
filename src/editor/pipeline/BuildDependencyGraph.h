#pragma once

#include <string>
#include <unordered_map>
#include <vector>

class BuildDependencyGraph {
public:
    enum class NodeKind {
        EntryScene,
        Asset,
        Resource,
        ResourceDependency,
        GeneratedRuntimeAsset,
        RuntimeSupport
    };

    struct Node {
        std::string key;
        std::string path;
        NodeKind kind = NodeKind::Asset;
        bool root = false;
        bool exists = false;
        bool buildable = false;
    };

    struct Edge {
        std::string from;
        std::string to;
        std::string reason;
    };

    void AddEntryScene(const std::string& scenePath);
    void AddProjectResources();
    void AddExplicitAsset(const std::string& assetPath,
                          NodeKind kind,
                          const std::string& reason,
                          const std::string& fromKey = {});

    const std::unordered_map<std::string, Node>& GetNodes() const { return m_Nodes; }
    const std::vector<Edge>& GetEdges() const { return m_Edges; }

    std::vector<std::string> FlattenExistingPaths() const;

private:
    void AddFileRecursive(const std::string& assetPath,
                          NodeKind kind,
                          const std::string& reason,
                          const std::string& fromKey);

    std::string NormalizeKey(const std::string& path) const;

    std::unordered_map<std::string, Node> m_Nodes;
    std::vector<std::string> m_Order;
    std::vector<Edge> m_Edges;
};
