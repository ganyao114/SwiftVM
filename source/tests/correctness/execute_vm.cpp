#include "execute.h"

#if defined(__linux__) || defined(__ANDROID__)

#include "arch/state.h"
#include "base/error.h"
#include "runtime/execution/vm.h"
#include "runtime/ir/program.h"
#include "runtime/platform/linux/host_runtime.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <elf.h>
#include <sys/mman.h>
#include <unistd.h>

#if !defined(SVM_TEST_BOUNDARY)
#error "The correctness VM must link the test boundary runtime variant"
#endif

namespace swift::tests::correctness {
namespace {
namespace runtime = ::swift::runtime;
namespace execution = runtime::execution;

constexpr std::uint64_t CodeGranule = 65536;

struct Image {
    std::vector<std::uint8_t> bytes;
    std::uint64_t code_begin{}, code_size{};
};

template <typename Header, typename Segment> Image GuestImage(const Case& input) {
    Image out;
    out.code_begin = input.code_address & ~(CodeGranule - 1);
    const auto code_end =
            (input.code_address + input.code.size() + CodeGranule - 1) & ~(CodeGranule - 1);
    out.code_size = code_end - out.code_begin;
    if (out.code_begin < CodeGranule || !out.code_size)
        swift::ThrowError("invalid correctness code extent", swift::ErrorKind::InvalidArgument);
    out.bytes.resize(static_cast<std::size_t>(CodeGranule + out.code_size));
    Header header{};
    std::memcpy(header.e_ident, ELFMAG, SELFMAG);
    header.e_ident[EI_CLASS] = sizeof(Header) == sizeof(Elf32_Ehdr) ? ELFCLASS32 : ELFCLASS64;
    header.e_ident[EI_DATA] = ELFDATA2LSB;
    header.e_ident[EI_VERSION] = EV_CURRENT;
    header.e_type = ET_EXEC;
    header.e_machine = input.isa == ISA::Arm32 ? EM_ARM
                       : input.isa == ISA::Arm64 ? EM_AARCH64
                                                  : EM_X86_64;
    header.e_version = EV_CURRENT;
    header.e_entry = input.initial.pc |
                     (input.isa == ISA::Arm32 ? ((input.initial.flags >> 5) & 1) : 0);
    header.e_phoff = sizeof(Header);
    header.e_ehsize = sizeof(Header);
    header.e_phentsize = sizeof(Segment);
    header.e_phnum = 2;
    if constexpr (sizeof(Header) == sizeof(Elf32_Ehdr)) header.e_flags = 0x05000000;
    Segment segments[2]{};
    segments[0].p_type = PT_LOAD;
    segments[0].p_vaddr = segments[0].p_paddr = out.code_begin - CodeGranule;
    segments[0].p_filesz = segments[0].p_memsz = CodeGranule;
    segments[0].p_flags = PF_R;
    segments[0].p_align = CodeGranule;
    segments[1].p_type = PT_LOAD;
    segments[1].p_offset = CodeGranule;
    segments[1].p_vaddr = segments[1].p_paddr = out.code_begin;
    segments[1].p_filesz = segments[1].p_memsz = out.code_size;
    segments[1].p_flags = PF_R | PF_X;
    segments[1].p_align = CodeGranule;
    std::memcpy(out.bytes.data(), &header, sizeof(header));
    std::memcpy(out.bytes.data() + sizeof(header), segments, sizeof(segments));
    std::copy(input.code.begin(),
              input.code.end(),
              out.bytes.begin() + CodeGranule + input.code_address - out.code_begin);
    return out;
}

void Materialize(execution::VM& vm, const Case& input) {
    auto& memory = vm.Memory();
    if (memory.PageSize() > CodeGranule)
        swift::ThrowError("correctness layout requires pages no larger than 64 KiB",
                          swift::ErrorKind::Unsupported);
    auto image = input.isa == ISA::Arm32 ? GuestImage<Elf32_Ehdr, Elf32_Phdr>(input)
                                        : GuestImage<Elf64_Ehdr, Elf64_Phdr>(input);
    vm.PrepareGuest(image.bytes, "correctness-case");
    // The Linux bootstrap is not an input to this ISA case. Retire its
    // headers, stack, brk reservation and kuser mappings through the real owner.
    const std::vector<execution::MemoryRegion> regions(memory.Regions().begin(),
                                                      memory.Regions().end());
    for (const auto& region : regions) {
        if (region.address == image.code_begin && region.size == image.code_size) continue;
        if (memory.Unmap(region.address, region.size))
            swift::ThrowSystemError("remove correctness bootstrap mapping", errno);
    }
    for (const auto& region : input.memory) {
        if (memory.Map(region.address,
                       region.size,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED) == UINT64_MAX)
            swift::ThrowSystemError("map correctness case memory", errno);
        if (!memory.CopyOut(region.address, region.bytes.data(), region.bytes.size()))
            swift::ThrowSystemError("initialize correctness case memory", errno);
        if (memory.Protect(region.address, region.size, region.protection))
            swift::ThrowSystemError("protect correctness case memory", errno);
    }
    if (!memory.CodeAt(input.terminal_pc).empty())
        swift::ThrowError("correctness terminal must lie outside executable source",
                          swift::ErrorKind::InvalidArgument);
    if (!memory.Contains(input.terminal_pc, 1, 0)) {
        const auto page = memory.PageSize();
        const auto guard = input.terminal_pc & ~(std::uint64_t(page) - 1);
        if (memory.Map(guard,
                       page,
                       PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED) == UINT64_MAX)
            swift::ThrowSystemError("map correctness terminal guard", errno);
    }
    if (input.isa == ISA::Arm32)
        Inject(input.initial, vm.Arm32State());
    else if (input.isa == ISA::Arm64)
        Inject(input.initial, vm.ArmState());
    else
        Inject(input.initial, vm.State());
}

State ProjectVM(execution::VM& vm) {
    if (vm.ISA() == runtime::GuestISA::Arm32) return Project(vm.Arm32State());
    if (vm.ISA() == runtime::GuestISA::Arm64) return Project(vm.ArmState());
    return Project(vm.State());
}

std::vector<MemoryRegion> CaptureMemory(execution::VM& vm, const Case& input) {
    auto output = input.memory;
    auto& memory = vm.Memory();
    for (auto& region : output) {
        const bool temporary_read = !(region.protection & PROT_READ);
        if (temporary_read &&
            memory.Protect(region.address, region.size, region.protection | PROT_READ))
            swift::ThrowSystemError("read correctness case protection", errno);
        region.bytes.resize(region.size);
        const auto copied = memory.CopyIn(region.bytes.data(), region.address, region.size);
        const auto saved_error = errno;
        if (temporary_read && memory.Protect(region.address, region.size, region.protection))
            swift::ThrowSystemError("restore correctness case protection", errno);
        if (!copied) swift::ThrowSystemError("capture correctness case memory", saved_error);
    }
    return output;
}

const char* FaultKind(runtime::GuestExceptionKind kind) {
    switch (kind) {
        case runtime::GuestExceptionKind::Memory: return "memory";
        case runtime::GuestExceptionKind::Alignment: return "alignment";
        case runtime::GuestExceptionKind::Divide: return "divide";
        case runtime::GuestExceptionKind::IllegalInstruction: return "illegal_instruction";
        case runtime::GuestExceptionKind::FloatingPoint: return "floating_point";
        case runtime::GuestExceptionKind::Breakpoint: return "breakpoint";
        case runtime::GuestExceptionKind::AsyncSignal: return "async_signal";
    }
    return "unknown";
}

const char* FaultAccess(runtime::GuestExceptionAccess access) {
    switch (access) {
        case runtime::GuestExceptionAccess::Unknown: return "unknown";
        case runtime::GuestExceptionAccess::Read: return "read";
        case runtime::GuestExceptionAccess::Write: return "write";
        case runtime::GuestExceptionAccess::Atomic: return "atomic";
        case runtime::GuestExceptionAccess::Execute: return "execute";
    }
    return "unknown";
}

struct FaultCapture {
    bool seen{};
    runtime::GuestExceptionInfo event;

    static runtime::GuestExceptionDisposition Stop(void* opaque,
                                                   runtime::GuestContextView,
                                                   const runtime::GuestExceptionInfo& event) {
        auto& self = *static_cast<FaultCapture*>(opaque);
        self.seen = true;
        self.event = event;
        return runtime::GuestExceptionDisposition::Stop;
    }
};

struct CompileCapture {
    using Compile = decltype(execution::HostBackendOps::compile);
    Compile compile{};
    Observation& output;
    CompileCapture* previous{};
    static thread_local CompileCapture* active;

    CompileCapture(execution::HostBackendOps& ops, Observation& output)
            : compile(ops.compile), output(output), previous(active) {
        active = this;
        if (compile) ops.compile = Capture;
    }

    ~CompileCapture() { active = previous; }

    static runtime::backend::CompileResult& Capture(void* context,
                                                    const runtime::ir::Program& program,
                                                    execution::HostCompileRequest request) {
        auto& self = *active;
        auto& output = self.output;
        output.stage = "compile";
        auto& result = self.compile(context, program, request);
        if (result) {
            output.actual_scope = static_cast<Scope>(program.ScopeKind());
            output.actual_ir_blocks =
                    std::max(output.actual_ir_blocks, std::uint32_t(program.Blocks().size()));
            output.spill_slots += result.spill_slots;
            output.frame_bytes = std::max(output.frame_bytes, result.frame_bytes);
            output.stage = "execute";
        }
        return result;
    }
};

thread_local CompileCapture* CompileCapture::active{};

void CaptureStats(const execution::RuntimeStats& stats, Observation& output) {
    output.compile_attempts = stats.compile_attempts;
    output.compiled_units = stats.code_units_published;
    output.interpreted_steps = stats.interpreted_steps;
    output.fallbacks = stats.fallbacks;
}

// A guest branch may form a native loop that never returns to the dispatch
// boundary; the worker then cannot report a budget on its own. The provisional
// observation written by the worker is what this exit leaves behind.
class ExecutionWatchdog {
  public:
    ExecutionWatchdog() {
        struct sigaction action {};
        action.sa_handler = [](int) { ::_exit(0); };
        sigemptyset(&action.sa_mask);
        if (::sigaction(SIGALRM, &action, &previous_)) return;
        active_ = true;
        ::alarm(BudgetSeconds);
    }

    ~ExecutionWatchdog() {
        if (!active_) return;
        ::alarm(0);
        ::sigaction(SIGALRM, &previous_, nullptr);
    }

    ExecutionWatchdog(const ExecutionWatchdog&) = delete;
    ExecutionWatchdog& operator=(const ExecutionWatchdog&) = delete;

  private:
    static constexpr unsigned BudgetSeconds = 8;
    struct sigaction previous_ {};
    bool active_{};
};
}  // namespace

// The test variant shares all execution, publication and recovery code with
// production. Only its existing cold return decisions accept a physical stop.
struct VMAccess {
    static std::uint64_t Run(execution::VM& vm, std::uint64_t terminal) {
        vm.host_return_pc = terminal;
        vm.host_call_active = true;
        vm.host_returned = false;
        vm.test_boundary_active = true;
        vm.test_boundary_service = 0;
        vm.test_boundary_native_status = 0;
        try {
            vm.UpdateFPEnvironment(true);
            vm.Execute();
        } catch (...) {
            vm.test_boundary_active = vm.host_call_active = false;
            throw;
        }
        vm.test_boundary_active = vm.host_call_active = false;
        return vm.test_boundary_service;
    }

    static bool Returned(const execution::VM& vm) { return vm.host_returned; }

    static bool NativeSemanticExit(const execution::VM& vm) {
        switch (static_cast<runtime::NativeExit>(vm.test_boundary_native_status & 255)) {
            case runtime::NativeExit::SemanticException:
            case runtime::NativeExit::DivideFault:
            case runtime::NativeExit::AlignmentFault:
            case runtime::NativeExit::MemoryFault:
            case runtime::NativeExit::FloatingFault:
            case runtime::NativeExit::X87Fault:
                return true;
            default:
                return false;
        }
    }
};

Backend NativeBackend() noexcept {
#if defined(__aarch64__)
    return Backend::Arm64;
#elif defined(__x86_64__)
    return Backend::X64;
#elif defined(__riscv) && __riscv_xlen == 64
    return Backend::Riscv64;
#elif defined(__loongarch64)
    return Backend::LoongArch64;
#else
    return Backend::Direct;
#endif
}

Observation ExecuteVM(const Case& input, const RunConfig& config) {
    if (config.compile_only) return ExecuteCompile(input, config);
    Observation output;
    output.backend = std::string(Name(config.backend));
    output.state = input.initial;
    output.stage = "prepare";
    if ((config.backend != Backend::Direct && config.backend != NativeBackend()) ||
        (!config.pin && config.backend != Backend::Direct)) {
        output.outcome = Outcome::Unavailable;
        output.error = !config.pin ? "runtime has no complete unpinned entry ABI binding"
                                   : "requested backend is not the native host ISA";
        return output;
    }
    runtime::platform::HostRuntime host{false};
    auto ops = host.Bind();
    ops.target.mode = static_cast<runtime::ir::TargetMode>(config.target);
    CompileCapture compile{ops, output};
    FaultCapture fault;
    execution::RuntimeStats stats;
    execution::RuntimeOptions options;
    options.mode = config.backend == Backend::Direct ? execution::ExecutionMode::InterpretOnly
                                                      : execution::ExecutionMode::JitOnly;
    options.scope = static_cast<runtime::ir::Scope>(config.scope);
    options.optimize = config.optimize;
    if (input.isa == ISA::Arm32 && input.profile == "armv8-user")
        options.arm32_profile.version = runtime::arm32::ArchVersion::v8;
    options.exception_handler = FaultCapture::Stop;
    options.exception_opaque = &fault;
    try {
        execution::VM vm{options, stats, ops};
        Materialize(vm, input);
        output.stage = "execute";
        ExecutionWatchdog watchdog;
        try {
            output.service = VMAccess::Run(vm, input.terminal_pc);
            if (fault.seen) {
                const auto& event = fault.event;
                output.outcome = Outcome::Fault;
                output.fault_kind = FaultKind(event.kind);
                output.fault_access = FaultAccess(event.access);
                output.fault_pc = event.instruction_pc;
                output.fault_address = event.address;
                output.fault_address_valid = event.address_valid;
                output.fault_width = event.access_bytes;
                output.fault_vector = event.exception_vector == UINT16_MAX
                                              ? -1
                                              : event.exception_vector;
            } else if (output.service)
                output.outcome = Outcome::Service;
            else if (VMAccess::Returned(vm))
                output.outcome = Outcome::Terminal;
            else {
                output.outcome = Outcome::HostFailure;
                output.error = "guest stopped without a test boundary or exception";
            }
        } catch (const swift::RuntimeError& error) {
            output.outcome = error.Kind() == swift::ErrorKind::Unsupported
                                     ? Outcome::Rejected
                                     : Outcome::HostFailure;
            output.error = error.what();
        }
        output.state = ProjectVM(vm);
        output.memory = CaptureMemory(vm, input);
        const auto released = vm.Close();
        if (!released.Complete()) {
            output.outcome = Outcome::HostFailure;
            output.error = "correctness VM mapping cleanup failed";
        }
        output.native_executed = config.backend != Backend::Direct &&
                                 stats.code_units_published && !stats.interpreted_steps &&
                                 (output.outcome == Outcome::Terminal ||
                                  output.outcome == Outcome::Service ||
                                  (fault.seen &&
                                   (fault.event.origin == runtime::GuestExceptionOrigin::Native ||
                                    VMAccess::NativeSemanticExit(vm))));
        CaptureStats(stats, output);
    } catch (const swift::RuntimeError& error) {
        output.outcome = error.Kind() == swift::ErrorKind::Unsupported ||
                                         error.SystemError() == EEXIST ||
                                         error.SystemError() == ENOMEM
                                 ? Outcome::Unavailable
                                 : Outcome::HostFailure;
        output.error = error.what();
        CaptureStats(stats, output);
    } catch (const std::exception& error) {
        output.outcome = Outcome::HostFailure;
        output.error = error.what();
        CaptureStats(stats, output);
    }
    return output;
}

}  // namespace swift::tests::correctness

#else

namespace swift::tests::correctness {
Backend NativeBackend() noexcept {
#if defined(__aarch64__)
    return Backend::Arm64;
#elif defined(__x86_64__)
    return Backend::X64;
#else
    return Backend::Direct;
#endif
}

Observation ExecuteVM(const Case& input, const RunConfig& config) {
    if (config.compile_only) return ExecuteCompile(input, config);
    Observation output;
    output.outcome = Outcome::Unavailable;
    output.state = input.initial;
    output.backend = std::string(Name(config.backend));
    output.stage = "prepare";
    output.error = "native correctness execution requires Linux or Android";
    return output;
}
}  // namespace swift::tests::correctness

#endif
