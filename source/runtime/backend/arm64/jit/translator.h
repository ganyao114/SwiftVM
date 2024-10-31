#pragma once

#include "base/common_funcs.h"
#include "jit_context.h"
#include "runtime/backend/code_cache.h"
#include "runtime/common/types.h"
#include "runtime/include/config.h"
#include "runtime/ir/block.h"

namespace swift::runtime::backend::arm64 {

namespace HostFlagsBit {
    constexpr auto N = 31;
    constexpr auto Z = 30;
    constexpr auto C = 29;
    constexpr auto V = 28;
    constexpr auto Parity = 27;
    constexpr auto AuxiliaryCarry = 26;
    constexpr auto ParityByte = 0;
    constexpr auto AFLeft = 11;
    constexpr auto AFRight = 15;
    constexpr u64 ParityByteMask = u64(0xF) << ParityByte;
}

enum class HostFlags : u64 {
    N = 1u << HostFlagsBit::N,
    Z = 1u << HostFlagsBit::Z,
    C = 1u << HostFlagsBit::C,
    V = 1u << HostFlagsBit::V,
    NZCV = N | Z | C | V,
    NZ = N | Z,
};

DECLARE_ENUM_FLAG_OPERATORS(HostFlags)

class JitTranslator {
public:
    explicit JitTranslator(JitContext& ctx);

    void Translate(ir::Block *block);

    void Translate(ir::HIRFunction *function);

    Operand EmitOperand(ir::Operand &ir_op);

    MemOperand EmitMemOperand(ir::Operand &ir_op, ir::ValueType type, bool pair = false);

#define INST(name, ...) void Emit##name(ir::Inst *inst);
#include "runtime/ir/ir.inc"
#undef INST

private:

    struct PseudoFlags {
        ir::Flags set{};
        ir::Flags clear{};

        [[nodiscard]] bool Null() const {
            return set == ir::Flags::None && clear == ir::Flags::None;
        }

        [[nodiscard]] bool IsNZCV() const {
            return True(set & ir::Flags::NZCV);
        }

        [[nodiscard]] bool IsCV() const {
            return True(set & ir::Flags::CV) && False(set & ir::Flags::NZ) && False(clear & ir::Flags::NZ);
        }

        [[nodiscard]] bool IsNZ_ZeroCV() const {
            return True(set & ir::Flags::NZ) && True(clear & ir::Flags::CV);
        }
    };

    void Translate(ir::Inst *inst);

    // Flags
    void SaveHostFlags(HostFlags host, ir::Flags guest);

    void ClearFlags(ir::Flags flags);

    void SaveParity(Register &value);

    void SaveNZ(Register &value, ir::ValueType type);

    void SaveCV(Register &value, ir::ValueType type);

    void SaveOF(Register &value, ir::ValueType type);

    void SaveAuxiliaryCarry(Register &left, Register &result);

    void GetParityFlag(Register &result);

    void TestParityFlag(Register &result);

    void TestAuxiliaryCarry(Register &result);

    [[nodiscard]] PseudoFlags GetPseudoFlags(ir::Inst *inst);

    [[nodiscard]] bool MatchMemoryOffsetCase(ir::Inst *inst);

    void FlushFlags();

    JitContext &context;
    MacroAssembler &masm;
    ir::Block *cur_block{};
    ir::Inst *cur_instr{};
    BitVector disable_instructions{};
    ir::Flags flags_set{};
    ir::Flags flags_clear{};
    bool save_in_nzcv{true};
    bool nzcv_dirty{false};
};

}