#include "test_support.h"

#include <bit>
#include <cerrno>
#include <csignal>
#include <functional>
#include <memory>
#include <vector>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include "runtime/backend/address_space.h"
#include "runtime/backend/atomic_fallback.h"
#include "runtime/backend/execution_owner.h"
#include "runtime/backend/guest_memory_scope.h"
#include "runtime/backend/loongarch64/jit/fault_scope.h"
#include "runtime/backend/runtime.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#include "translator/arm64/translator.h"

namespace swift::tests::loongarch64 {
namespace {
using Context = swift::arm64::ThreadContext64;
using Exit = translator::ExitReason;
using Trap = swift::arm64::InterruptReason;
using T = ir::ValueType;
constexpr u64 code_pc = 0x100, opaque_flags = 0xa5a50000a2345678ULL;

struct Mapping {
    size_t page{static_cast<size_t>(getpagesize())};
    size_t size{std::bit_ceil(page * 8)};
    u8* bytes{static_cast<u8*>(mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0))};
    Mapping() { Check(bytes != MAP_FAILED, "allocate single-instruction guest mapping"); }
    ~Mapping() { if (bytes != MAP_FAILED) munmap(bytes, size); }
    void Word(u64 address, u32 word) { std::memcpy(bytes + address, &word, sizeof(word)); }
    void Protect(u64 address, int protection) {
        Check(mprotect(bytes + address, page, protection) == 0, "protect single-instruction data page");
    }
};

struct Provider final : MemoryInterface {
    Mapping& mapping;
    u32 fetches{}, data_reads{}, pointers{};
    std::vector<std::pair<u64, size_t>> requests;
    CodeFetchResult status{CodeFetchResult::Success};
    std::function<void()> before_fetch;
    explicit Provider(Mapping& mapping) : mapping(mapping) {}
    CodeFetchResult ReadCode(void* destination, size_t address, size_t size) override {
        ++fetches; requests.emplace_back(address, size);
        if (before_fetch) before_fetch();
        if (status != CodeFetchResult::Success) return status;
        if (size != 4 || address > mapping.size - size) return CodeFetchResult::Fault;
        std::memcpy(destination, mapping.bytes + address, size);
        return CodeFetchResult::Success;
    }
    bool MatchesCodeAddressSpace(uintptr_t bias, u64 mask) const override {
        return bias == reinterpret_cast<uintptr_t>(mapping.bytes) && mask == mapping.size - 1;
    }
    bool Read(void*, size_t, size_t) override { ++data_reads; return false; }
    bool Write(void*, size_t, size_t) override { return false; }
    void* GetPointer(void* address) override {
        ++pointers;
        const auto guest = reinterpret_cast<uintptr_t>(address);
        return guest < mapping.size ? mapping.bytes + guest : nullptr;
    }
    static SingleDecodeResult Decode(const void*, LocationDescriptor address,
                                     MemoryInterface& memory, ir::Block& block) {
        ir::Assembler assembler{&block};
        swift::arm64::A64Decoder decoder{address, &memory, &assembler};
        const auto result = decoder.DecodeSingle();
        return result == CodeFetchResult::Success ? SingleDecodeResult::Ready :
               result == CodeFetchResult::Fault ? SingleDecodeResult::FaultIR : SingleDecodeResult::Unsupported;
    }
};

Context Initial() {
    Context context{};
    for (u32 index = 0; index < context.r.size(); ++index)
        context.r[index] = 0x8123456789abcdefULL ^ (u64(index) * 0x0101010101010101ULL);
    context.fp = 0xfedcba9876543210ULL; context.lr = 0x9988776655443322ULL;
    context.sp = 0x1234567887654321ULL; context.pc = code_pc;
    context.pstate = 0xa5a50a00; context.padding = 0x2468ace0;
    context.fpsr = 0x08000015; context.tpidr = 0x1020304050607080ULL;
    for (u32 index = 0; index < context.v.size(); ++index) {
        const std::array<u64, 2> lanes{0x1122334455667788ULL + index, 0x8877665544332211ULL - index};
        std::memcpy(&context.v[index], lanes.data(), sizeof(lanes));
    }
    return context;
}

Config Configuration(Provider& provider, bool native, SingleDecodeService service) {
    return {.loc_start = 0, .loc_end = provider.mapping.size, .enable_jit = native,
            .backend_isa = kLoongArch, .uniform_buffer_size = sizeof(Context), .static_program = true,
            .global_opts = Optimizations::None, .stack_alignment = 16,
            .memory_base = provider.mapping.bytes, .guest_addr_mask = provider.mapping.size - 1,
            .memory = &provider,
            .single_decode = service.callback ? service : SingleDecodeService{&Provider::Decode, nullptr}};
}

struct Fixture {
    Mapping mapping;
    Provider provider{mapping};
    backend::AddressSpace space;
    Runtime runtime;
    explicit Fixture(bool native = true, SingleDecodeService service = {})
            : space(Configuration(provider, native, service)), runtime(&space) { Seed(Initial()); }
    Context& Guest() { return *reinterpret_cast<Context*>(runtime.GetUniformBuffer().data()); }
    void Seed(const Context& value, u64 flags = opaque_flags) {
        Guest() = value; runtime.GetState()->host_cpu_flags = flags; runtime.SetLocation(value.pc);
    }
    void NoPublication(u64 address = code_pc) {
        Check(backend::IsEmpty(space.GetDefaultModule()->GetNode(ir::Location{address})), "Step leaves Module node unpublished");
        Check(space.GetCodeCache(address) == nullptr, "Step leaves ordinary dispatch unpublished");
        Check(space.GetCallCodeCacheTable().Lookup(address) == 0 &&
              space.GetPendingCallCodeCacheTable().Lookup(address) == 0, "Step leaves call dispatch unpublished");
    }
};

thread_local Runtime* abi_runtime{};
thread_local HaltReason abi_reason{};
HaltReason CallStep(backend::State*) { return abi_reason = abi_runtime->Step(); }

HaltReason Step(Fixture& fixture) {
    const auto readers = backend::riscv64::anonymous_memory_readers.load();
    const auto kernels = backend::riscv64::memory_kernel_depth.load();
    const auto mapping = backend::GuestMemoryScope::Current();
    const auto lock_address = fixture.runtime.GetState()->unaligned_atomic_lock_address;
    const auto pointer_reads = fixture.provider.pointers, data_reads = fixture.provider.data_reads;
    abi_runtime = &fixture.runtime;
    Check(CheckABI(&CallStep, nullptr) == 1, "public Step preserves LP64D GP/FPR registers, stack and FCSR");
    abi_runtime = nullptr;
    auto* state = fixture.runtime.GetState();
    Check(state->spill_area[backend::kRiscvRecoveryPcSlot] == 0 &&
          state->spill_area[backend::kRiscvRecoveryFrameSlot] == 0 &&
          state->spill_area[backend::kRiscvMemoryOwnedSlot] == 0 &&
          la::GetGuestFaultKind(state) == la::GuestFaultKind::None,
          "Step clears private recovery frame, fault scope and memory lease");
    auto* participant = reinterpret_cast<backend::riscv64::MemoryParticipant*>(
            state->spill_area[backend::kRiscvMemoryParticipantSlot]);
    Check(!participant || participant->active.load() == 0, "Step releases its registered reader");
    Check(!backend::owns_unaligned_atomic_guard.load() && backend::unaligned_atomic_lock.load() == 0 &&
          state->unaligned_atomic_lock_address == lock_address,
          "Step releases typed atomic writer ownership and preserves its shared lock address");
    Check(backend::riscv64::anonymous_memory_readers.load() == readers &&
          backend::riscv64::memory_kernel_depth.load() == kernels, "Step restores anonymous helper leases");
    Check(!backend::RuntimeExecutionActiveOnCurrentThread() &&
          backend::GuestMemoryScope::Current().bias == mapping.bias &&
          backend::GuestMemoryScope::Current().mask == mapping.mask, "Step restores execution ownership and guest mapping");
    Check(fixture.provider.pointers == pointer_reads && fixture.provider.data_reads == data_reads,
          "Step only fetches through safe ReadCode");
    return abi_reason;
}

void Equal(const Context& expected, const Context& actual, const std::string& detail) {
    Check(std::memcmp(&expected, &actual, sizeof(expected)) == 0, detail);
}
void Result(Fixture& fixture, const Context& expected, u64 pc, u64 flags = opaque_flags) {
    Equal(expected, fixture.Guest(), "raw Runtime Step preserves all unmodified architectural fields");
    Check(fixture.runtime.GetLocation() == pc && fixture.runtime.GetState()->host_cpu_flags == flags,
          "Runtime Step publishes the architectural successor and complete resident flags");
    fixture.NoPublication();
}

void FreshDecodeAndBranches() {
    Fixture fixture;
    fixture.mapping.Word(code_pc, 0x91000400); fixture.mapping.Word(code_pc + 4, 0xd2800034);
    auto expected = fixture.Guest(); ++expected.r[0];
    Check(Step(fixture) == HaltReason::Step, "native fresh add retires exactly once");
    Result(fixture, expected, code_pc + 4);
    Check(fixture.runtime.GetState()->prev_loc.Value() == code_pc, "Step publishes the retired instruction as prev_loc");
    fixture.mapping.Word(code_pc, 0x91000800); fixture.runtime.SetLocation(code_pc); expected.r[0] += 2;
    Check(Step(fixture) == HaltReason::Step, "Step fetches rewritten instruction without using stale code");
    Result(fixture, expected, code_pc + 4);
    Check(fixture.provider.fetches == 2 && fixture.provider.requests ==
          std::vector<std::pair<u64, size_t>>{{code_pc, 4}, {code_pc, 4}}, "each fresh Step fetches only its original four bytes");
    for (u32 word : {0x14000003u, 0x97fffffeu, 0xd61f03c0u, 0xd63f03c0u, 0xd65f03c0u}) {
        Fixture branch; branch.mapping.Word(code_pc, word);
        auto initial = Initial(); initial.lr = code_pc + 64; branch.Seed(initial);
        auto wanted = initial;
        if (word == 0x97fffffe || word == 0xd63f03c0) wanted.lr = code_pc + 4;
        const u64 target = word == 0x14000003 ? code_pc + 12 : word == 0x97fffffe ? code_pc - 8 : initial.lr;
        Check(Step(branch) == HaltReason::Step, "one branch unit retires without dispatching successor");
        Result(branch, wanted, target);
        Check(branch.runtime.GetState()->prev_loc.Value() == code_pc, "branch prev_loc retains original guest PC");
    }
    for (bool taken : {false, true}) {
        Fixture branch; branch.mapping.Word(code_pc, 0x54000060); // B.EQ +12
        const auto initial = branch.Guest(); const u64 flags = opaque_flags | (u64(taken) << 30);
        branch.Seed(initial, flags);
        Check(Step(branch) == HaltReason::Step, "conditional branch retires complete selected successor");
        Result(branch, initial, code_pc + (taken ? 12 : 4), flags);
    }
    Fixture interpreted{false}; interpreted.mapping.Word(code_pc, 0x91000400);
    expected = interpreted.Guest(); ++expected.r[0];
    Check(Step(interpreted) == HaltReason::Step, "portable single-instruction mode agrees with native retirement");
    Result(interpreted, expected, code_pc + 4);
}

struct RejectedUnit {
    u32 shape{}, decodes{}, calls{};
    static u64 Helper(u64 pointer, u64, u64) { ++reinterpret_cast<RejectedUnit*>(pointer)->calls; return 7; }
    static SingleDecodeResult Decode(const void* pointer, LocationDescriptor address, MemoryInterface&, ir::Block& block) {
        auto& unit = *static_cast<RejectedUnit*>(const_cast<void*>(pointer)); ++unit.decodes;
        ir::Assembler as{&block};
        auto value = as.LoadImm(ir::Imm{u64{0x1234}}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{u32(offsetof(Context, r)), T::U64}, value);
        auto destination = as.LoadImm(ir::Imm{u64{0x800}}).SetType(T::U64);
        as.StoreMemory(ir::Operand{destination}, value);
        if (unit.shape == 0) as.AdvancePC(ir::Imm{u32{4}});
        else if (unit.shape == 1) {
            auto argument = as.LoadImm(ir::Imm{reinterpret_cast<u64>(&unit)}).SetType(T::U64);
            as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Helper)}}, argument, argument, argument);
        } else if (unit.shape == 2) {
            auto edge = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)}); as.BindLabel(edge);
            auto* label = &block.GetInstList().back(); block.RemoveInst(label); block.InsertBefore(label, edge.Def()); block.ReIdInstr();
        } else return SingleDecodeResult::Ready;
        block.SetTerminal(ir::terminal::LinkBlock{ir::Location{address + 4}});
        return SingleDecodeResult::Ready;
    }
};

void RefusalsAndCore() {
    Fixture pending; pending.mapping.Word(code_pc, 0x91000400); const auto untouched = pending.Guest();
    pending.runtime.SignalInterrupt();
    Check(Step(pending) == HaltReason::Signal && Step(pending) == HaltReason::Signal, "pending Signal rejects Step before decode");
    Equal(untouched, pending.Guest(), "pending Signal leaves the complete guest context intact");
    Check(pending.provider.fetches == 0 && pending.runtime.GetLocation() == code_pc, "pending Signal does not fetch or advance PC");
    pending.runtime.ClearInterrupt();
    Check(Step(pending) == HaltReason::Step, "clear Signal permits fresh Step");
    for (u32 shape = 0; shape < 4; ++shape) {
        RejectedUnit rejected{shape}; Fixture fixture{true, {&RejectedUnit::Decode, &rejected}};
        fixture.mapping.bytes[0x800] = 0xa7;
        std::vector<u8> snapshot(sizeof(backend::State) + sizeof(Context));
        std::memcpy(snapshot.data(), fixture.runtime.GetState(), snapshot.size());
        Check(Step(fixture) == HaltReason::Unsupported, "unsupported complete unit refuses all prefix effects");
        Check(std::memcmp(snapshot.data(), fixture.runtime.GetState(), snapshot.size()) == 0 &&
              fixture.mapping.bytes[0x800] == 0xa7 && rejected.calls == 0 && rejected.decodes == 1,
              "unsupported unit preserves State, architecture and guest memory");
        fixture.NoPublication();
    }
    using Instance = translator::arm64::Arm64Instance;
    using Core = translator::arm64::Arm64Core;
    Mapping mapping; Provider provider{mapping};
    std::unique_ptr<Instance, decltype(&Instance::Destroy)> instance{
            Instance::Make(mapping.bytes, mapping.size - 1, &provider), Instance::Destroy};
    std::unique_ptr<Core, decltype(&Core::Destroy)> core{Core::Make(instance.get()), Core::Destroy};
    core->GetContext() = Initial(); mapping.Word(code_pc, 0x91000400);
    auto expected = core->GetContext(); ++expected.r[0]; expected.pc += 4;
    Check(core->Step() == Exit::Step, "Core commits one native instruction");
    Equal(expected, core->GetContext(), "Core Step commits only PC and instruction-defined registers");
    core->SignalInterrupt(); const auto fetches = provider.fetches;
    Check(core->Step() == Exit::Signal && provider.fetches == fetches, "Core pending Signal does not fetch");
    Equal(expected, core->GetContext(), "Core pending Signal leaves all architecture unchanged");
    core->ClearInterrupt(); provider.status = CodeFetchResult::Unsupported;
    Check(core->Step() == Exit::Unsupported, "Core preserves unsupported safe-fetch distinction");
    Equal(expected, core->GetContext(), "unsupported Core leaves PC, all registers and PSTATE unchanged");
    provider.status = CodeFetchResult::Success;
    struct CoreTrap { u32 word; Trap trap; Exit exit; bool next; };
    for (auto trap : {CoreTrap{0xd4000001, Trap::SVC, Exit::Syscall, true},
                      CoreTrap{0xd4200000, Trap::BRK, Exit::IllegalCode, false},
                      CoreTrap{0xd4400000, Trap::HLT, Exit::IllegalCode, false},
                      CoreTrap{0, Trap::ILL_CODE, Exit::IllegalCode, false}}) {
        mapping.Word(code_pc, trap.word); core->GetContext() = Initial(); core->GetContext().r[8] = 173;
        expected = core->GetContext(); expected.padding = u32(trap.trap); expected.pc += trap.next ? 4 : 0;
        const auto syscall = core->GetSyscallNumber();
        Check(core->Step() == trap.exit, "Core maps Step host traps to public exit reasons");
        Equal(expected, core->GetContext(), "Core trap preserves complete state and precise fault/successor PC");
        Check(core->GetSyscallNumber() == (trap.exit == Exit::Syscall ? 173 : syscall), "only SVC publishes syscall number");
    }
    Check(backend::IsEmpty(instance->GetAddressSpace()->GetDefaultModule()->GetNode(ir::Location{code_pc})) &&
          instance->GetAddressSpace()->GetCodeCache(code_pc) == nullptr, "public Core Step publishes no ordinary code");
}

void MemoryAndAtomicFaults() {
    // Aligned normal accesses are native; byte atomics use the audited typed
    // kernel and its writer gate. Every path must be reusable after recovery.
    for (u32 word : {0xf9400020u, 0xf9000020u, 0xf8408c20u, 0xf8008c20u, 0xf8200022u, 0x38200022u}) {
        Fixture fixture; const u64 guarded = fixture.mapping.page * 3;
        fixture.mapping.Word(code_pc, word); fixture.mapping.Word(code_pc + 4, 0xd2800034);
        const bool writeback = word == 0xf8408c20 || word == 0xf8008c20;
        const bool atomic = word == 0xf8200022 || word == 0x38200022;
        const bool byte = word == 0x38200022;
        auto initial = Initial(); initial.r[1] = guarded - (writeback ? 8 : 0); initial.r[0] = 3; fixture.Seed(initial);
        const u64 old = 37; std::memcpy(fixture.mapping.bytes + guarded, &old, 8);
        fixture.mapping.Protect(guarded, PROT_NONE);
        Check(Step(fixture) == HaltReason::PageFatal, "native/typed atomic fault reports precise PageFatal");
        Result(fixture, initial, code_pc);
        Check(fixture.runtime.GetState()->prev_loc.Value() == code_pc, "fault prev_loc stays on the unretired instruction");
        fixture.mapping.Protect(guarded, PROT_READ | PROT_WRITE);
        u64 memory{}; std::memcpy(&memory, fixture.mapping.bytes + guarded, 8);
        Check(memory == old, "fault suppresses protected store/atomic effects");
        fixture.runtime.SetLocation(code_pc); auto expected = initial;
        if (atomic) expected.r[2] = old;
        else if (word == 0xf9400020 || word == 0xf8408c20) expected.r[0] = old;
        if (writeback) expected.r[1] = guarded;
        Check(Step(fixture) == HaltReason::Step, "same instruction reacquires memory/atomic ownership after fault");
        Result(fixture, expected, code_pc + 4);
        std::memcpy(&memory, fixture.mapping.bytes + guarded, 8);
        Check(memory == (atomic ? (byte ? (old & ~u64{0xff}) | u8(old + 3) : old + 3) :
                          (word == 0xf9000020 || word == 0xf8008c20) ? initial.r[0] : old),
              "post-fault instruction commits exactly its requested memory effect");
    }
}

struct RangeRequest {
    Runtime* runtime{};
    void* bad_pointer{};
    bool signal{}, reject{};
    u32 calls{};
    static bool CheckRange(void* pointer, u64, u64) {
        auto& request = *static_cast<RangeRequest*>(pointer); ++request.calls;
        if (request.bad_pointer) *static_cast<volatile u8*>(request.bad_pointer) = 7;
        if (request.signal) request.runtime->SignalInterrupt();
        return !request.reject;
    }
};

void SignalBoundariesAndHostFaults() {
    for (u32 word : {0xf9400020u, 0xf9000020u, 0x38200022u}) for (bool reject : {false, true}) {
        Fixture fixture; const u64 address = fixture.mapping.page * 3, old = 37;
        fixture.mapping.Word(code_pc, word); fixture.mapping.Word(code_pc + 4, 0xd2800034);
        std::memcpy(fixture.mapping.bytes + address, &old, 8);
        auto initial = Initial(); initial.r[1] = address; initial.r[0] = 3; fixture.Seed(initial);
        RangeRequest request{&fixture.runtime, nullptr, true, reject};
        fixture.runtime.GetState()->interp_range_check = &RangeRequest::CheckRange;
        fixture.runtime.GetState()->interp_range_check_ctx = &request;
        auto expected = initial;
        if (!reject && word == 0xf9400020) expected.r[0] = old;
        if (!reject && word == 0x38200022) expected.r[2] = old;
        Check(Step(fixture) == (reject ? HaltReason::PageFatal : HaltReason::Signal),
              "helper-origin Signal waits for complete instruction and synchronous faults take precedence");
        Result(fixture, expected, code_pc + (reject ? 0 : 4));
        u64 memory{}; std::memcpy(&memory, fixture.mapping.bytes + address, 8);
        Check(memory == (reject || word == 0xf9400020 ? old : word == 0xf9000020 ? initial.r[0] : old + 3),
              "Signal delivery includes complete memory/atomic effects and never executes successor");
        Check(request.calls == 1, "single instruction validates its exact memory range once");
        fixture.runtime.GetState()->interp_range_check = nullptr;
        fixture.runtime.ClearInterrupt(); fixture.runtime.SetLocation(code_pc);
        Check(Step(fixture) == HaltReason::Step, "Step remains reusable after helper Signal or range failure");
    }
    for (bool atomic : {false, true}) {
        const auto child = fork(); Check(child >= 0, "fork isolated public Step host-fault probe");
        if (child == 0) {
            const rlimit no_core{0, 0}; (void)setrlimit(RLIMIT_CORE, &no_core); alarm(10);
            Fixture fixture; fixture.mapping.Word(code_pc, atomic ? 0x38200022 : 0xf9400020);
            auto initial = Initial(); initial.r[1] = fixture.mapping.page * 3; fixture.Seed(initial);
            void* inaccessible = mmap(nullptr, fixture.mapping.page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (inaccessible == MAP_FAILED) _exit(90);
            RangeRequest request{&fixture.runtime, inaccessible};
            fixture.runtime.GetState()->interp_range_check = &RangeRequest::CheckRange;
            fixture.runtime.GetState()->interp_range_check_ctx = &request;
            (void)fixture.runtime.Step(); _exit(91);
        }
        int status{}; pid_t waited{};
        do { waited = waitpid(child, &status, 0); } while (waited == -1 && errno == EINTR);
        Check(waited == child && WIFSIGNALED(status) && (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS),
              "public Step range callback host fault propagates instead of being absorbed status=" + std::to_string(status));
    }
}

void SelfModifyingStep() {
    const auto previous = backend::SmcTracker::IsEnabled();
    struct Restore { bool previous; ~Restore() { backend::SmcTracker::SetEnabled(previous); } } restore{previous};
    backend::SmcTracker::SetEnabled(true);
    Fixture fixture; const u64 target = fixture.mapping.page + 0x100;
    fixture.mapping.Word(code_pc, 0xb8004420); // STR W0,[X1],#4: writeback exposes replay
    fixture.mapping.Word(code_pc + 4, 0x91000694); // ADD X20,X20,#1: must not execute
    fixture.mapping.Word(target, 0x91000462); // ADD X2,X3,#1
    fixture.mapping.Word(target + 4, 0xd503201f); // Neighbor must survive a retry.
    IntrusivePtr<ir::Block> old{new ir::Block(ir::Location{target})};
    Check(Provider::Decode(nullptr, target, fixture.provider, *old) == SingleDecodeResult::Ready, "decode ordinary SMC target");
    old->SetTerminal(ir::terminal::ReturnToHost{});
    auto module = fixture.space.GetDefaultModule();
    Check(module->Push(old.get()), "publish ordinary SMC target IR");
    auto* old_code = backend::TranslateIR(module, old);
    Check(old_code != nullptr, "compile ordinary SMC target before private Step");
    fixture.space.PushCodeCache(target, old_code);
    fixture.space.GetSmcTracker().RegisterNode(module, old.get(), target, target + 4);
    Check(fixture.space.GetSmcTracker().HasProtectedPages(), "ordinary target is protected before private Step write");
    auto initial = Initial(); initial.r[0] = 0x91001462; initial.r[1] = target; fixture.Seed(initial);
    Check(Step(fixture) == HaltReason::Step, "private SMC store retires once after write-window retry");
    auto retired = initial; retired.r[1] += 4;
    Result(fixture, retired, code_pc + 4);
    Check(backend::IsEmpty(module->GetNode(ir::Location{target})) && fixture.space.GetCodeCache(target) == nullptr,
          "private Step closes SMC write window and detaches stale ordinary entry");
    u32 word{}; std::memcpy(&word, fixture.mapping.bytes + target, 4);
    Check(word == 0x91001462, "SMC store publishes replacement guest word");
    std::memcpy(&word, fixture.mapping.bytes + target + 4, 4);
    Check(word == 0xd503201f, "SMC retry writes once and preserves the post-index neighbor");
    fixture.runtime.SetLocation(target); auto expected = retired; expected.r[2] = initial.r[3] + 5;
    Check(Step(fixture) == HaltReason::Step, "Step decodes replacement SMC word freshly");
    Equal(expected, fixture.Guest(), "SMC replacement unit retires complete new arithmetic");
    Check(fixture.runtime.GetLocation() == target + 4 && fixture.runtime.GetState()->prev_loc.Value() == target,
          "replacement Step publishes successor and original PC");
    fixture.NoPublication(target);
    fixture.runtime.SetLocation(target);
    Check(fixture.runtime.Run() == HaltReason::CodeMiss, "ordinary Run cannot revive invalidated SMC entry");
    IntrusivePtr<ir::Block> fresh{new ir::Block(ir::Location{target})};
    Check(Provider::Decode(nullptr, target, fixture.provider, *fresh) == SingleDecodeResult::Ready, "decode replacement for ordinary Run");
    fresh->SetTerminal(ir::terminal::ReturnToHost{});
    Check(module->Push(fresh.get()), "publish replacement ordinary IR");
    auto* fresh_code = backend::TranslateIR(module, fresh); Check(fresh_code != nullptr, "compile replacement ordinary entry");
    fixture.space.PushCodeCache(target, fresh_code); fixture.runtime.SetLocation(target);
    Check(fixture.runtime.Run() == HaltReason::CallHost, "ordinary Run remains usable after native SMC Step");
    Equal(expected, fixture.Guest(), "ordinary Run uses new SMC instruction and preserves other architecture");
}
} // namespace

void RuntimeSteps() {
    FreshDecodeAndBranches(); RefusalsAndCore(); MemoryAndAtomicFaults();
    SignalBoundariesAndHostFaults(); SelfModifyingStep();
    std::cout << "PASS public native Step: fresh decode, precise retirement, faults/leases, Signal, host faults and SMC reuse\n";
}
} // namespace swift::tests::loongarch64
