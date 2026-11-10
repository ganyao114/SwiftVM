#include "catalog.h"

#include "base/error.h"

#include <algorithm>

namespace swift::tests::correctness {
namespace {

constexpr Form forms[]{
        {"x86_64/add/op01/rm64-r64/legacy",
         ISA::X86_64,
         "integer",
         Encoding::X86Register,
         0x01,
         0xff,
         64,
         3},
        {"x86_64/add/op01/rm32-r32/legacy",
         ISA::X86_64,
         "integer",
         Encoding::X86Register,
         0x01,
         0xff,
         32,
         2},
        {"x86_64/sub/op29/rm64-r64/legacy",
         ISA::X86_64,
         "integer",
         Encoding::X86Register,
         0x29,
         0xff,
         64,
         3},
        {"x86_64/adc/op11/rm64-r64/legacy",
         ISA::X86_64,
         "flags",
         Encoding::X86Register,
         0x11,
         0xff,
         64,
         3},
        {"x86_64/sbb/op19/rm64-r64/legacy",
         ISA::X86_64,
         "flags",
         Encoding::X86Register,
         0x19,
         0xff,
         64,
         3},
        {"x86_64/xor/op31/rm64-r64/legacy",
         ISA::X86_64,
         "integer",
         Encoding::X86Register,
         0x31,
         0xff,
         64,
         3},
        {"x86_64/mov/op88/rm8-r8/legacy",
         ISA::X86_64,
         "alias",
         Encoding::X86Register,
         0x88,
         0xff,
         8,
         2},
        {"x86_64/shl/opd3/rm64-cl", ISA::X86_64, "shift", Encoding::X86ShiftCL, 0xd3, 0xff, 64, 3},
        {"x86_64/shl/opd3/rm32-cl", ISA::X86_64, "shift", Encoding::X86ShiftCL, 0xd3, 0xff, 32, 2},
        {"x86_64/mov/op8b/r64-m64", ISA::X86_64, "memory", Encoding::X86Memory, 0x8b, 0xff, 64, 3},
        {"x86_64/mov/op89/m64-r64", ISA::X86_64, "memory", Encoding::X86Memory, 0x89, 0xff, 64, 3},
        {"x86_64/addsd/opf2-0f58/xmm-xmm",
         ISA::X86_64,
         "fp",
         Encoding::X86ScalarFP,
         0x58,
         0xff,
         64,
         4},
        {"x86_64/paddb/op66-0ffc/xmm-xmm",
         ISA::X86_64,
         "vector",
         Encoding::X86Vector,
         0xfc,
         0xff,
         8,
         4},
        {"a64/addsub_shifted/adds/x64",
         ISA::Arm64,
         "integer",
         Encoding::A64Shifted,
         0xab000000,
         0xff200000,
         64,
         4},
        {"a64/addsub_shifted/adds/w32",
         ISA::Arm64,
         "integer",
         Encoding::A64Shifted,
         0x2b000000,
         0xff200000,
         32,
         4},
        {"a64/addsub_shifted/subs/x64",
         ISA::Arm64,
         "flags",
         Encoding::A64Shifted,
         0xeb000000,
         0xff200000,
         64,
         4},
        {"a64/addsub_carry/adc/x64",
         ISA::Arm64,
         "flags",
         Encoding::A64Register,
         0x9a000000,
         0xffe0fc00,
         64,
         4},
        {"a64/shift_register/lslv/x64",
         ISA::Arm64,
         "shift",
         Encoding::A64Register,
         0x9ac02000,
         0xffe0fc00,
         64,
         4},
        {"a64/shift_register/lslv/w32",
         ISA::Arm64,
         "shift",
         Encoding::A64Register,
         0x1ac02000,
         0xffe0fc00,
         32,
         4},
        {"a64/fp_two_source/fadd/d64",
         ISA::Arm64,
         "fp",
         Encoding::A64ScalarFP,
         0x1e602800,
         0xffe0fc00,
         64,
         4},
        {"a64/neon_three_same/add/v16b",
         ISA::Arm64,
         "vector",
         Encoding::A64Vector,
         0x4e208400,
         0xffe0fc00,
         8,
         4},
        {"a64/load_unsigned/ldr/x64",
         ISA::Arm64,
         "memory",
         Encoding::A64Memory,
         0xf9400000,
         0xfffffc00,
         64,
         4},
        {"a64/store_unsigned/str/x64",
         ISA::Arm64,
         "memory",
         Encoding::A64Memory,
         0xf9000000,
         0xfffffc00,
         64,
         4},
        {"a32/arm/add_reg/S1",
         ISA::Arm32,
         "integer",
         Encoding::A32Register,
         0x00900000,
         0x0ff00010,
         32,
         4},
        {"a32/arm/adc_reg/S1",
         ISA::Arm32,
         "flags",
         Encoding::A32Register,
         0x00b00000,
         0x0ff00010,
         32,
         4},
        {"a32/arm/sub_reg/S1",
         ISA::Arm32,
         "integer",
         Encoding::A32Register,
         0x00500000,
         0x0ff00010,
         32,
         4},
        {"a32/arm/lsl_reg/S1",
         ISA::Arm32,
         "shift",
         Encoding::A32ShiftRegister,
         0x01b00010,
         0x0fff00f0,
         32,
         4},
        {"a32/arm/vadd/d64",
         ISA::Arm32,
         "fp",
         Encoding::A32ScalarFP,
         0xee300b00,
         0xffb00f50,
         64,
         4},
        {"a32/arm/neon_add/q16b",
         ISA::Arm32,
         "vector",
         Encoding::A32Vector,
         0xf2000840,
         0xffb00f50,
         8,
         4},
        {"a32/arm/ldr/word_offset0",
         ISA::Arm32,
         "memory",
         Encoding::A32Memory,
         0x05900000,
         0x0ff00fff,
         32,
         4},
        {"a32/arm/str/word_offset0",
         ISA::Arm32,
         "memory",
         Encoding::A32Memory,
         0x05800000,
         0x0ff00fff,
         32,
         4},
        {"a32/thumb16/add_reg_t1",
         ISA::Arm32,
         "integer",
         Encoding::Thumb16Register,
         0x1800,
         0xfe00,
         32,
         2},
        {"a32/thumb16/sub_reg_t1",
         ISA::Arm32,
         "integer",
         Encoding::Thumb16Register,
         0x1a00,
         0xfe00,
         32,
         2},
        {"a32/thumb16/lsl_reg_t1",
         ISA::Arm32,
         "shift",
         Encoding::Thumb16Shift,
         0x4080,
         0xffc0,
         32,
         2},
        {"a32/thumb32/add_reg_t3/S1",
         ISA::Arm32,
         "integer",
         Encoding::Thumb32Register,
         0xeb100000,
         0xfff080f0,
         32,
         4},
};

std::uint32_t Word(const Case& c) {
    std::uint32_t word{};
    for (std::size_t i = 0; i < c.code.size(); ++i) word |= std::uint32_t(c.code[i]) << (i * 8);
    return word;
}

bool Thumb(Encoding encoding) {
    return encoding == Encoding::Thumb16Register || encoding == Encoding::Thumb16Shift ||
           encoding == Encoding::Thumb32Register;
}

}  // namespace

std::span<const Form> Catalog() { return forms; }

const Form* FindForm(std::string_view id) {
    const auto found = std::find_if(
            std::begin(forms), std::end(forms), [id](const Form& f) { return f.id == id; });
    return found == std::end(forms) ? nullptr : found;
}

bool ValidateCase(const Case& c, std::string* reason) {
    const auto reject = [&](const char* message) {
        if (reason) *reason = message;
        return false;
    };
    const auto* form = FindForm(c.form_id);
    if (!form || form->isa != c.isa) return reject("unreviewed form or ISA mismatch");
    const auto profile = c.isa == ISA::X86_64  ? "x86_64-user"
                         : c.isa == ISA::Arm64 ? "a64-user"
                                               : "armv7-user";
    if (c.profile != profile) return reject("unsupported profile");
    if (c.code.size() != form->length || c.code_address > UINT64_MAX - c.code.size() ||
        c.terminal_pc != c.code_address + c.code.size() || c.initial.pc != c.code_address ||
        !c.max_steps)
        return reject("single-instruction boundary mismatch");
    if (c.isa == ISA::X86_64) {
        const auto offset = form->width == 64 && form->encoding != Encoding::X86ScalarFP ? 1u : 0u;
        if (offset && c.code.front() != 0x48) return reject("unexpected x86 prefix");
        if (form->encoding == Encoding::X86ScalarFP || form->encoding == Encoding::X86Vector) {
            if (c.code[0] != (form->encoding == Encoding::X86ScalarFP ? 0xf2 : 0x66) ||
                c.code[1] != 0x0f || c.code[2] != form->bits || (c.code[3] & 0xc0) != 0xc0)
                return reject("unexpected SSE encoding");
        } else {
            if (c.code[offset] != form->bits) return reject("unexpected x86 opcode");
            const auto modrm = c.code[offset + 1];
            if (form->encoding == Encoding::X86Memory) {
                if ((modrm & 0xc0) || (modrm & 7) == 4 || (modrm & 7) == 5)
                    return reject("memory form requires a plain base register");
            } else if ((modrm & 0xc0) != 0xc0 ||
                       (form->encoding == Encoding::X86ShiftCL && (modrm & 0x38) != 0x20))
                return reject("unexpected x86 register encoding");
        }
        if (c.initial.fp_control & ~0xffffu) return reject("reserved MXCSR input");
        if ((c.initial.flags & ~UINT64_C(0xed7)) || (c.initial.flags & 0x202) != 0x202)
            return reject("unsupported RFLAGS control/reserved input");
        return true;
    }
    auto word = Word(c);
    if (form->encoding == Encoding::Thumb32Register) word = (word << 16) | (word >> 16);
    if ((word & form->mask) != form->bits) return reject("encoding does not match form");
    if (c.isa == ISA::Arm64) {
        if (c.code_address & 3) return reject("unaligned A64 instruction");
        if ((c.initial.flags & ~UINT64_C(0xf0000000)) || (c.initial.fp_status & ~0x0800009fu))
            return reject("reserved NZCV/FPSR input");
        if (form->encoding == Encoding::A64Shifted &&
            (((word >> 22) & 3) == 3 || ((word >> 10) & 63) >= form->width))
            return reject("reserved A64 shift");
        if (c.initial.fp_control & ~0x07c00000u) return reject("unsupported FPCR input");
        return true;
    }
    const bool thumb = Thumb(form->encoding);
    if (bool(c.initial.flags & 32) != thumb || (c.initial.flags & 0x0600fe00) ||
        (c.initial.flags & 31) != 16 || (c.code_address & (thumb ? 1u : 3u)))
        return reject("unsupported ARM mode, IT, endian, or alignment");
    if (c.initial.fp_control & ~0xffc0009fu)
        return reject("unsupported FPSCR trap/short-vector controls");
    if (form->encoding == Encoding::A32Register || form->encoding == Encoding::A32Memory ||
        form->encoding == Encoding::A32ShiftRegister) {
        if ((word >> 28) == 15 || ((word >> 12) & 15) == 15 || ((word >> 16) & 15) == 15 ||
            (form->encoding != Encoding::A32Memory && (word & 15) == 15) ||
            (form->encoding == Encoding::A32ShiftRegister && ((word >> 8) & 15) == 15))
            return reject("PC operand outside the reviewed form subset");
    }
    if (form->encoding == Encoding::A32Vector &&
        ((word & 1) || (word & (1u << 12)) || (word & (1u << 16))))
        return reject("odd D register in a Q form");
    if (form->encoding == Encoding::Thumb32Register &&
        (((word >> 16) & 15) >= 13 || ((word >> 8) & 15) >= 13 || (word & 15) >= 13))
        return reject("SP/PC operand outside the reviewed Thumb-2 subset");
    return true;
}

ComparisonContract Contract(const Case& c) {
    std::string reason;
    if (!ValidateCase(c, &reason)) swift::ThrowError(reason, swift::ErrorKind::InvalidArgument);
    const auto& form = *FindForm(c.form_id);
    if (c.isa == ISA::Arm64) return {0xf0000000u, 0x07c00000u, 0x0800009fu};
    if (c.isa == ISA::Arm32) return {0xff0fffffu, 0xfff79f9fu, 0};
    ComparisonContract contract{0xed7, 0xffff, 0};
    if (form.bits == 0x31 && form.encoding == Encoding::X86Register)
        contract.flags_mask &= ~UINT64_C(0x10);  // XOR leaves AF undefined.
    if (form.encoding == Encoding::X86ShiftCL) {
        const auto count = c.initial.gpr[1] & (form.width == 64 ? 63 : 31);
        if (count) {
            contract.flags_mask &= ~UINT64_C(0x10);  // nonzero SHL leaves AF undefined.
            if (count != 1) contract.flags_mask &= ~UINT64_C(0x800);  // OF only at count 1.
        }
    }
    return contract;
}

}  // namespace swift::tests::correctness
