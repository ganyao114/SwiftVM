#pragma once
#include "runtime/arch/x86/state.h"
#include "runtime/arch/x86/x87.h"
#include "runtime/backend/arm64/abi.h"
#include "runtime/backend/arm64/compile.h"
#include "runtime/backend/arm64/patch.h"
#include "runtime/backend/arm64/recovery.h"
#include "runtime/ir/builder.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined(__aarch64__) && defined(__ANDROID__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>
#include "runtime/execution/code_cache.h"
#include "runtime/execution/code_heap.h"
#include "runtime/execution/dispatch.h"
#include "runtime/execution/fault.h"
#include "runtime/platform/linux/arm64_host.h"
#endif

#include "runtime/backend/arm64/allocate.h"
#include "tests/support/check.h"
#include "tests/support/ir_builder.h"
#include "tests/support/native_code.h"
#include "tests/support/test_mode.h"

namespace swift::tests::arm64_fixture {
using namespace runtime::ir;
namespace arm64 = runtime::backend::arm64;
using u64 = std::uint64_t;
using ir_builder::Constant;
using ir_builder::Need;
using ir_builder::Submit;
using swift::tests::Check;
using swift::tests::NativeMode;

struct QueryPair {
    u64 lower, upper;
};

inline BlockRef Create(Builder& builder) { return ir_builder::Block(builder); }

std::uint32_t WordOffset(runtime::backend::NativeOffset offset) noexcept;
#if defined(__aarch64__) && defined(__ANDROID__)
namespace execution = ::swift::runtime::execution;
const runtime::execution::HostBackendOps& TestBackendOps();
void BindFaultCallbacks(runtime::execution::FaultContext&, runtime::execution::NativeRunContext&);
void PatchLinkWords(std::uintptr_t*, const std::uintptr_t*, std::uintptr_t, std::uint32_t);
void UnlinkWords(std::uintptr_t*, const std::uintptr_t*, std::uint32_t);
#endif
Value Load(Builder&, std::uint32_t);
void Store(Builder&, std::uint32_t, Value);
void StoreCarrier(Builder&, std::uint32_t, Value);
u64 Read64(const void*);
void Write64(void*, u64);
arm64::CompileResult Compile(const Program&, arm64::CompileOptions = {});
void Execute(const arm64::CompileResult&, std::uint8_t*, u64);
Value Snapshot(Builder&, Value);
u64 ReferenceArithmetic(OpCode, u64, u64, unsigned, FlagProfile, u64, u64);
}  // namespace swift::tests::arm64_fixture
