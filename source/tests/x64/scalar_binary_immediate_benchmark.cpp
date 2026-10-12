#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <vector>
#include <unistd.h>
#include <xmmintrin.h>
#include "runtime/backend/x64/translator.h"
#include "runtime/backend/x64/uniform_forward.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#include "runtime/frontend/arm64/state_ranges.h"
#include "runtime/frontend/ir_assembler.h"
#include "runtime/ir/opts/const_folding_pass.h"
#include "runtime/ir/opts/deadcode_elimination_pass.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
using Context = swift::arm64::ThreadContext64;
using Clock = std::chrono::steady_clock;
using Fn = HaltReason (*)(backend::State*);

namespace {
constexpr u64 code_base = 0x1000, nzcv_mask = u64{15} << 28;
constexpr u32 constant_registers = 24, route_count = 4;
constexpr const char* scope = "decoded_a64_scalar_binary_immediate_cfg_pair";
enum class Kind { MovZ, MovN, MovK, Add, Sub, Adc, Sbc, And, Or, Xor, AndNot, AddImm, SubImm, AndImm, OrImm, XorImm };
struct Operation {
    Kind kind;
    u32 dst{}, lhs{}, rhs{}, immediate{}, shift{};
    bool wide{true}, flags{};
};
struct Program final : MemoryInterface {
    std::string name;
    std::vector<u32> words;
    std::vector<Operation> operations;
    u64 StopPc() const { return code_base + (words.size() - 1) * 4; }
    bool Read(void* out, size_t address, size_t size) override {
        if (address < code_base || address - code_base > words.size() * 4 ||
            size > words.size() * 4 - (address - code_base)) return false;
        std::memcpy(out, reinterpret_cast<const u8*>(words.data()) + address - code_base, size); return true;
    }
    bool Write(void*, size_t, size_t) override { return false; }
    void* GetPointer(void* address) override {
        const auto at = reinterpret_cast<uintptr_t>(address);
        if (at < code_base || at - code_base > words.size() * 4 || words.size() * 4 - (at - code_base) < 4)
            return nullptr;
        return reinterpret_cast<u8*>(words.data()) + at - code_base;
    }
    void Emit(Operation op) {
        u32 word{};
        if (op.kind == Kind::MovZ || op.kind == Kind::MovN || op.kind == Kind::MovK) {
            word = op.kind == Kind::MovZ ? 0x52800000u : op.kind == Kind::MovN ? 0x12800000u : 0x72800000u;
            word |= ((op.shift / 16) << 21) | (op.immediate << 5) | op.dst;
        } else if (op.kind == Kind::AddImm || op.kind == Kind::SubImm) {
            word = op.kind == Kind::SubImm ? 0x51000000u : 0x11000000u;
            if (op.flags) word |= 0x20000000u;
            word |= (op.shift == 12 ? 1u << 22 : 0) | (op.immediate << 10) | (op.lhs << 5) | op.dst;
        } else if (op.kind == Kind::AndImm || op.kind == Kind::OrImm || op.kind == Kind::XorImm) {
            // One full-width element of contiguous ones, rotated right.
            // N selects 64 bits; 32-bit imms has bit5 clear (element size32).
            word = op.kind == Kind::AndImm ? 0x12000000u : op.kind == Kind::OrImm ? 0x32000000u : 0x52000000u;
            word |= (op.wide ? 1u << 22 : 0) | (op.shift << 16) |
                    ((op.immediate - 1) << 10) | (op.lhs << 5) | op.dst;
        } else {
            switch (op.kind) {
                case Kind::Add: word = 0x0b000000u; break;
                case Kind::Sub: word = 0x4b000000u; break;
                case Kind::Adc: word = 0x1a000000u; break;
                case Kind::Sbc: word = 0x5a000000u; break;
                case Kind::And: word = 0x0a000000u; break;
                case Kind::Or: word = 0x2a000000u; break;
                case Kind::Xor: word = 0x4a000000u; break;
                case Kind::AndNot: word = 0x0a200000u; break;
                default: throw std::runtime_error("uncovered A64 assembler descriptor");
            }
            if (op.flags) word |= 0x20000000u;
            word |= (op.rhs << 16) | (op.lhs << 5) | op.dst;
        }
        if (op.wide) word |= 0x80000000u;
        words.push_back(word); operations.push_back(op);
    }
    void Finish() {
        words.push_back(0xd1000000u | (1u << 10) | (28u << 5) | 28u); // SUB X28,X28,#1
        const u32 displacement = (u32{0} - u32(words.size())) & 0x7ffff;
        words.push_back(0xb5000000u | (displacement << 5) | 28u); // CBNZ X28,root
        words.push_back(0xd4200000u); // BRK #0
    }
};
Program MakeProgram(bool mixed) {
    Program program; program.name = mixed ? "mixed_binary_immediate_loop" : "wide_binary_immediate_loop";
    // Define every register first, then revisit them with MOVK. The real
    // decoder's initial LoadImm values are simultaneously live at the later
    // forwarded BitExtract reads, even with the normal immediate-fold pass.
    // Unique bits also keep the normal bounded constant CSE enabled.
    for (u32 i = 0; i < constant_registers; ++i) {
        const bool wide = !mixed || i % 3 != 0;
        program.Emit({i & 1 ? Kind::MovN : Kind::MovZ, i, 0, 0, 0x91abu + i * 31,
                      (i % (wide ? 4 : 2)) * 16, wide});
    }
    for (u32 round = 1; round <= 2; ++round) for (u32 i = 0; i < constant_registers; ++i) {
        const bool wide = !mixed || i % 3 != 0;
        program.Emit({Kind::MovK, i, 0, 0, 0x3571u + i * 43 + round * 97,
                      ((i + round) % (wide ? 4 : 2)) * 16, wide});
    }
    // Real immediate arithmetic and logical words preserve four variable
    // accumulators across iterations; the preceding move-wide constants
    // keep allocation/rematerialization pressure identical in both routes.
    for (u32 i = 0; i < constant_registers; ++i) {
        const u32 acc = 24 + i % 4, other = 24 + (i + 1) % 4;
        const bool wide = !mixed || bool(i & 1);
        program.Emit({Kind::AddImm, acc, acc, 0, 17 + i * 31, i % 3 == 0 ? 12u : 0u, wide, true});
        program.Emit({Kind::SubImm, other, other, 0, 29 + i * 19, i % 5 == 0 ? 12u : 0u, wide, true});
        // Low8 and complement-low8 fit x86 sign-extended imm32. Low32 in a
        // 64-bit logical operation deliberately exercises the MOV fallback.
        const u32 bits = wide ? 64 : 32;
        const u32 ones = i % 3 == 0 ? 8 : i % 3 == 1 ? bits - 8 : bits / 2;
        const u32 rotation = i % 3 == 1 ? bits - 8 : 0;
        program.Emit({Kind::AndImm, acc, acc, 0, ones, rotation, wide});
        program.Emit({Kind::OrImm, other, other, 0, ones, rotation, wide});
        program.Emit({Kind::XorImm, 27, 27, 0, ones, rotation, wide});
        program.Emit({Kind::Add, acc, acc, i, 0, 0, wide, true});
        program.Emit({Kind::Adc, other, other, i, 0, 0, wide, true});
        program.Emit({Kind::Sub, 26, 26, i, 0, 0, wide, true});
        program.Emit({Kind::Sbc, 27, 27, i, 0, 0, wide, true});
        program.Emit({Kind::AndNot, acc, acc, i, 0, 0, wide});
        program.Emit({Kind::Or, other, other, i, 0, 0, wide});
        program.Emit({Kind::Xor, 26, 26, i, 0, 0, wide});
    }
    program.Finish(); return program;
}
struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + sizeof(Context)> bytes{};
    backend::State* state = new(bytes.data()) backend::State{};
    ~Machine() { state->~State(); }
    Context& Guest() { return *reinterpret_cast<Context*>(state->uniform_buffer_begin); }
    void Init(u64 seed, u32 trips) {
        auto* guest = reinterpret_cast<u8*>(&Guest());
        for (u32 i = 0; i < sizeof(Context); ++i) guest[i] = u8(seed + i * 37 + 0x59);
        for (u32 i = 0; i < 29; ++i) Guest().r[i] = seed ^ (0x9e3779b97f4a7c15ULL * (i + 1));
        Guest().r[28] = trips; Guest().fp = seed ^ 0x3141592653589793ULL;
        Guest().lr = ~seed; Guest().sp = 0x8000;
        state->host_cpu_flags = ((seed ^ 0xabcd321001234567ULL) & ~nzcv_mask) | (((seed >> 4) & 15) << 28);
        state->current_loc = ir::Location{0xfeedface}; state->halt_reason = HaltReason::None;
    }
};
u64 ArithmeticFlags(u64 old, u64 lhs, u64 rhs, u32 bits, u64 carry, bool subtract) {
    const u64 mask = bits == 64 ? UINT64_MAX : UINT32_MAX; lhs &= mask; rhs &= mask;
    const auto sum = static_cast<unsigned __int128>(lhs) + (subtract ? (~rhs & mask) : rhs) + carry;
    const u64 result = u64(sum) & mask;
    const auto signed_value = [bits](u64 value) -> __int128 {
        return value & (u64{1} << (bits - 1)) ? __int128(value) - (__int128{1} << bits) : __int128(value);
    };
    const auto mathematical = subtract ? signed_value(lhs) - signed_value(rhs) - (1 - carry)
                                       : signed_value(lhs) + signed_value(rhs) + carry;
    const bool overflow = mathematical < -(__int128{1} << (bits - 1)) || mathematical >= (__int128{1} << (bits - 1));
    return (old & ~nzcv_mask) | (((result >> (bits - 1)) & 1) << 31) | (u64(result == 0) << 30) |
           (u64(bool(sum >> bits)) << 29) | (u64(overflow) << 28);
}
void Model(const Program& program, Machine& machine, u32 trips) {
    auto& regs = machine.Guest().r; auto& flags = machine.state->host_cpu_flags;
    // Descriptors are assembler inputs. The oracle reads no decoded IR,
    // allocation, backend result, or rematerialization decision.
    for (u32 trip = 0; trip < trips; ++trip) {
        for (const auto& op : program.operations) {
            const u64 mask = op.wide ? UINT64_MAX : UINT32_MAX;
            const u64 piece = u64(op.immediate) << op.shift;
            if (op.kind == Kind::MovZ) regs[op.dst] = piece & mask;
            else if (op.kind == Kind::MovN) regs[op.dst] = ~piece & mask;
            else if (op.kind == Kind::MovK)
                regs[op.dst] = ((regs[op.dst] & ~(u64{0xffff} << op.shift)) | piece) & mask;
            else {
                const u64 lhs = regs[op.lhs] & mask;
                u64 rhs = regs[op.rhs] & mask;
                const bool logical_imm = op.kind == Kind::AndImm || op.kind == Kind::OrImm || op.kind == Kind::XorImm;
                if (op.kind == Kind::AddImm || op.kind == Kind::SubImm) rhs = u64(op.immediate) << op.shift;
                else if (logical_imm) {
                    const u32 bits = op.wide ? 64 : 32;
                    const u64 ones = (u64{1} << op.immediate) - 1;
                    rhs = (op.shift ? (ones >> op.shift) | (ones << (bits - op.shift)) : ones) & mask;
                }
                const bool subtract = op.kind == Kind::Sub || op.kind == Kind::SubImm || op.kind == Kind::Sbc;
                const u64 carry = op.kind == Kind::Adc || op.kind == Kind::Sbc ? (flags >> 29) & 1 : subtract ? 1 : 0;
                if (op.kind == Kind::And || op.kind == Kind::AndImm) regs[op.dst] = lhs & rhs;
                else if (op.kind == Kind::AndNot) regs[op.dst] = lhs & ~rhs & mask;
                else if (op.kind == Kind::Or || op.kind == Kind::OrImm) regs[op.dst] = lhs | rhs;
                else if (op.kind == Kind::Xor || op.kind == Kind::XorImm) regs[op.dst] = lhs ^ rhs;
                else regs[op.dst] = (subtract ? lhs - rhs - (1 - carry) : lhs + rhs + carry) & mask;
                if (op.flags) flags = ArithmeticFlags(flags, lhs, rhs, op.wide ? 64 : 32, carry, subtract);
            }
        }
        --regs[28];
    }
    machine.Guest().padding = u32(swift::arm64::InterruptReason::BRK);
    machine.state->current_loc = ir::Location{program.StopPc()};
    // FunctionPlan commits the CBNZ fallthrough edge to the BRK block on
    // every exit, including a single iteration. prev_loc is that edge's
    // source instruction, rather than the function entry or last backedge.
    machine.state->prev_loc = ir::Location{program.StopPc() - 4};
    machine.state->halt_reason = HaltReason::CallHost;
}
bool PlainScalarLoadImm(const ir::Inst& inst) {
    if (inst.GetOp() != ir::OpCode::LoadImm || inst.ReturnType() < ir::ValueType::U8 ||
        inst.ReturnType() > ir::ValueType::S64 || !inst.ArgAt(0).IsImm()) return false;
    for (u32 slot = 1; slot < ir::Inst::max_args; ++slot) if (!inst.ArgAt(slot).IsVoid()) return false;
    return true;
}
u32 PlainCount(const ir::Block* body) {
    u32 count{}; for (const auto& inst : body->GetInstList()) count += PlainScalarLoadImm(inst); return count;
}
struct Compiled {
    Config config{.backend_isa = kX86_64, .uniform_buffer_size = sizeof(Context),
                  .loop_gpr_uniform_ranges = swift::arm64::Arm64GprUniformRanges(), .stack_alignment = 16};
    backend::FunctionPlan plan;
    std::unique_ptr<x64::Translator> code;
    u32 route{}, guest_blocks{}, decoded_plain_loadimm{}, optimized_plain_loadimm{}, eligible_unassigned{};
    Fn Entry() const { return code->getCode<Fn>(); }
};
std::unique_ptr<Compiled> Compile(Program& program, u32 route) {
    auto unit = std::make_unique<Compiled>();
    unit->route = route;
    ir::HIRBuilder builder{1, true}; auto* function = builder.AppendFunction(ir::Location{code_base});
    for (;;) {
        std::vector<u64> pending;
        for (auto& hir : function->GetHIRBlockList()) {
            auto* block = hir.GetBlock();
            if (block->GetInstList().empty() && !block->HasTerminal()) pending.push_back(block->GetStartLocation().Value());
        }
        if (pending.empty()) break;
        if (unit->guest_blocks + pending.size() > 32) throw std::runtime_error("move-wide benchmark HIR block cap");
        for (auto address : pending) {
            builder.SetCurBlock(address); ir::Assembler as{&builder};
            swift::arm64::A64Decoder{address, &program, &as}.Decode(); ++unit->guest_blocks;
        }
    }
    function->EndFunction();
    std::vector<ir::HIRBlock*> ordered;
    for (auto* hir : function->GetHIRBlocks())
        if (hir && hir->GetBlock()->GetStartLocation() == ir::Location{code_base}) ordered.push_back(hir);
    for (auto* hir : function->GetHIRBlocks())
        if (hir && hir->GetBlock()->GetStartLocation() != ir::Location{code_base} &&
            hir->GetBlock()->GetStartLocation() != ir::Location::INVALID && hir->GetBlock()->HasTerminal()) ordered.push_back(hir);
    const auto features = GetSvmConfig().GetFeatureSet();
    for (auto* hir : ordered) {
        auto* body = hir->GetBlock(); unit->decoded_plain_loadimm += PlainCount(body);
        x64::ForwardUniformLoads(body, sizeof(Context));
        ir::ConstFoldingPass::Run(body, features); ir::DeadCodeEliminationPass::Run(body, function);
        unit->optimized_plain_loadimm += PlainCount(body);
    }
    unit->plan = backend::FunctionPlan::Build(function, ordered);
    unit->code = x64::Translator::PrepareFunction(unit->plan, x64::HostFeatures::Detect(), &unit->config,
        true, true, bool(route & 2), {.uniform_forwarding = true, .state_promotion = true,
                                    .scalar_rematerialization = true, .scalar_binary_immediates = bool(route & 1)});
    if (!unit->code->StatePromotionStats().accepted || !unit->code->UniformForwardStats().accepted)
        throw std::runtime_error("move-wide benchmark production CFG preparation rejected");
    unit->code->Translate();
    const auto& cfg = unit->code->CfgPlan(); const auto& frame = unit->code->FramePlan();
    if (!cfg.Enabled() || !cfg.Validate() || !frame.Enabled() || unit->guest_blocks < 2 || unit->plan.targets.empty())
        throw std::runtime_error("move-wide benchmark did not produce a validated shared guest loop");
    std::unordered_set<const ir::Inst*> reachable;
    for (const auto& block : cfg.Blocks()) if (block.reachable)
        for (u32 at = block.begin; at < block.end; ++at) reachable.insert(cfg.Instructions().at(at));
    for (const auto& allocation : cfg.Allocations()) {
        const auto* inst = allocation.producer;
        if (!PlainScalarLoadImm(*inst) || allocation.slot || !allocation.uses || allocation.requires_full_container ||
            !reachable.contains(inst)) continue;
        ++unit->eligible_unassigned;
        if (!cfg.RequiresCanonicalHome(inst)) throw std::runtime_error("unassigned constant allocation lacks canonical obligation");
        const auto rematerialized = frame.RematerializedValue(inst);
        if (!rematerialized || frame.Home(inst).has_value())
            throw std::runtime_error("decoded eligible constant does not follow the requested rematerialization route");
        const u32 bits = ir::GetValueSizeByte(inst->ReturnType()) * 8;
        const u64 mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
        if (rematerialized && *rematerialized != (inst->GetArg<ir::Imm>(0).Get() & mask))
            throw std::runtime_error("decoded constant rematerialization bits mismatch");
    }
    const auto& stats = unit->code->Statistics();
    if (unit->decoded_plain_loadimm < constant_registers || !unit->eligible_unassigned)
        throw std::runtime_error("real decoded optimized workload has no unassigned eligible LoadImm pressure");
    if (frame.Statistics().omitted_rematerialized != unit->eligible_unassigned ||
        stats.scalar_rematerialized_definition_sites != unit->eligible_unassigned || !stats.scalar_rematerialized_read_sites)
        throw std::runtime_error("decoded workload lacks fixed ON rematerialization proof");
    if (unit->code->ScalarBinaryImmediatesEnabled() != bool(route & 1) ||
        (route & 1 ? !stats.scalar_binary_immediate_sites : bool(stats.scalar_binary_immediate_sites || stats.scalar_binary_rematerialized_immediate_sites)))
        throw std::runtime_error("decoded workload lacks requested actual binary immediate emission");
    return unit;
}
std::runtime_error ModelMismatch(const Program& program, const Compiled& unit, u64 seed, u32 trips,
                                Machine& expected, Machine& actual, HaltReason reason,
                                u32 expected_mxcsr, u32 actual_mxcsr, const char* stage) {
    std::ostringstream out;
    out << program.name << " full ARM context and State independent-model mismatch"
        << " stage=" << stage << " seed=" << std::hex << seed << std::dec << " trips=" << trips
        << " route=" << unit.route << " scalar_binary_immediates=" << bool(unit.route & 1)
        << " flags_residency=" << bool(unit.route & 2)
        << " reason=" << static_cast<u32>(reason) << " expected_reason=" << static_cast<u32>(HaltReason::CallHost)
        << " mxcsr_actual=" << std::hex << actual_mxcsr << " mxcsr_expected=" << expected_mxcsr << '\n';
    const auto bytes = [&](const char* name, u32 offset, u32 size) {
        if (!std::memcmp(actual.bytes.data() + offset, expected.bytes.data() + offset, size)) return;
        out << "State." << name << " offset=" << std::dec << offset << " size=" << size << " actual=";
        for (u32 at = 0; at < size; ++at) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(actual.bytes[offset + at]);
        out << " expected=";
        for (u32 at = 0; at < size; ++at) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(expected.bytes[offset + at]);
        out << '\n';
    };
#define STATE_DIFF(field) bytes(#field, offsetof(backend::State, field), sizeof(actual.state->field))
    STATE_DIFF(exit_request); STATE_DIFF(indirect_l1_code_cache); STATE_DIFF(indirect_call_l1_code_cache);
    STATE_DIFF(pending_call_l1_code_cache); STATE_DIFF(interface); STATE_DIFF(halt_reason);
    STATE_DIFF(rsb_pointer); STATE_DIFF(rsb_empty); STATE_DIFF(current_loc); STATE_DIFF(prev_loc);
    STATE_DIFF(pt); STATE_DIFF(local_buffer); STATE_DIFF(host_cpu_flags); STATE_DIFF(l2_code_cache);
    STATE_DIFF(guest_addr_limit); STATE_DIFF(guest_addr_mask); STATE_DIFF(interp_range_check);
    STATE_DIFF(interp_range_check_ctx); STATE_DIFF(unaligned_atomic_lock_address); STATE_DIFF(spill_area);
#undef STATE_DIFF
    for (u32 i = 0; i < 31; ++i) {
        const u64 a = i < 29 ? actual.Guest().r[i] : i == 29 ? actual.Guest().fp : actual.Guest().lr;
        const u64 e = i < 29 ? expected.Guest().r[i] : i == 29 ? expected.Guest().fp : expected.Guest().lr;
        if (a != e) out << "guest.x" << std::dec << i << " actual=" << std::hex << a << " expected=" << e << '\n';
    }
#define GUEST_DIFF(field) if (actual.Guest().field != expected.Guest().field) \
    out << "guest." #field " actual=" << std::hex << actual.Guest().field << " expected=" << expected.Guest().field << '\n'
    GUEST_DIFF(sp); GUEST_DIFF(pc); GUEST_DIFF(pstate); GUEST_DIFF(padding);
    GUEST_DIFF(fpcr); GUEST_DIFF(fpsr); GUEST_DIFF(tpidr);
#undef GUEST_DIFF
    out << "guest.nzcv actual=" << std::hex << (actual.state->host_cpu_flags & nzcv_mask)
        << " expected=" << (expected.state->host_cpu_flags & nzcv_mask) << '\n';
    for (u32 at = 0; at < actual.bytes.size(); ++at) if (actual.bytes[at] != expected.bytes[at]) {
        out << (at < sizeof(backend::State) ? "state_changed_byte=" : "uniform_changed_byte=")
            << std::dec << (at < sizeof(backend::State) ? at : at - sizeof(backend::State))
            << " actual=" << std::hex << unsigned(actual.bytes[at]) << " expected=" << unsigned(expected.bytes[at]) << '\n';
    }
    return std::runtime_error(out.str());
}
void Validate(const Program& program, const Compiled& unit) {
    for (u64 seed : {u64{0}, UINT64_MAX, u64{0x123456789abcdef0}, u64{0x8000000000000040}})
    for (u32 trips : {1u, 2u, 7u}) {
        Machine expected, actual; expected.Init(seed, trips); actual.Init(seed, trips); Model(program, expected, trips);
        const auto mxcsr = _mm_getcsr(); const auto reason = unit.Entry()(actual.state);
        if (reason != HaltReason::CallHost || _mm_getcsr() != mxcsr ||
            std::memcmp(actual.bytes.data(), expected.bytes.data(), actual.bytes.size()))
            throw ModelMismatch(program, unit, seed, trips, expected, actual, reason, mxcsr, _mm_getcsr(), "validation");
    }
}
void JsonRegisters(std::ostream& out, Machine& machine) {
    out << '[';
    for (u32 i = 0; i < 29; ++i) { if (i) out << ','; out << machine.Guest().r[i]; }
    out << ',' << machine.Guest().fp << ',' << machine.Guest().lr << ']';
}
void JsonMachine(std::ostream& out, const char* prefix, Machine& machine) {
    out << ",\"" << prefix << "_x\":"; JsonRegisters(out, machine);
    out << ",\"" << prefix << "_sp\":" << machine.Guest().sp
        << ",\"" << prefix << "_nzcv\":" << (machine.state->host_cpu_flags & nzcv_mask);
}
void OracleCases(std::ostream& out, Program& program,
                 const std::array<std::unique_ptr<Compiled>, route_count>& units, bool& first) {
    for (u32 flags = 0; flags < 2; ++flags)
    for (u64 seed : {u64{0}, UINT64_MAX, u64{0x123456789abcdef0}, u64{0x8000000000000040}})
    for (u32 trips : {1u, 2u, 7u}) {
        Machine input, expected, actual_off, actual_on;
        for (auto* machine : {&input, &expected, &actual_off, &actual_on}) machine->Init(seed, trips);
        Model(program, expected, trips);
        for (u32 remat = 0; remat < 2; ++remat) {
            auto& actual = remat ? actual_on : actual_off; const auto mxcsr = _mm_getcsr();
            const auto reason = units[flags * 2 + remat]->Entry()(actual.state);
            if (reason != HaltReason::CallHost || _mm_getcsr() != mxcsr ||
                std::memcmp(actual.bytes.data(), expected.bytes.data(), actual.bytes.size()))
                throw ModelMismatch(program, *units[flags * 2 + remat], seed, trips, expected, actual,
                                    reason, mxcsr, _mm_getcsr(), "oracle_export");
        }
        if (!first) out << ','; first = false;
        out << "{\"workload\":\"" << program.name << "\",\"seed\":" << seed
            << ",\"trips\":" << trips << ",\"flags_residency\":" << flags
            << ",\"code_base\":" << code_base << ",\"stop_pc\":" << program.StopPc() << ",\"words\":[";
        for (size_t i = 0; i < program.words.size(); ++i) { if (i) out << ','; out << program.words[i]; }
        out << ']'; JsonMachine(out, "input", input); JsonMachine(out, "expected", expected);
        JsonMachine(out, "actual_off", actual_off); JsonMachine(out, "actual_on", actual_on); out << '}';
    }
}
void SaveExclusive(const char* path, const std::string& contents) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "exclusive oracle corpus create");
    size_t written{};
    while (written < contents.size()) {
        const auto count = write(fd, contents.data() + written, contents.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            const int error = count < 0 ? errno : EIO; close(fd);
            throw std::system_error(error, std::generic_category(), "oracle corpus write");
        }
        written += size_t(count);
    }
    if (close(fd)) throw std::system_error(errno, std::generic_category(), "oracle corpus close");
}
void PairProof(const Compiled& off, const Compiled& on) {
    const auto& a = off.code->Statistics(); const auto& b = on.code->Statistics();
    const auto& old_frame = off.code->FramePlan().Statistics(); const auto& new_frame = on.code->FramePlan().Statistics();
    if (off.eligible_unassigned != on.eligible_unassigned || old_frame.home_bytes != new_frame.home_bytes ||
        old_frame.retained_homes != new_frame.retained_homes || old_frame.omitted_rematerialized != new_frame.omitted_rematerialized ||
        a.scalar_rematerialized_definition_sites != b.scalar_rematerialized_definition_sites ||
        a.scalar_rematerialized_read_sites != b.scalar_rematerialized_read_sites + b.scalar_binary_rematerialized_immediate_sites ||
        off.code->getSize() <= on.code->getSize() || !b.scalar_binary_immediate_sites)
        throw std::runtime_error("paired decoded workload lacks same-layout binary immediate code reduction");
}
double Median(std::vector<double> values) { std::sort(values.begin(), values.end()); return values.at(values.size() / 2); }
std::array<double, route_count> Measure(const std::array<std::unique_ptr<Compiled>, route_count>& units, u32 calls, u32 trips) {
    std::array<Machine, route_count> machines; std::array<std::vector<double>, route_count> samples;
    for (u32 route = 0; route < route_count; ++route) {
        machines[route].Init(0x123456789abcdef0, trips);
        for (u32 warm = 0; warm < 32; ++warm) {
            machines[route].Guest().r[28] = trips; machines[route].state->halt_reason = HaltReason::None;
            if (units[route]->Entry()(machines[route].state) != HaltReason::CallHost) throw std::runtime_error("move-wide warmup exit");
        }
    }
    for (u32 repeat = 0; repeat < 7; ++repeat) for (u32 order = 0; order < route_count; ++order) {
        const u32 route = (order + repeat) % route_count; auto& machine = machines[route]; const auto entry = units[route]->Entry();
        const auto begin = Clock::now();
        for (u32 call = 0; call < calls; ++call) {
            machine.Guest().r[28] = trips; machine.state->halt_reason = HaltReason::None;
            if (entry(machine.state) != HaltReason::CallHost) throw std::runtime_error("move-wide measurement exit");
        }
        samples[route].push_back(std::chrono::duration<double, std::nano>(Clock::now() - begin).count() / calls);
    }
    std::array<double, route_count> result{}; for (u32 route = 0; route < route_count; ++route) result[route] = Median(samples[route]);
    return result;
}
u32 PositiveArgument(const char* value) {
    size_t consumed{}; const auto number = std::stoull(value, &consumed);
    if (consumed != std::strlen(value) || !number || number > std::numeric_limits<u32>::max())
        throw std::runtime_error("calls and trips must be positive uint32 values");
    return u32(number);
}
} // namespace

int main(int argc, char** argv) {
    try {
        const bool validate_only = argc == 2 && std::string{argv[1]} == "--validate-only";
        const bool oracle = argc == 3 && std::string{argv[1]} == "--oracle-corpus";
        const bool correctness_only = validate_only || oracle;
        if (!correctness_only && argc > 3)
            throw std::runtime_error("usage: benchmark [calls [trips]], --validate-only, or --oracle-corpus NEW_FILE");
        const u32 calls = correctness_only || argc < 2 ? 500 : PositiveArgument(argv[1]);
        const u32 trips = correctness_only || argc < 3 ? 64 : PositiveArgument(argv[2]);
        std::cerr << "scope=" << scope << "; cfg_on=1; rematerialization_on=1; decoded_rematerialized_rhs_claim=0; production_flat_forward_const_fold_dce=1; "
                     "oracle=independent_full_arm_context_and_state_model; transfer_counts=static_sites; "
                     "application_performance_claim=0; dynamic_spill_claim=0\n";
        std::ostringstream corpus; bool first_case = true;
        if (oracle) corpus << "{\"schema_version\":1,\"scope\":\"" << scope << "\",\"cases\":[";
        if (!correctness_only)
            std::cout << "workload,cfg_allocation,scalar_binary_immediates,flags_residency,guest_words,move_wide_instructions,"
                         "guest_blocks,cfg_blocks,trips,decoded_plain_loadimm,optimized_plain_loadimm,final_plain_loadimm,"
                         "eligible_unassigned_loadimm,omitted_rematerialized,rematerialized_definition_sites,rematerialized_read_sites,scalar_binary_immediate_sites,scalar_binary_rematerialized_immediate_sites,"
                         "assigned_values,physical_home_slots,retained_home_slots,reused_scalar_homes,shared_scalar_home_slots,"
                         "home_bytes,frame_bytes,code_bytes,observed_ssa_load_sites,observed_ssa_store_sites,ssa_load_bytes,ssa_store_bytes,"
                         "observed_uniform_load_sites,observed_uniform_store_sites,flags_state_load_sites,flags_state_store_sites,"
                         "flags_resident_merge_sites,flags_resident_update_sites,preparation_full_allocation_builds,"
                         "preparation_reused_final_allocation,forwarded_load_ir,state_promotion_accepted,forward_allocation_rounds,"
                         "decode_plan_prepare_emit_us,native_ns_per_call,native_ns_per_guest_iteration,guest_instructions_per_second\n";
        for (bool mixed : {false, true}) {
            auto program = MakeProgram(mixed); std::array<std::unique_ptr<Compiled>, route_count> units;
            std::array<std::vector<double>, route_count> preparation;
            if (!correctness_only) for (u32 repeat = 0; repeat < 9; ++repeat) for (u32 order = 0; order < route_count; ++order) {
                const u32 route = (order + repeat) % route_count; const auto begin = Clock::now(); auto sample = Compile(program, route);
                preparation[route].push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
            }
            for (u32 route = 0; route < route_count; ++route) { units[route] = Compile(program, route); Validate(program, *units[route]); }
            PairProof(*units[0], *units[1]); PairProof(*units[2], *units[3]);
            if (oracle) OracleCases(corpus, program, units, first_case);
            if (correctness_only) continue;
            const auto ns = Measure(units, calls, trips);
            const auto move_wide = std::count_if(program.operations.begin(), program.operations.end(), [](const auto& op) {
                return op.kind == Kind::MovZ || op.kind == Kind::MovN || op.kind == Kind::MovK;
            });
            for (u32 route = 0; route < route_count; ++route) {
                const auto& unit = *units[route]; const auto& code = *unit.code; const auto& cfg = code.CfgPlan();
                const auto& frame = code.FramePlan().Statistics(); const auto& stats = code.Statistics();
                const auto& ssa = stats.At(x64::TransferRegion::CanonicalSSA); const auto& uniform = stats.At(x64::TransferRegion::Uniform);
                const auto assigned = std::count_if(cfg.Allocations().begin(), cfg.Allocations().end(), [](const auto& allocation) { return allocation.slot.has_value(); });
                const u64 instructions = u64(trips) * (program.operations.size() + 2) + 1;
                std::cout << std::fixed << std::setprecision(2) << program.name << ",1," << bool(route & 1) << ',' << bool(route & 2)
                          << ',' << program.words.size() << ',' << move_wide << ',' << unit.guest_blocks << ',' << cfg.Blocks().size() << ',' << trips
                          << ',' << unit.decoded_plain_loadimm << ',' << unit.optimized_plain_loadimm << ',' << PlainCount(unit.plan.body.get())
                          << ',' << unit.eligible_unassigned << ',' << frame.omitted_rematerialized
                          << ',' << stats.scalar_rematerialized_definition_sites << ',' << stats.scalar_rematerialized_read_sites
                          << ',' << stats.scalar_binary_immediate_sites << ',' << stats.scalar_binary_rematerialized_immediate_sites
                          << ',' << assigned << ',' << frame.home_count << ',' << frame.retained_homes << ',' << frame.reused_scalar_homes << ',' << frame.shared_scalar_home_slots
                          << ',' << frame.home_bytes << ',' << code.FrameSize() << ',' << code.getSize()
                          << ',' << ssa.scalar_load_sites + ssa.vector_load_sites << ',' << ssa.scalar_store_sites + ssa.vector_store_sites
                          << ',' << ssa.load_bytes << ',' << ssa.store_bytes
                          << ',' << uniform.scalar_load_sites + uniform.vector_load_sites << ',' << uniform.scalar_store_sites + uniform.vector_store_sites
                          << ',' << stats.flags_state_load_sites << ',' << stats.flags_state_store_sites
                          << ',' << stats.flags_resident_merge_sites << ',' << stats.flags_resident_update_sites
                          << ',' << code.PreparationStats().full_allocation_builds << ',' << code.PreparationStats().reused_final_allocation
                          << ',' << code.UniformForwardStats().loads_forwarded << ',' << code.StatePromotionStats().accepted << ',' << code.UniformForwardStats().allocation_rounds
                          << ',' << Median(preparation[route]) << ',' << ns[route] << ',' << ns[route] / trips
                          << ',' << double(instructions) * 1e9 / ns[route] << '\n';
            }
        }
        if (oracle) { corpus << "]}\n"; SaveExclusive(argv[2], corpus.str()); std::cerr << "oracle_cases=48; exclusive_corpus=1\n"; }
        if (correctness_only) std::cerr << "validated_routes=8; seeds_per_route=4; trip_counts=1,2,7; paired_immediate_code_reduction=1\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "scalar binary immediate benchmark: " << error.what() << '\n'; return 1;
    }
}
