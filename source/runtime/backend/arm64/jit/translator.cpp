#pragma once

#include "translator.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <iterator>
#include "runtime/backend/context.h"
#include "runtime/backend/arm64/defines.h"
#include "runtime/common/backedge_control.h"
#include "translator/x86/cpu.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.


JitTranslator::JitTranslator(JitContext& ctx) : context(ctx), masm(ctx.GetMasm()) {
    auto& config = ctx.GetConfig();
    use_memory_base = config.memory_base != nullptr || config.page_table != nullptr;
    guest_addr_mask = config.guest_addr_mask;
    window_uxtw = guest_addr_mask == 0xFFFFFFFFull;
    sse_scalar_insert = True(config.arm64_features & Arm64Features::AFP);
    xmm_pool_ext = True(config.global_opts & Optimizations::XmmPoolExt);
    if (const char* shift_fast = PerfGetenv("SVM_SHIFT_IMM_FAST")) {
        shift_imm_fast = std::strcmp(shift_fast, "0") != 0;
    }
    if (const char* mem_fuse = PerfGetenv("SVM_MEM_NARROW_FUSE")) {
        mem_narrow_fuse = std::strcmp(mem_fuse, "0") != 0;
    }
    if (const char* nan_fast = PerfGetenv("SVM_SSE_NAN_FAST")) {
        sse_nan_fast = std::strcmp(nan_fast, "0") != 0;
    }
    if (const char* nan_coldpath = PerfGetenv("SVM_SSE_NAN_COLDPATH")) {
        sse_nan_coldpath = std::strcmp(nan_coldpath, "0") != 0;
    }
    backedge_latch = BackedgeLatchEnabled();
    backedge_flags = BackedgeFlagsEnabled();
}



void JitTranslator::Translate(ir::Block* block) {
    vixl::svm_vixl_prof::JitScope vixl_prof;
    ASSERT(vec_nan_cold_sites.empty());
    PerfScope2 perf_prologue{GetPerfStats2().codegen_prologue};
    cur_block = block;
    cur_block_is_call = false;
    for (auto& inst : block->GetInstList()) {
        if (inst.GetOp() == ir::OpCode::PushRSB) {
            cur_block_is_call = true;
            break;
        }
    }
    static_next_loc.reset();
    // Function mode keeps one function-sized suppression bitmap. Grow it
    // before the per-block backedge proof marks the two sunk IR instructions.
    disable_instructions.resize(
            std::max<size_t>(disable_instructions.size(), block->MaxInstrId()));
    backedge_flags_plan = PlanBackedgeFlags(block);
    if (backedge_flags_plan) {
        disable_instructions.set(backedge_flags_plan->polarity_load->Id());
        disable_instructions.set(backedge_flags_plan->polarity_store->Id());
    }
    backedge_exit_referenced = false;
    backedge_exit_label =
            backedge_latch && HasSelfEdge(block->GetTerminal())
                    ? std::make_unique<Label>()
                    : nullptr;
    context.SetCurrent(block, backedge_flags_plan != nullptr);
    if (backedge_flags_plan) {
        // Every published/external entry takes the cold initializer below;
        // only the self edge targets local_entry. This makes host NZCV valid
        // before a pre-producer guest fault without charging the steady loop.
        __ B(backedge_flags_plan->external_entry.get());
        __ Bind(backedge_flags_plan->local_entry.get());
        context.BeginBackedgeBody();
        backedge_host_begin = context.CurrentBufferSize();
    }
    // Count the remaining x86 GPR uniform-buffer traffic dynamically without
    // instrumenting each access: add the block's static emitted access count
    // once on entry. ThreadContext64 begins with the 16 8-byte GPRs, so the
    // first 128 uniform bytes are exactly the register-residency region.
    u32 gpr_uniform_accesses = 0;
    u32 xmm_uniform_accesses = 0;
    constexpr u32 kXmmBegin = offsetof(swift::x86::ThreadContext64, xmms);
    constexpr u32 kXmmEnd = kXmmBegin + sizeof(swift::x86::ThreadContext64::xmms);
    for (auto& inst : block->GetInstList()) {
        if ((inst.GetOp() == ir::OpCode::LoadUniform ||
             inst.GetOp() == ir::OpCode::StoreUniform)) {
            const u32 offset = inst.GetArg<ir::Uniform>(0).GetOffset();
            if (offset < 16 * sizeof(u64)) {
                ++gpr_uniform_accesses;
            } else if (offset >= kXmmBegin && offset < kXmmEnd) {
                ++xmm_uniform_accesses;
            }
        }
    }
    context.RecordExecCounter(exec_offset_gpr_uniform_accesses,
                              gpr_uniform_accesses);
    context.RecordExecCounter(exec_offset_xmm_uniform_accesses,
                              xmm_uniform_accesses);
    perf_prologue.Stop();
    PerfScope2 perf_body{GetPerfStats2().codegen_body};
    for (auto& inst : block->GetInstList()) {
        cur_instr = &inst;
        if (inst.Id() < disable_instructions.size() && disable_instructions.test(inst.Id())) {
            continue;
        }
        Translate(&inst);
    }
    perf_body.Stop();

    PerfScope2 perf_terminal{GetPerfStats2().codegen_terminal};
    context.BeginTerminalScratch();
    FlushFlags();
    if (!EmitBackedgeFlagsTerminal(block->GetTerminal())) {
        EmitTerminal(block->GetTerminal());
    }
    context.EndTerminalScratch();
    if (backedge_flags_plan) {
        backedge_host_end = context.CurrentBufferSize();
    }
    // Close the W71 accounting window before out-of-line NaN repair stubs.
    // The hot guard remains in the block; cold handlers are not executed on
    // the normal path and therefore do not belong in static x entry counts.
    context.FinishHotCoalesceBlock();
    context.BeginColdScratch();
    EmitBackedgeExitStub();
    EmitBackedgeColdPaths();
    EmitVecNaNColdPaths();
    context.EndColdScratch();
}

void JitTranslator::Translate(ir::HIRFunction* function) {
    vixl::svm_vixl_prof::JitScope vixl_prof;
    ASSERT(function);
    context.SetCurrent(function->GetFunction());
    disable_instructions.resize(function->MaxInstrCount());
    for (auto& hir_block : function->GetHIRBlocksRPO()) {
        // Undecoded successor left behind by lazy region compilation (and by
        // the pre-existing 128-block cap): no instructions and no terminal.
        // Emitting it would bind a label nobody branches to and then fall into
        // terminal::Invalid -> Ret without setting current_loc, which is a
        // dispatcher loop if it were ever entered.  Its guest address is
        // deliberately never published (TranslateIR skips empty blocks), so the
        // only way in is JitContext::Forward, which routes to it through the L2
        // dispatch slot instead.
        auto* block = hir_block.GetBlock();
        if (block->GetInstList().empty() && !block->HasTerminal()) {
            continue;
        }
        Translate(block);
    }
}

bool JitTranslator::PreservesHostNZCV(ir::OpCode op) {
    // Deliberately narrow emitter audit. These are the only pre-producer
    // operations admitted by the first spike; every listed ARM64 lowering is
    // flag-neutral (including its address arithmetic and CBNZ/TBZ guards).
    switch (op) {
        case ir::OpCode::LoadUniform:
        case ir::OpCode::StoreUniform:
        case ir::OpCode::LoadMemory:
        case ir::OpCode::StoreMemory:
        case ir::OpCode::GetHostGPR:
        case ir::OpCode::GetHostFPR:
        case ir::OpCode::SetHostGPR:
        case ir::OpCode::SetHostFPR:
        case ir::OpCode::LoadImm:
        case ir::OpCode::AdvancePC:
        case ir::OpCode::BitCast:
        case ir::OpCode::GetOperand:
        case ir::OpCode::Zero:
        case ir::OpCode::ZeroExtend32:
        case ir::OpCode::ZeroExtend32To64:
        case ir::OpCode::VecFAdd:
        case ir::OpCode::VecFSub:
        case ir::OpCode::VecFMul:
        // These lower to their non-S forms when no surviving SaveFlags pseudo
        // names them. The proof stops at the first producer that does have
        // such a pseudo, so earlier dead-flags pointer arithmetic is neutral.
        case ir::OpCode::Add:
        case ir::OpCode::Sub:
            return true;
        default:
            return false;
    }
}

bool JitTranslator::MayFaultOrObserve(ir::OpCode op) {
    switch (op) {
        case ir::OpCode::LoadMemory:
        case ir::OpCode::StoreMemory:
        case ir::OpCode::LoadMemoryTSO:
        case ir::OpCode::StoreMemoryTSO:
        case ir::OpCode::MemoryCopy:
        case ir::OpCode::MemoryCopyTSO:
        case ir::OpCode::CompareAndSwap:
        case ir::OpCode::CompareAndSwap128:
        case ir::OpCode::CheckMemoryAlignment:
        case ir::OpCode::AtomicExchange:
        case ir::OpCode::AtomicFetchAdd:
        case ir::OpCode::AtomicRMW:
        case ir::OpCode::CallLambda:
        case ir::OpCode::CallLocation:
        case ir::OpCode::CallDynamic:
        case ir::OpCode::X87Op:
        case ir::OpCode::Sse42Str:
            return true;
        default:
            return false;
    }
}

std::unique_ptr<JitTranslator::BackedgeFlagsPlan>
JitTranslator::PlanBackedgeFlags(ir::Block* block) {
    if (!backedge_flags || !block) {
        return nullptr;
    }

    std::optional<ir::Location> then_target;
    std::optional<ir::Location> else_target;
    ir::Value condition{};
    VisitVariant<void>(block->GetTerminal(), [&](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::If>) {
            condition = term.cond;
            auto direct = [](const ir::Terminal& edge) -> std::optional<ir::Location> {
                return VisitVariant<std::optional<ir::Location>>(
                        edge, [](const auto& target) -> std::optional<ir::Location> {
                            using E = std::decay_t<decltype(target)>;
                            if constexpr (std::is_same_v<E, ir::terminal::LinkBlock> ||
                                          std::is_same_v<E, ir::terminal::LinkBlockFast>) {
                                return target.next;
                            }
                            return std::nullopt;
                        });
            };
            then_target = direct(term.then_);
            else_target = direct(term.else_);
        }
    });
    if (!then_target || !else_target || !condition.Def() ||
        condition.Def()->GetOp() != ir::OpCode::LocalCondSet) {
        return nullptr;
    }
    const auto self = block->GetStartLocation();
    const bool then_self = *then_target == self;
    const bool else_self = *else_target == self;
    if (then_self == else_self) {
        return nullptr;
    }

    ir::Inst* first_producer = nullptr;
    ir::Inst* final_save = nullptr;
    ir::Flags final_requested{};
    for (auto& inst : block->GetInstList()) {
        if (inst.GetOp() != ir::OpCode::SaveFlags) {
            continue;
        }
        auto* producer = inst.GetArg<ir::Value>(0).Def();
        if (!producer) {
            return nullptr;
        }
        if (!first_producer || producer->Id() < first_producer->Id()) {
            first_producer = producer;
        }
        const auto requested = inst.GetArg<ir::Flags>(1);
        if (True(requested & ir::Flags::Parity) &&
            True(requested & ir::Flags::AuxiliaryCarry) &&
            True(requested & ir::Flags::NZCV)) {
            final_save = &inst;
            final_requested = requested;
        }
    }
    if (!first_producer || !final_save) {
        if (PerfGetenv("SVM_DUMP_IR")) {
            fmt::print(stderr, "[backedge-proof] {:#x} reject flags producer\n",
                       block->GetStartLocation().Value());
        }
        return nullptr;
    }

    ir::Inst* polarity_store = nullptr;
    ir::Inst* polarity_load = nullptr;
    u8 polarity = 0;
    constexpr u32 kCarryOffset = offsetof(swift::x86::ThreadContext64,
                                          carry_inverted);
    for (auto& inst : block->GetInstList()) {
        if (inst.Id() <= final_save->Id() ||
            inst.GetOp() != ir::OpCode::StoreUniform) {
            continue;
        }
        const auto uniform = inst.GetArg<ir::Uniform>(0);
        if (uniform.GetOffset() != kCarryOffset ||
            uniform.GetType() != ir::ValueType::U8) {
            continue;
        }
        auto value = inst.GetArg<ir::Value>(1);
        auto* def = value.Def();
        if (!def || def->GetOp() != ir::OpCode::LoadImm ||
            value.Type() != ir::ValueType::U8 || def->GetUses() != 1) {
            if (PerfGetenv("SVM_DUMP_IR")) {
                fmt::print(stderr,
                           "[backedge-proof] {:#x} reject polarity value def={} op={} type={} uses={}\n",
                           block->GetStartLocation().Value(),
                           def != nullptr,
                           def ? static_cast<u32>(def->GetOp()) : UINT32_MAX,
                           static_cast<u32>(value.Type()),
                           def ? def->GetUses() : UINT32_MAX);
            }
            return nullptr;
        }
        const u64 immediate = def->GetArg<ir::Imm>(0).Get();
        if (immediate > 1) {
            return nullptr;
        }
        polarity_store = &inst;
        polarity_load = def;
        polarity = static_cast<u8>(immediate);
    }
    if (!polarity_store || !polarity_load ||
        polarity_store->Id() >= condition.Def()->Id()) {
        if (PerfGetenv("SVM_DUMP_IR")) {
            fmt::print(stderr,
                       "[backedge-proof] {:#x} reject polarity store={} load={} cond={}\n",
                       block->GetStartLocation().Value(),
                       polarity_store ? polarity_store->Id() : UINT32_MAX,
                       polarity_load ? polarity_load->Id() : UINT32_MAX,
                       condition.Def()->Id());
        }
        return nullptr;
    }

    // The old block-entry flags stay live until the first producer. A fault is
    // allowed in that prefix; the per-block veneer below reconstructs them.
    for (auto& inst : block->GetInstList()) {
        if (&inst == first_producer) {
            break;
        }
        if (!PreservesHostNZCV(inst.GetOp())) {
            if (PerfGetenv("SVM_DUMP_IR")) {
                fmt::print(stderr,
                           "[backedge-proof] {:#x} reject pre-producer op={} id={}\n",
                           block->GetStartLocation().Value(),
                           static_cast<u32>(inst.GetOp()), inst.Id());
            }
            return nullptr;
        }
    }
    // Once the next producer overwrites host NZCV there may be no synchronous
    // fault or architectural observer before the terminal safepoint.
    for (auto& inst : block->GetInstList()) {
        if (inst.Id() > first_producer->Id() && MayFaultOrObserve(inst.GetOp())) {
            if (PerfGetenv("SVM_DUMP_IR")) {
                fmt::print(stderr,
                           "[backedge-proof] {:#x} reject post-producer fault op={} id={} producer={}\n",
                           block->GetStartLocation().Value(),
                           static_cast<u32>(inst.GetOp()), inst.Id(), first_producer->Id());
            }
            return nullptr;
        }
    }
    // The tail after the omitted store is intentionally tiny: advancing the
    // guest PC and consuming the already-live local condition only.
    ir::Inst* final_advance = nullptr;
    for (auto& inst : block->GetInstList()) {
        if (inst.Id() <= polarity_store->Id()) {
            continue;
        }
        if (inst.GetOp() != ir::OpCode::AdvancePC &&
            inst.GetOp() != ir::OpCode::LocalCondSet &&
            inst.GetOp() != ir::OpCode::ZeroExtend32 &&
            inst.GetOp() != ir::OpCode::ZeroExtend32To64 &&
            inst.GetOp() != ir::OpCode::SetHostGPR) {
            if (PerfGetenv("SVM_DUMP_IR")) {
                fmt::print(stderr,
                           "[backedge-proof] {:#x} reject tail op={} id={}\n",
                           block->GetStartLocation().Value(),
                           static_cast<u32>(inst.GetOp()), inst.Id());
            }
            return nullptr;
        }
        if (inst.GetOp() == ir::OpCode::AdvancePC) {
            if (final_advance) {
                return nullptr;
            }
            final_advance = &inst;
        }
    }
    if (!final_advance || final_advance->Id() >= condition.Def()->Id()) {
        return nullptr;
    }

    auto plan = std::make_unique<BackedgeFlagsPlan>();
    plan->self_is_then = then_self;
    plan->self_target = self;
    plan->cold_target = then_self ? *else_target : *then_target;
    plan->carry_inverted = polarity;
    plan->requested = GuestNZCVToHost(final_requested & ir::Flags::NZCV);
    plan->polarity_load = polarity_load;
    plan->polarity_store = polarity_store;
    plan->final_advance = final_advance;
    if (PerfGetenv("SVM_DUMP_IR")) {
        fmt::print(stderr, "[backedge-proof] {:#x} eligible\n",
                   block->GetStartLocation().Value());
    }
    return plan;
}

void JitTranslator::EmitBackedgeMaterialize(const BackedgeFlagsPlan& plan) {
    __ Mov(ipw1, plan.carry_inverted);
    __ Strb(ipw1,
            MemOperand(state,
                       state_offset_uniform_buffer +
                               offsetof(swift::x86::ThreadContext64,
                                        carry_inverted)));
    const u64 requested = static_cast<u64>(plan.requested);
    u64 keep = ~requested;
    __ Mrs(ip0, NZCV);
    __ And(flags, flags, ForceCast<s64>(keep));
    __ And(ip0, ip0, static_cast<u32>(requested));
    __ Orr(flags, flags, ip0);
}

bool JitTranslator::EmitBackedgeFlagsTerminal(const ir::Terminal& terminal) {
    if (!backedge_flags_plan) {
        return false;
    }
    auto& plan = *backedge_flags_plan;
    if (!save_in_nzcv || !nzcv_dirty || nzcv_requested != plan.requested) {
        if (PerfGetenv("SVM_DUMP_IR")) {
            fmt::print(stderr,
                       "[backedge-proof] {:#x} emitter fallback save={} dirty={} actual={:#x} expected={:#x}\n",
                       cur_block->GetStartLocation().Value(),
                       save_in_nzcv,
                       nzcv_dirty,
                       static_cast<u64>(nzcv_requested),
                       static_cast<u64>(plan.requested));
        }
        // Static proof and emitter state disagreed. Recreate the omitted
        // polarity write, commit through the ordinary path, and let the
        // generic terminal keep this block correct (but unoptimized).
        __ Mov(ipw1, plan.carry_inverted);
        __ Strb(ipw1,
                MemOperand(state,
                           state_offset_uniform_buffer +
                                   offsetof(swift::x86::ThreadContext64,
                                            carry_inverted)));
        MergeNZCV();
        plan.optimized = false;
        return false;
    }

    ir::Value condition{};
    VisitVariant<void>(terminal, [&](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::If>) {
            condition = term.cond;
        }
    });
    if (!condition.Def()) {
        return false;
    }
    if (auto local = LocalConditionFor(condition)) {
        const auto branch_to_cold = plan.self_is_then
                ? static_cast<Condition>(static_cast<u8>(*local) ^ 1)
                : *local;
        __ B(backedge_flags_plan->cold_exit.get(), branch_to_cold);
    } else if (plan.self_is_then) {
        __ Cbz(context.W(condition), backedge_flags_plan->cold_exit.get());
    } else {
        __ Cbnz(context.W(condition), backedge_flags_plan->cold_exit.get());
    }
    plan.cold_referenced = true;
    context.RecordExecCounter(exec_offset_exit_direct);
    backedge_exit_referenced = true;
    context.Forward(plan.self_target,
                    backedge_exit_label.get(),
                    plan.local_entry.get());
    return true;
}

void JitTranslator::EmitBackedgeColdPaths() {
    if (!backedge_flags_plan) {
        return;
    }
    auto& plan = *backedge_flags_plan;

    __ Bind(plan.external_entry.get());
    // All non-self entries begin with committed x26/State. Dispatcher lookup
    // clobbers NZCV. Normalize the committed carry representation to this
    // block's compile-time polarity before reconstructing host NZCV: a fault
    // before the first producer must see the same local ABI on an external
    // first iteration as it does after a self edge.
    Label polarity_ready;
    __ Ldrb(ipw0,
            MemOperand(state,
                       state_offset_uniform_buffer +
                               offsetof(swift::x86::ThreadContext64,
                                        carry_inverted)));
    __ Cmp(ipw0, plan.carry_inverted);
    __ B(&polarity_ready, eq);
    __ Eor(flags, flags, static_cast<u64>(HostFlags::C));
    __ Bind(&polarity_ready);
    __ Mov(ipw0, plan.carry_inverted);
    __ Strb(ipw0,
            MemOperand(state,
                       state_offset_uniform_buffer +
                               offsetof(swift::x86::ThreadContext64,
                                        carry_inverted)));
    __ And(ip0, flags, static_cast<u64>(HostFlags::NZCV));
    __ Msr(NZCV, ip0);
    __ B(plan.local_entry.get());

    if (plan.optimized && plan.cold_referenced) {
        __ Bind(plan.cold_exit.get());
        EmitBackedgeMaterialize(plan);
        context.RecordExecCounter(exec_offset_exit_direct);
        context.Forward(plan.cold_target);
    }

    u32 recovery_offset = 0;
    if (plan.optimized) {
        recovery_offset = context.CurrentBufferSize();
        __ Bind(plan.fault_recovery.get());
        EmitBackedgeMaterialize(plan);
        __ Ret();
    }
    backedge_block_metadata.push_back({cur_block->GetStartLocation().Value(),
                                       backedge_host_begin,
                                       backedge_host_end,
                                       recovery_offset});
    // The next emitted block always starts from the committed ABI. The local
    // state represented by this object has been materialized on every edge
    // that can reach it.
    nzcv_dirty = false;
    nzcv_requested = {};
    backedge_flags_plan.reset();
}

bool JitTranslator::IsSelfEdge(ir::Location target) const {
    return cur_block && target == cur_block->GetStartLocation();
}

bool JitTranslator::HasSelfEdge(const ir::Terminal& terminal) const {
    return VisitVariant<bool>(terminal, [this](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::LinkBlock> ||
                      std::is_same_v<T, ir::terminal::LinkBlockFast>) {
            return IsSelfEdge(term.next);
        } else if constexpr (std::is_same_v<T, ir::terminal::If> ||
                             std::is_same_v<T, ir::terminal::Condition>) {
            return HasSelfEdge(term.then_) || HasSelfEdge(term.else_);
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
            return HasSelfEdge(term.else_);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            return std::any_of(term.cases.begin(), term.cases.end(),
                               [this](const auto& item) {
                                   return HasSelfEdge(item.then);
                               });
        } else {
            return false;
        }
    });
}

void JitTranslator::EmitBackedgeExitStub() {
    if (!backedge_exit_label || !backedge_exit_referenced) {
        backedge_exit_label.reset();
        return;
    }
    Label signal;
    Label publish;
    __ Bind(backedge_exit_label.get());
    if (backedge_flags_plan && backedge_flags_plan->optimized) {
        EmitBackedgeMaterialize(*backedge_flags_plan);
    }
    __ Mov(ip1, cur_block->GetStartLocation().Value());
    __ Str(ip1, MemOperand(state, state_offset_current_loc));
    __ Tbnz(ip0, 63, &signal);
    __ Mov(ipw1, static_cast<u32>(HaltReason::CodeMiss));
    __ B(&publish);
    __ Bind(&signal);
    __ Mov(ipw1, static_cast<u32>(HaltReason::Signal));
    __ Bind(&publish);
    __ Str(ipw1, MemOperand(state, state_offset_halt_reason));
    __ Ret();
    backedge_exit_label.reset();
    backedge_exit_referenced = false;
}

void JitTranslator::EmitTerminal(const ir::Terminal& terminal) {
    VisitVariant<void>(terminal, [this](auto term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::Invalid>) {
            // Flat decoded blocks have no explicit terminal: the next location was
            // already written to state->current_loc by a SetLocation instruction.
            MergeNZCV();
            context.RecordExecCounter(static_next_loc ? exec_offset_exit_direct
                                                      : exec_offset_exit_indirect);
            if (!EmitStaticForward()) {
                __ Ret();
            }
        } else if constexpr (std::is_same_v<T, ir::terminal::ReturnToDispatch>) {
            MergeNZCV();
            context.RecordExecCounter(
                    cur_block_is_call ? exec_offset_exit_call
                                      : (static_next_loc ? exec_offset_exit_direct
                                                         : exec_offset_exit_indirect));
            if (!EmitStaticForward()) {
                __ Ret();
            }
        } else if constexpr (std::is_same_v<T, ir::terminal::ReturnToHost>) {
            MergeNZCV();
            context.RecordExecCounter(exec_offset_exit_syscall);
            __ Mov(ipw, static_cast<u32>(HaltReason::CallHost));
            __ Str(ipw, MemOperand(state, state_offset_halt_reason));
            __ Ret();
        } else if constexpr (std::is_same_v<T, ir::terminal::LinkBlock>) {
            MergeNZCV();
            context.RecordExecCounter(exec_offset_exit_direct);
            auto* exit = IsSelfEdge(term.next) && backedge_exit_label
                    ? backedge_exit_label.get()
                    : nullptr;
            backedge_exit_referenced |= exit != nullptr;
            auto* self_target = IsSelfEdge(term.next) && backedge_flags_plan
                    ? backedge_flags_plan->local_entry.get()
                    : nullptr;
            context.Forward(term.next, exit, self_target);
        } else if constexpr (std::is_same_v<T, ir::terminal::LinkBlockFast>) {
            MergeNZCV();
            context.RecordExecCounter(exec_offset_exit_direct);
            auto* exit = IsSelfEdge(term.next) && backedge_exit_label
                    ? backedge_exit_label.get()
                    : nullptr;
            backedge_exit_referenced |= exit != nullptr;
            auto* self_target = IsSelfEdge(term.next) && backedge_flags_plan
                    ? backedge_flags_plan->local_entry.get()
                    : nullptr;
            context.Forward(term.next, exit, self_target);
        } else if constexpr (std::is_same_v<T, ir::terminal::PopRSBHint>) {
            // Return Stack Buffer: this is the real pop+predict site. It must
            // run here (not at the PopRSB instruction) because guest flags have
            // just been committed by FlushFlags/MergeNZCV — the hit path
            // branches directly to the return target, which expects the flags
            // register to be current. EmitRSBPop ends in Br (hit) or Ret (miss/
            // underflow), so it fully terminates the block.
            MergeNZCV();
            context.RecordExecCounter(exec_offset_exit_ret);
            if (True(context.GetConfig().global_opts & Optimizations::ReturnStackBuffer)) {
                context.EmitRSBPop();
            } else {
                __ Ret();
            }
        } else if constexpr (std::is_same_v<T, ir::terminal::If>) {
            Label else_label;
            if (auto local = LocalConditionFor(term.cond)) {
                __ B(&else_label,
                     static_cast<Condition>(static_cast<u8>(*local) ^ 1));
            } else {
                __ Cbz(context.W(term.cond), &else_label);
            }
            EmitTerminal(term.then_);
            __ Bind(&else_label);
            EmitTerminal(term.else_);
        } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
            Label else_label;
            auto host_cond = MapCond(term.cond);
            if (!(save_in_nzcv && nzcv_dirty)) {
                LoadNZCVFromFlags();
            }
            __ B(&else_label, static_cast<Condition>(static_cast<u8>(host_cond) ^ 1));
            EmitTerminal(term.then_);
            __ Bind(&else_label);
            EmitTerminal(term.else_);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            // Linear compare chain; each arm ends with its own terminal.
            MergeNZCV();
            auto value = context.R(term.value);
            for (auto& case_ : term.cases) {
                Label next_case;
                __ Mov(ip, case_.case_value.Get());
                __ Cmp(value, ip);
                __ B(&next_case, ne);
                EmitTerminal(case_.then);
                __ Bind(&next_case);
            }
            // No case matched: bail out to the dispatcher.
            context.RecordExecCounter(exec_offset_exit_indirect);
            __ Ret();
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
            Label no_halt;
            __ Ldr(ipw, MemOperand(state, state_offset_halt_reason));
            __ Cbz(ipw, &no_halt);
            MergeNZCV();
            __ Ret();
            __ Bind(&no_halt);
            EmitTerminal(term.else_);
        } else {
            PANIC("Unknown terminal!");
        }
    });
}

std::optional<Condition> JitTranslator::LocalConditionFor(ir::Value value) const {
    if (!value.Def()) {
        return std::nullopt;
    }
    if (auto it = local_conditions.find(value.Def()); it != local_conditions.end()) {
        return it->second;
    }
    return std::nullopt;
}

bool JitTranslator::IsCompactFCmp(ir::Value value) {
    return value.Def() && value.Def()->GetOp() == ir::OpCode::VecFCmp &&
           value.Def()->GetArg<ir::Imm>(3).Get() != 0;
}

bool JitTranslator::RecordLocalCondition(ir::Inst* inst, ir::Cond cond) {
    if (inst->GetUses() != 1) {
        return false;
    }
    auto& list = cur_block->GetInstList();
    for (auto it = std::next(list.iterator_to(*inst)); it != list.end(); ++it) {
        bool names = false;
        for (auto value : it->GetValues()) {
            names = names || value.Def() == inst;
        }
        if (!names) {
            continue;
        }
        const bool supported =
                (it->GetOp() == ir::OpCode::Goto ||
                 it->GetOp() == ir::OpCode::NotGoto) &&
                        it->GetArg<ir::Value>(0).Def() == inst ||
                it->GetOp() == ir::OpCode::Select &&
                        it->GetArg<ir::Value>(0).Def() == inst;
        if (!supported) {
            return false;
        }
        local_conditions.emplace(inst, MapCond(cond));
        return true;
    }

    bool terminal_use = false;
    std::function<void(const ir::Terminal&)> visit = [&](const ir::Terminal& terminal) {
        VisitVariant<void>(terminal, [&](auto term) {
            using T = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<T, ir::terminal::If>) {
                if (term.cond.Def() == inst) {
                    terminal_use = true;
                }
                visit(term.then_);
                visit(term.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
                visit(term.then_);
                visit(term.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
                visit(term.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
                for (const auto& arm : term.cases) {
                    visit(arm.then);
                }
            }
        });
    };
    visit(cur_block->GetTerminal());
    if (terminal_use) {
        local_conditions.emplace(inst, MapCond(cond));
    }
    return terminal_use;
}

// A direct jmp/call decodes to SetLocation(imm) + ReturnToDispatcher, and the
// trampoline then re-reads state->current_loc and walks the L1 hash chain for
// a target that was already known when the code was emitted. The dispatch
// table indexed here is the same one the RSB pop and JitContext::Forward's
// BlockLink path already branch through, with the same safety property: SMC
// invalidation (SmcTracker::ClearDispatchSlots) zeroes the slot, so a stale
// translation degrades to the Cbz fallback rather than to a wild branch.
bool JitTranslator::EmitStaticForward() {
    if (!static_next_loc) {
        return false;
    }
    const u64 target = *static_next_loc;
    static_next_loc.reset();
    return context.ForwardStatic(ir::Location{target});
}

Label* JitTranslator::GetLocalLabel(ir::Inst* inst) {
    if (auto itr = local_labels.find(inst); itr != local_labels.end()) {
        return &itr->second;
    }
    return &local_labels.try_emplace(inst).first->second;
}

HostFlags JitTranslator::GuestNZCVToHost(ir::Flags guest) {
    HostFlags host{};
    if (True(guest & ir::Flags::Negate)) {
        host |= HostFlags::N;
    }
    if (True(guest & ir::Flags::Zero)) {
        host |= HostFlags::Z;
    }
    if (True(guest & ir::Flags::Carry)) {
        host |= HostFlags::C;
    }
    if (True(guest & ir::Flags::Overflow)) {
        host |= HostFlags::V;
    }
    return host;
}

Condition JitTranslator::MapCond(ir::Cond cond) {
    // ir::Cond values match the ARM condition encoding.
    return static_cast<Condition>(static_cast<u8>(cond) & 0xF);
}





Register JitTranslator::MaterializeOperand(const Operand& operand, ir::ValueType type) {
    auto tmp = context.GetTmpGPR(type);
    __ Mov(tmp, operand);
    return tmp;
}

void JitTranslator::Translate(ir::Inst* inst) {
    ASSERT(inst);
    context.TickIR(inst);
    if (inst->GetOp() != ir::OpCode::SetLocation) {
        static_next_loc.reset();
    }

#define INST(name, ...)                                                                            \
    case ir::OpCode::name:                                                                         \
        Emit##name(inst);                                                                          \
        break;

    switch (inst->GetOp()) {
#include "runtime/ir/ir.inc"
        default:
            ASSERT_MSG(false, "Instr unk op: {}", inst->GetOp());
    }

#undef INST
    context.EndInstructionScratch();
}

bool JitTranslator::MatchMemoryOffsetCase(ir::Inst* inst) { return false; }
































Operand JitTranslator::EmitOperand(ir::Operand& ir_op) {
    if (ir_op.GetRight().Null()) {
        if (ir_op.GetLeft().IsImm()) {
            auto imm = ir_op.GetLeft().imm.Get();
            auto imm_signed = ir_op.GetLeft().imm.GetSigned();
            bool can_imm = __ IsImmAddSub(imm_signed);
            if (can_imm) {
                return Operand{imm_signed};
            } else {
                auto tmp = context.GetTmpX();
                __ Mov(tmp, imm);
                return Operand{tmp};
            }
        } else {
            return Operand{context.R(ir_op.GetLeft().value, true)};
        }
    } else {
        Register left_reg;
        ir::ValueType left_type{ir::ValueType::U64};
        if (ir_op.GetLeft().IsImm()) {
            // Materialize an immediate left side (constant-based composite
            // operand) into a scratch register first.
            auto tmp = context.GetTmpX();
            __ Mov(tmp, ir_op.GetLeft().imm.Get());
            left_reg = tmp;
        } else {
            auto left_value = ir_op.GetLeft().value;
            left_type = left_value.Type();
            left_reg = context.R(left_value, true);
        }
        auto right = ir_op.GetRight();
        if (right.IsImm()) {
            auto imm = right.imm.GetSigned();
            auto is_lsl = ir_op.GetOp() == ir::OperandOp::LSL;
            auto is_lsr = ir_op.GetOp() == ir::OperandOp::LSR;
            if (is_lsl || is_lsr) {
                if ((left_reg.Is64Bits() || (imm < kWRegSize)) || (left_reg.Is32Bits() || (imm < kXRegSize))) {
                    return Operand{left_reg, is_lsl ? LSL : LSR, static_cast<u8>(imm)};
                } else {
                    PANIC();
                }
            } else if (ir_op.GetOp() == ir::OperandOp::Plus) {
                auto tmp = context.GetTmpGPR(left_type);
                bool can_imm = __ IsImmAddSub(imm);
                if (can_imm) {
                    __ Add(tmp, left_reg, imm);
                } else {
                    __ Mov(tmp, imm);
                    __ Add(tmp, left_reg, tmp);
                }
                return Operand{tmp};
            } else {
                PANIC();
            }
        } else {
            auto right_reg = context.R(right.value, true);
            auto tmp = context.GetTmpGPR(left_type);
            if (ir_op.GetOp() == ir::OperandOp::Plus) {
                __ Add(tmp, left_reg, right_reg);
                return Operand{tmp};
            } else if (ir_op.GetOp() == ir::OperandOp::LSL) {
                __ Lsl(tmp, left_reg, right_reg);
                return Operand{tmp};
            } else if (ir_op.GetOp() == ir::OperandOp::LSR) {
                __ Lsr(tmp, left_reg, right_reg);
                return Operand{tmp};
            } else if (ir_op.GetOp() == ir::OperandOp::PlusExt) {
                auto shift_amount = ir_op.GetOp().shift_ext;
                ASSERT(right_reg.Is64Bits() || (shift_amount < kWRegSize));
                ASSERT(right_reg.Is32Bits() || (shift_amount < kXRegSize));
                __ Add(tmp, left_reg, Operand{right_reg, LSL, shift_amount});
                return Operand{tmp};
            } else {
                PANIC();
            }
        }
        return {};
    }
}

MemOperand JitTranslator::EmitMemOperand(ir::Operand& ir_op,
                                         ir::ValueType type,
                                         bool pair,
                                         bool atomic,
                                         bool allow_writeback,
                                         bool structured_guest_ea) {
    auto access_size = ir::GetValueSizeByte(type);
    if (ir_op.GetRight().Null()) {
        if (ir_op.GetLeft().IsImm()) {
            auto imm = ir_op.GetLeft().imm.Get();
            auto imm_signed = ir_op.GetLeft().imm.GetSigned();
            if (use_memory_base) {
                // Absolute guest address: materialize it, then apply the pt
                // bias (guest addr + pt = host addr). With a bounded guest
                // window the truncation happens at translation time — the
                // immediate is a compile-time constant, so it is free.
                __ Mov(mem_scratch, guest_addr_mask ? (imm & guest_addr_mask) : imm);
                if (atomic) {
                    __ Add(mem_scratch, mem_scratch, pt);
                    return MemOperand{mem_scratch};
                }
                return MemOperand{mem_scratch, pt};
            }
            bool can_imm = pair ? __ IsImmLSPair(imm_signed, access_size) : __ IsImmLSUnscaled(imm_signed);
            if (can_imm) {
                return MemOperand{xzr, imm_signed};
            } else {
                auto tmp = context.GetTmpX();
                __ Mov(tmp, imm);
                return MemOperand{tmp};
            }
        } else {
            // Match Case: load store post/index & push/pop
            auto addr_value = ir_op.GetLeft().value;
            if (mem_narrow_fuse && addr_value.Def()->GetOp() == ir::OpCode::GetOperand &&
                addr_value.Def()->GetUses() == 1) {
                auto source_operand = addr_value.Def()->GetArg<ir::Operand>(0);
                auto source_left = source_operand.GetLeft();
                if (source_operand.GetRight().Null() && source_left.IsValue() &&
                    context.SharesGPR(addr_value, source_left.value)) {
                    // A simple EA does not need to be materialised in the
                    // GetOperand result register. The RA tie makes the result
                    // own that same register through the memory use, so
                    // consume the live result allocation here rather than
                    // extending the source SSA's lifetime in the emitter.
                    disable_instructions.set(addr_value.Def()->Id());
                    auto address_reg = context.R(addr_value, true);
                    if (use_memory_base) {
                        return BiasMem(address_reg, atomic);
                    }
                    return MemOperand{address_reg};
                }
            }
            auto& instr_list = cur_block->GetInstList();
            auto instr = addr_value.Def();
            // With the pt bias active, post-index forms cannot express
            // [base + pt] (+writeback), so the folding is disabled and the
            // address update executes as a normal Add/Sub.
            if (allow_writeback && !use_memory_base && addr_value.Def()->GetUses() == 2) {
                int search_times{0};
                for (auto itr = instr_list.iterator_to(*instr);
                     itr != instr_list.end() && search_times < 3;
                     itr++, search_times++) {
                    auto add_sub =
                            itr->GetOp() == ir::OpCode::Add || itr->GetOp() == ir::OpCode::Sub;
                    if (!add_sub) {
                        continue;
                    }
                    auto same_value = itr->GetArg<ir::Value>(0) == addr_value;
                    if (!same_value) {
                        continue;
                    }
                    auto operand = itr->GetArg<ir::Operand>(1);
                    auto no_right = operand.GetRight().Null();
                    if (!no_right) {
                        continue;
                    }
                    auto same_register = context.R(addr_value) == context.R(itr.operator->());
                    if (!same_register) {
                        continue;
                    }
                    auto left = operand.GetLeft();
                    if (left.IsImm()) {
                        auto imm = left.imm.GetSigned();
                        if (!pair && !__ IsImmLSUnscaled(imm)) {
                            continue;
                        }
                        if (pair && !__ IsImmLSPair(imm, access_size)) {
                            continue;
                        }
                        if (itr->GetOp() == ir::OpCode::Add) {
                            disable_instructions.set(itr->Id());
                            return MemOperand{context.R(addr_value), imm, PostIndex};
                        } else {
                            disable_instructions.set(itr->Id());
                            return MemOperand{context.R(addr_value), -imm, PostIndex};
                        }
                    } else {
                        if (itr->GetOp() == ir::OpCode::Add) {
                            disable_instructions.set(itr->Id());
                            return MemOperand{
                                    context.R(addr_value), context.R(left.value), PostIndex};
                        }
                    }
                }
            }
            if (use_memory_base) {
                return BiasMem(context.R(addr_value), atomic);
            }
            return MemOperand{context.R(addr_value)};
        }
    } else {
        Register left_reg;
        if (ir_op.GetLeft().IsImm()) {
            // Materialize an immediate left side (absolute address + offset
            // forms) into a scratch register first.
            auto tmp = context.GetTmpX();
            __ Mov(tmp, ir_op.GetLeft().imm.Get());
            left_reg = tmp;
        } else {
            left_reg = context.R(ir_op.GetLeft().value, true);
        }
        auto right = ir_op.GetRight();
        if (right.IsImm()) {
            auto imm = right.imm.GetSigned();
            bool can_imm = pair ? __ IsImmLSPair(imm, access_size) : __ IsImmLSUnscaled(imm);
            if (can_imm) {
                if (ir_op.GetOp() == ir::OperandOp::Plus) {
                    if (use_memory_base) {
                        return BiasMem(left_reg, imm, atomic);
                    }
                    return MemOperand{left_reg, imm};
                } else if (ir_op.GetOp() == ir::OperandOp::LSL) {
                    if (use_memory_base) {
                        __ Lsl(mem_scratch, left_reg, imm);
                        return BiasMem(mem_scratch, atomic);
                    }
                    auto tmp = context.GetTmpX();
                    __ Lsl(tmp, left_reg, imm);
                    return MemOperand{tmp};
                } else if (ir_op.GetOp() == ir::OperandOp::LSR) {
                    if (use_memory_base) {
                        __ Lsr(mem_scratch, left_reg, imm);
                        return BiasMem(mem_scratch, atomic);
                    }
                    auto tmp = context.GetTmpX();
                    __ Lsr(tmp, left_reg, imm);
                    return MemOperand{tmp};
                } else {
                    PANIC();
                }
            } else {
                if (use_memory_base) {
                    __ Mov(mem_scratch, imm);
                    if (ir_op.GetOp() == ir::OperandOp::Plus) {
                        __ Add(mem_scratch, left_reg, mem_scratch);
                    } else if (ir_op.GetOp() == ir::OperandOp::LSL) {
                        __ Lsl(mem_scratch, left_reg, mem_scratch);
                    } else if (ir_op.GetOp() == ir::OperandOp::LSR) {
                        __ Lsr(mem_scratch, left_reg, mem_scratch);
                    } else {
                        PANIC();
                    }
                    return BiasMem(mem_scratch, atomic);
                }
                auto tmp = context.GetTmpX();
                __ Mov(tmp, imm);
                if (ir_op.GetOp() == ir::OperandOp::Plus) {
                    return MemOperand{left_reg, tmp};
                } else if (ir_op.GetOp() == ir::OperandOp::LSL) {
                    return MemOperand{left_reg, tmp, LSL};
                } else if (ir_op.GetOp() == ir::OperandOp::LSR) {
                    return MemOperand{left_reg, tmp, LSR};
                } else {
                    PANIC();
                }
            }
        } else {
            auto right_reg = context.R(right.value, true);
            if (ir_op.GetOp() == ir::OperandOp::Plus) {
                if (use_memory_base) {
                    if (structured_guest_ea && window_uxtw) {
                        // Compute the guest EA in W form before applying the
                        // host bias: pt + ((base + index) mod 2^32).
                        // This deliberately is not a prebiased base.
                        __ Add(mem_scratch.W(), left_reg.W(), right_reg.W());
                    } else {
                        __ Add(mem_scratch, left_reg, right_reg);
                    }
                    return BiasMem(mem_scratch, atomic);
                }
                return MemOperand{left_reg, right_reg};
            } else if (ir_op.GetOp() == ir::OperandOp::LSL) {
                if (use_memory_base) {
                    __ Lsl(mem_scratch, left_reg, right_reg);
                    return BiasMem(mem_scratch, atomic);
                }
                return MemOperand{left_reg, right_reg, LSL};
            } else if (ir_op.GetOp() == ir::OperandOp::LSR) {
                if (use_memory_base) {
                    __ Lsr(mem_scratch, left_reg, right_reg);
                    return BiasMem(mem_scratch, atomic);
                }
                return MemOperand{left_reg, right_reg, LSR};
            } else if (ir_op.GetOp() == ir::OperandOp::PlusExt) {
                auto shift_amount = ir_op.GetOp().shift_ext;
                if (structured_guest_ea && use_memory_base) {
                    if (window_uxtw) {
                        // Keep base/index/scale in one wrapping W add.
                        // BiasMem supplies the final pt + Wguest, UXTW step.
                        __ Add(mem_scratch.W(),
                               left_reg.W(),
                               Operand{right_reg.W(), LSL, shift_amount});
                    } else {
                        __ Add(mem_scratch, left_reg, Operand{right_reg, LSL, shift_amount});
                    }
                    return BiasMem(mem_scratch, atomic);
                }
                if (ir::GetValueSizeByte(right.value.Type()) == shift_amount) {
                    if (use_memory_base) {
                        __ Add(mem_scratch,
                               left_reg,
                               Operand{right_reg, LSL, shift_amount});
                        return BiasMem(mem_scratch, atomic);
                    }
                    return MemOperand{left_reg, right_reg, LSL, shift_amount};
                } else {
                    if (use_memory_base) {
                        __ Lsl(mem_scratch, right_reg, shift_amount);
                        __ Add(mem_scratch, left_reg, mem_scratch);
                        return BiasMem(mem_scratch, atomic);
                    }
                    auto tmp = context.GetTmpX();
                    __ Lsl(tmp, right_reg, shift_amount);
                    return MemOperand{left_reg, tmp};
                }
            } else {
                PANIC();
            }
        }
        return {};
    }
}




























































#undef masm

}  // namespace swift::runtime::backend::arm64
