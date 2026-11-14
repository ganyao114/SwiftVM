#include "runtime/execution/vm.h"
#include "runtime/platform/linux/abi_arm32.h"
#include "runtime/platform/linux/host_runtime.h"
#include "runtime/platform/linux/signal_frame.h"
#include "tests/support/check.h"
#include "tests/support/test_mode.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <elf.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {
using namespace swift::runtime;
namespace exe = execution;
namespace linux32 = platform::abi32;
constexpr std::uint32_t Base = 0x20000000, Entry = Base + 0x100;

std::vector<std::uint8_t> Image(std::span<const std::uint8_t> code, bool thumb = false) {
    std::vector<std::uint8_t> bytes(0x100 + code.size());
    Elf32_Ehdr header{};
    std::memcpy(header.e_ident, ELFMAG, SELFMAG);
    header.e_ident[EI_CLASS] = ELFCLASS32;
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_ident[EI_VERSION] = EV_CURRENT;
    header.e_type = ET_EXEC;
    header.e_machine = EM_ARM;
    header.e_version = EV_CURRENT;
    header.e_entry = Entry | unsigned(thumb);
    header.e_phoff = sizeof(header);
    header.e_ehsize = sizeof(header);
    header.e_phentsize = sizeof(Elf32_Phdr);
    header.e_phnum = 1;
    header.e_flags = 0x05000000;
    Elf32_Phdr segment{};
    segment.p_type = PT_LOAD;
    segment.p_vaddr = segment.p_paddr = Base;
    segment.p_filesz = segment.p_memsz = bytes.size();
    segment.p_flags = PF_R | PF_X;
    segment.p_align = 4096;
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), &segment, sizeof(segment));
    std::memcpy(bytes.data() + 0x100, code.data(), code.size());
    return bytes;
}

std::vector<std::uint8_t> Interworking() {
    constexpr std::uint32_t arm[]{0xe28f300c, 0xe2833001, 0xe12fff33, 0xe3a07001, 0xef000000};
    constexpr std::uint16_t thumb[]{0x2000, 0x3001, 0x282a, 0xd1fc, 0x4770};
    std::vector<std::uint8_t> code(sizeof(arm) + sizeof(thumb));
    std::memcpy(code.data(), arm, sizeof(arm));
    std::memcpy(code.data() + sizeof(arm), thumb, sizeof(thumb));
    return Image(code);
}

struct DualForm {
    std::uint32_t load, store;
    bool thumb{}, writeback{}, pre{};
};

// LLVM-assembled LDRD/STRD r4,r5: register offset, immediate, post-index
// and pre-index. Thumb words below retain their little-endian byte order.
constexpr DualForm dual_forms[]{
        {0xe18140d2, 0xe18140f2},
        {0xe1c140d0, 0xe1c140f0},
        {0xe0c140d8, 0xe0c140f8, false, true},
        {0xe1e140d8, 0xe1e140f8, false, true, true},
        {0x4500e9d1, 0x4500e9c1, true},
        {0x4502e8f1, 0x4502e8e1, true, true},
        {0x4502e9f1, 0x4502e9e1, true, true, true},
};

std::vector<std::uint8_t> DualWord(DualForm form, bool store) {
    std::vector<std::uint8_t> code(4);
    const auto instruction = store ? form.store : form.load;
    std::memcpy(code.data(), &instruction, sizeof(instruction));
    constexpr std::uint32_t arm_exit[]{0xe3a0002a, 0xe3a07001, 0xef000000};
    constexpr std::uint16_t thumb_exit[]{0x202a, 0x2701, 0xdf00};
    const auto append = [&]<class T>(const T& exit) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&exit);
        code.insert(code.end(), bytes, bytes + sizeof(exit));
    };
    if (form.thumb)
        append(thumb_exit);
    else
        append(arm_exit);
    return Image(code, form.thumb);
}

std::uint64_t DualData(exe::VM& vm, bool second_page = true) {
    const auto page = vm.Memory().PageSize();
    const auto mapping =
            vm.Memory().Map(0, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    if (mapping == UINT64_MAX || mapping >= (UINT64_C(1) << 32)) return UINT64_MAX;
    if (!second_page && vm.Memory().Protect(mapping + page, page, PROT_NONE)) return UINT64_MAX;
    return mapping;
}

std::uint64_t StackPointer(exe::VM& vm) { return vm.Arm32State().r[13]; }
}  // namespace

#if defined(__linux__) && defined(__x86_64__)
TEST_CASE("ARM32 x64 pins use the supported bank and report the effective slots",
          "[native][arm32][x64]") {
    for (unsigned requested : {0u, 2u, 4u}) {
        for (auto scope : {ir::Scope::Block, ir::Scope::Function}) {
            platform::HostRuntime host{false};
            auto backend = host.Bind();
            REQUIRE(backend.pinned_uniform_slots == 2);
            exe::RuntimeOptions options;
            options.scope = scope;
            options.pinned_slots = requested;
            exe::RuntimeStats stats;
            exe::VM vm{options, stats, backend};
            vm.PrepareGuest(Interworking(), "arm32-pins");
            REQUIRE(vm.RunPrepared() == 42);
            CHECK(stats.pinned_uniform_slots == std::min(requested, 2u));
            CHECK(stats.native_bytes > 0 && stats.interpreted_steps == 0);
            CHECK(vm.Close().Complete());
        }
    }
}
#endif

TEST_CASE("ARM32 ELF32 stack, interworking and mixed promotion share one frontend",
          "[native][arm32]") {
    using Mode = exe::ExecutionMode;
    for (const auto mode : {Mode::InterpretOnly, Mode::JitOnly, Mode::Mixed}) {
        for (const auto scope : {ir::Scope::Block, ir::Scope::Function}) {
            INFO("mode=" << unsigned(mode) << " scope=" << unsigned(scope));
            platform::HostRuntime host{true};
            exe::RuntimeOptions options;
            options.mode = mode;
            options.scope = scope;
            options.hot_threshold = 3;
            options.decode_timing = true;
            exe::RuntimeStats stats;
            exe::VM vm{options, stats, host.Bind()};
            const std::array<std::string, 1> argv{"arm32-fixture"};
            vm.PrepareGuest(Interworking(), "arm32-fixture", argv);
            REQUIRE(vm.ISA() == GuestISA::Arm32);
            const auto sp = StackPointer(vm);
            CHECK(sp < (UINT64_C(1) << 32) && !(sp & 7));
            std::uint32_t words[3];
            REQUIRE(vm.Memory().CopyIn(words, sp, sizeof(words)));
            CHECK(words[0] == 1 && words[1] < sp + (8u << 20) && words[2] == 0);
            CHECK(vm.RunPrepared() == 42);
            if (mode == Mode::InterpretOnly)
                CHECK(stats.compile_attempts == 0);
            else
                CHECK(stats.compile_attempts != 0);
            if (mode == Mode::Mixed) CHECK(stats.interpreted_steps != 0);
            CHECK(vm.Close().Complete());
        }
    }
}

#if defined(__linux__) && defined(__x86_64__)
TEST_CASE("ARM and Thumb dual-word transfers preserve alignment paths and writeback",
          "[native][arm32][memory][x64]") {
    using Mode = exe::ExecutionMode;
    for (const auto form : dual_forms)
        for (const bool store : {false, true})
            for (const unsigned offset : {0u, 4u})
                for (const auto mode : {Mode::InterpretOnly, Mode::JitOnly})
                    for (const auto scope : {ir::Scope::Block, ir::Scope::Function})
                        for (const auto target : {ir::TargetMode::Known,
                                                  ir::TargetMode::Unknown,
                                                  ir::TargetMode::EarlyOff}) {
                            INFO(form.thumb << ' ' << store << ' ' << offset << ' '
                                            << unsigned(mode) << ' ' << unsigned(scope) << ' '
                                            << unsigned(target));
                            platform::HostRuntime host{false};
                            auto backend = host.Bind();
                            backend.target.mode = target;
                            exe::RuntimeOptions options;
                            options.mode = mode;
                            options.scope = scope;
                            exe::RuntimeStats stats;
                            exe::VM vm{options, stats, backend};
                            vm.PrepareGuest(DualWord(form, store), "arm32-dual-word");
                            const auto data = DualData(vm);
                            REQUIRE(data != UINT64_MAX);
                            const auto address = data + offset;
                            const std::uint32_t words[]{0xcafebabe, 0x89abcdef};
                            REQUIRE(vm.Memory().CopyOut(address, words, sizeof(words)));
                            auto& state = vm.Arm32State();
                            state.r[1] = address - (form.pre ? 8 : 0);
                            state.r[2] = 0;
                            state.r[4] = 0x11223344;
                            state.r[5] = 0x55667788;
                            REQUIRE(vm.RunPrepared() == 42);
                            CHECK(state.r[1] == address + (form.writeback && !form.pre ? 8 : 0));
                            if (store) {
                                std::uint32_t observed[2]{};
                                REQUIRE(vm.Memory().CopyIn(observed, address, sizeof(observed)));
                                CHECK(observed[0] == 0x11223344 && observed[1] == 0x55667788);
                            } else
                                CHECK(state.r[4] == words[0] && state.r[5] == words[1]);
                            CHECK(mode == Mode::InterpretOnly
                                          ? stats.native_bytes == 0
                                          : stats.native_bytes > 0 && stats.interpreted_steps == 0);
                            CHECK(vm.Close().Complete());
                        }
}

TEST_CASE("ARM32 dual-word faults retain each access boundary and deferred writeback",
          "[native][arm32][memory][x64]") {
    using Mode = exe::ExecutionMode;
    for (const auto form : dual_forms)
        for (const bool store : {false, true})
            // offset -4 faults on the second word; 0/4 fault on the first
            // aligned doubleword/word, respectively.
            for (const int offset : {-4, 0, 4})
                for (const auto mode : {Mode::InterpretOnly, Mode::JitOnly})
                    for (const auto scope : {ir::Scope::Block, ir::Scope::Function})
                        for (const auto target : {ir::TargetMode::Known,
                                                  ir::TargetMode::Unknown,
                                                  ir::TargetMode::EarlyOff}) {
                            INFO(form.thumb << ' ' << store << ' ' << offset << ' '
                                            << unsigned(mode) << ' ' << unsigned(scope) << ' '
                                            << unsigned(target));
                            platform::HostRuntime host{false};
                            auto backend = host.Bind();
                            backend.target.mode = target;
                            exe::RuntimeOptions options;
                            options.mode = mode;
                            options.scope = scope;
                            exe::RuntimeStats stats;
                            exe::VM vm{options, stats, backend};
                            vm.PrepareGuest(DualWord(form, store), "arm32-dual-fault");
                            const auto data = DualData(vm, false);
                            REQUIRE(data != UINT64_MAX);
                            const auto boundary = data + vm.Memory().PageSize();
                            const auto address = boundary + offset;
                            const std::uint32_t first = 0xcafebabe;
                            REQUIRE(vm.Memory().CopyOut(boundary - 4, &first, sizeof(first)));
                            auto& state = vm.Arm32State();
                            const auto original_base = address - (form.pre ? 8 : 0);
                            state.r[1] = original_base;
                            state.r[2] = 0;
                            state.r[4] = 0x11223344;
                            state.r[5] = 0x55667788;
                            (void)vm.RunPrepared();
                            REQUIRE((vm.StopReason() == GuestStopReason::GuestSignalTermination &&
                                     vm.TerminationSignal() == SIGSEGV));
                            CHECK(state.r[1] == original_base);
                            CHECK(state.r[4] == (!store && offset < 0 ? first : 0x11223344) &&
                                  state.r[5] == 0x55667788);
                            std::uint32_t observed{};
                            REQUIRE(vm.Memory().CopyIn(&observed, boundary - 4, sizeof(observed)));
                            CHECK(observed == (store && offset < 0 ? 0x11223344 : first));
                            const auto snapshot = vm.CaptureException();
                            REQUIRE(snapshot);
                            CHECK(snapshot->info.instruction_pc == Entry &&
                                  snapshot->info.access == (store ? GuestExceptionAccess::Write
                                                                  : GuestExceptionAccess::Read) &&
                                  snapshot->info.access_bytes == (offset == 0 ? 8 : 4) &&
                                  snapshot->info.address_valid &&
                                  snapshot->info.address == boundary + (offset > 0 ? offset : 0));
                            CHECK(vm.Close().Complete());
                        }
}

TEST_CASE("ARM32 overlapping NEON permutations remain executable on x64", "[native][arm32][x64]") {
    // VTRN.8 D0,D0; VUZP.8 D0,D0; VZIP.8 D0,D0; exit(42).
    constexpr std::uint32_t arm[]{
            0xf3b20080, 0xf3b20100, 0xf3b20180, 0xe3a0002a, 0xe3a07001, 0xef000000};
    constexpr std::uint16_t thumb[]{
            0xffb2, 0x0080, 0xffb2, 0x0100, 0xffb2, 0x0180, 0x202a, 0x2701, 0xdf00};
    for (const bool is_thumb : {false, true})
        for (const auto scope : {ir::Scope::Block, ir::Scope::Function}) {
            const std::span<const std::uint8_t> code =
                    is_thumb
                            ? std::span{reinterpret_cast<const std::uint8_t*>(thumb), sizeof(thumb)}
                            : std::span{reinterpret_cast<const std::uint8_t*>(arm), sizeof(arm)};
            platform::HostRuntime host{false};
            exe::RuntimeOptions options;
            options.scope = scope;
            exe::RuntimeStats stats;
            exe::VM vm{options, stats, host.Bind()};
            vm.PrepareGuest(Image(code, is_thumb), "arm32-neon-overlap");
            for (unsigned vector = 0; vector < 16; ++vector)
                std::memset(vm.Arm32State().q[vector], vector + 1, 16);
            const auto before = vm.Arm32State();
            REQUIRE(vm.RunPrepared() == 42);
            CHECK(std::memcmp(vm.Arm32State().q, before.q, sizeof(before.q)) == 0);
            CHECK(stats.native_bytes && !stats.interpreted_steps);
            CHECK(vm.Close().Complete());
        }
}

TEST_CASE("ARM32 VM executes mixed VFP and NEON float fast paths with exact FPSCR state",
          "[native][arm32][x64]") {
    constexpr std::uint32_t code_words[]{
            0xee300a81, 0xeec02a81, 0xeeb13ae0, 0xeee07a81, 0xeefd5ae0,
            0xf2420de4, 0xf242acf4, 0xf3fbe662,
            0xe3a0002a, 0xe3a07001, 0xef000000};
    const std::span<const std::uint8_t> code{
            reinterpret_cast<const std::uint8_t*>(code_words), sizeof(code_words)};
    const auto run_mode = [&](exe::ExecutionMode mode, ir::Scope scope, bool subnormal) {
        platform::HostRuntime host{false};
        exe::RuntimeOptions options;
        options.mode = mode;
        options.scope = scope;
        exe::RuntimeStats stats;
        exe::VM vm{options, stats, host.Bind()};
        vm.PrepareGuest(Image(code), "arm32-mixed-vfp-neon");
        auto& st = vm.Arm32State();
        st.SetCpsr(0x90000010u);
        st.fpscr = (1u << 27) | 0x60000000u;  // FZ=0 for VFP, QC=1, FPSCR NZCV=0b0110
        const std::uint32_t s1 = subnormal ? 0x00400000u : 0x41100000u;  // subnorm or 9.0f
        const std::uint32_t s2 = 0x40000000u;                            // 2.0f
        const std::uint32_t s15 = 0x3fc00000u;                           // 1.5f
        std::memcpy(st.q[0] + 4, &s1, 4);
        std::memcpy(st.q[0] + 8, &s2, 4);
        std::memcpy(st.q[3] + 12, &s15, 4);
        for (unsigned lane = 0; lane < 4; ++lane) {
            const std::uint32_t a = (subnormal && lane == 1) ? 0x00400000u
                                                             : (0x40400000u + (lane << 23));
            const float b = -1.5f + float(lane);
            const float acc = 0.25f * float(lane + 1);
            std::memcpy(st.q[9] + lane * 4, &a, 4);
            std::memcpy(st.q[10] + lane * 4, &b, 4);
            std::memcpy(st.q[13] + lane * 4, &acc, 4);
        }
        REQUIRE(vm.RunPrepared() == 42);
        if (mode == exe::ExecutionMode::JitOnly)
            CHECK((stats.native_bytes > 0 && stats.interpreted_steps == 0));
        const auto out = st;
        CHECK(vm.Close().Complete());
        return out;
    };
    for (const bool subnormal : {false, true}) {
        const auto ref = run_mode(exe::ExecutionMode::InterpretOnly, ir::Scope::Block, subnormal);
        for (const auto scope : {ir::Scope::Block, ir::Scope::Function}) {
            const auto jit = run_mode(exe::ExecutionMode::JitOnly, scope, subnormal);
            CHECK(std::memcmp(jit.q, ref.q, sizeof(ref.q)) == 0);
            CHECK(jit.fpscr == ref.fpscr);
            CHECK(jit.Cpsr() == ref.Cpsr());
        }
    }
}

TEST_CASE("ARM32 multi-register and VFP transfers preserve values and unaligned faults",
          "[native][arm32][memory][x64]") {
    constexpr std::uint32_t arm_words[]{
            0xe8a100f0, 0xe8b00f00, 0xed2d0b06, 0xecbd3b06,
            0xeca20a04, 0xecb42a04, 0xed833b02, 0xed936b02,
            0xe3a0002a, 0xe3a07001, 0xef000000};
    const std::span<const std::uint8_t> arm_code{
            reinterpret_cast<const std::uint8_t*>(arm_words), sizeof(arm_words)};
    struct TransferRun {
        int rc;
        GuestStopReason stop;
        int sig;
        arm32::GuestState st;
        exe::RuntimeStats stats;
    };
    const auto run_transfer = [&](exe::ExecutionMode mode, std::uint32_t misalign) {
        platform::HostRuntime host{false};
        exe::RuntimeOptions options;
        options.mode = mode;
        exe::RuntimeStats stats;
        exe::VM vm{options, stats, host.Bind()};
        vm.PrepareGuest(Image(arm_code), "arm32-multi-transfer");
        const auto buf =
                vm.Memory().Map(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
        REQUIRE(buf < (UINT64_C(1) << 32));
        auto& st = vm.Arm32State();
        st.r[0] = st.r[1] = std::uint32_t(buf + 64 + misalign);
        st.r[2] = st.r[4] = std::uint32_t(buf + 192);
        st.r[3] = std::uint32_t(buf + 320);
        st.r[5] = 0x22334455;
        st.r[6] = 0x66778899;
        st.r[7] = 0xaabbccdd;
        for (unsigned b = 0; b < 32; ++b) st.q[b / 16][b % 16] = static_cast<std::uint8_t>(b + 1);
        const int rc = vm.RunPrepared();
        const auto reason = vm.StopReason();
        const int sig = vm.TerminationSignal();
        const auto out = st;
        CHECK(vm.Close().Complete());
        return TransferRun{rc, reason, sig, out, stats};
    };
    const auto ref = run_transfer(exe::ExecutionMode::InterpretOnly, 0);
    const auto jit = run_transfer(exe::ExecutionMode::JitOnly, 0);
    CHECK((ref.rc == 42 && jit.rc == 42 && jit.stop == ref.stop));
    CHECK((jit.stats.native_bytes > 0 && jit.stats.interpreted_steps == 0));
    CHECK(std::memcmp(jit.st.r, ref.st.r, sizeof(ref.st.r)) == 0);
    CHECK(std::memcmp(jit.st.q, ref.st.q, sizeof(ref.st.q)) == 0);

    const auto u_ref = run_transfer(exe::ExecutionMode::InterpretOnly, 2);
    const auto u_jit = run_transfer(exe::ExecutionMode::JitOnly, 2);
    CHECK((u_ref.stop == GuestStopReason::GuestSignalTermination && u_jit.stop == u_ref.stop));
    CHECK(u_jit.sig == u_ref.sig);
}
#endif

TEST_CASE("ARM32 signal frame restores edited integer, VFP and full Thumb IT location",
          "[compile][arm32][signals]") {
    platform::HostRuntime host{false};
    exe::RuntimeStats stats;
    exe::VM vm{{}, stats, host.Bind()};
    vm.PrepareGuest(Interworking(), "arm32-frame");
    auto state = vm.Arm32State();
    state.pc = arm32::Location::Make(Entry + 20, true, 0x18, 1u << 22).Key();
    state.fpscr = 1u << 22;
    state.r[0] = 99;
    state.q[0][7] = 0xaa;
    GuestExceptionInfo event;
    event.isa = GuestISA::Arm32;
    event.address = 0x1234;
    event.address_valid = true;
    platform::GuestSignalInfo info;
    info.number = 11;
    for (const bool realtime : {false, true}) {
        platform::GuestSignalAction action{Entry + 21, realtime ? 4u : 0u, 0, 0};
        platform::SignalFrameContext saved{0x123456789abc, {}};
        platform::EncodedSignalFrame encoded;
        arm32::GuestState handler;
        REQUIRE(platform::EncodeArm32SignalFrame(
                state, event, info, action, saved, 0xffff0fe0, encoded, handler));
        CHECK(encoded.bytes.size() == (realtime ? 880 : 752));
        CHECK(handler.r[0] == 11);
        if (realtime) CHECK(handler.r[1] == encoded.address);
        CHECK(arm32::Location{handler.pc}.Thumb() && !arm32::Location{handler.pc}.IT().Value());
        const std::uint32_t changed_r0 = 0xdeadbeef;
        std::memcpy(encoded.bytes.data() + (realtime ? 128 : 0) + 20 + 12, &changed_r0, 4);
        REQUIRE(vm.Memory().CopyOut(encoded.address, encoded.bytes.data(), encoded.bytes.size()));
        arm32::GuestState restored;
        platform::SignalFrameContext restored_context;
        REQUIRE(platform::DecodeArm32SignalFrame(
                vm.Memory(), handler, restored, restored_context, realtime));
        CHECK(restored.pc == state.pc && restored.r[0] == changed_r0);
        CHECK(restored.q[0][7] == 0xaa && restored.fpscr == state.fpscr);
        CHECK(restored_context.mask == saved.mask);
    }
    CHECK(vm.Close().Complete());
}

TEST_CASE("ARM EABI converts pointer sized structures and private TLS calls",
          "[compile][arm32][syscall]") {
    platform::HostRuntime host{false};
    exe::RuntimeStats stats;
    exe::VM vm{{}, stats, host.Bind()};
    vm.PrepareGuest(Interworking(), "arm32-abi");
    const auto scratch =
            vm.Memory().Map(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    REQUIRE(scratch < (UINT64_C(1) << 32));
    platform::LinuxSyscalls service{vm};
    const auto invoke = [&](std::uint32_t number, std::array<std::uint32_t, 6> args) {
        auto& state = vm.Arm32State();
        state.service_pc = arm32::Location::Make(Entry, true, 0x18).Key();
        state.pc = arm32::Location::Make(Entry + 2, true).Key();
        state.r[7] = number;
        for (unsigned i = 0; i < 6; ++i) state.r[i] = args[i];
        service.Service();
        return std::int32_t(state.r[0]);
    };
    REQUIRE(invoke(0xf0005, {0x12345678}) == 0);
    CHECK(vm.Arm32State().tpidruro == 0x12345678);
    CHECK(std::uint32_t(invoke(0xf0006, {})) == 0x12345678);
    const linux32::Action action{Entry + 21, 4, 0, {0x2000, 0x200}};
    REQUIRE(vm.Memory().CopyOut(scratch, &action, sizeof(action)));
    REQUIRE(invoke(174, {10, std::uint32_t(scratch), 0, 8}) == 0);
    REQUIRE(invoke(174, {10, 0, std::uint32_t(scratch + 32), 8}) == 0);
    linux32::Action output;
    REQUIRE(vm.Memory().CopyIn(&output, scratch + 32, sizeof(output)));
    CHECK(output.handler == action.handler && output.mask == action.mask);
    const std::uint32_t sentinel = 0x11223344;
    REQUIRE(vm.Memory().CopyOut(scratch + 72, &sentinel, 4));
    REQUIRE(invoke(263, {1, std::uint32_t(scratch + 64)}) == 0);
    std::uint32_t observed;
    REQUIRE(vm.Memory().CopyIn(&observed, scratch + 72, 4));
    CHECK(observed == sentinel);
    CHECK(vm.Close().Complete());
}

TEST_CASE("Thumb SVC restart preserves its original IT and exact successor location",
          "[native][arm32][signals]") {
    for (const bool restart : {false, true}) {
        platform::HostRuntime host{false};
        exe::RuntimeOptions options;
        options.mode = exe::ExecutionMode::InterpretOnly;
        exe::RuntimeStats stats;
        exe::VM vm{options, stats, host.Bind()};
        vm.PrepareGuest(Interworking(), "thumb-restart");
        const auto scratch =
                vm.Memory().Map(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
        REQUIRE(scratch < (UINT64_C(1) << 32));
        platform::LinuxSyscalls service{vm};
        auto& state = vm.Arm32State();
        const auto before = arm32::Location::Make(Entry + 20, true, 0x18).Key();
        const auto after = arm32::Location::Make(Entry + 22, true).Key();
        const auto invoke = [&](std::uint32_t number, std::array<std::uint32_t, 6> args) {
            state.service_pc = before;
            state.pc = after;
            state.r[7] = number;
            for (unsigned i = 0; i < 6; ++i) state.r[i] = args[i];
            service.Service();
            return std::int32_t(state.r[0]);
        };
        const linux32::Action action{Entry, restart ? 0x10000004u : 4u, 0, {0, 0}};
        REQUIRE(vm.Memory().CopyOut(scratch, &action, sizeof(action)));
        REQUIRE(invoke(174, {SIGUSR1, std::uint32_t(scratch), 0, 8}) == 0);
        REQUIRE(invoke(37, {std::uint32_t(getpid()), SIGUSR1}) == 0);
        REQUIRE(invoke(240, {std::uint32_t(scratch + 256), 0, 0}) == -EINTR);
        // Delivery chooses restart before the frame is encoded. The handler
        // exits deliberately so the frame can be inspected without a host hook.
        REQUIRE(vm.RunPrepared() == 42);
        arm32::GuestState saved;
        platform::SignalFrameContext context;
        REQUIRE(platform::DecodeArm32SignalFrame(vm.Memory(), state, saved, context));
        CHECK(saved.pc == (restart ? before : after));
        CHECK(saved.r[0] == (restart ? std::uint32_t(scratch + 256) : std::uint32_t(-EINTR)));
        CHECK(vm.Close().Complete());
    }
}

TEST_CASE("ARM32 non-SIGINFO handlers return through the legacy EABI signal frame",
          "[native][arm32][signals]") {
    for (const auto mode : {exe::ExecutionMode::InterpretOnly, exe::ExecutionMode::JitOnly}) {
        for (const bool custom_restorer : {false, true}) {
            platform::HostRuntime host{false};
            exe::RuntimeOptions options;
            options.mode = mode;
            exe::RuntimeStats stats;
            exe::VM vm{options, stats, host.Bind()};
            vm.PrepareGuest(Interworking(), "arm32-legacy-signal");
            const auto scratch =
                    vm.Memory().Map(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
            REQUIRE(scratch < (UINT64_C(1) << 32));
            const std::uint32_t code[]{0xe12fff1e,
                                       0xe320f000,
                                       0xe320f000,
                                       0xe320f000,
                                       0xe3a07077,
                                       0xef000000,
                                       0xe7f000f0};
            REQUIRE(vm.Memory().CopyOut(scratch, code, sizeof(code)));
            const linux32::Action action{std::uint32_t(scratch),
                                         custom_restorer ? 0x04000000u : 0u,
                                         custom_restorer ? std::uint32_t(scratch + 16) : 0u,
                                         {0, 0}};
            REQUIRE(vm.Memory().CopyOut(scratch + 128, &action, sizeof(action)));
            platform::LinuxSyscalls service{vm};
            auto& state = vm.Arm32State();
            state.r[0] = SIGUSR1;
            state.r[1] = scratch + 128;
            state.r[2] = 0;
            state.r[3] = 8;
            state.r[7] = 174;
            service.Service();
            REQUIRE(state.r[0] == 0);
            REQUIRE(vm.Memory().Protect(scratch, vm.Memory().PageSize(), PROT_READ | PROT_EXEC) ==
                    0);
            state.r[0] = getpid();
            state.r[1] = SIGUSR1;
            state.r[7] = 37;
            service.Service();
            REQUIRE(state.r[0] == 0);
            REQUIRE(vm.RunPrepared() == 42);
            CHECK(stats.unsupported_syscalls == 0);
            CHECK(vm.Close().Complete());
        }
    }
}
