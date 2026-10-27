#pragma once

#include "runtime/backend/arm64/compile/actions.h"
#include "runtime/compile/allocate/writer.h"

namespace swift::runtime::compile {
class BlockSummary;
}

namespace swift::runtime::backend::arm64 {

struct FPRContract;
struct ScalarFPRActions;
struct SelectActions;
struct FlagsTokenActions;
struct ScalarActions;
struct CarryActions;
struct MemoryActions;
struct ConsumerActions;
struct IntegerFlagsActions;
struct IntegerBranchActions;
class MemorySelections;
class GPRResidences;
struct LoweredEdges;
struct BackedgeSelection;

class AllocationPreparation : public compile::AllocationWriter {
public:
    AllocationPreparation(compile::Allocation& result,
                          compile::SemanticFacts& facts,
                          compile::CompileWorkspace& workspace,
                          UnitActions& actions);
    [[nodiscard]] const UnitActions& Actions() const { return actions; }
    [[nodiscard]] bool TargetPlanning() const { return LocationsEditing(); }
    void BeginTargetPlanning();
    void SealTargetEdits();
    void EndTargetPlanning();
    void AbortTargetPlanning() noexcept;
    void CertifyWidthElision(u32 id, bool low32);
    void CertifyHostRead(u32 id);
    void CertifyZeroStore(u32 id);
    void CertifyFPR(const FPRContract&);
    void StoreScalarFPR(const ScalarFPRActions&);
    void StoreSelects(const SelectActions&);
    void StoreFlagsTokens(const FlagsTokenActions&);
    void StoreScalar(const ScalarActions&);
    void StoreCarry(const CarryActions&);
    void StoreMemory(const MemoryActions&);
    void StoreConsumers(const ConsumerActions&);
    void StoreIntegerFlags(const IntegerFlagsActions&);
    void StoreIntegerBranch(const IntegerBranchActions&);
    void StorePublications(std::span<const PublicationAction>);
    void StoreMemorySelections(const MemorySelections&);
    void StoreFlagsOnlySpills(std::span<const FlagsOnlySpillAction>);
    void StoreGPRResidences(const ir::Block*, GPRResidences);
    void StoreScratch(const ir::Block*, const ScratchActions&);
    void DeclareBlock(const ir::Block* block,
                      const compile::BlockSummary& summary,
                      compile::PseudoFlagsView flags = {});
    void StoreEdges(LoweredEdges);
    void StoreAtomics(AtomicActions);
    void StoreSharedHostCalls(std::span<const u64> targets);
    void StoreDeferredLocations(std::span<const u16> instructions);
    void StoreBackedge(const ir::Block*, BackedgeSelection);
    void DeclareElidedPublication(u32 id);

private:
    UnitActions& actions;
};

}  // namespace swift::runtime::backend::arm64
