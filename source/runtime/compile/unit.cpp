#include "unit.h"

#include <algorithm>
#include <utility>
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::compile {

CompileUnit::~CompileUnit() {
    Clear();
}

CompileUnit::CompileUnit(const IntrusivePtr<ir::Block>& owner) {
    auto* block = owner.get();
    ASSERT(block && !block->owning_function);
    block->CheckMutable();
    ASSERT(!block->instruction_owner.compiling);
    ir_owner = ir::NodeRef{block};
    input.block = block;
    kind = Kind::Block;
    MarkBound(true);
}

CompileUnit::CompileUnit(ir::HIRBlock* hir) {
    ASSERT(hir);
    auto storage = hir->RetainStorage();
    auto* block = hir->GetBlock();
    block->CheckMutable();
    ASSERT(!block->instruction_owner.compiling);
    ir::NodeRef owner;
    if (auto* parent = block->owning_function) {
        parent->CheckMutable();
        owner = ir::NodeRef{parent};
        parent->AcquireCompilation();
    } else if (!hir->unpublished_block) {
        ASSERT(hir->function && hir->function->standalone_blocks_published &&
               !hir->standalone_publication_pending);
        owner = ir::NodeRef{block};
    }
    hir_storage = std::move(storage);
    ir_owner = std::move(owner);
    input.hir_block = hir;
    kind = Kind::HIRBlock;
    MarkBound(true);
}

CompileUnit::CompileUnit(ir::HIRFunction* function) {
    ASSERT(function && !function->HasStandaloneBlocks());
    auto storage = function->RetainStorage();
    ASSERT(function->blocks.size() > 1 && function->block_list.empty());
    function->CheckMutable();
    auto* owner = function->blocks[1]->block->owning_function;
    ASSERT(owner);
    owner->CheckMutable();
    for (auto* hir : function->blocks) {
        if (!hir) continue;
        hir->block->CheckMutable();
        ASSERT(!hir->block->instruction_owner.compiling);
        ASSERT(hir->unpublished_block || hir->block->owning_function == owner);
    }
    ir::NodeRef persistent{owner};
    owner->AcquireCompilation();
    if (owner != function->GetFunction()) function->GetFunction()->AcquireCompilation();
    hir_storage = std::move(storage);
    ir_owner = std::move(persistent);
    input.function = function;
    kind = Kind::Function;
    MarkBound(true);
}

CompileUnit::CompileUnit(CompileUnit&& other) noexcept
        : ir_owner(std::move(other.ir_owner))
        , hir_storage(std::move(other.hir_storage))
        , loop_hoists(std::move(other.loop_hoists))
        , input(other.input)
        , kind(std::exchange(other.kind, Kind::Empty))
        , sealed(std::exchange(other.sealed, false)) {}

CompileUnit& CompileUnit::operator=(CompileUnit&& other) noexcept {
    if (this != &other) {
        Clear();
        ir_owner = std::move(other.ir_owner);
        hir_storage = std::move(other.hir_storage);
        loop_hoists = std::move(other.loop_hoists);
        input = other.input;
        kind = std::exchange(other.kind, Kind::Empty);
        sealed = std::exchange(other.sealed, false);
    }
    return *this;
}

const ir::Block* CompileUnit::GetBlock() const {
    if (kind == Kind::Block) return input.block;
    if (kind == Kind::HIRBlock) return input.hir_block->GetBlock();
    return nullptr;
}

const ir::HIRFunction* CompileUnit::GetFunction() const {
    return kind == Kind::Function ? input.function : nullptr;
}

ir::Block* CompileUnit::EditBlock() {
    ASSERT(!sealed && (kind == Kind::Block || kind == Kind::HIRBlock));
    return kind == Kind::Block ? input.block : input.hir_block->GetBlock();
}

ir::HIRFunction* CompileUnit::EditFunction() {
    ASSERT(!sealed && kind == Kind::Function);
    input.function->CheckMutable();
    return input.function;
}

ir::Function* CompileUnit::Parent() const {
    auto* node = ir_owner.Get();
    return node && node->GetNodeType() == ir::AddressNode::Function
                   ? static_cast<ir::Function*>(node)
                   : nullptr;
}

void CompileUnit::MarkBound(bool bound) {
    if (kind == Kind::Function) {
        for (auto* hir : input.function->blocks)
            if (hir) hir->block->instruction_owner.compiling = bound;
    } else if (kind == Kind::Block) {
        input.block->instruction_owner.compiling = bound;
    } else if (kind == Kind::HIRBlock) {
        input.hir_block->block->instruction_owner.compiling = bound;
    }
}

bool CompileUnit::Contains(const ir::Block* block) const {
    if (!block) return false;
    if (kind != Kind::Function) return GetBlock() == block;
    const auto order = block->instruction_owner.hir_order;
    const auto blocks = input.function->GetHIRBlocks();
    return order < blocks.size() && blocks[order] && blocks[order]->GetBlock() == block;
}

bool CompileUnit::MergeFunctions(std::span<CompileUnit* const> units) {
    ASSERT(!units.empty());
    if (units.size() == 1) {
        ASSERT(units.front() && units.front()->kind == Kind::Function && !units.front()->sealed);
        return true;
    }
    StackVector<ir::Function*, 4> sources;
    for (size_t i = 0; i < units.size(); ++i) {
        auto* unit = units[i];
        ASSERT(unit && unit->kind == Kind::Function && !unit->sealed);
        for (size_t j = 0; j < i; ++j)
            if (units[j] == unit) return false;
        auto* header = unit->input.function->GetFunction();
        ASSERT(unit->Parent() == header);
        ASSERT(header->compiling_regions.load(std::memory_order_acquire) == 1);
        if (i) sources.push_back(header);
    }
    auto* owner = units.front()->Parent();
    if (!owner->TransferBlocksFrom({sources.data(), sources.size()})) return false;
    for (size_t i = 1; i < units.size(); ++i) {
        owner->AcquireCompilation();
        // The source header remains held by its HIR pool and retains its own
        // compilation guard. The actual parent now also covers this unit.
        units[i]->ir_owner = ir::NodeRef{owner};
    }
    return true;
}

void CompileUnit::SealIR() {
    ASSERT(kind != Kind::Empty && !sealed);
    if (kind == Kind::Function) {
        input.function->SetIRReadOnly(true);
    } else {
        auto* block = EditBlock();
        block->CheckMutable();
        if (auto* parent = Parent()) parent->AcquireIRReadOnly();
        block->instruction_owner.sealed = true;
    }
    sealed = true;
}

void CompileUnit::SetLoopHoistMetadata(ir::Block* block,
                                      const ir::LoopHoistMetadata& metadata) {
    ASSERT(!sealed && Contains(block) && !metadata.anchors.empty());
    block->CheckMutable();
    auto found = std::find_if(loop_hoists.begin(), loop_hoists.end(),
            [block](const auto& storage) { return storage.block == block; });
    if (found == loop_hoists.end()) {
        LoopHoistStorage storage{block,
                                {metadata.anchors.begin(), metadata.anchors.end()}};
        loop_hoists.push_back(std::move(storage));
        found = std::prev(loop_hoists.end());
    } else {
        found->anchors.assign(metadata.anchors.begin(), metadata.anchors.end());
    }
    auto owned = metadata;
    owned.anchors = found->anchors;
    block->SetLoopHoistMetadata(owned);
}

void CompileUnit::ClearLoopHoistMetadata(ir::Block* block) {
    ASSERT(!sealed && Contains(block));
    block->CheckMutable();
    const auto found = std::find_if(loop_hoists.begin(), loop_hoists.end(),
            [block](const auto& storage) { return storage.block == block; });
    if (found == loop_hoists.end()) return;
    if (block->GetLoopHoistMetadata().anchors.data() == found->anchors.data()) {
        block->SetLoopHoistMetadata({});
    }
    loop_hoists.erase(found);
}

void CompileUnit::Clear() {
    if (kind == Kind::Empty) return;
    if (sealed) {
        if (kind == Kind::Function) {
            input.function->SetIRReadOnly(false);
        } else {
            auto* block = kind == Kind::Block ? input.block : input.hir_block->block;
            block->instruction_owner.sealed = false;
            if (auto* parent = Parent()) parent->ReleaseIRReadOnly();
        }
        sealed = false;
    }
    for (const auto& storage : loop_hoists) {
        if (storage.block->GetLoopHoistMetadata().anchors.data() == storage.anchors.data()) {
            storage.block->SetLoopHoistMetadata({});
        }
    }
    loop_hoists.clear();
    MarkBound(false);
    if (auto* parent = Parent()) {
        parent->ReleaseCompilation();
        if (kind == Kind::Function && input.function->GetFunction() != parent)
            input.function->GetFunction()->ReleaseCompilation();
    }
    kind = Kind::Empty;
    input.block = nullptr;
    hir_storage.Reset();
    ir_owner = {};
}

size_t PrepareFunctionGuestRanges(ir::HIRFunction* function) {
    size_t decoded_blocks = 0;
    VAddr max_end = function->GetFunction()->GetStartLocation().Value();
    for (auto* hir_block : function->GetHIRBlocks()) {
        if (!hir_block || hir_block == function->GetEntryBlock()) continue;
        auto* block = function->EditBlock(hir_block).GetBlock();
        if (block->GetInstList().empty()) continue;
        ++decoded_blocks;
        u64 block_size = 0;
        for (const auto& inst : block->GetInstList())
            if (inst.GetOp() == ir::OpCode::AdvancePC) block_size += inst.GetArg<ir::Imm>(0).Get();
        const auto start = block->GetStartLocation().Value();
        // A decoded block without a surviving AdvancePC still depends on its start byte.
        block->SetEndLocation(ir::Location{start + std::max<u64>(block_size, 1)});
        max_end = std::max(max_end, block->GetEndLocation().Value());
    }
    function->GetFunction()->SetEndLocation(ir::Location{max_end});
    return decoded_blocks;
}

}  // namespace swift::runtime::compile
