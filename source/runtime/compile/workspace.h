#pragma once

#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

#include "runtime/common/perf_storage.h"
#include "runtime/ir/instr.h"

namespace swift::runtime::compile {

// Temporary block positions and caller-specific role bits. Each selection
// owns its exclusive borrow; role meanings never cross that scope.
struct LoweringCandidate {
    u16 position, roles;
};
static_assert(sizeof(LoweringCandidate) == 4);

namespace detail {
// Stateless: allocation counters need no per-buffer header or allocator
// state. Only actual allocator requests are measured, never operand visits.
template <class T, WorkspaceBufferKind Kind> struct WorkspaceAllocator {
    using value_type = T;
    using is_always_equal = std::true_type;
    template <class U> struct rebind {
        using other = WorkspaceAllocator<U, Kind>;
    };
    WorkspaceAllocator() = default;
    template <class U> WorkspaceAllocator(const WorkspaceAllocator<U, Kind>&) noexcept {}
    [[nodiscard]] T* allocate(size_t count) {
        auto* result = std::allocator<T>{}.allocate(count);
        RecordWorkspaceAllocation(Kind, count * sizeof(T));
        return result;
    }
    void deallocate(T* buffer, size_t count) noexcept {
        RecordWorkspaceDeallocation(Kind, count * sizeof(T));
        std::allocator<T>{}.deallocate(buffer, count);
    }
    template <class U> bool operator==(const WorkspaceAllocator<U, Kind>&) const noexcept {
        return true;
    }
};
}  // namespace detail

// Temporary optimization and allocation storage. Prepared results may not
// retain references after the active preparation scope returns.
class CompileWorkspace final {
public:
    CompileWorkspace() = default;
    ~CompileWorkspace();
    CompileWorkspace(const CompileWorkspace&) = delete;
    CompileWorkspace& operator=(const CompileWorkspace&) = delete;
    void BeginAttempt() {
        ASSERT(allocation_active);
        spill_cursor.clear();
    }
    std::span<u32> PrepareSpillCursor(u32 count);
    [[nodiscard]] std::span<u32> SpillCursor() { return spill_cursor; }

private:
    friend class AllocationWriter;
    friend class CompileWorkspaceScope;
    friend class InstructionRowScope;
    friend class RewriteQueueScope;
    friend class LoweringCandidateScope;
    friend class ValueQueueScope;
    friend class ValueWorklist;
    void Reset();
    void AcquireAllocation();
    void ReleaseAllocation();
    template <class Visitor> void VisitBuffers(Visitor&&);
    void ActivateBuffers(u16 mask);
    void RetainBuffers(u16 mask);
    // The writer owns this borrow across physical retries, then releases it.
    void ReleaseSpillCursor();
    struct ValueRow {
        const ir::Inst* inst;
        ir::Block* block;
        u32 position;
        u8 pending{};
        bool eligible{};
        bool constant_shared{};
    };
    struct ValueLookup {
        const ir::Inst* inst;
        u32 row;
    };
    struct ValueLinks {
        u32 inputs{UINT32_MAX}, users{UINT32_MAX};
    };
    struct ValueUse {
        u32 definition, user, previous, next, next_input;
    };
    template <class T, WorkspaceBufferKind Kind> using Buffer =
            std::vector<T, detail::WorkspaceAllocator<T, Kind>>;
    Buffer<u32, WorkspaceBufferKind::SpillCursor> spill_cursor;
    Buffer<const ir::Inst*, WorkspaceBufferKind::DeletedInstructions> deleted;
    Buffer<u32, WorkspaceBufferKind::InstructionRows> instruction_rows;
    Buffer<LoweringCandidate, WorkspaceBufferKind::LoweringCandidates> lowering_candidates;
    Buffer<ValueRow, WorkspaceBufferKind::ValueRows> value_rows;
    Buffer<ValueLookup, WorkspaceBufferKind::ValueLookup> value_lookup;
    Buffer<u32, WorkspaceBufferKind::ValueQueue> value_queue;
    Buffer<ValueLinks, WorkspaceBufferKind::ValueLinks> value_links;
    Buffer<ValueUse, WorkspaceBufferKind::ValueUses> value_uses;
    bool instruction_rows_active{};
    bool rewrite_queue_active{};
    bool lowering_candidates_active{};
    bool value_queue_active{};
    bool active{};
    bool allocation_active{};
    // Only profiling uses these bits. They fit the existing tail padding;
    // one bit pairs each inactive buffer with reborrow/destruction.
    u16 retained_buffers{};
};

// Mutable value cleanup borrows its roster and queue through final DCE/flags
// feedback. Block-local edges are prepared only when an actual query needs
// them; whole Functions continue to use their existing HIR use lists.
class ValueQueueScope final {
public:
    explicit ValueQueueScope(CompileWorkspace&);
    ~ValueQueueScope();
    ValueQueueScope(const ValueQueueScope&) = delete;
    ValueQueueScope& operator=(const ValueQueueScope&) = delete;

private:
    CompileWorkspace& workspace;
};

// A Function rewrite borrows its deletion queue until the context ends.
// Queued IR pointers are discarded before another phase reuses the workspace.
class RewriteQueueScope final {
public:
    explicit RewriteQueueScope(CompileWorkspace&);
    ~RewriteQueueScope();
    RewriteQueueScope(const RewriteQueueScope&) = delete;
    RewriteQueueScope& operator=(const RewriteQueueScope&) = delete;
    [[nodiscard]] auto& Entries() { return workspace.deleted; }

private:
    CompileWorkspace& workspace;
};

// Consumer, vector, predicate and PF selection share this buffer in separate scopes.
// The borrow rejects overlap and clears all role bits on every exit.
class LoweringCandidateScope final {
public:
    explicit LoweringCandidateScope(CompileWorkspace&);
    ~LoweringCandidateScope();
    LoweringCandidateScope(const LoweringCandidateScope&) = delete;
    LoweringCandidateScope& operator=(const LoweringCandidateScope&) = delete;
    [[nodiscard]] auto& Entries() { return workspace.lowering_candidates; }
    [[nodiscard]] std::span<const LoweringCandidate> Entries() const {
        return workspace.lowering_candidates;
    }

private:
    CompileWorkspace& workspace;
};

// One sequential block-local selection borrows the row index. Storage is
// initialized only on its first insertion and never escapes in the result.
class InstructionRowScope final {
public:
    InstructionRowScope(CompileWorkspace&, size_t instruction_count);
    ~InstructionRowScope();
    InstructionRowScope(const InstructionRowScope&) = delete;
    InstructionRowScope& operator=(const InstructionRowScope&) = delete;
    [[nodiscard]] u32 Find(size_t position) const;
    u32& operator[](size_t position);

private:
    CompileWorkspace& workspace;
    size_t instruction_count;
};

// The outer preparation reuses thread storage; nested preparation owns a
// separate workspace. No work is allocated for nesting unless it occurs.
class CompileWorkspaceScope final {
public:
    CompileWorkspaceScope();
    ~CompileWorkspaceScope();
    CompileWorkspaceScope(const CompileWorkspaceScope&) = delete;
    CompileWorkspaceScope& operator=(const CompileWorkspaceScope&) = delete;
    [[nodiscard]] CompileWorkspace& Get() { return *workspace; }

private:
    std::optional<CompileWorkspace> nested;
    CompileWorkspace* workspace{};
};

}  // namespace swift::runtime::compile
