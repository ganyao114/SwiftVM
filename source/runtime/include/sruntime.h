//
// Created by 甘尧 on 2023/9/7.
//

#pragma once

#include <memory>
#include "config.h"

namespace swift::runtime {

namespace backend {
struct State;
}

enum class HaltReason : std::uint32_t {
    None = 0x00000000,
    Step = 0x00000001,
    Signal = 0x00000002,
    PageFatal = 0x00000004,
    CodeMiss = 0x00000008,
    ModuleMiss = 0x000000010,
    CacheMiss = 0x00000020,
    // Reserved ABI value: the old untracked linkage patcher was removed.
    ReservedLinkage = 0x00000040,
    IllegalCode = 0x00000080,
    CallHost = 0x000000100,
    RuntimeBusy = 0x00000200,
    Retirement = 0x00000400,
    InstructionPending = 0x00000800,
    InstructionRejected = 0x00001000
};

DECLARE_ENUM_FLAG_OPERATORS(HaltReason)

class Instance {
public:
    virtual ~Instance() = default;

    static std::unique_ptr<Instance> Make(const Config& config);
};

class Runtime {
public:
    struct InstructionKey {
        const void* runtime{};
        const void* address_space{};
        const void* control{};
        const void* descriptor{};
        const void* emitted_ir{};
        const void* allocation{};
        u64 generation{}, allocation_id{}, ir_identity{}, head_id{}, guest_pc{};
        friend bool operator==(const InstructionKey&, const InstructionKey&) = default;
    };
    explicit Runtime(Instance *instance);

    ~Runtime();

    // Concurrent or recursive entry on this Runtime returns RuntimeBusy.
    // Other state access and destruction require Run to be stopped.
    HaltReason Run();
    [[nodiscard]] InstructionKey SuspendedInstruction() const;
    HaltReason ResumeInstruction(const InstructionKey& key);
    [[nodiscard]] bool AbandonInstruction(const InstructionKey& key);

    HaltReason Step();

    void SignalInterrupt();

    void ClearInterrupt();

    void SetLocation(LocationDescriptor location);

    [[nodiscard]] LocationDescriptor GetLocation();

    [[nodiscard]] std::span<u8> GetUniformBuffer() const;

    [[nodiscard]] backend::State *GetState() const;

    // Called on the owning thread while Run() is stopped, e.g. at a syscall.
    void DumpExecutionTrace() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl{};
};

}  // namespace swift::runtime
