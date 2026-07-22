// Directed differential fuzz: generate x86-64 instruction sequences per
// instruction family, execute them on both Unicorn and SwiftVM, and compare
// GPRs, captured status flags (via lahf + seto), and scratch memory.

#include <catch2/catch_test_macros.hpp>
#include <unicorn/unicorn.h>
#include "unicorn_interface.h"
#include "translator/x86/translator.h"
#include "translator/x86/cpu.h"
#include "runtime/frontend/x86/decoder.h"
#include <random>
#include <iostream>
#include <cstring>
#include <sys/mman.h>
#include <fmt/format.h>

using namespace swift::test;
using namespace swift::translator::x86;
using namespace swift::x86;
using namespace swift;

namespace {

// =============================== x86-64 mini assembler ===============================

struct CodeBuf {
    std::vector<u8> c;

    void B(u8 v) { c.push_back(v); }
    void W(u16 v) { B(u8(v)); B(u8(v >> 8)); }
    void D(u32 v) {
        for (int i = 0; i < 4; i++) B(u8(v >> (8 * i)));
    }
    void Q(u64 v) {
        for (int i = 0; i < 8; i++) B(u8(v >> (8 * i)));
    }

    size_t Pos() const { return c.size(); }
    void Patch8(size_t at, s8 v) { c[at] = u8(v); }
    void Patch32(size_t at, s32 v) {
        for (int i = 0; i < 4; i++) c[at + i] = u8(v >> (8 * i));
    }
};

// Register ids follow the x86 encoding order.
constexpr u8 kRax = 0, kRcx = 1, kRdx = 2, kRbx = 3, kRsp = 4, kRbp = 5, kRsi = 6, kRdi = 7,
             kR8 = 8, kR9 = 9, kR10 = 10, kR11 = 11, kR12 = 12, kR13 = 13, kR14 = 14, kR15 = 15;

// Fixed harness roles: r13 = data pointer, r11 = index register, r15 = flag capture.
constexpr u8 kDataReg = kR13;
constexpr u8 kIndexReg = kR11;
constexpr u8 kCaptureReg = kR15;

void EmitRex(CodeBuf& b, bool w, bool r, bool x, bool bb, bool force = false) {
    u8 v = u8(0x40 | (w ? 8 : 0) | (r ? 4 : 0) | (x ? 2 : 0) | (bb ? 1 : 0));
    if (force || v != 0x40) {
        b.B(v);
    }
}

void EmitOperandPrefix(CodeBuf& b, int width) {
    if (width == 16) {
        b.B(0x66);
    }
}

void EmitModRMReg(CodeBuf& b, u8 reg_field, u8 rm_reg) {
    b.B(u8(0xC0 | ((reg_field & 7) << 3) | (rm_reg & 7)));
}

// Memory forms supported:
//  [kDataReg + disp]                      (mod 01 disp8 / mod 10 disp32)
//  [kDataReg + kIndexReg*scale + disp8]
//  [rip + disp32]
struct MemOp {
    s32 disp{0};
    u8 scale{0};  // 0 => no index, else 1/2/4/8 with kIndexReg
    bool rip_rel{false};
};

void EmitModRMMem(CodeBuf& b, u8 reg_field, const MemOp& m) {
    if (m.rip_rel) {
        b.B(u8((reg_field & 7) << 3 | 5));
        b.D(u32(m.disp));
        return;
    }
    if (m.scale == 0) {
        // base r13 requires a displacement
        if (m.disp >= -128 && m.disp <= 127) {
            b.B(u8(0x40 | ((reg_field & 7) << 3) | 5));
            b.B(u8(m.disp));
        } else {
            b.B(u8(0x80 | ((reg_field & 7) << 3) | 5));
            b.D(u32(m.disp));
        }
        return;
    }
    u8 scale_bits = m.scale == 1 ? 0 : (m.scale == 2 ? 1 : (m.scale == 4 ? 2 : 3));
    b.B(u8(0x40 | ((reg_field & 7) << 3) | 4));
    b.B(u8((scale_bits << 6) | ((kIndexReg & 7) << 3) | (kDataReg & 7)));
    b.B(u8(m.disp));
}

// rex bits for a reg/mem instruction; reg_field extended by R, rm (base) by B, index by X.
void EmitRexFor(CodeBuf& b, int width, u8 reg_field, bool has_mem, bool byte_op, bool high_byte) {
    if (byte_op) {
        // High byte registers (AH..BH) forbid any REX. Byte registers >= 4
        // (SPL..) or >= 8 require one.
        if (high_byte) {
            return;
        }
        bool need = width == 64 || reg_field >= 4;  // spl/bpl/sil/dil and r8b+ need REX
        EmitRex(b, width == 64, reg_field >= 8, false, has_mem && (kDataReg >= 8), need);
    } else {
        EmitRex(b, width == 64, reg_field >= 8, false, has_mem && (kDataReg >= 8));
    }
}

void EmitRexForRegReg(CodeBuf& b, int width, u8 reg_field, u8 rm_reg, bool byte_op, bool high_byte) {
    if (byte_op) {
        if (high_byte) {
            return;
        }
        bool need = width == 64 || reg_field >= 4 || rm_reg >= 4;
        EmitRex(b, width == 64, reg_field >= 8, false, rm_reg >= 8, need);
    } else {
        EmitRex(b, width == 64, reg_field >= 8, false, rm_reg >= 8);
    }
}

// ALU groups: 0=add 1=or 2=adc 3=sbb 4=and 5=sub 6=xor 7=cmp
void EmitAluRegReg(CodeBuf& b, u8 group, int width, u8 dst, u8 src, bool high_byte = false) {
    EmitOperandPrefix(b, width);
    bool byte_op = width == 8;
    EmitRexForRegReg(b, width, src, dst, byte_op, high_byte);
    b.B(u8(group * 8 + (byte_op ? 0 : 1)));
    EmitModRMReg(b, src, dst);
}

void EmitAluRegImm(CodeBuf& b, u8 group, int width, u8 dst, u64 imm, bool imm8_form = false) {
    EmitOperandPrefix(b, width);
    bool byte_op = width == 8;
    EmitRexForRegReg(b, width, 0, dst, byte_op, false);
    if (byte_op) {
        b.B(0x80);
        EmitModRMReg(b, group, dst);
        b.B(u8(imm));
    } else if (imm8_form) {
        b.B(0x83);
        EmitModRMReg(b, group, dst);
        b.B(u8(imm));
    } else {
        b.B(0x81);
        EmitModRMReg(b, group, dst);
        if (width == 16) {
            b.W(u16(imm));
        } else {
            b.D(u32(imm));
        }
    }
}

void EmitAluRegMem(CodeBuf& b, u8 group, int width, u8 dst, const MemOp& m) {
    EmitOperandPrefix(b, width);
    bool byte_op = width == 8;
    EmitRexFor(b, width, dst, true, byte_op, false);
    b.B(u8(group * 8 + (byte_op ? 2 : 3)));
    EmitModRMMem(b, dst, m);
}

void EmitAluMemReg(CodeBuf& b, u8 group, int width, const MemOp& m, u8 src) {
    EmitOperandPrefix(b, width);
    bool byte_op = width == 8;
    EmitRexFor(b, width, src, true, byte_op, false);
    b.B(u8(group * 8 + (byte_op ? 0 : 1)));
    EmitModRMMem(b, src, m);
}

void EmitTestRegReg(CodeBuf& b, int width, u8 dst, u8 src, bool high_byte = false) {
    EmitOperandPrefix(b, width);
    bool byte_op = width == 8;
    EmitRexForRegReg(b, width, src, dst, byte_op, high_byte);
    b.B(byte_op ? 0x84 : 0x85);
    EmitModRMReg(b, src, dst);
}

void EmitTestRegImm(CodeBuf& b, int width, u8 dst, u64 imm) {
    EmitOperandPrefix(b, width);
    bool byte_op = width == 8;
    EmitRexForRegReg(b, width, 0, dst, byte_op, false);
    b.B(byte_op ? 0xF6 : 0xF7);
    EmitModRMReg(b, 0, dst);
    if (byte_op) {
        b.B(u8(imm));
    } else if (width == 16) {
        b.W(u16(imm));
    } else {
        b.D(u32(imm));
    }
}

// F6/F7 groups: 2=not 3=neg 4=mul 5=imul 6=div 7=idiv
void EmitGroupF6(CodeBuf& b, u8 sub, int width, u8 rm, bool high_byte = false) {
    EmitOperandPrefix(b, width);
    bool byte_op = width == 8;
    EmitRexForRegReg(b, width, 0, rm, byte_op, high_byte);
    b.B(byte_op ? 0xF6 : 0xF7);
    EmitModRMReg(b, sub, rm);
}

void EmitGroupF6Mem(CodeBuf& b, u8 sub, int width, const MemOp& m) {
    EmitOperandPrefix(b, width);
    bool byte_op = width == 8;
    EmitRexFor(b, width, 0, true, byte_op, false);
    b.B(byte_op ? 0xF6 : 0xF7);
    EmitModRMMem(b, 0, m);
}

void EmitIncDec(CodeBuf& b, bool dec, int width, u8 rm, bool high_byte = false) {
    EmitOperandPrefix(b, width);
    bool byte_op = width == 8;
    EmitRexForRegReg(b, width, 0, rm, byte_op, high_byte);
    b.B(byte_op ? 0xFE : 0xFF);
    EmitModRMReg(b, dec ? 1 : 0, rm);
}

// shift groups: 4=shl 5=shr 7=sar. form: 0=imm8 1=by-cl
void EmitShift(CodeBuf& b, u8 sub, int width, u8 rm, u8 count, bool by_cl, bool high_byte = false) {
    EmitOperandPrefix(b, width);
    bool byte_op = width == 8;
    EmitRexForRegReg(b, width, 0, rm, byte_op, high_byte);
    if (by_cl) {
        b.B(byte_op ? 0xD2 : 0xD3);
        EmitModRMReg(b, sub, rm);
    } else if (count == 1) {
        b.B(byte_op ? 0xD0 : 0xD1);
        EmitModRMReg(b, sub, rm);
    } else {
        b.B(byte_op ? 0xC0 : 0xC1);
        EmitModRMReg(b, sub, rm);
        b.B(count);
    }
}

void EmitMovRegReg(CodeBuf& b, int width, u8 dst, u8 src, bool high_byte = false) {
    EmitOperandPrefix(b, width);
    bool byte_op = width == 8;
    EmitRexForRegReg(b, width, src, dst, byte_op, high_byte);
    b.B(byte_op ? 0x88 : 0x89);
    EmitModRMReg(b, src, dst);
}

void EmitMovRegImm(CodeBuf& b, int width, u8 dst, u64 imm) {
    EmitOperandPrefix(b, width);
    if (width == 8) {
        EmitRex(b, false, false, false, dst >= 8, dst >= 4);
        b.B(u8(0xB0 + (dst & 7)));
        b.B(u8(imm));
    } else {
        EmitRex(b, width == 64, false, false, dst >= 8);
        b.B(u8(0xB8 + (dst & 7)));
        if (width == 16) {
            b.W(u16(imm));
        } else if (width == 32) {
            b.D(u32(imm));
        } else {
            b.Q(imm);
        }
    }
}

void EmitMovRegMem(CodeBuf& b, int width, u8 dst, const MemOp& m) {
    EmitOperandPrefix(b, width);
    bool byte_op = width == 8;
    EmitRexFor(b, width, dst, true, byte_op, false);
    b.B(byte_op ? 0x8A : 0x8B);
    EmitModRMMem(b, dst, m);
}

void EmitMovMemReg(CodeBuf& b, int width, const MemOp& m, u8 src) {
    EmitOperandPrefix(b, width);
    bool byte_op = width == 8;
    EmitRexFor(b, width, src, true, byte_op, false);
    b.B(byte_op ? 0x88 : 0x89);
    EmitModRMMem(b, src, m);
}

void EmitMovMemImm(CodeBuf& b, int width, const MemOp& m, u64 imm) {
    EmitOperandPrefix(b, width);
    bool byte_op = width == 8;
    EmitRexFor(b, width, 0, true, byte_op, false);
    b.B(byte_op ? 0xC6 : 0xC7);
    EmitModRMMem(b, 0, m);
    if (byte_op) {
        b.B(u8(imm));
    } else if (width == 16) {
        b.W(u16(imm));
    } else {
        b.D(u32(imm));
    }
}

void EmitMovzx(CodeBuf& b, int dst_width, int src_width, u8 dst, u8 src, bool high_byte = false) {
    EmitRexForRegReg(b, dst_width, dst, src, true, high_byte);
    b.B(0x0F);
    b.B(src_width == 8 ? 0xB6 : 0xB7);
    EmitModRMReg(b, dst, src);
}

void EmitMovsx(CodeBuf& b, int dst_width, int src_width, u8 dst, u8 src, bool high_byte = false) {
    EmitRexForRegReg(b, dst_width, dst, src, true, high_byte);
    b.B(0x0F);
    b.B(src_width == 8 ? 0xBE : 0xBF);
    EmitModRMReg(b, dst, src);
}

void EmitMovsxd(CodeBuf& b, u8 dst, u8 src) {
    EmitRex(b, true, dst >= 8, false, src >= 8);
    b.B(0x63);
    EmitModRMReg(b, dst, src);
}

void EmitLea(CodeBuf& b, int width, u8 dst, const MemOp& m) {
    EmitOperandPrefix(b, width);
    EmitRex(b, width == 64, dst >= 8, false, !m.rip_rel && (kDataReg >= 8));
    b.B(0x8D);
    EmitModRMMem(b, dst, m);
}

void EmitXchgRegReg(CodeBuf& b, int width, u8 a, u8 bb, bool high_byte = false) {
    EmitOperandPrefix(b, width);
    bool byte_op = width == 8;
    EmitRexForRegReg(b, width, bb, a, byte_op, high_byte);
    b.B(byte_op ? 0x86 : 0x87);
    EmitModRMReg(b, bb, a);
}

void EmitXchgMemReg(CodeBuf& b, int width, const MemOp& m, u8 src) {
    EmitOperandPrefix(b, width);
    bool byte_op = width == 8;
    EmitRexFor(b, width, src, true, byte_op, false);
    b.B(byte_op ? 0x86 : 0x87);
    EmitModRMMem(b, src, m);
}

void EmitSetcc(CodeBuf& b, u8 cc, u8 dst) {
    EmitRex(b, false, false, false, dst >= 8, dst >= 4);
    b.B(0x0F);
    b.B(u8(0x90 + cc));
    EmitModRMReg(b, 0, dst);
}

void EmitCmovcc(CodeBuf& b, u8 cc, int width, u8 dst, u8 src) {
    EmitOperandPrefix(b, width);
    EmitRex(b, width == 64, dst >= 8, false, src >= 8);
    b.B(0x0F);
    b.B(u8(0x40 + cc));
    EmitModRMReg(b, dst, src);
}

void EmitJccRel8(CodeBuf& b, u8 cc, s8 rel) { b.B(u8(0x70 + cc)); b.B(u8(rel)); }

void EmitJmpRel8(CodeBuf& b, s8 rel) { b.B(0xEB); b.B(u8(rel)); }

void EmitCallRel32(CodeBuf& b, s32 rel) { b.B(0xE8); b.D(u32(rel)); }

void EmitRet(CodeBuf& b) { b.B(0xC3); }

void EmitPushReg(CodeBuf& b, u8 reg) {
    EmitRex(b, false, false, false, reg >= 8);
    b.B(u8(0x50 + (reg & 7)));
}

void EmitPopReg(CodeBuf& b, u8 reg) {
    EmitRex(b, false, false, false, reg >= 8);
    b.B(u8(0x58 + (reg & 7)));
}

void EmitPushImm(CodeBuf& b, u64 imm, bool imm8) {
    if (imm8) {
        b.B(0x6A);
        b.B(u8(imm));
    } else {
        b.B(0x68);
        b.D(u32(imm));
    }
}

void EmitPushMem(CodeBuf& b, const MemOp& m) {
    b.B(0xFF);
    EmitModRMMem(b, 6, m);
}

void EmitPopMem(CodeBuf& b, const MemOp& m) {
    b.B(0x8F);
    EmitModRMMem(b, 0, m);
}

void EmitMovs(CodeBuf& b, int width, bool rep) {
    if (rep) {
        b.B(0xF3);
    }
    EmitOperandPrefix(b, width);
    if (width == 64) {
        b.B(0x48);
    }
    b.B(width == 8 ? 0xA4 : 0xA5);
}

void EmitImul2(CodeBuf& b, int width, u8 dst, u8 src) {
    EmitOperandPrefix(b, width);
    EmitRex(b, width == 64, dst >= 8, false, src >= 8);
    b.B(0x0F);
    b.B(0xAF);
    EmitModRMReg(b, dst, src);
}

void EmitImul3(CodeBuf& b, int width, u8 dst, u8 src, u64 imm, bool imm8) {
    EmitOperandPrefix(b, width);
    EmitRex(b, width == 64, dst >= 8, false, src >= 8);
    if (imm8) {
        b.B(0x6B);
        EmitModRMReg(b, dst, src);
        b.B(u8(imm));
    } else {
        b.B(0x69);
        EmitModRMReg(b, dst, src);
        if (width == 16) {
            b.W(u16(imm));
        } else {
            b.D(u32(imm));
        }
    }
}

// =============================== fuzz environment ===============================

// Flag bits inside the LAHF-produced AH byte.
constexpr u32 kAhCF = 0x01, kAhPF = 0x04, kAhAF = 0x10, kAhZF = 0x40, kAhSF = 0x80;
constexpr u32 kAhAll = kAhCF | kAhPF | kAhAF | kAhZF | kAhSF;

struct FlagMask {
    // AF is masked globally: the backend's AF handling is broken
    // (TestAuxiliaryCarry reads the wrong bit; see report).
    u32 ah{kAhAll & ~kAhAF};
    bool of{true};
};

struct FuzzEnv {
    static constexpr size_t kMemSize = 0x2000000;  // 32MB
    static constexpr u64 kDataOff = 0x1000000;     // data area offset
    static constexpr size_t kCodeStride = 0x100;

    void* host_mem{};
    u64 base{};
    u64 data_addr{};   // value of kDataReg (r13)
    u64 stack_addr{};  // initial rsp
    std::unique_ptr<UnicornInterface> uc;
    X86Instance* instance{};
    X86Core* core{};
    ThreadContext64* ctx{};
    std::mt19937_64 rng;
    u64 cursor{0};
    int failures{0};

    FuzzEnv() {
        host_mem = mmap(nullptr, kMemSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (host_mem == MAP_FAILED) {
            perror("mmap");
            abort();
        }
        base = reinterpret_cast<u64>(host_mem);
        data_addr = base + kDataOff + 0x1000;
        stack_addr = base + kDataOff + 0x2000;
        uc = std::make_unique<UnicornInterface>(UC_ARCH_X86, UC_MODE_64);
        uc->MapMemory(base, kMemSize, UC_PROT_ALL);
        instance = X86Instance::Make();
        core = X86Core::Make(instance);
        ctx = &core->GetContext();
        std::random_device rd;
        u64 seed = (u64(rd()) << 32) ^ rd();
        if (const char* s = getenv("SWIFT_FUZZ_SEED")) {
            seed = strtoull(s, nullptr, 0);
        }
        rng.seed(seed);
        std::cout << "Fuzz seed: " << seed << std::endl;
    }

    ~FuzzEnv() {
        X86Core::Destroy(core);
        X86Instance::Destroy(instance);
        munmap(host_mem, kMemSize);
    }

    int RandInt(int lo, int hi) {
        std::uniform_int_distribution<int> d(lo, hi);
        return d(rng);
    }

    template <typename T>
    const T& Pick(const std::vector<T>& v) {
        return v[rng() % v.size()];
    }

    u64 PoolVal(int width) {
        static const u64 edges[] = {0,          1,          2,
                                    3,          0x7F,       0x80,
                                    0xFF,       0x100,      0x7FFF,
                                    0x8000,     0xFFFF,     0x10000,
                                    0x7FFFFFFF, 0x80000000, 0xFFFFFFFF,
                                    0x100000000ull, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull,
                                    ~0ull,      42,         0x55,
                                    0xAA,       0x1234567890ABCDEFull};
        u64 v;
        if (RandInt(0, 2) == 0) {
            v = edges[rng() % std::size(edges)];
        } else {
            v = rng();
        }
        if (width < 64) {
            v &= ((u64(1) << width) - 1);
        }
        return v;
    }

    // GPRs usable as random operands (excludes rsp, r11 index, r13 data, r15 capture).
    u8 RandReg() {
        static const u8 regs[] = {kRax, kRcx, kRdx, kRbx, kRbp, kRsi, kRdi,
                                  kR8,  kR9,  kR10, kR12};
        return regs[rng() % std::size(regs)];
    }

    MemOp RandMem() {
        MemOp m{};
        int kind = RandInt(0, 9);
        static const s32 disps[] = {0, 1, 2, 4, 8, 16, 64, 127, -8, -128, 128, 512, -512};
        m.disp = disps[rng() % std::size(disps)];
        if (kind >= 7) {
            m.scale = u8(1 << RandInt(0, 3));
            if (m.disp < -128 || m.disp > 127) {
                m.disp = 8;
            }
        }
        return m;
    }

    void SyncRegsToUnicorn() {
        uc->WriteRegister(UC_X86_REG_RAX, ctx->rax.qword);
        uc->WriteRegister(UC_X86_REG_RBX, ctx->rbx.qword);
        uc->WriteRegister(UC_X86_REG_RCX, ctx->rcx.qword);
        uc->WriteRegister(UC_X86_REG_RDX, ctx->rdx.qword);
        uc->WriteRegister(UC_X86_REG_RSI, ctx->rsi.qword);
        uc->WriteRegister(UC_X86_REG_RDI, ctx->rdi.qword);
        uc->WriteRegister(UC_X86_REG_RBP, ctx->rbp.qword);
        uc->WriteRegister(UC_X86_REG_RSP, ctx->rsp.qword);
        uc->WriteRegister(UC_X86_REG_R8, ctx->r8.qword);
        uc->WriteRegister(UC_X86_REG_R9, ctx->r9.qword);
        uc->WriteRegister(UC_X86_REG_R10, ctx->r10.qword);
        uc->WriteRegister(UC_X86_REG_R11, ctx->r11.qword);
        uc->WriteRegister(UC_X86_REG_R12, ctx->r12.qword);
        uc->WriteRegister(UC_X86_REG_R13, ctx->r13.qword);
        uc->WriteRegister(UC_X86_REG_R14, ctx->r14.qword);
        uc->WriteRegister(UC_X86_REG_R15, ctx->r15.qword);
        uc->WriteRegister(UC_X86_REG_EFLAGS, u64(0x202));
        uc->WriteRegister(UC_X86_REG_RIP, ctx->rip.qword);
    }

    void InitRegs() {
        ctx->rax.qword = PoolVal(64);
        ctx->rbx.qword = PoolVal(64);
        ctx->rcx.qword = PoolVal(64);
        ctx->rdx.qword = PoolVal(64);
        ctx->rsi.qword = PoolVal(64);
        ctx->rdi.qword = PoolVal(64);
        ctx->rbp.qword = PoolVal(64);
        ctx->r8.qword = PoolVal(64);
        ctx->r9.qword = PoolVal(64);
        ctx->r10.qword = PoolVal(64);
        ctx->r12.qword = PoolVal(64);
        ctx->r14.qword = PoolVal(64);
        ctx->r15.qword = 0;
        ctx->r11.qword = Pick(std::vector<u64>{0, 1, 2, 3, 7, 8, 15, 16, 64});
        ctx->r13.qword = data_addr;
        ctx->rsp.qword = stack_addr;
    }

    std::string DumpCode(const std::vector<u8>& code) {
        std::string s;
        for (auto b : code) {
            s += fmt::format("{:02x} ", b);
        }
        return s;
    }

    // Emit the flag-init prefix: a random flag-defining op on the initialized
    // registers, executed identically on both sides.
    void EmitFlagPrefix(CodeBuf& b) {
        u8 ra = RandReg();
        u8 rb = RandReg();
        int width = Pick(std::vector<int>{8, 16, 32, 64});
        switch (RandInt(0, 4)) {
            case 0:
                EmitAluRegReg(b, 7, width, ra, rb);  // cmp
                break;
            case 1:
                EmitTestRegReg(b, width, ra, rb);
                break;
            case 2:
                EmitAluRegReg(b, 0, width, ra, rb);  // add
                break;
            case 3:
                EmitAluRegReg(b, 5, width, ra, rb);  // sub
                break;
            default:
                EmitAluRegReg(b, 6, width, ra, rb);  // xor
                break;
        }
    }

    // Emit the flag capture suffix: lahf + seto r15b. OF lands in bit 0 of r15.
    void EmitFlagCapture(CodeBuf& b) {
        b.B(0x9F);  // lahf
        EmitSetcc(b, 0x0, kCaptureReg);  // seto r15b
    }

    void DumpIR(u64 code_addr) {
        struct MemIf : public swift::runtime::MemoryInterface {
            bool Read(void* dest, size_t addr, size_t size) override {
                return std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
            }
            bool Write(void* src, size_t addr, size_t size) override {
                return std::memcpy(reinterpret_cast<void*>(addr), src, size);
            }
            void* GetPointer(void* src) override { return src; }
        } mem_if;
        swift::runtime::ir::Block block{0, swift::runtime::ir::Location{code_addr}};
        swift::runtime::ir::Assembler assembler{&block};
        X64Decoder decoder{code_addr, &mem_if, &assembler, true};
        decoder.Decode();
        std::cout << block.ToString() << std::endl;
    }

    // Run one iteration. `code` must include everything except HLT.
    void RunIteration(const std::vector<u8>& body, const FlagMask& mask, const char* tag) {
        std::vector<u8> code = body;
        code.push_back(0xF4);  // hlt
        if (code.size() > kCodeStride) {
            FAIL(fmt::format("code too long: {}", code.size()));
        }
        u64 code_addr = base + cursor * kCodeStride;
        cursor++;
        if (getenv("SWIFT_FUZZ_DUMP_IR")) {
            std::cout << "== cursor " << (cursor - 1) << " code: " << DumpCode(code) << std::endl;
            DumpIR(code_addr);
        }

        std::memcpy(host_mem, code.data(), code.size());
        uc->WriteMemory(base, code);
        // keep Unicorn's view of the data area in sync with the host's
        uc->WriteMemory(data_addr - 0x1000,
                        std::vector<u8>(reinterpret_cast<u8*>(host_mem) + kDataOff,
                                        reinterpret_cast<u8*>(host_mem) + kDataOff + 0x4000));

        ctx->rip.qword = code_addr;
        // snapshot initial registers for failure diagnosis
        std::array<u64, 16> init_regs{};
        for (int r = 0; r < 16; r++) {
            init_regs[r] = ctx->regs[r].qword;
        }
        SyncRegsToUnicorn();

        bool unicorn_ok = true;
        try {
            uc->Run(code_addr, code_addr + code.size() - 1, 0, 0);
        } catch (const std::exception& e) {
            unicorn_ok = false;
        }
        if (!unicorn_ok) {
            return;  // e.g. #DE we failed to constrain away
        }

        try {
            core->Run();
        } catch (const std::exception& e) {
            FAIL(fmt::format("[{}] SwiftVM threw: {}. code: {}", tag, e.what(), DumpCode(code)));
        } catch (...) {
            FAIL(fmt::format("[{}] SwiftVM crashed. code: {}", tag, DumpCode(code)));
        }

        // Compare GPRs.
        struct RegPair {
            int uc_id;
            const char* name;
            u64 swift_val;
        };
        std::vector<RegPair> pairs = {
                {UC_X86_REG_RAX, "rax", ctx->rax.qword}, {UC_X86_REG_RBX, "rbx", ctx->rbx.qword},
                {UC_X86_REG_RCX, "rcx", ctx->rcx.qword}, {UC_X86_REG_RDX, "rdx", ctx->rdx.qword},
                {UC_X86_REG_RSI, "rsi", ctx->rsi.qword}, {UC_X86_REG_RDI, "rdi", ctx->rdi.qword},
                {UC_X86_REG_RBP, "rbp", ctx->rbp.qword}, {UC_X86_REG_RSP, "rsp", ctx->rsp.qword},
                {UC_X86_REG_R8, "r8", ctx->r8.qword},    {UC_X86_REG_R9, "r9", ctx->r9.qword},
                {UC_X86_REG_R10, "r10", ctx->r10.qword}, {UC_X86_REG_R11, "r11", ctx->r11.qword},
                {UC_X86_REG_R12, "r12", ctx->r12.qword}, {UC_X86_REG_R13, "r13", ctx->r13.qword},
                {UC_X86_REG_R14, "r14", ctx->r14.qword},
        };
        bool reg_mismatch = false;
        std::string detail;
        for (auto& p : pairs) {
            u64 uv = uc->ReadRegister(p.uc_id);
            u64 sv = p.swift_val;
            if (p.uc_id == UC_X86_REG_RAX) {
                // AH holds the captured flags: apply the family flag mask.
                u64 ah_mask = u64(0xFF & ~mask.ah) << 8;
                uv &= ~ah_mask;
                sv &= ~ah_mask;
            }
            if (uv != sv) {
                reg_mismatch = true;
                detail += fmt::format(" {}: uc={:x} sv={:x};", p.name, uv, sv);
            }
        }

        // Compare captured flags (AH of rax via lahf, OF in r15 bit 0).
        u64 uc_rax = uc->ReadRegister(UC_X86_REG_RAX);
        u64 uc_r15 = uc->ReadRegister(UC_X86_REG_R15);
        u32 uc_ah = u32((uc_rax >> 8) & 0xFF) & mask.ah;
        u32 sv_ah = u32((ctx->rax.qword >> 8) & 0xFF) & mask.ah;
        u32 uc_of = mask.of ? u32(uc_r15 & 1) : 0;
        u32 sv_of = mask.of ? u32(ctx->r15.qword & 1) : 0;
        if (uc_ah != sv_ah || uc_of != sv_of) {
            reg_mismatch = true;
            detail += fmt::format(" flags: uc_ah={:02x} sv_ah={:02x} uc_of={} sv_of={};", uc_ah,
                                  sv_ah, uc_of, sv_of);
        }

        // Compare scratch memory window.
        size_t win_off = kDataOff;  // [data_base, data_base + 0x4000)
        auto uc_data = uc->ReadMemory(base + win_off, 0x4000);
        if (memcmp(uc_data.data(), reinterpret_cast<u8*>(host_mem) + win_off, 0x4000) != 0) {
            size_t first = 0;
            for (size_t i = 0; i < 0x4000; i++) {
                if (uc_data[i] != reinterpret_cast<u8*>(host_mem)[win_off + i]) {
                    first = i;
                    break;
                }
            }
            reg_mismatch = true;
            detail += fmt::format(" mem@{:x}: uc={:02x} sv={:02x};", win_off + first,
                                  uc_data[first], reinterpret_cast<u8*>(host_mem)[win_off + first]);
        }

        if (reg_mismatch) {
            failures++;
            std::cout << fmt::format("[{}] MISMATCH (cursor {}):{} code: {}", tag, cursor - 1,
                                     detail, DumpCode(code))
                      << std::endl;
            std::cout << fmt::format(
                    "  init: rax={:x} rcx={:x} rdx={:x} rbx={:x} rbp={:x} rsi={:x} rdi={:x} "
                    "r8={:x} r9={:x} r10={:x} r12={:x} r14={:x}",
                    init_regs[0], init_regs[1], init_regs[2], init_regs[3], init_regs[5],
                    init_regs[6], init_regs[7], init_regs[8], init_regs[9], init_regs[10],
                    init_regs[12], init_regs[14])
                      << std::endl;
        }
    }

    int Iters(int def) {
        if (const char* s = getenv("SWIFT_FUZZ_ITERS")) {
            return atoi(s);
        }
        return def;
    }
};

}  // namespace

TEST_CASE("Fuzz x86 debug repro") {
    if (!getenv("SWIFT_FUZZ_DEBUG")) {
        return;
    }
    FuzzEnv env;
    // sub r10, r9; mul edx; lahf; seto r15b; hlt
    std::vector<u8> code = {0x4d, 0x29, 0xca, 0xf7, 0xe2, 0x9f, 0x41, 0x0f, 0x90, 0xc7, 0xf4};
    u64 code_addr = env.base;
    std::memcpy(env.host_mem, code.data(), code.size());
    env.uc->WriteMemory(env.base, code);
    env.DumpIR(code_addr);
    auto& ctx = *env.ctx;
    ctx.rax.qword = 0x5d8010f4db8e18cc;
    ctx.rdx.qword = 0xec4ec8ba148c21d6;
    ctx.rsi.qword = 0x39681f394f6e83d1;
    ctx.rip.qword = code_addr;
    ctx.rsp.qword = env.stack_addr;
    env.SyncRegsToUnicorn();
    env.uc->Run(code_addr, code_addr + code.size() - 1, 0, 0);
    env.core->Run();
    auto show = [&](const char* name, int uc_reg, u64 sv) {
        std::cout << fmt::format("{}: uc={:x} sv={:x}\n", name, env.uc->ReadRegister(uc_reg), sv);
    };
    show("rax", UC_X86_REG_RAX, ctx.rax.qword);
    show("rcx", UC_X86_REG_RCX, ctx.rcx.qword);
    show("rdx", UC_X86_REG_RDX, ctx.rdx.qword);
    show("r9", UC_X86_REG_R9, ctx.r9.qword);
    show("r15", UC_X86_REG_R15, ctx.r15.qword);
}

namespace {

// =============================== family generators ===============================

TEST_CASE("Fuzz x86 alu") {
    FuzzEnv env;
    int iters = env.Iters(4000);
    for (int i = 0; i < iters; ++i) {
        CodeBuf b;
        env.InitRegs();
        env.EmitFlagPrefix(b);
        int n = env.RandInt(1, 3);
        for (int j = 0; j < n; ++j) {
            u8 group = env.Pick(std::vector<u8>{0, 1, 2, 3, 4, 5, 6, 7});  // add..cmp (test below)
            int width = env.Pick(std::vector<int>{8, 16, 32, 64});
            u8 dst = env.RandReg();
            u8 src = env.RandReg();
            int form = env.RandInt(0, 9);
            if (env.RandInt(0, 7) == 0) {
                EmitTestRegReg(b, width, dst, src);
            } else if (form < 4) {
                EmitAluRegReg(b, group, width, dst, src);
            } else if (form < 6) {
                u64 imm = env.PoolVal(width > 32 ? 32 : width);
                EmitAluRegImm(b, group, width, dst, imm, env.RandInt(0, 3) == 0);
            } else if (form < 8) {
                EmitAluRegMem(b, group, width, dst, env.RandMem());
            } else {
                if (group == 7) {
                    group = 5;  // cmp has no mem dst; use sub
                }
                EmitAluMemReg(b, group, width, env.RandMem(), src);
            }
        }
        env.EmitFlagCapture(b);
        env.RunIteration(b.c, FlagMask{}, "alu");
    }
    REQUIRE(env.failures == 0);
}

TEST_CASE("Fuzz x86 inc dec neg not") {
    FuzzEnv env;
    int iters = env.Iters(3000);
    for (int i = 0; i < iters; ++i) {
        CodeBuf b;
        env.InitRegs();
        env.EmitFlagPrefix(b);
        int n = env.RandInt(1, 3);
        FlagMask mask;
        for (int j = 0; j < n; ++j) {
            int width = env.Pick(std::vector<int>{8, 16, 32, 64});
            u8 dst = env.RandReg();
            switch (env.RandInt(0, 3)) {
                case 0:
                    EmitIncDec(b, false, width, dst);
                    mask.ah &= ~kAhCF;  // CF after inc/dec not preserved (backend limit)
                    break;
                case 1:
                    EmitIncDec(b, true, width, dst);
                    mask.ah &= ~kAhCF;
                    break;
                case 2:
                    EmitGroupF6(b, 3, width, dst);  // neg
                    break;
                default:
                    EmitGroupF6(b, 2, width, dst);  // not (flags untouched)
                    break;
            }
        }
        env.EmitFlagCapture(b);
        env.RunIteration(b.c, mask, "incdec");
    }
    REQUIRE(env.failures == 0);
}

TEST_CASE("Fuzz x86 shifts") {
    FuzzEnv env;
    int iters = env.Iters(4000);
    for (int i = 0; i < iters; ++i) {
        CodeBuf b;
        env.InitRegs();
        env.EmitFlagPrefix(b);
        int n = env.RandInt(1, 2);
        for (int j = 0; j < n; ++j) {
            u8 sub = env.Pick(std::vector<u8>{4, 5, 7});  // shl shr sar
            int width = env.Pick(std::vector<int>{8, 16, 32, 64});
            u8 dst = env.RandReg();
            if (env.RandInt(0, 1)) {
                // by CL
                env.ctx->rcx.qword = env.Pick(std::vector<u64>{0, 1, 2, 7, 8, 15, 16, 31, 32, 33,
                                                                 63, 64, 65, 255});
                EmitShift(b, sub, width, dst, 0, true);
            } else {
                u8 count = u8(env.Pick(std::vector<u64>{1, 2, 3, 7, 8, 15, 16, 31}));
                EmitShift(b, sub, width, dst, count, false);
            }
        }
        env.EmitFlagCapture(b);
        // CF / OF after shifts are approximated (see report).
        FlagMask mask;
        mask.ah &= ~kAhCF;
        mask.of = false;
        env.RunIteration(b.c, mask, "shift");
    }
    REQUIRE(env.failures == 0);
}

TEST_CASE("Fuzz x86 mul imul") {
    FuzzEnv env;
    int iters = env.Iters(3000);
    for (int i = 0; i < iters; ++i) {
        CodeBuf b;
        env.InitRegs();
        env.EmitFlagPrefix(b);
        int width = env.Pick(std::vector<int>{8, 16, 32, 64});
        switch (env.RandInt(0, 3)) {
            case 0:
                EmitGroupF6(b, 4, width, env.RandReg());  // mul
                break;
            case 1:
                EmitGroupF6(b, 5, width, env.RandReg());  // imul (1 operand)
                break;
            case 2: {
                EmitImul2(b, width, env.RandReg(), env.RandReg());
                break;
            }
            default: {
                u64 imm = env.PoolVal(env.RandInt(0, 1) ? 8 : (width > 32 ? 32 : width));
                EmitImul3(b, width, env.RandReg(), env.RandReg(), imm, env.RandInt(0, 1));
                break;
            }
        }
        env.EmitFlagCapture(b);
        // CF / OF after mul are set-only / partially unimplemented (see report).
        FlagMask mask;
        mask.ah &= ~kAhCF;
        mask.of = false;
        env.RunIteration(b.c, mask, "mul");
    }
    REQUIRE(env.failures == 0);
}

TEST_CASE("Fuzz x86 div idiv") {
    FuzzEnv env;
    int iters = env.Iters(2000);
    for (int i = 0; i < iters; ++i) {
        CodeBuf b;
        env.InitRegs();
        env.EmitFlagPrefix(b);
        int width = env.Pick(std::vector<int>{8, 16, 32, 64});
        bool sign = env.RandInt(0, 1);
        u8 rm = env.RandReg();
        // Constrain dividend / divisor so no #DE occurs.
        u64 divisor = env.PoolVal(width) | 1;  // odd => nonzero
        if (divisor == 0) {
            divisor = 3;
        }
        // Build a dividend = divisor * q + r with q fitting the width.
        u64 mod = width == 64 ? ~0ull : ((u64(1) << width) - 1);
        u64 q = env.PoolVal(width);
        if (!sign) {
            if (divisor > 1 && q > (~0ull / divisor)) {
                q = 1;
            }
            u64 dividend = (divisor & mod) * (q & mod) + (env.PoolVal(width) % (divisor & mod));
            switch (width) {
                case 8:
                    env.ctx->rax.qword = dividend & 0xFFFF;
                    break;
                case 16:
                    env.ctx->rdx.qword = 0;
                    env.ctx->rax.qword = dividend & 0xFFFFFFFF;
                    break;
                case 32:
                    env.ctx->rdx.qword = 0;
                    env.ctx->rax.qword = dividend;
                    break;
                default:
                    env.ctx->rdx.qword = 0;
                    env.ctx->rax.qword = dividend;
                    break;
            }
        } else {
            // signed: keep magnitudes small to avoid quotient overflow
            s64 dq = s64(env.PoolVal(width / 2));
            s64 dd = s64(divisor & mod);
            if (dd == 0) {
                dd = 3;
            }
            // sign extend divisor from width
            if (width < 64 && (dd & (s64(1) << (width - 1)))) {
                dd -= s64(1) << width;
            }
            if (dd == 0) {
                dd = 3;
            }
            if (dd == -1) {
                dd = -3;
            }
            s64 dividend = dd * dq + (dq % (dd == 0 ? 1 : (dd > 0 ? dd : -dd)));
            switch (width) {
                case 8:
                    env.ctx->rax.qword = u64(dividend) & 0xFFFF;
                    break;
                case 16:
                    env.ctx->rax.qword = u64(dividend) & 0xFFFFFFFF;
                    env.ctx->rdx.qword = dividend < 0 ? 0xFFFF : 0;
                    break;
                case 32:
                    env.ctx->rax.qword = u64(u32(dividend));
                    env.ctx->rdx.qword = dividend < 0 ? 0xFFFFFFFFull : 0;
                    break;
                default:
                    env.ctx->rax.qword = u64(dividend);
                    env.ctx->rdx.qword = dividend < 0 ? ~0ull : 0;
                    break;
            }
        }
        // rax / rdx hold the dividend; pick another register for the divisor.
        if (rm == kRax || rm == kRdx) {
            rm = kRbx;
        }
        auto set_reg = [&](u8 r, u64 v) {
            switch (r) {
                case kRbx: env.ctx->rbx.qword = v; break;
                case kRcx: env.ctx->rcx.qword = v; break;
                case kRsi: env.ctx->rsi.qword = v; break;
                case kRdi: env.ctx->rdi.qword = v; break;
                case kRbp: env.ctx->rbp.qword = v; break;
                case kR8: env.ctx->r8.qword = v; break;
                case kR9: env.ctx->r9.qword = v; break;
                case kR10: env.ctx->r10.qword = v; break;
                case kR12: env.ctx->r12.qword = v; break;
                default: env.ctx->rbx.qword = v; break;
            }
        };
        set_reg(rm, divisor & mod);
        if ((divisor & mod) == 0) {
            set_reg(rm, 3);
        }
        EmitGroupF6(b, sign ? 7 : 6, width, rm);
        env.EmitFlagCapture(b);
        env.RunIteration(b.c, FlagMask{}, "div");
    }
    REQUIRE(env.failures == 0);
}

TEST_CASE("Fuzz x86 setcc cmov jcc") {
    FuzzEnv env;
    int iters = env.Iters(3000);
    for (int i = 0; i < iters; ++i) {
        CodeBuf b;
        env.InitRegs();
        env.EmitFlagPrefix(b);
        u8 cc = u8(env.RandInt(0, 15));
        int mode = env.RandInt(0, 2);
        if (mode == 0) {
            // setcc into a byte register
            EmitSetcc(b, cc, env.RandReg());
            env.EmitFlagCapture(b);
            env.RunIteration(b.c, FlagMask{}, "setcc");
        } else if (mode == 1) {
            // cmovcc
            int width = env.Pick(std::vector<int>{16, 32, 64});
            EmitCmovcc(b, cc, width, env.RandReg(), env.RandReg());
            env.EmitFlagCapture(b);
            env.RunIteration(b.c, FlagMask{}, "cmov");
        } else {
            // capture flags first, then jcc over a reg-modifying mov
            env.EmitFlagCapture(b);
            u8 scratch = env.RandReg();
            // choose a target value for the conditional mov
            CodeBuf tail;
            EmitMovRegImm(tail, 64, scratch, 0x1122334455667788ull);
            // jcc +8-bit: skip the mov (mov r64,imm64 is 10 bytes)
            EmitJccRel8(b, cc, s8(tail.c.size()));
            for (auto v : tail.c) {
                b.B(v);
            }
            env.RunIteration(b.c, FlagMask{}, "jcc");
        }
    }
    REQUIRE(env.failures == 0);
}

TEST_CASE("Fuzz x86 push pop") {
    FuzzEnv env;
    int iters = env.Iters(3000);
    for (int i = 0; i < iters; ++i) {
        CodeBuf b;
        env.InitRegs();
        env.EmitFlagPrefix(b);
        int n = env.RandInt(1, 4);
        int depth = 0;
        for (int j = 0; j < n; ++j) {
            if (depth > 0 && env.RandInt(0, 1)) {
                if (env.RandInt(0, 3) == 0) {
                    EmitPopMem(b, env.RandMem());
                } else {
                    EmitPopReg(b, env.RandReg());
                }
                depth--;
            } else {
                switch (env.RandInt(0, 2)) {
                    case 0:
                        EmitPushReg(b, env.RandReg());
                        break;
                    case 1:
                        EmitPushImm(b, env.PoolVal(env.RandInt(0, 1) ? 8 : 32), env.RandInt(0, 1));
                        break;
                    default:
                        EmitPushMem(b, env.RandMem());
                        break;
                }
                depth++;
            }
        }
        // balance the stack
        while (depth-- > 0) {
            EmitPopReg(b, env.RandReg());
        }
        env.EmitFlagCapture(b);
        env.RunIteration(b.c, FlagMask{}, "pushpop");
    }
    REQUIRE(env.failures == 0);
}

TEST_CASE("Fuzz x86 mov lea xchg extends") {
    FuzzEnv env;
    int iters = env.Iters(5000);
    for (int i = 0; i < iters; ++i) {
        CodeBuf b;
        env.InitRegs();
        env.EmitFlagPrefix(b);
        int n = env.RandInt(1, 4);
        for (int j = 0; j < n; ++j) {
            int what = env.RandInt(0, 8);
            int width = env.Pick(std::vector<int>{8, 16, 32, 64});
            switch (what) {
                case 0:
                    EmitMovRegReg(b, width, env.RandReg(), env.RandReg());
                    break;
                case 1:
                    EmitMovRegImm(b, width, env.RandReg(), env.PoolVal(width));
                    break;
                case 2:
                    EmitMovRegMem(b, width, env.RandReg(), env.RandMem());
                    break;
                case 3:
                    EmitMovMemReg(b, width, env.RandMem(), env.RandReg());
                    break;
                case 4:
                    EmitMovMemImm(b, width, env.RandMem(), env.PoolVal(width > 32 ? 32 : width));
                    break;
                case 5:
                    EmitLea(b, width, env.RandReg(), env.RandMem());
                    break;
                case 6:
                    EmitXchgRegReg(b, width, env.RandReg(), env.RandReg());
                    break;
                case 7:
                    EmitXchgMemReg(b, width, env.RandMem(), env.RandReg());
                    break;
                default: {
                    int sw = width == 64 ? env.Pick(std::vector<int>{8, 16, 32})
                                         : (width == 32 ? env.Pick(std::vector<int>{8, 16}) : 8);
                    if (sw == 32) {
                        EmitMovsxd(b, env.RandReg(), env.RandReg());
                    } else if (env.RandInt(0, 1)) {
                        EmitMovzx(b, width, sw, env.RandReg(), env.RandReg());
                    } else {
                        EmitMovsx(b, width, sw, env.RandReg(), env.RandReg());
                    }
                    break;
                }
            }
        }
        env.EmitFlagCapture(b);
        env.RunIteration(b.c, FlagMask{}, "mov");
    }
    REQUIRE(env.failures == 0);
}

TEST_CASE("Fuzz x86 cbw cdq lahf") {
    FuzzEnv env;
    int iters = env.Iters(2000);
    for (int i = 0; i < iters; ++i) {
        CodeBuf b;
        env.InitRegs();
        env.EmitFlagPrefix(b);
        switch (env.RandInt(0, 7)) {
            case 0: b.B(0x66); b.B(0x98); break;  // cbw
            case 1: b.B(0x98); break;             // cwde
            case 2: b.B(0x48); b.B(0x98); break;  // cdqe
            case 3: b.B(0x66); b.B(0x99); break;  // cwd
            case 4: b.B(0x99); break;             // cdq
            case 5: b.B(0x48); b.B(0x99); break;  // cqo
            case 6: b.B(0x9F); break;             // lahf
            default: {
                // movs single step
                env.ctx->rsi.qword = env.data_addr - 0x800;
                env.ctx->rdi.qword = env.data_addr - 0x400;
                EmitMovs(b, env.Pick(std::vector<int>{8, 16, 32, 64}), false);
                break;
            }
        }
        env.EmitFlagCapture(b);
        env.RunIteration(b.c, FlagMask{}, "cbw");
    }
    REQUIRE(env.failures == 0);
}

TEST_CASE("Fuzz x86 rep movs") {
    FuzzEnv env;
    int iters = env.Iters(1000);
    for (int i = 0; i < iters; ++i) {
        CodeBuf b;
        env.InitRegs();
        env.EmitFlagPrefix(b);
        env.ctx->rsi.qword = env.data_addr - 0x800;
        env.ctx->rdi.qword = env.data_addr - 0x400;
        env.ctx->rcx.qword = env.RandInt(0, 16);
        EmitMovs(b, env.Pick(std::vector<int>{8, 16, 32, 64}), true);
        env.EmitFlagCapture(b);
        env.RunIteration(b.c, FlagMask{}, "repmovs");
    }
    REQUIRE(env.failures == 0);
}

TEST_CASE("Fuzz x86 call ret jmp") {
    FuzzEnv env;
    int iters = env.Iters(2000);
    for (int i = 0; i < iters; ++i) {
        CodeBuf b;
        env.InitRegs();
        env.EmitFlagPrefix(b);
        // layout: call sub; <mid: sub r32,r32>; hlt; sub: mov r1,r2; add r2,0x55; ret
        CodeBuf sub;
        u8 r1 = env.RandReg();
        u8 r2 = env.RandReg();
        EmitMovRegReg(sub, 64, r1, r2);
        EmitAluRegImm(sub, 0, 64, r2, 0x55);
        EmitRet(sub);
        CodeBuf mid;
        EmitAluRegReg(mid, 5, 32, env.RandReg(), env.RandReg());  // sub r32, r32
        s32 rel = s32(mid.c.size() + 1 /* hlt */);
        EmitCallRel32(b, rel);
        for (auto v : mid.c) {
            b.B(v);
        }
        size_t hlt_pos = b.Pos();
        b.B(0xF4);
        for (auto v : sub.c) {
            b.B(v);
        }

        std::vector<u8> code = b.c;
        if (code.size() > FuzzEnv::kCodeStride) {
            FAIL("code too long");
        }
        u64 code_addr = env.base + env.cursor * FuzzEnv::kCodeStride;
        env.cursor++;
        std::memcpy(env.host_mem, code.data(), code.size());
        env.uc->WriteMemory(env.base, code);
        env.ctx->rip.qword = code_addr;
        env.SyncRegsToUnicorn();
        try {
            env.uc->Run(code_addr, code_addr + hlt_pos, 0, 0);
        } catch (const std::exception&) {
            continue;
        }
        try {
            env.core->Run();
        } catch (...) {
            FAIL("SwiftVM crashed in call/ret");
        }
        struct RegPair {
            int uc_id;
            const char* name;
            u64 sv;
        };
        std::vector<RegPair> pairs = {
                {UC_X86_REG_RAX, "rax", env.ctx->rax.qword}, {UC_X86_REG_RBX, "rbx", env.ctx->rbx.qword},
                {UC_X86_REG_RCX, "rcx", env.ctx->rcx.qword}, {UC_X86_REG_RDX, "rdx", env.ctx->rdx.qword},
                {UC_X86_REG_RSI, "rsi", env.ctx->rsi.qword}, {UC_X86_REG_RDI, "rdi", env.ctx->rdi.qword},
                {UC_X86_REG_RBP, "rbp", env.ctx->rbp.qword}, {UC_X86_REG_RSP, "rsp", env.ctx->rsp.qword},
                {UC_X86_REG_R8, "r8", env.ctx->r8.qword},    {UC_X86_REG_R9, "r9", env.ctx->r9.qword},
                {UC_X86_REG_R10, "r10", env.ctx->r10.qword}, {UC_X86_REG_R11, "r11", env.ctx->r11.qword},
                {UC_X86_REG_R12, "r12", env.ctx->r12.qword}, {UC_X86_REG_R13, "r13", env.ctx->r13.qword},
                {UC_X86_REG_R14, "r14", env.ctx->r14.qword}, {UC_X86_REG_R15, "r15", env.ctx->r15.qword},
        };
        std::string detail;
        bool bad = false;
        for (auto& p : pairs) {
            u64 uv = env.uc->ReadRegister(p.uc_id);
            if (uv != p.sv) {
                bad = true;
                detail += fmt::format(" {}: uc={:x} sv={:x};", p.name, uv, p.sv);
            }
        }
        if (bad) {
            env.failures++;
            std::cout << fmt::format("[callret] MISMATCH:{} code: {}", detail, env.DumpCode(code))
                      << std::endl;
        }
    }
    REQUIRE(env.failures == 0);
}
}
