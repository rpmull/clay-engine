#pragma once

#include "EditorPanel.h"
#include "core/utils/Profiler.h"
#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

class ProfilerPanel : public EditorPanel {
public:
	void OnImGuiRender() {
		if (!m_Open) return;
		Profiler& prof = Profiler::Get();
		if (!ImGui::Begin("Profiler", &m_Open)) {
			ImGui::End();
			return;
		}
		// Profiling runs only when this window is open (gated in Application::Run)
		ImGui::TextDisabled("Profiling active when window is open");

		const auto now = std::chrono::steady_clock::now();
		const bool shouldRefresh =
			!m_HasCachedData ||
			std::chrono::duration_cast<std::chrono::milliseconds>(now - m_LastRefreshTime).count() >= kRefreshIntervalMs;
		if (shouldRefresh) {
			m_CachedMemory = prof.GetProcessMemory();
			m_CachedCpuRows = prof.GetSortedLastFrameEntriesByTimeDesc();
			m_CachedPercentileRows = prof.GetPercentileEntriesByP95Desc();
			m_CachedCounters.clear();
			const auto& counters = prof.GetLastFrameCounters();
			m_CachedCounters.reserve(counters.size());
			for (const auto& kv : counters) {
				m_CachedCounters.emplace_back(kv.first, kv.second);
			}
			std::sort(m_CachedCounters.begin(), m_CachedCounters.end(), [](const auto& a, const auto& b) {
				return a.first < b.first;
			});
			m_LastRefreshTime = now;
			m_HasCachedData = true;
		}

		const Profiler::MemoryStats mem = m_CachedMemory;
		ImGui::Text("Working Set: %.2f MB", mem.workingSetBytes / (1024.0 * 1024.0));
		ImGui::SameLine();
		ImGui::Text("Private: %.2f MB", mem.privateBytes / (1024.0 * 1024.0));
		ImGui::Separator();

		if (ImGui::BeginTable("cpu", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
			ImGui::TableSetupColumn("Scope");
			ImGui::TableSetupColumn("Total (ms)");
			ImGui::TableSetupColumn("Calls");
			ImGui::TableHeadersRow();

			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(m_CachedCpuRows.size()));
			while (clipper.Step()) {
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
					const auto& e = m_CachedCpuRows[static_cast<size_t>(i)];
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(e.name.c_str());
					ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f", e.totalMs);
					ImGui::TableSetColumnIndex(2); ImGui::Text("%u", e.callCount);
				}
			}
			ImGui::EndTable();
		}

		if (ImGui::CollapsingHeader("Section Percentiles (rolling)", ImGuiTreeNodeFlags_DefaultOpen)) {
			if (ImGui::BeginTable("cpu_percentiles", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
				ImGui::TableSetupColumn("Scope");
				ImGui::TableSetupColumn("P50 (ms)");
				ImGui::TableSetupColumn("P95 (ms)");
				ImGui::TableSetupColumn("P99 (ms)");
				ImGui::TableSetupColumn("Samples");
				ImGui::TableHeadersRow();
				ImGuiListClipper clipper;
				clipper.Begin(static_cast<int>(m_CachedPercentileRows.size()));
				while (clipper.Step()) {
					for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
						const auto& row = m_CachedPercentileRows[static_cast<size_t>(i)];
						ImGui::TableNextRow();
						ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(row.name.c_str());
						ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f", row.p50Ms);
						ImGui::TableSetColumnIndex(2); ImGui::Text("%.3f", row.p95Ms);
						ImGui::TableSetColumnIndex(3); ImGui::Text("%.3f", row.p99Ms);
						ImGui::TableSetColumnIndex(4); ImGui::Text("%u", row.samples);
					}
				}
				ImGui::EndTable();
			}
		}

		if (ImGui::CollapsingHeader("Counters")) {
			if (ImGui::BeginTable("cpu_counters", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
				ImGui::TableSetupColumn("Counter");
				ImGui::TableSetupColumn("Value");
				ImGui::TableHeadersRow();
				ImGuiListClipper clipper;
				clipper.Begin(static_cast<int>(m_CachedCounters.size()));
				while (clipper.Step()) {
					for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
						const auto& kv = m_CachedCounters[static_cast<size_t>(i)];
						ImGui::TableNextRow();
						ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(kv.first.c_str());
						ImGui::TableSetColumnIndex(1); ImGui::Text("%llu", static_cast<unsigned long long>(kv.second));
					}
				}
				ImGui::EndTable();
			}
		}

		ImGui::End();
	}

	void Open() { m_Open = true; }
	void SetOpen(bool open) { m_Open = open; }
	bool IsOpen() const { return m_Open; }

private:
	bool m_Open = false;
	static constexpr int kRefreshIntervalMs = 200;
	bool m_HasCachedData = false;
	std::chrono::steady_clock::time_point m_LastRefreshTime{};
	Profiler::MemoryStats m_CachedMemory{};
	std::vector<Profiler::Entry> m_CachedCpuRows;
	std::vector<Profiler::PercentileEntry> m_CachedPercentileRows;
	std::vector<std::pair<std::string, uint64_t>> m_CachedCounters;
};


