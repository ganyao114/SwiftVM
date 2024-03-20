//
// Created by 甘尧 on 2024/2/23.
//

#pragma once

#include "cpu.h"
#include "translator/interface.h"

namespace swift::arm64 {

using namespace translator;

class Arm64Core : public Core {
public:

    ExitReason Run() override;
    ExitReason Step() override;
    void SignalInterrupt() override;
    void ClearInterrupt() override;
    uint64_t GetSyscallNumber() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace swift::arm64
