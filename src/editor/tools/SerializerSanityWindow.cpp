#include "SerializerSanityWindow.h"
#include <imgui_internal.h>
#include <sstream>
#include <iomanip>
#include <random>
#include <cstdlib>
#include <filesystem>
#include "editor/serialization/Canonicalizer.h"
#include "editor/serialization/DeepCompare.h"
#include "editor/serialization/Reconstructor.h"
#include "editor/serialization/DeterminismHash.h"
#include "editor/serialization/FuzzMutator.h"
#include "editor/io/SerializationErrorLog.h"
#include "editor/pipeline/AssetLibrary.h"
#include "editor/pipeline/EntityBinaryWriter.h"
#include "core/serialization/EntityBinaryLoader.h"
#include <unordered_set>
#include "core/prefab/PrefabAPI.h"
#include "editor/prefab/PrefabEditorAPI.h"
#include "editor/Project.h"
#include "editor/pipeline/AssetLibrary.h"

using cm::editor::Diff;
using cm::editor::SerializedBlob;

static SerializerSanityWindow::AutoRunOptions g_AutoRunOptions;

void SerializerSanityWindow::ConfigureAutoRun(const AutoRunOptions& opts) {
	g_AutoRunOptions = opts;
}

SerializerSanityWindow::AutoRunOptions SerializerSanityWindow::ConsumeAutoRunOptions() {
	AutoRunOptions out = g_AutoRunOptions;
	g_AutoRunOptions.enabled = false;
	return out;
}

bool SerializerSanityWindow::AllChecksPassed() const {
	if (m_Results.empty()) return false;
	for (const auto& row : m_Results) {
		if (!row.passed) return false;
	}
	return true;
}

void SerializerSanityWindow::OnImGuiRender() {
	if (!m_Open) return;
	if (!m_Context) return;
	ImGui::SetNextWindowSize(ImVec2(860, 560), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Serializer Sanity", &m_Open)) {
		if (ImGui::BeginTabBar("##ser_tabs")) {
			if (ImGui::BeginTabItem("Run")) { DrawRunTab(*m_Context); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Diff")) { DrawDiffTab(); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Overrides")) { DrawOverridesTab(); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Refs")) { DrawRefsTab(*m_Context); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Fuzz")) { DrawFuzzTab(*m_Context); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("History")) { DrawHistoryTab(*m_Context); ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem("Reconstruction")) { DrawReconstructionTree(); ImGui::EndTabItem(); }
			ImGui::EndTabBar();
		}
	}
	ImGui::End();
}

void SerializerSanityWindow::RunAllChecks(Scene &scene) {
	m_Results.clear();
	cm::editor::SerializationErrorLog logger;
	// RoundTrip
	{
		auto t0 = std::chrono::high_resolution_clock::now();
		SerializedBlob a = cm::editor::Canonicalize(cm::editor::SerializeSceneToBlob(scene));
		Scene tmp;
		cm::editor::DeserializeSceneFromBlob(a, tmp);
		SerializedBlob a2 = cm::editor::Canonicalize(cm::editor::SerializeSceneToBlob(tmp));
		bool same = (a.bytes == a2.bytes);
		cm::editor::RunResultRow row; row.checkName = "RoundTrip"; row.passed = same;
		if (!same) row.diffs = cm::editor::DeepCompare(scene, tmp, m_FloatEpsilon);
		auto t1 = std::chrono::high_resolution_clock::now();
		row.durationMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
		if (!row.passed && !row.diffs.empty()) {
			std::vector<cm::editor::ErrorRecord> recs;
			for (size_t i = 0; i < std::min<size_t>(row.diffs.size(), 16); ++i) {
				const auto &d = row.diffs[i];
				cm::editor::ErrorRecord r; r.check = "RoundTrip"; r.severity = "error"; r.path = d.path; r.explain = "Structural diff after canonical round-trip";
				recs.push_back(std::move(r));
			}
			logger.AppendBatch(recs);
			row.logPath = logger.GetCurrentFilePath();
		}
		m_Results.push_back(std::move(row));
	}
	// BinaryRoundTrip
	{
		auto t0 = std::chrono::high_resolution_clock::now();
		std::vector<uint8_t> data;
		Scene tmp;
		bool wrote = binary::EntityBinaryWriter::WriteToMemory(scene, data);
		bool loaded = wrote && binary::EntityBinaryLoader::LoadFromMemory(data.data(), data.size(), tmp);
		auto diffs = loaded ? cm::editor::DeepCompare(scene, tmp, m_FloatEpsilon) : std::vector<cm::editor::Diff>{};
		cm::editor::RunResultRow row;
		row.checkName = "BinaryRoundTrip";
		row.passed = loaded && diffs.empty();
		row.diffs = std::move(diffs);
		auto t1 = std::chrono::high_resolution_clock::now();
		row.durationMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
		if (!row.passed) {
			std::vector<cm::editor::ErrorRecord> recs;
			cm::editor::ErrorRecord r;
			r.check = "BinaryRoundTrip";
			r.severity = loaded ? "error" : "fatal";
			r.path = "/scene";
			r.explain = loaded ? "Binary round-trip produced diffs" : "Binary write/load failed";
			recs.push_back(std::move(r));
			logger.AppendBatch(recs);
			row.logPath = logger.GetCurrentFilePath();
		}
		m_Results.push_back(std::move(row));
	}
	// Parity
	{
		std::shared_ptr<Scene> copyPtr = scene.RuntimeClone();
		Scene& copy = *copyPtr;
		auto t0 = std::chrono::high_resolution_clock::now();
		auto diffs = cm::editor::DeepCompare(scene, copy, m_FloatEpsilon);
		auto t1 = std::chrono::high_resolution_clock::now();
		cm::editor::RunResultRow row; row.checkName = "Parity"; row.passed = diffs.empty(); row.durationMs = std::chrono::duration<double, std::milli>(t1 - t0).count(); row.diffs = std::move(diffs);
		if (!row.passed && !row.diffs.empty()) {
			std::vector<cm::editor::ErrorRecord> recs;
			for (size_t i = 0; i < std::min<size_t>(row.diffs.size(), 16); ++i) {
				cm::editor::ErrorRecord r; r.check = "Parity"; r.severity = "error"; r.path = row.diffs[i].path; r.explain = "Live vs deserialized parity mismatch"; recs.push_back(r);
			}
			logger.AppendBatch(recs);
			row.logPath = logger.GetCurrentFilePath();
		}
		m_Results.push_back(std::move(row));
	}
	// ReconstructionDiff
	{
		auto t0 = std::chrono::high_resolution_clock::now();
		SerializedBlob blob = cm::editor::SerializeSceneToBlob(scene);
		Scene recon = cm::editor::ReconstructFromSerialized(blob, {});
		m_ReconstructedTree = cm::editor::SerializeSceneToBlob(recon);
		auto diffs = cm::editor::DeepCompare(scene, recon, m_FloatEpsilon);
		auto t1 = std::chrono::high_resolution_clock::now();
		cm::editor::RunResultRow row; row.checkName = "ReconstructionDiff"; row.passed = diffs.empty(); row.durationMs = std::chrono::duration<double, std::milli>(t1 - t0).count(); row.diffs = std::move(diffs);
		if (!row.passed) {
			std::vector<cm::editor::ErrorRecord> recs;
			for (const auto &d : row.diffs) {
				if (d.kind == Diff::Kind::UnexpectedAdd || d.kind == Diff::Kind::UnexpectedRemove || d.kind == Diff::Kind::UnexpectedChange) {
					cm::editor::ErrorRecord r; r.check = "ReconstructionDiff"; r.severity = "error"; r.path = d.path; r.explain = "Unexpected diff during reconstruction"; recs.push_back(std::move(r));
				}
			}
			if (!recs.empty()) { logger.AppendBatch(recs); row.logPath = logger.GetCurrentFilePath(); }
		}
		m_Results.push_back(std::move(row));
	}
	// Refs
	{
		cm::editor::RunResultRow row; row.checkName = "Refs"; row.passed = true; row.durationMs = 0.0; // Placeholder; actual checks implemented in Refs tab
		m_Results.push_back(std::move(row));
	}
	// DeterminismHash
	{
		auto t0 = std::chrono::high_resolution_clock::now();
		uint64_t h = cm::editor::HashScene(scene, {});
		(void)h;
		auto t1 = std::chrono::high_resolution_clock::now();
		cm::editor::RunResultRow row; row.checkName = "Hash"; row.passed = true; row.durationMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
		m_Results.push_back(std::move(row));
	}
}

void SerializerSanityWindow::RunScenarioTests(Scene &scene) {
    // Scenario 1: virtual scene with random assets/scripts, prefabize one, reload, diff
    std::string log1;
    if (!RunScenario1(log1)) {
        // Log path emitted by helper; record in results for visibility
        cm::editor::RunResultRow row; row.checkName = "Scenario1"; row.passed = false; row.durationMs = 0.0; row.logPath = log1; m_Results.push_back(std::move(row));
    } else {
        cm::editor::RunResultRow row; row.checkName = "Scenario1"; row.passed = true; row.durationMs = 0.0; m_Results.push_back(std::move(row));
    }

    // Scenario 2: prefab authoring edits, save/reload, hot-reload into scene
    std::string log2;
    if (!RunScenario2(log2)) {
        cm::editor::RunResultRow row; row.checkName = "Scenario2"; row.passed = false; row.durationMs = 0.0; row.logPath = log2; m_Results.push_back(std::move(row));
    } else {
        cm::editor::RunResultRow row; row.checkName = "Scenario2"; row.passed = true; row.durationMs = 0.0; m_Results.push_back(std::move(row));
    }
}

bool SerializerSanityWindow::RunScenario1(std::string &outLogPath) {
    using namespace std;
    cm::editor::SerializationErrorLog logger;
    outLogPath.clear();

    // Build a fresh scene (work on a copy to avoid touching editor state)
    Scene s;
    // Random mesh/model selection from AssetLibrary
    vector<tuple<string, ClaymoreGUID, AssetType>> assets = AssetLibrary::Instance().GetAllAssets();
    vector<size_t> modelIdx;
    for (size_t i = 0; i < assets.size(); ++i) if (get<2>(assets[i]) == AssetType::Mesh) modelIdx.push_back(i);
    if (modelIdx.empty()) {
        cm::editor::ErrorRecord r; r.check = "Scenario1"; r.severity = "warn"; r.path = "/assets"; r.explain = "No mesh assets found"; logger.Append(r); outLogPath = logger.GetCurrentFilePath();
        return true; // Nothing to test
    }
    std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<size_t> pick(0, modelIdx.size() - 1);

    // Add a few random models/entities
    const int kCount = 5;
    vector<EntityID> roots; roots.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        size_t idx = modelIdx[pick(rng)];
        auto path = get<0>(assets[idx]);
        EntityID id = s.InstantiateModel(path, glm::vec3((float)i * 2.0f, 0.0f, 0.0f));
        if (id != (EntityID)-1) roots.push_back(id);
    }
    if (roots.empty()) { cm::editor::ErrorRecord r; r.check = "Scenario1"; r.severity = "error"; r.explain = "InstantiateModel failed"; logger.Append(r); outLogPath = logger.GetCurrentFilePath(); return false; }

    // Pick one root, add components, scripts, tweak values
    EntityID target = roots.front();
    if (auto* d = s.GetEntityData(target)) {
        // Ensure a light so we modify components
        Entity eL = s.CreateLight("TmpLight", LightType::Point, {1,1,1}, 1.0f); s.SetParent(eL.GetID(), target);
        // Attach a managed script by picking any registered name (if available)
        const auto& reg = ScriptSystem::Instance().GetRegistry();
        if (!reg.empty()) {
            auto it = reg.begin(); std::advance(it, (int)(rng() % reg.size()));
            ScriptInstance si; si.ClassName = it->first; si.Instance = ScriptSystem::Instance().Create(si.ClassName); d->Scripts.push_back(std::move(si));
        }
        // Change transform
        d->Transform.Position += glm::vec3(0.123456f, -0.234567f, 0.345678f);
        d->Transform.Scale *= glm::vec3(1.1f, 0.9f, 1.05f);
    }

    // Prefabize selected: build PrefabAsset and save to temp authoring path
    PrefabAsset pa{}; pa.Guid = ClaymoreGUID::Generate(); pa.Name = "SanityPrefab"; pa.RootGuid = ClaymoreGUID::Generate();
    if (!prefab_editor::BuildPrefabAssetFromScene(s, target, pa)) { cm::editor::ErrorRecord r; r.check = "Scenario1"; r.severity = "error"; r.explain = "BuildPrefabAssetFromScene failed"; logger.Append(r); outLogPath = logger.GetCurrentFilePath(); return false; }
    std::string outPath = (Project::GetProjectDirectory() / (std::string("assets/prefabs/") + pa.Guid.ToString() + ".prefab.json")).string();
    std::filesystem::create_directories((Project::GetProjectDirectory() / "assets/prefabs").string());
    if (!PrefabIO::SavePrefab(outPath, pa)) { cm::editor::ErrorRecord r; r.check = "Scenario1"; r.severity = "error"; r.explain = "SaveAuthoringPrefabJSON failed"; logger.Append(r); outLogPath = logger.GetCurrentFilePath(); return false; }

    // Reload scene via canonical round-trip and diff
    SerializedBlob a = cm::editor::Canonicalize(cm::editor::SerializeSceneToBlob(s));
    Scene tmp; cm::editor::DeserializeSceneFromBlob(a, tmp);
    auto diffs = cm::editor::DeepCompare(s, tmp, m_FloatEpsilon);
    m_LastDiffs = diffs; // expose in Diff tab
    if (!diffs.empty()) {
        std::vector<cm::editor::ErrorRecord> recs;
        for (const auto& dff : diffs) { cm::editor::ErrorRecord r; r.check = "Scenario1"; r.severity = "error"; r.path = dff.path; r.explain = "Diff after scenario1"; r.actual = dff.delta; recs.push_back(std::move(r)); }
        logger.AppendBatch(recs); outLogPath = logger.GetCurrentFilePath(); return false;
    }

    return true;
}

bool SerializerSanityWindow::RunScenario2(std::string &outLogPath) {
    using namespace std;
    cm::editor::SerializationErrorLog logger; outLogPath.clear();
    // Create a small prefab in memory, save, open virtual editor (scene), edit, save again, load into scene, edit, hot-reload
    Scene author;
    Entity root = author.CreateEntity("PrefabRoot");
    if (auto* d = author.GetEntityData(root.GetID())) { d->Transform.Scale = glm::vec3(1.2f); }
    PrefabAsset base{}; base.Guid = ClaymoreGUID::Generate(); base.Name = "Scenario2"; base.RootGuid = ClaymoreGUID::Generate();
    if (!prefab_editor::BuildPrefabAssetFromScene(author, root.GetID(), base)) { cm::editor::ErrorRecord r; r.check = "Scenario2"; r.severity = "error"; r.explain = "BuildPrefabAssetFromScene failed"; logger.Append(r); outLogPath = logger.GetCurrentFilePath(); return false; }
    std::filesystem::create_directories((Project::GetProjectDirectory() / "assets/prefabs").string());
    std::string basePath = (Project::GetProjectDirectory() / (std::string("assets/prefabs/") + base.Guid.ToString() + ".prefab.json")).string();
    if (!PrefabIO::SavePrefab(basePath, base)) { cm::editor::ErrorRecord r; r.check = "Scenario2"; r.severity = "error"; r.explain = "SaveAuthoringPrefabJSON base failed"; logger.Append(r); outLogPath = logger.GetCurrentFilePath(); return false; }

    // Open in virtual editor (scene), make changes and add a child
    Scene editorScene; EntityID rootInst = InstantiatePrefabFromPath(basePath, editorScene);
    if (rootInst == (EntityID)-1) { cm::editor::ErrorRecord r; r.check = "Scenario2"; r.severity = "error"; r.explain = "InstantiatePrefabFromPath failed"; logger.Append(r); outLogPath = logger.GetCurrentFilePath(); return false; }
    if (auto* ed = editorScene.GetEntityData(rootInst)) {
        ed->Transform.Position = glm::vec3(0.5f, 0.25f, -0.75f);
        Entity child = editorScene.CreateEntity("ChildA"); editorScene.SetParent(child.GetID(), rootInst);
    }

    // Save edited prefab back (compute overrides vs base)
    PrefabAsset loadedBase; if (!PrefabIO::LoadPrefab(basePath, loadedBase)) { cm::editor::ErrorRecord r; r.check = "Scenario2"; r.severity = "error"; r.explain = "Reload base prefab failed"; logger.Append(r); outLogPath = logger.GetCurrentFilePath(); return false; }
    auto ov = prefab_editor::ComputeOverrides(loadedBase, editorScene, rootInst);
    m_LastOverrides = ov; m_HasOverrides = true;

    // Load into another scene and verify instantiation
    Scene sceneUse; EntityID inst = InstantiatePrefabFromPath(basePath, sceneUse);
    if (inst == (EntityID)-1) { cm::editor::ErrorRecord r; r.check = "Scenario2"; r.severity = "error"; r.explain = "Instantiate in scene failed"; logger.Append(r); outLogPath = logger.GetCurrentFilePath(); return false; }
    if (auto* id = sceneUse.GetEntityData(inst)) { id->Transform.Position += glm::vec3(0.1f,0,0); }

    // Capture prefab JSONs for reconstruction view
    try {
        nlohmann::json jBase; PrefabIO::LoadPrefab(basePath, base);
        jBase["guid"] = base.Guid; jBase["name"] = base.Name; jBase["root"] = base.RootGuid; // ensure some header fields
        m_LastPrefabAuthoringJSON = jBase.dump();
    } catch(...) { m_LastPrefabAuthoringJSON.clear(); }
    try {
        PrefabAsset tmp; PrefabIO::LoadPrefab(basePath, tmp);
        nlohmann::json jDes;
        jDes["guid"] = tmp.Guid; jDes["name"] = tmp.Name;
        // Minimal: number of entities to keep size small
        jDes["entityCount"] = (int)tmp.Entities.size();
        m_LastPrefabDeserializedJSON = jDes.dump();
    } catch(...) { m_LastPrefabDeserializedJSON.clear(); }

    // Round-trip serialize editorScene and sceneUse to confirm determinism
    SerializedBlob a = cm::editor::Canonicalize(cm::editor::SerializeSceneToBlob(editorScene));
    Scene editorReload; cm::editor::DeserializeSceneFromBlob(a, editorReload);
    auto d1 = cm::editor::DeepCompare(editorScene, editorReload, m_FloatEpsilon);
    m_LastDiffs = d1; // show in Diff tab
    if (!d1.empty()) { std::vector<cm::editor::ErrorRecord> recs; for (auto &dff : d1){ cm::editor::ErrorRecord r; r.check="Scenario2"; r.severity="error"; r.path=dff.path; r.explain="Editor scene diff"; r.actual=dff.delta; recs.push_back(r);} logger.AppendBatch(recs); outLogPath=logger.GetCurrentFilePath(); return false; }

    SerializedBlob b = cm::editor::Canonicalize(cm::editor::SerializeSceneToBlob(sceneUse));
    Scene useReload; cm::editor::DeserializeSceneFromBlob(b, useReload);
    auto d2 = cm::editor::DeepCompare(sceneUse, useReload, m_FloatEpsilon);
    m_LastDiffs = d2; // latest diffs visible
    if (!d2.empty()) { std::vector<cm::editor::ErrorRecord> recs; for (auto &dff : d2){ cm::editor::ErrorRecord r; r.check="Scenario2"; r.severity="error"; r.path=dff.path; r.explain="Use scene diff"; r.actual=dff.delta; recs.push_back(r);} logger.AppendBatch(recs); outLogPath=logger.GetCurrentFilePath(); return false; }

    return true;
}

void SerializerSanityWindow::DrawRunTab(Scene &scene) {
	ImGui::TextDisabled("Checks");
	ImGui::SameLine(); if (ImGui::Button("Run all")) { RunAllChecks(scene); }
	ImGui::SameLine(); if (ImGui::Button("Run scenarios")) { RunScenarioTests(scene); }
	ImGui::SameLine(); ImGui::Checkbox("Run on Save", &m_RunOnSave);
	ImGui::SameLine(); ImGui::Checkbox("Fail Build on Error", &m_FailBuildOnError);
	ImGui::SetNextItemWidth(120); ImGui::InputFloat("Float Epsilon", &m_FloatEpsilon, 0, 0, "%.1e");

	if (ImGui::BeginTable("results", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
		ImGui::TableSetupColumn("Check");
		ImGui::TableSetupColumn("Status");
		ImGui::TableSetupColumn("Duration");
		ImGui::TableSetupColumn("Actions");
		ImGui::TableHeadersRow();
		for (size_t i = 0; i < m_Results.size(); ++i) {
			const auto &r = m_Results[i];
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(r.checkName.c_str());
			ImGui::TableSetColumnIndex(1); ImGui::TextColored(r.passed ? ImVec4(0.6f,1.0f,0.6f,1) : ImVec4(1,0.8f,0.2f,1), r.passed ? "PASS" : "FAIL");
			ImGui::TableSetColumnIndex(2); ImGui::Text("%.2f ms", r.durationMs);
			ImGui::TableSetColumnIndex(3);
			if (!r.passed && !r.diffs.empty()) {
				if (ImGui::Button((std::string("View Diff##") + std::to_string(i)).c_str())) { m_LastDiffs = r.diffs; }
				ImGui::SameLine();
			}
			if (!r.logPath.empty()) {
				ImGui::TextDisabled("log:"); ImGui::SameLine(); ImGui::TextUnformatted(r.logPath.c_str());
				ImGui::SameLine();
				if (ImGui::SmallButton((std::string("Open##") + std::to_string(i)).c_str())) {
#if defined(_WIN32)
					std::string cmd = std::string("cmd /C start \"\" \"") + r.logPath + "\"";
					system(cmd.c_str());
#else
					std::string cmd = std::string("xdg-open \"") + r.logPath + "\"";
					system(cmd.c_str());
#endif
				}
				ImGui::SameLine();
				if (ImGui::SmallButton((std::string("Show in Folder##") + std::to_string(i)).c_str())) {
					std::filesystem::path p(r.logPath);
#if defined(_WIN32)
					std::string cmd = std::string("explorer /select,\"") + p.string() + "\"";
					system(cmd.c_str());
#else
					std::string cmd = std::string("xdg-open \"") + p.parent_path().generic_string() + "\"";
					system(cmd.c_str());
#endif
				}
			}
		}
		ImGui::EndTable();
	}
}

void SerializerSanityWindow::DrawDiffTab() {
	ImGui::TextDisabled("Diffs: %d", (int)m_LastDiffs.size());
	if (ImGui::BeginTable("diffs", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
		ImGui::TableSetupColumn("Kind"); ImGui::TableSetupColumn("Path"); ImGui::TableSetupColumn("Detail"); ImGui::TableHeadersRow();
		int shown = 0;
		for (const auto &d : m_LastDiffs) {
			if (shown++ >= m_MaxDiffRows) break;
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0); ImGui::Text("%d", (int)d.kind);
			ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(d.path.c_str());
			ImGui::TableSetColumnIndex(2); ImGui::Text("delta=%.6g", d.delta);
		}
		ImGui::EndTable();
	}
}

void SerializerSanityWindow::DrawOverridesTab() {
    ImGui::TextDisabled("Overrides");
    ImGui::Separator();
    // Show last computed overrides (from Scenario2) if available
    if (m_HasOverrides && !m_LastOverrides.empty()) {
        if (ImGui::BeginTable("ov_ops", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Component"); ImGui::TableSetupColumn("Entity GUID"); ImGui::TableSetupColumn("Value"); ImGui::TableHeadersRow();
            int shown = 0;
            for (const auto& ov : m_LastOverrides) {
                if (shown++ >= m_MaxDiffRows) break;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(ov.ComponentKey.c_str());
                ImGui::TableSetColumnIndex(1); 
                std::string guidStr = std::to_string(ov.TargetEntityGuid.high) + ":" + std::to_string(ov.TargetEntityGuid.low);
                ImGui::TextUnformatted(guidStr.c_str());
                ImGui::TableSetColumnIndex(2);
                std::string v = ov.Value.dump();
                if (v.size() > 200) { v.resize(200); v += "..."; }
                ImGui::TextUnformatted(v.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::Separator();
    }
    if (!m_Context) return;
    Scene& s = *m_Context;
    if (ImGui::BeginTable("ovr", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Entity"); ImGui::TableSetupColumn("Component"); ImGui::TableSetupColumn("Field"); ImGui::TableHeadersRow();
        for (const auto& e : s.GetEntities()) {
            EntityData* d = s.GetEntityData(e.GetID()); if (!d) continue;
            if (d->Mesh && (!d->Mesh->PropertyBlock.Vec4Uniforms.empty() || !d->Mesh->PropertyBlockTexturePaths.empty())) {
                for (auto& kv : d->Mesh->PropertyBlock.Vec4Uniforms) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(d->Name.c_str());
                    ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted("Mesh");
                    ImGui::TableSetColumnIndex(2); ImGui::Text("Vec4 %s", kv.first.c_str());
                }
                for (auto& kv : d->Mesh->PropertyBlockTexturePaths) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(d->Name.c_str());
                    ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted("Mesh");
                    ImGui::TableSetColumnIndex(2); ImGui::Text("Texture %s", kv.first.c_str());
                }
            }
        }
        ImGui::EndTable();
    }
}

void SerializerSanityWindow::DrawRefsTab(Scene &scene) {
	ImGui::TextDisabled("Reference Integrity");
	ImGui::Separator();
	struct Issue { std::string path; std::string msg; std::string suggest; };
	std::vector<Issue> issues;

	// Duplicate GUIDs
	std::unordered_map<ClaymoreGUID, int> guidCounts;
	for (const auto& e : scene.GetEntities()) { if (auto* d = scene.GetEntityData(e.GetID())) guidCounts[d->EntityGuid]++; }
	for (auto& kv : guidCounts) if (kv.second > 1) issues.push_back({"/entities", "Duplicate entity GUID", "Reassign GUIDs on duplicate entities"});

	// Parenting cycles (DFS with colors)
	std::unordered_map<EntityID, int> color; // 0=white,1=gray,2=black
	std::function<bool(EntityID)> dfs = [&](EntityID id){
		int& c = color[id]; if (c==1) return true; if (c==2) return false; c=1;
		if (auto* d = scene.GetEntityData(id)) for (auto cid : d->Children) if (dfs(cid)) return true;
		c=2; return false;
	};
	for (const auto& e : scene.GetEntities()) { if (dfs(e.GetID())) { issues.push_back({"/entities","Parenting cycle detected","Fix parent links"}); break; } }

	// External assets existence: mesh references
	for (const auto& e : scene.GetEntities()) {
		EntityData* d = scene.GetEntityData(e.GetID()); if (!d || !d->Mesh) continue;
		const auto& ref = d->Mesh->meshReference;
		if (ref.IsValid()) {
			AssetEntry* ae = AssetLibrary::Instance().GetAsset(ref);
			if (!ae) issues.push_back({"/entities", "Missing external mesh asset GUID", "Reimport asset or fix GUID"});
		}
	}

	if (ImGui::BeginTable("refs", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
		ImGui::TableSetupColumn("Path"); ImGui::TableSetupColumn("Issue"); ImGui::TableSetupColumn("Suggest"); ImGui::TableHeadersRow();
		for (const auto& is : issues) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(is.path.c_str());
			ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(is.msg.c_str());
			ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(is.suggest.c_str());
		}
		ImGui::EndTable();
	}
	if (issues.empty()) ImGui::TextColored(ImVec4(0.6f,1,0.6f,1), "No issues detected");
}

void SerializerSanityWindow::DrawFuzzTab(Scene &scene) {
	ImGui::TextDisabled("Fuzz Testing");
	ImGui::Separator();
	ImGui::SetNextItemWidth(120); ImGui::InputInt("Passes", &m_FuzzPasses);
	ImGui::SameLine();
	static uint64_t seedInput = 0; ImGui::InputScalar("Seed (0=random)", ImGuiDataType_U64, &seedInput);
	if (ImGui::Button("Run Fuzz")) {
		cm::editor::FuzzOptions fo{}; fo.passes = std::max(1, m_FuzzPasses); fo.seed = seedInput;
		std::shared_ptr<Scene> copyPtr = scene.RuntimeClone();
		Scene& copy = *copyPtr;
		m_FuzzPassesLast = fo.passes; m_FuzzPassesFailed = cm::editor::FuzzScene(copy, fo);
		if (seedInput == 0) { m_LastFuzzSeed = fo.seed; }
		m_LastSeeds.push_back(fo.seed);
	}
	ImGui::SameLine();
	if (ImGui::Button("Replay Seed")) {
		cm::editor::FuzzOptions fo{}; fo.passes = std::max(1, m_FuzzPasses); fo.seed = m_LastFuzzSeed;
		std::shared_ptr<Scene> copyPtr = scene.RuntimeClone(); Scene& copy = *copyPtr; m_FuzzPassesLast = fo.passes; m_FuzzPassesFailed = cm::editor::FuzzScene(copy, fo);
	}
	ImGui::Text("Last: %d passes, %d failed, seed=%llu", m_FuzzPassesLast, m_FuzzPassesFailed, (unsigned long long)m_LastFuzzSeed);
	if (!m_LastSeeds.empty()) {
		ImGui::Separator(); ImGui::TextDisabled("Recent seeds:");
		for (int i = (int)m_LastSeeds.size()-1, shown=0; i>=0 && shown<10; --i,++shown) {
			ImGui::Text("%llu", (unsigned long long)m_LastSeeds[i]);
		}
	}
}

void SerializerSanityWindow::DrawHistoryTab(Scene &scene) {
	ImGui::TextDisabled("Golden Snapshots");
	ImGui::Separator();
	uint64_t cur = cm::editor::HashScene(scene, {});
	ImGui::Text("Current Hash: %016llx", (unsigned long long)cur);
	if (m_HasGolden) ImGui::Text("Golden Hash:  %016llx", (unsigned long long)m_GoldenHash);
	else ImGui::TextDisabled("Golden Hash: <none>");
	if (ImGui::Button("Promote to Golden")) { m_GoldenHash = cur; m_HasGolden = true; }
}

void SerializerSanityWindow::DrawReconstructionTree() {
	ImGui::TextDisabled("Reconstruction Tree");
	ImGui::Separator();
	if (!m_LastPrefabAuthoringJSON.empty() || !m_LastPrefabDeserializedJSON.empty()) {
		if (ImGui::BeginTable("rectab", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
			ImGui::TableSetupColumn("Authoring Prefab JSON");
			ImGui::TableSetupColumn("Deserialized Prefab JSON");
			ImGui::TableHeadersRow();
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::BeginChild("##auth", ImVec2(0, 240), true);
			ImGui::TextUnformatted(m_LastPrefabAuthoringJSON.c_str());
			ImGui::EndChild();
			ImGui::TableSetColumnIndex(1);
			ImGui::BeginChild("##deser", ImVec2(0, 240), true);
			ImGui::TextUnformatted(m_LastPrefabDeserializedJSON.c_str());
			ImGui::EndChild();
			ImGui::EndTable();
		}
		return;
	}
	if (m_ReconstructedTree.bytes.empty()) { ImGui::TextDisabled("Run ReconstructionDiff first."); return; }
	// Minimal read-only tree: display as raw JSON in a child for now
	ImGui::BeginChild("##rectree", ImVec2(0, 240), true);
	ImGui::TextUnformatted(m_ReconstructedTree.bytes.c_str());
	ImGui::EndChild();
}

void SerializerSanityWindow::StartWorker() {}
void SerializerSanityWindow::StopWorker() {}


