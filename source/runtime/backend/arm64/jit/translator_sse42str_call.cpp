#include "translator.h"

#include "runtime/backend/arm64/defines.h"
#include "runtime/frontend/x86/sse42str_helper.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

void JitTranslator::EmitSse42StrVectorCall(ir::Inst* inst,
                                           const VRegister& left,
                                           const VRegister& right,
                                           const WRegister& result,
                                           VAddr target,
                                           u8 imm) {
    using ABI = swift::x86::Sse42StrVectorCallABI;
    ASSERT(ABI::Supports(imm));

    auto live_gprs = context.GetLiveGPRs();
    live_gprs.Clear(result.GetCode());
    boost::container::small_vector<u32, 8> save_gprs;
    for (u32 code = 0; code < 32; ++code) {
        if ((ABI::GPRClobbers(imm) & (1u << code)) && live_gprs.Get(code)) {
            save_gprs.push_back(code);
        }
    }

    auto live_fprs = context.GetLiveFPRs();
    const bool left_live_after =
            context.ValueLiveAfter(inst->GetArg<ir::Value>(0));
    const bool right_live_after =
            context.ValueLiveAfter(inst->GetArg<ir::Value>(1));
    if (!left_live_after &&
        (left.GetCode() != right.GetCode() || !right_live_after)) {
        live_fprs.Clear(left.GetCode());
    }
    if (!right_live_after &&
        (right.GetCode() != left.GetCode() || !left_live_after)) {
        live_fprs.Clear(right.GetCode());
    }
    const u32 fpr_clobbers =
            ABI::FPRClobbers(imm) | ABI::ArgumentFPRClobbers;
    boost::container::small_vector<u32, 8> save_fprs;
    for (u32 code = 0; code < 8; ++code) {
        if ((fpr_clobbers & (1u << code)) && live_fprs.Get(code)) {
            save_fprs.push_back(code);
        }
    }

    std::array<int, 32> gpr_slots{};
    std::array<int, 8> fpr_slots{};
    gpr_slots.fill(-1);
    fpr_slots.fill(-1);
    u32 cursor{};
    for (u32 code : save_gprs) {
        gpr_slots[code] = int(cursor);
        cursor += 8;
    }
    const bool preserve_helper_result =
            std::find(save_gprs.begin(), save_gprs.end(), ABI::ResultGPR) !=
            save_gprs.end();
    int result_slot = -1;
    if (preserve_helper_result) {
        result_slot = int(cursor);
        cursor += 8;
    }
    const u32 simd_offset = (cursor + 15u) & ~15u;
    cursor = simd_offset;
    for (u32 code : save_fprs) {
        fpr_slots[code] = int(cursor);
        cursor += 16;
    }
    const u32 save_bytes = (cursor + 15u) & ~15u;

    const size_t paired_gprs = save_gprs.size() & ~size_t{1};
    const bool fpr_frame_anchor =
            save_gprs.empty() && !save_fprs.empty();
    if (paired_gprs) {
        __ Stp(XRegister(save_gprs[0]),
               XRegister(save_gprs[1]),
               MemOperand(sp, -static_cast<s64>(save_bytes), PreIndex));
    } else if (!save_gprs.empty()) {
        __ Str(XRegister(save_gprs.front()),
               MemOperand(sp, -static_cast<s64>(save_bytes), PreIndex));
    } else if (fpr_frame_anchor) {
        __ Str(VRegister::GetQRegFromCode(save_fprs.front()),
               MemOperand(sp, -static_cast<s64>(save_bytes), PreIndex));
    } else {
        ASSERT(save_bytes == 0);
    }
    for (size_t i = 2; i < paired_gprs; i += 2) {
        __ Stp(XRegister(save_gprs[i]),
               XRegister(save_gprs[i + 1]),
               MemOperand(sp, gpr_slots[save_gprs[i]]));
    }
    if (save_gprs.size() > 1 && (save_gprs.size() & 1u)) {
        __ Str(XRegister(save_gprs.back()),
               MemOperand(sp, gpr_slots[save_gprs.back()]));
    }
    size_t fpr_index = fpr_frame_anchor ? 1 : 0;
    for (; fpr_index + 1 < save_fprs.size(); fpr_index += 2) {
        __ Stp(VRegister::GetQRegFromCode(save_fprs[fpr_index]),
               VRegister::GetQRegFromCode(save_fprs[fpr_index + 1]),
               MemOperand(sp, fpr_slots[save_fprs[fpr_index]]));
    }
    if (fpr_index < save_fprs.size()) {
        __ Str(VRegister::GetQRegFromCode(save_fprs[fpr_index]),
               MemOperand(sp, fpr_slots[save_fprs[fpr_index]]));
    }

    auto load_argument = [&](const VRegister& destination,
                             const VRegister& source) {
        if (destination.GetCode() == source.GetCode()) {
            return;
        }
        if (source.GetCode() < 8 && fpr_slots[source.GetCode()] >= 0) {
            __ Ldr(destination,
                   MemOperand(sp, fpr_slots[source.GetCode()]));
        } else {
            __ Orr(destination.V16B(), source.V16B(), source.V16B());
        }
    };
    if (left.GetCode() == 1 && right.GetCode() == 0) {
        __ Orr(v7.V16B(), v0.V16B(), v0.V16B());
        __ Orr(v0.V16B(), v1.V16B(), v1.V16B());
        __ Orr(v1.V16B(), v7.V16B(), v7.V16B());
    } else if (right.GetCode() == 0 && left.GetCode() != 0) {
        load_argument(v1.Q(), right);
        load_argument(v0.Q(), left);
    } else {
        load_argument(v0.Q(), left);
        load_argument(v1.Q(), right);
    }

    const ir::Lambda lambda{
            ir::DataClass{ir::Imm{target}},
            ir::HelperCallTraits{
                    .uniform = ir::UniformEffectId::None,
                    .host_fp = ir::HostFpEffect::FPCRTransparent,
            }};
    Label resume;
    __ Adr(x17, &resume);
    if (!TryEmitSharedHostBranch(lambda)) {
        MaterializeHostCallTarget(target);
        __ Br(ip);
    }
    __ Bind(&resume);
    if (preserve_helper_result) {
        __ Str(WRegister(ABI::ResultGPR), MemOperand(sp, result_slot));
    } else if (result.GetCode() != ABI::ResultGPR) {
        __ Mov(result, WRegister(ABI::ResultGPR));
    }

    fpr_index = fpr_frame_anchor ? 1 : 0;
    for (; fpr_index + 1 < save_fprs.size(); fpr_index += 2) {
        __ Ldp(VRegister::GetQRegFromCode(save_fprs[fpr_index]),
               VRegister::GetQRegFromCode(save_fprs[fpr_index + 1]),
               MemOperand(sp, fpr_slots[save_fprs[fpr_index]]));
    }
    if (fpr_index < save_fprs.size()) {
        __ Ldr(VRegister::GetQRegFromCode(save_fprs[fpr_index]),
               MemOperand(sp, fpr_slots[save_fprs[fpr_index]]));
    }
    for (size_t i = 2; i < paired_gprs; i += 2) {
        __ Ldp(XRegister(save_gprs[i]),
               XRegister(save_gprs[i + 1]),
               MemOperand(sp, gpr_slots[save_gprs[i]]));
    }
    if (save_gprs.size() > 1 && (save_gprs.size() & 1u)) {
        __ Ldr(XRegister(save_gprs.back()),
               MemOperand(sp, gpr_slots[save_gprs.back()]));
    }
    if (preserve_helper_result) {
        __ Ldr(result, MemOperand(sp, result_slot));
    }
    if (paired_gprs) {
        __ Ldp(XRegister(save_gprs[0]),
               XRegister(save_gprs[1]),
               MemOperand(sp, static_cast<s64>(save_bytes), PostIndex));
    } else if (!save_gprs.empty()) {
        __ Ldr(XRegister(save_gprs.front()),
               MemOperand(sp, static_cast<s64>(save_bytes), PostIndex));
    } else if (fpr_frame_anchor) {
        __ Ldr(VRegister::GetQRegFromCode(save_fprs.front()),
               MemOperand(sp, static_cast<s64>(save_bytes), PostIndex));
    }

    flag_state.flags_set = ir::Flags::None;
    flag_state.flags_clear = ir::Flags::None;
    flag_state.nzcv_dirty = false;
    flag_state.nzcv_requested = {};
    InvalidateFlagsToken();
}

#undef __

}  // namespace swift::runtime::backend::arm64
