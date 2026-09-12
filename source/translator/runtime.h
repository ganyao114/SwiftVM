//
// Created by 甘尧 on 2024/2/23.
//

#pragma once

#include <cstdint>
#include <functional>

namespace swift::translator {

enum ExitReason : uint8_t { None = 0, IllegalCode, PageFatal, Syscall, Signal, Step };

class Instance {
public:
    virtual ~Instance() = default;

    // SMC hook (Phase 4): notify the runtime that the guest has changed
    // permissions or unmapped/remapped [start, end) — any translated code
    // overlapping that range must be invalidated. Default: no-op (safe when
    // no SMC tracker is wired, e.g. in unit tests).
    virtual void InvalidateCodeRange(uint64_t start, uint64_t end) {
        (void)start;
        (void)end;
    }
};

class Core {
public:
    virtual ExitReason Run() = 0;
    virtual ExitReason Step() = 0;
    virtual void SignalInterrupt() = 0;
    virtual void ClearInterrupt() = 0;
    virtual uint64_t GetSyscallNumber() = 0;

    // Invoke on the owning thread between Run() calls. The callback becomes
    // a no-op when its Core is destroyed and never refers to another Core.
    virtual std::function<void()> MakeExecutionTraceDumper() const { return {}; }
};

}  // namespace swift::translator
