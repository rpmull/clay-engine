#pragma once

#include "EditorActionRegistry.h"

namespace editorui {

class EditorCommandPalette {
public:
    void Open();
    void Render(EditorActionRegistry& registry);

private:
    void RefreshResults(EditorActionRegistry& registry);

private:
    bool m_OpenRequested = false;
    char m_Filter[160] = {};
    int m_Selection = 0;
    uint64_t m_LastRegistryVersion = 0;
    std::string m_LastQuery;
    std::vector<EditorActionSearchResult> m_CachedResults;
};

} // namespace editorui
