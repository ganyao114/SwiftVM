#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#include <unistd.h>
#include <xmmintrin.h>
#include "runtime/backend/x64/translator.h"
#include "runtime/backend/x64/uniform_forward.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#include "runtime/frontend/arm64/state_ranges.h"
#include "runtime/frontend/ir_assembler.h"
#include "runtime/ir/opts/const_folding_pass.h"
#include "runtime/ir/opts/deadcode_elimination_pass.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
using Context = swift::arm64::ThreadContext64;
using Clock = std::chrono::steady_clock;
using Fn = HaltReason (*)(backend::State*);

namespace {
constexpr u64 code_base = 0x1000, nzcv_mask = u64{15} << 28;
constexpr u32 route_count = 2;
constexpr const char* scope = "decoded_a64_vector_cfg_pair";
constexpr std::array<const char*, route_count> route_names{"off", "on"};
static_assert(offsetof(Context,v)==272 && sizeof(Context)==800, "vector oracle context byte layout");
enum class Kind { Eor, And, Orr, Add4S, Nop };
struct Operation { Kind kind; u32 dst{}, lhs{}, rhs{}; };
constexpr u32 stages = 4;
using VectorBytes = std::array<u8,16>;
struct Program final : MemoryInterface {
    std::string name; bool hybrid{}, flags{}; std::vector<u32> words;
    std::vector<Operation> preheader, hot;
    std::vector<u64> helper_pcs;
    u64 StopPc() const { return code_base + (words.size()-1)*4; }
    u64 Instructions(u32 trips) const { return 2 + u64(trips)*(stages*(5+u32(hybrid))+2)+3; }
    bool Read(void* out,size_t address,size_t size) override {
        if(address<code_base||address-code_base>words.size()*4||size>words.size()*4-(address-code_base))return false;
        std::memcpy(out,reinterpret_cast<const u8*>(words.data())+address-code_base,size);return true;
    }
    bool Write(void*,size_t,size_t) override { return false; }
    void* GetPointer(void* address) override {
        const auto at=reinterpret_cast<uintptr_t>(address);
        if(at<code_base||at-code_base>words.size()*4||words.size()*4-(at-code_base)<4)return nullptr;
        return reinterpret_cast<u8*>(words.data())+at-code_base;
    }
    void Emit(Operation op,std::vector<Operation>& into) {
        const u32 base=op.kind==Kind::Eor?0x6e201c00u:op.kind==Kind::And?0x4e201c00u:
            op.kind==Kind::Orr?0x4ea01c00u:0x4ea08400u;
        if(op.kind==Kind::Nop){helper_pcs.push_back(code_base+words.size()*4);words.push_back(0xd503201fu);}
        else words.push_back(base|(op.rhs<<16)|(op.lhs<<5)|op.dst);
        into.push_back(op);
    }
};
Program MakeProgram(bool hybrid,bool flags) {
    Program p;p.hybrid=hybrid;p.flags=flags;p.name=hybrid?"simd_reuse_helper_hybrid":"simd_reuse_pure";
    p.Emit({Kind::Eor,0,20,21},p.preheader);p.words.push_back(0x14000001u);const u32 begin=p.words.size();
    for(u32 stage=0;stage<stages;++stage){
        p.Emit({Kind::Eor,1,0,20},p.hot);
        if(hybrid)p.Emit({Kind::Nop},p.hot);
        p.Emit({Kind::And,2,1,21},p.hot);p.Emit({Kind::Orr,3,1,22},p.hot);
        p.Emit({Kind::Eor,4,1,23},p.hot);p.Emit({Kind::Add4S,0,2,3},p.hot);
    }
    p.words.push_back(0xd100079cu); // SUB X28,X28,#1; flags remain guest NZCV.
    p.words.push_back(0xb5000000u|(((begin-u32(p.words.size()))&0x7ffffu)<<5)|28u);
    p.words.push_back(0xf100001fu|(83u<<10)); // CMP X0,#83, fixed in both flags modes.
    p.words.push_back(0x14000001u);p.words.push_back(0xd4200000u);return p;
}
__attribute__((noinline)) u64 StateNeutralClobber(u64 a,u64 b,u64 c) {
    // No State pointer, globals, memory effects or guest architectural effect.
    // The hybrid's exact guest NOP is independently executed by Unicorn.
    asm volatile("pxor %%xmm8,%%xmm8\n\tpxor %%xmm9,%%xmm9\n\t"
                 "pxor %%xmm10,%%xmm10\n\tpxor %%xmm11,%%xmm11\n\t"
                 "pxor %%xmm12,%%xmm12\n\tpxor %%xmm13,%%xmm13\n\t"
                 "pxor %%xmm14,%%xmm14\n\tpxor %%xmm15,%%xmm15\n\t"
                 "test %0,%0"::"r"(a):"cc","xmm8","xmm9","xmm10","xmm11","xmm12","xmm13","xmm14","xmm15");
    return a^b^c;
}
struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + sizeof(Context)> bytes{};
    backend::State* state = new(bytes.data()) backend::State{};
    ~Machine() { state->~State(); }
    Context& Guest() { return *reinterpret_cast<Context*>(state->uniform_buffer_begin); }
    void Init(u64 seed, u32 trips) {
        auto* guest = reinterpret_cast<u8*>(&Guest());
        for (u32 i = 0; i < sizeof(Context); ++i) guest[i] = u8(seed + i * 37 + 0x59);
        for (u32 i = 0; i < 29; ++i) Guest().r[i] = seed ^ (0x9e3779b97f4a7c15ULL * (i + 1));
        Guest().r[28] = trips; Guest().fp = seed ^ 0x3141592653589793ULL; Guest().lr = ~seed; Guest().sp = 0x8000;
        state->host_cpu_flags = ((seed ^ 0xabcd321001234567ULL) & ~nzcv_mask) | (((seed >> 4) & 15) << 28);
        state->current_loc = ir::Location{0xfeedface}; state->halt_reason = HaltReason::None;
    }
};
VectorBytes ReadVector(const Context& guest,u32 reg) { VectorBytes bytes;std::memcpy(bytes.data(),&guest.v[reg],16);return bytes; }
void WriteVector(Context& guest,u32 reg,const VectorBytes& bytes) { std::memcpy(&guest.v[reg],bytes.data(),16); }
void Model(const Program& p,Machine& machine,u32 trips) {
    auto& guest=machine.Guest();
    const auto execute=[&](const auto& operations){for(const auto& op:operations){
        if(op.kind==Kind::Nop)continue;
        const auto a=ReadVector(guest,op.lhs),b=ReadVector(guest,op.rhs);VectorBytes out{};
        if(op.kind==Kind::Add4S){for(u32 lane=0;lane<4;++lane){u32 left,right;
            std::memcpy(&left,a.data()+lane*4,4);std::memcpy(&right,b.data()+lane*4,4);
            const u32 value=left+right;std::memcpy(out.data()+lane*4,&value,4);}}
        else for(u32 byte=0;byte<16;++byte)out[byte]=op.kind==Kind::Eor?a[byte]^b[byte]:op.kind==Kind::And?a[byte]&b[byte]:a[byte]|b[byte];
        WriteVector(guest,op.dst,out);
    }};
    execute(p.preheader);for(u32 trip=0;trip<trips;++trip){execute(p.hot);--guest.r[28];}
    const u64 lhs=guest.r[0],rhs=83,result=lhs-rhs;
    const bool overflow=bool(((lhs^rhs)&(lhs^result))>>63);
    const u64 nzcv=(u64(result>>63)<<31)|(u64(result==0)<<30)|(u64(lhs>=rhs)<<29)|(u64(overflow)<<28);
    machine.state->host_cpu_flags=(machine.state->host_cpu_flags&~nzcv_mask)|nzcv;
    guest.padding=u32(swift::arm64::InterruptReason::BRK);
    machine.state->current_loc=ir::Location{p.StopPc()};machine.state->prev_loc=ir::Location{p.StopPc()-4};machine.state->halt_reason=HaltReason::CallHost;
}
struct Compiled {
    Config config{.backend_isa=kX86_64,.uniform_buffer_size=sizeof(Context),.stack_alignment=16};
    backend::FunctionPlan plan;std::unique_ptr<x64::Translator> code;
    u32 route{},guest_blocks{},forwarded_vector_ir{},vector_definitions{},retained_vector_homes{},vector_assigned{};
    std::string fixed_ir;Fn Entry()const{return code->getCode<Fn>();}
};
std::unique_ptr<Compiled> Compile(Program& p,u32 route) {
    auto unit=std::make_unique<Compiled>();unit->route=route;
    // Preserve each guest retirement marker for the exact NOP helper anchors.
    FeatureSet build_features{};build_features.advpc_coalesce=false;
    ir::HIRBuilder builder{1,true,build_features};auto* function=builder.AppendFunction(ir::Location{code_base});
    for(;;){std::vector<u64> pending;
        for(auto& hir:function->GetHIRBlockList()){auto* body=hir.GetBlock();if(body->GetInstList().empty()&&!body->HasTerminal())pending.push_back(body->GetStartLocation().Value());}
        if(pending.empty())break;if(unit->guest_blocks+pending.size()>8)throw std::runtime_error("vector HIR cap");
        for(auto pc:pending){builder.SetCurBlock(pc);ir::Assembler as{&builder};swift::arm64::A64Decoder{pc,&p,&as}.Decode();++unit->guest_blocks;}
    }
    function->EndFunction();std::vector<ir::HIRBlock*> ordered;
    for(auto* hir:function->GetHIRBlocks())if(hir&&hir->GetBlock()->GetStartLocation()==ir::Location{code_base})ordered.push_back(hir);
    for(auto* hir:function->GetHIRBlocks())if(hir&&hir->GetBlock()->GetStartLocation()!=ir::Location{code_base}&&hir->GetBlock()->GetStartLocation()!=ir::Location::INVALID&&hir->GetBlock()->HasTerminal())ordered.push_back(hir);
    // Fixed, existing per-block byte forwarding exposes decoded fan-out SSA.
    // Stores are retained. VecAdd ends facts conservatively. No pass change.
    for(auto* hir:ordered){auto* body=hir->GetBlock();
        unit->forwarded_vector_ir+=x64::ForwardUniformLoads(body,sizeof(Context)).vector_loads;
        ir::ConstFoldingPass::Run(body,GetSvmConfig().GetFeatureSet());ir::DeadCodeEliminationPass::Run(body,function);
    }
    unit->plan=backend::FunctionPlan::Build(function,ordered);
    if(p.hybrid){
        u32 injected{};
        // FunctionPlan locations identify lowered block entries. AdvancePC is
        // metadata only. Inject AFTER fixed passes, at exact guest NOP retirement.
        u64 pc=code_base;std::vector<std::pair<ir::Inst*,u64>> anchors;
        for(auto& inst:unit->plan.body->GetInstList()){
            if(unit->plan.locations.contains(&inst))pc=unit->plan.locations.at(&inst).Value();
            if(inst.GetOp()==ir::OpCode::AdvancePC){
                if(std::find(p.helper_pcs.begin(),p.helper_pcs.end(),pc)!=p.helper_pcs.end())anchors.emplace_back(&inst,pc);
                pc+=inst.GetArg<ir::Imm>(0).Get();
            }
        }
        for(const auto& [anchor,guest_pc]:anchors){
            (void)guest_pc;auto* call=new ir::Inst(ir::OpCode::CallLambda);
            call->SetArgs(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&StateNeutralClobber)}});call->SetReturn(ir::ValueType::U64);
            unit->plan.body->InsertAfter(call,anchor);++injected;
        }
        if(injected!=stages)throw std::runtime_error("hybrid NOP helper injection inventory");
    }
    unit->fixed_ir=unit->plan.body->ToString();
    unit->code=x64::Translator::PrepareFunction(unit->plan,x64::HostFeatures::Detect(),&unit->config,true,true,p.flags,
        {.uniform_forwarding=false,.state_promotion=false,.scalar_rematerialization=true,.scalar_binary_immediates=true,
         .loop_weighted_costs=true,.profitable_flags_residency=false,.scalar_unary_phases=true,.scalar_unary_coalescing=true,
         .scalar_shift_immediates=true,.scalar_unary_mask_elision=true,.native_cf_chains=false,.vector_cfg_allocation=bool(route)});
    if(unit->fixed_ir!=unit->plan.body->ToString())throw std::runtime_error("vector preparation changed fixed IR");unit->code->Translate();
    if(!unit->code->CfgPlan().Validate()||!unit->code->FramePlan().Enabled()||unit->guest_blocks!=4||
       unit->code->VectorCfgAllocationEnabled()!=bool(route)||!unit->forwarded_vector_ir||
       !unit->code->ScalarUnaryMaskElisionEnabled()||!unit->code->ScalarShiftImmediatesEnabled()||unit->code->NativeCfChainsEnabled())
        throw std::runtime_error("vector complete CFG/fixed policy absent");
    for(auto* inst:unit->code->CfgPlan().Instructions())if(inst->ReturnType()>=ir::ValueType::V8&&inst->ReturnType()<=ir::ValueType::V128){
        ++unit->vector_definitions;unit->retained_vector_homes+=unit->code->FramePlan().Home(inst).has_value();
        unit->vector_assigned+=unit->code->VectorPlan().Register(inst).has_value();
    }
    return unit;
}
void Validate(const Program& p,const Compiled& unit) {
    for(u64 seed:{u64{0},UINT64_MAX,u64{0x123456789abcdef0}})for(u32 trips:{1u,2u,7u,19u}){
        Machine actual,expected;actual.Init(seed,trips);expected.Init(seed,trips);Model(p,expected,trips);const auto mxcsr=_mm_getcsr();
        if(unit.Entry()(actual.state)!=HaltReason::CallHost||_mm_getcsr()!=mxcsr||actual.bytes!=expected.bytes){
            std::ostringstream out;out<<p.name<<" full State/vector mismatch route="<<unit.route<<" seed="<<seed<<" trips="<<trips;
            for(u32 i=0;i<actual.bytes.size();++i)if(actual.bytes[i]!=expected.bytes[i])out<<" byte="<<i<<":"<<u32(actual.bytes[i])<<"/"<<u32(expected.bytes[i]);
            throw std::runtime_error(out.str());
        }
    }
}
void Proof(const Program& program, std::array<std::unique_ptr<Compiled>, route_count>& units) {
    auto& off = *units[0]; auto& on = *units[1];
    const auto& a = off.code->CfgPlan(); const auto& b = on.code->CfgPlan();
    const auto& pa = off.code->PreparationStats(); const auto& pb = on.code->PreparationStats();
    if (pa.full_allocation_builds != pb.full_allocation_builds || pa.reused_final_allocation != pb.reused_final_allocation ||
        pa.flags_policy_allocation_builds != pb.flags_policy_allocation_builds ||
        pa.flags_policy_allocation_recolors != pb.flags_policy_allocation_recolors)
        throw std::runtime_error("vector emission preparation census differs");
    const auto ordinal = [](const auto& cfg, const ir::Inst* inst) {
        return std::find(cfg.Instructions().begin(), cfg.Instructions().end(), inst) - cfg.Instructions().begin();
    };
    if (off.fixed_ir != on.fixed_ir || off.code->FlagsResident() != program.flags || on.code->FlagsResident() != program.flags ||
        a.PhysicalRegisters() != b.PhysicalRegisters() || a.Blocks().size() != b.Blocks().size() || a.Edges().size() != b.Edges().size() ||
        a.Instructions().size() != b.Instructions().size() || a.Allocations().size() != b.Allocations().size())
        throw std::runtime_error("vector emission pair changed fixed IR/flags/budget/graph");
    for (u32 i = 0; i < a.Blocks().size(); ++i) {
        const auto& x = a.Blocks()[i]; const auto& y = b.Blocks()[i];
        if (x.begin != y.begin || x.end != y.end || x.reachable != y.reachable || x.incoming_edges != y.incoming_edges ||
            x.outgoing_edges != y.outgoing_edges || x.loop_depth != y.loop_depth || x.loop_weight != y.loop_weight ||
            x.live_in.size() != y.live_in.size() || x.live_out.size() != y.live_out.size())
            throw std::runtime_error("vector emission block topology differs");
        for (u32 j = 0; j < x.live_in.size(); ++j)
            if (ordinal(a,x.live_in[j]) != ordinal(b,y.live_in[j])) throw std::runtime_error("vector emission live-in differs");
        for (u32 j = 0; j < x.live_out.size(); ++j)
            if (ordinal(a,x.live_out[j]) != ordinal(b,y.live_out[j])) throw std::runtime_error("vector emission live-out differs");
    }
    for(u32 from=0;from<a.Blocks().size();++from)for(u32 to=0;to<a.Blocks().size();++to)
        if(a.DominatesBlock(from,to)!=b.DominatesBlock(from,to))throw std::runtime_error("vector pair dominance differs");
    for (u32 i = 0; i < a.Edges().size(); ++i) {
        const auto& x = a.Edges()[i]; const auto& y = b.Edges()[i];
        if (x.from != y.from || x.to != y.to || x.taken != y.taken || x.host_entry != y.host_entry ||
            x.copies.size() != y.copies.size() || x.live.size() != y.live.size() || ordinal(a,x.source) != ordinal(b,y.source))
            throw std::runtime_error("vector emission edge topology differs");
        for (u32 j = 0; j < x.live.size(); ++j)
            if (ordinal(a,x.live[j]) != ordinal(b,y.live[j])) throw std::runtime_error("vector emission edge liveness differs");
        for (u32 j = 0; j < x.copies.size(); ++j) {
            const auto& left = x.copies[j]; const auto& right = y.copies[j];
            if (ordinal(a,left.destination) != ordinal(b,right.destination) || left.source.IsValue() != right.source.IsValue() ||
                left.source.IsImm() != right.source.IsImm() ||
                (left.source.IsValue() && (ordinal(a,left.source.value.Def()) != ordinal(b,right.source.value.Def()) || left.source.value.Type() != right.source.value.Type())) ||
                (left.source.IsImm() && left.source.imm.Get() != right.source.imm.Get()))
                throw std::runtime_error("vector emission parallel Phi copy differs");
        }
    }
    for (u32 i = 0; i < a.Allocations().size(); ++i) {
        const auto& x = a.Allocations()[i]; const auto& y = b.Allocations()[i];
        if (ordinal(a,x.producer) != ordinal(b,y.producer) || x.slot != y.slot || x.requires_canonical_home != y.requires_canonical_home ||
            x.uses != y.uses || x.scalar_reads != y.scalar_reads || x.estimated_benefit != y.estimated_benefit ||
            x.requires_full_container != y.requires_full_container || x.weighted_reads != y.weighted_reads || x.weighted_benefit != y.weighted_benefit)
            throw std::runtime_error("vector emission coloring/home obligation differs");
    }
    for (u32 i = 0; i < a.Instructions().size(); ++i) {
        const auto* x = a.Instructions()[i]; const auto* y = b.Instructions()[i];
        if (off.code->FramePlan().Home(x) != on.code->FramePlan().Home(y) ||
            off.code->FramePlan().Decision(x) != on.code->FramePlan().Decision(y) ||
            off.code->FramePlan().RematerializedValue(x) != on.code->FramePlan().RematerializedValue(y))
            throw std::runtime_error("vector emission every-instruction home map differs");
        for (u32 j = 0; j < a.Instructions().size(); ++j)
            if (a.Interferes(x,a.Instructions()[j]) != b.Interferes(y,b.Instructions()[j]) ||
                a.CanReuseUnaryInput(x,a.Instructions()[j]) != b.CanReuseUnaryInput(y,b.Instructions()[j]) ||
                a.CanReuseBinaryInput(x,a.Instructions()[j]) != b.CanReuseBinaryInput(y,b.Instructions()[j]))
                throw std::runtime_error("vector policy interference/reuse proof differs");
    }
    if(off.code->VectorPlan().Enabled()||off.vector_assigned||off.code->VectorCallFrameBytes())throw std::runtime_error("vector OFF owns colors");
    const auto& plan=on.code->VectorPlan();const auto& x=off.code->Statistics();const auto& y=on.code->Statistics();
    if(!plan.Validate()||!on.vector_assigned||!y.vector_cfg_cache_read_sites||plan.UsedRegisters().empty()||
       on.code->VectorCallFrameBytes()!=plan.UsedRegisters().size()*16||
       off.vector_definitions!=on.vector_definitions||off.retained_vector_homes!=off.vector_definitions||on.retained_vector_homes!=on.vector_definitions)
        throw std::runtime_error("vector colors/actual cache read/full canonical retention proof absent");
    for(auto reg:plan.UsedRegisters())if(reg<8||reg>15)throw std::runtime_error("vector pool outside XMM8..15");
    const auto helpers=program.hybrid?stages:0u;
    // EndMemory adds one common epilogue call after vector cache use ends.
    if(x.helper_call_sites!=helpers+1||y.helper_call_sites!=helpers+1||
       x.vector_cfg_helper_save_sites||x.vector_cfg_helper_restore_sites||
       y.vector_cfg_helper_save_sites!=helpers*plan.UsedRegisters().size()||y.vector_cfg_helper_restore_sites!=y.vector_cfg_helper_save_sites)
        throw std::runtime_error("central full-color helper ABI actual sites differ");
    // Compare scalar/state/metadata obligations; vector load/publication and
    // explicit helper/frame traffic may legally change with vector policy.
#define SAME_STAT(field) if(x.field!=y.field)throw std::runtime_error("vector changed fixed scalar/boundary statistic " #field)
    SAME_STAT(scalar_cache_read_sites);SAME_STAT(scalar_cache_write_sites);SAME_STAT(flags_snapshot_sites);
    SAME_STAT(flags_state_load_sites);SAME_STAT(flags_state_store_sites);SAME_STAT(flags_resident_merge_sites);
    SAME_STAT(flags_state_carry_read_sites);SAME_STAT(flags_resident_carry_read_sites);SAME_STAT(flags_materialization_ir_sites);
    SAME_STAT(phi_value_copy_sites);SAME_STAT(phi_immediate_copy_sites);SAME_STAT(direct_phi_copy_sites);
    SAME_STAT(scalar_coalesced_binary_sites);SAME_STAT(scalar_coalesced_unary_sites);SAME_STAT(scalar_high_word_store_omissions);
    SAME_STAT(scalar_rematerialized_read_sites);SAME_STAT(scalar_rematerialized_definition_sites);
#undef SAME_STAT
    for(auto region:{x64::TransferRegion::Uniform,x64::TransferRegion::ProducerMetadata,x64::TransferRegion::StateFlags,x64::TransferRegion::StateOther}){
        const auto& left=x.transfers[unsigned(region)];const auto& right=y.transfers[unsigned(region)];
        if(left.scalar_load_sites!=right.scalar_load_sites||left.scalar_store_sites!=right.scalar_store_sites||
           left.vector_load_sites!=right.vector_load_sites||left.vector_store_sites!=right.vector_store_sites||left.load_bytes!=right.load_bytes||left.store_bytes!=right.store_bytes)
            throw std::runtime_error("vector changed guest State/Uniform/metadata transfer census");
    }
    std::cerr<<"vector_fixture workload="<<program.name<<" flags="<<program.flags
        <<" fixed_ir=1 fixed_scalar_allocation=1 fixed_canonical_homes=1 retained_vectors="<<on.retained_vector_homes
        <<" vector_assigned="<<on.vector_assigned<<" used_colors="<<plan.UsedRegisters().size()
        <<" cache_reads="<<y.vector_cfg_cache_read_sites<<" cache_writes="<<y.vector_cfg_cache_write_sites
        <<" helper_sites="<<helpers<<" helper_saves="<<y.vector_cfg_helper_save_sites<<" helper_restores="<<y.vector_cfg_helper_restore_sites
        <<" call_frame_bytes="<<on.code->VectorCallFrameBytes()<<" code_bytes_off="<<off.code->getSize()<<" code_bytes_on="<<on.code->getSize()
        <<" frame_bytes_off="<<off.code->FrameSize()<<" frame_bytes_on="<<on.code->FrameSize()<<" home_store_reduction_claim=0 spill_reduction_claim=0\n";
}
void JsonMachine(std::ostream& out,const char* prefix,Machine& machine) {
    const char* digits="0123456789abcdef";
    out<<",\""<<prefix<<"_pc\":"<<machine.state->current_loc.Value()
        <<",\""<<prefix<<"_prev_pc\":"<<machine.state->prev_loc.Value()
        <<",\""<<prefix<<"_packed_flags\":"<<machine.state->host_cpu_flags
        <<",\""<<prefix<<"_state_hex\":\"";
    for(auto byte:machine.bytes)out<<digits[byte>>4]<<digits[byte&15];out<<"\"";
    out<<",\""<<prefix<<"_x\":[";
    for(u32 i=0;i<29;++i){if(i)out<<',';out<<machine.Guest().r[i];}
    out<<','<<machine.Guest().fp<<','<<machine.Guest().lr<<"]"
        <<",\""<<prefix<<"_sp\":"<<machine.Guest().sp
        <<",\""<<prefix<<"_nzcv\":"<<(machine.state->host_cpu_flags&nzcv_mask)
        <<",\""<<prefix<<"_v\":[";
    for(u32 reg=0;reg<32;++reg){if(reg)out<<',';out<<"\"";
        for(auto byte:ReadVector(machine.Guest(),reg))out<<digits[byte>>4]<<digits[byte&15];out<<"\"";
    }out<<']';
}
void OracleCases(std::ostream& out, Program& program, const std::array<std::unique_ptr<Compiled>, route_count>& units, bool& first) {
    for (u64 seed : {u64{0}, UINT64_MAX, u64{0x123456789abcdef0}}) for (u32 trips : {1u, 2u, 7u, 19u}) {
        Machine input, expected; input.Init(seed, trips); expected.Init(seed, trips); Model(program, expected, trips);
        if (!first) out << ','; first = false;
        out << "{\"workload\":\"" << program.name << "\",\"seed\":" << seed << ",\"trips\":" << trips
            << ",\"flags_residency\":" << program.flags << ",\"code_base\":" << code_base << ",\"stop_pc\":" << program.StopPc() << ",\"words\":[";
        for (size_t i = 0; i < program.words.size(); ++i) { if (i) out << ','; out << program.words[i]; }
        out << ']'; JsonMachine(out, "input", input); JsonMachine(out, "expected", expected);
        for (u32 route = 0; route < route_count; ++route) {
            Machine actual; actual.Init(seed, trips); const auto mxcsr = _mm_getcsr(); const auto reason = units[route]->Entry()(actual.state);
            if (reason != HaltReason::CallHost || _mm_getcsr() != mxcsr || std::memcmp(actual.bytes.data(), expected.bytes.data(), actual.bytes.size()))
                throw std::runtime_error("oracle export full Context+State mismatch");
            const std::string prefix = std::string{"actual_"} + route_names[route]; JsonMachine(out, prefix.c_str(), actual);
        }
        out << '}';
    }
}
void SaveExclusive(const char* path, const std::string& contents) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "exclusive oracle corpus create");
    size_t written{};
    while (written < contents.size()) {
        const auto count = write(fd, contents.data() + written, contents.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { const int error = count < 0 ? errno : EIO; close(fd); throw std::system_error(error, std::generic_category(), "oracle corpus write"); }
        written += size_t(count);
    }
    if (close(fd)) throw std::system_error(errno, std::generic_category(), "oracle corpus close");
}
double Median(std::vector<double> values) { std::sort(values.begin(), values.end()); return values.at(values.size() / 2); }
std::array<double, route_count> Measure(const std::array<std::unique_ptr<Compiled>, route_count>& units, u32 calls, u32 trips) {
    std::array<Machine, route_count> machines; std::array<std::vector<double>, route_count> samples;
    for (u32 route = 0; route < route_count; ++route) {
        machines[route].Init(0x123456789abcdef0, trips);
        for (u32 warm = 0; warm < 32; ++warm) {
            machines[route].Guest().r[28] = trips; machines[route].state->halt_reason = HaltReason::None;
            if (units[route]->Entry()(machines[route].state) != HaltReason::CallHost) throw std::runtime_error("vector CFG warmup exit");
        }
    }
    for (u32 repeat = 0; repeat < 7; ++repeat) for (u32 order = 0; order < route_count; ++order) {
        const u32 route = (order + repeat) % route_count; auto& machine = machines[route]; const auto entry = units[route]->Entry(); const auto begin = Clock::now();
        for (u32 call = 0; call < calls; ++call) {
            machine.Guest().r[28] = trips; machine.state->halt_reason = HaltReason::None;
            if (entry(machine.state) != HaltReason::CallHost) throw std::runtime_error("vector CFG timing exit");
        }
        samples[route].push_back(std::chrono::duration<double, std::nano>(Clock::now() - begin).count() / calls);
    }
    std::array<double, route_count> result{}; for (u32 route = 0; route < route_count; ++route) result[route] = Median(samples[route]); return result;
}
u32 PositiveArgument(const char* value) {
    size_t consumed{}; const auto number = std::stoull(value, &consumed);
    if (consumed != std::strlen(value) || !number || number > std::numeric_limits<u32>::max()) throw std::runtime_error("calls/trips require positive uint32");
    return u32(number);
}
} // namespace
int main(int argc,char** argv) {
    try {
        const bool validate_only=argc==2&&std::string{argv[1]}=="--validate-only",oracle=argc==3&&std::string{argv[1]}=="--oracle-corpus";
        const bool correctness=validate_only||oracle;if(!correctness&&argc>3)throw std::runtime_error("usage: benchmark [calls [trips]], --validate-only, --oracle-corpus NEW_FILE");
        const u32 calls=correctness||argc<2?500:PositiveArgument(argv[1]),trips=correctness||argc<3?64:PositiveArgument(argv[2]);
        std::cerr<<"scope="<<scope<<"; fixed_ir_before_policy=1; cfg_on=1; flat_forwarding_fixed=1; state_promotion=0; native_cf_chains=0; "
            "oracle=independent_full_context_vectors_and_state; transfer_counts=static_sites; helper_role=state_neutral_hybrid; "
            "rematerialization_on=1; unary_phases_on=1; unary_coalescing_on=1; shift_immediates_on=1; allocation_feedback=0; "
            "application_performance_claim=0; dynamic_spill_claim=0; home_store_reduction_claim=0\n";
        std::ostringstream corpus;bool first_case=true;if(oracle)corpus<<"{\"schema_version\":1,\"scope\":\""<<scope<<"\",\"cases\":[";
        if(!correctness)std::cout<<"workload,flags_residency,vector_cfg_allocation,guest_words,guest_blocks,cfg_blocks,trips,physical_registers,assigned_values,physical_home_slots,retained_home_slots,home_bytes,frame_bytes,code_bytes,vector_definitions,retained_vector_homes,vector_assigned,used_vector_colors,vector_call_frame_bytes,vector_cfg_cache_read_sites,vector_cfg_cache_write_sites,vector_cfg_helper_save_sites,vector_cfg_helper_restore_sites,vector_cfg_phi_publication_sites,vector_cfg_legacy_home_retentions,helper_call_sites,vector_forward_read_sites,vector_forward_write_sites,scalar_cache_read_sites,scalar_cache_write_sites,ssa_scalar_load_sites,ssa_scalar_store_sites,ssa_vector_load_sites,ssa_vector_store_sites,ssa_load_bytes,ssa_store_bytes,observed_uniform_load_sites,observed_uniform_store_sites,other_vector_load_sites,other_vector_store_sites,flags_state_load_sites,flags_state_store_sites,preparation_full_allocation_builds,preparation_reused_final_allocation,fixed_forwarded_vector_ir,forward_allocation_rounds,decode_plan_prepare_emit_us,native_ns_per_call,native_ns_per_guest_iteration,guest_instructions_per_second\n";
        for(bool hybrid:{false,true})for(bool flags:{false,true}){
            auto program=MakeProgram(hybrid,flags);std::array<std::unique_ptr<Compiled>,route_count> units;std::array<std::vector<double>,route_count> preparation;
            if(!correctness)for(u32 repeat=0;repeat<9;++repeat)for(u32 order=0;order<route_count;++order){
                const u32 route=(order+repeat)%route_count;const auto begin=Clock::now();auto sample=Compile(program,route);
                preparation[route].push_back(std::chrono::duration<double,std::micro>(Clock::now()-begin).count());
            }
            for(u32 route=0;route<route_count;++route){units[route]=Compile(program,route);Validate(program,*units[route]);}
            Proof(program,units);if(oracle)OracleCases(corpus,program,units,first_case);if(correctness)continue;
            const auto ns=Measure(units,calls,trips);
            for(u32 route=0;route<route_count;++route){auto& unit=*units[route];auto& code=*unit.code;const auto& s=code.Statistics();
                const auto& homes=code.FramePlan().Statistics();u32 assigned{};for(const auto& allocation:code.CfgPlan().Allocations())assigned+=allocation.slot.has_value();
                const auto& ssa=s.transfers[unsigned(x64::TransferRegion::CanonicalSSA)],&uniform=s.transfers[unsigned(x64::TransferRegion::Uniform)];
                const auto& other=s.transfers[unsigned(x64::TransferRegion::FrameOther)];
                std::cout<<std::fixed<<std::setprecision(2)<<program.name<<','<<flags<<','<<route<<','<<program.words.size()<<','<<unit.guest_blocks<<','<<code.CfgPlan().Blocks().size()<<','<<trips
                    <<','<<code.CfgPlan().PhysicalRegisters().size()<<','<<assigned<<','<<homes.home_count<<','<<homes.retained_homes<<','<<homes.home_bytes<<','<<code.FrameSize()<<','<<code.getSize()
                    <<','<<unit.vector_definitions<<','<<unit.retained_vector_homes<<','<<unit.vector_assigned<<','<<code.VectorPlan().UsedRegisters().size()<<','<<code.VectorCallFrameBytes()
                    <<','<<s.vector_cfg_cache_read_sites<<','<<s.vector_cfg_cache_write_sites<<','<<s.vector_cfg_helper_save_sites<<','<<s.vector_cfg_helper_restore_sites
                    <<','<<s.vector_cfg_phi_publication_sites<<','<<s.vector_cfg_legacy_home_retentions<<','<<s.helper_call_sites<<','<<s.vector_forward_read_sites<<','<<s.vector_forward_write_sites
                    <<','<<s.scalar_cache_read_sites<<','<<s.scalar_cache_write_sites<<','<<ssa.scalar_load_sites<<','<<ssa.scalar_store_sites<<','<<ssa.vector_load_sites<<','<<ssa.vector_store_sites<<','<<ssa.load_bytes<<','<<ssa.store_bytes
                    <<','<<uniform.scalar_load_sites+uniform.vector_load_sites<<','<<uniform.scalar_store_sites+uniform.vector_store_sites<<','<<other.vector_load_sites<<','<<other.vector_store_sites
                    <<','<<s.flags_state_load_sites<<','<<s.flags_state_store_sites<<','<<code.PreparationStats().full_allocation_builds<<','<<code.PreparationStats().reused_final_allocation<<','<<unit.forwarded_vector_ir<<','<<code.UniformForwardStats().allocation_rounds
                    <<','<<Median(preparation[route])<<','<<ns[route]<<','<<ns[route]/trips<<','<<double(program.Instructions(trips))*1e9/ns[route]<<'\n';
            }
        }
        if(oracle){corpus<<"]}\n";SaveExclusive(argv[2],corpus.str());std::cerr<<"oracle_cases=48; exclusive_corpus=1\n";}
        if(correctness)std::cerr<<"validated_routes=8; seeds_per_route=3; trip_counts=1,2,7,19; fixed_ir_and_scalar_allocation=1; retained_vector_canonical_homes=1; actual_vector_cache_reads=1; hybrid_helper_abi=1; no_speedup_threshold=1\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"vector CFG benchmark: "<<error.what()<<'\n';return 1;}
}
