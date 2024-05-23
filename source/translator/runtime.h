//
// Created by 甘尧 on 2024/2/23.
//

#pragma once

#include <cstdint>

namespace swift::translator {

enum ExitReason : uint8_t { None = 0, IllegalCode, PageFatal, Syscall, Signal, Step };

class Instance {

};

class Core {
public:
    virtual ExitReason Run() = 0;
    virtual ExitReason Step() = 0;
    virtual void SignalInterrupt() = 0;
    virtual void ClearInterrupt() = 0;
    virtual uint64_t GetSyscallNumber() = 0;
};

}  // namespace swift::translator