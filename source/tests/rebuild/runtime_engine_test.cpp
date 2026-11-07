#include "tests/support/check.h"
#include "tests/support/test_mode.h"
#include "tests/support/native_code.h"
#include "tests/support/guest_memory.h"
#include "runtime/execution/runtime.h"
#include "runtime/execution/vm.h"
#include "runtime/platform/linux/host_runtime.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <elf.h>
#include <fcntl.h>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace {
using swift::tests::Check;
using swift::tests::NativeMode;
namespace execution = swift::runtime::execution;
namespace backend = swift::runtime::backend;

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

TEST_CASE("ARM guest unsupported FP controls", "[native][execution][arm64-guest]") {
    CHECK_NOTHROW(ArmUnsupportedFPControl());
}
#endif
