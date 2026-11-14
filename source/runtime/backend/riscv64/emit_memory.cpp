#include "emit.h"
#include "runtime/exit.h"

#include <algorithm>

namespace swift::runtime::backend::riscv64::detail {
namespace {
constexpr std::int64_t kImm12Min = -2048;
constexpr std::int64_t kImm12Max = 2047;
constexpr std::int64_t kBTypeMin = -4096;
constexpr std::int64_t kBTypeMax = 4094;
constexpr std::int64_t kJTypeMin = -1048576;
constexpr std::int64_t kJTypeMax = 1048574;
constexpr u32 kColdFlushBlockThreshold = 2400;
constexpr u32 kColdFlushMidBlockThreshold = 3500;

u32 EstimateColdStubBytes(unsigned bits, bool store) noexcept {
    const u32 bytes = bits / 8u;
    return (store ? bytes * 8u : bytes * 12u) + 20u;
}
}  // namespace

void Writer::QueueColdMemoryStub(const ColdMemoryStub& stub) {
    cold_stubs.push_back(stub);
    cold_stub_bytes += EstimateColdStubBytes(stub.bits, stub.store);
}

bool Writer::NeedColdStubFlush(bool reserve_jump) const noexcept {
    if (cold_stubs.empty()) return false;
    const u32 span = (ByteOffset() - cold_stubs.front().branch_offset) + cold_stub_bytes +
                     (reserve_jump ? 4u : 0u);
    return span >= (reserve_jump ? kColdFlushMidBlockThreshold : kColdFlushBlockThreshold);
}

void Writer::FlushColdStubs(bool jump_over) {
    if (cold_stubs.empty()) return;
    u32 skip_offset = UINT32_MAX;
    if (jump_over) {
        skip_offset = ByteOffset();
        as.NOP();
    }
    for (const auto& stub : cold_stubs) {
        const u32 stub_entry = ByteOffset();
        const auto fwd_delta = std::int64_t{stub_entry} - std::int64_t{stub.branch_offset};
        if (fwd_delta < kBTypeMin || fwd_delta > kBTypeMax) throw ShortJumpOverflow{};
        {
            auto* site = reinterpret_cast<std::uint8_t*>(output.words.data()) + stub.branch_offset;
            Assembler patch{site, 4};
            patch.BNE(t0, x0, static_cast<std::int32_t>(fwd_delta));
        }

        const unsigned access_bytes = std::min<unsigned>(stub.bits, 64u) / 8u;
        GPR base{stub.base_reg};
        const GPR low{stub.low_reg};
        const GPR high{stub.high_reg};
        if (stub.store) {
            if (low != a1) as.MV(a1, low);
            if (stub.bits == 128 && high != a2) as.MV(a2, high);
        } else {
            if (base == low || (stub.bits == 128 && base == high)) {
                as.MV(a0, base);
                base = a0;
            }
            as.MV(low, x0);
            if (stub.bits == 128) as.MV(high, x0);
        }

        for (unsigned byte = 0; byte < stub.bits / 8u; ++byte) {
            const auto disp = stub.disp + static_cast<std::int32_t>(byte);
            if (stub.store) {
                const GPR source = byte < 8 ? a1 : a2;
                if (byte % 8) as.SRLI(source, source, 8);
                const u32 first = ByteOffset();
                as.SB(source, disp, base);
                RecordGuestAccessAt(first, stub.has_fault, stub.fault_pc, 2, access_bytes);
            } else {
                const u32 first = ByteOffset();
                as.LBU(t0, disp, base);
                RecordGuestAccessAt(first, stub.has_fault, stub.fault_pc, 1, access_bytes);
                if (byte % 8) as.SLLI(t0, t0, (byte % 8) * 8);
                const GPR dst = byte < 8 ? low : high;
                as.OR(dst, dst, t0);
            }
        }
        const auto back_delta = std::int64_t{stub.resume_offset} - std::int64_t{ByteOffset()};
        if (back_delta < kJTypeMin || back_delta > kJTypeMax) Reject(CompileError::CodeTooLarge);
        as.J(static_cast<std::int32_t>(back_delta));
    }
    if (jump_over) {
        const auto skip_delta = std::int64_t{ByteOffset()} - std::int64_t{skip_offset};
        if (skip_delta < kJTypeMin || skip_delta > kJTypeMax) Reject(CompileError::CodeTooLarge);
        auto* site = reinterpret_cast<std::uint8_t*>(output.words.data()) + skip_offset;
        Assembler patch{site, 4};
        patch.J(static_cast<std::int32_t>(skip_delta));
    }
    cold_stubs.clear();
    cold_stub_bytes = 0;
}

void Writer::Uniform(OpContext& ctx) {
    const bool store = ctx.operation.opcode == OpCode::StoreUniform;
    const auto offset = ctx.Arg(0).Payload();
    const auto bits = std::max(8u, TypeBits(store ? ctx.Arg(0).TypeView() : ctx.operation.type));
    if (offset > INT32_MAX - 16) Reject(CompileError::UnsupportedOperand);
    const auto disp = static_cast<std::int32_t>(offset);
    if (store) {
        if (bits == 128) {
            const auto arg = ctx.Arg(1);
            if (arg.Kind() == OperandKind::Value &&
                TypeBits(source.Operations()[arg.Payload()].type) == 128 &&
                !allocation.homes[arg.Payload()].spilled) {
                const auto& home = allocation.homes[arg.Payload()];
                Store(GPR{home.reg}, s1, disp, 64);
                Store(GPR{home.high_reg}, s1, disp + 8, 64);
            } else {
                ReadPair(arg, a0, a1);
                Store(a0, s1, disp, 64);
                Store(a1, s1, disp + 8, 64);
            }
            return;
        }
        const GPR val = ReadWidth(ctx.Arg(1), a0, 64);
        Store(val, s1, disp, bits);
        if (offset == options.flags_offset && bits == 64 && val != s11) as.MV(s11, val);
        if (offset == options.pc_offset) {
            static_pc_known = false;
            const auto value = ctx.Arg(1);
            if (bits == 64 && value.Kind() == OperandKind::Value &&
                source.Operations()[value.Payload()].opcode == OpCode::LoadImm) {
                const auto first = source.Operands(NodeId{static_cast<u32>(value.Payload())});
                static_pc = (*first.begin()).Payload();
                const auto width = std::min(64u, TypeBits(View(value)));
                if (width < 64) static_pc &= (u64{1} << width) - 1;
                static_pc_known = true;
            }
        }
    } else {
        if (offset == options.flags_offset && bits == 64)
            as.MV(ctx.destination, s11);
        else
            Load(ctx.destination, s1, disp, std::min(bits, 64u));
        if (bits == 128) Load(ctx.high, s1, disp + 8, 64);
    }
}

void Writer::Handle(Op<OpCode::LoadUniform>, OpContext& ctx) { Uniform(ctx); }

void Writer::Handle(Op<OpCode::StoreUniform>, OpContext& ctx) { Uniform(ctx); }

void Writer::Handle(Op<OpCode::GetUniformAddress>, OpContext& ctx) {
    Address(ctx.destination, s1, ctx.Arg(0).Payload());
}

void Writer::Memory(OpContext& ctx) {
    if (NeedColdStubFlush(true)) FlushColdStubs(true);

    const bool store = ctx.operation.opcode == OpCode::StoreMemory ||
                       ctx.operation.opcode == OpCode::StoreMemoryTSO;
    const bool ordered = ctx.operation.opcode == OpCode::LoadMemoryTSO ||
                         ctx.operation.opcode == OpCode::StoreMemoryTSO;

    auto probe = ctx.operands.begin();
    const auto end = ctx.operands.end();
    auto skip_expr = [&](auto& it) {
        auto self = [&](auto& self_ref, auto& cur) -> void {
            if (cur == end) Reject(CompileError::UnsupportedOperand);
            const auto c = *cur++;
            if (c.Kind() != OperandKind::Expression) return;
            self_ref(self_ref, cur);
            if (c.Count() > 1) self_ref(self_ref, cur);
        };
        self(self, it);
    };
    skip_expr(probe);
    const auto value = store ? *probe : CellView{};
    const unsigned bits = std::max(8u, store ? TypeBits(View(value)) : ctx.width);
    if (bits > 128) Reject(CompileError::UnsupportedType);

    const bool direct_unaligned = options.ual || bits == 8;
    const std::int64_t max_disp =
            direct_unaligned ? (bits == 128 ? 2039 : kImm12Max)
                             : (2048 - static_cast<std::int64_t>(bits / 8u));
    const unsigned align_mask = std::min(8u, bits / 8u) - 1u;

    auto at = ctx.operands.begin();
    GPR base = a0;
    std::int32_t disp = 0;

    bool folded = false;
    if ((*at).Kind() != OperandKind::Expression) {
        base = ReadWidth(*at++, a0, 64);
        folded = true;
    } else if ((*at).Count() == 2 &&
               ((*at).ExpressionOp() == OperandOp::Add || (*at).ExpressionOp() == OperandOp::Sub)) {
        auto look = at;
        const auto expr_cell = *look++;
        if (look != end && (*look).Kind() != OperandKind::Expression) {
            const auto base_cell = *look++;
            if (look != end && (*look).Kind() == OperandKind::Immediate) {
                const auto imm_cell = *look++;
                const unsigned vbits = std::min(64u, TypeBits(View(imm_cell)));
                const u64 raw = vbits >= 64 ? imm_cell.Payload()
                                            : (imm_cell.Payload() & ((u64{1} << vbits) - 1));
                const auto sraw = static_cast<std::int64_t>(raw);
                const bool ok_sign = expr_cell.ExpressionOp() == OperandOp::Add || sraw != INT64_MIN;
                const std::int64_t candidate =
                        ok_sign ? (expr_cell.ExpressionOp() == OperandOp::Sub ? -sraw : sraw)
                                : INT64_MAX;
                if (candidate >= kImm12Min && candidate <= max_disp &&
                    (direct_unaligned || (static_cast<u64>(candidate) & align_mask) == 0)) {
                    base = ReadWidth(base_cell, a0, 64);
                    disp = static_cast<std::int32_t>(candidate);
                    at = look;
                    folded = true;
                }
            }
        }
    }
    if (!folded) {
        Expression(at, end, a0);
        base = a0;
    }

    GPR store_low = a1;
    GPR store_high = a2;
    if (store) {
        if (bits == 128) {
            if (value.Kind() == OperandKind::Value &&
                TypeBits(source.Operations()[value.Payload()].type) == 128 &&
                !allocation.homes[value.Payload()].spilled) {
                const auto& home = allocation.homes[value.Payload()];
                store_low = GPR{home.reg};
                store_high = GPR{home.high_reg};
            } else {
                ReadPair(value, a1, a2);
            }
        } else {
            store_low = ReadWidth(value, a1, bits);
        }
    } else if (bits == 128 && base == ctx.destination) {
        as.MV(a0, base);
        base = a0;
    }

    if (ordered && store && !options.ztso) as.FENCE(FenceOrder::RW, FenceOrder::W);

    u32 branch_offset = UINT32_MAX;
    if (!direct_unaligned) {
        as.ANDI(t0, base, align_mask);
        branch_offset = ByteOffset();
        as.NOP();
    }

    if (store) {
        const u32 first = ByteOffset();
        Store(store_low, base, disp, std::min(bits, 64u));
        RecordGuestAccess(first, 2, std::min(bits, 64u) / 8u);
        if (bits == 128) {
            const u32 second = ByteOffset();
            Store(store_high, base, disp + 8, 64);
            RecordGuestAccess(second, 2, 8);
        }
    } else {
        const u32 first = ByteOffset();
        Load(ctx.destination, base, disp, std::min(bits, 64u));
        RecordGuestAccess(first, 1, std::min(bits, 64u) / 8u);
        if (bits == 128) {
            const u32 second = ByteOffset();
            Load(ctx.high, base, disp + 8, 64);
            RecordGuestAccess(second, 1, 8);
        }
    }

    if (!direct_unaligned) {
        QueueColdMemoryStub({
                .branch_offset = branch_offset,
                .resume_offset = ByteOffset(),
                .fault_pc = guest_pc,
                .disp = disp,
                .bits = static_cast<std::uint8_t>(bits),
                .base_reg = static_cast<std::uint8_t>(base.Index()),
                .low_reg = static_cast<std::uint8_t>((store ? store_low : ctx.destination).Index()),
                .high_reg = static_cast<std::uint8_t>((store ? store_high : ctx.high).Index()),
                .store = store,
                .has_fault = has_fault_location,
        });
    }

    if (ordered && !store && !options.ztso) as.FENCE(FenceOrder::R, FenceOrder::RW);
}

#define RV_MEMORY(name)                                                                            \
    void Writer::Handle(Op<OpCode::name>, OpContext& ctx) { Memory(ctx); }
RV_MEMORY(LoadMemory)
RV_MEMORY(StoreMemory)
RV_MEMORY(LoadMemoryTSO)
RV_MEMORY(StoreMemoryTSO)
#undef RV_MEMORY

void Writer::Handle(Op<OpCode::CheckMemoryAlignment>, OpContext& ctx) {
    const GPR addr = ReadRef(ctx.Arg(0), a0);
    const u64 mask = ctx.Arg(1).Payload();
    if (mask <= 2047) {
        as.ANDI(t0, addr, static_cast<u32>(mask));
    } else {
        as.LI(t0, mask);
        as.AND(t0, addr, t0);
    }
    Label aligned;
    as.BEQ(t0, x0, &aligned);
    Exit(ExitValue(NativeExit::AlignmentFault));
    as.Bind(&aligned);
}

void Writer::Handle(Op<OpCode::MemoryBarrierTSO>, OpContext&) {
    as.FENCE(FenceOrder::RW, FenceOrder::RW);
}

void Writer::Handle(Op<OpCode::MemoryStoreFence>, OpContext&) {
    if (!options.ztso) as.FENCE(FenceOrder::W, FenceOrder::W);
}

void Writer::Handle(Op<OpCode::MemoryLoadFence>, OpContext&) {
    if (!options.ztso) as.FENCE(FenceOrder::R, FenceOrder::R);
}

void Writer::Handle(Op<OpCode::CpuYield>, OpContext&) { as.NOP(); }

void Writer::Handle(Op<OpCode::StoreUniformSelectImm>, OpContext& ctx) {
    const auto& body = source.Blocks()[block.value];
    const bool dispatch = ctx.Arg(0).Payload() == options.pc_offset &&
                          ctx.node.value + 1 == body.first_node + body.node_count &&
                          body.terminal.kind == TerminalKind::Dispatch && Linked();
    const GPR cond = ReadRef(ctx.Arg(1), a0);
    if (dispatch) {
        Label zero;
        as.BEQ(cond, x0, &zero);
        as.LI(a0, ctx.Arg(2).Payload());
        Store(a0, s1, options.pc_offset, 64);
        StaticExit(ctx.Arg(2).Payload());
        as.Bind(&zero);
        as.LI(a0, ctx.Arg(3).Payload());
        Store(a0, s1, options.pc_offset, 64);
        StaticExit(ctx.Arg(3).Payload());
        terminated = true;
        return;
    }
    Label zero, done;
    as.BEQ(cond, x0, &zero);
    as.LI(a1, ctx.Arg(2).Payload());
    as.J(&done);
    as.Bind(&zero);
    as.LI(a1, ctx.Arg(3).Payload());
    as.Bind(&done);
    Store(a1, s1, ctx.Arg(0).Payload(), 64);
    if (ctx.Arg(0).Payload() == options.pc_offset) static_pc_known = false;
    if (ctx.Arg(0).Payload() == options.flags_offset) as.MV(s11, a1);
}
}  // namespace swift::runtime::backend::riscv64::detail
