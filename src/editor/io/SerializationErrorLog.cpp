#include "SerializationErrorLog.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iomanip>

namespace fs = std::filesystem;

namespace cm { namespace editor {

static std::string NowIso8601() {
	auto now = std::chrono::system_clock::now();
	std::time_t t = std::chrono::system_clock::to_time_t(now);
	std::tm tm{};
#if defined(_WIN32)
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	char buf[64];
	std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
	return std::string(buf);
}

// Safe for filenames on Windows (no ':'), includes milliseconds for uniqueness
static std::string NowForFilename() {
	auto now = std::chrono::system_clock::now();
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
	std::time_t t = std::chrono::system_clock::to_time_t(now);
	std::tm tm{};
#if defined(_WIN32)
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	char datebuf[64];
	// YYYYMMDD_HHMMSS
	std::strftime(datebuf, sizeof(datebuf), "%Y%m%d_%H%M%S", &tm);
	char out[80];
	std::snprintf(out, sizeof(out), "%s_%03lld", datebuf, (long long)ms.count());
	return std::string(out);
}

SerializationErrorLog::SerializationErrorLog() {
	EnsureFile();
}

void SerializationErrorLog::EnsureFile() {
	if (!m_FilePath.empty()) return;
	fs::create_directories("logs/serialization");
	// Timestamp safe for Windows filenames + milliseconds to avoid collisions
	m_FilePath = (fs::path("logs/serialization") / ("serializer_sanity_" + NowForFilename() + ".jsonl")).string();
}

void SerializationErrorLog::Append(const ErrorRecord& r) {
	EnsureFile();
	std::ofstream out(m_FilePath, std::ios::app);
	if (!out) return;
	nlohmann::json j = {
		{"ts", r.ts.empty()? NowIso8601() : r.ts},
		{"scenePath", r.scenePath},
		{"assetType", r.assetType},
		{"check", r.check},
		{"severity", r.severity},
		{"entityGuid", r.entityGuid},
		{"component", r.component},
		{"path", r.path},
		{"expected", r.expected},
		{"actual", r.actual},
		{"epsilon", r.epsilon},
		{"explain", r.explain}
	};
	out << j.dump() << "\n";
	out.flush();
}

void SerializationErrorLog::AppendBatch(const std::vector<ErrorRecord>& records) {
	for (const auto& r : records) Append(r);
}

}} // namespace cm::editor


