#pragma once

#include <span>
#include <vector>

#include "runtime/ir/function.h"
#include "runtime/ir/hir_storage.h"

namespace swift::runtime::compile {

// Owns the IR storage and compilation metadata through allocation/emission.
// Function-owned blocks remain owned through their actual persistent parent;
// independent blocks use their existing intrusive ownership.
class CompileUnit final {
public:
    explicit CompileUnit(const IntrusivePtr<ir::Block>& block);
    explicit CompileUnit(ir::HIRBlock* block);
    explicit CompileUnit(ir::HIRFunction* function);
    ~CompileUnit();
    CompileUnit(const CompileUnit&) = delete;
    CompileUnit& operator=(const CompileUnit&) = delete;
    CompileUnit(CompileUnit&& other) noexcept;
    CompileUnit& operator=(CompileUnit&& other) noexcept;

    void SetLoopHoistMetadata(ir::Block* block, const ir::LoopHoistMetadata& metadata);
    void ClearLoopHoistMetadata(ir::Block* block);
    // Ownership is fixed from construction. Only this coordinating operation
    // may merge preparing Functions and update every retained actual parent.
    [[nodiscard]] static bool MergeFunctions(std::span<CompileUnit* const> units);
    void SealIR();
    [[nodiscard]] bool IsSealed() const { return sealed; }
    [[nodiscard]] const ir::Block* GetBlock() const;
    [[nodiscard]] const ir::HIRFunction* GetFunction() const;
    [[nodiscard]] ir::Block* EditBlock();
    [[nodiscard]] ir::HIRFunction* EditFunction();

private:
    struct LoopHoistStorage {
        ir::Block* block;
        std::vector<const ir::Inst*> anchors;
    };

    void Clear();
    void MarkBound(bool bound);
    [[nodiscard]] bool Contains(const ir::Block* block) const;
    [[nodiscard]] ir::Function* Parent() const;
    enum class Kind : u8 { Empty, Block, HIRBlock, Function };
    union Input {
        ir::Block* block{};
        ir::HIRBlock* hir_block;
        ir::HIRFunction* function;
    };
    ir::NodeRef ir_owner;
    ir::HIRPoolLease hir_storage;
    std::vector<LoopHoistStorage> loop_hoists;
    Input input;
    Kind kind{Kind::Empty};
    bool sealed{};
};

// Finalize decoded byte ranges before semantic optimization or publication.
[[nodiscard]] size_t PrepareFunctionGuestRanges(ir::HIRFunction* function);

}  // namespace swift::runtime::compile
