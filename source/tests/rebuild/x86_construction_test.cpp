#include "x86_fixtures.h"
#include "runtime/externals/distorm/distorm.h"
#include "runtime/externals/distorm/mnemonics.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace swift::runtime::ir;
using namespace swift::tests::x86_ir;
using swift::x86::BuildIR;
using swift::x86::BlockWindow;

void Check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr,"x86 construction failed at %d: %s\n",line,expression);
        throw std::runtime_error("x86 construction contract failed");
    }
}
#define CHECK(x) Check(static_cast<bool>(x),#x,__LINE__)

std::size_t CellCount(CellRange cells) {
    return static_cast<std::size_t>(std::distance(cells.begin(), cells.end()));
}
// Explicitly linear, used only for small expected rows in this test binary.
CellView CellAt(CellRange cells, std::size_t index) {
    auto at = cells.begin();
    while (index && at != cells.end()) { --index; ++at; }
    CHECK(index == 0 && at != cells.end());
    return *at;
}


bool StoresImmediate(const Program& program, std::uint32_t offset, std::uint64_t bits) {
    for (const auto& op : program.Operations()) {
        if (op.opcode != OpCode::StoreUniform) continue;
        const auto args = program.Operands(op);
        if (CellCount(args)!=2 || CellAt(args, 0).Payload()!=offset || CellAt(args, 1).Kind()!=OperandKind::Value) continue;
        const auto& source=program.Operations()[CellAt(args, 1).Payload()];
        if (source.opcode==OpCode::LoadImm && CellAt(program.Operands(source), 0).Payload()==bits) return true;
    }
    return false;
}

bool HasImmediate(const Program& program, std::uint64_t bits) {
    for (std::size_t id=0;id<program.Operations().size();++id)
        for (const auto cell : program.Operands(NodeId{static_cast<std::uint32_t>(id)}))
            if (cell.Kind()==OperandKind::Immediate && cell.Payload()==bits) return true;
    return false;
}

void RealBytesAndControl() {
    for (const auto mode : {TargetMode::Known,TargetMode::EarlyOff,TargetMode::Unknown}) {
        const TargetInfo target{mode,0xa64,0x40};
        const auto result=BuildIR(windows,Scope::Function,layout,target);
        CHECK(result.program);
        CHECK(result.instruction_count==13);
        CHECK(result.decoded_bytes==entry.size()+middle.size()+return_body.size());
        const auto& p=result.program;
        CHECK(p.ScopeKind()==Scope::Function && p.Target()==target);
        CHECK(p.Blocks().size()==3);
        CHECK(p.Blocks()[0].terminal.kind==TerminalKind::Jump);
        CHECK(p.Blocks()[0].terminal.first.value==1);
        CHECK(!p.Blocks()[0].terminal.HasValue() && p.Blocks()[0].State()==BlockState::Closed);
        CHECK(p.Blocks()[1].terminal.kind==TerminalKind::Jump);
        CHECK(p.Blocks()[1].terminal.first.value==2);
        CHECK(p.Blocks()[2].terminal.kind==TerminalKind::Dispatch);
        CHECK(!p.Blocks()[2].terminal.HasValue() && p.Blocks()[2].State()==BlockState::Closed);
        CHECK(std::count_if(p.Operations().begin(),p.Operations().end(),[](const auto& op) {
            return op.opcode==OpCode::ZeroExtend32To64;
        })==0);
        CHECK(std::count_if(p.Operations().begin(),p.Operations().end(),[](const auto& op) {
            return op.opcode==OpCode::LoadMemoryTSO;
        })==3);
        CHECK(std::count_if(p.Operations().begin(),p.Operations().end(),[](const auto& op) {
            return op.opcode==OpCode::StoreMemoryTSO;
        })==2);
        // Each real memory access has published its fault PC before the access.
        for (std::size_t b=0;b<p.Blocks().size();++b) {
            bool pc_published=false;
            const auto& block=p.Blocks()[b];
            for (std::size_t i=block.first_node;i<block.first_node+block.node_count;++i) {
                const auto& op=p.Operations()[i];
                if (op.opcode==OpCode::StoreUniform && CellAt(p.Operands(op), 0).Payload()==layout.pc_offset)
                    pc_published=true;
                if (op.opcode==OpCode::LoadMemoryTSO || op.opcode==OpCode::StoreMemoryTSO) {
                    CHECK(pc_published);
                    pc_published=false;
                }
            }
        }
        CHECK(StoresImmediate(p,layout.pc_offset,0x1017)); // entry store
        CHECK(StoresImmediate(p,layout.pc_offset,0x1040)); // RIP-relative load
        CHECK(StoresImmediate(p,layout.pc_offset,0x1049)); // signed immediate store
        CHECK(StoresImmediate(p,layout.pc_offset,0x1081)); // indexed load
        CHECK(StoresImmediate(p,layout.pc_offset,0x1085)); // RET's stack load
        CHECK(HasImmediate(p,0x1057));
        CHECK(HasImmediate(p,std::numeric_limits<std::uint64_t>::max()));
        CHECK(HasImmediate(p,0x123456789abcdef0ULL));
        // RET commits RSP only after its actual stack load, and publishes
        // that same complete loaded value as PC after the stack update.
        const auto& last=p.Blocks().back();
        std::size_t ret_load=p.Operations().size(),sp_store=ret_load,pc_store=ret_load;
        for (std::size_t i=last.first_node;i<last.first_node+last.node_count;++i)
            if (p.Operations()[i].opcode==OpCode::LoadMemoryTSO) ret_load=i;
        CHECK(ret_load<p.Operations().size());
        CHECK(p.Operations()[ret_load].type==Type::U64);
        const auto stack_address=p.Operands(p.Operations()[ret_load]).front();
        CHECK(stack_address.Kind()==OperandKind::Value && stack_address.Payload()<ret_load);
        const auto& stack=p.Operations()[stack_address.Payload()];
        CHECK(stack.opcode==OpCode::LoadUniform);
        CHECK(p.Operands(stack).front().Payload()==layout.gprs_offset+4*layout.stride);
        for (std::size_t i=ret_load+1;i<last.first_node+last.node_count;++i) {
            const auto& op=p.Operations()[i];
            if (op.opcode!=OpCode::StoreUniform) continue;
            const auto cells=p.Operands(op);
            if (cells.front().Payload()==layout.gprs_offset+4*layout.stride) sp_store=i;
            if (cells.front().Payload()==layout.pc_offset && CellAt(cells,1).Kind()==OperandKind::Value &&
                CellAt(cells,1).Payload()==ret_load) pc_store=i;
        }
        CHECK(ret_load<sp_store && sp_store<pc_store && pc_store<p.Operations().size());
        const auto advanced=CellAt(p.Operands(p.Operations()[sp_store]),1);
        CHECK(advanced.Kind()==OperandKind::Value && advanced.Payload()<sp_store);
        const auto& add=p.Operations()[advanced.Payload()];
        CHECK(add.opcode==OpCode::Add && add.type==Type::U64);
        CHECK(add.Metadata()==OperationMetadata{});
        const auto add_cells=p.Operands(add);
        CHECK(CellCount(add_cells)==2);
        CHECK(add_cells.front().Kind()==OperandKind::Value && add_cells.front().Payload()==stack_address.Payload());
        CHECK(CellAt(add_cells,1).Kind()==OperandKind::Immediate && CellAt(add_cells,1).Payload()==8);
    }
    // The same JMP leaves a Block request through its actual external target.
    const auto block=BuildIR(std::span{windows}.first(1),Scope::Block,layout);
    CHECK(block.program && block.instruction_count==6 && block.decoded_bytes==entry.size());
    CHECK(block.program.Blocks()[0].terminal.kind==TerminalKind::Dispatch);
    CHECK(StoresImmediate(block.program,layout.pc_offset,0x1040));
}

void RejectedBytesCannotPublishAProgram() {
    const auto reject=[](std::span<const std::uint8_t> bytes) {
        const BlockWindow window{0x1000,bytes};
        CHECK(!BuildIR({&window,1},Scope::Block,layout).program);
    };
    reject({});
    const std::uint8_t truncated[]{0x48,0xb8,1};
    const std::uint8_t unsupported[]{0x0f,0x0b,0x90}; // UD2, followed by unfetched padding.
    const std::uint8_t address32[]{0x67,0x8b,0x00};
    const std::uint8_t fs[]{0x64,0x48,0x8b,0x00};
    const std::uint8_t lock[]{0xf0,0x48,0x89,0x00};
    const std::uint8_t far_ret[]{0xca,0x08,0x00}; // Far return is still outside this runtime.
    for (const auto bytes : {std::span{truncated},std::span{unsupported},std::span{address32}}) reject(bytes);
    reject(fs);reject(lock);reject(far_ret);
    CHECK(!BuildIR(windows,Scope::Block,layout).program);
    const std::array<BlockWindow,2> unsorted{{windows[1],windows[0]}};
    CHECK(!BuildIR(unsorted,Scope::Function,layout).program);
    const std::array<BlockWindow,2> duplicate{{windows[0],windows[0]}};
    CHECK(!BuildIR(duplicate,Scope::Function,layout).program);
}

void QualificationCountsTheActualAbandonedPrefix() {
    WorkCounts collected{9,9,9,9};
    const auto invalid=BuildIR({},Scope::Block,layout,{},&collected);
    CHECK(invalid.error==swift::x86::BuildError::InvalidInput && !invalid.program);
    CHECK(collected==WorkCounts{});
    const auto reject=[&](std::span<const std::uint8_t> bytes) {
        const BlockWindow window{0x4000,bytes};
        collected={9,9,9,9};
        auto result=BuildIR({&window,1},Scope::Block,layout,{},&collected);
        CHECK(result.error==swift::x86::BuildError::Unsupported && !result.program);
        return result;
    };
    const std::uint8_t ud2[]{0x0f,0x0b};
    const auto empty=reject(ud2);
    CHECK(empty.instruction_count==0 && empty.decoded_bytes==0 && empty.error_pc==0x4000);
    CHECK(collected.nodes==0 && collected.value_uses==0 && collected.operand_words==0);
    CHECK(collected.blocks==1); // The actual declared/open block still exists.
    const std::uint8_t prefix[]{0x48,0xb8,0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,0x0f,0x0b};
    const auto partial=reject(prefix);
    CHECK(partial.instruction_count==1 && partial.decoded_bytes==10 && partial.error_pc==0x400a);
    CHECK(collected.nodes==2 && collected.value_uses==1 && collected.operand_words==4 && collected.blocks==1);
    const std::uint8_t privileged[]{0x0f,0x22,0xc0}; // MOV CR0,RAX rejects after its GPR read.
    const auto born_before_rejection=reject(privileged);
    CHECK(born_before_rejection.instruction_count==0 && born_before_rejection.decoded_bytes==0);
    CHECK(born_before_rejection.error_pc==0x4000);
    CHECK(collected.nodes==1 && collected.value_uses==0 && collected.operand_words==1 && collected.blocks==1);

    const auto complete=BuildIR(windows,Scope::Function,layout,{},&collected);
    CHECK(complete);
    WorkCounts published{complete.program.Operations().size(),0,
                         complete.program.OperandArena().size(),complete.program.Blocks().size()};
    for (std::size_t id=0;id<complete.program.Operations().size();++id)
        for (const auto cell : complete.program.Operands(NodeId{static_cast<std::uint32_t>(id)}))
            published.value_uses+=cell.Kind()==OperandKind::Value;
    for (const auto& block : complete.program.Blocks()) published.value_uses+=block.terminal.HasValue();
    CHECK(collected==published);
}

void RexExtensionRequiresTheFinalPrefixPosition() {
    const auto check=[](std::span<const std::uint8_t> bytes,
                         std::initializer_list<unsigned> expected_reads, unsigned destination,
                         Type read_type=Type::U32) {
        const BlockWindow window{0x3000,bytes};
        const auto built=BuildIR({&window,1},Scope::Block,layout);
        CHECK(built && built.instruction_count==1 && built.decoded_bytes==bytes.size());
        std::uint32_t reads{},expected_mask{};
        std::size_t read_count{},writes{};
        for (const auto reg : expected_reads) expected_mask|=std::uint32_t{1}<<reg;
        for (const auto& op : built.program.Operations()) {
            if (op.opcode==OpCode::LoadUniform) {
                const auto field=built.program.Operands(op).front();
                CHECK(field.Kind()==OperandKind::Uniform && field.TypeView()==read_type);
                CHECK(field.Payload()>=layout.gprs_offset);
                const auto offset=field.Payload()-layout.gprs_offset;
                CHECK(offset%layout.stride==0 && offset/layout.stride<16);
                reads|=std::uint32_t{1}<<(offset/layout.stride);
                ++read_count;
            }
            if (op.opcode==OpCode::StoreUniform) {
                const auto field=built.program.Operands(op).front();
                if (field.Payload()==layout.pc_offset) continue;
                CHECK(field.Payload()==layout.gprs_offset+destination*layout.stride);
                CHECK(field.TypeView()==Type::U64);
                CHECK(CellAt(built.program.Operands(op),1).TypeView()==Type::U32);
                ++writes;
            }
        }
        CHECK(reads==expected_mask && read_count==expected_reads.size() && writes==1);
        CHECK(std::count_if(built.program.Operations().begin(),built.program.Operations().end(),
                           [](const auto& op) { return op.opcode==OpCode::ZeroExtend32To64; })==0);
    };
    const std::uint8_t ignored_b[]{0x41,0x2e,0x89,0xc0};
    const std::uint8_t ignored_r[]{0x44,0x2e,0x89,0xc0};
    const std::uint8_t active_b[]{0x2e,0x41,0x89,0xc0};
    const std::uint8_t replaced[]{0x41,0x2e,0x44,0x89,0xc0};
    const std::uint8_t all_ignored[]{0x41,0x44,0x2e,0x89,0xc0};
    const std::uint8_t memory[]{0x4b,0x2e,0x8b,0x04,0x08}; // Ignored W/X/B: mov eax,[rax+rcx].
    check(ignored_b,{0},0);
    check(ignored_r,{0},0);
    check(active_b,{0},8);
    check(replaced,{8},0);
    check(all_ignored,{0},0);
    check(memory,{0,1},0,Type::U64);

    // Exercise the original pure decoder as well: the shared extension field
    // must preserve valid VEX, and REX+VEX stays an invalid encoding.
    const auto vex=[](std::span<const std::uint8_t> bytes, bool valid) {
        _CodeInfo code{};
        code.code=bytes.data(); code.codeLen=static_cast<int>(bytes.size()); code.dt=Decode64Bits;
        _DInst instructions[15]{};
        unsigned used{};
        (void)distorm_decompose(&code,instructions,15,&used);
        CHECK(used!=0);
        const auto& instruction=instructions[0];
        if (valid) {
            CHECK(used==1);
            CHECK(instruction.flags!=FLAG_NOT_DECODABLE && instruction.opcode==I_VZEROUPPER);
            CHECK(instruction.size==bytes.size());
        } else {
            CHECK(instruction.flags==FLAG_NOT_DECODABLE && instruction.size==1);
            CHECK(instruction.imm.byte==0x40);
        }
    };
    const std::uint8_t vex2[]{0xc5,0xf8,0x77};
    const std::uint8_t vex3[]{0xc4,0xe1,0x78,0x77};
    const std::uint8_t rex_vex2[]{0x40,0xc5,0xf8,0x77};
    const std::uint8_t rex_vex3[]{0x40,0xc4,0xe1,0x78,0x77};
    vex(vex2,true); vex(vex3,true);
    vex(rex_vex2,false); vex(rex_vex3,false);
}

void KnownTargetEmitsOneShiftedAddress() {
    const std::uint8_t lea[]{0x48,0x8d,0x54,0x88,0xf0}; // lea rdx,[rax+rcx*4-16]
    const BlockWindow window{0x3000,lea};
    const auto feature=static_cast<std::uint64_t>(TargetFeature::ShiftedAddress);
    std::size_t generic_nodes{};
    for (const auto mode : {TargetMode::Unknown,TargetMode::EarlyOff,TargetMode::Known}) {
        const auto built=BuildIR({&window,1},Scope::Block,layout,{mode,0xa64,feature});
        CHECK(built && built.instruction_count==1);
        std::size_t shifted{},shifts{};
        for (const auto& op : built.program.Operations()) {
            shifts+=op.opcode==OpCode::LslImm;
            if (op.opcode!=OpCode::GetOperand) continue;
            const auto cells=built.program.Operands(op);
            CHECK(CellCount(cells)==3 && op.type==Type::U64);
            const auto expression=cells.front();
            CHECK(expression.Kind()==OperandKind::Expression);
            CHECK(expression.ExpressionOp()==OperandOp::AddShift);
            CHECK(expression.ShiftAmount()==2 && expression.Count()==2);
            CHECK(CellAt(cells,1).Kind()==OperandKind::Value);
            CHECK(CellAt(cells,2).Kind()==OperandKind::Value);
            ++shifted;
        }
        CHECK(HasImmediate(built.program,std::uint64_t(-16)));
        if (mode==TargetMode::Known) {
            CHECK(shifted==1 && shifts==0);
            CHECK(built.program.Operations().size()+1==generic_nodes);
        } else {
            CHECK(shifted==0 && shifts==1);
            if (!generic_nodes) generic_nodes=built.program.Operations().size();
            CHECK(built.program.Operations().size()==generic_nodes);
        }
    }
    const auto without=BuildIR({&window,1},Scope::Block,layout,{TargetMode::Known,0xa64,0});
    CHECK(without && without.program.Operations().size()==generic_nodes);

    // One actual address SSA can feed distinct faulting accesses; a changed
    // shift, index version or block still needs its own definition.
    const std::uint8_t accesses[]{
        0x4c,0x8b,0x04,0x88,               // mov r8,[rax+rcx*4]
        0x4c,0x8b,0x4c,0x88,0x08,          // mov r9,[rax+rcx*4+8]
        0x64,0x4c,0x8b,0x54,0x88,0x10,     // mov r10,fs:[rax+rcx*4+16]
        0x4c,0x8b,0x1c,0xc8,               // mov r11,[rax+rcx*8]
        0x4c,0x8b,0x64,0x88,0x10,          // mov r12,[rax+rcx*4+16]
        0x48,0xff,0xc1,                    // inc rcx: new captured index version
        0x4c,0x8b,0x6c,0x88,0x18,          // mov r13,[rax+rcx*4+24]
        0xe9,0xdb,0,0,0,                   // jmp 0x3500
    };
    const std::uint8_t next[]{0x4c,0x8b,0x74,0x88,0x20}; // mov r14,[rax+rcx*4+32]
    const std::array<BlockWindow,2> address_windows{{{0x3400,accesses},{0x3500,next}}};
    auto segmented_layout=layout;
    segmented_layout.fs_base_offset=layout.pc_offset+8;
    const auto features=feature|static_cast<std::uint64_t>(TargetFeature::PrivateGPRState);
    const auto reused=BuildIR(address_windows,Scope::Function,segmented_layout,{TargetMode::Known,0xa64,features});
    CHECK(reused && reused.instruction_count==9 && reused.decoded_bytes==sizeof(accesses)+sizeof(next));
    const auto& p=reused.program;
    std::vector<NodeId> addresses,loads,fs_reads;
    for (std::size_t i=0;i<p.Operations().size();++i) {
        const auto& op=p.Operations()[i];
        if (op.opcode==OpCode::GetOperand) {
            CHECK(op.type==Type::U64 && op.Metadata()==OperationMetadata{});
            addresses.push_back({static_cast<std::uint32_t>(i)});
        }
        if (op.opcode==OpCode::LoadMemoryTSO) loads.push_back({static_cast<std::uint32_t>(i)});
        if (op.opcode==OpCode::LoadUniform && p.Operands(op).front().Payload()==segmented_layout.fs_base_offset)
            fs_reads.push_back({static_cast<std::uint32_t>(i)});
    }
    CHECK(addresses.size()==5 && loads.size()==7 && fs_reads.size()==1);
    const auto base=[&](NodeId load) {
        const auto cells=p.Operands(load);
        if (cells.front().Kind()==OperandKind::Expression) {
            CHECK(CellCount(cells)==3 && cells.front().ExpressionOp()==OperandOp::Add);
            return CellAt(cells,1);
        }
        CHECK(CellCount(cells)==1);
        return cells.front();
    };
    CHECK(base(loads[0]).Payload()==addresses[0].value && base(loads[1]).Payload()==addresses[0].value);
    CHECK(CellAt(p.Operands(loads[1]),2).Payload()==8);
    const auto fs_base=base(loads[2]);
    const auto& fs_sum=p.Operations()[fs_base.Payload()];
    CHECK(fs_sum.opcode==OpCode::Add && fs_sum.type==Type::U64);
    CHECK(p.Operands(fs_sum).front().Payload()==addresses[0].value);
    CHECK(CellAt(p.Operands(fs_sum),1).Payload()==fs_reads[0].value);
    CHECK(CellAt(p.Operands(loads[2]),2).Payload()==16);
    CHECK(base(loads[3]).Payload()==addresses[1].value);
    CHECK(p.Operands(addresses[0]).front().ShiftAmount()==2 && p.Operands(addresses[1]).front().ShiftAmount()==3);
    CHECK(base(loads[4]).Payload()==addresses[2].value && CellAt(p.Operands(loads[4]),2).Payload()==16);
    const auto old_index=p.Operands(addresses[2]),new_index=p.Operands(addresses[3]);
    CHECK(old_index.front().Code()==new_index.front().Code());
    CHECK(CellAt(old_index,1).Payload()==CellAt(new_index,1).Payload());
    CHECK(CellAt(old_index,2).Payload()!=CellAt(new_index,2).Payload());
    CHECK(base(loads[5]).Payload()==addresses[3].value && CellAt(p.Operands(loads[5]),2).Payload()==24);
    CHECK(base(loads[6]).Payload()==addresses[4].value && CellAt(p.Operands(loads[6]),2).Payload()==32);
    CHECK(p.Operations()[addresses[3].value].block.value==0 && p.Operations()[addresses[4].value].block.value==1);
}

void IntegerAluAndDeferredFlags() {
    const auto one=[](std::initializer_list<std::uint8_t> bytes, OpCode expected, Type type,
                      bool stores_result, FlagProfile profile) {
        const BlockWindow window{0x5000,{bytes.begin(),bytes.size()}};
        auto built=BuildIR({&window,1},Scope::Block,layout);
        CHECK(built && built.instruction_count==1 && built.decoded_bytes==bytes.size());
        std::size_t writers{},results{},stores{};
        for (const auto& op : built.program.Operations()) {
            if (op.opcode==expected) {
                CHECK(op.type==type);
                CHECK((op.Metadata()==OperationMetadata{FlagBits(Flag::All),profile,false}));
                ++results;
            }
            writers+=op.Metadata().flag_outputs!=0;
            if (op.opcode==OpCode::StoreUniform && built.program.Operands(op).front().Payload()!=layout.pc_offset)
                ++stores;
        }
        CHECK(results==1 && writers==1 && stores==std::size_t(stores_result));
        return built;
    };
    for (const bool wide : {false,true}) {
        const auto type=wide?Type::U64:Type::U32;
        const auto run=[&](std::uint8_t opcode, OpCode expected, bool write, FlagProfile profile) {
            if (wide) (void)one({0x48,opcode,0xd8},expected,type,write,profile);
            else (void)one({opcode,0xd8},expected,type,write,profile);
        };
        run(0x01,OpCode::Add,true,FlagProfile::ArithmeticCanonicalCarry);
        run(0x29,OpCode::Sub,true,FlagProfile::ArithmeticCanonicalCarry);
        run(0x39,OpCode::Sub,false,FlagProfile::ArithmeticCanonicalCarry);
        run(0x21,OpCode::And,true,FlagProfile::LogicalZeroCVA);
        run(0x09,OpCode::Or,true,FlagProfile::LogicalZeroCVA);
        run(0x31,OpCode::Xor,true,FlagProfile::LogicalZeroCVA);
        run(0x85,OpCode::And,false,FlagProfile::LogicalZeroCVA);
        run(0xf7,OpCode::Neg,true,FlagProfile::ArithmeticCanonicalCarry);
    }
    const auto immediate=one({0x48,0x83,0xc0,0xff},OpCode::Add,Type::U64,true,
                             FlagProfile::ArithmeticCanonicalCarry);
    CHECK(HasImmediate(immediate.program,std::numeric_limits<std::uint64_t>::max()));
    const auto immediate32=one({0x83,0xc0,0xff},OpCode::Add,Type::U32,true,
                               FlagProfile::ArithmeticCanonicalCarry);
    CHECK(HasImmediate(immediate32.program,0xffffffffULL));

    const std::uint8_t overwrite[]{0x48,0x01,0xd8,0x48,0x83,0xe8,1,0x31,0xc0,0x74,2};
    const BlockWindow window{0x5000,overwrite};
    const auto built=BuildIR({&window,1},Scope::Block,layout);
    CHECK(built && built.instruction_count==4);
    std::size_t writers{};
    for (const auto& op : built.program.Operations()) {
        if (op.opcode==OpCode::Add || op.opcode==OpCode::Sub) CHECK(op.Metadata()==OperationMetadata{});
        if (op.Metadata().flag_outputs) {
            CHECK(op.opcode==OpCode::Xor && op.Metadata().profile==FlagProfile::LogicalZeroCVA);
            ++writers;
        }
        CHECK(op.opcode!=OpCode::SaveFlags);
    }
    CHECK(writers==1);
    const std::uint8_t fault[]{0x48,0x01,0xd8,0x48,0x8b,0x01,0x48,0x29,0xd8,0xc3};
    const BlockWindow fault_window{0x5000,fault};
    const auto observed=BuildIR({&fault_window,1},Scope::Block,layout);
    CHECK(observed && observed.instruction_count==4);
    writers=0;
    for (const auto& op : observed.program.Operations()) {
        if (op.opcode==OpCode::Add || op.opcode==OpCode::Sub) {
            // RET's internal RSP+8 is a pure address calculation.
            if (!op.Metadata().flag_outputs) continue;
            CHECK(op.Metadata().flag_outputs==FlagBits(Flag::All));
            ++writers;
        }
    }
    CHECK(writers==2); // The later SUB cannot discard flags visible at the MOV fault.
    for (const auto& bytes : {std::vector<std::uint8_t>{0x0f,0x0b}, // UD2
                             std::vector<std::uint8_t>{0xd9,0xee}, // x87 FLDZ
                             std::vector<std::uint8_t>{0x0f,0x58,0xc0}}) { // ADDPS floating arithmetic
        const BlockWindow unsupported{0x5000,bytes};
        CHECK(BuildIR({&unsupported,1},Scope::Block,layout).error==swift::x86::BuildError::Unsupported);
    }
}

std::uint64_t BlockPC(const Program& p, const BlockRecord& block) {
    for (std::size_t i=block.first_node;i<block.first_node+block.node_count;++i) {
        const auto& op=p.Operations()[i];
        if (op.opcode!=OpCode::StoreUniform) continue;
        const auto args=p.Operands(op);
        if (args.front().Payload()!=layout.pc_offset) continue;
        const auto& value=p.Operations()[CellAt(args,1).Payload()];
        CHECK(value.opcode==OpCode::LoadImm);
        return p.Operands(value).front().Payload();
    }
    throw std::runtime_error("block has no constant PC store");
}

void EveryConditionalBranchAndRealExits() {
    constexpr Condition conditions[]{Condition::VS,Condition::VC,Condition::CS,Condition::CC,
            Condition::EQ,Condition::NE,Condition::AL,Condition::AL,Condition::MI,Condition::PL,
            Condition::AL,Condition::AL,Condition::LT,Condition::GE,Condition::LE,Condition::GT};
    for (unsigned cc=0;cc<16;++cc) {
        const std::uint8_t bytes[]{static_cast<std::uint8_t>(0x70+cc),0x20};
        const BlockWindow window{0x6000,bytes};
        for (const auto scope : {Scope::Block,Scope::Function}) {
            const auto built=BuildIR({&window,1},scope,layout);
            CHECK(built && built.instruction_count==1 && built.decoded_bytes==2);
            const auto& p=built.program;
            const auto& terminal=p.Blocks()[0].terminal;
            NodeId predicate;
            if (scope==Scope::Block) {
                CHECK(p.Blocks().size()==1 && terminal.kind==TerminalKind::Dispatch);
                std::size_t selects{},pc_stores{};
                for (std::size_t id=0;id<p.Operations().size();++id) {
                    const auto& op=p.Operations()[id];
                    if (op.opcode==OpCode::Select) {
                        const auto args=p.Operands(op);
                        predicate={static_cast<std::uint32_t>(args.front().Payload())};
                        const auto yes=CellAt(args,1),no=CellAt(args,2);
                        CHECK(p.Operands(NodeId{static_cast<std::uint32_t>(yes.Payload())}).front().Payload()==0x6022);
                        CHECK(p.Operands(NodeId{static_cast<std::uint32_t>(no.Payload())}).front().Payload()==0x6002);
                        ++selects;
                    }
                    if (op.opcode==OpCode::StoreUniform && p.Operands(op).front().Payload()==layout.pc_offset) ++pc_stores;
                }
                CHECK(selects==1 && pc_stores==1);
            } else {
                CHECK(p.Blocks().size()==3 && terminal.kind==TerminalKind::Branch);
                CHECK(terminal.HasValue()); predicate=terminal.value;
                CHECK(BlockPC(p,p.Blocks()[terminal.first.value])==0x6022);
                CHECK(BlockPC(p,p.Blocks()[terminal.second.value])==0x6002);
                CHECK(p.Blocks()[terminal.first.value].terminal.kind==TerminalKind::Dispatch);
                CHECK(p.Blocks()[terminal.second.value].terminal.kind==TerminalKind::Dispatch);
                for (std::size_t i=p.Blocks()[0].first_node;i<p.Blocks()[0].first_node+p.Blocks()[0].node_count;++i)
                    CHECK(p.Operations()[i].opcode!=OpCode::StoreUniform);
            }
            CHECK(predicate);
            const auto& op=p.Operations()[predicate.value];
            const auto input=p.Operands(predicate).front();
            if (cc==6 || cc==7 || cc==10 || cc==11) {
                CHECK(op.opcode==((cc&1)?OpCode::TestNotFlags:OpCode::TestFlags));
                const auto flags=cc<8?(Flag::Carry|Flag::Zero):Flag::Parity;
                CHECK(input.Kind()==OperandKind::Flags && input.Payload()==FlagBits(flags));
            } else {
                CHECK(op.opcode==OpCode::CondSet && input.Kind()==OperandKind::Condition);
                CHECK(input.Payload()==static_cast<unsigned>(conditions[cc]));
            }
        }
    }
    const std::uint8_t head[]{0x74,0x1e},fallthrough[]{0x90},taken[]{0x90};
    const std::array<BlockWindow,3> internal{{{0x6000,head},{0x6002,fallthrough},{0x6020,taken}}};
    const auto built=BuildIR(internal,Scope::Function,layout);
    CHECK(built && built.program.Blocks().size()==3);
    CHECK(built.program.Blocks()[0].terminal.kind==TerminalKind::Branch);
    CHECK(built.program.Blocks()[0].terminal.first.value==2 && built.program.Blocks()[0].terminal.second.value==1);
}

void ActualDecodedBufferAndRejectedFrontier() {
    const std::uint8_t bytes[]{0x48,0x83,0xc0,0xff,0x48,0x39,0xc8,0x75,2};
    const BlockWindow source{0x7000,bytes};
    _CodeInfo code{};
    code.code=bytes; code.codeLen=sizeof(bytes); code.codeOffset=source.pc; code.dt=Decode64Bits;
    std::array<_DInst,3> decoded{};
    unsigned used{};
    (void)distorm_decompose(&code,decoded.data(),decoded.size(),&used);
    CHECK(used==decoded.size());
    swift::x86::DecodedBlock block{source.pc,0,used};
    const auto direct=swift::x86::BuildDecodedIR(decoded,{&block,1},{&source,1},Scope::Block,layout);
    const auto byte_path=BuildIR({&source,1},Scope::Block,layout);
    CHECK(direct && byte_path && direct.instruction_count==3 && direct.decoded_bytes==sizeof(bytes));
    CHECK(direct.program.Operations().size()==byte_path.program.Operations().size());
    for (std::size_t i=0;i<direct.program.Operations().size();++i) {
        const auto& a=direct.program.Operations()[i]; const auto& b=byte_path.program.Operations()[i];
        CHECK(a.opcode==b.opcode && a.type==b.type && a.Metadata()==b.Metadata());
        const auto x=direct.program.Operands(a),y=byte_path.program.Operands(b);
        CHECK(CellCount(x)==CellCount(y));
        auto at=x.begin(),bt=y.begin();
        for (;at!=x.end();++at,++bt) {
            const auto left=*at,right=*bt;
            CHECK(left.Kind()==right.Kind() && left.TypeView()==right.TypeView() &&
                  left.Payload()==right.Payload() && left.Count()==right.Count() && left.Code()==right.Code());
        }
    }
    auto invalid=decoded;
    ++invalid[0].addr;
    CHECK(swift::x86::BuildDecodedIR(invalid,{&block,1},{&source,1},Scope::Block,layout).error==swift::x86::BuildError::InvalidInput);
    auto bad_slice=block; bad_slice.first_instruction=1;
    CHECK(swift::x86::BuildDecodedIR(decoded,{&bad_slice,1},{&source,1},Scope::Block,layout).error==swift::x86::BuildError::InvalidInput);

    // Discovery may append the higher-PC body first and split into slices
    // later. Neither emission nor a Block request may reorder/copy that owner.
    const std::array<_DInst,3> discovered{{decoded[1],decoded[2],decoded[0]}};
    const std::array<swift::x86::DecodedBlock,2> slices{{{0x7000,2,1},{0x7004,0,2}}};
    const auto reordered=swift::x86::BuildDecodedIR(discovered,slices,{&source,1},Scope::Function,layout);
    CHECK(reordered && reordered.instruction_count==3 && reordered.decoded_bytes==sizeof(bytes));
    CHECK(reordered.program.Operations()[0].opcode==OpCode::LoadUniform);
    CHECK(reordered.program.Blocks()[0].terminal.kind==TerminalKind::Jump &&
          reordered.program.Blocks()[0].terminal.first.value==1);
    CHECK(reordered.program.Blocks()[1].terminal.kind==TerminalKind::Branch);
    WorkCounts selected_counts;
    const auto selected=swift::x86::BuildDecodedIR(discovered,std::span{slices}.subspan(1),{&source,1},
                                                   Scope::Block,layout,{},&selected_counts);
    CHECK(selected && selected.instruction_count==2 && selected.decoded_bytes==5);
    CHECK(selected.program.Blocks().size()==1 && selected_counts.blocks==1);
    CHECK(std::none_of(selected.program.Operations().begin(),selected.program.Operations().end(),
                       [](const auto& op) { return op.opcode==OpCode::Add; }));
    const auto tail=swift::x86::DecodedBlock{0x7007,1,1};
    const auto selected_tail=swift::x86::BuildDecodedIR(discovered,{&tail,1},{&source,1},Scope::Block,layout);
    CHECK(selected_tail && selected_tail.instruction_count==1 && selected_tail.decoded_bytes==2);
    auto overlapping=slices;overlapping[1].pc=0x7002;
    CHECK(swift::x86::BuildDecodedIR(discovered,overlapping,{&source,1},Scope::Function,layout).error==swift::x86::BuildError::InvalidInput);
    auto reused=slices;reused[1].first_instruction=2;reused[1].instruction_count=1;
    const auto duplicate=swift::x86::BuildDecodedIR(discovered,reused,{&source,1},Scope::Function,layout);
    CHECK(duplicate.error==swift::x86::BuildError::InvalidInput && duplicate.instruction_count==1);

    // An invalid encoding discovered later must retain the actual preceding
    // MOV births, without adding a fabricated fallthrough to its failed block.
    const std::uint8_t truncated[]{0xb8,1,0,0,0,0x48};
    const BlockWindow failure{0x7000,truncated};
    WorkCounts counts;
    const auto rejected=BuildIR({&failure,1},Scope::Block,layout,{},&counts);
    CHECK(!rejected && rejected.instruction_count==1 && rejected.decoded_bytes==5 && rejected.error_pc==0x7005);
    CHECK(counts.nodes==2 && counts.value_uses==1 && counts.blocks==1); // LoadImm, StoreUniform(U32 read view).
}

void AutomaticFunctionDiscoveryAndEntry() {
    using swift::x86::DecodeFunction;
    using swift::x86::BuildFunctionIR;
    // Both arms join at RET. The invalid byte at +5 is never fetched. The
    // join is discovered before the taken arm, so physical records are not PC sorted.
    const std::uint8_t diamond[]{0x74,4,0x90,0xeb,3,0x06,0x90,0x90,0xc3};
    const BlockWindow source{0x8000,diamond};
    const auto decoded=DecodeFunction({&source,1},source.pc);
    CHECK(decoded && decoded.blocks.size()==4 && decoded.instructions.size()==6);
    CHECK(decoded.decode_calls==6 && decoded.decoded_bytes==8 && decoded.entry_block==0);
    CHECK(decoded.instructions[3].addr==0x8008 && decoded.instructions[4].addr==0x8006);
    CHECK(std::none_of(decoded.instructions.begin(),decoded.instructions.end(),
                       [](const auto& instruction) { return instruction.addr==0x8005; }));
    const auto built=BuildFunctionIR({&source,1},source.pc,layout);
    CHECK(built && built.instruction_count==6 && built.decoded_bytes==8);
    CHECK(built.program.Blocks().size()==4 && built.program.EntryBlock().value==0);
    CHECK(built.program.Blocks()[0].terminal.kind==TerminalKind::Branch);
    CHECK(built.program.Blocks()[0].terminal.first.value==2 && built.program.Blocks()[0].terminal.second.value==1);

    // Start above a subsequently discovered lower-PC body. Entry identity must
    // survive sorted-PC slices and need not equal BlockId 0.
    const std::uint8_t reverse[]{0x90,0x90,0xc3,0x06,0xeb,0xfa};
    const BlockWindow reversed{0x9000,reverse};
    const auto nonlowest=DecodeFunction({&reversed,1},0x9004);
    CHECK(nonlowest && nonlowest.entry_block==1 && nonlowest.instructions.size()==4);
    CHECK(nonlowest.blocks[0].pc==0x9000 && nonlowest.blocks[1].pc==0x9004);
    CHECK(nonlowest.instructions[0].addr==0x9004 && nonlowest.blocks[1].first_instruction==0);
    const auto entry=BuildFunctionIR({&reversed,1},0x9004,layout);
    CHECK(entry && entry.program.EntryBlock().value==1);
    CHECK(entry.program.Blocks()[1].terminal.kind==TerminalKind::Jump && entry.program.Blocks()[1].terminal.first.value==0);
    const auto direct=swift::x86::BuildDecodedIR(nonlowest.instructions,nonlowest.blocks,{&reversed,1},
                        Scope::Function,layout,{},nullptr,nonlowest.entry_block);
    CHECK(direct && direct.program.EntryBlock().value==1);
    CHECK(swift::x86::BuildDecodedIR(nonlowest.instructions,nonlowest.blocks,{&reversed,1},
                        Scope::Function,layout,{},nullptr,2).error==swift::x86::BuildError::InvalidInput);

    // Backward JNZ discovers a new header inside its own already decoded run.
    // Splitting changes only slice bounds; all five records were decoded once.
    const std::uint8_t loop[]{0x90,0x90,0x90,0x75,0xfc,0xc3};
    const BlockWindow looping{0xa000,loop};
    const auto split=DecodeFunction({&looping,1},looping.pc);
    CHECK(split && split.blocks.size()==3 && split.instructions.size()==5);
    CHECK(split.decode_calls==5 && split.splits==1 && split.decoded_bytes==sizeof(loop));
    CHECK(split.blocks[0].pc==0xa000 && split.blocks[0].first_instruction==0 && split.blocks[0].instruction_count==1);
    CHECK(split.blocks[1].pc==0xa001 && split.blocks[1].first_instruction==1 && split.blocks[1].instruction_count==3);
    const auto formed=BuildFunctionIR({&looping,1},looping.pc,layout);
    CHECK(formed && formed.program.Blocks().size()==3);
    CHECK(formed.program.Blocks()[0].terminal.kind==TerminalKind::Jump && formed.program.Blocks()[0].terminal.first.value==1);
    CHECK(formed.program.Blocks()[1].terminal.kind==TerminalKind::Branch && formed.program.Blocks()[1].terminal.first.value==1);

    const auto capped=DecodeFunction({&looping,1},looping.pc,{3,3});
    CHECK(capped && capped.instructions.size()==3 && capped.decode_calls==3 && capped.decoded_bytes==3);
    CHECK(capped.blocks.size()==1 && capped.budget_exits==1);
    const auto cap_ir=BuildFunctionIR({&looping,1},looping.pc,layout,{3,3});
    CHECK(cap_ir && cap_ir.instruction_count==3 && cap_ir.program.Blocks().size()==1);
    CHECK(cap_ir.program.Blocks()[0].terminal.kind==TerminalKind::Dispatch);
    CHECK(StoresImmediate(cap_ir.program,layout.pc_offset,0xa003));
    const auto no_split=DecodeFunction({&looping,1},looping.pc,{16,1});
    CHECK(no_split && no_split.instructions.size()==4 && no_split.decode_calls==4);
    CHECK(no_split.blocks.size()==1 && no_split.splits==0 && no_split.budget_exits==2);
    const auto exact_split=DecodeFunction({&looping,1},looping.pc,{4,2});
    CHECK(exact_split && exact_split.blocks.size()==2 && exact_split.splits==1 && exact_split.decode_calls==4);
    CHECK(exact_split.budget_exits==1);

    // A long chain is processed by the actual iterative frontier, not the C++ stack.
    std::vector<std::uint8_t> chain;
    for (unsigned i=0;i<128;++i) { chain.push_back(0xeb);chain.push_back(0); }
    chain.push_back(0xc3);
    const BlockWindow linked{0xb000,chain};
    const auto iterative=DecodeFunction({&linked,1},linked.pc,{129,129});
    CHECK(iterative && iterative.blocks.size()==129 && iterative.instructions.size()==129);
    CHECK(iterative.decode_calls==129 && iterative.budget_exits==0);
}

void AutomaticFunctionFailuresAndFrontiers() {
    using swift::x86::DecodeFunction;
    using swift::x86::BuildFunctionIR;
    using swift::x86::DecodeError;
    const std::uint8_t external[]{0xeb,0x7f};
    const BlockWindow source{0xc000,external};
    const auto decoded=DecodeFunction({&source,1},source.pc);
    CHECK(decoded && decoded.blocks.size()==1 && decoded.instructions.size()==1 && decoded.decode_calls==1);
    const auto built=BuildFunctionIR({&source,1},source.pc,layout);
    CHECK(built && built.program.Blocks()[0].terminal.kind==TerminalKind::Dispatch);
    CHECK(StoresImmediate(built.program,layout.pc_offset,0xc081));
    CHECK(DecodeFunction({&source,1},0xc002).error==DecodeError::InvalidInput);
    CHECK(DecodeFunction({&source,1},source.pc,{0,1}).error==DecodeError::InvalidInput);
    CHECK(DecodeFunction({&source,1},source.pc,{1,0}).error==DecodeError::InvalidInput);
    const std::array<BlockWindow,2> overlap{{source,{0xc001,std::span{external}.first(1)}}};
    CHECK(DecodeFunction(overlap,source.pc).error==DecodeError::InvalidInput);

    const std::uint8_t middle[]{0xb8,1,0,0,0,0xeb,0xfa}; // JMP enters MOV's immediate.
    const BlockWindow misaligned{0xd000,middle};
    const auto bad_target=DecodeFunction({&misaligned,1},misaligned.pc);
    CHECK(bad_target.error==DecodeError::OverlappingTarget && bad_target.error_pc==0xd001);
    CHECK(bad_target.instructions.size()==2 && bad_target.decode_calls==2 && bad_target.blocks.empty());
    const std::uint8_t future_middle[]{0x74,3,0xb8,1,0,0,0};
    const BlockWindow future{0xd000,future_middle};
    const auto future_target=DecodeFunction({&future,1},future.pc);
    CHECK(future_target.error==DecodeError::OverlappingTarget && future_target.error_pc==0xd005);
    CHECK(future_target.instructions.size()==1 && future_target.decode_calls==2);

    // The same invalid byte is ignored when unreachable, and fails before any
    // IR birth when a real direct edge reaches it.
    const std::uint8_t skip[]{0xeb,1,0x06,0xc3},reach[]{0xeb,0,0x06};
    const BlockWindow skipped{0xe000,skip},reached{0xe000,reach};
    CHECK(DecodeFunction({&skipped,1},skipped.pc));
    const auto invalid=DecodeFunction({&reached,1},reached.pc);
    CHECK(!invalid && invalid.error_pc==0xe002 && invalid.instructions.size()==1 && invalid.decode_calls==2);
    WorkCounts counts{9,9,9,9};
    const auto rejected=BuildFunctionIR({&reached,1},reached.pc,layout,{}, {},&counts);
    CHECK(!rejected && rejected.instruction_count==0 && rejected.decoded_bytes==0 && counts==WorkCounts{});
    const std::uint8_t truncated[]{0x48,0xb8,1};
    const BlockWindow unfinished{0xe000,truncated};
    const auto short_input=DecodeFunction({&unfinished,1},unfinished.pc);
    CHECK(!short_input && short_input.decode_calls==1 && short_input.instructions.empty());

    // Flow and ISA support are separate: preserve CALL's actual record rather
    // than rejecting it in the discovery layer or exploring its callee.
    const std::uint8_t call[]{0xe8,0,0,0,0,0x06};
    const BlockWindow helper{0xf000,call};
    const auto unknown=DecodeFunction({&helper,1},helper.pc);
    CHECK(unknown && unknown.instructions.size()==1 && unknown.instructions[0].opcode==I_CALL);
    const auto emitted=BuildFunctionIR({&helper,1},helper.pc,layout);
    CHECK(emitted && emitted.instruction_count==1 && emitted.decoded_bytes==5);
    CHECK(emitted.program.Blocks()[0].terminal.kind==TerminalKind::Dispatch);
    CHECK(StoresImmediate(emitted.program,layout.pc_offset,0xf005));
}

void NarrowMovesAndStackControlAreConstructible() {
    // MOV AH,AL; ADD AX,BX; INC EAX; RET 8. These are supported forms, not
    // negative fixtures. The actual application run checks native execution.
    const std::uint8_t bytes[]{0x88,0xc4,0x66,0x01,0xd8,0xff,0xc0,0xc2,0x08,0x00};
    const BlockWindow window{0x12000,bytes};
    const auto built=BuildIR({&window,1},Scope::Block,layout);
    CHECK(built && built.instruction_count==4 && built.decoded_bytes==sizeof(bytes));
    bool high_byte{}, stack_load{}, stack_commit{};
    for (const auto& op : built.program.Operations()) {
        stack_load|=op.opcode==OpCode::LoadMemoryTSO && op.type==Type::U64;
        if (op.opcode!=OpCode::StoreUniform) continue;
        const auto field=built.program.Operands(op).front();
        high_byte|=field.Payload()==layout.gprs_offset+1 && field.TypeView()==Type::U8;
        stack_commit|=field.Payload()==layout.gprs_offset+4*layout.stride && field.TypeView()==Type::U64;
    }
    CHECK(high_byte && stack_load && stack_commit);
    CHECK(built.program.Blocks()[0].terminal.kind==TerminalKind::Dispatch);
}

void MixedGPRViewsKeepParentCoverage() {
    const std::uint8_t bytes[]{
        0x89,0xc1,                         // mov ecx,eax: low32 snapshot, high RAX remains unknown
        0x48,0x89,0xc2,                    // mov rdx,rax: must fetch that still-unknown high half
        0x89,0xc6,                         // mov esi,eax: derive low32 without replacing full parent
        0x89,0xc7,                         // mov edi,eax: reuse that same view, not another normalizer
        0x49,0x89,0xc0,                    // mov r8,rax
        0x88,0xe3,                         // mov bl,ah
        0x66,0x41,0x89,0xc1,               // mov r9w,ax
        0x49,0x89,0xc2,                    // mov r10,rax: same immutable full parent
        0x88,0xcc,                         // mov ah,cl: retain one pending byte over the whole parent
        0x89,0xc1,                         // mov ecx,eax: merge AH, then derive the new low32
        0x89,0xc0,                         // mov eax,eax: same Value now defines zero-high full RAX
        0x4d,0x8b,0x7d,0x00,               // mov r15,[r13]: publish, clearing dirty but not coverage
        0x49,0x89,0xc4,                    // mov r12,rax: widen zero-high parent, no state reload
        0x66,0x41,0x89,0xc6,               // mov r14w,ax: cannot narrow the parent cache
        0x49,0x89,0xc6,                    // mov r14,rax: retain that same full parent
    };
    const BlockWindow window{0x14000,bytes};
    const auto private_state=static_cast<std::uint64_t>(TargetFeature::PrivateGPRState);
    const auto built=BuildIR({&window,1},Scope::Block,layout,{TargetMode::Known,0xa64,private_state});
    CHECK(built && built.instruction_count==15 && built.decoded_bytes==sizeof(bytes));
    const auto& p=built.program;
    const auto rax=layout.gprs_offset;
    std::vector<NodeId> low_reads,full_reads;
    std::size_t fault=p.Operations().size(),ah_store=fault,full_store=fault,parent_merge=fault;
    for (std::size_t i=0;i<p.Operations().size();++i) {
        const auto& op=p.Operations()[i];
        if (op.opcode==OpCode::LoadMemoryTSO) fault=i;
        if (op.opcode==OpCode::BitInsert) {
            CHECK(parent_merge==p.Operations().size());
            parent_merge=i;
        }
        if (op.opcode!=OpCode::LoadUniform && op.opcode!=OpCode::StoreUniform) continue;
        const auto field=p.Operands(op).front();
        if (op.opcode==OpCode::LoadUniform && field.Payload()==rax) {
            if (field.TypeView()==Type::U32) low_reads.push_back({static_cast<std::uint32_t>(i)});
            if (field.TypeView()==Type::U64) full_reads.push_back({static_cast<std::uint32_t>(i)});
        }
        if (op.opcode==OpCode::StoreUniform && field.Payload()==rax+1) {
            CHECK(field.TypeView()==Type::U8);
            ah_store=i;
        }
        if (op.opcode==OpCode::StoreUniform && field.Payload()==rax) {
            CHECK(field.TypeView()==Type::U64 && full_store==p.Operations().size());
            full_store=i;
        }
    }
    CHECK(low_reads.size()==1 && full_reads.size()==1);
    CHECK(ah_store==p.Operations().size());
    CHECK(low_reads[0].value<full_reads[0].value && full_reads[0].value<parent_merge);
    CHECK(parent_merge<full_store && full_store<fault);
    const auto& merged=p.Operations()[parent_merge];
    const auto merge=p.Operands(merged);
    CHECK(merged.type==Type::U64 && merge.front().Payload()==full_reads[0].value);
    CHECK(CellAt(merge,2).Payload()==8 && CellAt(merge,3).Payload()==8);
    const auto inserted=CellAt(merge,1);
    CHECK(p.Operations()[inserted.Payload()].type==Type::U8);
    const auto inserted_byte=p.Operands(NodeId{static_cast<std::uint32_t>(inserted.Payload())});
    CHECK(p.Operations()[inserted.Payload()].opcode==OpCode::BitExtract);
    CHECK(inserted_byte.front().Payload()==low_reads[0].value);
    CHECK(CellAt(inserted_byte,1).Payload()==0 && CellAt(inserted_byte,2).Payload()==8);
    const auto stored=[&](unsigned slot,Type field_type) {
        for (const auto& op : p.Operations()) {
            if (op.opcode!=OpCode::StoreUniform) continue;
            const auto args=p.Operands(op);
            const auto field=args.front();
            if (field.Payload()==rax+slot*layout.stride && field.TypeView()==field_type) return CellAt(args,1);
        }
        throw std::runtime_error("missing mixed-width publication");
    };
    // These complete snapshots retain the initial high half across all narrow reads.
    for (const auto slot : {2u,8u,10u}) {
        const auto value=stored(slot,Type::U64);
        CHECK(value.Kind()==OperandKind::Value && value.Payload()==full_reads[0].value && value.TypeView()==Type::U64);
    }
    const auto low=stored(6,Type::U64);
    CHECK(low.TypeView()==Type::U32 && p.Operations()[low.Payload()].opcode==OpCode::ZeroExtend32);
    CHECK(p.Operands(NodeId{static_cast<std::uint32_t>(low.Payload())}).front().Payload()==full_reads[0].value);
    CHECK(stored(7,Type::U64).Payload()==low.Payload());
    const auto high_byte=stored(3,Type::U8),word=stored(9,Type::U16);
    for (const auto value : {high_byte,word}) {
        const auto& op=p.Operations()[value.Payload()];
        const auto args=p.Operands(op);
        CHECK(op.opcode==OpCode::BitExtract && op.type==value.TypeView());
        CHECK(args.front().Payload()==full_reads[0].value);
        CHECK(CellAt(args,1).Payload()==(value.TypeView()==Type::U8?8:0));
        CHECK(CellAt(args,2).Payload()==(value.TypeView()==Type::U8?8:16));
    }
    // Self-writing EAX turns the merged parent's low32 into a zero-high whole write.
    const auto publication=CellAt(p.Operands(NodeId{static_cast<std::uint32_t>(full_store)}),1);
    CHECK(publication.TypeView()==Type::U32);
    CHECK(p.Operations()[publication.Payload()].opcode==OpCode::ZeroExtend32);
    const auto low_after_merge=p.Operands(NodeId{static_cast<std::uint32_t>(publication.Payload())}).front();
    CHECK(low_after_merge.Payload()==parent_merge && low_after_merge.TypeView()==Type::U32);
    const auto full=stored(12,Type::U64),later=stored(14,Type::U64);
    CHECK(full.Payload()==later.Payload() && full.TypeView()==Type::U64);
    CHECK(p.Operations()[full.Payload()].opcode==OpCode::ZeroExtend64 && full.Payload()>fault);
    const auto input=p.Operands(NodeId{static_cast<std::uint32_t>(full.Payload())}).front();
    CHECK(input.Payload()==publication.Payload() && input.TypeView()==Type::U32);

    // The same real instruction sequence keeps the unbound/generic Uniform path.
    const auto generic=BuildIR({&window,1},Scope::Block,layout,{TargetMode::Unknown,0,private_state});
    CHECK(generic && generic.instruction_count==built.instruction_count);
    CHECK(std::none_of(generic.program.Operations().begin(),generic.program.Operations().end(),[](const auto& op) {
        return op.opcode==OpCode::ZeroExtend32 || op.opcode==OpCode::BitExtract || op.opcode==OpCode::BitInsert;
    }));
    for (const auto& op : generic.program.Operations()) {
        if (op.opcode!=OpCode::StoreUniform) continue;
        const auto args=generic.program.Operands(op);
        if (args.front().Payload()!=rax) continue;
        CHECK(args.front().TypeView()==Type::U64 && CellAt(args,1).TypeView()==Type::U32);
    }
}

void PendingPartialWritesKeepWholeParents() {
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
    const BlockWindow window{0x16000,bytes};
    const auto private_state=static_cast<std::uint64_t>(TargetFeature::PrivateGPRState);
    const auto built=BuildIR({&window,1},Scope::Block,layout,{TargetMode::Known,0xa64,private_state});
    CHECK(built && built.instruction_count==23 && built.decoded_bytes==sizeof(bytes));
    const auto& p=built.program;
    const auto rax=layout.gprs_offset, r9=rax+9*layout.stride;
    std::vector<NodeId> merges;
    auto fault=p.Operations().size(),rax_store=fault;
    for (std::size_t i=0;i<p.Operations().size();++i) {
        const auto& op=p.Operations()[i];
        if (op.opcode==OpCode::BitInsert) {
            merges.push_back({static_cast<std::uint32_t>(i)});
        }
        if (op.opcode==OpCode::LoadMemoryTSO) fault=i;
        if (op.opcode!=OpCode::StoreUniform) continue;
        const auto field=p.Operands(op).front();
        CHECK(!(field.Payload()==rax+1 && field.TypeView()==Type::U8));
        if (field.Payload()==rax || field.Payload()==r9) CHECK(field.TypeView()==Type::U64);
        if (field.Payload()==rax) {
            CHECK(rax_store==p.Operations().size());
            rax_store=i;
        }
    }
    CHECK(merges.size()==5 && merges[2].value<rax_store && rax_store<fault && fault<merges[3].value);
    for (std::size_t i=0;i<4;++i) CHECK(p.Operations()[merges[i].value].type==Type::U32);
    CHECK(p.Operations()[merges[4].value].type==Type::U64);
    const auto source=[&](unsigned slot,Type type,unsigned byte=0) {
        for (const auto& op:p.Operations()) {
            if (op.opcode!=OpCode::StoreUniform) continue;
            const auto args=p.Operands(op);
            if (args.front().Payload()==rax+slot*layout.stride+byte && args.front().TypeView()==type)
                return CellAt(args,1);
        }
        throw std::runtime_error("missing partial-parent publication");
    };
    const auto literal=[&](CellView value,std::uint64_t bits) {
        CHECK(value.Kind()==OperandKind::Value);
        const auto& op=p.Operations()[value.Payload()];
        CHECK(op.opcode==OpCode::LoadImm && p.Operands(op).front().Payload()==bits);
    };
    const auto first=p.Operands(merges[0]),second=p.Operands(merges[1]),third=p.Operands(merges[2]);
    literal(first.front(),0x12345678);
    literal(CellAt(first,1),0x22);
    CHECK(CellAt(first,2).Payload()==0 && CellAt(first,3).Payload()==8);
    CHECK(second.front().Payload()==merges[0].value);
    literal(CellAt(second,1),0x33);
    CHECK(CellAt(second,2).Payload()==8 && CellAt(second,3).Payload()==8);
    CHECK(third.front().Payload()==merges[1].value);
    literal(CellAt(third,1),0x4455);
    CHECK(CellAt(third,2).Payload()==0 && CellAt(third,3).Payload()==16);
    literal(source(3,Type::U8),0x22);       // BL gets the latest AL, not the first pending write.
    literal(source(2,Type::U16),0x4455);    // DX gets the exact pending AX value.
    CHECK(source(1,Type::U64).Payload()==merges[0].value); // ECX keeps the earlier LEA snapshot.
    CHECK(source(0,Type::U64).Payload()==merges[2].value && source(0,Type::U64).TypeView()==Type::U32);
    CHECK(source(8,Type::U64).Payload()==merges[2].value);
    literal(source(9,Type::U64),0x76543210);
    const auto full=source(11,Type::U64);
    CHECK(full.Payload()>fault && p.Operations()[full.Payload()].opcode==OpCode::ZeroExtend64);
    const auto widened=p.Operands(NodeId{static_cast<std::uint32_t>(full.Payload())}).front();
    CHECK(widened.Payload()==merges[2].value && widened.TypeView()==Type::U32);

    const auto cl=source(2,Type::U8);
    CHECK(cl.Payload()==source(2,Type::U8,1).Payload());
    CHECK(p.Operations()[cl.Payload()].opcode==OpCode::BitExtract && p.Operations()[cl.Payload()].type==Type::U8);
    const auto cut=p.Operands(NodeId{static_cast<std::uint32_t>(cl.Payload())});
    CHECK(CellAt(cut,1).Payload()==0 && CellAt(cut,2).Payload()==8);
    CHECK(p.Operations()[cut.front().Payload()].opcode==OpCode::LsrImm);
    CHECK(p.Operations()[cut.front().Payload()].type==Type::U32);
    const auto canonical_merge=p.Operands(merges[3]);
    literal(canonical_merge.front(),0x123456fe);
    CHECK(CellAt(canonical_merge,1).Payload()==cl.Payload());
    CHECK(CellAt(canonical_merge,2).Payload()==0 && CellAt(canonical_merge,3).Payload()==8);
    CHECK(source(12,Type::U64).Payload()==merges[3].value);

    const auto wide_merge=p.Operands(merges[4]);
    literal(wide_merge.front(),0x11223344556677feULL);
    const auto shift=CellAt(wide_merge,1);
    CHECK(p.Operations()[shift.Payload()].opcode==OpCode::LsrImm && p.Operations()[shift.Payload()].type==Type::U32);
    CHECK(CellAt(wide_merge,2).Payload()==0 && CellAt(wide_merge,3).Payload()==8);
    for (const auto slot:{13u,14u}) {
        const auto low=source(slot,Type::U64);
        CHECK(low.TypeView()==Type::U32 && p.Operations()[low.Payload()].opcode==OpCode::ZeroExtend32);
        CHECK(p.Operands(NodeId{static_cast<std::uint32_t>(low.Payload())}).front().Payload()==merges[4].value);
    }
}
} // namespace

int main() {
    try {
        RealBytesAndControl();
        RejectedBytesCannotPublishAProgram();
        QualificationCountsTheActualAbandonedPrefix();
        RexExtensionRequiresTheFinalPrefixPosition();
        KnownTargetEmitsOneShiftedAddress();
        IntegerAluAndDeferredFlags();
        EveryConditionalBranchAndRealExits();
        ActualDecodedBufferAndRejectedFrontier();
        AutomaticFunctionDiscoveryAndEntry();
        AutomaticFunctionFailuresAndFrontiers();
        NarrowMovesAndStackControlAreConstructible();
        MixedGPRViewsKeepParentCoverage();
        PendingPartialWritesKeepWholeParents();
        std::puts("Real x86 decode/build/submit subset contracts passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr,"%s\n",error.what());
        return 1;
    }
}
