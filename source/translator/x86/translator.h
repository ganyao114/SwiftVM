//
// Created by 甘尧 on 2024/6/21.
//

#include "cpu.h"
#include "translator/runtime.h"

namespace swift::translator::x86 {
using namespace swift::x86;
class X86Core;

class X86Instance : public Instance {
public:
    friend class X86Core;

    static X86Instance *Make();
    static void Destroy(X86Instance *instance);
private:
    explicit X86Instance();

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
