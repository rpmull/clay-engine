#include "Profiler.h"

#if defined(_WIN32)
#define NOMINMAX
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

#include <algorithm>
#include <iostream>
#include <cmath>

Profiler& Profiler::Get() {
	static Profiler instance;
	return instance;
}

void Profiler::SetEnabled(bool enabled) { m_Enabled = enabled; }
bool Profiler::IsEnabled() const { return m_Enabled; }
void Profiler::SetTelemetryEnabled(bool enabled) { m_TelemetryEnabled = enabled; }
bool Profiler::IsTelemetryEnabled() const { return m_TelemetryEnabled; }

void Profiler::BeginFrame() {
	if (!m_Enabled && !m_TelemetryEnabled) return;
	if (m_Enabled) {
		m_CurrentEntries.clear();
	}
	if (m_TelemetryEnabled) {
		m_CurrentFrameTotals.clear();
		m_CurrentCounters.clear();
	}
}

void Profiler::EndFrame() {
	if (!m_Enabled && !m_TelemetryEnabled) return;
	if (m_Enabled) {
		m_LastEntries = m_CurrentEntries;
	}
	if (m_TelemetryEnabled) {
		m_LastFrameTotals = m_CurrentFrameTotals;
		m_LastCounters = m_CurrentCounters;
		for (const auto& kv : m_CurrentFrameTotals) {
			PushFrameSample(kv.first, kv.second);
		}
		EvaluateBudgets();
	}
	++m_FrameIndex;
}

void Profiler::Record(const std::string& name, double durationMs) {
	if (!m_Enabled && !m_TelemetryEnabled) return;
	if (m_Enabled) {
		Entry& e = m_CurrentEntries[name];
		if (e.name.empty()) e.name = name;
		e.totalMs += durationMs;
		e.callCount += 1;
	}
	if (m_TelemetryEnabled) {
		m_CurrentFrameTotals[name] += durationMs;
	}
}

void Profiler::RecordScriptSample(const std::string& scriptClassName, double durationMs) {
	Record(std::string("Script/") + scriptClassName, durationMs);
}

const std::unordered_map<std::string, Profiler::Entry>& Profiler::GetEntries() const {
	return m_CurrentEntries;
}

std::vector<Profiler::Entry> Profiler::GetSortedEntriesByTimeDesc() const {
	std::vector<Entry> list;
	list.reserve(m_CurrentEntries.size());
	for (const auto& kv : m_CurrentEntries) list.push_back(kv.second);
	std::sort(list.begin(), list.end(), [](const Entry& a, const Entry& b){ return a.totalMs > b.totalMs; });
	return list;
}

std::vector<Profiler::PercentileEntry> Profiler::GetPercentileEntriesByP95Desc() const {
	std::vector<PercentileEntry> rows;
	rows.reserve(m_HistoryBySection.size());
	for (const auto& kv : m_HistoryBySection) {
		const auto& hist = kv.second;
		if (hist.empty()) continue;
		std::vector<double> sorted(hist.begin(), hist.end());
		std::sort(sorted.begin(), sorted.end());
		PercentileEntry row;
		row.name = kv.first;
		row.samples = static_cast<uint32_t>(sorted.size());
		row.lastMs = hist.back();
		row.p50Ms = PercentileFromSorted(sorted, 0.50);
		row.p95Ms = PercentileFromSorted(sorted, 0.95);
		row.p99Ms = PercentileFromSorted(sorted, 0.99);
		rows.push_back(std::move(row));
	}
	std::sort(rows.begin(), rows.end(), [](const PercentileEntry& a, const PercentileEntry& b) {
		return a.p95Ms > b.p95Ms;
	});
	return rows;
}

const std::unordered_map<std::string, Profiler::Entry>& Profiler::GetLastFrameEntries() const {
	return m_LastEntries.empty() ? m_CurrentEntries : m_LastEntries;
}

std::vector<Profiler::Entry> Profiler::GetSortedLastFrameEntriesByTimeDesc() const {
	const auto& src = m_LastEntries.empty() ? m_CurrentEntries : m_LastEntries;
	std::vector<Entry> list;
	list.reserve(src.size());
	for (const auto& kv : src) list.push_back(kv.second);
	std::sort(list.begin(), list.end(), [](const Entry& a, const Entry& b){ return a.totalMs > b.totalMs; });
	return list;
}

void Profiler::AddCounter(const std::string& name, uint64_t delta) {
	if (!m_TelemetryEnabled) return;
	m_CurrentCounters[name] += delta;
}

void Profiler::SetCounter(const std::string& name, uint64_t value) {
	if (!m_TelemetryEnabled) return;
	m_CurrentCounters[name] = value;
}

const std::unordered_map<std::string, uint64_t>& Profiler::GetLastFrameCounters() const {
	return m_LastCounters.empty() ? m_CurrentCounters : m_LastCounters;
}

void Profiler::SetBudgetP95(const std::string& name, double ms) {
	m_BudgetP95Ms[name] = std::max(0.0, ms);
}

void Profiler::ClearBudget(const std::string& name) {
	m_BudgetP95Ms.erase(name);
}

void Profiler::ClearBudgets() {
	m_BudgetP95Ms.clear();
}

std::vector<Profiler::BudgetAlert> Profiler::ConsumeBudgetAlerts() {
	std::vector<BudgetAlert> out = std::move(m_PendingAlerts);
	m_PendingAlerts.clear();
	return out;
}

void Profiler::ConfigureDefaultBudgets() {
	if (m_DefaultBudgetsConfigured) return;
	m_DefaultBudgetsConfigured = true;
	// CPU critical-path budgets (warn-only).
	SetBudgetP95("Navigation", 2.0);
	SetBudgetP95("Physics/Step", 2.5);
	SetBudgetP95("Render/MeshGather", 2.5);
	SetBudgetP95("Render/Terrain", 2.5);
	SetBudgetP95("Scripts/Update", 1.5);
	SetBudgetP95("AssetPipeline/MainThread", 1.0);
	SetBudgetP95("SyncContext", 1.5);
}

double Profiler::PercentileFromSorted(const std::vector<double>& values, double q) {
	if (values.empty()) return 0.0;
	if (q <= 0.0) return values.front();
	if (q >= 1.0) return values.back();
	const double pos = q * static_cast<double>(values.size() - 1);
	const size_t lo = static_cast<size_t>(std::floor(pos));
	const size_t hi = static_cast<size_t>(std::ceil(pos));
	if (lo == hi) return values[lo];
	const double t = pos - static_cast<double>(lo);
	return values[lo] * (1.0 - t) + values[hi] * t;
}

void Profiler::PushFrameSample(const std::string& name, double totalMs) {
	auto& hist = m_HistoryBySection[name];
	hist.push_back(totalMs);
	while (hist.size() > m_MaxHistoryFrames) {
		hist.pop_front();
	}
}

void Profiler::EvaluateBudgets() {
	if (m_BudgetP95Ms.empty()) return;
	for (const auto& kv : m_BudgetP95Ms) {
		const auto it = m_HistoryBySection.find(kv.first);
		if (it == m_HistoryBySection.end()) continue;
		const auto& hist = it->second;
		if (hist.size() < 30) continue; // Require enough samples for stable p95.
		std::vector<double> sorted(hist.begin(), hist.end());
		std::sort(sorted.begin(), sorted.end());
		const double p95 = PercentileFromSorted(sorted, 0.95);
		if (p95 <= kv.second) continue;
		BudgetAlert alert;
		alert.name = kv.first;
		alert.budgetP95Ms = kv.second;
		alert.observedP95Ms = p95;
		alert.frameIndex = m_FrameIndex;
		m_PendingAlerts.push_back(alert);
		// Rate-limit warning logs to avoid spamming the terminal.
		if (m_FrameIndex >= m_LastBudgetWarnFrame + 120) {
			std::cerr << "[PerfBudget] p95 budget exceeded: " << alert.name
				<< " p95=" << alert.observedP95Ms
				<< "ms budget=" << alert.budgetP95Ms << "ms\n";
			m_LastBudgetWarnFrame = m_FrameIndex;
		}
	}
}

Profiler::MemoryStats Profiler::GetProcessMemory() const {
	MemoryStats m{};
#if defined(_WIN32)
	PROCESS_MEMORY_COUNTERS_EX pmc{};
	pmc.cb = sizeof(pmc);
	if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&pmc), sizeof(pmc))) {
		m.workingSetBytes = static_cast<uint64_t>(pmc.WorkingSetSize);
		m.privateBytes    = static_cast<uint64_t>(pmc.PrivateUsage);
	}
#endif
	return m;
}

ScopedTimer::~ScopedTimer() {
	auto end = std::chrono::high_resolution_clock::now();
	double ms = std::chrono::duration<double, std::milli>(end - m_Start).count();
	Profiler::Get().Record(m_Label, ms);
}


