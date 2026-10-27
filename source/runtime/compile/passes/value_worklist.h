#pragma once

#include <optional>
#include "runtime/compile/workspace.h"

#include "passes.h"

namespace swift::runtime::compile {

enum class ValueCleanup : u8 { Values = 1, Widths = 2, All = 3 };

// Actual IR nodes, including pseudo operations; independent of sparse IDs.
inline constexpr u32 kConstantReuseWindow = 8;
inline constexpr bool IsConstantReuseBoundary(ir::OpCode op) {
    return op == ir::OpCode::Goto || op == ir::OpCode::NotGoto || op == ir::OpCode::BindLabel;
}

// One mutable IR epoch, before final numbering and SemanticFacts. The roster
// never dereferences a queued pointer after deletion. No rule inserts nodes;
// edits and deletes enter through RewriteContext while this scope is active.
// Construction prepares the first-phase queue without rewriting any IR.
class ValueWorklist final {
public:
    ValueWorklist(RewriteContext&, const FeatureSet&, const BuilderCapabilities&, ValueCleanup);
    ~ValueWorklist();
    ValueWorklist(const ValueWorklist&) = delete;
    ValueWorklist& operator=(const ValueWorklist&) = delete;
    void Run();
    void Drain();
    [[nodiscard]] bool Pending() const;
    [[nodiscard]] bool Nearby(const ir::Inst* source, const ir::Inst* user, u32 limit) const;
    [[nodiscard]] const ir::Inst* SingleConsumer(const ir::Inst*);
    [[nodiscard]] ir::Flags RequestedFlags(const ir::Inst*);
    [[nodiscard]] bool SharedConstant(const ir::Inst*) const;
    void ReplaceConstantUses(ir::Block*, const ir::Inst*, const ir::Inst*);

private:
    friend class RewriteContext;
    enum Reason : u8 { Simplify = 1, Propagate = 2, ReuseConstant = 4 };
    static constexpr u32 nil = UINT32_MAX;
    static constexpr u32 reuse_task = u32{1} << 31;
    void BeforeChange(const ir::Inst*);
    void AfterChange(const ir::Inst*, const ir::Inst::Values&);
    void BeforeErase(const ir::Inst*);
    void AfterErase(const ir::Inst::Values&);
    void BuildRoster();
    void PrepareUses();
    void AddInputs(u32 row);
    void RemoveInputs(u32 row);
    void SeedWidths();
    void Queue(u32 row, u8 reason);
    void QueueUsers(const ir::Inst*, bool propagate);
    void ChangedUseCount(const ir::Inst*);
    void QueueFollowingConstants(const ir::Inst*);
    void MarkSharedConstant(const ir::Inst*);
    [[nodiscard]] u32 Find(const ir::Inst*) const;
    [[nodiscard]] ir::Block* LiveOwner(const ir::Inst*) const;
    [[nodiscard]] bool Candidate(const ir::Inst&) const;
    template <class Visitor> void ForEachUser(const ir::Inst*, Visitor&&);

    RewriteContext& rewrite;
    CompileWorkspace& workspace;
    ValueQueueScope borrow;
    const FeatureSet& features;
    const BuilderCapabilities& target;
    ValueCleanup requested, active;
    u32 free_use{nil};
    size_t remaining{};
    bool uses_ready{};
    bool constants_ready{};
    bool ordered_queue{};
};

// Rules edit one live candidate. Initial ordered constant sharing seeds the
// same epoch; later scalar changes and deletion windows use its existing queue.
void SimplifyValue(RewriteContext&,
                   ValueWorklist&,
                   ir::Block*,
                   const ir::Inst*,
                   const FeatureSet&,
                   const BuilderCapabilities&);
void SimplifyWidth(RewriteContext&, ValueWorklist&, ir::Block*, const ir::Inst*);
void ShareConstants(RewriteContext&, const FeatureSet&);
void SimplifyConstant(
        RewriteContext&, ValueWorklist&, ir::Block*, const ir::Inst*, const FeatureSet&);
void RetargetConstantUser(
        RewriteContext&, ir::Block*, const ir::Inst*, const ir::Inst*, const ir::Inst*);

// Keep the borrow alive for later DCE/flags feedback only when this unit has
// an eligible value/width rule. Constant sharing still runs without a roster.
void BeginValueCleanup(std::optional<ValueWorklist>&,
                       RewriteContext&,
                       const FeatureSet&,
                       const BuilderCapabilities&,
                       ValueCleanup);

}  // namespace swift::runtime::compile
