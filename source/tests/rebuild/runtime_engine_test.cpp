#include "runtime/execution/runtime.h"
#include "runtime/execution/vm.h"
#include "runtime/platform/linux/arm64_runtime.h"

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
namespace execution = swift::runtime::execution;
namespace backend = swift::runtime::backend;
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
class GuestFile {
public:
    GuestFile() {
        const int fd = mkstemp(path.data());
        Check(fd >= 0, "cannot create engine-test guest");
        const std::uint8_t instructions[]{0xb8, 60, 0, 0, 0, 0xbf, 42, 0, 0, 0, 0x0f, 0x05};
        Elf64_Ehdr header{};
        std::memcpy(header.e_ident, ELFMAG, SELFMAG);
        header.e_ident[EI_CLASS] = ELFCLASS64;
        header.e_ident[EI_DATA] = ELFDATA2LSB;
        header.e_ident[EI_VERSION] = EV_CURRENT;
        header.e_type = ET_EXEC;
        header.e_machine = EM_X86_64;
        header.e_version = EV_CURRENT;
        header.e_entry = 0x400000 + sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr);
        header.e_phoff = sizeof(Elf64_Ehdr);
        header.e_ehsize = sizeof(Elf64_Ehdr);
        header.e_phentsize = sizeof(Elf64_Phdr);
        header.e_phnum = 1;
        Elf64_Phdr segment{};
        segment.p_type = PT_LOAD;
        segment.p_flags = PF_R | PF_W | PF_X;
        segment.p_vaddr = 0x400000;
        segment.p_paddr = segment.p_vaddr;
        segment.p_filesz = segment.p_memsz =
                sizeof(header) + sizeof(segment) + sizeof(instructions);
        segment.p_align = 4096;
        std::vector<std::uint8_t> image(segment.p_filesz);
        std::memcpy(image.data(), &header, sizeof(header));
        std::memcpy(image.data() + sizeof(header), &segment, sizeof(segment));
        std::memcpy(image.data() + sizeof(header) + sizeof(segment),
                    instructions,
                    sizeof(instructions));
        const auto written = write(fd, image.data(), image.size());
        const auto closed = close(fd);
        Check(written == static_cast<ssize_t>(image.size()) && closed == 0,
              "cannot write engine-test guest");
    }
    ~GuestFile() { unlink(path.data()); }
    std::string Path() const { return path.data(); }

private:
    std::array<char, 64> path{"/data/local/tmp/swiftvm-engine-test-XXXXXX"};
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
#if defined(__aarch64__)
void Mixed(const std::string& path, bool invalid) {
    swift::runtime::platform::Arm64Runtime host{false};
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
        Check(!stopped && code == 42 && stats.compile_attempts == 3 && stats.native_bytes == 0,
              "mixed resumes each unsupported request from actual guest PC/state");
}
#endif
}  // namespace
int main() {
    try {
        GuestFile guest;
        SourceAdmission();
        Interpreter(guest.Path());
#if defined(__aarch64__)
        Mixed(guest.Path(), false);
        Mixed(guest.Path(), true);
#endif
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts("runtime engine contracts passed");
}
