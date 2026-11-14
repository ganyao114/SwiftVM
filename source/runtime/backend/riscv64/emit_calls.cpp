#include "emit.h"
#include "runtime/exit.h"
#include "call_helpers.h"

#include "runtime/arch/x86/sse42_string.h"
#include "runtime/arch/x86/x87.h"

namespace swift::runtime::backend::riscv64::detail {
namespace x87 = ::swift::runtime::x86;
namespace sse42 = ::swift::runtime::x86::sse42;

void Writer::Handle(Op<OpCode::Div128>, OpContext& ctx) {
    const auto bits = TypeBits(View(ctx.Arg(0)));
    const auto sign = ctx.Arg(3).Payload();
    if ((bits != 8 && bits != 16 && bits != 32 && bits != 64) ||
        TypeBits(View(ctx.Arg(1))) != bits || TypeBits(View(ctx.Arg(2))) != bits || sign > 1)
        Reject(CompileError::UnsupportedOperand);
    const auto auxiliary = allocation.homes[ctx.node.value].auxiliary_offset;
    Read(ctx.Arg(0), a0);
    Read(ctx.Arg(1), a1);
    Read(ctx.Arg(2), a2);
    as.LI(a3, bits | (sign << 8));
    Address(a4, sp, auxiliary);
    Call(reinterpret_cast<std::uintptr_t>(&WideDivide), HostSymbol::RiscvWideDivide);
    FrameLoad(t0, auxiliary);
    Label valid;
    as.BNE(t0, x0, &valid);
    Exit(ExitValue(
            NativeExit::DivideFault));  // Explicit guest divide fault; no signal from a C++ helper.
    as.Bind(&valid);
    FrameStore(a1, auxiliary + 8);
    as.MV(ctx.destination, a0);
    Mask(ctx.destination, ctx.width);
}

void Writer::Handle(Op<OpCode::Div128Remainder>, OpContext& ctx) {
    const auto producer = ctx.Arg(0);
    if (producer.Kind() != OperandKind::Value ||
        source.Operations()[producer.Payload()].opcode != OpCode::Div128)
        Reject(CompileError::UnsupportedOperand);
    FrameLoad(ctx.destination, allocation.homes[producer.Payload()].auxiliary_offset + 8);
    Mask(ctx.destination, ctx.width);
}

void Writer::CpuQuery(OpContext& ctx) {
    const bool cpuid = ctx.operation.opcode == OpCode::Cpuid;
    const bool pair = ctx.operation.opcode == OpCode::ReadSystemPair;
    if (pair ? ctx.operation.type != Type::V128 : ctx.width != 64)
        Reject(CompileError::UnsupportedType);
    const auto kind = cpuid ? 0 : ctx.Arg(0).Payload();
    const auto target = ctx.Arg(cpuid ? 2 : 1).Payload();
    if (!target) Reject(CompileError::UnsupportedOperand);
    if (cpuid) {
        Read(ctx.Arg(0), a0);
        Read(ctx.Arg(1), a1);
    } else {
        as.LI(a0, kind);
        as.MV(a1, x0);
    }
    const bool serializing = cpuid || kind == 1;
    if (serializing) as.FENCE(FenceOrder::RW, FenceOrder::RW);
    Call(target);
    if (serializing) as.FENCE(FenceOrder::RW, FenceOrder::RW);
    if (cpuid) FrameStore(a1, allocation.homes[ctx.node.value].auxiliary_offset + 8);
    as.MV(ctx.destination, a0);
    if (pair) as.MV(ctx.high, a1);
}

void Writer::Handle(Op<OpCode::Cpuid>, OpContext& ctx) { CpuQuery(ctx); }

void Writer::Handle(Op<OpCode::ReadSystem>, OpContext& ctx) { CpuQuery(ctx); }

void Writer::Handle(Op<OpCode::ReadSystemPair>, OpContext& ctx) { CpuQuery(ctx); }

void Writer::Handle(Op<OpCode::CpuidUpper>, OpContext& ctx) {
    const auto producer = ctx.Arg(0);
    if (producer.Kind() != OperandKind::Value ||
        source.Operations()[producer.Payload()].opcode != OpCode::Cpuid)
        Reject(CompileError::UnsupportedOperand);
    FrameLoad(ctx.destination, allocation.homes[producer.Payload()].auxiliary_offset + 8);
}

void Writer::X87(OpContext& ctx) {
    const bool checked = ctx.operation.opcode == OpCode::X87Execute;
    if (ctx.operation.type != Type::U64 || TypeBits(View(ctx.Arg(0))) != 64 ||
        !IntegerType(View(ctx.Arg(2))) ||
        (checked && (TypeBits(View(ctx.Arg(3))) != 64 || options.pc_offset == UINT32_MAX)))
        Reject(CompileError::UnsupportedOperand);
    Read(ctx.Arg(0), a0, true);
    as.LI(a1, ctx.Arg(1).Payload());
    Read(ctx.Arg(2), a2, true);
    if (checked) {
        as.LI(a3, guest_pc);
        Read(ctx.Arg(3), a4, true);
        as.LI(a5, ctx.Arg(4).Payload());
        Call(reinterpret_cast<std::uintptr_t>(&CheckedX87), HostSymbol::RiscvCheckedX87);
    } else
        Call(reinterpret_cast<std::uintptr_t>(&SvmX87Execute), HostSymbol::X87Execute);
    Label success;
    as.BEQ(a0, x0, &success);
    if (options.pc_offset != UINT32_MAX) {
        as.LI(t0, guest_pc);
        Store(t0, s1, options.pc_offset, 64);
    }
    Exit(ExitValue(NativeExit::X87Fault));  // Unsupported/pending/new x87 exception; never replay
                                            // the helper.
    as.Bind(&success);
    as.MV(ctx.destination, x0);
}

void Writer::Handle(Op<OpCode::X87Op>, OpContext& ctx) { X87(ctx); }

void Writer::Handle(Op<OpCode::X87Execute>, OpContext& ctx) { X87(ctx); }

void Writer::Handle(Op<OpCode::X87CommitMetadata>, OpContext& ctx) {
    if (TypeBits(View(ctx.Arg(0))) != 64 || TypeBits(View(ctx.Arg(2))) != 64)
        Reject(CompileError::UnsupportedOperand);
    const auto metadata = ctx.Arg(3).Payload();
    Read(ctx.Arg(0), a0, true);
    as.LI(a1, ctx.Arg(1).Payload());
    Store(a1, a0, x87::X87MetadataOffset(metadata, 0), 64);
    if (x87::X87MetadataHasDP(metadata)) {
        Read(ctx.Arg(2), a1, true);
        Store(a1, a0, x87::X87MetadataOffset(metadata, 1), 64);
    }
    as.LI(a1, x87::X87MetadataOpcode(metadata));
    Store(a1, a0, x87::X87MetadataOffset(metadata, 2), 16);
}

void Writer::String(OpContext& ctx) {
    const bool explicit_lengths = ctx.operation.opcode == OpCode::Sse42StrExplicit;
    const auto control = ctx.Arg(explicit_lengths ? 3 : 2).Payload();
    if (ctx.width != 64 || control > (explicit_lengths ? 511u : 255u))
        Reject(CompileError::UnsupportedOperand);
    ReadPair(ctx.Arg(0), a0, a1);
    ReadPair(ctx.Arg(1), a2, a3);
    if (explicit_lengths) {
        ReadPair(ctx.Arg(2), a4, a5);
        as.LI(a6, control);
        Call(reinterpret_cast<std::uintptr_t>(&sse42::CompareExplicit), HostSymbol::StringExplicit);
    } else {
        as.LI(a4, control);
        Call(reinterpret_cast<std::uintptr_t>(&sse42::CompareImplicit), HostSymbol::StringImplicit);
    }
    as.MV(ctx.destination, a0);
}

void Writer::Handle(Op<OpCode::Sse42Str>, OpContext& ctx) { String(ctx); }

void Writer::Handle(Op<OpCode::Sse42StrExplicit>, OpContext& ctx) { String(ctx); }

void Writer::Handle(Op<OpCode::CallLambda>, OpContext& ctx) {
    // Lambda carries opaque effect traits followed by its real target.
    // The current scalar ABI has at most three integer arguments; it does
    // not infer a floating/vector C signature from a neutral value type.
    if (ctx.count < 2 || ctx.count > 5 || ctx.Arg(0).Kind() != OperandKind::Lambda)
        Reject(CompileError::UnsupportedOperand);
    if (ctx.operation.type != Type::Void && !IntegerType(ctx.operation.type) &&
        ctx.operation.type != Type::V128)
        Reject(CompileError::UnsupportedType);
    const auto target = ctx.Arg(1);
    if (options.relocatable && target.Kind() == OperandKind::Immediate)
        HostAddress(t5, target.Payload(), HostSymbol::None);
    else {
        // A neutral value does not carry a host callback identity. Keep JIT
        // execution available, but never persist an unrecorded host address.
        if (options.relocatable) output.relocatable = false;
        Read(target, t5, true);
    }
    for (unsigned i = 2; i < ctx.count; ++i) {
        if (!IntegerType(View(ctx.Arg(i)))) Reject(CompileError::UnsupportedOperand);
        Read(ctx.Arg(i), GPR{10 + i - 2});
    }
    CallRegister(t5);
    if (ctx.operation.type != Type::Void) {
        as.MV(ctx.destination, a0);
        if (ctx.operation.type == Type::V128)
            as.MV(ctx.high, a1);
        else
            Mask(ctx.destination, ctx.width);
    }
}

void Writer::Handle(Op<OpCode::CallLocation>, OpContext&) {
    Reject(CompileError::UnsupportedOperand);  // Params ABI is not emitted by the current frontend.
}

void Writer::Handle(Op<OpCode::CallDynamic>, OpContext&) {
    Reject(CompileError::UnsupportedOperand);
}

}  // namespace swift::runtime::backend::riscv64::detail
