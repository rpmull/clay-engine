#pragma once
#include <string>
#include <vector>
#include <span>
#include <nlohmann/json.hpp>

namespace cm { namespace editor {

struct ErrorRecord {
	std::string ts;
	std::string scenePath;
	std::string assetType;
	std::string check;
	std::string severity;
	std::string entityGuid;
	std::string component;
	std::string path;
	double expected = 0.0;
	double actual = 0.0;
	double epsilon = 0.0;
	std::string explain;
};

class SerializationErrorLog {
public:
	SerializationErrorLog();
	void Append(const ErrorRecord& r);
	void AppendBatch(const std::vector<ErrorRecord>& records);
	const std::string& GetCurrentFilePath() const { return m_FilePath; }
private:
	void EnsureFile();
	std::string m_FilePath;
};

}} // namespace cm::editor


