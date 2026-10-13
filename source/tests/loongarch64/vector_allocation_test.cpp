#include "test_support.h"

#include <algorithm>
#include <bit>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/ir_assembler.h"

namespace swift::tests::loongarch64 {
namespace {
using T = ir::ValueType;
using O = ir::OpCode;
using M = la::ScalarAllocationMode;

struct VectorCompiled {
    Config config{TestConfig()};
    la::JitContext context;
    la::JitTranslator translator{context};
    backend::CodeCache cache{config, 1u << 20, FeatureSet{}};
    BlockFn fn{};
    VectorCompiled(ir::Block* block, la::HostFeatures features, bool allocation,
                   M scalar = M::GlobalCFG) : context(true, features, {}, scalar) {
        context.SetVectorAllocation(allocation);
        translator.Translate(block);
        auto buffer = cache.AllocCode(context.CurrentBufferSize());
        Check(buffer.has_value(), "allocate vector residency fixture");
        context.Flush(*buffer);
        fn = reinterpret_cast<BlockFn>(buffer->exec_data);
    }
};

void Compare(VectorCompiled& compiled, ir::Block* block, u32 samples = 24, bool loop = false,
             bool memory = false) {
    // Portable interpretation owns raw128 homes. Compare fused/resident code
    // against a separately interpreted split clone; NativeWideOperations
    // also checks all 256 output bits with an independent lane oracle.
    IntrusivePtr<ir::Block> portable;
    if (la::WidePlan::Needed(block)) {
        auto lowered = la::WidePlan::Build(block);
        portable = lowered.body;
        // Native emission consumes a validated pointer+16 continuation
        // annotation. The portable interpreter has no such metadata, so
        // materialize its second address explicitly on this reference clone.
        for (auto* high : lowered.memory_continuations) {
            auto* base = new ir::Inst{O::GetOperand};
            base->SetArgs(high->GetArg<ir::Operand>(0)); base->SetReturn(T::U64);
            portable->InsertBefore(base, high);
            auto* next = new ir::Inst{O::Add};
            next->SetArgs(ir::Value{base}, ir::Operand{ir::Imm{u64{16}}}); next->SetReturn(T::U64);
            portable->InsertBefore(next, high);
            high->SetArg(0, ir::Operand{ir::Value{next}});
        }
        portable->ReIdInstr();
    }
    auto* interpreted = portable ? portable.get() : block;
    u64 seed = 0x4c09572aec3d8941ULL;
    for (u32 sample = 0; sample < samples; ++sample) {
        StateStorage actual, reference;
        alignas(32) std::array<u8, 256> actual_memory{}, reference_memory{};
        for (u32 offset = 0; offset < 1024; offset += 8) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            // Four distinct 64-bit quadrants expose swapped/dropped halves.
            const u64 data = sample == 0 ? u64(offset) * 0x0123456789abcdefULL :
                sample == 1 ? UINT64_MAX : sample == 2 ? 0x8000800080008000ULL :
                sample == 3 ? 0x7fff7fff7fff7fffULL : seed;
            actual.Put(offset, data); reference.Put(offset, data);
        }
        actual.Put(96, loop ? 1 + sample % 17 : sample & 1);
        reference.Put(96, loop ? 1 + sample % 17 : sample & 1);
        actual.Put(104, 17); reference.Put(104, 17);
        actual_memory.fill(0x9b); reference_memory = actual_memory;
        if (memory) for (auto* state : {actual.state, reference.state}) {
            state->pt = state == actual.state ? actual_memory.data() : reference_memory.data();
            state->guest_addr_limit = actual_memory.size(); state->guest_addr_mask = actual_memory.size() - 1;
        }
        actual.state->host_cpu_flags = reference.state->host_cpu_flags = std::rotl(seed, 23);
        backend::interp::Interpreter interpreter{*reference.state, interpreted};
        const auto expected_halt = interpreter.Run();
        const auto actual_halt = compiled.fn(actual.state);
        u32 first_difference = 1024;
        for (u32 offset = 0; offset < 1024; ++offset)
            if (actual.state->uniform_buffer_begin[offset] != reference.state->uniform_buffer_begin[offset]) {
                first_difference = offset; break;
            }
        Check(actual_halt == expected_halt &&
              std::memcmp(actual.state->uniform_buffer_begin, reference.state->uniform_buffer_begin, 1024) == 0 &&
              actual.state->host_cpu_flags == reference.state->host_cpu_flags &&
              actual_memory == reference_memory,
              "vector residency interpreter differential sample=" + std::to_string(sample) +
              " actual_halt=" + std::to_string(u32(actual_halt)) +
              " expected_halt=" + std::to_string(u32(expected_halt)) +
              " offset=" + std::to_string(first_difference) +
              " actual=" + std::to_string(actual.Get(first_difference / 8 * 8)) +
              " expected=" + std::to_string(reference.Get(first_difference / 8 * 8)) +
              " lsx=" + std::to_string(compiled.context.Features().lsx) +
              " lasx=" + std::to_string(compiled.context.Features().lasx) +
              " allocation=" + std::to_string(compiled.context.VectorAllocationEnabled()) +
              " scalar=" + std::to_string(u32(compiled.context.AllocationMode())) +
              " spills=" + std::to_string(compiled.translator.VectorStats().spills));
        if (!loop && sample % 12 == 0)
            Check(CheckABI(compiled.fn, actual.state) == 1, "vector residency preserves LP64D GPR/FPR/FCSR");
    }
}

u64 Saturate(u64 a, u64 b, u32 bits, bool sign, bool subtract) {
    const auto mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
    if (!sign) {
        if (subtract) return a < b ? 0 : a - b;
        const auto sum = static_cast<unsigned __int128>(a) + b;
        return sum > mask ? mask : u64(sum);
    }
    const __int128 limit = __int128{1} << (bits - 1);
    const auto decode = [=](u64 value) -> __int128 {
        return value & u64(limit) ? __int128(value) - 2 * limit : __int128(value);
    };
    return u64(std::clamp(subtract ? decode(a) - decode(b) : decode(a) + decode(b), -limit, limit - 1)) & mask;
}

void CheckWideOracle(VectorCompiled& compiled, O op, u32 bits, bool sign) {
    if (op == O::Vec4Add || op == O::Vec4Sub || op == O::Vec4Mul) bits = 32;
    const bool bitwise = op == O::Vec4And || op == O::Vec4Or || op == O::VecAnd ||
                         op == O::VecOr || op == O::VecXor || op == O::VecAndNot;
    if (bitwise) bits = 64;
    if (op == O::VecMulHigh16) bits = 16;
    u64 seed = 0x971ad0356d322ca9ULL;
    for (u32 sample = 0; sample < 24; ++sample) {
        StateStorage actual;
        std::array<u8, 32> a{}, b{}, expected{};
        const auto mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
        for (u32 lane = 0; lane < 256 / bits; ++lane) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            u64 left = (sample == 0 ? UINT64_MAX : sample == 1 ? 0 : sample == 2 ? u64{1} << (bits - 1) : seed) & mask;
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            u64 right = (sample == 0 ? 1 : sample == 1 ? UINT64_MAX : sample == 2 ? mask >> 1 : seed) & mask;
            const auto operate = [&](u64 x, u64 y) -> u64 {
                const __int128 limit = __int128{1} << (bits - 1);
                const auto decode = [=](u64 value) -> __int128 {
                    return value & u64(limit) ? __int128(value) - 2 * limit : __int128(value);
                };
                switch (op) {
                    case O::Vec4Add: case O::VecAdd: return (x + y) & mask;
                    case O::Vec4Sub: case O::VecSub: return (x - y) & mask;
                    case O::Vec4Mul: case O::VecMul: return (x * y) & mask;
                    case O::Vec4And: case O::VecAnd: return x & y;
                    case O::Vec4Or: case O::VecOr: return x | y;
                    case O::VecXor: return x ^ y;
                    case O::VecAndNot: return x & ~y;
                    case O::VecCmpEq: return x == y ? mask : 0;
                    case O::VecCmpGt: return decode(x) > decode(y) ? mask : 0;
                    case O::VecMin: return (sign ? decode(x) < decode(y) : x < y) ? x : y;
                    case O::VecMax: return (sign ? decode(x) > decode(y) : x > y) ? x : y;
                    case O::VecAvg: return u64((static_cast<unsigned __int128>(x) + y + 1) >> 1);
                    case O::VecSatAdd: return Saturate(x, y, bits, sign, false);
                    case O::VecSatSub: return Saturate(x, y, bits, sign, true);
                    case O::VecMulHigh16: return u64(sign ? (decode(x) * decode(y)) >> 16 : __int128(x * y) >> 16) & mask;
                    default: throw std::runtime_error("invalid independent wide lane oracle");
                }
            };
            const u64 result = operate(operate(left, right), left);
            std::memcpy(a.data() + lane * bits / 8, &left, bits / 8);
            std::memcpy(b.data() + lane * bits / 8, &right, bits / 8);
            std::memcpy(expected.data() + lane * bits / 8, &result, bits / 8);
        }
        std::memcpy(actual.state->uniform_buffer_begin, a.data(), a.size());
        std::memcpy(actual.state->uniform_buffer_begin + 32, b.data(), b.size());
        Check(compiled.fn(actual.state) == HaltReason::CallHost &&
              std::memcmp(actual.state->uniform_buffer_begin + 256, expected.data(), expected.size()) == 0,
              "all 256 output bits match independent lane arithmetic oracle op=" + std::string{ir::GetIRMetaInfo(op).name} +
              " bits=" + std::to_string(bits) + " signed=" + std::to_string(sign) + " sample=" + std::to_string(sample));
    }
}

void CompareModes(ir::Block* block, u32 samples = 24, bool loop = false, bool memory = false) {
    const auto host = la::HostFeatures::Detect();
    for (u32 mode = 0; mode < 3; ++mode) {
        auto features = host;
        if (mode) features.lasx = false;
        if (mode == 2) features.lsx = false;
        for (auto scalar : {M::GlobalCFG, M::CanonicalHomes})
        for (bool allocation : {false, true}) {
            VectorCompiled compiled{block, features, allocation, scalar};
            Compare(compiled, block, samples, loop, memory);
            if (!features.lasx || !features.lsx)
                Check(compiled.translator.VectorStats().lasx_fusions == 0,
                      "disabled LASX/LSX cannot fuse wide native operations");
            if (!allocation || !features.lsx)
                Check(compiled.translator.VectorStats().cache_hits == 0,
                      "disabled vector allocation cannot retain SIMD containers");
        }
    }
}

ir::Value Binary(ir::Assembler& as, ir::Block* block, O op, ir::Value a, ir::Value b,
                 u32 bits, bool sign, T type) {
    ir::Value result;
    switch (op) {
        case O::Vec4Add: result = as.Vec4Add(a, b); break;
        case O::Vec4Sub: result = as.Vec4Sub(a, b); break;
        case O::Vec4Mul: result = as.Vec4Mul(a, b); break;
        case O::Vec4And: result = as.Vec4And(a, b); break;
        case O::Vec4Or: result = as.Vec4Or(a, b); break;
        case O::VecAnd: result = as.VecAnd(a, b); break;
        case O::VecOr: result = as.VecOr(a, b); break;
        case O::VecXor: result = as.VecXor(a, b); break;
        case O::VecAndNot: result = as.VecAndNot(a, b); break;
        case O::VecMulHigh16: result = as.VecMulHigh16(a, b, ir::Imm{u64(sign)}); break;
        case O::VecMin: case O::VecMax: case O::VecSatAdd: case O::VecSatSub:
            result = ir::Value{block->AppendInst(op, a, b, ir::Imm{u64(bits)}, ir::Imm{u64(sign)})}; break;
        default: result = ir::Value{block->AppendInst(op, a, b, ir::Imm{u64(bits)})}; break;
    }
    return result.SetType(type);
}

void NativeWideOperations() {
    const auto host = la::HostFeatures::Detect();
    for (auto op : {O::Vec4Add, O::Vec4Sub, O::Vec4Mul, O::Vec4And, O::Vec4Or,
                   O::VecAdd, O::VecSub, O::VecMul, O::VecAnd, O::VecOr, O::VecXor, O::VecAndNot,
                   O::VecCmpEq, O::VecCmpGt, O::VecMin, O::VecMax, O::VecAvg,
                   O::VecSatAdd, O::VecSatSub, O::VecMulHigh16})
    for (u32 bits : {8u, 16u, 32u, 64u})
    for (bool sign : {false, true}) {
        const bool controlled = op == O::VecMin || op == O::VecMax ||
                                op == O::VecSatAdd || op == O::VecSatSub || op == O::VecMulHigh16;
        const bool fixed = op == O::Vec4Add || op == O::Vec4Sub || op == O::Vec4Mul ||
                           op == O::Vec4And || op == O::Vec4Or || op == O::VecAnd || op == O::VecOr ||
                           op == O::VecXor || op == O::VecAndNot || op == O::VecMulHigh16;
        if ((!controlled && sign) || (fixed && bits != 16) || (op == O::VecAvg && bits > 16)) continue;
        // The IR's saturating operations intentionally expose 8/16/32 bits.
        if ((op == O::VecSatAdd || op == O::VecSatSub) && bits == 64) continue;
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x7800})};
        ir::Assembler as{block.get()};
        auto a = as.LoadUniform(ir::Uniform{0, T::V256}).SetType(T::V256);
        auto b = as.LoadUniform(ir::Uniform{32, T::V256}).SetType(T::V256);
        auto value = Binary(as, block.get(), op, a, b, bits, sign, T::V256);
        value = Binary(as, block.get(), op, value, a, bits, sign, T::V256);
        as.StoreUniform(ir::Uniform{256, T::V256}, value);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        for (bool allocation : {false, true}) {
            VectorCompiled compiled{block.get(), host, allocation};
            Compare(compiled, block.get(), 12);
            CheckWideOracle(compiled, op, bits, sign);
            if (host.lsx && host.lasx) {
                Check(compiled.translator.VectorStats().lasx_fusions == 2 &&
                      compiled.translator.VectorStats().lasx_operations == 2,
                      "each original lane-local V256 binary emits one LASX operation");
                const auto& emitted = compiled.translator.Stats();
                Check(emitted.direct + emitted.helpers + compiled.translator.VectorStats().lasx_fusions ==
                      compiled.translator.EmittedIR()->GetInstList().size(),
                      "native emitter statistics count each fused LASX operation once and skip its tail");
            }
        }
        auto no_lasx = host; no_lasx.lasx = false;
        VectorCompiled split{block.get(), no_lasx, true};
        Compare(split, block.get(), 8);
        Check(split.translator.VectorStats().lasx_fusions == 0, "wide operations split when LASX is disabled");
    }
}

void LongChains() {
    const auto host = la::HostFeatures::Detect();
    for (auto type : {T::V128, T::V256}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x7900})};
        ir::Assembler as{block.get()};
        auto a = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
        auto b = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
        auto value = a;
        for (u32 i = 0; i < 48; ++i) {
            value = as.VecAdd(value, b, ir::Imm{u64{32}}).SetType(type);
            value = as.VecXor(value, a).SetType(type);
        }
        as.StoreUniform(ir::Uniform{256, type}, value);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        CompareModes(block.get());
        auto features = host;
        for (bool lasx : {false, true}) {
            features.lasx = lasx && host.lasx;
            VectorCompiled enabled{block.get(), features, true}, disabled{block.get(), features, false};
            if (host.lsx) {
                const auto on = enabled.translator.VectorStats(), off = disabled.translator.VectorStats();
                std::cout << "VECTOR-COST width=" << (type == T::V256 ? 256 : 128)
                          << " lasx=" << features.lasx << " bytes_off=" << disabled.context.CurrentBufferSize()
                          << " bytes_on=" << enabled.context.CurrentBufferSize()
                          << " home_bytes_off=" << off.load_bytes + off.store_bytes
                          << " home_bytes_on=" << on.load_bytes + on.store_bytes
                          << " moves_off=" << off.moves << " moves_on=" << on.moves
                          << " direct_reads_off=" << off.direct_reads << " direct_reads_on=" << on.direct_reads
                          << " fused=" << on.lasx_fusions << std::endl;
                Check(on.load_bytes + on.store_bytes < (off.load_bytes + off.store_bytes) / 8 &&
                      enabled.context.CurrentBufferSize() < disabled.context.CurrentBufferSize() &&
                      on.cache_hits > 90 && on.spills == 0,
                      "long native vector chain reduces home traffic and generated bytes without spills");
            }
        }
    }
}

void CompareRegionOperands(ir::Block* block, bool pressure) {
    const auto host = la::HostFeatures::Detect();
    for (bool lasx : {false, true}) {
        auto features = host;
        features.lasx = lasx && host.lasx;
        for (auto scalar : {M::GlobalCFG, M::CanonicalHomes})
        for (bool allocation : {false, true}) {
            VectorCompiled compiled{block, features, allocation, scalar};
            Compare(compiled, block);
            const auto& stats = compiled.translator.VectorStats();
            Check(!stats.global_cfg, "single-node read-only operands exercise region residency");
            if (allocation && features.lsx) {
                std::cout << "VECTOR-REGION pressure=" << pressure << " lasx=" << features.lasx
                          << " scalar=" << u32(scalar) << " direct_reads=" << stats.direct_reads
                          << " moves=" << stats.moves << " spills=" << stats.spills << std::endl;
                Check(stats.direct_reads > 0, "resident region operands are consumed directly");
                if (pressure) Check(stats.spills > 0, "retained region operands cause a real dirty spill");
            }
        }
    }
}

void RegionReadonlyPressure() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x7980})};
    ir::Assembler as{block.get()};
    std::array<ir::Value, 16> input, source;
    for (u32 i = 0; i < input.size(); ++i)
        input[i] = as.LoadUniform(ir::Uniform{i * 16, T::V128}).SetType(T::V128);
    // Uniform loads end a region. Fill the sixteen SIMD cache slots with
    // native definitions, keeping every original input and source live.
    for (u32 i = 0; i < source.size(); ++i)
        source[i] = as.VecAdd(input[i], input[(i + 5) % input.size()], ir::Imm{u64{32}}).SetType(T::V128);
    std::array<ir::Value, 8> probe;
    for (u32 i = 0; i < probe.size(); ++i) {
        const auto alias = (i & 1) ? as.VecMul(source[i], source[i], ir::Imm{u64{32}}).SetType(T::V128) :
                                    as.VecAnd(source[i], source[i]).SetType(T::V128);
        const auto mixed = as.VecAdd(source[i], source[source.size() - 1 - i], ir::Imm{u64{32}}).SetType(T::V128);
        probe[i] = as.VecXor(mixed, alias).SetType(T::V128);
    }
    // Checking all retained sources detects destructive input aliases even
    // when a corrupted source happens to cancel out of the final expression.
    for (u32 i = 0; i < input.size(); ++i)
        as.StoreUniform(ir::Uniform{384 + i * 16, T::V128}, input[i]);
    for (u32 i = 0; i < source.size(); ++i)
        as.StoreUniform(ir::Uniform{640 + i * 16, T::V128}, source[i]);
    for (u32 i = 0; i < probe.size(); ++i)
        as.StoreUniform(ir::Uniform{896 + i * 16, T::V128}, probe[i]);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    CompareRegionOperands(block.get(), true);
}

void RegionWideHalfPreservation() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x79c0})};
    ir::Assembler as{block.get()};
    std::array<ir::Value, 8> input;
    for (u32 i = 0; i < input.size(); ++i)
        input[i] = as.LoadUniform(ir::Uniform{i * 32, T::V256}).SetType(T::V256);
    std::array<ir::Value, 6> result;
    result[0] = as.VecAdd(input[0], input[1], ir::Imm{u64{32}}).SetType(T::V256);
    result[1] = as.VecXor(input[2], input[2]).SetType(T::V256);
    // Retain low/high shuffles separately, including two aliases of each
    // half. Later consumers and stores must still see the original wide value.
    result[2] = as.VecZip(result[0], result[0], ir::Imm{u64{16}}, ir::Imm{u64{0}}).SetType(T::V256);
    result[3] = as.VecZip(result[0], result[0], ir::Imm{u64{16}}, ir::Imm{u64{1}}).SetType(T::V256);
    result[4] = as.VecUnzip(result[3], result[0], ir::Imm{u64{32}}, ir::Imm{u64{0}}).SetType(T::V256);
    result[5] = as.VecXor(result[0], result[4]).SetType(T::V256);
    for (u32 i = 0; i < input.size(); ++i)
        as.StoreUniform(ir::Uniform{512 + i * 32, T::V256}, input[i]);
    for (u32 i = 0; i < result.size(); ++i)
        as.StoreUniform(ir::Uniform{768 + i * 32, T::V256}, result[i]);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    CompareRegionOperands(block.get(), false);
}

void Pressure() {
    const auto host = la::HostFeatures::Detect();
    for (auto type : {T::V128, T::V256}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x7a00})};
        ir::Assembler as{block.get()};
        auto a = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
        auto b = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
        // Zip lowers to independent dirty V128 halves, which must both stay
        // live when a later LASX head consumes the high input early.
        b = as.VecZip(b, a, ir::Imm{u64{16}}, ir::Imm{u64{1}}).SetType(type);
        std::array<ir::Value, 32> intermediates;
        auto value = a;
        for (u32 i = 0; i < intermediates.size(); ++i) {
            value = as.VecAdd(value, b, ir::Imm{u64{16}}).SetType(type);
            intermediates[i] = value;
        }
        for (u32 i = 0; i < intermediates.size(); ++i)
            value = as.VecXor(value, intermediates[(i * 13) % intermediates.size()]).SetType(type);
        as.StoreUniform(ir::Uniform{256, type}, value);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        CompareModes(block.get(), 16);
        VectorCompiled compiled{block.get(), host, true};
        if (host.lsx) Check(compiled.translator.VectorStats().spills > 0,
                            "more than sixteen live vector containers exercise a real dirty spill");
    }
}

u64 ClobberVectors(u64 value, u64, u64) {
    // LP64D caller-saved SIMD aliases must not be live across a helper call.
#if defined(__loongarch__)
    if (la::HostFeatures::Detect().lsx) asm volatile(
        "vxor.v $vr8, $vr8, $vr8\n\t" "vxor.v $vr9, $vr9, $vr9\n\t"
        "vxor.v $vr10, $vr10, $vr10\n\t" "vxor.v $vr11, $vr11, $vr11\n\t"
        "vxor.v $vr12, $vr12, $vr12\n\t" "vxor.v $vr13, $vr13, $vr13\n\t"
        "vxor.v $vr14, $vr14, $vr14\n\t" "vxor.v $vr15, $vr15, $vr15\n\t"
        "vxor.v $vr16, $vr16, $vr16\n\t" "vxor.v $vr17, $vr17, $vr17\n\t"
        "vxor.v $vr18, $vr18, $vr18\n\t" "vxor.v $vr19, $vr19, $vr19\n\t"
        "vxor.v $vr20, $vr20, $vr20\n\t" "vxor.v $vr21, $vr21, $vr21\n\t"
        "vxor.v $vr22, $vr22, $vr22\n\t" "vxor.v $vr23, $vr23, $vr23"
        ::: "f8", "f9", "f10", "f11", "f12", "f13", "f14", "f15",
            "f16", "f17", "f18", "f19", "f20", "f21", "f22", "f23", "memory");
#endif
    return std::rotl(value, 17) + 29;
}

void Boundaries() {
    for (auto type : {T::V128, T::V256}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x7b00})};
        ir::Assembler as{block.get()};
        auto a = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
        auto b = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
        auto condition = as.LoadUniform(ir::Uniform{96, T::U64}).SetType(T::U64);
        auto address = as.LoadUniform(ir::Uniform{104, T::U64}).SetType(T::U64);
        auto value = as.VecAdd(a, b, ir::Imm{u64{16}}).SetType(type);
        auto called = as.CallHost(ClobberVectors, condition).SetType(T::U64);
        auto after_call = as.VecXor(value, a).SetType(type);
        // Pack is an explicit canonical helper; byte shuffle and Zip/Unzip
        // exercise cross-half WidePlan lowering rather than LASX fusion.
        auto packed = as.VecPack(after_call, b, ir::Imm{u64{16}}, ir::Imm{u64{0}}).SetType(type);
        auto shifted = as.VecByteShift(packed, value, ir::Imm{type == T::V256 ? u64{17} : u64{7}},
                                       ir::Imm{u64{0}}).SetType(type);
        auto zipped = as.VecZip(packed, value, ir::Imm{u64{32}}, ir::Imm{u64{1}}).SetType(type);
        auto unzipped = as.VecUnzip(zipped, after_call, ir::Imm{u64{32}}, ir::Imm{u64{0}}).SetType(type);
        auto combined = as.VecOr(unzipped, shifted).SetType(type);
        as.StoreMemory(ir::Operand{address}, combined);
        auto memory_value = as.LoadMemory(ir::Operand{address}).SetType(type);
        auto fp = as.VecFAddScalar32(value, a).SetType(type);
        auto after_fp = as.VecXor(fp, value).SetType(type);
        auto edge = as.Goto(ir::BOOL{condition});
        as.StoreUniform(ir::Uniform{288, type}, as.VecXor(memory_value, combined).SetType(type));
        as.BindLabel(edge);
        as.StoreUniform(ir::Uniform{256, type}, as.VecAdd(after_fp, memory_value, ir::Imm{u64{32}}).SetType(type));
        as.StoreUniform(ir::Uniform{352, T::U64}, called);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        CompareModes(block.get(), 16, false, true);
    }
    IntrusivePtr<ir::Block> narrow{new ir::Block(ir::Location{0x7b80})};
    ir::Assembler as{narrow.get()};
    auto a = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
    auto b = as.LoadUniform(ir::Uniform{16, T::V128}).SetType(T::V128);
    auto view = as.VecXor(a, b).SetType(T::U8);
    auto masked = as.Add(view, ir::Operand{ir::Imm{u64{7}}}).SetType(T::U8);
    auto restored = as.BitCast(view).SetType(T::V128);
    as.StoreUniform(ir::Uniform{256, T::V128}, restored);
    as.StoreUniform(ir::Uniform{272, T::U8}, masked);
    narrow->SetTerminal(ir::terminal::ReturnToHost{});
    CompareModes(narrow.get());
}

void Loop() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x7c00})};
    ir::Assembler as{block.get()};
    auto count = as.LoadUniform(ir::Uniform{96, T::U64}).SetType(T::U64);
    auto value = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
    auto addend = as.LoadUniform(ir::Uniform{32, T::V128}).SetType(T::V128);
    auto sum = as.VecAdd(value, addend, ir::Imm{u64{32}}).SetType(T::V128);
    auto other = as.VecXor(sum, value).SetType(T::V128);
    as.StoreUniform(ir::Uniform{0, T::V128}, sum);
    auto next = as.Sub(count, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{96, T::U64}, next);
    auto branch = as.Goto(as.TestNotZero(next));
    as.BindLabel(branch);
    auto* label = &block->GetInstList().back();
    block->RemoveInst(label); block->InsertBefore(label, count.Def());
    as.StoreUniform(ir::Uniform{256, T::V128}, as.VecOr(sum, other).SetType(T::V128));
    block->SetTerminal(ir::terminal::ReturnToHost{}); block->ReIdInstr();
    CompareModes(block.get(), 24, true);
}

void NonContiguousHomes() {
    const auto host = la::HostFeatures::Detect();
    if (!host.lsx || !host.lasx) return;
    for (bool allocation : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x7d00})};
        ir::Assembler ir_as{block.get()};
        std::array<ir::Value, 4> input;
        for (u32 i = 0; i < 4; ++i) input[i] = ir_as.LoadUniform(ir::Uniform{i * 16, T::V128}).SetType(T::V128);
        auto low = ir_as.VecAdd(input[0], input[2], ir::Imm{u64{64}}).SetType(T::V128);
        auto high = ir_as.VecAdd(input[1], input[3], ir::Imm{u64{64}}).SetType(T::V128);
        ir_as.StoreUniform(ir::Uniform{256, T::V128}, low);
        ir_as.StoreUniform(ir::Uniform{272, T::V128}, high);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        u32 id = 1;
        for (auto& inst : block->GetInstList()) { inst.SetId(id); id += 2; }
        const la::WideVectorPair pair{low.Def(), high.Def(), {input[0].Def(), input[1].Def()},
                                                            {input[2].Def(), input[3].Def()}};
        la::JitContext context{false, host, {}, M::CanonicalHomes};
        context.ConfigureBlock(block.get());
        la::VectorContext vectors{context};
        vectors.Configure(block.get(), std::span{&pair, 1}, allocation);
        auto& as = context.GetMasm();
        as.addi_d(la::sp, la::sp, -16); as.st_d(la::ra, la::sp, 0); as.st_d(la::values, la::sp, 8);
        as.MV(la::values, la::a0);
        vectors.BeginInstruction(low.Def());
        Check(vectors.EmitWide(low.Def()), "explicit provenanced pair admits native wide operation");
        vectors.FinishInstruction();
        vectors.BeginInstruction(high.Def());
        Check(vectors.EmitWide(high.Def()), "wide high marker does not emit a duplicate operation");
        vectors.FinishInstruction(); vectors.Barrier();
        as.ld_d(la::values, la::sp, 8); as.ld_d(la::ra, la::sp, 0);
        as.addi_d(la::sp, la::sp, 16); as.jirl(la::zero, la::ra, 0); as.Finalize();
        Config config{TestConfig()}; backend::CodeCache cache{config, 1u << 20, FeatureSet{}};
        auto buffer = cache.AllocCode(context.CurrentBufferSize());
        Check(buffer.has_value(), "allocate non-contiguous LASX homes fixture"); context.Flush(*buffer);
        using Fn = void (*)(u64*);
        const auto fn = reinterpret_cast<Fn>(buffer->exec_data);
        for (u64 sample : {u64{0}, u64{1}, UINT64_MAX, u64{0x8000000000000000ULL}}) {
            alignas(32) std::array<u64, 64> homes;
            homes.fill(0xc95d812cc95d812cULL); auto expected = homes;
            for (u32 i = 0; i < 4; ++i) {
                const auto offset = context.HomeOffset(input[i].Def()) / 8;
                homes[offset] = sample + i * 0x0123456789abcdefULL;
                homes[offset + 1] = std::rotl(sample ^ (u64(i + 1) * 0x923cd58147bca968ULL), 11);
            }
            expected = homes;
            for (u32 half = 0; half < 2; ++half) for (u32 part = 0; part < 2; ++part)
                expected[context.HomeOffset(half ? high.Def() : low.Def()) / 8 + part] =
                    homes[context.HomeOffset(input[half].Def()) / 8 + part] +
                    homes[context.HomeOffset(input[half + 2].Def()) / 8 + part];
            fn(homes.data());
            Check(homes == expected, "LASX gathers/scatters only their two raw128 homes and preserve intervening sentinels");
        }
        Check(vectors.Stats().gathers == 2 && vectors.Stats().scatters == 1 &&
              vectors.Stats().permutations == 3 && vectors.Stats().lasx_operations == 1,
              "non-contiguous LASX traffic/permutation stats describe actual emitted instructions");
        auto malformed = pair;
        malformed.high = low.Def();
        bool rejected{};
        try { (void)la::VectorAllocationPlan::Build(block.get(), host, std::span{&malformed, 1}); }
        catch (const std::invalid_argument&) { rejected = true; }
        Check(rejected, "overlapping or non-adjacent fake LASX provenance rejects before emission");
        high.Def()->SetArg(2, ir::Imm{u64{32}}); rejected = false;
        try { (void)la::VectorAllocationPlan::Build(block.get(), host, std::span{&pair, 1}); }
        catch (const std::invalid_argument&) { rejected = true; }
        Check(rejected, "mismatched wide lane controls reject before emission");
    }
}
} // namespace

void VectorAllocationExecution() {
    NativeWideOperations(); LongChains(); RegionReadonlyPressure(); RegionWideHalfPreservation();
    Pressure(); Boundaries(); Loop(); NonContiguousHomes();
    std::cout << "PASS native LSX region residency, LASX fusion, pressure and canonical boundary differential\n";
}
} // namespace swift::tests::loongarch64
