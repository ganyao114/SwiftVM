#pragma once

#include <optional>
#include <span>
#include <vector>
#include "actions.h"

namespace swift::runtime::compile {
class BlockAnalysisIndex;
class CompileWorkspace;
class GuestStateRegisterAccesses;
struct LoweringCandidate;
}
namespace swift::runtime::ir {
class Inst;
}
namespace swift::runtime::backend::arm64 {

// A preparation-only certificate. Admission finishes for every block before
// any certificate is installed into the owning UnitActions.
struct FPRContract final {
    struct Instruction {
        u16 id;
        FPRAction actions;
        u8 control{};
        bool operator==(const Instruction&) const = default;
    };
    bool valid{true};
    const ir::Inst* rejected{};
    std::vector<Instruction> instructions;
    void Certify(u32 id, FPRAction);
    void SetControl(u32 id, u8 control);
    [[nodiscard]] bool Has(u32 id, FPRAction) const;
    [[nodiscard]] std::optional<u8> Control(u32 id) const;

private:
    [[nodiscard]] const Instruction* Find(u32 id) const;
};

namespace detail {
enum FPRProofRole : u16 {
    FPRProofCandidate = 1,
    FPRProofDirect = 2,
    FPRProofResolved = 4,
    FPRProofOwners = 8,
    FPRProofRegisters = 16,
    FPRProofObservations = 32
};
// Index position avoids keeping a second pointer roster or widening owner rows.
struct FPRProofOwner {
    u16 position, home;
};
static_assert(sizeof(FPRProofOwner) == 4);
u16 SelectFPRProof(const ir::Inst&,
                   compile::AllocationView,
                   const FeatureSet&,
                   bool scalar_insert,
                   bool scalar_tie);
FPRContract VerifyFPRContract(compile::AllocationView,
                              const FeatureSet&,
                              bool scalar_insert,
                              bool scalar_tie,
                              const compile::BlockAnalysisIndex&,
                              const compile::GuestStateRegisterAccesses*,
                              std::optional<std::span<const ir::Inst* const>>,
                              std::span<const compile::LoweringCandidate>,
                              std::span<const FPRProofOwner>,
                              compile::CompileWorkspace&);
}  // namespace detail
}  // namespace swift::runtime::backend::arm64
