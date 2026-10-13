#include "test_support.h"

#include <bit>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/ir_assembler.h"

namespace swift::tests::loongarch64 {

using T = ir::ValueType;

void FlagsDifferential() {
    for (auto type : {T::U8, T::U16, T::U32, T::U64})
    for (auto op : {ir::OpCode::Adc, ir::OpCode::Sbb}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x1f00})};
        ir::Assembler as{block.get()};
        auto a = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
        auto b = as.LoadUniform(ir::Uniform{8, type}).SetType(type);
        auto result = ir::Value{block->AppendInst(op, a, ir::Operand{b})}.SetType(type);
        auto one = as.LoadImm(ir::Imm{u64{1}}).SetType(T::U8);
        as.SetCarry(one);
        as.SaveFlags(result, ir::Flags::Carry | ir::Flags::Overflow);
        as.StoreUniform(ir::Uniform{16, type}, result);
        as.StoreUniform(ir::Uniform{24, T::U64}, as.GetFlags(result, ir::Flags::All).SetType(T::U64));
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled cached{block.get()}, uncached{block.get(), false};
        const u32 bits = ir::GetValueSizeByte(type) * 8;
        const u64 mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
        for (u64 left : {u64{0}, u64{1}, mask, mask >> 1, mask ^ (mask >> 1)})
        for (u64 right : {u64{0}, u64{1}, mask, mask >> 1, mask ^ (mask >> 1)})
        for (u32 initial_carry = 0; initial_carry < 2; ++initial_carry) {
            StateStorage actual, control, reference;
            for (auto* storage : {&actual, &control, &reference}) {
                storage->Put(0, left); storage->Put(8, right);
                storage->state->host_cpu_flags = (u64{1} << 50) | (u64(initial_carry) << 29) | 0x83;
            }
            backend::interp::Interpreter interpreter{*reference.state, block.get()};
            const auto halt = interpreter.Run();
            Check(cached.fn(actual.state) == halt && uncached.fn(control.state) == halt, "nonadjacent carry producer halt");
            Check(actual.Get(16) == reference.Get(16) && actual.Get(24) == reference.Get(24) &&
                  control.Get(16) == reference.Get(16) && control.Get(24) == reference.Get(24),
                  "SaveFlags recomputes carry after an intervening architectural carry update");
            if (op == ir::OpCode::Adc && left == 0 && right == 0 && initial_carry == 0)
                Check(((actual.Get(24) >> 29) & 1) == 0, "ADC zero operands do not manufacture carry from a stale result");
        }
    }
    for (u32 condition = 0; condition < 16; ++condition) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x2000})};
        ir::Assembler as{block.get()};
        auto output = as.CondSet(ir::Cond(condition)).SetType(T::U8);
        as.StoreUniform(ir::Uniform{0, T::U8}, output);
        block->SetTerminal(ir::terminal::Condition{ir::Cond(condition),
            ir::terminal::LinkBlock{ir::Location{0x2010}}, ir::terminal::LinkBlock{ir::Location{0x2020}}});
        Compiled cached{block.get()}, uncached{block.get(), false};
        Check(cached.translator.Stats().helpers == 0, "conditions lower without interpreter helpers");
        for (u32 flags = 0; flags < 16; ++flags) {
            const bool n = (flags & 8) != 0, z = (flags & 4) != 0;
            const bool c = (flags & 2) != 0, v = (flags & 1) != 0;
            const std::array<bool, 16> wanted{z, !z, c, !c, n, !n, v, !v,
                c && !z, !c || z, n == v, n != v, !z && n == v, z || n != v, true, true};
            StateStorage a, b;
            a.state->host_cpu_flags = b.state->host_cpu_flags = (u64(flags) << 28) | (u64{1} << 57) | 0xa5;
            Check(cached.fn(a.state) == HaltReason::None && uncached.fn(b.state) == HaltReason::None,
                  "condition link halt");
            Check(a.Get(0) == wanted[condition] && b.Get(0) == wanted[condition], "independent all-condition NZCV truth table");
            Check(a.state->current_loc.Value() == (wanted[condition] ? 0x2010 : 0x2020) &&
                  b.state->current_loc == a.state->current_loc, "condition terminal selects the correct successor");
        }
    }
    u64 seed = 0x31e60915c5a8df24ULL;
    for (u32 subset = 0; subset < 64; ++subset) {
        const auto mask = static_cast<ir::Flags>((subset & 31) | ((subset & 32) << 1));
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x2100})};
        ir::Assembler as{block.get()};
        auto yes = as.TestFlags(mask), no = as.TestNotFlags(mask);
        as.StoreUniform(ir::Uniform{0, T::U8}, yes);
        as.StoreUniform(ir::Uniform{8, T::U8}, no);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled cached{block.get()}, uncached{block.get(), false};
        Check(cached.translator.Stats().helpers == 0 && uncached.translator.Stats().helpers == 0,
              "all virtual flag predicates are native");
        for (u32 pattern = 0; pattern < 64; ++pattern) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            const u64 flags = seed;
            bool first = true, wanted = false;
            u64 nzcv{};
            if (subset & 1) nzcv |= u64{1} << 29;
            if (subset & 2) nzcv |= u64{1} << 28;
            if (subset & 4) nzcv |= u64{1} << 30;
            if (subset & 8) nzcv |= u64{1} << 31;
            if (nzcv) { wanted = (flags & nzcv) != 0; first = false; }
            if (subset & 16) {
                const bool parity = std::popcount(u8(flags)) % 2 == 0;
                wanted = first ? parity : wanted && parity; first = false;
            }
            if (subset & 32) {
                const bool af = ((flags >> 26) & 1) != 0;
                wanted = first ? af : wanted && af;
            }
            StateStorage a, b;
            a.state->host_cpu_flags = b.state->host_cpu_flags = flags;
            Check(cached.fn(a.state) == HaltReason::CallHost && uncached.fn(b.state) == HaltReason::CallHost,
                  "flag predicate halt");
            Check(a.Get(0) == wanted && b.Get(0) == wanted && a.Get(8) == !wanted && b.Get(8) == !wanted,
                  "independent complete virtual-flag-mask predicates");
            Check(a.state->host_cpu_flags == flags && b.state->host_cpu_flags == flags,
                  "read-only predicates preserve architectural flag bits");
        }
        for (bool compact : {false, true}) {
            IntrusivePtr<ir::Block> updates{new ir::Block(ir::Location{0x2200})};
            ir::Assembler update{updates.get()};
            auto left = update.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
            auto right = update.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
            auto sum = update.Adc(left, ir::Operand{right}).SetType(T::U64);
            update.SaveFlags(sum, mask);
            auto saved = update.GetFlags(sum, mask).SetType(T::U64);
            update.StoreUniform(ir::Uniform{16, T::U64}, saved);
            update.SetCarry(left); update.SetOverflow(right); update.InvertCarry();
            update.ClearFlags(mask);
            auto cleared = update.GetFlags(sum, ir::Flags::All).SetType(T::U64);
            update.StoreUniform(ir::Uniform{24, T::U64}, cleared);
            update.PublishFCmpFlags(left, ir::Imm{u64(compact)});
            auto published = update.GetFlags(sum, ir::Flags::All).SetType(T::U64);
            update.StoreUniform(ir::Uniform{32, T::U64}, published);
            update.PublishSse42StrFlags(right, mask);
            auto string_flags = update.GetFlags(sum, ir::Flags::All).SetType(T::U64);
            update.StoreUniform(ir::Uniform{40, T::U64}, string_flags);
            update.BranchOnlyFlags(sum, mask);
            updates->SetTerminal(ir::terminal::ReturnToHost{});
            Compiled native{updates.get()}, control{updates.get(), false};
            Check(native.translator.Stats().helpers == 0, "mixed partial flag publication is native");
            for (u32 i = 0; i < 16; ++i) {
                seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                StateStorage a, b, reference;
                for (auto* storage : {&a, &b, &reference}) {
                    storage->Put(0, i % 4 == 0 ? UINT64_MAX : seed);
                    storage->Put(8, i % 4 == 1 ? UINT64_MAX : std::rotl(seed, 13));
                    storage->state->host_cpu_flags = std::rotl(seed, 31);
                }
                backend::interp::Interpreter interpreter{*reference.state, updates.get()};
                const auto halt = interpreter.Run();
                Check(native.fn(a.state) == halt && control.fn(b.state) == halt, "flag publication halt");
                Check(std::memcmp(a.state->uniform_buffer_begin, reference.state->uniform_buffer_begin, 48) == 0 &&
                      std::memcmp(b.state->uniform_buffer_begin, reference.state->uniform_buffer_begin, 48) == 0,
                      "partial publication snapshots match canonical semantics");
                Check(a.state->host_cpu_flags == reference.state->host_cpu_flags &&
                      b.state->host_cpu_flags == reference.state->host_cpu_flags,
                      "partial flag updates preserve unrelated bits");
            }
        }
    }
    std::cout << "PASS all 16 conditions, 64 flag masks and mixed partial publications\n";
}

}  // namespace swift::tests::loongarch64
