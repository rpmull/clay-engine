#include "TestFramework.h"
#include "TestEnvironment.h"

int main(int argc, char** argv) {
    cm::test::JobSystemScope jobs(1);
    cm::test::RunOptions opts = cm::test::ParseArgs(argc, argv, {});
    return cm::test::RunAll(opts);
}
