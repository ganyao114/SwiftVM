#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <elfio/elfio.hpp>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "base/scope_exit.h"

#include "aarch64/constants-aarch64.h"
#include "aot/aot_call.h"
#include "aot/aot_compile.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/pair_call_trampoline.h"
#include "runtime/backend/arm64/compile/lower_edges.h"
#include "runtime/backend/context.h"
#include "runtime/backend/jit_cache.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/signal_handler.h"
#include "runtime/common/div128.h"
#include "runtime/common/svm_config.h"
#include "runtime/frontend/x86/sse42str_helper.h"
#include "runtime/ir/hir_builder.h"
#include "translator/x86/cpu.h"

namespace {
using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;
unsigned checks{};
void Check(bool condition, const char* detail) {
    ++checks;
    if (!condition) throw std::runtime_error(detail);
}
constexpr u64 source_pc = 0x100, return_pc = 0x180, target_pc = 0x1100;
constexpr u64 returned = 0x23456789abcd;
constexpr u32 pressure_inputs = 48;
constexpr u32 pressure_begin = 128;
constexpr u32 pressure_end = pressure_begin + pressure_inputs * sizeof(u64);
constexpr u32 division_begin = pressure_end + 2 * sizeof(u64);
constexpr u32 uniform_size = division_begin + 4 * sizeof(u64);
constexpr u64 pressure_first = 0x100000001, pressure_step = 0x101;
constexpr u64 pressure_sum = pressure_inputs * pressure_first +
                             pressure_step * pressure_inputs * (pressure_inputs - 1) / 2;

u64 SharedTransform(u64 value) { return value * 3 + 7; }

void InitializeCallInputs(Runtime& runtime) {
    Check(std::none_of(runtime.GetState()->spill_area.begin(),
                       runtime.GetState()->spill_area.end(),
                       [](u64 value) { return value == target_pc; }),
          "a fresh runtime has no pre-existing target value in its spill slots");
    const std::array<u8, 16> left{'a', 'b', 'c'};
    const std::array<u8, 16> right{'z', 'b', 'a', 'b'};
    std::memcpy(runtime.GetUniformBuffer().data() + 64, left.data(), left.size());
    std::memcpy(runtime.GetUniformBuffer().data() + 80, right.data(), right.size());
    const u64 salt = 0;
    std::memcpy(runtime.GetUniformBuffer().data() + 56, &salt, sizeof(salt));
    for (u32 i = 0; i < pressure_inputs; ++i) {
        const u64 value = pressure_first + i * pressure_step;
        std::memcpy(runtime.GetUniformBuffer().data() + pressure_begin + i * sizeof(value),
                    &value,
                    sizeof(value));
    }
}

void CheckCallEffects(Runtime& runtime, u64 input, bool indirect) {
    std::array<u64, 2> scalar{};
    std::memcpy(&scalar[0], runtime.GetUniformBuffer().data() + 40, sizeof(u64));
    std::memcpy(&scalar[1], runtime.GetUniformBuffer().data() + 48, sizeof(u64));
    Check(scalar[0] == input * 3 + 7 && scalar[1] == (input * 3 + 7) * 3 + 7,
          "both shared scalar calls preserve their independent results across the Function edge");
    std::array<u64, 4> packed{};
    std::memcpy(packed.data(), runtime.GetUniformBuffer().data() + 96, sizeof(packed));
    // "abc" versus "zbab": equal-any matches positions 1, 2, 3;
    // negative equal-each differs at 0, 2, 3. Comparing "abc" with itself
    // matches all three characters and has no negative equal-each match.
    constexpr std::array<u64, 4> expected{0x0701'000e, 0x0f00'000d, 0x0f00'0007, 0x0310'0000};
    Check(packed == expected, "both shared SSE helpers preserve masks, indices and packed flags");
    std::array<u64, 4> divisions{};
    std::memcpy(divisions.data(),
                runtime.GetUniformBuffer().data() + division_begin,
                sizeof(divisions));
    Check(input == 36 || input == ~u64{0},
          "division oracle receives one of its two explicit inputs");
    const std::array<u64, 4> expected_divisions =
            input == 36 ? std::array<u64, 4>{5, 1, 2635249153387078807ull, 3}
                        : std::array<u64, 4>{0, ~u64{0}, 5270498306774157604ull, 3};
    Check(divisions == expected_divisions,
          "native signed and wide unsigned division preserve quotient/remainder across Function "
          "edges and helpers");
    if (indirect) {
        u64 checksum{}, parity{}, difference{}, expected_parity{};
        for (u32 i = 0; i < pressure_inputs; ++i)
            expected_parity ^= pressure_first + i * pressure_step;
        std::memcpy(&checksum, runtime.GetUniformBuffer().data() + 56, sizeof(checksum));
        std::memcpy(&parity, runtime.GetUniformBuffer().data() + pressure_end, sizeof(parity));
        std::memcpy(&difference,
                    runtime.GetUniformBuffer().data() + pressure_end + sizeof(u64),
                    sizeof(difference));
        Check(checksum == pressure_sum && parity == expected_parity &&
                      difference == ~u64{0} - pressure_sum,
              "all three independent pressure reductions preserve their observable results");
    }
}

void CheckDivisionCode(std::span<const u8> code) {
    Check(code.size() % sizeof(u32) == 0,
          "captured Function code contains complete AArch64 instructions");
    unsigned signed_divisions{}, unsigned_divisions{};
    for (size_t offset = 0; offset < code.size(); offset += sizeof(u32)) {
        u32 instruction{};
        std::memcpy(&instruction, code.data() + offset, sizeof(instruction));
        const auto opcode = instruction & vixl::aarch64::DataProcessing2SourceMask;
        signed_divisions += opcode == vixl::aarch64::SDIV_x;
        unsigned_divisions += opcode == vixl::aarch64::UDIV_x;
    }
    Check(signed_divisions == 1 && unsigned_divisions == 0,
          "published Function keeps one native signed divide and the real wide unsigned helper "
          "path");
}

u32 PairCallOffset(std::span<const SerialRegionBranch> branches) {
    const auto count = std::count_if(branches.begin(), branches.end(), [](const auto& branch) {
        return branch.kind == SerialRegionBranchKind::PairCall;
    });
    Check(count == 1, "wide division has exactly one checked pair-call region recipe");
    return std::find_if(branches.begin(),
                        branches.end(),
                        [](const auto& branch) {
                            return branch.kind == SerialRegionBranchKind::PairCall;
                        })
            ->code_offset;
}

void CheckPairCallBinding(const u8* allocation, u32 offset, CodeCache& cache) {
    const auto* target = static_cast<const u8*>(cache.GetPairCallRegionTrampoline());
    Check(target && cache.GetRegion().ContainsRx(target),
          "pair-call trampoline belongs to the destination code region");
    const auto expected = EncodeBL(target - (allocation + offset));
    u32 actual{};
    std::memcpy(&actual, allocation + offset, sizeof(actual));
    Check(expected && actual == *expected,
          "actual native pair-call instruction reaches the destination region trampoline");
}

void CheckTargetCode(const SerialUnit& unit) {
    CheckDivisionCode(unit.code);
    const auto pair_offset = PairCallOffset(unit.region_branches);
    Check(pair_offset >= 16, "pair-call recipe follows its fixed-width helper materialization");
    const std::array<u64, 4> targets{reinterpret_cast<u64>(&SharedTransform),
                                     swift::x86::Sse42StrVectorHelperAddress(0x02),
                                     swift::x86::Sse42StrVectorHelperAddress(0x1a),
                                     reinterpret_cast<u64>(&DivideUnsigned128)};
    for (const auto target : targets) {
        Check(target && GetHostImage().Contains(target),
              "shared helper belongs to the current host image");
        const auto count =
                std::count_if(unit.relocs.begin(), unit.relocs.end(), [&](const auto& relocation) {
                    return relocation.kind == RelocKind::HostImageAbs64 &&
                           relocation.use == RelocUse::BranchTarget &&
                           relocation.recorded_value == target;
                });
        Check(count == 1, "each expected helper has exactly one relocatable materialization");
    }
    Check(std::count_if(unit.relocs.begin(),
                        unit.relocs.end(),
                        [&](const auto& relocation) {
                            return relocation.code_offset == pair_offset - 16 &&
                                   relocation.inst_count == 4 &&
                                   relocation.reg == arm64::PairCallFrame::TargetRegister &&
                                   relocation.recorded_value ==
                                           reinterpret_cast<u64>(&DivideUnsigned128);
                        }) == 1,
          "wide helper relocation is attached to the actual pair-call recipe");
}

struct MissObservation {
    std::atomic<unsigned> count{};
    std::atomic<uintptr_t> pc{}, link_register{};
    State* state{};
    u64 target{};
    std::atomic<unsigned> target_spill_slots{};
};
bool ObserveMiss(void* opaque, ucontext_t* context, int signal, siginfo_t*) {
    if (signal == SIGSEGV || signal == SIGBUS) {
        auto& observed = *static_cast<MissObservation*>(opaque);
        observed.pc.store(SignalHandler::GetContextPC(context), std::memory_order_relaxed);
        observed.link_register.store(SignalHandler::GetContextGPR(context, 30), std::memory_order_relaxed);
        unsigned target_slots{};
        if (observed.state)
            for (const u64 value : observed.state->spill_area)
                target_slots += value == observed.target;
        observed.target_spill_slots.store(target_slots, std::memory_order_relaxed);
        observed.count.fetch_add(1, std::memory_order_relaxed);
    }
    return false;
}

void CompileTarget(const std::shared_ptr<Module>& module) {
    HIRBuilder builder{1, true, ResolveFeatureSet(module->GetModuleConfig())};
    auto* function = builder.AppendFunction(Location{target_pc}, Location{target_pc + 4});
    const auto input = function->LoadUniform(ValueType::U64, Uniform{8, ValueType::U64});
    const auto one = function->LoadImm(ValueType::U64, Imm{u64{1}});
    const auto sum = function->Add(ValueType::U64, input, Operand{one});
    function->SaveFlags(sum, Flags::All);
    function->StoreUniform(Uniform{0, ValueType::U64}, sum);
    const auto divisor = function->LoadImm(ValueType::U64, Imm{u64{7}});
    const auto sign = function->AsrImm(ValueType::U64, input, Imm{u32{63}});
    const auto signed_quotient =
            function->Div128(ValueType::U64, sign, input, divisor, Imm{u64{1}});
    const auto signed_remainder = function->Div128Remainder(ValueType::U64, signed_quotient);
    const Lambda helper{DataClass{Imm{reinterpret_cast<u64>(&SharedTransform)}},
                        {.uniform = UniformEffectId::None,
                         .host_fp = HostFpEffect::FPCRTransparent,
                         .guest_state = HelperGuestStateEffect::None,
                         .fault = HelperFaultEffect::NoDirectFault,
                         .reentry = HelperReentryEffect::NoReentry}};
    const auto first = function->CallLambda(ValueType::U64, helper, input);
    function->StoreUniform(Uniform{40, ValueType::U64}, first);
    const auto left = function->LoadUniform(Uniform{64, ValueType::V128});
    const auto right = function->LoadUniform(Uniform{80, ValueType::V128});
    function->StoreUniform(Uniform{96, ValueType::U64},
                           function->Sse42Str(ValueType::U64, left, right, Imm{u64{0x02}}));
    function->StoreUniform(Uniform{104, ValueType::U64},
                           function->Sse42Str(ValueType::U64, left, right, Imm{u64{0x1a}}));
    function->AdvancePC(Imm{u64{1}});
    auto* tail = builder.LinkBlock(terminal::LinkBlock{Location{target_pc + 2}});
    builder.SetCurBlock(tail);
    function->StoreUniform(Uniform{48, ValueType::U64},
                           function->CallLambda(ValueType::U64, helper, first));
    const auto wide_high = function->LoadImm(ValueType::U64, Imm{u64{1}});
    const auto wide_quotient =
            function->Div128(ValueType::U64, wide_high, input, divisor, Imm{u64{0}});
    const auto wide_remainder = function->Div128Remainder(ValueType::U64, wide_quotient);
    function->StoreUniform(Uniform{division_begin, ValueType::U64}, signed_quotient);
    function->StoreUniform(Uniform{division_begin + 8, ValueType::U64}, signed_remainder);
    function->StoreUniform(Uniform{division_begin + 16, ValueType::U64}, wide_quotient);
    function->StoreUniform(Uniform{division_begin + 24, ValueType::U64}, wide_remainder);
    function->StoreUniform(Uniform{112, ValueType::U64},
                           function->Sse42Str(ValueType::U64, left, left, Imm{u64{0x02}}));
    function->StoreUniform(Uniform{120, ValueType::U64},
                           function->Sse42Str(ValueType::U64, left, left, Imm{u64{0x1a}}));
    const auto resume = function->LoadUniform<TypedValue<ValueType::U64>>(Uniform{16, ValueType::U64});
    function->SetLocation(Lambda{resume});
    function->EndBlock(terminal::PopRSBHint{});
    function->EndFunction();
    Check(TranslateIR(module, function) != nullptr, "compile real flags-producing returning target");
}

void CompileSource(const std::shared_ptr<Module>& module, bool indirect) {
    HIRBuilder builder{1, true, ResolveFeatureSet(module->GetModuleConfig())};
    auto* function = builder.AppendFunction(Location{source_pc}, Location{return_pc + 1});
    Lambda target{Imm{target_pc}};
    if (indirect) {
        // The target is a computed SSA value, not a guest-home alias. Keep
        // its next use beyond more live integers than any AArch64 GPR pool.
        // Each pressure input has three uses, so eviction cannot prefer a
        // cheap single-use input over this twice-used target.
        const auto address = function->LoadUniform(ValueType::U64, Uniform{32, ValueType::U64});
        const auto salt = function->LoadUniform(ValueType::U64, Uniform{56, ValueType::U64});
        target = Lambda{function->Xor(ValueType::U64, address, Operand{salt})};
        std::array<Value, pressure_inputs> values;
        for (u32 i = 0; i < values.size(); ++i)
            values[i] = function->LoadUniform(
                    ValueType::U64, Uniform{pressure_begin + i * u32{sizeof(u64)}, ValueType::U64});
        auto checksum = values.front();
        auto parity = values.front();
        auto difference = function->LoadImm(ValueType::U64, Imm{~u64{0}});
        for (u32 i = 0; i < values.size(); ++i) {
            if (i) {
                checksum = function->Add(ValueType::U64, checksum, Operand{values[i]});
                parity = function->Xor(ValueType::U64, parity, Operand{values[i]});
            }
            difference = function->Sub(ValueType::U64, difference, Operand{values[i]});
        }
        function->StoreUniform(Uniform{56, ValueType::U64}, checksum);
        function->StoreUniform(Uniform{pressure_end, ValueType::U64}, parity);
        function->StoreUniform(Uniform{pressure_end + u32{sizeof(u64)}, ValueType::U64},
                               difference);
    }
    function->StoreUniform(Uniform{24, ValueType::U64},
                           function->LoadImm(ValueType::U64, Imm{u64{0}}));
    const auto left = function->LoadImm(ValueType::U64, Imm{~u64{0}});
    const auto one = function->LoadImm(ValueType::U64, Imm{u64{1}});
    const auto sum = function->Add(ValueType::U64, left, Operand{one});
    function->SaveFlags(sum, Flags::All);
    const auto resume = function->LoadImm(ValueType::U64, Imm{return_pc});
    function->CallReturn(resume, Imm{return_pc}, target);
    function->SetLocation(target);
    builder.RegisterCallReturn(Location{return_pc});
    function->EndBlock(terminal::ReturnToDispatch{});
    builder.SetCurBlock(Location{return_pc});
    function->StoreUniform(Uniform{24, ValueType::U64},
                           function->LoadImm(ValueType::U64, Imm{returned}));
    function->AdvancePC(Imm{u64{1}});
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    Check(TranslateIR(module, function) != nullptr, "compile real static or indirect Call and guest return body");
}

aot::AotImage CaptureCalls(AddressSpace& space, void* guest, size_t bytes,
                            std::span<const u64> functions = {},
                            std::shared_ptr<Module> module = {}) {
    if (!module) module = space.GetDefaultModule();
    const std::array<u64, 2> call_functions{source_pc, target_pc};
    if (functions.empty()) functions = call_functions;
    aot::AotImage image;
    image.host_isa = kArm64;
    image.guest.entry = source_pc;
    image.key = {kCacheFormatVersion, ComputeBuildId(), ComputeConfigHash(space.GetConfig()), ComputeEnvHash(),
                 aot::HashGuestImage(image.segments, image.guest.entry)};
    for (const u64 pc : functions) {
        const auto node = module->GetNode(Location{pc});
        Check(IsFunction(node), "capture production function owner");
        const auto function = GetFunction(node);
        auto* allocation = static_cast<u8*>(module->GetJitCache(function->GetJitCache()));
        FaultEntry extent;
        Check(module->LookupCodeAllocation(allocation, extent), "resolve whole production allocation");
        SerialUnit captured;
        std::string error;
        Check(CaptureSerialCode(*module, allocation, u32(extent.host_end - extent.host_start),
              GetHostImage(), bytes, captured, &error), error.c_str());
        if (pc == target_pc) CheckTargetCode(captured);
        aot::AotUnit unit;
        unit.guest_start = pc;
        unit.code_offset = u32(image.code.size());
        unit.code_size = u32(captured.code.size());
        unit.relocs = std::move(captured.relocs);
        unit.fault_sites = std::move(captured.fault_sites);
        unit.link_sites = std::move(captured.link_sites);
        unit.region_branches = std::move(captured.region_branches);
        const auto offset = [&](const void* pointer) -> u32 {
            if (!pointer) return UINT32_MAX;
            const auto* address = static_cast<const u8*>(pointer);
            Check(address >= allocation && address < extent.host_end, "captured entry belongs to its owner");
            return u32(address - allocation);
        };
        for (const auto& member : function->GetBlocks()) {
            const auto start = member.GetStartLocation().Value();
            const auto end = std::max(start + 1, member.GetEndLocation().Value());
            SerialBlock block{start, end, UINT32_MAX,
                HashBytes(static_cast<const u8*>(guest) + start, end - start, 0xCBF29CE484222325ull)};
            if (const auto target = space.GetLinkManager().QueryTarget(start)) {
                block.code_offset = offset(target->host_pc);
                block.direct_code_offset = offset(target->direct_host_pc);
                block.pending_flags_code_offset = offset(target->pending_flags_host_pc);
                block.pending_flags_contract = target->pending_flags_contract;
                block.continuation_code_offset = offset(target->call_host_pc);
                block.pending_flags_continuation_code_offset = offset(target->call_pending_flags_host_pc);
            } else if (auto* entry = space.GetCodeCache(Location{start})) {
                block.code_offset = offset(entry);
                block.entry_flags = 0;
            } else block.entry_flags = SerialBlock::MetadataOnly;
            Check(block.ValidEntryContract(unit.code_size), "capture all actual function entry contracts");
            unit.blocks.push_back(block);
        }
        image.code.insert(image.code.end(), captured.code.begin(), captured.code.end());
        image.units.push_back(std::move(unit));
    }
    space.GetCodeCacheTable().ForEachEntry([&](u32 index, size_t key, size_t) {
        image.dispatch_slots.emplace_back(u64(key), index);
    });
    image.code_hash = HashBytes(image.code.data(), image.code.size(), 0xCBF29CE484222325ull);
    return image;
}

// The legacy SUB representation uses raw ARM C plus carry_inverted=1.  Keep
// that explicit instead of relying on host FlagM support in this AOT fixture.
constexpr u64 loop_cold_pc = source_pc + 4;
constexpr u64 loop_data_guest = 0x2000;
constexpr u32 loop_counter_offset = 8, loop_flags_offset = 32;
constexpr std::array<Flags, 6> loop_flag_masks{
        Flags::Carry, Flags::Parity, Flags::AuxiliaryCarry,
        Flags::Zero, Flags::Negate, Flags::Overflow};

void CompileEligibleLoop(const std::shared_ptr<Module>& module) {
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    Check(FlagsRegsEnabled() && features.flags_loop_lazy,
          "eligible loop requests flags registers and module loop-lazy override");
    HIRBuilder builder{1, true, features};
    auto* function = builder.AppendFunction(Location{source_pc}, Location{source_pc + 8});
    auto* loop = function->AppendBlock(Location{source_pc}, Location{loop_cold_pc});
    auto* cold = function->AppendBlock(Location{loop_cold_pc}, Location{source_pc + 8});
    builder.SetCurBlock(loop);
    // This observable, valid V128 load makes incoming flags live in the loop
    // prefix. A fault-free overwrite-only loop could instead select the
    // separate dead-successor recipe and would not test a private self entry.
    const auto packed = function->LoadMemoryTSO(
            ValueType::V128, Operand{Imm{loop_data_guest}});
    function->StoreUniform(Uniform{64, ValueType::V128}, packed);
    const auto count = function->LoadUniform(ValueType::U64,
                                             Uniform{loop_counter_offset, ValueType::U64});
    const auto result = function->Sub(ValueType::U64, count, Operand{Imm{u64{1}}});
    function->StoreUniform(Uniform{loop_counter_offset, ValueType::U64}, result);
    // The store belongs before the final SaveFlags marker. The admitted tail
    // consists only of polarity, AdvancePC and the local condition.
    function->SaveFlags(result, Flags::All);
    function->StoreUniform(
            Uniform{offsetof(swift::x86::ThreadContext64, carry_inverted), ValueType::U8},
            function->LoadImm(ValueType::U8, Imm{u8{1}}));
    function->AdvancePC(Imm{u64{1}});
    const auto condition = function->LocalCondSet(Cond::NE);
    function->AddEdge(loop, loop);
    function->AddEdge(loop, cold);
    function->EndBlock(terminal::If{condition,
                                    terminal::LinkBlock{Location{source_pc}},
                                    terminal::LinkBlock{Location{loop_cold_pc}}});
    builder.SetCurBlock(cold);
    for (u32 index = 0; index < loop_flag_masks.size(); ++index)
        function->StoreUniform(Uniform{loop_flags_offset + index, ValueType::U8},
                               function->TestFlags(loop_flag_masks[index]));
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    arm64::BackedgeInputs inputs{features};
    inputs.self_enabled = true;
    inputs.canonical_carry = false;
    const auto selected = arm64::LowerBackedge(*loop->GetBlock(), inputs);
    Check(selected && !selected->action.dead_successor &&
                  selected->action.self_target == Location{source_pc} &&
                  selected->action.cold_target == Location{loop_cold_pc} &&
                  selected->action.Required() == Flags::NZCV,
          "the real loop IR selects the non-dead self recipe with all NZCV requested");
    Check(TranslateIR(module, function) != nullptr,
          "compile the statically eligible two-block loop");
}

bool HasPrivateLoopRecovery(const std::shared_ptr<Module>& module) {
    const auto node = module->GetNode(Location{source_pc});
    Check(IsFunction(node), "eligible loop has a real persistent Function owner");
    auto* allocation = static_cast<u8*>(module->GetJitCache(GetFunction(node)->GetJitCache()));
    FaultEntry extent;
    std::vector<FaultEntry> ranges;
    Check(module->LookupCodeAllocation(allocation, extent, &ranges),
          "read the published loop's complete allocation and copied fault metadata");
    // Poll recovery records are one instruction. The private self recipe
    // additionally publishes a whole body range with its own flags recovery.
    return std::any_of(ranges.begin(), ranges.end(), [](const auto& range) {
        return range.guest_loc == source_pc && range.recovery != nullptr &&
               range.host_end - range.host_start > sizeof(u32);
    });
}

void RunEligibleLoop(AddressSpace& space) {
    for (const u64 input : {u64{1}, u64{2}, u64{17}}) {
        Runtime runtime{&space};
        auto uniform = runtime.GetUniformBuffer();
        std::memcpy(uniform.data() + loop_counter_offset, &input, sizeof(input));
        runtime.GetState()->host_cpu_flags = 0xb4000001u;
        runtime.SetLocation(source_pc);
        Check(runtime.Run() == HaltReason::CallHost,
              "eligible loop reaches its observer cold block");
        u64 count{};
        std::memcpy(&count, uniform.data() + loop_counter_offset, sizeof(count));
        Check(count == 0, "all three positive counter inputs terminate at zero");
        // The final arithmetic is always 1-1. Raw C=1 means no borrow;
        // carry_inverted=1 makes guest CF=0. PF and ZF are set; AF/SF/OF clear.
        constexpr std::array<u8, 6> expected{1, 1, 0, 1, 0, 0};
        for (u32 index = 0; index < expected.size(); ++index)
            Check(uniform[loop_flags_offset + index] == expected[index],
                  "cold block observes the independently specified final SUB flags");
        const auto polarity = uniform[offsetof(swift::x86::ThreadContext64, carry_inverted)];
        Check(polarity == 1 && (uniform[loop_flags_offset] ^ polarity) == 0,
              "legacy raw carry and polarity preserve guest no-borrow CF=0");
        Check((runtime.GetState()->host_cpu_flags & 0xf0000000u) == 0x60000000u,
              "host exit preserves the complete final NZCV carrier");
        Check(std::all_of(uniform.begin() + 64, uniform.begin() + 80,
                          [](u8 byte) { return byte == 0xa5; }),
              "the real V128 TSO prefix remains observable after every iteration");
    }
}

void AotEligibleLoop() {
    Check(FlagsRegsEnabled(), "AOT eligible-loop regression requires flags registers");
    const auto bytes = 16 * static_cast<size_t>(getpagesize());
    void* source_guest = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void* guest = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(source_guest != MAP_FAILED && guest != MAP_FAILED,
          "map independent eligible-loop source and install guest windows");
    const auto release = ScopeExit([&] { munmap(source_guest, bytes); munmap(guest, bytes); });
    std::memset(source_guest, 0xa5, bytes);
    std::memset(guest, 0xa5, bytes);
    Config config{};
    config.loc_start = 0;
    config.loc_end = bytes;
    config.enable_jit = true;
    config.backend_isa = kArm64;
    config.uniform_buffer_size = std::max(uniform_size, u32{sizeof(swift::x86::ThreadContext64)});
    config.stack_alignment = 16;
    config.global_opts = Optimizations::BlockLink;
    config.region_edges = true;
    config.memory_base = source_guest;
    config.guest_addr_mask = bytes - 1;
    const auto map = [&](AddressSpace& space) {
        auto options = space.GetDefaultModule()->GetModuleConfig();
        options.feature_overrides.Set(FeatureId::flags_loop_lazy, true);
        auto module = space.MapModule(0, bytes, options);
        Check(bool(module), "map explicit module with the loop-lazy override");
        return module;
    };
    {
        AddressSpace control{config};
        const auto* cache = control.GetJitDiskCache();
        Check(!control.SerializableCodeEmission() && (!cache || !cache->Enabled()),
              "ordinary loop control is eligible for import without either storage gate");
        const auto module = map(control);
        CompileEligibleLoop(module);
        Check(HasPrivateLoopRecovery(module),
              "ordinary emission actually imports and optimizes the private self recipe");
        RunEligibleLoop(control);
    }
    auto source = std::make_unique<AddressSpace>(config);
    source->SetSerializableCodeEmission();
    auto source_module = map(*source);
    CompileEligibleLoop(source_module);
    Check(!HasPrivateLoopRecovery(source_module),
          "serializable emission does not import private loop flags recovery");
    const std::array<u64, 1> functions{source_pc};
    const auto image = CaptureCalls(*source, source_guest, bytes, functions, source_module);
    Check(image.units.size() == 1 && image.units.front().blocks.size() == 2,
          "capture the genuine loop and observer as one complete Function unit");
    // AddFaultEntry's default kind for the actual broad loop range is
    // GuestFault; CaptureSerialCode preserves it. Require that real record
    // before asserting the absence of private recovery, rather than testing
    // an empty GuestRange-kind subset. Single-instruction poll recoveries
    // remain valid in the ordinary serializable path.
    Check(std::any_of(image.units.front().fault_sites.begin(),
                       image.units.front().fault_sites.end(), [](const auto& site) {
                           return site.guest_start == source_pc &&
                                  site.recovery_kind == u8(FaultRecoveryKind::GuestFault) &&
                                  site.host_end - site.host_begin > sizeof(u32);
                       }),
          "capture includes the actual wide loop GuestFault range");
    Check(std::none_of(image.units.front().fault_sites.begin(),
                        image.units.front().fault_sites.end(), [](const auto& site) {
                            return site.guest_start == source_pc &&
                                   site.host_end - site.host_begin > sizeof(u32) &&
                                   site.recovery_offset != UINT32_MAX;
                        }),
          "captured wide loop ranges of every kind contain no private recovery offsets");
    const auto blob = aot::EncodeInfoBlob(image);
    aot::AotImage restored;
    std::string error;
    Check(aot::DecodeInfoBlob(blob.data(), blob.size(), restored, error), error.c_str());
    restored.host_isa = image.host_isa;
    restored.code = image.code;
    config.memory_base = guest;
    AddressSpace destination{config};
    const auto destination_module = map(destination);
    aot::LoadReport report;
    Check(aot::InstallArtifact(destination, restored, guest, config.guest_addr_mask,
                               report, error) == aot::kAotOk,
          error.c_str());
    Check(report.units_installed == 1 && report.mapping &&
                  destination.GetCodeCache(Location{source_pc}) != source->GetCodeCache(Location{source_pc}),
          "install a distinct real RX loop owner while its producer remains alive");
    source_module.reset();
    source.reset();
    Check(destination.GetCodeCache(Location{source_pc}) &&
                  !HasPrivateLoopRecovery(destination_module),
          "installed standard loop metadata survives complete producer destruction");
    RunEligibleLoop(destination);
    std::printf("aot_loop units=1 static_selected=2 control_private=1 serial_private=0 "
                "executions=6 inputs=1,2,17 counter=0 raw_flags=110100 source_destroyed=1\n");
}

void AotCalls(bool indirect) {
    const auto bytes = 16 * static_cast<size_t>(getpagesize());
    void* source_guest = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void* guest = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(source_guest != MAP_FAILED && guest != MAP_FAILED, "map separate source and destination guest windows");
    const auto release = ScopeExit([&] { munmap(source_guest, bytes); munmap(guest, bytes); });
    std::memset(source_guest, 0xa5, bytes);
    std::memset(guest, 0xa5, bytes);
    Config config{};
    config.loc_start = 0;
    config.loc_end = bytes;
    config.enable_jit = true;
    config.backend_isa = kArm64;
    config.uniform_buffer_size = uniform_size;
    config.stack_alignment = 16;
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    config.memory_base = source_guest;
    config.guest_addr_mask = bytes - 1;
    auto source = std::make_unique<AddressSpace>(config);
    source->SetSerializableCodeEmission();
    CompileTarget(source->GetDefaultModule());
    CompileSource(source->GetDefaultModule(), indirect);
    const auto image = CaptureCalls(*source, source_guest, bytes);
    auto* source_cache = source->GetDefaultModule()->GetCodeCache(
            static_cast<u8*>(source->GetCodeCache(Location{target_pc})));
    Check(source_cache != nullptr, "resolve source region for pair-call identity");
    const auto source_pair_call = source_cache->GetPairCallRegionTrampoline();
    const auto& source_unit = image.units.front();
    Check(indirect ? source_unit.link_sites.empty() : source_unit.link_sites.size() == 1 &&
          source_unit.link_sites.front().kind == u8(LinkSiteKind::Call),
          "production serializable emitter captures the actual static or indirect Call shape");
    Check(indirect || !FlagsRegsEnabled() || source_unit.link_sites.front().HasFlagsBypass(),
          "flags1 static Call carries its real physical flags-bypass recipe");
    const auto blob = aot::EncodeInfoBlob(image);
    aot::AotImage restored;
    std::string error;
    Check(aot::DecodeInfoBlob(blob.data(), blob.size(), restored, error), error.c_str());
    restored.host_isa = image.host_isa;
    restored.code = image.code;
    config.memory_base = guest;
    AddressSpace destination{config};
    aot::LoadReport report;
    Check(aot::InstallArtifact(destination, restored, guest, config.guest_addr_mask, report, error) == aot::kAotOk,
          error.c_str());
    Check(report.units_installed == 2 && report.mapping, "install exactly both captured Call units");
    for (const u64 pc : {source_pc, target_pc}) Check(destination.GetCodeCache(Location{pc}) &&
          destination.GetCodeCache(Location{pc}) != source->GetCodeCache(Location{pc}),
          "installed RX identity differs while both source allocations are still alive");
    source.reset();
    const auto& target_unit = restored.units.back();
    const auto* installed_target =
            static_cast<const u8*>(aot::ResolveCodeOffset(report.mapping, target_unit.code_offset));
    Check(installed_target != nullptr,
          "resolve installed division Function after source destruction");
    CheckDivisionCode({installed_target, target_unit.code_size});
    const auto module = destination.GetDefaultModule();
    auto* target_cache = module->GetCodeCache(const_cast<u8*>(installed_target));
    Check(target_cache && target_cache->GetPairCallRegionTrampoline() != source_pair_call,
          "AOT pair-call trampoline differs from the retired source region");
    CheckPairCallBinding(
            installed_target, PairCallOffset(image.units.back().region_branches), *target_cache);
    auto* source_base = static_cast<u8*>(aot::ResolveCodeOffset(report.mapping, source_unit.code_offset));
    const auto target = destination.GetLinkManager().QueryTarget(target_pc);
    Check(target && target->call_host_pc && (!FlagsRegsEnabled() || target->call_pending_flags_host_pc),
          "AOT publishes actual physical Call and pending Call target entries");
    for (const auto& unit : restored.units) for (const auto& block : unit.blocks) {
        if (!block.IsPublished()) continue;
        auto* base = static_cast<u8*>(aot::ResolveCodeOffset(report.mapping, unit.code_offset));
        const auto pointer = [&](u32 offset) -> void* { return offset == UINT32_MAX ? nullptr : base + offset; };
        Check(destination.GetCodeCache(Location{block.guest_start}) == pointer(block.code_offset) &&
              reinterpret_cast<void*>(destination.GetCallCodeCacheTable().Lookup(block.guest_start)) == pointer(block.continuation_code_offset) &&
              reinterpret_cast<void*>(destination.GetPendingCallCodeCacheTable().Lookup(block.guest_start)) == pointer(block.pending_flags_continuation_code_offset),
              "installed canonical and both Call tables match serialized entries exactly");
    }
    unsigned spilled_misses{};
    for (unsigned iteration = 0; iteration < 4; ++iteration) {
        const u64 input = iteration & 1 ? ~u64{0} : u64{36};
        const bool miss = indirect && iteration >= 2;
        if (miss) Check(destination.GetCallCodeCacheTable().Zero(target_pc) &&
                        destination.GetPendingCallCodeCacheTable().Zero(target_pc), "remove both physical indirect targets");
        Runtime runtime{&destination};
        MissObservation observed{.state = runtime.GetState(), .target = target_pc};
        SignalHandler::RegisterHandler(&ObserveMiss, &observed, -1);
        const auto unregister = ScopeExit([&] { SignalHandler::UnregisterHandler(&observed); });
        const u64 resume = return_pc, target_address = target_pc;
        std::memcpy(runtime.GetUniformBuffer().data() + 8, &input, 8);
        std::memcpy(runtime.GetUniformBuffer().data() + 16, &resume, 8);
        std::memcpy(runtime.GetUniformBuffer().data() + 32, &target_address, 8);
        InitializeCallInputs(runtime);
        runtime.SetLocation(source_pc);
        Check(runtime.Run() == HaltReason::CallHost, "AOT Call returns after source AddressSpace destruction");
        CheckCallEffects(runtime, input, indirect);
        u64 value{}, witness{};
        std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
        std::memcpy(&witness, runtime.GetUniformBuffer().data() + 24, 8);
        Check(value == input + 1 && witness == returned, "restored Call result and guest return body are independently correct");
        Check((runtime.GetState()->host_cpu_flags & 0xf0000000u) == (value == 0 ? 0x60000000u : 0),
              "restored physical Call preserves independent callee NZCV");
        Check(runtime.GetState()->rsb_empty && runtime.GetState()->rsb_pointer == runtime.GetState()->rsb_empty,
              "restored Call balances its external frame");
        Check(observed.count.load() == (miss ? 1u : 0u), "restored Call takes exactly its intended native missing-entry faults");
        if (miss) {
            Check(observed.target_spill_slots.load() > 0,
                  "the installed indirect target really has spill backing at its missing-entry "
                  "fault");
            ++spilled_misses;
            FaultEntry fault;
            Check(module->LookupFault(reinterpret_cast<const u8*>(observed.link_register.load() - 4), fault) &&
                  fault.recovery_kind == FaultRecoveryKind::IndirectCallMiss && fault.recovery && fault.guest_loc == source_pc,
                  "AOT preserves the exact actual IndirectCallMiss recipe");
            Check(destination.PushCallCodeCache(Location{target_pc}, target->call_host_pc) &&
                  destination.PushPendingCallCodeCache(Location{target_pc}, target->call_pending_flags_host_pc),
                  "restore both actual target entries after native fault");
        }
    }
    const auto sites = destination.GetLinkManager().QuerySourceSites({module.get(), source_base});
    Check(indirect ? sites.empty() : sites.size() == 1 && sites.front().kind == LinkSiteKind::Call &&
          sites.front().state == LinkSiteState::Linked, "restored static Call really patches its live source");
    destination.InvalidateCodeRange(target_pc, target_pc + 4);
    Check(!destination.GetCodeCache(Location{target_pc}) && !destination.GetCallCodeCacheTable().Lookup(target_pc) &&
          !destination.GetPendingCallCodeCacheTable().Lookup(target_pc) &&
          !aot::ResolveCodeOffset(report.mapping, restored.units.back().code_offset) &&
          aot::ResolveCodeOffset(report.mapping, source_unit.code_offset) == source_base,
          "target range retirement revokes every Call alias while retaining source ownership");
    const auto retired_sites = destination.GetLinkManager().QuerySourceSites({module.get(), source_base});
    Check(indirect || retired_sites.size() == 1 && retired_sites.front().state == LinkSiteState::Unlinked,
          "target retirement unlinks the restored physical Call");
    auto malformed = restored;
    malformed.units.back().relocs.push_back({.code_offset = malformed.units.back().code_size,
        .inst_count = 1, .kind = RelocKind::HostImageAbs64});
    {
        AddressSpace rejected{config};
        aot::LoadReport rejected_report;
        Check(aot::InstallArtifact(rejected, malformed, guest, config.guest_addr_mask, rejected_report, error) == aot::kAotRelocFailed &&
              !rejected_report.units_installed && !rejected_report.mapping && !rejected.GetCodeCache(Location{source_pc}) &&
              !rejected.GetCallCodeCacheTable().Lookup(source_pc) && !rejected.GetLinkManager().QueryTarget(source_pc),
              "bad later-unit relocation rejects before earlier Call publication");
    }
    std::printf(
            "aot_call indirect=%u units=2 executions=4 faults=%u distinct_rx=1 source_destroyed=1 "
            "retired=1 "
            "target_spill_misses=%u native_divs=1 wide_divs=1\n",
            indirect,
            indirect ? 2u : 0u,
            spilled_misses);
}

void AotReject() {
    const auto bytes = 16 * static_cast<size_t>(getpagesize());
    void* source_guest = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void* guest = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(source_guest != MAP_FAILED && guest != MAP_FAILED, "map separate source and destination guest windows");
    const auto release = ScopeExit([&] { munmap(source_guest, bytes); munmap(guest, bytes); });
    std::memset(source_guest, 0xa5, bytes);
    std::memset(guest, 0xa5, bytes);
    Config config{};
    config.loc_start = 0;
    config.loc_end = bytes;
    config.enable_jit = true;
    config.backend_isa = kArm64;
    config.uniform_buffer_size = uniform_size;
    config.stack_alignment = 16;
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    config.memory_base = source_guest;
    config.guest_addr_mask = bytes - 1;
    auto source = std::make_unique<AddressSpace>(config);
    source->SetSerializableCodeEmission();
    CompileTarget(source->GetDefaultModule());
    CompileSource(source->GetDefaultModule(), false);
    const auto image = CaptureCalls(*source, source_guest, bytes);
    const auto blob = aot::EncodeInfoBlob(image);
    aot::AotImage pristine;
    std::string error;
    Check(aot::DecodeInfoBlob(blob.data(), blob.size(), pristine, error), error.c_str());
    pristine.host_isa = image.host_isa;
    pristine.code = image.code;
    source.reset();
    config.memory_base = guest;
    const auto entry_block = [](aot::AotImage& candidate, u64 pc) -> SerialBlock* {
        for (auto& unit : candidate.units) for (auto& block : unit.blocks)
            if (block.guest_start == pc && block.IsPublished()) return &block;
        return nullptr;
    };
    const auto expect_rejected = [&](aot::AotImage& candidate, const char* why) {
        AddressSpace destination{config};
        aot::LoadReport report;
        std::string reject_error;
        const auto status = aot::InstallArtifact(destination, candidate, guest, config.guest_addr_mask, report, reject_error);
        Check(status != aot::kAotOk && !report.units_installed && !report.mapping &&
              !destination.GetCodeCache(Location{source_pc}) && !destination.GetCodeCache(Location{target_pc}) &&
              !destination.GetCallCodeCacheTable().Lookup(target_pc) &&
              !destination.GetPendingCallCodeCacheTable().Lookup(target_pc) &&
              !destination.GetLinkManager().QueryTarget(source_pc) &&
              !destination.GetLinkManager().QueryTarget(target_pc), why);
    };
    unsigned rejected{};
    const auto mutate_target = [&](auto&& mutate, const char* why) {
        auto candidate = pristine;
        auto* block = entry_block(candidate, target_pc);
        Check(block != nullptr, "locate published target entry for contract mutation");
        mutate(*block, candidate);
        expect_rejected(candidate, why);
        ++rejected;
    };
    mutate_target([](SerialBlock& block, aot::AotImage& candidate) {
        block.continuation_code_offset = u32(candidate.units[1].code_size);
    }, "continuation offset at the code-size boundary rejects before publication");
    mutate_target([](SerialBlock& block, aot::AotImage&) {
        block.pending_flags_continuation_code_offset |= 1u;
    }, "misaligned pending continuation offset rejects before publication");
    mutate_target([](SerialBlock& block, aot::AotImage&) {
        block.pending_flags_contract.observed_nzcv_mask = 0x1f;
    }, "observed NZCV mask outside the NZCV nibble rejects before publication");
    mutate_target([](SerialBlock& block, aot::AotImage&) {
        block.pending_flags_contract.overwrite_before_observe = 0x08000000u;
    }, "overwrite mask outside host NZCV rejects before publication");
    mutate_target([](SerialBlock& block, aot::AotImage&) {
        block.pending_flags_contract.commits_before_fault = false;
    }, "pending entry without commit-before-fault rejects before publication");
    mutate_target([](SerialBlock& block, aot::AotImage&) {
        block.pending_flags_code_offset = UINT32_MAX;
    }, "pending continuation without a pending entry rejects before publication");
    mutate_target([](SerialBlock& block, aot::AotImage&) {
        block.code_offset = UINT32_MAX;
    }, "published entry without a canonical offset rejects before publication");
    mutate_target([](SerialBlock& block, aot::AotImage&) {
        block.entry_flags = u8(block.entry_flags | 0x80u);
    }, "unknown entry flag bits reject before publication");
    for (unsigned mutation = 0; mutation < 5; ++mutation) {
        auto candidate = pristine;
        auto& unit = candidate.units.back();
        const auto pair_offset = PairCallOffset(unit.region_branches);
        auto helper =
                std::find_if(unit.relocs.begin(), unit.relocs.end(), [&](const auto& relocation) {
                    return relocation.code_offset == pair_offset - 16;
                });
        Check(helper != unit.relocs.end(), "locate wide helper relocation for rejection matrix");
        if (mutation == 0) unit.relocs.erase(helper);
        if (mutation == 1) {
            const auto duplicate = *helper;
            unit.relocs.push_back(duplicate);
        }
        if (mutation == 2) {
            const u32 wrong_branch = 0x1400'0000u;
            std::memcpy(candidate.code.data() + unit.code_offset + pair_offset, &wrong_branch, 4);
        }
        if (mutation >= 3) {
            auto& branch = *std::find_if(
                    unit.region_branches.begin(),
                    unit.region_branches.end(),
                    [&](const auto& entry) { return entry.code_offset == pair_offset; });
            if (mutation == 3)
                branch.mask = 1;
            else
                branch.kind = SerialRegionBranchKind::Return;
        }
        expect_rejected(
                candidate,
                "malformed pair-call recipe or helper relocation rejects before any publication");
        ++rejected;
    }
    {
        auto candidate = pristine;
        Check(!candidate.units.front().link_sites.empty(), "static Call capture carries its serial site");
        candidate.units.front().link_sites.front().kind = 0xff;
        expect_rejected(candidate, "invalid serial Call site kind rejects before publication");
        ++rejected;
    }
    {
        auto candidate = pristine;
        auto* block = entry_block(candidate, target_pc);
        Check(block != nullptr, "locate published target entry for ownership mutation");
        candidate.units.front().blocks.push_back(*block);
        expect_rejected(candidate, "duplicate published Call entry ownership rejects before publication");
        ++rejected;
    }
    {
        auto candidate = pristine;
        candidate.host_isa = kRiscv64;
        expect_rejected(candidate, "foreign host ISA rejects before publication");
        ++rejected;
    }
    {
        auto candidate = pristine;
        candidate.key.format_version = kCacheFormatVersion - 1;
        expect_rejected(candidate, "stale cache format rejects before publication");
        ++rejected;
    }
    {
        auto candidate = pristine;
        candidate.key.build_id ^= 1;
        expect_rejected(candidate, "foreign build id rejects before publication");
        ++rejected;
    }
    {
        auto candidate = pristine;
        candidate.key.config_hash ^= 1;
        expect_rejected(candidate, "foreign runtime config rejects before publication");
        ++rejected;
    }
    {
        auto candidate = pristine;
        candidate.key.env_hash ^= 1;
        expect_rejected(candidate, "foreign environment hash rejects before publication");
        ++rejected;
    }
    {
        auto candidate = pristine;
        candidate.guest.entry += 4;
        expect_rejected(candidate, "edited guest entry rejects before publication");
        ++rejected;
    }
    {
        auto* bytes_guest = static_cast<u8*>(guest);
        const u8 original = bytes_guest[source_pc];
        bytes_guest[source_pc] = u8(original ^ 0xffu);
        expect_rejected(pristine, "mutated guest bytes reject before publication");
        bytes_guest[source_pc] = original;
        ++rejected;
    }
    {
        AddressSpace destination{config};
        ModuleConfig module_config{};
        module_config.optimizations = Optimizations::ReturnStackBuffer;
        Check(destination.MapModule(0, bytes, module_config) != nullptr,
              "map a direct-link-disabled module over the destination window");
        aot::LoadReport report;
        std::string reject_error;
        const auto status = aot::InstallArtifact(destination, pristine, guest, config.guest_addr_mask, report, reject_error);
        Check(status != aot::kAotOk && !report.units_installed && !report.mapping &&
              !destination.GetCodeCache(Location{source_pc}) && !destination.GetCodeCache(Location{target_pc}) &&
              !destination.GetCallCodeCacheTable().Lookup(target_pc) &&
              !destination.GetPendingCallCodeCacheTable().Lookup(target_pc),
              "a module without direct-link configuration rejects serial Call metadata before publication");
        ++rejected;
    }
    {
        auto candidate = pristine;
        auto* block = entry_block(candidate, target_pc);
        Check(block != nullptr && block->pending_flags_contract.CanPublishPendingEntry(),
              "locate a pending-capable target contract for the compatibility case");
        block->pending_flags_contract.observed_nzcv_mask = 0xf;
        AddressSpace destination{config};
        aot::LoadReport report;
        Check(aot::InstallArtifact(destination, candidate, guest, config.guest_addr_mask, report, error) == aot::kAotOk &&
              report.units_installed == 2, error.c_str());
        const auto module = destination.GetDefaultModule();
        auto* source_base = static_cast<u8*>(aot::ResolveCodeOffset(report.mapping, candidate.units.front().code_offset));
        for (unsigned iteration = 0; iteration < 2; ++iteration) {
            Runtime runtime{&destination};
            const u64 input = 36, resume = return_pc;
            std::memcpy(runtime.GetUniformBuffer().data() + 8, &input, 8);
            std::memcpy(runtime.GetUniformBuffer().data() + 16, &resume, 8);
            InitializeCallInputs(runtime);
            runtime.SetLocation(source_pc);
            Check(runtime.Run() == HaltReason::CallHost, "contract-incompatible pending Call still executes correctly");
            CheckCallEffects(runtime, input, false);
            u64 value{}, witness{};
            std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
            std::memcpy(&witness, runtime.GetUniformBuffer().data() + 24, 8);
            Check(value == input + 1 && witness == returned &&
                  (runtime.GetState()->host_cpu_flags & 0xf0000000u) == 0,
                  "canonical fallback preserves Call result, return body and NZCV");
        }
        const auto sites = destination.GetLinkManager().QuerySourceSites({module.get(), source_base});
        Check(sites.size() == 1 && sites.front().state == LinkSiteState::Linked && !sites.front().pending_flags_compatible,
              "observing contract links canonically without pending admission");
    }
    {
        AddressSpace destination{config};
        aot::LoadReport report;
        Check(aot::InstallArtifact(destination, pristine, guest, config.guest_addr_mask, report, error) == aot::kAotOk &&
              report.units_installed == 2, "pristine artifact still installs after every rejection");
        Runtime runtime{&destination};
        const u64 input = 36, resume = return_pc;
        std::memcpy(runtime.GetUniformBuffer().data() + 8, &input, 8);
        std::memcpy(runtime.GetUniformBuffer().data() + 16, &resume, 8);
        InitializeCallInputs(runtime);
        runtime.SetLocation(source_pc);
        Check(runtime.Run() == HaltReason::CallHost, "pristine Call executes after the rejection matrix");
        CheckCallEffects(runtime, input, false);
        u64 value{}, witness{};
        std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
        std::memcpy(&witness, runtime.GetUniformBuffer().data() + 24, 8);
        Check(value == input + 1 && witness == returned, "pristine result and return body stay correct");
    }
    std::printf("aot_reject cases=%u rejected=%u compatible=1 pristine=1\n", rejected + 2, rejected);
}

void AotProducer() {
#if defined(__ANDROID__)
    char directory[] = "/data/local/tmp/swiftvm-aot-call.XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-aot-call.XXXXXX";
#endif
    Check(mkdtemp(directory) != nullptr, "create owned real AOT producer fixture directory");
    const auto cleanup = ScopeExit([&] { std::filesystem::remove_all(directory); });
    const std::string guest_path = std::string{directory} + "/guest.elf";
    const std::string artifact_path = std::string{directory} + "/guest.aot";
    using namespace ELFIO;
    elfio guest;
    guest.create(ELFCLASS64, ELFDATA2LSB);
    guest.set_os_abi(ELFOSABI_LINUX);
    guest.set_type(ET_EXEC);
    guest.set_machine(EM_X86_64);
    guest.set_entry(0x401020);
    auto* text = guest.sections.add(".text");
    text->set_type(SHT_PROGBITS);
    text->set_flags(SHF_ALLOC | SHF_EXECINSTR);
    text->set_address(0x401000);
    text->set_addr_align(16);
    std::array<char, 64> code{};
    const unsigned char caller[] = {0xb8, 36, 0, 0, 0, 0xe8, 0xd6, 0xff, 0xff, 0xff, 0xc3};
    const unsigned char callee[] = {0x48, 0x83, 0xc0, 1, 0xc3};
    // Discovery visits the callee first, making the caller's edge cross a
    // real allocation boundary instead of absorbing the callee into its CFG.
    std::memcpy(code.data(), callee, sizeof(callee));
    std::memcpy(code.data() + 32, caller, sizeof(caller));
    text->set_data(code.data(), code.size());
    // ELFIO places a section-less PT_PHDR over the actual header table.
    // Turn that fixture descriptor into a read-only LOAD after layout so the
    // ordinary guest loader can find AT_PHDR in mapped guest memory.
    auto* headers = guest.segments.add();
    headers->set_type(PT_PHDR);
    headers->set_flags(PF_R);
    headers->set_virtual_address(0x400040);
    headers->set_physical_address(0x400040);
    headers->set_align(8);
    auto* load = guest.segments.add();
    load->set_type(PT_LOAD);
    load->set_flags(PF_R | PF_X);
    load->set_virtual_address(0x401000);
    load->set_physical_address(0x401000);
    load->set_align(0x1000);
    load->add_section_index(text->get_index(), 16);
    auto* strings = guest.sections.add(".strtab");
    strings->set_type(SHT_STRTAB);
    auto* table = guest.sections.add(".symtab");
    table->set_type(SHT_SYMTAB);
    table->set_link(strings->get_index());
    table->set_entry_size(guest.get_default_entry_size(SHT_SYMTAB));
    table->set_addr_align(8);
    table->set_info(1);
    string_section_accessor names{strings};
    symbol_section_accessor symbols{guest, table};
    symbols.add_symbol(names, "caller", 0x401020, sizeof(caller), STB_GLOBAL, STT_FUNC, 0, text->get_index());
    symbols.add_symbol(names, "callee", 0x401000, sizeof(callee), STB_GLOBAL, STT_FUNC, 0, text->get_index());
    Check(guest.save(guest_path), "write executable x86 guest with actual static Call and symbols");
    const int fixture = open(guest_path.c_str(), O_WRONLY | O_CLOEXEC);
    Check(fixture >= 0, "open own ELF fixture for header mapping descriptor");
    const u32 load_type = PT_LOAD;
    const bool mapped_headers = pwrite(fixture, &load_type, sizeof(load_type), guest.get_segments_offset()) == sizeof(load_type);
    const int closed = close(fixture);
    Check(mapped_headers && closed == 0, "map program header table through a valid read-only LOAD");
    aot::CompileOptions options{.guest_elf = guest_path, .out_path = artifact_path, .verbose = true,
                                .decode_budget = 64, .sweep = true};
    aot::AotStats stats;
    std::string error;
    Check(aot::CompileArtifact(options, stats, error), error.c_str());
    Check(stats.units_emitted >= 2 && !stats.fail_scan && !stats.fail_translate && !stats.symbols_stubbed,
          "production AOT compiler captures every real function without rejected scans or stubs");
    aot::AotGuestEnv env;
    Check(env.Init(guest_path, artifact_path, error), error.c_str());
    Check(env.load_report().units_installed == env.artifact().units.size(), "real artifact installs all producer units");
    size_t calls{};
    for (const auto& unit : env.artifact().units) for (const auto& site : unit.link_sites)
        calls += site.kind == u8(LinkSiteKind::Call);
    if (!calls) {
        std::string detail = "real producer has no physical Call recipes:";
        for (const auto& unit : env.artifact().units) {
            detail += " unit=" + std::to_string(unit.guest_start) + " function=" + std::to_string(unit.is_function) +
                      " blocks=" + std::to_string(unit.blocks.size()) + " links=" + std::to_string(unit.link_sites.size());
            for (const auto& site : unit.link_sites) detail += " kind=" + std::to_string(site.kind);
        }
        throw std::runtime_error(detail);
    }
    Check(calls > 0, "real production producer preserves physical static Call recipes");
    for (const char* name : {"caller", "callee"}) {
        bool mismatch{};
        Check(env.EntryOf(name, mismatch) && !mismatch, "artifact symbols resolve to exact published native entries");
    }
    auto caller_fn = env.Lookup<u64()>("caller");
    for (unsigned iteration = 0; iteration < 3; ++iteration) {
        const auto result = caller_fn.Try();
        Check(result.ok() && result.value() == 37, "actual compiled artifact executes static Call by rewritten symbol");
    }
    std::printf("aot_producer units=%u calls=%zu symbol_calls=3 result=37\n", stats.units_emitted, calls);
}

void Phase(std::string_view phase, bool indirect, const char* directory) {
    Check(FlagsRegsEnabled(), "production Call persistence requires flags1");
    Check(SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0, "set isolated production disk-cache directory");
    const auto bytes = 16 * static_cast<size_t>(getpagesize());
    void* guest = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(guest != MAP_FAILED, "map child guest image");
    const auto release = ScopeExit([&] { munmap(guest, bytes); });
    std::memset(guest, 0xa5, bytes);
    Config config{};
    config.loc_start = 0;
    config.loc_end = bytes;
    config.enable_jit = true;
    config.backend_isa = kArm64;
    config.uniform_buffer_size = uniform_size;
    config.stack_alignment = 16;
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    config.memory_base = guest;
    config.guest_addr_mask = bytes - 1;
    {
        AddressSpace space{config};
        space.LoadJitCache();
        const auto module = space.GetDefaultModule();
        const auto disk = space.GetJitDiskCache();
        Check(disk && disk->Enabled(), "production disk cache admits complete flags1 Call contract");
        if (phase == "reject") {
            Check(disk->Stats().reject_header.load() == 1 && disk->Stats().units_loaded.load() == 0 &&
                  !space.GetCodeCache(Location{source_pc}) && !space.GetCodeCache(Location{target_pc}) &&
                  !space.GetCallCodeCacheTable().Lookup(target_pc) && !space.GetPendingCallCodeCacheTable().Lookup(target_pc),
                  "old flags1 cache format rejects every canonical and Call publication");
            std::printf("phase=reject pid=%d loaded=0 compiled=0 checks=%u\n", getpid(), checks);
            return;
        }
        if (phase == "write") {
            Check(disk->Stats().units_loaded.load() == 0, "writer starts without inherited units");
            CompileTarget(module);
            CompileSource(module, indirect);
            Check(disk->Stats().units_compiled.load() == 2 && disk->Stats().reject_scan.load() == 0,
                  "production recorder captures both real flags1 Call units without scanner rejection");
        } else {
            Check(phase == "read" && disk->Stats().units_loaded.load() == 2 &&
                  disk->Stats().units_compiled.load() == 0 && disk->Stats().reject_reloc.load() == 0 &&
                  disk->Stats().reject_guest_bytes.load() == 0,
                  "fresh process revives exactly two units without compiling or relocation fallback");
        }
        const auto target = space.GetLinkManager().QueryTarget(target_pc);
        Check(target && target->call_host_pc && target->pending_flags_host_pc &&
              target->call_pending_flags_host_pc &&
              target->call_host_pc == reinterpret_cast<void*>(space.GetCallCodeCacheTable().Lookup(target_pc)) &&
              target->call_pending_flags_host_pc == reinterpret_cast<void*>(space.GetPendingCallCodeCacheTable().Lookup(target_pc)),
              "actual target publishes canonical, pending and both physical Call entries");
        FaultEntry target_extent;
        const auto* target_allocation = static_cast<const u8*>(target->target_owner.allocation);
        Check(module->LookupCodeAllocation(target_allocation, target_extent),
              "resolve the shared-helper allocation after compilation or disk revival");
        SerialUnit target_code;
        std::string capture_error;
        const bool captured = CaptureSerialCode(*module,
                                                target_allocation,
                                                u32(target_extent.host_end - target_allocation),
                                                GetHostImage(),
                                                bytes,
                                                target_code,
                                                &capture_error);
        Check(captured, capture_error.c_str());
        CheckTargetCode(target_code);
        auto* target_cache = module->GetCodeCache(const_cast<u8*>(target_allocation));
        Check(target_cache != nullptr, "resolve actual division Function code region");
        CheckPairCallBinding(
                target_allocation, PairCallOffset(target_code.region_branches), *target_cache);
        auto* source_entry = space.GetCodeCache(Location{source_pc});
        auto* target_entry = space.GetCodeCache(Location{target_pc});
        Check(source_entry && target_entry && space.GetCodeCache(Location{return_pc}),
              "source, target and guest return label are all present before native execution");
        const auto node = module->GetNode(Location{source_pc});
        Check(IsFunction(node), "source unit retains its real function owner");
        auto source_function = GetFunction(node);
        auto* source_allocation = static_cast<u8*>(module->GetJitCache(source_function->GetJitCache()));
        const auto sites = space.GetLinkManager().QuerySourceSites({module.get(), source_allocation});
        Check(indirect ? sites.empty() : sites.size() == 1 && sites.front().kind == LinkSiteKind::Call,
              "recorded source is the expected actual static-Call or indirect shape");
        if (!indirect && phase == "read") Check(sites.front().state == LinkSiteState::Unlinked,
              "fresh process restores static Call in cold unlinked state");
        unsigned iteration{}, spilled_misses{};
        for (const auto input : {u64{36}, ~u64{0}, u64{36}, ~u64{0}}) {
            const bool forced_miss = indirect && iteration >= 2;
            if (forced_miss) Check(space.GetCallCodeCacheTable().Zero(target_pc) &&
                                   space.GetPendingCallCodeCacheTable().Zero(target_pc),
                                   "force actual canonical and pending Call target misses");
            Runtime runtime{&space};
            MissObservation observed{.state = runtime.GetState(), .target = target_pc};
            SignalHandler::RegisterHandler(&ObserveMiss, &observed, -1);
            const auto unregister = ScopeExit([&] { SignalHandler::UnregisterHandler(&observed); });
            const u64 resume = return_pc, target_address = target_pc;
            std::memcpy(runtime.GetUniformBuffer().data() + 8, &input, 8);
            std::memcpy(runtime.GetUniformBuffer().data() + 16, &resume, 8);
            std::memcpy(runtime.GetUniformBuffer().data() + 32, &target_address, 8);
            InitializeCallInputs(runtime);
            runtime.SetLocation(source_pc);
            Check(runtime.Run() == HaltReason::CallHost, "fresh/warm Call executes and returns to native host");
            CheckCallEffects(runtime, input, indirect);
            u64 value{}, return_marker{};
            std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
            std::memcpy(&return_marker, runtime.GetUniformBuffer().data() + 24, 8);
            Check(value == input + 1 && return_marker == returned,
                  "actual target result and guest return-body witness are both correct");
            if ((runtime.GetState()->host_cpu_flags & 0xf0000000u) != (value == 0 ? 0x60000000u : 0))
                throw std::runtime_error("Call NZCV mismatch: input=" + std::to_string(input) +
                    " result=" + std::to_string(value) + " flags=" + std::to_string(runtime.GetState()->host_cpu_flags));
            Check((runtime.GetState()->host_cpu_flags & 0xf0000000u) == (value == 0 ? 0x60000000u : 0),
                  "Call revival preserves independent target NZCV through physical return");
            Check(runtime.GetState()->rsb_empty && runtime.GetState()->rsb_pointer == runtime.GetState()->rsb_empty,
                  "actual static/indirect Call balances its external continuation frame");
            Check(observed.count.load() == (forced_miss ? 1u : 0u),
                  "only explicitly missing indirect entries take one genuine native fault");
            if (forced_miss) {
                Check(observed.target_spill_slots.load() > 0,
                      "the compiled or disk-revived indirect target really has spill backing at "
                      "its fault");
                ++spilled_misses;
                FaultEntry recovery;
                Check(module->LookupFault(reinterpret_cast<const u8*>(observed.link_register.load() - 4), recovery) &&
                      recovery.recovery_kind == FaultRecoveryKind::IndirectCallMiss && recovery.recovery &&
                      recovery.guest_loc == source_pc,
                      "actual fault uses the source's persisted precise IndirectCallMiss recovery recipe");
                Check(space.PushCallCodeCache(Location{target_pc}, target->call_host_pc) &&
                      space.PushPendingCallCodeCache(Location{target_pc}, target->call_pending_flags_host_pc),
                      "restore existing target entries after exercising their missing-entry fault path");
            }
            ++iteration;
        }
        Check(space.GetCodeCache(Location{source_pc}) == source_entry && space.GetCodeCache(Location{target_pc}) == target_entry,
              "execution retains restored code identities without recompiling");
        Check(disk->Stats().units_compiled.load() == (phase == "write" ? 2 : 0),
              "all cold and warm executions use the admitted code");
        if (phase == "write") {
            disk->Save();
            Check(disk->Stats().units_stored.load() == 2, "production Save writes both complete Call units");
        } else {
            space.InvalidateCodeRange(target_pc, target_pc + 4);
            Check(!space.GetCodeCache(Location{target_pc}) && !space.GetCallCodeCacheTable().Lookup(target_pc) &&
                  !space.GetPendingCallCodeCacheTable().Lookup(target_pc) && space.GetCodeCache(Location{source_pc}) == source_entry,
                  "target range retirement revokes all restored Call entries while preserving source ownership");
        }
        std::printf(
                "phase=%.*s pid=%d loaded=%llu compiled=%llu host_image=0x%llx helper_relocs=4 "
                "checks=%u target_spill_misses=%u native_divs=1 wide_divs=1\n",
                static_cast<int>(phase.size()),
                phase.data(),
                getpid(),
                static_cast<unsigned long long>(disk->Stats().units_loaded.load()),
                static_cast<unsigned long long>(disk->Stats().units_compiled.load()),
                static_cast<unsigned long long>(GetHostImage().base),
                checks,
                spilled_misses);
    }
}

void Child(const char* executable, const char* mode, const char* phase, const char* directory) {
    const auto pid = fork();
    Check(pid >= 0, "fork independent disk-cache phase");
    if (pid == 0) {
        setenv("CALL_CACHE_PHASE", phase, 1);
        setenv("CALL_CACHE_DIR", directory, 1);
        execl(executable, executable, mode, static_cast<char*>(nullptr));
        _exit(127);
    }
    int status{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
        const auto result = waitpid(pid, &status, WNOHANG);
        if (result < 0) throw std::runtime_error("observe child process status");
        if (result == pid) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            throw std::runtime_error("native disk-cache phase timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "independent writer/reader/rejection process passes full native oracle");
}

void Matrix(const char* mode) {
    std::array<char, 4096> executable{};
    const auto size = readlink("/proc/self/exe", executable.data(), executable.size() - 1);
    Check(size > 0, "locate current executable for fresh exec phases");
    std::array<char, 128> directory{};
#if defined(__ANDROID__)
    std::strcpy(directory.data(), "/data/local/tmp/swiftvm-flags1-call.XXXXXX");
#else
    std::strcpy(directory.data(), "/tmp/swiftvm-flags1-call.XXXXXX");
#endif
    Check(mkdtemp(directory.data()) != nullptr, "allocate private cache directory");
    const auto cleanup = ScopeExit([&] { std::filesystem::remove_all(directory.data()); });
    Child(executable.data(), mode, "write", directory.data());
    Child(executable.data(), mode, "read", directory.data());
    std::filesystem::path cache;
    for (const auto& entry : std::filesystem::directory_iterator(directory.data())) {
        if (entry.path().extension() == ".bin" && entry.path().filename().string().starts_with("svmjit-")) {
            Check(cache.empty(), "one exact cache file is produced");
            cache = entry.path();
        }
    }
    Check(!cache.empty(), "production writer created the persisted cache");
    const int fd = open(cache.c_str(), O_WRONLY | O_CLOEXEC);
    Check(fd >= 0, "open own cache fixture for old-version mutation");
    const u64 previous = kCacheFormatVersion - 1;
    const bool written = pwrite(fd, &previous, sizeof(previous), 8) == sizeof(previous);
    const int closed = close(fd);
    Check(written && closed == 0, "mutate only disk format to previous version");
    Child(executable.data(), mode, "reject", directory.data());
}
}

int main(int argc, char** argv) {
    try {
        const auto* mode = argc > 1 ? argv[1] : "static";
        if (std::string_view{mode} == "aot-static" || std::string_view{mode} == "aot-indirect" ||
            std::string_view{mode} == "aot-producer" || std::string_view{mode} == "aot-reject") {
            if (std::string_view{mode} == "aot-producer") AotProducer();
            else if (std::string_view{mode} == "aot-reject") AotReject();
            else {
                const bool indirect = std::string_view{mode} == "aot-indirect";
                AotCalls(indirect);
                if (!indirect) AotEligibleLoop();
            }
            std::printf("mode=%s checks=%u passed\n", mode, checks);
            return 0;
        }
        Check(std::string_view{mode} == "static" || std::string_view{mode} == "indirect", "known production Call mode");
        if (const char* phase = std::getenv("CALL_CACHE_PHASE")) {
            const auto directory = std::getenv("CALL_CACHE_DIR");
            Check(directory != nullptr, "child received isolated cache directory");
            Phase(phase, std::string_view{mode} == "indirect", directory);
        } else {
            Matrix(mode);
            std::printf("mode=%s checks=%u passed\n", mode, checks);
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "failure: %s\n", error.what());
        return 1;
    }
}
