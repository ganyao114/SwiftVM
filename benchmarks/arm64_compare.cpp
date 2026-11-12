// Callable ARM64 kernels: identical ELF bytes, memory reset outside timing,
// and an unexecutable return sentinel. This is not a Linux emulator benchmark.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <elf.h>
#include <sys/mman.h>

#if defined(SVM_BENCH_DYNARMIC)
#include <dynarmic/interface/A64/a64.h>
#else
#include "runtime/execution/vm.h"
#include "runtime/platform/linux/host_runtime.h"
#endif

namespace {
using Clock = std::chrono::steady_clock;
using u64 = std::uint64_t;
constexpr u64 StackAddress = 0x70000000, StackSize = 1024 * 1024, StopPC = 0;

u64 Nanoseconds(Clock::time_point begin) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
}

struct Options {
    std::string elf, scope{"function"};
    u64 entry{}, output{}, output_size{};
    unsigned repeats{3}, pins{};
    std::vector<std::uint8_t> expected;
};

u64 Number(const std::string& text) {
    std::size_t used{};
    const auto value = std::stoull(text, &used, 0);
    if (used != text.size()) throw std::runtime_error("invalid numeric argument");
    return value;
}

Options Parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (++i == argc) throw std::runtime_error("missing option value");
        const std::string value = argv[i];
        if (key == "--elf")
            options.elf = value;
        else if (key == "--entry")
            options.entry = Number(value);
        else if (key == "--output")
            options.output = Number(value);
        else if (key == "--output-size")
            options.output_size = Number(value);
        else if (key == "--repeats")
            options.repeats = Number(value);
        else if (key == "--scope")
            options.scope = value;
        else if (key == "--pin-slots")
            options.pins = Number(value);
        else if (key == "--expected") {
            if (value.size() % 2) throw std::runtime_error("invalid expected hex");
            for (std::size_t at = 0; at < value.size(); at += 2) {
                std::size_t used{};
                const auto byte = std::stoul(value.substr(at, 2), &used, 16);
                if (used != 2) throw std::runtime_error("invalid expected hex");
                options.expected.push_back(byte);
            }
        } else
            throw std::runtime_error("unknown option: " + key);
    }
    if (options.elf.empty() || !options.entry || !options.output || !options.output_size ||
        options.expected.size() != options.output_size || options.output_size > 4096 ||
        !options.repeats || options.repeats > 100 || options.pins > 2 ||
        (options.scope != "block" && options.scope != "function"))
        throw std::runtime_error("invalid benchmark options");
    return options;
}

struct Segment {
    u64 address, size, offset, file_size;
    unsigned flags;
};

struct Image {
    std::vector<std::uint8_t> bytes;
    std::vector<Segment> segments;

    explicit Image(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file) throw std::runtime_error("cannot open ELF");
        bytes.assign(std::istreambuf_iterator<char>(file), {});
        if (bytes.size() < sizeof(Elf64_Ehdr)) throw std::runtime_error("truncated ELF");
        Elf64_Ehdr header;
        std::memcpy(&header, bytes.data(), sizeof(header));
        if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) ||
            header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_ident[EI_DATA] != ELFDATA2LSB ||
            header.e_machine != EM_AARCH64 || header.e_type != ET_EXEC ||
            header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phoff > bytes.size() ||
            header.e_phnum > (bytes.size() - header.e_phoff) / sizeof(Elf64_Phdr))
            throw std::runtime_error("expected a static little-endian AArch64 ELF");
        for (unsigned i = 0; i < header.e_phnum; ++i) {
            Elf64_Phdr ph;
            std::memcpy(&ph, bytes.data() + header.e_phoff + i * sizeof(ph), sizeof(ph));
            if (ph.p_type == PT_INTERP) throw std::runtime_error("dynamic ELF unsupported");
            if (ph.p_type != PT_LOAD || !ph.p_memsz) continue;
            if (ph.p_filesz > ph.p_memsz || ph.p_offset > bytes.size() ||
                ph.p_filesz > bytes.size() - ph.p_offset || ph.p_vaddr < 0x10000 ||
                ph.p_memsz > 64 * 1024 * 1024 || ph.p_vaddr > (u64{1} << 32) - ph.p_memsz)
                throw std::runtime_error("invalid benchmark ELF segment");
            segments.push_back({ph.p_vaddr, ph.p_memsz, ph.p_offset, ph.p_filesz, ph.p_flags});
        }
        if (segments.empty()) throw std::runtime_error("ELF has no load segments");
    }

    bool Contains(u64 address, u64 size, unsigned flags) const {
        for (const auto& segment : segments)
            if ((segment.flags & flags) == flags && address >= segment.address &&
                address - segment.address <= segment.size &&
                size <= segment.size - (address - segment.address))
                return true;
        return false;
    }
};

struct Snapshot {
    struct Range {
        u64 address;
        std::vector<std::uint8_t> bytes;
    };

    std::vector<Range> ranges;

    explicit Snapshot(const Image& image) {
        for (const auto& segment : image.segments)
            if (segment.flags & PF_W) Save(segment.address, segment.size);
        Save(StackAddress, StackSize);
    }

    void Save(u64 address, u64 size) {
        const auto* begin = reinterpret_cast<const std::uint8_t*>(address);
        ranges.push_back({address, {begin, begin + size}});
    }

    void Restore() const {
        for (const auto& range : ranges)
            std::memcpy(
                    reinterpret_cast<void*>(range.address), range.bytes.data(), range.bytes.size());
    }
};

#if defined(SVM_BENCH_DYNARMIC)
class Engine final : public Dynarmic::A64::UserCallbacks {
public:
    Engine(const Options& options, const Image& image) : options(options), image(image) {
        u64 low = std::numeric_limits<u64>::max(), high{};
        for (const auto& segment : image.segments) {
            low = std::min(low, segment.address & ~u64{4095});
            high = std::max(high, (segment.address + segment.size + 4095) & ~u64{4095});
        }
        Map(low, high - low);
        for (const auto& segment : image.segments)
            std::memcpy(reinterpret_cast<void*>(segment.address),
                        image.bytes.data() + segment.offset,
                        segment.file_size);
        for (u64 page = low; page < high; page += 4096) {
            int protection{};
            for (const auto& segment : image.segments)
                if (segment.address < page + 4096 && segment.address + segment.size > page) {
                    if (segment.flags & PF_R) protection |= PROT_READ;
                    if (segment.flags & PF_W) protection |= PROT_WRITE;
                    // Guest code is read as data; it is never executed by the host.
                }
            if (mprotect(reinterpret_cast<void*>(page), 4096, protection))
                throw std::runtime_error("cannot protect guest page");
        }
        Map(StackAddress, StackSize);
        Dynarmic::A64::UserConfig config{};
        config.callbacks = this;
        config.fastmem_pointer = 0;
        config.fastmem_address_space_bits = 64;
        config.silently_mirror_fastmem = false;
        config.enable_cycle_counting = false;
        config.unsafe_optimizations = false;
        jit = std::make_unique<Dynarmic::A64::Jit>(config);
    }

    ~Engine() override {
        jit.reset();
        for (const auto& region : mappings) munmap(region.first, region.second);
    }

    void Reset() {
        jit->Reset();
        jit->SetPC(options.entry);
        jit->SetSP(StackAddress + StackSize - 16);
        jit->SetRegister(30, StopPC);
        stopped = false;
        error.clear();
    }

    void Run() {
        jit->Run();
        if (!error.empty()) throw std::runtime_error(error);
        // Dynarmic may advance its stored PC after the exception callback.
        // The callback verifies the actual faulting PC and exception kind.
        if (!stopped) throw std::runtime_error("Dynarmic did not return to the sentinel");
    }

    u64 Compiles() const { return code_reads; }

    u64 Fallbacks() const { return fallbacks; }

    u64 Interpreted() const { return 0; }

    u64 DataCallbacks() const { return data_callbacks; }

    const char* Name() const { return "dynarmic"; }

private:
    const Options& options;
    const Image& image;
    std::vector<std::pair<void*, std::size_t>> mappings;
    std::unique_ptr<Dynarmic::A64::Jit> jit;
    u64 code_reads{}, fallbacks{}, data_callbacks{};
    bool stopped{};
    std::string error;

    void Map(u64 address, u64 size) {
        void* pointer = mmap(reinterpret_cast<void*>(address),
                             size,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                             -1,
                             0);
        if (pointer == MAP_FAILED) throw std::runtime_error("cannot map benchmark memory");
        mappings.emplace_back(pointer, size);
    }

    void Fail(const char* message) {
        error = message;
        jit->HaltExecution();
    }

    bool Accessible(u64 address, std::size_t size, unsigned flags) const {
        return image.Contains(address, size, flags) ||
               (address >= StackAddress && address - StackAddress <= StackSize &&
                size <= StackSize - (address - StackAddress));
    }

    template <class T> T Read(u64 address) {
        ++data_callbacks;
        T value{};
        if (!Accessible(address, sizeof(T), PF_R))
            Fail("invalid guest read");
        else
            std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
        return value;
    }

    template <class T> void Write(u64 address, T value) {
        ++data_callbacks;
        if (!Accessible(address, sizeof(T), PF_W))
            Fail("invalid guest write");
        else
            std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(T));
    }

    std::optional<std::uint32_t> MemoryReadCode(u64 address) override {
        ++code_reads;
        if (!image.Contains(address, 4, PF_R | PF_X)) return std::nullopt;
        std::uint32_t instruction;
        std::memcpy(&instruction, reinterpret_cast<const void*>(address), 4);
        return instruction;
    }

    std::uint8_t MemoryRead8(u64 address) override { return Read<std::uint8_t>(address); }

    std::uint16_t MemoryRead16(u64 address) override { return Read<std::uint16_t>(address); }

    std::uint32_t MemoryRead32(u64 address) override { return Read<std::uint32_t>(address); }

    u64 MemoryRead64(u64 address) override { return Read<u64>(address); }

    Dynarmic::A64::Vector MemoryRead128(u64 address) override {
        return Read<Dynarmic::A64::Vector>(address);
    }

    void MemoryWrite8(u64 address, std::uint8_t value) override { Write(address, value); }

    void MemoryWrite16(u64 address, std::uint16_t value) override { Write(address, value); }

    void MemoryWrite32(u64 address, std::uint32_t value) override { Write(address, value); }

    void MemoryWrite64(u64 address, u64 value) override { Write(address, value); }

    void MemoryWrite128(u64 address, Dynarmic::A64::Vector value) override {
        Write(address, value);
    }

    void InterpreterFallback(u64, std::size_t) override {
        ++fallbacks;
        Fail("interpreter fallback is forbidden");
    }

    void CallSVC(std::uint32_t) override { Fail("syscall in callable kernel"); }

    void ExceptionRaised(u64 pc, Dynarmic::A64::Exception exception) override {
        if (pc == StopPC && exception == Dynarmic::A64::Exception::NoExecuteFault) {
            stopped = true;
            jit->HaltExecution();
        } else
            Fail("unexpected guest exception");
    }

    void AddTicks(u64) override { Fail("cycle counting unexpectedly enabled"); }

    u64 GetTicksRemaining() override {
        Fail("cycle counting unexpectedly enabled");
        return 0;
    }

    u64 GetCNTPCT() override {
        Fail("timer access in benchmark kernel");
        return 0;
    }
};
#else
class Engine {
public:
    Engine(const Options& options, const Image&) : options(options), host(false) {
        swift::runtime::execution::RuntimeOptions policy;
        policy.scope = options.scope == "block" ? swift::runtime::ir::Scope::Block
                                                : swift::runtime::ir::Scope::Function;
        policy.pinned_slots = options.pins;
        vm = std::make_unique<swift::runtime::execution::VM>(policy, stats, host.Bind());
        vm->PrepareGuest(options.elf);
        const auto address = vm->Memory().Map(StackAddress,
                                              StackSize,
                                              PROT_READ | PROT_WRITE,
                                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE);
        if (address != StackAddress) throw std::runtime_error("cannot map benchmark stack");
        initial = vm->ArmState();
    }

    void Reset() {
        vm->ArmState() = initial;
        vm->ArmState().gprs[30] = StopPC;
    }

    void Run() { vm->CallGuest(options.entry, StackAddress + StackSize - 16, StopPC); }

    u64 Compiles() const { return stats.compile_attempts; }

    u64 CompileNS() const { return stats.compile_request_ns; }
    const swift::runtime::execution::RuntimeStats& Statistics() const { return stats; }

    unsigned PinSlots() const { return stats.pinned_uniform_slots; }

    u64 Fallbacks() const { return stats.fallbacks; }

    u64 Interpreted() const { return stats.interpreted_steps; }

    u64 DataCallbacks() const { return 0; }

    const char* Name() const { return "swiftvm"; }

    ~Engine() {
        if (vm && !vm->Close().Complete()) std::terminate();
    }

private:
    const Options& options;
    swift::runtime::execution::RuntimeStats stats;
    swift::runtime::platform::HostRuntime host;
    std::unique_ptr<swift::runtime::execution::VM> vm;
    swift::runtime::arm64::GuestState initial;
};
#endif
}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = Parse(argc, argv);
        const Image image{options.elf};
        if (!image.Contains(options.entry, 4, PF_R | PF_X) ||
            !image.Contains(options.output, options.output_size, PF_R | PF_W))
            throw std::runtime_error("kernel entry/output outside expected ELF segments");
        const auto begin = Clock::now();
        Engine engine{options, image};
        const Snapshot snapshot{image};
        const auto initialization = Nanoseconds(begin);
        std::vector<u64> times;
        u64 cold_compiles{};
        for (unsigned run = 0; run <= options.repeats; ++run) {
            snapshot.Restore();
            engine.Reset();
            const auto start = Clock::now();
            engine.Run();
            times.push_back(Nanoseconds(start));
            if (std::memcmp(reinterpret_cast<const void*>(options.output),
                            options.expected.data(),
                            options.output_size))
                throw std::runtime_error("kernel output disagrees with independent reference");
            if (engine.Fallbacks() || engine.Interpreted() || engine.DataCallbacks())
                throw std::runtime_error("expected native execution and direct memory access");
            if (!run)
                cold_compiles = engine.Compiles();
            else if (engine.Compiles() != cold_compiles)
                throw std::runtime_error("warm execution compiled additional guest code");
        }
        std::printf(
                "{\"engine\":\"%s\",\"scope\":\"%s\",\"pin_slots\":%u,"
                "\"initialization_ns\":%llu,\"cold_run_ns\":%llu,\"warm_run_ns\":[",
                engine.Name(),
                options.scope.c_str(),
                options.pins,
                static_cast<unsigned long long>(initialization),
                static_cast<unsigned long long>(times.front()));
        for (std::size_t i = 1; i < times.size(); ++i)
            std::printf("%s%llu", i == 1 ? "" : ",", static_cast<unsigned long long>(times[i]));
        std::printf("],\"compile_observations\":%llu,",
                    static_cast<unsigned long long>(cold_compiles));
#if !defined(SVM_BENCH_DYNARMIC)
        std::printf("\"effective_pin_slots\":%u,", engine.PinSlots());
        std::printf("\"swiftvm_compile_ns\":%llu,",
                    static_cast<unsigned long long>(engine.CompileNS()));
        const auto& stats = engine.Statistics();
        std::printf("\"frontend_ns\":%llu,\"optimize_ns\":%llu,\"backend_ns\":%llu,"
                    "\"publication_ns\":%llu,\"native_bytes\":%llu,\"fp_domains_created\":%llu,"
                    "\"fp_domain_switches\":%llu,\"fp_domain_switch_ns\":%llu,",
                    static_cast<unsigned long long>(stats.frontend_ns),
                    static_cast<unsigned long long>(stats.optimize_ns),
                    static_cast<unsigned long long>(stats.backend_ns),
                    static_cast<unsigned long long>(stats.publication_ns),
                    static_cast<unsigned long long>(stats.native_bytes),
                    static_cast<unsigned long long>(stats.fp_domains_created),
                    static_cast<unsigned long long>(stats.fp_domain_switches),
                    static_cast<unsigned long long>(stats.fp_domain_switch_ns));
#endif
        std::printf(
                "\"fallbacks\":0,\"interpreted_steps\":0,\"data_callbacks\":0,"
                "\"output_verified\":true,\"warm_cache_verified\":true}\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "benchmark failed: %s\n", error.what());
        return 1;
    }
}
