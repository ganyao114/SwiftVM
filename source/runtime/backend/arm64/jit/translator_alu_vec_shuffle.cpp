#include "translator.h"

#include <algorithm>
#include <cstring>
#include <functional>

#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/context.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

void JitTranslator::EmitVecShuffle32(ir::Inst* inst) {
    auto src = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    auto indexes = context.GetTmpV();
    auto tmp = context.GetTmpX();
    const u32 control = inst->GetArg<ir::Imm>(1).Get();
    u64 index_lo = 0;
    u64 index_hi = 0;
    for (u32 byte = 0; byte < 16; ++byte) {
        const u32 output_lane = byte / 4;
        const u32 selected_lane = (control >> (output_lane * 2)) & 3;
        const u8 index = selected_lane * 4 + (byte & 3);
        auto& half = byte < 8 ? index_lo : index_hi;
        half |= u64(index) << ((byte & 7) * 8);
    }
    __ Mov(tmp, index_lo);
    __ Fmov(indexes.D(), tmp);
    __ Mov(tmp, index_hi);
    __ Ins(indexes.V2D(), 1, tmp);
    __ Tbl(result.V16B(), src.V16B(), indexes.V16B());
}

bool JitTranslator::ReproveShufpsImmTie(ir::Inst* inst) const {
    if (!sse_shufps_imm || !inst ||
        inst->GetOp() != ir::OpCode::VecShuffle32TwoSrc) {
        return false;
    }
    const u32 control = inst->GetArg<ir::Imm>(2).Get() & 0xffu;
    auto resolve = [](ir::Value value) {
        while (value.Defined() && value.Def()->IsBitCastOperation()) {
            value = value.Def()->GetArg<ir::Value>(0);
        }
        return value;
    };
    auto left = resolve(inst->GetArg<ir::Value>(0));
    const bool alias_shape = inst->GetArg<ir::Value>(0).Id() ==
                             inst->GetArg<ir::Value>(1).Id();
    if (control != 0xe4 &&
        !((control == 0xe0 || control == 0xe5) && alias_shape)) {
        return false;
    }
    if (!left.Defined() || !left.Def() || context.IsHostReadCoalesced(left.Id()) ||
        !context.SharesFPR(left, ir::Value{inst})) {
        return false;
    }
    u32 last_use = left.Def()->Id();
    for (auto& scan : cur_block->GetInstList()) {
        for (auto use : scan.GetValues()) {
            if (resolve(use).Def() == left.Def()) {
                last_use = std::max<u32>(last_use, scan.Id());
            }
        }
    }
    return last_use == inst->Id();
}

void JitTranslator::EmitVecShuffle32TwoSrc(ir::Inst* inst) {
    const auto left_value = inst->GetArg<ir::Value>(0);
    const auto right_value = inst->GetArg<ir::Value>(1);
    auto left = context.V(left_value);
    auto right = context.V(right_value);
    auto result = context.V(ir::Value{inst});
    const u32 control = inst->GetArg<ir::Imm>(2).Get() & 0xFFu;
    const bool same_source = left_value.Id() == right_value.Id();

    // One-instruction shapes. The 64-bit ZIPs select one complete qword from
    // each source; the 32-bit UZPs select even/odd dwords. EXT covers the
    // contiguous a[2..3],b[0..1] form.
    if (control == 0x44) {
        __ Zip1(result.V2D(), left.V2D(), right.V2D());
        return;
    }
    if (control == 0xEE) {
        __ Zip2(result.V2D(), left.V2D(), right.V2D());
        return;
    }
    if (control == 0x4E) {
        __ Ext(result.V16B(), left.V16B(), right.V16B(), 8);
        return;
    }
    if (control == 0x88) {
        __ Uzp1(result.V4S(), left.V4S(), right.V4S());
        return;
    }
    if (control == 0xDD) {
        __ Uzp2(result.V4S(), left.V4S(), right.V4S());
        return;
    }

    if (same_source) {
        // Alias-only shapes gain more one-instruction mappings because both
        // architectural operands name the same old destination.
        switch (control) {
            case 0xE4:
                if (sse_shufps_imm && result.GetCode() == left.GetCode()) {
                    ASSERT_MSG(ReproveShufpsImmTie(inst),
                               "SHUFPS imm tie proof diverged at IR {}", inst->Id());
                }
                if (result.GetCode() != left.GetCode()) {
                    __ Orr(result.V16B(), left.V16B(), left.V16B());
                }
                return;
            case 0x00: __ Dup(result.V4S(), left.V4S(), 0); return;
            case 0x55: __ Dup(result.V4S(), left.V4S(), 1); return;
            case 0xAA: __ Dup(result.V4S(), left.V4S(), 2); return;
            case 0xFF: __ Dup(result.V4S(), left.V4S(), 3); return;
            case 0x39: __ Ext(result.V16B(), left.V16B(), left.V16B(), 4); return;
            case 0x93: __ Ext(result.V16B(), left.V16B(), left.V16B(), 12); return;
            case 0x50: __ Zip1(result.V4S(), left.V4S(), left.V4S()); return;
            case 0xFA: __ Zip2(result.V4S(), left.V4S(), left.V4S()); return;
            case 0xA0: __ Trn1(result.V4S(), left.V4S(), left.V4S()); return;
            case 0xF5: __ Trn2(result.V4S(), left.V4S(), left.V4S()); return;
            // c-ray's two dominant splat-low shapes preserve the old high
            // qword. When RA ties result to source, one INS is sufficient.
            case 0xE0:
            case 0xE5: {
                if (sse_shufps_imm && result.GetCode() == left.GetCode()) {
                    ASSERT_MSG(ReproveShufpsImmTie(inst),
                               "SHUFPS imm tie proof diverged at IR {}", inst->Id());
                }
                if (result.GetCode() != left.GetCode()) {
                    __ Orr(result.V16B(), left.V16B(), left.V16B());
                }
                const u32 selected = control & 3;
                const u32 destination = selected == 0 ? 1 : 0;
                __ Ins(result.V4S(), destination, left.V4S(), selected);
                return;
            }
            default: break;
        }

        // A single table is sufficient for every remaining alias control.
        auto indexes = context.GetTmpV();
        auto tmp = context.GetTmpX();
        u64 index_lo = 0;
        u64 index_hi = 0;
        for (u32 byte = 0; byte < 16; ++byte) {
            const u32 lane = byte / 4;
            const u8 index = u8(((control >> (lane * 2)) & 3) * 4 + (byte & 3));
            auto& half = byte < 8 ? index_lo : index_hi;
            half |= u64(index) << ((byte & 7) * 8);
        }
        __ Mov(tmp, index_lo);
        __ Fmov(indexes.D(), tmp);
        __ Mov(tmp, index_hi);
        __ Ins(indexes.V2D(), 1, tmp);
        __ Tbl(result.V16B(), left.V16B(), indexes.V16B());
        return;
    }

    if (sse_shufps_imm && control == 0xe4 &&
        result.GetCode() == left.GetCode()) {
        ASSERT_MSG(ReproveShufpsImmTie(inst),
                   "SHUFPS imm tie proof diverged at IR {}", inst->Id());
        __ Ins(result.V2D(), 1, right.V2D(), 1);
        return;
    }

    u64 index_lo = 0;
    u64 index_hi = 0;
    for (u32 byte = 0; byte < 16; ++byte) {
        const u32 lane = byte / 4;
        const u32 table_base = lane < 2 ? 0 : 16;
        const u8 index =
                u8(table_base + ((control >> (lane * 2)) & 3) * 4 + (byte & 3));
        auto& half = byte < 8 ? index_lo : index_hi;
        half |= u64(index) << ((byte & 7) * 8);
    }

    VRegister table0;
    VRegister table1;
    if (context.TryGetConsecutiveTmpV2(table0, table1)) {
        auto indexes = context.GetTmpV();
        auto tmp = context.GetTmpX();
        __ Orr(table0.V16B(), left.V16B(), left.V16B());
        __ Orr(table1.V16B(), right.V16B(), right.V16B());
        __ Mov(tmp, index_lo);
        __ Fmov(indexes.D(), tmp);
        __ Mov(tmp, index_hi);
        __ Ins(indexes.V2D(), 1, tmp);
        __ Tbl(result.V16B(), table0.V16B(), table1.V16B(), indexes.V16B());
        return;
    }

    // Extremely fragmented high-pressure fallback: two arbitrary scratch
    // registers are enough for TBL(left) followed by TBX(right). It has the
    // same 32-byte table semantics and still never leaves generated code.
    auto indexes = context.GetTmpV();
    auto sixteen = context.GetTmpV();
    auto tmp = context.GetTmpX();
    __ Mov(tmp, index_lo);
    __ Fmov(indexes.D(), tmp);
    __ Mov(tmp, index_hi);
    __ Ins(indexes.V2D(), 1, tmp);
    __ Tbl(result.V16B(), left.V16B(), indexes.V16B());
    __ Movi(sixteen.V16B(), 16);
    __ Sub(indexes.V16B(), indexes.V16B(), sixteen.V16B());
    __ Tbx(result.V16B(), right.V16B(), indexes.V16B());
}

bool JitTranslator::ReprovePshufd4eExtConstant(ir::Inst* inst) const {
    if (!context.GetFeatures().pshufd_4e_ext || !inst ||
        inst->GetOp() != ir::OpCode::VecLoadConst ||
        !context.IsPshufd4eExt(inst->Id()) ||
        inst->GetArg<ir::Imm>(0).Get() != 0x0f0e0d0c0b0a0908ull ||
        inst->GetArg<ir::Imm>(1).Get() != 0x0706050403020100ull) {
        return false;
    }

    u32 local_uses = 0;
    bool saw_shuffle = false;
    for (auto& consumer : cur_block->GetInstList()) {
        for (auto value : consumer.GetValues()) {
            if (value.Def() != inst) {
                continue;
            }
            ++local_uses;
            if (consumer.GetOp() != ir::OpCode::VecShuffle32Indexed ||
                consumer.GetArg<ir::Value>(1).Def() != inst ||
                consumer.GetArg<ir::Value>(0).Def() == inst ||
                !context.IsPshufd4eExt(consumer.Id())) {
                return false;
            }
            saw_shuffle = true;
        }
    }
    return saw_shuffle && local_uses == inst->GetUses();
}

bool JitTranslator::ReprovePshufd4eExtShuffle(ir::Inst* inst) const {
    if (!context.GetFeatures().pshufd_4e_ext || !inst ||
        inst->GetOp() != ir::OpCode::VecShuffle32Indexed ||
        !context.IsPshufd4eExt(inst->Id())) {
        return false;
    }
    auto indexes = inst->GetArg<ir::Value>(1);
    return indexes.Defined() && indexes.Def() &&
           context.IsPshufd4eExt(indexes.Id()) &&
           ReprovePshufd4eExtConstant(indexes.Def());
}

void JitTranslator::EmitVecLoadConst(ir::Inst* inst) {
    if (context.IsPshufd4eExt(inst->Id())) {
        ASSERT_MSG(ReprovePshufd4eExtConstant(inst),
                   "PSHUFD 0x4e constant proof diverged at IR {}", inst->Id());
        return;
    }
    auto result = context.V(ir::Value{inst});
    auto tmp = context.GetTmpX();
    const u64 low = inst->GetArg<ir::Imm>(0).Get();
    const u64 high = inst->GetArg<ir::Imm>(1).Get();
    // JitContext sizes the code-cache allocation before FinalizeCode() emits
    // VIXL's pending literal pool.  A literal LDR here therefore makes the
    // finalized unit larger than its allocation and truncates/corrupts the
    // pool.  Keep the block-local SSA cache, but materialize its one canonical
    // table with the established PIC-safe sequence until code-cache sizing can
    // account for deferred pools.
    __ Mov(tmp, low);
    __ Fmov(result.D(), tmp);
    __ Mov(tmp, high);
    __ Ins(result.V2D(), 1, tmp);
}

void JitTranslator::EmitVecShuffle32Indexed(ir::Inst* inst) {
    auto src = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    if (context.IsPshufd4eExt(inst->Id())) {
        ASSERT_MSG(ReprovePshufd4eExtShuffle(inst),
                   "PSHUFD 0x4e EXT proof diverged at IR {}", inst->Id());
        __ Ext(result.V16B(), src.V16B(), src.V16B(), 8);
        return;
    }
    auto indexes = context.V(inst->GetArg<ir::Value>(1));
    __ Tbl(result.V16B(), src.V16B(), indexes.V16B());
}

void JitTranslator::EmitVecSharedZero(ir::Inst* inst) {
    auto result = context.V(ir::Value{inst});
    __ Eor(result.V16B(), result.V16B(), result.V16B());
}

void JitTranslator::EmitVecShuffle16(ir::Inst* inst) {
    auto src = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    auto indexes = context.GetTmpV();
    auto tmp = context.GetTmpX();
    const u32 control = inst->GetArg<ir::Imm>(1).Get();
    const u32 base_lane = inst->GetArg<ir::Imm>(2).Get() ? 4 : 0;
    u64 index_lo = 0;
    u64 index_hi = 0;
    for (u32 byte = 0; byte < 16; ++byte) {
        const u32 lane = byte / 2;
        u32 selected_lane = lane;
        if (lane >= base_lane && lane < base_lane + 4) {
            selected_lane = base_lane + ((control >> ((lane - base_lane) * 2)) & 3);
        }
        const u8 index = selected_lane * 2 + (byte & 1);
        auto& half = byte < 8 ? index_lo : index_hi;
        half |= u64(index) << ((byte & 7) * 8);
    }
    __ Mov(tmp, index_lo);
    __ Fmov(indexes.D(), tmp);
    __ Mov(tmp, index_hi);
    __ Ins(indexes.V2D(), 1, tmp);
    __ Tbl(result.V16B(), src.V16B(), indexes.V16B());
}

void JitTranslator::EmitVecZip(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const bool high = inst->GetArg<ir::Imm>(3).Get() != 0;
    switch (inst->GetArg<ir::Imm>(2).Get()) {
        case 8:
            if (high)
                __ Zip2(result.V16B(), left.V16B(), right.V16B());
            else
                __ Zip1(result.V16B(), left.V16B(), right.V16B());
            break;
        case 16:
            if (high)
                __ Zip2(result.V8H(), left.V8H(), right.V8H());
            else
                __ Zip1(result.V8H(), left.V8H(), right.V8H());
            break;
        case 32:
            if (high)
                __ Zip2(result.V4S(), left.V4S(), right.V4S());
            else
                __ Zip1(result.V4S(), left.V4S(), right.V4S());
            break;
        case 64:
            if (high)
                __ Zip2(result.V2D(), left.V2D(), right.V2D());
            else
                __ Zip1(result.V2D(), left.V2D(), right.V2D());
            break;
        default:
            PANIC("invalid vector lane width");
    }
}

void JitTranslator::EmitVecDupPairs32(ir::Inst* inst) {
    auto src = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    if (inst->GetArg<ir::Imm>(1).Get()) {
        __ Trn2(result.V4S(), src.V4S(), src.V4S());
    } else {
        __ Trn1(result.V4S(), src.V4S(), src.V4S());
    }
}

void JitTranslator::EmitVecDup64(ir::Inst* inst) {
    auto src = context.X(inst->GetArg<ir::Value>(0));
    auto result = context.V(ir::Value{inst});
    __ Dup(result.V2D(), src);
}

void JitTranslator::EmitVecExtract64(ir::Inst* inst) {
    auto src = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.X(ir::Value{inst});
    const u32 lane = inst->GetArg<ir::Imm>(1).Get() & 1;
    __ Umov(result, src.V2D(), lane);
}

void JitTranslator::EmitVecExtract16(ir::Inst* inst) {
    auto src = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.W(ir::Value{inst});
    const u32 lane = inst->GetArg<ir::Imm>(1).Get() & 7;
    __ Umov(result, src.V8H(), lane);
}

void JitTranslator::EmitVecInsert16(ir::Inst* inst) {
    auto dest = context.V(inst->GetArg<ir::Value>(0));
    auto value = context.W(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const u32 lane = inst->GetArg<ir::Imm>(2).Get() & 7;
    __ Orr(result.V16B(), dest.V16B(), dest.V16B());
    __ Ins(result.V8H(), lane, value);
}

void JitTranslator::EmitVecMovMask(ir::Inst* inst) {
    auto src = context.V(inst->GetArg<ir::Value>(0));
    auto result = context.W(ir::Value{inst});
    const u32 lane_bits = inst->GetArg<ir::Imm>(1).Get();
    if (lane_bits == 8) {
        auto packed = context.GetTmpV();
        auto work = context.GetTmpV();
        __ Ushr(work.V16B(), src.V16B(), 7);
        __ Uzp1(packed.V16B(), work.V16B(), work.V16B());
        __ Uzp2(work.V16B(), work.V16B(), work.V16B());
        __ Sli(packed.V16B(), work.V16B(), 1);
        __ Uzp2(work.V16B(), packed.V16B(), packed.V16B());
        __ Uzp1(packed.V16B(), packed.V16B(), packed.V16B());
        __ Sli(packed.V16B(), work.V16B(), 2);
        __ Uzp2(work.V16B(), packed.V16B(), packed.V16B());
        __ Uzp1(packed.V16B(), packed.V16B(), packed.V16B());
        __ Sli(packed.V16B(), work.V16B(), 4);
        __ Umov(result, packed.V8H(), 0);
        return;
    }
    if (lane_bits == 32) {
        auto work = context.GetTmpV();
        auto shifts = context.GetTmpV();
        __ Ushr(work.V4S(), src.V4S(), 31);
        __ Movi(shifts.V16B(), 0x0000000300000002ull, 0x0000000100000000ull);
        __ Ushl(work.V4S(), work.V4S(), shifts.V4S());
        __ Addv(work.S(), work.V4S());
        __ Umov(result, work.V4S(), 0);
        return;
    }
    if (lane_bits == 64) {
        auto work = context.GetTmpV();
        auto packed = context.GetTmpX();
        __ Uzp2(work.V4S(), src.V4S(), src.V4S());
        __ Umov(packed, work.V2D(), 0);
        __ Bfi(packed, packed, 31, 32);
        __ Lsr(packed, packed, 62);
        __ Mov(result, packed.W());
        return;
    }
    PANIC("invalid vector movmask lane width");
}

void JitTranslator::EmitVecTableLookup8(ir::Inst* inst) {
    auto table = context.V(inst->GetArg<ir::Value>(0));
    auto control = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    auto indexes = context.GetTmpV();
    __ Movi(indexes.V16B(), 0x8F);
    __ And(indexes.V16B(), control.V16B(), indexes.V16B());
    __ Tbl(result.V16B(), table.V16B(), indexes.V16B());
}

// De-interleave {left, right}, keeping the even (UZP1) or odd (UZP2) lanes.
// At 64-bit lanes UZP1/UZP2 coincide with ZIP1/ZIP2; the opcode still accepts
// that width so a caller does not have to special-case it.
void JitTranslator::EmitVecUnzip(ir::Inst* inst) {
    auto left = context.V(inst->GetArg<ir::Value>(0));
    auto right = context.V(inst->GetArg<ir::Value>(1));
    auto result = context.V(ir::Value{inst});
    const bool odd = inst->GetArg<ir::Imm>(3).Get() != 0;
    switch (inst->GetArg<ir::Imm>(2).Get()) {
        case 8:
            if (odd)
                __ Uzp2(result.V16B(), left.V16B(), right.V16B());
            else
                __ Uzp1(result.V16B(), left.V16B(), right.V16B());
            break;
        case 16:
            if (odd)
                __ Uzp2(result.V8H(), left.V8H(), right.V8H());
            else
                __ Uzp1(result.V8H(), left.V8H(), right.V8H());
            break;
        case 32:
            if (odd)
                __ Uzp2(result.V4S(), left.V4S(), right.V4S());
            else
                __ Uzp1(result.V4S(), left.V4S(), right.V4S());
            break;
        case 64:
            if (odd)
                __ Uzp2(result.V2D(), left.V2D(), right.V2D());
            else
                __ Uzp1(result.V2D(), left.V2D(), right.V2D());
            break;
        default:
            PANIC("invalid vector lane width");
    }
}


#undef __

}  // namespace swift::runtime::backend::arm64
