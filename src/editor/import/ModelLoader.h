#pragma once
#include <vector>
#include <string>
#include <glm/glm.hpp>
#include "core/rendering/Mesh.h"
#include "core/ecs/AnimationComponents.h"
#include "core/rendering/MaterialSource.h"

struct Model {
   std::vector<std::shared_ptr<Mesh>> Meshes;
   std::vector<MaterialSource> Materials;
   std::vector<std::string> MaterialSlotNames;
   std::vector<BlendShapeComponent> BlendShapes;
   std::vector<std::string> BoneNames;
   std::vector<glm::mat4> InverseBindPoses;
   std::string SourcePath;
   std::string SourceDirectory;
   std::string ModelName;
   // Scene graph info for fast instantiation without re-parsing
   glm::mat4 RootLocal;
   std::vector<glm::mat4> MeshTransforms;       // per aiScene mesh index, relative to root
   std::vector<std::string> MeshEntityNames;    // per aiScene mesh index, preferred node name
   std::vector<int> BoneParents;                // per bone index, parent bone index or -1
   };

class ModelLoader {
public:
    // Global import configuration (applies to all loads)
    static void SetFlipYAxis(bool enabled);
    static void SetFlipZAxis(bool enabled);
    static bool GetFlipYAxis();
    static bool GetFlipZAxis();
    static void SetRotateY180(bool enabled);
    static bool GetRotateY180();

    // Load a model using the current global configuration
    static Model LoadModel(const std::string& filepath);

private:
    static bool s_FlipY;
    static bool s_FlipZ;
    static bool s_RotateY180;
};