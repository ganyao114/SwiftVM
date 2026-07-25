#include <atomic>
#include <cstring>
#include "runtime/frontend/x86/decoder_internal.h"

namespace swift::x86 {

using namespace swift::runtime::frontend;

#define __ assembler->

namespace {
std::atomic<u64> g_guest_mem_bias{0};
// Memory ordering mode installed by the embedding translator (Config::tso_mode
// -> x86::SetTsoMode). Relaxed by default: correct for single-threaded guests.
std::atomic<u8> g_tso_mode{static_cast<u8>(runtime::TsoMode::Relaxed)};
}

void SetGuestMemBias(u64 bias) { g_guest_mem_bias.store(bias, std::memory_order_relaxed); }
u64 GetGuestMemBias() { return g_guest_mem_bias.load(std::memory_order_relaxed); }

void SetTsoMode(runtime::TsoMode mode) {
    g_tso_mode.store(static_cast<u8>(mode), std::memory_order_relaxed);
}
runtime::TsoMode GetTsoMode() {
    return static_cast<runtime::TsoMode>(g_tso_mode.load(std::memory_order_relaxed));
}

static void RepMovs(u64 dst, u64 src, u64 bytes) {
    const u64 bias = g_guest_mem_bias.load(std::memory_order_relaxed);
    std::memmove(reinterpret_cast<void*>(dst + bias), reinterpret_cast<const void*>(src + bias), bytes);
}

// rep stos fill helpers, one per element size (CallHost takes 3 args max).
// They return the end address: the call result feeds the RDI update, which
// keeps the host call alive in the JIT pipeline.
static u64 RepStos1(u64 dst, u64 value, u64 count) {
    const u64 bias = g_guest_mem_bias.load(std::memory_order_relaxed);
    std::memset(reinterpret_cast<void*>(dst + bias), int(value & 0xFF), count);
    return dst + count;
}
static u64 RepStos2(u64 dst, u64 value, u64 count) {
    const u64 bias = g_guest_mem_bias.load(std::memory_order_relaxed);
    auto* p = reinterpret_cast<u8*>(dst + bias);
    for (u64 i = 0; i < count; ++i) {
        std::memcpy(p + i * 2, &value, 2);
    }
    return dst + count * 2;
}
static u64 RepStos4(u64 dst, u64 value, u64 count) {
    const u64 bias = g_guest_mem_bias.load(std::memory_order_relaxed);
    auto* p = reinterpret_cast<u8*>(dst + bias);
    for (u64 i = 0; i < count; ++i) {
        std::memcpy(p + i * 4, &value, 4);
    }
    return dst + count * 4;
}
static u64 RepStos8(u64 dst, u64 value, u64 count) {
    const u64 bias = g_guest_mem_bias.load(std::memory_order_relaxed);
    auto* p = reinterpret_cast<u8*>(dst + bias);
    for (u64 i = 0; i < count; ++i) {
        std::memcpy(p + i * 8, &value, 8);
    }
    return dst + count * 8;
}

// rep cmps/scas: run the early-terminating comparison loop and return the
// number of elements actually compared (the decoder reloads the final pair to
// produce flags). REPZ (repnz=0) stops on the first not-equal element; REPNZ
// (repnz=1) stops on the first equal element. Separate Z/NZ entry points keep
// the decoder from packing a mode bit with a flag-clobbering OR.
static u64 RepCmpsN(const u8* s, const u8* d, u64 count, u64 repnz, u64 sz) {
    u64 i = 0;
    while (i < count) {
        bool eq = std::memcmp(s + i * sz, d + i * sz, sz) == 0;
        ++i;
        if (repnz ? eq : !eq) {
            break;
        }
    }
    return i;
}
#define DEFINE_REP_CMPS(name, sz, repnz)                                                           \
    static u64 name(u64 rsi, u64 rdi, u64 count) {                                                 \
        const u64 bias = g_guest_mem_bias.load(std::memory_order_relaxed);                         \
        return RepCmpsN(reinterpret_cast<const u8*>(rsi + bias),                                  \
                         reinterpret_cast<const u8*>(rdi + bias), count, repnz, sz);               \
    }
DEFINE_REP_CMPS(RepCmpsZ1, 1, 0)
DEFINE_REP_CMPS(RepCmpsNZ1, 1, 1)
DEFINE_REP_CMPS(RepCmpsZ2, 2, 0)
DEFINE_REP_CMPS(RepCmpsNZ2, 2, 1)
DEFINE_REP_CMPS(RepCmpsZ4, 4, 0)
DEFINE_REP_CMPS(RepCmpsNZ4, 4, 1)
DEFINE_REP_CMPS(RepCmpsZ8, 8, 0)
DEFINE_REP_CMPS(RepCmpsNZ8, 8, 1)
#undef DEFINE_REP_CMPS

// rep scas: compare the accumulator (acc) against each [RDI] element.
static u64 RepScasN(const u8* acc, const u8* d, u64 count, u64 repnz, u64 sz) {
    u64 i = 0;
    while (i < count) {
        bool eq = std::memcmp(acc, d + i * sz, sz) == 0;
        ++i;
        if (repnz ? eq : !eq) {
            break;
        }
    }
    return i;
}
#define DEFINE_REP_SCAS(name, sz, repnz)                                                           \
    static u64 name(u64 acc, u64 rdi, u64 count) {                                                 \
        const u64 bias = g_guest_mem_bias.load(std::memory_order_relaxed);                         \
        u8 ab[sz];                                                                                 \
        std::memcpy(ab, &acc, sz);                                                                 \
        return RepScasN(ab, reinterpret_cast<const u8*>(rdi + bias), count, repnz, sz);            \
    }
DEFINE_REP_SCAS(RepScasZ1, 1, 0)
DEFINE_REP_SCAS(RepScasNZ1, 1, 1)
DEFINE_REP_SCAS(RepScasZ2, 2, 0)
DEFINE_REP_SCAS(RepScasNZ2, 2, 1)
DEFINE_REP_SCAS(RepScasZ4, 4, 0)
DEFINE_REP_SCAS(RepScasNZ4, 4, 1)
DEFINE_REP_SCAS(RepScasZ8, 8, 0)
DEFINE_REP_SCAS(RepScasNZ8, 8, 1)
#undef DEFINE_REP_SCAS

// fxsave: zero the 512-byte region and plant the architectural defaults
// (FCW = 0x037F, MXCSR_MASK = 0x0000FFFF); the decoder then stores the live
// mxcsr and xmm0-15 over it via IR.
u64 FxsaveFill(u64 guest_addr) {
    const u64 bias = g_guest_mem_bias.load(std::memory_order_relaxed);
    auto* p = reinterpret_cast<u8*>(guest_addr + bias);
    std::memset(p, 0, 512);
    u16 fcw = 0x037F;
    std::memcpy(p, &fcw, 2);
    u32 mask = 0x0000FFFF;
    std::memcpy(p + 28, &mask, 4);
    return 0;
}

// ---------------------------------------------------------------------------

void X64Decoder::DecodeMovs(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    if ((insn.flags & FLAG_REP) || (insn.flags & FLAG_REPNZ)) {
        // TODO: DF (direction flag) and segment overrides are not modelled,
        // assume DF == 0 and default segments. The IR MemoryCopyTSO op takes an
        // immediate count, so a dynamic RCX count goes through a host call.
        auto size = GetSize(op0.size);
        auto src_reg = is_64bit ? _RegisterType::R_RSI : _RegisterType::R_ESI;
        auto dst_reg = is_64bit ? _RegisterType::R_RDI : _RegisterType::R_EDI;
        auto cnt_reg = is_64bit ? _RegisterType::R_RCX : _RegisterType::R_ECX;
        auto src_addr = R(src_reg);
        auto dst_addr = R(dst_reg);
        auto count = __ ZeroExtend64(R(cnt_reg));
        auto bytes =
                __ Mul(count, ir::Operand{ir::Imm(u64(ir::GetValueSizeByte(size)))});
        // REP MOVS is not atomic on x86. Order the copy as one operation
        // relative to surrounding guest memory accesses; the host helper may
        // use ordinary/vector accesses internally without a fence per element.
        if (TsoOrdered(insn)) {
            __ MemoryBarrierTSO();
        }
        __ CallHost(&RepMovs, dst_addr, src_addr, bytes);
        if (TsoOrdered(insn)) {
            __ MemoryBarrierTSO();
        }
        R(src_reg, __ Add(src_addr, ir::Operand{bytes}));
        R(dst_reg, __ Add(dst_addr, ir::Operand{bytes}));
        R(cnt_reg, __ LoadImm(ir::Imm(u64(0))));
    } else {
        // TODO: DF (direction flag) is not modelled, assume DF == 0.
        auto size = GetSize(op0.size);
        auto src_reg = is_64bit ? _RegisterType::R_RSI : _RegisterType::R_ESI;
        auto dst_reg = is_64bit ? _RegisterType::R_RDI : _RegisterType::R_EDI;
        auto src_addr = R(src_reg);
        auto dst_addr = R(dst_reg);
        auto value = MemLoad(ir::Operand{src_addr}, size, TsoOrdered(insn));
        MemStore(ir::Operand{dst_addr}, value, TsoOrdered(insn));
        auto step = ir::Imm(u64(ir::GetValueSizeByte(size)));
        R(src_reg, __ Add(src_addr, ir::Operand{step}));
        R(dst_reg, __ Add(dst_addr, ir::Operand{step}));
    }
}

void X64Decoder::DecodeStos(_DInst& insn) {
    auto& op0 = insn.ops[0];
    const auto size = GetSize(op0.size);
    auto dst_reg = is_64bit ? _RegisterType::R_RDI : _RegisterType::R_EDI;
    auto cnt_reg = is_64bit ? _RegisterType::R_RCX : _RegisterType::R_ECX;
    auto acc = [this, size] {
        switch (ir::GetValueSizeByte(size)) {
            case 1: return R(_RegisterType::R_AL);
            case 2: return R(_RegisterType::R_AX);
            case 4: return R(_RegisterType::R_EAX);
            default: return R(_RegisterType::R_RAX);
        }
    }();
    auto dst_addr = R(dst_reg);

    if ((insn.flags & FLAG_REP) || (insn.flags & FLAG_REPNZ)) {
        // TODO: DF (direction flag) and segment overrides are not modelled,
        // assume DF == 0 and default segments.
        auto count = __ ZeroExtend64(R(cnt_reg));
        // Widen the accumulator: a narrow-typed value passed straight into a
        // host call gets a spill allocation the JIT cannot produce.
        auto acc64 = __ ZeroExtend64(acc);
        ir::Value end;
        // As with REP MOVS, x86 requires ordering at the operation boundary,
        // not atomic visibility of the whole filled range.
        if (TsoOrdered(insn)) {
            __ MemoryBarrierTSO();
        }
        switch (ir::GetValueSizeByte(size)) {
            case 1: end = __ CallHost(&RepStos1, dst_addr, acc64, count); break;
            case 2: end = __ CallHost(&RepStos2, dst_addr, acc64, count); break;
            case 4: end = __ CallHost(&RepStos4, dst_addr, acc64, count); break;
            default: end = __ CallHost(&RepStos8, dst_addr, acc64, count); break;
        }
        if (TsoOrdered(insn)) {
            __ MemoryBarrierTSO();
        }
        // The helper returns the fill end address, keeping the call alive.
        R(dst_reg, end);
        R(cnt_reg, __ LoadImm(ir::Imm(u64(0))));
    } else {
        // TODO: DF (direction flag) is not modelled, assume DF == 0.
        MemStore(ir::Operand{dst_addr}, acc.SetType(size), TsoOrdered(insn));
        auto step = ir::Imm(u64(ir::GetValueSizeByte(size)));
        R(dst_reg, __ Add(dst_addr, ir::Operand{step}));
    }
}

void X64Decoder::DecodeLods(_DInst& insn) {
    // lods: accumulator = [RSI]; RSI += size. REP: only the final element
    // survives (each iteration overwrites the accumulator). DF assumed 0.
    auto& op0 = insn.ops[0];
    const auto size = GetSize(op0.size);
    const u64 step = ir::GetValueSizeByte(size);
    auto si_reg = is_64bit ? _RegisterType::R_RSI : _RegisterType::R_ESI;
    auto cnt_reg = is_64bit ? _RegisterType::R_RCX : _RegisterType::R_ECX;
    auto acc_reg = [step] {
        switch (step) {
            case 1: return _RegisterType::R_AL;
            case 2: return _RegisterType::R_AX;
            case 4: return _RegisterType::R_EAX;
            default: return _RegisterType::R_RAX;
        }
    }();
    auto si0 = R(si_reg);
    if ((insn.flags & FLAG_REP) || (insn.flags & FLAG_REPNZ)) {
        auto count = __ ZeroExtend64(R(cnt_reg));
        // The final element (index count-1) is the one left in the accumulator.
        auto do_load = __ TestNotZero(count);
        auto skip = __ NotGoto(do_load);
        auto last = __ Sub(count, ir::Operand{ir::Imm(u64(1))});
        auto last_addr = __ Add(si0, ir::Operand{__ Mul(last, ir::Operand{ir::Imm(step)})});
        R(acc_reg, MemLoad(ir::Operand{last_addr}, size, TsoOrdered(insn)));
        __ BindLabel(skip);
        R(si_reg, __ Add(si0, ir::Operand{__ Mul(count, ir::Operand{ir::Imm(step)})}));
        R(cnt_reg, __ LoadImm(ir::Imm(u64(0))));
    } else {
        R(acc_reg, MemLoad(ir::Operand{si0}, size, TsoOrdered(insn)));
        R(si_reg, __ Add(si0, ir::Operand{ir::Imm(step)}));
    }
}

void X64Decoder::DecodeCmps(_DInst& insn) {
    // cmps: flags = [RSI] - [RDI]; RSI/RDI += size. REP compares up to RCX
    // elements, stopping early per REPZ/REPNZ; flags reflect the last compare.
    auto& op0 = insn.ops[0];
    const auto size = GetSize(op0.size);
    const u64 step = ir::GetValueSizeByte(size);
    auto si_reg = is_64bit ? _RegisterType::R_RSI : _RegisterType::R_ESI;
    auto di_reg = is_64bit ? _RegisterType::R_RDI : _RegisterType::R_EDI;
    auto cnt_reg = is_64bit ? _RegisterType::R_RCX : _RegisterType::R_ECX;
    auto si0 = R(si_reg);
    auto di0 = R(di_reg);
    auto cmp_flags = [&](ir::Value a, ir::Value b) {
        ArithWithFlags(a, b, ArithOp::Sub, op0.size, ir::Flags::All);
    };
    if (!((insn.flags & FLAG_REP) || (insn.flags & FLAG_REPNZ))) {
        auto a = MemLoad(ir::Operand{si0}, size, TsoOrdered(insn));
        auto b = MemLoad(ir::Operand{di0}, size, TsoOrdered(insn));
        cmp_flags(a, b);
        R(si_reg, __ Add(si0, ir::Operand{ir::Imm(step)}));
        R(di_reg, __ Add(di0, ir::Operand{ir::Imm(step)}));
        return;
    }
    auto count = __ ZeroExtend64(R(cnt_reg));
    bool repnz = (insn.flags & FLAG_REPNZ) != 0;
    // RCX == 0 => the instruction is a no-op (pointers, RCX and flags all
    // unchanged). Branch on the pre-call `count` FIRST and keep the CallHost
    // (whose helper leaves stale host NZCV from its internal comparisons)
    // inside the branch, so the no-op path executes nothing that disturbs the
    // live flags a subsequent LAHF/pushf would read. On the active path
    // ArithWithFlags is the final flag commit. (Mirrors DecodeShift, which
    // branches on its plain count before the flag-defining work.)
    auto skip = __ NotGoto(__ TestNotZero(count));
    ir::Value iters;
    if (repnz) {
        switch (step) {
            case 1: iters = __ CallHost(&RepCmpsNZ1, si0, di0, count); break;
            case 2: iters = __ CallHost(&RepCmpsNZ2, si0, di0, count); break;
            case 4: iters = __ CallHost(&RepCmpsNZ4, si0, di0, count); break;
            default: iters = __ CallHost(&RepCmpsNZ8, si0, di0, count); break;
        }
    } else {
        switch (step) {
            case 1: iters = __ CallHost(&RepCmpsZ1, si0, di0, count); break;
            case 2: iters = __ CallHost(&RepCmpsZ2, si0, di0, count); break;
            case 4: iters = __ CallHost(&RepCmpsZ4, si0, di0, count); break;
            default: iters = __ CallHost(&RepCmpsZ8, si0, di0, count); break;
        }
    }
    iters = iters.SetType(ir::ValueType::U64);
    auto adv = __ Mul(iters, ir::Operand{ir::Imm(step)});
    R(si_reg, __ Add(si0, ir::Operand{adv}));
    R(di_reg, __ Add(di0, ir::Operand{adv}));
    R(cnt_reg, __ Sub(count, ir::Operand{iters}));
    auto last_off = __ Mul(__ Sub(iters, ir::Operand{ir::Imm(u64(1))}),
                           ir::Operand{ir::Imm(step)});
    auto a = MemLoad(ir::Operand{__ Add(si0, ir::Operand{last_off})}, size, TsoOrdered(insn));
    auto b = MemLoad(ir::Operand{__ Add(di0, ir::Operand{last_off})}, size, TsoOrdered(insn));
    cmp_flags(a, b);
    __ BindLabel(skip);
}

void X64Decoder::DecodeScas(_DInst& insn) {
    // scas: flags = accumulator - [RDI]; RDI += size. REP form behaves like cmps.
    auto& op0 = insn.ops[0];
    const auto size = GetSize(op0.size);
    const u64 step = ir::GetValueSizeByte(size);
    auto di_reg = is_64bit ? _RegisterType::R_RDI : _RegisterType::R_EDI;
    auto cnt_reg = is_64bit ? _RegisterType::R_RCX : _RegisterType::R_ECX;
    auto acc = [this, step] {
        switch (step) {
            case 1: return R(_RegisterType::R_AL);
            case 2: return R(_RegisterType::R_AX);
            case 4: return R(_RegisterType::R_EAX);
            default: return R(_RegisterType::R_RAX);
        }
    }();
    auto di0 = R(di_reg);
    if (!((insn.flags & FLAG_REP) || (insn.flags & FLAG_REPNZ))) {
        auto b = MemLoad(ir::Operand{di0}, size, TsoOrdered(insn));
        ArithWithFlags(acc, b, ArithOp::Sub, op0.size, ir::Flags::All);
        R(di_reg, __ Add(di0, ir::Operand{ir::Imm(step)}));
        return;
    }
    auto count = __ ZeroExtend64(R(cnt_reg));
    bool repnz = (insn.flags & FLAG_REPNZ) != 0;
    auto acc64 = __ ZeroExtend64(acc);
    // RCX == 0 => no-op (RDI, RCX and flags unchanged). See DecodeCmps: the
    // CallHost stays inside the branch so its stale NZCV never leaks into the
    // no-op path; ArithWithFlags is the final flag commit on the active path.
    auto skip = __ NotGoto(__ TestNotZero(count));
    ir::Value iters;
    if (repnz) {
        switch (step) {
            case 1: iters = __ CallHost(&RepScasNZ1, acc64, di0, count); break;
            case 2: iters = __ CallHost(&RepScasNZ2, acc64, di0, count); break;
            case 4: iters = __ CallHost(&RepScasNZ4, acc64, di0, count); break;
            default: iters = __ CallHost(&RepScasNZ8, acc64, di0, count); break;
        }
    } else {
        switch (step) {
            case 1: iters = __ CallHost(&RepScasZ1, acc64, di0, count); break;
            case 2: iters = __ CallHost(&RepScasZ2, acc64, di0, count); break;
            case 4: iters = __ CallHost(&RepScasZ4, acc64, di0, count); break;
            default: iters = __ CallHost(&RepScasZ8, acc64, di0, count); break;
        }
    }
    iters = iters.SetType(ir::ValueType::U64);
    auto adv = __ Mul(iters, ir::Operand{ir::Imm(step)});
    R(di_reg, __ Add(di0, ir::Operand{adv}));
    R(cnt_reg, __ Sub(count, ir::Operand{iters}));
    auto last_off = __ Mul(__ Sub(iters, ir::Operand{ir::Imm(u64(1))}),
                           ir::Operand{ir::Imm(step)});
    auto b = MemLoad(ir::Operand{__ Add(di0, ir::Operand{last_off})}, size, TsoOrdered(insn));
    ArithWithFlags(acc, b, ArithOp::Sub, op0.size, ir::Flags::All);
    __ BindLabel(skip);
}



}  // namespace swift::x86
