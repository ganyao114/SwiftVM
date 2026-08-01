//
// Created by 甘尧 on 2024/6/20.
//

#include "uniform_elimination_pass.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace swift::runtime::ir {


// Cached diagnostic-env probes: these sit on the per-block path and
// Darwin's getenv() walks `environ` on every call.
static bool EnvOnce(const char* name) { return std::getenv(name) != nullptr; }
static const bool kEnv_dump_ir = EnvOnce("SVM_DUMP_IR");
static const bool kEnv_dump_ir_post = EnvOnce("SVM_DUMP_IR_POST");

[[nodiscard]] static bool UniformRangeEnabled() {
    static const bool enabled = [] {
        const char* value = PerfGetenv("SVM_IR_UNIFORM_RANGE");
        return value && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

[[nodiscard]] static const UniformEffectSet* HelperUniformEffects(const Inst& inst) {
    switch (inst.GetOp()) {
        case OpCode::CallLambda:
        case OpCode::CallDynamic:
        case OpCode::CallLocation:
            return LookupUniformEffectSet(inst.GetArg<Lambda>(0).GetUniformEffectId());
        default:
            return nullptr;
    }
}

[[nodiscard]] static bool ValidUniformEffects(const UniformEffectSet& effects,
                                              u32 uniform_size) {
    if (effects.count && !effects.ranges) {
        return false;
    }
    for (u8 i = 0; i < effects.count; ++i) {
        const auto& range = effects.ranges[i];
        if (range.offset > uniform_size || range.size > uniform_size - range.offset) {
            return false;
        }
    }
    return true;
}

struct UniformValue {
    Value value{};
    u8 offset{};

    [[nodiscard]] bool Defined() const {
        return value.Defined();
    }
};

[[nodiscard]] static bool PathForwardOff() {
    static const bool off = [] {
        const char* e = PerfGetenv("SVM_UNIFORM_PATH_FWD");
        return e && std::strcmp(e, "0") == 0;
    }();
    return off;
}

// Keep the XMM state resident only within one IR unit.  This switch controls
// the Load/StoreUniform byte facts and their matching dead-store sweep; it
// neither changes the trampoline's entry fill/exit spill nor enables any
// cross-block residency.  The x86 frontend supplies the range so U64
// XmmLo/XmmHi views are covered alongside direct V128 accesses.
[[nodiscard]] static bool XmmUniformForwardOff() {
    static const bool off = [] {
        const char* e = PerfGetenv("SVM_XMM_UNIFORM_FWD");
        return e && std::strcmp(e, "0") == 0;
    }();
    return off;
}

// Phase 2 complements store->load forwarding with load->load forwarding for
// XMM state only. A first load is a valid byte fact until the same barriers and
// stores that already delimit the legacy table; subsequent identical views can
// therefore reuse it without touching the architectural context again.
[[nodiscard]] static bool XmmSsaForward2Off() {
    static const bool off = [] {
        const char* e = PerfGetenv("SVM_XMM_SSA_FWD2");
        return e && std::strcmp(e, "0") == 0;
    }();
    return off;
}

// A V32/V64 scalar operand names the low lane of the same FPR value as the
// frontend's architectural V128 XMM store/load. Keep that view as a typed SSA
// alias instead of forcing a uniform-buffer round trip merely because its IR
// width is narrower. SVM_XMM_NARROW_FWD=0 restores the pre-fix rule for A/B.
[[nodiscard]] static bool XmmNarrowForwardOff() {
    static const bool off = [] {
        const char* e = PerfGetenv("SVM_XMM_NARROW_FWD");
        return e && std::strcmp(e, "0") == 0;
    }();
    return off;
}

// --- dead uniform store elimination ----------------------------------------
//
// The forward pass above forwards uniform *loads*; nothing removed a uniform
// *store* whose bytes are overwritten again before anybody reads them.  The x86
// front end emits such stores constantly: every flag-setting arithmetic
// instruction writes the carry-polarity byte (decoder.cc StorePolarity ->
// LoadImm + StoreUniform), and a guest register written twice in one block
// stores twice.  Measured over the 25 e2e guests plus the five bench kernels,
// StoreUniform was 11.4% of all emitted IR and 7.7% of all emitted host bytes.
//
// The analysis is the mirror image of the forward one and deliberately uses the
// *same* barrier set: anything opaque enough that a folded load could not see
// through it is also opaque enough that a store before it may be observed.
// Beyond that:
//   * the walk is backward, starting with nothing known dead -- uniforms are
//     the guest context and every byte is live out of the block;
//   * a LoadUniform resurrects exactly the bytes it reads;
//   * a store is removed only when *every* byte it writes is already known to
//     be overwritten later.
// Nothing here is x86-specific: "a register write that another register write
// overwrites before any read" is dead in any guest ISA.

// Would DeadCodeEliminationPass keep `def` even after its last use is gone?
[[nodiscard]] static bool SurvivesDCE(const Inst* def) {
    switch (def->GetOp()) {
        // Keep in sync with Inst::HasSideEffects() plus the guest-load rule in
        // DeadCodeEliminationPass: every value-returning opcode either of them
        // refuses to delete.
        case OpCode::LoadMemory:
        case OpCode::LoadMemoryTSO:
        case OpCode::CompareAndSwap:
        case OpCode::CompareAndSwap128:
        case OpCode::AtomicExchange:
        case OpCode::AtomicFetchAdd:
        case OpCode::AtomicRMW:
        case OpCode::X87Op:
        case OpCode::CallLambda:
        case OpCode::CallLocation:
        case OpCode::CallDynamic:
            return true;
        default:
            return false;
    }
}

// This is the constraint that decides which stores may be removed at all.
//
// Deleting a store orphans the value it stored; DeadCodeEliminationPass then
// walks that def chain backwards, deleting each producer as it loses its last
// use.  If the chain reaches an instruction DCE must KEEP -- a guest memory
// read, an atomic RMW, a host helper call -- that instruction is left with no
// uses at all, and the block-mode register allocator gives a value with no
// recorded use no live interval and hence no register
// (register_alloc_pass.cpp: `if (!end) continue;`).  The back end then asserts
// `alloc_result[id].type == GPR` the moment it emits it.
//
// The chain is real and short: `mov ecx,[r13+0x80]` is
// LoadMemory -> ZeroExtend32 -> ZeroExtend64 -> StoreUniform, so checking only
// the store's immediate operand is not enough.  Found by SVM_FUNC_BASE=0
// swift_test, the only configuration that takes the block-mode allocator path.
[[nodiscard]] static bool ChainWouldStrandASurvivor(const Value& root, const Inst* store) {
    StackVector<Inst*, 16> work{};
    u32 visited = 0;
    constexpr u32 kMaxVisited = 64;  // give up (and keep the store) beyond this
    if (auto* def = root.Def()) {
        work.push_back(def);
    }
    while (!work.empty()) {
        if (++visited > kMaxVisited) {
            return true;
        }
        auto* def = work.back();
        work.pop_back();
        if (SurvivesDCE(def)) {
            return true;
        }
        // More than the one use being removed: this producer stays alive, so
        // nothing behind it dies either.
        const u8 uses = const_cast<Inst*>(def)->GetUses(false);
        if (uses > 1) {
            continue;
        }
        for (auto& value : const_cast<Inst*>(def)->GetValues()) {
            if (auto* next = value.Def(); next && next != store) {
                work.push_back(next);
            }
        }
    }
    return false;
}

static void EliminateDeadStores(Block* block, const UniformInfo& info,
                                HIRFunction* hir_function, bool xmm_only = false) {
    PerfScope2 perf_dse{GetPerfStats2().uniform_dse};
    auto& inst_list = block->GetInstList();
    // killed[i] = byte i of the uniform buffer is overwritten later in this
    // block before any read.
    StackVector<u8, 0x100> killed{};
    killed.resize(info.uniform_size);
    std::fill(killed.begin(), killed.end(), u8{0});
    StackVector<Inst*, 16> victims{};
    // Backward counterpart of the forward value intersection below. At a
    // BindLabel, `killed` describes writes after the merge and therefore
    // applies to both incoming paths. Keep that state for the taken edge, walk
    // the fallthrough edge, then intersect at its Goto/NotGoto: a byte is dead
    // before the branch only when it is overwritten on both successors.
    std::unordered_map<Inst*, std::vector<u8>> label_killed;

    auto clear_all = [&] { std::fill(killed.begin(), killed.end(), u8{0}); };

    for (auto it = inst_list.rbegin(); it != inst_list.rend(); ++it) {
        Inst& inst = *it;
        switch (inst.GetOp()) {
            case OpCode::StoreUniform: {
                const auto uniform = inst.GetArg<Uniform>(0);
                const auto value = inst.GetArg<Value>(1);
                const u32 off = uniform.GetOffset();
                // The backend stores according to the *value* width, not the
                // descriptor width -- the same rule the forward pass tracks.
                const u32 size = GetValueSizeByte(value.Type());
                if (xmm_only && !info.IsXmmUniformRange(off, size)) {
                    break;
                }
                if (XmmUniformForwardOff() && info.IsXmmUniformRange(off, size)) {
                    break;
                }
                if (off + size > killed.size()) {
                    break;
                }
                bool all_dead = true;
                for (u32 i = 0; i < size; ++i) {
                    if (!killed[off + i]) {
                        all_dead = false;
                        break;
                    }
                }
                if (all_dead && !ChainWouldStrandASurvivor(value, &inst)) {
                    victims.push_back(&inst);
                    break;  // a removed store kills nothing
                }
                for (u32 i = 0; i < size; ++i) {
                    killed[off + i] = 1;
                }
                break;
            }
            case OpCode::LoadUniform: {
                const auto uniform = inst.GetArg<Uniform>(0);
                const u32 off = uniform.GetOffset();
                const u32 size = GetValueSizeByte(uniform.GetType());
                if (xmm_only && !info.IsXmmUniformRange(off, size)) {
                    break;
                }
                if (XmmUniformForwardOff() && info.IsXmmUniformRange(off, size)) {
                    break;
                }
                for (u32 i = 0; i < size && off + i < killed.size(); ++i) {
                    killed[off + i] = 0;
                }
                break;
            }
            case OpCode::CallLambda:
            case OpCode::CallDynamic:
            case OpCode::CallLocation:
            case OpCode::X87Op:
            case OpCode::MemoryCopy:
            case OpCode::MemoryCopyTSO:
            case OpCode::SetHostFPR:
            case OpCode::GetHostFPR:
            case OpCode::SetLocation:
                clear_all();
                break;
            case OpCode::SetHostGPR:
            case OpCode::GetHostGPR:
                // In the XMM-only cleanup these operations touch a disjoint,
                // statically mapped GPR and cannot observe the XMM uniform
                // bytes. The full generic sweep stays conservative.
                if (!xmm_only) {
                    clear_all();
                }
                break;
            case OpCode::BindLabel:
                if (PathForwardOff()) {
                    clear_all();
                } else {
                    label_killed[inst.GetArg<Value>(0).Def()] =
                            std::vector<u8>(killed.begin(), killed.end());
                }
                break;
            case OpCode::Goto:
            case OpCode::NotGoto: {
                if (PathForwardOff()) {
                    clear_all();
                    break;
                }
                auto taken = label_killed.find(&inst);
                if (taken == label_killed.end() ||
                    taken->second.size() != killed.size()) {
                    clear_all();
                    break;
                }
                for (size_t i = 0; i < killed.size(); ++i) {
                    killed[i] &= taken->second[i];
                }
                label_killed.erase(taken);
                break;
            }
            default:
                break;
        }
    }

    for (auto* victim : victims) {
        if (hir_function) {
            hir_function->EraseInst(block, victim);
        } else {
            inst_list.erase(inst_list.iterator_to(*victim));
            delete victim;
        }
    }

    if (!victims.empty() && kEnv_dump_ir) {
        fmt::print(stderr, "[uniform-dse] block {:#x}: removed {} dead uniform store(s)\n",
                   block->GetStartLocation().Value(), victims.size());
    }
    if (Perf2Enabled()) {
        auto& stats = GetPerfStats2();
        // The PIN_EXT repair sweep revisits the same block for XMM stores
        // only. Keep the historical one-block/one-count probe contract while
        // still reporting every additional victim it finds.
        if (!xmm_only) {
            stats.uniform_dse_blocks.fetch_add(1, std::memory_order_relaxed);
        }
        stats.uniform_dse_victims.fetch_add(victims.size(), std::memory_order_relaxed);
    }
}

void UniformEliminationPass::Run(Block* block, const UniformInfo& info, bool fast_path,
                                 HIRFunction* hir_function) {
    PerfScope2 perf_forward{GetPerfStats2().uniform_forward};

    // The corpus is dominated by blocks with no uniform operations. Probe only
    // until the first relevant instruction: a miss replaces the legacy forward
    // scan and lets us avoid both fixed-size byte tables plus the reverse scan;
    // a hit pays only the usually short prefix before entering the unchanged
    // transfer logic below.
    if (fast_path) {
        u32 probed_instructions{};
        bool has_uniform_op = false;
        for (const auto& inst : block->GetInstList()) {
            probed_instructions++;
            const auto op = inst.GetOp();
            has_uniform_op = op == OpCode::LoadUniform || op == OpCode::StoreUniform;
            if (has_uniform_op) {
                break;
            }
        }
        if (Perf2Enabled()) {
            auto& stats = GetPerfStats2();
            stats.uniform_probe_insts.fetch_add(probed_instructions, std::memory_order_relaxed);
            stats.uniform_probe_hits.fetch_add(has_uniform_op, std::memory_order_relaxed);
        }
        if (!has_uniform_op) {
            perf_forward.Stop();
            if (Perf2Enabled()) {
                auto& stats = GetPerfStats2();
                stats.uniform_blocks.fetch_add(1, std::memory_order_relaxed);
                stats.uniform_no_ops_blocks.fetch_add(1, std::memory_order_relaxed);
                stats.uniform_insts.fetch_add(probed_instructions, std::memory_order_relaxed);
            }
            return;
        }
    }

    // W55 descriptors would otherwise be converted to SetHostGPR before the
    // reverse sweep, making each mapped write an opaque barrier and losing the
    // existing dead-store wins. Run the same proven sweep on the original
    // Load/StoreUniform stream first, then skip the post-conversion sweep.
    // OFF retains the historical pass order byte-for-byte.
    static const bool dse_off = [] {
        const char* e = PerfGetenv("SVM_UNIFORM_DSE");
        return e && std::strcmp(e, "0") == 0;
    }();
    const bool pin_ext_dse = info.uni_gprs.Get(22) && info.uni_gprs.Get(23) &&
                             info.uni_gprs.Get(29);
    if (pin_ext_dse && !dse_off) {
        EliminateDeadStores(block, info, hir_function);
    }

    StackVector<UniformValue, 0x100> uniform_values{info.uniform_size};
    // A local Goto/NotGoto has two successors: the following instruction and
    // its BindLabel. Facts established before the branch dominate both paths,
    // so dropping the whole table at each marker needlessly reloads guest state
    // after shift/count guard regions. Snapshot the taken edge and intersect it
    // with the fallthrough state at the label. A byte survives only when both
    // paths name the same defining value and byte offset.
    //
    // The opt-out is intentionally scoped to this path merge; all legacy
    // straight-line forwarding and barriers remain byte-for-byte unchanged.
    std::unordered_map<Inst*, std::vector<UniformValue>> label_values;
    u32 load_count{};
    u32 folded_load_count{};
    u32 mapped_load_count{};
    u32 mapped_store_count{};
    u32 invalidation_count{};
    u32 full_invalidation_count{};
    u32 range_invalidation_count{};
    u64 preserved_fact_count{};
    u32 instruction_count{};
    u32 store_count{};
    u32 barrier_count{};
    u32 path_merge_count{};
    u32 path_merge_bytes{};

    auto invalidate_uniform_values = [&] {
        std::fill(uniform_values.begin(), uniform_values.end(), UniformValue{});
        invalidation_count++;
        full_invalidation_count++;
    };
    auto invalidate_helper_effects = [&](const Inst& inst) {
        const auto* effects = HelperUniformEffects(inst);
        if (!effects || !ValidUniformEffects(*effects, uniform_values.size())) {
            invalidate_uniform_values();
            return;
        }
        for (u8 range_index = 0; range_index < effects->count; ++range_index) {
            const auto& range = effects->ranges[range_index];
            std::fill(uniform_values.begin() + range.offset,
                      uniform_values.begin() + range.offset + range.size, UniformValue{});
        }
        invalidation_count++;
        range_invalidation_count++;
        preserved_fact_count += std::count_if(
                uniform_values.begin(), uniform_values.end(),
                [](const UniformValue& value) { return value.Defined(); });
    };
    auto update_mapped_gpr_facts = [&](u32 offset, u32 size, const Value& value) {
        preserved_fact_count += std::count_if(
                uniform_values.begin(), uniform_values.begin() + offset,
                [](const UniformValue& fact) { return fact.Defined(); });
        preserved_fact_count += std::count_if(
                uniform_values.begin() + offset + size, uniform_values.end(),
                [](const UniformValue& fact) { return fact.Defined(); });
        for (u32 byte = 0; byte < size; ++byte) {
            uniform_values[offset + byte] = {value, static_cast<u8>(byte)};
        }
        invalidation_count++;
        range_invalidation_count++;
    };

    for (auto& inst : block->GetInstList()) {
        instruction_count++;
        switch (inst.GetOp()) {
            case OpCode::LoadUniform: {
                load_count++;
                auto uniform = inst.GetArg<Uniform>(0);
                auto uni_offset{uniform.GetOffset()};
                auto uni_type{uniform.GetType()};
                auto uni_size{GetValueSizeByte(uni_type)};
                auto is_float{IsFloatValueType(uni_type)};
                const bool xmm_forward_disabled =
                        XmmUniformForwardOff() && info.IsXmmUniformRange(uni_offset, uni_size);
                ASSERT_MSG(uni_offset + uni_size <= uniform_values.size(),
                           "uniform load [{}, {}) exceeds uniform buffer size {}",
                           uni_offset, uni_offset + uni_size, uniform_values.size());
                auto try_forward_load = [&] {
                    Value value_load{};
                    u8 value_offset{0};
                    for (u8 offset = 0; !xmm_forward_disabled && offset < uni_size; ++offset) {
                        auto& uni_value = uniform_values[uni_offset + offset];
                        if (!uni_value.Defined()) {
                            value_load = {};
                            break;
                        }
                        if (offset == 0) {
                            value_load = uni_value.value;
                            value_offset = uni_value.offset;
                        } else if (value_load != uni_value.value ||
                                   uni_value.offset != value_offset + offset) {
                            value_load = {};
                            break;
                        }
                    }

                    // BitCast is a zero-cost register alias; it does NOT implement
                    // the zero-extension of a narrow uniform load. Use it only
                    // when the load consumes the complete stored value. A partial
                    // scalar integer load needs a real BitExtract/UBFX so upper
                    // source bits cannot leak into users such as a host-call
                    // argument. BitExtract is GPR-only in the current backend, so
                    // nonzero-offset partial vector/FPR folds stay disabled; the
                    // zero-offset low-lane alias is handled explicitly below.
                    const auto value_size = value_load.Defined()
                                                  ? GetValueSizeByte(value_load.Type())
                                                  : 0;
                    const bool same_reg_class =
                            value_load.Defined() &&
                            IsFloatValueType(value_load.Type()) == is_float;
                    // Narrow arithmetic producers use a W/X-register container and
                    // are not required to clear bits above their logical width.
                    // A Value cast wrapper changes Value::Type(), but does not emit
                    // any narrowing instruction. Therefore even an apparently
                    // same-width U8/U16/U32 store/load pair must extract explicitly:
                    // a U32 wrapper may still name a U64 producer, and CallLambda
                    // consumes its X register. Only a complete U64 scalar or exact
                    // FPR value is guaranteed safe as a zero-cost alias.
                    const bool full_value =
                            same_reg_class && value_offset == 0 &&
                            uni_size == value_size && (is_float || uni_size == sizeof(u64));
                    // A low V32/V64 view of a wider FPR value needs no extract:
                    // every consumer addresses lane zero of the same physical
                    // SIMD register. UniformStoreSinkPass already relies on
                    // this exact alias rule for pending XMM stores; applying it
                    // to the ordinary byte-fact table closes the earlier
                    // store/load and load/load forwarding gap as well.
                    const bool low_fpr_view =
                            !XmmNarrowForwardOff() && same_reg_class && is_float &&
                            value_offset == 0 && uni_size < value_size;
                    const bool scalar_extract =
                            same_reg_class && !is_float &&
                            value_offset + uni_size <= value_size;
                    if (!full_value && !low_fpr_view && !scalar_extract) {
                        return false;
                    }
                    inst.Reset();
                    if (full_value || low_fpr_view) {
                        inst.BitCast(value_load).SetReturn(uni_type);
                    } else {
                        inst.BitExtract(value_load, Imm(value_offset * 8u),
                                        Imm(uni_size * 8u)).SetReturn(uni_type);
                    }
                    folded_load_count++;
                    return true;
                };
                auto uniform_register = info.uniform_regs_map.GetValueAt(uniform.GetOffset());
                if (uniform_register.Null()) {
                    for (u8 offset = 1; offset < uni_size; ++offset) {
                        if (!info.uniform_regs_map.GetValueAt(uni_offset + offset).Null()) {
                            PANIC("Cross uniform load: {}", fmt::format("{}", inst));
                        }
                    }
                }
                // static uniform load
                if (!uniform_register.Null()) {
                    auto uni_reg_offset{uniform_register.uniform.GetOffset()};
                    auto uni_reg_type{uniform_register.uniform.GetType()};
                    auto uni_reg_size{GetValueSizeByte(uni_reg_type)};
                    // A statically resident V128 is also the architectural
                    // backing for the scalar XmmLo/XmmHi views.  Those views
                    // deliberately use U64 IR values, so bridge them through
                    // GetHostFPR and let the backend UMOV the selected lane to
                    // a GPR.  Other cross-register-class mappings remain
                    // invalid.
                    const bool scalar_from_fpr =
                            IsFloatValueType(uni_reg_type) && !is_float &&
                            uni_size <= sizeof(u64);
                    if ((!scalar_from_fpr &&
                         IsFloatValueType(uni_reg_type) != is_float) ||
                        uni_offset < uni_reg_offset ||
                        (uni_offset + uni_size) > (uni_reg_offset + uni_reg_size) ||
                        info.uniform_regs_map.GetContinuousSizeFrom(uni_offset) < uni_size) {
                        PANIC("Cross uniform load: {}", fmt::format("{}", inst));
                        break;
                    }
                    const bool pin_ext_gpr = !uniform_register.host_reg.is_fpr &&
                            (uniform_register.host_reg.gpr.id <= 9 ||
                             uniform_register.host_reg.gpr.id == 22 ||
                             uniform_register.host_reg.gpr.id == 23 ||
                             uniform_register.host_reg.gpr.id == 29);
                    if ((UniformRangeEnabled() || pin_ext_gpr) &&
                        !uniform_register.host_reg.is_fpr && try_forward_load()) {
                        break;
                    }
                    inst.Reset();
                    const bool mapped_fpr = uniform_register.host_reg.is_fpr;
                    auto reg_index = mapped_fpr ? uniform_register.host_reg.fpr.id
                                                : uniform_register.host_reg.gpr.id;
                    Imm offset_in{static_cast<u8>(uni_offset - uni_reg_offset)};
                    if (mapped_fpr) {
                        inst.GetHostFPR(HostRegIndex(reg_index), offset_in).SetReturn(uni_type);
                    } else {
                        inst.GetHostGPR(HostRegIndex(reg_index), offset_in).SetReturn(uni_type);
                    }
                    mapped_load_count++;
                    if (pin_ext_gpr) {
                        // Seed load-to-load forwarding for the new resident
                        // GPRs. Snapshot safety is enforced later by the
                        // allocator's SetHost crossing check; a repeated full
                        // load can therefore become a zero-cost BitCast alias.
                        const Value loaded{&inst};
                        for (u8 byte = 0; byte < uni_size; ++byte) {
                            uniform_values[uni_offset + byte] = {loaded, byte};
                        }
                    }
                    break;
                }

                if (try_forward_load()) {
                    break;
                }
                if (!xmm_forward_disabled && !XmmSsaForward2Off() &&
                           info.IsXmmUniformRange(uni_offset, uni_size)) {
                    // Unlike a store fact, this value names the load itself.
                    // Recording every byte preserves the existing continuity
                    // check for full V128 and U64 lane views. A later partial
                    // view that cannot be represented safely simply replaces
                    // the covered bytes with its own materialized load.
                    const Value loaded{&inst};
                    for (u8 offset = 0; offset < uni_size; ++offset) {
                        uniform_values[uni_offset + offset] = {loaded, offset};
                    }
                }
                break;
            }
            case OpCode::StoreUniform: {
                store_count++;
                auto uniform = inst.GetArg<Uniform>(0);
                auto value = inst.GetArg<Value>(1);
                auto uni_offset{uniform.GetOffset()};
                auto value_type{value.Type()};
                auto value_size{GetValueSizeByte(value_type)};
                auto value_is_float{IsFloatValueType(value_type)};
                const bool xmm_forward_disabled =
                        XmmUniformForwardOff() && info.IsXmmUniformRange(uni_offset, value_size);
                ASSERT_MSG(uni_offset + value_size <= uniform_values.size(),
                           "uniform store [{}, {}) exceeds uniform buffer size {}",
                           uni_offset, uni_offset + value_size, uniform_values.size());
                auto uniform_register = info.uniform_regs_map.GetValueAt(uniform.GetOffset());
                if (uniform_register.Null()) {
                    for (u8 offset = 1; offset < value_size; ++offset) {
                        if (!info.uniform_regs_map.GetValueAt(uni_offset + offset).Null()) {
                            PANIC("Cross uniform store: {}", fmt::format("{}", inst));
                        }
                    }
                }
                // Static uniform store. The whole access must be contained in
                // one mapping; a cross-boundary access cannot be represented by
                // a single SetHost* operation.
                if (!uniform_register.Null()) {
                    auto uni_reg_offset{uniform_register.uniform.GetOffset()};
                    auto uni_reg_type{uniform_register.uniform.GetType()};
                    auto uni_reg_size{GetValueSizeByte(uni_reg_type)};
                    // Mirror the scalar XMM load bridge above: XmmLo/XmmHi
                    // stores arrive as U64 values and become INS into the
                    // pinned V128 register.
                    const bool scalar_to_fpr =
                            IsFloatValueType(uni_reg_type) && !value_is_float &&
                            value_size <= sizeof(u64);
                    if ((!scalar_to_fpr &&
                         IsFloatValueType(uni_reg_type) != value_is_float) ||
                        uni_offset < uni_reg_offset ||
                        (uni_offset + value_size) > (uni_reg_offset + uni_reg_size) ||
                        info.uniform_regs_map.GetContinuousSizeFrom(uni_offset) < value_size) {
                        PANIC("Cross uniform store: {}", fmt::format("{}", inst));
                        break;
                    }
                    inst.Reset();
                    const bool mapped_fpr = uniform_register.host_reg.is_fpr;
                    auto reg_index = mapped_fpr ? uniform_register.host_reg.fpr.id
                                                : uniform_register.host_reg.gpr.id;
                    Imm offset_in{static_cast<u8>(uni_offset - uni_reg_offset)};
                    if (mapped_fpr) {
                        inst.SetHostFPR(value, HostRegIndex(reg_index), offset_in);
                    } else {
                        inst.SetHostGPR(value, HostRegIndex(reg_index), offset_in);
                    }
                    mapped_store_count++;
                    const bool pin_ext_gpr = !mapped_fpr &&
                            (reg_index <= 9 || reg_index == 22 ||
                             reg_index == 23 || reg_index == 29);
                    if ((UniformRangeEnabled() || pin_ext_gpr) && !mapped_fpr &&
                        uni_reg_size == sizeof(u64)) {
                        // The pinned GPR now contains this value in exactly the
                        // bytes written by SetHostGPR. Keep every other byte
                        // fact, including untouched bytes in this same GPR.
                        // W55's extended pins always use this W50 range-local
                        // path: a new hot mapped store must not turn into a
                        // whole-context fact barrier merely because the global
                        // diagnostic switch remains at its default OFF.
                        update_mapped_gpr_facts(uni_offset, value_size, value);
                    } else {
                        // OFF retains the legacy opaque mapped-store barrier.
                        // Mapped FPR stores stay conservative in this phase.
                        invalidate_uniform_values();
                    }
                    break;
                }
                // The backend stores according to the Value type, not the
                // Uniform descriptor type. Track those actual bytes so a
                // mismatched descriptor cannot make untouched bytes look
                // overwritten. Loads spanning old and new values fail the
                // value/offset-continuity check above.
                if (!xmm_forward_disabled) {
                    for (u8 offset = 0; offset < value_size; ++offset) {
                        uniform_values[uni_offset + offset] = {value, offset};
                    }
                }
                break;
            }
            case OpCode::CallLambda:
            case OpCode::CallDynamic:
                barrier_count++;
                if (UniformRangeEnabled()) {
                    invalidate_helper_effects(inst);
                } else {
                    invalidate_uniform_values();
                }
                break;
            case OpCode::X87Op:
            case OpCode::MemoryCopy:
            case OpCode::MemoryCopyTSO:
            case OpCode::SetHostGPR:
            case OpCode::SetHostFPR:
            case OpCode::SetLocation:
                barrier_count++;
                // Calls are conservatively opaque even though today's x86
                // helpers do not receive the uniform buffer. MemoryCopy uses
                // guest addresses today, but is kept opaque at this generic IR
                // layer. SetLocation covers interrupt/syscall exits; terminals
                // end the block and therefore have no following load to fold.
                // Direct SetHost* operations mutate statically mapped state
                // outside the byte cache. Local branches are handled by the
                // path-intersection cases below.
                invalidate_uniform_values();
                break;
            case OpCode::Goto:
            case OpCode::NotGoto:
                barrier_count++;
                if (PathForwardOff()) {
                    invalidate_uniform_values();
                } else {
                    label_values[&inst] =
                            std::vector<UniformValue>(uniform_values.begin(),
                                                      uniform_values.end());
                }
                break;
            case OpCode::BindLabel: {
                barrier_count++;
                if (PathForwardOff()) {
                    invalidate_uniform_values();
                    break;
                }
                auto* branch = inst.GetArg<Value>(0).Def();
                auto target = label_values.find(branch);
                if (target == label_values.end() ||
                    target->second.size() != uniform_values.size()) {
                    invalidate_uniform_values();
                    break;
                }
                u32 preserved{};
                for (size_t i = 0; i < uniform_values.size(); ++i) {
                    const auto& taken = target->second[i];
                    auto& fallthrough = uniform_values[i];
                    const bool same = taken.Defined() && fallthrough.Defined() &&
                                      taken.value == fallthrough.value &&
                                      taken.offset == fallthrough.offset;
                    if (!same) {
                        fallthrough = {};
                    } else {
                        preserved++;
                    }
                }
                path_merge_count++;
                path_merge_bytes += preserved;
                label_values.erase(target);
                break;
            }
            default:
                break;
        }
    }

    perf_forward.Stop();
    if (Perf2Enabled()) {
        auto& stats = GetPerfStats2();
        stats.uniform_blocks.fetch_add(1, std::memory_order_relaxed);
        stats.uniform_no_ops_blocks.fetch_add(load_count + store_count == 0,
                                               std::memory_order_relaxed);
        stats.uniform_insts.fetch_add(instruction_count, std::memory_order_relaxed);
        stats.uniform_loads.fetch_add(load_count, std::memory_order_relaxed);
        stats.uniform_stores.fetch_add(store_count, std::memory_order_relaxed);
        stats.uniform_barriers.fetch_add(barrier_count, std::memory_order_relaxed);
        stats.uniform_invalidations.fetch_add(invalidation_count, std::memory_order_relaxed);
        stats.uniform_full_invalidations.fetch_add(full_invalidation_count,
                                                   std::memory_order_relaxed);
        stats.uniform_range_invalidations.fetch_add(range_invalidation_count,
                                                    std::memory_order_relaxed);
        stats.uniform_preserved_facts.fetch_add(preserved_fact_count,
                                                std::memory_order_relaxed);
    }

    if (kEnv_dump_ir) {
        fmt::print(stderr,
                   "[uniform-elim] block {:#x}: LoadUniform {} -> {} "
                   "(folded {}, mapped {}), mapped stores {}, invalidations {}, "
                   "full {}, range {}, preserved facts {}, "
                   "path merges {} ({} byte-facts preserved)\n",
                   block->GetStartLocation().Value(), load_count,
                   load_count - folded_load_count - mapped_load_count,
                   folded_load_count, mapped_load_count, mapped_store_count,
                   invalidation_count, full_invalidation_count,
                   range_invalidation_count, preserved_fact_count,
                   path_merge_count, path_merge_bytes);
        if (kEnv_dump_ir_post) {
            fmt::print(stderr, "--- post-uniform block {:#x} ---\n{}\n",
                       block->GetStartLocation().Value(), block->ToString());
        }
    }

    // Escape hatch for bisecting a suspected DSE bug against the load-folding
    // half of this pass, which SVM_UNIFORM_ELIM=0 cannot separate.
    // With every uniform byte live-out, a store can be dead only if at least
    // one later store overwrites it. Fewer than two stores therefore makes the
    // reverse dataflow scan provably a no-op.
    if (!dse_off && (!fast_path || store_count >= 2)) {
        if (!pin_ext_dse) {
            EliminateDeadStores(block, info, hir_function);
        } else if (!XmmNarrowForwardOff()) {
            // PIN_EXT must run the generic sweep before mapped StoreUniforms
            // become SetHostGPR. That early sweep still sees V32/V64 loads and
            // therefore cannot know they will fold to a V128 SSA value below.
            // Revisit only XMM stores after forwarding; mapped GPR operations
            // are disjoint and no longer block the newly exposed dead stores.
            EliminateDeadStores(block, info, hir_function, true);
        }
    }
}

void UniformEliminationPass::Run(Block* block, const UniformInfo& info,
                                 HIRFunction* hir_function) {
    static const bool fast_path = [] {
        const char* env = PerfGetenv("SVM_UNIFORM_FAST");
        return !env || std::strcmp(env, "0") != 0;
    }();
    Run(block, info, fast_path, hir_function);
}

void UniformEliminationPass::Run(HIRBuilder* hir_builder, const UniformInfo& info, bool mem_to_regs) {
    for (auto &func : hir_builder->GetHIRFunctions()) {
        Run(&func, info, mem_to_regs);
    }
}

void UniformEliminationPass::Run(HIRFunction* hir_func, const UniformInfo& info, bool mem_to_regs) {
    (void)mem_to_regs;
    for (auto* hir_block : hir_func->GetHIRBlocks()) {
        if (hir_block) {
            Run(hir_block->GetBlock(), info, hir_func);
        }
    }
}

}  // namespace swift::runtime::ir
