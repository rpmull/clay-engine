#pragma once
#include "EditorPanel.h"
#include "editor/pipeline/AssetLibrary.h"
#include <imgui.h>

class AssetRegistryPanel : public EditorPanel {
public:
    AssetRegistryPanel() = default;
    ~AssetRegistryPanel() = default;

    void OnImGuiRender(bool* open = nullptr);
};


