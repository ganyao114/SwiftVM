#include "base/error.h"
#include "runtime/arch/x86/state.h"
#include "runtime/arch/x86/x87.h"
#include "runtime/exit.h"
#include "runtime/externals/distorm/distorm.h"
#include "runtime/externals/distorm/mnemonics.h"
#include "runtime/frontend/x86/detail/decoded.h"
#include "runtime/frontend/x86/interpreter.h"
#include "tests/support/check.h"
#include "tests/support/guest_memory.h"
#include "tests/support/native_code.h"
#include "tests/support/test_mode.h"
#include "x86_fixtures.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using swift::runtime::x86::X87Command;
using swift::runtime::x86::X87Encode;
using swift::tests::Check;
using swift::tests::NativeMode;
using namespace swift::runtime::ir;
using namespace swift::tests::x86_ir;
using swift::x86::BlockWindow;
using swift::x86::BuildIR;

std::size_t CellCount(CellRange cells) {
    return static_cast<std::size_t>(std::distance(cells.begin(), cells.end()));
}

// Explicitly linear, used only for small expected rows in this test binary.
CellView CellAt(CellRange cells, std::size_t index) {
    auto at = cells.begin();
    while (index && at != cells.end()) {
        --index;
        ++at;
    }
    CHECK(index == 0 && at != cells.end());
    return *at;
}

bool StoresImmediate(const Program& program, std::uint32_t offset, std::uint64_t bits) {
    for (const auto& op : program.Operations()) {
        if (op.opcode != OpCode::StoreUniform) continue;
        const auto args = program.Operands(op);
        if (CellCount(args) != 2 || CellAt(args, 0).Payload() != offset ||
            CellAt(args, 1).Kind() != OperandKind::Value)
            continue;
        const auto& source = program.Operations()[CellAt(args, 1).Payload()];
        if (source.opcode == OpCode::LoadImm &&
            CellAt(program.Operands(source), 0).Payload() == bits)
            return true;
    }
    return false;
}

bool HasImmediate(const Program& program, std::uint64_t bits) {
    for (std::size_t id = 0; id < program.Operations().size(); ++id)
        for (const auto cell : program.Operands(NodeId{static_cast<std::uint32_t>(id)}))
            if (cell.Kind() == OperandKind::Immediate && cell.Payload() == bits) return true;
    return false;
}

void RealBytesAndControl() {
    for (const auto mode : {TargetMode::Known, TargetMode::EarlyOff, TargetMode::Unknown}) {
        const TargetInfo target{mode, 0xa64, 0x40};
        const auto result = BuildIR(windows, Scope::Function, layout, target);
        CHECK(result.program);
        CHECK(result.instruction_count == 13);
        CHECK(result.decoded_bytes == entry.size() + middle.size() + return_body.size());
        const auto& p = result.program;
        CHECK(p.ScopeKind() == Scope::Function && p.Target() == target);
        CHECK(p.Blocks().size() == 3);
        CHECK(p.Blocks()[0].terminal.kind == TerminalKind::Jump);
        CHECK(p.Blocks()[0].terminal.first.value == 1);
        CHECK(!p.Blocks()[0].terminal.HasValue() && p.Blocks()[0].State() == BlockState::Closed);
        CHECK(p.Blocks()[1].terminal.kind == TerminalKind::Jump);
        CHECK(p.Blocks()[1].terminal.first.value == 2);
        CHECK(p.Blocks()[2].terminal.kind == TerminalKind::Dispatch);
        CHECK(!p.Blocks()[2].terminal.HasValue() && p.Blocks()[2].State() == BlockState::Closed);
        CHECK(std::count_if(p.Operations().begin(), p.Operations().end(), [](const auto& op) {
                  return op.opcode == OpCode::ZeroExtend32To64;
              }) == 0);
        CHECK(std::count_if(p.Operations().begin(), p.Operations().end(), [](const auto& op) {
                  return op.opcode == OpCode::LoadMemoryTSO;
              }) == 3);
        CHECK(std::count_if(p.Operations().begin(), p.Operations().end(), [](const auto& op) {
                  return op.opcode == OpCode::StoreMemoryTSO;
              }) == 2);
        // Each real memory access has published its fault PC before the access.
        for (std::size_t b = 0; b < p.Blocks().size(); ++b) {
            bool pc_published = false;
            const auto& block = p.Blocks()[b];
            for (std::size_t i = block.first_node; i < block.first_node + block.node_count; ++i) {
                const auto& op = p.Operations()[i];
                if (op.opcode == OpCode::StoreUniform &&
                    CellAt(p.Operands(op), 0).Payload() == layout.pc_offset)
                    pc_published = true;
                if (op.opcode == OpCode::LoadMemoryTSO || op.opcode == OpCode::StoreMemoryTSO) {
                    CHECK(pc_published);
                    pc_published = false;
                }
            }
        }
        CHECK(StoresImmediate(p, layout.pc_offset, 0x1017));  // entry store
        CHECK(StoresImmediate(p, layout.pc_offset, 0x1040));  // RIP-relative load
        CHECK(StoresImmediate(p, layout.pc_offset, 0x1049));  // signed immediate store
        CHECK(StoresImmediate(p, layout.pc_offset, 0x1081));  // indexed load
        CHECK(StoresImmediate(p, layout.pc_offset, 0x1085));  // RET's stack load
        CHECK(HasImmediate(p, 0x1057));
        CHECK(HasImmediate(p, std::numeric_limits<std::uint64_t>::max()));
        CHECK(HasImmediate(p, 0x123456789abcdef0ULL));
        // RET commits RSP only after its actual stack load, and publishes
        // that same complete loaded value as PC after the stack update.
        const auto& last = p.Blocks().back();
        std::size_t ret_load = p.Operations().size(), sp_store = ret_load, pc_store = ret_load;
        for (std::size_t i = last.first_node; i < last.first_node + last.node_count; ++i)
            if (p.Operations()[i].opcode == OpCode::LoadMemoryTSO) ret_load = i;
        CHECK(ret_load < p.Operations().size());
        CHECK(p.Operations()[ret_load].type == Type::U64);
        const auto stack_address = p.Operands(p.Operations()[ret_load]).front();
        CHECK(stack_address.Kind() == OperandKind::Value && stack_address.Payload() < ret_load);
        const auto& stack = p.Operations()[stack_address.Payload()];
        CHECK(stack.opcode == OpCode::LoadUniform);
        CHECK(p.Operands(stack).front().Payload() == layout.gprs_offset + 4 * layout.stride);
        for (std::size_t i = ret_load + 1; i < last.first_node + last.node_count; ++i) {
            const auto& op = p.Operations()[i];
            if (op.opcode != OpCode::StoreUniform) continue;
            const auto cells = p.Operands(op);
            if (cells.front().Payload() == layout.gprs_offset + 4 * layout.stride) sp_store = i;
            if (cells.front().Payload() == layout.pc_offset &&
                CellAt(cells, 1).Kind() == OperandKind::Value &&
                CellAt(cells, 1).Payload() == ret_load)
                pc_store = i;
        }
        CHECK(ret_load < sp_store && sp_store < pc_store && pc_store < p.Operations().size());
        const auto advanced = CellAt(p.Operands(p.Operations()[sp_store]), 1);
        CHECK(advanced.Kind() == OperandKind::Value && advanced.Payload() < sp_store);
        const auto& add = p.Operations()[advanced.Payload()];
        CHECK(add.opcode == OpCode::Add && add.type == Type::U64);
        CHECK(add.Metadata() == OperationMetadata{});
        const auto add_cells = p.Operands(add);
        CHECK(CellCount(add_cells) == 2);
        CHECK(add_cells.front().Kind() == OperandKind::Value &&
              add_cells.front().Payload() == stack_address.Payload());
        CHECK(CellAt(add_cells, 1).Kind() == OperandKind::Immediate &&
              CellAt(add_cells, 1).Payload() == 8);
    }
    // The same JMP leaves a Block request through its actual external target.
    const auto block = BuildIR(std::span{windows}.first(1), Scope::Block, layout);
    CHECK(block.program && block.instruction_count == 6 && block.decoded_bytes == entry.size());
    CHECK(block.program.Blocks()[0].terminal.kind == TerminalKind::Dispatch);
    CHECK(StoresImmediate(block.program, layout.pc_offset, 0x1040));
}

void RejectedBytesCannotPublishAProgram() {
    const auto reject = [](std::span<const std::uint8_t> bytes) {
        const BlockWindow window{0x1000, bytes};
        CHECK(!BuildIR({&window, 1}, Scope::Block, layout).program);
    };
    reject({});
    const std::uint8_t truncated[]{0x48, 0xb8, 1};
    const std::uint8_t unsupported[]{0xf4, 0x90, 0x90};  // HLT, followed by unfetched padding.
    const std::uint8_t address32[]{0x67, 0x8b, 0x00};
    const std::uint8_t fs[]{0x64, 0x48, 0x8b, 0x00};
    const std::uint8_t lock[]{0xf0, 0x48, 0x89, 0x00};
    const std::uint8_t far_ret[]{0xca, 0x08, 0x00};  // Far return is still outside this runtime.
    for (const auto bytes : {std::span{truncated}, std::span{unsupported}}) reject(bytes);
    reject(fs);
    reject(lock);
    reject(far_ret);
    const BlockWindow wrapping{0x1000, address32};
    CHECK(BuildIR({&wrapping, 1}, Scope::Block, layout).program);
    CHECK(!BuildIR(windows, Scope::Block, layout).program);
    const std::array<BlockWindow, 2> unsorted{{windows[1], windows[0]}};
    CHECK(!BuildIR(unsorted, Scope::Function, layout).program);
    const std::array<BlockWindow, 2> duplicate{{windows[0], windows[0]}};
    CHECK(!BuildIR(duplicate, Scope::Function, layout).program);
}

void QualificationCountsTheActualAbandonedPrefix() {
    WorkCounts collected{9, 9, 9, 9};
    const auto invalid = BuildIR({}, Scope::Block, layout, {}, &collected);
    CHECK(invalid.error == swift::x86::BuildError::InvalidInput && !invalid.program);
    CHECK(collected == WorkCounts{});
    const auto reject = [&](std::span<const std::uint8_t> bytes) {
        const BlockWindow window{0x4000, bytes};
        collected = {9, 9, 9, 9};
        auto result = BuildIR({&window, 1}, Scope::Block, layout, {}, &collected);
        CHECK(result.error == swift::x86::BuildError::Unsupported && !result.program);
        return result;
    };
    const std::uint8_t hlt[]{0xf4, 0x90};
    const auto empty = reject(hlt);
    CHECK(empty.instruction_count == 0 && empty.decoded_bytes == 0 && empty.error_pc == 0x4000);
    CHECK(collected.nodes == 0 && collected.value_uses == 0 && collected.operand_words == 0);
    CHECK(collected.blocks == 1);  // The actual declared/open block still exists.
    const std::uint8_t prefix[]{
            0x48, 0xb8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xf4, 0x90};
    const auto partial = reject(prefix);
    CHECK(partial.instruction_count == 1 && partial.decoded_bytes == 10 &&
          partial.error_pc == 0x400a);
    CHECK(collected.nodes == 2 && collected.value_uses == 1 && collected.operand_words == 4 &&
          collected.blocks == 1);
    const std::uint8_t privileged[]{
            0x0f, 0x22, 0xc0};  // MOV CR0,RAX is rejected before operand access.
    const auto born_before_rejection = reject(privileged);
    CHECK(born_before_rejection.instruction_count == 0 && born_before_rejection.decoded_bytes == 0);
    CHECK(born_before_rejection.error_pc == 0x4000);
    CHECK(collected.nodes == 0 && collected.value_uses == 0 && collected.operand_words == 0 &&
          collected.blocks == 1);

    const auto complete = BuildIR(windows, Scope::Function, layout, {}, &collected);
    CHECK(complete);
    WorkCounts published{complete.program.Operations().size(),
                         0,
                         complete.program.OperandArena().size(),
                         complete.program.Blocks().size()};
    for (std::size_t id = 0; id < complete.program.Operations().size(); ++id)
        for (const auto cell : complete.program.Operands(NodeId{static_cast<std::uint32_t>(id)}))
            published.value_uses += cell.Kind() == OperandKind::Value;
    for (const auto& block : complete.program.Blocks())
        published.value_uses += block.terminal.HasValue();
    CHECK(collected == published);
}

void RexExtensionRequiresTheFinalPrefixPosition() {
    const auto check = [](std::span<const std::uint8_t> bytes,
                          std::initializer_list<unsigned> expected_reads,
                          unsigned destination,
                          Type read_type = Type::U32) {
        const BlockWindow window{0x3000, bytes};
        const auto built = BuildIR({&window, 1}, Scope::Block, layout);
        CHECK(built && built.instruction_count == 1 && built.decoded_bytes == bytes.size());
        std::uint32_t reads{}, expected_mask{};
        std::size_t read_count{}, writes{};
        for (const auto reg : expected_reads) expected_mask |= std::uint32_t{1} << reg;
        for (const auto& op : built.program.Operations()) {
            if (op.opcode == OpCode::LoadUniform) {
                const auto field = built.program.Operands(op).front();
                CHECK(field.Kind() == OperandKind::Uniform && field.TypeView() == read_type);
                CHECK(field.Payload() >= layout.gprs_offset);
                const auto offset = field.Payload() - layout.gprs_offset;
                CHECK(offset % layout.stride == 0 && offset / layout.stride < 16);
                reads |= std::uint32_t{1} << (offset / layout.stride);
                ++read_count;
            }
            if (op.opcode == OpCode::StoreUniform) {
                const auto field = built.program.Operands(op).front();
                if (field.Payload() == layout.pc_offset) continue;
                CHECK(field.Payload() == layout.gprs_offset + destination * layout.stride);
                CHECK(field.TypeView() == Type::U64);
                CHECK(CellAt(built.program.Operands(op), 1).TypeView() == Type::U32);
                ++writes;
            }
        }
        CHECK(reads == expected_mask && read_count == expected_reads.size() && writes == 1);
        CHECK(std::count_if(built.program.Operations().begin(),
                            built.program.Operations().end(),
                            [](const auto& op) { return op.opcode == OpCode::ZeroExtend32To64; }) ==
              0);
    };
    const std::uint8_t ignored_b[]{0x41, 0x2e, 0x89, 0xc0};
    const std::uint8_t ignored_r[]{0x44, 0x2e, 0x89, 0xc0};
    const std::uint8_t active_b[]{0x2e, 0x41, 0x89, 0xc0};
    const std::uint8_t replaced[]{0x41, 0x2e, 0x44, 0x89, 0xc0};
    const std::uint8_t all_ignored[]{0x41, 0x44, 0x2e, 0x89, 0xc0};
    const std::uint8_t memory[]{0x4b, 0x2e, 0x8b, 0x04, 0x08};  // Ignored W/X/B: mov eax,[rax+rcx].
    check(ignored_b, {0}, 0);
    check(ignored_r, {0}, 0);
    check(active_b, {0}, 8);
    check(replaced, {8}, 0);
    check(all_ignored, {0}, 0);
    check(memory, {0, 1}, 0, Type::U64);

    // Exercise the original pure decoder as well: the shared extension field
    // must preserve valid VEX, and REX+VEX stays an invalid encoding.
    const auto vex = [](std::span<const std::uint8_t> bytes, bool valid) {
        _CodeInfo code{};
        code.code = bytes.data();
        code.codeLen = static_cast<int>(bytes.size());
        code.dt = Decode64Bits;
        _DInst instructions[15]{};
        unsigned used{};
        (void)distorm_decompose(&code, instructions, 15, &used);
        CHECK(used != 0);
        const auto& instruction = instructions[0];
        if (valid) {
            CHECK(used == 1);
            CHECK(instruction.flags != FLAG_NOT_DECODABLE && instruction.opcode == I_VZEROUPPER);
            CHECK(instruction.size == bytes.size());
        } else {
            CHECK(instruction.flags == FLAG_NOT_DECODABLE && instruction.size == 1);
            CHECK(instruction.imm.byte == 0x40);
        }
    };
    const std::uint8_t vex2[]{0xc5, 0xf8, 0x77};
    const std::uint8_t vex3[]{0xc4, 0xe1, 0x78, 0x77};
    const std::uint8_t rex_vex2[]{0x40, 0xc5, 0xf8, 0x77};
    const std::uint8_t rex_vex3[]{0x40, 0xc4, 0xe1, 0x78, 0x77};
    vex(vex2, true);
    vex(vex3, true);
    vex(rex_vex2, false);
    vex(rex_vex3, false);
}

void KnownTargetEmitsOneShiftedAddress() {
    const std::uint8_t lea[]{0x48, 0x8d, 0x54, 0x88, 0xf0};  // lea rdx,[rax+rcx*4-16]
    const BlockWindow window{0x3000, lea};
    const auto feature = static_cast<std::uint64_t>(TargetFeature::ShiftedAddress);
    std::size_t generic_nodes{};
    for (const auto mode : {TargetMode::Unknown, TargetMode::EarlyOff, TargetMode::Known}) {
        const auto built = BuildIR({&window, 1}, Scope::Block, layout, {mode, 0xa64, feature});
        CHECK(built && built.instruction_count == 1);
        std::size_t shifted{}, shifts{};
        for (const auto& op : built.program.Operations()) {
            shifts += op.opcode == OpCode::LslImm;
            if (op.opcode != OpCode::GetOperand) continue;
            const auto cells = built.program.Operands(op);
            CHECK(CellCount(cells) == 3 && op.type == Type::U64);
            const auto expression = cells.front();
            CHECK(expression.Kind() == OperandKind::Expression);
            CHECK(expression.ExpressionOp() == OperandOp::AddShift);
            CHECK(expression.ShiftAmount() == 2 && expression.Count() == 2);
            CHECK(CellAt(cells, 1).Kind() == OperandKind::Value);
            CHECK(CellAt(cells, 2).Kind() == OperandKind::Value);
            ++shifted;
        }
        CHECK(HasImmediate(built.program, std::uint64_t(-16)));
        if (mode == TargetMode::Known) {
            CHECK(shifted == 1 && shifts == 0);
            CHECK(built.program.Operations().size() + 1 == generic_nodes);
        } else {
            CHECK(shifted == 0 && shifts == 1);
            if (!generic_nodes) generic_nodes = built.program.Operations().size();
            CHECK(built.program.Operations().size() == generic_nodes);
        }
    }
    const auto without = BuildIR({&window, 1}, Scope::Block, layout, {TargetMode::Known, 0xa64, 0});
    CHECK(without && without.program.Operations().size() == generic_nodes);

    const std::uint8_t addr32_lea[]{
            0x67, 0x8d, 0x51, 0xff,              // lea edx,[ecx-1]: actual glibc entry.
            0x67, 0x4f, 0x8d, 0x44, 0x8a, 0x10,  // lea r8,[r10d+r9d*4+16].
            0x66, 0x67, 0x8d, 0x41, 0x01,        // lea ax,[ecx+1]: retain RAX's high bits.
            0x64, 0x67, 0x48, 0x8d, 0x05, 0xff,
            0xff, 0xff, 0xff};  // FS ignored; EIP-relative wraps.
    const BlockWindow addr32_source{0x100003100ULL, addr32_lea};
    const auto addr32 =
            BuildIR({&addr32_source, 1}, Scope::Block, layout, {TargetMode::Known, 0xa64, feature});
    CHECK(addr32 && addr32.instruction_count == 4 && addr32.decoded_bytes == sizeof(addr32_lea));
    std::vector<std::uint64_t> addr32_reads;
    std::size_t widened{}, partial{}, full32{}, scaled32{};
    for (const auto& op : addr32.program.Operations()) {
        const auto cells = addr32.program.Operands(op);
        CHECK(op.Metadata() == OperationMetadata{});
        CHECK(op.opcode != OpCode::LoadMemoryTSO && op.opcode != OpCode::FaultLocation);
        if (op.opcode == OpCode::LoadUniform) {
            CHECK(op.type == Type::U32);
            addr32_reads.push_back(cells.front().Payload());
        }
        if (op.opcode == OpCode::Add) {
            CHECK(op.type == Type::U32 && cells.front().TypeView() == Type::U32);
        }
        if (op.opcode == OpCode::GetOperand) {
            CHECK(op.type == Type::U32 && cells.front().ExpressionOp() == OperandOp::AddShift);
            CHECK(CellAt(cells, 1).TypeView() == Type::U32 &&
                  CellAt(cells, 2).TypeView() == Type::U32);
            ++scaled32;
        }
        if (op.opcode == OpCode::ZeroExtend64) {
            CHECK(op.type == Type::U64 && cells.front().TypeView() == Type::U32);
            ++widened;
        }
        if (op.opcode == OpCode::StoreUniform && cells.front().Payload() == layout.gprs_offset &&
            cells.front().TypeView() == Type::U16)
            ++partial;
        if (op.opcode == OpCode::StoreUniform &&
            cells.front().Payload() == layout.gprs_offset + 16) {
            CHECK(cells.front().TypeView() == Type::U64 &&
                  CellAt(cells, 1).TypeView() == Type::U32);
            ++full32;
        }
    }
    CHECK(addr32_reads == std::vector<std::uint64_t>({layout.gprs_offset + 8,
                                                      layout.gprs_offset + 80,
                                                      layout.gprs_offset + 72,
                                                      layout.gprs_offset + 8}));
    CHECK(widened == 2 && partial == 1 && full32 == 1 && scaled32 == 1);
    CHECK(HasImmediate(addr32.program, 0xffffffffu));
    CHECK(HasImmediate(addr32.program,
                       static_cast<std::uint32_t>(addr32_source.pc + sizeof(addr32_lea) - 1)));

    // One actual address SSA can feed distinct faulting accesses; a changed
    // shift, index version or block still needs its own definition.
    const std::uint8_t accesses[]{
            0x4c, 0x8b, 0x04, 0x88,              // mov r8,[rax+rcx*4]
            0x4c, 0x8b, 0x4c, 0x88, 0x08,        // mov r9,[rax+rcx*4+8]
            0x64, 0x4c, 0x8b, 0x54, 0x88, 0x10,  // mov r10,fs:[rax+rcx*4+16]
            0x4c, 0x8b, 0x1c, 0xc8,              // mov r11,[rax+rcx*8]
            0x4c, 0x8b, 0x64, 0x88, 0x10,        // mov r12,[rax+rcx*4+16]
            0x48, 0xff, 0xc1,                    // inc rcx: new captured index version
            0x4c, 0x8b, 0x6c, 0x88, 0x18,        // mov r13,[rax+rcx*4+24]
            0xe9, 0xdb, 0,    0,    0,           // jmp 0x3500
    };
    const std::uint8_t next[]{0x4c, 0x8b, 0x74, 0x88, 0x20};  // mov r14,[rax+rcx*4+32]
    const std::array<BlockWindow, 2> address_windows{{{0x3400, accesses}, {0x3500, next}}};
    auto segmented_layout = layout;
    segmented_layout.fs_base_offset = layout.pc_offset + 8;
    const auto features = feature | static_cast<std::uint64_t>(TargetFeature::PrivateGPRState);
    const auto reused = BuildIR(address_windows,
                                Scope::Function,
                                segmented_layout,
                                {TargetMode::Known, 0xa64, features});
    CHECK(reused && reused.instruction_count == 9 &&
          reused.decoded_bytes == sizeof(accesses) + sizeof(next));
    const auto& p = reused.program;
    std::vector<NodeId> addresses, loads, fs_reads;
    for (std::size_t i = 0; i < p.Operations().size(); ++i) {
        const auto& op = p.Operations()[i];
        if (op.opcode == OpCode::GetOperand) {
            CHECK(op.type == Type::U64 && op.Metadata() == OperationMetadata{});
            addresses.push_back({static_cast<std::uint32_t>(i)});
        }
        if (op.opcode == OpCode::LoadMemoryTSO) loads.push_back({static_cast<std::uint32_t>(i)});
        if (op.opcode == OpCode::LoadUniform &&
            p.Operands(op).front().Payload() == segmented_layout.fs_base_offset)
            fs_reads.push_back({static_cast<std::uint32_t>(i)});
    }
    CHECK(addresses.size() == 5 && loads.size() == 7 && fs_reads.size() == 1);
    const auto base = [&](NodeId load) {
        const auto cells = p.Operands(load);
        if (cells.front().Kind() == OperandKind::Expression) {
            CHECK(CellCount(cells) == 3 && cells.front().ExpressionOp() == OperandOp::Add);
            return CellAt(cells, 1);
        }
        CHECK(CellCount(cells) == 1);
        return cells.front();
    };
    CHECK(base(loads[0]).Payload() == addresses[0].value &&
          base(loads[1]).Payload() == addresses[0].value);
    CHECK(CellAt(p.Operands(loads[1]), 2).Payload() == 8);
    const auto fs_base = base(loads[2]);
    const auto& fs_sum = p.Operations()[fs_base.Payload()];
    CHECK(fs_sum.opcode == OpCode::Add && fs_sum.type == Type::U64);
    CHECK(p.Operands(fs_sum).front().Payload() == addresses[0].value);
    CHECK(CellAt(p.Operands(fs_sum), 1).Payload() == fs_reads[0].value);
    CHECK(CellAt(p.Operands(loads[2]), 2).Payload() == 16);
    CHECK(base(loads[3]).Payload() == addresses[1].value);
    CHECK(p.Operands(addresses[0]).front().ShiftAmount() == 2 &&
          p.Operands(addresses[1]).front().ShiftAmount() == 3);
    CHECK(base(loads[4]).Payload() == addresses[2].value &&
          CellAt(p.Operands(loads[4]), 2).Payload() == 16);
    const auto old_index = p.Operands(addresses[2]), new_index = p.Operands(addresses[3]);
    CHECK(old_index.front().Code() == new_index.front().Code());
    CHECK(CellAt(old_index, 1).Payload() == CellAt(new_index, 1).Payload());
    CHECK(CellAt(old_index, 2).Payload() != CellAt(new_index, 2).Payload());
    CHECK(base(loads[5]).Payload() == addresses[3].value &&
          CellAt(p.Operands(loads[5]), 2).Payload() == 24);
    CHECK(base(loads[6]).Payload() == addresses[4].value &&
          CellAt(p.Operands(loads[6]), 2).Payload() == 32);
    CHECK(p.Operations()[addresses[3].value].block.value == 0 &&
          p.Operations()[addresses[4].value].block.value == 1);
}

void IntegerAluAndDeferredFlags() {
    const auto one = [](std::initializer_list<std::uint8_t> bytes,
                        OpCode expected,
                        Type type,
                        bool stores_result,
                        FlagProfile profile) {
        const BlockWindow window{0x5000, {bytes.begin(), bytes.size()}};
        auto built = BuildIR({&window, 1}, Scope::Block, layout);
        CHECK(built && built.instruction_count == 1 && built.decoded_bytes == bytes.size());
        std::size_t writers{}, results{}, stores{};
        for (const auto& op : built.program.Operations()) {
            if (op.opcode == expected) {
                CHECK(op.type == type);
                CHECK((op.Metadata() == OperationMetadata{FlagBits(Flag::All), profile, false}));
                ++results;
            }
            writers += op.Metadata().flag_outputs != 0;
            if (op.opcode == OpCode::StoreUniform &&
                built.program.Operands(op).front().Payload() != layout.pc_offset)
                ++stores;
        }
        CHECK(results == 1 && writers == 1 && stores == std::size_t(stores_result));
        return built;
    };
    for (const bool wide : {false, true}) {
        const auto type = wide ? Type::U64 : Type::U32;
        const auto run =
                [&](std::uint8_t opcode, OpCode expected, bool write, FlagProfile profile) {
                    if (wide)
                        (void)one({0x48, opcode, 0xd8}, expected, type, write, profile);
                    else
                        (void)one({opcode, 0xd8}, expected, type, write, profile);
                };
        run(0x01, OpCode::Add, true, FlagProfile::ArithmeticCanonicalCarry);
        run(0x29, OpCode::Sub, true, FlagProfile::ArithmeticCanonicalCarry);
        run(0x39, OpCode::Sub, false, FlagProfile::ArithmeticCanonicalCarry);
        run(0x21, OpCode::And, true, FlagProfile::LogicalZeroCVA);
        run(0x09, OpCode::Or, true, FlagProfile::LogicalZeroCVA);
        run(0x31, OpCode::Xor, true, FlagProfile::LogicalZeroCVA);
        run(0x85, OpCode::And, false, FlagProfile::LogicalZeroCVA);
        run(0xf7, OpCode::Neg, true, FlagProfile::ArithmeticCanonicalCarry);
    }
    const auto immediate = one({0x48, 0x83, 0xc0, 0xff},
                               OpCode::Add,
                               Type::U64,
                               true,
                               FlagProfile::ArithmeticCanonicalCarry);
    CHECK(HasImmediate(immediate.program, std::numeric_limits<std::uint64_t>::max()));
    const auto immediate32 = one({0x83, 0xc0, 0xff},
                                 OpCode::Add,
                                 Type::U32,
                                 true,
                                 FlagProfile::ArithmeticCanonicalCarry);
    CHECK(HasImmediate(immediate32.program, 0xffffffffULL));

    const std::uint8_t overwrite[]{0x48, 0x01, 0xd8, 0x48, 0x83, 0xe8, 1, 0x31, 0xc0, 0x74, 2};
    const BlockWindow window{0x5000, overwrite};
    const auto built = BuildIR({&window, 1}, Scope::Block, layout);
    CHECK(built && built.instruction_count == 4);
    std::size_t writers{};
    for (const auto& op : built.program.Operations()) {
        if (op.opcode == OpCode::Add || op.opcode == OpCode::Sub)
            CHECK(op.Metadata() == OperationMetadata{});
        if (op.Metadata().flag_outputs) {
            CHECK(op.opcode == OpCode::Xor && op.Metadata().profile == FlagProfile::LogicalZeroCVA);
            ++writers;
        }
        CHECK(op.opcode != OpCode::SaveFlags);
    }
    CHECK(writers == 1);
    const std::uint8_t fault[]{0x48, 0x01, 0xd8, 0x48, 0x8b, 0x01, 0x48, 0x29, 0xd8, 0xc3};
    const BlockWindow fault_window{0x5000, fault};
    const auto observed = BuildIR({&fault_window, 1}, Scope::Block, layout);
    CHECK(observed && observed.instruction_count == 4);
    writers = 0;
    for (const auto& op : observed.program.Operations()) {
        if (op.opcode == OpCode::Add || op.opcode == OpCode::Sub) {
            // RET's internal RSP+8 is a pure address calculation.
            if (!op.Metadata().flag_outputs) continue;
            CHECK(op.Metadata().flag_outputs == FlagBits(Flag::All));
            ++writers;
        }
    }
    CHECK(writers == 2);  // The later SUB cannot discard flags visible at the MOV fault.
    for (const auto& bytes :
         {std::vector<std::uint8_t>{0xf4},                 // HLT
          std::vector<std::uint8_t>{0xd9, 0xee},           // x87 FLDZ
          std::vector<std::uint8_t>{0x0f, 0x58, 0xc0}}) {  // ADDPS floating arithmetic
        const BlockWindow unsupported{0x5000, bytes};
        CHECK(BuildIR({&unsupported, 1}, Scope::Block, layout).error ==
              swift::x86::BuildError::Unsupported);
    }
}

std::uint64_t BlockPC(const Program& p, const BlockRecord& block) {
    for (std::size_t i = block.first_node; i < block.first_node + block.node_count; ++i) {
        const auto& op = p.Operations()[i];
        if (op.opcode != OpCode::StoreUniform) continue;
        const auto args = p.Operands(op);
        if (args.front().Payload() != layout.pc_offset) continue;
        const auto& value = p.Operations()[CellAt(args, 1).Payload()];
        CHECK(value.opcode == OpCode::LoadImm);
        return p.Operands(value).front().Payload();
    }
    swift::ThrowError("block has no constant PC store");
}

void ExternalEntriesAndReadyFrontiers() {
    // Entry reuse does not expand the region through a CALL. Its continuation
    // is decoded only when actually requested, even with the entry capability.
    const std::uint8_t bytes[]{0xe8, 0xfb, 0x0f, 0x00, 0x00, 0x48, 0x83, 0xc0, 0x01, 0xc3};
    const BlockWindow window{0x1000, bytes};
    const auto capability = static_cast<std::uint64_t>(TargetFeature::ExternalEntries);
    const TargetInfo known{TargetMode::Known, 0xa64, capability};
    const auto count_entries = [](const Program& p) {
        std::vector<std::uint64_t> pcs;
        for (const auto& op : p.Operations())
            if (op.opcode == OpCode::EntryPoint) {
                CHECK(op.Metadata() == OperationMetadata{});
                pcs.push_back(p.Operands(op).front().Payload());
            }
        return pcs;
    };
    const auto built = swift::x86::BuildFunctionIR({&window, 1}, window.pc, layout, {}, known);
    CHECK(built && built.instruction_count == 1 && built.program.Blocks().size() == 1);
    CHECK(count_entries(built.program).empty());
    const auto continuation = swift::x86::BuildFunctionIR({&window, 1}, 0x1005, layout, {}, known);
    CHECK(continuation && continuation.instruction_count == 2 &&
          count_entries(continuation.program).empty());
    for (auto mode : {TargetMode::Unknown, TargetMode::EarlyOff}) {
        const auto old = swift::x86::BuildFunctionIR(
                {&window, 1}, window.pc, layout, {}, {mode, 0xa64, capability});
        CHECK(old && old.instruction_count == 1 && count_entries(old.program).empty());
    }
    const std::uint8_t branch[]{0x75, 0x02, 0x90, 0xc3, 0xc3};
    const BlockWindow frontier{0x2000, branch};
    const auto formed = swift::x86::BuildFunctionIR({&frontier, 1}, frontier.pc, layout, {}, known);
    CHECK(formed && formed.instruction_count == 4);
    CHECK(count_entries(formed.program) == std::vector<std::uint64_t>({0x2002, 0x2004}));
    std::uint64_t ready_pc = 0x2004;
    const swift::x86::ReadyEntryLookup ready{&ready_pc, [](void* p, std::uint64_t pc) noexcept {
                                                 return pc == *static_cast<std::uint64_t*>(p);
                                             }};
    const auto stopped = swift::x86::BuildFunctionIR(
            {&frontier, 1}, frontier.pc, layout, {}, known, nullptr, nullptr, nullptr, ready);
    CHECK(stopped && stopped.instruction_count == 3 &&
          count_entries(stopped.program) == std::vector<std::uint64_t>{0x2002});
    ready_pc = 0x2003;
    const auto sequential = swift::x86::BuildFunctionIR(
            {&frontier, 1}, frontier.pc, layout, {}, known, nullptr, nullptr, nullptr, ready);
    CHECK(sequential && sequential.instruction_count == 3 &&
          count_entries(sequential.program) == std::vector<std::uint64_t>({0x2002, 0x2004}));
    for (auto mode : {TargetMode::Unknown, TargetMode::EarlyOff}) {
        const auto old = swift::x86::BuildFunctionIR({&frontier, 1},
                                                     frontier.pc,
                                                     layout,
                                                     {},
                                                     {mode, 0xa64, capability},
                                                     nullptr,
                                                     nullptr,
                                                     nullptr,
                                                     ready);
        CHECK(old && old.instruction_count == 4 && count_entries(old.program).empty());
    }
    const auto bounded =
            swift::x86::BuildFunctionIR({&frontier, 1}, frontier.pc, layout, {2, 8}, known);
    CHECK(bounded && bounded.instruction_count == 2);
    const auto entries = count_entries(bounded.program);
    CHECK(std::find(entries.begin(), entries.end(), 0x2004) ==
          entries.end());  // Empty budget frontier cannot call itself.
    const std::uint8_t noreturn[]{0xe8, 0xfb, 0x0f, 0x00, 0x00, 0xf4};
    const BlockWindow padded{0x3000, noreturn};
    const auto before_trap =
            swift::x86::BuildFunctionIR({&padded, 1}, padded.pc, layout, {}, known);
    CHECK(before_trap && before_trap.instruction_count == 1 &&
          count_entries(before_trap.program).empty());
    CHECK(swift::x86::BuildFunctionIR({&padded, 1}, 0x3005, layout, {}, known).error ==
          swift::x86::BuildError::Unsupported);
}

void EveryConditionalBranchAndRealExits() {
    constexpr Condition conditions[]{Condition::VS,
                                     Condition::VC,
                                     Condition::CS,
                                     Condition::CC,
                                     Condition::EQ,
                                     Condition::NE,
                                     Condition::AL,
                                     Condition::AL,
                                     Condition::MI,
                                     Condition::PL,
                                     Condition::AL,
                                     Condition::AL,
                                     Condition::LT,
                                     Condition::GE,
                                     Condition::LE,
                                     Condition::GT};
    for (unsigned cc = 0; cc < 16; ++cc) {
        const std::uint8_t bytes[]{static_cast<std::uint8_t>(0x70 + cc), 0x20};
        const BlockWindow window{0x6000, bytes};
        for (const auto scope : {Scope::Block, Scope::Function}) {
            const auto built = BuildIR({&window, 1}, scope, layout);
            CHECK(built && built.instruction_count == 1 && built.decoded_bytes == 2);
            const auto& p = built.program;
            const auto& terminal = p.Blocks()[0].terminal;
            NodeId predicate;
            if (scope == Scope::Block) {
                CHECK(p.Blocks().size() == 1 && terminal.kind == TerminalKind::Dispatch);
                std::size_t selects{}, pc_stores{};
                for (std::size_t id = 0; id < p.Operations().size(); ++id) {
                    const auto& op = p.Operations()[id];
                    if (op.opcode == OpCode::Select) {
                        const auto args = p.Operands(op);
                        predicate = {static_cast<std::uint32_t>(args.front().Payload())};
                        const auto yes = CellAt(args, 1), no = CellAt(args, 2);
                        CHECK(p.Operands(NodeId{static_cast<std::uint32_t>(yes.Payload())})
                                      .front()
                                      .Payload() == 0x6022);
                        CHECK(p.Operands(NodeId{static_cast<std::uint32_t>(no.Payload())})
                                      .front()
                                      .Payload() == 0x6002);
                        ++selects;
                    }
                    if (op.opcode == OpCode::StoreUniform &&
                        p.Operands(op).front().Payload() == layout.pc_offset)
                        ++pc_stores;
                }
                CHECK(selects == 1 && pc_stores == 1);
            } else {
                CHECK(p.Blocks().size() == 3 && terminal.kind == TerminalKind::Branch);
                CHECK(terminal.HasValue());
                predicate = terminal.value;
                CHECK(BlockPC(p, p.Blocks()[terminal.first.value]) == 0x6022);
                CHECK(BlockPC(p, p.Blocks()[terminal.second.value]) == 0x6002);
                CHECK(p.Blocks()[terminal.first.value].terminal.kind == TerminalKind::Dispatch);
                CHECK(p.Blocks()[terminal.second.value].terminal.kind == TerminalKind::Dispatch);
                for (std::size_t i = p.Blocks()[0].first_node;
                     i < p.Blocks()[0].first_node + p.Blocks()[0].node_count;
                     ++i)
                    CHECK(p.Operations()[i].opcode != OpCode::StoreUniform);
            }
            CHECK(predicate);
            const auto& op = p.Operations()[predicate.value];
            const auto input = p.Operands(predicate).front();
            if (cc == 6 || cc == 7 || cc == 10 || cc == 11) {
                CHECK(op.opcode == ((cc & 1) ? OpCode::TestNotFlags : OpCode::TestFlags));
                const auto flags = cc < 8 ? (Flag::Carry | Flag::Zero) : Flag::Parity;
                CHECK(input.Kind() == OperandKind::Flags && input.Payload() == FlagBits(flags));
            } else {
                CHECK(op.opcode == OpCode::CondSet && input.Kind() == OperandKind::Condition);
                CHECK(input.Payload() == static_cast<unsigned>(conditions[cc]));
            }
        }
        const auto capability = static_cast<std::uint64_t>(TargetFeature::ConditionalUniformStore);
        for (const auto mode : {TargetMode::Known, TargetMode::Unknown, TargetMode::EarlyOff}) {
            const auto outgoing =
                    BuildIR({&window, 1}, Scope::Function, layout, {mode, 0xa64, capability});
            CHECK(outgoing && outgoing.program.ScopeKind() == Scope::Function);
            const auto& p = outgoing.program;
            if (mode == TargetMode::Known) {
                CHECK(p.Blocks().size() == 1 &&
                      p.Blocks()[0].terminal.kind == TerminalKind::Dispatch);
                const auto tail = NodeId{static_cast<std::uint32_t>(p.Operations().size() - 1)};
                CHECK(p.Operations()[tail.value].opcode == OpCode::StoreUniformSelectImm);
                const auto args = p.Operands(tail);
                CHECK(args.front().Payload() == layout.pc_offset &&
                      CellAt(args, 2).Payload() == 0x6022 && CellAt(args, 3).Payload() == 0x6002);
            } else
                CHECK(p.Blocks().size() == 3 &&
                      p.Blocks()[0].terminal.kind == TerminalKind::Branch);
        }
    }
    const std::uint8_t head[]{0x74, 0x1e}, fallthrough[]{0x90}, taken[]{0x90};
    const std::array<BlockWindow, 3> internal{
            {{0x6000, head}, {0x6002, fallthrough}, {0x6020, taken}}};
    const auto built = BuildIR(internal, Scope::Function, layout);
    CHECK(built && built.program.Blocks().size() == 3);
    CHECK(built.program.Blocks()[0].terminal.kind == TerminalKind::Branch);
    CHECK(built.program.Blocks()[0].terminal.first.value == 2 &&
          built.program.Blocks()[0].terminal.second.value == 1);
    const TargetInfo known{TargetMode::Known,
                           0xa64,
                           static_cast<std::uint64_t>(TargetFeature::ConditionalUniformStore)};
    const auto mixed = BuildIR(std::span{internal}.first(2), Scope::Function, layout, known);
    CHECK(mixed && mixed.program.Blocks().size() == 3 &&
          mixed.program.Blocks()[0].terminal.kind == TerminalKind::Branch);
    const auto external = BuildIR(internal, Scope::Function, layout, known, nullptr, nullptr, true);
    CHECK(external && external.program.Blocks().size() == 3 &&
          external.program.Blocks()[0].terminal.kind == TerminalKind::Dispatch);
}

void ActualDecodedBufferAndRejectedFrontier() {
    const std::uint8_t bytes[]{0x48, 0x83, 0xc0, 0xff, 0x48, 0x39, 0xc8, 0x75, 2};
    const BlockWindow source{0x7000, bytes};
    _CodeInfo code{};
    code.code = bytes;
    code.codeLen = sizeof(bytes);
    code.codeOffset = source.pc;
    code.dt = Decode64Bits;
    std::array<_DInst, 3> decoded{};
    unsigned used{};
    (void)distorm_decompose(&code, decoded.data(), decoded.size(), &used);
    CHECK(used == decoded.size());
    swift::x86::DecodedBlock block{source.pc, 0, used};
    const auto direct = swift::x86::BuildDecodedIR(swift::x86::detail::FixtureNormalize(decoded),
                                                   {&block, 1},
                                                   {&source, 1},
                                                   Scope::Block,
                                                   layout);
    const auto byte_path = BuildIR({&source, 1}, Scope::Block, layout);
    CHECK(direct && byte_path && direct.instruction_count == 3 &&
          direct.decoded_bytes == sizeof(bytes));
    CHECK(direct.program.Operations().size() == byte_path.program.Operations().size());
    for (std::size_t i = 0; i < direct.program.Operations().size(); ++i) {
        const auto& a = direct.program.Operations()[i];
        const auto& b = byte_path.program.Operations()[i];
        CHECK(a.opcode == b.opcode && a.type == b.type && a.Metadata() == b.Metadata());
        const auto x = direct.program.Operands(a), y = byte_path.program.Operands(b);
        CHECK(CellCount(x) == CellCount(y));
        auto at = x.begin(), bt = y.begin();
        for (; at != x.end(); ++at, ++bt) {
            const auto left = *at, right = *bt;
            CHECK(left.Kind() == right.Kind() && left.TypeView() == right.TypeView() &&
                  left.Payload() == right.Payload() && left.Count() == right.Count() &&
                  left.Code() == right.Code());
        }
    }
    auto invalid = decoded;
    ++invalid[0].addr;
    CHECK(swift::x86::BuildDecodedIR(swift::x86::detail::FixtureNormalize(invalid),
                                     {&block, 1},
                                     {&source, 1},
                                     Scope::Block,
                                     layout)
                  .error == swift::x86::BuildError::InvalidInput);
    auto bad_slice = block;
    bad_slice.first_instruction = 1;
    CHECK(swift::x86::BuildDecodedIR(swift::x86::detail::FixtureNormalize(decoded),
                                     {&bad_slice, 1},
                                     {&source, 1},
                                     Scope::Block,
                                     layout)
                  .error == swift::x86::BuildError::InvalidInput);

    // Discovery may append the higher-PC body first and split into slices
    // later. Neither emission nor a Block request may reorder/copy that owner.
    const std::array<_DInst, 3> discovered{{decoded[1], decoded[2], decoded[0]}};
    const std::array<swift::x86::DecodedBlock, 2> slices{{{0x7000, 2, 1}, {0x7004, 0, 2}}};
    const auto reordered =
            swift::x86::BuildDecodedIR(swift::x86::detail::FixtureNormalize(discovered),
                                       slices,
                                       {&source, 1},
                                       Scope::Function,
                                       layout);
    CHECK(reordered && reordered.instruction_count == 3 &&
          reordered.decoded_bytes == sizeof(bytes));
    CHECK(reordered.program.Operations()[0].opcode == OpCode::LoadUniform);
    CHECK(reordered.program.Blocks()[0].terminal.kind == TerminalKind::Jump &&
          reordered.program.Blocks()[0].terminal.first.value == 1);
    CHECK(reordered.program.Blocks()[1].terminal.kind == TerminalKind::Branch);
    WorkCounts selected_counts;
    const auto selected =
            swift::x86::BuildDecodedIR(swift::x86::detail::FixtureNormalize(discovered),
                                       std::span{slices}.subspan(1),
                                       {&source, 1},
                                       Scope::Block,
                                       layout,
                                       {},
                                       &selected_counts);
    CHECK(selected && selected.instruction_count == 2 && selected.decoded_bytes == 5);
    CHECK(selected.program.Blocks().size() == 1 && selected_counts.blocks == 1);
    CHECK(std::none_of(selected.program.Operations().begin(),
                       selected.program.Operations().end(),
                       [](const auto& op) { return op.opcode == OpCode::Add; }));
    const auto tail = swift::x86::DecodedBlock{0x7007, 1, 1};
    const auto selected_tail =
            swift::x86::BuildDecodedIR(swift::x86::detail::FixtureNormalize(discovered),
                                       {&tail, 1},
                                       {&source, 1},
                                       Scope::Block,
                                       layout);
    CHECK(selected_tail && selected_tail.instruction_count == 1 &&
          selected_tail.decoded_bytes == 2);
    auto overlapping = slices;
    overlapping[1].pc = 0x7002;
    CHECK(swift::x86::BuildDecodedIR(swift::x86::detail::FixtureNormalize(discovered),
                                     overlapping,
                                     {&source, 1},
                                     Scope::Function,
                                     layout)
                  .error == swift::x86::BuildError::InvalidInput);
    auto reused = slices;
    reused[1].first_instruction = 2;
    reused[1].instruction_count = 1;
    const auto duplicate =
            swift::x86::BuildDecodedIR(swift::x86::detail::FixtureNormalize(discovered),
                                       reused,
                                       {&source, 1},
                                       Scope::Function,
                                       layout);
    CHECK(duplicate.error == swift::x86::BuildError::InvalidInput &&
          duplicate.instruction_count == 1);

    // An invalid encoding discovered later must retain the actual preceding
    // MOV births, without adding a fabricated fallthrough to its failed block.
    const std::uint8_t truncated[]{0xb8, 1, 0, 0, 0, 0x48};
    const BlockWindow failure{0x7000, truncated};
    WorkCounts counts;
    const auto rejected = BuildIR({&failure, 1}, Scope::Block, layout, {}, &counts);
    CHECK(!rejected && rejected.instruction_count == 1 && rejected.decoded_bytes == 5 &&
          rejected.error_pc == 0x7005);
    CHECK(counts.nodes == 2 && counts.value_uses == 1 &&
          counts.blocks == 1);  // LoadImm, StoreUniform(U32 read view).
}

void BlockBackedgesSeparateThePrefixBeforeIRBirth() {
    // Five prefix instructions, ten in the loop, and five at the exit. The
    // first request decodes the backedge, but births only the prefix's IR.
    const std::uint8_t bytes[]{
            0xb8, 1,    0,    0, 0,    0xb9, 3, 0,    0,    0,    0xba, 2,    0,    0,    0,
            0xbb, 4,    0,    0, 0,    0xbe, 5, 0,    0,    0,    0x83, 0xc0, 1,    0x83, 0xc0,
            1,    0x83, 0xc0, 1, 0x83, 0xc0, 1, 0x83, 0xc0, 1,    0x83, 0xc0, 1,    0x83, 0xc0,
            1,    0x83, 0xc0, 1, 0x83, 0xe9, 1, 0x75, 0xe3, 0x90, 0x90, 0x90, 0x90, 0xc3};
    constexpr std::uint64_t start = 0x11000, leader = start + 25, exit = leader + 29;
    const BlockWindow prefix{start, bytes};
    const BlockWindow loop{leader, std::span{bytes}.subspan(25)};
    const BlockWindow tail{exit, std::span{bytes}.subspan(54)};
    for (const auto mode : {TargetMode::Known, TargetMode::Unknown}) {
        const TargetInfo target{mode, 0xa64, 0};
        const auto first = BuildIR({&prefix, 1}, Scope::Block, layout, target);
        CHECK(first && first.instruction_count == 5 && first.decoded_bytes == 25);
        CHECK(first.program.Blocks().size() == 1 &&
              first.program.Blocks()[0].terminal.kind == TerminalKind::Dispatch);
        CHECK(StoresImmediate(first.program, layout.pc_offset, leader));
        CHECK(std::none_of(first.program.Operations().begin(),
                           first.program.Operations().end(),
                           [](const auto& op) {
                               return op.opcode == OpCode::Add || op.opcode == OpCode::Sub ||
                                      op.opcode == OpCode::CondSet;
                           }));
        const auto body = BuildIR({&loop, 1}, Scope::Block, layout, target);
        const auto done = BuildIR({&tail, 1}, Scope::Block, layout, target);
        CHECK(body && body.instruction_count == 10 && body.decoded_bytes == 29);
        CHECK(HasImmediate(body.program, leader) && HasImmediate(body.program, exit));
        CHECK(done && done.instruction_count == 5 && done.decoded_bytes == 5);
        CHECK(first.instruction_count + body.instruction_count + done.instruction_count == 20);
    }
    const auto function = BuildIR({&prefix, 1}, Scope::Function, layout);
    CHECK(function && function.instruction_count == 15 && function.decoded_bytes == 54);

    const std::uint8_t jump[]{0xb8, 1, 0, 0, 0, 0x90, 0xeb, 0xfd};
    const BlockWindow direct{0x12000, jump};
    const auto cut = BuildIR({&direct, 1}, Scope::Block, layout);
    CHECK(cut && cut.instruction_count == 1 && cut.decoded_bytes == 5);
    CHECK(StoresImmediate(cut.program, layout.pc_offset, 0x12005));
    const std::uint8_t middle[]{0xb8, 1, 0, 0, 0, 0x90, 0xeb, 0xfa};
    const BlockWindow overlapping{0x12000, middle};
    const auto uncut = BuildIR({&overlapping, 1}, Scope::Block, layout);
    CHECK(uncut && uncut.instruction_count == 3 && uncut.decoded_bytes == sizeof(middle));
    CHECK(StoresImmediate(uncut.program, layout.pc_offset, 0x12002));

    // Without a successfully decoded terminal, retain the actual rejected prefix.
    const BlockWindow truncated{start, std::span{bytes}.first(53)};
    const auto rejected = BuildIR({&truncated, 1}, Scope::Block, layout);
    CHECK(!rejected && rejected.error == swift::x86::BuildError::Truncated);
    CHECK(rejected.instruction_count == 14 && rejected.decoded_bytes == 52 &&
          rejected.error_pc == start + 52);
}

void ShortDecodeStorageGrowsWithoutChangingSlices() {
    std::vector<std::uint8_t> bytes;
    for (unsigned value = 1; value <= 40; ++value)
        bytes.insert(bytes.end(), {0xb8, static_cast<std::uint8_t>(value), 0, 0, 0});
    bytes.insert(bytes.end(), {0x75, 0xf9});  // JNZ to the final MOV, after both growth points.
    const BlockWindow source{0x13000, bytes};
    const auto block = BuildIR({&source, 1}, Scope::Block, layout);
    CHECK(block && block.instruction_count == 39 && block.decoded_bytes == 195);
    CHECK(block.program.Blocks().size() == 1);
    CHECK(StoresImmediate(block.program, layout.pc_offset, source.pc + 195));
    CHECK(HasImmediate(block.program, 39) && !HasImmediate(block.program, 40));

    const std::uint8_t tail[]{0x89, 0xc3, 0x90};  // MOV EBX,EAX; NOP, in a separate window.
    const std::array<BlockWindow, 2> windows{{source, {0x14000, tail}}};
    const auto function = BuildIR(windows, Scope::Function, layout);
    CHECK(function && function.instruction_count == 43 &&
          function.decoded_bytes == bytes.size() + sizeof(tail));
    CHECK(function.program.Blocks().size() == 4 && HasImmediate(function.program, 40));
    const auto& second = function.program.Blocks()[1];
    const auto& load = function.program.Operations()[second.first_node];
    CHECK(load.opcode == OpCode::LoadUniform && load.type == Type::U32);
    CHECK(function.program.Operands(load).front().Payload() == layout.gprs_offset);
    CHECK(StoresImmediate(function.program, layout.pc_offset, 0x14003));
}

void AutomaticFunctionDiscoveryAndEntry() {
    using swift::x86::BuildFunctionIR;
    using swift::x86::DecodeFunction;
    // Both arms join at RET. The invalid byte at +5 is never fetched. The
    // join is discovered before the taken arm, so physical records are not PC sorted.
    const std::uint8_t diamond[]{0x74, 4, 0x90, 0xeb, 3, 0x06, 0x90, 0x90, 0xc3};
    const BlockWindow source{0x8000, diamond};
    const auto decoded = DecodeFunction({&source, 1}, source.pc);
    CHECK(decoded && decoded.blocks.size() == 4 && decoded.instructions.size() == 6);
    CHECK(decoded.decode_calls == 6 && decoded.decoded_bytes == 8 && decoded.entry_block == 0);
    CHECK(decoded.instructions[3].View().PC() == 0x8008 &&
          decoded.instructions[4].View().PC() == 0x8006);
    CHECK(std::none_of(decoded.instructions.begin(),
                       decoded.instructions.end(),
                       [](const auto& instruction) { return instruction.View().PC() == 0x8005; }));
    const auto built = BuildFunctionIR({&source, 1}, source.pc, layout);
    CHECK(built && built.instruction_count == 6 && built.decoded_bytes == 8);
    CHECK(built.program.Blocks().size() == 4 && built.program.EntryBlock().value == 0);
    CHECK(built.program.Blocks()[0].terminal.kind == TerminalKind::Branch);
    CHECK(built.program.Blocks()[0].terminal.first.value == 2 &&
          built.program.Blocks()[0].terminal.second.value == 1);

    // Start above a subsequently discovered lower-PC body. Entry identity must
    // survive sorted-PC slices and need not equal BlockId 0.
    const std::uint8_t reverse[]{0x90, 0x90, 0xc3, 0x06, 0xeb, 0xfa};
    const BlockWindow reversed{0x9000, reverse};
    const auto nonlowest = DecodeFunction({&reversed, 1}, 0x9004);
    CHECK(nonlowest && nonlowest.entry_block == 1 && nonlowest.instructions.size() == 4);
    CHECK(nonlowest.blocks[0].pc == 0x9000 && nonlowest.blocks[1].pc == 0x9004);
    CHECK(nonlowest.instructions[0].View().PC() == 0x9004 &&
          nonlowest.blocks[1].first_instruction == 0);
    const auto entry = BuildFunctionIR({&reversed, 1}, 0x9004, layout);
    CHECK(entry && entry.program.EntryBlock().value == 1);
    CHECK(entry.program.Blocks()[1].terminal.kind == TerminalKind::Jump &&
          entry.program.Blocks()[1].terminal.first.value == 0);
    const auto direct = swift::x86::BuildDecodedIR(nonlowest.instructions,
                                                   nonlowest.blocks,
                                                   {&reversed, 1},
                                                   Scope::Function,
                                                   layout,
                                                   {},
                                                   nullptr,
                                                   nonlowest.entry_block);
    CHECK(direct && direct.program.EntryBlock().value == 1);
    CHECK(direct.program.Operations().size() == entry.program.Operations().size() &&
          direct.program.OperandArena().size() == entry.program.OperandArena().size());
    for (std::size_t i = 0; i < direct.program.Operations().size(); ++i) {
        const auto& checked = direct.program.Operations()[i];
        const auto& formed = entry.program.Operations()[i];
        CHECK(checked.opcode == formed.opcode && checked.type == formed.type &&
              checked.Metadata() == formed.Metadata());
    }
    CHECK(swift::x86::BuildDecodedIR(nonlowest.instructions,
                                     nonlowest.blocks,
                                     {&reversed, 1},
                                     Scope::Function,
                                     layout,
                                     {},
                                     nullptr,
                                     2)
                  .error == swift::x86::BuildError::InvalidInput);

    // Backward JNZ discovers a new header inside its own already decoded run.
    // Splitting changes only slice bounds; all five records were decoded once.
    const std::uint8_t loop[]{0x90, 0x90, 0x90, 0x75, 0xfc, 0xc3};
    const BlockWindow looping{0xa000, loop};
    const auto split = DecodeFunction({&looping, 1}, looping.pc);
    CHECK(split && split.blocks.size() == 3 && split.instructions.size() == 5);
    CHECK(split.decode_calls == 5 && split.splits == 1 && split.decoded_bytes == sizeof(loop));
    CHECK(split.blocks[0].pc == 0xa000 && split.blocks[0].first_instruction == 0 &&
          split.blocks[0].instruction_count == 1);
    CHECK(split.blocks[1].pc == 0xa001 && split.blocks[1].first_instruction == 1 &&
          split.blocks[1].instruction_count == 3);
    const auto formed = BuildFunctionIR({&looping, 1}, looping.pc, layout);
    CHECK(formed && formed.program.Blocks().size() == 3);
    CHECK(formed.program.Blocks()[0].terminal.kind == TerminalKind::Jump &&
          formed.program.Blocks()[0].terminal.first.value == 1);
    CHECK(formed.program.Blocks()[1].terminal.kind == TerminalKind::Branch &&
          formed.program.Blocks()[1].terminal.first.value == 1);

    const auto capped = DecodeFunction({&looping, 1}, looping.pc, {3, 3});
    CHECK(capped && capped.instructions.size() == 3 && capped.decode_calls == 3 &&
          capped.decoded_bytes == 3);
    CHECK(capped.blocks.size() == 1 && capped.budget_exits == 1);
    const auto cap_ir = BuildFunctionIR({&looping, 1}, looping.pc, layout, {3, 3});
    CHECK(cap_ir && cap_ir.instruction_count == 3 && cap_ir.program.Blocks().size() == 1);
    CHECK(cap_ir.program.Blocks()[0].terminal.kind == TerminalKind::Dispatch);
    CHECK(StoresImmediate(cap_ir.program, layout.pc_offset, 0xa003));
    const auto no_split = DecodeFunction({&looping, 1}, looping.pc, {16, 1});
    CHECK(no_split && no_split.instructions.size() == 4 && no_split.decode_calls == 4);
    CHECK(no_split.blocks.size() == 1 && no_split.splits == 0 && no_split.budget_exits == 2);
    const auto exact_split = DecodeFunction({&looping, 1}, looping.pc, {4, 2});
    CHECK(exact_split && exact_split.blocks.size() == 2 && exact_split.splits == 1 &&
          exact_split.decode_calls == 4);
    CHECK(exact_split.budget_exits == 1);

    // The iterative frontier exceeds the local PMR arena and uses its upstream.
    // Returned records remain independently owned after that arena is destroyed.
    std::vector<std::uint8_t> chain;
    for (unsigned i = 0; i < 128; ++i) {
        chain.push_back(0xeb);
        chain.push_back(0);
    }
    chain.push_back(0xc3);
    const BlockWindow linked{0xb000, chain};
    const auto iterative = DecodeFunction({&linked, 1}, linked.pc, {129, 129});
    CHECK(iterative && iterative.blocks.size() == 129 && iterative.instructions.size() == 129);
    CHECK(iterative.decode_calls == 129 && iterative.budget_exits == 0);
    const auto iterative_ir = swift::x86::BuildDecodedIR(iterative.instructions,
                                                         iterative.blocks,
                                                         {&linked, 1},
                                                         Scope::Function,
                                                         layout,
                                                         {},
                                                         nullptr,
                                                         iterative.entry_block);
    CHECK(iterative_ir && iterative_ir.instruction_count == 129 &&
          iterative_ir.program.Blocks().size() == 129);
}

void AutomaticFunctionFailuresAndFrontiers() {
    using swift::x86::BuildFunctionIR;
    using swift::x86::DecodeError;
    using swift::x86::DecodeFunction;
    const std::uint8_t external[]{0xeb, 0x7f};
    const BlockWindow source{0xc000, external};
    const auto decoded = DecodeFunction({&source, 1}, source.pc);
    CHECK(decoded && decoded.blocks.size() == 1 && decoded.instructions.size() == 1 &&
          decoded.decode_calls == 1);
    const auto built = BuildFunctionIR({&source, 1}, source.pc, layout);
    CHECK(built && built.program.Blocks()[0].terminal.kind == TerminalKind::Dispatch);
    CHECK(StoresImmediate(built.program, layout.pc_offset, 0xc081));
    CHECK(DecodeFunction({&source, 1}, 0xc002).error == DecodeError::InvalidInput);
    CHECK(DecodeFunction({&source, 1}, source.pc, {0, 1}).error == DecodeError::InvalidInput);
    CHECK(DecodeFunction({&source, 1}, source.pc, {1, 0}).error == DecodeError::InvalidInput);
    const std::array<BlockWindow, 2> overlap{{source, {0xc001, std::span{external}.first(1)}}};
    CHECK(DecodeFunction(overlap, source.pc).error == DecodeError::InvalidInput);

    const std::uint8_t middle[]{0xb8, 1, 0, 0, 0, 0xeb, 0xfa};  // JMP enters MOV's immediate.
    const BlockWindow misaligned{0xd000, middle};
    const auto bad_target = DecodeFunction({&misaligned, 1}, misaligned.pc);
    CHECK(bad_target.error == DecodeError::OverlappingTarget && bad_target.error_pc == 0xd001);
    CHECK(bad_target.instructions.size() == 2 && bad_target.decode_calls == 2 &&
          bad_target.blocks.empty());
    const std::uint8_t future_middle[]{0x74, 3, 0xb8, 1, 0, 0, 0};
    const BlockWindow future{0xd000, future_middle};
    const auto future_target = DecodeFunction({&future, 1}, future.pc);
    CHECK(future_target.error == DecodeError::OverlappingTarget &&
          future_target.error_pc == 0xd005);
    CHECK(future_target.instructions.size() == 1 && future_target.decode_calls == 2);

    // The same invalid byte is ignored when unreachable, and fails before any
    // IR birth when a real direct edge reaches it.
    const std::uint8_t skip[]{0xeb, 1, 0x06, 0xc3}, reach[]{0xeb, 0, 0x06};
    const BlockWindow skipped{0xe000, skip}, reached{0xe000, reach};
    CHECK(DecodeFunction({&skipped, 1}, skipped.pc));
    const auto invalid = DecodeFunction({&reached, 1}, reached.pc);
    CHECK(!invalid && invalid.error_pc == 0xe002 && invalid.instructions.size() == 1 &&
          invalid.decode_calls == 2);
    WorkCounts counts{9, 9, 9, 9};
    const auto rejected = BuildFunctionIR({&reached, 1}, reached.pc, layout, {}, {}, &counts);
    CHECK(!rejected && rejected.instruction_count == 0 && rejected.decoded_bytes == 0 &&
          counts == WorkCounts{});
    const std::uint8_t truncated[]{0x48, 0xb8, 1};
    const BlockWindow unfinished{0xe000, truncated};
    const auto short_input = DecodeFunction({&unfinished, 1}, unfinished.pc);
    CHECK(!short_input && short_input.decode_calls == 1 && short_input.instructions.empty());

    // Flow and ISA support are separate: preserve CALL's actual record rather
    // than rejecting it in the discovery layer or exploring its callee.
    const std::uint8_t call[]{0xe8, 0, 0, 0, 0, 0x06};
    const BlockWindow helper{0xf000, call};
    const auto unknown = DecodeFunction({&helper, 1}, helper.pc);
    CHECK(unknown && unknown.instructions.size() == 1 &&
          unknown.instructions[0].View().Opcode() == swift::x86::Op::CALL);
    const auto emitted = BuildFunctionIR({&helper, 1}, helper.pc, layout);
    CHECK(emitted && emitted.instruction_count == 1 && emitted.decoded_bytes == 5);
    CHECK(emitted.program.Blocks()[0].terminal.kind == TerminalKind::Dispatch);
    CHECK(StoresImmediate(emitted.program, layout.pc_offset, 0xf005));

    // Formed records still undergo ISA/prefix admission, at the same original
    // instruction as externally supplied decoded records, after the same prefix.
    const std::uint8_t bad_lock[]{0x90, 0xf0, 0x48, 0x89, 0x00};
    const BlockWindow prefixed{0xf100, bad_lock};
    const auto records = DecodeFunction({&prefixed, 1}, prefixed.pc);
    CHECK(records && records.instructions.size() == 2);
    const auto formed_bad = BuildFunctionIR({&prefixed, 1}, prefixed.pc, layout);
    const auto checked_bad = swift::x86::BuildDecodedIR(
            records.instructions, records.blocks, {&prefixed, 1}, Scope::Function, layout);
    CHECK(!formed_bad && !checked_bad && formed_bad.error == swift::x86::BuildError::Unsupported &&
          formed_bad.error == checked_bad.error && formed_bad.error_pc == prefixed.pc + 1 &&
          formed_bad.error_pc == checked_bad.error_pc);
    CHECK(formed_bad.instruction_count == 1 && checked_bad.instruction_count == 1 &&
          formed_bad.decoded_bytes == 1 && checked_bad.decoded_bytes == 1);
}

void NarrowMovesAndStackControlAreConstructible() {
    // MOV AH,AL; ADD AX,BX; INC EAX; RET 8. These are supported forms, not
    // negative fixtures. The actual application run checks native execution.
    const std::uint8_t bytes[]{0x88, 0xc4, 0x66, 0x01, 0xd8, 0xff, 0xc0, 0xc2, 0x08, 0x00};
    const BlockWindow window{0x12000, bytes};
    const auto built = BuildIR({&window, 1}, Scope::Block, layout);
    CHECK(built && built.instruction_count == 4 && built.decoded_bytes == sizeof(bytes));
    bool high_byte{}, stack_load{}, stack_commit{};
    for (const auto& op : built.program.Operations()) {
        stack_load |= op.opcode == OpCode::LoadMemoryTSO && op.type == Type::U64;
        if (op.opcode != OpCode::StoreUniform) continue;
        const auto field = built.program.Operands(op).front();
        high_byte |= field.Payload() == layout.gprs_offset + 1 && field.TypeView() == Type::U8;
        stack_commit |= field.Payload() == layout.gprs_offset + 4 * layout.stride &&
                        field.TypeView() == Type::U64;
    }
    CHECK(high_byte && stack_load && stack_commit);
    CHECK(built.program.Blocks()[0].terminal.kind == TerminalKind::Dispatch);
}

void MixedGPRViewsKeepParentCoverage() {
    const std::uint8_t bytes[]{
            0x89, 0xc1,              // mov ecx,eax: low32 snapshot, high RAX remains unknown
            0x48, 0x89, 0xc2,        // mov rdx,rax: must fetch that still-unknown high half
            0x89, 0xc6,              // mov esi,eax: derive low32 without replacing full parent
            0x89, 0xc7,              // mov edi,eax: reuse that same view, not another normalizer
            0x49, 0x89, 0xc0,        // mov r8,rax
            0x88, 0xe3,              // mov bl,ah
            0x66, 0x41, 0x89, 0xc1,  // mov r9w,ax
            0x49, 0x89, 0xc2,        // mov r10,rax: same immutable full parent
            0x88, 0xcc,              // mov ah,cl: retain one pending byte over the whole parent
            0x89, 0xc1,              // mov ecx,eax: merge AH, then derive the new low32
            0x89, 0xc0,              // mov eax,eax: same Value now defines zero-high full RAX
            0x4d, 0x8b, 0x7d, 0x00,  // mov r15,[r13]: publish, clearing dirty but not coverage
            0x49, 0x89, 0xc4,        // mov r12,rax: widen zero-high parent, no state reload
            0x66, 0x41, 0x89, 0xc6,  // mov r14w,ax: cannot narrow the parent cache
            0x49, 0x89, 0xc6,        // mov r14,rax: retain that same full parent
    };
    const BlockWindow window{0x14000, bytes};
    const auto private_state = static_cast<std::uint64_t>(TargetFeature::PrivateGPRState);
    const auto built =
            BuildIR({&window, 1}, Scope::Block, layout, {TargetMode::Known, 0xa64, private_state});
    CHECK(built && built.instruction_count == 15 && built.decoded_bytes == sizeof(bytes));
    const auto& p = built.program;
    const auto rax = layout.gprs_offset;
    std::vector<NodeId> low_reads, full_reads;
    std::size_t fault = p.Operations().size(), ah_store = fault, full_store = fault,
                parent_merge = fault;
    for (std::size_t i = 0; i < p.Operations().size(); ++i) {
        const auto& op = p.Operations()[i];
        if (op.opcode == OpCode::LoadMemoryTSO) fault = i;
        if (op.opcode == OpCode::BitInsert) {
            CHECK(parent_merge == p.Operations().size());
            parent_merge = i;
        }
        if (op.opcode != OpCode::LoadUniform && op.opcode != OpCode::StoreUniform) continue;
        const auto field = p.Operands(op).front();
        if (op.opcode == OpCode::LoadUniform && field.Payload() == rax) {
            if (field.TypeView() == Type::U32) low_reads.push_back({static_cast<std::uint32_t>(i)});
            if (field.TypeView() == Type::U64)
                full_reads.push_back({static_cast<std::uint32_t>(i)});
        }
        if (op.opcode == OpCode::StoreUniform && field.Payload() == rax + 1) {
            CHECK(field.TypeView() == Type::U8);
            ah_store = i;
        }
        if (op.opcode == OpCode::StoreUniform && field.Payload() == rax) {
            CHECK(field.TypeView() == Type::U64 && full_store == p.Operations().size());
            full_store = i;
        }
    }
    CHECK(low_reads.size() == 1 && full_reads.size() == 1);
    CHECK(ah_store == p.Operations().size());
    CHECK(low_reads[0].value < full_reads[0].value && full_reads[0].value < parent_merge);
    CHECK(parent_merge < full_store && full_store < fault);
    const auto& merged = p.Operations()[parent_merge];
    const auto merge = p.Operands(merged);
    CHECK(merged.type == Type::U64 && merge.front().Payload() == full_reads[0].value);
    CHECK(CellAt(merge, 2).Payload() == 8 && CellAt(merge, 3).Payload() == 8);
    const auto inserted = CellAt(merge, 1);
    CHECK(p.Operations()[inserted.Payload()].type == Type::U8);
    const auto inserted_byte = p.Operands(NodeId{static_cast<std::uint32_t>(inserted.Payload())});
    CHECK(p.Operations()[inserted.Payload()].opcode == OpCode::BitExtract);
    CHECK(inserted_byte.front().Payload() == low_reads[0].value);
    CHECK(CellAt(inserted_byte, 1).Payload() == 0 && CellAt(inserted_byte, 2).Payload() == 8);
    const auto stored = [&](unsigned slot, Type field_type) {
        for (const auto& op : p.Operations()) {
            if (op.opcode != OpCode::StoreUniform) continue;
            const auto args = p.Operands(op);
            const auto field = args.front();
            if (field.Payload() == rax + slot * layout.stride && field.TypeView() == field_type)
                return CellAt(args, 1);
        }
        swift::ThrowError("missing mixed-width publication");
    };
    // These complete snapshots retain the initial high half across all narrow reads.
    for (const auto slot : {2u, 8u, 10u}) {
        const auto value = stored(slot, Type::U64);
        CHECK(value.Kind() == OperandKind::Value && value.Payload() == full_reads[0].value &&
              value.TypeView() == Type::U64);
    }
    const auto low = stored(6, Type::U64);
    CHECK(low.TypeView() == Type::U32 &&
          p.Operations()[low.Payload()].opcode == OpCode::ZeroExtend32);
    CHECK(p.Operands(NodeId{static_cast<std::uint32_t>(low.Payload())}).front().Payload() ==
          full_reads[0].value);
    CHECK(stored(7, Type::U64).Payload() == low.Payload());
    const auto high_byte = stored(3, Type::U8), word = stored(9, Type::U16);
    for (const auto value : {high_byte, word}) {
        const auto& op = p.Operations()[value.Payload()];
        const auto args = p.Operands(op);
        CHECK(op.opcode == OpCode::BitExtract && op.type == value.TypeView());
        CHECK(args.front().Payload() == full_reads[0].value);
        CHECK(CellAt(args, 1).Payload() == (value.TypeView() == Type::U8 ? 8 : 0));
        CHECK(CellAt(args, 2).Payload() == (value.TypeView() == Type::U8 ? 8 : 16));
    }
    // Self-writing EAX turns the merged parent's low32 into a zero-high whole write.
    const auto publication = CellAt(p.Operands(NodeId{static_cast<std::uint32_t>(full_store)}), 1);
    CHECK(publication.TypeView() == Type::U32);
    CHECK(p.Operations()[publication.Payload()].opcode == OpCode::ZeroExtend32);
    const auto low_after_merge =
            p.Operands(NodeId{static_cast<std::uint32_t>(publication.Payload())}).front();
    CHECK(low_after_merge.Payload() == parent_merge && low_after_merge.TypeView() == Type::U32);
    const auto full = stored(12, Type::U64), later = stored(14, Type::U64);
    CHECK(full.Payload() == later.Payload() && full.TypeView() == Type::U64);
    CHECK(p.Operations()[full.Payload()].opcode == OpCode::ZeroExtend64 && full.Payload() > fault);
    const auto input = p.Operands(NodeId{static_cast<std::uint32_t>(full.Payload())}).front();
    CHECK(input.Payload() == publication.Payload() && input.TypeView() == Type::U32);

    // The same real instruction sequence keeps the unbound/generic Uniform path.
    const auto generic =
            BuildIR({&window, 1}, Scope::Block, layout, {TargetMode::Unknown, 0, private_state});
    CHECK(generic && generic.instruction_count == built.instruction_count);
    CHECK(std::none_of(generic.program.Operations().begin(),
                       generic.program.Operations().end(),
                       [](const auto& op) {
                           return op.opcode == OpCode::ZeroExtend32 ||
                                  op.opcode == OpCode::BitExtract || op.opcode == OpCode::BitInsert;
                       }));
    for (const auto& op : generic.program.Operations()) {
        if (op.opcode != OpCode::StoreUniform) continue;
        const auto args = generic.program.Operands(op);
        if (args.front().Payload() != rax) continue;
        CHECK(args.front().TypeView() == Type::U64 && CellAt(args, 1).TypeView() == Type::U32);
    }
}

void PendingPartialWritesKeepWholeParents() {
    // clang-format off
    const std::uint8_t bytes[]{
        0xb8,0x78,0x56,0x34,0x12,          // mov eax,0x12345678
        0xb0,0x11,0xb0,0x22,               // mov al,0x11; mov al,0x22: replace the same field
        0x88,0xc3,                         // mov bl,al: exact pending read
        0x8d,0x08,                         // lea ecx,[rax]: direct address-cache read must merge AL
        0xb4,0x33,                         // mov ah,0x33
        0x66,0xb8,0x55,0x44,               // mov ax,0x4455: materialize the different AH field
        0x66,0x89,0xc2,                    // mov dx,ax: exact pending word read
        0x41,0x89,0xc0,                    // mov r8d,eax: materialize AX, retaining zero-high coverage
        0x41,0xb9,0x01,0xef,0xcd,0xab,     // mov r9d,0xabcdef01
        0x41,0xb1,0x66,                    // mov r9b,0x66
        0x41,0xb9,0x10,0x32,0x54,0x76,     // mov r9d,0x76543210: kill that partial without a merge
        0x4d,0x8b,0x7d,0x00,               // mov r15,[r13]: complete state before the real fault
        0x49,0x89,0xc3,                    // mov r11,rax: published EAX still defines high32 zero
        0xb9,0xfe,0x56,0x34,0x12,          // mov ecx,0x123456fe
        0xd0,0xe9,                         // shr cl,1: U32 result, pending field is only U8
        0x88,0xca,0x88,0xce,               // mov dl,cl; mov dh,cl: one lazy canonical byte
        0x44,0x8d,0x21,                    // lea r12d,[rcx]: materialize that byte over the U32 parent
        0x48,0xb9,0xfe,0x77,0x66,0x55,0x44,0x33,0x22,0x11, // mov rcx,0x11223344556677fe
        0xd0,0xe9,                         // shr cl,1: keep the wider pending source unnormalized
        0x44,0x8d,0x29,                    // lea r13d,[rcx]: merge U8, clear the uncanonical U32 view
        0x41,0x89,0xce,                    // mov r14d,ecx: must read the merged parent, not the shift alone
    };
    // clang-format on
    const BlockWindow window{0x16000, bytes};
    const auto private_state = static_cast<std::uint64_t>(TargetFeature::PrivateGPRState);
    const auto built =
            BuildIR({&window, 1}, Scope::Block, layout, {TargetMode::Known, 0xa64, private_state});
    CHECK(built && built.instruction_count == 23 && built.decoded_bytes == sizeof(bytes));
    const auto& p = built.program;
    const auto rax = layout.gprs_offset, r9 = rax + 9 * layout.stride;
    std::vector<NodeId> merges;
    auto fault = p.Operations().size(), rax_store = fault;
    for (std::size_t i = 0; i < p.Operations().size(); ++i) {
        const auto& op = p.Operations()[i];
        if (op.opcode == OpCode::BitInsert) {
            merges.push_back({static_cast<std::uint32_t>(i)});
        }
        if (op.opcode == OpCode::LoadMemoryTSO) fault = i;
        if (op.opcode != OpCode::StoreUniform) continue;
        const auto field = p.Operands(op).front();
        CHECK(!(field.Payload() == rax + 1 && field.TypeView() == Type::U8));
        if (field.Payload() == rax || field.Payload() == r9) CHECK(field.TypeView() == Type::U64);
        if (field.Payload() == rax) {
            CHECK(rax_store == p.Operations().size());
            rax_store = i;
        }
    }
    CHECK(merges.size() == 5 && merges[2].value < rax_store && rax_store < fault &&
          fault < merges[3].value);
    for (std::size_t i = 0; i < 4; ++i) CHECK(p.Operations()[merges[i].value].type == Type::U32);
    CHECK(p.Operations()[merges[4].value].type == Type::U64);
    const auto source = [&](unsigned slot, Type type, unsigned byte = 0) {
        for (const auto& op : p.Operations()) {
            if (op.opcode != OpCode::StoreUniform) continue;
            const auto args = p.Operands(op);
            if (args.front().Payload() == rax + slot * layout.stride + byte &&
                args.front().TypeView() == type)
                return CellAt(args, 1);
        }
        swift::ThrowError("missing partial-parent publication");
    };
    const auto literal = [&](CellView value, std::uint64_t bits) {
        CHECK(value.Kind() == OperandKind::Value);
        const auto& op = p.Operations()[value.Payload()];
        CHECK(op.opcode == OpCode::LoadImm && p.Operands(op).front().Payload() == bits);
    };
    const auto first = p.Operands(merges[0]), second = p.Operands(merges[1]),
               third = p.Operands(merges[2]);
    literal(first.front(), 0x12345678);
    literal(CellAt(first, 1), 0x22);
    CHECK(CellAt(first, 2).Payload() == 0 && CellAt(first, 3).Payload() == 8);
    CHECK(second.front().Payload() == merges[0].value);
    literal(CellAt(second, 1), 0x33);
    CHECK(CellAt(second, 2).Payload() == 8 && CellAt(second, 3).Payload() == 8);
    CHECK(third.front().Payload() == merges[1].value);
    literal(CellAt(third, 1), 0x4455);
    CHECK(CellAt(third, 2).Payload() == 0 && CellAt(third, 3).Payload() == 16);
    literal(source(3, Type::U8), 0x22);     // BL gets the latest AL, not the first pending write.
    literal(source(2, Type::U16), 0x4455);  // DX gets the exact pending AX value.
    CHECK(source(1, Type::U64).Payload() ==
          merges[0].value);  // ECX keeps the earlier LEA snapshot.
    CHECK(source(0, Type::U64).Payload() == merges[2].value &&
          source(0, Type::U64).TypeView() == Type::U32);
    CHECK(source(8, Type::U64).Payload() == merges[2].value);
    literal(source(9, Type::U64), 0x76543210);
    const auto full = source(11, Type::U64);
    CHECK(full.Payload() > fault && p.Operations()[full.Payload()].opcode == OpCode::ZeroExtend64);
    const auto widened = p.Operands(NodeId{static_cast<std::uint32_t>(full.Payload())}).front();
    CHECK(widened.Payload() == merges[2].value && widened.TypeView() == Type::U32);

    const auto cl = source(2, Type::U8);
    CHECK(cl.Payload() == source(2, Type::U8, 1).Payload());
    CHECK(p.Operations()[cl.Payload()].opcode == OpCode::BitExtract &&
          p.Operations()[cl.Payload()].type == Type::U8);
    const auto cut = p.Operands(NodeId{static_cast<std::uint32_t>(cl.Payload())});
    CHECK(CellAt(cut, 1).Payload() == 0 && CellAt(cut, 2).Payload() == 8);
    CHECK(p.Operations()[cut.front().Payload()].opcode == OpCode::LsrImm);
    CHECK(p.Operations()[cut.front().Payload()].type == Type::U32);
    const auto canonical_merge = p.Operands(merges[3]);
    literal(canonical_merge.front(), 0x123456fe);
    CHECK(CellAt(canonical_merge, 1).Payload() == cl.Payload());
    CHECK(CellAt(canonical_merge, 2).Payload() == 0 && CellAt(canonical_merge, 3).Payload() == 8);
    CHECK(source(12, Type::U64).Payload() == merges[3].value);

    const auto wide_merge = p.Operands(merges[4]);
    literal(wide_merge.front(), 0x11223344556677feULL);
    const auto shift = CellAt(wide_merge, 1);
    CHECK(p.Operations()[shift.Payload()].opcode == OpCode::LsrImm &&
          p.Operations()[shift.Payload()].type == Type::U32);
    CHECK(CellAt(wide_merge, 2).Payload() == 0 && CellAt(wide_merge, 3).Payload() == 8);
    for (const auto slot : {13u, 14u}) {
        const auto low = source(slot, Type::U64);
        CHECK(low.TypeView() == Type::U32 &&
              p.Operations()[low.Payload()].opcode == OpCode::ZeroExtend32);
        CHECK(p.Operands(NodeId{static_cast<std::uint32_t>(low.Payload())}).front().Payload() ==
              merges[4].value);
    }
}

void PackedSseAndLandingHints() {
    const std::array<std::uint8_t, 84> bytes{
            0xf3, 0x0f, 0x1e, 0xfa, 0xf3, 0x0f, 0x1e, 0xfb,  // ENDBR64/32, no guest IBT.
            0xf3, 0x0f, 0x1e, 0xc8,                          // RDSSPD eax, preserves the full RAX.
            0xf3, 0x48, 0x0f, 0x1e, 0xc8,                    // Actual glibc RDSSPQ rax.
            0xf3, 0x41, 0x0f, 0x1e, 0xc8,                    // RDSSPD r8d.
            0xf3, 0x49, 0x0f, 0x1e, 0xcf,                    // RDSSPQ r15.
            0x66, 0x0f, 0x6c, 0xc1, 0x66, 0x0f, 0x6d, 0xc1,  // PUNPCKLQDQ/HQDQ xmm0,xmm1.
            0x0f, 0x58, 0xc1, 0x66, 0x0f, 0x58, 0x00,        // ADDPS reg, ADDPD full m128.
            0x0f, 0x5c, 0xc1, 0x66, 0x0f, 0x5c, 0xc1, 0x0f, 0x59, 0xc1,
            0x66, 0x0f, 0x59, 0xc1, 0x0f, 0x5e, 0xc1, 0x66, 0x0f, 0x5e,
            0xc1, 0x66, 0x0f, 0x68, 0xc1, 0x66, 0x0f, 0x69, 0xc1,  // PUNPCKHBW/HWD.
            0x66, 0x0f, 0x6a, 0x08,                                // PUNPCKHDQ xmm1,full m128.
            0x66, 0x0f, 0xda, 0xec,                                // PMINUB xmm5,xmm4.
            0xe9, 0x00, 0x00, 0x00, 0x00};
    auto vector_layout = layout;
    vector_layout.fprs_offset = 0x400;
    const BlockWindow source{0x6000, bytes};
    const auto inspect = [&](const swift::x86::BuildResult& built) {
        CHECK(built && built.instruction_count == 21 && built.decoded_bytes == bytes.size());
        const auto& p = built.program;
        std::size_t zip{}, fp{}, hints{}, memory{}, minimum{};
        bool fault_pc{};
        constexpr std::array<OpCode, 8> arithmetic{OpCode::VecFAdd,
                                                   OpCode::VecFAdd,
                                                   OpCode::VecFSub,
                                                   OpCode::VecFSub,
                                                   OpCode::VecFMul,
                                                   OpCode::VecFMul,
                                                   OpCode::VecFDiv,
                                                   OpCode::VecFDiv};
        constexpr std::array<unsigned, 5> zip_bits{64, 64, 8, 16, 32}, zip_halves{0, 1, 1, 1, 1};
        for (std::size_t i = 0; i < p.Operations().size(); ++i) {
            const auto& op = p.Operations()[i];
            const auto args = p.Operands(NodeId{static_cast<std::uint32_t>(i)});
            if (op.opcode == OpCode::Nop) ++hints;
            if (op.opcode == OpCode::VecZip) {
                CHECK(zip < zip_bits.size() && op.type == Type::V128 && CellCount(args) == 4);
                CHECK(CellAt(args, 2).Payload() == zip_bits[zip] &&
                      CellAt(args, 3).Payload() == zip_halves[zip]);
                ++zip;
            }
            if (op.opcode == OpCode::VecMin) {
                CHECK(op.type == Type::V128 && CellCount(args) == 4 &&
                      op.Metadata() == OperationMetadata{});
                CHECK(CellAt(args, 2).Payload() == 8 && CellAt(args, 3).Payload() == 0);
                ++minimum;
            }
            if (op.opcode == OpCode::VecFAdd || op.opcode == OpCode::VecFSub ||
                op.opcode == OpCode::VecFMul || op.opcode == OpCode::VecFDiv) {
                CHECK(fp < arithmetic.size() && op.opcode == arithmetic[fp] &&
                      op.type == Type::V128);
                CHECK(op.Metadata() == OperationMetadata{} && CellCount(args) == 3);
                CHECK(CellAt(args, 2).Payload() == (fp % 2 ? 64u : 32u));
                ++fp;
            }
            if (op.opcode == OpCode::StoreUniform) {
                CHECK(args.front().Payload() !=
                      layout.gprs_offset);  // RDSSPD/Q did not rewrite/zero RAX.
                if (args.front().Payload() == layout.pc_offset) fault_pc = true;
            }
            if (op.opcode == OpCode::LoadMemoryTSO) {
                CHECK(fault_pc && op.type == Type::V128);
                ++memory;
            }
        }
        CHECK(hints == 6 && zip == 5 && fp == 8 && memory == 2 && minimum == 1);
        CHECK(StoresImmediate(p, layout.pc_offset, source.pc + 38));
        CHECK(StoresImmediate(p, layout.pc_offset, source.pc + 71));
    };
    swift::x86::DecodeTiming direct_timing;
    const auto direct =
            BuildIR({&source, 1}, Scope::Block, vector_layout, {}, nullptr, &direct_timing);
    inspect(direct);
    CHECK(direct_timing.calls == 21);
    swift::x86::DecodeTiming function_timing;
    const auto function = swift::x86::BuildFunctionIR(
            {&source, 1}, source.pc, vector_layout, {}, {}, nullptr, &function_timing);
    inspect(function);
    CHECK(function_timing.calls == 21);
    // Recognition does not swallow another instruction's failure or a partial hint.
    const std::array<std::uint8_t, 6> bad{0xf3, 0x0f, 0x1e, 0xfa, 0xf4, 0x90};
    const BlockWindow unsupported{0x7000, bad};
    CHECK(!BuildIR({&unsupported, 1}, Scope::Block, vector_layout).program);
    const BlockWindow truncated{0x7100, std::span{bad}.first(3)};
    CHECK(!BuildIR({&truncated, 1}, Scope::Block, vector_layout).program);
    const std::array<std::uint8_t, 5> bad_extension{0xf3, 0x4c, 0x0f, 0x1e, 0xc8};
    const BlockWindow extension{0x7200, bad_extension};
    CHECK(!BuildIR({&extension, 1}, Scope::Block, vector_layout).program);
    const std::array<std::uint8_t, 5> other_group{0xf3, 0x48, 0x0f, 0x1e, 0xd0};
    const BlockWindow other{0x7300, other_group};
    CHECK(!BuildIR({&other, 1}, Scope::Block, vector_layout).program);
    const BlockWindow partial{0x7400, std::span{other_group}.first(4)};
    CHECK(!BuildIR({&partial, 1}, Scope::Block, vector_layout).program);
}

void PackedByteAndComparisonsAndRelativeCall() {
    std::vector<std::uint8_t> bytes{0x66, 0x41, 0x0f, 0x67, 0xe8};  // PACKUSWB xmm5,xmm8.
    for (std::uint8_t condition = 0; condition < 8; ++condition)
        bytes.insert(bytes.end(), {0x44, 0x0f, 0xc2, 0xdb, condition});  // CMPPS xmm11,xmm3,imm8.
    bytes.insert(bytes.end(),
                 {0x66,
                  0x0f,
                  0xc2,
                  0xc1,
                  0x01,  // Existing CMPLTPD path.
                  0x66,
                  0x0f,
                  0x67,
                  0x00,  // PACKUSWB xmm0,full m128.
                  0xe9,
                  0x00,
                  0x00,
                  0x00,
                  0x00});
    auto vector_layout = layout;
    vector_layout.fprs_offset = 0x400;
    const BlockWindow source{0x8000, bytes};
    const auto built = BuildIR({&source, 1}, Scope::Block, vector_layout);
    CHECK(built);
    CHECK(built.instruction_count == 12 && built.decoded_bytes == bytes.size());
    // Legacy LT/LE/NLT/NLE predicates signal on quiet NaNs (IR bit 4).
    constexpr std::array<std::uint64_t, 8> relations{2, 17, 19, 8, 13, 30, 28, 7};
    std::size_t packs{}, compares{};
    for (const auto& op : built.program.Operations()) {
        const auto args = built.program.Operands(op);
        CHECK(op.opcode != OpCode::PublishFCmpFlags);
        if (op.opcode == OpCode::VecPack) {
            CHECK(op.type == Type::V128 && CellCount(args) == 4);
            CHECK(CellAt(args, 2).Payload() == 16 && CellAt(args, 3).Payload() == 1);
            ++packs;
        }
        if (op.opcode == OpCode::VecFCmpMask) {
            CHECK(compares < 9 && op.type == Type::V128 && CellCount(args) == 5);
            CHECK(CellAt(args, 2).Payload() == (compares < 8 ? 32u : 64u));
            CHECK(CellAt(args, 3).Payload() == (compares < 8 ? relations[compares] : 17));
            CHECK(CellAt(args, 4).Payload() == 0 && op.Metadata() == OperationMetadata{});
            ++compares;
        }
    }
    CHECK(packs == 2 && compares == 9);

    const std::array<std::uint8_t, 6> call_bytes{0x67, 0xe8, 0xe0, 0xff, 0xff, 0xff};
    const BlockWindow call{UINT64_C(0x100004000), call_bytes};
    const auto called = BuildIR({&call, 1}, Scope::Block, layout);
    CHECK(called);
    CHECK(called.instruction_count == 1 && called.decoded_bytes == call_bytes.size());
    CHECK(StoresImmediate(called.program, layout.pc_offset, call.pc + call_bytes.size() - 32));
    bool return_pc{};
    for (const auto& op : called.program.Operations()) {
        if (op.opcode != OpCode::StoreMemoryTSO) continue;
        const auto value = CellAt(called.program.Operands(op), 1);
        CHECK(value.TypeView() == Type::U64 && value.Kind() == OperandKind::Value);
        const auto& producer = called.program.Operations()[value.Payload()];
        return_pc = producer.opcode == OpCode::LoadImm &&
                    called.program.Operands(producer).front().Payload() == call.pc + 6;
    }
    CHECK(return_pc);
    const std::array<std::uint8_t, 4> addr32_memory{0x67, 0x48, 0x8b, 0x00};
    const BlockWindow wrapping_memory{0x9000, addr32_memory};
    CHECK(BuildIR({&wrapping_memory, 1}, Scope::Block, layout).program);
    const std::array<std::uint8_t, 3> addr32_indirect{0x67, 0xff, 0x10};
    const BlockWindow indirect{0x9100, addr32_indirect};
    CHECK(BuildIR({&indirect, 1}, Scope::Block, layout).program);
}

void RepeatStoreHasAZeroBypassAndARealFrontier() {
    const std::array<std::uint8_t, 4> bytes{
            0xf3, 0x48, 0xab, 0x90};  // REP STOSQ; next guest instruction.
    const BlockWindow source{0xa000, bytes};
    const auto features = static_cast<std::uint64_t>(TargetFeature::FaultLocations) |
                          static_cast<std::uint64_t>(TargetFeature::PrivateGPRState);
    const TargetInfo target{TargetMode::Known, 0xa64, features};
    const auto inspect = [&](const swift::x86::BuildResult& built, Scope scope) {
        CHECK(built && built.instruction_count == 1 && built.decoded_bytes == 3);
        const auto& p = built.program;
        CHECK(p.ScopeKind() == scope && p.Blocks().size() == 1 &&
              p.Blocks()[0].terminal.kind == TerminalKind::Dispatch);
        std::size_t guard = p.Operations().size(), store = guard, bind = guard, di = guard,
                    count = guard, df = guard;
        for (std::size_t i = 0; i < p.Operations().size(); ++i) {
            const auto& op = p.Operations()[i];
            const auto args = p.Operands(NodeId{static_cast<std::uint32_t>(i)});
            if (op.opcode == OpCode::NotGoto) guard = i;
            if (op.opcode == OpCode::StoreMemoryTSO) {
                CHECK(store == p.Operations().size());
                store = i;
            }
            if (op.opcode == OpCode::BindLabel) bind = i;
            if (op.opcode == OpCode::BitExtract && CellCount(args) == 3 &&
                CellAt(args, 1).Payload() == 10)
                df = i;
            if (op.opcode == OpCode::StoreUniform) {
                if (args.front().Payload() == layout.gprs_offset + 7 * layout.stride) di = i;
                if (args.front().Payload() == layout.gprs_offset + layout.stride) count = i;
            }
            CHECK(op.Metadata() == OperationMetadata{});
        }
        CHECK(guard < df && df < store && store < di && store < count && di < bind && count < bind);
        CHECK(bind + 1 ==
              p.Operations().size());  // No conditional-body SSA is read after the join.
        const auto count_input = p.Operands(NodeId{static_cast<std::uint32_t>(guard)}).front();
        CHECK(p.Operations()[count_input.Payload()].opcode == OpCode::LoadUniform);
        CHECK(p.Operands(NodeId{static_cast<std::uint32_t>(count_input.Payload())})
                      .front()
                      .Payload() == layout.gprs_offset + layout.stride);
        CHECK(p.Operands(NodeId{static_cast<std::uint32_t>(bind)}).front().Payload() == guard);
        const auto raw_flags = p.Operands(NodeId{static_cast<std::uint32_t>(df)}).front();
        CHECK(p.Operations()[raw_flags.Payload()].opcode == OpCode::GetFlags);
        CHECK(StoresImmediate(p, layout.pc_offset, source.pc + 3));
        CHECK(HasImmediate(p, 8) && HasImmediate(p, std::uint64_t{} - 8));
    };
    inspect(BuildIR({&source, 1}, Scope::Block, layout, target), Scope::Block);
    inspect(swift::x86::BuildFunctionIR({&source, 1}, source.pc, layout, {}, target),
            Scope::Function);
    _CodeInfo code{};
    code.codeOffset = source.pc;
    code.code = bytes.data();
    code.codeLen = bytes.size();
    code.dt = Decode64Bits;
    std::array<_DInst, 2> decoded{};
    unsigned used{};
    distorm_decompose(&code, decoded.data(), decoded.size(), &used);
    CHECK(used == 2);
    const swift::x86::DecodedBlock oversized{source.pc, 0, 2};
    CHECK(!swift::x86::BuildDecodedIR(swift::x86::detail::FixtureNormalize(decoded),
                                      {&oversized, 1},
                                      {&source, 1},
                                      Scope::Block,
                                      layout,
                                      target)
                   .program);
    const std::array<std::uint8_t, 3> addr32{0xf3, 0x67, 0xab};
    const BlockWindow wrapping_store{0xa100, addr32};
    CHECK(BuildIR({&wrapping_store, 1}, Scope::Block, layout, target).program);
}

void HalfPackedSingleMemoryKeepsTheOtherHalf() {
    const std::array<std::uint8_t, 27> bytes{
            0x0f, 0x28, 0xc1,        // MOVAPS xmm0,xmm1: pending full value.
            0x0f, 0x16, 0x00,        // MOVHPS xmm0,[rax].
            0x0f, 0x11, 0x40, 0x10,  // MOVUPS [rax+16],xmm0: reload merged state.
            0x0f, 0x12, 0x48, 0x08,  // MOVLPS xmm1,[rax+8].
            0x0f, 0x17, 0x40, 0x10,  // MOVHPS [rax+16],xmm0.
            0x0f, 0x13, 0x48, 0x18,  // MOVLPS [rax+24],xmm1.
            0xe9, 0x00, 0x00, 0x00, 0x00};
    auto vector_layout = layout;
    vector_layout.fprs_offset = 0x400;
    const TargetInfo target{TargetMode::Known,
                            0xa64,
                            static_cast<std::uint64_t>(TargetFeature::PrivateVectorState)};
    const BlockWindow source{0xb000, bytes};
    const auto built = BuildIR({&source, 1}, Scope::Block, vector_layout, target);
    CHECK(built);
    CHECK(built.instruction_count == 7 && built.decoded_bytes == bytes.size());
    const auto& p = built.program;
    std::size_t full_publication = p.Operations().size(), high_publication = full_publication,
                low_publication = full_publication;
    std::size_t loads{}, stores{}, merged{};
    for (std::size_t i = 0; i < p.Operations().size(); ++i) {
        const auto& op = p.Operations()[i];
        const auto args = p.Operands(NodeId{static_cast<std::uint32_t>(i)});
        if (op.opcode == OpCode::StoreUniform && args.front().TypeView() == Type::V128 &&
            args.front().Payload() == 0x400)
            full_publication = i;
        if (op.opcode == OpCode::StoreUniform && args.front().TypeView() == Type::U64) {
            if (args.front().Payload() == 0x408) high_publication = i;
            if (args.front().Payload() == 0x410) low_publication = i;
        }
        if (op.opcode == OpCode::LoadMemoryTSO) {
            CHECK(op.type == Type::U64);
            ++loads;
        }
        if (op.opcode != OpCode::StoreMemoryTSO) continue;
        ++stores;
        const auto value = CellAt(args, CellCount(args) - 1);
        const auto& producer = p.Operations()[value.Payload()];
        CHECK(producer.opcode == OpCode::LoadUniform);
        const auto field = p.Operands(producer).front();
        if (field.TypeView() == Type::V128) {
            CHECK(field.Payload() == 0x400 && value.Payload() > high_publication);
            ++merged;
        } else
            CHECK(field.TypeView() == Type::U64 &&
                  (field.Payload() == 0x408 || field.Payload() == 0x410));
    }
    CHECK(full_publication < high_publication && high_publication < low_publication);
    CHECK(loads == 2 && stores == 3 && merged == 1);
    const std::array<std::uint8_t, 3> register_high{0x0f, 0x16, 0xc1},
            register_low{0x0f, 0x12, 0xc1};
    const BlockWindow high{0xb100, register_high}, low{0xb200, register_low};
    const auto register_half = [&](const BlockWindow& window, bool high_to_low) {
        const auto result = BuildIR({&window, 1}, Scope::Block, vector_layout, target);
        CHECK(result);
        std::size_t copies{};
        for (const auto& op : result.program.Operations()) {
            CHECK(op.opcode != OpCode::LoadMemoryTSO && op.opcode != OpCode::StoreMemoryTSO);
            if (op.opcode != OpCode::VecZip) continue;
            const auto args = result.program.Operands(op);
            CHECK(op.type == Type::V128 && CellCount(args) == 4 && CellAt(args, 2).Payload() == 64);
            CHECK(CellAt(args, 3).Payload() == high_to_low);
            const auto first = CellAt(args, 0), second = CellAt(args, 1);
            const auto& left = result.program.Operations()[first.Payload()];
            const auto& right = result.program.Operations()[second.Payload()];
            CHECK(left.opcode == OpCode::LoadUniform && right.opcode == OpCode::LoadUniform);
            CHECK(result.program.Operands(left).front().Payload() ==
                  (high_to_low ? 0x410u : 0x400u));
            CHECK(result.program.Operands(right).front().Payload() ==
                  (high_to_low ? 0x400u : 0x410u));
            ++copies;
        }
        CHECK(copies == 1);
    };
    register_half(high, false);  // MOVLHPS xmm0,xmm1.
    register_half(low, true);    // MOVHLPS xmm0,xmm1.
}

void X87WindowsKeepTheirFirstServicePC() {
    const std::array<std::uint8_t, 15> bytes{0xd9,
                                             0xc9,
                                             0xd9,
                                             0xc9,  // FXCH ST1 twice.
                                             0xd9,
                                             0xe8,
                                             0xd9,
                                             0xfa,
                                             0xdd,
                                             0xd8,  // FLD1; FSQRT; FSTP ST0.
                                             0xd9,
                                             0xc9,
                                             0x90,
                                             0xd9,
                                             0xc9};  // A gap retains both ordinary exchanges.
    const BlockWindow source{0xb000, bytes};
    auto x87_layout = layout;
    x87_layout.x87_offset = 0x600;
    const TargetInfo target{
            TargetMode::Known, 0xa64, static_cast<std::uint64_t>(TargetFeature::FaultLocations)};
    const std::vector<std::uint64_t> expected{X87Encode(X87Command::ExchangeTwice, 1),
                                              X87Encode(X87Command::PushOnePop),
                                              X87Encode(X87Command::Exchange, 1),
                                              X87Encode(X87Command::Exchange, 1)};
    for (const auto scope : {Scope::Block, Scope::Function}) {
        const auto built = BuildIR({&source, 1}, scope, x87_layout, target);
        CHECK(built && built.instruction_count == 8 && built.decoded_bytes == bytes.size());
        CHECK(built.program.ScopeKind() == scope && built.program.Blocks().size() == 1);
        std::vector<std::uint64_t> commands, locations;
        NodeId state_node, zero_node;
        for (const auto& op : built.program.Operations()) {
            const auto cells = built.program.Operands(op);
            if (op.opcode == OpCode::FaultLocation) locations.push_back(cells.front().Payload());
            if (op.opcode != OpCode::X87Op) continue;
            CHECK(CellCount(cells) == 3 && CellAt(cells, 1).Kind() == OperandKind::Immediate);
            commands.push_back(CellAt(cells, 1).Payload());
            if (state_node) {
                CHECK(CellAt(cells, 0).Payload() == state_node.value &&
                      CellAt(cells, 2).Payload() == zero_node.value);
            } else {
                state_node = {static_cast<std::uint32_t>(CellAt(cells, 0).Payload())};
                zero_node = {static_cast<std::uint32_t>(CellAt(cells, 2).Payload())};
            }
            const auto& raw = built.program.Operations()[CellAt(cells, 2).Payload()];
            CHECK(raw.opcode == OpCode::LoadImm &&
                  built.program.Operands(raw).front().Payload() == 0);
        }
        CHECK(commands == expected);
        CHECK(locations == std::vector<std::uint64_t>({0xb000, 0xb004, 0xb00a, 0xb00d}));
    }
    const std::array<BlockWindow, 2> split{
            {{0xb000, std::span{bytes}.first(2)}, {0xb002, std::span{bytes}.subspan(2, 2)}}};
    const auto separated = BuildIR(split, Scope::Function, x87_layout, target);
    CHECK(separated && separated.instruction_count == 2 && separated.program.Blocks().size() == 2);
    std::size_t ordinary{};
    NodeId previous_state, previous_zero;
    for (const auto& op : separated.program.Operations())
        if (op.opcode == OpCode::X87Op) {
            const auto cells = separated.program.Operands(op);
            CHECK(CellAt(cells, 1).Payload() == X87Encode(X87Command::Exchange, 1));
            CHECK(CellAt(cells, 0).Payload() != previous_state.value &&
                  CellAt(cells, 2).Payload() != previous_zero.value);
            previous_state = {static_cast<std::uint32_t>(CellAt(cells, 0).Payload())};
            previous_zero = {static_cast<std::uint32_t>(CellAt(cells, 2).Payload())};
            ++ordinary;
        }
    CHECK(ordinary == 2);
}
}  // namespace

TEST_CASE("RealBytesAndControl", "[compile][x86-construction]") {
    CHECK_NOTHROW(RealBytesAndControl());
}

TEST_CASE("RejectedBytesCannotPublishAProgram", "[compile][x86-construction]") {
    CHECK_NOTHROW(RejectedBytesCannotPublishAProgram());
}

TEST_CASE("QualificationCountsTheActualAbandonedPrefix", "[compile][x86-construction]") {
    CHECK_NOTHROW(QualificationCountsTheActualAbandonedPrefix());
}

TEST_CASE("RexExtensionRequiresTheFinalPrefixPosition", "[compile][x86-construction]") {
    CHECK_NOTHROW(RexExtensionRequiresTheFinalPrefixPosition());
}

TEST_CASE("KnownTargetEmitsOneShiftedAddress", "[compile][x86-construction]") {
    CHECK_NOTHROW(KnownTargetEmitsOneShiftedAddress());
}

TEST_CASE("IntegerAluAndDeferredFlags", "[compile][x86-construction]") {
    CHECK_NOTHROW(IntegerAluAndDeferredFlags());
}

TEST_CASE("EveryConditionalBranchAndRealExits", "[compile][x86-construction]") {
    CHECK_NOTHROW(EveryConditionalBranchAndRealExits());
}

TEST_CASE("ExternalEntriesAndReadyFrontiers", "[compile][x86-construction]") {
    CHECK_NOTHROW(ExternalEntriesAndReadyFrontiers());
}

TEST_CASE("ActualDecodedBufferAndRejectedFrontier", "[compile][x86-construction]") {
    CHECK_NOTHROW(ActualDecodedBufferAndRejectedFrontier());
}

TEST_CASE("BlockBackedgesSeparateThePrefixBeforeIRBirth", "[compile][x86-construction]") {
    CHECK_NOTHROW(BlockBackedgesSeparateThePrefixBeforeIRBirth());
}

TEST_CASE("ShortDecodeStorageGrowsWithoutChangingSlices", "[compile][x86-construction]") {
    CHECK_NOTHROW(ShortDecodeStorageGrowsWithoutChangingSlices());
}

TEST_CASE("AutomaticFunctionDiscoveryAndEntry", "[compile][x86-construction]") {
    CHECK_NOTHROW(AutomaticFunctionDiscoveryAndEntry());
}

TEST_CASE("AutomaticFunctionFailuresAndFrontiers", "[compile][x86-construction]") {
    CHECK_NOTHROW(AutomaticFunctionFailuresAndFrontiers());
}

TEST_CASE("NarrowMovesAndStackControlAreConstructible", "[compile][x86-construction]") {
    CHECK_NOTHROW(NarrowMovesAndStackControlAreConstructible());
}

TEST_CASE("MixedGPRViewsKeepParentCoverage", "[compile][x86-construction]") {
    CHECK_NOTHROW(MixedGPRViewsKeepParentCoverage());
}

TEST_CASE("PendingPartialWritesKeepWholeParents", "[compile][x86-construction]") {
    CHECK_NOTHROW(PendingPartialWritesKeepWholeParents());
}

TEST_CASE("PackedSseAndLandingHints", "[compile][x86-construction]") {
    CHECK_NOTHROW(PackedSseAndLandingHints());
}

TEST_CASE("PackedByteAndComparisonsAndRelativeCall", "[compile][x86-construction]") {
    CHECK_NOTHROW(PackedByteAndComparisonsAndRelativeCall());
}

TEST_CASE("RepeatStoreHasAZeroBypassAndARealFrontier", "[compile][x86-construction]") {
    CHECK_NOTHROW(RepeatStoreHasAZeroBypassAndARealFrontier());
}

TEST_CASE("HalfPackedSingleMemoryKeepsTheOtherHalf", "[compile][x86-construction]") {
    CHECK_NOTHROW(HalfPackedSingleMemoryKeepsTheOtherHalf());
}

TEST_CASE("X87WindowsKeepTheirFirstServicePC", "[compile][x86-construction]") {
    CHECK_NOTHROW(X87WindowsKeepTheirFirstServicePC());
}

TEST_CASE("Guest event source boundaries", "[compile][x86-construction]") {
    using Point = std::pair<OpCode, std::uint64_t>;
    const auto point = [](OpCode op, std::uint64_t pc) { return std::vector<Point>{{op, pc}}; };
    const auto points = [](const auto& code, std::uint64_t pc, Scope scope, TargetMode mode) {
        const BlockWindow window{pc, code};
        const auto result =
                BuildIR({&window, 1},
                        scope,
                        layout,
                        {mode, 0, static_cast<std::uint64_t>(TargetFeature::GuestEvents)});
        REQUIRE(bool(result));
        std::vector<Point> actual;
        for (const auto& op : result.program.Operations())
            if (op.opcode == OpCode::GuestSafepoint || op.opcode == OpCode::GuestDispatchSafepoint)
                actual.emplace_back(op.opcode, result.program.Operands(op).front().Payload());
        return actual;
    };
    for (auto mode : {TargetMode::Known, TargetMode::Unknown, TargetMode::EarlyOff})
        for (auto scope : {Scope::Block, Scope::Function}) {
            const std::array<std::uint8_t, 2> forward{0xeb, 0x10}, loop{0xe2, 0xfe},
                    indirect{0xff, 0xd0}, indirect_jump{0xff, 0xe0};
            CHECK(points(forward, 0x1800, scope, mode).empty());
            CHECK(points(loop, 0x1800, scope, mode) == point(OpCode::GuestSafepoint, 0x1800));
            CHECK(points(indirect, 0x1800, scope, mode) ==
                  point(OpCode::GuestDispatchSafepoint, 0x1800));
            CHECK(points(indirect_jump, 0x1800, scope, mode) ==
                  point(OpCode::GuestDispatchSafepoint, 0x1800));
            const std::array<std::uint8_t, 1> nop{0x90}, ret{0xc3};
            CHECK(points(nop, 0x1fff, scope, mode) == point(OpCode::GuestSafepoint, 0x1fff));
            CHECK(points(ret, 0x1800, scope, mode) ==
                  point(OpCode::GuestDispatchSafepoint, 0x1800));
            CHECK(points(indirect, 0x1fff, scope, mode) == point(OpCode::GuestSafepoint, 0x1fff));
            CHECK(points(ret, 0x1fff, scope, mode) == point(OpCode::GuestSafepoint, 0x1fff));
            const std::vector<std::uint8_t> linear(257, 0x90);
            CHECK(points(linear, 0x1200, scope, mode) == point(OpCode::GuestSafepoint, 0x1300));
            auto budget_indirect = linear;
            budget_indirect.pop_back();
            budget_indirect.insert(budget_indirect.end(), indirect.begin(), indirect.end());
            CHECK(points(budget_indirect, 0x1200, scope, mode) ==
                  point(OpCode::GuestSafepoint, 0x1300));
            const std::array<std::uint8_t, 2> rep{0xf3, 0xaa};
            CHECK(points(rep, 0x1800, scope, mode) == point(OpCode::GuestSafepoint, 0x1800));
        }
}

TEST_CASE("Guest events stop LOOP before its counter effect", "[compile][x86-construction]") {
    const std::array<std::uint8_t, 2> bytes{0xe2, 0xfe};
    const auto decoded = swift::x86::DecodeOne(bytes, 0x1800);
    REQUIRE(bool(decoded));
    swift::runtime::x86::GuestState state;
    state.pc = 0x1800;
    state.gprs[swift::runtime::x86::RCX] = 9;
    const auto flags = state.packed_flags;
    swift::x86::DirectServices services;
    services.guest_events = [](void*) noexcept { return true; };
    const auto result = swift::x86::InterpretDecoded(decoded.instruction, bytes, state, services);
    CHECK(result.kind == swift::x86::StepKind::Service);
    CHECK(result.service == swift::runtime::ExitValue(swift::runtime::NativeExit::GuestEvents));
    CHECK(state.pc == 0x1800);
    CHECK(state.gprs[swift::runtime::x86::RCX] == 9);
    CHECK(state.packed_flags == flags);
}

TEST_CASE("Direct frontend preserves divide and alignment fault kinds",
          "[compile][x86-construction]") {
    const auto fault = [](auto bytes, swift::runtime::NativeExit cause) {
        const auto decoded = swift::x86::DecodeOne(bytes, 0x1800);
        REQUIRE(bool(decoded));
        swift::runtime::x86::GuestState state;
        state.pc = 0x1800;
        state.gprs[swift::runtime::x86::RAX] = 1;
        const auto result = swift::x86::InterpretDecoded(decoded.instruction, bytes, state, {});
        CHECK(result.kind == swift::x86::StepKind::Fault);
        CHECK(result.service == swift::runtime::ExitValue(cause));
        CHECK(state.pc == 0x1800);
    };
    fault(std::array<std::uint8_t, 2>{0xf7, 0xf1}, swift::runtime::NativeExit::DivideFault);
    fault(std::array<std::uint8_t, 3>{0x0f, 0x28, 0x00},
          swift::runtime::NativeExit::AlignmentFault);
}

TEST_CASE("x86 software exceptions preserve ISA fault and resume PCs",
          "[compile][x86-construction]") {
    using swift::runtime::GuestExceptionKind;
    for (const auto& bytes : {std::vector<std::uint8_t>{0x0f, 0x0b},
                              std::vector<std::uint8_t>{0xcc},
                              std::vector<std::uint8_t>{0x66, 0xcc}}) {
        const bool breakpoint = bytes.back() == 0xcc;
        constexpr std::uint64_t pc = 0x1800;
        const auto resume = pc + (breakpoint ? bytes.size() : 0);
        const auto service = swift::runtime::SemanticExceptionExit(
                breakpoint ? GuestExceptionKind::Breakpoint
                           : GuestExceptionKind::IllegalInstruction,
                breakpoint ? bytes.size() : 0);
        const auto decoded = swift::x86::DecodeOne(bytes, pc);
        REQUIRE(bool(decoded));
        CHECK(decoded.instruction.View().Size() == bytes.size());
        CHECK(decoded.instruction.View().UnitBoundary());
        for (auto scope : {Scope::Block, Scope::Function}) {
            const BlockWindow window{pc, bytes};
            const auto built = BuildIR({&window, 1}, scope, layout);
            REQUIRE(bool(built));
            CHECK(built.instruction_count == 1 && built.decoded_bytes == bytes.size());
            CHECK(StoresImmediate(built.program, layout.pc_offset, resume));
            const auto& terminal = built.program.Blocks()[0].terminal;
            CHECK(terminal.kind == TerminalKind::Return && terminal.HasValue());
            const auto& result = built.program.Operations()[terminal.value.value];
            CHECK(result.opcode == OpCode::LoadImm);
            CHECK(built.program.Operands(result).front().Payload() == service);
        }
        swift::runtime::x86::GuestState state;
        state.pc = pc;
        state.gprs[swift::runtime::x86::RAX] = 123;
        const auto flags = state.packed_flags;
        const auto step = swift::x86::InterpretDecoded(decoded.instruction, bytes, state, {});
        CHECK(step.kind == swift::x86::StepKind::Service && step.service == service);
        CHECK(step.pc == pc && step.next_pc == resume && state.pc == resume);
        CHECK(state.gprs[swift::runtime::x86::RAX] == 123 && state.packed_flags == flags);
    }
}
