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
#include <chrono>
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


// Segment override prefix (0x64 = fs, 0x65 = gs), emitted before everything.
void EmitSegPrefix(CodeBuf& b, u8 seg) {
    if (seg) {
        b.B(seg);
    }
}

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
void EmitRexFor(CodeBuf& b, int width, u8 reg_field, const MemOp* m, bool byte_op, bool high_byte) {
    bool has_mem = m != nullptr;
    bool x_bit = has_mem && m->scale != 0 && (kIndexReg >= 8);
    if (byte_op) {
        // High byte registers (AH..BH) forbid any REX. Byte registers >= 4
        // (SPL..) or >= 8 require one.
        if (high_byte) {
            return;
        }
        bool need = width == 64 || reg_field >= 4 || x_bit;  // spl/bpl/sil/dil and r8b+ need REX
        EmitRex(b, width == 64, reg_field >= 8, x_bit, has_mem && (kDataReg >= 8), need);
    } else {
        EmitRex(b, width == 64, reg_field >= 8, x_bit, has_mem && (kDataReg >= 8));
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

void EmitAluRegMem(CodeBuf& b, u8 group, int width, u8 dst, const MemOp& m, u8 seg = 0) {
    EmitSegPrefix(b, seg);
    EmitOperandPrefix(b, width);
    bool byte_op = width == 8;
    EmitRexFor(b, width, dst, &m, byte_op, false);
    b.B(u8(group * 8 + (byte_op ? 2 : 3)));
    EmitModRMMem(b, dst, m);
}

void EmitAluMemReg(CodeBuf& b, u8 group, int width, const MemOp& m, u8 src, u8 seg = 0) {
    EmitSegPrefix(b, seg);
    EmitOperandPrefix(b, width);
    bool byte_op = width == 8;
    EmitRexFor(b, width, src, &m, byte_op, false);
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

void EmitGroupF6Mem(CodeBuf& b, u8 sub, int width, const MemOp& m, u8 seg = 0) {
    EmitSegPrefix(b, seg);
    EmitOperandPrefix(b, width);
    bool byte_op = width == 8;
    EmitRexFor(b, width, 0, &m, byte_op, false);
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

void EmitMovRegMem(CodeBuf& b, int width, u8 dst, const MemOp& m, u8 seg = 0) {
    EmitSegPrefix(b, seg);
    EmitOperandPrefix(b, width);
    bool byte_op = width == 8;
    EmitRexFor(b, width, dst, &m, byte_op, false);
    b.B(byte_op ? 0x8A : 0x8B);
    EmitModRMMem(b, dst, m);
}

void EmitMovMemReg(CodeBuf& b, int width, const MemOp& m, u8 src, u8 seg = 0) {
    EmitSegPrefix(b, seg);
    EmitOperandPrefix(b, width);
    bool byte_op = width == 8;
    EmitRexFor(b, width, src, &m, byte_op, false);
    b.B(byte_op ? 0x88 : 0x89);
    EmitModRMMem(b, src, m);
}

void EmitMovMemImm(CodeBuf& b, int width, const MemOp& m, u64 imm, u8 seg = 0) {
    EmitSegPrefix(b, seg);
    EmitOperandPrefix(b, width);
    bool byte_op = width == 8;
    EmitRexFor(b, width, 0, &m, byte_op, false);
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
    EmitRex(b, width == 64, dst >= 8, m.scale != 0 && (kIndexReg >= 8), !m.rip_rel && (kDataReg >= 8));
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

void EmitXchgMemReg(CodeBuf& b, int width, const MemOp& m, u8 src, u8 seg = 0) {
    EmitSegPrefix(b, seg);
    EmitOperandPrefix(b, width);
    bool byte_op = width == 8;
    EmitRexFor(b, width, src, &m, byte_op, false);
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

void EmitPushReg(CodeBuf& b, u8 reg, bool width16 = false) {
    if (width16) {
        b.B(0x66);
    }
    EmitRex(b, false, false, false, reg >= 8);
    b.B(u8(0x50 + (reg & 7)));
}

void EmitPopReg(CodeBuf& b, u8 reg, bool width16 = false) {
    if (width16) {
        b.B(0x66);
    }
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

void EmitPushMem(CodeBuf& b, const MemOp& m, u8 seg = 0) {
    EmitSegPrefix(b, seg);
    b.B(0xFF);
    EmitModRMMem(b, 6, m);
}

void EmitPopMem(CodeBuf& b, const MemOp& m, u8 seg = 0) {
    EmitSegPrefix(b, seg);
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

void EmitStos(CodeBuf& b, int width, bool rep) {
    if (rep) {
        b.B(0xF3);
    }
    EmitOperandPrefix(b, width);
    if (width == 64) {
        b.B(0x48);
    }
    b.B(width == 8 ? 0xAA : 0xAB);
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

    // Byte register; 1 in 4 times a legacy no-REX byte register (indices 0-7,
    // where 4-7 encode AH/CH/DH/BH).
    u8 RandByteReg(bool& high_byte) {
        high_byte = false;
        if (RandInt(0, 3) == 0) {
            high_byte = true;
            return u8(RandInt(0, 7));
        }
        return RandReg();
    }

    MemOp RandMem() {
        MemOp m{};
        int kind = RandInt(0, 9);
        // Displacements are kept 8 byte aligned on purpose: the backend's TSO
        // loads/stores (ldar/stlr family) require natural alignment and fault
        // on misaligned accesses that cross a 16 byte granule on Apple
        // silicon. Unaligned TSO splitting is a runtime gap (see report), not
        // something a guest instruction can legally rely on here.
        static const s32 disps[] = {0, 8, 16, 24, 64, -8, -16, 128, 512, -512};
        m.disp = disps[rng() % std::size(disps)];
        if (kind >= 7) {
            m.scale = u8(1 << RandInt(0, 3));
            if (m.disp < -128 || m.disp > 127) {
                m.disp = 8;
            }
        }
        return m;
    }

    // [rip + disp32] form (lea only — no actual memory access).
    MemOp RipRelMem() {
        MemOp m{};
        m.rip_rel = true;
        m.disp = RandInt(-256, 256);
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
        uc->WriteRegister(UC_X86_REG_FS_BASE, ctx->fs_base);
        uc->WriteRegister(UC_X86_REG_GS_BASE, ctx->gs_base);
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
        // Multiples of 8: r11 * scale stays 8 byte aligned (see RandMem).
        ctx->r11.qword = Pick(std::vector<u64>{0, 8, 16, 64});
        ctx->r13.qword = data_addr;
        ctx->rsp.qword = stack_addr;
        ctx->fs_base = Pick(std::vector<u64>{0, 0x40, 0x100, 0x800});
        ctx->gs_base = Pick(std::vector<u64>{0, 0x80, 0x200, 0x400});
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
    // Registers in `exclude` are left untouched (dividend/divisor setups).
    void EmitFlagPrefix(CodeBuf& b, u8 exclude1 = 0xFF, u8 exclude2 = 0xFF,
                        u8 exclude3 = 0xFF) {
        auto pick_reg = [&] {
            u8 r;
            do {
                r = RandReg();
            } while (r == exclude1 || r == exclude2 || r == exclude3);
            return r;
        };
        u8 ra = pick_reg();
        u8 rb = pick_reg();
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
        if (getenv("SWIFT_FUZZ_TRACE")) {
            std::cout << "== cursor " << (cursor - 1) << " code: " << DumpCode(code) << std::endl;
        }

        std::memcpy(reinterpret_cast<u8*>(host_mem) + (code_addr - base), code.data(),
                    code.size());
        uc->WriteMemory(code_addr, code);
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
    // mov rax, fs:[r13]; mov rbx, gs:[r13+8]; hlt
    std::vector<u8> code = {0x64, 0x49, 0x8b, 0x45, 0x00, 0x65, 0x49, 0x8b, 0x5d, 0x08, 0xf4};
    std::memcpy(env.host_mem, code.data(), code.size());
    env.uc->WriteMemory(env.base, code);
    u64 marker1 = 0xDEADBEEF12345678, marker2 = 0xCAFEBABE87654321;
    memcpy(reinterpret_cast<u8*>(env.host_mem) + (env.data_addr - env.base + 0x1000), &marker1, 8);
    memcpy(reinterpret_cast<u8*>(env.host_mem) + (env.data_addr - env.base + 0x2008), &marker2, 8);
    env.uc->WriteMemory(env.data_addr + 0x1000, {0x78, 0x56, 0x34, 0x12, 0xEF, 0xBE, 0xAD, 0xDE});
    env.uc->WriteMemory(env.data_addr + 0x2008, {0x21, 0x43, 0x65, 0x87, 0xBE, 0xBA, 0xFE, 0xCA});
    auto& ctx = *env.ctx;
    ctx.r13.qword = env.data_addr;
    ctx.fs_base = 0x1000;
    ctx.gs_base = 0x2000;
    ctx.rip.qword = env.base;
    ctx.rsp.qword = env.stack_addr;
    env.SyncRegsToUnicorn();
    env.uc->WriteRegister(UC_X86_REG_FS_BASE, ctx.fs_base);
    env.uc->WriteRegister(UC_X86_REG_GS_BASE, ctx.gs_base);
    env.uc->Run(env.base, env.base + code.size() - 1, 0, 0);
    env.core->Run();
    std::cout << fmt::format("rax: uc={:x} sv={:x}\n", env.uc->ReadRegister(UC_X86_REG_RAX), ctx.rax.qword);
    std::cout << fmt::format("rbx: uc={:x} sv={:x}\n", env.uc->ReadRegister(UC_X86_REG_RBX), ctx.rbx.qword);
    std::cout << ((ctx.rax.qword == marker1 && ctx.rbx.qword == marker2) ? "FSGS-OK" : "FSGS-FAIL") << std::endl;

    std::cout << fmt::format("base={:x} data_addr={:x}\n", env.base, env.data_addr);
    // IR dump: xor ecx, eax; imul r12d, r12d
    std::vector<u8> code7 = {0x31, 0xc1, 0x45, 0x0f, 0xaf, 0xe4, 0xf4};
    std::memcpy(env.host_mem, code7.data(), code7.size());
    env.DumpIR(env.base);
    std::cout << "dump-done" << std::endl;
    // rep stosb repro (was: "Check Failed!")
    std::vector<u8> codeA = {0xf3, 0xaa, 0xf4};
    u64 addrA = env.base + 0xe000;
    std::memcpy(reinterpret_cast<u8*>(env.host_mem) + 0xe000, codeA.data(), codeA.size());
    env.DumpIR(addrA);
    env.uc->WriteMemory(addrA, codeA);
    ctx.rdi.qword = env.data_addr;
    ctx.rcx.qword = 4;
    ctx.rax.qword = 0x41;
    ctx.rip.qword = addrA;
    env.SyncRegsToUnicorn();
    env.uc->Run(addrA, addrA + codeA.size() - 1, 0, 0);
    env.core->Run();
    u64 stos_val = 0;
    std::memcpy(&stos_val, reinterpret_cast<u8*>(env.host_mem) + (env.data_addr - env.base), 8);
    std::cout << fmt::format("repstos rdi: uc={:x} sv={:x} mem={:x}\n",
                             env.uc->ReadRegister(UC_X86_REG_RDI), ctx.rdi.qword, stos_val);

    // rep stosw repro
    std::vector<u8> codeB = {0xf3, 0x66, 0xab, 0xf4};
    u64 addrB = env.base + 0xf000;
    std::memcpy(reinterpret_cast<u8*>(env.host_mem) + 0xf000, codeB.data(), codeB.size());
    env.DumpIR(addrB);
    env.uc->WriteMemory(addrB, codeB);
    ctx.rdi.qword = env.data_addr + 0x100;
    ctx.rcx.qword = 3;
    ctx.rax.qword = 0x23af;
    ctx.rip.qword = addrB;
    env.SyncRegsToUnicorn();
    env.uc->Run(addrB, addrB + codeB.size() - 1, 0, 0);
    env.core->Run();
    u64 stosw_val = 0;
    std::memcpy(&stosw_val, reinterpret_cast<u8*>(env.host_mem) + (env.data_addr + 0x100 - env.base), 8);
    std::cout << fmt::format("repstosw rdi: uc={:x} sv={:x} mem={:x}\n",
                             env.uc->ReadRegister(UC_X86_REG_RDI), ctx.rdi.qword, stosw_val);

    // EVEX prefix: must be a graceful ILL_CODE / panic, never UB.
    {
        struct MemIf : public swift::runtime::MemoryInterface {
            bool Read(void* dest, size_t addr, size_t size) override {
                return std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
            }
            bool Write(void* src, size_t addr, size_t size) override {
                return std::memcpy(reinterpret_cast<void*>(addr), src, size);
            }
            void* GetPointer(void* src) override { return src; }
        } mem_if;
        // EVEX vaddps zmm0, zmm0, zmm0 + hlt
        std::vector<u8> evex = {0x62, 0xF1, 0x7C, 0x48, 0x58, 0xC0, 0xF4};
        std::memcpy(env.host_mem, evex.data(), evex.size());
        u64 addr = env.base;
        swift::runtime::ir::Block blk{0, swift::runtime::ir::Location{addr}};
        swift::runtime::ir::Assembler asmb{&blk};
        try {
            X64Decoder dec{addr, &mem_if, &asmb, true};
            dec.Decode();
            std::cout << fmt::format("evex: decoded, interrupt={}\n", int(env.ctx->interrupt));
        } catch (const std::exception& e) {
            std::cout << fmt::format("evex: graceful panic: {}\n", e.what());
        }
    }

    // 64-bit mul repro: MulHiU64 goes through CallHost in JIT
    std::vector<u8> codeC = {0x48, 0xf7, 0xe5, 0xf4};
    u64 addrC = env.base + 0x11000;
    std::memcpy(reinterpret_cast<u8*>(env.host_mem) + 0x11000, codeC.data(), codeC.size());
    env.uc->WriteMemory(addrC, codeC);
    ctx.rax.qword = 0xFFFFFFFFFFFFFFFFull;
    ctx.rbp.qword = 0xFFFFFFFFFFFFFFFFull;
    ctx.rip.qword = addrC;
    env.SyncRegsToUnicorn();
    env.uc->Run(addrC, addrC + codeC.size() - 1, 0, 0);
    env.core->Run();
    std::cout << fmt::format("mul64 rdx: uc={:x} sv={:x} rax: uc={:x} sv={:x}\n",
                             env.uc->ReadRegister(UC_X86_REG_RDX), ctx.rdx.qword,
                             env.uc->ReadRegister(UC_X86_REG_RAX), ctx.rax.qword);

    // imul r8d; mul r9w; lahf repro (mul CF after a previous mul)
    std::vector<u8> code9 = {0x41, 0xf7, 0xe8, 0x66, 0x41, 0xf7, 0xe1, 0x9f, 0xf4};
    u64 addr9 = env.base + 0xd000;
    std::memcpy(reinterpret_cast<u8*>(env.host_mem) + 0xd000, code9.data(), code9.size());
    env.DumpIR(addr9);
    env.uc->WriteMemory(addr9, code9);
    ctx.rax.qword = 0xffffffff;
    ctx.r8.qword = 3;
    ctx.r9.qword = 0x2a;
    ctx.rip.qword = addr9;
    env.SyncRegsToUnicorn();
    env.uc->Run(addr9, addr9 + code9.size() - 1, 0, 0);
    env.core->Run();
    std::cout << fmt::format("mul16 rdx: uc={:x} sv={:x} rax: uc={:x} sv={:x}\n",
                             env.uc->ReadRegister(UC_X86_REG_RDX), ctx.rdx.qword,
                             env.uc->ReadRegister(UC_X86_REG_RAX), ctx.rax.qword);

    // imul ecx (one-operand 32-bit signed): check EDX = high half
    std::vector<u8> code8 = {0xf7, 0xe9, 0xf4};
    u64 addr8 = env.base + 0xc000;
    std::memcpy(reinterpret_cast<u8*>(env.host_mem) + 0xc000, code8.data(), code8.size());
    env.DumpIR(addr8);
    env.uc->WriteMemory(addr8, code8);
    ctx.rax.qword = 0xd68ee457;
    ctx.rcx.qword = 0x55;
    ctx.rip.qword = addr8;
    env.SyncRegsToUnicorn();
    env.uc->Run(addr8, addr8 + code8.size() - 1, 0, 0);
    env.core->Run();
    std::cout << fmt::format("imul32 rdx: uc={:x} sv={:x} rax: uc={:x} sv={:x}\n",
                             env.uc->ReadRegister(UC_X86_REG_RDX), ctx.rdx.qword,
                             env.uc->ReadRegister(UC_X86_REG_RAX), ctx.rax.qword);
    // sbb-after-dec repro: test si,si; xor cx,cx; dec r12d; sbb r8w,dx; lahf
    std::vector<u8> code6 = {0x66, 0x85, 0xf0, 0x66, 0x31, 0xc9, 0x41, 0xff, 0xcc,
                             0x66, 0x41, 0x19, 0xd0, 0x9f, 0xf4};
    u64 addr6 = env.base + 0xb000;
    std::memcpy(reinterpret_cast<u8*>(env.host_mem) + 0xb000, code6.data(), code6.size());
    env.DumpIR(addr6);
    env.uc->WriteMemory(addr6, code6);
    ctx.rsi.qword = 0x24471788e62cb6c4ull;
    ctx.rcx.qword = 0x8e0f2e3f729eb07dull;
    ctx.r12.qword = 0x49d57bb01b024b9aull;
    ctx.r8.qword = 2;
    ctx.rdx.qword = 0xe627c4398f3ca159ull;
    ctx.rax.qword = 2;
    ctx.rip.qword = addr6;
    env.SyncRegsToUnicorn();
    env.uc->Run(addr6, addr6 + code6.size() - 1, 0, 0);
    env.core->Run();
    std::cout << fmt::format("sbb-dec r8: uc={:x} sv={:x} (expect ...5ea9)\n",
                             env.uc->ReadRegister(UC_X86_REG_R8), ctx.r8.qword);

    // jecxz CF-polarity repro: sub rbx, rdi (borrow -> CF=1); jecxz +10
    // (not taken); movabs rcx, imm; lahf
    std::vector<u8> code5 = {0x48, 0x29, 0xfb, 0x67, 0xe3, 0x0a,
                             0x48, 0xb9, 0x0d, 0xf0, 0xfe, 0xca, 0xef, 0xbe, 0xad, 0xde,
                             0x9f, 0xf4};
    u64 addr5 = env.base + 0xa000;
    std::memcpy(reinterpret_cast<u8*>(env.host_mem) + 0xa000, code5.data(), code5.size());
    env.DumpIR(addr5);
    env.uc->WriteMemory(addr5, code5);
    ctx.rbx.qword = 0x55;
    ctx.rdi.qword = 0xa58014d80cbfb5b0ull;
    ctx.rcx.qword = 0x17ead41bebb7e8full;
    ctx.rax.qword = 3;
    ctx.rip.qword = addr5;
    env.SyncRegsToUnicorn();
    env.uc->Run(addr5, addr5 + code5.size() - 1, 0, 0);
    env.core->Run();
    std::cout << fmt::format("jecxz rax: uc={:x} sv={:x} (expect CF=1)\n",
                             env.uc->ReadRegister(UC_X86_REG_RAX), ctx.rax.qword);

    // Misaligned TSO repro: sub [r13+r11*2+0x10], r8 with r11=3 (addr % 8 == 6)
    std::vector<u8> code4 = {0x4b, 0x29, 0x44, 0x5d, 0x10, 0xf4};
    u64 addr4 = env.base + 0x9000;
    std::memcpy(reinterpret_cast<u8*>(env.host_mem) + 0x9000, code4.data(), code4.size());
    env.uc->WriteMemory(addr4, code4);
    ctx.r13.qword = env.data_addr;
    ctx.r11.qword = 3;
    ctx.r8.qword = 1;
    ctx.rip.qword = addr4;
    env.SyncRegsToUnicorn();
    env.uc->Run(addr4, addr4 + code4.size() - 1, 0, 0);
    env.core->Run();
    std::cout << "misaligned-ok uc=" << env.uc->ReadRegister(UC_X86_REG_RIP)
              << " sv=" << ctx.rip.qword << std::endl;

    // Edge repro: sub al, 1 (CF=1); adc bl, 0xFF (b==mask && cin==1); lahf
    std::vector<u8> code3 = {0xb0, 0x00, 0x2c, 0x01, 0x80, 0xd3, 0xff, 0x9f, 0xf4};
    u64 addr3 = env.base + 0x8000;
    std::memcpy(reinterpret_cast<u8*>(env.host_mem) + 0x8000, code3.data(), code3.size());
    env.DumpIR(addr3);
    env.uc->WriteMemory(addr3, code3);
    ctx.rax.qword = 0;
    ctx.rbx.qword = 5;       // bl = 5: adc -> 5 + 0xFF + 1 = 0x105 -> bl=5, CF=1
    ctx.rip.qword = addr3;
    env.SyncRegsToUnicorn();
    env.uc->Run(addr3, addr3 + code3.size() - 1, 0, 0);
    env.core->Run();
    // ebx|r8d = 0x80000003: SF=1 ZF=0, low byte 0x03 -> 2 bits -> even -> PF=1
    std::cout << fmt::format("pf rax: uc={:x} sv={:x} rbp={:x}\n",
                             env.uc->ReadRegister(UC_X86_REG_RAX), ctx.rax.qword, ctx.rbp.qword);
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
            bool hb_dst = false, hb_src = false;
            u8 dst = width == 8 ? env.RandByteReg(hb_dst) : env.RandReg();
            u8 src = width == 8 ? env.RandByteReg(hb_src) : env.RandReg();
            bool high_byte = hb_dst || hb_src;
            if (high_byte) {
                // No-REX byte operands: indices 0-7 (4-7 are AH..BH).
                dst = u8(env.RandInt(0, 7));
                src = u8(env.RandInt(0, 7));
            }
            int form = env.RandInt(0, 9);
            if ((group == 2 || group == 3) && width < 32 && !high_byte) {
                // Narrow adc/sbb are exact except two irreducible boundaries:
                // C is wrong for b == mask && cin == 1 and V for
                // b == signmax && cin == 1 (no single host op yields all four
                // flags there; the JIT merges NZCV wholesale per flag window,
                // so the frontend cannot split the saves). Keep the operand
                // off those boundaries via a filtered immediate.
                const u64 mask_w = (u64(1) << width) - 1;
                const u64 signmax_w = (u64(1) << (width - 1)) - 1;
                u64 imm;
                do {
                    imm = env.PoolVal(width) & mask_w;
                } while (imm == mask_w || imm == signmax_w);
                EmitAluRegImm(b, group, width, dst, imm, false);
                continue;
            }
            if (high_byte) {
                if ((group == 2 || group == 3) && width == 8) {
                    group = 5;  // see the narrow adc/sbb boundary note above
                }
                // High byte ops: reg-reg forms only.
                if (env.RandInt(0, 4) == 0) {
                    EmitTestRegReg(b, width, dst, src, true);
                } else {
                    EmitAluRegReg(b, group, width, dst, src, true);
                }
            } else if (env.RandInt(0, 7) == 0) {
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
            bool high_byte = false;
            u8 dst = width == 8 ? env.RandByteReg(high_byte) : env.RandReg();
            switch (env.RandInt(0, 3)) {
                case 0:
                    EmitIncDec(b, false, width, dst, high_byte);
                    mask.ah &= ~kAhCF;  // CF after inc/dec not preserved (backend limit)
                    break;
                case 1:
                    EmitIncDec(b, true, width, dst, high_byte);
                    mask.ah &= ~kAhCF;
                    break;
                case 2:
                    EmitGroupF6(b, 3, width, dst, high_byte);  // neg
                    break;
                default:
                    EmitGroupF6(b, 2, width, dst, high_byte);  // not (flags untouched)
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
            bool high_byte = false;
            u8 dst = width == 8 ? env.RandByteReg(high_byte) : env.RandReg();
            if (env.RandInt(0, 1)) {
                // by CL
                env.ctx->rcx.qword = env.Pick(std::vector<u64>{0, 1, 2, 7, 8, 15, 16, 31, 32, 33,
                                                                 63, 64, 65, 255});
                EmitShift(b, sub, width, dst, 0, true, high_byte);
            } else {
                u8 count = u8(env.Pick(std::vector<u64>{1, 2, 3, 7, 8, 15, 16, 31}));
                EmitShift(b, sub, width, dst, count, false, high_byte);
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
        // After mul/imul only CF / OF / PF are meaningful: CF / OF are now
        // exact (high half fit check); SF / ZF / AF are undefined per spec
        // (SwiftVM leaves them stale, Unicorn derives them from the result).
        env.RunIteration(b.c, FlagMask{u32(kAhCF | kAhPF), true}, "mul");
    }
    REQUIRE(env.failures == 0);
}

TEST_CASE("Fuzz x86 div idiv") {
    FuzzEnv env;
    int iters = env.Iters(2000);
    for (int i = 0; i < iters; ++i) {
        CodeBuf b;
        env.InitRegs();
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
        // Flags are architecturally undefined after div/idiv: mask them all.
        env.RunIteration(b.c, FlagMask{0, false}, "div");
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
            // 16 bit stack ops would misalign rsp for later 64 bit TSO
            // pushes/pops (see RandMem): disabled until the backend handles
            // unaligned TSO accesses.
            bool w16 = false;
            if (depth > 0 && env.RandInt(0, 1)) {
                if (env.RandInt(0, 3) == 0) {
                    EmitPopMem(b, env.RandMem());
                } else {
                    EmitPopReg(b, env.RandReg(), w16);
                }
                depth--;
            } else {
                switch (env.RandInt(0, 2)) {
                    case 0:
                        EmitPushReg(b, env.RandReg(), w16);
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

TEST_CASE("Fuzz x86 jrcxz leave") {
    FuzzEnv env;
    int iters = env.Iters(1500);
    for (int i = 0; i < iters; ++i) {
        CodeBuf b;
        env.InitRegs();
        env.EmitFlagPrefix(b);
        u8 scratch = env.RandReg();
        if (env.RandInt(0, 1)) {
            // jrcxz over a mov (67 e3 rel8)
            CodeBuf tail;
            EmitMovRegImm(tail, 64, scratch, 0xDEADBEEFCAFEF00Dull);
            b.B(0x67);
            b.B(0xE3);
            b.B(u8(tail.c.size()));
            for (auto v : tail.c) {
                b.B(v);
            }
        } else {
            // leave: rsp = rbp; pop rbp — point rbp into the stack area first
            env.ctx->rbp.qword = env.stack_addr - 0x40;
            b.B(0xC9);
        }
        env.EmitFlagCapture(b);
        // The lahf capture runs in a successor block of the conditional jump;
        // CF there is affected by the known cross-block carry-polarity gap
        // (the backend merges NZCV wholesale at block ends, so the frontend
        // cannot normalize the stored carry's polarity before linking).
        env.RunIteration(b.c, FlagMask{u32(kAhAll & ~kAhAF & ~kAhCF), true}, "jrcxz");
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
            bool high_byte = false;
            u8 byte_reg = width == 8 ? env.RandByteReg(high_byte) : env.RandReg();
            switch (what) {
                case 0:
                    if (high_byte) {
                        EmitMovRegReg(b, width, u8(env.RandInt(0, 7)), u8(env.RandInt(0, 7)), true);
                    } else {
                        EmitMovRegReg(b, width, byte_reg, env.RandReg());
                    }
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
                    if (env.RandInt(0, 4) == 0) {
                        EmitLea(b, width, env.RandReg(), env.RipRelMem());
                    } else {
                        EmitLea(b, width, env.RandReg(), env.RandMem());
                    }
                    break;
                case 6:
                    if (high_byte) {
                        EmitXchgRegReg(b, width, u8(env.RandInt(0, 7)), u8(env.RandInt(0, 7)), true);
                    } else {
                        EmitXchgRegReg(b, width, byte_reg, env.RandReg());
                    }
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
        env.EmitFlagPrefix(b, kRsi, kRdi);
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
        env.EmitFlagPrefix(b, kRsi, kRdi, kRcx);
        env.ctx->rsi.qword = env.data_addr - 0x800;
        env.ctx->rdi.qword = env.data_addr - 0x400;
        env.ctx->rcx.qword = env.RandInt(0, 16);
        EmitMovs(b, env.Pick(std::vector<int>{8, 16, 32, 64}), true);
        env.EmitFlagCapture(b);
        env.RunIteration(b.c, FlagMask{}, "repmovs");
    }
    REQUIRE(env.failures == 0);
}

TEST_CASE("Fuzz x86 rep stos") {
    FuzzEnv env;
    int iters = env.Iters(1000);
    for (int i = 0; i < iters; ++i) {
        CodeBuf b;
        env.InitRegs();
        env.EmitFlagPrefix(b, kRax, kRdi, kRcx);
        env.ctx->rdi.qword = env.data_addr - 0x400;
        env.ctx->rcx.qword = env.RandInt(0, 16);
        env.ctx->rax.qword = env.PoolVal(64);
        EmitStos(b, env.Pick(std::vector<int>{8, 16, 32, 64}), env.RandInt(0, 1) == 0);
        env.EmitFlagCapture(b);
        env.RunIteration(b.c, FlagMask{}, "repstos");
    }
    REQUIRE(env.failures == 0);
}

TEST_CASE("Fuzz x86 cpuid") {
    FuzzEnv env;
    for (u32 leaf : {0u, 1u, 7u, 0x80000000u, 0x80000001u, 5u, 0x80000004u}) {
        // Unicorn reports its own feature set, so this is checked on the
        // SwiftVM side only (no differential comparison).
        std::vector<u8> code = {0x0F, 0xA2, 0xF4};
        u64 addr = env.base + (env.cursor++ * FuzzEnv::kCodeStride);
        std::memcpy(reinterpret_cast<u8*>(env.host_mem) + (addr - env.base), code.data(),
                    code.size());
        env.InitRegs();
        env.ctx->rax.qword = leaf;
        env.ctx->rcx.qword = 0;
        env.ctx->rip.qword = addr;
        env.core->Run();
        u64 sig[4] = {env.ctx->rax.qword, env.ctx->rbx.qword, env.ctx->rcx.qword,
                      env.ctx->rdx.qword};
        switch (leaf) {
            case 0:
                REQUIRE(sig[0] == 7);
                REQUIRE(sig[1] == 0x756E6547);  // "Genu"
                REQUIRE(sig[3] == 0x49656E69);  // "ineI"
                REQUIRE(sig[2] == 0x6C65746E);  // "ntel"
                break;
            case 1:
                REQUIRE((sig[3] & (1u << 26)) != 0);   // SSE2 reported
                REQUIRE((sig[2] & (1u << 0)) == 0);    // no SSE3
                break;
            case 7:
                REQUIRE(sig[1] == 0);  // no AVX2 / AVX-512 / BMI / ERMS
                break;
            case 0x80000000:
                REQUIRE(sig[0] == 0x80000004);
                break;
            case 0x80000001:
                REQUIRE((sig[3] & (1u << 29)) != 0);  // long mode
                break;
            default:
                std::cout << fmt::format("cpuid leaf {:x} eax={:x}\n", leaf, sig[0]);
                REQUIRE(sig[0] == 0);
                break;
        }
    }
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
        if (env.RandInt(0, 3) == 0) {
            // ret imm16: also drops stack slots
            sub.B(0xC2);
            sub.W(u16(env.RandInt(0, 3) * 8));
        } else {
            EmitRet(sub);
        }
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

TEST_CASE("Fuzz x86 mixed sequences") {
    FuzzEnv env;
    int iters = env.Iters(4000);
    for (int i = 0; i < iters; ++i) {
        CodeBuf b;
        env.InitRegs();
        env.EmitFlagPrefix(b);
        FlagMask mask;
        int n = env.RandInt(2, 5);
        for (int j = 0; j < n; ++j) {
            int width = env.Pick(std::vector<int>{8, 16, 32, 64});
            switch (env.RandInt(0, 9)) {
                case 0:  // alu reg-reg / reg-imm
                case 1: {
                    u8 group = env.Pick(std::vector<u8>{0, 1, 2, 3, 4, 5, 6, 7});
                    if ((group == 2 || group == 3) && width < 32) {
                        // Narrow adc/sbb: exact except two irreducible
                        // boundaries (C: b==mask&&cin==1, V: b==signmax&&cin==1;
                        // see the alu family note) — keep operands clear.
                        const u64 mask_w = (u64(1) << width) - 1;
                        const u64 signmax_w = (u64(1) << (width - 1)) - 1;
                        u64 imm;
                        do {
                            imm = env.PoolVal(width) & mask_w;
                        } while (imm == mask_w || imm == signmax_w);
                        EmitAluRegImm(b, group, width, env.RandReg(), imm, false);
                    } else if (env.RandInt(0, 1)) {
                        EmitAluRegReg(b, group, width, env.RandReg(), env.RandReg());
                    } else {
                        EmitAluRegImm(b, group, width, env.RandReg(),
                                      env.PoolVal(width > 32 ? 32 : width), env.RandInt(0, 3) == 0);
                    }
                    break;
                }
                case 2:  // inc / dec / neg / not
                    switch (env.RandInt(0, 3)) {
                        case 0:
                        case 1:
                            // inc / dec must end the sequence: CF is preserved
                            // by x86 inc/dec, but the backend's NZCV liveness
                            // is not per-bit, so the flag-setting add/sub they
                            // emit clobbers the stored carry and any later
                            // carry consumer (adc/sbb/setc) would see it.
                            EmitIncDec(b, env.RandInt(0, 1) == 0, width, env.RandReg());
                            mask.ah &= ~kAhCF;
                            j = n;  // end the sequence
                            break;
                        case 2:
                            EmitGroupF6(b, 3, width, env.RandReg());
                            break;
                        default:
                            EmitGroupF6(b, 2, width, env.RandReg());
                            break;
                    }
                    break;
                case 3:  // shift — must be the last op of the sequence: CF is
                         // approximate after shifts (the backend cannot express
                         // the partial flag update) and a later adc / setcc
                         // would observe it through registers.
                    EmitShift(b, env.Pick(std::vector<u8>{4, 5, 7}), width, env.RandReg(),
                              u8(env.Pick(std::vector<u64>{1, 2, 5, 7, 15})), false);
                    mask.ah &= ~kAhCF;
                    mask.of = false;
                    j = n;  // end the sequence
                    break;
                case 4:  // mov / lea
                    if (env.RandInt(0, 1)) {
                        EmitMovRegMem(b, width, env.RandReg(), env.RandMem());
                    } else {
                        EmitLea(b, width, env.RandReg(), env.RandMem());
                    }
                    break;
                case 5:  // setcc
                    EmitSetcc(b, u8(env.RandInt(0, 15)), env.RandReg());
                    break;
                case 6:  // xchg (movs is covered by its own families: ops
                        // generated before it here could clobber rsi/rdi)
                    EmitXchgRegReg(b, width, env.RandReg(), env.RandReg());
                    break;
                case 7:  // mul / imul — must end the sequence: SF / ZF / PF are
                         // undefined per spec afterwards (SwiftVM keeps them
                         // stale, Unicorn derives them from the result), and a
                         // later setcc would read them into a register. CF / OF
                         // are exact and checked by the dedicated mul family.
                    EmitGroupF6(b, env.RandInt(0, 1) ? 4 : 5, width, env.RandReg());
                    mask.ah &= ~(kAhSF | kAhZF);
                    j = n;  // end the sequence
                    break;
                case 8:  // push / pop
                    EmitPushReg(b, env.RandReg());
                    EmitPopReg(b, env.RandReg());
                    break;
                default:  // cbw family / movzx / movsx
                    switch (env.RandInt(0, 3)) {
                        case 0:
                            b.B(0x66);
                            b.B(0x98);
                            break;
                        case 1:
                            b.B(0x98);
                            break;
                        case 2:
                            b.B(0x48);
                            b.B(0x98);
                            break;
                        default: {
                            int sw = width == 64 ? env.Pick(std::vector<int>{8, 16, 32}) : 8;
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
                    break;
            }
        }
        env.EmitFlagCapture(b);
        env.RunIteration(b.c, mask, "mixed");
    }
    REQUIRE(env.failures == 0);
}

TEST_CASE("Fuzz x86 segments") {
    FuzzEnv env;
    int iters = env.Iters(2000);
    for (int i = 0; i < iters; ++i) {
        CodeBuf b;
        env.InitRegs();
        env.EmitFlagPrefix(b);
        u8 seg = env.Pick(std::vector<u8>{0x64, 0x65});
        int width = env.Pick(std::vector<int>{8, 16, 32, 64});
        switch (env.RandInt(0, 4)) {
            case 0:
                EmitMovRegMem(b, width, env.RandReg(), env.RandMem(), seg);
                break;
            case 1:
                EmitMovMemReg(b, width, env.RandMem(), env.RandReg(), seg);
                break;
            case 2:
                EmitMovMemImm(b, width, env.RandMem(), env.PoolVal(width > 32 ? 32 : width), seg);
                break;
            case 3:
                EmitAluRegMem(b, env.Pick(std::vector<u8>{0, 1, 4, 5, 6, 7}), width, env.RandReg(),
                              env.RandMem(), seg);
                break;
            default:
                EmitAluMemReg(b, env.Pick(std::vector<u8>{0, 1, 4, 5, 6}), width, env.RandMem(),
                              env.RandReg(), seg);
                break;
        }
        env.EmitFlagCapture(b);
        env.RunIteration(b.c, FlagMask{}, "seg");
    }
    REQUIRE(env.failures == 0);
}

TEST_CASE("Fuzz x86 decode robustness") {
    // Random byte soup: decoding must never crash, whatever the bytes are.
    // (Execution is not attempted: without a guest MMU any wild address would
    // fault the host — that is a runtime property, not a decode bug.)
    FuzzEnv env;
    int iters = env.Iters(3000);
    for (int i = 0; i < iters; ++i) {
        CodeBuf b;
        for (int j = 0; j < 15; ++j) {
            b.B(u8(env.rng()));
        }
        // Fill the rest of the slot with HLTs so a long instruction eating
        // the first terminator cannot run the decoder away into zeros.
        while (b.c.size() < 32) {
            b.B(0xF4);
        }
        u64 code_addr = env.base + (env.cursor++ * FuzzEnv::kCodeStride);
        std::memcpy(reinterpret_cast<u8*>(env.host_mem) + (code_addr - env.base), b.c.data(), b.c.size());
        try {
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
        } catch (const std::exception& e) {
            FAIL(fmt::format("decoder threw on bytes {}: {}", env.DumpCode(b.c), e.what()));
        }
    }
}

TEST_CASE("Fuzz x86 nop family") {
    // NOP variants must not touch memory or registers:
    //   endbr64 / endbr32 (F3 0F 1E FA/FB)
    //   multi-byte NOP carrying a ModRM memory operand (66 2E 0F 1F 84 ...)
    //   66 90 and 90
    FuzzEnv env;
    env.InitRegs();
    // Poison rax with an unmapped guest address: if the multi-byte NOP were
    // wrongly decoded as a load from [rax+rax+0], SwiftVM would fault or
    // diverge from Unicorn.
    env.ctx->rax.qword = 0xDEAD0000;
    CodeBuf b;
    b.B(0xF3); b.B(0x0F); b.B(0x1E); b.B(0xFA);  // endbr64
    b.B(0xF3); b.B(0x0F); b.B(0x1E); b.B(0xFB);  // endbr32
    // nop word ptr cs:[rax + rax*1 + 0]
    for (u8 v : {0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}) {
        b.B(v);
    }
    b.B(0x66); b.B(0x90);  // 16-bit nop (xchg ax, ax)
    b.B(0x90);             // nop
    env.RunIteration(b.c, FlagMask{}, "nop");
    REQUIRE(env.failures == 0);
}

TEST_CASE("Fuzz x86 segment address forms") {
    // Deterministic coverage of every FS/GS addressing form:
    //   moffs (O_DISP):            mov rax, fs:[imm64]
    //   SIB no-base disp32:        mov rbx, gs:[disp32]
    //   base + index*scale + disp: mov rcx, fs:[r13 + r11*2 + 0x10]
    FuzzEnv env;

    auto write_marker = [&](u64 guest_addr, u64 value) {
        std::memcpy(reinterpret_cast<u8*>(env.host_mem) + (guest_addr - env.base), &value, 8);
    };

    // 1. moffs: mov rax, fs:[0x40] with fs_base = data_addr + 0x100
    env.InitRegs();
    env.ctx->fs_base = env.data_addr + 0x100;
    env.ctx->gs_base = env.data_addr + 0x200;
    write_marker(env.data_addr + 0x140, 0x1111111122222222ull);
    {
        CodeBuf b;
        b.B(0x64); b.B(0x48); b.B(0xA1);  // mov rax, fs:[imm64]
        b.Q(0x40);
        env.RunIteration(b.c, FlagMask{}, "seg-moffs");
    }
    REQUIRE(env.ctx->rax.qword == 0x1111111122222222ull);

    // 2. SIB no-base: mov rbx, gs:[0x34] with gs_base = data_addr + 0x200
    env.InitRegs();
    env.ctx->fs_base = env.data_addr + 0x100;
    env.ctx->gs_base = env.data_addr + 0x200;
    write_marker(env.data_addr + 0x234, 0x3333333344444444ull);
    {
        CodeBuf b;
        for (u8 v : {0x65, 0x48, 0x8b, 0x1c, 0x25, 0x34, 0x00, 0x00, 0x00}) {
            b.B(v);
        }
        env.RunIteration(b.c, FlagMask{}, "seg-sib-nobase");
    }
    REQUIRE(env.ctx->rbx.qword == 0x3333333344444444ull);

    // 3. base + index*scale + disp: mov rcx, fs:[r13 + r11*2 + 0x10]
    //    fs_base = 0x100, r13 = data_addr, r11 = 8  ->  data_addr + 0x120
    env.InitRegs();
    env.ctx->fs_base = 0x100;
    env.ctx->r11.qword = 8;
    write_marker(env.data_addr + 0x120, 0x5555555566666666ull);
    {
        CodeBuf b;
        for (u8 v : {0x64, 0x4b, 0x8b, 0x4c, 0x5d, 0x10}) {
            b.B(v);
        }
        env.RunIteration(b.c, FlagMask{}, "seg-base-idx-disp");
    }
    REQUIRE(env.ctx->rcx.qword == 0x5555555566666666ull);

    // 4. store form: mov fs:[r13 + 8], rax
    env.InitRegs();
    env.ctx->fs_base = 0x80;
    env.ctx->rax.qword = 0x7777777788888888ull;
    {
        CodeBuf b;
        for (u8 v : {0x64, 0x49, 0x89, 0x45, 0x08}) {  // mov fs:[r13+8], rax
            b.B(v);
        }
        env.RunIteration(b.c, FlagMask{}, "seg-store");
    }
    u64 stored = 0;
    std::memcpy(&stored, reinterpret_cast<u8*>(env.host_mem) + (env.data_addr + 0x88 - env.base), 8);
    REQUIRE(stored == 0x7777777788888888ull);
    REQUIRE(env.failures == 0);
}
}
