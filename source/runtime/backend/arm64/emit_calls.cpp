#include "emit.h"
#include "runtime/arch/x86/x87.h"
#include "runtime/exit.h"

#define __ masm.exact.

namespace swift::runtime::backend::arm64 {
std::uintptr_t DivisionHelper(unsigned, bool) noexcept;
std::uintptr_t CpuidHelper() noexcept;
std::uintptr_t X87Helper() noexcept;
std::uintptr_t LambdaHelper() noexcept;

namespace detail {
using x86::X87Command;
using x86::X87Encode;
using x86::X87MetadataHasDP;
using x86::X87MetadataOffset;
using x86::X87MetadataOpcode;
using x86::X87Result;
using x86::X87State;

void Writer::HandleCallLambdaFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto header = cursor.Take();
    if (header.Kind() != OperandKind::Lambda) Reject(CompileError::UnsupportedOperand);
    const auto callback = cursor.Take();
    if (callback.Kind() != OperandKind::Immediate || !callback.Payload())
        Reject(CompileError::UnsupportedOperand);
    if (ctx.operation.type != Type::Void && !IntegerType(ctx.operation.type))
        Reject(CompileError::UnsupportedType);
    Shadow(false);
    PublishPinned();
    HelperFPEnvironment(false);
    constexpr unsigned arguments[]{0, 16, 17};
    for (const auto reg : arguments) {
        if (cursor.at == cursor.end)
            Immediate(reg, 0);
        else {
            const auto argument = cursor.Take();
            if (!IntegerType(View(argument))) Reject(CompileError::UnsupportedOperand);
            Move(reg, Read(argument, reg));
        }
    }
    if (cursor.at != cursor.end) Reject(CompileError::UnsupportedOperand);
    __ sub(v::sp, v::sp, Operand(16));
    __ str(v::x1, MemOperand(v::sp));
    HostAddress(1, callback.Payload(), HostSymbol::None);
    HostAddress(30, LambdaHelper(), HostSymbol::ArmLambdaBridge);
    __ blr(v::x30);
    __ ldr(v::x1, MemOperand(v::sp));
    __ add(v::sp, v::sp, Operand(16));
    HelperFPEnvironment(true);
    ReloadPinned();
    Shadow(true);
    NativePhase(SVM_NATIVE_PHASE_GUEST);
    nzcv_matches = 0;
    if (ctx.operation.type != Type::Void) {
        Move(ctx.destination, 0);
        if (ctx.width < 64) Extract(ctx.destination, ctx.destination, 0, ctx.width);
        SaveResult(ctx);
    }
}

void Writer::HandleReadSystemFamily(OpContext& ctx) {
    const auto kind = IntegerImmediate(ctx.cursor.Take()),
               callback = IntegerImmediate(ctx.cursor.Take());
    if (!callback) Reject(CompileError::UnsupportedOperand);
    Shadow(false);
    PublishPinned();
    HelperFPEnvironment(false);
    if (kind == 1) __ dmb(v::InnerShareable, v::BarrierAll);
    Immediate(0, kind);
    Immediate(16, 0);
    HostAddress(17, callback, HostSymbol::None);
    HostAddress(30, CpuidHelper(), HostSymbol::ArmPairBridge);
    __ blr(v::x30);
    HelperFPEnvironment(true);
    if (kind == 1) __ isb();
    if (ctx.operation.opcode == OpCode::ReadSystemPair) {
        if (ctx.operation.type != Type::V128) Reject(CompileError::UnsupportedType);
        __ fmov(D(ctx.destination), v::x0);
        __ ins(Lanes(ctx.destination, 64), 1, v::x16);
    } else
        Move(ctx.destination, 0);
    nzcv_matches = 0;
    RestoreHelperNZCV();
    NativePhase(SVM_NATIVE_PHASE_GUEST);
}

void Writer::HandleDiv128Family(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto& home = ctx.home;
    const auto destination = ctx.destination;
    const auto high = cursor.Take(), low = cursor.Take(), divisor = cursor.Take();
    const auto sign = IntegerImmediate(cursor.Take());
    const auto bits = Width(View(high));
    if ((bits != 8 && bits != 16 && bits != 32 && bits != 64) || Width(View(low)) != bits ||
        Width(View(divisor)) != bits || sign > 1)
        Reject(CompileError::UnsupportedOperand);
    Shadow(false);
    PublishPinned();
    HelperFPEnvironment(false);
    Move(0, Read(high, 0));
    Move(16, Read(low, 16));
    Move(17, Read(divisor, 17));
    const auto symbol = static_cast<HostSymbol>(
            static_cast<unsigned>(sign ? HostSymbol::ArmDivideS8 : HostSymbol::ArmDivideU8) +
            std::countr_zero(bits) - 3);
    HostAddress(30, DivisionHelper(bits, sign != 0), symbol);
    __ blr(v::x30);  // BLR x30.
    // The private helper returns a real status in x17; no host signal
    // crosses its C++ frame. Test before restoring FP controls clobbers x17.
    const auto valid = masm.Here();
    __ cbnz(v::x17, std::int64_t{0});
    HelperFPEnvironment(true);
    if (has_fault_location && options.pc_offset != UINT32_MAX) {
        Immediate(0, fault_location);
        PCMemory(false);
    }
    Immediate(0, ExitValue(NativeExit::DivideFault));
    Epilogue();
    masm.Patch(valid, masm.Here());
    HelperFPEnvironment(true);
    nzcv_matches = 0;
    Memory(false, 64, 16, 31, home.extra_slot - 1);
    Move(destination, 0);
    RestoreHelperNZCV();
    NativePhase(SVM_NATIVE_PHASE_GUEST);
}

void Writer::HandleDiv128RemainderFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto owner = cursor.Take();
    if (owner.Kind() != OperandKind::Value ||
        source.Operations()[owner.Payload()].opcode != OpCode::Div128)
        Reject(CompileError::UnsupportedOperand);
    Memory(true, 64, destination, 31, allocation.homes[owner.Payload()].extra_slot - 1);
}

void Writer::HandleCpuidFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto& home = ctx.home;
    const auto destination = ctx.destination;
    const auto leaf = cursor.Take(), subleaf = cursor.Take();
    const auto callback = IntegerImmediate(cursor.Take());
    if (!callback) Reject(CompileError::UnsupportedOperand);
    Shadow(false);
    PublishPinned();
    __ dmb(v::InnerShareable,
           v::BarrierAll);  // DMB ISH: prior guest memory precedes the serializing query.
    HelperFPEnvironment(false);
    Move(0, Read(leaf, 0));
    Move(16, Read(subleaf, 16));
    HostAddress(17, callback, HostSymbol::None);
    HostAddress(30, CpuidHelper(), HostSymbol::ArmPairBridge);
    __ blr(v::x30);  // BLR x30.
    HelperFPEnvironment(true);
    __ isb();  // ISB: following instructions start after the query.
    nzcv_matches = 0;
    Memory(false, 64, 16, 31, home.extra_slot - 1);
    Move(destination, 0);
    RestoreHelperNZCV();
    NativePhase(SVM_NATIVE_PHASE_GUEST);
}

void Writer::HandleCpuidUpperFamily(OpContext& ctx) {
    auto& cursor = ctx.cursor;
    const auto destination = ctx.destination;
    const auto owner = cursor.Take();
    if (owner.Kind() != OperandKind::Value ||
        source.Operations()[owner.Payload()].opcode != OpCode::Cpuid)
        Reject(CompileError::UnsupportedOperand);
    Memory(true, 64, destination, 31, allocation.homes[owner.Payload()].extra_slot - 1);
}

void Writer::WriteX87Metadata(CellView state, CellView dp, u64 metadata, u64 pc, bool known_pc) {
    // An ordinary Uniform address is already an exact location in x19's
    // state buffer. Use it directly; do not rebuild three pointer expressions
    // or three separate IR stores for the same architectural commit.
    u64 uniform_base = UINT64_MAX;
    if (state.Kind() == OperandKind::Value &&
        source.Operations()[state.Payload()].opcode == OpCode::GetUniformAddress)
        uniform_base = IntegerImmediate(
                source.Operands(NodeId{static_cast<u32>(state.Payload())}).front());
    if (known_pc)
        Immediate(16, pc);
    else {
        PCMemory(true);
        Move(16, 0);
    }
    if (uniform_base == UINT64_MAX) Move(0, Read(state, 0));
    const auto store = [&](unsigned field, unsigned bits) {
        const auto relative = X87MetadataOffset(metadata, field);
        if (uniform_base != UINT64_MAX) {
            const auto offset = static_cast<std::int64_t>(uniform_base) + relative;
            const auto bytes = bits / 8;
            if (offset >= 0 && offset % bytes == 0 && static_cast<u64>(offset) / bytes <= 4095)
                Memory(false, bits, 16, 19, static_cast<u32>(offset / bytes));
            else {
                Immediate(17, static_cast<u64>(offset));
                __ add(v::x17, v::x19, v::Operand(v::x17));
                Memory(false, bits, 16, 17);
            }
        } else {
            Immediate(17, static_cast<u64>(static_cast<std::int64_t>(relative)));
            __ add(v::x17, v::x0, v::Operand(v::x17));
            Memory(false, bits, 16, 17);
        }
    };
    store(0, 64);
    if (X87MetadataHasDP(metadata)) {
        Move(16, Read(dp, 16));
        store(1, 64);
    }
    Immediate(16, X87MetadataOpcode(metadata));
    store(2, 16);
}

void Writer::HandleX87CommitMetadataFamily(OpContext& ctx) {
    const auto state = ctx.cursor.Take();
    const auto pc = IntegerImmediate(ctx.cursor.Take());
    const auto dp = ctx.cursor.Take();
    const auto metadata = IntegerImmediate(ctx.cursor.Take());
    if (Width(View(state)) != 64 || Width(View(dp)) != 64) Reject(CompileError::UnsupportedOperand);
    WriteX87Metadata(state, dp, metadata, pc, true);
}

void Writer::HandleX87OpFamily(OpContext& ctx) {
    const auto state = ctx.cursor.Take();
    const auto command = IntegerImmediate(ctx.cursor.Take());
    const auto raw = ctx.cursor.Take();
    const bool checked = ctx.operation.opcode == OpCode::X87Execute;
    CellView dp{};
    u64 metadata{};
    if (checked) {
        dp = ctx.cursor.Take();
        metadata = IntegerImmediate(ctx.cursor.Take());
    }
    if (ctx.operation.type != Type::U64 || Width(View(state)) != 64 || !IntegerType(View(raw)) ||
        Width(View(raw)) > 64 ||
        (checked && (Width(View(dp)) != 64 || options.pc_offset == UINT32_MAX)))
        Reject(CompileError::UnsupportedOperand);
    const bool result_used = ctx.home.last_use != ctx.home.definition_position;
    if (command == X87Encode(X87Command::Init) && state.Kind() == OperandKind::Value &&
        source.Operations()[state.Payload()].opcode == OpCode::GetUniformAddress) {
        const auto address = Read(state, 17);
        Immediate(16, UINT64_C(0x0000ffff0000037f));
        Memory(false, 64, 16, address, offsetof(X87State, control) / 8);
        Memory(false, 64, 31, address, offsetof(X87State, result) / 8);
        if (result_used)
            Move(ctx.destination, 31);
        else
            ctx.saved = true;
        return;
    }
    Shadow(false);
#if !SVM_X87_GPR_ONLY
    PublishPinned();
    HelperFPEnvironment(false);
#endif
    NativePhase(SVM_NATIVE_PHASE_HELPER);
    Move(0, Read(state, 0));
    Immediate(16, command);
    Move(17, Read(raw, 17));
    HostAddress(30, X87Helper(), HostSymbol::ArmX87Bridge);
    __ blr(v::x30);
#if !SVM_X87_GPR_ONLY
    HelperFPEnvironment(true);
    nzcv_matches = 0;
    RestoreHelperNZCV();
#endif
    NativePhase(SVM_NATIVE_PHASE_GUEST);
    const auto success = masm.Here();
    __ cbz(v::x0, std::int64_t{0});
    if (checked) {
        // Older pending exceptions retain their original x87 pointers.
        Immediate(16, static_cast<u64>(X87Result::RaisedException));
        __ eor(v::x16, v::x16, v::x0);
        const auto pending = masm.Here();
        __ cbnz(v::x16, std::int64_t{0});
        WriteX87Metadata(state, dp, metadata, fault_location, has_fault_location);
        masm.Patch(pending, masm.Here());
    }
    if (has_fault_location && options.pc_offset != UINT32_MAX) {
        Immediate(0, fault_location);
        PCMemory(false);
    }
    Immediate(0, ExitValue(NativeExit::X87Fault));
    Epilogue();
    masm.Patch(success, masm.Here());
    if (checked && x86::X87MetadataCommitsSuccess(metadata))
        WriteX87Metadata(state, dp, metadata, fault_location, has_fault_location);
    if (result_used)
        Immediate(ctx.destination, 0);  // Successful helper results are status zero.
    else
        ctx.saved = true;
}
}  // namespace detail
}  // namespace swift::runtime::backend::arm64

#undef __
