#include "test_support.h"

#include <bit>
#include <chrono>
#include <iomanip>
#include "runtime/frontend/ir_assembler.h"

namespace swift::tests::riscv {

using O = ir::OpCode;
using T = ir::ValueType;

namespace {

void WarmKernel(BlockFn function, backend::State* state) {
    // Keep the selected core busy long enough for the board's existing
    // ondemand policy to settle, without changing any frequency settings.
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    do {
        for (u32 i = 0; i < 1000; ++i) function(state);
    } while (std::chrono::steady_clock::now() < until);
}

u64 ReverseBytes(u64 value, u32 width) {
    u64 result{};
    for (u32 byte = 0; byte < width / 8; ++byte) {
        result = (result << 8) | (value & 255);
        value >>= 8;
    }
    return result;
}

u32 ReferenceCrc(u32 crc, u64 input, u32 width) {
    for (u32 bit = 0; bit < width; ++bit) {
        const bool feedback = ((crc ^ input) & 1) != 0;
        crc >>= 1; input >>= 1;
        if (feedback) crc ^= 0x82f63b78;
    }
    return crc;
}

u64 ReferenceField(O op, u64 a, u64 b, u32 lsb, u32 width) {
    u64 result = op == O::BitExtract ? 0 : a;
    for (u32 i = 0; i < width; ++i) {
        const u64 target = u64{1} << (op == O::BitExtract ? i : lsb + i);
        const bool set = op == O::BitExtract ? ((a >> (lsb + i)) & 1) :
                         op == O::BitInsert ? ((b >> i) & 1) : false;
        result = set ? result | target : result & ~target;
    }
    return result;
}

}  // namespace

void NativeScalarBits(bool zbb) {
    rv::HostFeatures features{.zbb = zbb};
    const std::array<u64, 12> edges{0, 1, 2, 3, 127, 128, 255, 256,
            0x8000000000000000ULL, 0xffffffff, 0x0123456789abcdefULL, UINT64_MAX};
    u64 seed = 0x609323548734e5bdULL;
    for (O op : {O::PopCount, O::CountLeadingZeros64, O::CountTrailingZeros64,
                 O::CountLeadingZeros32, O::CountTrailingZeros32, O::ByteSwap, O::Crc32c,
                 O::BitExtract, O::BitInsert, O::BitClear, O::LocalParitySet}) {
        const bool count32 = op == O::CountLeadingZeros32 || op == O::CountTrailingZeros32;
        const u32 variants = op == O::ByteSwap ? 3 : op == O::Crc32c ? 4 :
                            op == O::LocalParitySet ? 2 :
                            (op == O::BitExtract || op == O::BitInsert || op == O::BitClear) ? 8 : 1;
        for (u32 variant = 0; variant < variants; ++variant) {
            const u32 width = op == O::ByteSwap ? 16u << variant : op == O::Crc32c ? 8u << variant :
                              std::array{0u, 1u, 8u, 16u, 31u, 32u, 63u, 64u}[variant];
            const u32 lsb = (op == O::BitExtract || op == O::BitInsert || op == O::BitClear) ? 64 - width : 0;
            // A zero-width field still has a valid shift position.
            const u32 position = width ? lsb : 37;
            IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xa100})};
            ir::Assembler as{block.get()};
            auto left = as.LoadUniform(ir::Uniform{0, count32 ? T::U32 : T::U64}).SetType(count32 ? T::U32 : T::U64);
            auto right = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
            ir::Value output;
            if (op == O::ByteSwap) output = as.ByteSwap(left, ir::Imm{u64(width)});
            else if (op == O::Crc32c) output = as.Crc32c(left, right, ir::Imm{u64(width)});
            else if (op == O::BitExtract || op == O::BitClear)
                output = ir::Value{block->AppendInst(op, left, ir::Imm{u64(position)}, ir::Imm{u64(width)})}.SetType(T::U64);
            else if (op == O::BitInsert)
                output = as.BitInsert(left, right, ir::Imm{u64(position)}, ir::Imm{u64(width)}).SetType(T::U64);
            else if (op == O::LocalParitySet) output = as.LocalParitySet(left, ir::Imm{u64(variant)});
            else output = ir::Value{block->AppendInst(op, left)}.SetType(count32 ? T::U32 : T::U64);
            as.StoreUniform(ir::Uniform{16, output.Type()}, output);
            block->SetTerminal(ir::terminal::ReturnToHost{});
            Compiled native{block.get(), true, features}, control{block.get(), false, features};
            Check(native.translator.Stats().helpers == 0 && control.translator.Stats().helpers == 0,
                  "scalar bit operations never enter the interpreter");
            const auto bytes = native.translator.Stats().bytes[static_cast<size_t>(op)];
            Check(bytes <= 384, "scalar bit operation stays within its native code budget");
            if (zbb && (op == O::PopCount || op == O::CountLeadingZeros64 || op == O::CountTrailingZeros64 || count32))
                Check(bytes == 4, "Zbb counts lower to exactly one instruction without spills");
            for (u32 i = 0; i < 96; ++i) {
                seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                const auto a = i < edges.size() ? edges[i] : seed;
                const auto b = std::rotl(a, 17) ^ 0xcbaed1943206587fULL;
                u64 wanted{};
                if (op == O::PopCount) wanted = std::popcount(a);
                else if (op == O::CountLeadingZeros64) wanted = std::countl_zero(a);
                else if (op == O::CountTrailingZeros64) wanted = std::countr_zero(a);
                else if (op == O::CountLeadingZeros32) wanted = std::countl_zero(u32(a));
                else if (op == O::CountTrailingZeros32) wanted = std::countr_zero(u32(a));
                else if (op == O::ByteSwap) wanted = ReverseBytes(a, width);
                else if (op == O::Crc32c) wanted = ReferenceCrc(a, b, width);
                else if (op == O::LocalParitySet) wanted = (std::popcount(u8(a)) % 2 == 0) ^ (variant != 0);
                else wanted = ReferenceField(op, a, b, position, width);
                StateStorage actual, expected;
                actual.Put(0, a); expected.Put(0, a); actual.Put(8, b); expected.Put(8, b);
                Check(native.fn(actual.state) == HaltReason::CallHost && control.fn(expected.state) == HaltReason::CallHost,
                      "scalar bit native/control halt");
                Check(actual.Get(16) == wanted && expected.Get(16) == wanted,
                      std::string{ir::GetIRMetaInfo(op).name} + "/" + std::to_string(variant) +
                      "/" + std::to_string(i) + ": expected " + std::to_string(wanted) +
                      ", cached " + std::to_string(actual.Get(16)) + ", uncached " + std::to_string(expected.Get(16)));
            }
        }
    }
    for (u32 cond = 0; cond < 16; ++cond) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xa200})};
        ir::Assembler as{block.get()};
        auto input = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto output = as.FCmpCondSet(input, ir::Cond(cond));
        as.StoreUniform(ir::Uniform{8, T::U8}, output);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled cached{block.get(), true, features}, uncached{block.get(), false, features};
        Check(cached.translator.Stats().helpers == 0, "float predicates use native integer instructions");
        for (u64 packed : {u64{0}, u64{1}, u64{4}, u64{7}}) {
            StateStorage a, b;
            a.Put(0, packed); b.Put(0, packed);
            const bool n = packed == 1, z = packed == 4, v = packed == 7;
            const std::array<bool, 16> wanted{z, !z, !n, n, n, !n, v, !v,
                !n && !z, n || z, n == v, n != v, !z && n == v, z || n != v, true, true};
            Check(cached.fn(a.state) == HaltReason::CallHost && uncached.fn(b.state) == HaltReason::CallHost,
                  "packed FP predicate halt");
            Check(a.Get(8) == wanted[cond] && b.Get(8) == wanted[cond], "independent packed FP predicate");
        }
    }
    std::cout << "PASS native scalar bits, CRC32C and FP predicates, Zbb=" << zbb << '\n';
}

void NativeScalarALU(bool zbb) {
    rv::HostFeatures features{.zbb = zbb};
    for (T type : {T::U8, T::U16, T::U32, T::U64})
    for (O op : {O::Add, O::Sub, O::And, O::Or, O::Xor, O::Adc, O::Sbb, O::AndNot})
    for (s64 immediate : {-2049, -2048, -1, 0, 1, 2047, 2048}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xa500})};
        ir::Assembler as{block.get()};
        auto input = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
        auto output = ir::Value{block->AppendInst(op, input, ir::Operand{ir::Imm{u64(immediate)}})}.SetType(type);
        as.SaveFlags(output, ir::Flags::All);
        as.StoreUniform(ir::Uniform{8, type}, output);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled native{block.get(), true, features}, control{block.get(), false, features};
        Check(native.translator.Stats().helpers == 0, "ALU and its flags are emitted without interpreter calls");
        const s64 encoded = op == O::Sub || op == O::Sbb ? -immediate : immediate;
        if (type == T::U64 && encoded >= -2048 && encoded <= 2047 &&
            op != O::Adc && op != O::Sbb && op != O::AndNot)
            Check(native.translator.Stats().bytes[static_cast<size_t>(op)] == 4,
                  "encodable U64 ALU immediate lowers to exactly one instruction");
        for (u64 value : {u64{0}, u64{1}, u64{0x7fffffff}, u64{0x8000000080000000ULL}, UINT64_MAX})
        for (bool carry : {false, true}) {
            const u32 bits = ir::GetValueSizeByte(type) * 8;
            const u64 mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
            const u64 left = value & mask, right = u64(immediate);
            u64 wanted{};
            if (op == O::Add) wanted = left + right;
            if (op == O::Adc) wanted = left + right + carry;
            if (op == O::Sub) wanted = left - right;
            if (op == O::Sbb) wanted = left - right - !carry;
            if (op == O::And) wanted = left & right;
            if (op == O::Or) wanted = left | right;
            if (op == O::Xor) wanted = left ^ right;
            if (op == O::AndNot) wanted = left & ~right;
            StateStorage a, b;
            a.Put(0, value); b.Put(0, value);
            a.state->host_cpu_flags = b.state->host_cpu_flags = (u64{1} << 50) | (u64(carry) << 29);
            Check(native.fn(a.state) == HaltReason::CallHost && control.fn(b.state) == HaltReason::CallHost,
                  "immediate ALU native/control halt");
            Check(a.Get(8) == (wanted & mask) && b.Get(8) == (wanted & mask), "independent immediate ALU value");
            Check(a.state->host_cpu_flags == b.state->host_cpu_flags, "immediate ALU native/control flags");
        }
    }
    std::cout << "PASS native ALU immediate boundaries and single-instruction budgets, Zbb=" << zbb << '\n';
}

void NativeScalarShifts() {
    u64 seed = 0x93c284b42c7d1631ULL;
    for (T type : {T::U8, T::U16, T::U32, T::U64})
    for (O op : {O::LslImm, O::LsrImm, O::AsrImm, O::RorImm,
                 O::LslValue, O::LsrValue, O::AsrValue, O::RorValue})
    for (bool raw : {false, true}) {
        const u32 bits = ir::GetValueSizeByte(type) * 8;
        const u64 mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
        const bool immediate = op == O::LslImm || op == O::LsrImm || op == O::AsrImm || op == O::RorImm;
        const std::array counts{0u, 1u, bits - 1, bits, bits + 1, 63u, 64u, 255u};
        for (u32 variant = 0; variant < (immediate ? counts.size() : 1); ++variant) {
            const auto count = counts[variant];
            IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xa600})};
            ir::Assembler as{block.get()};
            auto input = as.LoadUniform(ir::Uniform{0, raw ? T::U64 : type}).SetType(raw ? T::U64 : type);
            if (raw) input = as.BitCast(input).SetType(type);
            auto shift = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
            auto output = immediate ? ir::Value{block->AppendInst(op, input, ir::Imm{u64(count)})}.SetType(type) :
                                      ir::Value{block->AppendInst(op, input, shift)}.SetType(type);
            // U64 consumers expose stray high bits that a narrow store would hide.
            auto exposed = as.ZeroExtend64(output).SetType(T::U64);
            as.StoreUniform(ir::Uniform{16, T::U64}, exposed);
            block->SetTerminal(ir::terminal::ReturnToHost{});
            for (bool cached : {false, true}) {
                Compiled native{block.get(), cached, rv::HostFeatures{}};
                Check(native.translator.Stats().helpers == 0, "all scalar shifts remain native RV64G");
                for (u32 sample = 0; sample < (immediate ? 32 : 32 * counts.size()); ++sample) {
                    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                    const auto edge = sample % 32;
                    const u64 value = edge == 0 ? UINT64_MAX : edge == 1 ? u64{1} << (bits - 1) :
                                      edge == 2 ? 0 : edge == 3 ? 1 : seed;
                    const auto actual_count = immediate ? count : counts[sample / 32];
                    const u32 n = actual_count & (bits - 1);
                    u64 wanted{};
                    if (op == O::LslImm || op == O::LslValue) wanted = value << n;
                    else if (op == O::LsrImm || op == O::LsrValue) wanted = (value & mask) >> n;
                    else if (op == O::AsrImm || op == O::AsrValue) {
                        wanted = (value & mask) >> n;
                        if ((value & (u64{1} << (bits - 1))) && n) wanted |= mask ^ (mask >> n);
                    } else wanted = n ? ((value & mask) >> n) | (value << (bits - n)) : value;
                    StateStorage state; state.Put(0, value); state.Put(8, actual_count);
                    Check(native.fn(state.state) == HaltReason::CallHost, "scalar shift halt");
                    Check(state.Get(16) == (wanted & mask), std::string{ir::GetIRMetaInfo(op).name} +
                          "/bits=" + std::to_string(bits) + "/raw=" + std::to_string(raw) +
                          "/count=" + std::to_string(actual_count) + "/sample=" + std::to_string(sample));
                }
            }
        }
    }
    std::cout << "PASS scalar shift widths, masked counts and raw BitCast upper bits\n";
}

void ScalarShiftBenchmark() {
    using Clock = std::chrono::steady_clock;
    constexpr u32 length = 128, iterations = 100000, compiles = 100;
    for (O op : {O::LsrImm, O::LsrValue, O::AsrImm, O::AsrValue}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xa700})};
        ir::Assembler as{block.get()};
        auto input = as.LoadUniform(ir::Uniform{0, T::U32}).SetType(T::U32);
        auto count = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        const bool immediate = op == O::LsrImm || op == O::AsrImm;
        for (u32 i = 0; i < length; ++i) {
            input = immediate ? ir::Value{block->AppendInst(op, input, ir::Imm{u64{1}})}.SetType(T::U32) :
                                ir::Value{block->AppendInst(op, input, count)}.SetType(T::U32);
            input = as.Xor(input, ir::Operand{ir::Imm{u64{0x932567a1}}}).SetType(T::U32);
        }
        as.StoreUniform(ir::Uniform{16, T::U32}, input);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled native{block.get(), true, rv::HostFeatures{}};
        StateStorage state; state.Put(0, 0xadc83f21); state.Put(8, 1);
        u32 wanted = 0xadc83f21;
        for (u32 i = 0; i < length; ++i) {
            const bool arithmetic = op == O::AsrImm || op == O::AsrValue;
            wanted = arithmetic ? (wanted >> 1) | (wanted & 0x80000000) : wanted >> 1;
            wanted ^= 0x932567a1;
        }
        WarmKernel(native.fn, state.state);
        const auto start = Clock::now();
        u64 checksum{};
        for (u32 i = 0; i < iterations; ++i) {
            native.fn(state.state); checksum += state.Get(16);
        }
        const auto end = Clock::now();
        Check(checksum == u64(wanted) * iterations, "benchmark independent shift checksum");
        const auto compile_start = Clock::now();
        for (u32 i = 0; i < compiles; ++i) {
            rv::JitContext context{true, rv::HostFeatures{}};
            rv::JitTranslator translator{context}; translator.Translate(block.get());
        }
        const auto compile_end = Clock::now();
        std::cout << std::fixed << std::setprecision(3) << "BENCH " << ir::GetIRMetaInfo(op).name
                  << " shift_bytes=" << native.translator.Stats().bytes[static_cast<size_t>(op)]
                  << " code_bytes=" << native.context.CurrentBufferSize()
                  << " runtime_ns_per_shift=" << std::chrono::duration<double, std::nano>(end - start).count() / iterations / length
                  << " compile_us_per_block=" << std::chrono::duration<double, std::micro>(compile_end - compile_start).count() / compiles
                  << " checksum=" << checksum << '\n';
    }
}

void ScalarFeatureBenchmark() {
    using Clock = std::chrono::steady_clock;
    constexpr u32 length = 128, iterations = 100000, compiles = 100;
    const auto features = rv::HostFeatures::Detect();
    for (O op : {O::PopCount, O::CountLeadingZeros64, O::CountTrailingZeros64}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xa800})};
        ir::Assembler as{block.get()};
        auto input = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        for (u32 i = 0; i < length; ++i) {
            input = ir::Value{block->AppendInst(op, input)}.SetType(T::U64);
            input = as.Xor(input, ir::Operand{ir::Imm{u64{0x932567a1}}}).SetType(T::U64);
        }
        as.StoreUniform(ir::Uniform{16, T::U64}, input);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled native{block.get(), true, features};
        StateStorage state; state.Put(0, 0xadc83f21);
        u64 wanted = 0xadc83f21;
        for (u32 i = 0; i < length; ++i) {
            wanted = op == O::PopCount ? std::popcount(wanted) :
                     op == O::CountLeadingZeros64 ? std::countl_zero(wanted) : std::countr_zero(wanted);
            wanted ^= 0x932567a1;
        }
        WarmKernel(native.fn, state.state);
        const auto start = Clock::now();
        u64 checksum{};
        for (u32 i = 0; i < iterations; ++i) { native.fn(state.state); checksum += state.Get(16); }
        const auto end = Clock::now();
        Check(checksum == wanted * iterations, "benchmark independent scalar count checksum");
        const auto compile_start = Clock::now();
        for (u32 i = 0; i < compiles; ++i) {
            rv::JitContext context{true, features};
            rv::JitTranslator translator{context}; translator.Translate(block.get());
        }
        const auto compile_end = Clock::now();
        std::cout << std::fixed << std::setprecision(3) << "BENCH " << ir::GetIRMetaInfo(op).name
                  << " detected_zbb=" << features.zbb
                  << " op_bytes=" << native.translator.Stats().bytes[static_cast<size_t>(op)]
                  << " code_bytes=" << native.context.CurrentBufferSize()
                  << " runtime_ns_per_op=" << std::chrono::duration<double, std::nano>(end - start).count() / iterations / length
                  << " compile_us_per_block=" << std::chrono::duration<double, std::micro>(compile_end - compile_start).count() / compiles
                  << " checksum=" << checksum << '\n';
    }
}

}  // namespace swift::tests::riscv
