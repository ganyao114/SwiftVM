//
// Created by 甘尧 on 2024/2/23.
//

#include "runtime/frontend/arm64/arm64_frontend.h"
#include "runtime/backend/context.h"
#include "runtime/include/sruntime.h"
#include "translator.h"

namespace swift::arm64 {

struct Arm64Core::Impl final {
    Impl() {}

    ExitReason Run() {
        // update backend location
        s_runtime->SetLocation(GetCPUContext()->pc);
        auto hr = s_runtime->Run();
        if ((hr & runtime::HaltReason::CodeMiss) != runtime::HaltReason::None) {
            // No cache, do translate

            hr = s_runtime->Run();
        }
        // update frontend location
        GetCPUContext()->pc = s_runtime->GetLocation();
        return PageFatal;
    }

    ExitReason Step() { return PageFatal; }

    ThreadContext64* GetCPUContext() {
        return reinterpret_cast<ThreadContext64*>(s_runtime->GetUniformBuffer().data());
    }

    std::shared_ptr<runtime::Interface> s_runtime{};
};

ExitReason Arm64Core::Run() { return PageFatal; }

ExitReason Arm64Core::Step() { return PageFatal; }

void Arm64Core::SignalInterrupt() {}

void Arm64Core::ClearInterrupt() {}

uint64_t Arm64Core::GetSyscallNumber() { return 0; }

}  // namespace swift::arm64