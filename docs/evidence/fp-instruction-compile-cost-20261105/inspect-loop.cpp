#include "runtime/frontend/x86/build_ir.h"
#include "runtime/backend/arm64/compile.h"
#include "runtime/compile/optimize.h"
#include "runtime/execution/guest_state.h"
#include <fstream>
#include <iterator>
#include <cstdio>
using namespace swift::runtime;
static const char* Names[]={
#define INST(name, ...) #name,
#include "runtime/ir/opcodes.inc"
#undef INST
};
int main(int argc,char**argv) {
 std::ifstream in(argv[1],std::ios::binary); std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),{});
 auto scope=argc>3 ? ir::Scope::Function : ir::Scope::Block;

 swift::x86::BlockWindow window{0x400ff0,{bytes.data()+0xff0,0xe2}};
 swift::x86::StateLayout layout{0,128,8,160};
 auto target=backend::arm64::KnownTargetInfo(); target.features|=std::uint64_t(ir::TargetFeature::PrivateGPRState)|std::uint64_t(ir::TargetFeature::PrivateVectorState)|std::uint64_t(ir::TargetFeature::FaultLocations);
 auto built=swift::x86::BuildIR({&window,1},scope,layout,target,nullptr,nullptr,argc>4);
 if(!built){std::printf("build error %u at %llx\n",unsigned(built.error),(unsigned long long)built.error_pc);return 1;}
 auto opt=compile::Optimize(std::move(built.program),SIZE_MAX,{{{136,8}},true});
 if(!opt)return 2;
 backend::arm64::CompileOptions options{136}; options.pinned_uniform_offset=0;options.pinned_uniform_slots=16;options.pinned_vector_offset=160;options.private_uniforms=true;
 options.dispatcher=0x100000;options.exit_handler=0x100010;options.pc_offset=128;options.native_nzcv=true;options.fault_locations=options.recover_unaligned=true;options.deferred_pc=true;options.entry_pc=window.pc;options.local_flags=true;options.native_fp_environment=true;
 options.mxcsr_flush_offset=416;options.mxcsr_flush_mode=0;options.rcpc_loads=options.rcpc_imm9=true;options.callable_directory=true;options.dispatch_mode=backend::arm64::DispatchMode::Inline;options.local_dispatch_cache=true;
 auto alloc=scope==ir::Scope::Block?backend::arm64::AllocateBlock(opt.program,&options):backend::arm64::AllocateFunction(opt.program,&options);
 unsigned id{};for(auto &op:opt.program.Operations()){
 auto &h=alloc.homes[id];std::printf("%3u B%u %-28s r%u spill%u at%u last%u",id,op.block.value,Names[unsigned(op.opcode)],h.reg,h.spilled,h.definition_position,h.last_use);
 for(auto c:opt.program.Operands(ir::NodeId{id}))std::printf(" [%u/%u %llu]",unsigned(c.Kind()),unsigned(c.TypeView()),(unsigned long long)c.Payload());
 std::puts("");++id;}
 auto out=backend::arm64::Compile(opt.program,options);std::printf("native valid=%u words=%zu frame=%u spills=%u nodes=%zu\n",bool(out),out.words.size(),out.frame_bytes,out.spill_slots,opt.program.Operations().size());
 std::ofstream assembly(argv[2]);assembly<<".text\n";for(auto word:out.words){char row[40];std::snprintf(row,sizeof(row),".inst 0x%08x\n",word);assembly<<row;}return out?0:3;
}
