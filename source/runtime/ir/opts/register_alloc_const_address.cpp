#include "base/logging.h"
#include "register_alloc_internal.h"
#include <cstdio>

namespace swift::runtime::ir {

namespace {

constexpr u64 kConstAddressPageOffsetMask = 0xfff;
constexpr u64 kConstAddressPageMask = ~kConstAddressPageOffsetMask;

bool CanUsePageOffset(Inst& use, u64 address) {
    ValueType type{};
    if (use.GetOp() == OpCode::LoadMemory) {
        type = use.ReturnType();
    } else if (use.GetOp() == OpCode::StoreMemory) {
        type = use.GetArg<Value>(1).Type();
    } else {
        return false;
    }
    const u64 size = GetValueSizeByte(type);
    const u64 offset = address & kConstAddressPageOffsetMask;
    return size != 0 && (offset <= 255 || offset % size == 0);
}

}  // namespace

std::optional<u64> RawConstAddressValue(Inst* inst) {
    if (!inst || inst->GetOp() != OpCode::GetOperand) {
        return std::nullopt;
    }
    const auto operand = inst->GetArg<Operand>(0);
    if (!operand.GetLeft().IsImm()) {
        return std::nullopt;
    }
    if (operand.GetRight().Null()) {
        return operand.GetLeft().imm.Get();
    }
    if (operand.GetOp() == OperandOp::Plus && operand.GetRight().IsImm() &&
        operand.GetRight().imm.Get() == 0) {
        return operand.GetLeft().imm.Get();
    }
    return std::nullopt;
}

std::optional<u64> ConstAddressValue(Inst* inst) {
    if (!inst || inst->ReturnType() != ValueType::U64 || inst->GetUses() != 1) {
        return std::nullopt;
    }
    return RawConstAddressValue(inst);
}

bool IsConstAddressBarrier(OpCode op) {
    return op == OpCode::Goto || op == OpCode::NotGoto ||
           op == OpCode::BindLabel;
}

void CacheConstantAddressesForBlock(
        Block* lir_block,
        backend::RegAlloc* reg_alloc,
        u64 unit_pc,
        bool audit,
        const RegisterAllocFamilyCallbacks& callbacks) {
    auto CheckInstr = [&](Inst* inst, u32 extra_gpr, u32 extra_fpr) {
        return callbacks.check_instr(callbacks.context, inst, extra_gpr, extra_fpr);
    };
    auto DirectlyFeedsMemory = [&](Inst* inst) {
        return callbacks.directly_feeds_memory(callbacks.context, inst);
    };
    struct Group {
        u64 base{};
        Vector<ConstAddressCandidate> candidates{};
        u32 cached_reuses{};
        u32 residual_no_free{};
        u32 residual_verify{};
    };
    Vector<Group> groups{};
    auto& list = lir_block->GetInstList();
    u32 segment = 0;
    u32 raw = 0;
    u32 eligible = 0;
    u32 mismatch_width = 0;
    u32 mismatch_uses = 0;
    u32 mismatch_alloc = 0;
    u32 mismatch_feed = 0;
    u32 mismatch_barrier = 0;

    auto process_groups = [&] {
        for (auto& group : groups) {
            if (group.candidates.size() < 2) {
                if (audit && !group.candidates.empty()) {
                    SVM_DIAG_PRINT(RegisterAllocation,
                            "[svm-const-addr-group] unit=0x%llx block=0x%llx "
                            "segment=%u base=0x%llx occurrences=1 potential=0 "
                            "cached=0 no_free=0 verify=0\n",
                            static_cast<unsigned long long>(unit_pc),
                            static_cast<unsigned long long>(
                                    lir_block->GetStartLocation().Value()),
                            segment,
                            static_cast<unsigned long long>(group.base));
                }
                continue;
            }
            std::size_t first = 0;
            while (first + 1 < group.candidates.size()) {
                bool cached = false;
                bool saw_free_window = false;
                bool saw_verify_failure = false;
                for (std::size_t last = group.candidates.size() - 1;
                     last > first && !cached; --last) {
                    bool last_saw_free = false;
                    bool last_saw_verify_failure = false;
                    const u32 range_begin = group.candidates[first].inst->Id();
                    const u32 range_end = group.candidates[last].use_id;
                    // The first materialization already owns a GPR. That slot
                    // is dirty at the def, so a naive [first, last] scan can
                    // never pick it and the pass used to demand a third idle
                    // register. Skip the remapped defs themselves; gaps still
                    // reject a target that another value reused.
                    Vector<u32> remapped_defs{};
                    remapped_defs.reserve(last - first + 1);
                    for (std::size_t i = first; i <= last; ++i) {
                        remapped_defs.push_back(group.candidates[i].inst->Id());
                    }
                    auto is_remapped_def = [&](u32 id) {
                        for (u32 def_id : remapped_defs) {
                            if (def_id == id) {
                                return true;
                            }
                        }
                        return false;
                    };
                    u32 preferred = 32;
                    if (reg_alloc->ValueType(Value{group.candidates[first].inst}) ==
                        backend::RegAlloc::GPR) {
                        preferred = reg_alloc->ValueGPR(
                                            Value{group.candidates[first].inst})
                                            .id;
                    }
                    auto consider_target = [&](u32 target) {
                        if (target >= 32 || reg_alloc->GetGprs().Get(target)) {
                            return false;
                        }
                        for (auto& scan : list) {
                            if (scan.Id() < range_begin || scan.Id() > range_end ||
                                is_remapped_def(scan.Id())) {
                                continue;
                            }
                            if (reg_alloc->DirtyGPR(scan.Id()).Get(target)) {
                                return false;
                            }
                        }
                        return true;
                    };
                    for (u32 step = 0; step < 33 && !cached; ++step) {
                        const u32 target =
                                step == 0 ? preferred : static_cast<u32>(step - 1);
                        if (step > 0 && target == preferred) {
                            continue;
                        }
                        if (!consider_target(target)) {
                            continue;
                        }
                        last_saw_free = true;

                        // 缓存所有者记入每条指令的 active mask。这样
                        // emitter 瞬时 scratch 与 spill reload 仍由既有硬门
                        // 计费；少一条空闲 GPR 后无法容纳就原样回退。
                        struct SavedMask {
                            u32 id;
                            backend::GPRSMask gprs;
                            backend::FPRSMask fprs;
                        };
                        Vector<SavedMask> saved{};
                        for (auto& scan : list) {
                            if (scan.Id() < range_begin || scan.Id() > range_end) {
                                continue;
                            }
                            auto gprs = reg_alloc->DirtyGPR(scan.Id());
                            auto fprs = reg_alloc->DirtyFPR(scan.Id());
                            saved.push_back({scan.Id(), gprs, fprs});
                            gprs.Mark(target);
                            reg_alloc->SetActiveRegs(scan.Id(), gprs, fprs);
                        }
                        bool verified = true;
                        for (auto& scan : list) {
                            if (scan.Id() >= range_begin && scan.Id() <= range_end &&
                                !CheckInstr(&scan, 0, 0)) {
                                verified = false;
                                break;
                            }
                        }
                        if (!verified) {
                            last_saw_verify_failure = true;
                            for (auto& old : saved) {
                                reg_alloc->SetActiveRegs(old.id, old.gprs, old.fprs);
                            }
                            continue;
                        }

                        const u32 anchor = group.candidates[first].inst->Id();
                        for (std::size_t i = first; i <= last; ++i) {
                            auto* candidate = group.candidates[i].inst;
                            reg_alloc->MapRegister(
                                    candidate->Id(),
                                    HostGPR{static_cast<u16>(target)});
                            reg_alloc->MarkConstAddressCached(candidate->Id(), anchor);
                        }
                        group.cached_reuses += static_cast<u32>(last - first);
                        // 不能覆盖到组尾意味着这里必须重新锚定一次。较长窗口
                        // 已按从长到短次序全部失败，把这一个残余归到实际失败门。
                        if (last + 1 < group.candidates.size()) {
                            if (saw_verify_failure) {
                                ++group.residual_verify;
                            } else {
                                ++group.residual_no_free;
                            }
                        }
                        first = last + 1;
                        cached = true;
                    }
                    if (!cached) {
                        saw_free_window |= last_saw_free;
                        saw_verify_failure |= last_saw_verify_failure;
                    }
                }
                if (!cached) {
                    if (saw_free_window && saw_verify_failure) {
                        ++group.residual_verify;
                    } else {
                        ++group.residual_no_free;
                    }
                    ++first;
                }
            }
            if (audit) {
                const u32 potential =
                        static_cast<u32>(group.candidates.size() - 1);
                ASSERT(group.cached_reuses + group.residual_no_free +
                               group.residual_verify ==
                       potential);
                SVM_DIAG_PRINT(RegisterAllocation,
                        "[svm-const-addr-group] unit=0x%llx block=0x%llx "
                        "segment=%u base=0x%llx occurrences=%zu potential=%u "
                        "cached=%u no_free=%u verify=%u\n",
                        static_cast<unsigned long long>(unit_pc),
                        static_cast<unsigned long long>(
                                lir_block->GetStartLocation().Value()),
                        segment,
                        static_cast<unsigned long long>(group.base),
                        group.candidates.size(), potential,
                        group.cached_reuses, group.residual_no_free,
                        group.residual_verify);
            }
        }
        groups.clear();
        ++segment;
    };

    for (auto& inst : list) {
        if (IsConstAddressBarrier(inst.GetOp())) {
            process_groups();
            continue;
        }
        const auto raw_address = RawConstAddressValue(&inst);
        if (raw_address) {
            ++raw;
            if (inst.ReturnType() != ValueType::U64) {
                ++mismatch_width;
                continue;
            }
            if (inst.GetUses() != 1) {
                ++mismatch_uses;
                continue;
            }
            if (reg_alloc->ValueType(Value{&inst}) != backend::RegAlloc::GPR) {
                ++mismatch_alloc;
                continue;
            }
            if (!DirectlyFeedsMemory(&inst)) {
                ++mismatch_feed;
                continue;
            }
        }
        const auto address = ConstAddressValue(&inst);
        if (!address) {
            continue;
        }
        u32 use_id = inst.Id();
        Inst* memory_use = nullptr;
        for (auto& use : list) {
            if (use.Id() <= inst.Id()) {
                continue;
            }
            if (IsConstAddressBarrier(use.GetOp())) {
                break;
            }
            bool names = false;
            for (auto value : use.GetValues()) {
                names |= value.Def() == &inst;
            }
            if (names) {
                use_id = use.Id();
                memory_use = &use;
                break;
            }
        }
        if (!memory_use || !CanUsePageOffset(*memory_use, *address)) {
            ++mismatch_barrier;
            continue;
        }
        ++eligible;
        const u64 base = *address & kConstAddressPageMask;
        auto group = std::find_if(groups.begin(), groups.end(),
                                  [&](const Group& item) {
                                      return item.base == base;
                                  });
        if (group == groups.end()) {
            groups.push_back(Group{base, {}});
            group = std::prev(groups.end());
        }
        group->candidates.push_back({&inst, use_id});
    }
    process_groups();
    if (audit && raw) {
        SVM_DIAG_PRINT(RegisterAllocation,
                "[svm-const-addr-shape] unit=0x%llx block=0x%llx raw=%u "
                "eligible=%u width=%u uses=%u alloc=%u feed=%u barrier=%u\n",
                static_cast<unsigned long long>(unit_pc),
                static_cast<unsigned long long>(
                        lir_block->GetStartLocation().Value()),
                raw, eligible, mismatch_width, mismatch_uses,
                mismatch_alloc, mismatch_feed, mismatch_barrier);
    }
}


}  // namespace swift::runtime::ir
