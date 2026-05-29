#pragma once
#include <chrono>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <cstring>
#include <atomic>

// High-performance frame profiler for GPU/CPU timing
// Designed to match Unity/Unreal profiler accuracy with minimal overhead
// Uses ring buffers and lock-free structures for profiling without affecting performance

namespace cm {
namespace profiling {

// Frame timing categories matching PERF_PLANS.md budget
enum class FrameSection : uint8_t {
    Rendering = 0,
    Physics,
    Scripts,
    Jobs,
    EditorUI,
    Total,
    Count
};

inline const char* GetSectionName(FrameSection section) {
    switch (section) {
        case FrameSection::Rendering: return "Rendering";
        case FrameSection::Physics: return "Physics";
        case FrameSection::Scripts: return "Scripts";
        case FrameSection::Jobs: return "Jobs";
        case FrameSection::EditorUI: return "EditorUI";
        case FrameSection::Total: return "Total";
        default: return "Unknown";
    }
}

// Target budgets from PERF_PLANS.md (in milliseconds)
inline float GetTargetBudget(FrameSection section) {
    switch (section) {
        case FrameSection::Rendering: return 7.0f;
        case FrameSection::Physics: return 2.5f;
        case FrameSection::Scripts: return 1.5f;
        case FrameSection::Jobs: return 1.0f;
        case FrameSection::EditorUI: return 1.0f;
        case FrameSection::Total: return 16.67f; // 60fps target
        default: return 16.67f;
    }
}

// Timing sample with minimal overhead
struct TimingSample {
    float durationMs = 0.0f;
    uint32_t frameNumber = 0;
};

// Rolling statistics for a section
struct SectionStats {
    float averageMs = 0.0f;
    float minMs = 0.0f;
    float maxMs = 0.0f;
    float lastMs = 0.0f;
    uint32_t sampleCount = 0;
    bool overBudget = false;
    
    void Reset() {
        averageMs = minMs = maxMs = lastMs = 0.0f;
        sampleCount = 0;
        overBudget = false;
    }
};

// Lock-free ring buffer for timing samples
template<size_t N>
class TimingRingBuffer {
public:
    static_assert((N & (N - 1)) == 0, "N must be power of 2");
    
    void Push(float durationMs, uint32_t frameNumber) {
        size_t idx = m_writePos.fetch_add(1, std::memory_order_relaxed) & (N - 1);
        m_samples[idx].durationMs = durationMs;
        m_samples[idx].frameNumber = frameNumber;
    }
    
    SectionStats ComputeStats(uint32_t currentFrame, uint32_t windowFrames = 60) const {
        SectionStats stats;
        stats.Reset();
        
        float sum = 0.0f;
        float minVal = 1e9f;
        float maxVal = 0.0f;
        uint32_t count = 0;
        
        for (size_t i = 0; i < N; ++i) {
            const auto& s = m_samples[i];
            if (currentFrame - s.frameNumber < windowFrames && s.durationMs > 0.0f) {
                sum += s.durationMs;
                minVal = std::min(minVal, s.durationMs);
                maxVal = std::max(maxVal, s.durationMs);
                ++count;
            }
        }
        
        if (count > 0) {
            stats.averageMs = sum / count;
            stats.minMs = minVal;
            stats.maxMs = maxVal;
            stats.sampleCount = count;
            
            // Get most recent sample
            size_t lastIdx = (m_writePos.load(std::memory_order_relaxed) - 1) & (N - 1);
            stats.lastMs = m_samples[lastIdx].durationMs;
        }
        
        return stats;
    }
    
private:
    std::array<TimingSample, N> m_samples{};
    std::atomic<size_t> m_writePos{0};
};

// Main profiler class
class FrameProfiler {
public:
    static constexpr size_t kHistorySize = 256; // Power of 2 for efficient ring buffer
    
    static FrameProfiler& Get() {
        static FrameProfiler instance;
        return instance;
    }
    
    void BeginFrame() {
        m_frameStart = std::chrono::high_resolution_clock::now();
        ++m_frameNumber;
    }
    
    void EndFrame() {
        auto now = std::chrono::high_resolution_clock::now();
        float totalMs = std::chrono::duration<float, std::milli>(now - m_frameStart).count();
        m_sections[static_cast<size_t>(FrameSection::Total)].Push(totalMs, m_frameNumber);
    }
    
    void BeginSection(FrameSection section) {
        m_sectionStarts[static_cast<size_t>(section)] = std::chrono::high_resolution_clock::now();
    }
    
    void EndSection(FrameSection section) {
        auto now = std::chrono::high_resolution_clock::now();
        auto start = m_sectionStarts[static_cast<size_t>(section)];
        float durationMs = std::chrono::duration<float, std::milli>(now - start).count();
        m_sections[static_cast<size_t>(section)].Push(durationMs, m_frameNumber);
    }
    
    SectionStats GetStats(FrameSection section, uint32_t windowFrames = 60) const {
        SectionStats stats = m_sections[static_cast<size_t>(section)]
            .ComputeStats(m_frameNumber, windowFrames);
        stats.overBudget = stats.averageMs > GetTargetBudget(section);
        return stats;
    }
    
    uint32_t GetFrameNumber() const { return m_frameNumber; }
    
    // Get current FPS based on Total frame time
    float GetCurrentFPS() const {
        SectionStats total = GetStats(FrameSection::Total, 30);
        return total.averageMs > 0.0f ? 1000.0f / total.averageMs : 0.0f;
    }
    
    // Check if we're meeting the 60fps target
    bool IsMeetingBudget() const {
        return GetStats(FrameSection::Total).averageMs <= 16.67f;
    }
    
    // Get formatted stats string for debugging
    std::string GetStatsString() const {
        std::string result;
        result.reserve(512);
        result += "Frame Profiler Stats:\n";
        
        for (size_t i = 0; i < static_cast<size_t>(FrameSection::Count); ++i) {
            FrameSection section = static_cast<FrameSection>(i);
            SectionStats stats = GetStats(section);
            
            char buf[128];
            snprintf(buf, sizeof(buf), "  %s: %.2f ms (avg), %.2f ms (max), %.2f ms (target) %s\n",
                GetSectionName(section),
                stats.averageMs,
                stats.maxMs,
                GetTargetBudget(section),
                stats.overBudget ? "[OVER BUDGET]" : "");
            result += buf;
        }
        
        char fpsStr[64];
        snprintf(fpsStr, sizeof(fpsStr), "  FPS: %.1f\n", GetCurrentFPS());
        result += fpsStr;
        
        return result;
    }
    
private:
    FrameProfiler() = default;
    
    std::array<TimingRingBuffer<kHistorySize>, static_cast<size_t>(FrameSection::Count)> m_sections;
    std::array<std::chrono::high_resolution_clock::time_point, static_cast<size_t>(FrameSection::Count)> m_sectionStarts;
    std::chrono::high_resolution_clock::time_point m_frameStart;
    uint32_t m_frameNumber = 0;
};

// RAII scoped timer for profiling sections
class ScopedProfileSection {
public:
    explicit ScopedProfileSection(FrameSection section) : m_section(section) {
        FrameProfiler::Get().BeginSection(m_section);
    }
    ~ScopedProfileSection() {
        FrameProfiler::Get().EndSection(m_section);
    }
    
    ScopedProfileSection(const ScopedProfileSection&) = delete;
    ScopedProfileSection& operator=(const ScopedProfileSection&) = delete;
    
private:
    FrameSection m_section;
};

// Convenience macros
#define CM_PROFILE_FRAME_BEGIN() cm::profiling::FrameProfiler::Get().BeginFrame()
#define CM_PROFILE_FRAME_END() cm::profiling::FrameProfiler::Get().EndFrame()
#define CM_PROFILE_SECTION(section) cm::profiling::ScopedProfileSection _profiler_##section(cm::profiling::FrameSection::section)
#define CM_PROFILE_RENDERING() CM_PROFILE_SECTION(Rendering)
#define CM_PROFILE_PHYSICS() CM_PROFILE_SECTION(Physics)
#define CM_PROFILE_SCRIPTS() CM_PROFILE_SECTION(Scripts)
#define CM_PROFILE_JOBS() CM_PROFILE_SECTION(Jobs)
#define CM_PROFILE_EDITOR_UI() CM_PROFILE_SECTION(EditorUI)

} // namespace profiling
} // namespace cm

