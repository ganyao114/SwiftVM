#pragma once

#include <memory>
#include <optional>
#include <span>
#include <vector>
#include "runtime/common/types.h"

namespace swift::runtime::compile {
class CompileUnit;
class CompileWorkspace;
class OptimizedUnit;
class AllocationView;
}
namespace swift::runtime::ir {
class Block;
class HIRBlock;
class HIRFunction;
}  // namespace swift::runtime::ir
namespace swift::runtime {
struct PerfFixedCapture2;
}
namespace swift::runtime::backend {
class Module;
}  // namespace swift::runtime::backend

namespace swift::runtime::backend::arm64 {

enum class AllocationPreparationResult : u8 { Ready, RetryWithoutFPRCoalescing };

class UnitActions;
class AllocationPreparation;

// Preparation for standalone allocation clients. The ARM64 compiler owns
// target admission and asks the common allocator for explicit physical retries.
void CompleteARM64Allocation(const std::shared_ptr<Module>& module,
                             AllocationPreparation& allocation);

// Standalone clients use the same preparation and admission gates. These
// queries never renumber an existing HIR block or replan a stale result.
[[nodiscard]] bool CompleteARM64UnitForEmission(const std::shared_ptr<Module>&,
                                                AllocationPreparation&,
                                                const ir::Block*,
                                                const ir::HIRFunction*);
[[nodiscard]] bool ARM64UnitReadyForEmission(const std::shared_ptr<Module>&,
                                             compile::AllocationView,
                                             const UnitActions&,
                                             const ir::Block*,
                                             const ir::HIRFunction*);
[[nodiscard]] std::optional<u32> ARM64StandaloneHIRCapacity(const ir::Block&);

// A successful preparation owns its IR, allocation and target actions through
// emission. Moving it preserves all internal views and allocation addresses.
class Arm64PreparedUnit final {
public:
    ~Arm64PreparedUnit();
    Arm64PreparedUnit(const Arm64PreparedUnit&) = delete;
    Arm64PreparedUnit& operator=(const Arm64PreparedUnit&) = delete;
    Arm64PreparedUnit(Arm64PreparedUnit&&) noexcept;
    Arm64PreparedUnit& operator=(Arm64PreparedUnit&&) noexcept;

    [[nodiscard]] const compile::CompileUnit& Unit() const;
    [[nodiscard]] compile::AllocationView Allocation() const;
    [[nodiscard]] const UnitActions& Actions() const;
    [[nodiscard]] const ir::Block* Block() const;
    [[nodiscard]] const ir::HIRFunction* Function() const;
    [[nodiscard]] bool ReadyForEmission(const std::shared_ptr<Module>& module) const;

private:
    static std::optional<Arm64PreparedUnit> FromOptimized(
            const std::shared_ptr<Module>& module,
            compile::OptimizedUnit input,
            compile::CompileWorkspace& workspace,
            const PerfFixedCapture2* capture,
            std::optional<u32> final_hir_capacity = {});
    struct Storage;
    explicit Arm64PreparedUnit(std::unique_ptr<Storage> storage) noexcept;
    std::unique_ptr<Storage> storage;
    friend std::optional<Arm64PreparedUnit> PrepareBlock(const std::shared_ptr<Module>&,
                                                         const IntrusivePtr<ir::Block>&);
    friend std::optional<Arm64PreparedUnit> PrepareFinalBlock(const std::shared_ptr<Module>&,
                                                              ir::HIRBlock*);
    friend std::optional<Arm64PreparedUnit> PrepareFunction(const std::shared_ptr<Module>&,
                                                            ir::HIRFunction*);
    friend std::optional<std::vector<Arm64PreparedUnit>> PrepareFunctions(
            const std::shared_ptr<Module>&, std::span<ir::HIRFunction* const>);
};

[[nodiscard]] std::optional<Arm64PreparedUnit> PrepareBlock(const std::shared_ptr<Module>& module,
                                                            const IntrusivePtr<ir::Block>& block);
// Already-numbered HIR blocks use their final value/terminal contract and do
// not run the ordinary independent-Block optimization pipeline again.
[[nodiscard]] std::optional<Arm64PreparedUnit> PrepareFinalBlock(
        const std::shared_ptr<Module>& module, ir::HIRBlock* block);
[[nodiscard]] std::optional<Arm64PreparedUnit> PrepareFunction(
        const std::shared_ptr<Module>& module, ir::HIRFunction* function);
[[nodiscard]] std::optional<std::vector<Arm64PreparedUnit>> PrepareFunctions(
        const std::shared_ptr<Module>& module, std::span<ir::HIRFunction* const> functions);

}  // namespace swift::runtime::backend::arm64
