//
// Shared guest VM fixtures for the call-layer tests.
//
// Only ONE JitGuestEnv can be alive at a time (the runtime's guest memory bias
// and the host signal-handler probes are process-global), so this hands out a
// lazily created environment and tears down the other one on a switch.
//
#pragma once

#include <cstdint>

#include "jit_env.h"

namespace svmtest {

enum class VmKind { Stub, Glibc };

// The environment for `kind`.  Aborts the test run (FAIL) if it cannot be
// brought up -- a silently skipped test would defeat the point.
swift::guest_call::JitGuestEnv& Vm(VmKind kind);

// Guest address of stub `index` (abi_stubs_index.h), read from the pointer
// table at the stub ELF's entry point.
std::uint64_t StubEntry(int index);

// Host pointer to the argument-dump buffer written by kStubDumpArgs.
const std::uint64_t* DumpBuffer();
void ClearDumpBuffer();

}  // namespace svmtest
