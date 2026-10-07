#include "test_support.h"

#include <bit>
#include "runtime/frontend/ir_assembler.h"

namespace swift::tests::riscv {

using O = ir::OpCode;
using T = ir::ValueType;

namespace {

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

}  // namespace swift::tests::riscv
