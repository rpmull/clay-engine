#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <cstdint>
#include <deque>

// Lightweight per-frame CPU profiler and memory sampler for the editor
class Profiler {
public:
	struct Entry {
		std::string name;
		double totalMs = 0.0;   // sum for this frame
		uint32_t callCount = 0; // calls this frame
	};

	struct MemoryStats {
		uint64_t workingSetBytes = 0;   // Resident Set Size (private + shareable)
		uint64_t privateBytes    = 0;   // Private bytes/commit
	};

	struct PercentileEntry {
		std::string name;
		double lastMs = 0.0;
		double p50Ms = 0.0;
		double p95Ms = 0.0;
		double p99Ms = 0.0;
		uint32_t samples = 0;
	};

	struct BudgetAlert {
		std::string name;
		double budgetP95Ms = 0.0;
		double observedP95Ms = 0.0;
		uint64_t frameIndex = 0;
	};

	static Profiler& Get();

	void SetEnabled(bool enabled);
	bool IsEnabled() const;
	void SetTelemetryEnabled(bool enabled);
	bool IsTelemetryEnabled() const;

	// Called once per frame at the very beginning of the loop
	void BeginFrame();
	// Called once per frame near the end of the loop
	void EndFrame();

	// Record a completed timing sample (in milliseconds)
	void Record(const std::string& name, double durationMs);

	// Convenience for script timings
	void RecordScriptSample(const std::string& scriptClassName, double durationMs);

	// Current frame entries (unsorted)
	const std::unordered_map<std::string, Entry>& GetEntries() const;
	// Last completed frame entries (unsorted)
	const std::unordered_map<std::string, Entry>& GetLastFrameEntries() const;

	// Sorted copy of current frame entries by totalMs desc
	std::vector<Entry> GetSortedEntriesByTimeDesc() const;
	std::vector<Entry> GetSortedLastFrameEntriesByTimeDesc() const;
	std::vector<PercentileEntry> GetPercentileEntriesByP95Desc() const;

	// Process memory at the moment of the call
	MemoryStats GetProcessMemory() const;

	// Per-frame counters (for scanned entities, queued jobs, etc.)
	void AddCounter(const std::string& name, uint64_t delta = 1);
	void SetCounter(const std::string& name, uint64_t value);
	const std::unordered_map<std::string, uint64_t>& GetLastFrameCounters() const;

	// Percentile budgets: warn when observed p95 exceeds configured threshold.
	void SetBudgetP95(const std::string& name, double ms);
	void ClearBudget(const std::string& name);
	void ClearBudgets();
	std::vector<BudgetAlert> ConsumeBudgetAlerts();

	// Initializes the default CPU-section budgets used by runtime/editor-play.
	void ConfigureDefaultBudgets();

private:
	static double PercentileFromSorted(const std::vector<double>& values, double q);
	void PushFrameSample(const std::string& name, double totalMs);
	void EvaluateBudgets();

	Profiler() = default;
	std::unordered_map<std::string, Entry> m_CurrentEntries;
	std::unordered_map<std::string, Entry> m_LastEntries;
	std::unordered_map<std::string, double> m_CurrentFrameTotals;
	std::unordered_map<std::string, double> m_LastFrameTotals;
	std::unordered_map<std::string, std::deque<double>> m_HistoryBySection;
	std::unordered_map<std::string, uint64_t> m_CurrentCounters;
	std::unordered_map<std::string, uint64_t> m_LastCounters;
	std::unordered_map<std::string, double> m_BudgetP95Ms;
	std::vector<BudgetAlert> m_PendingAlerts;
	bool m_Enabled = false;  // Only true when profiler window is open (editor mode)
	bool m_TelemetryEnabled = true;
	bool m_DefaultBudgetsConfigured = false;
	uint64_t m_FrameIndex = 0;
	uint64_t m_LastBudgetWarnFrame = 0;
	size_t m_MaxHistoryFrames = 240;
};

// RAII helper to time a scope and submit to Profiler on destruction
class ScopedTimer {
public:
	explicit ScopedTimer(const char* label)
		: m_Label(label), m_Start(std::chrono::high_resolution_clock::now()) {}
	~ScopedTimer();

private:
	std::string m_Label;
	std::chrono::high_resolution_clock::time_point m_Start;
};


