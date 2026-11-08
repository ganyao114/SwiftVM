#include "base/error.h"
#include "tests/support/arm64_fixture.h"

#if defined(__aarch64__) && defined(__ANDROID__)
#include "runtime/execution/dispatch_cache.h"
#endif

namespace {
using namespace swift::tests::arm64_fixture;

void PCTailPredicates() {
    const auto source = [](OpCode opcode, u64 argument, unsigned other_use = 0) {
        Builder builder{Scope::Block};
        Need(builder.BeginBlock(Create(builder)));
        const auto one = other_use == 1 ? Constant(builder, 1) : Value{};
        const auto condition = Need(
                builder.Append(opcode,
                               Type::Bool,
                               {opcode == OpCode::CondSet
                                        ? Operand::Condition(static_cast<std::uint32_t>(argument))
                                        : Operand::Flags(argument)}));
        if (other_use == 1)
            Need(builder.Emit<OpCode::SetCarry>(Type::Void, {Operand::ValueOf(one)}));
        if (other_use == 2)
            (void)Need(builder.Emit<OpCode::TestZero>(Type::Bool, {Operand::ValueOf(condition)}));
        Need(builder.Emit<OpCode::StoreUniformSelectImm>(Type::Void,
                                                         {Operand::Uniform(128, Type::U64),
                                                          Operand::ValueOf(condition),
                                                          Operand::Immediate(0x2000, Type::U64),
                                                          Operand::Immediate(0x3000, Type::U64)}));
        Need(builder.EndBlock(Terminal::Dispatch()));
        return std::pair{Submit(builder), condition.node};
    };
    arm64::CompileOptions options;
    options.flags_offset = 136;
    options.pc_offset = 128;
    options.dispatcher = 0x1111222233334444ULL;
    options.exit_handler = 0xaaaabbbbccccddddULL;
    options.native_nzcv = true;
    for (const auto condition : {Condition::EQ, Condition::AL, Condition::NV}) {
        auto [program, predicate] = source(OpCode::CondSet, static_cast<u64>(condition));
        const auto allocation = arm64::AllocateBlock(program, &options);
        const auto tail = program.Operations().size() - 1;
        Check(bool(allocation) && allocation.homes[predicate.value].terminal_only &&
                      allocation.homes[predicate.value].reg == 0 &&
                      !allocation.homes[predicate.value].spilled &&
                      allocation.homes[predicate.value].last_use ==
                              allocation.homes[tail].definition_position,
              "sole PC-tail predicate retains its actual use without a scalar home");
        const auto code = arm64::Compile(program, options);
        Check(bool(code) && code.links.size() == (condition == Condition::EQ ? 2u : 1u) &&
                      code.links.front().target_pc == 0x2000,
              "PC-tail uses two real edges; AL and NV both select the true edge");
        Check(std::none_of(code.words.begin(),
                           code.words.end(),
                           [](std::uint32_t word) {
                               return (word & 0xffff0fe0u) == 0x9a9f07e0u ||
                                      (word & 0xff00001fu) == 0xb4000010u;
                           }),
              "direct PC predicate has no materialized CSET or copied boolean CBZ");
        const auto ordinary = arm64::AllocateBlock(program);
        Check(bool(ordinary) && !ordinary.homes[predicate.value].terminal_only,
              "ordinary PC stores do not borrow a linked-only predicate");
    }
    for (const auto mask : {FlagBits(Flag::Carry | Flag::Zero),
                            FlagBits(Flag::AuxiliaryCarry),
                            FlagBits(Flag::Parity)}) {
        auto [program, predicate] = source(OpCode::TestFlags, mask);
        const auto allocation = arm64::AllocateBlock(program, &options);
        const auto code = arm64::Compile(program, options);
        Check(bool(allocation) && bool(code) &&
                      allocation.homes[predicate.value].terminal_only ==
                              (mask != FlagBits(Flag::Parity)),
              "native C-or-Z and AF use direct edges; PF retains its numeric reader");
    }
    for (unsigned other_use : {1u, 2u}) {
        auto [program, predicate] =
                source(OpCode::CondSet, static_cast<u64>(Condition::CS), other_use);
        const auto allocation = arm64::AllocateBlock(program, &options);
        Check(bool(allocation) && !allocation.homes[predicate.value].terminal_only &&
                      bool(arm64::Compile(program, options)),
              "intervening flag writers and an extra pure use keep the original predicate "
              "snapshot");
    }
}

void DeferredPCLinks() {
    arm64::CompileOptions options;
    options.flags_offset = 136;
    options.pc_offset = 128;
    options.pinned_uniform_offset = 0;
    options.dispatcher = 0x1111222233334444ULL;
    options.exit_handler = 0xaaaabbbbccccddddULL;
    options.private_uniforms = options.fault_locations = options.deferred_pc = true;
    options.entry_pc = 0x1000;
    const auto source = [](bool another_use, bool observe_pc, bool odd_prefix = false) {
        Builder builder{Scope::Block};
        Need(builder.BeginBlock(Create(builder)));
        if (odd_prefix) (void)Constant(builder, 0x55);
        if (observe_pc) Store(builder, 8, Load(builder, 128));
        const auto target = Constant(builder, 0x2000);
        if (another_use) Store(builder, 0, target);
        Store(builder, 128, target);
        Need(builder.EndBlock(Terminal::Dispatch()));
        return std::pair{Submit(builder), target.node};
    };
    auto [program, target] = source(false, false);
    const auto allocation = arm64::AllocateBlock(program, &options);
    const auto code = arm64::Compile(program, options);
    Check(bool(allocation) && !allocation.publish_entry_pc &&
                  allocation.homes[target.value].terminal_only && bool(code) &&
                  code.words.front() == 0x58000050u && code.links.size() == 1,
          "pure static edge has no hot PC literal or state publication");
    Check(sizeof(arm64::LinkSite) == 24 && WordOffset(code.links.front().branch_offset) == 0 &&
                  WordOffset(code.links.front().data_offset) == 2 &&
                  WordOffset(code.links.front().data_offset) + 2 < code.words.size(),
          "cold PC stub follows the original aligned DATA without a second relocation");
    auto [odd_program, odd_target] = source(false, false, true);
    (void)odd_target;
    const auto odd = arm64::Compile(odd_program, options);
    Check(bool(odd) && odd.links.size() == 1 && WordOffset(odd.links.front().branch_offset) == 1 &&
                  WordOffset(odd.links.front().data_offset) == 4 && odd.words[1] == 0x58000070u &&
                  odd.words[2] == 0xd61f0200u && odd.words[3] == 0xd503201fu,
          "odd tail jumps directly through LDR/BR while padding remains unreachable before aligned "
          "DATA");
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        namespace execution = swift::runtime::execution;
        execution::CodeHeap arena{TestBackendOps(),
                                  static_cast<std::size_t>(sysconf(_SC_PAGESIZE))};
        for (const auto* layout : {&code, &odd}) {
            const auto published = arena.Publish(layout->words, {}, layout->links);
            const auto& site = layout->links.front();
            const auto back_words = WordOffset(site.data_offset) - WordOffset(site.branch_offset);
            const auto entry = reinterpret_cast<std::uintptr_t>(published.entry);

            struct Ready {
                u64 pc;
                std::uintptr_t entry;
                unsigned calls{};
            } ready{site.target_pc, entry};

            const auto lookup =
                    [](void* opaque, u64 pc, std::uintptr_t) noexcept -> std::uintptr_t {
                auto& target = *static_cast<Ready*>(opaque);
                ++target.calls;
                return pc == target.pc ? target.entry : 0;
            };
            const auto prelinked = arena.Publish(layout->words, {}, layout->links, lookup, &ready);
            const auto linked_entry = reinterpret_cast<std::uintptr_t>(prelinked.entry);
            const auto linked_delta = static_cast<std::int64_t>(
                    entry - (linked_entry + WordOffset(site.branch_offset) * 4));
            const auto* linked_words = reinterpret_cast<const std::uint32_t*>(prelinked.entry);
            Check(linked_words[WordOffset(site.branch_offset)] ==
                                  (prelinked.writable ? 0x14000000u | (static_cast<std::uint32_t>(
                                                                               linked_delta / 4) &
                                                                       0x03ffffffu)
                                                      : 0x58000010u | (back_words << 5)) &&
                          Read64(linked_words + WordOffset(site.data_offset)) ==
                                  (prelinked.writable
                                           ? entry
                                           : linked_entry + (WordOffset(site.data_offset) + 2) * 4),
                  "ready links publish patched with an unlinkable alias; single mappings retain "
                  "their "
                  "cold owner");
            ready.entry = entry + (std::uintptr_t{1} << 28);
            const auto prelinked_far =
                    arena.Publish(layout->words, {}, layout->links, lookup, &ready);
            const auto* far_words = reinterpret_cast<const std::uint32_t*>(prelinked_far.entry);
            Check(ready.calls == (prelinked.writable ? 2u : 0u) &&
                          far_words[WordOffset(site.branch_offset)] ==
                                  (0x58000010u | (back_words << 5)) &&
                          Read64(far_words + WordOffset(site.data_offset)) ==
                                  (prelinked_far.writable
                                           ? ready.entry
                                           : reinterpret_cast<std::uintptr_t>(prelinked_far.entry) +
                                                     (WordOffset(site.data_offset) + 2) * 4),
                  "far ready links keep LDR/BR; a single mapping never creates a link it cannot "
                  "later "
                  "unlink");
            if (!published.writable) continue;
            auto* write = reinterpret_cast<std::uintptr_t*>(published.writable +
                                                            WordOffset(site.data_offset) * 4);
            const auto* execute = reinterpret_cast<const std::uintptr_t*>(
                    entry + WordOffset(site.data_offset) * 4);
            const auto* branch =
                    reinterpret_cast<const std::uint32_t*>(entry) + WordOffset(site.branch_offset);
            const auto load = 0x58000010u | (back_words << 5);
            const auto near = 0x14000000u | ((0u - WordOffset(site.branch_offset)) & 0x03ffffffu);
            PatchLinkWords(write, execute, entry, back_words);
            Check(*branch == near && Read64(execute) == entry,
                  "even and odd links patch B from their actual branch instruction");
            const auto far = entry + (std::uintptr_t{1} << 28);
            PatchLinkWords(write, execute, far, back_words);
            Check(*branch == load && branch[1] == 0xd61f0200u && Read64(execute) == far,
                  "far relink restores the explicit DATA displacement without executing alignment "
                  "padding");
            PatchLinkWords(write, execute, entry, back_words);
            UnlinkWords(write, execute, back_words);
            Check(*branch == load &&
                          Read64(execute) == reinterpret_cast<std::uintptr_t>(execute + 1),
                  "even and odd unlink restore the original load and this edge's RX cold stub");
        }
        static unsigned patch_calls{}, unlink_calls{};
        patch_calls = unlink_calls = 0;
        auto counted_backend = TestBackendOps();
        counted_backend.patch_link = [](std::uintptr_t* write,
                                        const std::uintptr_t* execute,
                                        std::uintptr_t target,
                                        std::uint32_t back) noexcept {
            ++patch_calls;
            arm64::PatchNativeLink(write, execute, target, back);
        };
        counted_backend.unlink = [](std::uintptr_t* write,
                                    const std::uintptr_t* execute,
                                    std::uint32_t back) noexcept {
            ++unlink_calls;
            arm64::UnlinkNativeLink(write, execute, back);
        };
        execution::CodeCache self_cache{nullptr, counted_backend};
        const auto& self_site = code.links.front();
        const auto incoming = arena.Publish(code.words, {}, code.links);
        const std::array canonical_alias{swift::runtime::backend::EntryPoint{options.entry_pc, 4}};
        self_cache.InsertReady(options.entry_pc, incoming, canonical_alias);
        Check(self_cache.Entries() == 1 && self_cache.Find(options.entry_pc) == incoming.entry,
              "a repeated canonical PC retains the original base rather than its alias offset");
        const std::array invalid_alias{swift::runtime::backend::EntryPoint{
                options.entry_pc, static_cast<std::uint32_t>(incoming.bytes)}};
        bool invalid_entry{};
        try {
            self_cache.InsertReady(options.entry_pc, incoming, invalid_alias);
        } catch (const swift::RuntimeError& error) {
            invalid_entry = error.Kind() == swift::ErrorKind::Runtime;
        }
        Check(invalid_entry && self_cache.Find(options.entry_pc) == incoming.entry,
              "a repeated canonical PC still rejects an alias outside its owning code range");
        self_cache.AddStaticLink(incoming, self_site);

        struct SelfLookup {
            execution::CodeCache& cache;
            u64 pc;
            unsigned calls{};
        } self_lookup{self_cache, self_site.target_pc};

        const auto resolve =
                [](void* opaque, u64 pc, std::uintptr_t unit) noexcept -> std::uintptr_t {
            auto& lookup = *static_cast<SelfLookup*>(opaque);
            ++lookup.calls;
            if (const auto ready = lookup.cache.Find(pc))
                return reinterpret_cast<std::uintptr_t>(ready);
            return pc == lookup.pc ? unit : 0;
        };
        const auto self = arena.Publish(code.words, {}, code.links, resolve, &self_lookup);
        self_cache.AddStaticLink(self, self_site);
        Check(self_lookup.calls == (self.writable ? 1u : 0u) &&
                      self_cache.Pending() == (self.writable ? 2u : 0u),
              "an unpublished self target uses its actual RX base while both relocation rows "
              "remain "
              "pending until ready registration");
        self_cache.InsertReady(self_site.target_pc, self, {});
        Check(patch_calls == (self.writable ? 1u : 0u) &&
                      self_cache.Patched() == (self.writable ? 2u : 0u) &&
                      self_cache.Pending() == 0,
              "ready registration patches the old incoming edge once without resynchronizing the "
              "already prepared self edge");

        std::array<execution::NativeCode, 2> ready_owners;
        for (auto& owner : ready_owners) {
            owner = arena.Publish(code.words, {}, code.links, resolve, &self_lookup);
            const auto* words = reinterpret_cast<const std::uint32_t*>(owner.entry);
            Check(!owner.writable || Read64(words + WordOffset(self_site.data_offset)) ==
                                             reinterpret_cast<std::uintptr_t>(self.entry),
                  "each committed owner contains the actual already-ready target");
            self_cache.AddStaticLink(owner, self_site, self.entry);
        }
        Check(patch_calls == (self.writable ? 1u : 0u) && self_cache.Pending() == 0 &&
                      self_cache.LinkRows() == (self.writable ? 4u : 0u) &&
                      self_cache.Patched() == (self.writable ? 4u : 0u),
              "prepared ready targets retain each owner's unlink row without a second patch");

        auto invalid_links = code.links;
        invalid_links.push_back(self_site);
        invalid_links.back().data_offset = UINT32_MAX;
        bool rejected{};
        try {
            (void)arena.Publish(code.words, {}, invalid_links, resolve, &self_lookup);
        } catch (const swift::RuntimeError& error) {
            rejected = error.Kind() == swift::ErrorKind::InvalidArgument;
        }
        Check(rejected && self_cache.Pending() == 0 &&
                      self_cache.LinkRows() == (self.writable ? 4u : 0u),
              "a rejected unpublished slice leaves no cache relocation for a later reuse");

        self_cache.ResetLinks();
        if (self.writable) {
            const auto base = reinterpret_cast<std::uintptr_t>(self.entry);
            const auto* words = reinterpret_cast<const std::uint32_t*>(self.entry);
            Check(unlink_calls == 4 &&
                          Read64(words + WordOffset(self_site.data_offset)) ==
                                  base + self_site.fallback_offset &&
                          words[WordOffset(self_site.branch_offset)] ==
                                  (0x58000010u | ((WordOffset(self_site.data_offset) -
                                                   WordOffset(self_site.branch_offset))
                                                  << 5)),
                  "the prelinked self edge keeps its real relocation and restores its cold gate on "
                  "unlink");
            for (const auto& owner : ready_owners) {
                const auto base = reinterpret_cast<std::uintptr_t>(owner.entry);
                const auto* words = reinterpret_cast<const std::uint32_t*>(owner.entry);
                Check(Read64(words + WordOffset(self_site.data_offset)) ==
                              base + self_site.fallback_offset,
                      "revocation restores the cold gate of every prepared ready owner");
            }
        }
    }
#endif
    auto [shared, shared_target] = source(true, false);
    const auto shared_home = arm64::AllocateBlock(shared, &options);
    Check(bool(shared_home) && !shared_home.homes[shared_target.value].terminal_only &&
                  bool(arm64::Compile(shared, options)),
          "a real data use retains the PC literal's normal scalar home");
    auto [observed, observed_target] = source(false, true);
    (void)observed_target;
    const auto observed_home = arm64::AllocateBlock(observed, &options);
    const auto observed_code = arm64::Compile(observed, options);
    Check(bool(observed_home) && observed_home.publish_entry_pc && bool(observed_code) &&
                  observed_code.words.front() != 0x58000050u,
          "an actual PC Uniform reader receives the explicit request PC at entry");
    options.deferred_pc = false;
    const auto eager = arm64::AllocateBlock(program, &options);
    Check(bool(eager) && !eager.homes[target.value].terminal_only &&
                  bool(arm64::Compile(program, options)),
          "generic linked ABI retains eager PC publication and literal materialization");
}

void CompleteAdditionalEntries() {
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        namespace execution = swift::runtime::execution;

        struct State {
            execution::NativeRunContext native;
            std::array<u64, 20> inputs;
            u64 seen_pc{};
        };

        constexpr auto state_offset = offsetof(execution::NativeRunContext, state);

        constexpr u64 first_pc = 0x4100, other_pc = 0x4200, flags = 0xa5a50000bc000085ULL;
        for (const bool native_nzcv : {false, true}) {
            if (native_nzcv && !(getauxval(AT_HWCAP) & HWCAP_FLAGM)) continue;
            Builder builder{Scope::Function};
            const auto first = Create(builder), other = Create(builder);
            Need(builder.BeginBlock(first));
            Store(builder,
                  offsetof(swift::runtime::x86::GuestState, gprs) + swift::runtime::x86::RDX * 8,
                  Constant(builder, 123));
            Need(builder.EndBlock(Terminal::Jump(other)));
            Need(builder.BeginBlock(other));
            Need(builder.Emit<OpCode::EntryPoint>(Type::Void,
                                                  {Operand::Immediate(other_pc, Type::U64)}));
            std::array<Value, 20> values;
            for (unsigned i = 0; i < values.size(); ++i)
                values[i] = Load(builder, offsetof(State, inputs) - state_offset + i * 8);
            auto sum = values.front();
            for (unsigned i = 1; i < values.size(); ++i)
                sum = Need(builder.Emit<OpCode::Add>(
                        Type::U64, {Operand::ValueOf(sum), Operand::ValueOf(values[i])}));
            Store(builder,
                  offsetof(swift::runtime::x86::GuestState, gprs) + swift::runtime::x86::R8 * 8,
                  sum);
            Store(builder,
                  offsetof(State, seen_pc) - state_offset,
                  Load(builder, offsetof(swift::runtime::x86::GuestState, pc)));
            Need(builder.EndBlock(Terminal::Return(Constant(builder, 42))));
            auto program = Submit(builder);
            arm64::CompileOptions options{offsetof(swift::runtime::x86::GuestState, packed_flags)};
            options.dispatcher =
                    reinterpret_cast<std::uintptr_t>(native_nzcv ? &execution::SvmNativeDispatchNZCV
                                                                 : &execution::SvmNativeDispatch);
            options.exit_handler = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeExit);
            options.pc_offset = offsetof(swift::runtime::x86::GuestState, pc);
            options.pinned_uniform_offset = offsetof(swift::runtime::x86::GuestState, gprs);
            options.pinned_uniform_slots = 16;
            options.private_uniforms = options.fault_locations = options.deferred_pc = true;
            options.native_nzcv = native_nzcv;
            options.entry_pc = first_pc;
            const auto compiled = arm64::Compile(program, options);
            Check(bool(compiled) && compiled.entries.size() == 1 &&
                          compiled.entries[0].pc == other_pc && compiled.frame_bytes > 0,
                  "additional entry owns the same actual nonzero frame as its original unit");
            execution::CodeHeap arena{TestBackendOps(),
                                      static_cast<std::size_t>(sysconf(_SC_PAGESIZE))};
            const auto unit = arena.Publish(compiled.words, compiled.faults, compiled.links);
            const auto alternate = reinterpret_cast<execution::NativeCode::Function>(
                    reinterpret_cast<std::uintptr_t>(unit.entry) +
                    std::uintptr_t{WordOffset(compiled.entries[0].offset)} * 4);

            struct Resolver {
                execution::NativeCode::Function entry;
                unsigned calls{};
            } resolver{};

            State state;
            std::atomic<const execution::NativeCode*> active{};
            auto& context = state.native;
            auto& cpu = context.state;
            context.opaque = &resolver;
            context.resolve = [](void* p, execution::NativeRunContext*) noexcept -> std::uintptr_t {
                auto& value = *static_cast<Resolver*>(p);
                ++value.calls;
                return reinterpret_cast<std::uintptr_t>(value.entry);
            };
            context.active = &active;
            context.anchor = &unit;
            context.pinned_uniform_offset = options.pinned_uniform_offset;
            context.pinned_uniform_slots = 16;
            context.flags_offset = options.flags_offset;
            context.pc_offset = options.pc_offset;
            context.native_nzcv = native_nzcv;
            for (const bool secondary : {false, true}) {
                cpu = {};
                state.seen_pc = 0;
                cpu.pc = secondary ? other_pc : first_pc;
                cpu.packed_flags = flags;
                for (unsigned i = 0; i < state.inputs.size(); ++i) state.inputs[i] = i + 1;
                resolver.entry = secondary ? alternate : unit.entry;
                resolver.calls = 0;
                Check(execution::SvmRunNative(&context) == 42 && resolver.calls == 1 &&
                              cpu.gprs[swift::runtime::x86::R8] == 210 &&
                              state.seen_pc == (secondary ? other_pc : first_pc) &&
                              cpu.gprs[swift::runtime::x86::RDX] == (secondary ? 0 : 123) &&
                              cpu.packed_flags == flags,
                      "external prologue and internal body edge preserve SP, own PC, values and "
                      "complete flags");
            }
        }
    }
#endif
}

void LinkedDynamicPC() {
    for (const auto scope : {Scope::Block, Scope::Function}) {
        Builder builder{scope};
        Need(builder.BeginBlock(Create(builder)));
        Store(builder, 128, Load(builder, 0));
        Need(builder.EndBlock(Terminal::Dispatch()));
        auto program = Submit(builder);
        arm64::CompileOptions options;
        options.flags_offset = 136;
        options.pc_offset = 128;
        options.dispatcher = 0x1111222233334444ULL;
        options.exit_handler = 0xaaaabbbbccccddddULL;
        options.l2_dispatcher = 0x5555666677778888ULL;
        options.relocatable = true;
        options.dispatch_mode = arm64::DispatchMode::Inline;
        const auto code = arm64::Compile(program, options);
        Check(bool(code) && code.links.empty(),
              "Block and Function dynamic exits create no per-site link or cache resources");
        // LDR L1; UBFIZ index; ADD slot; LDP tag/entry; SUB tag; CBNZ miss; BR entry.
        constexpr std::array<std::uint32_t, 5> lookup{
                0xf941b670u, 0xd37c4c07u, 0x8b070207u, 0xa94044f0u, 0xcb000210u};
        const auto at =
                std::search(code.words.begin(), code.words.end(), lookup.begin(), lookup.end());
        Check(at != code.words.end() && code.words.end() - at >= 7 &&
                      (at[5] & 0xff00001fu) == 0xb5000010u && at[6] == 0xd61f0220u,
              "the ready-only L1 hit is seven flag-preserving instructions with its own BR");
        Check(std::any_of(code.relocations.begin(),
                          code.relocations.end(),
                          [](const auto& r) {
                              return r.symbol == swift::runtime::backend::HostSymbol::DispatcherL2;
                          }),
              "only the shared L2 miss callback owns the dynamic exit relocation");
        options.dispatch_mode = arm64::DispatchMode::Callback;
        options.l2_dispatcher = 0;
        const auto callback = arm64::Compile(program, options);
        Check(bool(callback) &&
                      std::none_of(callback.relocations.begin(),
                                   callback.relocations.end(),
                                   [](const auto& r) {
                                       return r.symbol ==
                                              swift::runtime::backend::HostSymbol::DispatcherL2;
                                   }),
              "ordinary custom callbacks retain their ABI without a thread L1 requirement");
    }
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        namespace execution = swift::runtime::execution;
        constexpr u64 entry_pc = 0x1000, next_pc = 0x2000, initial = 0xa5a50000bc00003cULL;
        for (const bool native_nzcv : {false, true}) {
            if (native_nzcv && !(getauxval(AT_HWCAP) & HWCAP_FLAGM)) continue;
            arm64::CompileOptions options;
            options.flags_offset = offsetof(swift::runtime::x86::GuestState, packed_flags);
            options.pc_offset = offsetof(swift::runtime::x86::GuestState, pc);
            options.pinned_uniform_offset = offsetof(swift::runtime::x86::GuestState, gprs);
            options.pinned_uniform_slots = 16;
            options.private_uniforms = options.fault_locations = options.deferred_pc = true;
            options.native_nzcv = native_nzcv;
            options.entry_pc = entry_pc;
            options.dispatch_mode = arm64::DispatchMode::Inline;
            options.dispatcher =
                    native_nzcv
                            ? reinterpret_cast<std::uintptr_t>(&execution::SvmNativeDispatchNZCV)
                            : reinterpret_cast<std::uintptr_t>(&execution::SvmNativeDispatch);
            options.exit_handler = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeExit);
            options.l2_dispatcher = reinterpret_cast<std::uintptr_t>(
                    native_nzcv ? &execution::SvmNativeL2DispatchNZCV
                                : &execution::SvmNativeL2Dispatch);

            Builder source{Scope::Block};
            Need(source.BeginBlock(Create(source)));
            Store(source,
                  options.pc_offset,
                  Load(source,
                       options.pinned_uniform_offset + swift::runtime::x86::RAX * sizeof(u64)));
            Need(source.EndBlock(Terminal::Dispatch()));
            auto source_program = Submit(source);
            const auto source_allocation = arm64::AllocateBlock(source_program, &options);
            const auto source_code = arm64::Compile(source_program, options);
            Check(bool(source_allocation) && !source_allocation.publish_entry_pc &&
                          bool(source_code) && source_code.links.empty(),
                  "GPR-sourced PC tail remains dynamic without a static target relocation");

            Builder target{Scope::Block};
            Need(target.BeginBlock(Create(target)));
            Store(target,
                  options.pinned_uniform_offset + swift::runtime::x86::RCX * sizeof(u64),
                  Load(target, options.pc_offset));
            Need(target.EndBlock(Terminal::Return(Constant(target, 42))));
            auto target_program = Submit(target);
            options.entry_pc = next_pc;
            const auto target_allocation = arm64::AllocateBlock(target_program, &options);
            const auto target_code = arm64::Compile(target_program, options);
            Check(bool(target_allocation) && target_allocation.publish_entry_pc &&
                          bool(target_code),
                  "dynamic target's actual PC observer requires entry publication");

            execution::CodeHeap arena{TestBackendOps(),
                                      static_cast<std::size_t>(sysconf(_SC_PAGESIZE))};
            const auto entry = arena.Publish(source_code.words, {}, source_code.links);
            const auto destination = arena.Publish(target_code.words, {}, target_code.links);
            std::array<execution::DispatchSlot, 4> slots{};
            const auto home =
                    (next_pc ^ (next_pc >> arm64::abi::DispatchHashShift)) & (slots.size() - 1);
            slots[home].pc = next_pc + 4;
            slots[home].occupied = true;
            auto& ready = slots[(home + 1) & (slots.size() - 1)];

            struct Resolver {
                const execution::NativeCode* first;
                const execution::NativeCode* second;
                execution::NativeCode anchor;
                execution::DispatchSlot* ready;
                unsigned calls{};
                std::array<u64, 2> observed{};
                u64 target_pc{};
            } resolver{&entry, &destination, entry, &ready};

            resolver.target_pc = next_pc;
            std::atomic<const execution::NativeCode*> active{};
            execution::DispatchCache cache;
            cache.Ensure();
            execution::NativeRunContext context;
            context.l1 = cache.Data();
            auto& state = context.state;
            context.opaque = &resolver;
            context.resolve = [](void* opaque,
                                 execution::NativeRunContext* current) noexcept -> std::uintptr_t {
                auto& request = *static_cast<Resolver*>(opaque);
                const auto call = request.calls++;
                const auto pc = current->state.pc;
                if (call < request.observed.size()) request.observed[call] = pc;
                if (call == 0 && pc == entry_pc) {
                    request.anchor = *request.first;
                    return reinterpret_cast<std::uintptr_t>(request.first->entry);
                }
                if (call == 1 && pc == request.target_pc) {
                    request.ready->pc = request.target_pc;
                    request.ready->entry = request.second->entry;
                    request.ready->code_id = 0;
                    request.ready->occupied = true;
                    request.anchor = *request.second;
                    return reinterpret_cast<std::uintptr_t>(request.second->entry);
                }
                return 0;
            };
            context.slots = slots.data();
            context.mask = slots.size() - 1;
            context.active = &active;
            context.anchor = &resolver.anchor;
            context.pinned_uniform_offset = options.pinned_uniform_offset;
            context.pinned_uniform_slots = options.pinned_uniform_slots;
            context.flags_offset = options.flags_offset;
            context.pc_offset = options.pc_offset;
            context.native_nzcv = native_nzcv;
            for (const bool warm : {false, true}) {
                cache.Reset();
                context.slots =
                        warm ? slots.data()
                             : nullptr;  // Public nullable context keeps the original cold guard.
                state = {};
                state.pc = entry_pc;
                state.packed_flags = initial;
                state.gprs[swift::runtime::x86::RAX] = next_pc;
                resolver.calls = 0;
                resolver.observed = {};
                Check(execution::SvmRunNative(&context) == 42,
                      "dynamic Dispatch keeps its PC distinct from the target's Return reason");
                Check(resolver.observed[0] == entry_pc && resolver.calls == (warm ? 1u : 2u) &&
                              (warm || resolver.observed[1] == next_pc),
                      "root resolves its original PC; only a dynamic cold miss resolves the true "
                      "next "
                      "PC");
                Check(state.pc == next_pc && state.gprs[swift::runtime::x86::RCX] == next_pc &&
                              state.packed_flags == initial,
                      "cold and ready dynamic edges preserve whole packed flags and expose the "
                      "target "
                      "PC");
            }
            Builder local{Scope::Block};
            Need(local.BeginBlock(Create(local)));
            const auto count = Load(
                    local, options.pinned_uniform_offset + swift::runtime::x86::RDX * sizeof(u64));
            Store(local, options.pc_offset, Constant(local, next_pc));
            const auto skip =
                    Need(local.Emit<OpCode::NotGoto>(Type::U64, {Operand::ValueOf(count)}));
            const auto address = Load(
                    local, options.pinned_uniform_offset + swift::runtime::x86::R8 * sizeof(u64));
            Need(local.Emit<OpCode::StoreMemoryTSO>(
                    Type::Void,
                    {Operand::ValueOf(address), Operand::ValueOf(Constant(local, 0x55))}));
            Store(local,
                  options.pinned_uniform_offset + swift::runtime::x86::RCX * sizeof(u64),
                  Constant(local, 0x55));
            Store(local, options.pc_offset, Constant(local, 0x3000));
            Need(local.Emit<OpCode::BindLabel>(Type::Void, {Operand::ValueOf(skip)}));
            Store(local,
                  options.pinned_uniform_offset + swift::runtime::x86::R11 * sizeof(u64),
                  skip);
            Need(local.EndBlock(Terminal::Dispatch()));
            auto local_program = Submit(local);
            auto local_options = options;
            local_options.entry_pc = entry_pc;
            const auto local_code = arm64::Compile(local_program, local_options);
            Check(bool(local_code) && local_code.links.empty(),
                  "a forward control join reloads the actual PC rather than using the body's "
                  "constant "
                  "target");
            Builder local_target{Scope::Block};
            Need(local_target.BeginBlock(Create(local_target)));
            Need(local_target.EndBlock(Terminal::Return(Constant(local_target, 42))));
            auto local_target_program = Submit(local_target);
            // This one Return body is shared by two real PCs. Keep the PC
            // already published by cold dispatch instead of assigning both
            // entries the source unit's deferred entry_pc.
            auto local_target_options = local_options;
            local_target_options.deferred_pc = false;
            const auto local_target_code =
                    arm64::Compile(local_target_program, local_target_options);
            Check(bool(local_target_code), "forward-control target uses the original Return ABI");
            const auto local_entry = arena.Publish(local_code.words, {}, local_code.links);
            const auto local_destination =
                    arena.Publish(local_target_code.words, {}, local_target_code.links);
            execution::DispatchSlot local_slot;
            resolver.first = &local_entry;
            resolver.second = &local_destination;
            resolver.ready = &local_slot;
            for (const u64 count_value : {u64{0}, u64{0x100}}) {
                cache.Reset();
                u64 memory = 0x77;
                const auto selected_pc = count_value ? 0x3000 : next_pc;
                state = {};
                state.pc = entry_pc;
                state.packed_flags = initial;
                state.gprs[swift::runtime::x86::RDX] = count_value;
                state.gprs[swift::runtime::x86::RCX] = 0x11;
                state.gprs[swift::runtime::x86::R8] =
                        count_value ? reinterpret_cast<std::uintptr_t>(&memory) : 0;
                resolver.calls = 0;
                resolver.observed = {};
                resolver.target_pc = selected_pc;
                context.slots = nullptr;
                context.mask = 0;
                const auto returned = execution::SvmRunNative(&context);
                const bool okay =
                        returned == 42 && resolver.calls == 2 && resolver.observed[0] == entry_pc &&
                        resolver.observed[1] == selected_pc && state.pc == selected_pc &&
                        state.packed_flags == initial &&
                        state.gprs[swift::runtime::x86::RCX] == (count_value ? 0x55 : 0x11) &&
                        state.gprs[swift::runtime::x86::R11] == count_value &&
                        memory == (count_value ? 0x55 : 0x77);
                if (!okay)
                    std::fprintf(
                            stderr,
                            "forward-local native=%u count=%llx ret=%llx calls=%u "
                            "observed=%llx/%llx "
                            "pc=%llx flags=%llx rcx=%llx r11=%llx memory=%llx\n",
                            static_cast<unsigned>(native_nzcv),
                            static_cast<unsigned long long>(count_value),
                            static_cast<unsigned long long>(returned),
                            resolver.calls,
                            static_cast<unsigned long long>(resolver.observed[0]),
                            static_cast<unsigned long long>(resolver.observed[1]),
                            static_cast<unsigned long long>(state.pc),
                            static_cast<unsigned long long>(state.packed_flags),
                            static_cast<unsigned long long>(state.gprs[swift::runtime::x86::RCX]),
                            static_cast<unsigned long long>(state.gprs[swift::runtime::x86::R11]),
                            static_cast<unsigned long long>(memory));
                Check(okay,
                      "zero skips even a null memory operand; nonzero commits body state and both "
                      "paths retain flags, PC and branch value");
            }
            resolver.first = &entry;
            resolver.second = &destination;
            resolver.ready = &ready;
            resolver.target_pc = next_pc;
            context.slots = slots.data();
            context.mask = slots.size() - 1;
            if (native_nzcv)
                for (const auto scope : {Scope::Block, Scope::Function}) {
                    constexpr u64 other_pc = 0x3000;
                    const auto predicate_source = [&](Condition condition) {
                        Builder builder{scope};
                        Need(builder.BeginBlock(Create(builder)));
                        const auto check = Need(builder.Emit<OpCode::CondSet>(
                                Type::Bool, {Operand::Condition(condition)}));
                        Need(builder.Emit<OpCode::StoreUniformSelectImm>(
                                Type::Void,
                                {Operand::Uniform(options.pc_offset, Type::U64),
                                 Operand::ValueOf(check),
                                 Operand::Immediate(other_pc, Type::U64),
                                 Operand::Immediate(next_pc, Type::U64)}));
                        Need(builder.EndBlock(Terminal::Dispatch()));
                        return Submit(builder);
                    };
                    auto predicate_options = options;
                    predicate_options.entry_pc = entry_pc;
                    auto predicate_program = predicate_source(Condition::EQ);
                    const auto predicate_code =
                            arm64::Compile(predicate_program, predicate_options);
                    Check(bool(predicate_code) && predicate_code.links.size() == 2,
                          "direct PC predicate compiles with two owned edges");
                    const auto primary = std::find_if(
                            predicate_code.links.begin(),
                            predicate_code.links.end(),
                            [](const arm64::LinkSite& link) { return link.target_pc == next_pc; });
                    Check(primary != predicate_code.links.end() &&
                                  (predicate_code.words[WordOffset(primary->branch_offset)] &
                                   0xff00001fu) == 0x54000001u,
                          "two forward targets choose predicate-false as a patchable B.NE head");
                    auto backward_options = predicate_options;
                    backward_options.entry_pc = 0x4000;
                    const auto backward = arm64::Compile(predicate_program, backward_options);
                    Check(bool(backward) && std::any_of(backward.links.begin(),
                                                        backward.links.end(),
                                                        [&](const arm64::LinkSite& link) {
                                                            return link.target_pc == other_pc &&
                                                                   (backward.words[WordOffset(
                                                                            link.branch_offset)] &
                                                                    0xff00001fu) == 0x54000000u;
                                                        }),
                          "a backward yes target has priority over the default no layout");
                    auto flip_program = predicate_source(Condition::HI);
                    const auto flip = arm64::Compile(flip_program, predicate_options);
                    Check(bool(flip) &&
                                  std::count(flip.words.begin(), flip.words.end(), 0xd500401fu) ==
                                          3 &&
                                  std::all_of(
                                          flip.links.begin(),
                                          flip.links.end(),
                                          [&](const arm64::LinkSite& link) {
                                              return (flip.words[WordOffset(link.branch_offset)] &
                                                      0xff00001fu) == 0x58000010u;
                                          }),
                          "temporary carry inversion retains the old per-edge restoration and "
                          "ordinary "
                          "links");

                    auto other_options = options;
                    other_options.entry_pc = other_pc;
                    const auto other_code = arm64::Compile(target_program, other_options);
                    Check(bool(other_code),
                          "second predicate target preserves its actual entry PC observer");
                    const auto other_destination =
                            arena.Publish(other_code.words, {}, other_code.links);
                    auto at = (other_pc ^ (other_pc >> arm64::abi::DispatchHashShift)) &
                              (slots.size() - 1);
                    while (slots[at].occupied && slots[at].pc != other_pc)
                        at = (at + 1) & (slots.size() - 1);
                    slots[at].pc = other_pc;
                    slots[at].entry = other_destination.entry;
                    slots[at].occupied = true;

                    struct ReadyTargets {
                        std::uintptr_t no, yes;
                        unsigned calls{};
                    } targets{reinterpret_cast<std::uintptr_t>(destination.entry),
                              reinterpret_cast<std::uintptr_t>(other_destination.entry)};

                    const auto predicate_entry = arena.Publish(
                            predicate_code.words,
                            {},
                            predicate_code.links,
                            [](void* opaque, u64 pc, std::uintptr_t) noexcept -> std::uintptr_t {
                                auto& ready = *static_cast<ReadyTargets*>(opaque);
                                ++ready.calls;
                                return pc == next_pc ? ready.no : pc == other_pc ? ready.yes : 0;
                            },
                            &targets);
                    Check(targets.calls == (predicate_entry.writable ? 2u : 0u),
                          "only unlinkable conditional edges resolve during unpublished "
                          "initialization");
                    resolver.first = &predicate_entry;
                    const auto run_predicate = [&](bool zero) {
                        cache.Reset();
                        const auto flags =
                                zero ? initial | (u64{1} << 30) : initial & ~(u64{1} << 30);
                        state = {};
                        state.pc = entry_pc;
                        state.packed_flags = flags;
                        resolver.calls = 0;
                        resolver.observed = {};
                        const auto selected_pc = zero ? other_pc : next_pc;
                        Check(execution::SvmRunNative(&context) == 42 && resolver.calls == 1 &&
                                      state.pc == selected_pc &&
                                      state.gprs[swift::runtime::x86::RCX] == selected_pc &&
                                      state.packed_flags == flags,
                              "both predicate outcomes preserve whole flags/reserved and observe "
                              "the "
                              "true "
                              "target PC before Return42");
                    };
                    run_predicate(false);
                    run_predicate(true);
                    if (predicate_entry.writable) {
                        auto* write = reinterpret_cast<std::uintptr_t*>(
                                predicate_entry.writable + WordOffset(primary->data_offset) * 4);
                        const auto* execute = reinterpret_cast<const std::uintptr_t*>(
                                reinterpret_cast<const std::uint8_t*>(predicate_entry.entry) +
                                WordOffset(primary->data_offset) * 4);
                        const auto back = WordOffset(primary->data_offset) -
                                          WordOffset(primary->branch_offset);
                        const auto* words =
                                reinterpret_cast<const std::uint32_t*>(predicate_entry.entry);
                        PatchLinkWords(write,
                                       execute,
                                       reinterpret_cast<std::uintptr_t>(predicate_entry.entry) +
                                               (std::uintptr_t{1} << 21),
                                       back);
                        Check(words[WordOffset(primary->branch_offset)] ==
                                              predicate_code
                                                      .words[WordOffset(primary->branch_offset)] &&
                                      (words[WordOffset(primary->data_offset) - 2] & 0xfc000000u) ==
                                              0x14000000u,
                              "far B.cond target preserves its condition and falls back through "
                              "the "
                              "real "
                              "B26 gate");
                        UnlinkWords(write, execute, back);
                        Check(words[WordOffset(primary->branch_offset)] ==
                                              predicate_code
                                                      .words[WordOffset(primary->branch_offset)] &&
                                      words[WordOffset(primary->data_offset) - 2] == 0x58000050u &&
                                      Read64(execute) ==
                                              reinterpret_cast<std::uintptr_t>(execute + 1),
                              "B.cond unlink restores the owned cold stub without changing its "
                              "predicate");
                        run_predicate(false);
                    }
                }
            // The same scene owns a callable seed. Packed/plain covers the empty
            // PC-zero tag match; Native/counting covers empty mismatch and pending.
            auto callable_options = options;
            callable_options.entry_pc = entry_pc;
            callable_options.dispatch_mode =
                    native_nzcv ? arm64::DispatchMode::InlineCounted : arm64::DispatchMode::Inline;
            if (native_nzcv) {
                callable_options.dispatcher =
                        reinterpret_cast<std::uintptr_t>(&execution::SvmNativeDispatchNZCVCounted);
                callable_options.l2_dispatcher = reinterpret_cast<std::uintptr_t>(
                        &execution::SvmNativeL2DispatchNZCVCounted);
            }
            const auto callable_code = arm64::Compile(source_program, callable_options);
            Check(bool(callable_code) &&
                          (native_nzcv || callable_code.words.size() == source_code.words.size()),
                  "ready-only L1 lookup has one shape independent of callable L2 payloads");
            const u64 callable_pc = native_nzcv ? next_pc : 0;
            auto callable_target_options = callable_options;
            callable_target_options.entry_pc = callable_pc;
            const auto callable_target_code =
                    arm64::Compile(target_program, callable_target_options);
            Check(bool(callable_target_code),
                  "callable cold target preserves its real PC observer");
            const auto callable_entry = arena.Publish(callable_code.words, {}, callable_code.links);
            const auto callable_target =
                    arena.Publish(callable_target_code.words, {}, callable_target_code.links);
            execution::DispatchSlot seed;
            const auto cold = reinterpret_cast<execution::NativeCode::Function>(
                    native_nzcv ? &execution::SvmNativePendingCounted
                                : &execution::SvmNativePending);
            resolver.first = &callable_entry;
            resolver.second = &callable_target;
            resolver.ready = &seed;
            resolver.target_pc = callable_pc;
            context.slots = &seed;
            context.mask = 0;
            const auto run_callable = [&](bool pending) {
                cache.Reset();
                seed = {};
                seed.entry = cold;
                if (pending) {
                    seed.pc = callable_pc;
                    seed.occupied = true;
                }
                state = {};
                state.pc = entry_pc;
                state.packed_flags = initial;
                state.gprs[swift::runtime::x86::RAX] = callable_pc;
                resolver.calls = 0;
                resolver.observed = {};
                context.dispatcher_entries = context.probe_steps = 0;
                context.cache_hits = 37;
                Check(execution::SvmRunNative(&context) == 42 && resolver.calls == 2 &&
                              resolver.observed[0] == entry_pc &&
                              resolver.observed[1] == callable_pc && state.pc == callable_pc &&
                              state.gprs[swift::runtime::x86::RCX] == callable_pc &&
                              state.packed_flags == initial,
                      "callable empty/pending paths commit the actual PC and preserve complete "
                      "flags/reserved");
                Check(context.cache_hits == 37 &&
                              (!native_nzcv ||
                               (context.dispatcher_entries == 1 && context.probe_steps == 1)),
                      "L1 miss probes L2 once and only a matched pending thunk cancels its hit");
            };
            run_callable(false);
            if (native_nzcv) run_callable(true);

            auto cached_options = options;
            cached_options.entry_pc = entry_pc;
            cached_options.dispatch_mode = arm64::DispatchMode::InlineCounted;
            cached_options.dispatcher = reinterpret_cast<std::uintptr_t>(
                    native_nzcv ? &execution::SvmNativeDispatchNZCVCounted
                                : &execution::SvmNativeDispatchCounted);
            cached_options.l2_dispatcher = reinterpret_cast<std::uintptr_t>(
                    native_nzcv ? &execution::SvmNativeL2DispatchNZCVCounted
                                : &execution::SvmNativeL2DispatchCounted);
            const auto cached_code = arm64::Compile(source_program, cached_options);
            Check(bool(cached_code) && cached_code.links.empty(),
                  "dynamic tails borrow thread L1 without native DATA or link resources");
            const auto cached_entry = arena.Publish(cached_code.words, {}, cached_code.links);
            const auto second_entry = arena.Publish(cached_code.words, {}, cached_code.links);
            execution::DispatchSlot cached_slot;
            cached_slot.pc = callable_pc;
            cached_slot.entry = reinterpret_cast<execution::NativeCode::Function>(
                    &execution::SvmNativePendingCounted);
            cached_slot.occupied = true;
            resolver.first = &cached_entry;
            resolver.second = &callable_target;
            resolver.ready = &cached_slot;
            resolver.target_pc = callable_pc;
            const auto run_cached = [&](unsigned calls, u64 probes, u64 hits, u64 target_pc) {
                state = {};
                state.pc = entry_pc;
                state.packed_flags = initial;
                state.gprs[swift::runtime::x86::RAX] = target_pc;
                resolver.calls = 0;
                resolver.observed = {};
                resolver.target_pc = target_pc;
                context.dispatcher_entries = context.probe_steps = context.cache_hits = 0;
                Check(execution::SvmRunNative(&context) == 42 && resolver.calls == calls &&
                              state.pc == target_pc &&
                              state.gprs[swift::runtime::x86::RCX] == target_pc &&
                              state.packed_flags == initial && context.dispatcher_entries == 1 &&
                              context.probe_steps == probes && context.cache_hits == hits,
                      "thread L1 and shared L2 preserve PC/Return/flags and count each transfer "
                      "once");
            };
            cache.Reset();
            Check(cache.Data()[0].pc == UINT64_MAX && !cache.Data()[0].entry,
                  "the empty slot-zero sentinel cannot falsely hit guest PC zero");
            context.slots = &cached_slot;
            context.mask = 0;
            run_cached(2, 1, 0, callable_pc);
            const auto first_index = execution::DispatchCache::Index(callable_pc);
            Check(!cache.Data()[first_index].entry,
                  "a pending L2 payload never enters the ready-only thread L1");
            run_cached(1, 1, 1, callable_pc);
            Check(cache.Data()[first_index].pc == callable_pc &&
                          cache.Data()[first_index].entry == callable_target.entry,
                  "the shared L2 ready match fills the supplied L1 slot");
            context.slots = nullptr;
            run_cached(1, 0, 1, callable_pc);
            resolver.first = &second_entry;
            run_cached(1, 0, 1, callable_pc);
            Check(second_entry.entry != cached_entry.entry,
                  "separate dynamic BR sites reuse the same thread-owned ready cache");
            resolver.first = &cached_entry;

            cache.Reset();
            context.slots = &cached_slot;
            run_cached(1, 1, 1, callable_pc);
            const auto changed_pc = callable_pc + 4;
            auto changed_options = callable_target_options;
            changed_options.entry_pc = changed_pc;
            const auto changed_code = arm64::Compile(target_program, changed_options);
            Check(bool(changed_code), "a different-index target retains its own PC observer");
            const auto changed_target = arena.Publish(changed_code.words, {}, changed_code.links);
            cached_slot.pc = changed_pc;
            cached_slot.entry = changed_target.entry;
            cached_slot.code_id = 1;
            resolver.second = &changed_target;
            run_cached(1, 1, 1, changed_pc);
            const auto second_index = execution::DispatchCache::Index(changed_pc);
            Check(first_index != second_index && cache.Data()[first_index].pc == callable_pc &&
                          cache.Data()[second_index].pc == changed_pc &&
                          cache.Data()[second_index].entry == changed_target.entry,
                  "different L1 indices retain both full-PC ready targets");
            context.slots = nullptr;
            run_cached(1, 0, 1, callable_pc);
            run_cached(1, 0, 1, changed_pc);

            const auto collision_pc = callable_pc + (u64{1} << SVM_DISPATCH_L1_BITS);
            auto collision_options = callable_target_options;
            collision_options.entry_pc = collision_pc;
            const auto collision_code = arm64::Compile(target_program, collision_options);
            Check(bool(collision_code), "a 1MiB alias target retains its own PC observer");
            const auto collision_target =
                    arena.Publish(collision_code.words, {}, collision_code.links);
            context.slots = &cached_slot;
            cached_slot.pc = collision_pc;
            cached_slot.entry = collision_target.entry;
            cached_slot.code_id = 2;
            resolver.second = &collision_target;
            run_cached(1, 1, 1, collision_pc);
            Check(execution::DispatchCache::Index(collision_pc) == first_index &&
                          cache.Data()[first_index].pc == collision_pc &&
                          cache.Data()[first_index].entry == collision_target.entry &&
                          cache.Data()[second_index].pc == changed_pc,
                  "full-PC comparison sends a 1MiB alias through L2 before replacing its slot");
            context.slots = nullptr;
            run_cached(1, 0, 1, collision_pc);
            run_cached(1, 0, 1, changed_pc);
            context.slots = &cached_slot;
            cached_slot.pc = callable_pc;
            cached_slot.entry = callable_target.entry;
            cached_slot.code_id = 0;
            resolver.second = &callable_target;
            run_cached(1, 1, 1, callable_pc);
            context.slots = nullptr;
            run_cached(1, 0, 1, callable_pc);
            cache.ClearKey(callable_pc);
            Check(!cache.Data()[first_index].entry &&
                          cache.Data()[second_index].entry == changed_target.entry,
                  "clearing one key preserves ready targets in other L1 slots");
            context.slots = &cached_slot;
            run_cached(1, 1, 1, callable_pc);
            cache.Reset();
            Check(cache.Data()[0].pc == UINT64_MAX && !cache.Data()[first_index].entry &&
                          !cache.Data()[second_index].entry,
                  "owner reset restores zero pages and the slot-zero sentinel");
            run_cached(1, 1, 1, callable_pc);
        }
    }
#endif
}

void LinkedPinnedUniforms() {
    Builder builder{Scope::Block};
    Need(builder.BeginBlock(Create(builder)));
    const auto old = Load(builder, 0), changed = Constant(builder, 0xabcdef01000000aaULL);
    Need(builder.Emit<OpCode::StoreUniform>(
            Type::Void, {Operand::Uniform(1, Type::U8), Operand::ValueOf(changed, Type::U8)}));
    Need(builder.Emit<OpCode::StoreUniform>(
            Type::Void, {Operand::Uniform(8, Type::U64), Operand::ValueOf(changed, Type::U32)}));
    StoreCarrier(builder, 80, old);  // A pin write must not retroactively change this SSA read.
    const auto deferred =
            Need(builder.Emit<OpCode::LoadImm>(Type::U8, {Operand::Immediate(0x77, Type::U8)}));
    const auto unrelated = Load(builder, 96);
    Store(builder, 16, deferred);
    const auto later = Need(builder.Emit<OpCode::Add>(
            Type::U64, {Operand::ValueOf(deferred), Operand::ValueOf(unrelated)}));
    StoreCarrier(builder, 104, later);
    Need(builder.Emit<OpCode::UniformBarrier>(Type::Void, {}));
    const auto target = Constant(builder, 0x1234567890ULL);
    Store(builder, 128, target);
    Need(builder.EndBlock(Terminal::Dispatch()));
    auto program = Submit(builder);
    arm64::CompileOptions options;
    options.flags_offset = 136;
    options.dispatcher = 0x1111222233334444ULL;
    options.exit_handler = 0xaaaabbbbccccddddULL;
    options.pc_offset = 128;
    options.pinned_uniform_offset = 0;
    options.private_uniforms = true;
    const auto locations = arm64::AllocateBlock(program, &options);
    Check(bool(locations) && locations.homes[deferred.node.value].reg == 22 &&
                  !locations.homes[deferred.node.value].narrow_pin,
          "actual private ABI coalesces a non-adjacent numeric publication through its later uses");
    const auto code = arm64::Compile(program, options);
    Check(bool(code) && code.frame_bytes == 0 && code.spill_slots == 0 &&
                  code.words.front() == 0xaa1403e2u,
          "linked unit copies pinned state to an ordinary SSA home without a root prologue");
    Check(std::any_of(code.words.begin(),
                      code.words.end(),
                      [](std::uint32_t word) {
                          return (word & 0xffe0ffffu) ==
                                 0x2a0003f5u;  // MOV W21,Wn zeroes the high half.
                      }),
          "U32 input into a full U64 pinned field clears its high half");
    for (unsigned i = 0; i < 8; ++i)
        Check(std::count(code.words.begin(),
                         code.words.end(),
                         0xf9000000u | (i << 10) | (19u << 5) | (20u + i)) == 1,
              "UniformBarrier publishes each actual pinned slot once");
    Check(code.links.size() == 1 && code.links[0].target_pc == 0x1234567890ULL,
          "only the real local full-width PC store creates a direct link");
    const auto pointer = WordOffset(code.links[0].data_offset);
    const auto branch = WordOffset(code.links[0].branch_offset);
    Check(!(pointer & 1) && branch < pointer && pointer + 1 < code.words.size() &&
                  pointer - branch == 2 + (branch & 1) &&
                  code.words[branch] == (0x58000010u | ((pointer - branch) << 5)) &&
                  code.words[branch + 1] == 0xd61f0200u &&
                  (u64{code.words[pointer]} | (u64{code.words[pointer + 1]} << 32)) ==
                          options.dispatcher,
          "direct link names an aligned dispatcher pointer DATA relocation");
    Check(std::find(code.words.begin(), code.words.end(), 0xf9404661u) == code.words.end() &&
                  std::find(code.words.begin(), code.words.end(), 0xf9004661u) == code.words.end(),
          "private linked entry/dispatch keep the live flags carrier without state "
          "reload/publication");
}

void LinkedExitAndCrossSlot() {
    Builder builder{Scope::Function};
    const auto entry = Create(builder), dispatch = Create(builder), exit = Create(builder);
    Need(builder.BeginBlock(entry));
    const auto target = Constant(builder, 0x87654321);
    const auto condition = Load(builder, 80);
    Need(builder.EndBlock(Terminal::Branch(condition, dispatch, exit)));
    Need(builder.BeginBlock(dispatch));
    Store(builder, 128, target);
    Need(builder.EndBlock(Terminal::Dispatch()));
    Need(builder.BeginBlock(exit));
    const auto crossing = Load(builder, 7);
    const auto reason = Constant(builder, 1);
    Need(builder.EndBlock(Terminal::Return(reason)));
    auto program = Submit(builder);
    arm64::CompileOptions options;
    options.flags_offset = 136;
    options.dispatcher = 0x1111222233334444ULL;
    options.exit_handler = 0xaaaabbbbccccddddULL;
    options.pc_offset = 128;
    options.pinned_uniform_offset = 0;
    options.private_uniforms = true;
    const auto rejected = arm64::Compile(program, options);
    Check(!rejected && rejected.error == arm64::CompileError::UnsupportedOperand &&
                  rejected.words.empty() && rejected.links.empty(),
          "the explicit pin ABI rejects a field that crosses slot boundaries");
    auto editor = Builder::Edit(std::move(program));
    Need(editor.Rewrite(
            crossing.node, OpCode::LoadUniform, Type::U64, {Operand::Uniform(80, Type::U64)}));
    auto admitted = Submit(editor);
    const auto code = arm64::Compile(admitted, options);
    Check(bool(code) && code.links.empty() && code.frame_bytes == 0,
          "foreign-block PC literals retain dispatcher lookup without a guessed direct target");
    bool exit_pointer{}, dispatcher_pointer{};
    for (std::size_t at = 0; at + 1 < code.words.size(); ++at) {
        if ((code.words[at] & 0xff00001fu) != 0x58000010u || code.words[at + 1] != 0xd61f0200u)
            continue;
        const auto encoded = (code.words[at] >> 5) & 0x7ffffu;
        const auto distance =
                encoded < 0x40000u ? std::int64_t{encoded} : std::int64_t{encoded} - 0x80000;
        const auto data = static_cast<std::int64_t>(at) + distance;
        if (data < 0 || static_cast<std::uint64_t>(data) + 1 >= code.words.size()) continue;
        const auto pointer = u64{code.words[data]} | (u64{code.words[data + 1]} << 32);
        exit_pointer |= pointer == options.exit_handler;
        dispatcher_pointer |= pointer == options.dispatcher;
    }
    Check(exit_pointer && dispatcher_pointer &&
                  std::find(code.words.begin(), code.words.end(), 0xd65f03c0u) == code.words.end(),
          "linked Return and Dispatch target their separate runtime services without a unit RET");
}

}  // namespace

TEST_CASE("PCTailPredicates", "[compile][native][arm64]") { CHECK_NOTHROW(PCTailPredicates()); }

TEST_CASE("DeferredPCLinks", "[compile][native][arm64]") { CHECK_NOTHROW(DeferredPCLinks()); }

TEST_CASE("CompleteAdditionalEntries", "[arm64][native]") {
    CHECK_NOTHROW(CompleteAdditionalEntries());
}

TEST_CASE("LinkedDynamicPC", "[compile][arm64][native]") { CHECK_NOTHROW(LinkedDynamicPC()); }

TEST_CASE("LinkedPinnedUniforms", "[compile][native][arm64]") {
    CHECK_NOTHROW(LinkedPinnedUniforms());
}

TEST_CASE("LinkedExitAndCrossSlot", "[compile][native][arm64]") {
    CHECK_NOTHROW(LinkedExitAndCrossSlot());
}
