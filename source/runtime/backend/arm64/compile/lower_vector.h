#pragma once

#include <array>
#include <span>
#include <vector>
#include "actions.h"

namespace swift::runtime::compile {
class BlockAnalysisIndex;
struct LoweringCandidate;
}
namespace swift::runtime::backend::arm64 {

// Preparation-only output. The prepared result retains compact payloads and
// coverage in UnitActions, without instruction pointers or a per-block plan.
struct ScalarFPRActions final {
    std::vector<ScalarFPRAction> actions;
    std::vector<u16> covered;
    [[nodiscard]] const ScalarFPRAction* Find(u32 id) const;
    [[nodiscard]] bool Covers(u32 id) const;
};

namespace detail {
enum VectorRole : u16 {
    Publication = 1,
    PublicationLoad = 2,
    Conversion = 4,
    MemoryRead = 8,
    ExtractStore = 16,
};
struct ScalarFPROwner {
    u32 birth, end;
    u16 home;
    bool external_uses;
};
constexpr bool IsVectorOpaqueBarrier(ir::OpCode op) {
    using O = ir::OpCode;
    switch (op) {
        case O::CallLambda:
        case O::CallLocation:
        case O::CallDynamic:
        case O::X87Op:
        case O::Sse42Str:
        case O::GetUniformAddress:
        case O::UniformBarrier:
        case O::Goto:
        case O::NotGoto:
        case O::BindLabel:
            return true;
        default:
            return false;
    }
}
u16 SelectScalarPublication(const ir::Inst&, const ir::Inst*, const compile::BlockAnalysisIndex&);
bool SelectResidentConversion(const ir::Inst&,
                              std::array<bool, 32>&,
                              const compile::BlockAnalysisIndex&);
bool IsResidentMemoryRead(const ir::Inst&);
bool IsResidentExtract(const ir::Inst&);
ScalarFPRActions LowerScalarFPRPublications(const compile::BlockAnalysisIndex&,
                                            std::span<const compile::LoweringCandidate>,
                                            std::span<const ScalarFPROwner>);
ScalarFPRActions LowerResidentScalarFPR(const compile::BlockAnalysisIndex&,
                                        std::span<const compile::LoweringCandidate>);
}  // namespace detail
}  // namespace swift::runtime::backend::arm64
