#pragma once
#include "core/jobs/JobSystem.h"
#include "core/jobs/Jobs.h"

namespace cm { namespace test {

class JobSystemScope {
public:
    explicit JobSystemScope(size_t threads = 1)
        : m_Jobs(threads), m_Prev(cm::g_JobSystem) {
        cm::g_JobSystem = &m_Jobs;
    }

    ~JobSystemScope() {
        cm::g_JobSystem = m_Prev;
    }

    JobSystemScope(const JobSystemScope&) = delete;
    JobSystemScope& operator=(const JobSystemScope&) = delete;

private:
    JobSystem m_Jobs;
    JobSystem* m_Prev = nullptr;
};

}} // namespace cm::test
