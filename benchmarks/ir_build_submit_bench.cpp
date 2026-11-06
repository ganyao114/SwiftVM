#include "runtime/ir/builder.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The replacement allocator is local to this executable. Counts are requested
// C++ new/new[] bytes (including aligned new), excluding this header and malloc
// bookkeeping. The benchmark and the core execute on one thread.
namespace allocation {
enum Phase : unsigned { Construction, Submit, Destruction };
struct Counters {
    std::array<std::uint64_t, 3> calls{}, bytes{};
    std::uint64_t live{}, peak{}, unit_floor{}, unit_peak{};
};
struct Header {
    void* raw;
    std::size_t bytes;
    std::uint64_t epoch;
};
constinit Counters counters{};
constinit std::uint64_t epoch{};
constinit bool enabled{};
constinit Phase phase{Construction};

void* Allocate(std::size_t bytes, std::size_t alignment) {
    alignment = std::max(alignment, alignof(std::max_align_t));
    const auto payload = std::max<std::size_t>(bytes, 1);
    if (payload > std::numeric_limits<std::size_t>::max() - sizeof(Header) - alignment)
        throw std::bad_alloc{};
    void* raw = std::malloc(payload + sizeof(Header) + alignment - 1);
    if (!raw) throw std::bad_alloc{};
    const auto address = (reinterpret_cast<std::uintptr_t>(raw) + sizeof(Header) + alignment - 1) &
                         ~(alignment - 1);
    auto* header = reinterpret_cast<Header*>(address) - 1;
    *header = {raw, bytes, enabled ? epoch : 0};
    if (enabled) {
        ++counters.calls[phase];
        counters.bytes[phase] += bytes;
        counters.live += bytes;
        counters.peak = std::max(counters.peak, counters.live);
        counters.unit_peak = std::max(counters.unit_peak, counters.live - counters.unit_floor);
    }
    return reinterpret_cast<void*>(address);
}
void Free(void* pointer) noexcept {
    if (!pointer) return;
    const auto* header = static_cast<Header*>(pointer) - 1;
    if (enabled && header->epoch == epoch) {
        if (header->bytes > counters.live) std::abort();
        counters.live -= header->bytes;
    }
    std::free(header->raw);
}
void Begin() noexcept {
    counters = {};
    if (++epoch == 0) std::abort();
    phase = Construction;
    enabled = true;
}
}  // namespace allocation

void* operator new(std::size_t size) {
    return allocation::Allocate(size, alignof(std::max_align_t));
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, std::align_val_t align) {
    return allocation::Allocate(size, static_cast<std::size_t>(align));
}
void* operator new[](std::size_t size, std::align_val_t align) {
    return ::operator new(size, align);
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}
void* operator new(std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size, align);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, align, tag);
}
void operator delete(void* p) noexcept { allocation::Free(p); }
void operator delete[](void* p) noexcept { allocation::Free(p); }
void operator delete(void* p, std::size_t) noexcept { allocation::Free(p); }
void operator delete[](void* p, std::size_t) noexcept { allocation::Free(p); }
void operator delete(void* p, std::align_val_t) noexcept { allocation::Free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { allocation::Free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { allocation::Free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { allocation::Free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { allocation::Free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { allocation::Free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    allocation::Free(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    allocation::Free(p);
}

namespace {
using namespace swift::runtime::ir;
using Clock = std::chrono::steady_clock;
constexpr std::size_t max_blocks = 256;
struct Failure {
    Error error;
    std::size_t block{}, local{};
};
void Check(Error error, std::size_t block, std::size_t local) {
    if (error != Error::None) throw Failure{error, block, local};
}
struct Fixture {
    std::size_t nodes{}, blocks{}, cells{}, logical_cells{}, value_uses{}, terminal_uses{};
    Scope scope{};
    TargetInfo target{};
    bool reserve{};
};
std::size_t BlockSize(const Fixture& f, std::size_t b) {
    return f.nodes / f.blocks + (b < f.nodes % f.blocks);
}
bool BranchBlock(const Fixture& f, std::size_t b) {
    return f.scope == Scope::Function && b % 3 == 0 && b + 2 < f.blocks;
}
// Recipe counting is outside the timed region. It creates no IR/operand array.
Fixture MakeFixture(
        std::size_t nodes, std::size_t blocks, Scope scope, TargetMode mode, bool reserve) {
    Fixture f{nodes,
              scope == Scope::Block ? 1 : blocks,
              0,
              0,
              0,
              0,
              scope,
              {mode, 0xa64, 0x1020304050607080ull},
              reserve};
    constexpr std::array<std::size_t, 12> cells{1, 3, 4, 3, 4, 1, 2, 3, 7, 3, 1, 3};
    constexpr std::array<std::size_t, 12> uses{0, 1, 2, 1, 2, 0, 1, 2, 3, 1, 1, 1};
    for (std::size_t b = 0; b < f.blocks; ++b) {
        for (std::size_t i = 0; i < BlockSize(f, b); ++i) {
            const bool branch = BranchBlock(f, b) && i + 1 == BlockSize(f, b);
            f.cells += branch ? 1 : cells[i % 12];
            const auto identity = i % 12 == 1 || i % 12 == 7 || i % 12 == 11;
            f.logical_cells += branch ? 1 : cells[i % 12] - identity;
            f.value_uses += branch ? 0 : uses[i % 12];
            if (!branch && i == 1 && b) ++f.value_uses;  // actual cross-block use
        }
        f.terminal_uses += BranchBlock(f, b) || b + 1 == f.blocks;
    }
    return f;
}
// Same fixture and actual storage; only the opcode's binding time differs.
template <OpCode opcode> AppendResult Emit(Builder& builder,
                                           Type result,
                                           std::initializer_list<Operand> operands,
                                           OperationMetadata metadata = {}) {
#ifdef SVM_IR_STATIC_WRITER
    return builder.Emit<opcode>(result, operands, metadata);
#else
    return builder.Append(opcode, result, operands, metadata);
#endif
}
std::size_t ReserveOperandUnits(const Fixture& f) {
#ifdef SVM_IR_BASELINE_LAYOUT
    return f.cells;  // Baseline Reserve counts 16-byte stored cells.
#else
    // This fixture has no wide payloads: canonical cells are actual 8-byte words.
    return f.logical_cells;
#endif
}
void Build(Builder& builder, const Fixture& f) {
    if (f.reserve) Check(builder.Reserve(f.nodes, ReserveOperandUnits(f), f.blocks), 0, 0);
    std::array<BlockRef, max_blocks> blocks{};
    for (std::size_t b = 0; b < f.blocks; ++b) {
        const auto result = builder.CreateBlock();
        Check(result.error, b, 0);
        blocks[b] = result.block;
    }
    Value previous{};
    for (std::size_t b = 0; b < f.blocks; ++b) {
        Check(builder.BeginBlock(blocks[b]), b, 0);
        Value current{}, base{}, condition{};
        const auto count = BlockSize(f, b);
        for (std::size_t i = 0; i < count; ++i) {
            AppendResult out;
            if (BranchBlock(f, b) && i + 1 == count) {
                out = Emit<OpCode::LoadImm>(
                        builder, Type::Void, {Operand::Immediate(b & 1, Type::Bool)});
                condition = out.GetValue();
            } else {
                const OperationMetadata arithmetic{0x41, FlagProfile::ArithmeticRaw, false};
                switch (i % 12) {
                    case 0:
                        out = Emit<OpCode::LoadImm>(
                                builder,
                                Type::Void,
                                {Operand::Immediate(0x12340000 + b * 4096 + i, Type::U64)});
                        break;
                    case 1:
                        out = Emit<OpCode::Add>(builder,
                                                Type::Void,
                                                {Operand::ValueOf(current),
                                                 Operand::Expression(OperandOp::Identity),
                                                 b && i == 1 ? Operand::ValueOf(previous)
                                                             : Operand::Immediate(7, Type::U64)},
                                                arithmetic);
                        break;
                    case 2:
                        out = Emit<OpCode::Xor>(builder,
                                                Type::Void,
                                                {Operand::ValueOf(current),
                                                 Operand::Expression(OperandOp::Lsl, 2),
                                                 Operand::ValueOf(base),
                                                 Operand::Immediate(3, Type::U64)},
                                                {0x40, FlagProfile::LogicalZeroCVA, false});
                        break;
                    case 3:
                        out = Emit<OpCode::LoadMemory>(
                                builder,
                                Type::U64,
                                {Operand::Expression(OperandOp::Add, 2),
                                 Operand::ValueOf(base),
                                 Operand::Immediate((i & 63) * 8, Type::U64)});
                        break;
                    case 4:
                        out = Emit<OpCode::StoreMemory>(
                                builder,
                                Type::Void,
                                {Operand::Expression(OperandOp::Add, 2),
                                 Operand::ValueOf(base),
                                 Operand::Immediate((i & 63) * 8, Type::U64),
                                 Operand::ValueOf(current)});
                        break;
                    case 5:
                        out = Emit<OpCode::LoadUniform>(
                                builder,
                                Type::Void,
                                {Operand::Uniform(static_cast<std::uint32_t>((i & 31) * 8),
                                                  Type::U64)});
                        break;
                    case 6:
                        out = Emit<OpCode::StoreUniform>(
                                builder,
                                Type::Void,
                                {Operand::Uniform(static_cast<std::uint32_t>((i & 31) * 8),
                                                  Type::U64),
                                 Operand::ValueOf(current)});
                        break;
                    case 7:
                        out = Emit<OpCode::Mul>(builder,
                                                Type::Void,
                                                {Operand::ValueOf(current),
                                                 Operand::Expression(OperandOp::Identity),
                                                 Operand::ValueOf(base)});
                        break;
                    case 8:
                        out = Emit<OpCode::CallLocation>(builder,
                                                         Type::U64,
                                                         {Operand::Lambda(),
                                                          Operand::Immediate(0x1000, Type::U64),
                                                          Operand::Arguments(4),
                                                          Operand::ValueOf(current),
                                                          Operand::ValueOf(base),
                                                          Operand::Immediate(17, Type::U32),
                                                          Operand::ValueOf(base)});
                        break;
                    case 9:
                        out = Emit<OpCode::BitExtract>(builder,
                                                       Type::Void,
                                                       {Operand::ValueOf(current, Type::U32),
                                                        Operand::Immediate(0, Type::U32),
                                                        Operand::Immediate(32, Type::U32)});
                        break;
                    case 10:
                        out = Emit<OpCode::ZeroExtend32To64>(
                                builder, Type::Void, {Operand::ValueOf(current)});
                        break;
                    case 11:
                        out = Emit<OpCode::Sub>(builder,
                                                Type::Void,
                                                {Operand::ValueOf(current),
                                                 Operand::Expression(OperandOp::Identity),
                                                 Operand::Immediate(1, Type::U64)},
                                                arithmetic);
                        break;
                }
                if (out.GetValue()) current = out.GetValue();
                if (i == 0) base = current;
            }
            Check(out.error, b, i);
        }
        const auto terminal = BranchBlock(f, b)
                                      ? Terminal::Branch(condition, blocks[b + 1], blocks[b + 2])
                              : b + 1 < f.blocks ? Terminal::Jump(blocks[b + 1])
                                                 : Terminal::Return(current);
        Check(builder.EndBlock(terminal), b, count);
        previous = current;
    }
}

// Keep canonical_semantic_v1 identical in both benchmark translation units.
// This read-only diagnostic shim is also compiled against the saved baseline.
struct Semantics {
    std::uint64_t checksum{1469598103934665603ull};
    std::size_t stored_cells{}, logical_cells{}, physical_words{}, values{}, terminals{};
};
std::uint64_t Mix(std::uint64_t hash, std::uint64_t value) {
    return (hash ^ value) * 1099511628211ull;
}
Semantics ObserveProgram(const Program& p) {
#if !defined(SVM_IR_BASELINE_LAYOUT) && !defined(SVM_IR_BASELINE_COMPACT)
    // v1's fixtures have an implicit fixed entry. Do not apply that schema to
    // newly supported nonzero-entry programs without a new semantic identity.
    if (p.EntryBlock().value != 0)
        throw std::runtime_error("canonical_semantic_v1 requires entry 0");
#endif
    Semantics out;
    auto& hash = out.checksum;
    hash = Mix(hash, static_cast<unsigned>(p.ScopeKind()));
    const auto target = p.Target();
    hash = Mix(hash, static_cast<unsigned>(target.mode));
    hash = Mix(hash, target.target);
    hash = Mix(hash, target.features);
    hash = Mix(hash, p.Operations().size());
    hash = Mix(hash, p.Blocks().size());
#ifdef SVM_IR_BASELINE_LAYOUT
    out.physical_words = p.FlatOperands().size() * 2;  // Historical Cell16.
#elif defined(SVM_IR_BASELINE_COMPACT)
    out.physical_words = p.FlatOperands().WordCount();
#else
    out.physical_words = p.OperandArena().size();
#endif
#if !defined(SVM_IR_BASELINE_LAYOUT) && !defined(SVM_IR_BASELINE_COMPACT)
    std::uint32_t local_id{};
#endif
    for (const auto& op : p.Operations()) {
#ifdef SVM_IR_BASELINE_LAYOUT
        const auto metadata = op.metadata;
#else
        const auto metadata = op.Metadata();
#endif
        hash = Mix(hash, 0x4f50424547494eull);  // operation boundary, not a physical offset
        hash = Mix(hash, static_cast<unsigned>(op.opcode));
        hash = Mix(hash, static_cast<unsigned>(op.type));
        hash = Mix(hash, op.block.value);
        hash = Mix(hash, metadata.flag_outputs);
        hash = Mix(hash, static_cast<unsigned>(metadata.profile));
        hash = Mix(hash, metadata.branch_only);
#if defined(SVM_IR_BASELINE_LAYOUT) || defined(SVM_IR_BASELINE_COMPACT)
        const auto operands = p.Operands(op);
#else
        const auto operands = p.Operands(NodeId{local_id++});
#endif
        for (const auto cell : operands) {
#ifdef SVM_IR_BASELINE_LAYOUT
            const auto kind = cell.kind;
            const auto view = cell.type;
            const auto payload = cell.payload;
            const auto count = cell.count;
            const auto code = cell.code;
#else
            const auto kind = cell.Kind();
            const auto view = cell.TypeView();
            const auto payload = cell.Payload();
            const auto count = cell.Count();
            const auto code = cell.Code();
#endif
            ++out.stored_cells;
            // Identity(1) and the same direct atom have identical semantics.
            if (kind == OperandKind::Expression &&
                code == static_cast<unsigned>(OperandOp::Identity) && count == 1)
                continue;
            ++out.logical_cells;
            out.values += kind == OperandKind::Value;
            hash = Mix(hash, 0x43454c4cull);
            hash = Mix(hash, static_cast<unsigned>(kind));
            hash = Mix(hash, static_cast<unsigned>(view));
            hash = Mix(hash, payload);
            hash = Mix(hash, count);
            hash = Mix(hash, code);
        }
        hash = Mix(hash, 0x4f50454e44ull);
    }
    for (const auto& block : p.Blocks()) {
#ifdef SVM_IR_BASELINE_LAYOUT
        const auto state = block.state;
        const auto has_value = block.terminal.value_present;
        const auto value = block.terminal.value.node.value;
        const auto first = block.terminal.first.id.value, second = block.terminal.second.id.value;
#else
        const auto state = block.State();
        const auto has_value = block.terminal.HasValue();
        const auto value = block.terminal.value.value;
        const auto first = block.terminal.first.value, second = block.terminal.second.value;
#endif
        out.terminals += has_value;
        hash = Mix(hash, 0x424c4f434bull);
        hash = Mix(hash, block.first_node);
        hash = Mix(hash, block.node_count);
        hash = Mix(hash, static_cast<unsigned>(state));
        hash = Mix(hash, static_cast<unsigned>(block.terminal.kind));
        hash = Mix(hash, has_value);
        hash = Mix(hash, static_cast<unsigned>(block.terminal.value_type));
        hash = Mix(hash, value);
        hash = Mix(hash, first);
        hash = Mix(hash, second);
    }
    return out;
}
// All semantic traversal is outside construction, Submit and destruction.
Semantics Validate(const Program& p, const Fixture& f) {
    if (p.Operations().size() != f.nodes || p.Blocks().size() != f.blocks ||
        p.Target() != f.target || p.ScopeKind() != f.scope)
        throw std::runtime_error("fixture output shape/metadata mismatch");
    const auto observed = ObserveProgram(p);
#ifdef SVM_IR_BASELINE_LAYOUT
    const auto expected_stored_cells = f.cells, expected_words = f.cells * 2;
#else
    const auto expected_stored_cells = f.logical_cells, expected_words = f.logical_cells;
#endif
    if (observed.values != f.value_uses || observed.terminals != f.terminal_uses ||
        observed.logical_cells != f.logical_cells ||
        observed.stored_cells != expected_stored_cells || observed.physical_words != expected_words)
        throw std::runtime_error("fixture occurrence/storage count mismatch");
    return observed;
}
double Ns(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::nano>(b - a).count();
}
enum class Mode { Sequential, BatchDiagnostic };
struct Counts {
    std::size_t attempted{}, published{}, failed{}, abandoned{};
    std::size_t published_N{}, published_words{}, E_from_verified_recipe{};
    std::size_t abandoned_N{}, abandoned_words_from_verified_recipe{};
};
struct VerifiedSequence {
    Semantics positive;
    std::uint64_t checksum{1469598103934665603ull};
    std::array<Semantics, 2> rejected_prefixes{};
};
// A rejected request is a workload outcome, not a benchmark process failure.
// Recovery below is used only by the separate untimed semantic pass.
Semantics RejectedPrefix(const Fixture& f, unsigned which, bool recover) {
    allocation::counters.unit_floor = allocation::counters.live;
    allocation::phase = allocation::Construction;
    Builder builder(f.scope, f.target);
    if (f.reserve) Check(builder.Reserve(1, 1, 1), 0, 0);
    const auto block = builder.CreateBlock();
    Check(block.error, 0, 0);
    Check(builder.BeginBlock(block.block), 0, 0);
    const auto prefix =
            Emit<OpCode::LoadImm>(builder, Type::Void, {Operand::Immediate(7, Type::U64)});
    Check(prefix.error, 0, 0);
    if (prefix.node.value != 0) throw std::runtime_error("unexpected rejected-prefix identity");
    auto foreign = prefix.GetValue();
    ++foreign.unit;
    const auto rejected = which == 0 ? builder.Append(OpCode::Count, Type::Void, {})
                                     : builder.Append(OpCode::Add,
                                                      Type::Void,
                                                      {Operand::ValueOf(prefix.GetValue()),
                                                       Operand::Expression(OperandOp::Identity),
                                                       Operand::ValueOf(foreign)});
    const auto expected = which == 0 ? Error::InvalidOpcode : Error::ForeignUnit;
    if (rejected.error != expected)
        throw std::runtime_error("fixed negative request changed outcome");
    Semantics observed;
    if (recover) {
        Check(builder.EndBlock(Terminal::Unreachable()), 0, 0);
        auto result = builder.Submit();
        Check(result.error, 0, 0);
        observed = ObserveProgram(result.program);
        if (result.program.Operations().size() != 1 || observed.values || observed.terminals ||
            observed.logical_cells != 1)
            throw std::runtime_error("rejected append changed its prefix");
    }
    allocation::phase = allocation::Destruction;
    return observed;  // In the timed path this destroys the abandoned Builder.
}
VerifiedSequence VerifySequence(const Fixture& f, std::size_t requests) {
    VerifiedSequence verified;
    // Every request in the fixed sequence is replayed and checked, not front().
    for (std::size_t i = 0; i < requests; ++i) {
        Builder builder(f.scope, f.target);
        Build(builder, f);
        auto result = builder.Submit();
        Check(result.error, 0, 0);
        const auto observed = Validate(result.program, f);
        if (!i)
            verified.positive = observed;
        else if (observed.checksum != verified.positive.checksum)
            throw std::runtime_error("fixed sequence semantics differ");
        verified.checksum = Mix(verified.checksum, observed.checksum);
    }
    for (unsigned i = 0; i < 2; ++i) {
        verified.rejected_prefixes[i] = RejectedPrefix(f, i, true);
        verified.checksum = Mix(verified.checksum, verified.rejected_prefixes[i].checksum);
    }
    return verified;
}
struct Sample {
    double sequence{}, construction{}, submit{}, destruction{}, rejected{};
    allocation::Counters allocations{};
    Counts counts;
    std::size_t storage_bytes{}, reserved_bytes{};
};
void AccountPublished(const Program& p,
                      const Fixture& f,
                      const VerifiedSequence& verified,
                      Counts& counts) {
#ifdef SVM_IR_BASELINE_LAYOUT
    const auto words = p.FlatOperands().size() * 2;
#elif defined(SVM_IR_BASELINE_COMPACT)
    const auto words = p.FlatOperands().WordCount();
#else
    const auto words = p.OperandArena().size();
#endif
    const auto nodes = p.Operations().size();
    if (nodes != f.nodes || words != verified.positive.physical_words)
        throw std::runtime_error("timed request shape differs from verified recipe");
    ++counts.attempted;
    ++counts.published;
    counts.published_N += nodes;
    counts.published_words += words;
    // E is from the complete untimed replay of this fixed recipe, not a new
    // traversal of the timed Program. O(1) request accounting is timed.
    counts.E_from_verified_recipe += verified.positive.values + verified.positive.terminals;
}
void RunRejected(const Fixture& f, const VerifiedSequence& verified, Counts& counts) {
    for (unsigned i = 0; i < 2; ++i) {
        RejectedPrefix(f, i, false);
        ++counts.attempted;
        ++counts.failed;
        ++counts.abandoned;
        ++counts.abandoned_N;  // Actual accepted prefix LoadImm/NodeId checked above.
        counts.abandoned_words_from_verified_recipe += verified.rejected_prefixes[i].physical_words;
    }
}
Sample Run(const Fixture& f, std::size_t requests, Mode mode, const VerifiedSequence& verified) {
    Sample sample;
    // Only the explicit batch diagnostic allocates harness slots.
    std::vector<std::optional<Builder>> builders(mode == Mode::BatchDiagnostic ? requests : 0);
    std::vector<std::optional<Program>> programs(mode == Mode::BatchDiagnostic ? requests : 0);
    allocation::Begin();
    try {
        if (mode == Mode::Sequential) {
            const auto begin = Clock::now();
            for (std::size_t i = 0; i < requests; ++i) {
                allocation::counters.unit_floor = allocation::counters.live;
                allocation::phase = allocation::Construction;
                Builder builder(f.scope, f.target);
                Build(builder, f);
                allocation::phase = allocation::Submit;
                auto result = builder.Submit();
                Check(result.error, 0, 0);
                AccountPublished(result.program, f, verified, sample.counts);
                sample.storage_bytes = result.program.StorageBytes();
                sample.reserved_bytes = result.program.ReservedBytes();
                allocation::phase = allocation::Destruction;
            }  // Program AND consumed Builder destruction are in the primary timer.
            RunRejected(f, verified, sample.counts);
            sample.sequence = Ns(begin, Clock::now());
        } else {
            const auto a = Clock::now();
            for (auto& builder : builders) {
                allocation::counters.unit_floor = allocation::counters.live;
                builder.emplace(f.scope, f.target);
                Build(*builder, f);
            }
            const auto b = Clock::now();
            allocation::phase = allocation::Submit;
            for (std::size_t i = 0; i < requests; ++i) {
                auto result = builders[i]->Submit();
                Check(result.error, 0, 0);
                programs[i].emplace(std::move(result.program));
                AccountPublished(*programs[i], f, verified, sample.counts);
            }
            const auto c = Clock::now();
            // Every already-timed Program is checked outside diagnostic timers.
            for (const auto& program : programs)
                if (Validate(*program, f).checksum != verified.positive.checksum)
                    throw std::runtime_error("batch Program semantics differ");
            sample.storage_bytes = programs.front()->StorageBytes();
            sample.reserved_bytes = programs.front()->ReservedBytes();
            allocation::phase = allocation::Destruction;
            const auto d = Clock::now();
            for (auto& program : programs) program.reset();
            for (auto& builder : builders) builder.reset();
            const auto e = Clock::now();
            RunRejected(f, verified, sample.counts);
            const auto g = Clock::now();
            sample.construction = Ns(a, b);
            sample.submit = Ns(b, c);
            sample.destruction = Ns(d, e);
            sample.rejected = Ns(e, g);
        }
        if (allocation::counters.calls[allocation::Submit])
            throw std::runtime_error("Submit allocated");
        sample.allocations = allocation::counters;
        allocation::enabled = false;
        if (sample.allocations.live) throw std::runtime_error("core allocations remain live");
    } catch (...) {
        for (auto& program : programs) program.reset();
        for (auto& builder : builders) builder.reset();
        allocation::enabled = false;
        throw;
    }
    return sample;
}
struct Options {
    std::vector<std::size_t> nodes{1024, 16384, 262144};
    std::size_t blocks{32}, samples{9}, warmup{2}, requests{32};
    Mode mode{Mode::Sequential};
};
Options Parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--help") {
            std::cout << "ir_build_submit_bench [--nodes 1024,16384,262144] [--blocks 32] "
                         "[--samples 9] [--warmup 2] [--requests 32] [--mode "
                         "sequential|batch-diagnostic]\n";
            std::exit(0);
        }
        if (++i == argc) throw std::runtime_error("missing option value");
        const std::string value = argv[i];
        if (key == "--nodes") {
            options.nodes.clear();
            for (std::size_t first = 0; first < value.size();) {
                const auto end = value.find(',', first);
                options.nodes.push_back(std::stoull(value.substr(first, end - first)));
                if (end == std::string::npos) break;
                first = end + 1;
            }
        } else if (key == "--blocks")
            options.blocks = std::stoull(value);
        else if (key == "--samples")
            options.samples = std::stoull(value);
        else if (key == "--warmup")
            options.warmup = std::stoull(value);
        else if (key == "--requests")
            options.requests = std::stoull(value);
        else if (key == "--mode") {
            if (value == "sequential")
                options.mode = Mode::Sequential;
            else if (value == "batch-diagnostic")
                options.mode = Mode::BatchDiagnostic;
            else
                throw std::runtime_error("invalid mode");
        } else
            throw std::runtime_error("unknown option");
    }
    if (options.nodes.empty() || !options.blocks || options.blocks > max_blocks ||
        options.samples < 3 || options.samples > 1000 || options.warmup > 1000 ||
        !options.requests || options.requests > 4096)
        throw std::runtime_error("invalid benchmark bounds");
    for (const auto n : options.nodes)
        if (n < 2 * options.blocks || n > 10000000)
            throw std::runtime_error("nodes must be >= 2*blocks and <= 10000000");
    return options;
}
struct Case {
    Fixture fixture;
    VerifiedSequence verified;
    std::vector<Sample> samples;
};
void Numbers(const std::vector<double>& values) {
    std::cout << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) std::cout << ',';
        std::cout << values[i];
    }
    std::cout << ']';
}
void Stats(std::vector<double> values) {
    const auto raw = values;
    std::sort(values.begin(), values.end());
    const auto q = [&](double fraction) {
        return values[static_cast<std::size_t>(fraction * (values.size() - 1))];
    };
    std::cout << "{\"median\":" << q(.5) << ",\"p05\":" << q(.05) << ",\"p95\":" << q(.95)
              << ",\"min\":" << values.front() << ",\"max\":" << values.back() << ",\"samples\":";
    Numbers(raw);
    std::cout << '}';
}
void Print(const Options& options, const std::vector<Case>& cases, double timer_floor) {
#ifdef SVM_IR_STATIC_WRITER
    constexpr const char* writer = "compile_time_opcode";
#else
    constexpr const char* writer = "runtime_opcode";
#endif
#ifdef SVM_IR_BASELINE_LAYOUT
    constexpr const char* layout = "baseline_cell16";
#else
    constexpr const char* layout = "compact_word8";
#endif
    const bool sequential = options.mode == Mode::Sequential;
    std::cout << std::fixed << std::setprecision(3)
              << "{\"schema\":3,\"measurement_scope\":\"fixed_synthetic_request_sequence\","
                 "\"fixture\":\"mixed_flat_v1_with_two_rejected_prefixes\",\"application_"
                 "workload\":false,"
                 "\"synthetic_emission_not_real_decode\":true,\"full_compile_or_fex_comparison\":"
                 "false,"
                 "\"passes_analysis_ssa_cfg_ra_backend_run\":false,\"checksum_schema\":\"canonical_"
                 "semantic_v1\","
                 "\"semantic_verification\":\"complete_untimed_replay_of_same_fixed_sequence\","
                 "\"timed_programs_individually_checksummed\":"
              << (sequential ? "false" : "true")
              << ",\"timed_E_basis\":\"untimed_verified_fixed_recipe_not_timed_payload_traversal\","
                 "\"request_accounting_in_timing\":true,\"allocation_tracking_in_timing\":true,"
                 "\"abandoned_prefix_recovery_only_in_untimed_verification\":true,"
                 "\"mode\":\""
              << (sequential ? "sequential" : "batch-diagnostic")
              << "\",\"primary_latency_available\":" << (sequential ? "true" : "false")
              << ",\"primary_reservation\":\"growth\",\"physical_word_bytes\":8,\"layout\":\""
              << layout << "\",\"writer\":\"" << writer << "\",\"requests\":" << options.requests
              << ",\"warmup_sequences\":" << options.warmup
              << ",\"empty_timer_pair_ns_median\":" << timer_floor << ",\"cases\":[";
    for (std::size_t i = 0; i < cases.size(); ++i) {
        if (i) std::cout << ',';
        const auto& c = cases[i];
        const auto& f = c.fixture;
        const auto& first = c.samples.front();
        const auto& v = c.verified.positive;
        const auto& counts = first.counts;
        Counts totals;
        for (const auto& sample : c.samples) {
            totals.attempted += sample.counts.attempted;
            totals.published += sample.counts.published;
            totals.failed += sample.counts.failed;
            totals.abandoned += sample.counts.abandoned;
            totals.published_N += sample.counts.published_N;
            totals.published_words += sample.counts.published_words;
            totals.E_from_verified_recipe += sample.counts.E_from_verified_recipe;
            totals.abandoned_N += sample.counts.abandoned_N;
            totals.abandoned_words_from_verified_recipe +=
                    sample.counts.abandoned_words_from_verified_recipe;
        }
        const char* target = f.target.mode == TargetMode::Known      ? "known"
                             : f.target.mode == TargetMode::EarlyOff ? "early_off"
                                                                     : "unknown";
        std::cout << "{\"scope\":\"" << (f.scope == Scope::Block ? "block" : "function")
                  << "\",\"target_mode\":\"" << target << "\",\"reservation\":\""
                  << (f.reserve ? "exact" : "growth") << "\",\"positive_fixture_N\":" << f.nodes
                  << ",\"positive_fixture_E_verified\":" << v.values + v.terminals
                  << ",\"blocks_per_positive\":" << f.blocks
                  << ",\"input_operand_cells\":" << f.cells
                  << ",\"logical_cells_per_positive\":" << v.logical_cells
                  << ",\"physical_words_per_positive\":" << v.physical_words
                  << ",\"canonical_sequence_checksum\":\"" << c.verified.checksum
                  << "\",\"storage_bytes_per_positive\":" << first.storage_bytes
                  << ",\"reserved_bytes_per_positive\":" << first.reserved_bytes
                  << ",\"attempts_per_sequence\":" << counts.attempted
                  << ",\"published_per_sequence\":" << counts.published
                  << ",\"failed_per_sequence\":" << counts.failed
                  << ",\"abandoned_per_sequence\":" << counts.abandoned
                  << ",\"actual_published_N_per_sequence\":" << counts.published_N
                  << ",\"actual_published_words_per_sequence\":" << counts.published_words
                  << ",\"E_per_sequence_from_verified_recipe\":" << counts.E_from_verified_recipe
                  << ",\"actual_abandoned_prefix_N_per_sequence\":" << counts.abandoned_N
                  << ",\"abandoned_prefix_E_verified\":0,\"abandoned_prefix_words_from_verified_"
                     "recipe\":"
                  << counts.abandoned_words_from_verified_recipe
                  << ",\"measured_attempts_total\":" << totals.attempted
                  << ",\"measured_published_total\":" << totals.published
                  << ",\"measured_failed_total\":" << totals.failed
                  << ",\"measured_abandoned_total\":" << totals.abandoned
                  << ",\"measured_actual_published_N_total\":" << totals.published_N
                  << ",\"measured_actual_published_words_total\":" << totals.published_words
                  << ",\"measured_E_total_from_verified_recipe\":" << totals.E_from_verified_recipe
                  << ",\"measured_actual_abandoned_N_total\":" << totals.abandoned_N
                  << ",\"measured_abandoned_words_from_verified_recipe\":"
                  << totals.abandoned_words_from_verified_recipe
                  << ",\"warmup_attempts_total\":" << counts.attempted * options.warmup
                  << ",\"untimed_verification_attempts\":" << counts.attempted
                  << ",\"negative_requests\":[\"invalid_opcode_after_LoadImm\",\"foreign_value_"
                     "after_LoadImm\"]";
        std::vector<double> values;
        if (sequential) {
            for (const auto& s : c.samples) values.push_back(s.sequence);
            std::cout << ",\"sequence_build_submit_release_ns\":";
            Stats(values);
            for (auto& n : values) n /= counts.attempted;
            std::cout << ",\"ns_per_attempt_including_rejected_and_release\":";
            Stats(values);
        } else {
            for (unsigned phase = 0; phase < 4; ++phase) {
                values.clear();
                for (const auto& s : c.samples)
                    values.push_back(phase == 0   ? s.construction
                                     : phase == 1 ? s.submit
                                     : phase == 2 ? s.destruction
                                                  : s.rejected);
                const char* label = phase == 0   ? "construction"
                                    : phase == 1 ? "submit"
                                    : phase == 2 ? "destruction"
                                                 : "rejected_requests";
                std::cout << ",\"diagnostic_" << label << "_ns\":";
                Stats(values);
            }
        }
        for (unsigned phase = 0; phase < 3; ++phase) {
            values.clear();
            for (const auto& s : c.samples) values.push_back(s.allocations.calls[phase]);
            std::cout << ",\"allocation_calls_phase_" << phase << "_per_sequence\":";
            Stats(values);
            values.clear();
            for (const auto& s : c.samples) values.push_back(s.allocations.bytes[phase]);
            std::cout << ",\"requested_bytes_phase_" << phase << "_per_sequence\":";
            Stats(values);
        }
        values.clear();
        for (const auto& s : c.samples) values.push_back(s.allocations.peak);
        std::cout << ",\"peak_live_requested_bytes\":";
        Stats(values);
        std::cout << ",\"submit_zero_allocations_all_samples\":true,\"core_live_bytes_after_"
                     "destruction\":0}";
    }
    std::cout << "]}\n";
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = Parse(argc, argv);
        std::vector<double> timer_samples;
        for (unsigned i = 0; i < 101; ++i) {
            const auto a = Clock::now();
            const auto b = Clock::now();
            timer_samples.push_back(Ns(a, b));
        }
        std::sort(timer_samples.begin(), timer_samples.end());
        std::vector<Case> cases;
        for (const bool reserve : {false, true})  // cold capacity growth is primary
            for (const auto nodes : options.nodes)
                for (const auto scope : {Scope::Block, Scope::Function})
                    for (const auto mode :
                         {TargetMode::Known, TargetMode::EarlyOff, TargetMode::Unknown}) {
                        const auto fixture =
                                MakeFixture(nodes, options.blocks, scope, mode, reserve);
                        const auto verified = VerifySequence(fixture, options.requests);
                        Case c{fixture, verified, {}};
                        for (std::size_t i = 0; i < options.warmup; ++i)
                            Run(fixture, options.requests, options.mode, verified);
                        c.samples.reserve(options.samples);
                        for (std::size_t i = 0; i < options.samples; ++i)
                            c.samples.push_back(
                                    Run(fixture, options.requests, options.mode, verified));
                        cases.push_back(std::move(c));
                    }
        Print(options, cases, timer_samples[50]);
        return 0;
    } catch (const Failure& failure) {
        std::cerr << "IR fixture error=" << static_cast<unsigned>(failure.error)
                  << " block=" << failure.block << " local=" << failure.local << '\n';
    } catch (const std::exception& error) {
        std::cerr << "benchmark failed: " << error.what() << '\n';
    }
    return 1;
}
