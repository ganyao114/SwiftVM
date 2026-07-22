//
// Created by 甘尧 on 2024/2/23.
//

#pragma once

#include "cpu.h"
#include "translator/runtime.h"

namespace swift::translator::arm64 {

using namespace swift::arm64;

class Arm64Core;

class Arm64Instance : public Instance {
public:
    friend class Arm64Core;

    static Arm64Instance *Make();
    static void Destroy(Arm64Instance *instance);

private:
    explicit Arm64Instance();

    struct Impl;
    std::unique_ptr<Impl> impl{};
};

class Arm64Core : public Core {
public:
    static Arm64Core *Make(Arm64Instance *instance);
    static void Destroy(Arm64Core *core);

    ExitReason Run() override;
    ExitReason Step() override;
    void SignalInterrupt() override;
    void ClearInterrupt() override;
    uint64_t GetSyscallNumber() override;

    // Guest CPU context, backed by the runtime uniform buffer. The loader
    // initializes pc / sp here before the first Run(), and the syscall
    // handler reads x8 / x0-x5 and writes x0 after each ExitReason::Syscall.
    //
    // Syscall convention (ARM64 Linux):
    //   - guest executes `svc #0`
    //   - frontend must store all guest regs into the uniform buffer
    //     (ThreadContext64), set pc = address of the instruction *after* the
    //     svc, and halt with HaltReason::CallHost
    //   - Run() then returns ExitReason::Syscall; x8 holds the syscall
    //     number, x0-x5 the arguments; the host handler emulates the call and
    //     writes the (signed 64-bit) return value back into x0, then calls
    //     Run() again to resume the guest.
    ThreadContext64 &GetContext();

private:
    explicit Arm64Core(Arm64Instance *instance);

    struct Impl;
    std::unique_ptr<Impl> impl{};
    Arm64Instance *instance{};
};

}  // namespace swift::translator::arm64
