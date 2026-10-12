#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include "runtime/backend/x64/cfg_uniform_forward.h"
#include "runtime/backend/x64/cfg_state_promote.h"
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

// Only this test executable defines the friend. No product runtime hook or
// alternate allocation is introduced by the read-only diagnostic view.
namespace swift::runtime::backend::x64 {
struct CfgDiagnosticProbe {
    static const auto& Homes(const Translator& code) { return code.homes; }
    static const auto& Labels(const Translator& code) { return code.labels; }
    static std::optional<unsigned> Register(const Translator& code, ir::Inst* inst) {
        return code.AllocatedScalarRegister(inst);
    }
    static bool RequiresHome(const Translator& code, ir::Inst* inst) { return code.RequiresScalarHome(inst); }
    static bool FlagProducer(const Translator& code, ir::Inst* inst) { return code.flag_producers.contains(inst); }
    static u32 PhiScratch(const Translator& code) { return code.phi_scratch; }
    static u32 MaxPhiMoves(const Translator& code) { return code.max_phi_moves; }
};
}

namespace {
constexpr u64 code_base = 0x1000;
constexpr u64 nzcv_mask = u64{15} << 28;
enum class Kind { Add, Xor, Compare };
struct Operation {
    Kind kind;
    u32 dst, lhs, rhs;
    bool wide{true}, flags{}, carry{}, subtract{};
    u32 fallback{};
};
struct Program final : MemoryInterface {
    std::vector<u32> words;
    std::vector<Operation> operations;
    std::string name;
    u64 StopPc() const { return code_base + (words.size() - 1) * 4; }
    bool Read(void* out, size_t address, size_t size) override {
        if (address < code_base || address - code_base > words.size() * 4 ||
            size > words.size() * 4 - (address - code_base)) return false;
        std::memcpy(out, reinterpret_cast<const u8*>(words.data()) + address - code_base, size); return true;
    }
    bool Write(void*, size_t, size_t) override { return false; }
    void* GetPointer(void* address) override {
        const auto at = reinterpret_cast<uintptr_t>(address);
        if (at < code_base || at - code_base > words.size() * 4 ||
            words.size() * 4 - (at - code_base) < 4) return nullptr;
        return reinterpret_cast<u8*>(words.data()) + at - code_base;
    }
    void Emit(Operation op) {
        u32 encoding{};
        if (op.kind == Kind::Compare) {
            // CCMP/CCMN register, condition EQ. The descriptor is an input to
            // the independent integer model, not a decoder/IR result.
            encoding = (op.subtract ? 0x7a400000u : 0x3a400000u) |
                       (op.rhs << 16) | (op.lhs << 5) | op.fallback;
        } else if (op.kind == Kind::Xor)
            encoding = 0x4a000000u | (op.rhs << 16) | (op.lhs << 5) | op.dst;
        else {
            encoding = op.carry ? 0x1a000000u : 0x0b000000u;
            if (op.flags) encoding |= 0x20000000u;
            encoding |= (op.rhs << 16) | (op.lhs << 5) | op.dst;
        }
        if (op.wide) encoding |= 0x80000000u;
        words.push_back(encoding); operations.push_back(op);
    }
    void Finish() {
        // SUB X28,X28,#1 preserves carry; CBNZ returns to the actual guest
        // root through a FunctionPlan edge, without a host call per iteration.
        words.push_back(0xd1000000u | (1u << 10) | (28u << 5) | 28u);
        const auto branch_index = u32(words.size());
        const auto displacement = (u32{0} - branch_index) & 0x7ffff;
        words.push_back(0xb5000000u | (displacement << 5) | 28u);
        words.push_back(0xd4200000u);
    }
};
Program MakeProgram(const std::string& name) {
    Program program; program.name = name;
    if (name == "pressure_loop") {
        for (u32 i = 0; i < 64; ++i)
            program.Emit({Kind::Add, i % 24, i % 24, (i + 7) % 24, bool(i & 1)});
    } else if (name == "conditional_diamond_loop") {
        for (u32 i = 0; i < 8; ++i) {
            program.Emit({Kind::Add, 0, 0, 1, bool(i & 1)});
            program.Emit({Kind::Compare, 0, 0, 2, bool(i & 1), true, false, bool(i & 2), (i * 3 + 1) & 15});
            program.Emit({Kind::Add, 3, 0, 4, true});
            program.Emit({Kind::Xor, 5, 3, 6, true});
        }
    } else {
        program.Emit({Kind::Add, 0, 0, 1, true, true});
        for (u32 i = 0; i < 62; ++i) {
            if (i == 30) program.Emit({Kind::Add, 2, 2, 3, true, true, true});
            else program.Emit({Kind::Add, 4 + i % 20, 4 + i % 20, 4 + (i + 7) % 20, bool(i & 1)});
        }
        program.Emit({Kind::Add, 7, 7, 8, true, true, true});
    }
    program.Finish(); return program;
}

struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + sizeof(Context)> bytes{};
    backend::State* state = new(bytes.data()) backend::State{};
    ~Machine() { state->~State(); }
    Context& Guest() { return *reinterpret_cast<Context*>(state->uniform_buffer_begin); }
    void Init(u64 seed, u32 trips) {
        auto* context = reinterpret_cast<u8*>(&Guest());
        for (u32 i = 0; i < sizeof(Context); ++i) context[i] = u8(seed + i * 37 + 0x59);
        for (u32 i = 0; i < 29; ++i) Guest().r[i] = seed ^ (0x9e3779b97f4a7c15ULL * (i + 1));
        Guest().r[28] = trips; Guest().fp = seed ^ 0x3141592653589793ULL;
        Guest().lr = ~seed; Guest().sp = 0x8000;
        state->host_cpu_flags = (seed ^ 0xabcd321001234567ULL) & ~nzcv_mask;
        state->host_cpu_flags |= ((seed >> 4) & 15) << 28;
        state->current_loc = ir::Location{0xfeedface}; state->halt_reason = HaltReason::None;
    }
};
u64 ArithmeticFlags(u64 old, u64 lhs, u64 rhs, bool subtract, u32 bits, u64 carry) {
    const u64 mask = bits == 64 ? UINT64_MAX : UINT32_MAX;
    lhs &= mask; rhs &= mask;
    const unsigned __int128 sum = static_cast<unsigned __int128>(lhs) + rhs + carry;
    const u64 result = (subtract ? lhs - rhs : u64(sum)) & mask;
    const auto signed_value = [bits](u64 value) -> __int128 {
        return value & (u64{1} << (bits - 1)) ? __int128(value) - (__int128{1} << bits) : __int128(value);
    };
    const auto mathematical = subtract ? signed_value(lhs) - signed_value(rhs)
                                      : signed_value(lhs) + signed_value(rhs) + carry;
    const bool overflow = mathematical < -(__int128{1} << (bits - 1)) ||
                          mathematical >= (__int128{1} << (bits - 1));
    const bool cout = subtract ? lhs >= rhs : bool(sum >> bits);
    return (old & ~nzcv_mask) | (((result >> (bits - 1)) & 1) << 31) |
           (u64(result == 0) << 30) | (u64(cout) << 29) | (u64(overflow) << 28);
}
void Model(const Program& program, Machine& machine, u32 trips) {
    auto& regs = machine.Guest().r;
    auto& flags = machine.state->host_cpu_flags;
    for (u32 trip = 0; trip < trips; ++trip) {
        for (const auto& op : program.operations) {
            const u32 bits = op.wide ? 64 : 32;
            const u64 mask = op.wide ? UINT64_MAX : UINT32_MAX;
            const u64 lhs = regs[op.lhs] & mask, rhs = regs[op.rhs] & mask;
            if (op.kind == Kind::Compare) {
                flags = flags & (u64{1} << 30) ? ArithmeticFlags(flags, lhs, rhs, op.subtract, bits, 0)
                                              : (flags & ~nzcv_mask) | (u64(op.fallback) << 28);
            } else if (op.kind == Kind::Xor) regs[op.dst] = (lhs ^ rhs) & mask;
            else {
                const u64 carry = op.carry ? (flags >> 29) & 1 : 0;
                regs[op.dst] = (lhs + rhs + carry) & mask;
                if (op.flags) flags = ArithmeticFlags(flags, lhs, rhs, false, bits, carry);
            }
        }
        --regs[28];
    }
    machine.Guest().padding = u32(swift::arm64::InterruptReason::BRK);
    machine.state->current_loc = ir::Location{program.StopPc()};
    machine.state->halt_reason = HaltReason::CallHost;
}

using ForwardChange = x64::CfgUniformForwardChange;
struct Compiled {
    Config config{.backend_isa = kX86_64, .uniform_buffer_size = sizeof(Context),
                  .loop_gpr_uniform_ranges = swift::arm64::Arm64GprUniformRanges(), .stack_alignment = 16};
    backend::FunctionPlan plan;
    x64::CfgUniformForwardStats forward;
    x64::CfgStatePromoteStats promoted;
    x64::CfgRegisterPlan allocation;
    std::unique_ptr<x64::Translator> translator;
    std::vector<ForwardChange> forward_changes;
    u32 guest_blocks{};
    Fn Entry() const { return translator->getCode<Fn>(); }
};
std::unique_ptr<Compiled> Compile(Program& program, u32 route, bool capture = false) {
    auto result = std::make_unique<Compiled>();
    ir::HIRBuilder builder{1, true}; auto* function = builder.AppendFunction(ir::Location{code_base});
    for (;;) {
        std::vector<u64> pending;
        for (auto& hir : function->GetHIRBlockList()) {
            auto* block = hir.GetBlock();
            if (block->GetInstList().empty() && !block->HasTerminal()) pending.push_back(block->GetStartLocation().Value());
        }
        if (pending.empty()) break;
        if (result->guest_blocks + pending.size() > 32) throw std::runtime_error("benchmark HIR block cap");
        for (auto address : pending) {
            builder.SetCurBlock(address); ir::Assembler as{&builder};
            swift::arm64::A64Decoder{address, &program, &as}.Decode(); ++result->guest_blocks;
        }
    }
    function->EndFunction();
    std::vector<ir::HIRBlock*> ordered;
    for (auto* hir : function->GetHIRBlocks())
        if (hir && hir->GetBlock()->GetStartLocation() == ir::Location{code_base}) ordered.push_back(hir);
    for (auto* hir : function->GetHIRBlocks())
        if (hir && hir->GetBlock()->GetStartLocation() != ir::Location{code_base} &&
            hir->GetBlock()->GetStartLocation() != ir::Location::INVALID && hir->GetBlock()->HasTerminal()) ordered.push_back(hir);
    // Every route starts from the current production optimizations. Perform
    // DCE before lowering, while HIR owns its instructions and terminals;
    // FunctionPlan's synthetic edge/exit/location metadata must remain intact.
    const auto features = GetSvmConfig().GetFeatureSet();
    for (auto* hir : ordered) {
        auto* block = hir->GetBlock();
        x64::ForwardUniformLoads(block, sizeof(Context));
        ir::ConstFoldingPass::Run(block, features);
        ir::DeadCodeEliminationPass::Run(block, function);
    }
    result->plan = backend::FunctionPlan::Build(function, ordered);
    result->translator = x64::Translator::PrepareFunction(result->plan, x64::HostFeatures::Detect(),
            &result->config, true, bool(route & 1), bool(route & 4),
            {.uniform_forwarding = bool(route & 2), .state_promotion = bool((route & 3) == 3),
             .capture_forward_changes = capture});
    result->promoted = result->translator->StatePromotionStats();
    result->forward = result->translator->UniformForwardStats();
    result->forward_changes = result->translator->ForwardChanges();
    if ((route & 3) == 3 && !result->promoted.accepted)
        throw std::runtime_error("benchmark CFG state promotion rejected");
    if ((route & 2) && !result->forward.accepted)
        throw std::runtime_error("benchmark CFG uniform forwarding rejected");
    result->translator->Translate();
    // Reuse the emitter's actual assignment (including adaptive RBX budget).
    // Baseline diagnostics need topology only, not a duplicate allocator.
    result->allocation = route & 1 ? result->translator->CfgPlan()
                                  : x64::CfgRegisterPlan::ValidateGraph(result->plan.body.get(), result->plan);
    if (!result->allocation.Enabled()) throw std::runtime_error("benchmark rewritten CFG validation failed");
    if (result->guest_blocks < 2 || result->plan.targets.empty())
        throw std::runtime_error("benchmark did not decode a shared guest loop");
    return result;
}
void Validate(const Program& program, const Compiled& compiled) {
    for (u64 seed : {u64{0}, UINT64_MAX, u64{0x123456789abcdef0}, u64{0x8000000000000040}})
    for (u32 trips : {1u, 2u, 7u}) {
        Machine expected, actual; expected.Init(seed, trips); actual.Init(seed, trips);
        Model(program, expected, trips);
        const auto reason = compiled.Entry()(actual.state);
        if (reason != HaltReason::CallHost || actual.state->halt_reason != expected.state->halt_reason ||
            std::memcmp(&actual.Guest(), &expected.Guest(), sizeof(Context)) ||
            actual.state->host_cpu_flags != expected.state->host_cpu_flags ||
            actual.state->current_loc != expected.state->current_loc)
            throw std::runtime_error(program.name + " shared CFG full-state integer-model mismatch");
    }
}
double Median(std::vector<double> values) {
    std::sort(values.begin(), values.end()); return values.at(values.size() / 2);
}
std::array<double, 8> Measure(const std::array<std::unique_ptr<Compiled>, 8>& compiled,
                              u32 calls, u32 trips) {
    std::array<Machine, 8> machines;
    std::array<std::vector<double>, 8> samples;
    for (u32 route = 0; route < 8; ++route) {
        machines[route].Init(0x123456789abcdef0, trips);
        for (u32 warm = 0; warm < 32; ++warm) {
            machines[route].Guest().r[28] = trips; machines[route].state->halt_reason = HaltReason::None;
            if (compiled[route]->Entry()(machines[route].state) != HaltReason::CallHost) throw std::runtime_error("warmup exit");
        }
    }
    for (u32 repeat = 0; repeat < 7; ++repeat) for (u32 order = 0; order < 8; ++order) {
        const u32 route = (order + repeat) & 7;
        auto& machine = machines[route]; const auto entry = compiled[route]->Entry();
        const auto begin = Clock::now();
        for (u32 call = 0; call < calls; ++call) {
            machine.Guest().r[28] = trips; machine.state->halt_reason = HaltReason::None;
            if (entry(machine.state) != HaltReason::CallHost) throw std::runtime_error("measurement exit");
        }
        samples[route].push_back(std::chrono::duration<double, std::nano>(Clock::now() - begin).count() / calls);
    }
    std::array<double, 8> result;
    for (u32 route = 0; route < 8; ++route) result[route] = Median(samples[route]);
    return result;
}

void JsonString(std::ostream& out, const std::string& value) {
    constexpr char hex[] = "0123456789abcdef";
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << char(c);
        else if (c < 32) out << "\\u00" << hex[c >> 4] << hex[c & 15];
        else out << char(c);
    }
    out << '"';
}
using Positions = std::unordered_map<const ir::Inst*, u32>;
void JsonValue(std::ostream& out, ir::Value value, const Positions& positions) {
    out << "{\"kind\":\"Value\",\"def\":";
    if (value.Defined()) out << positions.at(value.Def()); else out << "null";
    out << ",\"view_type\":";
    if (value.Defined()) JsonString(out, ir::ValueTypeString(value.Type())); else out << "null";
    out << '}';
}
void JsonData(std::ostream& out, const ir::DataClass& data, const Positions& positions) {
    if (data.IsValue()) JsonValue(out, data.value, positions);
    else if (data.IsImm()) {
        out << "{\"kind\":\"Imm\",\"value\":" << data.imm.Get() << ",\"type\":";
        JsonString(out, ir::ValueTypeString(data.imm.GetType())); out << '}';
    } else out << "{\"kind\":\"Void\"}";
}
void JsonArg(std::ostream& out, ir::Arg& arg, const Positions& positions) {
    if (arg.IsValue() || arg.IsImm() || arg.IsVoid()) { JsonData(out, arg.ToDataClass(), positions); return; }
    out << "{\"kind\":"; JsonString(out, ir::ArgTypeString(arg.GetType()));
    switch (arg.GetType()) {
        case ir::ArgType::Uniform: {
            const auto uniform = arg.Get<ir::Uniform>();
            out << ",\"offset\":" << uniform.GetOffset() << ",\"type\":";
            JsonString(out, ir::ValueTypeString(uniform.GetType())); break;
        }
        case ir::ArgType::Local: {
            const auto local = arg.Get<ir::Local>(); out << ",\"id\":" << local.id << ",\"type\":";
            JsonString(out, ir::ValueTypeString(local.type)); break;
        }
        case ir::ArgType::Cond: out << ",\"value\":" << u32(arg.Get<ir::Cond>()); break;
        case ir::ArgType::Flags: out << ",\"value\":" << u64(arg.Get<ir::Flags>()); break;
        case ir::ArgType::Operand: {
            const auto op = arg.Get<ir::OperandOp>();
            out << ",\"operator\":" << u32(op.type) << ",\"shift_ext\":" << u32(op.shift_ext); break;
        }
        case ir::ArgType::Lambda: {
            const auto lambda = arg.Get<ir::Lambda>(); out << ",\"address\":";
            if (lambda.IsValue()) JsonValue(out, lambda.GetValue(), positions);
            else if (lambda.IsImm()) JsonData(out, ir::DataClass{lambda.GetImm()}, positions);
            else out << "null";
            out << ",\"uniform_effect_id\":" << u32(lambda.GetUniformEffectId())
                << ",\"helper_abi\":" << u32(lambda.GetHelperABI()); break;
        }
        case ir::ArgType::Params: {
            out << ",\"values\":["; bool first = true;
            for (const auto& param : arg.Get<ir::Params>()) {
                if (!first) out << ','; first = false; JsonData(out, param.data, positions);
            }
            out << ']'; break;
        }
        default: throw std::runtime_error("diagnostic unsupported argument kind");
    }
    out << '}';
}
void JsonIds(std::ostream& out, const std::vector<ir::Inst*>& values, const Positions& positions) {
    out << '[';
    for (size_t i = 0; i < values.size(); ++i) { if (i) out << ','; out << positions.at(values[i]); }
    out << ']';
}
void JsonOptional(std::ostream& out, std::optional<unsigned> value) {
    if (value) out << *value; else out << "null";
}
void JsonAllocation(std::ostream& out, ir::Inst* inst, const Compiled& unit) {
    const auto& code = *unit.translator; const auto& cfg = code.CfgPlan();
    out << "{\"register\":"; JsonOptional(out, x64::CfgDiagnosticProbe::Register(code, inst));
    out << ",\"cfg_requires_canonical_home\":" << cfg.RequiresCanonicalHome(inst);
    out << ",\"requires_canonical_home\":" << x64::CfgDiagnosticProbe::RequiresHome(code, inst)
        << ",\"cfg_enabled\":" << cfg.Enabled() << ",\"cfg_details\":";
    const auto found = std::find_if(cfg.Allocations().begin(), cfg.Allocations().end(),
                                   [&](const auto& value) { return value.producer == inst; });
    if (found == cfg.Allocations().end()) out << "null";
    else {
        out << "{\"slot\":"; JsonOptional(out, found->slot);
        out << ",\"uses\":" << found->uses << ",\"scalar_reads\":" << found->scalar_reads
            << ",\"estimated_benefit\":" << found->estimated_benefit
            << ",\"weighted_reads\":" << found->weighted_reads << ",\"weighted_benefit\":" << found->weighted_benefit
            << ",\"requires_full_container\":" << found->requires_full_container << '}';
    }
    out << ",\"legacy_details\":";
    if (const auto* legacy = code.ScalarPlan().Find(inst))
        out << "{\"slot\":" << legacy->slot << ",\"definition_position\":" << legacy->definition_position
            << ",\"last_use\":" << legacy->last_use << ",\"segment\":" << legacy->segment << '}';
    else out << "null";
    out << '}';
}
void JsonHome(std::ostream& out, ir::Inst* inst, const x64::Translator& code) {
    const auto& homes = x64::CfgDiagnosticProbe::Homes(code); const auto found = homes.find(inst);
    out << "{\"present\":" << (found != homes.end()) << ",\"offset\":";
    if (found == homes.end()) out << "null"; else out << found->second;
    out << ",\"sparse_plan_enabled\":" << code.FramePlan().Enabled() << ",\"sparse_decision\":";
    if (code.FramePlan().Enabled()) out << u32(code.FramePlan().Decision(inst)); else out << "null";
    out << ",\"rematerialized_value\":";
    if (const auto constant = code.FramePlan().RematerializedValue(inst)) out << *constant; else out << "null";
    out << '}';
}
void JsonStatistics(std::ostream& out, const x64::EmitStatistics& stats) {
    out << '{';
#define DIAGNOSTIC_STAT(field) out << "\"" #field "\":" << stats.field << ','
    DIAGNOSTIC_STAT(helper_call_sites); DIAGNOSTIC_STAT(flags_snapshot_sites);
    DIAGNOSTIC_STAT(flags_state_load_sites); DIAGNOSTIC_STAT(flags_state_store_sites);
    DIAGNOSTIC_STAT(flags_resident_read_sites); DIAGNOSTIC_STAT(flags_resident_write_sites);
    DIAGNOSTIC_STAT(flags_resident_merge_sites); DIAGNOSTIC_STAT(flags_resident_update_sites); DIAGNOSTIC_STAT(flags_materialization_ir_sites);
    DIAGNOSTIC_STAT(phi_value_copy_sites); DIAGNOSTIC_STAT(phi_immediate_copy_sites); DIAGNOSTIC_STAT(direct_phi_copy_sites);
    DIAGNOSTIC_STAT(vector_forward_read_sites); DIAGNOSTIC_STAT(vector_forward_write_sites);
    DIAGNOSTIC_STAT(scalar_cache_read_sites); DIAGNOSTIC_STAT(scalar_cache_write_sites);
    DIAGNOSTIC_STAT(scalar_coalesced_binary_sites); DIAGNOSTIC_STAT(scalar_high_word_store_omissions);
    DIAGNOSTIC_STAT(scalar_rematerialized_read_sites); DIAGNOSTIC_STAT(scalar_rematerialized_definition_sites);
    DIAGNOSTIC_STAT(scalar_binary_immediate_sites); DIAGNOSTIC_STAT(scalar_binary_rematerialized_immediate_sites);
    DIAGNOSTIC_STAT(unresolved_transfer_width_sites);
#undef DIAGNOSTIC_STAT
    out << "\"complete_memory_census\":false,\"transfers\":[";
    constexpr const char* names[]{"CanonicalSSA", "ProducerMetadata", "AddressScratch", "PhiScratch", "Local", "Uniform",
                                "StateFlags", "StateOther", "FrameOther", "IndexedFrame", "OtherAddress"};
    static_assert(std::size(names) == unsigned(x64::TransferRegion::Count));
    for (unsigned i = 0; i < std::size(names); ++i) {
        if (i) out << ','; const auto& count = stats.transfers[i];
        out << "{\"region\":"; JsonString(out, names[i]);
        out << ",\"scalar_load_sites\":" << count.scalar_load_sites << ",\"scalar_store_sites\":" << count.scalar_store_sites
            << ",\"vector_load_sites\":" << count.vector_load_sites << ",\"vector_store_sites\":" << count.vector_store_sites
            << ",\"load_bytes\":" << count.load_bytes << ",\"store_bytes\":" << count.store_bytes << '}';
    }
    out << "]}";
}
void DumpDiagnostic(const std::filesystem::path& directory, const Program& program, u32 route, const Compiled& unit) {
    const auto& code = *unit.translator; const auto& graph = unit.allocation;
    Positions positions; std::vector<ir::Inst*> insts;
    for (auto& inst : unit.plan.body->GetInstList()) { positions.emplace(&inst, insts.size()); insts.push_back(&inst); }
    std::vector<u32> block_at(insts.size());
    for (u32 b = 0; b < graph.Blocks().size(); ++b)
        for (u32 p = graph.Blocks()[b].begin; p < graph.Blocks()[b].end; ++p) block_at[p] = b;
    std::ofstream binary{directory / "code.bin", std::ios::binary}; binary.exceptions(std::ios::failbit | std::ios::badbit);
    binary.write(reinterpret_cast<const char*>(code.getCode()), code.getSize()); binary.close();
    std::ofstream out{directory / "diagnostic.json"}; out.exceptions(std::ios::failbit | std::ios::badbit); out << std::boolalpha;
    out << "{\"schema_version\":1,\"scope\":\"static_cfg_diagnostic_no_performance_measurement\",\"workload\":";
    JsonString(out, program.name);
    out << ",\"route\":" << route << ",\"cfg_allocation\":" << bool(route & 1) << ",\"read_forward\":" << bool(route & 2)
        << ",\"flags_residency_requested\":" << bool(route & 4) << ",\"flags_resident\":" << code.FlagsResident()
        << ",\"scalar_binary_immediates_enabled\":" << code.ScalarBinaryImmediatesEnabled()
        << ",\"loop_weighted_costs_enabled\":" << code.LoopWeightedCostsEnabled()
        << ",\"compiler\":"; JsonString(out, __VERSION__);
    out << ",\"guest_base\":" << code_base << ",\"guest_words\":[";
    for (size_t i = 0; i < program.words.size(); ++i) { if (i) out << ','; out << program.words[i]; }
    out << "],\"code_file\":\"code.bin\",\"code_bytes\":" << code.getSize() << ",\"frame_bytes\":" << code.FrameSize()
        << ",\"code_address_mod64\":" << (reinterpret_cast<uintptr_t>(code.getCode()) & 63)
        << ",\"code_address_mod4096\":" << (reinterpret_cast<uintptr_t>(code.getCode()) & 4095)
        << ",\"frame_runtime_address\":null,\"phi_scratch_offset\":" << x64::CfgDiagnosticProbe::PhiScratch(code)
        << ",\"max_phi_moves\":" << x64::CfgDiagnosticProbe::MaxPhiMoves(code)
        << ",\"interference_available\":" << code.CfgPlan().HasInterference() << ",\"register_budget\":[";
    const auto& budget = code.CfgPlan().PhysicalRegisters();
    for (size_t i = 0; i < budget.size(); ++i) { if (i) out << ','; out << budget[i]; }
    const auto& construction = code.CfgPlan().ConstructionStats();
    out << "],\"interference_construction\":{\"dense_used\":" << construction.dense_used
        << ",\"sparse_fallback\":" << construction.sparse_fallback << ",\"dense_bytes\":" << construction.dense_bytes
        << ",\"clique_calls\":" << construction.clique_calls << ",\"instructions\":" << code.CfgPlan().Instructions().size()
        << ",\"natural_loops\":" << construction.natural_loops << ",\"max_loop_depth\":" << construction.max_loop_depth
        << "},\"statistics\":"; JsonStatistics(out, code.Statistics());
    const auto& layout = code.FramePlan().Statistics();
    out << ",\"home_layout\":{\"physical_home_slots\":" << layout.home_count
        << ",\"retained_home_slots\":" << layout.retained_homes << ",\"home_bytes\":" << layout.home_bytes
        << ",\"reused_scalar_homes\":" << layout.reused_scalar_homes
        << ",\"shared_scalar_home_slots\":" << layout.shared_scalar_home_slots
        << ",\"scalar_rematerialization_enabled\":" << code.FramePlan().RematerializationEnabled()
        << ",\"omitted_rematerialized_homes\":" << layout.omitted_rematerialized << '}';
    out << ",\"forward_changes\":[";
    for (size_t i = 0; i < unit.forward_changes.size(); ++i) {
        if (i) out << ','; const auto& change = unit.forward_changes[i];
        const auto source = positions.at(change.source.Def()), target = positions.at(change.load);
        u32 locations_between{};
        for (u32 p = std::min(source, target) + 1; p < std::max(source, target); ++p)
            locations_between += insts[p]->GetOp() == ir::OpCode::SetLocation;
        out << "{\"load\":" << target << ",\"original_position\":" << change.position
            << ",\"original_uniform_offset\":" << change.uniform_offset << ",\"original_type\":";
        JsonString(out, ir::ValueTypeString(change.type)); out << ",\"source\":"; JsonValue(out, change.source, positions);
        out << ",\"source_position\":" << source << ",\"source_block\":" << block_at[source]
            << ",\"target_block\":" << block_at[target] << ",\"position_distance\":" << (s64(target) - source)
            << ",\"intervening_set_locations\":" << locations_between
            << ",\"byte\":" << change.byte << ",\"bytes\":" << change.bytes << ",\"source_allocation\":";
        JsonAllocation(out, change.source.Def(), unit); out << ",\"source_home\":"; JsonHome(out, change.source.Def(), code);
        out << ",\"target_allocation\":"; JsonAllocation(out, change.load, unit); out << ",\"target_home\":";
        JsonHome(out, change.load, code); out << '}';
    }
    out << "],\"instructions\":[";
    for (u32 i = 0; i < insts.size(); ++i) {
        if (i) out << ','; auto* inst = insts[i];
        out << "{\"id\":" << i << ",\"ir_id\":" << inst->Id() << ",\"block\":" << block_at[i]
            << ",\"op\":"; JsonString(out, ir::GetIRMetaInfo(inst->GetOp()).name);
        out << ",\"type\":"; JsonString(out, ir::ValueTypeString(inst->ReturnType()));
        out << ",\"flag_producer\":" << x64::CfgDiagnosticProbe::FlagProducer(code, inst);
        out << ",\"physical_args\":[";
        for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) { if (slot) out << ','; JsonArg(out, inst->ArgAt(slot), positions); }
        out << "],\"allocation\":"; JsonAllocation(out, inst, unit); out << ",\"home\":"; JsonHome(out, inst, code);
        out << ",\"interferes_with\":["; bool first = true;
        if (code.CfgPlan().HasInterference()) for (u32 other = 0; other < insts.size(); ++other)
            if (code.CfgPlan().Interferes(inst, insts[other])) { if (!first) out << ','; first = false; out << other; }
        out << "]}";
    }
    out << "],\"blocks\":[";
    for (u32 b = 0; b < graph.Blocks().size(); ++b) {
        if (b) out << ','; const auto& block = graph.Blocks()[b];
        out << "{\"id\":" << b << ",\"begin\":" << block.begin << ",\"end\":" << block.end
            << ",\"reachable\":" << block.reachable << ",\"loop_depth\":" << block.loop_depth
            << ",\"loop_weight\":" << block.loop_weight << ",\"live_in\":"; JsonIds(out, block.live_in, positions);
        out << ",\"live_out\":"; JsonIds(out, block.live_out, positions); out << '}';
    }
    out << "],\"edges\":[";
    for (u32 e = 0; e < graph.Edges().size(); ++e) {
        if (e) out << ','; const auto& edge = graph.Edges()[e];
        out << "{\"id\":" << e << ",\"from\":" << edge.from << ",\"to\":" << edge.to
            << ",\"taken\":" << edge.taken << ",\"host_entry\":" << edge.host_entry << ",\"source\":";
        if (edge.source) out << positions.at(edge.source); else out << "null";
        out << ",\"live\":"; JsonIds(out, edge.live, positions); out << ",\"copies\":[";
        for (size_t i = 0; i < edge.copies.size(); ++i) {
            if (i) out << ','; const auto& copy = edge.copies[i];
            out << "{\"destination\":" << positions.at(copy.destination) << ",\"source\":";
            JsonData(out, copy.source, positions); out << '}';
        }
        out << "]}";
    }
    out << "],\"labels\":["; bool first = true;
    for (auto* inst : insts) if (inst->GetOp() == ir::OpCode::BindLabel) {
        if (!first) out << ','; first = false; const auto identity = inst->GetArg<ir::Value>(0).Def();
        const auto* address = x64::CfgDiagnosticProbe::Labels(code).at(identity).getAddress();
        if (!address || address < code.getCode() || address > code.getCode() + code.getSize())
            throw std::runtime_error("diagnostic label outside generated code");
        out << "{\"instruction\":" << positions.at(inst) << ",\"identity\":" << positions.at(identity)
            << ",\"native_offset\":" << (address - code.getCode()) << ",\"guest_pc\":";
        if (const auto found = unit.plan.locations.find(inst); found != unit.plan.locations.end()) out << found->second.Value();
        else out << "null";
        out << '}';
    }
    out << "],\"validation\":{\"passed\":true,\"oracle\":\"independent_full_context_integer_model\",\"seed_count\":4,\"trip_counts\":[1,2,7]}}\n";
    out.close();
}
int Diagnostic(const std::filesystem::path& directory) {
    // create_directory is exclusive: an existing directory (even empty), a
    // symlink or a file must never be used as a destination for this mode.
    if (!std::filesystem::create_directory(directory)) throw std::runtime_error("diagnostic output directory already exists");
    for (const std::string name : {"pressure_loop", "conditional_diamond_loop", "carry_loop"}) {
        auto program = MakeProgram(name);
        for (u32 route = 0; route < 8; ++route) {
            auto compiled = Compile(program, route, true); Validate(program, *compiled);
            const auto target = directory / (name + "-route-" + std::to_string(route));
            if (!std::filesystem::create_directory(target)) throw std::runtime_error("diagnostic route directory already exists");
            DumpDiagnostic(target, program, route, *compiled);
        }
    }
    // Written last; a partial dump has no completion manifest. No measured
    // timing or execution of a serialized code.bin belongs to this mode.
    std::ofstream complete{directory / "completion.json"}; complete.exceptions(std::ios::failbit | std::ios::badbit);
    complete << "{\"schema_version\":1,\"complete\":true,\"validation_passed\":true,\"routes\":24,\"performance_measured\":false,\"serialized_code_executed\":false}\n";
    complete.close();
    std::cout << "diagnostic_complete routes=24 directory=" << directory.string() << '\n';
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string{argv[1]} == "--diagnostic-dir") {
        if (argc != 3 || !*argv[2]) throw std::runtime_error("usage: swift_x64_cfg_allocation_benchmark --diagnostic-dir NEW_DIRECTORY");
        return Diagnostic(argv[2]);
    }
    const u32 calls = argc > 1 ? std::stoul(argv[1]) : 200;
    const u32 trips = argc > 2 ? std::stoul(argv[2]) : 64;
    if (!calls || !trips || argc > 3) throw std::runtime_error("usage: swift_x64_cfg_allocation_benchmark [calls] [trips]");
    std::cerr << "scope=existing_flat_forward_legacy_cache_baseline_scalar_rematerialization_binary_immediates_loop_costs; actual_a64_decode_shared_function_plan_integer_loops; "
                 "flat_uniform_forward_constfold_dce_before_plan_all_routes; legacy_scalar_cache_on_cfg_replaces_when_enabled; "
                 "direct_emitter_not_runtime_publication; static_cfg_assignment_home_inventory_not_dynamic_spills; "
                 "all_architectural_stores_retained\n";
    std::cout << "workload,cfg_allocation,read_forward,flags_residency,guest_blocks,cfg_blocks,shared_frame,trips,"
                 "uniform_load_ir,uniform_store_ir,forwarded_load_ir,assigned_values,home_only_values,"
                 "observed_ssa_load_sites,observed_ssa_store_sites,observed_uniform_load_sites,observed_uniform_store_sites,"
                 "flags_state_load_sites,flags_state_store_sites,flags_resident_reads,flags_resident_writes,"
                 "flags_snapshot_sites,phi_value_copies,phi_immediate_copies,helper_calls,code_bytes,frame_bytes,"
                 "state_promotion_accepted,state_entry_seeds,state_phis,state_forwarded_load_ir,"
                 "sparse_frame_enabled,retained_home_slots,omitted_control_slots,omitted_register_slots,omitted_unused_slots,home_bytes,"
                 "direct_phi_copy_sites,phi_scratch_load_sites,phi_scratch_store_sites,"
                 "scalar_high_word_store_omissions,state_candidate_lanes,state_rejected_dirty_loop_lanes,"
                 "flags_resident_merge_sites,scalar_coalesced_binary_sites,scalar_cache_read_sites,scalar_cache_write_sites,"
                 "forward_policy_enabled,forward_candidates,forward_scalar_candidates,forward_rejected_scalar_sources,forward_allocation_rounds,forward_allocation_limit_reached,forward_final_scalar_sources_allocated,"
                 "preparation_full_allocation_builds,preparation_reused_final_allocation,"
                 "cfg_interference_dense,cfg_interference_sparse_fallback,cfg_interference_dense_bytes,cfg_interference_clique_calls,cfg_interference_instructions,flags_resident_update_sites,"
                 "physical_home_slots,reused_scalar_homes,shared_scalar_home_slots,"
                 "scalar_rematerialization_enabled,omitted_rematerialized_homes,scalar_rematerialized_read_sites,scalar_rematerialized_definition_sites,"
                 "scalar_binary_immediates_enabled,scalar_binary_immediate_sites,scalar_binary_rematerialized_immediate_sites,"
                 "loop_weighted_costs_enabled,cfg_natural_loops,cfg_max_loop_depth,"
                 "decode_plan_prepare_emit_us,native_ns_per_call,native_ns_per_guest_iteration,guest_instructions_per_second\n";
    for (const std::string name : {"pressure_loop", "conditional_diamond_loop", "carry_loop"}) {
        auto program = MakeProgram(name);
        std::array<std::unique_ptr<Compiled>, 8> compiled;
        std::array<std::vector<double>, 8> times;
        for (u32 repeat = 0; repeat < 9; ++repeat) for (u32 order = 0; order < 8; ++order) {
            const u32 route = (order + repeat) & 7; const auto begin = Clock::now();
            auto sample = Compile(program, route);
            times[route].push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
        }
        for (u32 route = 0; route < 8; ++route) { compiled[route] = Compile(program, route); Validate(program, *compiled[route]); }
        const auto ns = Measure(compiled, calls, trips);
        for (u32 route = 0; route < 8; ++route) {
            const auto& unit = *compiled[route]; const auto& stats = unit.translator->Statistics();
            const auto& preparation = unit.translator->PreparationStats();
            const auto& frame = unit.translator->FramePlan(); const auto& layout = frame.Statistics();
            const auto& ssa = stats.At(x64::TransferRegion::CanonicalSSA);
            const auto& uniform = stats.At(x64::TransferRegion::Uniform);
            u32 loads{}, stores{}, assigned{}, home{};
            for (auto& inst : unit.plan.body->GetInstList()) {
                loads += inst.GetOp() == ir::OpCode::LoadUniform; stores += inst.GetOp() == ir::OpCode::StoreUniform;
            }
            for (const auto& allocation : unit.allocation.Allocations()) {
                assigned += bool(allocation.slot); home += allocation.requires_canonical_home;
            }
            if (!(route & 1)) for (const auto& inst : unit.plan.body->GetInstList())
                home += inst.ReturnType() >= ir::ValueType::U8 && inst.ReturnType() <= ir::ValueType::S64;
            const auto dynamic_instructions = u64(trips) * (program.operations.size() + 2) + 1;
            std::cout << std::fixed << std::setprecision(2) << name << ',' << bool(route & 1) << ',' << bool(route & 2) << ',' << bool(route & 4)
                      << ',' << unit.guest_blocks << ',' << unit.allocation.Blocks().size() << ",1," << trips
                      << ',' << loads << ',' << stores << ',' << unit.forward.loads_forwarded << ',' << assigned << ',' << home
                      << ',' << ssa.scalar_load_sites + ssa.vector_load_sites << ',' << ssa.scalar_store_sites + ssa.vector_store_sites
                      << ',' << uniform.scalar_load_sites + uniform.vector_load_sites << ',' << uniform.scalar_store_sites + uniform.vector_store_sites
                      << ',' << stats.flags_state_load_sites << ',' << stats.flags_state_store_sites
                      << ',' << stats.flags_resident_read_sites << ',' << stats.flags_resident_write_sites
                      << ',' << stats.flags_snapshot_sites << ',' << stats.phi_value_copy_sites << ',' << stats.phi_immediate_copy_sites
                      << ',' << stats.helper_call_sites << ',' << unit.translator->getSize() << ',' << unit.translator->FrameSize()
                      << ',' << unit.promoted.accepted << ',' << unit.promoted.entry_seeds
                      << ',' << unit.promoted.state_phis << ',' << unit.promoted.loads_forwarded
                      << ',' << frame.Enabled() << ',' << layout.retained_homes << ',' << layout.omitted_control
                      << ',' << layout.omitted_register << ',' << layout.omitted_unused << ',' << layout.home_bytes
                      << ',' << stats.direct_phi_copy_sites
                      << ',' << stats.At(x64::TransferRegion::PhiScratch).scalar_load_sites + stats.At(x64::TransferRegion::PhiScratch).vector_load_sites
                      << ',' << stats.At(x64::TransferRegion::PhiScratch).scalar_store_sites + stats.At(x64::TransferRegion::PhiScratch).vector_store_sites
                      << ',' << stats.scalar_high_word_store_omissions
                      << ',' << unit.promoted.candidate_lanes << ',' << unit.promoted.rejected_dirty_loop_lanes
                      << ',' << stats.flags_resident_merge_sites << ',' << stats.scalar_coalesced_binary_sites
                      << ',' << stats.scalar_cache_read_sites << ',' << stats.scalar_cache_write_sites
                      << ',' << unit.forward.allocation_policy_enabled << ',' << unit.forward.candidate_loads
                      << ',' << unit.forward.scalar_candidates << ',' << unit.forward.rejected_scalar_sources
                      << ',' << unit.forward.allocation_rounds << ',' << unit.forward.allocation_limit_reached
                      << ',' << unit.forward.final_scalar_sources_allocated
                      << ',' << preparation.full_allocation_builds << ',' << preparation.reused_final_allocation
                      << ',' << unit.translator->CfgPlan().ConstructionStats().dense_used
                      << ',' << unit.translator->CfgPlan().ConstructionStats().sparse_fallback
                      << ',' << unit.translator->CfgPlan().ConstructionStats().dense_bytes
                      << ',' << unit.translator->CfgPlan().ConstructionStats().clique_calls
                      << ',' << unit.translator->CfgPlan().Instructions().size()
                      << ',' << stats.flags_resident_update_sites
                      << ',' << layout.home_count << ',' << layout.reused_scalar_homes << ',' << layout.shared_scalar_home_slots
                      << ',' << frame.RematerializationEnabled() << ',' << layout.omitted_rematerialized
                      << ',' << stats.scalar_rematerialized_read_sites << ',' << stats.scalar_rematerialized_definition_sites
                      << ',' << unit.translator->ScalarBinaryImmediatesEnabled() << ',' << stats.scalar_binary_immediate_sites
                      << ',' << stats.scalar_binary_rematerialized_immediate_sites
                      << ',' << unit.translator->LoopWeightedCostsEnabled()
                      << ',' << unit.translator->CfgPlan().ConstructionStats().natural_loops
                      << ',' << unit.translator->CfgPlan().ConstructionStats().max_loop_depth
                      << ',' << Median(times[route]) << ',' << ns[route] << ',' << ns[route] / trips
                      << ',' << double(dynamic_instructions) * 1e9 / ns[route] << '\n';
        }
    }
}
