#include "runtime/arch/arm64/numeric.h"
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
#include <csignal>
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

    execution::RuntimeOptions options;
    execution::RuntimeStats stats;
    execution::VM vm{options, stats, {}};
    execution::Smc watches{vm};
    auto& watched_memory = vm.Memory();
    GuestFile file;
    const auto file_fd = open(file.Path().c_str(), O_RDWR | O_CLOEXEC);
    Check(file_fd >= 0 && ftruncate(file_fd, size * 2) == 0,
          "create shared source and writable-alias backing");
    const auto source = watched_memory.Map(0, size * 2, PROT_READ | PROT_EXEC, MAP_SHARED, file_fd);
    const auto writer =
            watched_memory.Map(0, size * 2, PROT_READ | PROT_WRITE, MAP_SHARED, file_fd);
    Check(close(file_fd) == 0 && source != UINT64_MAX && writer != UINT64_MAX,
          "map the known shared writable source alias");
    const std::array spans{swift::runtime::SourceSpan{source + size - 1, 2},
                           swift::runtime::SourceSpan{source + size, 1},
                           swift::runtime::SourceSpan{source, 0}};
    watches.Arm(spans);
    watches.Arm(spans);  // Already armed, including the duplicate/cross-page source.
    Check(!watches.Recover(source) && watches.Recover(writer + size) &&
                  !watches.Recover(writer + size),
          "batch source watches protect writable aliases once, leaving RX source unchanged");
    watches.Arm(source, 1);  // Its alias page is still armed; the second is unarmed.
    Check(watches.Recover(writer + size) && watches.Recover(writer),
          "arming an existing page also preserves rearming of older unarmed records");
    watches.Clear();

    const auto partial = watched_memory.Map(
            0, size * 2, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS);
    Check(partial != UINT64_MAX && munmap(reinterpret_cast<void*>(partial + size), size) == 0,
          "inject a later host protection failure after the first candidate page");
    bool failed{};
    try {
        watches.Arm(partial, size * 2);
    } catch (const std::exception&) {
        failed = true;
    }
    Check(failed, "partial source protection failure remains a host error");
    Check(watched_memory.Map(partial + size,
                             size,
                             PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED) == partial + size,
          "restore the deliberately missing host page");
    watches.Arm(partial, 1);  // First page stayed armed across the failed batch.
    Check(watches.Recover(partial + size) && watches.Recover(partial),
          "a failed batch retains every page record for later protection and recovery");
    watches.Clear();
    Check(vm.Close().Complete(), "batched source-watch fixtures release their mappings");
}

void FaultMetadataLifetime() {
    execution::HostBackendOps operations;
    execution::CodeHeap heap{operations, static_cast<std::size_t>(sysconf(_SC_PAGESIZE))};
    constexpr std::array<std::uint32_t, 1> words{0};  // Published data is never executed.
    Check(heap.Publish(words, {}, {}).faults.empty(), "empty fault input publishes an empty view");
    const auto record = [](std::uint64_t pc) {
        backend::FaultSite site{0, 4, pc};
        site.recovery = {8, 12, 1, 0, 4, 3, 2, 1};
        return site;
    };
    const auto equal = [](const backend::FaultSite& actual, const backend::FaultSite& expected) {
        return actual.first_offset == expected.first_offset &&
               actual.last_offset == expected.last_offset && actual.guest_pc == expected.guest_pc &&
               actual.unaligned_offset == expected.unaligned_offset &&
               actual.fallback_offset == expected.fallback_offset &&
               actual.recovery == expected.recovery;
    };
    execution::NativeCode first;
    {
        std::vector<backend::FaultSite> compiler{record(0x1000), record(0x1004)};
        first = heap.Publish(words, compiler, {});
        compiler.front() = record(0xdead);
        Check(equal(first.faults.front(), record(0x1000)),
              "published fault metadata does not borrow mutable compiler records");
    }
    const auto* first_address = first.faults.data();
    // Republishing an existing view exercises an input that aliases the last
    // fragment while its unused capacity can still hold the copied records.
    const auto copied = heap.Publish(words, first.faults, {});
    const auto* copied_address = copied.faults.data();
    constexpr std::size_t records = 257;
    constexpr std::size_t units = 12;
    std::array<execution::NativeCode, units> published;
    std::array<const backend::FaultSite*, units> addresses;
    std::vector<backend::FaultSite> compiler(records);
    for (std::size_t unit = 0; unit < published.size(); ++unit) {
        for (std::size_t i = 0; i < records; ++i) compiler[i] = record(0x2000 + unit * records + i);
        published[unit] = heap.Publish(words, compiler, {});
        addresses[unit] = published[unit].faults.data();
        std::fill(compiler.begin(), compiler.end(), backend::FaultSite{});
    }
    Check(first.faults.data() == first_address && copied.faults.data() == copied_address &&
                  first.faults.size() == 2 && copied.faults.size() == 2 &&
                  equal(first.faults[0], record(0x1000)) &&
                  equal(first.faults[1], record(0x1004)) &&
                  equal(copied.faults[0], record(0x1000)) &&
                  equal(copied.faults[1], record(0x1004)),
          "older fault views survive compiler destruction, appends and outer-container growth");
    for (std::size_t unit = 0; unit < published.size(); ++unit) {
        Check(published[unit].faults.data() == addresses[unit] &&
                      published[unit].faults.size() == records,
              "each fault span retains its address and its own unit extent");
        for (std::size_t i = 0; i < records; ++i)
            Check(equal(published[unit].faults[i], record(0x2000 + unit * records + i)),
                  "later metadata fragments retain every copied fault record");
    }
    Check(heap.Release().Complete(), "fault metadata owner releases its native mappings");
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
            (void)vm.Run(guest.Path(), argv, {});
            const bool guest_fault =
                    vm.StopReason() == swift::runtime::GuestStopReason::GuestSignalTermination &&
                    vm.TerminationSignal() == SIGSEGV;
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

#if defined(__x86_64__)
void X64ExplicitPCFaults() {
    constexpr std::uint8_t x86_load[]{0x48, 0x8b, 0x00};  // MOV RAX,[RAX].
    constexpr std::uint32_t arm_load = 0xf9400001;        // LDR X1,[X0].
    for (const bool arm : {false, true}) {
        const std::span<const std::uint8_t> bytes =
                arm ? std::span{reinterpret_cast<const std::uint8_t*>(&arm_load), sizeof(arm_load)}
                    : std::span<const std::uint8_t>{x86_load};
        GuestFile guest{bytes, PF_R | PF_X, arm ? EM_AARCH64 : EM_X86_64};
        for (const auto target : {swift::runtime::ir::TargetMode::Known,
                                  swift::runtime::ir::TargetMode::Unknown,
                                  swift::runtime::ir::TargetMode::EarlyOff})
            for (const auto scope :
                 {swift::runtime::ir::Scope::Block, swift::runtime::ir::Scope::Function})
                for (const auto pins : {0u, 2u}) {
                    swift::runtime::platform::HostRuntime host{false};
                    auto backend = host.Bind();
                    backend.target.mode = target;
                    execution::RuntimeOptions options;
                    options.scope = scope;
                    options.pinned_slots = pins;
                    execution::RuntimeStats stats;
                    execution::VM vm{options, stats, backend};
                    vm.PrepareGuest(guest.Path());
                    const auto pc = arm ? vm.ArmState().pc : vm.State().pc;
                    if (arm)
                        vm.ArmState().gprs[0] = 0;
                    else
                        vm.State().gprs[x86::RAX] = 0;
                    (void)vm.RunPrepared();
                    const auto fault = vm.CaptureException();
                    Check(vm.StopReason() ==
                                          swift::runtime::GuestStopReason::GuestSignalTermination &&
                                  vm.TerminationSignal() == SIGSEGV && fault &&
                                  fault->info.instruction_pc == pc && fault->info.address_valid &&
                                  fault->info.address == 0 && fault->info.access_bytes == 8 &&
                                  fault->info.access ==
                                          swift::runtime::GuestExceptionAccess::Read &&
                                  stats.native_bytes && !stats.interpreted_steps,
                          "x64 recovers explicit-PC guest memory faults for every target policy");
                    Check(vm.Close().Complete(), "explicit-PC fault fixture releases mappings");
                }
    }
}
#endif

void EmbeddedExceptionContext() {
    constexpr std::uint8_t load[]{0x4c, 0x8b, 0x20};  // MOV R12,[RAX].
    std::vector<std::uint8_t> code(std::begin(load), std::end(load));
    code.insert(code.end(), std::begin(exit_program), std::end(exit_program));
    GuestFile guest{code, PF_R | PF_X};

    struct Recovery {
        std::uint64_t address{}, fault_pc{};
        unsigned calls{};
    } recovery;

    execution::RuntimeOptions options;
    options.exception_opaque = &recovery;
    options.exception_handler = [](void* opaque,
                                   swift::runtime::GuestContextView view,
                                   const swift::runtime::GuestExceptionInfo& event) {
        auto& recovery = *static_cast<Recovery*>(opaque);
        ++recovery.calls;
        recovery.fault_pc = event.instruction_pc;
        auto& state = *std::get<x86::GuestState*>(view);
        state.gprs[x86::RAX] = recovery.address;
        state.mxcsr &= ~0x3fu;
        return swift::runtime::GuestExceptionDisposition::Resume;
    };
    swift::runtime::platform::HostRuntime host{false};
    execution::RuntimeStats stats;
    execution::VM vm{options, stats, host.Bind()};
    vm.PrepareGuest(guest.Path());
    recovery.address = vm.Memory().Map(
            0, vm.Memory().PageSize(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    const std::uint64_t data = 0x8877665544332211;
    Check(recovery.address != UINT64_MAX && vm.Memory().CopyOut(recovery.address, &data, 8),
          "initialize embedding recovery data");
    const auto pc = vm.State().pc;
    vm.State().gprs[x86::RAX] = 0;
    vm.State().mxcsr |= 0x24;
    Check(vm.RunPrepared() == 42 && recovery.calls == 1 && recovery.fault_pc == pc &&
                  vm.State().gprs[x86::R12] == data && !(vm.State().mxcsr & 0x3f) &&
                  stats.interpreted_steps == 0,
          "embedding callback edits canonical state and resumes the faulting PC via JIT");
    Check(vm.Close().Complete(), "embedding recovery releases native owners");

    execution::RuntimeOptions capture;
    capture.exception_handler =
            [](void*, swift::runtime::GuestContextView, const swift::runtime::GuestExceptionInfo&) {
                return swift::runtime::GuestExceptionDisposition::Stop;
            };
    const auto result = execution::RunGuest(guest.Path(), {}, {}, capture);
    Check(result.reason == swift::runtime::GuestStopReason::UnhandledGuestEvent &&
                  result.exception && !result.failure && !result.cleanup_failure &&
                  result.exception->info.instruction_pc == pc &&
                  result.exception->info.address_valid && !result.exception->info.address &&
                  std::get<x86::GuestState>(result.exception->state).pc == pc,
          "an unhandled embedding event retains a complete snapshot after VM destruction");

    // Two different direct accesses fault in one VM. The second event must
    // describe its own read rather than retaining the handled byte-store hint.
    constexpr std::uint8_t accesses[]{0xc6, 0x00, 0x7f, 0x8b, 0x0a};
    std::vector<std::uint8_t> direct_code(std::begin(accesses), std::end(accesses));
    direct_code.insert(direct_code.end(), std::begin(exit_program), std::end(exit_program));
    GuestFile direct_guest{direct_code, PF_R | PF_X};

    struct AccessRecovery {
        std::uint64_t address{};
        std::array<swift::runtime::GuestExceptionInfo, 2> events;
        unsigned calls{};
    } access_recovery;

    execution::RuntimeOptions direct_options;
    direct_options.mode = execution::ExecutionMode::InterpretOnly;
    direct_options.exception_opaque = &access_recovery;
    direct_options.exception_handler = [](void* opaque,
                                          swift::runtime::GuestContextView view,
                                          const swift::runtime::GuestExceptionInfo& event) {
        auto& recovery = *static_cast<AccessRecovery*>(opaque);
        if (recovery.calls == recovery.events.size())
            return swift::runtime::GuestExceptionDisposition::Stop;
        recovery.events[recovery.calls] = event;
        auto& state = *std::get<x86::GuestState*>(view);
        state.gprs[recovery.calls++ == 0 ? x86::RAX : x86::RDX] = recovery.address;
        return swift::runtime::GuestExceptionDisposition::Resume;
    };
    execution::RuntimeStats direct_stats;
    execution::VM direct_vm{direct_options, direct_stats, host.Bind()};
    direct_vm.PrepareGuest(direct_guest.Path());
    access_recovery.address = direct_vm.Memory().Map(
            0, direct_vm.Memory().PageSize(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    Check(access_recovery.address != UINT64_MAX &&
                  direct_vm.Memory().CopyOut(access_recovery.address, &data, sizeof(data)),
          "initialize direct sequential-fault recovery data");
    direct_vm.State().gprs[x86::RAX] = direct_vm.State().gprs[x86::RDX] = 0;
    Check(direct_vm.RunPrepared() == 42 && access_recovery.calls == 2 &&
                  direct_stats.interpreted_steps != 0 && direct_stats.native_bytes == 0,
          "direct execution resumes the byte store and then the word load");
    Check(access_recovery.events[0].access == swift::runtime::GuestExceptionAccess::Write &&
                  access_recovery.events[0].access_bytes == 1 &&
                  access_recovery.events[1].access == swift::runtime::GuestExceptionAccess::Read &&
                  access_recovery.events[1].access_bytes == 4 &&
                  access_recovery.events[0].address_valid &&
                  access_recovery.events[1].address_valid && !access_recovery.events[0].address &&
                  !access_recovery.events[1].address && !access_recovery.events[0].signal_code &&
                  !access_recovery.events[1].signal_code,
          "each semantic fault reports current access facts without prior signal metadata");
    Check(direct_vm.Close().Complete(), "direct recovery releases memory owners");
}

void IllegalDecodePrefix() {
    constexpr std::uint32_t instructions[]{0xd2800220, 0x00000000};  // MOV X0,17; UDF #0.
    const std::span<const std::uint8_t> bytes{reinterpret_cast<const std::uint8_t*>(instructions),
                                              sizeof(instructions)};
    GuestFile guest{bytes, PF_R | PF_X, EM_AARCH64};
    const auto fault_pc = 0x400000 + sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr) + 4;
    for (const auto scope :
         {swift::runtime::ir::Scope::Block, swift::runtime::ir::Scope::Function}) {
        execution::RuntimeOptions options;
        options.scope = scope;
        const auto result = execution::RunGuest(guest.Path(), {}, {}, options);
        Check(result.reason == swift::runtime::GuestStopReason::GuestSignalTermination &&
                      result.termination_signal == SIGILL && result.exception && !result.failure &&
                      result.stats.interpreted_steps == 0 &&
                      result.exception->info.instruction_pc == fault_pc &&
                      std::get<swift::runtime::arm64::GuestState>(result.exception->state)
                                      .gprs[0] == 17,
              "a bad decode is delivered only after its valid native prefix executes");
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
        (void)vm.RunPrepared();
        const auto snapshot = vm.CaptureException();
        const bool fault =
                vm.StopReason() == swift::runtime::GuestStopReason::GuestSignalTermination &&
                vm.TerminationSignal() == SIGFPE && snapshot &&
                snapshot->info.kind == swift::runtime::GuestExceptionKind::FloatingPoint;
        Check(fault && vm.State().pc == original_pc && (vm.State().mxcsr & 1) &&
                      vm.State().xmm[0][0] == 0 && vm.State().xmm[0][15] == 0xa5,
              "unmasked SSE invalid traps before destination commit");
        Check(vm.Close().Complete(), "unmasked SSE fixture releases mappings");
    }
    Check(statuses_match, "native/direct SSE status matches architectural exception flags");
}

#if defined(__aarch64__)
struct HostFPState {
    std::uint64_t control{}, status{};

    static HostFPState Read() noexcept {
        HostFPState result;
        asm volatile("mrs %0, fpcr\n mrs %1, fpsr"
                     : "=r"(result.control), "=r"(result.status)
                     :
                     : "memory");
        return result;
    }

    void Write() const noexcept {
        asm volatile("msr fpcr, %0\n msr fpsr, %1" ::"r"(control), "r"(status) : "memory");
    }

    bool operator==(const HostFPState&) const = default;
};

struct ScopedHostFP {
    HostFPState saved{HostFPState::Read()};

    ~ScopedHostFP() { saved.Write(); }
};

struct NativeFPResult {
    HostFPState host;
    int exit_code{-1}, signal_number{}, signal_code{};
    bool guest_fault{};
};

// Round toward zero and default-NaN differ from the guest's nearest/default
// controls. A preexisting host invalid sticky must survive either native exit.
constexpr HostFPState host_fp_fixture{(std::uint64_t{3} << 22) | (std::uint64_t{1} << 25), 1};

NativeFPResult RunWithHostFP(execution::VM& vm) {
    const ScopedHostFP restore;
    NativeFPResult result;
    host_fp_fixture.Write();
    result.exit_code = vm.RunPrepared();
    result.host = HostFPState::Read();
    restore.saved.Write();
    if (const auto snapshot = vm.CaptureException()) {
        result.guest_fault =
                vm.StopReason() == swift::runtime::GuestStopReason::GuestSignalTermination;
        result.signal_number = snapshot->info.signal_number;
        result.signal_code = snapshot->info.signal_code;
    }
    return result;
}

void NativeFPEnvironmentPreservation() {
    std::vector<std::uint8_t> code{0xf3, 0x0f, 0x5e, 0xc1};  // DIVSS XMM0,XMM1.
    code.insert(code.end(), std::begin(exit_program), std::end(exit_program));
    GuestFile guest{code, PF_R | PF_X};
    for (const auto scope :
         {swift::runtime::ir::Scope::Block, swift::runtime::ir::Scope::Function}) {
        swift::runtime::platform::HostRuntime host{false};
        execution::RuntimeOptions options;
        options.mode = execution::ExecutionMode::JitOnly;
        options.scope = scope;
        options.decode_timing = true;
        execution::RuntimeStats stats;
        execution::VM vm{options, stats, host.Bind()};
        vm.PrepareGuest(guest.Path());
        const std::array<std::uint32_t, 4> left{0x3f800000, 0x12345678, 0x9abcdef0, 0xa5a55a5a};
        const std::uint32_t right = 0x40400000;  // 1/3 rounds differently under host toward-zero.
        std::memcpy(vm.State().xmm[0], left.data(), sizeof left);
        std::memcpy(vm.State().xmm[1], &right, sizeof right);
        vm.State().mxcsr = 0x1f80;
        const auto result = RunWithHostFP(vm);
        std::array<std::uint32_t, 4> actual{};
        std::memcpy(actual.data(), vm.State().xmm[0], sizeof actual);
        Check(result.exit_code == 42 && !result.guest_fault && stats.interpreted_steps == 0 &&
                      stats.compile_attempts == 1 && stats.code_units_published == 1 &&
                      stats.native_bytes,
              "real VM executes both scopes through one native FP unit");
        Check(result.host == host_fp_fixture,
              "native return preserves the caller's exact nondefault FPCR and nonzero FPSR");
        Check(actual == std::array<std::uint32_t, 4>{0x3eaaaaab, left[1], left[2], left[3]} &&
                      vm.State().mxcsr == 0x1fa0,
              "native arithmetic uses guest rounding and retains guest precision without host "
              "invalid");
        Check(vm.Close().Complete(), "native FP environment fixture releases mappings");
    }
}

void SSEStatusSurvivesNativeMemoryFault() {
    // No status observer, helper, branch or guest dispatch separates FDIV from
    // the bad load: its freshly raised ZE remains only in hardware FPSR.
    std::vector<std::uint8_t> code{0xf3,
                                   0x0f,
                                   0x5e,
                                   0xc1,  // DIVSS XMM0,XMM1.
                                   0x49,
                                   0x83,
                                   0xc4,
                                   0x01,  // ADD R12,1.
                                   0x4c,
                                   0x8b,
                                   0x30};  // MOV R14,[RAX].
    code.insert(code.end(), std::begin(exit_program), std::end(exit_program));
    GuestFile guest{code, PF_R | PF_X};
    for (const auto scope :
         {swift::runtime::ir::Scope::Block, swift::runtime::ir::Scope::Function}) {
        swift::runtime::platform::HostRuntime host{false};
        execution::RuntimeOptions options;
        options.mode = execution::ExecutionMode::JitOnly;
        options.scope = scope;
        options.decode_timing = true;
        execution::RuntimeStats stats;
        execution::VM vm{options, stats, host.Bind()};
        vm.PrepareGuest(guest.Path());
        const auto missing =
                vm.Memory().Map(0, vm.Memory().PageSize(), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS);
        Check(missing != UINT64_MAX, "map a real inaccessible native load destination");
        const auto fault_pc = vm.State().pc + 8;
        constexpr std::uint64_t original_r12 = 0x1122334455667788,
                                original_r14 = 0x8877665544332211;
        vm.State().gprs[x86::RAX] = missing;
        vm.State().gprs[x86::R12] = original_r12;
        vm.State().gprs[x86::R14] = original_r14;
        const std::array<std::uint32_t, 4> left{0x3f800000, 0x12345678, 0x9abcdef0, 0xa5a55a5a};
        const std::array<std::uint32_t, 4> right{0, 0x87654321, 0x0fedcba9, 0x5a5aa5a5};
        std::memcpy(vm.State().xmm[0], left.data(), sizeof left);
        std::memcpy(vm.State().xmm[1], right.data(), sizeof right);
        vm.State().mxcsr = 0x1fa0;  // Preexisting PE; the masked divide newly raises ZE.
        const auto result = RunWithHostFP(vm);
        std::array<std::uint32_t, 4> actual{}, unchanged{};
        std::memcpy(actual.data(), vm.State().xmm[0], sizeof actual);
        std::memcpy(unchanged.data(), vm.State().xmm[1], sizeof unchanged);
        Check(result.guest_fault && result.signal_number == SIGSEGV &&
                      result.signal_code == SEGV_ACCERR && stats.interpreted_steps == 0 &&
                      stats.compile_attempts == 1 && stats.code_units_published == 1 &&
                      stats.fault_sites == 1 && stats.native_bytes,
              "one native unit reaches the actual inaccessible load through its SIGSEGV owner");
        Check(vm.State().mxcsr == 0x1fa4,
              "native memory fault retains preexisting PE and newly accrued masked divide ZE");
        Check(vm.State().pc == fault_pc && vm.State().gprs[x86::RAX] == missing &&
                      vm.State().gprs[x86::R12] == original_r12 + 1 &&
                      vm.State().gprs[x86::R14] == original_r14 &&
                      actual ==
                              std::array<std::uint32_t, 4>{0x7f800000, left[1], left[2], left[3]} &&
                      unchanged == right,
              "fatal load publishes exact PC and completed GPR/XMM changes without its "
              "destination");
        Check(result.host == host_fp_fixture,
              "fatal native memory recovery preserves exact caller FPCR and FPSR");
        Check(vm.Close().Complete(), "native FP fault fixture releases mappings");
    }
}
#endif

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
        (void)vm.RunPrepared();
        const auto snapshot = vm.CaptureException();
        const bool fault =
                vm.StopReason() == swift::runtime::GuestStopReason::GuestSignalTermination &&
                vm.TerminationSignal() == SIGSEGV && snapshot &&
                snapshot->info.kind == swift::runtime::GuestExceptionKind::Memory;
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
            const bool expected_failure =
                    rejection == Rejection::SharedWithoutHardware
                            ? failure == swift::ErrorKind::Unsupported
                            : vm.StopReason() == swift::runtime::GuestStopReason::
                                                         GuestSignalTermination &&
                                      vm.TerminationSignal() == SIGSEGV;
            Check(expected_failure && state.pc == pc &&
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

void ArmSupportedFPControl(unsigned pinned_slots) {
    // FADD D0,D1,D2; MOV X0,42; MOV X8,93; SVC 0.
    constexpr std::array<std::uint32_t, 4> instructions{
            0x1e622820, 0xd2800540, 0xd2800ba8, 0xd4000001};
    const std::span<const std::uint8_t> bytes{
            reinterpret_cast<const std::uint8_t*>(instructions.data()), sizeof(instructions)};
    GuestFile guest{bytes, PF_R | PF_X, EM_AARCH64};
    for (const auto scope :
         {swift::runtime::ir::Scope::Block, swift::runtime::ir::Scope::Function}) {
        for (const auto mode : {execution::ExecutionMode::JitOnly,
                                execution::ExecutionMode::InterpretOnly,
                                execution::ExecutionMode::Mixed}) {
            for (unsigned control = 0; control < 16; ++control) {
                execution::RuntimeOptions options;
                options.scope = scope;
                options.mode = mode;
                options.hot_threshold = 0;
                options.pinned_slots = pinned_slots;
                execution::RuntimeStats stats;
                swift::runtime::platform::HostRuntime host{false};
                execution::VM vm{options, stats, host.Bind()};
                vm.PrepareGuest(guest.Path());
                auto& state = vm.ArmState();
                const auto fpcr = ((control & 3) << 22) | ((control & 4) ? 1u << 24 : 0) |
                                  ((control & 8) ? 1u << 25 : 0);
                state.fpcr = fpcr;
                // 1 + 2^-53 is halfway between adjacent doubles: only +infinity
                // rounds upward. Upper destination bits must become zero.
                constexpr std::uint64_t lhs = 0x3ff0000000000000, rhs = 0x3ca0000000000000;
                std::memset(state.q[0], 0xff, sizeof(state.q[0]));
                std::memcpy(state.q[1], &lhs, sizeof(lhs));
                std::memcpy(state.q[2], &rhs, sizeof(rhs));
                REQUIRE(vm.RunPrepared() == 42);
                std::array<std::uint64_t, 2> result;
                std::memcpy(result.data(), state.q[0], sizeof(result));
                CHECK(result[0] == lhs + ((control & 3) == 1));
                CHECK(result[1] == 0);
                CHECK(state.fpcr == fpcr);
                CHECK(state.fpsr == (1u << 4));
                if (mode != execution::ExecutionMode::InterpretOnly) {
                    CHECK(stats.native_bytes > 0);
                    CHECK(stats.interpreted_steps == 0);
                    CHECK(stats.pinned_uniform_slots == pinned_slots);
                }
                CHECK(vm.Close().Complete());
            }
        }
    }
}

void ArmFPBinaryBoundaryState() {
    // Fast scalar; FPSR observation; exact FDIV helper; mixed-lane vector;
    // indexed multiply ignoring an sNaN in the unselected lane; FPSR reset;
    // subnormal slow path; actual cross-page vector load fault.
    constexpr std::array<std::uint32_t, 13> instructions{0x1e622820,
                                                         0xd53b4423,
                                                         0x1e651883,
                                                         0x4e68d4e6,
                                                         0x4fcb9949,
                                                         0xd53b4424,
                                                         0xd51b442c,
                                                         0x1e2e29ac,
                                                         0xd53b4425,
                                                         0x3dc0002f,
                                                         0xd2800540,
                                                         0xd2800ba8,
                                                         0xd4000001};
    const std::span<const std::uint8_t> bytes{
            reinterpret_cast<const std::uint8_t*>(instructions.data()), sizeof(instructions)};
    GuestFile guest{bytes, PF_R | PF_X, EM_AARCH64};
    for (const auto scope : {swift::runtime::ir::Scope::Block, swift::runtime::ir::Scope::Function})
        for (const auto pins : {0u, 2u})
            for (const auto mode :
                 {execution::ExecutionMode::JitOnly, execution::ExecutionMode::InterpretOnly}) {
                execution::RuntimeOptions options;
                options.scope = scope;
                options.mode = mode;
                options.pinned_slots = pins;
                execution::RuntimeStats stats;
                swift::runtime::platform::HostRuntime host{false};
                execution::VM vm{options, stats, host.Bind()};
                vm.PrepareGuest(guest.Path());
                auto& state = vm.ArmState();
                const auto fault_pc = state.pc + 9 * 4;
                const auto page = vm.Memory().PageSize();
                const auto memory = vm.Memory().Map(
                        0, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
                REQUIRE(memory != UINT64_MAX);
                REQUIRE(vm.Memory().Protect(memory + page, page, PROT_NONE) == 0);
                state.gprs[1] = memory + page - 8;
                state.gprs[12] = 0x08000004;
                state.fpsr = 0x08000008;
                const auto vector = [&](unsigned reg, std::uint64_t low, std::uint64_t high = 0) {
                    const std::array value{low, high};
                    std::memcpy(state.q[reg], value.data(), sizeof(value));
                };
                vector(0, ~0ull, ~0ull);
                vector(1, 0x3ff0000000000000);
                vector(2, 0x3ca0000000000000);
                vector(4, 0x3ff0000000000000);
                vector(5, 0);
                vector(7, 0x3ff0000000000000, 0x7ff0000000000042);
                vector(8, 0x4000000000000000, 0x3ff0000000000000);
                vector(10, 0x3ff8000000000000, 0x8000000000000000);
                vector(11, 0x7ff0000000000042, 0x4000000000000000);
                vector(13, 0x3f800000);
                vector(14, 1);
                vector(15, 0x1122334455667788, 0x8877665544332211);
                unsigned previous{}, after{};
#if defined(__x86_64__)
                asm volatile("stmxcsr %0" : "=m"(previous));
                const unsigned host_control = 0x7fa1;
                asm volatile("ldmxcsr %0" ::"m"(host_control));
#endif
                (void)vm.RunPrepared();
#if defined(__x86_64__)
                asm volatile("stmxcsr %0" : "=m"(after));
                asm volatile("ldmxcsr %0" ::"m"(previous));
                CHECK(after == host_control);
#endif
                const auto fault = vm.CaptureException();
                REQUIRE(fault);
                CHECK(fault->info.instruction_pc == fault_pc);
                CHECK(fault->info.access_bytes == 16);
                CHECK(fault->info.access == swift::runtime::GuestExceptionAccess::Read);
                CHECK(state.gprs[3] == 0x08000018);
                CHECK(state.gprs[4] == 0x0800001b);
                CHECK(state.gprs[5] == 0x08000014);
                CHECK(state.fpsr == 0x08000014);
                const auto expect = [&](unsigned reg, std::uint64_t low, std::uint64_t high) {
                    std::array<std::uint64_t, 2> actual;
                    std::memcpy(actual.data(), state.q[reg], sizeof(actual));
                    CHECK(actual == (std::array{low, high}));
                };
                expect(0, 0x3ff0000000000000, 0);
                expect(3, 0x7ff0000000000000, 0);
                expect(6, 0x4008000000000000, 0x7ff8000000000042);
                expect(9, 0x4008000000000000, 0x8000000000000000);
                expect(12, 0x3f800000, 0);
                expect(15, 0x1122334455667788, 0x8877665544332211);
                if (mode == execution::ExecutionMode::JitOnly) {
                    CHECK(stats.native_bytes > 0);
                    CHECK(stats.interpreted_steps == 0);
                    CHECK(stats.fallbacks == 0);
                }
                CHECK(vm.Close().Complete());
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

TEST_CASE("FaultMetadataLifetime", "[compile][execution]") {
    CHECK_NOTHROW(FaultMetadataLifetime());
}

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

#if defined(__x86_64__)
TEST_CASE("X64 recovers guest memory faults with explicit PC stores", "[native][execution][x64]") {
    CHECK_NOTHROW(X64ExplicitPCFaults());
}
#endif

TEST_CASE("ARM FP controls retain the selected host environment",
          "[native][execution][floating][arm64-guest]") {
    ArmSupportedFPControl(0);
    ArmSupportedFPControl(2);
}

TEST_CASE("ARM FP status survives helpers, state services and a vector memory fault",
          "[native][execution][floating][arm64-guest]") {
    ArmFPBinaryBoundaryState();
}

TEST_CASE("SSE exception state", "[native][execution][floating]") {
    CHECK_NOTHROW(SSEExceptionState());
}

#if defined(__aarch64__)
TEST_CASE("Native FP environment preservation", "[native][execution][floating][arm64]") {
    CHECK_NOTHROW(NativeFPEnvironmentPreservation());
}

TEST_CASE("SSE status survives native memory fault", "[native][execution][floating][arm64]") {
    CHECK_NOTHROW(SSEStatusSurvivesNativeMemoryFault());
}
#endif

TEST_CASE("Unaligned atomic owner", "[native][execution][atomic]") {
    CHECK_NOTHROW(UnalignedAtomicOwner());
}

TEST_CASE("Atomic faults before commit", "[native][execution][atomic]") {
    CHECK_NOTHROW(AtomicFaultBeforeCommit());
}

TEST_CASE("Embedding exception callback and owning snapshot",
          "[native][execution][exception-api]") {
    CHECK_NOTHROW(EmbeddedExceptionContext());
}

TEST_CASE("CMPXCHG16B owner", "[native][execution][atomic][atomic128]") {
    CHECK_NOTHROW(Atomic128Owner());
}

TEST_CASE("Illegal decode retains executed prefix", "[native][execution][exception-api]") {
    CHECK_NOTHROW(IllegalDecodePrefix());
}

TEST_CASE("CMPXCHG16B rejects before commit", "[native][execution][atomic][atomic128]") {
    CHECK_NOTHROW(Atomic128RejectBeforeCommit());
}

TEST_CASE("ARM guest unsupported FP controls", "[native][execution][arm64-guest]") {
    CHECK_NOTHROW(ArmUnsupportedFPControl());
}

TEST_CASE("ARM FPCR domains compile on demand and reuse every supported profile",
          "[native][execution][floating][arm64-guest]") {
    namespace arm = swift::runtime::arm64;
    constexpr std::array<std::uint32_t, 5> instructions{
            0xb100053f, 0x1e622820, 0x1e622803, 0x1e620824, 0xd65f03c0};
    const std::span<const std::uint8_t> bytes{
            reinterpret_cast<const std::uint8_t*>(instructions.data()), sizeof(instructions)};
    GuestFile guest{bytes, PF_R | PF_X, EM_AARCH64};
    for (auto scope : {swift::runtime::ir::Scope::Block, swift::runtime::ir::Scope::Function}) {
        execution::RuntimeOptions options;
        options.scope = scope;
        execution::RuntimeStats stats;
        swift::runtime::platform::HostRuntime host{false};
        const auto backend = host.Bind();
        execution::VM vm{options, stats, backend};
        vm.PrepareGuest(guest.Path());
        const auto entry = vm.ArmState().pc, stack = vm.ArmState().gprs[31];
        for (unsigned round = 0; round < 2; ++round) {
            for (unsigned profile = 0; profile < 32; ++profile) {
                const auto domain = round ? 31 - profile : profile;
                auto& state = vm.ArmState();
                state.fpcr = arm::FPCRForDomain(domain);
                state.fpsr = 0x08000000;
                state.gprs[9] = UINT64_MAX;
                state.gprs[30] = 0;
                state.packed_flags = (std::uint64_t{1} << 55) | 0x25;
                const std::uint64_t left = round ? 0x7ff0000000000042 : 0x3ff0000000000000;
                const std::uint64_t right = domain & 1 ? 1 : 0x3ca0000000000000;
                std::memcpy(state.q[1], &left, 8);
                std::memcpy(state.q[2], &right, 8);
                const auto first = arm::EvaluateBinaryFP(left, right, 64, 2, state.fpcr);
                const auto second = arm::EvaluateBinaryFP(first.value, right, 64, 2, state.fpcr);
                const auto third = arm::EvaluateBinaryFP(left, right, 64, 0, state.fpcr);
                vm.CallGuest(entry, stack, 0);
                const auto result = [&](unsigned reg, std::uint64_t expected) {
                    std::array<std::uint64_t, 2> value;
                    std::memcpy(value.data(), state.q[reg], sizeof(value));
                    CHECK(value == (std::array{expected, std::uint64_t{0}}));
                };
                result(0, first.value);
                result(3, second.value);
                result(4, third.value);
                CHECK(state.fpsr ==
                      (0x08000000 | first.exceptions | second.exceptions | third.exceptions));
                CHECK(state.packed_flags == ((std::uint64_t{1} << 55) | 0x60000025));
                if (backend.specializes_arm_fpcr)
                    CHECK(stats.compile_attempts == (round ? 32 : profile + 1));
            }
        }
        CHECK(stats.interpreted_steps == 0);
        CHECK(stats.fallbacks == 0);
        CHECK(vm.Close().Complete());
    }
}
#endif
