#include "test_support.h"

#include <bit>
#include <memory>
#include <span>
#include <sys/mman.h>
#include <unistd.h>
#include "runtime/backend/address_space.h"
#include "runtime/backend/smc_tracker.h"
#include "translator/arm64/translator.h"

namespace swift::tests::loongarch64 {
namespace {
class GuestCode final : public MemoryInterface {
public:
    const size_t page_size{static_cast<size_t>(getpagesize())};
    const size_t size{std::bit_ceil(page_size * 4)};
    GuestCode() {
        allocation = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (allocation == MAP_FAILED) throw std::runtime_error("allocate SMC guest code");
    }
    ~GuestCode() { munmap(allocation, size); }
    void* Base() const { return allocation; }
    void Put(u64 address, u32 instruction) {
        *reinterpret_cast<volatile u32*>(static_cast<u8*>(allocation) + address) = instruction;
    }
    bool Read(void* output, size_t address, size_t length) override {
        if (address >= size || length > size - address) return false;
        std::memcpy(output, static_cast<u8*>(allocation) + address, length);
        return true;
    }
    bool Write(void* input, size_t address, size_t length) override {
        if (address >= size || length > size - address) return false;
        std::memcpy(static_cast<u8*>(allocation) + address, input, length);
        return true;
    }
    void* GetPointer(void* guest) override {
        const auto address = reinterpret_cast<uintptr_t>(guest);
        return address < size ? static_cast<u8*>(allocation) + address : nullptr;
    }
    CodeFetchResult ReadCode(void* output, size_t address, size_t length) override {
        return Read(output, address, length) ? CodeFetchResult::Success : CodeFetchResult::Fault;
    }
    bool MatchesCodeAddressSpace(uintptr_t bias, u64 mask) const override {
        return bias == reinterpret_cast<uintptr_t>(allocation) && mask == size - 1;
    }
private:
    void* allocation{};
};
}

void RuntimeLifecycle() {
    const auto prior_smc = backend::SmcTracker::IsEnabled();
    struct RestoreSMC {
        bool previous;
        ~RestoreSMC() { backend::SmcTracker::SetEnabled(previous); }
    } restore{prior_smc};
    backend::SmcTracker::SetEnabled(true);
    GuestCode memory;
    constexpr u64 target = 0x100;
    const auto writer = memory.page_size + 0x100;
    memory.Put(target, 0xd2800020);     // mov x0, #1
    memory.Put(target + 4, 0xd2800ba8); // mov x8, #93
    memory.Put(target + 8, 0xd4000001); // svc #0
    memory.Put(writer, 0xb9000001);    // str w1, [x0]
    memory.Put(writer + 4, 0x91000442); // add x2, x2, #1
    memory.Put(writer + 8, 0xd2800ba8);
    memory.Put(writer + 12, 0xd4000001);
    using Instance = translator::arm64::Arm64Instance;
    using Core = translator::arm64::Arm64Core;
    std::unique_ptr<Instance, decltype(&Instance::Destroy)> instance{
            Instance::Make(memory.Base(), memory.size - 1, &memory), Instance::Destroy};
    std::unique_ptr<Core, decltype(&Core::Destroy)> core{Core::Make(instance.get()), Core::Destroy};
    auto& context = core->GetContext();
    context.pstate = 0xa5a50a00;
    context.pc = target;
    Check(core->Run() == translator::ExitReason::Syscall && context.r[0] == 1,
          "SMC fixture executes the first guest translation");
    Check(instance->GetAddressSpace()->GetSmcTracker().HasProtectedPages(),
          "executed guest code is tracked and write protected");
    const auto flags = context.pstate;
    // This write occurs outside JitRun. The owning Runtime's SMC signal
    // handler must retire the old translation before it can be entered.
    memory.Put(target, 0xd2800040); // mov x0, #2
    context.pc = target;
    Check(core->Run() == translator::ExitReason::Syscall && context.r[0] == 2,
          "host write to protected guest code invalidates and recompiles the old entry");
    Check(context.pstate == flags, "host SMC handoff preserves all public PSTATE bits");
    context.pc = writer;
    context.r[0] = target;
    context.r[1] = 0xd2800060; // mov x0, #3
    context.r[2] = 17;
    const auto writer_exit = core->Run();
    Check(writer_exit == translator::ExitReason::Syscall && context.r[2] == 18,
          "guest SMC store retires once and resumes at the following instruction: exit=" +
                  std::to_string(u32(writer_exit)) + " expected_exit=" +
                  std::to_string(u32(translator::ExitReason::Syscall)) + " r2=" +
                  std::to_string(context.r[2]) + " expected_r2=18 pc=" +
                  std::to_string(context.pc) + " expected_pc=" + std::to_string(writer + 16));
    Check(context.pc == writer + 16 && context.pstate == flags,
          "guest SMC handoff preserves PC and public PSTATE");
    context.pc = target;
    Check(core->Run() == translator::ExitReason::Syscall && context.r[0] == 3,
          "guest rewrite replaces the target translation and published instructions are visible");
    context.pc = target;
    auto after_step = context;
    after_step.r[0] = 3;
    after_step.pc = target + 4;
    Check(core->Step() == translator::ExitReason::Step &&
                  std::memcmp(&context, &after_step, sizeof(context)) == 0,
          "native LoongArch Step retires one fresh instruction and preserves all other architectural state");
    const auto before_interrupt = context;
    core->SignalInterrupt();
    Check(core->Step() == translator::ExitReason::Signal &&
                  std::memcmp(&context, &before_interrupt, sizeof(context)) == 0,
          "pending interrupt rejects Step before changing CPU state");
    core->ClearInterrupt();
    context.pc = target;
    Check(core->Run() == translator::ExitReason::Syscall && context.r[0] == 3,
          "Run remains usable after native and interrupted Step requests");
    std::cout << "PASS actual host/guest SMC writes, retirement, recompile and I-cache visibility\n";
}
}  // namespace swift::tests::loongarch64
