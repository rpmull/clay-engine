#pragma once
#include <string>
#include <vector>
#include <stdexcept>

namespace cm { namespace test {

struct TestCase {
    const char* name;
    void (*fn)();
};

class TestFailure : public std::runtime_error {
public:
    TestFailure(const char* expr, const char* file, int line);
};

struct Registrar {
    Registrar(const char* name, void (*fn)());
};

struct RunOptions {
    std::string filter;
    bool list = false;
    bool verbose = false;
};

RunOptions ParseArgs(int argc, char** argv, const RunOptions& defaults);
int RunAll(const RunOptions& opts);

}} // namespace cm::test

#define CM_TEST_NAMED(func, display_name) \
    static void func(); \
    static cm::test::Registrar reg_##func(display_name, func); \
    static void func()

#define CM_TEST(name) CM_TEST_NAMED(name, #name)

#define CM_ASSERT(expr) \
    do { \
        if (!(expr)) { \
            throw cm::test::TestFailure(#expr, __FILE__, __LINE__); \
        } \
    } while (0)
