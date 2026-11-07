#include "test_mode.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <catch2/catch_session.hpp>

#ifndef SVM_TEST_NATIVE_ISA
#define SVM_TEST_NATIVE_ISA None
#endif

int main(int argc, char** argv) {
    std::vector<char*> arguments;
    arguments.reserve(static_cast<unsigned>(argc) + 1);
    arguments.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--native") == 0)
            swift::tests::SetNativeMode(true);
        else if (std::strcmp(argv[i], "--compile-only") == 0)
            swift::tests::SetNativeMode(false);
        else
            arguments.push_back(argv[i]);
    }
    if (swift::tests::NativeMode() &&
        !swift::tests::CanExecute(swift::tests::NativeISA::SVM_TEST_NATIVE_ISA)) {
        std::puts("native execution unavailable for this target; skipped");
        return 77;
    }
    Catch::Session session;
    const auto parsed =
            session.applyCommandLine(static_cast<int>(arguments.size()), arguments.data());
    if (parsed) return parsed;
    if (session.configData().testsOrTags.empty())
        session.configData().testsOrTags.push_back(swift::tests::NativeMode() ? "[native]"
                                                                              : "[compile]");
    return session.run();
}
