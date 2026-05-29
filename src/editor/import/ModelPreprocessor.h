#pragma once

#include "ModelLoader.h"

struct PreparedMeshEntry
{
    std::shared_ptr<Mesh> MeshData;
    std::vector<MaterialSource> Materials;
    std::vector<std::string> MaterialSlotNames;
    BlendShapeComponent BlendShapes;
    std::vector<int> SourceMeshIndices;
    glm::mat4 LocalTransform = glm::mat4(1.0f);
    std::string NodeName;
    bool Skinned = false;
};

struct PreparedProxyEntry
{
    std::string NodeName;
    std::string DisplayName;
    glm::mat4 LocalTransform = glm::mat4(1.0f);
    size_t MeshEntryIndex = 0;
    std::vector<uint32_t> SubmeshSlots;
    bool Skinned = false;
    int OriginalMeshIndex = -1;
};

struct PreparedSkeleton
{
    bool HasSkeleton = false;
    std::vector<std::string> BoneNames;
    std::vector<int> BoneParents;
    std::vector<glm::mat4> InverseBindPoses;
};

struct PreparedModel
{
    glm::mat4 RootLocal = glm::mat4(1.0f);
    PreparedSkeleton Skeleton;
    std::vector<PreparedMeshEntry> Meshes;
    std::vector<PreparedProxyEntry> Proxies;
};

PreparedModel BuildPreparedModel(const Model& model);

