#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace swift::runtime {

enum class WorkspaceBufferKind : unsigned char {
    SpillCursor,
    DeletedInstructions,
    InstructionRows,
    LoweringCandidates,
    ValueRows,
    ValueLookup,
    ValueQueue,
    ValueLinks,
    ValueUses,
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(WorkspaceBufferKind::Count)>
        kWorkspaceBufferNames{"spill_cursor",
                              "deleted",
                              "instruction_rows",
                              "lowering_candidates",
                              "value_rows",
                              "value_lookup",
                              "value_queue",
                              "value_links",
                              "value_uses"};

// Requested container bytes, including overlapping old/new buffers during
// growth. These are neither allocator usable sizes nor process RSS. Read the
// snapshot with compilation quiescent to compare all fields consistently.
struct WorkspaceStorageStats {
    uint64_t allocation_calls{}, deallocation_calls{};
    uint64_t allocated_bytes{}, released_bytes{};
    uint64_t live_bytes{}, peak_bytes{};
    uint64_t retention_events{}, retained_bytes{}, peak_retained_bytes{};
    // Explicit bulk fills only: spill cursors, instruction rows and value
    // links. Appended payloads, relocation copies and hardware traffic are
    // outside this logical byte count.
    uint64_t zero_init_bytes{}, pattern_init_bytes{};
};

struct WorkspaceStorageSnapshot {
    bool enabled{};
    WorkspaceStorageStats total;
    std::array<WorkspaceStorageStats, kWorkspaceBufferNames.size()> buffers;
};

// SVM_PROF2 is captured on first storage use, including a snapshot. Freezing
// the setting keeps allocation/free accounting paired across test config
// reloads and thread handoff; enable it before the process starts compiling.
[[nodiscard]] WorkspaceStorageSnapshot SnapshotWorkspaceStorage();
[[nodiscard]] bool WorkspaceStorageEnabled();
void RecordWorkspaceAllocation(WorkspaceBufferKind, size_t bytes) noexcept;
void RecordWorkspaceDeallocation(WorkspaceBufferKind, size_t bytes) noexcept;
void RecordWorkspaceRetain(WorkspaceBufferKind, size_t bytes) noexcept;
void RecordWorkspaceActivate(WorkspaceBufferKind, size_t bytes) noexcept;
void RecordWorkspaceInitialization(WorkspaceBufferKind, size_t bytes, bool zero) noexcept;

}  // namespace swift::runtime
