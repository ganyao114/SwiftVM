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

TEST_CASE("resident vector publications keep precise unaligned load faults",
          "[fpr-publication][fault]") {
    using namespace swift::runtime;
    using namespace swift::translator;
    using namespace swift::translator::x86;

    if (!GetSvmConfig().xmm_resident) {
        SUCCEED("fault integration is enabled by SVM_XMM_RESIDENT=1");
        return;
    }
    const long page_long = sysconf(_SC_PAGESIZE);
    REQUIRE(page_long > 0);
    const auto page = static_cast<size_t>(page_long);
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        CAPTURE(scenario);
        auto* data = static_cast<swift::u8*>(
                mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON, -1, 0));
        auto* code = static_cast<swift::u8*>(
                mmap(nullptr, page, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON, -1, 0));
        REQUIRE(data != MAP_FAILED);
        REQUIRE(code != MAP_FAILED);
        // Aligned, unaligned, readable page crossing, then two faulting page
        // crossings. The last first commits PANDN to the load's destination.
        auto* input = data + (scenario == 0 ? 32 : scenario == 1 ? 33 : page - 8);
        std::array<swift::u8, 16> loaded{};
        for (size_t byte = 0; byte < loaded.size(); ++byte)
            loaded[byte] = static_cast<swift::u8>(byte * 19u + 0x35u);
        std::memcpy(input, loaded.data(), loaded.size());
        const bool faults = scenario >= 3;
        if (faults) REQUIRE(mprotect(data + page, page, PROT_NONE) == 0);

        // [pandn %xmm1,%xmm0;] movdqu (%rax),%xmm0; hlt.
        // MOVDQU permits an unaligned guest access; protection of the second
        // page, rather than guest alignment, must be the failure in cases3/4.
        const std::array<swift::u8, 9> program{
                0x66, 0x0f, 0xdf, 0xc1, 0xf3, 0x0f, 0x6f, 0x00, 0xf4};
        const bool prior_publication = scenario == 4;
        std::memcpy(code, program.data() + (prior_publication ? 0 : 4),
                    prior_publication ? program.size() : program.size() - 4);

        const bool smc_enabled = backend::SmcTracker::IsEnabled();
        backend::SmcTracker::SetEnabled(false);
        auto* instance = X86Instance::Make();
        INFO("TSO mode=" << static_cast<unsigned>(
                     instance->GetAddressSpace()->GetConfig().tso_mode));
        auto* core = X86Core::Make(instance);
        auto& state = core->GetContext();
        state.rip.qword = reinterpret_cast<swift::u64>(code);
        state.rax.qword = reinterpret_cast<swift::u64>(input);
        std::array<std::array<swift::u8, 16>, 16> initial{};
        for (size_t index = 0; index < initial.size(); ++index) {
            for (size_t byte = 0; byte < initial[index].size(); ++byte)
                initial[index][byte] = static_cast<swift::u8>(index * 29u + byte * 7u + 3u);
            std::memcpy(&state.xmms[index], initial[index].data(), initial[index].size());
        }
        auto expected = initial;
        if (!faults) {
            expected[0] = loaded;
        } else if (prior_publication) {
            for (size_t byte = 0; byte < expected[0].size(); ++byte)
                expected[0][byte] = static_cast<swift::u8>(~initial[0][byte] & initial[1][byte]);
        }
        const auto exit = core->Run();
        CHECK(exit == (faults ? ExitReason::PageFatal : ExitReason::None));
        for (size_t index = 0; index < expected.size(); ++index) {
            CAPTURE(index);
            CHECK(std::memcmp(&state.xmms[index], expected[index].data(),
                              expected[index].size()) == 0);
        }
        X86Core::Destroy(core);
        X86Instance::Destroy(instance);
        backend::SmcTracker::SetEnabled(smc_enabled);
        CHECK(munmap(data, 2 * page) == 0);
        CHECK(munmap(code, page) == 0);
    }
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

TEST_CASE("loop prefix faults publish the preceding DEC arithmetic flags",
          "[inc-dec-local-flags][fault][fault-flags]") {
#if defined(__aarch64__)
    using namespace swift::runtime;
    using namespace swift::translator;
    using namespace swift::translator::x86;
    using namespace swift::x86;
    const long page_long = sysconf(_SC_PAGESIZE);
    REQUIRE(page_long > 0);
    const auto page = static_cast<size_t>(page_long);
    REQUIRE(page <= INT32_MAX);
    auto* data = static_cast<swift::u8*>(mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANON, -1, 0));
    auto* code = static_cast<swift::u8*>(mmap(nullptr, page, PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANON, -1, 0));
    REQUIRE(data != MAP_FAILED);
    REQUIRE(code != MAP_FAILED);
    struct Mapping {
        swift::u8* data;
        swift::u8* code;
        size_t page;
        bool smc;
        ~Mapping() {
            munmap(data, 2 * page);
            munmap(code, page);
            backend::SmcTracker::SetEnabled(smc);
        }
    } cleanup{data, code, page, backend::SmcTracker::IsEnabled()};
    std::array<swift::u8, 16> loaded{};
    for (size_t byte = 0; byte < loaded.size(); ++byte)
        loaded[byte] = static_cast<swift::u8>(byte * 19u + 0x35u);
    std::memcpy(data, loaded.data(), loaded.size());
    REQUIRE(mprotect(data + page, page, PROT_NONE) == 0);
    backend::SmcTracker::SetEnabled(false);
    std::unique_ptr<X86Instance, decltype(&X86Instance::Destroy)> instance{
            X86Instance::Make(), &X86Instance::Destroy};
    INFO("TSO mode=" << static_cast<unsigned>(instance->GetAddressSpace()->GetConfig().tso_mode));
    // Isolated guest entry points seed and read the native flag carrier.
    // ThreadContext64::ef is a uniform cache, not the PageFatal flag result.
    const std::array<swift::u8, 2> seed_program{0x9d, 0xf4}; // popfq; hlt
    const std::array<swift::u8, 9> readback_program{
            0x9f, 0x41, 0x0f, 0x90, 0xc0, 0x0f, 0xb6, 0xdc, 0xf4}; // lahf; seto r8b; movzx ebx,ah; hlt
    auto* seed_entry = code + 128;
    auto* readback_entry = code + 160;
    REQUIRE(160 + readback_program.size() < page);
    std::memcpy(seed_entry, seed_program.data(), seed_program.size());
    std::memcpy(readback_entry, readback_program.data(), readback_program.size());
    auto* seed_word = data + page - sizeof(swift::u64);
    for (bool subtract : {false, true}) for (unsigned width : {32u, 64u}) {
        // movdqu xmm0,[rdx]; lea rdx,[rdx+page]; dec eax/rax; jne loop;
        // xor esi,esi; hlt. Both ordinary successors eventually overwrite
        // flags, but the next loop load must still observe the preceding DEC.
        // The separate SUB variant also has a potentially faulting cold load
        // before XOR, so neither successor can claim an unobserved overwrite.
        std::vector<swift::u8> program{0xf3, 0x0f, 0x6f, 0x02, 0x48, 0x8d, 0x92};
        for (unsigned byte = 0; byte < 4; ++byte)
            program.push_back(static_cast<swift::u8>(page >> (8 * byte)));
        if (width == 64) program.push_back(0x48);
        if (subtract) program.insert(program.end(), {0x83, 0xe8, 1, 0x75}); // sub eax/rax,1
        else program.insert(program.end(), {0xff, 0xc8, 0x75});
        program.push_back(static_cast<swift::u8>(-static_cast<int>(program.size() + 1)));
        if (subtract) program.insert(program.end(), {0xf3, 0x0f, 0x6f, 0x0f}); // movdqu xmm1,[rdi]
        program.insert(program.end(), {0x31, 0xf6, 0xf4});
        auto* entry = code + (subtract ? 256 : 0) + (width == 64 ? 64 : 0);
        REQUIRE(static_cast<size_t>(entry - code) + program.size() < page);
        std::memcpy(entry, program.data(), program.size());
        const swift::u64 sign = swift::u64{1} << (width - 1);
        for (swift::u64 count : {swift::u64{2}, swift::u64{16}, sign})
        for (bool carry : {false, true}) for (bool first_fault : {false, true}) {
            CAPTURE(subtract, width, count, carry, first_fault);
            const auto result = count - 1;
            EFlags decremented{};
            decremented.cf = subtract ? false : carry;
            decremented.pf = (std::popcount(static_cast<swift::u8>(result)) & 1) == 0;
            decremented.af = ((count ^ result ^ 1) & 0x10) != 0;
            decremented.zf = result == 0;
            decremented.sf = (result & sign) != 0;
            decremented.of = count == sign;
            EFlags incoming = decremented;
            incoming.cf = carry;
            incoming.pf = !*decremented.pf;
            incoming.af = !*decremented.af;
            incoming.zf = !*decremented.zf;
            incoming.sf = !*decremented.sf;
            incoming.of = !*decremented.of;
            std::unique_ptr<X86Core, decltype(&X86Core::Destroy)> core{
                    X86Core::Make(instance.get()), &X86Core::Destroy};
            auto& state = core->GetContext();
            state.rip.qword = reinterpret_cast<swift::u64>(seed_entry);
            const auto original = width == 32 ? UINT64_C(0x6b5a000000000000) | count : count;
            state.rax.qword = original;
            state.rbx.qword = UINT64_MAX;
            state.rdx.qword = reinterpret_cast<swift::u64>(data + (first_fault ? page : 0));
            if (subtract) state.rdi.qword = reinterpret_cast<swift::u64>(data);
            state.rsp.qword = reinterpret_cast<swift::u64>(seed_word);
            state.rsi.qword = UINT64_C(0x123456789abcdef0);
            state.r8.qword = 2;
            const swift::u64 raw_flags = 0x202 | *incoming.cf | (*incoming.pf << 2) |
                                         (*incoming.af << 4) | (*incoming.zf << 6) |
                                         (*incoming.sf << 7) | (*incoming.of << 11);
            std::memcpy(seed_word, &raw_flags, sizeof(raw_flags));
            std::array<std::array<swift::u8, 16>, 16> expected{};
            for (size_t index = 0; index < expected.size(); ++index) {
                for (size_t byte = 0; byte < expected[index].size(); ++byte)
                    expected[index][byte] = static_cast<swift::u8>(index * 29u + byte * 7u + 3u);
                std::memcpy(&state.xmms[index], expected[index].data(), expected[index].size());
            }
            if (!first_fault) expected[0] = loaded;
            const auto flags = first_fault ? incoming : decremented;
            // Completing this separate HLT makes all six seeded flags live,
            // independent of optimization of the following loop.
            REQUIRE(core->Run() == ExitReason::None);
            REQUIRE(state.rsp.qword == reinterpret_cast<swift::u64>(data + page));
            state.rip.qword = reinterpret_cast<swift::u64>(entry);
            const auto fault_exit = core->Run();
            CHECK(fault_exit == ExitReason::PageFatal);
            CHECK(state.rip.qword == reinterpret_cast<swift::u64>(entry));
            CHECK(state.rax.qword == (first_fault ? original : result));
            CHECK(state.rdx.qword == reinterpret_cast<swift::u64>(data + page));
            if (subtract) CHECK(state.rdi.qword == reinterpret_cast<swift::u64>(data));
            CHECK(state.rsi.qword == UINT64_C(0x123456789abcdef0));
            for (size_t index = 0; index < expected.size(); ++index) {
                CAPTURE(index);
                CHECK(std::memcmp(&state.xmms[index], expected[index].data(),
                                  expected[index].size()) == 0);
            }
            // Re-enter the same core without a flags writer or a second POPFQ.
            // LAHF/SETO therefore read precisely the carrier preserved on exit.
            state.rip.qword = reinterpret_cast<swift::u64>(readback_entry);
            const auto readback_exit = core->Run();
            CHECK(readback_exit == ExitReason::None);
            CHECK((state.rbx.qword & 1) == *flags.cf);
            CHECK(((state.rbx.qword >> 2) & 1) == *flags.pf);
            CHECK(((state.rbx.qword >> 4) & 1) == *flags.af);
            CHECK(((state.rbx.qword >> 6) & 1) == *flags.zf);
            CHECK(((state.rbx.qword >> 7) & 1) == *flags.sf);
            CHECK((state.r8.qword & 0xff) == *flags.of);
            CHECK((state.rbx.qword & ~swift::u64{0xd5}) == 2);
        }
    }
#endif
}

TEST_CASE("foreign loop entries preserve predecessor flags at a prefix fault",
          "[inc-dec-local-flags][fault][fault-flags][foreign-entry-flags]") {
#if defined(__aarch64__)
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::translator;
    using namespace swift::translator::x86;
    using namespace swift::x86;
    const long page_long = sysconf(_SC_PAGESIZE);
    REQUIRE(page_long >= 4096);
    const auto page = static_cast<size_t>(page_long);
    REQUIRE(page <= INT32_MAX);
    auto* data = static_cast<swift::u8*>(mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANON, -1, 0));
    auto* code = static_cast<swift::u8*>(mmap(nullptr, page, PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANON, -1, 0));
    REQUIRE(data != MAP_FAILED);
    REQUIRE(code != MAP_FAILED);
    struct Mapping {
        swift::u8* data;
        swift::u8* code;
        size_t page;
        bool smc;
        ~Mapping() {
            munmap(data, 2 * page); munmap(code, page);
            backend::SmcTracker::SetEnabled(smc);
        }
    } cleanup{data, code, page, backend::SmcTracker::IsEnabled()};
    REQUIRE(mprotect(data + page, page, PROT_NONE) == 0);
    std::memset(code, 0x90, page);
    const std::array<swift::u8, 2> seed_program{0x9d, 0xf4};
    const std::array<swift::u8, 9> readback_program{
            0x9f, 0x41, 0x0f, 0x90, 0xc0, 0x0f, 0xb6, 0xdc, 0xf4};
    auto* seed_entry = code + 2048;
    auto* readback_entry = code + 2080;
    std::memcpy(seed_entry, seed_program.data(), seed_program.size());
    std::memcpy(readback_entry, readback_program.data(), readback_program.size());
    auto* seed_word = data + page - sizeof(swift::u64);
    backend::SmcTracker::SetEnabled(false);
    std::unique_ptr<X86Instance, decltype(&X86Instance::Destroy)> instance{
            X86Instance::Make(), &X86Instance::Destroy};
    auto* space = instance->GetAddressSpace();
    REQUIRE(space->GetConfig().enable_jit);
    for (unsigned width : {32u, 64u}) for (bool chain : {false, true}) {
        auto* entry = code + (width == 64 ? 512 : 0) + (chain ? 256 : 0);
        auto* predecessor = entry + (chain ? 4 : 2);
        auto* relay = entry + 32;
        auto* loop = entry + 64;
        // JRCXZ predeclares the loop before the ADD's JMP is decoded. RCX=1
        // takes the ADD fallthrough. A second JRCXZ predeclares the optional
        // relay, so every JMP names an existing HIR block rather than dispatch.
        entry[0] = 0xe3; entry[1] = 62;
        if (chain) { entry[2] = 0xe3; entry[3] = 28; }
        const std::array<swift::u8, 4> add{swift::u8(width == 64 ? 0x49 : 0x41),
                                           0x83, 0xc1, 1}; // add r9d/r9,1
        std::memcpy(predecessor, add.data(), add.size());
        predecessor[4] = 0xeb;
        predecessor[5] = static_cast<swift::u8>((chain ? relay : loop) - (predecessor + 6));
        relay[0] = 0x90; relay[1] = 0xeb; relay[2] = 29; // nop; jmp loop
        std::vector<swift::u8> body{0xf3, 0x0f, 0x6f, 0x02, 0x48, 0x8d, 0x92};
        for (unsigned byte = 0; byte < 4; ++byte)
            body.push_back(static_cast<swift::u8>(page >> (8 * byte)));
        if (width == 64) body.push_back(0x48);
        body.insert(body.end(), {0x83, 0xe8, 1, 0x75}); // sub eax/rax,1; jne loop
        body.push_back(static_cast<swift::u8>(-static_cast<int>(body.size() + 1)));
        body.insert(body.end(), {0xf3, 0x0f, 0x6f, 0x0f, 0x31, 0xf6, 0xf4});
        std::memcpy(loop, body.data(), body.size());
        const auto mask = width == 64 ? UINT64_MAX : swift::u64{UINT32_MAX};
        const auto sign = swift::u64{1} << (width - 1);
        for (swift::u64 input : {swift::u64{0}, mask, sign - 1})
        for (bool from_predecessor : {true, false}) {
            CAPTURE(width, chain, input, from_predecessor);
            const auto result = (input + 1) & mask;
            EFlags added{};
            added.cf = input == mask;
            added.pf = (std::popcount(static_cast<swift::u8>(result)) & 1) == 0;
            added.af = ((input ^ result ^ 1) & 0x10) != 0;
            added.zf = result == 0; added.sf = (result & sign) != 0; added.of = input == sign - 1;
            EFlags initial{};
            initial.cf = !*added.cf; initial.pf = !*added.pf; initial.af = !*added.af;
            initial.zf = !*added.zf; initial.sf = !*added.sf; initial.of = !*added.of;
            const swift::u64 raw_flags = 0x202 | *initial.cf | (*initial.pf << 2) |
                    (*initial.af << 4) | (*initial.zf << 6) | (*initial.sf << 7) | (*initial.of << 11);
            std::memcpy(seed_word, &raw_flags, sizeof(raw_flags));
            std::unique_ptr<X86Core, decltype(&X86Core::Destroy)> core{
                    X86Core::Make(instance.get()), &X86Core::Destroy};
            auto& state = core->GetContext();
            state.rip.qword = reinterpret_cast<swift::u64>(seed_entry);
            state.rsp.qword = reinterpret_cast<swift::u64>(seed_word);
            const auto counter = width == 32 ? UINT64_C(0x6b5a000000000002) : swift::u64{2};
            const auto original_r9 = width == 32 ? UINT64_C(0x6b5a000000000000) | input : input;
            state.rax.qword = counter; state.r9.qword = original_r9;
            state.rcx.qword = 1; state.rbx.qword = UINT64_MAX; state.r8.qword = 2;
            state.rdx.qword = reinterpret_cast<swift::u64>(data + page);
            state.rdi.qword = reinterpret_cast<swift::u64>(data);
            state.rsi.qword = UINT64_C(0x123456789abcdef0);
            std::array<std::array<swift::u8, 16>, 16> expected_xmm{};
            for (size_t index = 0; index < expected_xmm.size(); ++index) {
                for (size_t byte = 0; byte < expected_xmm[index].size(); ++byte)
                    expected_xmm[index][byte] = static_cast<swift::u8>(index * 29u + byte * 7u + 3u);
                std::memcpy(&state.xmms[index], expected_xmm[index].data(), expected_xmm[index].size());
            }
            REQUIRE(core->Run() == ExitReason::None);
            REQUIRE(state.rsp.qword == reinterpret_cast<swift::u64>(data + page));
            state.rip.qword = reinterpret_cast<swift::u64>(from_predecessor ? entry : loop);
            const auto fault_exit = core->Run();
            CHECK(fault_exit == ExitReason::PageFatal);
            CHECK(state.rip.qword == reinterpret_cast<swift::u64>(loop));
            CHECK(state.rax.qword == counter);
            CHECK(state.r9.qword == (from_predecessor ? result : original_r9));
            CHECK(state.rcx.qword == 1);
            CHECK(state.rdx.qword == reinterpret_cast<swift::u64>(data + page));
            CHECK(state.rdi.qword == reinterpret_cast<swift::u64>(data));
            CHECK(state.rsi.qword == UINT64_C(0x123456789abcdef0));
            for (size_t index = 0; index < expected_xmm.size(); ++index) {
                CAPTURE(index);
                CHECK(std::memcmp(&state.xmms[index], expected_xmm[index].data(), expected_xmm[index].size()) == 0);
            }
            if (GetSvmConfig().func_base) {
                // Inspect actual published owners, even when FuncIRFree has
                // released instructions. LinkBlock terminals contain no freed Value.
                auto module = space->GetModule(reinterpret_cast<swift::u64>(entry));
                auto node = module->GetNode(Location{reinterpret_cast<swift::u64>(entry)});
                REQUIRE(backend::IsFunction(node));
                auto function = backend::GetFunction(node);
                auto guard = function->LockRead();
                const auto* foreign = function->FindBlock(Location{reinterpret_cast<swift::u64>(predecessor)});
                REQUIRE(foreign);
                REQUIRE(function->FindBlock(Location{reinterpret_cast<swift::u64>(loop)}));
                const auto require_link = [&](const Block* block, const swift::u8* target) {
                    REQUIRE(block);
                    bool matches = false;
                    VisitVariant<void>(block->GetTerminal(), [&](const auto& term) {
                        if constexpr (std::is_same_v<std::decay_t<decltype(term)>, terminal::LinkBlock> ||
                                      std::is_same_v<std::decay_t<decltype(term)>, terminal::LinkBlockFast>)
                            matches = term.next.Value() == reinterpret_cast<swift::u64>(target);
                    });
                    REQUIRE(matches);
                };
                require_link(foreign, chain ? relay : loop);
                if (chain) require_link(function->FindBlock(Location{reinterpret_cast<swift::u64>(relay)}), loop);
                REQUIRE(space->GetConfig().region_edges);
                if (ResolveFeatureSet(module->GetModuleConfig()).flags_loop_lazy && FlagsRegsEnabled()) {
                    backend::FaultEntry allocation{};
                    std::vector<backend::FaultEntry> faults;
                    const auto* host = static_cast<const swift::u8*>(space->GetCodeCache(Location{reinterpret_cast<swift::u64>(entry)}));
                    REQUIRE(host);
                    REQUIRE(module->LookupCodeAllocation(host, allocation, &faults));
                    REQUIRE(std::any_of(faults.begin(), faults.end(), [&](const auto& fault) {
                        return fault.guest_loc == reinterpret_cast<swift::u64>(loop) && fault.recovery &&
                               fault.recovery_kind == backend::FaultRecoveryKind::GuestFault &&
                               fault.host_end - fault.host_start > static_cast<ptrdiff_t>(sizeof(swift::u32));
                    }));
                }
            }
            // Public re-entry restores the fault's authoritative State flags.
            state.rip.qword = reinterpret_cast<swift::u64>(readback_entry);
            const auto readback_exit = core->Run();
            CHECK(readback_exit == ExitReason::None);
            const auto flags = from_predecessor ? added : initial;
            CHECK((state.rbx.qword & 1) == *flags.cf);
            CHECK(((state.rbx.qword >> 2) & 1) == *flags.pf);
            CHECK(((state.rbx.qword >> 4) & 1) == *flags.af);
            CHECK(((state.rbx.qword >> 6) & 1) == *flags.zf);
            CHECK(((state.rbx.qword >> 7) & 1) == *flags.sf);
            CHECK((state.r8.qword & 0xff) == *flags.of);
            CHECK((state.rbx.qword & ~swift::u64{0xd5}) == 2);
        }
    }
#endif
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
