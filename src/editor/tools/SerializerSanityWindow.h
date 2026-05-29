#pragma once
#include <imgui.h>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <chrono>
#include "core/ecs/Scene.h"
#include "editor/ui/panels/EditorPanel.h"
#include "editor/serialization/DiffTypes.h"
#include "core/prefab/PrefabInstanceComponent.h"

// Forward declarations
namespace cm { namespace editor {
	struct RunResultRow {
		std::string checkName;
		bool passed = false;
		double durationMs = 0.0;
		std::vector<cm::editor::Diff> diffs;
		std::string logPath;
	};
}}

class SerializerSanityWindow : public EditorPanel {
public:
	SerializerSanityWindow() = default;
	~SerializerSanityWindow() = default;

	void OnImGuiRender();
	void Open() { m_Open = true; }
	void SetOpen(bool open) { m_Open = open; }
	bool IsOpen() const { return m_Open; }
	bool IsRunOnSaveEnabled() const { return m_RunOnSave; }
	bool AllChecksPassed() const;

	struct AutoRunOptions {
		bool enabled = false;
		bool exitOnComplete = true;
		std::string scenePath;
	};
	static void ConfigureAutoRun(const AutoRunOptions& opts);
	static AutoRunOptions ConsumeAutoRunOptions();

	// Orchestration entrypoints
	void RunAllChecks(Scene &scene);
	void RunScenarioTests(Scene &scene);

private:
	// UI helpers
	void DrawRunTab(Scene &scene);
	void DrawDiffTab();
	void DrawOverridesTab();
	void DrawRefsTab(Scene &scene);
	void DrawFuzzTab(Scene &scene);
	void DrawHistoryTab(Scene &scene);
	void DrawReconstructionTree();

	// Background task management
	void StartWorker();
	void StopWorker();

	// Scenario helpers
	bool RunScenario1(std::string &outLogPath);
	bool RunScenario2(std::string &outLogPath);

private:
	bool m_Open = false;
	// Options
	bool m_RunOnSave = false;
	bool m_FailBuildOnError = false;
	float m_FloatEpsilon = 1e-5f;
	int m_MaxDiffRows = 200;

	// Results
	std::vector<cm::editor::RunResultRow> m_Results;
	std::vector<cm::editor::Diff> m_LastDiffs;
	std::string m_LastLogFile;
	std::vector<prefab::PropertyOverride> m_LastOverrides;
	bool m_HasOverrides = false;

	// Reconstruction view (read-only)
	cm::editor::SerializedBlob m_LastSerialized;
	cm::editor::SerializedBlob m_ReconstructedTree;
	std::string m_LastPrefabAuthoringJSON;
	std::string m_LastPrefabDeserializedJSON;

	// Fuzz state
	uint64_t m_LastFuzzSeed = 0;
	std::vector<uint64_t> m_LastSeeds;
	int m_FuzzPasses = 10;
	int m_FuzzPassesLast = 0;
	int m_FuzzPassesFailed = 0;
	std::vector<cm::editor::RunResultRow> m_FuzzLastResults;

	// History
	uint64_t m_LastHash = 0;
	uint64_t m_GoldenHash = 0;
	bool m_HasGolden = false;
};


