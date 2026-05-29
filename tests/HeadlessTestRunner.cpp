#include "TestFramework.h"
#include "TestEnvironment.h"
#include "HeadlessConfig.h"
#include <cstring>
#include <iostream>

static void PrintUsage() {
    std::cout << "ClaymoreHeadlessTests options:\n"
              << "  --scene <path>     Load a scene file for round-trip test\n"
              << "  --epsilon <value>  Float epsilon for deep compare\n"
              << "  --filter <text>    Run tests matching substring\n"
              << "  --all              Run all tests (default is headless/*)\n"
              << "  --list             List tests\n"
              << "  --verbose          Print passing tests\n";
}

int main(int argc, char** argv) {
    cm::test::JobSystemScope jobs(1);
    cm::test::RunOptions opts;
    opts.filter = "headless/";

    auto& config = cm::test::GetHeadlessConfig();

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            PrintUsage();
            return 0;
        } else if (std::strcmp(arg, "--scene") == 0 && i + 1 < argc) {
            config.scenePath = argv[++i];
        } else if (std::strncmp(arg, "--scene=", 8) == 0) {
            config.scenePath = arg + 8;
        } else if (std::strcmp(arg, "--epsilon") == 0 && i + 1 < argc) {
            config.floatEpsilon = std::stof(argv[++i]);
        } else if (std::strncmp(arg, "--epsilon=", 10) == 0) {
            config.floatEpsilon = std::stof(arg + 10);
        } else if (std::strcmp(arg, "--all") == 0) {
            opts.filter.clear();
        }
    }

    opts = cm::test::ParseArgs(argc, argv, opts);
    return cm::test::RunAll(opts);
}
