#include "HeadlessConfig.h"

namespace cm { namespace test {

HeadlessConfig& GetHeadlessConfig() {
    static HeadlessConfig config;
    return config;
}

}} // namespace cm::test
