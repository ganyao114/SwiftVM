#include "call.h"
#include "elf.h"

#include "base/error.h"
#include "runtime/execution/vm.h"
#include "runtime/platform/linux/artifact_file.h"
#include "runtime/platform/linux/host_runtime.h"

#include <algorithm>
#include <cerrno>
#include <unordered_map>
#include <sys/mman.h>

namespace swift::aot {
namespace execution = runtime::execution;

namespace {
constexpr std::size_t StackBytes = 1u << 20;
}

struct AotGuestEnv::Impl {
    struct Scratch {
        std::uint64_t address{}, mapping{};
        std::size_t size{};
    };

    runtime::platform::HostRuntime host;
    execution::RuntimeStats stats;
    execution::VM vm;
    Artifact artifact;
    std::vector<Scratch> scratch;
    std::unordered_map<std::string, std::uint64_t> resolved;
    std::uint64_t stack_top{}, sentinel{}, initial_pc{}, initial_sp{};
    swift::Failure failure;

    explicit Impl(execution::RuntimeOptions options)
            : host(options.decode_timing), vm(options, stats, host.Bind()) {}
};

AotGuestEnv::AotGuestEnv() = default;
AotGuestEnv::~AotGuestEnv() = default;

bool AotGuestEnv::Init(const std::string& artifact_path,
                       std::string& error,
                       execution::RuntimeOptions options) {
    return Init({}, artifact_path, error, std::move(options));
}

bool AotGuestEnv::Init(const std::string& guest_path,
                       const std::string& artifact_path,
                       std::string& error,
                       execution::RuntimeOptions options) {
    try {
        if (impl)
            ThrowError("AOT call environment is already initialized", ErrorKind::InvalidState);
        Artifact artifact;
        if (!ReadArtifact(artifact_path, artifact, error)) return false;
        if (!guest_path.empty() && HashBytes(runtime::platform::ReadArtifactFile(guest_path)) !=
                                           artifact.code.key.guest_id)
            ThrowError("supplied guest does not match AOT artifact", ErrorKind::InvalidArgument);
        options.scope = artifact.code.key.requested_scope;
        options.mode = execution::ExecutionMode::JitOnly;
        options.artifact_path.clear();
        auto candidate = std::make_unique<Impl>(options);
        candidate->artifact = std::move(artifact);
        auto& image = candidate->artifact;
        const std::array<std::string, 1> arguments{image.guest_path};
        candidate->vm.PrepareGuest(image.guest_elf, image.guest_path, arguments, {}, &image.code);
        candidate->initial_pc = candidate->vm.State().pc;
        candidate->initial_sp = candidate->vm.State().gprs[runtime::x86::RSP];
        auto& memory = candidate->vm.Memory();
        const auto page = memory.PageSize();
        const auto stack =
                memory.Map(0, StackBytes + page * 2, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS);
        if (stack == UINT64_MAX || memory.Protect(stack + page, StackBytes, PROT_READ | PROT_WRITE))
            ThrowSystemError("map guest call stack", errno);
        candidate->stack_top = stack + page + StackBytes;
        candidate->sentinel = stack;
        impl = std::move(candidate);
        error.clear();
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

bool AotGuestEnv::RunStartupUntil(const std::string& name, std::string& error, bool patch_stop) {
    try {
        if (!impl) ThrowError("AOT call environment is not initialized", ErrorKind::InvalidState);
        if (patch_stop)
            ThrowError(
                    "AOT startup requires PatchGuestStopByte before compilation; runtime patching "
                    "is unsupported",
                    ErrorKind::InvalidArgument);
        const auto* symbol = FindSymbol(impl->artifact, name);
        if (!symbol || symbol->type != 2)
            ThrowError("startup stop symbol is not a defined STT_FUNC", ErrorKind::InvalidArgument);
        // Check the immutable embedded snapshot, not only the current guest
        // byte. A runtime write cannot manufacture an accepted startup stop.
        const elf::View guest{impl->artifact.guest_elf};
        bool patched{};
        for (const auto& segment : guest.Segments()) {
            if (segment.type != elf::Load || !(segment.flags & 1) ||
                symbol->guest_value < segment.vaddr ||
                symbol->guest_value - segment.vaddr >= segment.filesz)
                continue;
            const auto offset = segment.offset + symbol->guest_value - segment.vaddr;
            patched = impl->artifact.guest_elf[offset] == 0xf4;
            break;
        }
        if (!patched)
            ThrowError("startup stop must be pre-patched with F4 before AOT compilation",
                       ErrorKind::InvalidArgument);
        for (const auto& unit : impl->artifact.code.units) {
            if (unit.pc == symbol->guest_value)
                ThrowError("startup stop already has a callable native entry",
                           ErrorKind::InvalidState);
            for (const auto& entry : unit.code.entries)
                if (entry.pc == symbol->guest_value)
                    ThrowError("startup stop already has a callable native entry",
                               ErrorKind::InvalidState);
        }
        impl->vm.State().pc = impl->initial_pc;
        impl->vm.State().gprs[runtime::x86::RSP] = impl->initial_sp;
        impl->vm.RunUntilGuestStop(symbol->guest_value);
        error.clear();
        return true;
    } catch (const std::exception& failure) {
        if (impl) impl->failure.Capture(failure);
        error = failure.what();
        return false;
    }
}

runtime::x86::GuestState& AotGuestEnv::Context() {
    if (!impl) ThrowError("AOT call environment is not initialized", ErrorKind::InvalidState);
    return impl->vm.State();
}

void* AotGuestEnv::HostPointer(std::uint64_t address, std::uint64_t size, bool write) {
    return impl ? impl->vm.HostPointer(address, size, write) : nullptr;
}

std::uint64_t AotGuestEnv::SentinelAddress() const { return impl ? impl->sentinel : 0; }

std::uint64_t AotGuestEnv::CallStackTop() { return impl ? impl->stack_top : 0; }

guest_call::GuestRunReport AotGuestEnv::RunToSentinel() {
    using guest_call::GuestRunOutcome;
    impl->failure.Clear();
    try {
        const auto& state = impl->vm.State();
        impl->vm.CallGuest(state.pc, state.gprs[runtime::x86::RSP], impl->sentinel);
        return {GuestRunOutcome::ReturnedToSentinel};
    } catch (const std::exception& failure) {
        impl->failure.Capture(failure);
        return {GuestRunOutcome::Faulted};
    }
}

std::uint64_t AotGuestEnv::ScratchAlloc(std::uint64_t size, std::uint64_t alignment) {
    if (!impl || !size || !alignment || (alignment & (alignment - 1)) || alignment > (1u << 20) ||
        size > (256u << 20))
        return 0;
    auto& memory = impl->vm.Memory();
    const auto page = memory.PageSize();
    const auto length = static_cast<std::size_t>((size + alignment + page - 1) & ~(page - 1));
    // Reserve the owner row before acquiring a mapping.
    impl->scratch.reserve(impl->scratch.size() + 1);
    const auto mapped = memory.Map(0, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
    if (mapped == UINT64_MAX) return 0;
    const auto address = (mapped + alignment - 1) & ~(alignment - 1);
    impl->scratch.push_back({address, mapped, length});
    return address;
}

void AotGuestEnv::ScratchFree(std::uint64_t address, std::uint64_t) noexcept {
    if (!impl) return;
    const auto found = std::find_if(impl->scratch.begin(),
                                    impl->scratch.end(),
                                    [&](const auto& item) { return item.address == address; });
    if (found == impl->scratch.end()) return;
    try {
        if (impl->vm.Memory().Unmap(found->mapping, found->size)) {
            impl->failure.Set(ErrorKind::System, "unmap guest scratch", errno);
            return;
        }
        impl->scratch.erase(found);
    } catch (const std::exception& error) {
        impl->failure.Capture(error);
    }
}

std::uint64_t AotGuestEnv::LookupSymbol(const std::string& name) {
    if (!impl) return 0;
    if (const auto ready = impl->resolved.find(name); ready != impl->resolved.end())
        return ready->second;
    const auto* symbol = FindSymbol(impl->artifact, name);
    if (!symbol) return 0;
    auto address = symbol->guest_value;
    if (symbol->type == 10) {
        const auto result = guest_call::GuestFn<std::uint64_t()>{this, address}.Try();
        if (!result) throw guest_call::GuestCallError(result.diagnostic());
        address = result.value();
    }
    impl->resolved.emplace(name, address);
    return address;
}

const Artifact& AotGuestEnv::Image() const {
    if (!impl) ThrowError("AOT call environment is not initialized", ErrorKind::InvalidState);
    return impl->artifact;
}

const execution::RuntimeStats& AotGuestEnv::Stats() const {
    if (!impl) ThrowError("AOT call environment is not initialized", ErrorKind::InvalidState);
    return impl->stats;
}

const swift::Failure& AotGuestEnv::LastFailure() const {
    if (!impl) ThrowError("AOT call environment is not initialized", ErrorKind::InvalidState);
    return impl->failure;
}

execution::MappingRelease AotGuestEnv::Close() noexcept {
    return impl ? impl->vm.Close() : execution::MappingRelease{};
}
}  // namespace swift::aot
