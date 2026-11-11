#include "execute.h"

#include <type_traits>
#include "base/format.h"
#include "runtime/arch/arm32/state.h"
#include "runtime/arch/arm64/state.h"
#include "runtime/arch/x86/state.h"
#include "runtime/backend/arm64/compile.h"
#include "runtime/backend/loongarch64/compile.h"
#include "runtime/backend/riscv64/compile.h"
#include "runtime/backend/x64/compile.h"
#include "runtime/frontend/arm32/build_ir.h"
#include "runtime/frontend/arm64/build_ir.h"
#include "runtime/frontend/x86/build_ir.h"
#include "runtime/ir/optimize.h"

namespace swift::tests::correctness {
namespace {
namespace ir = runtime::ir;
namespace backend = runtime::backend;

ir::TargetInfo Target(const RunConfig& config) {
    ir::TargetInfo target;
    switch (config.backend) {
        case Backend::Arm64:
            target = backend::arm64::KnownTargetInfo();
            break;
        case Backend::X64:
            target = backend::x64::KnownTargetInfo();
            break;
        case Backend::Riscv64:
            target = backend::riscv64::KnownTargetInfo();
            break;
        case Backend::LoongArch64:
            target = backend::loongarch64::KnownTargetInfo();
            break;
        case Backend::Direct:
            break;
    }
    target.mode = static_cast<ir::TargetMode>(config.target);
    return target;
}

runtime::frontend::BuildResult Build(const Case& c, const RunConfig& config) {
    const runtime::frontend::BlockWindow window{c.code_address, c.code};
    const std::span windows{&window, 1};
    const auto scope = static_cast<ir::Scope>(config.scope);
    const auto target = Target(config);
    const runtime::frontend::DecodeLimits limits{128, 32};
    if (c.isa == ISA::Arm64)
        return config.scope == Scope::Function
                       ? swift::arm64::BuildFunctionIR(windows, c.initial.pc, {}, limits, target)
                       : swift::arm64::BuildIR(windows, scope, {}, target);
    if (c.isa == ISA::Arm32) {
        const auto cpsr = static_cast<std::uint32_t>(c.initial.flags);
        const auto it = static_cast<std::uint8_t>(((cpsr >> 25) & 3) | ((cpsr >> 8) & 252));
        const auto pc = runtime::arm32::Location::Make(c.initial.pc,
                                                      cpsr & 32,
                                                      it,
                                                      c.initial.fp_control)
                                .SetEFlag(cpsr & 512)
                                .Key();
        runtime::arm32::Profile profile;
        if (c.profile == "armv8-user") profile.version = runtime::arm32::ArchVersion::v8;
        return config.scope == Scope::Function
                       ? swift::arm32::BuildFunctionIR(windows, pc, {}, limits, target, nullptr,
                                                      nullptr, nullptr, {}, nullptr, nullptr,
                                                      profile)
                       : swift::arm32::BuildIR(windows, pc, scope, {}, target, nullptr, nullptr,
                                               false, nullptr, nullptr, nullptr, profile);
    }
    using State = runtime::x86::GuestState;
    swift::x86::StateLayout layout;
    layout.gprs_offset = offsetof(State, gprs);
    layout.pc_offset = offsetof(State, pc);
    layout.fprs_offset = offsetof(State, xmm);
    layout.fs_base_offset = offsetof(State, fs_base);
    layout.gs_base_offset = offsetof(State, gs_base);
    layout.mxcsr_offset = offsetof(State, mxcsr);
    layout.x87_offset = offsetof(State, x87);
    layout.x87_ip_offset = offsetof(State, x87_ip);
    layout.x87_dp_offset = offsetof(State, x87_dp);
    layout.x87_opcode_offset = offsetof(State, x87_opcode);
    layout.ymm_high_offset = offsetof(State, ymm_high);
    return config.scope == Scope::Function
                   ? swift::x86::BuildFunctionIR(windows, c.initial.pc, layout, limits, target)
                   : swift::x86::BuildIR(windows, scope, layout, target);
}

template <class Options> Options OptionsFor(ISA isa) {
    Options options;
    if (isa == ISA::Arm64) {
        options.pc_offset = offsetof(runtime::arm64::GuestState, pc);
        options.flags_offset = offsetof(runtime::arm64::GuestState, packed_flags);
    } else {
        options.pc_offset = offsetof(runtime::x86::GuestState, pc);
        options.flags_offset = offsetof(runtime::x86::GuestState, packed_flags);
    }
    // The compile-only matrix covers the full backend surface; VM execution
    // still gates optional instructions on the probed host capabilities.
    if constexpr (std::is_same_v<Options, backend::arm64::CompileOptions>) {
        options.crc32c = true;
        if (isa == ISA::Arm64) options.fpsr_offset = offsetof(runtime::arm64::GuestState, fpsr);
    }
    return options;
}

}  // namespace

Observation ExecuteCompile(const Case& c, const RunConfig& config) {
    Observation out;
    out.backend = Name(config.backend);
    out.stage = "build";
    out.outcome = Outcome::Rejected;
    auto built = Build(c, config);
    if (!built) {
        out.error = Format("frontend={} ir={} pc={:#x}",
                           unsigned(built.error),
                           unsigned(built.ir_error),
                           built.error_pc);
        return out;
    }
    if (config.optimize) {
        out.stage = "optimize";
        auto optimized = ir::Optimize(std::move(built.program));
        if (!optimized) {
            out.error = Format("IR optimize error={}", unsigned(optimized.error));
            return out;
        }
        built.program = std::move(optimized.program);
    }
    out.actual_scope = static_cast<Scope>(built.program.ScopeKind());
    out.actual_ir_blocks = built.program.Blocks().size();
    out.stage = "compile";
    backend::CompileResult code;
    switch (config.backend) {
        case Backend::Arm64:
            code = backend::arm64::Compile(built.program,
                                           OptionsFor<backend::arm64::CompileOptions>(c.isa));
            break;
        case Backend::X64:
            code = backend::x64::Compile(built.program,
                                         OptionsFor<backend::x64::CompileOptions>(c.isa));
            break;
        case Backend::Riscv64:
            code = backend::riscv64::Compile(built.program,
                                             OptionsFor<backend::riscv64::CompileOptions>(c.isa));
            break;
        case Backend::LoongArch64:
            code = backend::loongarch64::Compile(
                    built.program, OptionsFor<backend::loongarch64::CompileOptions>(c.isa));
            break;
        case Backend::Direct:
            out.outcome = Outcome::Unavailable;
            out.error = "compile-only requires a native backend";
            return out;
    }
    out.compile_attempts = 1;
    if (!code) {
        const auto operations = built.program.Operations();
        const auto opcode = code.node.value < operations.size()
                                    ? ir::Name(operations[code.node.value].opcode)
                                    : "?";
        out.error = Format("backend={} node={} block={} opcode={}",
                           unsigned(code.error),
                           code.node.value,
                           code.block.value,
                           opcode);
        return out;
    }
    out.spill_slots = code.spill_slots;
    out.frame_bytes = code.frame_bytes;
    out.outcome = Outcome::Compiled;
    return out;
}

}  // namespace swift::tests::correctness
