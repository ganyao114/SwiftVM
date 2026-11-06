#include "program.h"

#include <atomic>
#include <stdexcept>
#include <utility>

namespace swift::runtime::ir {
namespace {
std::uint64_t NewUnitId() {
    // Request identity is minted once, never once per operation/edge. Refuse
    // exhaustion permanently rather than reusing a destroyed request's ID.
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    for (;;) {
        if (value == std::numeric_limits<std::uint64_t>::max())
            throw std::length_error("IR request identities exhausted");
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
            return value;
    }
}
}  // namespace

Program::Storage::Storage(Scope scope, TargetInfo target)
        : unit(NewUnitId()), scope(scope), target(target) {}

void Program::Storage::Reset(Scope next_scope, TargetInfo next_target) {
    const auto next_unit = NewUnitId();
    operations.clear(); operands.clear(); blocks.clear();
    unit = next_unit; scope = next_scope; target = next_target;
    entry = {}; active = {}; unfinished = 0;
}

Error Program::Storage::CheckBlock(BlockRef ref) const noexcept {
    if (!ref.unit || !ref.id) return Error::InvalidBlock;
    if (ref.unit != unit) return Error::ForeignUnit;
    return ref.id.value < blocks.size() ? Error::None : Error::InvalidBlock;
}

WorkCounts Program::Storage::CountWork() const noexcept {
    WorkCounts result{operations.size(), 0, operands.size(), blocks.size()};
    const std::span<const Cell> arena{operands};
    for (const auto& op : operations) {
        // Empty records have no arena position, including after tail edits.
        if (!op.operand_count) continue;
        for (const auto cell : CellRange{arena.subspan(op.first_operand, op.operand_count)})
            result.value_uses += cell.Kind() == OperandKind::Value;
    }
    for (const auto& block : blocks) result.value_uses += block.terminal.HasValue();
    return result;
}

Program::Program() noexcept = default;
Program::~Program() = default;
Program::Program(Program&&) noexcept = default;
Program& Program::operator=(Program&&) noexcept = default;
Program::Program(std::unique_ptr<Storage> input) noexcept : storage(std::move(input)) {}
CellRange Program::Operands(const Operation& op) const noexcept {
    if (!storage || storage->operations.empty()) return {};
    const auto address = reinterpret_cast<std::uintptr_t>(&op);
    const auto base = reinterpret_cast<std::uintptr_t>(storage->operations.data());
    if (address < base) return {};
    const auto offset = address - base;
    if (offset % sizeof(Operation) || offset / sizeof(Operation) >= storage->operations.size()) return {};
    return Operands(NodeId{static_cast<std::uint32_t>(offset / sizeof(Operation))});
}
bool Program::Contains(Value value) const noexcept {
    Type type;
    return storage && storage->ReadType(value, Type::Void, type) == Error::None;
}
std::size_t Program::StorageBytes() const noexcept {
    return storage ? sizeof(Storage) + storage->operations.size() * sizeof(Operation) +
            storage->operands.size() * sizeof(Cell) + storage->blocks.size() * sizeof(BlockRecord) : 0;
}
std::size_t Program::ReservedBytes() const noexcept {
    return storage ? sizeof(Storage) + storage->operations.capacity() * sizeof(Operation) +
            storage->operands.capacity() * sizeof(Cell) + storage->blocks.capacity() * sizeof(BlockRecord) : 0;
}

}  // namespace swift::runtime::ir
