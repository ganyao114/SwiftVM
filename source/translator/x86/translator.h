//
// Created by 甘尧 on 2024/6/21.
//

#include "cpu.h"
#include "translator/runtime.h"

namespace swift::runtime::backend {
class AddressSpace;
}

namespace swift::translator::x86 {
using namespace swift::x86;
class X86Core;

class X86Instance : public Instance {
public:
    friend class X86Core;

    // memory_base: guest->host bias for guest address virtualization
    // (host addr = guest addr + bias); nullptr = identity mapping (default).
    // guest_addr_mask: bounded guest window (Config::guest_addr_mask),
    // 0 = unbounded (legacy).
    static X86Instance *Make(void* memory_base = nullptr, u64 guest_addr_mask = 0);
    static void Destroy(X86Instance *instance);

    // SMC hook: forwards to AddressSpace::InvalidateCodeRange — see
    // translator::Instance for the contract.
    void InvalidateCodeRange(uint64_t start, uint64_t end) override;

    // Interpreter wild-pointer guard: fn(ctx, guest_addr, size) must return
    // true if [guest_addr, guest_addr+size) is mapped guest memory. Wired by
    // the linux loader to GuestMemory::RangeIsMapped before creating a Core.
    void SetInterpRangeCheck(bool (*fn)(void*, uint64_t, uint64_t), void* ctx);

    // Called before the first guest thread is spawned. Enables QSBR epoch
    // publication for MT-safe SMC invalidation/reclamation. SVM_SMC_MT=0
    // retains the old diagnostic fallback that disables SMC for the process.
    void PrepareForMultithreading();

    // --- AOT pre-compilation (source/aot) ---------------------------------
    // Compile the code at guest address `pc` *without* executing anything,
    // and hand back the entry pointer (null if the translator refused it).
    // This is the same Impl::Translate() the CodeMiss path calls, exposed so
    // the AOT compiler can drive it from a symbol table instead of from
    // control flow -- deliberately not a second translation entry point,
    // since a divergent AOT front end is the failure mode docs/aot-design.md
    // §3 rules out.
    void* CompileAt(uint64_t pc);

    // Clear the process-wide "a function exceeded the block cap, stay on
    // block compilation" latch. That latch exists to stop the *runtime* from
    // repeatedly decoding overlapping suffixes of an oversized CFG; an
    // offline pass walks a fixed symbol list once, so the repetition it
    // guards against cannot happen, and leaving it latched costs almost all
    // of AOT's whole-function coverage (130 of 1074 units on
    // func_tests_x86_64 instead of ~1000). Per-location memos
    // (block_only_locations) are deliberately NOT cleared.
    void ResetFunctionModeLatch();

    // The backing address space, so the AOT compiler can harvest the units
    // Impl::Translate published and so the AOT loader can install units into
    // it. Never null.
    swift::runtime::backend::AddressSpace* GetAddressSpace();

private:
    explicit X86Instance(void* memory_base, u64 guest_addr_mask);

    struct Impl;
    std::unique_ptr<Impl> impl{};
};

class X86Core : public Core {
public:
    static X86Core *Make(X86Instance* instance);
    static void Destroy(X86Core* core);

    ExitReason Run() override;
    ExitReason Step() override;
    void SignalInterrupt() override;
    void ClearInterrupt() override;
    uint64_t GetSyscallNumber() override;
    ThreadContext64& GetContext();

private:
    explicit X86Core(X86Instance* instance);

    struct Impl;
    std::unique_ptr<Impl> impl{};
    X86Instance* instance{};
};

}  // namespace swift::translator::x86
