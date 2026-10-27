#include "workspace.h"

namespace swift::runtime::compile {

namespace {
using Kind = WorkspaceBufferKind;
constexpr u16 Bit(Kind kind) { return u16{1} << static_cast<unsigned>(kind); }
static_assert(static_cast<unsigned>(Kind::Count) < 16);
constexpr u16 kAllStorage = Bit(Kind::Count) - 1;
constexpr u16 kValueStorage = Bit(Kind::ValueRows) | Bit(Kind::ValueLookup) |
                              Bit(Kind::ValueQueue) | Bit(Kind::ValueLinks) | Bit(Kind::ValueUses);

// Keep ordinary units reusable, while bounding retained capacity after an
// exceptional unit even when the workspace is owned directly by the caller.
template <class Buffer> void Release(Buffer& buffer) {
    buffer.clear();
    constexpr size_t kMaxRetainedEntries = 64 * 1024;
    if (buffer.capacity() > kMaxRetainedEntries) Buffer{}.swap(buffer);
}
}  // namespace

template <class Visitor> void CompileWorkspace::VisitBuffers(Visitor&& visit) {
    visit(Kind::SpillCursor, spill_cursor);
    visit(Kind::DeletedInstructions, deleted);
    visit(Kind::InstructionRows, instruction_rows);
    visit(Kind::LoweringCandidates, lowering_candidates);
    visit(Kind::ValueRows, value_rows);
    visit(Kind::ValueLookup, value_lookup);
    visit(Kind::ValueQueue, value_queue);
    visit(Kind::ValueLinks, value_links);
    visit(Kind::ValueUses, value_uses);
}

void CompileWorkspace::ActivateBuffers(u16 mask) {
    const u16 selected = retained_buffers & mask;
    if (!selected) return;
    VisitBuffers([&](Kind kind, const auto& buffer) {
        if (selected & Bit(kind))
            RecordWorkspaceActivate(
                    kind,
                    buffer.capacity() *
                            sizeof(typename std::remove_cvref_t<decltype(buffer)>::value_type));
    });
    retained_buffers &= ~selected;
}

void CompileWorkspace::RetainBuffers(u16 mask) {
    if (!WorkspaceStorageEnabled()) return;
    const u16 selected = mask & ~retained_buffers;
    if (!selected) return;
    VisitBuffers([&](Kind kind, const auto& buffer) {
        if (!(selected & Bit(kind)) || buffer.capacity() == 0) return;
        RecordWorkspaceRetain(
                kind,
                buffer.capacity() *
                        sizeof(typename std::remove_cvref_t<decltype(buffer)>::value_type));
        retained_buffers |= Bit(kind);
    });
}

CompileWorkspace::~CompileWorkspace() {
    // Retained bytes stop belonging to the cache before the member vectors
    // free their allocations, including owner destruction on another thread.
    if (retained_buffers) ActivateBuffers(kAllStorage);
}

void CompileWorkspace::AcquireAllocation() {
    ASSERT(!allocation_active);
    ActivateBuffers(Bit(Kind::SpillCursor));
    allocation_active = true;
}

void CompileWorkspace::ReleaseAllocation() {
    ASSERT(allocation_active);
    ReleaseSpillCursor();
    allocation_active = false;
}

std::span<u32> CompileWorkspace::PrepareSpillCursor(u32 count) {
    ActivateBuffers(Bit(Kind::SpillCursor));
    spill_cursor.assign(count, 0);
    RecordWorkspaceInitialization(Kind::SpillCursor, count * sizeof(u32), true);
    return spill_cursor;
}

void CompileWorkspace::ReleaseSpillCursor() {
    Release(spill_cursor);
    RetainBuffers(Bit(Kind::SpillCursor));
}

void CompileWorkspace::Reset() {
    ASSERT(!instruction_rows_active && !rewrite_queue_active && !lowering_candidates_active &&
           !value_queue_active && !allocation_active);
    ReleaseSpillCursor();
    Release(deleted);
    Release(instruction_rows);
    Release(lowering_candidates);
    Release(value_rows);
    Release(value_lookup);
    Release(value_queue);
    Release(value_links);
    Release(value_uses);
    RetainBuffers(kAllStorage);
}

ValueQueueScope::ValueQueueScope(CompileWorkspace& storage) : workspace(storage) {
    ASSERT(!workspace.value_queue_active);
    workspace.ActivateBuffers(kValueStorage);
    workspace.value_queue_active = true;
    workspace.value_rows.clear();
    workspace.value_lookup.clear();
    workspace.value_queue.clear();
    workspace.value_links.clear();
    workspace.value_uses.clear();
}

ValueQueueScope::~ValueQueueScope() {
    Release(workspace.value_rows);
    Release(workspace.value_lookup);
    Release(workspace.value_queue);
    Release(workspace.value_links);
    Release(workspace.value_uses);
    workspace.RetainBuffers(kValueStorage);
    workspace.value_queue_active = false;
}

RewriteQueueScope::RewriteQueueScope(CompileWorkspace& storage) : workspace(storage) {
    ASSERT(!workspace.rewrite_queue_active);
    workspace.ActivateBuffers(Bit(Kind::DeletedInstructions));
    workspace.rewrite_queue_active = true;
    workspace.deleted.clear();
}

RewriteQueueScope::~RewriteQueueScope() {
    Release(workspace.deleted);
    workspace.RetainBuffers(Bit(Kind::DeletedInstructions));
    workspace.rewrite_queue_active = false;
}

LoweringCandidateScope::LoweringCandidateScope(CompileWorkspace& storage) : workspace(storage) {
    ASSERT(!workspace.lowering_candidates_active);
    workspace.ActivateBuffers(Bit(Kind::LoweringCandidates));
    workspace.lowering_candidates_active = true;
    workspace.lowering_candidates.clear();
}

LoweringCandidateScope::~LoweringCandidateScope() {
    Release(workspace.lowering_candidates);
    workspace.RetainBuffers(Bit(Kind::LoweringCandidates));
    workspace.lowering_candidates_active = false;
}

InstructionRowScope::InstructionRowScope(CompileWorkspace& storage, size_t count)
        : workspace(storage), instruction_count(count) {
    ASSERT(!workspace.instruction_rows_active);
    workspace.ActivateBuffers(Bit(Kind::InstructionRows));
    workspace.instruction_rows_active = true;
    workspace.instruction_rows.clear();
}

InstructionRowScope::~InstructionRowScope() {
    Release(workspace.instruction_rows);
    workspace.RetainBuffers(Bit(Kind::InstructionRows));
    workspace.instruction_rows_active = false;
}

u32 InstructionRowScope::Find(size_t position) const {
    ASSERT(position < instruction_count);
    return workspace.instruction_rows.empty() ? UINT32_MAX : workspace.instruction_rows[position];
}

u32& InstructionRowScope::operator[](size_t position) {
    ASSERT(position < instruction_count);
    if (workspace.instruction_rows.empty()) {
        workspace.instruction_rows.assign(instruction_count, UINT32_MAX);
        RecordWorkspaceInitialization(
                Kind::InstructionRows, instruction_count * sizeof(u32), false);
    }
    return workspace.instruction_rows[position];
}

CompileWorkspaceScope::CompileWorkspaceScope() {
    static thread_local CompileWorkspace reusable;
    if (reusable.active) {
        workspace = &nested.emplace();
    } else {
        workspace = &reusable;
    }
    workspace->active = true;
}

CompileWorkspaceScope::~CompileWorkspaceScope() {
    workspace->Reset();
    workspace->active = false;
}

}  // namespace swift::runtime::compile
