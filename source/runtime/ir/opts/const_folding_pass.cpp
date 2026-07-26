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
// and the shift decoders), so a constant defined before a forward branch has
// NOT necessarily executed when control reaches the label.  Those three opcodes
// therefore reset the table, exactly as they do in UniformEliminationPass.
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

// IR instructions; roughly one guest instruction's worth of expansion.
constexpr u32 kReuseWindow = 8;

void DedupConstants(Block* block) {
    // Bisect switch, mirroring SVM_UNIFORM_DSE.
    static const bool off = [] {
        const char* e = std::getenv("SVM_CONST_CSE");
        return e && std::strcmp(e, "0") == 0;
    }();
    if (off) return;
    struct Entry {
        Value value;
        u32 index;
    };
    std::unordered_map<ImmKey, Entry, ImmKeyHash> canonical;
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
        if (inst.GetOp() != OpCode::LoadImm) {
            continue;
        }
        const auto imm = inst.GetArg<Imm>(0);
        // The return type matters: both back ends size the destination register
        // from it, so a U32 and a U64 LoadImm of the same bits are different
        // values.
        const ImmKey key{imm.Get(), static_cast<u8>(inst.ReturnType())};
        auto [it, inserted] = canonical.try_emplace(key, Entry{Value{&inst}, index});
        if (!inserted) {
            if (index - it->second.index <= kReuseWindow) {
                replacement.emplace(&inst, it->second.value);
            } else {
                // Too far to reuse: this definition becomes the new anchor.
                it->second = Entry{Value{&inst}, index};
            }
        }
    }
}

}  // namespace

void ConstFoldingPass::Run(HIRBuilder* hir_builder) {
    for (auto &hir_func : hir_builder->GetHIRFunctions()) {
        Run(&hir_func);
    }
}

void ConstFoldingPass::Run(HIRFunction* hir_function) {
    for (auto& hir_block : hir_function->GetHIRBlocksRPO()) {
        DedupConstants(hir_block.GetBlock());
    }
}

void ConstFoldingPass::Run(Block* block) { DedupConstants(block); }

}
