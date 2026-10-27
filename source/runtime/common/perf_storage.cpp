#include "perf_storage.h"

#include "perf_stats.h"

namespace swift::runtime {

bool WorkspaceStorageEnabled() {
    static const bool enabled = Perf2Enabled();
    return enabled;
}

namespace {
void RaisePeak(std::atomic<unsigned long long>& counter, unsigned long long value) {
    auto peak = counter.load(std::memory_order_relaxed);
    while (peak < value && !counter.compare_exchange_weak(peak, value, std::memory_order_relaxed)) {
    }
}

void Allocate(PerfWorkspaceStorage& counters, size_t bytes) {
    counters.allocation_calls.fetch_add(1, std::memory_order_relaxed);
    counters.allocated_bytes.fetch_add(bytes, std::memory_order_relaxed);
    const auto live = counters.live_bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    RaisePeak(counters.peak_bytes, live);
}

void Deallocate(PerfWorkspaceStorage& counters, size_t bytes) {
    counters.deallocation_calls.fetch_add(1, std::memory_order_relaxed);
    counters.released_bytes.fetch_add(bytes, std::memory_order_relaxed);
    counters.live_bytes.fetch_sub(bytes, std::memory_order_relaxed);
}

void Retain(PerfWorkspaceStorage& counters, size_t bytes) {
    counters.retention_events.fetch_add(1, std::memory_order_relaxed);
    const auto retained =
            counters.retained_bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    RaisePeak(counters.peak_retained_bytes, retained);
}

WorkspaceStorageStats Snapshot(const PerfWorkspaceStorage& counters) {
    const auto read = [](const auto& value) { return value.load(std::memory_order_relaxed); };
    return {read(counters.allocation_calls),
            read(counters.deallocation_calls),
            read(counters.allocated_bytes),
            read(counters.released_bytes),
            read(counters.live_bytes),
            read(counters.peak_bytes),
            read(counters.retention_events),
            read(counters.retained_bytes),
            read(counters.peak_retained_bytes),
            read(counters.zero_init_bytes),
            read(counters.pattern_init_bytes)};
}
}  // namespace

void RecordWorkspaceAllocation(WorkspaceBufferKind kind, size_t bytes) noexcept {
    if (!WorkspaceStorageEnabled()) return;
    auto& counters = GetPerfStats2();
    Allocate(counters.workspace_buffers[static_cast<size_t>(kind)], bytes);
    Allocate(counters.workspace_total, bytes);
}

void RecordWorkspaceDeallocation(WorkspaceBufferKind kind, size_t bytes) noexcept {
    if (!WorkspaceStorageEnabled()) return;
    auto& counters = GetPerfStats2();
    Deallocate(counters.workspace_buffers[static_cast<size_t>(kind)], bytes);
    Deallocate(counters.workspace_total, bytes);
}

void RecordWorkspaceRetain(WorkspaceBufferKind kind, size_t bytes) noexcept {
    if (!WorkspaceStorageEnabled()) return;
    auto& counters = GetPerfStats2();
    Retain(counters.workspace_buffers[static_cast<size_t>(kind)], bytes);
    Retain(counters.workspace_total, bytes);
}

void RecordWorkspaceActivate(WorkspaceBufferKind kind, size_t bytes) noexcept {
    if (!WorkspaceStorageEnabled()) return;
    auto& counters = GetPerfStats2();
    counters.workspace_buffers[static_cast<size_t>(kind)].retained_bytes.fetch_sub(
            bytes, std::memory_order_relaxed);
    counters.workspace_total.retained_bytes.fetch_sub(bytes, std::memory_order_relaxed);
}

void RecordWorkspaceInitialization(WorkspaceBufferKind kind, size_t bytes, bool zero) noexcept {
    if (!bytes || !WorkspaceStorageEnabled()) return;
    auto& counters = GetPerfStats2();
    auto& buffer = counters.workspace_buffers[static_cast<size_t>(kind)];
    (zero ? buffer.zero_init_bytes : buffer.pattern_init_bytes)
            .fetch_add(bytes, std::memory_order_relaxed);
    auto& total = counters.workspace_total;
    (zero ? total.zero_init_bytes : total.pattern_init_bytes)
            .fetch_add(bytes, std::memory_order_relaxed);
}

WorkspaceStorageSnapshot SnapshotWorkspaceStorage() {
    WorkspaceStorageSnapshot snapshot{};
    snapshot.enabled = WorkspaceStorageEnabled();
    if (!snapshot.enabled) return snapshot;
    const auto& counters = GetPerfStats2();
    snapshot.total = Snapshot(counters.workspace_total);
    for (size_t i = 0; i < snapshot.buffers.size(); ++i)
        snapshot.buffers[i] = Snapshot(counters.workspace_buffers[i]);
    return snapshot;
}

}  // namespace swift::runtime
