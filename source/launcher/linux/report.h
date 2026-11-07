#pragma once

#include "runtime/execution/runtime.h"
#include <cstdio>

namespace swift::launcher {
void WriteReport(std::FILE*,
                 const runtime::execution::RunResult&,
                 const runtime::execution::RuntimeOptions&);
}  // namespace swift::launcher
