#include "runtime/arch/x86/flags.h"
#include "runtime/execution/runtime.h"
#include "runtime/execution/vm.h"
#include "runtime/platform/linux/faultable_atomic.h"
#include "runtime/platform/linux/host_runtime.h"
#include "tests/support/check.h"
#include "tests/support/guest_memory.h"
#include "tests/support/native_code.h"
#include "tests/support/test_mode.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {
using swift::tests::Check;
using swift::tests::NativeMode;
namespace execution = swift::runtime::execution;
namespace backend = swift::runtime::backend;
namespace x86 = swift::runtime::x86;

constexpr std::uint8_t exit_program[]{0xb8, 60, 0, 0, 0, 0xbf, 42, 0, 0, 0, 0x0f, 0x05};

class GuestFile {
public:
    explicit GuestFile(std::span<const std::uint8_t> instructions = exit_program,
                       std::uint32_t permissions = PF_R | PF_W | PF_X,
                       std::uint16_t machine = EM_X86_64) {
        const int fd = mkstemp(path.data());
        Check(fd >= 0, "cannot create engine-test guest");
        Elf64_Ehdr header{};
        std::memcpy(header.e_ident, ELFMAG, SELFMAG);
        header.e_ident[EI_CLASS] = ELFCLASS64;
        header.e_ident[EI_DATA] = ELFDATA2LSB;
        header.e_ident[EI_VERSION] = EV_CURRENT;
        header.e_type = ET_EXEC;
        header.e_machine = machine;
        header.e_version = EV_CURRENT;
        header.e_entry = 0x400000 + sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr);
        header.e_phoff = sizeof(Elf64_Ehdr);
        header.e_ehsize = sizeof(Elf64_Ehdr);
        header.e_phentsize = sizeof(Elf64_Phdr);
        header.e_phnum = 1;
        Elf64_Phdr segment{};
        segment.p_type = PT_LOAD;
        segment.p_flags = permissions;
        segment.p_vaddr = 0x400000;
        segment.p_paddr = segment.p_vaddr;
        segment.p_filesz = segment.p_memsz = sizeof(header) + sizeof(segment) + instructions.size();
        segment.p_align = 4096;
        std::vector<std::uint8_t> image(segment.p_filesz);
        std::memcpy(image.data(), &header, sizeof(header));
        std::memcpy(image.data() + sizeof(header), &segment, sizeof(segment));
        std::memcpy(image.data() + sizeof(header) + sizeof(segment),
                    instructions.data(),
                    instructions.size());
        const auto written = write(fd, image.data(), image.size());
        const auto closed = close(fd);
        Check(written == static_cast<ssize_t>(image.size()) && closed == 0,
              "cannot write engine-test guest");
    }

    ~GuestFile() { unlink(path.data()); }

    std::string Path() const { return path.data(); }

private:
#if defined(__ANDROID__)
    std::array<char, 64> path{"/data/local/tmp/swiftvm-engine-test-XXXXXX"};
#else
    std::array<char, 64> path{"/tmp/swiftvm-engine-test-XXXXXX"};
#endif
};

void Interpreter(const std::string& path) {
    execution::RuntimeOptions options;
    options.mode = execution::ExecutionMode::InterpretOnly;
    const std::array<std::string, 1> argv{path};
    const auto result = execution::RunGuest(path, argv, {}, options);
    if (!result) std::fprintf(stderr, "%s\n", result.error.c_str());
    Check(result && result.exit_code == 42 && result.stats.compile_attempts == 0 &&
                  result.stats.native_bytes == 0 && result.stats.peak_slabs == 0,
          "InterpretOnly executes through safe memory without constructing native code");
}

void SourceAdmission() {
    execution::GuestMemory memory;
    const auto size = memory.PageSize();
    const auto anonymous = memory.Map(0, size, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS);
    Check(anonymous != UINT64_MAX &&
                  memory.ClassifySource(anonymous) == execution::SourceMode::Immutable,
          "read-only anonymous executable code retains immutable formation");
    Check(memory.Protect(anonymous, size, PROT_READ | PROT_WRITE | PROT_EXEC) == 0 &&
                  memory.ClassifySource(anonymous) == execution::SourceMode::WatchedBlock,
          "writable executable code requires the SMC single-block policy");
    const int fd = open("/dev/zero", O_RDONLY | O_CLOEXEC);
    Check(fd >= 0, "cannot open the non-regular mapping fixture");
    const auto device = memory.Map(0, size, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd);
    const auto mapping_error = errno;
    const auto closed = close(fd);
    Check(closed == 0, "device fixture closes its fd once");
    if (device == UINT64_MAX) {
        Check(mapping_error == EACCES || mapping_error == EPERM,
              "device fixture failed for an unexpected reason");
        std::puts(
                "device executable mapping denied by host; volatile RX case unavailable on this "
                "host");
    } else
        Check(memory.ClassifySource(device) == execution::SourceMode::Volatile &&
                      memory.CodeAt(device).empty(),
              "unqualified device bytes cannot enter a persistent JIT or direct-fetch span");
    Check(memory.Release().Complete(), "source fixtures release all guest mappings");
}

void ColdDirectory() {
    execution::HostBackendOps operations;
    const auto pending = reinterpret_cast<execution::NativeCode::Function>(std::uintptr_t{0x1000});
    execution::CodeCache cache{pending, operations};
    Check(cache.DeferCompilation(0, 2) && cache.DeferCompilation(0, 2) &&
                  !cache.DeferCompilation(0, 2) && !cache.Find(0) && !cache.Entries(),
          "cold entries defer exactly the requested visits without becoming native code");
    Check(!cache.DeferCompilation(0x100, 0), "zero threshold requests immediate compilation");
    cache.DisableCompilation(0x100);
    for (unsigned i = 2; i < 18; ++i) (void)cache.DeferCompilation(i * 0x100, 2);
    Check(cache.Allocations() > 1 && !cache.DeferCompilation(0, 2) &&
                  cache.DeferCompilation(0x100, 0),
          "growth and collisions preserve heat and unsupported status in their actual PC slots");
    const auto entry = reinterpret_cast<execution::NativeCode::Function>(std::uintptr_t{0x2000});
    const std::array extra{swift::runtime::backend::EntryPoint{0x1000, 4}};
    cache.InsertReady(0x100, {entry, 8, nullptr, {}}, extra);
    Check(cache.Find(0x100) == entry && cache.Find(0x1000) && cache.Entries() == 2,
          "ready publication and additional entries take precedence over cold metadata");
    cache.Clear();
    Check(!cache.Find(0x100) && cache.DeferCompilation(0, 2) && !cache.DeferCompilation(0x100, 0),
          "source invalidation clears native entries, heat and unsupported status together");
    cache.DisableCompilation(0x100);
    cache.Release();
    Check(cache.DeferCompilation(0, 2) && !cache.DeferCompilation(0x100, 0),
          "release starts fresh heat and unsupported state");
}

#if defined(__linux__) && (defined(__aarch64__) || (defined(__riscv) && __riscv_xlen == 64) ||     \
                           defined(__x86_64__) || defined(__loongarch64))
void Tiered() {
    // The first request includes one loop iteration. Its taken edge enters
    // the independently hot body; the exit remains a cold block in Block mode.
    constexpr std::uint8_t instructions[]{0xb9, 40,   0, 0, 0,  // MOV ECX,40
                                          0x31, 0xff,           // XOR EDI,EDI
                                          0x83, 0xc7, 1,        // ADD EDI,1
                                          0xff, 0xc9,           // DEC ECX
                                          0x75, 0xf9,           // JNZ body
                                          0xb8, 60,   0, 0, 0,  // MOV EAX,60
                                          0x0f, 0x05};          // SYSCALL
    GuestFile guest{instructions, PF_R | PF_X};
    const std::array argv{guest.Path()};
    for (const auto scope :
         {swift::runtime::ir::Scope::Block, swift::runtime::ir::Scope::Function}) {
        for (const bool promote : {false, true}) {
            execution::RuntimeOptions options;
            options.mode = execution::ExecutionMode::Mixed;
            options.scope = scope;
            options.hot_threshold = promote ? 2 : 255;
            const auto result = execution::RunGuest(guest.Path(), argv, {}, options);
            if (!result) std::fprintf(stderr, "%s\n", result.error.c_str());
            Check(result && result.exit_code == 40, "cold/native loop handoff preserves state");
            if (promote)
                Check(result.stats.compile_attempts == 1 &&
                              result.stats.code_units_published == 1 && result.stats.native_bytes &&
                              result.stats.interpreted_steps > 0 &&
                              result.stats.interpreted_steps < 124,
                      "hot entry promotes once and executes the remaining loop natively");
            else
                Check(!result.stats.compile_attempts && !result.stats.native_bytes &&
                              !result.stats.peak_slabs && result.stats.interpreted_steps == 124,
                      "cold code runs without constructing IR or allocating native code");
        }
    }
    swift::runtime::platform::HostRuntime host{false};
    auto operations = host.Bind();
    operations.compile = [](void*,
                            const swift::runtime::ir::Program&,
                            execution::HostCompileRequest) -> backend::CompileResult& {
        static backend::CompileResult unsupported;
        unsupported.error = backend::CompileError::UnsupportedOpcode;
        return unsupported;
    };
    execution::RuntimeOptions options;
    options.mode = execution::ExecutionMode::Mixed;
    options.hot_threshold = 2;
    execution::RuntimeStats stats;
    execution::VM vm{options, stats, operations};
    const auto code = vm.Run(guest.Path(), argv, {});
    Check(vm.Close().Complete(), "unsupported hot loop releases the same actual owners");
    Check(code == 40 && stats.compile_attempts == 1 && !stats.native_bytes &&
                  stats.interpreted_steps == 124,
          "an unsupported hot entry is interpreted without repeated failed compilations");
}

void Mixed(const std::string& path, bool invalid) {
    swift::runtime::platform::HostRuntime host{false};
    auto operations = host.Bind();
    if (invalid)
        operations.compile = [](void*,
                                const swift::runtime::ir::Program&,
                                execution::HostCompileRequest) -> backend::CompileResult& {
            static backend::CompileResult result;
            result.error = backend::CompileError::InvalidProgram;
            return result;
        };
    else
        operations.compile = [](void*,
                                const swift::runtime::ir::Program&,
                                execution::HostCompileRequest) -> backend::CompileResult& {
            static backend::CompileResult result;
            result.error = backend::CompileError::UnsupportedOpcode;
            return result;
        };
    execution::RuntimeStats stats;
    execution::RuntimeOptions options;
    options.mode = execution::ExecutionMode::Mixed;
    options.hot_threshold = 0;
    execution::VM vm{options, stats, operations};
    const std::array<std::string, 1> argv{path};
    bool stopped{};
    int code{};
    try {
        code = vm.Run(path, argv, {});
    } catch (const std::exception&) {
        stopped = true;
    }
    Check(vm.Close().Complete(), "mixed VM releases its real owners");
    if (invalid)
        Check(stopped && stats.syscalls == 0,
              "invalid producer/backend data cannot become an interpreter fallback");
    else
        Check(!stopped && code == 42 && stats.compile_attempts == 1 && stats.native_bytes == 0 &&
                      stats.interpreted_steps == 3,
              "mixed executes the unsupported entry fragment once from actual guest PC/state");
}

void ArmGuestFaultState() {
    // MOV X0,17; MOV X19,0x313; MOV X1,0; LDR X2,[X1]; exit(17).
    // X0 can be pinned while X19 remains a normal Uniform location.
    constexpr std::array<std::uint32_t, 6> instructions{
            0xd2800220, 0xd2806273, 0xd2800001, 0xf9400022, 0xd2800ba8, 0xd4000001};
    const std::span<const std::uint8_t> bytes{
            reinterpret_cast<const std::uint8_t*>(instructions.data()), sizeof(instructions)};
    GuestFile guest{bytes, PF_R | PF_X, EM_AARCH64};
    const std::array argv{guest.Path()};
    for (const auto scope :
         {swift::runtime::ir::Scope::Block, swift::runtime::ir::Scope::Function}) {
        for (const auto mode : {execution::ExecutionMode::JitOnly,
                                execution::ExecutionMode::InterpretOnly,
                                execution::ExecutionMode::Mixed}) {
            swift::runtime::platform::HostRuntime host{false};
            execution::RuntimeOptions options;
            options.scope = scope;
            options.mode = mode;
            options.hot_threshold = 0;
            execution::RuntimeStats stats;
            execution::VM vm{options, stats, host.Bind()};
            bool guest_fault{};
            try {
                (void)vm.Run(guest.Path(), argv, {});
            } catch (const swift::RuntimeError& error) {
                guest_fault = error.Kind() == swift::ErrorKind::GuestFault;
            }
            Check(guest_fault && vm.ISA() == swift::runtime::GuestISA::Arm64 &&
                          vm.ArmState().pc ==
                                  0x400000 + sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr) + 12 &&
                          vm.ArmState().gprs[0] == 17 && vm.ArmState().gprs[19] == 0x313 &&
                          vm.ArmState().gprs[2] == 0,
                  "ARM guest faults retain exact PC and committed Uniform/pinned state");
            Check(vm.Close().Complete(), "ARM fault VM releases its actual owners");
        }
    }
}

void SSEExceptionState() {
    struct Example {
        std::array<std::uint8_t, 6> operation;
        unsigned length;
        std::uint32_t left, right, status;
        std::uint32_t control{0x1f80};
        bool check_result{};
        std::uint32_t result{};
    };

    // Independent architectural outcomes, including cases where a numerical
    // result alone cannot distinguish quiet from signaling predicates.
    constexpr std::array examples{
            Example{{0xf3, 0x0f, 0x5e, 0xc1}, 4, 0, 0, 1},
            Example{{0xf3, 0x0f, 0x5e, 0xc1}, 4, 0x3f800000, 0, 4},
            Example{{0xf3, 0x0f, 0x58, 0xc1}, 4, 1, 0, 2},
            Example{{0x0f, 0x2e, 0xc1}, 3, 0x7fc00001, 0, 0},
            Example{{0x0f, 0x2f, 0xc1}, 3, 0x7fc00001, 0, 1},
            Example{{0xf3, 0x0f, 0x2c, 0xd0}, 4, 0x3fc00000, 0, 32},
            Example{{0x66, 0x0f, 0x3a, 0x0a, 0xc0, 8}, 6, 0x3fc00000, 0, 0},
            Example{{0x66, 0x0f, 0x3a, 0x0a, 0xc0, 0}, 6, 0x3fc00000, 0, 32},
            Example{{0xf3, 0x0f, 0x53, 0xc1}, 4, 0, 0, 0},
            Example{{0xf3, 0x0f, 0x5d, 0xc1}, 4, 0x3f800000, 1, 0, 0x1fc0, true, 0},
            Example{{0xf3, 0x0f, 0x5f, 0xc1},
                    4,
                    0xbf800000,
                    0x80000001,
                    0,
                    0x1fc0,
                    true,
                    0x80000000},
    };
    bool statuses_match = true;
    for (const auto mode :
         {execution::ExecutionMode::JitOnly, execution::ExecutionMode::InterpretOnly}) {
        for (const auto& example : examples) {
            std::vector<std::uint8_t> code(example.operation.begin(),
                                           example.operation.begin() + example.length);
            code.insert(code.end(), {0x0f, 0xae, 0x18});  // STMXCSR [RAX].
            code.insert(code.end(), std::begin(exit_program), std::end(exit_program));
            GuestFile guest{code, PF_R | PF_X};
            swift::runtime::platform::HostRuntime host{false};
            execution::RuntimeOptions options;
            options.mode = mode;
            execution::RuntimeStats stats;
            execution::VM vm{options, stats, host.Bind()};
            vm.PrepareGuest(guest.Path());
            const auto buffer =
                    vm.Memory().Map(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
            Check(buffer != UINT64_MAX, "allocate SSE status destination");
            vm.State().gprs[0] = buffer;
            vm.State().mxcsr = example.control;
            std::memcpy(vm.State().xmm[0], &example.left, 4);
            std::memcpy(vm.State().xmm[1], &example.right, 4);
            Check(vm.RunPrepared() == 42, "SSE status observer returns through the owner");
            std::uint32_t actual{};
            const bool matches =
                    vm.Memory().CopyIn(&actual, buffer, 4) && (actual & 63) == example.status;
            if (!matches)
                std::fprintf(stderr,
                             "SSE status mode=%u op=%02x%02x%02x%02x left=%08x right=%08x "
                             "expected=%02x actual=%02x\n",
                             static_cast<unsigned>(mode),
                             example.operation[0],
                             example.operation[1],
                             example.operation[2],
                             example.operation[3],
                             example.left,
                             example.right,
                             example.status,
                             actual & 63);
            statuses_match &= matches;
            if (example.check_result) {
                std::uint32_t value;
                std::memcpy(&value, vm.State().xmm[0], 4);
                if (value != example.result)
                    std::fprintf(stderr,
                                 "SSE DAZ result mode=%u op=%02x expected=%08x actual=%08x\n",
                                 static_cast<unsigned>(mode),
                                 example.operation[2],
                                 example.result,
                                 value);
                statuses_match &= value == example.result;
            }
            Check(vm.Close().Complete(), "SSE state fixture releases mappings");
        }
    }
    // Unmasked invalid must preserve the original destination and the exact
    // instruction PC, while recording the newly raised sticky exception.
    constexpr std::array<std::uint8_t, 4> divide{0xf3, 0x0f, 0x5e, 0xc1};
    GuestFile guest{divide, PF_R | PF_X};
    for (const auto mode :
         {execution::ExecutionMode::JitOnly, execution::ExecutionMode::InterpretOnly}) {
        swift::runtime::platform::HostRuntime host{false};
        execution::RuntimeOptions options;
        options.mode = mode;
        execution::RuntimeStats stats;
        execution::VM vm{options, stats, host.Bind()};
        vm.PrepareGuest(guest.Path());
        const auto original_pc = vm.State().pc;
        vm.State().mxcsr &= ~(1u << 7);
        vm.State().xmm[0][15] = 0xa5;
        bool fault{};
        try {
            (void)vm.RunPrepared();
        } catch (const swift::RuntimeError& failure) {
            fault = failure.Kind() == swift::ErrorKind::GuestFault;
        }
        Check(fault && vm.State().pc == original_pc && (vm.State().mxcsr & 1) &&
                      vm.State().xmm[0][0] == 0 && vm.State().xmm[0][15] == 0xa5,
              "unmasked SSE invalid traps before destination commit");
        Check(vm.Close().Complete(), "unmasked SSE fixture releases mappings");
    }
    Check(statuses_match, "native/direct SSE status matches architectural exception flags");
}

void UnalignedAtomicOwner() {
    constexpr std::array<std::uint8_t, 4> add{0xf0, 0x83, 0x00, 1};
    std::vector<std::uint8_t> code(add.begin(), add.end());
    code.insert(code.end(), std::begin(exit_program), std::end(exit_program));
    GuestFile guest{code, PF_R | PF_X};
    for (const auto mode :
         {execution::ExecutionMode::JitOnly, execution::ExecutionMode::InterpretOnly}) {
        for (const unsigned offset : {1u, 7u, 4094u}) {
            swift::runtime::platform::HostRuntime host{false};
            execution::RuntimeOptions options;
            options.mode = mode;
            execution::RuntimeStats stats;
            execution::VM vm{options, stats, host.Bind()};
            vm.PrepareGuest(guest.Path());
            const auto page = vm.Memory().PageSize();
            const auto buffer = vm.Memory().Map(
                    0, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
            const auto address = buffer + (offset == 4094 ? page - 2 : offset);
            const std::array<std::uint8_t, 6> input{0xa5, 41, 0, 0, 0, 0x5a};
            Check(vm.Memory().CopyOut(address - 1, input.data(), input.size()),
                  "initialize atomic neighbors");
            vm.State().gprs[0] = address;
            Check(vm.RunPrepared() == 42, "unaligned atomic executes on private memory");
            std::array<std::uint8_t, 6> actual{};
            Check(vm.Memory().CopyIn(actual.data(), address - 1, actual.size()) &&
                          actual == std::array<std::uint8_t, 6>{0xa5, 42, 0, 0, 0, 0x5a},
                  "unaligned atomics preserve adjacent bytes, including a page crossing");
            Check(vm.Close().Complete(), "atomic fixture releases mappings");
        }
    }
}

void AtomicFaultBeforeCommit() {
    constexpr std::array<std::uint8_t, 4> add{0xf0, 0x83, 0x00, 1};
    GuestFile guest{add, PF_R | PF_X};
    for (const auto mode :
         {execution::ExecutionMode::JitOnly, execution::ExecutionMode::InterpretOnly}) {
        swift::runtime::platform::HostRuntime host{false};
        execution::RuntimeOptions options;
        options.mode = mode;
        execution::RuntimeStats stats;
        execution::VM vm{options, stats, host.Bind()};
        vm.PrepareGuest(guest.Path());
        const auto page = vm.Memory().PageSize();
        const auto buffer =
                vm.Memory().Map(0, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
        const auto address = buffer + page - 2, pc = vm.State().pc;
        const std::array<std::uint8_t, 4> input{41, 0, 0, 0};
        Check(vm.Memory().CopyOut(address, input.data(), input.size()),
              "initialize cross-page atomic");
        Check(vm.Memory().Protect(buffer + page, page, PROT_READ) == 0,
              "protect atomic second page");
        vm.State().gprs[0] = address;
        bool fault{};
        try {
            (void)vm.RunPrepared();
        } catch (const swift::RuntimeError& failure) {
            fault = failure.Kind() == swift::ErrorKind::GuestFault;
        }
        std::array<std::uint8_t, 4> actual{};
        Check(fault && vm.State().pc == pc && vm.State().gprs[0] == address &&
                      vm.Memory().CopyIn(actual.data(), address, actual.size()) && actual == input,
              "cross-page atomic permission faults precede all memory/register commits");
        Check(vm.Close().Complete(), "atomic fault fixture releases mappings");
    }
}

std::vector<std::uint8_t> Atomic128Program() {
    std::vector<std::uint8_t> code{0xf0,
                                   0x48,
                                   0x0f,
                                   0xc7,
                                   0x0e,  // LOCK CMPXCHG16B [RSI].
                                   0x49,
                                   0x89,
                                   0xc0,  // MOV R8,RAX.
                                   0x49,
                                   0x89,
                                   0xd1,  // MOV R9,RDX.
                                   0x49,
                                   0x89,
                                   0xda,  // MOV R10,RBX.
                                   0x49,
                                   0x89,
                                   0xcc};  // MOV R12,RCX; SYSCALL overwrites R11.
    code.insert(code.end(), std::begin(exit_program), std::end(exit_program));
    return code;
}

void Atomic128Owner() {
    const auto code = Atomic128Program();
    GuestFile guest{code, PF_R | PF_X};
    constexpr std::array<std::uint64_t, 2> original{0x0123456789abcdef, 0xfedcba9876543210};
    constexpr std::array<std::uint64_t, 2> desired{0x5555555555555555, 0xaaaaaaaaaaaaaaaa};
    for (const auto scope :
         {swift::runtime::ir::Scope::Block, swift::runtime::ir::Scope::Function}) {
        for (const auto mode :
             {execution::ExecutionMode::JitOnly, execution::ExecutionMode::InterpretOnly}) {
            for (const bool match : {true, false}) {
                INFO("atomic128 scope=" << static_cast<unsigned>(scope) << " mode="
                                        << static_cast<unsigned>(mode) << " match=" << match);
                swift::runtime::platform::HostRuntime host{false};
                execution::RuntimeOptions options;
                options.scope = scope;
                options.mode = mode;
                execution::RuntimeStats stats;
                execution::VM vm{options, stats, host.Bind()};
                vm.PrepareGuest(guest.Path());
                const auto address = vm.Memory().Map(0,
                                                     vm.Memory().PageSize(),
                                                     PROT_READ | PROT_WRITE,
                                                     MAP_PRIVATE | MAP_ANONYMOUS);
                Check(address != UINT64_MAX &&
                              vm.Memory().CopyOut(address, original.data(), sizeof(original)),
                      "initialize aligned private CMPXCHG16B destination");
                auto& state = vm.State();
                state.gprs[x86::RSI] = address;
                state.gprs[x86::RAX] = original[0] ^ !match;
                state.gprs[x86::RDX] = original[1];
                state.gprs[x86::RBX] = desired[0];
                state.gprs[x86::RCX] = desired[1];
                const auto flags = x86::flags::Carry | x86::flags::Sign | x86::flags::Overflow |
                                   x86::flags::Auxiliary | x86::flags::Direction | 0x35 |
                                   (match ? 0 : x86::flags::Zero);
                state.packed_flags = flags;
                Check(vm.RunPrepared() == 42, "CMPXCHG16B returns through the owner");
                std::array<std::uint64_t, 2> actual{};
                Check(vm.Memory().CopyIn(actual.data(), address, sizeof(actual)) &&
                              actual == (match ? desired : original),
                      "CMPXCHG16B commits both halves on success and preserves memory on failure");
                Check(state.gprs[x86::R8] == original[0] && state.gprs[x86::R9] == original[1] &&
                              state.gprs[x86::R10] == desired[0] &&
                              state.gprs[x86::R12] == desired[1] &&
                              x86::flags::GetZF(state.packed_flags) == match &&
                              (x86::flags::ToRflags(state.packed_flags) & ~(1u << 6)) ==
                                      (x86::flags::ToRflags(flags) & ~(1u << 6)),
                      "CMPXCHG16B returns observed RDX:RAX, preserves RCX:RBX and updates only ZF");
                Check(mode == execution::ExecutionMode::JitOnly
                              ? stats.native_bytes && stats.compile_attempts &&
                                        !stats.interpreted_steps
                              : !stats.native_bytes && !stats.compile_attempts &&
                                        stats.interpreted_steps,
                      "CMPXCHG16B actually exercises the requested native or direct engine");
                Check(vm.Close().Complete(), "CMPXCHG16B fixture releases mappings");
            }
        }
    }
}

void Atomic128RejectBeforeCommit() {
    const auto code = Atomic128Program();
    GuestFile guest{code, PF_R | PF_X};
    enum class Rejection { Alignment, ReadOnly, SharedWithoutHardware };
    const bool hardware = swift::runtime::platform::HasAtomic128();
    std::printf("atomic128 native_hardware=%u\n", hardware);
    for (const auto mode :
         {execution::ExecutionMode::JitOnly, execution::ExecutionMode::InterpretOnly}) {
        for (const auto rejection :
             {Rejection::Alignment, Rejection::ReadOnly, Rejection::SharedWithoutHardware}) {
            if (rejection == Rejection::SharedWithoutHardware && hardware) continue;
            INFO("atomic128 mode=" << static_cast<unsigned>(mode)
                                   << " rejection=" << static_cast<unsigned>(rejection));
            swift::runtime::platform::HostRuntime host{false};
            execution::RuntimeOptions options;
            options.mode = mode;
            execution::RuntimeStats stats;
            execution::VM vm{options, stats, host.Bind()};
            vm.PrepareGuest(guest.Path());
            const auto page = vm.Memory().PageSize();
            const auto buffer = vm.Memory().Map(
                    0,
                    page,
                    PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | (rejection == Rejection::SharedWithoutHardware ? MAP_SHARED
                                                                                   : MAP_PRIVATE));
            constexpr std::array<std::uint64_t, 4> original{
                    0xa5a5a5a5a5a5a5a5, 0x0123456789abcdef, 0xfedcba9876543210, 0x5a5a5a5a5a5a5a5a};
            Check(buffer != UINT64_MAX &&
                          vm.Memory().CopyOut(buffer + 8, original.data(), sizeof(original)),
                  "initialize rejected CMPXCHG16B destination and neighbors");
            if (rejection == Rejection::ReadOnly)
                Check(vm.Memory().Protect(buffer, page, PROT_READ) == 0,
                      "make CMPXCHG16B destination read-only");
            auto& state = vm.State();
            const auto pc = state.pc;
            state.gprs[x86::RSI] = buffer + 16 + (rejection == Rejection::Alignment);
            // A failed compare still requires a real write cycle; READONLY must
            // fault even though neither half would acquire a different value.
            state.gprs[x86::RAX] = original[1] ^ 1;
            state.gprs[x86::RDX] = original[2];
            state.gprs[x86::RBX] = 0x5555555555555555;
            state.gprs[x86::RCX] = 0xaaaaaaaaaaaaaaaa;
            state.packed_flags = x86::flags::Carry | x86::flags::Zero | 0x35;
            const auto before = state;
            swift::ErrorKind failure{};
            try {
                (void)vm.RunPrepared();
            } catch (const swift::RuntimeError& error) {
                failure = error.Kind();
            }
            std::array<std::uint64_t, 4> actual{};
            Check(failure == (rejection == Rejection::SharedWithoutHardware
                                      ? swift::ErrorKind::Unsupported
                                      : swift::ErrorKind::GuestFault) &&
                          state.pc == pc &&
                          x86::flags::ToRflags(state.packed_flags) ==
                                  x86::flags::ToRflags(before.packed_flags) &&
                          std::equal(std::begin(state.gprs),
                                     std::end(state.gprs),
                                     std::begin(before.gprs)) &&
                          vm.Memory().CopyIn(actual.data(), buffer + 8, sizeof(actual)) &&
                          actual == original,
                  "CMPXCHG16B alignment, permission and unsupported backing reject before commit");
            Check(vm.Close().Complete(), "rejected CMPXCHG16B fixture releases mappings");
        }
    }
}

void ArmUnsupportedFPControl() {
    // MOV X0,1<<26; MSR FPCR,X0 (AHP); MOV X8,93; SVC 0.
    constexpr std::array<std::uint32_t, 4> instructions{
            0xd2a08000, 0xd51b4400, 0xd2800ba8, 0xd4000001};
    const std::span<const std::uint8_t> bytes{
            reinterpret_cast<const std::uint8_t*>(instructions.data()), sizeof(instructions)};
    GuestFile guest{bytes, PF_R | PF_X, EM_AARCH64};
    const std::array argv{guest.Path()};
    for (const auto scope :
         {swift::runtime::ir::Scope::Block, swift::runtime::ir::Scope::Function}) {
        for (const auto mode : {execution::ExecutionMode::JitOnly,
                                execution::ExecutionMode::InterpretOnly,
                                execution::ExecutionMode::Mixed}) {
            execution::RuntimeOptions options;
            options.scope = scope;
            options.mode = mode;
            options.hot_threshold = 0;
            const auto result = execution::RunGuest(guest.Path(), argv, {}, options);
            Check(!result && result.failure.kind == swift::ErrorKind::Unsupported &&
                          result.stats.syscalls == 0 &&
                          result.error.find("FPCR") != std::string::npos,
                  "all ARM engines reject unsupported controls at the FPCR service boundary");
        }
    }
}
#endif
}  // namespace

TEST_CASE("SourceAdmission", "[compile][execution]") { CHECK_NOTHROW(SourceAdmission()); }

TEST_CASE("ColdDirectory", "[compile][execution]") { CHECK_NOTHROW(ColdDirectory()); }

TEST_CASE("InterpretOnly through runtime", "[compile][execution]") {
    GuestFile guest;
    CHECK_NOTHROW(Interpreter(guest.Path()));
}

#if defined(__linux__) && (defined(__aarch64__) || (defined(__riscv) && __riscv_xlen == 64) ||     \
                           defined(__x86_64__) || defined(__loongarch64))

TEST_CASE("Mixed hot promotion", "[native][execution]") { CHECK_NOTHROW(Tiered()); }

TEST_CASE("Mixed unsupported fallback", "[native][execution]") {
    GuestFile guest;
    CHECK_NOTHROW(Mixed(guest.Path(), false));
}

TEST_CASE("Mixed rejects invalid producer", "[native][execution]") {
    GuestFile guest;
    CHECK_NOTHROW(Mixed(guest.Path(), true));
}

TEST_CASE("ARM guest exact fault state", "[native][execution][arm64-guest]") {
    CHECK_NOTHROW(ArmGuestFaultState());
}

TEST_CASE("SSE exception state", "[native][execution][floating]") {
    CHECK_NOTHROW(SSEExceptionState());
}

TEST_CASE("Unaligned atomic owner", "[native][execution][atomic]") {
    CHECK_NOTHROW(UnalignedAtomicOwner());
}

TEST_CASE("Atomic faults before commit", "[native][execution][atomic]") {
    CHECK_NOTHROW(AtomicFaultBeforeCommit());
}

TEST_CASE("CMPXCHG16B owner", "[native][execution][atomic][atomic128]") {
    CHECK_NOTHROW(Atomic128Owner());
}

TEST_CASE("CMPXCHG16B rejects before commit", "[native][execution][atomic][atomic128]") {
    CHECK_NOTHROW(Atomic128RejectBeforeCommit());
}

TEST_CASE("ARM guest unsupported FP controls", "[native][execution][arm64-guest]") {
    CHECK_NOTHROW(ArmUnsupportedFPControl());
}
#endif
