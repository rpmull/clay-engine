#pragma once
#include <string>

namespace cm { namespace test {

struct HeadlessConfig {
    std::string scenePath;
    float floatEpsilon = 1.0e-5f;
};

HeadlessConfig& GetHeadlessConfig();

}} // namespace cm::test
