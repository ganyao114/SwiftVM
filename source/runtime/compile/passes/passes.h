#pragma once

#include <span>
#include <unordered_map>
#include <utility>

#include "runtime/common/svm_config.h"
#include "runtime/compile/rules/value_rules.h"
#include "runtime/compile/workspace.h"
#include "runtime/ir/block.h"

namespace swift::runtime::ir { class HIRFunction; }

namespace swift::runtime::compile {

struct GuestStateLayout;
class ValueWorklist;

// Mutable optimization scope. IR pointers and HIR uses stay valid through all
// notified edits; allocation facts are built only after this scope ends.
class RewriteContext final {
public:
    explicit RewriteContext(ir::Block* block,
                            CompileWorkspace& workspace,
                            ir::HIRFunction* owner = nullptr,
                            bool eliminate_dead = true);
    explicit RewriteContext(ir::HIRFunction* function,
                            CompileWorkspace& workspace,
                            bool eliminate_dead = true);
    RewriteContext(const RewriteContext&) = delete;
    RewriteContext& operator=(const RewriteContext&) = delete;

    template <typename Edit> void Change(ir::Block* block, const ir::Inst* inst, Edit&& edit) {
        CheckBlock(block);
        auto& writer = block->EditInst(inst);
        const auto inputs = inst->GetValues();
        const bool flags = FlagRelevant(*inst);
        BeforeChange(inst);
        edit(writer);
        AfterChange(block, inst, inputs, flags);
    }
    template <typename Arg>
        requires requires(ir::Inst& target, Arg&& arg) {
            target.SetArg(0, std::forward<Arg>(arg));
        }
    void SetArg(ir::Block* block, const ir::Inst* inst, u32 slot, Arg&& arg) {
        Change(block, inst, [&](ir::Inst& target) {
            target.SetArg(slot, std::forward<Arg>(arg));
        });
    }
    void Erase(ir::Block* block, const ir::Inst* inst);
    bool EraseDead(ir::Block* block, const ir::Inst* inst);
    void CollectDead();
    [[nodiscard]] bool HasFlagChanges() const { return !flag_changes.empty(); }
    void ClearFlagChanges() { flag_changes.clear(); }
    [[nodiscard]] std::span<ir::Block* const> Blocks() const { return blocks; }
    [[nodiscard]] ir::HIRFunction* Function() const { return function; }
    [[nodiscard]] bool IsSingleBlock() const { return single_block; }
    [[nodiscard]] bool EliminatesDead() const { return eliminate_dead; }
    bool BeginFlags() { return !std::exchange(flags_prepared, true); }

private:
    friend class ValueWorklist;
    friend void ShareConstants(RewriteContext&, const FeatureSet&);
    void CheckBlock(const ir::Block* block) const;
    static bool FlagRelevant(const ir::Inst& inst);
    bool Removable(const ir::Inst& inst) const;
    void BeforeChange(const ir::Inst* inst);
    void AfterChange(ir::Block* block,
                     const ir::Inst* inst,
                     const ir::Inst::Values& inputs,
                     bool flags);
    void NoteFlags(ir::Block* block);
    void QueueDead(const ir::Inst* inst);
    void RefreshRoots();
    [[nodiscard]] ir::Block* LiveOwner(const ir::Inst* inst) const;
    void ResetDeadOwners();

    ir::HIRFunction* function{};
    StackVector<ir::Block*, 4> blocks;
    // Only whole-Function DCE needs owners and a cross-block queue. An isolated
    // Block uses its legal reverse order, without constructing another graph.
    std::unordered_map<const ir::Inst*, ir::Block*> owners;
    CompileWorkspace& workspace;
    std::optional<RewriteQueueScope> dead;
    StackVector<const ir::Inst*, 4> implicit_roots;
    StackVector<ir::Block*, 4> flag_changes;
    bool single_block{};
    bool eliminate_dead{};
    bool dead_seeded{};
    bool block_dead_pending{true};
    bool flags_prepared{};
    ValueWorklist* values{};
};

void EliminateFlags(RewriteContext& rewrite, const FeatureSet& features,
                    ir::Flags live_out = ir::Flags::All);
void FoldValues(RewriteContext& rewrite,
                const FeatureSet& features,
                const BuilderCapabilities& target = {});
void NormalizeWidths(RewriteContext& rewrite);
void CleanupState(RewriteContext& rewrite, const GuestStateLayout& info,
                  const FeatureSet& features);
void SinkStateStores(RewriteContext& rewrite, const GuestStateLayout& info);
void FinalizeStateMappings(RewriteContext& rewrite, const GuestStateLayout& info);

}  // namespace swift::runtime::compile
