#include "TestFramework.h"
#include <iostream>
#include <sstream>
#include <cstring>

namespace cm { namespace test {

static std::vector<TestCase>& Registry() {
    static std::vector<TestCase> tests;
    return tests;
}

TestFailure::TestFailure(const char* expr, const char* file, int line)
    : std::runtime_error([&]() {
        std::ostringstream oss;
        oss << file << ":" << line << " assertion failed: " << expr;
        return oss.str();
    }()) {}

Registrar::Registrar(const char* name, void (*fn)()) {
    Registry().push_back({ name, fn });
}

static bool MatchesFilter(const char* name, const std::string& filter) {
    if (filter.empty()) return true;
    return std::string(name).find(filter) != std::string::npos;
}

RunOptions ParseArgs(int argc, char** argv, const RunOptions& defaults) {
    RunOptions opts = defaults;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--list") == 0) {
            opts.list = true;
        } else if (std::strcmp(arg, "--verbose") == 0) {
            opts.verbose = true;
        } else if (std::strcmp(arg, "--filter") == 0 && i + 1 < argc) {
            opts.filter = argv[++i];
        } else if (std::strncmp(arg, "--filter=", 9) == 0) {
            opts.filter = arg + 9;
        }
    }
    return opts;
}

int RunAll(const RunOptions& opts) {
    const auto& tests = Registry();
    if (opts.list) {
        for (const auto& t : tests) {
            if (MatchesFilter(t.name, opts.filter)) {
                std::cout << t.name << "\n";
            }
        }
        return 0;
    }

    int failures = 0;
    int ran = 0;
    for (const auto& t : tests) {
        if (!MatchesFilter(t.name, opts.filter)) {
            continue;
        }
        ++ran;
        try {
            t.fn();
            if (opts.verbose) {
                std::cout << "[PASS] " << t.name << "\n";
            }
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "[FAIL] " << t.name << " - " << e.what() << "\n";
        } catch (...) {
            ++failures;
            std::cerr << "[FAIL] " << t.name << " - unknown exception\n";
        }
    }

    std::cout << "[Tests] Ran " << ran << " test(s), " << failures << " failure(s)\n";
    return failures;
}

}} // namespace cm::test
