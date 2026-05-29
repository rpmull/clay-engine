#pragma once
#include "core/jobs/JobSystem.h"

// Global job system accessor - set by Application or RuntimeApplication at startup
namespace cm {
    inline JobSystem* g_JobSystem = nullptr;
}

inline JobSystem& Jobs() { 
    return *cm::g_JobSystem; 
}
