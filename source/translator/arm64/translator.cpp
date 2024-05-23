//
// Created by 甘尧 on 2024/2/23.
//

#include "runtime/backend/context.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#include "runtime/include/sruntime.h"
#include "translator.h"

namespace swift::translator::arm64 {

using namespace swift::runtime;
using namespace swift::arm64;

struct Arm64Core::Impl final {
    Impl() = default;

    [[nodiscard]] ExitReason Run() const {
        // update backend location
        s_runtime->SetLocation(GetCPUContext()->pc);
        auto hr = HaltReason::None;
        while (hr == HaltReason::None) {
            hr = s_runtime->Run();
            // update frontend location
            GetCPUContext()->pc = s_runtime->GetLocation();
            if ((hr & runtime::HaltReason::CodeMiss) != runtime::HaltReason::None) {
                // No cache, do translate
            }
        }
        if (hr == HaltReason::PageFatal) {
            return ExitReason::PageFatal;
        } else if (hr == HaltReason::Signal) {
            return ExitReason::Signal;
        } else if (hr == HaltReason::IllegalCode) {
            return ExitReason::IllegalCode;
        } else {
            return ExitReason::None;
        }
    }

    ExitReason Step() { return ExitReason::Step; }

    [[nodiscard]] ThreadContext64* GetCPUContext() const {
        return reinterpret_cast<ThreadContext64*>(s_runtime->GetUniformBuffer().data());
    }

    std::shared_ptr<runtime::Runtime> s_runtime{};
    u64 svc_num{};
};

ExitReason Arm64Core::Run() { return impl->Run(); }

ExitReason Arm64Core::Step() { return impl->Step(); }

void Arm64Core::SignalInterrupt() {

}

void Arm64Core::ClearInterrupt() {

}

uint64_t Arm64Core::GetSyscallNumber() { return impl->svc_num; }

}  // namespace swift::arm64