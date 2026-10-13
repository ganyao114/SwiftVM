#include "sparse_homes_plan.h"

#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <type_traits>
#include "runtime/common/variant_util.h"

namespace swift::runtime::backend::loongarch64 {
namespace {
using O = ir::OpCode;
using R = SparseHomeReason;
using P = SparseHomePayload;

u32 Bytes(ir::ValueType type) {
    // GetValueSizeByte deliberately rejects VOID. BindLabel's self-reference
    // is metadata rather than a data value and has this return annotation.
    return type == ir::ValueType::VOID ? 0 : ir::GetValueSizeByte(type);
}

bool EarlyResult(ir::Inst* inst) {
    return inst->GetOp() == O::VecSatMulQC || inst->GetOp() == O::VecSatShiftQC ||
           inst->GetOp() == O::Div128Remainder || inst->GetOp() == O::CpuidUpper;
}

template<class F> void Operands(ir::Inst* inst, const F& visit) {
    // Operand's physical slots already contain its Value members. Lambda
    // addresses and Params lists need a separate walk, as in RegisterPlan.
    for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
        auto& arg = inst->ArgAt(slot);
        if (arg.IsValue()) visit(arg.Get<ir::Value>());
        else if (arg.IsLambda() && arg.Get<ir::Lambda>().IsValue()) visit(arg.Get<ir::Lambda>().GetValue());
        else if (arg.IsParams()) for (const auto& parameter : arg.Get<ir::Params>())
            if (parameter.data.IsValue()) visit(parameter.data.value);
    }
}

bool MemoryWidth(ir::Inst* inst, bool store) {
    // A memory Operand occupies three physical slots; StoreUniform's first
    // argument occupies one. GetArg uses public rather than physical indices.
    const u32 stored_slot = inst->GetOp() == O::StoreUniform ? 1 : 3;
    if (store && !inst->ArgAt(stored_slot).IsValue()) return false;
    const auto type = store ? inst->GetArg<ir::Value>(1).Type() : inst->ReturnType();
    const u32 bytes = Bytes(type);
    return bytes && bytes <= SparseHomesPlan::kStride;
}

void ValidateType(ir::ValueType type) {
    if (type >= ir::ValueType::V256)
        throw std::invalid_argument("LoongArch sparse homes require legalized, valid 128-bit IR");
}

P PayloadContract(ir::Inst* inst) {
    if (inst->ReturnType() == ir::ValueType::VOID) return P::Isolated;
    switch (inst->GetOp()) {
        // These write the whole raw container even when its visible return
        // type is U8/U32/U64. Width-based classification loses that contract.
        case O::BitCast: case O::GetResult: return P::Full128;
        case O::Select: case O::SelectZero: case O::CondSelect:
            return ir::IsFloatValueType(inst->ReturnType()) ? P::Full128 : P::ZeroHigh;
        case O::AddPhi: case O::Zero:
        case O::LoadUniform: case O::LoadMemory: case O::LoadMemoryTSO:
            return Bytes(inst->ReturnType()) <= sizeof(u64) ? P::ZeroHigh : P::Full128;
        // Native extracts write only low64. Typed WriteScalar may clear high
        // for a vector annotation, which still satisfies this zero contract.
        case O::VecExtract64: case O::VecExtract16: case O::VecMovMask:
        case O::VecFCmp: case O::VecFCvtIntToFloat: case O::VecFCvtFloatToInt: case O::VecFCvtScalar:
        case O::CallLambda: case O::CallLocation: case O::CallDynamic:
        case O::Div128: case O::Div128Remainder: case O::Cpuid: case O::CpuidUpper:
        case O::VecSatMulQC: case O::VecSatShiftQC: case O::Sse42Str: case O::X87Op:
        case O::GetLocation: case O::GetUniformAddress:
        case O::AtomicExchange: case O::AtomicFetchAdd: case O::AtomicRMW: case O::CompareAndSwap:
            return P::ZeroHigh;
        case O::CompareAndSwap128: return P::Full128;
        default: break;
    }
    // Positive audited emitter/helper lists. A new opcode stays isolated
    // until both its physical write width and hidden lifetime are checked.
    switch (inst->GetOp()) {
#define NATIVE(name) case O::name:
#include "scalar_native.inc"
#undef NATIVE
            return P::ZeroHigh;
        default: break;
    }
    switch (inst->GetOp()) {
#define NATIVE(name) case O::name:
#include "vector_native.inc"
#include "float_native.inc"
#undef NATIVE
        case O::VecDot: case O::VecSatMulHigh: case O::VecSatShiftImm: case O::VecUnsignedEstimate:
        case O::VecPack: case O::VecAbsDiffSum8: case O::VecMadd16:
        case O::VecAesEnc: case O::VecAesEncLast: case O::VecAesDec: case O::VecAesDecLast:
        case O::VecAesEncFast: case O::VecAesEncLastFast: case O::VecAesDecFast: case O::VecAesDecLastFast:
        case O::VecAesKeygenAssist: case O::VecPclMul:
        case O::VecSha256Msg1: case O::VecSha256Msg2: case O::VecSha256Rnds2:
        case O::VecFMinMax: case O::VecFUnary: case O::VecFCmpMask: case O::VecFCvtPacked:
        case O::VecFMulAdd: case O::VecFRoundInt:
            return P::Full128;
        default: return P::Isolated;
    }
}

struct ReuseBudgetExceeded {};
constexpr u32 kMaxReuseInstructions = 4096;
constexpr u32 kMaxReuseHomes = 4096;
constexpr u64 kMaxReusePairWork = 1u << 24;
constexpr u32 kMaxReuseEdges = 1u << 20;

}  // namespace

bool SparseHomesPlan::MayCallTypedHelper(ir::Inst* inst) {
    if (!inst) throw std::invalid_argument("LoongArch sparse helper classifier needs an instruction");
    switch (inst->GetOp()) {
        // These emitters have a return-width guard before their native path.
        case O::LoadImm: case O::GetOperand:
        case O::Add: case O::Sub: case O::Adc: case O::Sbb: case O::Mul:
        case O::MulSub: case O::MulHigh: case O::Div:
        case O::And: case O::Or: case O::Xor: case O::Not: case O::AndNot: case O::Neg:
        case O::LslImm: case O::LsrImm: case O::AsrImm: case O::RorImm:
        case O::LslValue: case O::LsrValue: case O::AsrValue: case O::RorValue:
            return !ScalarAllocationPlan::Eligible(inst);
        case O::Zero: case O::BitCast: case O::GetResult:
        case O::Select: case O::SelectZero: case O::CondSelect:
            return Bytes(inst->ReturnType()) > kStride;
        case O::LoadUniform: case O::LoadMemory: case O::LoadMemoryTSO:
            return !MemoryWidth(inst, false);
        case O::StoreUniform: case O::StoreMemory: case O::StoreMemoryTSO:
            return !MemoryWidth(inst, true);
        // Audited unconditional native scalar/control emitters. Their calls
        // to context.Read/Write use assigned registers or existing spills.
        case O::SignedDiv64: case O::ZeroExtend32: case O::ZeroExtend32To64:
        case O::ZeroExtend64: case O::SignExtend:
        case O::TestZero: case O::TestNotZero: case O::TestBit:
        case O::BitExtract: case O::BitInsert: case O::BitClear: case O::ByteSwap:
        case O::CountLeadingZeros64: case O::CountTrailingZeros64:
        case O::CountLeadingZeros32: case O::CountTrailingZeros32:
        case O::PopCount: case O::LocalParitySet: case O::FCmpCondSet:
        case O::Crc32: case O::Crc32c:
        case O::CondSet: case O::LocalCondSet: case O::GetFlags:
        case O::SaveFlags: case O::BranchOnlyFlags: case O::TestFlags: case O::TestNotFlags:
        case O::ClearFlags: case O::SetCarry: case O::SetOverflow: case O::InvertCarry:
        case O::PublishFCmpFlags: case O::PublishSse42StrFlags:
        case O::Goto: case O::NotGoto: case O::BindLabel: case O::AddPhi:
        case O::AdvancePC: case O::SetLocation: case O::GetLocation:
        case O::Nop: case O::UniformBarrier: case O::XchgBarrier: case O::Checkpoint:
        case O::PushRSB: case O::PopRSB: case O::CallReturn:
        case O::GetUniformAddress: case O::MemoryBarrierTSO: case O::CheckMemoryAlignment:
            return false;
        // All vector/FP paths are deliberately conservative, including the
        // baseline raw-pair implementations. Scalar atomics retain their
        // complete typed inputs/results for alignment and feature fallbacks.
        // New opcodes inherit the same fail-closed behavior.
        default: return true;
    }
}

std::optional<u16> SparseHomesPlan::HomeId(ir::Inst* inst) const {
    const auto found = homes.find(inst);
    return found == homes.end() ? std::nullopt : std::optional{found->second};
}

u32 SparseHomesPlan::HomeOffset(ir::Inst* inst, u32 part) const {
    if (part > 1) throw std::out_of_range("invalid LoongArch SSA home half");
    const auto id = HomeId(inst);
    if (!id) throw std::runtime_error("missing LoongArch SSA home");
    return u32(*id) * kStride + part * sizeof(u64);
}

u32 SparseHomesPlan::Reasons(ir::Inst* inst) const {
    const auto found = reasons.find(inst);
    return found == reasons.end() ? 0 : found->second;
}

std::optional<SparseHomePayload> SparseHomesPlan::Payload(ir::Inst* inst) const {
    const auto found = payloads.find(inst);
    return found == payloads.end() ? std::nullopt : std::optional{found->second};
}

SparseHomesPlan SparseHomesPlan::Build(ir::Block* block, const ScalarAllocationPlan& allocation,
                                      const RegisterPlan::Exits& exits, const ir::Terminal* terminal,
                                      bool reuse, const RegisterPlan::Targets& targets,
                                      const RegisterPlan::Stops& stops, std::span<const WideVectorPair> wide) {
    if (!block) throw std::invalid_argument("LoongArch sparse homes need an IR block");
    SparseHomesPlan result;
    std::vector<ir::Inst*> instructions;
    LiveSet members;
    for (auto& inst : block->GetInstList()) {
        ValidateType(inst.ReturnType());
        instructions.push_back(&inst); members.insert(&inst);
    }
    const auto member = [&](ir::Inst* inst) {
        if (!inst || !members.contains(inst))
            throw std::invalid_argument("foreign LoongArch sparse-home instruction");
    };
    const auto require = [&](ir::Inst* inst, R reason) {
        member(inst); result.required.insert(inst); result.reasons[inst] |= u32(reason);
    };
    const auto value = [&](ir::Value input) {
        member(input.Def()); ValidateType(input.Type());
        // Wide views of scalar carriers still use their second home half.
        if (Bytes(input.Type()) > sizeof(u64)) require(input.Def(), R::RawContainer);
    };
    for (auto* inst : instructions) {
        Operands(inst, value);
        for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
            auto& arg = inst->ArgAt(slot);
            if (arg.GetType() == ir::ArgType::Uniform) ValidateType(arg.Get<ir::Uniform>().GetType());
            else if (arg.GetType() == ir::ArgType::Local) ValidateType(arg.Get<ir::Local>().type);
            else if (arg.IsImm()) ValidateType(arg.Get<ir::Imm>().GetType());
        }
        if (allocation.Spilled(inst)) require(inst, R::Spill);
        if (allocation.NeedsSplitHome(inst)) require(inst, R::Split);
        if (ir::IsFloatValueType(inst->ReturnType())) require(inst, R::Vector);
        if (inst->GetOp() == O::AddPhi) require(inst, R::PhiDestination);
        if (EarlyResult(inst)) require(inst, R::ProducerPseudo);
        // Foreign pseudos must be rejected even if the lexical marker is
        // missing: typed producers can write their results immediately.
        if (!inst->IsPseudoOperation())
            for (auto* pseudo : inst->GetPseudoOperations()) member(pseudo);
        if (MayCallTypedHelper(inst)) {
            // Keep VOID ownhomes too. A typed helper's implementation may
            // write a raw result/pseudo independent of its return annotation.
            require(inst, R::HelperInstruction);
            Operands(inst, [&](ir::Value input) { require(input.Def(), R::HelperInput); });
            if (!inst->IsPseudoOperation())
                for (auto* pseudo : inst->GetPseudoOperations()) require(pseudo, R::ProducerPseudo);
        }
        if (inst->GetOp() == O::BitCast || inst->GetOp() == O::GetResult) {
            // CopyValue always preserves both halves, even with U8 return.
            require(inst, R::RawContainer);
            require(inst->GetArg<ir::Value>(0).Def(), R::RawContainer);
        }
        if ((inst->GetOp() == O::Select || inst->GetOp() == O::SelectZero || inst->GetOp() == O::CondSelect) &&
            ir::IsFloatValueType(inst->ReturnType())) {
            require(inst, R::RawContainer);
            require(inst->GetArg<ir::Value>(1).Def(), R::RawContainer);
            require(inst->GetArg<ir::Value>(2).Def(), R::RawContainer);
        }
    }
    // Terminals consume typed scalar registers directly. Visit all nested
    // exits to validate foreign/wide operands, without forcing register-only
    // predicates or switch values to acquire a home.
    const auto terminal_values = [&](const auto& recurse, const ir::Terminal& exit) -> void {
        VisitVariant<void>(exit, [&](const auto& term) {
            using T = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<T, ir::terminal::If>) {
                value(term.cond); recurse(recurse, term.then_); recurse(recurse, term.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
                recurse(recurse, term.then_); recurse(recurse, term.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
                value(term.value);
                for (const auto& item : term.cases) recurse(recurse, item.then);
            } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) recurse(recurse, term.else_);
        });
    };
    terminal_values(terminal_values, terminal ? *terminal : block->GetTerminal());
    for (const auto& [at, exit] : exits) { member(at); terminal_values(terminal_values, exit); }

    // UINT16_MAX remains an invalid/sentinel ID. Exactly 65535 homes fit,
    // with valid IDs [0,65534]; a 65536th home must reject compilation.
    if (result.required.size() > ir::Inst::invalid_id)
        throw std::overflow_error("too many LoongArch sparse SSA homes");
    std::vector<ir::Inst*> values;
    std::unordered_map<ir::Inst*, u32> indices, positions;
    for (u32 i = 0; i < instructions.size(); ++i) positions.emplace(instructions[i], i);
    for (auto* inst : instructions) if (result.required.contains(inst)) {
        indices.emplace(inst, values.size()); values.push_back(inst);
        const auto payload = PayloadContract(inst);
        result.payloads.emplace(inst, payload);
        result.stats.zero_high_homes += payload == P::ZeroHigh;
        result.stats.full_payload_homes += payload == P::Full128;
        result.stats.isolated_homes += payload == P::Isolated;
        const auto why = result.reasons.at(inst);
        result.stats.spill_homes += bool(why & u32(R::Spill));
        result.stats.vector_homes += bool(why & u32(R::Vector));
        result.stats.helper_homes += bool(why & (u32(R::HelperInstruction) | u32(R::HelperInput)));
        result.stats.pseudo_homes += bool(why & u32(R::ProducerPseudo));
        result.stats.raw_homes += bool(why & u32(R::RawContainer));
        result.stats.phi_homes += bool(why & u32(R::PhiDestination));
        result.stats.split_homes += bool(why & u32(R::Split));
    }
    result.stats.instructions = instructions.size();
    result.stats.required_homes = values.size();
    result.stats.reuse_requested = reuse;
    result.stats.dense_bytes = u64(std::max(instructions.size(), size_t{1})) * kStride;
    result.stats.unique_bytes = u64(std::max(values.size(), size_t{1})) * kStride;
    // The legacy inventory keeps IDs [0,65534]. Reuse/fallback reserves 0 for
    // recovery; neither a missing home nor invalid_id may name that slot.
    const auto unique = [&] {
        const u32 first = reuse ? 1 : 0;
        if (u64(first) + values.size() > ir::Inst::invalid_id)
            throw std::overflow_error("too many LoongArch non-sentinel SSA homes");
        result.homes.clear();
        u32 next = first;
        for (auto* inst : values) result.homes.emplace(inst, static_cast<u16>(next++));
        result.stats.home_count = std::max(next, u32{1});
    };
    if (!reuse) unique();
    else {
        // Validate coordinates even if no home is required or analysis takes
        // its bounded unique-slot fallback. Foreign metadata cannot silently
        // change the liveness graph.
        for (const auto& [from, to] : targets) {
            member(from); member(to);
            if (from->GetOp() != O::Goto && from->GetOp() != O::NotGoto)
                throw std::invalid_argument("LoongArch home target needs a branch source");
        }
        for (auto* at : stops) member(at);
        for (const auto& pair : wide) {
            for (auto* inst : {pair.low, pair.high, pair.left[0], pair.left[1], pair.right[0], pair.right[1]}) {
                member(inst);
                if (!indices.contains(inst) || Bytes(inst->ReturnType()) != kStride)
                    throw std::invalid_argument("LoongArch home pairing needs 128-bit container homes");
            }
            if (pair.low == pair.high || pair.left[0] == pair.left[1] || pair.right[0] == pair.right[1])
                throw std::invalid_argument("LoongArch home pairing needs distinct halves");
        }
        try {
            if (instructions.size() > kMaxReuseInstructions || values.size() > kMaxReuseHomes ||
                u64(instructions.size()) * values.size() > kMaxReusePairWork)
                throw ReuseBudgetExceeded{};
            const auto live = RegisterPlan::Build(block, targets, stops, {}, exits, terminal);
            result.stats.cfg_nodes = live.Stats().nodes;
            const u32 count = values.size();
            std::vector<std::unordered_set<u32>> graph(count);
            std::vector<u32> parent(count);
            std::iota(parent.begin(), parent.end(), 0);
            const auto root = [&](u32 item) {
                while (parent[item] != item) { parent[item] = parent[parent[item]]; item = parent[item]; }
                return item;
            };
            const auto pair_halves = [&](ir::Inst* low, ir::Inst* high) {
                parent[root(indices.at(high))] = root(indices.at(low));
            };
            for (const auto& pair : wide) {
                pair_halves(pair.low, pair.high);
                pair_halves(pair.left[0], pair.left[1]); pair_halves(pair.right[0], pair.right[1]);
            }
            std::vector<std::vector<u32>> groups(count);
            for (u32 i = 0; i < count; ++i) groups[root(i)].push_back(i);
            for (u32 i = 0; i < count; ++i)
                result.stats.paired_homes += groups[root(i)].size() > 1;
            u64 work{};
            u32 edge_count{};
            const auto clique = [&](LiveSet set) {
                std::unordered_set<u32> present;
                for (auto* inst : set) if (auto found = indices.find(inst); found != indices.end())
                    for (const auto index : groups[root(found->second)]) present.insert(index);
                std::vector<u32> simultaneous(present.begin(), present.end());
                work += u64(simultaneous.size()) * (simultaneous.size() ? simultaneous.size() - 1 : 0) / 2;
                if (work > kMaxReusePairWork) throw ReuseBudgetExceeded{};
                for (u32 i = 0; i < simultaneous.size(); ++i) for (u32 j = i + 1; j < simultaneous.size(); ++j) {
                    const u32 a = simultaneous[i], b = simultaneous[j];
                    const auto payload = result.payloads.at(values[a]);
                    // Separate payload pools already prohibit these aliases.
                    if (payload == P::Isolated || payload != result.payloads.at(values[b])) continue;
                    if (graph[a].insert(b).second) {
                        graph[b].insert(a);
                        if (++edge_count > kMaxReuseEdges) throw ReuseBudgetExceeded{};
                    }
                }
            };
            std::vector<LiveSet> definitions(instructions.size());
            for (u32 i = 0; i < instructions.size(); ++i) {
                auto* inst = instructions[i];
                if (EarlyResult(inst)) definitions[positions.at(inst->GetArg<ir::Value>(0).Def())].insert(inst);
                else if (inst->GetOp() != O::AddPhi && result.required.contains(inst)) definitions[i].insert(inst);
                if (MayCallTypedHelper(inst) && !inst->IsPseudoOperation())
                    for (auto* pseudo : inst->GetPseudoOperations()) definitions[i].insert(pseudo);
            }
            for (const auto& pair : wide) definitions[positions.at(pair.low)].insert(pair.high);

            // Derive only CFG coordinates and simultaneous phi writes here;
            // the actual fixed-point liveness is the all-type RegisterPlan.
            const u32 instruction_count = instructions.size();
            std::vector<bool> leaders(instruction_count + 1);
            leaders.front() = leaders.back() = true;
            std::unordered_map<ir::Inst*, u32> bound;
            for (u32 i = 0; i < instruction_count; ++i) {
                auto* inst = instructions[i];
                if (inst->GetOp() == O::BindLabel) {
                    leaders[i] = true; bound.emplace(inst->GetArg<ir::Value>(0).Def(), i);
                }
                if (inst->GetOp() == O::Goto || inst->GetOp() == O::NotGoto || stops.contains(inst) || exits.contains(inst))
                    leaders[i + 1] = true;
            }
            std::vector<u32> starts, node_at(instruction_count);
            for (u32 i = 0; i < instruction_count; ++i) {
                if (leaders[i]) starts.push_back(i);
                node_at[i] = starts.size() - 1;
            }
            const u32 nodes = starts.size(); starts.push_back(instruction_count);
            std::vector<LiveSet> phis(nodes);
            for (u32 b = 0; b < nodes; ++b) {
                u32 i = starts[b];
                if (instructions[i]->GetOp() == O::BindLabel) ++i;
                while (i < starts[b + 1] && instructions[i]->GetOp() == O::AddPhi)
                    phis[b].insert(instructions[i++]);
                definitions[starts[b]].insert(phis[b].begin(), phis[b].end());
            }
            for (u32 i = 0; i < instruction_count; ++i) {
                auto* inst = instructions[i];
                LiveSet simultaneous = live.At(inst);
                simultaneous.insert(live.After(inst).begin(), live.After(inst).end());
                // Dead definitions still write their home and must not
                // destroy another value waiting for a later read.
                simultaneous.insert(definitions[i].begin(), definitions[i].end());
                for (const auto& move : allocation.MovesBefore(inst)) {
                    member(move.value);
                    if (!result.required.contains(move.value))
                        throw std::runtime_error("LoongArch split transfer lacks a staging home");
                    simultaneous.insert(move.value);
                }
                clique(std::move(simultaneous));
            }
            const auto edge_clique = [&](u32 to, const LiveSet& source) {
                LiveSet simultaneous = source;
                simultaneous.insert(phis[to].begin(), phis[to].end());
                // Sources, retained successor values, and every destination
                // remain distinct throughout edge publication/parallel copy.
                clique(std::move(simultaneous));
            };
            for (u32 b = 0; b < nodes; ++b) {
                auto* last = instructions[starts[b + 1] - 1];
                if (b + 1 < nodes && !stops.contains(last) && !exits.contains(last))
                    edge_clique(b + 1, live.ForFallthrough(last));
                if (last->GetOp() == O::Goto || last->GetOp() == O::NotGoto) {
                    auto* anchor = targets.contains(last) ? targets.at(last) : last;
                    const u32 to = bound.contains(anchor) ? bound.at(anchor) : positions.at(anchor);
                    edge_clique(node_at[to], live.ForBranch(last));
                }
            }

            std::vector<u32> order(count), colors(count);
            std::iota(order.begin(), order.end(), 0);
            std::sort(order.begin(), order.end(), [&](u32 left, u32 right) {
                if (graph[left].size() != graph[right].size()) return graph[left].size() > graph[right].size();
                return left < right;
            });
            std::array<std::vector<u32>, 2> pools;
            u32 next = 1;
            const auto new_slot = [&] {
                if (next == ir::Inst::invalid_id) throw std::overflow_error("too many reused LoongArch SSA homes");
                return next++;
            };
            for (const auto index : order) {
                const auto payload = result.payloads.at(values[index]);
                if (payload == P::Isolated) { colors[index] = new_slot(); continue; }
                auto& pool = pools[payload == P::ZeroHigh ? 0 : 1];
                std::unordered_set<u32> forbidden;
                for (const auto other : graph[index]) if (colors[other]) forbidden.insert(colors[other]);
                const auto available = std::find_if(pool.begin(), pool.end(), [&](u32 slot) { return !forbidden.contains(slot); });
                if (available != pool.end()) colors[index] = *available;
                else { colors[index] = new_slot(); pool.push_back(colors[index]); }
            }
            for (u32 i = 0; i < count; ++i) result.homes.emplace(values[i], static_cast<u16>(colors[i]));
            result.stats.home_count = next;
            result.stats.reused_homes = count - (next - 1);
            result.stats.interference_edges = edge_count;
            result.stats.reuse_active = true;
        } catch (const ReuseBudgetExceeded&) {
            result.stats.budget_rejected = true;
            result.stats.paired_homes = 0;
            unique();
        }
    }
    result.stats.compact_bytes = u64(result.stats.home_count) * kStride;
    result.stats.saved_bytes = s64(result.stats.unique_bytes) - s64(result.stats.compact_bytes);
    return result;
}

}  // namespace swift::runtime::backend::loongarch64
