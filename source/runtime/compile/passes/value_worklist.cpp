#include "value_worklist.h"

#include <algorithm>
#include <functional>
#include "runtime/compile/analysis/value_bits.h"
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::compile {
using namespace ir;

namespace {
bool IsCandidate(const Inst& inst, const FeatureSet& features, ValueCleanup mode) {
    const auto op = inst.GetOp();
    if ((static_cast<u8>(mode) & static_cast<u8>(ValueCleanup::Values)) && features.int_imm_fold &&
        (IsWidthOperation(op) || IsConditionalValueOperation(op) ||
         op == OpCode::Add || op == OpCode::Sub || op == OpCode::And ||
         op == OpCode::Or || op == OpCode::Xor))
        return true;
    return (static_cast<u8>(mode) & static_cast<u8>(ValueCleanup::Widths)) &&
           (op == OpCode::ZeroExtend32 || op == OpCode::And || op == OpCode::BitExtract);
}
}  // namespace

void BeginValueCleanup(std::optional<ValueWorklist>& values,
                       RewriteContext& rewrite,
                       const FeatureSet& features,
                       const BuilderCapabilities& target,
                       ValueCleanup mode) {
    ASSERT(!values);
    const auto candidate_block = [&](const Block* block) {
        return std::any_of(block->GetInstList().begin(),
                           block->GetInstList().end(),
                           [&](const Inst& inst) { return IsCandidate(inst, features, mode); });
    };
    bool candidate = false;
    if (rewrite.IsSingleBlock()) {
        candidate = candidate_block(rewrite.Blocks().front());
    } else {
        for (const auto& block : rewrite.Function()->GetHIRBlocksRPO())
            if (candidate_block(block.GetBlock())) {
                candidate = true;
                break;
            }
    }
    if (candidate) {
        values.emplace(rewrite, features, target, mode);
        values->Run();
    } else if (static_cast<u8>(mode) & static_cast<u8>(ValueCleanup::Values)) {
        // CSE edits operand views but does not introduce any of the absent
        // arithmetic/width opcodes. Later DCE/flags cleanup cannot expose a
        // value rule when this family has no eligible instruction at all.
        ShareConstants(rewrite, features);
    }
}

ValueWorklist::ValueWorklist(RewriteContext& context,
                             const FeatureSet& options,
                             const BuilderCapabilities& capabilities,
                             ValueCleanup mode)
        : rewrite(context)
        , workspace(context.workspace)
        , borrow(workspace)
        , features(options)
        , target(capabilities)
        , requested(mode)
        , active(static_cast<u8>(mode) & static_cast<u8>(ValueCleanup::Values)
                         ? ValueCleanup::Values
                         : mode) {
    ASSERT(!rewrite.values);
    BuildRoster();
    remaining = workspace.value_rows.size() * 8 + 32;
    // Construction can fail before this point. Retire a previous DCE owner
    // table only after the complete replacement roster is available.
    rewrite.ResetDeadOwners();
    rewrite.values = this;
}

ValueWorklist::~ValueWorklist() {
    ASSERT(rewrite.values == this);
    // DCE may outlive this epoch. Its next run must seed from live IR rather
    // than retain a queue whose membership depended on this borrowed roster.
    rewrite.ResetDeadOwners();
    rewrite.values = nullptr;
}

void ValueWorklist::BuildRoster() {
    auto& rows = workspace.value_rows;
    auto& lookup = workspace.value_lookup;
    size_t count = 0;
    for (const auto* block : rewrite.Blocks()) count += block->GetInstList().size();
    ASSERT(count < reuse_task);
    rows.reserve(count);
    lookup.reserve(count);
    const auto add = [&](Block* block, bool eligible) {
        u32 position = 0;
        for (const auto& inst : block->GetInstList()) {
            lookup.push_back({&inst, static_cast<u32>(rows.size())});
            rows.push_back({&inst, block, position++, 0, eligible});
            // Fresh rows have no pending feedback or shared-constant state.
            // Initial seeding only needs eligibility, not Queue's task/heap
            // transitions for subsequent rewrites and use-count changes.
            if (eligible && Candidate(inst)) {
                workspace.value_queue.push_back(static_cast<u32>(rows.size() - 1));
                rows.back().pending = Simplify;
            }
        }
    };
    if (rewrite.IsSingleBlock()) {
        add(rewrite.Blocks().front(), true);
    } else {
        StackVector<u8, 32> reachable;
        reachable.resize(rewrite.Function()->MaxBlockCount(), 0);
        for (auto& hir : rewrite.Function()->GetHIRBlocksRPO()) {
            reachable[hir.GetOrderId()] = 1;
            add(rewrite.Function()->EditBlock(&hir).GetBlock(), true);
        }
        // Unreachable nodes may still be deleted by whole-Function DCE. Keep
        // their identities, without seeding them as optimization candidates.
        for (const auto* hir : rewrite.Function()->GetHIRBlocks())
            if (hir && !reachable[hir->GetOrderId()])
                add(rewrite.Function()->EditBlock(hir).GetBlock(), false);
    }
    // The initial queue is a stack. Match the old reverse-row seeding order,
    // so the earliest eligible instruction is still processed first.
    ASSERT(!ordered_queue);
    std::reverse(workspace.value_queue.begin(), workspace.value_queue.end());
    std::sort(lookup.begin(), lookup.end(), [](const auto& left, const auto& right) {
        return std::less<const Inst*>{}(left.inst, right.inst);
    });
}

u32 ValueWorklist::Find(const Inst* inst) const {
    const auto& lookup = workspace.value_lookup;
    const auto found = std::lower_bound(
            lookup.begin(), lookup.end(), inst, [](const auto& row, const Inst* key) {
                return std::less<const Inst*>{}(row.inst, key);
            });
    if (found == lookup.end() || found->inst != inst ||
        workspace.value_rows[found->row].inst != inst)
        return nil;
    return found->row;
}

Block* ValueWorklist::LiveOwner(const Inst* inst) const {
    const auto row = Find(inst);
    return row == nil ? nullptr : workspace.value_rows[row].block;
}

bool ValueWorklist::Nearby(const Inst* source, const Inst* user, u32 limit) const {
    const auto first = Find(source), second = Find(user);
    if (first == nil || second == nil) return false;
    const auto& left = workspace.value_rows[first];
    const auto& right = workspace.value_rows[second];
    return left.block == right.block && left.position < right.position &&
           right.position - left.position <= limit;
}

bool ValueWorklist::Candidate(const Inst& inst) const {
    return IsCandidate(inst, features, active);
}

void ValueWorklist::Queue(u32 index, u8 reason) {
    if (index == nil || !remaining) return;
    auto& row = workspace.value_rows[index];
    if (!row.inst || !row.eligible) return;
    if (row.constant_shared) reason &= ~ReuseConstant;
    // Opaque results cannot change facts understood by the bounded bit query.
    // They may still need their own local rule, but no transitive bit walk.
    if (!row.inst->HasValue() || row.inst->ReturnType() == ValueType::VOID ||
        (row.inst->GetOp() != OpCode::LoadImm && !TransfersIntegerValueBits(row.inst->GetOp())))
        reason &= Simplify;
    if (!Candidate(*row.inst) && !(reason & (Propagate | ReuseConstant))) return;
    auto& queue = workspace.value_queue;
    if ((reason & ReuseConstant) && !ordered_queue) {
        // Only an actual scalar feedback window needs order. Earlier value
        // cleanup keeps its stack discipline and pays no heap maintenance.
        std::make_heap(queue.begin(), queue.end(), std::greater<u32>{});
        ordered_queue = true;
    }
    const auto push = [&](u32 task) {
        queue.push_back(task);
        if (ordered_queue) std::push_heap(queue.begin(), queue.end(), std::greater<u32>{});
    };
    // Normalization must finish before CSE can increase a constant's uses.
    // Encode the two tasks in the existing u32 queue, deduplicating each by
    // its own pending bits. No second queue or per-row index is needed.
    if ((reason & (Simplify | Propagate)) && !(row.pending & (Simplify | Propagate))) push(index);
    if ((reason & ReuseConstant) && !(row.pending & ReuseConstant)) push(index | reuse_task);
    row.pending |= reason;
}

void ValueWorklist::SeedWidths() {
    ASSERT(active == ValueCleanup::Widths);
    for (size_t i = workspace.value_rows.size(); i-- > 0;) Queue(static_cast<u32>(i), Simplify);
}

bool ValueWorklist::Pending() const { return !workspace.value_queue.empty(); }

void ValueWorklist::Run() {
    if (static_cast<u8>(requested) & static_cast<u8>(ValueCleanup::Values)) {
        active = ValueCleanup::Values;
        Drain();
        ShareConstants(rewrite, features);
        constants_ready = true;
    }
    if (static_cast<u8>(requested) & static_cast<u8>(ValueCleanup::Widths)) {
        active = ValueCleanup::Widths;
        // Values/CSE can expose fresh width rules. Widths-only cleanup was
        // already seeded while building the roster and needs no second walk.
        if (static_cast<u8>(requested) & static_cast<u8>(ValueCleanup::Values)) SeedWidths();
    }
    active = requested;
    Drain();
}

void ValueWorklist::Drain() {
    auto& queue = workspace.value_queue;
    while (!queue.empty() && remaining) {
        if (ordered_queue) std::pop_heap(queue.begin(), queue.end(), std::greater<u32>{});
        const auto task = queue.back();
        queue.pop_back();
        const auto index = task & ~reuse_task;
        auto& row = workspace.value_rows[index];
        const u8 reason = row.pending & (task & reuse_task ? ReuseConstant : Simplify | Propagate);
        row.pending &= ~reason;
        if (!row.inst || !reason) continue;
        --remaining;
        if (reason & ReuseConstant) SimplifyConstant(rewrite, *this, row.block, row.inst, features);
        if (reason & Simplify) {
            if (static_cast<u8>(active) & static_cast<u8>(ValueCleanup::Values))
                SimplifyValue(rewrite, *this, row.block, row.inst, features, target);
            if (static_cast<u8>(active) & static_cast<u8>(ValueCleanup::Widths))
                SimplifyWidth(rewrite, *this, row.block, row.inst);
        }
        if (reason & Propagate) QueueUsers(row.inst, true);
    }
    if (!remaining) {
        // Stop optional simplification, while RewriteContext continues to
        // maintain real uses and finish all required DCE/flags work.
        for (auto task : queue) workspace.value_rows[task & ~reuse_task].pending = 0;
        queue.clear();
    }
}

void ValueWorklist::PrepareUses() {
    ASSERT(rewrite.IsSingleBlock());
    if (uses_ready) return;
    workspace.value_links.resize(workspace.value_rows.size());
    RecordWorkspaceInitialization(
            WorkspaceBufferKind::ValueLinks, workspace.value_links.size() * 2 * sizeof(u32), false);
    for (u32 i = 0; i < workspace.value_rows.size(); ++i)
        if (workspace.value_rows[i].inst) AddInputs(i);
    uses_ready = true;
}

void ValueWorklist::AddInputs(u32 user) {
    auto& links = workspace.value_links;
    auto& uses = workspace.value_uses;
    workspace.value_rows[user].inst->ForEachValue([&](Value value) {
        const auto definition = Find(value.Def());
        if (definition == nil) return;
        u32 index;
        if (free_use == nil) {
            index = static_cast<u32>(uses.size());
            uses.push_back({});
        } else {
            index = free_use;
            free_use = uses[index].next;
        }
        const auto head = links[definition].users;
        uses[index] = {definition, user, nil, head, links[user].inputs};
        if (head != nil) uses[head].previous = index;
        links[definition].users = index;
        links[user].inputs = index;
    });
}

void ValueWorklist::RemoveInputs(u32 user) {
    auto& links = workspace.value_links;
    auto& uses = workspace.value_uses;
    auto index = std::exchange(links[user].inputs, nil);
    while (index != nil) {
        const auto edge = uses[index];
        if (edge.previous == nil)
            links[edge.definition].users = edge.next;
        else
            uses[edge.previous].next = edge.next;
        if (edge.next != nil) uses[edge.next].previous = edge.previous;
        uses[index].next = free_use;
        free_use = index;
        index = edge.next_input;
    }
}

template <class Visitor> void ValueWorklist::ForEachUser(const Inst* inst, Visitor&& visit) {
    const auto row = Find(inst);
    if (row == nil || !inst->HasValue() || inst->ReturnType() == ValueType::VOID) return;
    if (rewrite.IsSingleBlock()) {
        PrepareUses();
        for (auto i = workspace.value_links[row].users; i != nil; i = workspace.value_uses[i].next)
            visit(workspace.value_rows[workspace.value_uses[i].user].inst);
        return;
    }
    if (const auto* value = rewrite.Function()->GetHIRValue(Value{inst})) {
        for (const auto& use : value->GetUses()) visit(use.inst);
        return;
    }
    // Manual/unregistered HIR values have no existing reverse list. Keep
    // this explicit fallback out of the normal registered Function path.
    for (const auto& current : workspace.value_rows) {
        if (!current.inst) continue;
        current.inst->ForEachValue([&](Value value) {
            if (value.Def() == inst) visit(current.inst);
        });
    }
}

const Inst* ValueWorklist::SingleConsumer(const Inst* inst) {
    const auto row = Find(inst);
    if (row == nil) return nullptr;
    const Inst* consumer{};
    u32 count = 0;
    ForEachUser(inst, [&](const Inst* user) {
        const auto id = Find(user);
        if (id == nil || workspace.value_rows[id].block != workspace.value_rows[row].block) return;
        ++count;
        consumer = user;
    });
    return count == 1 ? consumer : nullptr;
}

Flags ValueWorklist::RequestedFlags(const Inst* inst) {
    Flags flags{};
    ForEachUser(inst, [&](const Inst* user) {
        if (Find(user) == nil) return;
        if ((user->GetOp() == OpCode::SaveFlags || user->GetOp() == OpCode::BranchOnlyFlags) &&
            user->ArgAt(0).IsValue() && user->GetArg<Value>(0).Def() == inst)
            flags |= user->GetArg<Flags>(1);
    });
    return flags;
}

void ValueWorklist::QueueUsers(const Inst* inst, bool propagate) {
    ForEachUser(inst, [&](const Inst* user) {
        const auto row = Find(user);
        if (row != nil) {
            const bool forward = propagate && TransfersIntegerValueBits(user->GetOp());
            Queue(row, Simplify | (forward ? Propagate : 0));
        }
    });
}

void ValueWorklist::ChangedUseCount(const Inst* inst) {
    const auto row = Find(inst);
    if (row == nil || !remaining) return;
    Queue(row, Simplify);
    if (inst->GetUses(false) == 1) {
        if (const auto* user = SingleConsumer(inst)) Queue(Find(user), Simplify);
    }
}

bool ValueWorklist::SharedConstant(const Inst* inst) const {
    const auto row = Find(inst);
    return row == nil || workspace.value_rows[row].constant_shared;
}

void ValueWorklist::MarkSharedConstant(const Inst* inst) {
    const auto row = Find(inst);
    ASSERT(row != nil);
    workspace.value_rows[row].constant_shared = true;
}

void ValueWorklist::ReplaceConstantUses(Block* block, const Inst* from, const Inst* to) {
    const auto source = Find(from), target_row = Find(to);
    ASSERT(source != nil && target_row != nil);
    ASSERT(workspace.value_rows[source].block == block &&
           workspace.value_rows[target_row].block == block &&
           workspace.value_rows[target_row].position < workspace.value_rows[source].position);
    MarkSharedConstant(from);
    if (from->GetUses(false) == 0) return;
    // Rewriting removes/reinstalls every input edge of a consumer. Snapshot
    // row identities before touching either Block edges or the HIR use list.
    StackVector<u32, 8> users;
    ForEachUser(from, [&](const Inst* user) {
        const auto row = Find(user);
        if (row != nil && workspace.value_rows[row].block == block &&
            workspace.value_rows[row].position > workspace.value_rows[source].position &&
            !user->IsPseudoOperation() && !IsConstantReuseBoundary(user->GetOp()) &&
            (users.empty() || users.back() != row))
            users.push_back(row);
    });
    std::sort(users.begin(), users.end());
    users.erase(std::unique(users.begin(), users.end()), users.end());
    for (const auto row : users)
        if (const auto* user = workspace.value_rows[row].inst)
            RetargetConstantUser(rewrite, block, user, from, to);
}

void ValueWorklist::QueueFollowingConstants(const Inst* inst) {
    if (!constants_ready || !features.const_cse || !remaining) return;
    const auto row = Find(inst);
    ASSERT(row != nil);
    const auto& list = workspace.value_rows[row].block->GetInstList();
    auto cursor = std::next(list.iterator_to(*inst));
    for (u32 distance = 0; cursor != list.end() && distance < kConstantReuseWindow;
         ++distance, ++cursor) {
        if (IsConstantReuseBoundary(cursor->GetOp())) break;
        if (cursor->GetOp() == OpCode::LoadImm) Queue(Find(&*cursor), ReuseConstant);
    }
}

void ValueWorklist::BeforeChange(const Inst* inst) {
    const auto row = Find(inst);
    ASSERT(row != nil);
    if (uses_ready) RemoveInputs(row);
}

void ValueWorklist::AfterChange(const Inst* inst, const Inst::Values& previous) {
    const auto row = Find(inst);
    ASSERT(row != nil);
    if (uses_ready) AddInputs(row);
    workspace.value_rows[row].constant_shared = false;
    const bool constant = constants_ready && features.const_cse && inst->GetOp() == OpCode::LoadImm;
    Queue(row, Simplify | Propagate | (constant ? ReuseConstant : 0));
    if (constant) QueueFollowingConstants(inst);
    for (const auto value : previous) ChangedUseCount(value.Def());
    inst->ForEachValue([&](Value value) { ChangedUseCount(value.Def()); });
}

void ValueWorklist::BeforeErase(const Inst* inst) {
    const auto row = Find(inst);
    ASSERT(row != nil);
    QueueFollowingConstants(inst);
    if (uses_ready) {
        RemoveInputs(row);
        ASSERT(workspace.value_links[row].users == nil);
    }
    workspace.value_rows[row].inst = nullptr;
    workspace.value_rows[row].pending = 0;
}

void ValueWorklist::AfterErase(const Inst::Values& inputs) {
    for (const auto value : inputs) ChangedUseCount(value.Def());
}

}  // namespace swift::runtime::compile
