#include "ScriptRegistryPanel.h"
#include <imgui.h>

void ScriptRegistryPanel::OnImGuiRender(bool* open)
   {
   if (!m_ScriptNames) return;

   // Early exit if window is collapsed - skip content rendering
   if (!ImGui::Begin("Script Registry", open))
      {
      ImGui::End();
      return;
      }
   
   ImGui::Text("Total: %d script(s)", static_cast<int>(m_ScriptNames->size()));
   ImGui::Separator();

   for (const auto& name : *m_ScriptNames)
      {
      ImGui::BulletText("%s", name.c_str());
      }
   
   ImGui::End();
   }
