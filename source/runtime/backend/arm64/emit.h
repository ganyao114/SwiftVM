#pragma once

// Private declarations for one machine-code writer. Program, homes and
// output remain borrowed from Compile; no intermediate program is created.
#include "compile.h"
#include "abi.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>

namespace swift::runtime::backend::arm64::detail {
using namespace ir;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
struct Rejected { CompileError error; NodeId node; BlockId block; };

// A logical immediate is one rotated run of ones in a repeated power-of-two
// element. Return its actual N:immr:imms instruction fields, not an IR fact.
inline bool LogicalImmediate(u64 value, unsigned width, u32& fields) {
    const auto mask = width == 64 ? ~u64{} : UINT64_C(0xffffffff);
    value &= mask;
    if (!value || value == mask) return false;
    auto size = width;
    while (size > 2) {
        const auto half = size / 2;
        const auto part = (u64{1} << half) - 1;
        if ((value & part) != ((value >> half) & part)) break;
        size = half;
    }
    const auto part = ~u64{} >> (64 - size);
    const auto pattern = value & part;
    const auto ones = static_cast<unsigned>(std::popcount(pattern));
    const auto first = static_cast<unsigned>(std::countr_zero(pattern));
    unsigned rotation;
    if ((pattern >> first) == (u64{1} << ones) - 1) rotation = (size - first) & (size - 1);
    else {
        const auto low = static_cast<unsigned>(std::countr_one(pattern));
        const auto high = static_cast<unsigned>(std::countl_one(pattern << (64 - size)));
        if (low + high != ones) return false;
        rotation = high;
    }
    const auto imms = ((~(size - 1u) << 1) | (ones - 1u)) & 63u;
    fields = (static_cast<u32>(size == 64) << 22) | (rotation << 16) | (imms << 10);
    return true;
}

class Writer {
public:
    Writer(const Program& source, const Allocation& allocation, CompileResult& output, CompileOptions options,
           bool long_forward_branches = false)
            : source(source), allocation(allocation), output(output), options(options),
              long_forward_branches(long_forward_branches), labels(source.Blocks().size(), UINT32_MAX) {}

    // False requests one fresh long-forward emission before publication.
    // Ordinary errors, including B26 overflow, retain their typed rejection.
    bool Run();

private:
    struct Cursor {
        CellRange::Iterator at, end;
        Writer& writer;
        CellView Take() {
            if (at == end) writer.Reject(CompileError::UnsupportedOperand);
            return *at++;
        }
        void Done() { if (at != end) writer.Reject(CompileError::UnsupportedOperand); }
    };
    struct Fixup { u32 word, target; };
    const Program& source;
    const Allocation& allocation;
    CompileResult& output;
    CompileOptions options;
    bool long_forward_branches{};
    std::vector<u32> labels;
    std::vector<Fixup> fixups;
    NodeId node{};
    BlockId block{};
    NodeId pending_save{}; // At most the adjacent legacy publication, never a node table.
    bool shadow_dirty{};
    bool packed_current{}; // Native mode: x1 has current NZCV and, when raw, AF.
    u64 nzcv_matches{}; // Host NZCV bits currently equal the authoritative packed x1.
    unsigned deferred_parity{}; // Zero means x1; otherwise this actual pin supplies its low byte.
    bool direct_carry{}; // Hardware C equals guest C until its deferred inversion.
    std::uint8_t fault_parity{}, fault_carry{}; // Captured at the actual scalar memory instruction.
    unsigned terminal_condition{18}; // 0..15 neutral cond; 16/17 AF; 20/21 native C|Z; 18 materialized.
    u64 fault_location{};
    bool has_fault_location{};
    u32 unaligned_word{UINT32_MAX}; // Actual current-op recoverable fast instruction.
    NodeId elided_consumer{}; // Only the adjacent numeric consumer already emitted with its producer.
    u64 static_pc{};
    bool static_pc_valid{};
    u64 alternate_pc{};
    bool alternate_pc_valid{};
    bool dispatch_emitted{}; // The actual final PC store emitted its dispatch.
    u32 dispatcher_literal{UINT32_MAX}; // Immutable DATA in this unit, never an outer link target.
    // One streaming recurrence in this Writer. Prefix operations are emitted
    // normally; these two actual sites acquire recipes only at the tail TEST.
    struct DeferredTest {
        NodeId snapshot{}, loaded{}, value{};
        u32 snapshot_offset{}, capture_offset{};
        std::array<std::size_t, 2> sites{};
        std::uint8_t site_count{}, bits{}, snapshot_register{}, value_register{};
        bool active{}, captured{};
    } deferred_test;
    bool deferred_test_used{};

    static constexpr u64 carry = FlagBits(Flag::Carry), overflow = FlagBits(Flag::Overflow),
            zero = FlagBits(Flag::Zero), negative = FlagBits(Flag::Negate),
            parity = FlagBits(Flag::Parity), auxiliary = FlagBits(Flag::AuxiliaryCarry);
    static constexpr u64 all_flags = FlagBits(Flag::All);
    static constexpr u64 nzcv_flags = FlagBits(Flag::NZCV);
    bool Enabled() const { return options.flags_offset != UINT32_MAX; }
    bool NativeNZCV() const { return options.native_nzcv; }
    bool Linked() const { return options.dispatcher || options.exit_handler; }
    bool RawAF() const { return NativeNZCV() && Linked(); }
    bool Pinned() const { return options.pinned_uniform_offset != UINT32_MAX; }
    bool VectorPinned() const { return options.pinned_vector_offset != UINT32_MAX; }
    unsigned VectorTemporary(bool right = false) const { return (VectorPinned() ? 8u : 30u) + right; }
    bool CanDeferFlags() const {
        if (!options.local_flags || !RawAF() || !options.private_uniforms || !options.fault_locations || !node)
            return false;
        const auto& operation = source.Operations()[node.value];
        const auto metadata = operation.Metadata();
        return (operation.opcode == OpCode::Add || operation.opcode == OpCode::Sub || operation.opcode == OpCode::Neg) &&
                (operation.type == Type::U32 || operation.type == Type::U64) && metadata.flag_outputs &&
                (metadata.profile == FlagProfile::ArithmeticRaw || metadata.profile == FlagProfile::ArithmeticCanonicalCarry);
    }
    void CompleteParity() {
        if (deferred_parity) {
            Move(1, deferred_parity); deferred_parity = 0; packed_current = false;
        }
    }
    void CompleteCarry() {
        if (direct_carry) {
            Put(0xd500401fu); direct_carry = false; packed_current = false;
        }
    }
    void CompleteLocalFlags() { CompleteParity(); CompleteCarry(); }
    bool RcpcImm9() const {
        return options.rcpc_imm9 && options.fault_locations && options.recover_unaligned && has_fault_location;
    }
    bool TestField(CellView field) const {
        const auto offset = field.Payload();
        return field.Kind() == OperandKind::Uniform && offset >= options.pinned_uniform_offset &&
                offset - options.pinned_uniform_offset < options.pinned_uniform_slots * 8u &&
                !((offset - options.pinned_uniform_offset) & 7u);
    }
    bool DeferTest(const Operation& operation, Cursor& cursor);
    void CompleteDeferredTest();
    void RequireFlags() const { if (!Enabled()) Reject(CompileError::UnsupportedFlags); }

    [[noreturn]] void Reject(CompileError error) const { throw Rejected{error, node, block}; }
    void Put(u32 word) {
        if (output.words.size() >= std::numeric_limits<u32>::max()) Reject(CompileError::CodeTooLarge);
        output.words.push_back(word);
    }
    static unsigned Bits(Type type) {
        return IntegerType(type) ? TypeBits(type) : 0;
    }
    unsigned Width(Type type) const {
        const auto bits = Bits(type);
        if (!bits || bits > 64) Reject(CompileError::UnsupportedType);
        return bits;
    }
    void Move(unsigned to, unsigned from) {
        if (to != from) Put(0xaa0003e0u | (from << 16) | to);
    }
    void Immediate(unsigned reg, u64 bits) {
        const auto parts = bits <= UINT32_MAX ? 2u : 4u;
        unsigned zero{}, full{};
        for (unsigned part = 0; part < parts; ++part) {
            const auto word = static_cast<unsigned>((bits >> (part * 16)) & 0xffff);
            zero += word == 0; full += word == 0xffff;
        }
        const bool invert = full > zero;
        const auto seed = invert ? ~bits : bits;
        const auto first = std::min(parts - 1, static_cast<unsigned>(std::countr_zero(seed)) / 16);
        const auto wide = parts == 4 ? 1u << 31 : 0u;
        Put((invert ? 0x12800000u : 0x52800000u) | wide | (first << 21) |
                (static_cast<u32>(seed >> (first * 16)) & 0xffffu) << 5 | reg);
        for (unsigned part = 0; part < parts; ++part) {
            const auto word = static_cast<u32>((bits >> (part * 16)) & 0xffff);
            if (part != first && word != (invert ? 0xffffu : 0u))
                Put(0x72800000u | wide | (part << 21) | (word << 5) | reg);
        }
    }
    void Narrow(unsigned to, unsigned from, unsigned bits, bool sign = false) {
        if (bits == 64) { Move(to, from); return; }
        if (bits == 32 && !sign) { Put(0x2a0003e0u | (from << 16) | to); return; }
        Put((sign ? 0x93400000u : 0xd3400000u) | ((bits - 1) << 10) | (from << 5) | to);
    }
    void Extract(unsigned to, unsigned from, unsigned first, unsigned count) {
        Put(0xd3400000u | (first << 16) | ((first + count - 1) << 10) | (from << 5) | to);
    }
    void Insert(unsigned to, unsigned from, unsigned first, unsigned count, unsigned native_width = 64) {
        Put((native_width == 32 ? 0x33000000u : 0xb3400000u) |
                (((native_width - first) & (native_width - 1)) << 16) |
                ((count - 1) << 10) | (from << 5) | to);
    }
    void ShiftLeft(unsigned to, unsigned from, unsigned amount) {
        if (!amount) { Move(to, from); return; }
        Put(0xd3400000u | ((64 - amount) << 16) | ((63 - amount) << 10) | (from << 5) | to);
    }
    void StackAdjust(bool subtract);
    void Memory(bool load, unsigned bits, unsigned value, unsigned address, u32 scaled = 0) {
        const auto opcode = bits == 64 ? (load ? 0xf9400000u : 0xf9000000u)
                : bits == 32 ? (load ? 0xb9400000u : 0xb9000000u)
                : bits == 16 ? (load ? 0x79400000u : 0x79000000u)
                : bits == 8 ? (load ? 0x39400000u : 0x39000000u) : 0;
        if (!opcode || scaled > 4095) Reject(CompileError::UnsupportedType);
        Put(opcode | (scaled << 10) | (address << 5) | value);
    }
    void PCMemory(bool load);
    void VectorMemory(bool load, unsigned value, unsigned address, u32 scaled = 0) {
        Put((load ? 0x3dc00000u : 0x3d800000u) | (scaled << 10) | (address << 5) | value);
    }
    void OrderedMemory(bool load, unsigned bits, unsigned value, unsigned address, std::int32_t offset = 0);
    void VectorMove(unsigned to, unsigned from) {
        if (to != from) Put(0x4ea01c00u | (from << 16) | (from << 5) | to);
    }
    void LoadMxcsrFlushControl();
    void FlushScalarDenormal(unsigned value, unsigned control_bit, unsigned lane = 0);
    void TruncateDoubleToInteger(unsigned destination, unsigned raw, unsigned bits);
    unsigned VectorValue(CellView cell, unsigned scratch);
    void VectorUniform(bool load, CellView uniform, unsigned value);
    int VectorPinnedSlot(u32 offset, unsigned bytes) const;
    void VectorAlias(bool reload, u32 offset, unsigned bytes);
    unsigned RawValue(u32 id, unsigned scratch, bool physical = false) {
        if (id >= allocation.homes.size() || source.Operations()[id].type == Type::Void)
            Reject(CompileError::InvalidProgram);
        const auto& home = allocation.homes[id];
        unsigned reg = home.reg;
        if (home.spilled) { Memory(true, 64, scratch, 31, home.stack_offset / 8); reg = scratch; }
        if (home.narrow_pin && !physical) { Narrow(scratch, reg, home.CarrierBits()); return scratch; }
        return reg;
    }
    Type View(CellView cell) const {
        if (cell.Kind() == OperandKind::Value) {
            if (cell.Payload() >= source.Operations().size()) Reject(CompileError::InvalidProgram);
            return cell.TypeView() == Type::Void ? source.Operations()[cell.Payload()].type : cell.TypeView();
        }
        if (cell.Kind() == OperandKind::Immediate) return cell.TypeView();
        Reject(CompileError::UnsupportedOperand);
    }
    // limit/sign describe the logical read. native_width only says that the
    // consuming instruction itself ignores all bits above its W input.
    unsigned Read(CellView cell, unsigned scratch, unsigned limit = 64, bool sign = false,
                  unsigned native_width = 64) {
        auto bits = std::min(Width(View(cell)), limit);
        unsigned reg = scratch;
        if (cell.Kind() == OperandKind::Value) {
            const auto id = static_cast<u32>(cell.Payload());
            reg = RawValue(id, scratch, true);
            const auto& producer = source.Operations()[id];
            const auto& home = allocation.homes[id];
            const auto narrow_pin = home.narrow_pin;
            const auto carrier_bits = home.CarrierBits();
            if (bits > carrier_bits) { bits = carrier_bits; sign = false; }
            if (native_width == 32 && bits >= 32) return reg;
            // Numeric producers already normalize to their natural width.
            // Raw transport is the exception; its narrow label is not a mask.
            if (!narrow_pin && !sign && Bits(producer.type) <= bits && producer.opcode != OpCode::BitCast &&
                producer.opcode != OpCode::GetResult && producer.opcode != OpCode::GetFlags)
                return reg;
        } else {
            auto value = cell.Payload();
            if (bits < 64) {
                const auto mask = (u64{1} << bits) - 1;
                value &= mask;
                if (sign && (value & (u64{1} << (bits - 1)))) value |= ~mask;
            }
            Immediate(scratch, value);
            return scratch;
        }
        if (bits == 64) return reg;
        Narrow(scratch, reg, bits, sign); return scratch;
    }
    unsigned RawScalar(CellView cell, unsigned scratch, unsigned physical_bits = 0) {
        if (cell.Kind() != OperandKind::Value) Reject(CompileError::UnsupportedOperand);
        Width(View(cell)); // The integer view is valid but does not cut a raw container.
        const auto id = static_cast<u32>(cell.Payload());
        const auto value = RawValue(id, scratch, physical_bits != 0);
        // A consumer may cut its own low bits only when the real carrier
        // supplies all of them. Otherwise retain the borrowed value's zeros.
        if (physical_bits && allocation.homes[id].CarrierBits() < physical_bits) {
            Narrow(scratch, value, allocation.homes[id].CarrierBits());
            return scratch;
        }
        return value;
    }
    u64 IntegerImmediate(CellView cell) const {
        if (cell.Kind() != OperandKind::Immediate) Reject(CompileError::UnsupportedOperand);
        Width(cell.TypeView());
        return cell.Payload();
    }
    void Field(u64 first, u64 count) const {
        if (first >= 64 || count > 64 - first) Reject(CompileError::UnsupportedOperand);
    }
    unsigned Expression(Cursor& cursor, unsigned first, unsigned second, unsigned native_width = 64);
    unsigned OrderedAddress(Cursor& cursor, std::int32_t& offset, bool store = false);
    bool PartialByteStore(const Operation& operation, const Home& home) const;
    void Uniform(bool load, CellView uniform, unsigned value);
    int PinnedSlot(u32 offset, unsigned size) const {
        const auto bytes = options.pinned_uniform_slots * 8u;
        if (!Pinned() || u64{offset} >= u64{options.pinned_uniform_offset} + bytes ||
            options.pinned_uniform_offset >= u64{offset} + size) return -1;
        if (offset < options.pinned_uniform_offset || u64{offset} + size > u64{options.pinned_uniform_offset} + bytes ||
            ((offset - options.pinned_uniform_offset) & 7u) + size > 8) Reject(CompileError::UnsupportedOperand);
        return static_cast<int>(abi::PinnedUniformRegister((offset - options.pinned_uniform_offset) / 8,
                                                         options.pinned_uniform_slots));
    }
    void PublishPinned();
    bool ConstantPC(CellView value, u64& pc) const;
    void ObservePCStore(CellView uniform, CellView value);
    void FlagsMemory(bool load, unsigned value);
    void Shadow(bool load);
    void HelperFPEnvironment(bool guest);
    bool OverlapsShadow(CellView uniform) const;
    u64 Flags(CellView cell, bool validate = true) const {
        if (cell.Kind() != OperandKind::Flags || (validate && (cell.Payload() & ~all_flags)))
            Reject(CompileError::UnsupportedFlags);
        return cell.Payload();
    }
    unsigned Condition(CellView cell) const {
        if (cell.Kind() != OperandKind::Condition || cell.Payload() >= 16) Reject(CompileError::UnsupportedOperand);
        return static_cast<unsigned>(cell.Payload());
    }
    static u32 HostMask(u64 mask) {
        return (mask & carry ? 1u << 29 : 0) | (mask & overflow ? 1u << 28 : 0) |
                (mask & zero ? 1u << 30 : 0) | (mask & negative ? 1u << 31 : 0);
    }
    void PackNZCV();
    void UnpackRawFlags();
    void RestoreNZCV();
    void RestoreHelperNZCV();
    void SnapshotCompareNZCV() {
        if (RawAF()) Put(0xd53b421eu); // MRS x30,NZCV before reading scratch operands.
        else PackNZCV();
    }
    void RestoreCompareNZCV() {
        if (RawAF()) { Put(0xd51b421eu); packed_current = false; }
        else RestoreNZCV();
    }
    void Rmif(unsigned value, unsigned rotation, u32 mask) {
        Put(0xba000400u | (rotation << 15) | (value << 5) | mask);
    }
    void PrepareNZCV(u64 mask) {
        const auto clobber = RawAF() ? allocation.homes[node.value].nzcv_clobber : 0u;
        const auto preserved = nzcv_flags & ~mask & ~clobber;
        if (NativeNZCV() && (mask & nzcv_flags) && preserved) {
            if (preserved & carry) CompleteCarry();
            Put(0xd53b421eu); // MRS x30,NZCV: preserve the unwritten fields.
        }
    }
    void FinishNZCV(u64 mask, bool invert_carry) {
        if (!(mask & nzcv_flags)) return;
        const auto clobber = RawAF() ? allocation.homes[node.value].nzcv_clobber : 0u;
        if (mask & carry) {
            direct_carry = invert_carry && CanDeferFlags();
            if (invert_carry && !direct_carry) Put(0xd500401fu); // CFINV.
        } else if (clobber & carry) direct_carry = false;
        if (const auto preserved = HostMask(nzcv_flags & ~mask & ~clobber) >> 28) Rmif(30, 28, preserved);
        packed_current = false;
    }
    void EnsureNZCV(unsigned condition);
    void ClearMask(u64 mask);
    void MergeNZCV(u64 mask, bool flip_carry = false);
    void Parity(unsigned to, unsigned value);
    void Test(u64 mask, unsigned destination, bool inverse);
    void Predicate(unsigned destination, unsigned value, bool zero) {
        if (NativeNZCV()) {
            Put(0xdac01000u | (value << 5) | destination); // CLZ Xd,Xn, including 64 for zero.
            Extract(destination, destination, 6, 1);
            if (!zero) Put(0xd2400000u | (destination << 5) | destination); // EOR Xd,Xd,#1.
            return;
        }
        nzcv_matches = 0;
        Put(0xf100001fu | (value << 5)); // CMP Xn,#0
        Put((zero ? 0x9a9f17e0u : 0x9a9f07e0u) | destination); // CSET EQ/NE
        RestoreNZCV();
    }
    static bool Logical(OpCode op) {
        return op == OpCode::And || op == OpCode::Or || op == OpCode::Xor || op == OpCode::AndNot;
    }
    static bool Arithmetic(OpCode op) {
        return op == OpCode::Add || op == OpCode::Sub || op == OpCode::Neg || op == OpCode::Adc || op == OpCode::Sbb;
    }
    void Metadata(const Operation& op) const;
    // The only legacy domain accepted here is a fixed adjacent pair (or one
    // disjoint ClearFlags in between). No producer walk or demand table.
    u64 LegacyMask(NodeId owner, NodeId save) const;
    u64 CaptureLegacy(const Operation& op);
    void WriteParity(unsigned result, bool preserve_af);
    void ArithmeticFlags(unsigned result, u64 mask, bool canonical);
    void LogicalFlags(unsigned result, unsigned width, u64 mask, bool clear_af, bool nzcv_ready = false,
                      bool invert_carry = true);
    unsigned LaneSize(u64 bits, bool allow64 = true) const {
        if (bits == 8) return 0;
        if (bits == 16) return 1;
        if (bits == 32) return 2;
        if (bits == 64 && allow64) return 3;
        Reject(CompileError::UnsupportedOperand);
    }
    bool EmitVector(const Operation& operation, Cursor& cursor, unsigned destination);
    void Emit(const Operation& operation, Effect effects);
    void Branch(BlockId target);
    bool ShortBranch(u32 opcode, BlockId target);
    void TailJump(std::uintptr_t destination, bool link = false, u64 target_pc = 0, u32 conditional_head = UINT32_MAX);
    void DynamicDispatch();
    void PublishPCAndJump(u64 target_pc);
    void Epilogue();
    unsigned PrepareTerminalCondition(unsigned condition, bool& restore, bool& flip);
    void RestoreTerminalCondition(bool restore, bool flip);
    void ConditionPC(unsigned condition, u64 taken, u64 fallthrough);
    void ConditionEdges(unsigned condition, const StoredTerminal& terminal, BlockId next);
    void Terminal(const StoredTerminal& terminal, BlockId next);
    void EmitState(const Operation&, Cursor&, unsigned destination, const Home&, bool test_prefix);
};

} // namespace swift::runtime::backend::arm64::detail
