// Directed, Unicorn-free coverage for FSGSBASE, ADX and architectural PKRU.
// Every semantic case runs through both the ARM64 JIT and the interpreter.

#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <sys/mman.h>
#include "distorm.h"
#include "mnemonics.h"
#include "runtime/backend/smc_tracker.h"
#include "translator/x86/cpu.h"
#include "translator/x86/translator.h"

using namespace swift;
using namespace swift::translator::x86;

namespace {

struct ScopedEnv {
    ScopedEnv(const char* name_, const char* value) : name(name_) {
        const char* old = std::getenv(name);
        had = old != nullptr;
        if (had) saved = old;
        if (value) {
            setenv(name, value, 1);
        } else {
            unsetenv(name);
        }
    }
    ~ScopedEnv() {
        if (had) {
            setenv(name, saved.c_str(), 1);
        } else {
            unsetenv(name);
        }
    }
    const char* name;
    bool had{};
    std::string saved;
};

struct Result {
    int exit{};
    ThreadContext64 ctx{};
};

struct Vm {
    static constexpr size_t kSize = 0x200000;
    void* arena{MAP_FAILED};
    u64 base{};
    X86Instance* jit_instance{};
    X86Instance* interp_instance{};
    X86Core* jit{};
    X86Core* interp{};
    u32 slot{1};

    Vm() {
        swift::runtime::backend::SmcTracker::SetEnabled(false);
        arena = mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        REQUIRE(arena != MAP_FAILED);
        base = reinterpret_cast<u64>(arena);
        {
            ScopedEnv jit_on{"SVM_ENABLE_JIT", "1"};
            jit_instance = X86Instance::Make();
        }
        {
            ScopedEnv jit_off{"SVM_ENABLE_JIT", "0"};
            interp_instance = X86Instance::Make();
        }
        jit = X86Core::Make(jit_instance);
        interp = X86Core::Make(interp_instance);
    }

    ~Vm() {
        if (jit) X86Core::Destroy(jit);
        if (interp) X86Core::Destroy(interp);
        if (jit_instance) X86Instance::Destroy(jit_instance);
        if (interp_instance) X86Instance::Destroy(interp_instance);
        if (arena != MAP_FAILED) munmap(arena, kSize);
        swift::runtime::backend::SmcTracker::SetEnabled(true);
    }

    Result RunOne(X86Core* core,
                  const std::vector<u8>& code,
                  u64 address,
                  const std::function<void(ThreadContext64&)>& init) {
        std::memcpy(reinterpret_cast<void*>(address), code.data(), code.size());
        auto& ctx = core->GetContext();
        ctx = ThreadContext64{};
        ctx.rip.qword = address;
        ctx.rsp.qword = base + 0x180000;
        init(ctx);
        Result out;
        out.exit = int(core->Run());
        out.ctx = ctx;
        return out;
    }

    std::pair<Result, Result> Both(
            const std::vector<u8>& code,
            const std::function<void(ThreadContext64&)>& init = [](ThreadContext64&) {}) {
        const u64 jit_addr = base + u64(slot++) * 0x100;
        const u64 interp_addr = base + u64(slot++) * 0x100;
        return {RunOne(jit, code, jit_addr, init), RunOne(interp, code, interp_addr, init)};
    }
};

void CheckSameCoreState(const Result& jit, const Result& interp) {
    CHECK(jit.exit == interp.exit);
    for (u32 i = 0; i < 16; ++i) {
        INFO("register " << i);
        CHECK(jit.ctx.regs[i].qword == interp.ctx.regs[i].qword);
    }
    CHECK(jit.ctx.fs_base == interp.ctx.fs_base);
    CHECK(jit.ctx.gs_base == interp.ctx.gs_base);
    CHECK(jit.ctx.pkru == interp.ctx.pkru);
}

void EmitAdxReg(std::vector<u8>& c, bool adox, u8 dst, u8 src, bool wide = true) {
    c.push_back(adox ? 0xF3 : 0x66);
    c.push_back(u8(0x40 | (wide ? 8 : 0) |
                   (dst >= 8 ? 4 : 0) | (src >= 8 ? 1 : 0)));
    c.insert(c.end(), {0x0F, 0x38, 0xF6});
    c.push_back(u8(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

}  // namespace

TEST_CASE("FSGSBASE distorm probe and shared segment-base state") {
    // Probe evidence for choosing normal dispatch instead of raw decode.
    struct Probe {
        std::vector<u8> bytes;
        u16 opcode;
    };
    const std::vector<Probe> probes = {
            {{0xF3, 0x48, 0x0F, 0xAE, 0xC0}, I_RDFSBASE},
            {{0xF3, 0x48, 0x0F, 0xAE, 0xC8}, I_RDGSBASE},
            {{0xF3, 0x0F, 0xAE, 0xD0}, I_WRFSBASE},
            {{0xF3, 0x48, 0x0F, 0xAE, 0xD8}, I_WRGSBASE},
    };
    for (const auto& p : probes) {
        auto bytes = p.bytes;
        const auto insn = DisDecode(bytes.data(), bytes.size(), 1);
        CHECK(insn.opcode == p.opcode);
        CHECK(insn.size == bytes.size());
        REQUIRE(insn.ops[0].type == O_REG);
    }
    // Pin the snapshot defect that requires FixupFsgsbaseOperand: /2 is
    // incorrectly exposed as RDX and 64-bit, although this encoding names EAX.
    {
        std::vector<u8> bytes = {0xF3, 0x0F, 0xAE, 0xD0};
        const auto raw = DisDecode(bytes.data(), bytes.size(), 1);
        CHECK(raw.opcode == I_WRFSBASE);
        CHECK(raw.ops[0].index == R_RDX);
        CHECK(raw.ops[0].size == 64);
    }

    Vm vm;
    const u64 fs = vm.base + 0x100000;
    const u64 gs = vm.base + 0x110000;
    *reinterpret_cast<u64*>(fs) = UINT64_C(0x0123456789ABCDEF);
    {
        ScopedEnv gate{"SVM_FSGSBASE", "1"};
        // wrfsbase rax; wrgsbase rdx; rdfsbase rcx; rdgsbase rsi;
        // mov rbx, qword ptr fs:[0]; hlt
        const std::vector<u8> code = {
                0xF3, 0x48, 0x0F, 0xAE, 0xD0, 0xF3, 0x48, 0x0F, 0xAE, 0xDA,
                0xF3, 0x48, 0x0F, 0xAE, 0xC1, 0xF3, 0x48, 0x0F, 0xAE, 0xCE,
                0x64, 0x48, 0x8B, 0x1C, 0x25, 0x00, 0x00, 0x00, 0x00, 0xF4};
        const auto [jit, interp] = vm.Both(code, [&](ThreadContext64& ctx) {
            ctx.rax.qword = fs;
            ctx.rdx.qword = gs;
        });
        CheckSameCoreState(jit, interp);
        CHECK(jit.exit == int(swift::translator::None));
        CHECK(jit.ctx.fs_base == fs);
        CHECK(jit.ctx.gs_base == gs);
        CHECK(jit.ctx.rcx.qword == fs);
        CHECK(jit.ctx.rsi.qword == gs);
        CHECK(jit.ctx.rbx.qword == UINT64_C(0x0123456789ABCDEF));

        // The no-REX.W form consumes EAX and zero-extends into the base.
        const std::vector<u8> narrow = {
                0xF3, 0x0F, 0xAE, 0xD0,       // wrfsbase eax
                0xF3, 0x48, 0x0F, 0xAE, 0xC1, // rdfsbase rcx
                0xF3, 0x0F, 0xAE, 0xC2,       // rdfsbase edx
                0xF4};
        const auto [jit32, interp32] = vm.Both(narrow, [](ThreadContext64& ctx) {
            ctx.rax.qword = UINT64_C(0xDEADBEEF12345678);
            ctx.rdx.qword = UINT64_MAX;
        });
        CheckSameCoreState(jit32, interp32);
        CHECK(jit32.ctx.rcx.qword == UINT64_C(0x12345678));
        CHECK(jit32.ctx.rdx.qword == UINT64_C(0x12345678));
    }
    {
        ScopedEnv gate{"SVM_FSGSBASE", nullptr};
        const auto [jit, interp] =
                vm.Both({0xF3, 0x48, 0x0F, 0xAE, 0xC0, 0xF4});  // rdfsbase rax
        CHECK(jit.exit == int(swift::translator::IllegalCode));
        CHECK(interp.exit == int(swift::translator::IllegalCode));
    }
}

TEST_CASE("ADX keeps independent CF and OF chains and supports memory sources") {
    Vm vm;
    ScopedEnv gate{"SVM_ADX", "1"};

    // mov eax,0x7fffffff; add eax,1 => OF=1, CF=0, SF/PF/AF=1, ZF=0.
    // stc => CF=1 without touching OF. Interleaving then proves that ADCX did
    // not destroy OF and ADOX did not destroy CF.
    std::vector<u8> code = {0xB8, 0xFF, 0xFF, 0xFF, 0x7F, 0x83, 0xC0, 0x01, 0xF9};
    EmitAdxReg(code, false, 8, 9);   // adcx r8,r9  : max+0+1 = 0, CF=1
    EmitAdxReg(code, true, 10, 11);  // adox r10,r11: 0+0+1 = 1, OF=0
    EmitAdxReg(code, false, 12, 13); // adcx r12,r13: 0+0+1 = 1, CF=0
    EmitAdxReg(code, true, 14, 15);  // adox r14,r15: max+0+0 = max
    code.push_back(0x9F);            // lahf: capture SF/ZF/AF/PF/CF in AH
    code.insert(code.end(), {0x0F, 0x90, 0xC3, 0xF4});  // seto bl; hlt
    const auto [jit, interp] = vm.Both(code, [](ThreadContext64& ctx) {
        ctx.r8.qword = UINT64_MAX;
        ctx.r9.qword = 0;
        ctx.r10.qword = 0;
        ctx.r11.qword = 0;
        ctx.r12.qword = 0;
        ctx.r13.qword = 0;
        ctx.r14.qword = UINT64_MAX;
        ctx.r15.qword = 0;
    });
    CheckSameCoreState(jit, interp);
    CHECK(jit.exit == int(swift::translator::None));
    CHECK(jit.ctx.r8.qword == 0);
    CHECK(jit.ctx.r10.qword == 1);
    CHECK(jit.ctx.r12.qword == 1);
    CHECK(jit.ctx.r14.qword == UINT64_MAX);
    CHECK(jit.ctx.rbx.low.low.low == 0);   // final OF
    CHECK(jit.ctx.rax.low.low.high == 0x96);  // SF=1 ZF=0 AF=1 PF=1 CF=0

    // Memory r/m64 forms use the same TSO-aware Src path as scalar ALU ops.
    const u64 data = vm.base + 0x120000;
    *reinterpret_cast<u64*>(data) = 0;
    *reinterpret_cast<u64*>(data + 8) = 0;
    const std::vector<u8> memory_code = {
            0xB8, 0xFF, 0xFF, 0xFF, 0x7F, 0x83, 0xC0, 0x01, 0xF9,
            0x66, 0x4C, 0x0F, 0x38, 0xF6, 0x07,        // adcx r8,[rdi]
            0xF3, 0x4C, 0x0F, 0x38, 0xF6, 0x4F, 0x08, // adox r9,[rdi+8]
            0xF4};
    const auto [mem_jit, mem_interp] = vm.Both(memory_code, [&](ThreadContext64& ctx) {
        ctx.rdi.qword = data;
        ctx.r8.qword = UINT64_MAX;
        ctx.r9.qword = 0;
    });
    CheckSameCoreState(mem_jit, mem_interp);
    CHECK(mem_jit.exit == int(swift::translator::None));
    CHECK(mem_jit.ctx.r8.qword == 0);
    CHECK(mem_jit.ctx.r9.qword == 1);

    // The no-REX.W forms operate on 32-bit values and zero-extend the
    // destination GPR, while retaining the same independent carry chains.
    std::vector<u8> narrow = {
            0xB8, 0xFF, 0xFF, 0xFF, 0x7F, 0x83, 0xC0, 0x01, 0xF9};
    EmitAdxReg(narrow, false, 8, 9, false);  // adcx r8d,r9d
    EmitAdxReg(narrow, true, 10, 11, false); // adox r10d,r11d
    narrow.push_back(0xF4);
    const auto [narrow_jit, narrow_interp] =
            vm.Both(narrow, [](ThreadContext64& ctx) {
                ctx.r8.qword = UINT64_C(0xAAAAAAAAFFFFFFFF);
                ctx.r9.qword = 0;
                ctx.r10.qword = UINT64_C(0xBBBBBBBB00000000);
                ctx.r11.qword = 0;
            });
    CheckSameCoreState(narrow_jit, narrow_interp);
    CHECK(narrow_jit.exit == int(swift::translator::None));
    CHECK(narrow_jit.ctx.r8.qword == 0);
    CHECK(narrow_jit.ctx.r10.qword == 1);

    {
        ScopedEnv off{"SVM_ADX", nullptr};
        const auto [off_jit, off_interp] =
                vm.Both({0x66, 0x48, 0x0F, 0x38, 0xF6, 0xC1, 0xF4});
        CHECK(off_jit.exit == int(swift::translator::IllegalCode));
        CHECK(off_interp.exit == int(swift::translator::IllegalCode));
    }
}

TEST_CASE("PKRU register semantics, fault convention, and CPUID non-advertisement") {
    Vm vm;
    // wrpkru; xor eax,eax; rdpkru; hlt
    const std::vector<u8> roundtrip = {
            0x0F, 0x01, 0xEF, 0x31, 0xC0, 0x0F, 0x01, 0xEE, 0xF4};
    const auto [jit, interp] = vm.Both(roundtrip, [](ThreadContext64& ctx) {
        ctx.rax.qword = UINT64_C(0xAAAAAAAA89ABCDEF);
        ctx.rcx.qword = 0;
        ctx.rdx.qword = 0;
    });
    CheckSameCoreState(jit, interp);
    CHECK(jit.exit == int(swift::translator::None));
    CHECK(jit.ctx.pkru == 0x89ABCDEFu);
    CHECK(jit.ctx.rax.qword == 0x89ABCDEFu);
    CHECK(jit.ctx.rdx.qword == 0);

    const auto [bad_jit, bad_interp] =
            vm.Both({0x0F, 0x01, 0xEF, 0xF4}, [](ThreadContext64& ctx) {
                ctx.pkru = 0x13579BDFu;
                ctx.rax.qword = 0x2468ACE0u;
                ctx.rcx.qword = 1;  // architectural #GP
                ctx.rdx.qword = 0;
            });
    CHECK(bad_jit.exit == int(swift::translator::IllegalCode));
    CHECK(bad_interp.exit == int(swift::translator::IllegalCode));
    CHECK(bad_jit.ctx.pkru == 0x13579BDFu);
    CHECK(bad_interp.ctx.pkru == 0x13579BDFu);

    // CPUID leaf 7 ECX.PKU remains clear even though the register instructions
    // are accepted unconditionally: there is no pkey access enforcement.
    const auto [cpuid_jit, cpuid_interp] =
            vm.Both({0x0F, 0xA2, 0xF4}, [](ThreadContext64& ctx) {
                ctx.rax.qword = 7;
                ctx.rcx.qword = 0;
            });
    CheckSameCoreState(cpuid_jit, cpuid_interp);
    CHECK((cpuid_jit.ctx.rcx.qword & (1u << 3)) == 0);
}

TEST_CASE("FSGSBASE and ADX CPUID bits follow only their gates") {
    Vm vm;
    const auto cpuid7 = [&](const char* fsgs, const char* adx) {
        ScopedEnv fsgs_gate{"SVM_FSGSBASE", fsgs};
        ScopedEnv adx_gate{"SVM_ADX", adx};
        return vm.Both({0x0F, 0xA2, 0xF4}, [](ThreadContext64& ctx) {
            ctx.rax.qword = 7;
            ctx.rcx.qword = 0;
        });
    };
    {
        const auto [jit, interp] = cpuid7(nullptr, nullptr);
        CheckSameCoreState(jit, interp);
        CHECK((jit.ctx.rbx.qword & (1u << 0)) == 0);
        CHECK((jit.ctx.rbx.qword & (1u << 19)) == 0);
    }
    {
        const auto [jit, interp] = cpuid7("1", "1");
        CheckSameCoreState(jit, interp);
        CHECK((jit.ctx.rbx.qword & (1u << 0)) != 0);
        CHECK((jit.ctx.rbx.qword & (1u << 19)) != 0);
    }
}
