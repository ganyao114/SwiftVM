#include "test_support.h"

#include <bit>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/ir_assembler.h"

namespace swift::tests::riscv {

using T = ir::ValueType;

void NativeFlags() {
    u64 seed = 0x31e60915c5a8df24ULL;
    for (u32 subset = 0; subset < 64; ++subset) {
        const auto mask = static_cast<ir::Flags>((subset & 31) | ((subset & 32) << 1));
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xa300})};
        ir::Assembler as{block.get()};
        auto yes = as.TestFlags(mask), no = as.TestNotFlags(mask);
        as.StoreUniform(ir::Uniform{0, T::U8}, yes);
        as.StoreUniform(ir::Uniform{8, T::U8}, no);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled cached{block.get()}, uncached{block.get(), false};
        Check(cached.translator.Stats().helpers == 0 && uncached.translator.Stats().helpers == 0,
              "all combinations of virtual flag predicates are native");
        for (u32 pattern = 0; pattern < 128; ++pattern) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            const auto flags = seed;
            bool first = true, wanted = false;
            u64 nzcv{};
            if (subset & 1) nzcv |= u64{1} << 29;
            if (subset & 2) nzcv |= u64{1} << 28;
            if (subset & 4) nzcv |= u64{1} << 30;
            if (subset & 8) nzcv |= u64{1} << 31;
            if (nzcv) { wanted = (flags & nzcv) != 0; first = false; }
            if (subset & 16) {
                const bool parity = std::popcount(u8(flags)) % 2 == 0;
                wanted = first ? parity : wanted && parity;
                first = false;
            }
            if (subset & 32) {
                const bool af = ((flags >> 26) & 1) != 0;
                wanted = first ? af : wanted && af;
            }
            StateStorage a, b;
            a.state->host_cpu_flags = b.state->host_cpu_flags = flags;
            Check(cached.fn(a.state) == HaltReason::CallHost && uncached.fn(b.state) == HaltReason::CallHost,
                  "native flag predicate halt");
            Check(a.Get(0) == wanted && b.Get(0) == wanted && a.Get(8) == !wanted && b.Get(8) == !wanted,
                  "independent all-mask flag predicate expected values");
            Check(a.state->host_cpu_flags == flags && b.state->host_cpu_flags == flags,
                  "flag predicates preserve all architectural bits");
        }
        for (bool compact : {false, true}) {
            IntrusivePtr<ir::Block> updates{new ir::Block(ir::Location{0xa400})};
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
            // Branch-only publication is a canonical semantic equivalent;
            // future liveness optimization may omit its architectural store.
            update.BranchOnlyFlags(sum, mask);
            updates->SetTerminal(ir::terminal::ReturnToHost{});
            Compiled native{updates.get()}, control{updates.get(), false};
            Check(native.translator.Stats().helpers == 0, "all flag updates and publications are native");
            for (u32 i = 0; i < 32; ++i) {
                seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                StateStorage a, b, reference;
                for (auto* storage : {&a, &b, &reference}) {
                    storage->Put(0, i % 4 == 0 ? UINT64_MAX : seed);
                    storage->Put(8, i % 4 == 1 ? UINT64_MAX : std::rotl(seed, 13));
                    storage->state->host_cpu_flags = std::rotl(seed, 31);
                }
                backend::interp::Interpreter interpreter{*reference.state, updates.get()};
                const auto halt = interpreter.Run();
                Check(native.fn(a.state) == halt && control.fn(b.state) == halt, "native flag updates halt");
                Check(std::memcmp(a.state->uniform_buffer_begin, reference.state->uniform_buffer_begin, 48) == 0 &&
                      std::memcmp(b.state->uniform_buffer_begin, reference.state->uniform_buffer_begin, 48) == 0,
                      "partial flag publication snapshots match canonical semantics");
                Check(a.state->host_cpu_flags == reference.state->host_cpu_flags &&
                      b.state->host_cpu_flags == reference.state->host_cpu_flags,
                      "flags preserve unrelated bits through mixed update sequences");
            }
        }
    }
    // A conditional edge skips an update; its join must see the flags from
    // the path actually taken, even though compilation visits both paths.
    IntrusivePtr<ir::Block> joined{new ir::Block(ir::Location{0xa500})};
    ir::Assembler join{joined.get()};
    auto condition = join.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8);
    auto zero = join.Zero().SetType(T::U8);
    join.SaveFlags(zero, ir::Flags::Zero | ir::Flags::Parity);
    auto edge = join.Goto(ir::BOOL{condition});
    join.InvertCarry();
    join.BindLabel(edge);
    auto snapshot = join.GetFlags(zero, ir::Flags::All).SetType(T::U64);
    join.StoreUniform(ir::Uniform{8, T::U64}, snapshot);
    joined->SetTerminal(ir::terminal::Condition{ir::Cond::EQ,
            ir::terminal::LinkBlock{ir::Location{0xa501}},
            ir::terminal::LinkBlock{ir::Location{0xa502}}});
    Compiled cached_join{joined.get()}, uncached_join{joined.get(), false};
    for (u32 taken = 0; taken < 2; ++taken) {
        StateStorage a, b;
        const auto initial = (u64{1} << 53) | 0x83;
        a.Put(0, taken); b.Put(0, taken);
        a.state->host_cpu_flags = b.state->host_cpu_flags = initial;
        const auto wanted = (initial & ~u64{255}) | (u64{1} << 30) | (taken ? 0 : (u64{1} << 29));
        Check(cached_join.fn(a.state) == HaltReason::None && uncached_join.fn(b.state) == HaltReason::None,
              "cached flags at a conditional control-flow join");
        Check(a.Get(8) == wanted && b.Get(8) == wanted && a.state->host_cpu_flags == wanted &&
              b.state->host_cpu_flags == wanted && a.state->current_loc.Value() == 0xa501 &&
              b.state->current_loc.Value() == 0xa501, "join snapshots and terminal use the selected flags path");
    }
    std::cout << "PASS native flags: all 64 masks, cached/uncached paths and publications\n";
}

}  // namespace swift::tests::riscv
