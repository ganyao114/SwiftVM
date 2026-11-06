#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <span>
#include <utility>

#include "build_ir.h"
#include "instruction.h"
#include "runtime/arch/x86/x87.h"
#include "runtime/arch/x86/flags.h"

namespace swift::x86 {
std::uint8_t DisabledCetHintSize(std::span<const std::uint8_t>) noexcept;
}

namespace swift::x86::detail {
using namespace runtime::ir;
using runtime::x86::X87Command;
using runtime::x86::X87Encode;
using runtime::x86::X87Result;
using u64 = std::uint64_t;
using u32 = std::uint32_t;
struct Rejected {
    BuildError error;
    Error detail{Error::None};
};
using ::swift::x86::DisabledCetHintSize;

template <class Sink> struct Semantics {
    using Emitter = Sink;
    using Value = typename Sink::Value;
    using Producer = typename Sink::Producer;
    using Operand = typename Sink::Operand;
    static Value Require(typename Sink::Result result) {
        if (!result) throw Rejected{BuildError::IRRejected, result.error};
        return result.GetValue();
    }
    static void Require(Error error) {
        if (error != Error::None) throw Rejected{BuildError::IRRejected, error};
    }
    [[noreturn]] static void Unsupported() { throw Rejected{BuildError::Unsupported}; }

#include "semantics_operands.inc"
#include "semantics_integer.inc"
#include "semantics_scalar_ext.inc"
#include "semantics_bmi.inc"
#include "semantics_control.inc"
#include "semantics_vector.inc"
#include "semantics_sse_ext.inc"
#include "semantics_sse42.inc"
#include "semantics_sse_fp.inc"
#include "semantics_crypto.inc"
#include "semantics_system.inc"
#include "semantics_user.inc"
#include "semantics_x87.inc"
#include "semantics_mmx.inc"
#include "semantics_avx.inc"
#include "semantics_dispatch.inc"
};
}  // namespace swift::x86::detail
