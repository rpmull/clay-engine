#pragma once

#include "editor/ui/panels/EditorPanel.h"

#include <imgui.h>

#include <string>
#include <utility>
#include <vector>

class UILayer;

class PrefabLoadTestPanel : public EditorPanel {
public:
    struct PrefabOption {
        std::string DisplayName;
        std::string Path;
    };

    enum class ScenarioMode {
        GeneralPrefab = 0,
        OnscreenCrowd = 1,
        OnscreenCrowdFullLod = 2,
    };

    struct BenchmarkConfig {
        int MaxInstances = 200;
        int InstanceStep = 25;
        int WarmupFrames = 60;
        int SampleFrames = 120;
        int MinStabilizationFrames = 30;
        int MaxStabilizationFrames = 360;
        int StabilizationWindowFrames = 30;
        int RenderWidth = 640;
        int RenderHeight = 360;
        float UnplayableFps = 30.0f;
        float StabilizationTolerancePct = 5.0f;
        bool StopAtUnplayable = true;
        bool RequireSteadyState = true;
        bool AutoExport = true;
        ScenarioMode Scenario = ScenarioMode::GeneralPrefab;
    };

    struct NumericStats {
        double Average = 0.0;
        double Min = 0.0;
        double Max = 0.0;
        double P50 = 0.0;
        double P95 = 0.0;
        double P99 = 0.0;
        size_t Samples = 0;
    };

    struct SectionStats {
        NumericStats TotalMs;
        NumericStats CallCount;
    };

    struct CounterStats {
        NumericStats Values;
    };

    struct PrefabComposition {
        size_t EntityCount = 0;
        size_t MeshCount = 0;
        size_t SkinnedMeshCount = 0;
        size_t CameraCount = 0;
        size_t LightCount = 0;
        size_t ParticleEmitterCount = 0;
        size_t NavigationMeshCount = 0;
        size_t NavAgentCount = 0;
        size_t NavLinkCount = 0;
        size_t CharacterControllerCount = 0;
        size_t AudioSourceCount = 0;
        size_t ScriptCount = 0;
        size_t RigidBodyCount = 0;
        size_t StaticBodyCount = 0;
        bool HasVisualContent = false;
        bool NeedsNavigationFloor = false;
    };

    struct StageResult {
        int InstanceCount = 0;
        int WarmupFramesUsed = 0;
        int StabilizationFramesUsed = 0;
        bool Unplayable = false;
        bool ReachedSteadyState = false;
        NumericStats FrameMs;
        NumericStats SceneUpdateMs;
        NumericStats AudioMs;
        NumericStats RenderMs;
        NumericStats WorkingSetMb;
        NumericStats PrivateMb;
        NumericStats RenderedMeshObjects;
        NumericStats CulledMeshObjects;
        NumericStats RenderedSkinnedMeshObjects;
        NumericStats CulledSkinnedMeshObjects;
        std::string StabilizationReason;
        std::vector<std::pair<std::string, SectionStats>> Sections;
        std::vector<std::pair<std::string, CounterStats>> Counters;
    };

    struct BenchmarkRequest {
        std::string PrefabPath;
        BenchmarkConfig Config;
    };

    struct RunResult {
        bool Success = false;
        std::string PrefabPath;
        std::string PrefabName;
        std::string StartedAtLocal;
        BenchmarkConfig Config;
        PrefabComposition Composition;
        int FirstUnplayableInstanceCount = 0;
        double UnplayableFrameBudgetMs = 0.0;
        std::string UnplayableReason;
        std::string ErrorMessage;
        std::string JsonExportPath;
        std::string LogExportPath;
        std::vector<StageResult> Stages;
    };

    explicit PrefabLoadTestPanel(UILayer* uiLayer = nullptr);

    void SetUILayer(UILayer* uiLayer) { m_UILayer = uiLayer; }
    void Open() { m_Open = true; }
    bool IsOpen() const { return m_Open; }

    void OnImGuiRender();
    void ProcessPendingRun();

private:
    void RefreshPrefabOptions();
    void QueueRunFromUI();
    void ExportLatestResult();
    bool ExportResult(RunResult& result) const;
    RunResult RunBenchmark(const BenchmarkRequest& request) const;
    void RenderResults(const RunResult& result);

    UILayer* m_UILayer = nullptr;
    bool m_Open = false;
    bool m_Running = false;
    bool m_HasPendingRequest = false;
    bool m_HasLastResult = false;
    std::string m_StatusMessage;
    BenchmarkConfig m_Config;
    BenchmarkRequest m_PendingRequest{};
    RunResult m_LastResult{};
    std::vector<PrefabOption> m_Prefabs;
    int m_SelectedPrefabIndex = -1;
};
