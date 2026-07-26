#include <atomic>
#include <cstring>
#include "runtime/frontend/x86/decoder_internal.h"

namespace swift::x86 {

using namespace swift::runtime::frontend;

#define __ assembler->

namespace {
std::atomic<u64> g_guest_mem_bias{0};
// Bounded guest window mask; UINT64_MAX = disabled. See decoder.h.
std::atomic<u64> g_guest_addr_mask{UINT64_MAX};
// Memory ordering mode installed by the embedding translator (Config::tso_mode
// -> x86::SetTsoMode). Relaxed by default: correct for single-threaded guests.
std::atomic<u8> g_tso_mode{static_cast<u8>(runtime::TsoMode::Relaxed)};
}

void SetGuestMemBias(u64 bias) { g_guest_mem_bias.store(bias, std::memory_order_relaxed); }
u64 GetGuestMemBias() { return g_guest_mem_bias.load(std::memory_order_relaxed); }

void SetGuestAddrMask(u64 mask) {
    g_guest_addr_mask.store(mask ? mask : UINT64_MAX, std::memory_order_relaxed);
}
u64 GetGuestAddrMask() { return g_guest_addr_mask.load(std::memory_order_relaxed); }

u8* GuestHostPtr(u64 guest_addr) {
    return reinterpret_cast<u8*>((guest_addr & g_guest_addr_mask.load(std::memory_order_relaxed)) +
                                 g_guest_mem_bias.load(std::memory_order_relaxed));
}

void SetTsoMode(runtime::TsoMode mode) {
    g_tso_mode.store(static_cast<u8>(mode), std::memory_order_relaxed);
}
runtime::TsoMode GetTsoMode() {
    return static_cast<runtime::TsoMode>(g_tso_mode.load(std::memory_order_relaxed));
}

constexpr u64 kStringBackward = u64(1) << 63;
constexpr u64 kStringStepShift = 61;
constexpr u64 kStringCountMask = (u64(1) << kStringStepShift) - 1;

// Clamps a rep-string walk into the bounded guest window and returns the host
// pointer for its (truncated) start element. `count` is reduced so that every
// byte the loop touches stays inside the window — without this the loop would
// walk out of the window and dereference host memory even though the *start*
// address was truncated. With the window disabled this is a plain bias add.
//
// NOTE (unresolved, see the isolation report): clamping keeps the walk off
// host memory, but a wild in-window address still faults inside this host
// frame, which runtime.cpp's HandleFault cannot recover.
static u8* ClampGuestWalk(u64 start, u64 step, bool backward, u64& count) {
    const u64 mask = g_guest_addr_mask.load(std::memory_order_relaxed);
    const u64 bias = g_guest_mem_bias.load(std::memory_order_relaxed);
    const u64 base = start & mask;
    if (mask != UINT64_MAX && count != 0) {
        const u64 avail = mask - base + 1;  // bytes from base to the window end
        if (avail < step) {
            count = 0;
        } else if (backward) {
            const u64 room = base / step + 1;
            if (count > room) count = room;
        } else {
            const u64 room = avail / step;
            if (count > room) count = room;
        }
    }
    return reinterpret_cast<u8*>(base + bias);
}

static void RepMovs(u64 dst, u64 src, u64 packed) {
    const bool backward = (packed & kStringBackward) != 0;
    const u64 step = u64(1) << ((packed >> kStringStepShift) & 3);
    u64 dst_count = packed & kStringCountMask;
    u64 src_count = dst_count;
    auto* d = ClampGuestWalk(dst, step, backward, dst_count);
    const auto* s = ClampGuestWalk(src, step, backward, src_count);
    const u64 count = dst_count < src_count ? dst_count : src_count;
    for (u64 i = 0; i < count; ++i) {
        std::memmove(d, s, step);
        d += backward ? -static_cast<s64>(step) : static_cast<s64>(step);
        s += backward ? -static_cast<s64>(step) : static_cast<s64>(step);
    }
}

// rep stos fill helpers, one per element size (CallHost takes 3 args max).
// They return the end address: the call result feeds the RDI update, which
// keeps the host call alive in the JIT pipeline.
static u64 RepStos1(u64 dst, u64 value, u64 count) {
    const bool backward = (count & kStringBackward) != 0;
    count &= ~kStringBackward;
    const u64 arch_count = count;
    auto* p = ClampGuestWalk(dst, 1, backward, count);
    for (u64 i = 0; i < count; ++i) {
        *p = u8(value);
        p += backward ? -1 : 1;
    }
    return backward ? dst - arch_count : dst + arch_count;
}
static u64 RepStos2(u64 dst, u64 value, u64 count) {
    const bool backward = (count & kStringBackward) != 0;
    count &= ~kStringBackward;
    const u64 arch_count = count;
    auto* p = ClampGuestWalk(dst, 2, backward, count);
    for (u64 i = 0; i < count; ++i) {
        std::memcpy(p, &value, 2);
        p += backward ? -2 : 2;
    }
    return backward ? dst - arch_count * 2 : dst + arch_count * 2;
}
static u64 RepStos4(u64 dst, u64 value, u64 count) {
    const bool backward = (count & kStringBackward) != 0;
    count &= ~kStringBackward;
    const u64 arch_count = count;
    auto* p = ClampGuestWalk(dst, 4, backward, count);
    for (u64 i = 0; i < count; ++i) {
        std::memcpy(p, &value, 4);
        p += backward ? -4 : 4;
    }
    return backward ? dst - arch_count * 4 : dst + arch_count * 4;
}
static u64 RepStos8(u64 dst, u64 value, u64 count) {
    const bool backward = (count & kStringBackward) != 0;
    count &= ~kStringBackward;
    const u64 arch_count = count;
    auto* p = ClampGuestWalk(dst, 8, backward, count);
    for (u64 i = 0; i < count; ++i) {
        std::memcpy(p, &value, 8);
        p += backward ? -8 : 8;
    }
    return backward ? dst - arch_count * 8 : dst + arch_count * 8;
}

// rep cmps/scas: run the early-terminating comparison loop and return the
// number of elements actually compared (the decoder reloads the final pair to
// produce flags). REPZ (repnz=0) stops on the first not-equal element; REPNZ
// (repnz=1) stops on the first equal element. Separate Z/NZ entry points keep
// the decoder from packing a mode bit with a flag-clobbering OR.
static u64 RepCmpsN(const u8* s, const u8* d, u64 count, u64 repnz, u64 sz) {
    const bool backward = (count & kStringBackward) != 0;
    count &= ~kStringBackward;
    u64 i = 0;
    while (i < count) {
        const auto offset = static_cast<s64>(i * sz) * (backward ? -1 : 1);
        bool eq = std::memcmp(s + offset, d + offset, sz) == 0;
        ++i;
        if (repnz ? eq : !eq) {
            break;
        }
    }
    return i;
}
#define DEFINE_REP_CMPS(name, sz, repnz)                                                           \
    static u64 name(u64 rsi, u64 rdi, u64 count) {                                                 \
        const bool bwd = (count & kStringBackward) != 0;                                           \
        u64 n_s = count & ~kStringBackward, n_d = n_s;                                             \
        const auto* s_ptr = ClampGuestWalk(rsi, sz, bwd, n_s);                                     \
        const auto* d_ptr = ClampGuestWalk(rdi, sz, bwd, n_d);                                     \
        const u64 n = (n_s < n_d ? n_s : n_d) | (bwd ? kStringBackward : 0);                       \
        return RepCmpsN(s_ptr, d_ptr, n, repnz, sz);                                               \
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
    const bool backward = (count & kStringBackward) != 0;
    count &= ~kStringBackward;
    u64 i = 0;
    while (i < count) {
        const auto offset = static_cast<s64>(i * sz) * (backward ? -1 : 1);
        bool eq = std::memcmp(acc, d + offset, sz) == 0;
        ++i;
        if (repnz ? eq : !eq) {
            break;
        }
    }
    return i;
}
#define DEFINE_REP_SCAS(name, sz, repnz)                                                           \
    static u64 name(u64 acc, u64 rdi, u64 count) {                                                 \
        u8 ab[sz];                                                                                 \
        std::memcpy(ab, &acc, sz);                                                                 \
        const bool bwd = (count & kStringBackward) != 0;                                           \
        u64 n = count & ~kStringBackward;                                                          \
        const auto* d_ptr = ClampGuestWalk(rdi, sz, bwd, n);                                       \
        return RepScasN(ab, d_ptr, n | (bwd ? kStringBackward : 0), repnz, sz);                    \
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
    u64 count = 512;
    auto* p = ClampGuestWalk(guest_addr, 1, false, count);
    if (count < 512) return 0;  // would leave the guest window
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
        // Segment overrides are not modelled. A dynamic RCX count goes through
        // a host call; pack DF and the element width into its third argument.
        auto size = GetSize(op0.size);
        const u64 step = ir::GetValueSizeByte(size);
        const u64 step_log2 = step == 8 ? 3 : (step == 4 ? 2 : (step == 2 ? 1 : 0));
        auto src_reg = is_64bit ? _RegisterType::R_RSI : _RegisterType::R_ESI;
        auto dst_reg = is_64bit ? _RegisterType::R_RDI : _RegisterType::R_EDI;
        auto cnt_reg = is_64bit ? _RegisterType::R_RCX : _RegisterType::R_ECX;
        auto src_addr = R(src_reg);
        auto dst_addr = R(dst_reg);
        auto count = __ ZeroExtend64(R(cnt_reg));
        auto df = __ ZeroExtend64(DirectionValue());
        auto packed = __ Or(
                count,
                ir::Operand{__ Or(__ LslImm(df, ir::Imm(63u)),
                                  ir::Operand{ir::Imm(step_log2 << kStringStepShift)})});
        auto bytes = __ Mul(count, ir::Operand{ir::Imm(step)});
        // REP MOVS is not atomic on x86. Order the copy as one operation
        // relative to surrounding guest memory accesses; the host helper may
        // use ordinary/vector accesses internally without a fence per element.
        if (TsoOrdered(insn)) {
            __ MemoryBarrierTSO();
        }
        __ CallHost(&RepMovs, dst_addr, src_addr, packed);
        if (TsoOrdered(insn)) {
            __ MemoryBarrierTSO();
        }
        auto backward = __ TestNotZero(df);
        R(src_reg,
          __ Select(backward,
                    __ Sub(src_addr, ir::Operand{bytes}),
                    __ Add(src_addr, ir::Operand{bytes})).SetType(src_addr.Type()));
        R(dst_reg,
          __ Select(backward,
                    __ Sub(dst_addr, ir::Operand{bytes}),
                    __ Add(dst_addr, ir::Operand{bytes})).SetType(dst_addr.Type()));
        R(cnt_reg, __ LoadImm(ir::Imm(u64(0))));
    } else {
        auto size = GetSize(op0.size);
        auto src_reg = is_64bit ? _RegisterType::R_RSI : _RegisterType::R_ESI;
        auto dst_reg = is_64bit ? _RegisterType::R_RDI : _RegisterType::R_EDI;
        auto src_addr = R(src_reg);
        auto dst_addr = R(dst_reg);
        auto value = MemLoad(ir::Operand{src_addr}, size, TsoOrdered(insn));
        MemStore(ir::Operand{dst_addr}, value, TsoOrdered(insn));
        auto step = ir::Imm(u64(ir::GetValueSizeByte(size)));
        auto backward = __ TestNotZero(DirectionValue());
        R(src_reg,
          __ Select(backward,
                    __ Sub(src_addr, ir::Operand{step}),
                    __ Add(src_addr, ir::Operand{step})).SetType(src_addr.Type()));
        R(dst_reg,
          __ Select(backward,
                    __ Sub(dst_addr, ir::Operand{step}),
                    __ Add(dst_addr, ir::Operand{step})).SetType(dst_addr.Type()));
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
        auto count = __ ZeroExtend64(R(cnt_reg));
        auto packed_count =
                __ Or(count,
                      ir::Operand{__ LslImm(__ ZeroExtend64(DirectionValue()), ir::Imm(63u))});
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
            case 1: end = __ CallHost(&RepStos1, dst_addr, acc64, packed_count); break;
            case 2: end = __ CallHost(&RepStos2, dst_addr, acc64, packed_count); break;
            case 4: end = __ CallHost(&RepStos4, dst_addr, acc64, packed_count); break;
            default: end = __ CallHost(&RepStos8, dst_addr, acc64, packed_count); break;
        }
        if (TsoOrdered(insn)) {
            __ MemoryBarrierTSO();
        }
        // The helper returns the fill end address, keeping the call alive.
        R(dst_reg, end);
        R(cnt_reg, __ LoadImm(ir::Imm(u64(0))));
    } else {
        MemStore(ir::Operand{dst_addr}, acc.SetType(size), TsoOrdered(insn));
        auto step = ir::Imm(u64(ir::GetValueSizeByte(size)));
        auto backward = __ TestNotZero(DirectionValue());
        R(dst_reg,
          __ Select(backward,
                    __ Sub(dst_addr, ir::Operand{step}),
                    __ Add(dst_addr, ir::Operand{step})).SetType(dst_addr.Type()));
    }
}

void X64Decoder::DecodeLods(_DInst& insn) {
    // lods: accumulator = [RSI]; RSI changes by +/- size according to DF. REP
    // leaves the final visited element in the accumulator.
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
    auto backward = __ TestNotZero(DirectionValue());
    if ((insn.flags & FLAG_REP) || (insn.flags & FLAG_REPNZ)) {
        auto count = __ ZeroExtend64(R(cnt_reg));
        // The final element (index count-1) is the one left in the accumulator.
        auto do_load = __ TestNotZero(count);
        auto skip = __ NotGoto(do_load);
        auto last = __ Sub(count, ir::Operand{ir::Imm(u64(1))});
        auto last_off = __ Mul(last, ir::Operand{ir::Imm(step)});
        auto last_addr = __ Select(backward,
                                  __ Sub(si0, ir::Operand{last_off}),
                                  __ Add(si0, ir::Operand{last_off})).SetType(si0.Type());
        R(acc_reg, MemLoad(ir::Operand{last_addr}, size, TsoOrdered(insn)));
        __ BindLabel(skip);
        auto adv = __ Mul(count, ir::Operand{ir::Imm(step)});
        R(si_reg,
          __ Select(backward,
                    __ Sub(si0, ir::Operand{adv}),
                    __ Add(si0, ir::Operand{adv})).SetType(si0.Type()));
        R(cnt_reg, __ LoadImm(ir::Imm(u64(0))));
    } else {
        R(acc_reg, MemLoad(ir::Operand{si0}, size, TsoOrdered(insn)));
        R(si_reg,
          __ Select(backward,
                    __ Sub(si0, ir::Operand{ir::Imm(step)}),
                    __ Add(si0, ir::Operand{ir::Imm(step)})).SetType(si0.Type()));
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
    auto df = __ ZeroExtend64(DirectionValue());
    auto backward = __ TestNotZero(df);
    auto cmp_flags = [&](ir::Value a, ir::Value b) {
        ArithWithFlags(a, b, ArithOp::Sub, op0.size, ir::Flags::All);
    };
    if (!((insn.flags & FLAG_REP) || (insn.flags & FLAG_REPNZ))) {
        auto a = MemLoad(ir::Operand{si0}, size, TsoOrdered(insn));
        auto b = MemLoad(ir::Operand{di0}, size, TsoOrdered(insn));
        cmp_flags(a, b);
        R(si_reg,
          __ Select(backward,
                    __ Sub(si0, ir::Operand{ir::Imm(step)}),
                    __ Add(si0, ir::Operand{ir::Imm(step)})).SetType(si0.Type()));
        R(di_reg,
          __ Select(backward,
                    __ Sub(di0, ir::Operand{ir::Imm(step)}),
                    __ Add(di0, ir::Operand{ir::Imm(step)})).SetType(di0.Type()));
        return;
    }
    auto count = __ ZeroExtend64(R(cnt_reg));
    auto packed_count = __ Or(count, ir::Operand{__ LslImm(df, ir::Imm(63u))});
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
            case 1: iters = __ CallHost(&RepCmpsNZ1, si0, di0, packed_count); break;
            case 2: iters = __ CallHost(&RepCmpsNZ2, si0, di0, packed_count); break;
            case 4: iters = __ CallHost(&RepCmpsNZ4, si0, di0, packed_count); break;
            default: iters = __ CallHost(&RepCmpsNZ8, si0, di0, packed_count); break;
        }
    } else {
        switch (step) {
            case 1: iters = __ CallHost(&RepCmpsZ1, si0, di0, packed_count); break;
            case 2: iters = __ CallHost(&RepCmpsZ2, si0, di0, packed_count); break;
            case 4: iters = __ CallHost(&RepCmpsZ4, si0, di0, packed_count); break;
            default: iters = __ CallHost(&RepCmpsZ8, si0, di0, packed_count); break;
        }
    }
    iters = iters.SetType(ir::ValueType::U64);
    auto adv = __ Mul(iters, ir::Operand{ir::Imm(step)});
    R(si_reg,
      __ Select(backward, __ Sub(si0, ir::Operand{adv}), __ Add(si0, ir::Operand{adv}))
              .SetType(si0.Type()));
    R(di_reg,
      __ Select(backward, __ Sub(di0, ir::Operand{adv}), __ Add(di0, ir::Operand{adv}))
              .SetType(di0.Type()));
    R(cnt_reg, __ Sub(count, ir::Operand{iters}));
    auto last_off = __ Mul(__ Sub(iters, ir::Operand{ir::Imm(u64(1))}),
                           ir::Operand{ir::Imm(step)});
    auto last_si = __ Select(backward,
                            __ Sub(si0, ir::Operand{last_off}),
                            __ Add(si0, ir::Operand{last_off})).SetType(si0.Type());
    auto last_di = __ Select(backward,
                            __ Sub(di0, ir::Operand{last_off}),
                            __ Add(di0, ir::Operand{last_off})).SetType(di0.Type());
    auto a = MemLoad(ir::Operand{last_si}, size, TsoOrdered(insn));
    auto b = MemLoad(ir::Operand{last_di}, size, TsoOrdered(insn));
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
    auto df = __ ZeroExtend64(DirectionValue());
    auto backward = __ TestNotZero(df);
    if (!((insn.flags & FLAG_REP) || (insn.flags & FLAG_REPNZ))) {
        auto b = MemLoad(ir::Operand{di0}, size, TsoOrdered(insn));
        ArithWithFlags(acc, b, ArithOp::Sub, op0.size, ir::Flags::All);
        R(di_reg,
          __ Select(backward,
                    __ Sub(di0, ir::Operand{ir::Imm(step)}),
                    __ Add(di0, ir::Operand{ir::Imm(step)})).SetType(di0.Type()));
        return;
    }
    auto count = __ ZeroExtend64(R(cnt_reg));
    auto packed_count = __ Or(count, ir::Operand{__ LslImm(df, ir::Imm(63u))});
    bool repnz = (insn.flags & FLAG_REPNZ) != 0;
    auto acc64 = __ ZeroExtend64(acc);
    // RCX == 0 => no-op (RDI, RCX and flags unchanged). See DecodeCmps: the
    // CallHost stays inside the branch so its stale NZCV never leaks into the
    // no-op path; ArithWithFlags is the final flag commit on the active path.
    auto skip = __ NotGoto(__ TestNotZero(count));
    ir::Value iters;
    if (repnz) {
        switch (step) {
            case 1: iters = __ CallHost(&RepScasNZ1, acc64, di0, packed_count); break;
            case 2: iters = __ CallHost(&RepScasNZ2, acc64, di0, packed_count); break;
            case 4: iters = __ CallHost(&RepScasNZ4, acc64, di0, packed_count); break;
            default: iters = __ CallHost(&RepScasNZ8, acc64, di0, packed_count); break;
        }
    } else {
        switch (step) {
            case 1: iters = __ CallHost(&RepScasZ1, acc64, di0, packed_count); break;
            case 2: iters = __ CallHost(&RepScasZ2, acc64, di0, packed_count); break;
            case 4: iters = __ CallHost(&RepScasZ4, acc64, di0, packed_count); break;
            default: iters = __ CallHost(&RepScasZ8, acc64, di0, packed_count); break;
        }
    }
    iters = iters.SetType(ir::ValueType::U64);
    auto adv = __ Mul(iters, ir::Operand{ir::Imm(step)});
    R(di_reg,
      __ Select(backward, __ Sub(di0, ir::Operand{adv}), __ Add(di0, ir::Operand{adv}))
              .SetType(di0.Type()));
    R(cnt_reg, __ Sub(count, ir::Operand{iters}));
    auto last_off = __ Mul(__ Sub(iters, ir::Operand{ir::Imm(u64(1))}),
                           ir::Operand{ir::Imm(step)});
    auto last_di = __ Select(backward,
                            __ Sub(di0, ir::Operand{last_off}),
                            __ Add(di0, ir::Operand{last_off})).SetType(di0.Type());
    auto b = MemLoad(ir::Operand{last_di}, size, TsoOrdered(insn));
    ArithWithFlags(acc, b, ArithOp::Sub, op0.size, ir::Flags::All);
    __ BindLabel(skip);
}



}  // namespace swift::x86
