#include "test_support.h"

#include <algorithm>
#include <bit>
#include <functional>
#include <memory>
#include <vector>
#include "runtime/backend/address_space.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/backend/module.h"
#include "runtime/frontend/ir_assembler.h"

namespace swift::tests::loongarch64 {
namespace {
using T = ir::ValueType;
using O = ir::OpCode;
using V = ir::Value;
using M = la::ScalarAllocationMode;
using Bytes = std::array<u8, 32>;
using Memory = std::array<u8, 512>;
constexpr u32 kUniformBytes = 4096, kSide = 2048, kOutput = 3072;
constexpr u32 kFPLeft = 1200, kFPRight = 1232;
constexpr u32 kCondition = 3840, kCount = 3848, kAddress = 3856, kScalar = 3864;
constexpr u32 kShiftCount = 3872, kShiftMask = 3888;
constexpr u64 kCountMask = 0x18357a2b6c9de4f0ULL;

u32 Width(T type) { return type == T::V256 ? 32 : 16; }

Bytes Read(const StateStorage& state, u32 offset, u32 width) {
    Bytes result{};
    std::memcpy(result.data(), state.state->uniform_buffer_begin + offset, width);
    return result;
}

void Write(StateStorage& state, u32 offset, const Bytes& value, u32 width) {
    std::memcpy(state.state->uniform_buffer_begin + offset, value.data(), width);
}

// This model uses byte arrays and explicit lane arithmetic. It never reads
// emitted IR, register assignments, canonical homes, or JIT output.
Bytes Binary(const Bytes& left, const Bytes& right, u32 width, u32 bits, O op) {
    Bytes result{};
    const u32 size = bits / 8;
    for (u32 offset = 0; offset < width; offset += size) {
        u64 a{}, b{}, value{};
        std::memcpy(&a, left.data() + offset, size);
        std::memcpy(&b, right.data() + offset, size);
        switch (op) {
            case O::VecAdd: value = a + b; break;
            case O::VecSub: value = a - b; break;
            case O::VecXor: value = a ^ b; break;
            case O::VecOr: value = a | b; break;
            default: throw std::runtime_error("invalid CFG independent lane operation");
        }
        std::memcpy(result.data() + offset, &value, size);
    }
    return result;
}

Bytes Shift(const Bytes& input, u64 count, u32 width, u32 bits, O op) {
    Bytes result{};
    const u32 size = bits / 8;
    const u64 mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
    for (u32 offset = 0; offset < width; offset += size) {
        u64 lane{}, value{};
        std::memcpy(&lane, input.data() + offset, size);
        if (op == O::VecShiftLeft) value = count >= bits ? 0 : lane << count;
        else if (op == O::VecShiftRight) value = count >= bits ? 0 : lane >> count;
        else {
            const auto clamped = u32(std::min(count, u64(bits - 1)));
            value = lane >> clamped;
            if (clamped && (lane & (u64{1} << (bits - 1))))
                value |= mask ^ (mask >> clamped);
        }
        std::memcpy(result.data() + offset, &value, size);
    }
    return result;
}

Bytes Extract(const Bytes& left, const Bytes& right, u32 width, u32 count) {
    Bytes result{};
    for (u32 byte = 0; byte < width; ++byte)
        result[byte] = byte + count < width ? left[byte + count] : right[byte + count - width];
    return result;
}

Bytes Shuffle32(const Bytes& input, u32 width, u32 control) {
    Bytes result{};
    for (u32 group = 0; group < width; group += 16)
        for (u32 lane = 0; lane < 4; ++lane)
            for (u32 byte = 0; byte < 4; ++byte)
                result[group + lane * 4 + byte] = input[group + ((control >> (lane * 2)) & 3) * 4 + byte];
    return result;
}

Bytes Zip(const Bytes& left, const Bytes& right, u32 width, u32 bits, bool high) {
    Bytes result{};
    const u32 size = bits / 8, half_lanes = width / (2 * size);
    for (u32 lane = 0; lane < half_lanes; ++lane)
        for (u32 byte = 0; byte < size; ++byte) {
            const auto input = (lane + (high ? half_lanes : 0)) * size + byte;
            result[(lane * 2) * size + byte] = left[input];
            result[(lane * 2 + 1) * size + byte] = right[input];
        }
    return result;
}

u64 callback_calls{}, callback_argument{}, callback_clobbers{};

u64 ClobberCallerVectors(u64 value, u64, u64) {
    ++callback_calls;
    callback_argument = value;
#if defined(__loongarch__)
    const auto features = la::HostFeatures::Detect();
    if (features.lasx) {
#define CLOBBER_X(n) "xvxor.v $xr" #n ", $xr" #n ", $xr" #n "\n\t"
        asm volatile(
            CLOBBER_X(8) CLOBBER_X(9) CLOBBER_X(10) CLOBBER_X(11)
            CLOBBER_X(12) CLOBBER_X(13) CLOBBER_X(14) CLOBBER_X(15)
            CLOBBER_X(16) CLOBBER_X(17) CLOBBER_X(18) CLOBBER_X(19)
            CLOBBER_X(20) CLOBBER_X(21) CLOBBER_X(22) CLOBBER_X(23)
            ::: "f8", "f9", "f10", "f11", "f12", "f13", "f14", "f15",
                "f16", "f17", "f18", "f19", "f20", "f21", "f22", "f23", "memory");
#undef CLOBBER_X
        callback_clobbers += 16;
    } else if (features.lsx) {
#define CLOBBER_V(n) "vxor.v $vr" #n ", $vr" #n ", $vr" #n "\n\t"
        asm volatile(
            CLOBBER_V(8) CLOBBER_V(9) CLOBBER_V(10) CLOBBER_V(11)
            CLOBBER_V(12) CLOBBER_V(13) CLOBBER_V(14) CLOBBER_V(15)
            CLOBBER_V(16) CLOBBER_V(17) CLOBBER_V(18) CLOBBER_V(19)
            CLOBBER_V(20) CLOBBER_V(21) CLOBBER_V(22) CLOBBER_V(23)
            ::: "f8", "f9", "f10", "f11", "f12", "f13", "f14", "f15",
                "f16", "f17", "f18", "f19", "f20", "f21", "f22", "f23", "memory");
#undef CLOBBER_V
        callback_clobbers += 16;
    }
#endif
    return std::rotl(value, 17) + 29;
}

struct Expected {
    bool callback{};
    u64 argument{};
    u32 calls{1};
};

struct Fixture {
    std::string name;
    T type;
    IntrusivePtr<ir::Block> block;
    la::RegisterPlan::Targets targets;
    la::RegisterPlan::Stops stops;
    std::function<Expected(StateStorage&, Memory&)> model;
    bool interpreter{true}, pressure{}, boundary{}, improves{}, publication{}, dense{};
    u32 publication_sites{2}, publication_live_reloads{6};
    std::array<ir::Inst*, 3> boundary_homes{};
    Fixture(std::string name, T type, u64 location)
        : name(std::move(name)), type(type), block(new ir::Block(ir::Location{location})) {}
    void Finish() {
        block->SetTerminal(ir::terminal::ReturnToHost{});
        block->ReIdInstr();
    }
};

struct CFGCompiled {
    Config config{TestConfig()};
    la::JitContext context;
    la::JitTranslator translator{context};
    backend::CodeCache cache{config, 1u << 20, FeatureSet{}};
    BlockFn fn{};
    M scalar;
    CFGCompiled(Fixture& fixture, la::HostFeatures features, bool global,
                M scalar = M::GlobalCFG, backend::Module* owner = nullptr)
        : context(true, features, {}, scalar), scalar(scalar) {
        context.SetVectorAllocation(true);
        context.SetVectorCFGAllocation(global);
        if (fixture.dense) context.SetSparseHomes(false);
        // Uniform forwarding is independently qualified. Keep both modes on
        // the same unforwarded IR so this fixture measures vector CFG edges.
        context.SetUniformForwarding(false);
        if (owner) context.ConfigureModule(owner);
        context.ConfigureFlow(fixture.targets, fixture.stops, {});
        translator.Translate(fixture.block.get());
        auto buffer = cache.AllocCode(context.CurrentBufferSize());
        Check(buffer.has_value(), "allocate vector CFG fixture=" + fixture.name);
        context.Flush(*buffer);
        fn = reinterpret_cast<BlockFn>(buffer->exec_data);
    }
};

void Initialize(StateStorage& state, Memory& memory, u32 sample) {
    u64 seed = 0x684958a7d83bf095ULL ^ (u64(sample + 1) * 0x9e3779b97f4a7c15ULL);
    for (u32 offset = 0; offset < kUniformBytes; offset += 8) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        const u64 value = sample == 0 ? (u64(offset) + 1) * 0x0123456789abcdefULL :
                          sample == 1 ? UINT64_MAX ^ u64(offset) :
                          sample == 2 ? 0x8000800080008000ULL ^ (u64(offset) << 24) :
                          sample == 3 ? 0x7fff7fff7fff7fffULL ^ (u64(offset) << 32) : seed;
        state.Put(offset, value);
    }
    state.Put(kCondition, sample & 1);
    state.Put(kCount, 1 + sample % 17);
    constexpr std::array<u64, 4> addresses{1, 17, 31, 127};
    state.Put(kAddress, addresses[sample % addresses.size()]);
    state.Put(kScalar, std::rotl(seed, 11));
    // Every count appears on both conditional paths in the 24-sample fixture.
    constexpr std::array<u64, 12> counts{0, 1, 7, 8, 15, 16, 31, 32, 63, 64, 65, UINT64_MAX};
    state.Put(kShiftCount, counts[(sample / 2) % counts.size()] ^ kCountMask);
    state.Put(kShiftMask, kCountMask);
    const u32 fp_left = std::bit_cast<u32>(float(1 + sample % 8));
    const u32 fp_right = std::bit_cast<u32>(float(s32(sample % 5) - 2) / 4.0f);
    std::memcpy(state.state->uniform_buffer_begin + kFPLeft, &fp_left, 4);
    std::memcpy(state.state->uniform_buffer_begin + kFPRight, &fp_right, 4);
    for (u32 i = 0; i < memory.size(); ++i)
        memory[i] = u8(0x9b + i * 29 + sample * 7);
    state.state->pt = memory.data();
    state.state->guest_addr_limit = memory.size();
    state.state->guest_addr_mask = memory.size() - 1;
    state.state->host_cpu_flags = std::rotl(seed, 23);
}

void Equal(const StateStorage& actual, const StateStorage& expected,
           const Memory& actual_memory, const Memory& expected_memory, const std::string& detail) {
    u32 difference = kUniformBytes;
    for (u32 i = 0; i < kUniformBytes; ++i)
        if (actual.state->uniform_buffer_begin[i] != expected.state->uniform_buffer_begin[i]) {
            difference = i; break;
        }
    Check(difference == kUniformBytes && actual_memory == expected_memory &&
          actual.state->host_cpu_flags == expected.state->host_cpu_flags &&
          actual.state->current_loc == expected.state->current_loc &&
          actual.state->exit_request == expected.state->exit_request,
          detail + " first_uniform_difference=" + std::to_string(difference) +
          " actual=" + std::to_string(difference < kUniformBytes ? actual.state->uniform_buffer_begin[difference] : 0) +
          " expected=" + std::to_string(difference < kUniformBytes ? expected.state->uniform_buffer_begin[difference] : 0));
}

IntrusivePtr<ir::Block> Portable(ir::Block* source) {
    if (!la::WidePlan::Needed(source)) return {};
    auto lowered = la::WidePlan::Build(source);
    // Native continuations carry pointer+16 metadata. The interpreter does
    // not consume it, so make that address explicit only on its private IR.
    for (auto* high : lowered.memory_continuations) {
        auto* address = new ir::Inst{O::GetOperand};
        address->SetArgs(high->GetArg<ir::Operand>(0)); address->SetReturn(T::U64);
        lowered.body->InsertBefore(address, high);
        auto* plus16 = new ir::Inst{O::Add};
        plus16->SetArgs(V{address}, ir::Operand{ir::Imm{u64{16}}}); plus16->SetReturn(T::U64);
        lowered.body->InsertBefore(plus16, high);
        high->SetArg(0, ir::Operand{V{plus16}});
    }
    lowered.body->ReIdInstr();
    return lowered.body;
}

std::vector<u16> SourceIDs(ir::Block* block) {
    std::vector<u16> ids;
    for (auto& inst : block->GetInstList()) ids.push_back(inst.Id());
    return ids;
}

void Compare(Fixture& fixture, CFGCompiled& compiled, bool global, u32 samples = 24) {
    auto portable = fixture.interpreter ? Portable(fixture.block.get()) : IntrusivePtr<ir::Block>{};
    for (u32 sample = 0; sample < samples; ++sample) {
        StateStorage expected, actual;
        Memory expected_memory{}, actual_memory{};
        Initialize(expected, expected_memory, sample);
        Initialize(actual, actual_memory, sample);
        const auto behavior = fixture.model(expected, expected_memory);
        const auto detail = fixture.name + " width=" + std::to_string(Width(fixture.type) * 8) +
            " global=" + std::to_string(global) + " lasx=" + std::to_string(compiled.context.Features().lasx) +
            " scalar=" + std::to_string(u32(compiled.scalar)) +
            " sample=" + std::to_string(sample);
        if (fixture.interpreter) {
            StateStorage interpreted;
            Memory interpreted_memory{};
            Initialize(interpreted, interpreted_memory, sample);
            backend::interp::Interpreter interpreter{*interpreted.state,
                portable ? portable.get() : fixture.block.get()};
            Check(interpreter.Run() == HaltReason::CallHost, "independent interpreter halt " + detail);
            Equal(interpreted, expected, interpreted_memory, expected_memory, "C lane model versus interpreter " + detail);
        }
        callback_calls = callback_argument = callback_clobbers = 0;
        Check(compiled.fn(actual.state) == HaltReason::CallHost, "vector CFG actual halt " + detail);
        Equal(actual, expected, actual_memory, expected_memory, "vector CFG complete result " + detail);
        if (behavior.callback) {
            Check(callback_calls == behavior.calls && callback_argument == behavior.argument &&
                  callback_clobbers == behavior.calls * (la::HostFeatures::Detect().lsx ? 16 : 0),
                  "helper really clobbers every caller-saved vector and receives typed input " + detail);
        }
        if (sample % 12 == 0) {
            StateStorage abi;
            Memory abi_memory{};
            Initialize(abi, abi_memory, sample);
            Check(CheckABI(compiled.fn, abi.state) == 1, "vector CFG preserves complete LP64D ABI " + detail);
            Equal(abi, expected, abi_memory, expected_memory, "ABI-wrapped complete vector CFG result " + detail);
        }
    }
}

void CheckPressurePlan(Fixture& fixture, const la::HostFeatures& features) {
    // Inspect the public plan directly. Legalize the complete V256 fixture,
    // including its control edges, before asking for raw128 assignments.
    la::WidePlan lowered;
    auto* block = fixture.block.get();
    auto targets = fixture.targets;
    auto stops = fixture.stops;
    if (la::WidePlan::Needed(block)) {
        lowered = la::WidePlan::Build(block);
        block = lowered.body.get();
        targets.clear(); stops.clear();
        for (const auto& [branch, target] : fixture.targets)
            targets.emplace(lowered.instructions.at(branch), lowered.instructions.at(target));
        for (auto* stop : fixture.stops) stops.insert(lowered.instructions.at(stop));
    }
    auto plan = la::VectorAllocationPlan::Build(block, features, lowered.native_pairs, targets, stops);
    auto baseline = la::VectorAllocationPlan::Build(block, features, lowered.native_pairs, targets, stops, {}, false);
    const auto& stats = plan.AllocationStats();
    u32 values{}, assigned{}, spilled{};
    const auto live_registers = [&](const la::LiveSet& live) {
        std::array<ir::Inst*, la::VectorAllocationPlan::kRegisterCount> occupants{};
        for (auto* value : live) if (auto index = plan.Register(value)) {
            Check(*index < occupants.size(), "live vector cache index fits allocatable bank " + fixture.name);
            Check(!occupants[*index] || occupants[*index] == value,
                  "simultaneously live raw128 values have different registers " + fixture.name);
            occupants[*index] = value;
        }
    };
    for (auto& inst : block->GetInstList()) {
        Check(!baseline.Register(&inst), "disabled CFG plan has no fixed assignments " + fixture.name);
        const auto index = plan.Register(&inst);
        if (la::VectorAllocationPlan::Eligible(&inst)) {
            ++values;
            if (index) ++assigned; else ++spilled;
        }
        if (index) {
            const auto physical = *index + la::VectorAllocationPlan::kFirstRegister;
            Check(*index < la::VectorAllocationPlan::kRegisterCount && physical >= 8 && physical <= 23 &&
                  inst.ReturnType() >= T::V8 && inst.ReturnType() <= T::V128,
                  "fixed mapping uses raw128 views of v8..v23 only " + fixture.name);
        }
        live_registers(plan.At(&inst)); live_registers(plan.After(&inst));
    }
    Check(plan.CFGNodes() > 1 && stats.max_pressure >= (fixture.type == T::V256 ? 64u : 32u) &&
          stats.interference_edges > 0 && assigned > 0 && spilled > 0,
          "public vector plan reports a real pressured CFG " + fixture.name);
    Check(values == stats.values && assigned == stats.register_values && spilled == stats.spilled_values &&
          values == assigned + spilled,
          "public vector assignments and spill counts agree with every SSA value " + fixture.name);
}

void RunModes(Fixture& fixture) {
    const auto host = la::HostFeatures::Detect();
    const auto ids = SourceIDs(fixture.block.get());
    for (bool lasx : {false, true}) {
        auto features = host;
        features.lasx = lasx && host.lasx;
        if (fixture.pressure && host.lsx) CheckPressurePlan(fixture, features);
        for (auto scalar : {M::GlobalCFG, M::LocalCache}) {
        CFGCompiled local{fixture, features, false, scalar}, global{fixture, features, true, scalar};
        Compare(fixture, local, false);
        Compare(fixture, global, true);
        const auto& off = local.translator.VectorStats();
        const auto& on = global.translator.VectorStats();
        if (fixture.dense) {
            constexpr std::array<u32, 3> offsets{2032, 2048, 4112};
            for (auto* compiled : {&local, &global}) {
                Check(!compiled->context.SparseHomesActive() &&
                      compiled->translator.EmittedIR() == fixture.block.get(),
                      "home displacement boundary fixture keeps the executed dense source layout");
                for (u32 i = 0; i < offsets.size(); ++i)
                    Check(compiled->context.HomeOffset(fixture.boundary_homes[i]) == offsets[i],
                          "executed LSX home has the precise displacement " + std::to_string(offsets[i]));
            }
            if (host.lsx)
                Check(on.boundary_store_bytes >= 48 && on.boundary_load_bytes >= 48,
                      "all three displacement-boundary vectors are actually published and restored across clobber");
        }
        Check(!off.global_cfg, "explicit disabled CFG allocation retains region baseline " + fixture.name);
        Check(on.global_cfg == host.lsx, "explicit enabled CFG allocation follows LSX support " + fixture.name);
        if (host.lsx) {
            Check(on.cfg_nodes > 1 && on.assigned_values > 0 && on.register_reads > 0,
                  "fixture exercises actual whole-CFG vector assignments " + fixture.name);
            if (fixture.pressure)
                Check(on.spilled_values > 0, "32 live vector values exercise global interference spills " + fixture.name);
            if (fixture.boundary)
                Check(on.boundary_load_bytes > 0 && on.boundary_store_bytes > 0,
                      "observable helper/FP/memory boundaries publish and restore live mapped vectors " + fixture.name);
            if (fixture.publication) {
                // Only the explicitly dirty native producer needs a store;
                // clean sources still need complete reloads after clobbers.
                std::cout << "VECTOR-PUBLICATION-COST fixture=" << fixture.name
                          << " width=" << Width(fixture.type) * 8 << " lasx=" << features.lasx
                          << " scalar=" << u32(scalar) << " stores=" << on.stores
                          << " boundary_store=" << on.boundary_store_bytes
                          << " boundary_load=" << on.boundary_load_bytes
                          << " direct_reads=" << on.direct_reads << " direct_writes=" << on.direct_writes
                          << " skipped=" << on.skipped_publications
                          << " skipped_bytes=" << on.skipped_publication_bytes
                          << " publication_iterations=" << on.publication_iterations
                          << '\n' << std::flush;
                Check(on.boundary_store_bytes == fixture.publication_sites * Width(fixture.type) &&
                      on.stores == fixture.publication_sites * (Width(fixture.type) / 16) &&
                      on.boundary_load_bytes >= fixture.publication_live_reloads * Width(fixture.type) &&
                      on.skipped_publications > 0 && on.skipped_publication_bytes == u64(on.skipped_publications) * 16,
                      "each possibly dirty dynamic definition publishes and clean clobbers reload without redundant stores " + fixture.name);
            }
            if (fixture.improves) {
                const auto on_bytes = on.load_bytes + on.store_bytes;
                const auto off_bytes = off.load_bytes + off.store_bytes;
                std::cout << "VECTOR-CFG-COST fixture=" << fixture.name << " width=" << Width(fixture.type) * 8
                          << " lasx=" << features.lasx
                          << " scalar=" << u32(scalar)
                          << " code_off=" << local.context.CurrentBufferSize()
                          << " code_on=" << global.context.CurrentBufferSize()
                          << " home_off=" << off_bytes << " home_on=" << on_bytes
                          << " loads_off=" << off.load_bytes << " loads_on=" << on.load_bytes
                          << " stores_off=" << off.store_bytes << " stores_on=" << on.store_bytes
                          << " boundary_load_off=" << off.boundary_load_bytes
                          << " boundary_load_on=" << on.boundary_load_bytes
                          << " boundary_store_off=" << off.boundary_store_bytes
                          << " boundary_store_on=" << on.boundary_store_bytes
                          << " moves_off=" << off.moves << " moves_on=" << on.moves
                          << " permutations_off=" << off.permutations << " permutations_on=" << on.permutations
                          << " direct_reads=" << on.direct_reads << " direct_writes=" << on.direct_writes
                          << " skipped=" << on.skipped_publications
                          << " skipped_bytes=" << on.skipped_publication_bytes
                          << " assigned=" << on.assigned_values << " spilled=" << on.spilled_values
                          << " preserved=" << on.edge_preserved << " register_reads=" << on.register_reads << '\n' << std::flush;
                Check(on.edge_preserved > 0 && on_bytes < off_bytes &&
                      on.boundary_store_bytes < off.boundary_store_bytes,
                      "pure CFG retains mapped values and reduces total home bytes and boundary stores " + fixture.name +
                      " width=" + std::to_string(Width(fixture.type) * 8) + " lasx=" + std::to_string(features.lasx) +
                      " scalar=" + std::to_string(u32(scalar)) + " off=" + std::to_string(off_bytes) +
                      " on=" + std::to_string(on_bytes));
                if (fixture.type == T::V128 || !features.lasx)
                    Check(on.moves < off.moves,
                          "fixed LSX operands reduce actual emitted vector copies " + fixture.name);
            }
        }
        if (!features.lasx)
            Check(off.lasx_fusions == 0 && on.lasx_fusions == 0, "LASX disabled executes both raw128 halves " + fixture.name);
        else if (fixture.type == T::V256)
            Check(off.lasx_fusions > 0 && on.lasx_fusions > 0, "wide CFG uses provenanced LASX operations " + fixture.name);
        Check(SourceIDs(fixture.block.get()) == ids, "CFG compilation does not mutate public source IDs " + fixture.name);
        }
    }
}

Fixture Edges(T type) {
    Fixture fixture{"taken-fallthrough-joins", type, 0x9c00};
    ir::Assembler as{fixture.block.get()};
    auto a = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
    auto b = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
    auto condition = as.LoadUniform(ir::Uniform{kCondition, T::U64}).SetType(T::U64);
    auto value = as.VecAdd(a, b, ir::Imm{u64{32}}).SetType(type);
    for (u32 stage = 0; stage < 12; ++stage) {
        auto before = as.VecXor(value, a).SetType(type);
        auto test = as.TestNotZero(condition);
        auto branch = stage & 1 ? as.NotGoto(test) : as.Goto(test);
        auto optional = as.VecSub(before, b, ir::Imm{u64{16}}).SetType(type);
        as.StoreUniform(ir::Uniform{kSide + stage * 32, type}, optional);
        as.BindLabel(branch);
        value = as.VecAdd(before, b, ir::Imm{u64{32}}).SetType(type);
    }
    as.StoreUniform(ir::Uniform{kOutput, type}, value);
    fixture.model = [width = Width(type)](StateStorage& state, Memory&) {
        const auto a = Read(state, 0, width), b = Read(state, 32, width);
        const bool condition = state.Get(kCondition) != 0;
        auto value = Binary(a, b, width, 32, O::VecAdd);
        for (u32 stage = 0; stage < 12; ++stage) {
            auto before = Binary(value, a, width, 64, O::VecXor);
            const bool taken = stage & 1 ? !condition : condition;
            if (!taken) Write(state, kSide + stage * 32, Binary(before, b, width, 16, O::VecSub), width);
            value = Binary(before, b, width, 32, O::VecAdd);
        }
        Write(state, kOutput, value, width);
        return Expected{};
    };
    fixture.improves = true;
    fixture.Finish();
    return fixture;
}

Fixture Loop(T type) {
    Fixture fixture{"backedge-dominator", type, 0x9d00};
    ir::Assembler as{fixture.block.get()};
    auto a = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
    auto b = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
    auto anchor = as.VecXor(a, b).SetType(type);
    auto count = as.LoadUniform(ir::Uniform{kCount, T::U64}).SetType(T::U64);
    auto current = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
    auto sum = as.VecAdd(current, anchor, ir::Imm{u64{16}}).SetType(type);
    as.StoreUniform(ir::Uniform{0, type}, sum);
    auto next = as.Sub(count, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{kCount, T::U64}, next);
    auto back = as.Goto(as.TestNotZero(next));
    as.BindLabel(back);
    auto* label = &fixture.block->GetInstList().back();
    fixture.block->RemoveInst(label);
    fixture.block->InsertBefore(label, count.Def());
    as.StoreUniform(ir::Uniform{kOutput, type}, as.VecXor(sum, anchor).SetType(type));
    as.StoreUniform(ir::Uniform{kOutput + 32, type}, anchor);
    fixture.model = [width = Width(type)](StateStorage& state, Memory&) {
        auto value = Read(state, 0, width);
        const auto anchor = Binary(value, Read(state, 32, width), width, 64, O::VecXor);
        const auto count = state.Get(kCount);
        for (u64 iteration = 0; iteration < count; ++iteration)
            value = Binary(value, anchor, width, 16, O::VecAdd);
        Write(state, 0, value, width);
        state.Put(kCount, 0);
        Write(state, kOutput, Binary(value, anchor, width, 64, O::VecXor), width);
        Write(state, kOutput + 32, anchor, width);
        return Expected{};
    };
    fixture.improves = true;
    fixture.Finish();
    return fixture;
}

Fixture DiamondPhi(T type) {
    Fixture fixture{"diamond-vector-phi", type, 0x9e00};
    ir::Assembler as{fixture.block.get()};
    auto a = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
    auto b = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
    auto condition = as.LoadUniform(ir::Uniform{kCondition, T::U64}).SetType(T::U64);
    auto base = as.VecAdd(a, b, ir::Imm{u64{32}}).SetType(type);
    auto branch = as.Goto(as.TestNotZero(condition));
    auto left = as.VecSub(base, b, ir::Imm{u64{16}}).SetType(type);
    auto join = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)});
    fixture.stops.insert(join.Def());
    as.BindLabel(branch);
    auto right = as.VecXor(base, a).SetType(type);
    as.BindLabel(join);
    ir::Params inputs;
    inputs.Push(left); inputs.Push(right);
    auto phi = as.AddPhi(inputs).SetType(type);
    auto result = as.VecAdd(phi, a, ir::Imm{u64{64}}).SetType(type);
    as.StoreUniform(ir::Uniform{kOutput, type}, result);
    as.StoreUniform(ir::Uniform{kOutput + 32, type}, phi);
    fixture.model = [width = Width(type)](StateStorage& state, Memory&) {
        const auto a = Read(state, 0, width), b = Read(state, 32, width);
        const auto base = Binary(a, b, width, 32, O::VecAdd);
        const auto phi = state.Get(kCondition) ? Binary(base, a, width, 64, O::VecXor) :
                                               Binary(base, b, width, 16, O::VecSub);
        Write(state, kOutput, Binary(phi, a, width, 64, O::VecAdd), width);
        Write(state, kOutput + 32, phi, width);
        return Expected{};
    };
    fixture.interpreter = false; // RunAddPhi has no interpreter semantics.
    fixture.Finish();
    return fixture;
}

Fixture CyclicPhi(T type) {
    Fixture fixture{"backedge-parallel-vector-phi", type, 0x9f00};
    ir::Assembler as{fixture.block.get()};
    auto a = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
    auto b = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
    auto initial = as.LoadUniform(ir::Uniform{kCount, T::U64}).SetType(T::U64);
    auto* label = new ir::Inst{O::BindLabel};
    label->SetArgs(V{label}); label->SetReturn(T::VOID);
    fixture.block->AppendInstUnchecked(label);
    auto* x = new ir::Inst{O::AddPhi};
    auto* y = new ir::Inst{O::AddPhi};
    auto* n = new ir::Inst{O::AddPhi};
    for (auto* phi : {x, y, n}) {
        phi->SetReturn(phi == n ? T::U64 : type);
        fixture.block->AppendInstUnchecked(phi);
    }
    auto difference = as.VecSub(V{x}, V{y}, ir::Imm{u64{32}}).SetType(type);
    auto next = as.Sub(V{n}, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
    auto back = as.Goto(as.TestNotZero(next));
    fixture.targets.emplace(back.Def(), label);
    as.StoreUniform(ir::Uniform{kOutput, type}, V{x});
    as.StoreUniform(ir::Uniform{kOutput + 32, type}, V{y});
    as.StoreUniform(ir::Uniform{kOutput + 64, type}, difference);
    as.StoreUniform(ir::Uniform{kOutput + 96, T::U64}, next);
    ir::Params xp; xp.Push(a); xp.Push(V{y}); x->SetArgs(xp);
    ir::Params yp; yp.Push(b); yp.Push(V{x}); y->SetArgs(yp);
    ir::Params np; np.Push(initial); np.Push(next); n->SetArgs(np);
    fixture.model = [width = Width(type)](StateStorage& state, Memory&) {
        auto x = Read(state, 0, width), y = Read(state, 32, width);
        for (u64 iteration = 1; iteration < state.Get(kCount); ++iteration) std::swap(x, y);
        Write(state, kOutput, x, width);
        Write(state, kOutput + 32, y, width);
        Write(state, kOutput + 64, Binary(x, y, width, 32, O::VecSub), width);
        state.Put(kOutput + 96, 0);
        return Expected{};
    };
    fixture.interpreter = false;
    fixture.Finish();
    return fixture;
}

Fixture Pressure(T type) {
    Fixture fixture{"32-live-across-edge-and-clobber", type, 0xa000};
    ir::Assembler as{fixture.block.get()};
    auto a = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
    auto b = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
    auto condition = as.LoadUniform(ir::Uniform{kCondition, T::U64}).SetType(T::U64);
    auto scalar = as.LoadUniform(ir::Uniform{kScalar, T::U64}).SetType(T::U64);
    std::array<V, 32> live;
    auto chain = a;
    for (auto& value : live) {
        chain = as.VecAdd(chain, b, ir::Imm{u64{32}}).SetType(type);
        value = as.VecXor(chain, a).SetType(type);
    }
    auto edge = as.Goto(as.TestNotZero(condition));
    as.StoreUniform(ir::Uniform{kSide, type}, as.VecSub(live[2], live[15], ir::Imm{u64{16}}).SetType(type));
    as.BindLabel(edge);
    auto called = as.CallHost(ClobberCallerVectors, scalar).SetType(T::U64);
    auto result = a;
    for (u32 i = 0; i < live.size(); ++i)
        result = as.VecXor(result, live[(i * 13) % live.size()]).SetType(type);
    as.StoreUniform(ir::Uniform{kOutput, type}, result);
    as.StoreUniform(ir::Uniform{kOutput + 32, type}, live.back());
    as.StoreUniform(ir::Uniform{kOutput + 64, T::U64}, called);
    fixture.model = [width = Width(type)](StateStorage& state, Memory&) {
        const auto a = Read(state, 0, width), b = Read(state, 32, width);
        std::array<Bytes, 32> live;
        auto chain = a;
        for (auto& value : live) {
            chain = Binary(chain, b, width, 32, O::VecAdd);
            value = Binary(chain, a, width, 64, O::VecXor);
        }
        if (!state.Get(kCondition)) Write(state, kSide, Binary(live[2], live[15], width, 16, O::VecSub), width);
        auto result = a;
        for (u32 i = 0; i < live.size(); ++i)
            result = Binary(result, live[(i * 13) % live.size()], width, 64, O::VecXor);
        Write(state, kOutput, result, width);
        Write(state, kOutput + 32, live.back(), width);
        const auto scalar = state.Get(kScalar);
        state.Put(kOutput + 64, std::rotl(scalar, 17) + 29);
        return Expected{true, scalar};
    };
    fixture.pressure = fixture.boundary = true;
    fixture.Finish();
    return fixture;
}

Fixture Boundaries(T type) {
    Fixture fixture{"FP-memory-raw-and-narrow-boundaries", type, 0xa100};
    ir::Assembler as{fixture.block.get()};
    auto a = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
    auto b = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
    auto condition = as.LoadUniform(ir::Uniform{kCondition, T::U64}).SetType(T::U64);
    auto address = as.LoadUniform(ir::Uniform{kAddress, T::U64}).SetType(T::U64);
    auto value = as.VecAdd(a, b, ir::Imm{u64{16}}).SetType(type);
    auto edge = as.Goto(as.TestNotZero(condition));
    as.StoreUniform(ir::Uniform{kSide, type}, as.VecXor(value, a).SetType(type));
    as.BindLabel(edge);
    auto raw = as.BitCast(value).SetType(T::U8);
    auto called = as.CallHost(ClobberCallerVectors, raw).SetType(T::U64);
    auto after_call = as.VecXor(value, b).SetType(type);
    as.StoreMemory(ir::Operand{address}, after_call);
    auto loaded = as.LoadMemory(ir::Operand{address}).SetType(type);
    auto fa = as.LoadUniform(ir::Uniform{kFPLeft, type}).SetType(type);
    auto fb = as.LoadUniform(ir::Uniform{kFPRight, type}).SetType(type);
    auto fp = as.VecFAddScalar32(fa, fb).SetType(type);
    auto after_fp = as.VecOr(fp, value).SetType(type);
    auto output = as.VecAdd(after_fp, loaded, ir::Imm{u64{32}}).SetType(type);
    as.StoreUniform(ir::Uniform{kOutput, type}, output);
    as.StoreUniform(ir::Uniform{kOutput + 32, type}, loaded);
    as.StoreUniform(ir::Uniform{kOutput + 64, type}, value);
    as.StoreUniform(ir::Uniform{kOutput + 96, type}, fp);
    as.StoreUniform(ir::Uniform{kOutput + 128, T::V128}, as.BitCast(raw).SetType(T::V128));
    as.StoreUniform(ir::Uniform{kOutput + 160, T::U64}, called);
    auto narrow = as.Add(raw, ir::Operand{ir::Imm{u64{7}}}).SetType(T::U8);
    as.StoreUniform(ir::Uniform{kOutput + 176, T::U8}, narrow);
    fixture.model = [width = Width(type)](StateStorage& state, Memory& memory) {
        const auto a = Read(state, 0, width), b = Read(state, 32, width);
        const auto value = Binary(a, b, width, 16, O::VecAdd);
        if (!state.Get(kCondition)) Write(state, kSide, Binary(value, a, width, 64, O::VecXor), width);
        const auto after_call = Binary(value, b, width, 64, O::VecXor);
        std::memcpy(memory.data() + state.Get(kAddress), after_call.data(), width);
        auto fp = Read(state, kFPLeft, width);
        const auto fb = Read(state, kFPRight, width);
        float left{}, right{};
        std::memcpy(&left, fp.data(), 4); std::memcpy(&right, fb.data(), 4);
        const float sum = left + right;
        std::memcpy(fp.data(), &sum, 4); // Generic scalar FP merges all upper input bits.
        const auto output = Binary(Binary(fp, value, width, 64, O::VecOr), after_call, width, 32, O::VecAdd);
        Write(state, kOutput, output, width);
        Write(state, kOutput + 32, after_call, width);
        Write(state, kOutput + 64, value, width);
        Write(state, kOutput + 96, fp, width);
        Write(state, kOutput + 128, value, 16);
        state.Put(kOutput + 160, std::rotl(u64(value[0]), 17) + 29);
        state.state->uniform_buffer_begin[kOutput + 176] = u8(value[0] + 7);
        return Expected{true, value[0]};
    };
    fixture.boundary = true;
    fixture.Finish();
    return fixture;
}

Fixture VariableCounts(T type) {
    Fixture fixture{"raw-V64-variable-shift-counts", type, 0xa200};
    ir::Assembler as{fixture.block.get()};
    auto a = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
    auto b = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
    auto condition = as.LoadUniform(ir::Uniform{kCondition, T::U64}).SetType(T::U64);
    auto encoded = as.LoadUniform(ir::Uniform{kShiftCount, T::V64}).SetType(T::V64);
    auto mask = as.LoadUniform(ir::Uniform{kShiftMask, T::V64}).SetType(T::V64);
    // This native producer changes the raw vector value, so a subsequent GPR
    // count read cannot succeed using the original LoadUniform canonical home.
    auto count = as.VecXor(encoded, mask).SetType(T::V64);
    auto value = as.VecXor(a, b).SetType(type);
    u32 stage{}, output{};
    for (u32 bits : {8u, 16u, 32u, 64u}) {
        auto edge = stage & 1 ? as.NotGoto(as.TestNotZero(condition)) : as.Goto(as.TestNotZero(condition));
        as.StoreUniform(ir::Uniform{kSide + stage * 32, type},
                        as.VecSub(value, b, ir::Imm{u64{bits}}).SetType(type));
        as.BindLabel(edge);
        for (auto op : {O::VecShiftLeft, O::VecShiftRight, O::VecShiftRightArithmetic}) {
            auto shifted = V{fixture.block->AppendInst(op, value, count, ir::Imm{u64{bits}})}.SetType(type);
            as.StoreUniform(ir::Uniform{kOutput + output++ * 32, type}, shifted);
        }
        ++stage;
    }
    as.StoreUniform(ir::Uniform{kOutput + output * 32, T::V64}, count);
    fixture.model = [width = Width(type)](StateStorage& state, Memory&) {
        const auto a = Read(state, 0, width), b = Read(state, 32, width);
        const auto value = Binary(a, b, width, 64, O::VecXor);
        const auto count = state.Get(kShiftCount) ^ state.Get(kShiftMask);
        u32 stage{}, output{};
        for (u32 bits : {8u, 16u, 32u, 64u}) {
            const bool taken = stage & 1 ? !state.Get(kCondition) : state.Get(kCondition) != 0;
            if (!taken) Write(state, kSide + stage * 32, Binary(value, b, width, bits, O::VecSub), width);
            for (auto op : {O::VecShiftLeft, O::VecShiftRight, O::VecShiftRightArithmetic})
                Write(state, kOutput + output++ * 32, Shift(value, count, width, bits, op), width);
            ++stage;
        }
        state.Put(kOutput + output * 32, count);
        return Expected{};
    };
    fixture.Finish();
    return fixture;
}

Fixture FixedSources(T type) {
    Fixture fixture{"direct-mapped-sources-preserved", type, 0xa300};
    ir::Assembler as{fixture.block.get()};
    auto a = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
    auto b = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
    auto condition = as.LoadUniform(ir::Uniform{kCondition, T::U64}).SetType(T::U64);
    auto left = as.VecAdd(a, b, ir::Imm{u64{32}}).SetType(type);
    auto right = as.VecXor(a, b).SetType(type);
    auto edge = as.Goto(as.TestNotZero(condition));
    as.StoreUniform(ir::Uniform{512, type}, as.VecAdd(left, left, ir::Imm{u64{64}}).SetType(type));
    as.BindLabel(edge);
    u32 output{};
    const auto emit = [&](V value) { as.StoreUniform(ir::Uniform{1280 + output++ * 32, type}, value); };
    for (u32 count : {1u, 7u, 8u, 15u, 16u, 17u, 23u, 31u}) {
        if (count >= Width(type)) continue;
        emit(as.VecExtractBytes(left, right, ir::Imm{u64(count)}).SetType(type));
        emit(as.VecExtractBytes(right, left, ir::Imm{u64(count)}).SetType(type));
        emit(as.VecExtractBytes(right, right, ir::Imm{u64(count)}).SetType(type));
        // Extract must not destroy either mapped input. In particular its
        // right shift temporary cannot alias the right SSA register.
        emit(as.VecXor(right, left).SetType(type));
    }
    for (u32 control : {0x1bu, 0x00u, 0x4eu, 0xffu})
        emit(as.VecShuffle32(right, ir::Imm{u64(control)}).SetType(type));
    for (bool odd : {false, true})
        emit(as.VecDupPairs32(left, ir::Imm{u64(odd)}).SetType(type));
    for (u32 bits : {8u, 16u, 32u, 64u})
        for (bool high : {false, true})
            emit(V{fixture.block->AppendInst(O::VecZip, left, right, ir::Imm{u64(bits)}, ir::Imm{u64(high)})}.SetType(type));
    emit(V{fixture.block->AppendInst(O::VecZip, right, right, ir::Imm{u64{64}}, ir::Imm{u64{1}})}.SetType(type));
    emit(as.VecXor(left, left).SetType(type));
    emit(as.VecAdd(right, right, ir::Imm{u64{64}}).SetType(type));
    emit(as.VecAdd(left, right, ir::Imm{u64{16}}).SetType(type));
    emit(as.VecAdd(left, right, ir::Imm{u64{16}}).SetType(type));
    for (auto value : {a, b, left, right}) emit(value);
    fixture.model = [width = Width(type)](StateStorage& state, Memory&) {
        const auto a = Read(state, 0, width), b = Read(state, 32, width);
        const auto left = Binary(a, b, width, 32, O::VecAdd), right = Binary(a, b, width, 64, O::VecXor);
        if (!state.Get(kCondition)) Write(state, 512, Binary(left, left, width, 64, O::VecAdd), width);
        u32 output{};
        const auto emit = [&](const Bytes& value) { Write(state, 1280 + output++ * 32, value, width); };
        for (u32 count : {1u, 7u, 8u, 15u, 16u, 17u, 23u, 31u}) {
            if (count >= width) continue;
            emit(Extract(left, right, width, count));
            emit(Extract(right, left, width, count));
            emit(Extract(right, right, width, count));
            emit(Binary(right, left, width, 64, O::VecXor));
        }
        for (u32 control : {0x1bu, 0x00u, 0x4eu, 0xffu}) emit(Shuffle32(right, width, control));
        for (bool odd : {false, true}) {
            Bytes duplicated{};
            for (u32 group = 0; group < width; group += 16)
                for (u32 lane = 0; lane < 4; ++lane)
                    std::memcpy(duplicated.data() + group + lane * 4,
                                left.data() + group + ((lane & ~1u) + u32(odd)) * 4, 4);
            emit(duplicated);
        }
        for (u32 bits : {8u, 16u, 32u, 64u})
            for (bool high : {false, true}) emit(Zip(left, right, width, bits, high));
        emit(Zip(right, right, width, 64, true));
        emit(Binary(left, left, width, 64, O::VecXor));
        emit(Binary(right, right, width, 64, O::VecAdd));
        emit(Binary(left, right, width, 16, O::VecAdd));
        emit(Binary(left, right, width, 16, O::VecAdd));
        for (const auto& value : {a, b, left, right}) emit(value);
        return Expected{};
    };
    fixture.Finish();
    return fixture;
}

Fixture ImmediateCounts(T type) {
    Fixture fixture{"direct-mapped-immediate-shift-counts", type, 0xa400};
    ir::Assembler as{fixture.block.get()};
    auto a = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
    auto b = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
    auto condition = as.LoadUniform(ir::Uniform{kCondition, T::U64}).SetType(T::U64);
    auto value = as.VecAdd(a, b, ir::Imm{u64{32}}).SetType(type);
    u32 stage{}, output{};
    for (u32 bits : {8u, 16u, 32u, 64u}) {
        auto edge = stage & 1 ? as.NotGoto(as.TestNotZero(condition)) : as.Goto(as.TestNotZero(condition));
        as.StoreUniform(ir::Uniform{256 + stage * 32, type}, as.VecXor(value, a).SetType(type));
        as.BindLabel(edge);
        for (u64 count : {u64{0}, u64{1}, u64(bits - 1), u64(bits), u64(bits + 1), u64{63}, u64{64}, UINT64_MAX})
            for (auto op : {O::VecShiftLeftImm, O::VecShiftRightImm, O::VecShiftRightArithmeticImm}) {
                auto shifted = V{fixture.block->AppendInst(op, value, ir::Imm{count}, ir::Imm{u64(bits)})}.SetType(type);
                as.StoreUniform(ir::Uniform{512 + output++ * 32, type}, shifted);
            }
        ++stage;
    }
    for (auto preserved : {value, a, b}) as.StoreUniform(ir::Uniform{512 + output++ * 32, type}, preserved);
    fixture.model = [width = Width(type)](StateStorage& state, Memory&) {
        const auto a = Read(state, 0, width), b = Read(state, 32, width);
        const auto value = Binary(a, b, width, 32, O::VecAdd);
        u32 stage{}, output{};
        for (u32 bits : {8u, 16u, 32u, 64u}) {
            const bool taken = stage & 1 ? !state.Get(kCondition) : state.Get(kCondition) != 0;
            if (!taken) Write(state, 256 + stage * 32, Binary(value, a, width, 64, O::VecXor), width);
            for (u64 count : {u64{0}, u64{1}, u64(bits - 1), u64(bits), u64(bits + 1), u64{63}, u64{64}, UINT64_MAX})
                for (auto op : {O::VecShiftLeft, O::VecShiftRight, O::VecShiftRightArithmetic})
                    Write(state, 512 + output++ * 32, Shift(value, count, width, bits, op), width);
            ++stage;
        }
        for (const auto& preserved : {value, a, b}) Write(state, 512 + output++ * 32, preserved, width);
        return Expected{};
    };
    fixture.Finish();
    return fixture;
}

Fixture RepeatedPublication(T type) {
    Fixture fixture{"diamond-may-dirty-and-two-clobbers", type, 0xa500};
    ir::Assembler as{fixture.block.get()};
    auto a = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
    auto b = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
    auto condition = as.LoadUniform(ir::Uniform{kCondition, T::U64}).SetType(T::U64);
    auto scalar = as.LoadUniform(ir::Uniform{kScalar, T::U64}).SetType(T::U64);
    auto value = as.VecAdd(a, b, ir::Imm{u64{32}}).SetType(type);
    auto edge = as.Goto(as.TestNotZero(condition));
    auto optional = as.CallHost(ClobberCallerVectors, scalar).SetType(T::U64);
    as.StoreUniform(ir::Uniform{kSide, T::U64}, optional);
    as.BindLabel(edge);
    // Fallthrough's helper has made value clean; the taken path has not
    // published it. A lexical clean flag here would lose the taken value.
    auto first = as.CallHost(ClobberCallerVectors, scalar).SetType(T::U64);
    auto second = as.CallHost(ClobberCallerVectors, first).SetType(T::U64);
    as.StoreUniform(ir::Uniform{kOutput, type}, as.VecXor(value, b).SetType(type));
    as.StoreUniform(ir::Uniform{kOutput + 32, type}, value);
    as.StoreUniform(ir::Uniform{kOutput + 64, T::U64}, first);
    as.StoreUniform(ir::Uniform{kOutput + 72, T::U64}, second);
    fixture.model = [width = Width(type)](StateStorage& state, Memory&) {
        const auto a = Read(state, 0, width), b = Read(state, 32, width);
        const auto value = Binary(a, b, width, 32, O::VecAdd);
        const auto first = std::rotl(state.Get(kScalar), 17) + 29;
        const bool optional = !state.Get(kCondition);
        if (optional) state.Put(kSide, first);
        Write(state, kOutput, Binary(value, b, width, 64, O::VecXor), width);
        Write(state, kOutput + 32, value, width);
        state.Put(kOutput + 64, first);
        state.Put(kOutput + 72, std::rotl(first, 17) + 29);
        return Expected{true, first, u32(optional ? 3 : 2)};
    };
    fixture.boundary = fixture.publication = true;
    fixture.Finish();
    return fixture;
}

Fixture LoopPublication(T type) {
    Fixture fixture{"loop-redefines-native-before-clobber", type, 0xa600};
    ir::Assembler as{fixture.block.get()};
    auto b = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
    auto count = as.LoadUniform(ir::Uniform{kCount, T::U64}).SetType(T::U64);
    auto current = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
    // This single static SSA instruction runs repeatedly. Its canonical home
    // becomes dirty again before each helper, despite the prior publication.
    auto sum = as.VecAdd(current, b, ir::Imm{u64{16}}).SetType(type);
    auto called = as.CallHost(ClobberCallerVectors, count).SetType(T::U64);
    as.StoreUniform(ir::Uniform{0, type}, sum);
    auto next = as.Sub(count, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{kCount, T::U64}, next);
    auto back = as.Goto(as.TestNotZero(next));
    as.BindLabel(back);
    auto* label = &fixture.block->GetInstList().back();
    fixture.block->RemoveInst(label);
    fixture.block->InsertBefore(label, count.Def());
    as.StoreUniform(ir::Uniform{kOutput, type}, sum);
    as.StoreUniform(ir::Uniform{kOutput + 32, type}, b);
    as.StoreUniform(ir::Uniform{kOutput + 64, T::U64}, called);
    fixture.model = [width = Width(type)](StateStorage& state, Memory&) {
        auto value = Read(state, 0, width);
        const auto b = Read(state, 32, width);
        const auto count = state.Get(kCount);
        for (u64 iteration = 0; iteration < count; ++iteration)
            value = Binary(value, b, width, 16, O::VecAdd);
        Write(state, 0, value, width);
        Write(state, kOutput, value, width);
        Write(state, kOutput + 32, b, width);
        state.Put(kCount, 0);
        state.Put(kOutput + 64, std::rotl(u64{1}, 17) + 29);
        return Expected{true, 1, u32(count)};
    };
    fixture.boundary = fixture.publication = true;
    fixture.publication_sites = 1;
    fixture.publication_live_reloads = 2;
    fixture.Finish();
    return fixture;
}

Fixture HomeDisplacements() {
    Fixture fixture{"LSX-home-displacements-2032-2048-4112", T::V128, 0xa700};
    fixture.dense = fixture.boundary = true;
    ir::Assembler as{fixture.block.get()};
    auto a = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
    auto b = as.LoadUniform(ir::Uniform{32, T::V128}).SetType(T::V128);
    auto condition = as.LoadUniform(ir::Uniform{kCondition, T::U64}).SetType(T::U64);
    auto scalar = as.LoadUniform(ir::Uniform{kScalar, T::U64}).SetType(T::U64);
    auto x = as.VecAdd(a, b, ir::Imm{u64{32}}).SetType(T::V128);
    auto y = as.VecXor(a, b).SetType(T::V128);
    auto z = as.VecSub(a, b, ir::Imm{u64{16}}).SetType(T::V128);
    auto edge = as.Goto(as.TestNotZero(condition));
    as.StoreUniform(ir::Uniform{kSide, T::V128}, as.VecXor(x, y).SetType(T::V128));
    as.BindLabel(edge);
    auto called = as.CallHost(ClobberCallerVectors, scalar).SetType(T::U64);
    // The helper forces real vst/vld execution for all three homes. Both
    // arithmetic mixing and intact outputs expose wrong low/high addressing,
    // accidental truncation at 2048, and overlap between neighboring homes.
    as.StoreUniform(ir::Uniform{kOutput, T::V128}, as.VecAdd(x, y, ir::Imm{u64{64}}).SetType(T::V128));
    as.StoreUniform(ir::Uniform{kOutput + 32, T::V128}, as.VecXor(z, x).SetType(T::V128));
    for (u32 i = 0; i < 3; ++i)
        as.StoreUniform(ir::Uniform{kOutput + 64 + i * 32, T::V128}, std::array<V, 3>{x, y, z}[i]);
    as.StoreUniform(ir::Uniform{kOutput + 160, T::U64}, called);
    fixture.model = [](StateStorage& state, Memory&) {
        const auto a = Read(state, 0, 16), b = Read(state, 32, 16);
        const auto x = Binary(a, b, 16, 32, O::VecAdd);
        const auto y = Binary(a, b, 16, 64, O::VecXor);
        const auto z = Binary(a, b, 16, 16, O::VecSub);
        if (!state.Get(kCondition)) Write(state, kSide, Binary(x, y, 16, 64, O::VecXor), 16);
        Write(state, kOutput, Binary(x, y, 16, 64, O::VecAdd), 16);
        Write(state, kOutput + 32, Binary(z, x, 16, 64, O::VecXor), 16);
        for (u32 i = 0; i < 3; ++i) Write(state, kOutput + 64 + i * 32, std::array<Bytes, 3>{x, y, z}[i], 16);
        const auto scalar = state.Get(kScalar);
        state.Put(kOutput + 160, std::rotl(scalar, 17) + 29);
        return Expected{true, scalar};
    };
    fixture.Finish();
    fixture.boundary_homes = {x.Def(), y.Def(), z.Def()};
    constexpr std::array<u16, 3> ids{127, 128, 257};
    for (u32 i = 0; i < ids.size(); ++i) fixture.boundary_homes[i]->SetId(ids[i]);
    return fixture;
}

void PolicyAndEvidence() {
    constexpr auto id = FeatureId::loongarch_vector_cfg_allocation;
    Check(FeatureName(id) == "loongarch_vector_cfg_allocation" &&
          FeatureIdFromName("loongarch_vector_cfg_allocation") == id,
          "vector CFG allocation has a stable module feature name");
    Check(FeatureSet{}.loongarch_vector_cfg_allocation && la::JitContext{}.VectorCFGAllocationEnabled(),
          "vector CFG allocation is enabled by default in both feature set and JIT context");
    auto on = GetSvmConfig().GetFeatureSet(), off = on;
    on.loongarch_vector_cfg_allocation = true; off.loongarch_vector_cfg_allocation = false;
    Check(HashFeatureSet(on) != HashFeatureSet(off), "vector CFG ON/OFF changes the cache fingerprint");
    auto fixture = Edges(T::V128);
    backend::AddressSpace space{TestConfig()};
    for (bool enabled : {false, true}) {
        backend::ModuleConfig policy;
        policy.feature_overrides.Set(id, enabled);
        policy.feature_overrides.Set(FeatureId::loongarch_vector_allocation, true);
        policy.feature_overrides.Set(FeatureId::loongarch_uniform_forwarding, false);
        auto module = std::make_shared<backend::Module>(space, 0, 1u << 20, policy);
        CFGCompiled compiled{fixture, la::HostFeatures::Detect(), !enabled, M::GlobalCFG, module.get()};
        Check(compiled.context.VectorCFGAllocationEnabled() == enabled,
              "module policy overrides an oppositely preconfigured vector CFG mode");
        Check(compiled.translator.VectorStats().global_cfg == (enabled && compiled.context.Features().lsx),
              "module vector CFG policy selects actual fixed mappings");
        Compare(fixture, compiled, enabled);
    }
}
} // namespace

void VectorCFGAllocationExecution() {
    for (auto type : {T::V128, T::V256}) {
        auto edges = Edges(type); RunModes(edges);
        auto loop = Loop(type); RunModes(loop);
        auto diamond = DiamondPhi(type); RunModes(diamond);
        auto cycle = CyclicPhi(type); RunModes(cycle);
        auto pressure = Pressure(type); RunModes(pressure);
        auto boundaries = Boundaries(type); RunModes(boundaries);
        auto counts = VariableCounts(type); RunModes(counts);
        auto sources = FixedSources(type); RunModes(sources);
        auto immediate = ImmediateCounts(type); RunModes(immediate);
        auto publication = RepeatedPublication(type); RunModes(publication);
        auto loop_publication = LoopPublication(type); RunModes(loop_publication);
    }
    auto displacements = HomeDisplacements(); RunModes(displacements);
    PolicyAndEvidence();
    std::cout << "PASS global/region vector CFG, taken/fallthrough joins, backedges, parallel phis, "
                 "32-live pressure, scalar GlobalCFG/LocalCache, caller clobbers, raw V64 shift counts, "
                 "direct mapped source preservation, alias inputs, shuffle/zip/immediate shifts, "
                 "may-dirty joins, repeated loop definitions and consecutive helper publication, "
                 "2032/2048/4112 LSX home displacements, module policy and complete raw/FP/memory results\n";
}
} // namespace swift::tests::loongarch64
