#pragma once

#include "tests/correctness/case.h"
#include "runtime/arch/arm32/state.h"
#include "runtime/arch/arm64/state.h"
#include "runtime/arch/x86/state.h"

namespace swift::tests::correctness {

void Inject(const State&, runtime::x86::GuestState&);
void Inject(const State&, runtime::arm64::GuestState&);
void Inject(const State&, runtime::arm32::GuestState&);
State Project(const runtime::x86::GuestState&);
State Project(const runtime::arm64::GuestState&);
State Project(const runtime::arm32::GuestState&);

}  // namespace swift::tests::correctness
