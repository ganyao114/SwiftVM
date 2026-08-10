#include "register_alloc_internal.h"
#include <cstdio>

namespace swift::runtime::ir {

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
        u64 address{};
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
                    std::fprintf(
                            stderr,
                            "[svm-const-addr-group] unit=0x%llx block=0x%llx "
                            "segment=%u address=0x%llx occurrences=1 potential=0 "
                            "cached=0 no_free=0 verify=0\n",
                            static_cast<unsigned long long>(unit_pc),
                            static_cast<unsigned long long>(
                                    lir_block->GetStartLocation().Value()),
                            segment,
                            static_cast<unsigned long long>(group.address));
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
                    for (u32 target = 0; target < 32 && !cached; ++target) {
                        if (reg_alloc->GetGprs().Get(target)) {
                            continue;
                        }
                        bool free = true;
                        for (auto& scan : list) {
                            if (scan.Id() < range_begin || scan.Id() > range_end) {
                                continue;
                            }
                            if (reg_alloc->DirtyGPR(scan.Id()).Get(target)) {
                                free = false;
                                break;
                            }
                        }
                        if (!free) {
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
                std::fprintf(
                        stderr,
                        "[svm-const-addr-group] unit=0x%llx block=0x%llx "
                        "segment=%u address=0x%llx occurrences=%zu potential=%u "
                        "cached=%u no_free=%u verify=%u\n",
                        static_cast<unsigned long long>(unit_pc),
                        static_cast<unsigned long long>(
                                lir_block->GetStartLocation().Value()),
                        segment,
                        static_cast<unsigned long long>(group.address),
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
        bool valid_use = false;
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
                valid_use = true;
                break;
            }
        }
        if (!valid_use) {
            ++mismatch_barrier;
            continue;
        }
        ++eligible;
        auto group = std::find_if(groups.begin(), groups.end(),
                                  [&](const Group& item) {
                                      return item.address == *address;
                                  });
        if (group == groups.end()) {
            groups.push_back(Group{*address, {}});
            group = std::prev(groups.end());
        }
        group->candidates.push_back({&inst, use_id});
    }
    process_groups();
    if (audit && raw) {
        std::fprintf(
                stderr,
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
