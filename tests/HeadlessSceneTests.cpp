#include "TestFramework.h"
#include "HeadlessConfig.h"
#include "core/ecs/Scene.h"
#include "core/ecs/AnimationComponents.h"
#include "core/serialization/Serializer.h"
#include "editor/serialization/DeepCompare.h"
#include <cmath>
#include <memory>

static void BuildSimpleScene(Scene& scene) {
    Entity root = scene.CreateEntity("Root");
    if (auto* data = scene.GetEntityData(root.GetID())) {
        data->Transform.Position = { 1.25f, -0.5f, 2.0f };
        data->Transform.Scale = { 1.1f, 0.9f, 1.05f };
    }

    Entity child = scene.CreateEntity("Child");
    scene.SetParent(child.GetID(), root.GetID(), true);
    if (auto* data = scene.GetEntityData(child.GetID())) {
        data->Transform.Position = { -0.25f, 0.75f, 0.0f };
    }

    scene.CreateLight("TestLight", LightType::Point, { 1.0f, 0.9f, 0.8f }, 4.0f);
}

CM_TEST_NAMED(HeadlessSceneRoundTrip, "headless/scene_roundtrip") {
    auto& config = cm::test::GetHeadlessConfig();

    Scene source;
    if (!config.scenePath.empty()) {
        CM_ASSERT(Serializer::LoadSceneFromFile(config.scenePath, source));
    } else {
        BuildSimpleScene(source);
    }

    auto data = Serializer::SerializeScene(source);
    Scene roundTrip;
    CM_ASSERT(Serializer::DeserializeScene(data, roundTrip));

    auto diffs = cm::editor::DeepCompare(source, roundTrip, config.floatEpsilon);
    CM_ASSERT(diffs.empty());
}

CM_TEST_NAMED(SceneSetParentRootPreservesWorld, "scene/set_parent_root_preserves_world") {
    Scene scene;
    Entity root = scene.CreateEntity("Root");
    Entity child = scene.CreateEntity("Child");

    auto* rootData = scene.GetEntityData(root.GetID());
    auto* childData = scene.GetEntityData(child.GetID());
    CM_ASSERT(rootData != nullptr);
    CM_ASSERT(childData != nullptr);

    rootData->Transform.Position = { 3.0f, 0.0f, -2.0f };
    childData->Transform.Position = { 1.0f, 2.0f, 0.5f };
    scene.MarkTransformDirty(root.GetID());
    scene.MarkTransformDirty(child.GetID());
    scene.UpdateTransforms();

    scene.SetParent(child.GetID(), root.GetID(), true);
    scene.UpdateTransforms();
    const glm::vec3 parentedWorld = glm::vec3(childData->Transform.WorldMatrix[3]);

    scene.SetParent(child.GetID(), INVALID_ENTITY_ID, true);
    scene.UpdateTransforms();
    const glm::vec3 unparentedWorld = glm::vec3(childData->Transform.WorldMatrix[3]);

    CM_ASSERT(childData->Parent == INVALID_ENTITY_ID);
    CM_ASSERT(glm::length(parentedWorld - unparentedWorld) < 0.0001f);
    CM_ASSERT(glm::length(childData->Transform.Position - unparentedWorld) < 0.0001f);
}

CM_TEST_NAMED(UnifiedMorphPropagatesAllMembers, "morph/unified_propagates_all_members") {
    Scene scene;
    Entity root = scene.CreateEntity("HumanRoot");
    Entity body = scene.CreateEntity("BodyMesh");
    Entity armor = scene.CreateEntity("ArmorMesh");
    scene.SetParent(body.GetID(), root.GetID(), true);
    scene.SetParent(armor.GetID(), root.GetID(), true);

    auto* rootData = scene.GetEntityData(root.GetID());
    auto* bodyData = scene.GetEntityData(body.GetID());
    auto* armorData = scene.GetEntityData(armor.GetID());
    CM_ASSERT(rootData != nullptr);
    CM_ASSERT(bodyData != nullptr);
    CM_ASSERT(armorData != nullptr);

    rootData->UnifiedMorph = std::make_unique<UnifiedMorphComponent>();
    rootData->UnifiedMorph->Names = { "TorsoWidth" };
    rootData->UnifiedMorph->Weights = { 0.625f };
    rootData->UnifiedMorph->MemberMeshes = { body.GetID(), armor.GetID() };

    bodyData->BlendShapes = std::make_unique<BlendShapeComponent>();
    armorData->BlendShapes = std::make_unique<BlendShapeComponent>();
    bodyData->BlendShapes->Shapes.push_back(BlendShape{ "TorsoWidth" });
    armorData->BlendShapes->Shapes.push_back(BlendShape{ "TorsoWidth" });

    scene.PropagateUnifiedMorphWeights(root.GetID());

    CM_ASSERT(std::abs(bodyData->BlendShapes->Shapes[0].Weight - 0.625f) < 0.0001f);
    CM_ASSERT(std::abs(armorData->BlendShapes->Shapes[0].Weight - 0.625f) < 0.0001f);
    CM_ASSERT(bodyData->BlendShapes->Dirty);
    CM_ASSERT(armorData->BlendShapes->Dirty);
}
