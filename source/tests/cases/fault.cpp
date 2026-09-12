#include "../support/case_support.h"

TEST_CASE("resident register homes survive a guest page fault") {
    using namespace swift::runtime;
    using namespace swift::translator::x86;

    if (!GetSvmConfig().xmm_resident) {
        SUCCEED("fault integration is enabled by SVM_XMM_RESIDENT=1");
        return;
    }
    const long page_long = sysconf(_SC_PAGESIZE);
    REQUIRE(page_long > 0);
    const auto page = static_cast<size_t>(page_long);
    auto* data = static_cast<swift::u8*>(
            mmap(nullptr, page, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0));
    auto* code = static_cast<swift::u8*>(
            mmap(nullptr, page, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON, -1, 0));
    REQUIRE(data != MAP_FAILED);
    REQUIRE(code != MAP_FAILED);
    // mov (%rax),%rbx; hlt
    const std::array<swift::u8, 4> guest{0x48, 0x8b, 0x18, 0xf4};
    std::memcpy(code, guest.data(), guest.size());

    backend::SmcTracker::SetEnabled(false);
    auto* instance = X86Instance::Make();
    auto* core = X86Core::Make(instance);
    auto& state = core->GetContext();
    state.rip.qword = reinterpret_cast<swift::u64>(code);
    state.rax.qword = reinterpret_cast<swift::u64>(data);
    constexpr swift::u64 expected_rbx = 0x123456789abcdef0ull;
    state.rbx.qword = expected_rbx;
    std::array<std::array<swift::u8, 16>, 16> expected{};
    for (swift::u32 index = 0; index < expected.size(); ++index) {
        for (swift::u32 byte = 0; byte < expected[index].size(); ++byte) {
            expected[index][byte] = static_cast<swift::u8>(index * 29u + byte * 7u + 3u);
        }
        std::memcpy(&state.xmms[index], expected[index].data(), expected[index].size());
    }
    REQUIRE(core->Run() == swift::translator::ExitReason::PageFatal);
    REQUIRE(state.rbx.qword == expected_rbx);
    for (swift::u32 index = 0; index < expected.size(); ++index) {
        CAPTURE(index);
        REQUIRE(std::memcmp(&state.xmms[index], expected[index].data(),
                            expected[index].size()) == 0);
    }
    X86Core::Destroy(core);
    X86Instance::Destroy(instance);
    backend::SmcTracker::SetEnabled(true);
    munmap(data, page);
    munmap(code, page);
}

TEST_CASE("fault capture publishes the latest XMM arithmetic result") {
    using namespace swift::translator;
    using namespace swift::translator::x86;

    const long page_long = sysconf(_SC_PAGESIZE);
    REQUIRE(page_long > 0);
    const auto page = static_cast<size_t>(page_long);
    auto* input = static_cast<swift::u8*>(
            mmap(nullptr, page, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON, -1, 0));
    auto* fault = static_cast<swift::u8*>(
            mmap(nullptr, page, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0));
    auto* code = static_cast<swift::u8*>(
            mmap(nullptr, page, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON, -1, 0));
    REQUIRE(input != MAP_FAILED);
    REQUIRE(fault != MAP_FAILED);
    REQUIRE(code != MAP_FAILED);

    const std::array<double, 2> raw{1.5, -4.0};
    const std::array<double, 2> scalar{2.0, 3.0};
    const std::array<double, 2> expected{3.0, -12.0};
    std::memcpy(input, raw.data(), sizeof(raw));

    // movapd (%rbx),%xmm0; mulpd %xmm1,%xmm0;
    // addpd (%rax),%xmm0; hlt.  The addpd RHS faults after mulpd has committed.
    const std::array<swift::u8, 13> guest{
            0x66, 0x0f, 0x28, 0x03,
            0x66, 0x0f, 0x59, 0xc1,
            0x66, 0x0f, 0x58, 0x00,
            0xf4,
    };
    std::memcpy(code, guest.data(), guest.size());

    backend::SmcTracker::SetEnabled(false);
    auto* instance = X86Instance::Make();
    auto* core = X86Core::Make(instance);
    auto& state = core->GetContext();
    state.rip.qword = reinterpret_cast<swift::u64>(code);
    state.rax.qword = reinterpret_cast<swift::u64>(fault);
    state.rbx.qword = reinterpret_cast<swift::u64>(input);
    std::memcpy(&state.xmm1, scalar.data(), sizeof(scalar));

    REQUIRE(core->Run() == ExitReason::PageFatal);
    std::array<double, 2> actual{};
    std::memcpy(actual.data(), &state.xmm0, sizeof(actual));
    CAPTURE(raw[0], raw[1], expected[0], expected[1], actual[0], actual[1]);
    REQUIRE(std::memcmp(actual.data(), expected.data(), sizeof(expected)) == 0);

    X86Core::Destroy(core);
    X86Instance::Destroy(instance);
    backend::SmcTracker::SetEnabled(true);
    munmap(input, page);
    munmap(fault, page);
    munmap(code, page);
}

TEST_CASE("fault capture preserves committed W GPR high-zero state") {
    using namespace swift::translator;
    using namespace swift::translator::x86;

    const long page_long = sysconf(_SC_PAGESIZE);
    REQUIRE(page_long > 0);
    const auto page = static_cast<size_t>(page_long);
    auto* fault = static_cast<swift::u8*>(
            mmap(nullptr, page, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0));
    auto* code = static_cast<swift::u8*>(
            mmap(nullptr, page, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON, -1, 0));
    REQUIRE(fault != MAP_FAILED);
    REQUIRE(code != MAP_FAILED);

    const std::array<swift::u8, 9> guest{
            0xb8, 0xef, 0xcd, 0xab, 0x89,
            0x48, 0x8b, 0x1a,
            0xf4,
    };
    std::memcpy(code, guest.data(), guest.size());

    backend::SmcTracker::SetEnabled(false);
    auto* instance = X86Instance::Make();
    auto* core = X86Core::Make(instance);
    auto& state = core->GetContext();
    state.rip.qword = reinterpret_cast<swift::u64>(code);
    state.rax.qword = UINT64_C(0xfedcba9876543210);
    state.rbx.qword = UINT64_C(0x123456789abcdef0);
    state.rdx.qword = reinterpret_cast<swift::u64>(fault);

    REQUIRE(core->Run() == ExitReason::PageFatal);
    REQUIRE(state.rax.qword == UINT64_C(0x0000000089abcdef));
    REQUIRE(state.rbx.qword == UINT64_C(0x123456789abcdef0));

    X86Core::Destroy(core);
    X86Instance::Destroy(instance);
    backend::SmcTracker::SetEnabled(true);
    munmap(fault, page);
    munmap(code, page);
}

TEST_CASE("integer width chains keep X high halves zero for W and X consumers") {
    using namespace swift::x86;

    struct Program {
        std::vector<swift::u8> bytes;
        bool memory_source;
    };
    const std::array programs{
            // mov eax,ecx; add eax,edx; shr eax,3; mov r8,rax; hlt
            Program{{0x89, 0xc8, 0x01, 0xd0, 0xc1, 0xe8, 0x03,
                     0x49, 0x89, 0xc0, 0xf4}, false},
            // mov eax,[rsi]; add eax,edx; shl eax,1; mov r8d,eax;
            // mov r9,r8; hlt. The W consumer must clear r8's old high half
            // before the following X consumer observes it.
            Program{{0x8b, 0x06, 0x01, 0xd0, 0xd1, 0xe0,
                     0x41, 0x89, 0xc0, 0x4d, 0x89, 0xc1, 0xf4}, true},
    };
    const std::array upper_patterns{
            UINT64_C(0x0000000000000000),
            UINT64_C(0xffffffffffffffff),
            UINT64_C(0xa5a5f00d5a5a1234),
    };

    void* guest_code = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
    void* guest_data = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(guest_code != MAP_FAILED);
    REQUIRE(guest_data != MAP_FAILED);
    *reinterpret_cast<swift::u32*>(guest_data) = 0xf13579bdu;

    backend::SmcTracker::SetEnabled(false);
    auto* instance = swift::translator::x86::X86Instance::Make();
    for (std::size_t program_index = 0; program_index < programs.size(); ++program_index) {
        const auto& program = programs[program_index];
        auto* program_code = static_cast<swift::u8*>(guest_code) + program_index * 64;
        std::memcpy(program_code, program.bytes.data(), program.bytes.size());
        for (auto upper : upper_patterns) {
            INFO("memory source=" << program.memory_source << " upper=0x" << std::hex
                                  << upper);
            auto* core = swift::translator::x86::X86Core::Make(instance);
            auto& context = core->GetContext();
            context.rip.qword = reinterpret_cast<VAddr>(program_code);
            context.rax.qword = upper;
            context.rcx.qword = upper ^ UINT64_C(0x0123456789abcdef);
            context.rdx.qword = upper ^ UINT64_C(0xfedcba9876543210);
            context.rsi.qword = reinterpret_cast<VAddr>(guest_data);
            context.r8.qword = upper;
            context.r9.qword = upper;
            core->Run();

            const swift::u32 left = program.memory_source
                    ? *reinterpret_cast<swift::u32*>(guest_data)
                    : static_cast<swift::u32>(upper ^ UINT64_C(0x0123456789abcdef));
            const swift::u32 sum = left +
                    static_cast<swift::u32>(upper ^ UINT64_C(0xfedcba9876543210));
            const swift::u64 expected = program.memory_source
                    ? static_cast<swift::u32>(sum << 1)
                    : static_cast<swift::u32>(sum >> 3);
            REQUIRE(context.rax.qword == expected);
            REQUIRE(context.r8.qword == expected);
            if (program.memory_source) {
                REQUIRE(context.r9.qword == expected);
            }
            swift::translator::x86::X86Core::Destroy(core);
        }
    }
    swift::translator::x86::X86Instance::Destroy(instance);
    backend::SmcTracker::SetEnabled(true);
    munmap(guest_data, 4096);
    munmap(guest_code, 4096);
}
