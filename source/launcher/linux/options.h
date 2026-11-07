#pragma once

#include "runtime/execution/runtime.h"

namespace swift::launcher {
struct Options {
    runtime::execution::RuntimeOptions runtime;
    int guest_argument{};
    bool help{};
};

Options ParseOptions(int argc, char* const* argv);
void PrintUsage();
}  // namespace swift::launcher
