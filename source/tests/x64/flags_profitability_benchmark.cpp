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
constexpr u32 globals = 4, hot_blocks = 3, route_count = 3;
constexpr const char* scope = "decoded_a64_flags_profitability_three_routes";
constexpr std::array<const char*, route_count> route_names{"off", "forced", "profitable"};
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
         .scalar_binary_immediates = true, .loop_weighted_costs = true, .profitable_flags_residency = route == 2});
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
    if (off.code->FlagsResident() || !forced.code->FlagsResident() || profit.code->FlagsResident() != program.carry)
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
            unit->code->PreparationStats().full_allocation_builds != 1 || unit->code->UniformForwardStats().allocation_rounds ||
            cfg.PhysicalRegisters().size() != (unit->code->FlagsResident() ? 2u : 3u))
            throw std::runtime_error("profitability pair changed fixed IR/topology or scalar budget contract");
        for (u32 i = 0; i < cfg.Allocations().size(); ++i)
            if (cfg.Allocations()[i].slot.has_value() != off.code->CfgPlan().Allocations()[i].slot.has_value()) ++unit->assignment_changes;
    }
    if (!forced.assignment_changes || (program.carry ? !profit.assignment_changes : profit.assignment_changes != 0))
        throw std::runtime_error("profitability decoded pressure fixture does not exercise allocation presence changes");
    if (!program.carry && (off.code->getSize() != profit.code->getSize() || off.code->FrameSize() != profit.code->FrameSize()))
        throw std::runtime_error("rejected residency did not preserve OFF emission sizes");
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
            "policy_reason,policy_allocation_builds,weighted_flag_reads,weighted_flag_writes,weighted_carry_reads,weighted_helper_calls,weighted_exit_boundaries,"
            "resident_overhead,scalar_lost_benefit,net_savings,preparation_full_allocation_builds,preparation_reused_final_allocation,fixed_promoted_load_ir,forward_allocation_rounds,"
            "decode_plan_prepare_emit_us,native_ns_per_call,native_ns_per_guest_iteration,guest_instructions_per_second\n";
        for (bool carry : {false, true}) {
            auto program = MakeProgram(carry); std::array<std::unique_ptr<Compiled>, route_count> units;
            std::array<std::vector<double>, route_count> preparation;
            if (!correctness_only) for (u32 repeat = 0; repeat < 9; ++repeat) for (u32 order = 0; order < route_count; ++order) {
                const u32 route = (order + repeat) % route_count; const auto begin = Clock::now(); auto sample = Compile(program, route);
                preparation[route].push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
            }
            for (u32 route = 0; route < route_count; ++route) { units[route] = Compile(program, route); Validate(program, *units[route]); }
            Proof(program, units); if (oracle) OracleCases(corpus, program, units, first_case); if (correctness_only) continue;
            const auto ns = Measure(units, calls, trips);
            for (u32 route = 0; route < route_count; ++route) {
                const auto& unit = *units[route]; const auto& code = *unit.code; const auto& cfg = code.CfgPlan();
                const auto& frame = code.FramePlan().Statistics(); const auto& stats = code.Statistics(); const auto& policy = code.FlagsDecision();
                const auto& ssa = stats.At(x64::TransferRegion::CanonicalSSA); const auto& uniform = stats.At(x64::TransferRegion::Uniform);
                const auto assigned = std::count_if(cfg.Allocations().begin(), cfg.Allocations().end(), [](const auto& a) { return a.slot.has_value(); });
                std::cout << std::fixed << std::setprecision(2) << program.name << ',' << route_names[route] << ",1," << (route != 0) << ',' << (route == 2) << ',' << code.FlagsResident()
                    << ',' << program.words.size() << ',' << unit.guest_blocks << ',' << cfg.Blocks().size() << ',' << trips
                    << ',' << cfg.PhysicalRegisters().size() << ',' << assigned << ',' << unit.assignment_changes
                    << ',' << frame.home_count << ',' << frame.retained_homes << ',' << frame.home_bytes << ',' << code.FrameSize() << ',' << code.getSize()
                    << ',' << ssa.scalar_load_sites + ssa.vector_load_sites << ',' << ssa.scalar_store_sites + ssa.vector_store_sites << ',' << ssa.load_bytes << ',' << ssa.store_bytes
                    << ',' << uniform.scalar_load_sites + uniform.vector_load_sites << ',' << uniform.scalar_store_sites + uniform.vector_store_sites
                    << ',' << stats.flags_state_load_sites << ',' << stats.flags_state_store_sites << ',' << stats.flags_resident_merge_sites << ',' << stats.flags_resident_update_sites
                    << ',' << static_cast<u32>(policy.reason) << ',' << policy.allocation_builds
                    << ',' << policy.weighted_flag_reads << ',' << policy.weighted_flag_writes << ',' << policy.weighted_carry_reads << ',' << policy.weighted_helper_calls << ',' << policy.weighted_exit_boundaries
                    << ',' << policy.resident_overhead << ',' << policy.scalar_lost_benefit << ',' << policy.net_savings
                    << ',' << code.PreparationStats().full_allocation_builds << ',' << code.PreparationStats().reused_final_allocation << ',' << unit.promoted.loads_replaced << ',' << code.UniformForwardStats().allocation_rounds
                    << ',' << Median(preparation[route]) << ',' << ns[route] << ',' << ns[route] / trips << ',' << double(program.Instructions(trips)) * 1e9 / ns[route] << '\n';
            }
        }
        if (oracle) { corpus << "]}\n"; SaveExclusive(argv[2], corpus.str()); std::cerr << "oracle_cases=24; exclusive_corpus=1\n"; }
        if (correctness_only) std::cerr << "validated_routes=6; seeds_per_route=3; trip_counts=1,2,7,19; sparse_rejected=1; hot_carry_selected=1; scalar_budget_assignment_change=1\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "flags profitability benchmark: " << error.what() << '\n'; return 1; }
}
