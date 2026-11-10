#pragma once

#include "case.h"

namespace swift::tests::correctness {

Observation ExecuteDirect(const Case&);
Observation ExecuteCompile(const Case&, const RunConfig&);
Observation ExecuteVM(const Case&, const RunConfig&);
Backend NativeBackend() noexcept;

}  // namespace swift::tests::correctness
