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
#include <unordered_map>
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
constexpr u32 cold_registers = 3, hot_blocks = 3, cold_blocks = 8, route_count = 4;
constexpr const char* scope = "decoded_a64_cfg_loop_weighted_pair";
enum class Kind { Add, Sub, Xor, Compare };
struct Operation { Kind kind; u32 dst{}, lhs{}, rhs{}; bool wide{true}, flags{}; u32 immediate{}; };
struct Program final : MemoryInterface {
    std::string name;
    std::vector<u32> words;
    std::vector<Operation> preheader, hot, tail;
    u32 hot_begin{}, hot_end{}, tail_begin{};
    u64 StopPc() const { return code_base + (words.size() - 1) * 4; }
    u64 Instructions(u32 trips) const {
        return u64(preheader.size() + 1) + u64(trips) * (hot.size() + hot_blocks + 1) + tail.size() + cold_blocks + 1;
    }
    bool Read(void* out, size_t address, size_t size) override {
        if (address < code_base || address - code_base > words.size() * 4 ||
            size > words.size() * 4 - (address - code_base)) return false;
        std::memcpy(out, reinterpret_cast<const u8*>(words.data()) + address - code_base, size); return true;
    }
    bool Write(void*, size_t, size_t) override { return false; }
    void* GetPointer(void* address) override {
        const auto at = reinterpret_cast<uintptr_t>(address);
        if (at < code_base || at - code_base > words.size() * 4 || words.size() * 4 - (at - code_base) < 4) return nullptr;
        return reinterpret_cast<u8*>(words.data()) + at - code_base;
    }
    void Emit(Operation op, std::vector<Operation>& region) {
        u32 word{};
        if (op.kind == Kind::Compare) {
            word = 0x7100001fu | (op.immediate << 10) | (op.lhs << 5); // SUBS WZR,Wn,#imm.
            if (op.wide) word |= 0x80000000u;
        } else {
            word = op.kind == Kind::Add ? 0x0b000000u : op.kind == Kind::Sub ? 0x4b000000u : 0x4a000000u;
            if (op.wide) word |= 0x80000000u;
            if (op.flags) word |= 0x20000000u;
            word |= (op.rhs << 16) | (op.lhs << 5) | op.dst;
        }
        words.push_back(word); region.push_back(op);
    }
};
Program MakeProgram(bool mixed) {
    Program program; program.name = mixed ? "mixed_hot_cold_loop" : "wide_hot_cold_loop";
    // Six immutable non-immediate globals share long lifetimes. Their guest
    // lanes are never modified in either the hot loop or the cold exit tail.
    for (u32 i = 0; i < cold_registers * 2; ++i)
        program.Emit({Kind::Xor, i, 20 + i % 4, 24 + i % 4, true, false}, program.preheader);
    program.words.push_back(0x14000001u);
    program.hot_begin = program.words.size();
    // Flags-only sinks have no useful result or accumulator chains. Each
    // guest block's first read promotes directly to the same preheader SSA.
    for (u32 block = 0; block < hot_blocks; ++block) {
        for (u32 i = 0; i < cold_registers; ++i)
            program.Emit({Kind::Compare, 31, cold_registers + i, 0,
                          !mixed || bool((i + block) & 1), true, 17 + block * 11 + i}, program.hot);
        if (block + 1 < hot_blocks) program.words.push_back(0x14000001u);
    }
    program.words.push_back(0xd100079cu);
    const auto displacement = (program.hot_begin - u32(program.words.size())) & 0x7ffff;
    program.words.push_back(0xb5000000u | (displacement << 5) | 28u);
    program.hot_end = program.words.size(); program.tail_begin = program.hot_end;
    for (u32 block = 0; block < cold_blocks; ++block) {
        for (u32 i = 0; i < cold_registers; ++i)
            program.Emit({Kind::Compare, 31, i, 0,
                          !mixed || bool((i + block) & 1), true, 31 + block * 7 + i}, program.tail);
        // Retain hot globals through the same complete tail as cold globals;
        // OFF compares real static use frequency, ON adds hot loop weights.
        if (block + 1 == cold_blocks) for (u32 i = 0; i < cold_registers; ++i)
            program.Emit({Kind::Compare, 31, cold_registers + i, 0,
                          !mixed || bool(i & 1), true, 83 + i}, program.tail);
        program.words.push_back(0x14000001u);
    }
    program.words.push_back(0xd4200000u); return program;
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
    // The independent model consumes only assembler descriptors.
    const auto execute = [&](const std::vector<Operation>& operations) {
        for (const auto& op : operations) {
            const auto mask = op.wide ? UINT64_MAX : u64{UINT32_MAX};
            const auto lhs = regs[op.lhs] & mask, rhs = op.kind == Kind::Compare ? u64(op.immediate) : regs[op.rhs] & mask;
            if (op.kind != Kind::Compare) regs[op.dst] = (op.kind == Kind::Xor ? lhs ^ rhs : op.kind == Kind::Sub ? lhs - rhs : lhs + rhs) & mask;
            if (op.flags) flags = ArithmeticFlags(flags, lhs, rhs, op.wide ? 64 : 32,
                                                  op.kind == Kind::Sub || op.kind == Kind::Compare ? 1 : 0,
                                                  op.kind == Kind::Sub || op.kind == Kind::Compare);
        }
    };
    execute(program.preheader);
    for (u32 trip = 0; trip < trips; ++trip) { execute(program.hot); --regs[28]; }
    execute(program.tail);
    machine.Guest().padding = u32(swift::arm64::InterruptReason::BRK);
    machine.state->current_loc = ir::Location{program.StopPc()};
    // The final cold B commits the edge to the BRK block on every exit.
    machine.state->prev_loc = ir::Location{program.StopPc() - 4};
    machine.state->halt_reason = HaltReason::CallHost;
}
struct Compiled {
    Config config{.backend_isa = kX86_64, .uniform_buffer_size = sizeof(Context),
                  .loop_gpr_uniform_ranges = swift::arm64::Arm64GprUniformRanges(), .stack_alignment = 16};
    backend::FunctionPlan plan;
    std::unique_ptr<x64::Translator> code;
    x64::CfgStatePromoteStats promoted;
    u32 route{}, guest_blocks{}, hot_assigned{}, cold_assigned{}, assignment_changes{};
    u64 reference_weighted_saved_reads{}, reference_weighted_benefit{};
    std::string fixed_ir;
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
        if (unit->guest_blocks + pending.size() > 32) throw std::runtime_error("loop weighted benchmark HIR block cap");
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
        auto* body = hir->GetBlock();
        x64::ForwardUniformLoads(body, sizeof(Context));
        ir::ConstFoldingPass::Run(body, features); ir::DeadCodeEliminationPass::Run(body, function);
    }
    unit->plan = backend::FunctionPlan::Build(function, ordered);
    auto graph = x64::CfgRegisterPlan::ValidateGraph(unit->plan.body.get(), unit->plan);
    if (!graph.Enabled()) throw std::runtime_error("decoded graph rejected before fixed promotion");
    std::vector<u32> lanes;
    for (const auto& range : unit->config.loop_gpr_uniform_ranges) lanes.push_back(range.offset);
    unit->promoted = x64::PromoteCfgState(unit->plan, sizeof(Context), lanes, graph, {.reject_dirty_loop_phis = true});
    if (!unit->promoted.accepted || !unit->promoted.loads_replaced)
        throw std::runtime_error("decoded hot/cold workload lacks fixed state promotion");
    unit->fixed_ir = unit->plan.body->ToString();
    // All IR transforms precede the policy split. Production allocation-aware
    // forwarding is covered separately; this comparison isolates coloring.
    unit->code = x64::Translator::PrepareFunction(unit->plan, x64::HostFeatures::Detect(), &unit->config,
        true, true, bool(route & 2), {.uniform_forwarding = false, .state_promotion = false,
                                    .scalar_rematerialization = true, .scalar_binary_immediates = true,
                                    .loop_weighted_costs = bool(route & 1)});
    if (unit->fixed_ir != unit->plan.body->ToString()) throw std::runtime_error("policy preparation changed fixed IR");
    unit->code->Translate();
    const auto& cfg = unit->code->CfgPlan();
    if (!cfg.Enabled() || !cfg.Validate() || !unit->code->FramePlan().Enabled() ||
        unit->guest_blocks != cold_blocks + hot_blocks + 2 || cfg.LoopWeightedCostsEnabled() != bool(route & 1))
        throw std::runtime_error("loop weighted benchmark lacks validated decoded CFG and policy");
    return unit;
}
std::runtime_error ModelMismatch(const Program& program, const Compiled& unit, u64 seed, u32 trips,
                                Machine& expected, Machine& actual, HaltReason reason,
                                u32 expected_mxcsr, u32 actual_mxcsr, const char* stage) {
    std::ostringstream out;
    out << program.name << " full ARM context and State independent-model mismatch"
        << " stage=" << stage << " seed=" << std::hex << seed << std::dec << " trips=" << trips
        << " route=" << unit.route << " loop_weighted_costs=" << bool(unit.route & 1)
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
    for (u64 seed : {u64{0}, UINT64_MAX, u64{0x123456789abcdef0}})
    for (u32 trips : {1u, 2u, 7u, 19u}) {
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
    for (u64 seed : {u64{0}, UINT64_MAX, u64{0x123456789abcdef0}})
    for (u32 trips : {1u, 2u, 7u, 19u}) {
        Machine input, expected, actual_off, actual_on;
        for (auto* machine : {&input, &expected, &actual_off, &actual_on}) machine->Init(seed, trips);
        Model(program, expected, trips);
        for (u32 weighted = 0; weighted < 2; ++weighted) {
            auto& actual = weighted ? actual_on : actual_off; const auto mxcsr = _mm_getcsr();
            const auto reason = units[flags * 2 + weighted]->Entry()(actual.state);
            if (reason != HaltReason::CallHost || _mm_getcsr() != mxcsr ||
                std::memcmp(actual.bytes.data(), expected.bytes.data(), actual.bytes.size()))
                throw ModelMismatch(program, *units[flags * 2 + weighted], seed, trips, expected, actual,
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
void PairProof(Compiled& off, Compiled& on) {
    const auto& a = off.code->CfgPlan(); const auto& b = on.code->CfgPlan();
    if (off.fixed_ir != on.fixed_ir || a.Instructions().size() != b.Instructions().size() ||
        a.PhysicalRegisters() != b.PhysicalRegisters() || a.Blocks().size() != b.Blocks().size() ||
        a.Allocations().size() != b.Allocations().size() || a.Edges().size() != b.Edges().size() ||
        off.code->PreparationStats().full_allocation_builds != on.code->PreparationStats().full_allocation_builds ||
        off.code->UniformForwardStats().allocation_rounds || on.code->UniformForwardStats().allocation_rounds)
        throw std::runtime_error("weighted pair changed fixed CFG/IR or preparation build count");
    const auto& stats = b.ConstructionStats();
    if (stats.natural_loops != 1 || stats.max_loop_depth != 1 || a.ConstructionStats().natural_loops)
        throw std::runtime_error("expected separate preheader, natural hot loop and cold tail");
    std::vector<u32> depth(b.Instructions().size());
    for (u32 i = 0; i < b.Blocks().size(); ++i) {
        const auto& left = a.Blocks()[i]; const auto& right = b.Blocks()[i];
        if (left.begin != right.begin || left.end != right.end || left.reachable != right.reachable ||
            left.incoming_edges != right.incoming_edges || left.outgoing_edges != right.outgoing_edges ||
            left.loop_depth || left.loop_weight != 1 || right.loop_depth > 1 ||
            right.loop_weight != (right.loop_depth ? 8u : 1u))
            throw std::runtime_error("paired CFG block topology differs");
        for (u32 at = right.begin; at < right.end; ++at) depth[at] = right.loop_depth;
    }
    for (u32 i = 0; i < a.Edges().size(); ++i) {
        const auto& left = a.Edges()[i]; const auto& right = b.Edges()[i];
        if (left.from != right.from || left.to != right.to || left.taken != right.taken ||
            left.host_entry != right.host_entry || left.copies.size() != right.copies.size())
            throw std::runtime_error("paired executable edge topology differs");
    }
    std::unordered_map<const ir::Inst*, u32> ordinal;
    for (u32 at = 0; at < b.Instructions().size(); ++at) ordinal.emplace(b.Instructions()[at], at);
    u32 immutable_globals{}, global_assignment_changes{};
    for (u32 i = 0; i < a.Allocations().size(); ++i) {
        const auto& left = a.Allocations()[i]; const auto& right = b.Allocations()[i];
        if (left.scalar_reads != right.scalar_reads || left.uses != right.uses ||
            left.requires_full_container != right.requires_full_container)
            throw std::runtime_error("paired native read/opaque obligations differ");
        // Classify by actual audited hot consumers, including immutable
        // preheader globals read by the loop; definition depth alone is cold.
        const bool hot = right.weighted_reads > right.scalar_reads;
        if (right.producer->GetOp() == ir::OpCode::Xor && !depth[ordinal.at(right.producer)]) {
            ++immutable_globals;
            const auto expected_reads = hot ? hot_blocks + 2 : cold_blocks + 1;
            const auto expected_weighted_reads = hot ? hot_blocks * 8 + 2 : cold_blocks + 1;
            if (right.scalar_reads != expected_reads || right.weighted_reads != expected_weighted_reads)
                throw std::runtime_error("preheader globals did not retain independently repeated hot/cold SSA reads");
            global_assignment_changes += left.slot.has_value() != right.slot.has_value();
        }
        if (left.slot.has_value() != right.slot.has_value()) ++off.assignment_changes;
        for (auto* unit : {&off, &on}) {
            const bool assigned = unit == &off ? left.slot.has_value() : right.slot.has_value();
            if (!assigned) continue;
            (hot ? unit->hot_assigned : unit->cold_assigned)++;
            unit->reference_weighted_saved_reads += right.weighted_reads;
            unit->reference_weighted_benefit += right.weighted_benefit;
        }
    }
    on.assignment_changes = off.assignment_changes;
    if (immutable_globals != cold_registers * 2 || !global_assignment_changes || !off.assignment_changes ||
        off.reference_weighted_saved_reads == on.reference_weighted_saved_reads) {
        for (u32 i = 0; i < a.Allocations().size(); ++i) {
            const auto& left = a.Allocations()[i]; const auto& right = b.Allocations()[i];
            std::cerr << "allocation_candidate ordinal=" << ordinal.at(right.producer)
                      << " op=" << ir::GetIRMetaInfo(right.producer->GetOp()).name
                      << " depth=" << depth[ordinal.at(right.producer)] << " reads=" << right.scalar_reads
                      << " weighted_reads=" << right.weighted_reads << " benefit=" << right.estimated_benefit
                      << " weighted_benefit=" << right.weighted_benefit
                      << " off_assigned=" << left.slot.has_value() << " on_assigned=" << right.slot.has_value() << '\n';
        }
        throw std::runtime_error("decoded workload does not change allocation and reference weighted reads");
    }
    std::cerr << "paired_allocation workload=" << off.plan.body->GetStartLocation().Value()
              << " flags=" << bool(off.route & 2) << " globals=" << immutable_globals
              << " global_changes=" << global_assignment_changes << " changes=" << off.assignment_changes
              << " hot_off=" << off.hot_assigned << " hot_on=" << on.hot_assigned
              << " cold_off=" << off.cold_assigned << " cold_on=" << on.cold_assigned
              << " weighted_reads_off=" << off.reference_weighted_saved_reads
              << " weighted_reads_on=" << on.reference_weighted_saved_reads << '\n';
}
double Median(std::vector<double> values) { std::sort(values.begin(), values.end()); return values.at(values.size() / 2); }
std::array<double, route_count> Measure(const std::array<std::unique_ptr<Compiled>, route_count>& units, u32 calls, u32 trips) {
    std::array<Machine, route_count> machines; std::array<std::vector<double>, route_count> samples;
    for (u32 route = 0; route < route_count; ++route) {
        machines[route].Init(0x123456789abcdef0, trips);
        for (u32 warm = 0; warm < 32; ++warm) {
            machines[route].Guest().r[28] = trips; machines[route].state->halt_reason = HaltReason::None;
            if (units[route]->Entry()(machines[route].state) != HaltReason::CallHost) throw std::runtime_error("loop weighted warmup exit");
        }
    }
    for (u32 repeat = 0; repeat < 7; ++repeat) for (u32 order = 0; order < route_count; ++order) {
        const u32 route = (order + repeat) % route_count; auto& machine = machines[route]; const auto entry = units[route]->Entry();
        const auto begin = Clock::now();
        for (u32 call = 0; call < calls; ++call) {
            machine.Guest().r[28] = trips; machine.state->halt_reason = HaltReason::None;
            if (entry(machine.state) != HaltReason::CallHost) throw std::runtime_error("loop weighted measurement exit");
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
        std::cerr << "scope=" << scope << "; cfg_on=1; rematerialization_on=1; fixed_ir_before_policy=1; production_flat_forward_const_fold_dce=1; "
                     "fixed_state_promotion=1; allocation_feedback_in_comparison=0; "
                     "oracle=independent_full_arm_context_and_state_model; transfer_counts=static_sites; "
                     "weighted_costs=structural_estimate; application_performance_claim=0; dynamic_spill_claim=0\n";
        std::ostringstream corpus; bool first_case = true;
        if (oracle) corpus << "{\"schema_version\":1,\"scope\":\"" << scope << "\",\"cases\":[";
        if (!correctness_only)
            std::cout << "workload,cfg_allocation,loop_weighted_costs,flags_residency,guest_words,preheader_words,hot_words,cold_words,guest_blocks,cfg_blocks,trips,"
                         "natural_loops,max_loop_depth,assigned_values,hot_assigned_values,cold_assigned_values,assignment_changes,reference_weighted_saved_reads,reference_weighted_benefit,"
                         "physical_home_slots,retained_home_slots,reused_scalar_homes,shared_scalar_home_slots,home_bytes,frame_bytes,code_bytes,"
                         "observed_ssa_load_sites,observed_ssa_store_sites,ssa_load_bytes,ssa_store_bytes,observed_uniform_load_sites,observed_uniform_store_sites,"
                         "flags_state_load_sites,flags_state_store_sites,flags_resident_merge_sites,flags_resident_update_sites,preparation_full_allocation_builds,"
                         "preparation_reused_final_allocation,fixed_promoted_load_ir,forward_allocation_rounds,"
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
            for (u32 route = 0; route < route_count; ++route) {
                const auto& unit = *units[route]; const auto& code = *unit.code; const auto& cfg = code.CfgPlan();
                const auto& frame = code.FramePlan().Statistics(); const auto& stats = code.Statistics();
                const auto& ssa = stats.At(x64::TransferRegion::CanonicalSSA); const auto& uniform = stats.At(x64::TransferRegion::Uniform);
                const auto assigned = std::count_if(cfg.Allocations().begin(), cfg.Allocations().end(), [](const auto& allocation) { return allocation.slot.has_value(); });
                const u64 instructions = program.Instructions(trips);
                std::cout << std::fixed << std::setprecision(2) << program.name << ",1," << bool(route & 1) << ',' << bool(route & 2)
                          << ',' << program.words.size() << ',' << program.preheader.size() + 1 << ',' << program.hot.size() + hot_blocks + 1
                          << ',' << program.tail.size() + cold_blocks << ',' << unit.guest_blocks << ',' << cfg.Blocks().size() << ',' << trips
                          << ',' << cfg.ConstructionStats().natural_loops << ',' << cfg.ConstructionStats().max_loop_depth
                          << ',' << assigned << ',' << unit.hot_assigned << ',' << unit.cold_assigned << ',' << unit.assignment_changes
                          << ',' << unit.reference_weighted_saved_reads << ',' << unit.reference_weighted_benefit
                          << ',' << frame.home_count << ',' << frame.retained_homes << ',' << frame.reused_scalar_homes << ',' << frame.shared_scalar_home_slots
                          << ',' << frame.home_bytes << ',' << code.FrameSize() << ',' << code.getSize()
                          << ',' << ssa.scalar_load_sites + ssa.vector_load_sites << ',' << ssa.scalar_store_sites + ssa.vector_store_sites
                          << ',' << ssa.load_bytes << ',' << ssa.store_bytes
                          << ',' << uniform.scalar_load_sites + uniform.vector_load_sites << ',' << uniform.scalar_store_sites + uniform.vector_store_sites
                          << ',' << stats.flags_state_load_sites << ',' << stats.flags_state_store_sites
                          << ',' << stats.flags_resident_merge_sites << ',' << stats.flags_resident_update_sites
                          << ',' << code.PreparationStats().full_allocation_builds << ',' << code.PreparationStats().reused_final_allocation
                          << ',' << unit.promoted.loads_replaced << ',' << code.UniformForwardStats().allocation_rounds
                          << ',' << Median(preparation[route]) << ',' << ns[route] << ',' << ns[route] / trips
                          << ',' << double(instructions) * 1e9 / ns[route] << '\n';
            }
        }
        if (oracle) { corpus << "]}\n"; SaveExclusive(argv[2], corpus.str()); std::cerr << "oracle_cases=48; exclusive_corpus=1\n"; }
        if (correctness_only) std::cerr << "validated_routes=8; seeds_per_route=3; trip_counts=1,2,7,19; paired_weighted_allocation_change=1\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CFG loop weighted benchmark: " << error.what() << '\n'; return 1;
    }
}
