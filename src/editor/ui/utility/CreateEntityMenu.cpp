#include "CreateEntityMenu.h"
#include "core/rendering/StandardMeshManager.h"
#include "core/rendering/MaterialManager.h"
#include "core/assets/AssetReference.h"
#include "core/ecs/Components.h"
#include "core/ecs/InstancerComponent.h"
#include <cmath>
#include <utility>

namespace {
CanvasComponent* FindAncestorCanvas(Scene* context, EntityID parentId)
{
    if (!context || parentId == INVALID_ENTITY_ID)
        return nullptr;

    EntityID current = parentId;
    while (current != INVALID_ENTITY_ID) {
        auto* data = context->GetEntityData(current);
        if (!data)
            break;

        if (data->Canvas)
            return data->Canvas.get();

        current = data->Parent;
    }

    return nullptr;
}

bool IsUICreationCandidate(const EntityData* data)
{
    if (!data)
        return false;

    return data->Panel
        || data->Button
        || data->Slider
        || data->ProgressBar
        || data->Toggle
        || data->ScrollView
        || data->LayoutGroup
        || data->InputField
        || data->Dropdown
        || data->Text
        || data->Canvas;
}

bool IsLegacyScreenSpaceUIEntity(const EntityData* data)
{
    if (!data)
        return false;

    return data->Panel
        || data->Button
        || data->Slider
        || data->ProgressBar
        || data->Toggle
        || data->ScrollView
        || data->LayoutGroup
        || data->InputField
        || data->Dropdown
        || (data->Text && !data->Text->WorldSpace);
}

bool ShouldDefaultChildUIToWorldSpace(Scene* context, EntityID parentId)
{
    if (!context || parentId == INVALID_ENTITY_ID)
        return false;

    EntityID current = parentId;
    while (current != INVALID_ENTITY_ID) {
        auto* data = context->GetEntityData(current);
        if (!data)
            break;

        if (data->Canvas)
            return data->Canvas->Space == CanvasComponent::RenderSpace::WorldSpace;

        if (data->Text)
            return data->Text->WorldSpace;

        if (IsLegacyScreenSpaceUIEntity(data))
            return false;

        current = data->Parent;
    }

    return true;
}

std::pair<int, int> ComputeSuggestedWorldCanvasSize(const EntityData* data)
{
    float width = 0.0f;
    float height = 0.0f;

    if (data && data->Panel) {
        width = std::max(width, std::abs(data->Panel->Size.x * data->Panel->Scale.x));
        height = std::max(height, std::abs(data->Panel->Size.y * data->Panel->Scale.y));
    }

    if (data && data->Text) {
        const float pixelSize = std::max(data->Text->PixelSize, 1.0f);
        const float fallbackWidth = data->Text->RectSize.x > 0.0f
            ? data->Text->RectSize.x
            : pixelSize * std::max<size_t>(data->Text->Text.empty() ? 8u : data->Text->Text.size(), 4u) * 0.6f;
        const float fallbackHeight = data->Text->RectSize.y > 0.0f
            ? data->Text->RectSize.y
            : pixelSize * 1.5f;
        width = std::max(width, fallbackWidth);
        height = std::max(height, fallbackHeight);
    }

    if (width <= 0.0f)
        width = 256.0f;
    if (height <= 0.0f)
        height = 256.0f;

    return {
        std::max(1, static_cast<int>(std::ceil(width))),
        std::max(1, static_cast<int>(std::ceil(height)))
    };
}

void NormalizeWorldSpaceRootUILayout(EntityData* data)
{
    if (!data)
        return;

    if (data->Panel) {
        if (data->UIRect && data->UIRect->AnchorToParent) {
            data->UIRect->Offset = glm::vec2(0.0f);
        } else {
            data->Panel->AnchorEnabled = false;
            data->Panel->AnchorToParentUI = false;
            data->Panel->AnchorOffset = glm::vec2(0.0f);
            data->Panel->Position = glm::vec2(0.0f);
        }
    }

    if (data->Text && !data->Text->WorldSpace) {
        if (data->UIRect && data->UIRect->AnchorToParent) {
            data->UIRect->Offset = glm::vec2(0.0f);
        } else {
            data->Text->AnchorEnabled = false;
            data->Text->AnchorToParentUI = false;
            data->Text->AnchorOffset = glm::vec2(0.0f);
            data->Transform.Position.x = 0.0f;
            data->Transform.Position.y = 0.0f;
        }
    }

    if (!data->Panel && !data->Text && data->UIRect) {
        if (data->UIRect->AnchorToParent) {
            data->UIRect->Offset = glm::vec2(0.0f);
        } else {
            data->Transform.Position.x = 0.0f;
            data->Transform.Position.y = 0.0f;
        }
    }
}

void PromoteToWorldSpaceUIRoot(EntityData* data)
{
    if (!data)
        return;

    if (!data->Canvas)
        data->Canvas = std::make_unique<CanvasComponent>();

    data->Canvas->Space = CanvasComponent::RenderSpace::WorldSpace;
    data->Canvas->Billboard = true;

    const auto [suggestedWidth, suggestedHeight] = ComputeSuggestedWorldCanvasSize(data);
    if (data->Canvas->Width <= 0)
        data->Canvas->Width = suggestedWidth;
    if (data->Canvas->Height <= 0)
        data->Canvas->Height = suggestedHeight;

    if (data->Text)
        data->Text->WorldSpace = false;

    NormalizeWorldSpaceRootUILayout(data);
}

void ApplyUICreationDefaults(Scene* context, EntityID parentId, EntityID entityId)
{
    if (!context || entityId == INVALID_ENTITY_ID)
        return;

    auto* data = context->GetEntityData(entityId);
    if (!data)
        return;

    CanvasComponent* parentCanvas = FindAncestorCanvas(context, parentId);
    const bool shouldDefaultToWorldSpace = ShouldDefaultChildUIToWorldSpace(context, parentId);
    if (data->Canvas) {
        if (parentCanvas) {
            data->Canvas->Space = parentCanvas->Space;
            data->Canvas->Billboard = parentCanvas->Billboard;
        } else if (shouldDefaultToWorldSpace) {
            PromoteToWorldSpaceUIRoot(data);
        }
    }

    if (!parentCanvas && shouldDefaultToWorldSpace && IsUICreationCandidate(data))
        PromoteToWorldSpaceUIRoot(data);
}
}

bool DrawCreateEntityMenuItems(Scene* context, EntityID* selectedEntityOut, EntityID parentId)
{
    if (!context || !selectedEntityOut) return false;
    bool created = false;

    if (ImGui::MenuItem("Empty")) {
        auto e = context->CreateEntity("Empty Entity");
        *selectedEntityOut = e.GetID();
        created = true;
    }

    if (ImGui::MenuItem("Camera")) {
        auto e = context->CreateEntity("Camera");
        if (auto* d = context->GetEntityData(e.GetID())) {
            d->Camera = std::make_unique<CameraComponent>();
        }
        *selectedEntityOut = e.GetID();
        created = true;
    }

    if (ImGui::MenuItem("Cube")) {
        auto e = context->CreateEntity("Cube");
        if (auto* d = context->GetEntityData(e.GetID())) {
            d->Mesh = std::make_unique<MeshComponent>();
            d->Mesh->mesh = StandardMeshManager::Instance().GetCubeMesh();
            d->Mesh->meshReference = AssetReference::CreatePrimitive("Cube");
            d->Mesh->material = MaterialManager::Instance().CreateSceneDefaultMaterial(&Scene::Get());
            d->Mesh->MeshName = "Cube";
            if (!d->RenderOverrides) d->RenderOverrides = std::make_unique<RenderOverridesComponent>();
        }
        *selectedEntityOut = e.GetID();
        created = true;
    }

    if (ImGui::MenuItem("Sphere")) {
        auto e = context->CreateEntity("Sphere");
        if (auto* d = context->GetEntityData(e.GetID())) {
            d->Mesh = std::make_unique<MeshComponent>();
            d->Mesh->mesh = StandardMeshManager::Instance().GetSphereMesh();
            d->Mesh->meshReference = AssetReference::CreatePrimitive("Sphere");
            d->Mesh->material = MaterialManager::Instance().CreateSceneDefaultMaterial(&Scene::Get());
            d->Mesh->MeshName = "Sphere";
            if (!d->RenderOverrides) d->RenderOverrides = std::make_unique<RenderOverridesComponent>();
        }
        *selectedEntityOut = e.GetID();
        created = true;
    }

    if (ImGui::MenuItem("Plane")) {
        auto e = context->CreateEntity("Plane");
        if (auto* d = context->GetEntityData(e.GetID())) {
            d->Mesh = std::make_unique<MeshComponent>();
            d->Mesh->mesh = StandardMeshManager::Instance().GetPlaneMesh();
            d->Mesh->meshReference = AssetReference::CreatePrimitive("Plane");
            d->Mesh->material = MaterialManager::Instance().CreateSceneDefaultMaterial(&Scene::Get());
            d->Mesh->MeshName = "Plane";
            if (!d->RenderOverrides) d->RenderOverrides = std::make_unique<RenderOverridesComponent>();
        }
        *selectedEntityOut = e.GetID();
        created = true;
    }

    if (ImGui::MenuItem("Capsule")) {
        auto e = context->CreateEntity("Capsule");
        if (auto* d = context->GetEntityData(e.GetID())) {
            d->Mesh = std::make_unique<MeshComponent>();
            d->Mesh->mesh = StandardMeshManager::Instance().GetCapsuleMesh();
            d->Mesh->meshReference = AssetReference::CreatePrimitive("Capsule");
            d->Mesh->material = MaterialManager::Instance().CreateSceneDefaultMaterial(&Scene::Get());
            d->Mesh->MeshName = "Capsule";
            if (!d->RenderOverrides) d->RenderOverrides = std::make_unique<RenderOverridesComponent>();
        }
        *selectedEntityOut = e.GetID();
        created = true;
    }

    if (ImGui::BeginMenu("Light")) {
        if (ImGui::MenuItem("Directional")) {
            auto e = context->CreateEntity("Directional Light");
            if (auto* d = context->GetEntityData(e.GetID())) {
                d->Light = std::make_unique<LightComponent>(LightType::Directional, glm::vec3(1.0f), 1.0f);
            }
            *selectedEntityOut = e.GetID();
            created = true;
        }
        if (ImGui::MenuItem("Point")) {
            auto e = context->CreateEntity("Point Light");
            if (auto* d = context->GetEntityData(e.GetID())) {
                d->Light = std::make_unique<LightComponent>(LightType::Point, glm::vec3(1.0f), 1.0f);
            }
            *selectedEntityOut = e.GetID();
            created = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::MenuItem("Terrain")) {
        auto e = context->CreateEntity("Terrain");
        if (auto* d = context->GetEntityData(e.GetID())) {
            d->Terrain = std::make_unique<TerrainComponent>();
            glm::vec2 halfSize = 0.5f * d->Terrain->WorldSize;
            d->Transform.Position = glm::vec3(-halfSize.x, 0.0f, -halfSize.y);
        }
        *selectedEntityOut = e.GetID();
        created = true;
    }

    if (ImGui::MenuItem("Particle Emitter")) {
        auto e = context->CreateEntity("Particle Emitter");
        if (auto* d = context->GetEntityData(e.GetID())) {
            d->Emitter = std::make_unique<ParticleEmitterComponent>();
        }
        *selectedEntityOut = e.GetID();
        created = true;
    }

    if (ImGui::MenuItem("Spline")) {
        auto e = context->CreateEntity("Spline");
        if (auto* d = context->GetEntityData(e.GetID())) {
            d->Spline = std::make_unique<SplineComponent>();
        }
        *selectedEntityOut = e.GetID();
        created = true;
    }

    if (ImGui::MenuItem("Instancer")) {
        auto e = context->CreateEntity("Instancer");
        if (auto* d = context->GetEntityData(e.GetID())) {
            d->Instancer = std::make_unique<cm::instancer::InstancerComponent>();
        }
        *selectedEntityOut = e.GetID();
        created = true;
    }

    if (ImGui::BeginMenu("UI")) {
        if (ImGui::MenuItem("Canvas")) {
            auto e = context->CreateEntity("Canvas");
            if (auto* d = context->GetEntityData(e.GetID())) {
                d->Canvas = std::make_unique<CanvasComponent>();
            }
            ApplyUICreationDefaults(context, parentId, e.GetID());
            *selectedEntityOut = e.GetID();
            created = true;
        }
        if (ImGui::MenuItem("Panel")) {
            auto e = context->CreateEntity("Panel");
            if (auto* d = context->GetEntityData(e.GetID())) {
                d->Panel = std::make_unique<PanelComponent>();
            }
            ApplyUICreationDefaults(context, parentId, e.GetID());
            *selectedEntityOut = e.GetID();
            created = true;
        }
        if (ImGui::MenuItem("Button")) {
            auto e = context->CreateEntity("Button");
            if (auto* d = context->GetEntityData(e.GetID())) {
                d->Panel = std::make_unique<PanelComponent>();
                d->Button = std::make_unique<ButtonComponent>();
            }
            ApplyUICreationDefaults(context, parentId, e.GetID());
            *selectedEntityOut = e.GetID();
            created = true;
        }
        if (ImGui::MenuItem("Slider")) {
            auto e = context->CreateEntity("Slider");
            if (auto* d = context->GetEntityData(e.GetID())) {
                d->Panel = std::make_unique<PanelComponent>();
                d->Panel->Size = {200.0f, 20.0f};
                d->Panel->TintColor = {0.3f, 0.3f, 0.3f, 1.0f};
                d->Slider = std::make_unique<SliderComponent>();
            }
            ApplyUICreationDefaults(context, parentId, e.GetID());
            *selectedEntityOut = e.GetID();
            created = true;
        }
        if (ImGui::MenuItem("Progress Bar")) {
            auto e = context->CreateEntity("Progress Bar");
            if (auto* d = context->GetEntityData(e.GetID())) {
                d->Panel = std::make_unique<PanelComponent>();
                d->Panel->Size = {200.0f, 25.0f};
                d->Panel->TintColor = {0.2f, 0.2f, 0.2f, 1.0f};
                d->ProgressBar = std::make_unique<ProgressBarComponent>();
            }
            ApplyUICreationDefaults(context, parentId, e.GetID());
            *selectedEntityOut = e.GetID();
            created = true;
        }
        if (ImGui::MenuItem("Toggle")) {
            auto e = context->CreateEntity("Toggle");
            if (auto* d = context->GetEntityData(e.GetID())) {
                d->Panel = std::make_unique<PanelComponent>();
                d->Panel->Size = {24.0f, 24.0f};
                d->Toggle = std::make_unique<ToggleComponent>();
            }
            ApplyUICreationDefaults(context, parentId, e.GetID());
            *selectedEntityOut = e.GetID();
            created = true;
        }
        if (ImGui::MenuItem("Scroll View")) {
            auto e = context->CreateEntity("Scroll View");
            if (auto* d = context->GetEntityData(e.GetID())) {
                d->Panel = std::make_unique<PanelComponent>();
                d->Panel->Size = {300.0f, 400.0f};
                d->ScrollView = std::make_unique<ScrollViewComponent>();
            }
            ApplyUICreationDefaults(context, parentId, e.GetID());
            *selectedEntityOut = e.GetID();
            created = true;
        }
        if (ImGui::MenuItem("Layout Group")) {
            auto e = context->CreateEntity("Layout Group");
            if (auto* d = context->GetEntityData(e.GetID())) {
                d->Panel = std::make_unique<PanelComponent>();
                d->Panel->Size = {300.0f, 400.0f};
                d->LayoutGroup = std::make_unique<LayoutGroupComponent>();
            }
            ApplyUICreationDefaults(context, parentId, e.GetID());
            *selectedEntityOut = e.GetID();
            created = true;
        }
        if (ImGui::MenuItem("Input Field")) {
            auto e = context->CreateEntity("Input Field");
            if (auto* d = context->GetEntityData(e.GetID())) {
                d->Panel = std::make_unique<PanelComponent>();
                d->Panel->Size = {200.0f, 30.0f};
                d->Panel->TintColor = {0.15f, 0.15f, 0.15f, 1.0f};
                d->InputField = std::make_unique<InputFieldComponent>();
                d->Text = std::make_unique<TextRendererComponent>();
                d->Text->WorldSpace = false;
            }
            ApplyUICreationDefaults(context, parentId, e.GetID());
            *selectedEntityOut = e.GetID();
            created = true;
        }
        if (ImGui::MenuItem("Dropdown")) {
            auto e = context->CreateEntity("Dropdown");
            if (auto* d = context->GetEntityData(e.GetID())) {
                d->Panel = std::make_unique<PanelComponent>();
                d->Panel->Size = {200.0f, 30.0f};
                d->Panel->TintColor = {0.25f, 0.25f, 0.25f, 1.0f};
                d->Dropdown = std::make_unique<DropdownComponent>();
                d->Dropdown->Options = {"Option 1", "Option 2", "Option 3"};
                d->Text = std::make_unique<TextRendererComponent>();
                d->Text->WorldSpace = false;
            }
            ApplyUICreationDefaults(context, parentId, e.GetID());
            *selectedEntityOut = e.GetID();
            created = true;
        }
        if (ImGui::MenuItem("Text")) {
            auto e = context->CreateEntity("Text");
            if (auto* d = context->GetEntityData(e.GetID())) {
                d->Text = std::make_unique<TextRendererComponent>();
                d->Text->WorldSpace = false;
            }
            ApplyUICreationDefaults(context, parentId, e.GetID());
            *selectedEntityOut = e.GetID();
            created = true;
        }
        ImGui::EndMenu();
    }

    return created;
}


