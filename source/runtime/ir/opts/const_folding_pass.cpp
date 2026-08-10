//
// Created by 甘尧 on 2023/12/6.
//

#include "const_folding_pass.h"

#include <cstring>
#include <unordered_map>

namespace swift::runtime::ir {

namespace {

// Block-local common subexpression elimination for constant materialization.
//
// LoadImm is the single most numerous IR opcode the x86 front end produces:
// measured over the 25 e2e guests plus the five bench kernels it was 15.2% of
// all emitted IR and 11.1% of all emitted host bytes, at ~1.1 arm64
// instructions each.  Many are duplicates.  Every conditional branch, SETcc and
// CMOVcc goes through CheckCond, which materializes a fresh LoadImm(1) /
// LoadImm(0) pair for its CondSelect; every flag-setting arithmetic instruction
// materializes a fresh carry-polarity byte; and an immediate reused inside a
// block is re-materialized per use.
//
// Rewriting a use to an earlier identical constant is unconditionally valid for
// a pure value, with one exception that is easy to get wrong: the x86 front end
// builds intra-block control flow (Goto / NotGoto / BindLabel -- see CondGoto
// and the shift decoders), so a constant defined between a forward branch and
// its label has NOT necessarily executed when control reaches the label.  Those
// three opcodes therefore reset the table, exactly as they do in
// UniformEliminationPass.
//
// Of the three, BindLabel is the one carrying the correctness: these labels are
// forward-only (the label value is produced by the branch and bound after it),
// so code before a branch dominates everything after it and only the merge at
// the label can bring in a definition that did not run.  The resets at Goto /
// NotGoto are redundant with that and are kept for the same reason
// UniformEliminationPass keeps them -- to scope facts to one straight-line
// region -- not because a case is known to need them.
//
// The BindLabel reset is NOT hypothetical.  Instrumenting the pass with a
// second, barrier-free table and comparing the two shows it suppressing 13
// in-window reuses per func_tests_x86_64 translation and 12 per
// real_busy_x86_64, every one of them with the anchor materialized at a
// greater open-branch depth than the use -- i.e. exactly the unsafe shape
// (DecodeShift in decoder_alu.cc materializes LoadImm(0) for SAR's OF inside a
// NotGoto/BindLabel region a few instructions before the label).
//
// What that instrumentation ALSO showed is why this needs a unit test rather
// than trust in the corpus: with the BindLabel reset deleted, all 25 guest e2e
// exit codes are unchanged, func_tests' output stays byte-identical, and the
// whole swift_test suite -- 161k assertions including the Unicorn differential
// fuzz -- stays green.  The suppressed reuses are real but land on flag bits
// nothing goes on to read.  "Constant CSE does not reuse a constant
// materialized under a branch" (source/tests/main_case.cpp) is therefore the
// only thing that pins this reset, and it carries a straight-line positive
// control so it cannot pass by the pass simply doing nothing.
//
// The orphaned duplicate is left for DeadCodeEliminationPass, which runs last
// in the pipeline and already iterates to a fixpoint.
//
// The reuse window is NOT cosmetic.  Deduplicating a constant merges two short
// live ranges into one long one, and doing that block-wide is a real
// regression: an unbounded version of this pass made func_tests_x86_64 abort
// with "spill area exhausted: slot 64 (+1) >= 64 reserved slots".  The window
// keeps the cases that motivated the pass -- CheckCond materializes its 1/0
// pair two instructions apart, an immediate operand and its reuse sit inside
// one guest instruction's expansion -- while bounding how far any range grows.
struct ImmKey {
    u64 bits;
    u8 type;

    bool operator==(const ImmKey& rhs) const { return bits == rhs.bits && type == rhs.type; }
};

struct ImmKeyHash {
    std::size_t operator()(const ImmKey& k) const {
        return std::hash<u64>{}(k.bits) ^ (std::size_t(k.type) * 0x9E3779B9u);
    }
};

struct ScalarImmEntry {
    Value value;
    u32 index;
};

struct VecKey {
    u64 low;
    u64 high;

    bool operator==(const VecKey& rhs) const {
        return low == rhs.low && high == rhs.high;
    }
};

struct VecKeyHash {
    std::size_t operator()(const VecKey& key) const {
        return std::hash<u64>{}(key.low) ^
               (std::hash<u64>{}(key.high) + 0x9E3779B97F4A7C15ull);
    }
};

// IR instructions; roughly one guest instruction's worth of expansion.
constexpr u32 kReuseWindow = 8;
// The forensic pool was pre-registered with a five-IR producer/consumer
// window.  Keep the mechanism inside that audited live-range envelope.
constexpr u32 kIntegerImmediateUseWindow = 5;

u64 WidthMask(u32 bytes) {
    return bytes >= sizeof(u64) ? UINT64_MAX : ((u64{1} << (bytes * 8)) - 1);
}

bool IsAddSubImmediate(s64 value) {
    if (value < 0) return false;
    const auto immediate = static_cast<u64>(value);
    return immediate <= 0xfff ||
           ((immediate & 0xfff) == 0 && (immediate >> 12) <= 0xfff);
}

u64 RotateRightWidth(u64 value, u32 amount, u32 width) {
    const u64 mask = width == 64 ? UINT64_MAX : ((u64{1} << width) - 1);
    value &= mask;
    amount %= width;
    if (amount == 0) return value;
    return ((value >> amount) | (value << (width - amount))) & mask;
}

// Exact A64 logical-immediate language: a non-zero/non-all-ones run of ones,
// rotated within a power-of-two element and replicated to W/X width.
bool IsLogicalImmediate(u64 value, u32 width) {
    const u64 width_mask = width == 64 ? UINT64_MAX : UINT32_MAX;
    value &= width_mask;
    if (value == 0 || value == width_mask) return false;
    for (u32 element_bits = 2; element_bits <= width; element_bits <<= 1) {
        const u64 element_mask = element_bits == 64
                ? UINT64_MAX
                : ((u64{1} << element_bits) - 1);
        const u64 element = value & element_mask;
        u64 replicated{};
        for (u32 offset = 0; offset < width; offset += element_bits) {
            replicated |= element << offset;
        }
        if (replicated != value) continue;
        for (u32 ones = 1; ones < element_bits; ++ones) {
            const u64 run = ones == 64 ? UINT64_MAX : ((u64{1} << ones) - 1);
            for (u32 rotation = 0; rotation < element_bits; ++rotation) {
                if (RotateRightWidth(run, rotation, element_bits) == element) {
                    return true;
                }
            }
        }
    }
    return false;
}

std::optional<Imm> AddSubImmediate(const Imm& source, ValueType type) {
    const u32 bytes = GetValueSizeByte(type);
    if (bytes == 0 || bytes > sizeof(u64) || IsFloatValueType(type)) {
        return std::nullopt;
    }
    const u32 reg_bits = bytes <= sizeof(u32) ? 32 : 64;
    const u64 raw = source.Get() & WidthMask(bytes);
    const u64 widened = raw & (reg_bits == 32 ? UINT32_MAX : UINT64_MAX);
    if (IsAddSubImmediate(static_cast<s64>(widened))) {
        return reg_bits == 32 ? Imm{static_cast<u32>(widened)} : Imm{widened};
    }
    const s64 signed_value = reg_bits == 32
            ? static_cast<s64>(static_cast<s32>(static_cast<u32>(widened)))
            : static_cast<s64>(widened);
    if (signed_value < 0 && signed_value != INT64_MIN &&
        IsAddSubImmediate(-signed_value)) {
        return reg_bits == 32 ? Imm{static_cast<s32>(signed_value)}
                              : Imm{signed_value};
    }
    return std::nullopt;
}

std::optional<Imm> LogicalImmediate(const Imm& source, ValueType type) {
    const u32 bytes = GetValueSizeByte(type);
    if (bytes == 0 || bytes > sizeof(u64) || IsFloatValueType(type)) {
        return std::nullopt;
    }
    const u32 reg_bits = bytes <= sizeof(u32) ? 32 : 64;
    const u64 raw = source.Get() & WidthMask(bytes);
    const u64 widened = raw & (reg_bits == 32 ? UINT32_MAX : UINT64_MAX);
    if (!IsLogicalImmediate(widened, reg_bits)) {
        return std::nullopt;
    }
    return reg_bits == 32 ? Imm{static_cast<u32>(widened)} : Imm{widened};
}

Flags RequestedFlags(const Inst& inst) {
    Flags flags{};
    for (auto& pseudo : const_cast<Inst&>(inst).GetPseudoOperations()) {
        if (pseudo->GetOp() == OpCode::SaveFlags ||
            pseudo->GetOp() == OpCode::BranchOnlyFlags) {
            flags |= pseudo->GetArg<Flags>(1);
        }
    }
    return flags;
}

// Canonicalize a constant directly into the sole ALU consumer before constant
// CSE has a chance to merge several short LoadImm ranges into one multi-use
// range.  The following DCE pass removes the now-orphaned materialization.
void FoldIntegerImmediates(Block* block, const FeatureSet& features) {
    if (!features.int_imm_fold) return;

    std::unordered_map<Inst*, u32> seen;
    u32 index{};
    for (auto& inst : block->GetInstList()) {
        seen.emplace(&inst, index++);
        const auto op = inst.GetOp();
        if (op != OpCode::Add && op != OpCode::Sub && op != OpCode::And &&
            op != OpCode::Or && op != OpCode::Xor) {
            continue;
        }

        // `0 - x` has its constant on the left, which Operand cannot express.
        // Give that exact shape a unary canonical form; flags pseudos remain
        // attached to the producer instruction.
        if (op == OpCode::Sub) {
            const auto left = inst.GetArg<Value>(0);
            const auto right = inst.GetArg<Operand>(1);
            auto* zero = left.Def();
            const auto zero_pos = zero ? seen.find(zero) : seen.end();
            if (zero_pos != seen.end() &&
                index - 1 - zero_pos->second <= kIntegerImmediateUseWindow &&
                zero->GetOp() == OpCode::LoadImm &&
                zero->GetArg<Imm>(0).Get() == 0 && zero->GetUses(false) == 1 &&
                right.GetRight().Null() && right.GetLeft().IsValue() &&
                right.GetOp() == OperandPlus) {
                const auto source = right.GetLeft().value;
                const auto type = inst.ReturnType();
                inst.DestroyArgs();
                inst.SetInst(OpCode::Neg, source);
                inst.SetReturn(type);
                continue;
            }
        }

        const auto operand = inst.GetArg<Operand>(1);
        if (!operand.GetRight().Null() || !operand.GetLeft().IsValue() ||
            operand.GetOp() != OperandPlus) {
            continue;
        }
        auto* load = operand.GetLeft().value.Def();
        const auto load_pos = load ? seen.find(load) : seen.end();
        if (load_pos == seen.end() ||
            index - 1 - load_pos->second > kIntegerImmediateUseWindow ||
            load->GetOp() != OpCode::LoadImm ||
            load->GetUses(false) != 1) {
            continue;
        }

        // Narrow Add/Sub NZCV uses the mandatory sign-alignment path, which
        // still materializes an immediate.  Folding it has no host-code win.
        const u32 bytes = GetValueSizeByte(inst.ReturnType());
        if ((op == OpCode::Add || op == OpCode::Sub) && bytes <= 2 &&
            True(RequestedFlags(inst) & Flags::NZCV)) {
            continue;
        }

        const auto source = load->GetArg<Imm>(0);
        const auto folded = (op == OpCode::Add || op == OpCode::Sub)
                ? AddSubImmediate(source, inst.ReturnType())
                : LogicalImmediate(source, inst.ReturnType());
        if (folded) {
            inst.SetArg(1, Operand{*folded});
        }
    }
}

void DedupConstants(Block* block, const FeatureSet& features) {
    // Bisect switch, mirroring SVM_UNIFORM_DSE.
    const bool imm_off = !features.const_cse;
    std::unordered_map<ImmKey, ScalarImmEntry, ImmKeyHash> canonical;
    // These two constants exist specifically for the vector lowering gates.
    // Unlike scalar LoadImm, their reuse is block-wide: one 16-byte table or
    // one zero vector is worth keeping live across the GHASH/AES straight-line
    // body.  Control-flow pseudo labels clear them below, so a definition is
    // never reused from a path which may not have executed.
    std::unordered_map<VecKey, Value, VecKeyHash> vec_canonical;
    Value zero_canonical{};
    // duplicate LoadImm def -> the value that replaces it
    std::unordered_map<Inst*, Value> replacement;
    u32 index = 0;

    for (auto& inst : block->GetInstList()) {
        ++index;
        switch (inst.GetOp()) {
            case OpCode::Goto:
            case OpCode::NotGoto:
            case OpCode::BindLabel:
                // A constant defined on one side of an intra-block branch is
                // not available on the other.  Drop every fact; instructions
                // already rewritten stay rewritten -- they were rewritten to a
                // definition that dominated them.
                canonical.clear();
                vec_canonical.clear();
                zero_canonical = {};
                continue;
            default:
                break;
        }
        // Pseudo operations (SaveFlags / ClearFlags / GetResult ...) name their
        // producer through arg 0 and are linked to it by Inst::next_pseudo_inst.
        // Retargeting that argument would break the pairing, and none of them
        // reads a constant anyway.
        if (!inst.IsPseudoOperation()) {
            for (int i = 0; i < Inst::max_args; ++i) {
                auto& arg = inst.ArgAt(i);
                if (arg.IsValue()) {
                    auto def = arg.Get<Value>().Def();
                    if (!def) continue;
                    if (auto it = replacement.find(def); it != replacement.end()) {
                        inst.SetArg(i, it->second);
                    }
                } else if (arg.IsLambda()) {
                    auto lambda = arg.Get<Lambda>();
                    if (!lambda.IsValue()) continue;
                    auto def = lambda.GetValue().Def();
                    if (!def) continue;
                    if (auto it = replacement.find(def); it != replacement.end()) {
                        inst.SetArg(i, Lambda{it->second});
                    }
                }
            }
        }
        if (inst.GetOp() == OpCode::VecLoadConst) {
            const VecKey key{
                    inst.GetArg<Imm>(0).Get(),
                    inst.GetArg<Imm>(1).Get(),
            };
            auto [it, inserted] =
                    vec_canonical.try_emplace(key, Value{&inst});
            if (!inserted) {
                replacement.emplace(&inst, it->second);
            }
            continue;
        }
        if (inst.GetOp() == OpCode::VecSharedZero) {
            if (zero_canonical.Defined()) {
                replacement.emplace(&inst, zero_canonical);
            } else {
                zero_canonical = Value{&inst};
            }
            continue;
        }
        if (inst.GetOp() != OpCode::LoadImm || imm_off) {
            continue;
        }
        const auto imm = inst.GetArg<Imm>(0);
        // The return type matters: both back ends size the destination register
        // from it, so a U32 and a U64 LoadImm of the same bits are different
        // values.
        const ImmKey key{imm.Get(), static_cast<u8>(inst.ReturnType())};
        auto [it, inserted] =
                canonical.try_emplace(key, ScalarImmEntry{Value{&inst}, index});
        if (!inserted) {
            if (index - it->second.index <= kReuseWindow) {
                replacement.emplace(&inst, it->second.value);
            } else {
                // Too far to reuse: this definition becomes the new anchor.
                it->second = ScalarImmEntry{Value{&inst}, index};
            }
        }
    }
}

}  // namespace

void ConstFoldingPass::Run(HIRBuilder* hir_builder, const FeatureSet& features) {
    for (auto &hir_func : hir_builder->GetHIRFunctions()) {
        Run(&hir_func, features);
    }
}

void ConstFoldingPass::Run(HIRFunction* hir_function, const FeatureSet& features) {
    for (auto& hir_block : hir_function->GetHIRBlocksRPO()) {
        FoldIntegerImmediates(hir_block.GetBlock(), features);
        DedupConstants(hir_block.GetBlock(), features);
    }
}

void ConstFoldingPass::Run(Block* block, const FeatureSet& features) {
    FoldIntegerImmediates(block, features);
    DedupConstants(block, features);
}

}
