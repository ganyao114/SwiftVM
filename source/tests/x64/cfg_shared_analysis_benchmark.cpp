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
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#include <unordered_set>
#include <unistd.h>
#include <xmmintrin.h>
#include "runtime/backend/x64/translator.h"
#include "runtime/backend/x64/uniform_forward.h"
#include "runtime/backend/x64/flags_usage.h"
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
constexpr u32 globals = 4, hot_blocks = 3, route_count = 4;
constexpr const char* scope = "decoded_a64_cfg_shared_analysis_pair";
constexpr std::array<const char*, route_count> route_names{"off", "forced", "profitable", "profitable_full_build"};
enum class Kind { Add, AddCarry, Xor, Compare };
struct Operation { Kind kind; u32 dst{}, lhs{}, rhs{}, immediate{}; };
struct Program final : MemoryInterface {
    std::string name;
    bool carry{};
    std::vector<u32> words;
    std::vector<Operation> preheader, hot, tail;
    u32 hot_begin{};
    u64 StopPc() const { return code_base + (words.size() - 1) * 4; }
    u64 Instructions(u32 trips) const { return preheader.size() + 1 + u64(trips) * (hot.size() + hot_blocks + 1) + tail.size() + 2; }
    bool Read(void* out, size_t address, size_t size) override {
        if (address < code_base || address - code_base > words.size() * 4 || size > words.size() * 4 - (address - code_base)) return false;
        std::memcpy(out, reinterpret_cast<const u8*>(words.data()) + address - code_base, size); return true;
    }
    bool Write(void*, size_t, size_t) override { return false; }
    void* GetPointer(void* address) override {
        const auto at = reinterpret_cast<uintptr_t>(address);
        if (at < code_base || at - code_base > words.size() * 4 || words.size() * 4 - (at - code_base) < 4) return nullptr;
        return reinterpret_cast<u8*>(words.data()) + at - code_base;
    }
    void Emit(Operation op, std::vector<Operation>& region) {
        const u32 word = op.kind == Kind::Compare ? 0xf100001fu | (op.immediate << 10) | (op.lhs << 5) :
            (op.kind == Kind::Xor ? 0xca000000u : op.kind == Kind::AddCarry ? 0xba000000u : 0x8b000000u) |
            (op.rhs << 16) | (op.lhs << 5) | op.dst;
        words.push_back(word); region.push_back(op);
    }
};
Program MakeProgram(bool carry) {
    Program program; program.carry = carry;
    program.name = carry ? "hot_adcs_pressure_loop" : "sparse_cmp_pressure_loop";
    for (u32 i = 0; i < globals; ++i) program.Emit({Kind::Xor, i, 20 + i, 24 + i}, program.preheader);
    program.words.push_back(0x14000001u); program.hot_begin = program.words.size();
    // Every block reads the same immutable preheader SSA values. Each result
    // is observable through guest state; dirty loop lanes retain their stores.
    // The three-block loop is a real frontend CFG, with no flags selector in
    // the sparse case and a genuine serial architectural carry chain otherwise.
    for (u32 block = 0; block < hot_blocks; ++block) {
        for (u32 i = 0; i < globals; ++i)
            program.Emit({carry ? Kind::AddCarry : Kind::Add, 8 + i, i, 20 + i}, program.hot);
        if (block + 1 < hot_blocks) program.words.push_back(0x14000001u);
    }
    program.words.push_back(0xd100079cu); // SUB X28,X28,#1, no flags.
    const auto displacement = (program.hot_begin - u32(program.words.size())) & 0x7ffff;
    program.words.push_back(0xb5000000u | (displacement << 5) | 28u);
    for (u32 i = 0; i < globals; ++i) program.Emit({Kind::Xor, 12 + i, i, 24 + i}, program.tail);
    program.Emit({Kind::Compare, 31, 15, 0, 83}, program.tail);
    program.words.push_back(0x14000001u); program.words.push_back(0xd4200000u);
    return program;
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
        Guest().r[28] = trips; Guest().fp = seed ^ 0x3141592653589793ULL; Guest().lr = ~seed; Guest().sp = 0x8000;
        state->host_cpu_flags = ((seed ^ 0xabcd321001234567ULL) & ~nzcv_mask) | (((seed >> 4) & 15) << 28);
        state->current_loc = ir::Location{0xfeedface}; state->halt_reason = HaltReason::None;
    }
};
u64 ArithmeticFlags(u64 old, u64 lhs, u64 rhs, u64 carry, bool subtract) {
    const auto sum = static_cast<unsigned __int128>(lhs) + (subtract ? ~rhs : rhs) + carry;
    const auto result = u64(sum);
    const auto signed_value = [](u64 value) -> __int128 { return value >> 63 ? __int128(value) - (__int128{1} << 64) : __int128(value); };
    const auto mathematical = subtract ? signed_value(lhs) - signed_value(rhs) - (1 - carry) : signed_value(lhs) + signed_value(rhs) + carry;
    const bool overflow = mathematical < -(__int128{1} << 63) || mathematical >= (__int128{1} << 63);
    return (old & ~nzcv_mask) | ((result >> 63) << 31) | (u64(result == 0) << 30) | (u64(bool(sum >> 64)) << 29) | (u64(overflow) << 28);
}
void Model(const Program& program, Machine& machine, u32 trips) {
    auto& regs = machine.Guest().r; auto& flags = machine.state->host_cpu_flags;
    const auto execute = [&](const std::vector<Operation>& operations) {
        for (const auto& op : operations) {
            const u64 lhs = regs[op.lhs], rhs = op.kind == Kind::Compare ? op.immediate : regs[op.rhs];
            const u64 carry = op.kind == Kind::AddCarry ? (flags >> 29) & 1 : op.kind == Kind::Compare ? 1 : 0;
            if (op.kind != Kind::Compare) regs[op.dst] = op.kind == Kind::Xor ? lhs ^ rhs : lhs + rhs + carry;
            if (op.kind == Kind::Compare || op.kind == Kind::AddCarry) flags = ArithmeticFlags(flags, lhs, rhs, carry, op.kind == Kind::Compare);
        }
    };
    execute(program.preheader);
    for (u32 trip = 0; trip < trips; ++trip) { execute(program.hot); --regs[28]; }
    execute(program.tail);
    machine.Guest().padding = u32(swift::arm64::InterruptReason::BRK);
    machine.state->current_loc = ir::Location{program.StopPc()}; machine.state->prev_loc = ir::Location{program.StopPc() - 4};
    machine.state->halt_reason = HaltReason::CallHost;
}
struct Compiled {
    Config config{.backend_isa = kX86_64, .uniform_buffer_size = sizeof(Context),
                  .loop_gpr_uniform_ranges = swift::arm64::Arm64GprUniformRanges(), .stack_alignment = 16};
    backend::FunctionPlan plan;
    std::unique_ptr<x64::Translator> code;
    x64::CfgStatePromoteStats promoted;
    u32 route{}, guest_blocks{}, assignment_changes{};
    bool selection_code_equal{}, selection_plan_equal{};
    bool direct_shared_analysis{}, direct_shared_backing{}, direct_allocations_independent{}, direct_plan_equal{}, direct_home_equal{}, direct_seed_unchanged{};
    double cfg_full_build_us{}, cfg_shared_recolor_us{};
    std::string fixed_ir;
    Fn Entry() const { return code->getCode<Fn>(); }
};
std::unique_ptr<Compiled> Compile(Program& program, u32 route) {
    auto unit = std::make_unique<Compiled>(); unit->route = route;
    ir::HIRBuilder builder{1, true}; auto* function = builder.AppendFunction(ir::Location{code_base});
    for (;;) {
        std::vector<u64> pending;
        for (auto& hir : function->GetHIRBlockList()) {
            auto* block = hir.GetBlock();
            if (block->GetInstList().empty() && !block->HasTerminal()) pending.push_back(block->GetStartLocation().Value());
        }
        if (pending.empty()) break;
        if (unit->guest_blocks + pending.size() > 32) throw std::runtime_error("profitability HIR block cap");
        for (auto address : pending) {
            builder.SetCurBlock(address); ir::Assembler as{&builder};
            swift::arm64::A64Decoder{address, &program, &as}.Decode(); ++unit->guest_blocks;
        }
    }
    function->EndFunction(); std::vector<ir::HIRBlock*> ordered;
    for (auto* hir : function->GetHIRBlocks()) if (hir && hir->GetBlock()->GetStartLocation() == ir::Location{code_base}) ordered.push_back(hir);
    for (auto* hir : function->GetHIRBlocks())
        if (hir && hir->GetBlock()->GetStartLocation() != ir::Location{code_base} && hir->GetBlock()->GetStartLocation() != ir::Location::INVALID && hir->GetBlock()->HasTerminal()) ordered.push_back(hir);
    const auto features = GetSvmConfig().GetFeatureSet();
    for (auto* hir : ordered) {
        auto* body = hir->GetBlock(); x64::ForwardUniformLoads(body, sizeof(Context));
        ir::ConstFoldingPass::Run(body, features); ir::DeadCodeEliminationPass::Run(body, function);
    }
    unit->plan = backend::FunctionPlan::Build(function, ordered);
    auto graph = x64::CfgRegisterPlan::ValidateGraph(unit->plan.body.get(), unit->plan);
    if (!graph.Enabled()) throw std::runtime_error("profitability decoded CFG invalid");
    std::vector<u32> lanes; for (const auto& range : unit->config.loop_gpr_uniform_ranges) lanes.push_back(range.offset);
    unit->promoted = x64::PromoteCfgState(unit->plan, sizeof(Context), lanes, graph, {.reject_dirty_loop_phis = true});
    if (!unit->promoted.accepted || !unit->promoted.loads_replaced) throw std::runtime_error("profitability fixture lacks fixed state promotion");
    unit->fixed_ir = unit->plan.body->ToString();
    unit->code = x64::Translator::PrepareFunction(unit->plan, x64::HostFeatures::Detect(), &unit->config, true, true, route != 0,
        {.uniform_forwarding = false, .state_promotion = false, .scalar_rematerialization = true,
         .scalar_binary_immediates = true, .loop_weighted_costs = true, .profitable_flags_residency = route >= 2, .flags_selection_recolor = route != 3, .scalar_unary_coalescing = true, .scalar_shift_immediates = true});
    if (unit->fixed_ir != unit->plan.body->ToString()) throw std::runtime_error("profitability preparation changed fixed IR");
    unit->code->Translate();
    if (!unit->code->CfgPlan().Enabled() || !unit->code->CfgPlan().Validate() || !unit->code->FramePlan().Enabled() ||
        unit->guest_blocks != hot_blocks + 3 || !unit->code->LoopWeightedCostsEnabled()) throw std::runtime_error("profitability fixture lacks validated complete CFG");
    return unit;
}
void Validate(const Program& program, const Compiled& unit) {
    for (u64 seed : {u64{0}, UINT64_MAX, u64{0x123456789abcdef0}}) for (u32 trips : {1u, 2u, 7u, 19u}) {
        Machine expected, actual; expected.Init(seed, trips); actual.Init(seed, trips); Model(program, expected, trips);
        const auto mxcsr = _mm_getcsr(); const auto reason = unit.Entry()(actual.state);
        if (reason != HaltReason::CallHost || _mm_getcsr() != mxcsr || std::memcmp(actual.bytes.data(), expected.bytes.data(), actual.bytes.size())) {
            std::ostringstream out; out << program.name << " full Context+State independent model mismatch route=" << route_names[unit.route]
                << " seed=" << std::hex << seed << std::dec << " trips=" << trips << " reason=" << u32(reason)
                << " mxcsr=" << _mm_getcsr() << " expected_mxcsr=" << mxcsr;
            for (u32 at = 0; at < actual.bytes.size(); ++at) if (actual.bytes[at] != expected.bytes[at])
                out << " byte=" << at << ":" << unsigned(actual.bytes[at]) << "/" << unsigned(expected.bytes[at]);
            throw std::runtime_error(out.str());
        }
    }
}
void Proof(const Program& program, std::array<std::unique_ptr<Compiled>, route_count>& units) {
    const auto& off = *units[0]; const auto& forced = *units[1]; const auto& profit = *units[2];
    if (off.code->FlagsResident() || !forced.code->FlagsResident() || profit.code->FlagsResident() != program.carry || units[3]->code->FlagsResident() != program.carry)
        throw std::runtime_error("actual profitability decision differs from required sparse-OFF/hot-ON");
    for (const auto& unit : units) {
        const auto& cfg = unit->code->CfgPlan();
        const auto& stats = unit->code->Statistics();
        const u64 expected_carry = program.carry ? globals * hot_blocks : 0;
        if (stats.flags_state_carry_read_sites != (unit->code->FlagsResident() ? 0 : expected_carry) ||
            stats.flags_resident_carry_read_sites != (unit->code->FlagsResident() ? expected_carry : 0))
            throw std::runtime_error("profitability fixture does not exercise actual memory/register carry BT sites");
        if (unit->fixed_ir != off.fixed_ir || cfg.Instructions().size() != off.code->CfgPlan().Instructions().size() ||
            cfg.Blocks().size() != off.code->CfgPlan().Blocks().size() || cfg.Edges().size() != off.code->CfgPlan().Edges().size() ||
            cfg.Allocations().size() != off.code->CfgPlan().Allocations().size() ||
            unit->code->PreparationStats().full_allocation_builds != (unit->route >= 2 ? 0u : 1u) ||
            unit->code->PreparationStats().reused_final_allocation != (unit->route >= 2) || unit->code->UniformForwardStats().allocation_rounds ||
            cfg.PhysicalRegisters().size() != (unit->code->FlagsResident() ? 2u : 3u))
            throw std::runtime_error("profitability pair changed fixed IR/topology or scalar budget contract");
        for (u32 i = 0; i < cfg.Allocations().size(); ++i)
            if (cfg.Allocations()[i].slot.has_value() != off.code->CfgPlan().Allocations()[i].slot.has_value()) ++unit->assignment_changes;
    }
    if (!forced.assignment_changes || (program.carry ? !profit.assignment_changes : profit.assignment_changes != 0))
        throw std::runtime_error("profitability decoded pressure fixture does not exercise allocation presence changes");
    if (!program.carry && (off.code->getSize() != profit.code->getSize() || off.code->FrameSize() != profit.code->FrameSize()))
        throw std::runtime_error("rejected residency did not preserve OFF emission sizes");
    auto& optimized = *units[2]; auto& full = *units[3];
    const auto& a = optimized.code->CfgPlan(); const auto& b = full.code->CfgPlan();
    const auto& da = optimized.code->FlagsDecision(); const auto& db = full.code->FlagsDecision();
    if (da.allocation_builds != 1 || da.allocation_recolors != 1 || db.allocation_builds != 2 || db.allocation_recolors != 0 ||
        optimized.code->PreparationStats().flags_policy_allocation_recolors != 1 ||
        full.code->PreparationStats().flags_policy_allocation_recolors != 0)
        throw std::runtime_error("controlled selection pair did not exercise one build+recolor versus two full builds");
    if (da.selected != db.selected || da.reason != db.reason ||
        da.weighted_flag_reads != db.weighted_flag_reads || da.weighted_flag_writes != db.weighted_flag_writes ||
        da.weighted_carry_reads != db.weighted_carry_reads || da.weighted_helper_calls != db.weighted_helper_calls ||
        da.weighted_exit_boundaries != db.weighted_exit_boundaries || da.resident_overhead != db.resident_overhead ||
        da.scalar_lost_benefit != db.scalar_lost_benefit || da.net_savings != db.net_savings ||
        a.PhysicalRegisters() != b.PhysicalRegisters() || a.Blocks().size() != b.Blocks().size() || a.Edges().size() != b.Edges().size() ||
        a.Instructions().size() != b.Instructions().size() || a.Allocations().size() != b.Allocations().size())
        throw std::runtime_error("controlled selection pair changed decision/graph/budget");
    for (u32 i = 0; i < a.Blocks().size(); ++i) {
        const auto& x = a.Blocks()[i]; const auto& y = b.Blocks()[i];
        if (x.begin != y.begin || x.end != y.end || x.reachable != y.reachable || x.incoming_edges != y.incoming_edges ||
            x.outgoing_edges != y.outgoing_edges || x.loop_depth != y.loop_depth || x.loop_weight != y.loop_weight ||
            x.live_in.size() != y.live_in.size() || x.live_out.size() != y.live_out.size())
            throw std::runtime_error("controlled selection block topology differs");
    }
    const auto ordinal = [](const auto& cfg, const ir::Inst* inst) {
        return std::find(cfg.Instructions().begin(), cfg.Instructions().end(), inst) - cfg.Instructions().begin();
    };
    for (u32 i = 0; i < a.Blocks().size(); ++i) {
        const auto& x = a.Blocks()[i]; const auto& y = b.Blocks()[i];
        for (u32 j = 0; j < x.live_in.size(); ++j)
            if (ordinal(a,x.live_in[j]) != ordinal(b,y.live_in[j])) throw std::runtime_error("controlled selection block live-in differs");
        for (u32 j = 0; j < x.live_out.size(); ++j)
            if (ordinal(a,x.live_out[j]) != ordinal(b,y.live_out[j])) throw std::runtime_error("controlled selection block live-out differs");
    }
    for (u32 i = 0; i < a.Edges().size(); ++i) {
        const auto& x = a.Edges()[i]; const auto& y = b.Edges()[i];
        if (x.from != y.from || x.to != y.to || x.taken != y.taken || x.host_entry != y.host_entry ||
            x.copies.size() != y.copies.size() || x.live.size() != y.live.size())
            throw std::runtime_error("controlled selection edge topology differs");
        for (u32 j = 0; j < x.live.size(); ++j)
            if (ordinal(a,x.live[j]) != ordinal(b,y.live[j])) throw std::runtime_error("controlled selection edge liveness differs");
        for (u32 j = 0; j < x.copies.size(); ++j) {
            const auto& left = x.copies[j]; const auto& right = y.copies[j];
            if (ordinal(a,left.destination) != ordinal(b,right.destination) || left.source.IsValue() != right.source.IsValue() ||
                left.source.IsImm() != right.source.IsImm() ||
                (left.source.IsValue() && (ordinal(a,left.source.value.Def()) != ordinal(b,right.source.value.Def()) || left.source.value.Type() != right.source.value.Type())) ||
                (left.source.IsImm() && left.source.imm.Get() != right.source.imm.Get()))
                throw std::runtime_error("controlled selection parallel Phi copy differs");
        }
    }
    for (u32 i = 0; i < a.Instructions().size(); ++i) {
        const auto* x = a.Instructions()[i]; const auto* y = b.Instructions()[i];
        if (optimized.code->FramePlan().Home(x) != full.code->FramePlan().Home(y) ||
            optimized.code->FramePlan().Decision(x) != full.code->FramePlan().Decision(y) ||
            optimized.code->FramePlan().RematerializedValue(x) != full.code->FramePlan().RematerializedValue(y))
            throw std::runtime_error("controlled selection every-instruction sparse home map differs");
    }
    for (u32 i = 0; i < a.Allocations().size(); ++i) {
        const auto& x = a.Allocations()[i]; const auto& y = b.Allocations()[i];
        if (ordinal(a,x.producer) != ordinal(b,y.producer) || x.slot != y.slot || x.requires_canonical_home != y.requires_canonical_home ||
            x.uses != y.uses || x.scalar_reads != y.scalar_reads || x.estimated_benefit != y.estimated_benefit ||
            x.requires_full_container != y.requires_full_container || x.weighted_reads != y.weighted_reads || x.weighted_benefit != y.weighted_benefit)
            throw std::runtime_error("controlled selection actual coloring/home obligation differs");
    }
    optimized.selection_plan_equal = full.selection_plan_equal = true;
    if (optimized.code->getSize() != full.code->getSize() || optimized.code->FrameSize() != full.code->FrameSize() ||
        std::memcmp(optimized.code->getCode<const u8*>(), full.code->getCode<const u8*>(), optimized.code->getSize()))
        throw std::runtime_error("controlled selection generated code byte-for-byte differs");
    optimized.selection_code_equal = full.selection_code_equal = true;
    std::cerr << "selection_pair workload=" << program.name << " plan_equal=1 code_equal=1 recolor_full_builds=1 recolors=1 baseline_full_builds=2 baseline_recolors=0 final_adopt_both=1\n";
    std::cerr << "policy_fixture workload=" << program.name << " forced_assignment_changes=" << forced.assignment_changes
        << " profitable_assignment_changes=" << profit.assignment_changes << " selected=" << profit.code->FlagsResident()
        << " carry_state_off=" << off.code->Statistics().flags_state_carry_read_sites
        << " carry_resident_forced=" << forced.code->Statistics().flags_resident_carry_read_sites
        << " carry_resident_profitable=" << profit.code->Statistics().flags_resident_carry_read_sites << '\n';
}
void JsonMachine(std::ostream& out, const char* prefix, Machine& machine) {
    out << ",\"" << prefix << "_x\":[";
    for (u32 i = 0; i < 29; ++i) { if (i) out << ','; out << machine.Guest().r[i]; }
    out << ',' << machine.Guest().fp << ',' << machine.Guest().lr << "]"
        << ",\"" << prefix << "_sp\":" << machine.Guest().sp << ",\"" << prefix << "_nzcv\":" << (machine.state->host_cpu_flags & nzcv_mask);
}
void OracleCases(std::ostream& out, Program& program, const std::array<std::unique_ptr<Compiled>, route_count>& units, bool& first) {
    for (u64 seed : {u64{0}, UINT64_MAX, u64{0x123456789abcdef0}}) for (u32 trips : {1u, 2u, 7u, 19u}) {
        Machine input, expected; input.Init(seed, trips); expected.Init(seed, trips); Model(program, expected, trips);
        if (!first) out << ','; first = false;
        out << "{\"workload\":\"" << program.name << "\",\"seed\":" << seed << ",\"trips\":" << trips
            << ",\"code_base\":" << code_base << ",\"stop_pc\":" << program.StopPc() << ",\"words\":[";
        for (size_t i = 0; i < program.words.size(); ++i) { if (i) out << ','; out << program.words[i]; }
        out << ']'; JsonMachine(out, "input", input); JsonMachine(out, "expected", expected);
        for (u32 route = 0; route < route_count; ++route) {
            Machine actual; actual.Init(seed, trips); const auto mxcsr = _mm_getcsr(); const auto reason = units[route]->Entry()(actual.state);
            if (reason != HaltReason::CallHost || _mm_getcsr() != mxcsr || std::memcmp(actual.bytes.data(), expected.bytes.data(), actual.bytes.size()))
                throw std::runtime_error("oracle export full Context+State mismatch");
            const std::string prefix = std::string{"actual_"} + route_names[route]; JsonMachine(out, prefix.c_str(), actual);
        }
        out << '}';
    }
}
void SaveExclusive(const char* path, const std::string& contents) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "exclusive oracle corpus create");
    size_t written{};
    while (written < contents.size()) {
        const auto count = write(fd, contents.data() + written, contents.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { const int error = count < 0 ? errno : EIO; close(fd); throw std::system_error(error, std::generic_category(), "oracle corpus write"); }
        written += size_t(count);
    }
    if (close(fd)) throw std::system_error(errno, std::generic_category(), "oracle corpus close");
}
double Median(std::vector<double> values) { std::sort(values.begin(), values.end()); return values.at(values.size() / 2); }
void EqualDirectPlans(const x64::CfgRegisterPlan& a, const x64::CfgRegisterPlan& b) {
    if (!a.Enabled() || !b.Enabled() || !a.Validate() || !b.Validate() || a.Body() != b.Body() ||
        a.PhysicalRegisters() != b.PhysicalRegisters() || a.Instructions() != b.Instructions() ||
        a.HasLiveness() != b.HasLiveness() || a.HasInterference() != b.HasInterference() ||
        a.LoopWeightedCostsEnabled() != b.LoopWeightedCostsEnabled() || a.ScalarUnaryPhasesEnabled() != b.ScalarUnaryPhasesEnabled() ||
        a.Blocks().size() != b.Blocks().size() || a.Edges().size() != b.Edges().size() || a.Allocations().size() != b.Allocations().size())
        throw std::runtime_error("direct Build/Recolor capabilities/body/budget differ");
    const auto& x = a.ConstructionStats(); const auto& y = b.ConstructionStats();
    if (x.dense_used != y.dense_used || x.sparse_fallback != y.sparse_fallback || x.dense_bytes != y.dense_bytes ||
        x.clique_calls != y.clique_calls || x.natural_loops != y.natural_loops || x.max_loop_depth != y.max_loop_depth)
        throw std::runtime_error("direct Build/Recolor construction statistics differ");
    for (u32 i = 0; i < a.Blocks().size(); ++i) {
        const auto& left = a.Blocks()[i]; const auto& right = b.Blocks()[i];
        if (left.begin != right.begin || left.end != right.end || left.reachable != right.reachable ||
            left.incoming_edges != right.incoming_edges || left.outgoing_edges != right.outgoing_edges ||
            left.live_in != right.live_in || left.live_out != right.live_out ||
            left.loop_depth != right.loop_depth || left.loop_weight != right.loop_weight)
            throw std::runtime_error("direct Build/Recolor topology/liveness/loop costs differ");
        for (u32 j = 0; j < a.Blocks().size(); ++j)
            if (a.DominatesBlock(i,j) != b.DominatesBlock(i,j)) throw std::runtime_error("direct Build/Recolor dominators differ");
    }
    for (u32 i = 0; i < a.Edges().size(); ++i) {
        const auto& left = a.Edges()[i]; const auto& right = b.Edges()[i];
        if (left.from != right.from || left.to != right.to || left.source != right.source || left.taken != right.taken ||
            left.host_entry != right.host_entry || left.live != right.live || left.copies.size() != right.copies.size())
            throw std::runtime_error("direct Build/Recolor edge/Phi topology differs");
        for (u32 j = 0; j < left.copies.size(); ++j) {
            const auto& p = left.copies[j]; const auto& q = right.copies[j];
            if (p.destination != q.destination || p.source.type != q.source.type ||
                (p.source.IsValue() && (p.source.value.Def() != q.source.value.Def() || p.source.value.Type() != q.source.value.Type())) ||
                (p.source.IsImm() && (p.source.imm.Get() != q.source.imm.Get() || p.source.imm.GetType() != q.source.imm.GetType())))
                throw std::runtime_error("direct Build/Recolor parallel Phi copies differ");
        }
    }
    for (u32 i = 0; i < a.Allocations().size(); ++i) {
        const auto& left = a.Allocations()[i]; const auto& right = b.Allocations()[i];
        if (left.producer != right.producer || left.slot != right.slot || left.requires_canonical_home != right.requires_canonical_home ||
            left.requires_full_container != right.requires_full_container || left.uses != right.uses ||
            left.scalar_reads != right.scalar_reads || left.estimated_benefit != right.estimated_benefit ||
            left.weighted_reads != right.weighted_reads || left.weighted_benefit != right.weighted_benefit)
            throw std::runtime_error("direct Build/Recolor colors and home obligations differ");
    }
    for (const auto* left : a.Instructions()) {
        if (a.Register(left) != b.Register(left) || a.Slot(left) != b.Slot(left) ||
            a.RequiresCanonicalHome(left) != b.RequiresCanonicalHome(left) || a.RequiresFullContainer(left) != b.RequiresFullContainer(left))
            throw std::runtime_error("direct Build/Recolor every-instruction allocation facts differ");
        for (const auto* right : a.Instructions())
            if (a.Interferes(left,right) != b.Interferes(left,right) ||
                a.CanReuseBinaryInput(left,right) != b.CanReuseBinaryInput(left,right) ||
                a.CanReuseUnaryInput(left,right) != b.CanReuseUnaryInput(left,right))
                throw std::runtime_error("direct Build/Recolor interference/reuse proof differs");
    }
}
void DirectAnalysisProbe(const Program& program, Compiled& unit, bool timing) {
    const auto* body = unit.plan.body.get();
    const auto final_budget = unit.code->CfgPlan().PhysicalRegisters();
    auto options = x64::CfgRegisterOptions{.physical_registers = {3,5,15},
        .loop_weighted_costs = true, .scalar_unary_phases = true};
    const auto seed = x64::CfgRegisterPlan::Build(body, unit.plan, options);
    const auto original_allocations = seed.Allocations();
    auto recolored = seed.Recolor(final_budget);
    options.physical_registers = final_budget;
    auto built = x64::CfgRegisterPlan::Build(body, unit.plan, options);
    unit.direct_shared_analysis = seed.SharesAnalysisWith(recolored) && recolored.SharesAnalysisWith(seed) && !seed.SharesAnalysisWith(built);
    unit.direct_shared_backing = &seed.Instructions() == &recolored.Instructions() && &seed.Blocks() == &recolored.Blocks() &&
        &seed.Edges() == &recolored.Edges() && &seed.ConstructionStats() == &recolored.ConstructionStats();
    unit.direct_allocations_independent = !seed.Allocations().empty() &&
        &seed.Allocations() != &recolored.Allocations() && seed.Allocations().data() != recolored.Allocations().data() &&
        &built.Allocations() != &recolored.Allocations() && built.Allocations().data() != recolored.Allocations().data();
    if (!unit.direct_shared_analysis || !unit.direct_shared_backing || !unit.direct_allocations_independent)
        throw std::runtime_error("direct Recolor did not share immutable analysis with independent allocation storage");
    EqualDirectPlans(recolored, built); EqualDirectPlans(recolored, unit.code->CfgPlan());
    unit.direct_plan_equal = true;
    std::unordered_set<ir::Inst*> flag_producers;
    for (auto* inst : seed.Instructions()) {
        if (inst->GetOp() == ir::OpCode::SaveFlags || inst->GetOp() == ir::OpCode::BranchOnlyFlags) {
            auto* producer = inst->GetArg<ir::Value>(0).Def();
            if (!producer) throw std::runtime_error("direct probe missing flags producer");
            if (x64::ClassifyProducerFlags(producer->GetOp(), producer->ReturnType(), inst->GetArg<ir::Flags>(1)).needs_metadata)
                flag_producers.insert(producer);
        }
    }
    const auto recolored_frame = x64::SparseFramePlan::Build(body, &unit.plan, recolored, flag_producers, true);
    const auto built_frame = x64::SparseFramePlan::Build(body, &unit.plan, built, flag_producers, true);
    if (!recolored_frame.Validate(body,&unit.plan,recolored,flag_producers,true) || !built_frame.Validate(body,&unit.plan,built,flag_producers,true))
        throw std::runtime_error("direct Build/Recolor sparse frame invalid");
    for (auto* inst : seed.Instructions()) {
        if (recolored_frame.Home(inst) != built_frame.Home(inst) || recolored_frame.Decision(inst) != built_frame.Decision(inst) ||
            recolored_frame.RematerializedValue(inst) != built_frame.RematerializedValue(inst) ||
            recolored_frame.Home(inst) != unit.code->FramePlan().Home(inst) ||
            recolored_frame.Decision(inst) != unit.code->FramePlan().Decision(inst) ||
            recolored_frame.RematerializedValue(inst) != unit.code->FramePlan().RematerializedValue(inst))
            throw std::runtime_error("direct Build/Recolor/production every-instruction home map differs");
    }
    unit.direct_home_equal = true;
    if (timing) {
        std::array<std::vector<double>,2> samples;
        // Existing immutable seed is outside both intervals. Each result is
        // kept alive until after the stop timestamp; validation/destruction
        // are outside the micro-timing intervals and cannot be optimized out.
        for (u32 repeat = 0; repeat < 31; ++repeat) for (u32 order = 0; order < 2; ++order) {
            const auto route = (order + repeat) & 1;
            const auto begin = Clock::now();
            auto sample = route ? seed.Recolor(final_budget) : x64::CfgRegisterPlan::Build(body,unit.plan,options);
            const auto end = Clock::now();
            if (!sample.Validate() || sample.Allocations().size() != built.Allocations().size() ||
                (route && !seed.SharesAnalysisWith(sample)) || (!route && seed.SharesAnalysisWith(sample)))
                throw std::runtime_error("timed direct Build/Recolor result or sharing invalid");
            samples[route].push_back(std::chrono::duration<double,std::micro>(end-begin).count());
        }
        unit.cfg_full_build_us = Median(samples[0]); unit.cfg_shared_recolor_us = Median(samples[1]);
    }
    if (seed.Allocations().size() != original_allocations.size()) throw std::runtime_error("direct seed allocations changed");
    for (u32 i = 0; i < original_allocations.size(); ++i) {
        const auto& a = seed.Allocations()[i]; const auto& b = original_allocations[i];
        if (a.producer != b.producer || a.slot != b.slot || a.requires_canonical_home != b.requires_canonical_home ||
            a.requires_full_container != b.requires_full_container || a.uses != b.uses || a.scalar_reads != b.scalar_reads ||
            a.estimated_benefit != b.estimated_benefit || a.weighted_reads != b.weighted_reads || a.weighted_benefit != b.weighted_benefit)
            throw std::runtime_error("direct Recolor changed seed allocation facts");
    }
    if (unit.fixed_ir != unit.plan.body->ToString()) throw std::runtime_error("direct probe changed fixed IR");
    unit.direct_seed_unchanged = true;
    std::cerr << "shared_analysis_pair workload=" << program.name << " route=" << route_names[unit.route]
        << " analysis_identity=1 reference_backing=1 allocations_independent=1 direct_plan_equal=1 direct_home_equal=1 seed_unchanged=1\n";
}
std::array<double, route_count> Measure(const std::array<std::unique_ptr<Compiled>, route_count>& units, u32 calls, u32 trips) {
    std::array<Machine, route_count> machines; std::array<std::vector<double>, route_count> samples;
    for (u32 route = 0; route < route_count; ++route) {
        machines[route].Init(0x123456789abcdef0, trips);
        for (u32 warm = 0; warm < 32; ++warm) {
            machines[route].Guest().r[28] = trips; machines[route].state->halt_reason = HaltReason::None;
            if (units[route]->Entry()(machines[route].state) != HaltReason::CallHost) throw std::runtime_error("profitability warmup exit");
        }
    }
    for (u32 repeat = 0; repeat < 7; ++repeat) for (u32 order = 0; order < route_count; ++order) {
        const u32 route = (order + repeat) % route_count; auto& machine = machines[route]; const auto entry = units[route]->Entry(); const auto begin = Clock::now();
        for (u32 call = 0; call < calls; ++call) {
            machine.Guest().r[28] = trips; machine.state->halt_reason = HaltReason::None;
            if (entry(machine.state) != HaltReason::CallHost) throw std::runtime_error("profitability timing exit");
        }
        samples[route].push_back(std::chrono::duration<double, std::nano>(Clock::now() - begin).count() / calls);
    }
    std::array<double, route_count> result{}; for (u32 route = 0; route < route_count; ++route) result[route] = Median(samples[route]); return result;
}
u32 PositiveArgument(const char* value) {
    size_t consumed{}; const auto number = std::stoull(value, &consumed);
    if (consumed != std::strlen(value) || !number || number > std::numeric_limits<u32>::max()) throw std::runtime_error("calls/trips require positive uint32");
    return u32(number);
}
} // namespace
int main(int argc, char** argv) {
    try {
        const bool validate_only = argc == 2 && std::string{argv[1]} == "--validate-only";
        const bool oracle = argc == 3 && std::string{argv[1]} == "--oracle-corpus";
        const bool correctness_only = validate_only || oracle;
        if (!correctness_only && argc > 3) throw std::runtime_error("usage: benchmark [calls [trips]], --validate-only, --oracle-corpus NEW_FILE");
        const u32 calls = correctness_only || argc < 2 ? 500 : PositiveArgument(argv[1]);
        const u32 trips = correctness_only || argc < 3 ? 64 : PositiveArgument(argv[2]);
        std::cerr << "scope=" << scope << "; fixed_ir_before_policy=1; fixed_state_promotion=1; allocation_feedback=0; "
                     "oracle=independent_full_context_and_state; transfer_counts=static_sites; loop_weights=structural_estimate; "
                     "application_performance_claim=0; dynamic_spill_claim=0\n";
        std::ostringstream corpus; bool first_case = true;
        if (oracle) corpus << "{\"schema_version\":1,\"scope\":\"" << scope << "\",\"cases\":[";
        if (!correctness_only) std::cout << "workload,route,cfg_allocation,flags_requested,profitable_flags_residency,flags_selected,guest_words,guest_blocks,cfg_blocks,trips,"
            "physical_registers,assigned_values,assignment_changes,physical_home_slots,retained_home_slots,home_bytes,frame_bytes,code_bytes,"
            "observed_ssa_load_sites,observed_ssa_store_sites,ssa_load_bytes,ssa_store_bytes,observed_uniform_load_sites,observed_uniform_store_sites,"
            "flags_state_load_sites,flags_state_store_sites,flags_resident_merge_sites,flags_resident_update_sites,"
            "flags_selection_recolor,selection_plan_equal,selection_code_equal,policy_reason,policy_allocation_builds,policy_allocation_recolors,weighted_flag_reads,weighted_flag_writes,weighted_carry_reads,weighted_helper_calls,weighted_exit_boundaries,"
            "resident_overhead,scalar_lost_benefit,net_savings,preparation_flags_policy_allocation_recolors,preparation_full_allocation_builds,preparation_reused_final_allocation,fixed_promoted_load_ir,forward_allocation_rounds,"
            "direct_shared_analysis,direct_shared_backing,direct_allocations_independent,direct_plan_equal,direct_home_equal,direct_seed_unchanged,"
            "cfg_full_build_us,cfg_shared_recolor_us,decode_plan_prepare_emit_us,native_ns_per_call,native_ns_per_guest_iteration,guest_instructions_per_second\n";
        for (bool carry : {false, true}) {
            auto program = MakeProgram(carry); std::array<std::unique_ptr<Compiled>, route_count> units;
            std::array<std::vector<double>, route_count> preparation;
            if (!correctness_only) for (u32 repeat = 0; repeat < 9; ++repeat) for (u32 order = 0; order < route_count; ++order) {
                const u32 route = (order + repeat) % route_count; const auto begin = Clock::now(); auto sample = Compile(program, route);
                preparation[route].push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
            }
            for (u32 route = 0; route < route_count; ++route) { units[route] = Compile(program, route); Validate(program, *units[route]); }
            Proof(program, units);
            for (auto& unit : units) DirectAnalysisProbe(program, *unit, !correctness_only);
            if (oracle) OracleCases(corpus, program, units, first_case); if (correctness_only) continue;
            const auto ns = Measure(units, calls, trips);
            for (u32 route = 0; route < route_count; ++route) {
                const auto& unit = *units[route]; const auto& code = *unit.code; const auto& cfg = code.CfgPlan();
                const auto& frame = code.FramePlan().Statistics(); const auto& stats = code.Statistics(); const auto& policy = code.FlagsDecision();
                const auto& ssa = stats.At(x64::TransferRegion::CanonicalSSA); const auto& uniform = stats.At(x64::TransferRegion::Uniform);
                const auto assigned = std::count_if(cfg.Allocations().begin(), cfg.Allocations().end(), [](const auto& a) { return a.slot.has_value(); });
                std::cout << std::fixed << std::setprecision(2) << program.name << ',' << route_names[route] << ",1," << (route != 0) << ',' << (route >= 2) << ',' << code.FlagsResident()
                    << ',' << program.words.size() << ',' << unit.guest_blocks << ',' << cfg.Blocks().size() << ',' << trips
                    << ',' << cfg.PhysicalRegisters().size() << ',' << assigned << ',' << unit.assignment_changes
                    << ',' << frame.home_count << ',' << frame.retained_homes << ',' << frame.home_bytes << ',' << code.FrameSize() << ',' << code.getSize()
                    << ',' << ssa.scalar_load_sites + ssa.vector_load_sites << ',' << ssa.scalar_store_sites + ssa.vector_store_sites << ',' << ssa.load_bytes << ',' << ssa.store_bytes
                    << ',' << uniform.scalar_load_sites + uniform.vector_load_sites << ',' << uniform.scalar_store_sites + uniform.vector_store_sites
                    << ',' << stats.flags_state_load_sites << ',' << stats.flags_state_store_sites << ',' << stats.flags_resident_merge_sites << ',' << stats.flags_resident_update_sites
                    << ',' << (route != 3) << ',' << unit.selection_plan_equal << ',' << unit.selection_code_equal
                    << ',' << static_cast<u32>(policy.reason) << ',' << policy.allocation_builds << ',' << policy.allocation_recolors
                    << ',' << policy.weighted_flag_reads << ',' << policy.weighted_flag_writes << ',' << policy.weighted_carry_reads << ',' << policy.weighted_helper_calls << ',' << policy.weighted_exit_boundaries
                    << ',' << policy.resident_overhead << ',' << policy.scalar_lost_benefit << ',' << policy.net_savings
                    << ',' << code.PreparationStats().flags_policy_allocation_recolors << ',' << code.PreparationStats().full_allocation_builds << ',' << code.PreparationStats().reused_final_allocation << ',' << unit.promoted.loads_replaced << ',' << code.UniformForwardStats().allocation_rounds
                    << ',' << unit.direct_shared_analysis << ',' << unit.direct_shared_backing << ',' << unit.direct_allocations_independent
                    << ',' << unit.direct_plan_equal << ',' << unit.direct_home_equal << ',' << unit.direct_seed_unchanged
                    << ',' << unit.cfg_full_build_us << ',' << unit.cfg_shared_recolor_us << ',' << Median(preparation[route]) << ',' << ns[route] << ',' << ns[route] / trips << ',' << double(program.Instructions(trips)) * 1e9 / ns[route] << '\n';
            }
        }
        if (oracle) { corpus << "]}\n"; SaveExclusive(argv[2], corpus.str()); std::cerr << "oracle_cases=24; exclusive_corpus=1\n"; }
        if (correctness_only) std::cerr << "validated_routes=8; seeds_per_route=3; trip_counts=1,2,7,19; sparse_rejected=1; hot_carry_selected=1; scalar_budget_assignment_change=1; controlled_recolor_pair_code_equal=1; actual_shared_analysis_backing=1; direct_full_build_recolor_plan_home_equal=1\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "CFG shared analysis benchmark: " << error.what() << '\n'; return 1; }
}
