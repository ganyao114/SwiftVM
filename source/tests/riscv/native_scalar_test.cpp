#include "test_support.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <iomanip>
#include "runtime/compile/builder/ir_builder.h"

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

u64 ReferenceIntegerShift(O op, u64 raw, u32 source_bits, u32 result_bits, u64 count) {
    const u32 copied = std::min(source_bits, result_bits);
    const u32 amount = count & (result_bits - 1);
    const bool arithmetic = op == O::AsrImm || op == O::AsrValue;
    u64 result{};
    for (u32 bit = 0; bit < result_bits; ++bit) {
        u32 source{};
        if (op == O::LslImm || op == O::LslValue)
            source = bit >= amount ? bit - amount : result_bits;
        else if (op == O::RorImm || op == O::RorValue)
            source = (bit + amount) % result_bits;
        else source = arithmetic ? std::min(bit + amount, copied - 1) : bit + amount;
        if (source < copied && ((raw >> source) & 1)) result |= u64{1} << bit;
    }
    return result;
}

void NativeIntegerConversions(bool zbb) {
    const std::array<u64, 8> inputs{0, 1, 0xdeadbeef00000080ULL, 0xabcdef0080008000ULL,
            0x100000000ULL, 0x8000000080000000ULL, 0xdeadbeef12345678ULL, UINT64_MAX};
    for (const auto source_type : {T::U8, T::S8, T::U16, T::S16, T::U32, T::S32, T::U64, T::S64})
    for (const auto op : {O::ZeroExtend32, O::ZeroExtend32To64, O::ZeroExtend64, O::SignExtend})
    for (const auto result_type : {T::U8, T::S8, T::U16, T::S16, T::U32, T::S32, T::U64, T::S64})
    for (const u32 transport : {0u, 1u, 2u})
    for (const bool keep_input : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block{ir::Location{0xa040}}};
        compile::IrBuilder as{block.get()};
        const auto raw = as.LoadUniform(T::U64, ir::Uniform{0, T::U64});
        auto input = transport ? as.BitCast(source_type, raw)
                               : ir::Value{raw}.SetCastType(source_type);
        if (transport == 2) input = as.GetResult(source_type, input, input);
        const auto output = ir::Value{as.AppendInst(op, result_type, input)};
        as.StoreUniform(ir::Uniform{16, T::U64}, as.ZeroExtend64(T::U64, output));
        if (keep_input)
            as.StoreUniform(ir::Uniform{24, T::U32}, ir::Value{input}.SetCastType(T::U32));
        block->SetTerminal(ir::terminal::ReturnToHost{});
        const u32 source_bits = ir::GetValueSizeByte(source_type) * 8;
        const u32 copied = op == O::ZeroExtend32 || op == O::ZeroExtend32To64
                ? std::min(source_bits, 32u) : source_bits;
        const auto label = std::string{ir::GetIRMetaInfo(op).name} + "/source=" +
                std::to_string(static_cast<u32>(source_type)) + "/result=" +
                std::to_string(static_cast<u32>(result_type)) + "/transport=" +
                std::to_string(transport) + "/keep=" + std::to_string(keep_input);
        for (const bool cached : {false, true}) {
            Compiled native{block.get(), cached, rv::HostFeatures{.zbb = zbb}};
            Check(native.translator.Stats().helpers == 0, "integer conversions remain native");
            for (const auto value : inputs) {
                u64 expected{};
                for (u32 bit = 0; bit < ir::GetValueSizeByte(result_type) * 8; ++bit) {
                    const bool on = bit < copied ? (value >> bit) & 1
                            : op == O::SignExtend && ((value >> (copied - 1)) & 1);
                    if (on) expected |= u64{1} << bit;
                }
                StateStorage state;
                state.Put(0, value); state.Put(16, UINT64_MAX); state.Put(24, UINT64_MAX);
                Check(native.fn(state.state) == HaltReason::CallHost, "integer conversion halt");
                Check(state.Get(16) == expected, label + "/cached=" + std::to_string(cached));
                if (keep_input)
                    Check(static_cast<u32>(state.Get(24)) == static_cast<u32>(value),
                          "conversion preserves its borrowed raw source");
            }
        }
    }
    std::cout << "PASS integer conversion views, raw sources and result widths, Zbb=" << zbb << '\n';
}

void NativeIntegerPredicates(bool zbb) {
    const rv::HostFeatures features{.zbb = zbb};
    for (O op : {O::TestZero, O::TestNotZero, O::Not, O::TestBit})
    for (u32 role = 0; role < (op == O::TestBit ? 6u : 2u); ++role) {
        IntrusivePtr<ir::Block> block{new ir::Block{ir::Location{0xa090}}};
        const auto input = block->LoadImm(ir::Imm{u64{0}});
        auto* invalid = op == O::TestBit ? block->AppendInst(op, T::U64, input, ir::Imm{u64{0}})
                                        : block->AppendInst(op, T::U64, input);
        if (role == 0) invalid->SetArg(0, ir::Value{input}.SetCastType(T::V64));
        else if (role == 1) invalid->SetReturn(T::V64);
        else invalid->SetArg(1, role == 2 ? ir::Imm{u64{0}, T::V32}
                : ir::Imm{role == 3 ? u64{64} : role == 4 ? u64{0x100000001} : UINT64_MAX});
        block->SetTerminal(ir::terminal::ReturnToHost{});
        block->ReIdInstr();
        rv::JitContext context{true, features};
        rv::JitTranslator translator{context};
        bool rejected = false;
        try { translator.Translate(block.get()); }
        catch (const std::runtime_error&) { rejected = true; }
        Check(rejected && context.CurrentBufferSize() == 0, "predicate preflight");
    }
    const auto config = TestConfig();
    backend::CodeCache arena{config, 1u << 20, FeatureSet{}};
    QuiescentRawCache release{arena};
    u32 kernels{};
    u64 predicate_values{}, selection_values{}, branch_values{};
    const auto execute = [&](ir::Block* block, bool cached, auto&& samples) {
        rv::JitContext context{cached, features};
        rv::JitTranslator translator{context};
        translator.Translate(block);
        ++kernels;
        auto code = arena.AllocCode(context.CurrentBufferSize());
        Check(code.has_value(), "predicate code");
        context.Flush(*code);
        Check(translator.Stats().helpers == 0, "native predicate");
        samples(reinterpret_cast<BlockFn>(code->exec_data));
        Check(arena.FreeCode(code->exec_data), "free predicate");
    };
    const std::array<u64, 9> values{0x100, 0, 1, 0x8000, 0x10000, 0x100000000ULL,
                                   0x8000000000000000ULL, UINT64_MAX, 0xfedcba9876543200ULL};
    const std::array<u64, 5> positions{0, 7, 15, 31, 63};
    constexpr u64 left_pattern = 0xfedcba9876543210ULL, right_pattern = 0x1234567890abcdefULL;
    struct PredicateCase { O op; u32 offset; u64 bit; bool raw; bool keep; };
    struct SelectionCase { O op; u32 offset; u32 arms; bool raw; bool keep; };
    for (T source_type : {T::U8, T::S8, T::U16, T::S16, T::U32, T::S32, T::U64, T::S64})
    for (T result_type : {T::U8, T::S8, T::U16, T::S16, T::U32, T::S32, T::U64, T::S64}) {
        const u32 source_bits = ir::GetValueSizeByte(source_type) * 8;
        const u32 result_bits = ir::GetValueSizeByte(result_type) * 8;
        const u64 source_mask = source_bits == 64 ? UINT64_MAX : (u64{1} << source_bits) - 1;
        const u64 result_mask = result_bits == 64 ? UINT64_MAX : (u64{1} << result_bits) - 1;
        IntrusivePtr<ir::Block> predicates{new ir::Block{ir::Location{0xa098}}};
        compile::IrBuilder p{predicates.get()};
        std::array<PredicateCase, 32> pcases;
        u32 pcount{};
        for (O op : {O::TestZero, O::TestNotZero, O::Not, O::TestBit})
        for (bool raw : {false, true})
        for (bool keep : {false, true})
        for (u32 variant = 0; variant < (op == O::TestBit ? positions.size() : 1); ++variant) {
            const u32 offset = pcount * 24;
            pcases[pcount++] = {op, offset, positions[variant], raw, keep};
            const auto original = p.LoadUniform(ir::Uniform{offset, raw ? T::U64 : source_type});
            const auto input = raw ? p.BitCast(source_type, original) : original;
            const auto result = op == O::TestBit
                    ? ir::Value{predicates->AppendInst(op, result_type, input, ir::Imm{positions[variant]})}
                    : ir::Value{predicates->AppendInst(op, result_type, input)};
            p.StoreUniform(ir::Uniform{offset + 8, T::U64}, ir::Value{result}.SetCastType(T::U64));
            if (keep) p.StoreUniform(ir::Uniform{offset + 16, T::U64}, ir::Value{original}.SetCastType(T::U64));
        }
        predicates->SetTerminal(ir::terminal::ReturnToHost{});
        for (bool cached : {false, true}) execute(predicates.get(), cached, [&](BlockFn entry) {
            StateStorage state;
            for (u64 raw : values) {
                for (const auto& test : pcases) {
                    state.Put(test.offset, raw);
                    state.Put(test.offset + 8, UINT64_MAX);
                    state.Put(test.offset + 16, UINT64_MAX);
                }
                Check(entry(state.state) == HaltReason::CallHost, "predicate halt");
                for (const auto& test : pcases) {
                    const u64 logical = raw & source_mask;
                    const u64 wanted = test.op == O::TestBit ? (logical >> test.bit) & 1
                            : test.op == O::TestNotZero ? logical != 0 : logical == 0;
                    const u64 actual = state.Get(test.offset + 8);
                    if (actual != wanted)
                        Check(false, std::string{ir::GetIRMetaInfo(test.op).name} + "/source=" +
                              std::to_string(static_cast<u32>(source_type)) + "/result=" +
                              std::to_string(static_cast<u32>(result_type)) + "/bit=" +
                              std::to_string(test.bit) + "/raw=" + std::to_string(raw) +
                              "/cached=" + std::to_string(cached));
                    Check(actual == wanted, "predicate value");
                    ++predicate_values;
                    if (test.keep)
                        Check(state.Get(test.offset + 16) == (test.raw ? raw : logical), "predicate raw");
                }
            }
        });
        IntrusivePtr<ir::Block> selections{new ir::Block{ir::Location{0xa09c}}};
        compile::IrBuilder s{selections.get()};
        std::array<SelectionCase, 32> scases;
        u32 scount{};
        for (O op : {O::Select, O::SelectZero})
        for (bool raw : {false, true})
        for (bool keep : {false, true})
        for (u32 arms : {0u, 1u, 2u, 3u}) {
            const u32 offset = scount * 64;
            scases[scount++] = {op, offset, arms, raw, keep};
            const auto original = s.LoadUniform(ir::Uniform{offset, raw ? T::U64 : source_type});
            auto condition = raw ? s.BitCast(source_type, original) : original;
            const auto left = arms == 3 ? s.LoadImm(ir::Imm{u64{1}})
                    : arms == 1 ? original : s.LoadUniform(ir::Uniform{offset + 8, T::U64});
            const auto right = arms == 3 ? s.LoadImm(ir::Imm{u64{0}})
                    : arms == 2 ? original : s.LoadUniform(ir::Uniform{offset + 16, T::U64});
            if (arms == 3) condition = s.TestNotZero(condition);
            const auto result = ir::Value{selections->AppendInst(op, result_type, condition,
                    ir::Value{left}.SetCastType(result_type), ir::Value{right}.SetCastType(result_type))};
            s.StoreUniform(ir::Uniform{offset + 24, result_type}, result);
            if (keep) {
                s.StoreUniform(ir::Uniform{offset + 32, T::U64}, ir::Value{original}.SetCastType(T::U64));
                s.StoreUniform(ir::Uniform{offset + 40, T::U64}, ir::Value{left}.SetCastType(T::U64));
                s.StoreUniform(ir::Uniform{offset + 48, T::U64}, ir::Value{right}.SetCastType(T::U64));
            }
        }
        selections->SetTerminal(ir::terminal::ReturnToHost{});
        for (bool cached : {false, true}) execute(selections.get(), cached, [&](BlockFn entry) {
            StateStorage state;
            for (u64 raw : values) {
                for (const auto& test : scases) {
                    state.Put(test.offset, raw);
                    state.Put(test.offset + 8, left_pattern);
                    state.Put(test.offset + 16, right_pattern);
                    for (u32 field : {24u, 32u, 40u, 48u}) state.Put(test.offset + field, UINT64_MAX);
                }
                Check(entry(state.state) == HaltReason::CallHost, "selection halt");
                for (const auto& test : scases) {
                    const u64 materialized = test.raw ? raw : raw & source_mask;
                    const u64 left = test.arms == 3 ? 1 : test.arms == 1 ? materialized : left_pattern;
                    const u64 right = test.arms == 3 ? 0 : test.arms == 2 ? materialized : right_pattern;
                    const bool nonzero = (raw & source_mask) != 0;
                    const u64 wanted = ((test.op == O::SelectZero ? !nonzero : nonzero) ? left : right) & result_mask;
                    const u64 actual = state.Get(test.offset + 24) & result_mask;
                    if (actual != wanted)
                        Check(false, "selection/source=" + std::to_string(static_cast<u32>(source_type)) +
                              "/result=" + std::to_string(static_cast<u32>(result_type)) +
                              "/arms=" + std::to_string(test.arms) + "/raw=" + std::to_string(raw) +
                              "/cached=" + std::to_string(cached));
                    Check(actual == wanted, "selection value");
                    ++selection_values;
                    if (test.keep) {
                        Check(state.Get(test.offset + 32) == materialized, "condition raw");
                        Check(state.Get(test.offset + 40) == left, "left raw");
                        Check(state.Get(test.offset + 48) == right, "right raw");
                    }
                }
            }
        });
    }
    for (T type : {T::U8, T::S8, T::U16, T::S16, T::U32, T::S32, T::U64, T::S64})
    for (O op : {O::Goto, O::NotGoto}) {
        IntrusivePtr<ir::Block> block{new ir::Block{ir::Location{0xa09e}}};
        const auto original = block->LoadUniform(ir::Uniform{0, T::U64});
        const auto input = block->BitCast(type, original);
        const auto zero = block->LoadImm(ir::Imm{u64{0}}), one = block->LoadImm(ir::Imm{u64{1}});
        block->StoreUniform(ir::Uniform{8, T::U64}, zero);
        const auto jump = ir::Value{block->AppendInst(op, T::U8, input)};
        block->StoreUniform(ir::Uniform{8, T::U64}, one);
        block->BindLabel(jump);
        block->StoreUniform(ir::Uniform{16, T::U64}, original);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        const u32 bits = ir::GetValueSizeByte(type) * 8;
        const u64 mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
        for (bool cached : {false, true}) execute(block.get(), cached, [&](BlockFn entry) {
            StateStorage state;
            for (u64 raw : values) {
                state.Put(0, raw); state.Put(8, UINT64_MAX); state.Put(16, UINT64_MAX);
                Check(entry(state.state) == HaltReason::CallHost, "condition halt");
                const bool taken = op == O::Goto ? (raw & mask) != 0 : (raw & mask) == 0;
                Check(state.Get(8) == (taken ? 0 : 1), "condition value");
                Check(state.Get(16) == raw, "branch raw");
                ++branch_values;
            }
        });
    }
    Check(kernels == 288 && predicate_values == 36864 && selection_values == 36864 && branch_values == 288,
          "predicate matrix");
    std::cout << "PASS integer predicate views and conditional aliases, Zbb=" << zbb
              << " kernels=" << kernels << " predicate_values=" << predicate_values
              << " selection_values=" << selection_values << " branch_values=" << branch_values << '\n';
}

}  // namespace

void NativeScalarBits(bool zbb) {
    NativeIntegerConversions(zbb);
    NativeIntegerPredicates(zbb);
    rv::HostFeatures features{.zbb = zbb};
    // Imported invalid domains must fail before BeginBlock or frame bytes.
    for (O op : {O::BitExtract, O::BitInsert, O::BitClear, O::ByteSwap, O::Crc32c,
                 O::CountLeadingZeros32, O::CountTrailingZeros64}) {
        IntrusivePtr<ir::Block> block{new ir::Block{ir::Location{0xa080}}};
        const auto source = block->LoadUniform(ir::Uniform{0, T::U64});
        if (op == O::BitExtract || op == O::BitClear)
            block->AppendInst(op, T::U64, source, ir::Imm{u64{64}}, ir::Imm{u64{1}});
        else if (op == O::BitInsert)
            block->BitInsert(T::U64, source, source, ir::Imm{u64{1}}, ir::Imm{UINT64_MAX});
        else if (op == O::ByteSwap)
            block->ByteSwap(T::U64, source, ir::Imm{(u64{1} << 32) | 32});
        else if (op == O::Crc32c)
            block->Crc32c(source, source, ir::Imm{u64{0}});
        else block->AppendInst(op, T::U64, ir::Value{source}.SetCastType(
                                                 op == O::CountLeadingZeros32 ? T::U64 : T::U32));
        block->SetTerminal(ir::terminal::ReturnToHost{});
        block->ReIdInstr();
        rv::JitContext context{true, features};
        rv::JitTranslator translator{context};
        bool rejected = false;
        try { translator.Translate(block.get()); }
        catch (const std::runtime_error&) { rejected = true; }
        Check(rejected && context.CurrentBufferSize() == 0,
              "common scalar contract rejects before RV64 code emission");
    }
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
        const bool count64 = op == O::CountLeadingZeros64 || op == O::CountTrailingZeros64;
        for (bool signed_view : {false, true})
        for (u32 variant = 0; variant < variants; ++variant) {
            if (signed_view && !count32 && !count64) continue;
            const u32 width = op == O::ByteSwap ? 16u << variant : op == O::Crc32c ? 8u << variant :
                              std::array{0u, 1u, 8u, 16u, 31u, 32u, 63u, 64u}[variant];
            const u32 lsb = (op == O::BitExtract || op == O::BitInsert || op == O::BitClear) ? 64 - width : 0;
            // A zero-width field still has a valid shift position.
            const u32 position = width ? lsb : 37;
            IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xa100})};
            compile::IrBuilder as{block.get()};
            auto left = as.LoadUniform(count32 ? T::U32 : T::U64,
                                       ir::Uniform{0, count32 ? T::U32 : T::U64});
            if (signed_view) left = left.SetCastType(count32 ? T::S32 : T::S64);
            auto right = as.LoadUniform(T::U64, ir::Uniform{8, T::U64});
            ir::Value output;
            if (op == O::ByteSwap) output = as.ByteSwap(left, ir::Imm{u64(width)});
            else if (op == O::Crc32c) output = as.Crc32c(left, right, ir::Imm{u64(width)});
            else if (op == O::BitExtract || op == O::BitClear)
                output = ir::Value{block->AppendInst(
                        op, T::U64, left, ir::Imm{u64(position)}, ir::Imm{u64(width)})};
            else if (op == O::BitInsert)
                output = as.BitInsert(
                        T::U64, left, right, ir::Imm{u64(position)}, ir::Imm{u64(width)});
            else if (op == O::LocalParitySet)
                output = as.LocalParitySet(left, ir::Imm{u64(variant)});
            else
                output = ir::Value{block->AppendInst(op, count32 ? T::U32 : T::U64, left)};
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
        compile::IrBuilder as{block.get()};
        auto input = as.LoadUniform(T::U64, ir::Uniform{0, T::U64});
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
        compile::IrBuilder as{block.get()};
        auto input = as.LoadUniform(type, ir::Uniform{0, type});
        auto output =
                ir::Value{block->AppendInst(op, type, input, ir::Operand{ir::Imm{u64(immediate)}})};
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

void NativeScalarShifts(bool zbb) {
    for (O op : {O::LslImm, O::LsrImm, O::AsrImm, O::RorImm,
                 O::LslValue, O::LsrValue, O::AsrValue, O::RorValue}) {
        const bool immediate = op == O::LslImm || op == O::LsrImm || op == O::AsrImm || op == O::RorImm;
        IntrusivePtr<ir::Block> block{new ir::Block{ir::Location{0xa580}}};
        const auto input = block->LoadImm(ir::Imm{u64{1}});
        if (immediate) block->AppendInst(op, T::U32, input, ir::Imm{u64{1}, T::V32});
        else block->AppendInst(op, T::U32, input, ir::Value{input}.SetCastType(T::V32));
        block->SetTerminal(ir::terminal::ReturnToHost{});
        block->ReIdInstr();
        rv::JitContext context{true, rv::HostFeatures{.zbb = zbb}};
        rv::JitTranslator translator{context};
        bool rejected = false;
        try { translator.Translate(block.get()); }
        catch (const std::runtime_error&) { rejected = true; }
        Check(rejected && context.CurrentBufferSize() == 0, "shift count rejected before RV64 emission");
    }
    // Every invocation and source/count check finishes before this buffer is
    // freed. Reuse one quiescent arena rather than mapping a new 1MiB cache
    // for every source/result/count combination under QEMU.
    const auto config = TestConfig();
    backend::CodeCache arena{config, 1u << 20, FeatureSet{}};
    QuiescentRawCache release_arena{arena};
    struct ShiftCase {
        bool raw;
        bool keep_inputs;
        bool shared_count;
        u32 offset;
        u32 variant;
        u32 samples;
        std::array<u64, 2> sample_seeds;
    };
    u64 seed = 0x93c284b42c7d1631ULL;
    u32 kernels{};
    u64 numerical_checks{};
    for (T source_type : {T::U8, T::S8, T::U16, T::S16, T::U32, T::S32, T::U64, T::S64})
    for (T result_type : {T::U8, T::S8, T::U16, T::S16, T::U32, T::S32, T::U64, T::S64})
    for (O op : {O::LslImm, O::LsrImm, O::AsrImm, O::RorImm,
                 O::LslValue, O::LsrValue, O::AsrValue, O::RorValue}) {
        const u32 source_bits = ir::GetValueSizeByte(source_type) * 8;
        const u32 bits = ir::GetValueSizeByte(result_type) * 8;
        const u64 source_mask = source_bits == 64 ? UINT64_MAX : (u64{1} << source_bits) - 1;
        const bool immediate = op == O::LslImm || op == O::LsrImm || op == O::AsrImm || op == O::RorImm;
        const std::array<u64, 8> counts{0, 1, bits - 1, bits, bits + 1, 63, 0x100000001ULL, UINT64_MAX};
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xa600})};
        compile::IrBuilder as{block.get()};
        std::array<ShiftCase, 32> cases;
        u32 case_count{};
        // Each case has independent loads and stores, so batching retains the
        // original source/count death points and the keep-input variants.
        // Save both PRNG streams in the original case/cache/sample order.
        for (bool raw : {false, true})
        for (bool keep_inputs : {false, true})
        for (u32 variant = 0; variant < (immediate ? counts.size() : 2); ++variant) {
            const bool shared_count = !immediate && variant == 1;
            const u32 offset = case_count * 40;
            auto& test = cases[case_count++];
            test = {raw, keep_inputs, shared_count, offset, variant,
                    immediate || shared_count ? 8u : 8u * static_cast<u32>(counts.size()), {}};
            for (bool cached : {false, true}) {
                test.sample_seeds[cached] = seed;
                for (u32 sample = 0; sample < test.samples; ++sample) {
                    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                }
            }
            const auto original = as.LoadUniform(raw ? T::U64 : source_type,
                                                 ir::Uniform{offset, raw ? T::U64 : source_type});
            const auto input = raw ? as.BitCast(source_type, original) : original;
            ir::Value raw_count, count;
            if (!immediate) {
                raw_count = shared_count ? original : as.LoadUniform(T::U64, ir::Uniform{offset + 8, T::U64});
                count = shared_count ? input : ir::Value{raw_count}.SetCastType(source_type);
            }
            const auto output = immediate
                    ? ir::Value{block->AppendInst(op, result_type, input, ir::Imm{counts[variant]})}
                    : ir::Value{block->AppendInst(op, result_type, input, count)};
            as.StoreUniform(ir::Uniform{offset + 16, T::U64}, as.ZeroExtend64(T::U64, output));
            if (keep_inputs) {
                as.StoreUniform(ir::Uniform{offset + 24, T::U64}, ir::Value{original}.SetCastType(T::U64));
                if (!immediate) as.StoreUniform(ir::Uniform{offset + 32, T::U64}, ir::Value{raw_count}.SetCastType(T::U64));
            }
        }
        block->SetTerminal(ir::terminal::ReturnToHost{});
        for (bool cached : {false, true}) {
            rv::JitContext context{cached, rv::HostFeatures{.zbb = zbb}};
            rv::JitTranslator translator{context};
            translator.Translate(block.get());
            ++kernels;
            auto code = arena.AllocCode(context.CurrentBufferSize());
            Check(code.has_value(), "allocate scalar shift code");
            context.Flush(*code);
            const auto entry = reinterpret_cast<BlockFn>(code->exec_data);
            Check(translator.Stats().helpers == 0, "native shifts");
            std::array<u64, 32> sample_seeds, materialized, amounts;
            for (u32 i = 0; i < case_count; ++i) sample_seeds[i] = cases[i].sample_seeds[cached];
            StateStorage state;
            for (u32 sample = 0; sample < (immediate ? 8u : 8u * counts.size()); ++sample) {
                for (u32 i = 0; i < case_count; ++i) {
                    const auto& test = cases[i];
                    if (sample >= test.samples) continue;
                    auto& value_seed = sample_seeds[i];
                    value_seed ^= value_seed << 13; value_seed ^= value_seed >> 7; value_seed ^= value_seed << 17;
                    const auto edge = sample % 8;
                    const u64 value = edge == 0 ? UINT64_MAX : edge == 1 ? u64{1} << (source_bits - 1) :
                                      edge == 2 ? 0 : edge == 3 ? 0x180 : value_seed;
                    materialized[i] = test.raw ? value : value & source_mask;
                    amounts[i] = immediate ? counts[test.variant]
                            : test.shared_count ? materialized[i] : counts[sample / 8];
                    state.Put(test.offset, value); state.Put(test.offset + 8, amounts[i]);
                    state.Put(test.offset + 16, UINT64_MAX);
                    if (test.keep_inputs) {
                        state.Put(test.offset + 24, UINT64_MAX);
                        if (!immediate) state.Put(test.offset + 32, UINT64_MAX);
                    }
                }
                Check(entry(state.state) == HaltReason::CallHost, "shift halt");
                for (u32 i = 0; i < case_count; ++i) {
                    const auto& test = cases[i];
                    if (sample >= test.samples) continue;
                    const auto expected = ReferenceIntegerShift(op, materialized[i], source_bits, bits, amounts[i]);
                    const auto actual = state.Get(test.offset + 16);
                    if (actual != expected)
                        Check(false, std::string{ir::GetIRMetaInfo(op).name} + "/source=" +
                              std::to_string(static_cast<u32>(source_type)) + "/result=" +
                              std::to_string(static_cast<u32>(result_type)) + "/raw=" + std::to_string(test.raw) +
                              "/keep=" + std::to_string(test.keep_inputs) + "/cached=" + std::to_string(cached) +
                              "/count=" + std::to_string(amounts[i]) + "/sample=" + std::to_string(sample) +
                              ": expected " + std::to_string(expected) + ", actual " + std::to_string(actual));
                    Check(actual == expected, "shift value");
                    ++numerical_checks;
                    if (test.keep_inputs) {
                        Check(state.Get(test.offset + 24) == materialized[i], "raw source");
                        if (!immediate) Check(state.Get(test.offset + 32) == amounts[i], "raw count");
                    }
                }
            }
            Check(arena.FreeCode(code->exec_data), "free shift code");
        }
    }
    Check(kernels == 1024 && numerical_checks == 278528, "shift coverage");
    std::cout << "PASS scalar shift views, mixed widths, masked counts and preserved sources, Zbb=" << zbb
              << " kernels=" << kernels << " values=" << numerical_checks << '\n';
}

void ScalarShiftBenchmark() {
    using Clock = std::chrono::steady_clock;
    constexpr u32 length = 128, iterations = 100000, compiles = 100;
    for (O op : {O::LsrImm, O::LsrValue, O::AsrImm, O::AsrValue}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xa700})};
        compile::IrBuilder as{block.get()};
        auto input = as.LoadUniform(T::U32, ir::Uniform{0, T::U32});
        auto count = as.LoadUniform(T::U64, ir::Uniform{8, T::U64});
        const bool immediate = op == O::LsrImm || op == O::AsrImm;
        for (u32 i = 0; i < length; ++i) {
            input = immediate ? ir::Value{block->AppendInst(op, T::U32, input, ir::Imm{u64{1}})}
                              : ir::Value{block->AppendInst(op, T::U32, input, count)};
            input = as.Xor(T::U32, input, ir::Operand{ir::Imm{u64{0x932567a1}}});
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
        compile::IrBuilder as{block.get()};
        auto input = as.LoadUniform(T::U64, ir::Uniform{0, T::U64});
        for (u32 i = 0; i < length; ++i) {
            input = ir::Value{block->AppendInst(op, T::U64, input)};
            input = as.Xor(T::U64, input, ir::Operand{ir::Imm{u64{0x932567a1}}});
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
