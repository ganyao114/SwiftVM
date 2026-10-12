#include "translator.h"

#include <stdexcept>

namespace swift::runtime::backend::x64 {
using namespace Xbyak;

void Translator::ReadVectorOperands(ir::Inst* inst) {
    ReadVector(xmm0, Source(inst, 0));
    ReadVector(xmm1, Source(inst, 1));
}

void Translator::LoadLane(const Reg64& reg, u32 offset, u32 bits) {
    if (bits == 8) movzx(reg.cvt32(), byte[r13 + offset]);
    else if (bits == 16) movzx(reg.cvt32(), word[r13 + offset]);
    else if (bits == 32) mov(reg.cvt32(), dword[r13 + offset]);
    else mov(reg, qword[r13 + offset]);
}

void Translator::StoreLane(const Reg64& reg, u32 offset, u32 bits) {
    if (bits == 8) mov(byte[r13 + offset], reg.cvt8());
    else if (bits == 16) mov(word[r13 + offset], reg.cvt16());
    else if (bits == 32) mov(dword[r13 + offset], reg.cvt32());
    else mov(qword[r13 + offset], reg);
}

void Translator::ValidateLaneWidth(u32 bits) {
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64)
        throw std::runtime_error("x64 vector lane width must be 8,16,32,64");
}

}  // namespace swift::runtime::backend::x64
