// Supplemental diagnostics for analyze_dynarmic_gap.py. Not linked into normal runners.
#pragma once

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <sstream>
#include <utility>

#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#if !defined(SVM_BENCH_DYNARMIC)
#include "runtime/ir/format.h"
#endif

namespace gap {
inline std::filesystem::path Output() {
    const char* path = std::getenv("SVM_GAP_OUT");
    if (!path) throw std::runtime_error("SVM_GAP_OUT is required");
    std::filesystem::create_directories(path);
    return path;
}

class Counters {
public:
    Counters() {
        constexpr std::array<std::uint64_t, 4> events{PERF_COUNT_HW_CPU_CYCLES,
                                                   PERF_COUNT_HW_INSTRUCTIONS,
                                                   PERF_COUNT_HW_BRANCH_INSTRUCTIONS,
                                                   PERF_COUNT_HW_BRANCH_MISSES};
        for (unsigned i = 0; i < events.size(); ++i) {
            perf_event_attr attr{};
            attr.size = sizeof(attr);
            attr.type = PERF_TYPE_HARDWARE;
            attr.config = events[i];
            attr.disabled = i == 0;
            attr.pinned = i == 0;
            attr.exclude_kernel = attr.exclude_hv = 1;
            attr.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED |
                               PERF_FORMAT_TOTAL_TIME_RUNNING;
            descriptors[i] = syscall(SYS_perf_event_open, &attr, 0, -1,
                                     i ? descriptors[0] : -1, PERF_FLAG_FD_CLOEXEC);
            if (descriptors[i] < 0) throw std::runtime_error("cannot open PMU group: " + std::string(std::strerror(errno)));
        }
    }

    ~Counters() { for (int fd : descriptors) if (fd >= 0) close(fd); }

    void Start() {
        if (ioctl(descriptors[0], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) ||
            ioctl(descriptors[0], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP))
            throw std::runtime_error("cannot start PMU group");
    }

    void Stop() {
        if (ioctl(descriptors[0], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP))
            throw std::runtime_error("cannot stop PMU group");
        Record record{};
        if (read(descriptors[0], &record, sizeof(record)) != sizeof(record) || record.number != 4 ||
            !record.running || record.enabled != record.running)
            throw std::runtime_error("PMU unavailable or multiplexed");
        records.push_back(record);
    }

    void Write() const {
        std::ofstream out(Output() / "pmu.json");
        out << "[";
        for (unsigned i = 0; i < records.size(); ++i) {
            const auto& r = records[i];
            out << (i ? "," : "") << "{\"cycles\":" << r.values[0]
                << ",\"instructions\":" << r.values[1] << ",\"branches\":" << r.values[2]
                << ",\"branch_misses\":" << r.values[3] << ",\"enabled_ns\":" << r.enabled
                << ",\"running_ns\":" << r.running << "}";
        }
        out << "]\n";
    }

private:
    struct Record { std::uint64_t number{}, enabled{}, running{}; std::array<std::uint64_t, 4> values{}; };
    std::array<int, 4> descriptors{-1,-1,-1,-1};
    std::vector<Record> records;
};

class Sampler {
public:
    Sampler() {
        if (!std::getenv("SVM_GAP_SAMPLE")) return;
        perf_event_attr attr{};
        attr.size = sizeof(attr);
        attr.type = PERF_TYPE_SOFTWARE;
        attr.config = PERF_COUNT_SW_CPU_CLOCK;
        attr.sample_period = 250000;
        attr.sample_type = PERF_SAMPLE_IP;
        attr.disabled = 1;
        attr.exclude_kernel = attr.exclude_hv = 1;
        fd = syscall(SYS_perf_event_open, &attr, 0, -1, -1, PERF_FLAG_FD_CLOEXEC);
        if (fd < 0) throw std::runtime_error("cannot open IP sampler");
        page = sysconf(_SC_PAGESIZE);
        bytes = page * 65;
        mapping = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (mapping == MAP_FAILED) throw std::runtime_error("cannot map IP sampler");
    }

    ~Sampler() { if (mapping != MAP_FAILED) munmap(mapping, bytes); if (fd >= 0) close(fd); }

    void Start(unsigned run) {
        if (fd >= 0 && run && ioctl(fd, PERF_EVENT_IOC_ENABLE, 0))
            throw std::runtime_error("cannot start IP sampler");
    }

    void Stop(unsigned run) {
        if (fd < 0 || !run) return;
        if (ioctl(fd, PERF_EVENT_IOC_DISABLE, 0)) throw std::runtime_error("cannot stop IP sampler");
        auto* meta = static_cast<perf_event_mmap_page*>(mapping);
        const auto head = __atomic_load_n(&meta->data_head, __ATOMIC_ACQUIRE);
        const auto size = meta->data_size;
        const auto* data = static_cast<const std::uint8_t*>(mapping) + meta->data_offset;
        auto tail = meta->data_tail;
        if (head - tail > size) throw std::runtime_error("IP sampling ring overrun");
        const auto copy = [&](void* output, std::size_t length, std::uint64_t offset) {
            auto* target = static_cast<std::uint8_t*>(output);
            for (std::size_t i = 0; i < length; ++i) target[i] = data[(offset + i) % size];
        };
        while (tail < head) {
            perf_event_header header{};
            copy(&header, sizeof(header), tail);
            if (header.size < sizeof(header) || header.size > head - tail)
                throw std::runtime_error("invalid IP sample record");
            if (header.type == PERF_RECORD_SAMPLE) {
                std::uint64_t ip{};
                copy(&ip, sizeof(ip), tail + sizeof(header));
                ++samples[ip];
            } else if (header.type == PERF_RECORD_LOST) {
                std::uint64_t count{};
                copy(&count, sizeof(count), tail + sizeof(header) + 8);
                lost += count;
            }
            tail += header.size;
        }
        __atomic_store_n(&meta->data_tail, tail, __ATOMIC_RELEASE);
    }

    void Write() const {
        if (fd < 0) return;
        std::ofstream out(Output() / "samples.tsv");
        for (const auto& [ip,count] : samples) out << std::hex << ip << std::dec << '\t' << count << '\n';
        std::ofstream(Output() / "sampling.json") << "{\"period_ns\":250000,\"lost\":" << lost << "}\n";
    }

private:
    int fd{-1};
    void* mapping{MAP_FAILED};
    std::size_t page{}, bytes{};
    std::map<std::uint64_t,std::uint64_t> samples;
    std::uint64_t lost{};
};

inline Counters counters;
inline Sampler sampler;
inline void Start(unsigned run) { sampler.Start(run); counters.Start(); }
inline void Stop(unsigned run) { counters.Stop(); sampler.Stop(run); }

inline void WriteDisassembly(const std::vector<std::string>& lines) {
    std::ofstream out(Output() / "dynarmic.asm");
    for (const auto& line : lines) out << line << '\n';
}

#if !defined(SVM_BENCH_DYNARMIC)
namespace exe = swift::runtime::execution;
namespace ir = swift::runtime::ir;
namespace backend = swift::runtime::backend;
inline decltype(exe::HostBackendOps::compile) original_compile;
inline decltype(exe::HostBackendOps::synchronize_code) original_sync;
inline unsigned unit{};
inline std::uint64_t guest_pc{};
struct Published { const std::uint8_t* address; std::size_t size; unsigned index; };
inline std::vector<Published> published;

inline backend::CompileResult& Observe(void* opaque, const ir::Program& program,
                                      exe::HostCompileRequest request) {
    auto& result = original_compile(opaque, program, request);
    guest_pc = request.entry_pc;
    const auto prefix = Output() / std::to_string(unit);
    std::ofstream(prefix.string() + ".ir") << ir::ToString(program);
    std::ofstream ranges(prefix.string() + ".data.tsv");
    for (const auto& link : result.links) ranges << link.data_offset << '\t' << link.fallback_offset << '\n';
    std::ofstream entries(prefix.string() + ".entries.tsv");
    for (const auto& entry : result.entries) entries << entry.pc << '\t' << entry.offset << '\n';
    std::ofstream metadata(Output() / "units.tsv", std::ios::app);
    metadata << unit << '\t' << request.entry_pc << '\t' << program.Blocks().size() << '\t'
             << program.Operations().size() << '\t' << result.words.size() * 4 << '\t'
             << result.spill_slots << '\t' << result.frame_bytes << '\n';
    ++unit;
    return result;
}

inline int Synchronize(std::uint8_t* write, const std::uint8_t* execute, std::size_t size) noexcept {
    const auto result = original_sync ? original_sync(write, execute, size) : 0;
    published.push_back({execute,size,unit-1});
    std::ofstream out(Output() / "maps.tsv", std::ios::app);
    out << unit-1 << '\t' << std::hex << reinterpret_cast<std::uintptr_t>(execute) << std::dec
        << '\t' << size << '\t' << guest_pc << '\n';
    return result;
}

inline void Wrap(exe::HostBackendOps& ops) {
    original_compile = ops.compile;
    original_sync = ops.synchronize_code;
    ops.compile = Observe;
    ops.synchronize_code = Synchronize;
}

inline void WriteStats(const exe::RuntimeStats& stats) {
    std::ofstream(Output() / "runtime.json") << "{\"native_bytes\":" << stats.native_bytes
        << ",\"compile_attempts\":" << stats.compile_attempts << ",\"fallbacks\":" << stats.fallbacks
        << ",\"interpreted_steps\":" << stats.interpreted_steps << "}\n";
    for (const auto& code : published) {
        std::ofstream out(Output() / (std::to_string(code.index) + ".bin"), std::ios::binary);
        out.write(reinterpret_cast<const char*>(code.address), code.size);
    }
}
#endif

inline void Finish() {
    counters.Write();
    sampler.Write();
    std::ifstream maps("/proc/self/maps");
    std::ofstream(Output() / "process.maps") << maps.rdbuf();
}
} // namespace gap
