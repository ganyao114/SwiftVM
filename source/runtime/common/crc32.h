#pragma once

#include <array>
#include <stdexcept>
#include "runtime/ir/instr.h"

namespace swift::runtime {

// Raw reflected IEEE CRC-32; neither seed nor result is complemented.
inline constexpr auto ieee_crc32_table = [] {
    std::array<u32, 256> table{};
    for (u32 index = 0; index < table.size(); ++index) {
        u32 value = index;
        for (u32 bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ (0xEDB88320u & (0u - (value & 1u)));
        table[index] = value;
    }
    return table;
}();

inline u32 IeeeCrcInputBits(ir::Value value) {
    const u32 view = ir::GetValueSizeByte(value.Type()) * 8;
    const u32 producer = ir::GetValueSizeByte(value.Def()->ReturnType()) * 8;
    return view < producer ? view : producer;
}

inline u32 ValidateIeeeCrc32(ir::Inst* inst) {
    if (inst->ReturnType() != ir::ValueType::U32 || !inst->ArgAt(2).IsImm())
        throw std::runtime_error("Crc32 requires a U32 result and immediate width");
    // Check the complete immediate before narrowing it to the emitter width.
    const u64 width = inst->GetArg<ir::Imm>(2).Get();
    if (width != 8 && width != 16 && width != 32 && width != 64)
        throw std::runtime_error("Crc32 width must be 8,16,32,64");
    const auto scalar = [](ir::ValueType type) { return type >= ir::ValueType::U8 && type <= ir::ValueType::S64; };
    for (u32 index = 0; index < 2; ++index) {
        if (!inst->ArgAt(index).IsValue()) throw std::runtime_error("Crc32 requires scalar inputs");
        const auto value = inst->GetArg<ir::Value>(index);
        if (!value.Defined() || !scalar(value.Type()) || !scalar(value.Def()->ReturnType()))
            throw std::runtime_error("Crc32 requires defined scalar producers and scalar input views");
    }
    return u32(width);
}

}  // namespace swift::runtime
