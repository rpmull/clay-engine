#pragma once
#include <imgui.h>
#include "core/ecs/Scene.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>      // Base Windows API
#undef min
#undef max
#include <shobjidl.h> 
#include "ProjectPanel.h"
#include "EditorPanel.h"
#include <string>
#include <vector>

class UILayer; // Forward declaration 

class MenuBarPanel : public EditorPanel{
public:
    MenuBarPanel(Scene* scene, EntityID* selectedEntity, ProjectPanel* projectPanel, UILayer* uiLayer = nullptr)
        : m_SelectedEntity(selectedEntity), m_ProjectPanel(projectPanel), m_UILayer(uiLayer) {
       SetContext(scene);
       }



   void OnImGuiRender();
   void RenderExportPopup();
   void OpenExportDialog();

private:
   bool RenderRegisteredAction(const char* id);
   bool RenderRegisteredActionMenu(const char* category);
   EntityID* m_SelectedEntity;

   ProjectPanel* m_ProjectPanel;
   UILayer* m_UILayer;

   // Export dialog state
   bool m_ExportPopupOpen = false;
   std::string m_ExportOutDir;
   std::string m_ExportEntryScene;
   bool m_ExportValidateFirst = true;
   bool m_ExportCompressPak = true;
   
   // Export progress state
   bool m_ExportInProgress = false;
   float m_ExportProgress = 0.0f;
   std::string m_ExportStatus;
   std::vector<std::string> m_ExportLog;
   bool m_ExportSucceeded = false;
   bool m_ShowExportResult = false;
   };
