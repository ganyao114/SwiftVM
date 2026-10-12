#include <algorithm>
#include <array>
#include <chrono>
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
constexpr u32 pairs = 8, route_count = 2;
constexpr const char* scope = "decoded_a64_uniform_dead_store_cross_pc_pair";
constexpr std::array<const char*, route_count> route_names{"off", "on"};

enum class Kind { UnrelatedPositive, SelfAliasNegative };
constexpr std::array<const char*,2> kind_names{"unrelated_wx_overwrites","self_alias_wx"};
struct Program final : MemoryInterface {
    std::string name; Kind kind{}; bool flags{}; std::vector<u32> words;
    bool Positive() const { return kind==Kind::UnrelatedPositive; }
    u64 StopPc() const { return code_base+(words.size()-1)*4; }
    u64 Instructions(u32 trips) const { return u64(trips)*(pairs*2+3)+2; }
    bool Read(void* out,size_t address,size_t size) override {
        if(address<code_base||address-code_base>words.size()*4||size>words.size()*4-(address-code_base))return false;
        std::memcpy(out,reinterpret_cast<const u8*>(words.data())+address-code_base,size);return true;
    }
    bool Write(void*,size_t,size_t) override {return false;}
    void* GetPointer(void* pointer) override {
        const auto at=reinterpret_cast<uintptr_t>(pointer);
        return at>=code_base&&at-code_base<words.size()*4?reinterpret_cast<u8*>(words.data())+at-code_base:nullptr;
    }
};
Program MakeProgram(Kind kind,bool flags) {
    Program p;p.kind=kind;p.flags=flags;p.name=kind_names[u32(kind)];
    p.words.push_back(0xd53b420fu); // MRS X15,NZCV: observer BEFORE the overwrite region.
    for(u32 pair=0;pair<pairs;++pair) {
        p.words.push_back(p.Positive()?0x0b020020u:0x0b020000u); // ADD W0,W1/W0,W2.
        p.words.push_back(p.Positive()?0x8b040060u:0x8b040000u); // ADD X0,X3/X0,X4.
    }
    p.words.push_back(0xd100079cu); // SUB X28,X28,#1, preserves guest flags.
    p.words.push_back(0xb5000000u|(((0u-u32(p.words.size()))&0x7ffffu)<<5)|28u);
    p.words.push_back(0x14000001u);p.words.push_back(0xd4200000u);return p;
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

void Model(const Program& p,Machine& machine,u32 trips) {
    auto& regs=machine.Guest().r;
    for(u32 trip=0;trip<trips;++trip) {
        regs[15]=machine.state->host_cpu_flags&nzcv_mask;
        for(u32 pair=0;pair<pairs;++pair) {
            const u64 w_lhs=p.Positive()?regs[1]:regs[0];
            regs[0]=u32(u32(w_lhs)+u32(regs[2]));
            const u64 x_lhs=p.Positive()?regs[3]:regs[0];
            regs[0]=x_lhs+regs[4];
        }
        --regs[28];
    }
    machine.Guest().padding=u32(swift::arm64::InterruptReason::BRK);
    machine.state->current_loc=ir::Location{p.StopPc()};machine.state->prev_loc=ir::Location{p.StopPc()-4};
    machine.state->halt_reason=HaltReason::CallHost;
}
u64 Hash(const std::string& value) {
    u64 hash=14695981039346656037ULL;
    for(unsigned char byte:value){hash^=byte;hash*=1099511628211ULL;}return hash;
}
struct Census {
    u32 stores{},nops{},pc{};u64 allocation_hash{},homes_hash{};
    std::vector<std::string> assignments,homes;
};
Census Inspect(const x64::Translator& code) {
    Census result;std::ostringstream assignment_text,home_text;
    const auto& cfg=code.CfgPlan();
    for(auto* inst:cfg.Instructions()) {
        result.stores+=inst->GetOp()==ir::OpCode::StoreUniform;
        result.nops+=inst->GetOp()==ir::OpCode::Nop;
        result.pc+=inst->GetOp()==ir::OpCode::AdvancePC;
        std::ostringstream assigned,home,slot;
        const auto found=std::find_if(cfg.Allocations().begin(),cfg.Allocations().end(),[&](const auto& a){return a.producer==inst;});
        if(found==cfg.Allocations().end())assigned<<"none";
        else assigned<<(found->slot?u32(*found->slot):UINT32_MAX)<<':'<<found->requires_canonical_home<<':'<<found->requires_full_container
                     <<':'<<found->uses<<':'<<found->scalar_reads<<':'<<found->weighted_reads<<':'<<found->weighted_benefit;
        if(found==cfg.Allocations().end())slot<<"absent";
        else slot<<(found->slot?u32(*found->slot):UINT32_MAX);
        const auto offset=code.FramePlan().Home(inst);
        const auto remat=code.FramePlan().RematerializedValue(inst);
        home<<(offset?*offset:UINT32_MAX)<<':'<<u32(code.FramePlan().Decision(inst))<<':'<<bool(remat)<<':'<<remat.value_or(0);
        result.assignments.push_back(slot.str());result.homes.push_back(home.str());
        assignment_text<<assigned.str()<<'\n';home_text<<home.str()<<'\n';
    }
    result.allocation_hash=Hash(assignment_text.str());result.homes_hash=Hash(home_text.str());return result;
}
struct Compiled {
    Config config{.backend_isa=kX86_64,.uniform_buffer_size=sizeof(Context),.stack_alignment=16};
    backend::FunctionPlan plan;std::unique_ptr<x64::Translator> code;u32 route{},guest_blocks{};
    std::string decoded_ir,prepared_ir;Census census;
    Fn Entry()const{return code->getCode<Fn>();}
};
std::unique_ptr<Compiled> Compile(Program& p,u32 route) {
    auto unit=std::make_unique<Compiled>();unit->route=route;unit->config.enable_jit=true;
    // Keep exact ordinary decode-progress markers on both routes. The default
    // HIR builder folds non-flags markers and cannot exercise crossing them.
    FeatureSet build_features{};
    build_features.advpc_coalesce = false;
    ir::HIRBuilder builder{1,true,build_features};auto* function=builder.AppendFunction(ir::Location{code_base});
    for(;;) {
        std::vector<u64> pending;
        for(auto& hir:function->GetHIRBlockList()){auto* body=hir.GetBlock();if(body->GetInstList().empty()&&!body->HasTerminal())pending.push_back(body->GetStartLocation().Value());}
        if(pending.empty())break;if(unit->guest_blocks+pending.size()>8)throw std::runtime_error("DSE HIR cap");
        for(auto pc:pending){builder.SetCurBlock(pc);ir::Assembler as{&builder};swift::arm64::A64Decoder{pc,&p,&as}.Decode();++unit->guest_blocks;}
    }
    function->EndFunction();std::vector<ir::HIRBlock*> ordered;
    for(auto* hir:function->GetHIRBlocks())if(hir&&hir->GetBlock()->GetStartLocation()==ir::Location{code_base})ordered.push_back(hir);
    for(auto* hir:function->GetHIRBlocks())if(hir&&hir->GetBlock()->GetStartLocation()!=ir::Location{code_base}&&hir->GetBlock()->GetStartLocation()!=ir::Location::INVALID&&hir->GetBlock()->HasTerminal())ordered.push_back(hir);
    unit->plan=backend::FunctionPlan::Build(function,ordered);unit->decoded_ir=unit->plan.body->ToString();
    unit->code=x64::Translator::PrepareFunction(unit->plan,x64::HostFeatures::Detect(),&unit->config,true,true,p.flags,
        {.uniform_forwarding=false,.state_promotion=false,.scalar_rematerialization=true,.scalar_binary_immediates=true,
         .loop_weighted_costs=true,.profitable_flags_residency=false,.scalar_unary_phases=true,.scalar_unary_coalescing=true,
         .scalar_shift_immediates=true,.scalar_unary_mask_elision=true,.uniform_dead_store_cross_pc=bool(route),.native_cf_chains=false});
    unit->prepared_ir=unit->plan.body->ToString();unit->code->Translate();unit->census=Inspect(*unit->code);
    if(!unit->code->CfgPlan().Validate()||!unit->code->FramePlan().Enabled()||unit->guest_blocks!=3||unit->code->FlagsResident()!=p.flags)
        throw std::runtime_error("DSE final CFG/frame/flags policy absent");
    return unit;
}
void Validate(const Program& p,const Compiled& unit) {
    for(u64 seed:{u64{0},UINT64_MAX,u64{0x123456789abcdef0}})for(u32 trips:{1u,2u,7u,19u}) {
        Machine actual,expected;actual.Init(seed,trips);expected.Init(seed,trips);Model(p,expected,trips);const auto saved=_mm_getcsr();
        const auto reason=unit.Entry()(actual.state);
        if(reason!=HaltReason::CallHost||_mm_getcsr()!=saved||actual.bytes!=expected.bytes){std::ostringstream out;out<<p.name<<" full State mismatch route="<<unit.route<<" seed="<<seed<<" trips="<<trips;for(u32 i=0;i<actual.bytes.size();++i)if(actual.bytes[i]!=expected.bytes[i])out<<" byte="<<i<<":"<<u32(actual.bytes[i])<<"/"<<u32(expected.bytes[i]);throw std::runtime_error(out.str());}
    }
}

std::pair<u32,u32> Changes(const Compiled& off,const Compiled& on) {
    if(off.census.assignments.size()!=on.census.assignments.size()||off.census.homes.size()!=on.census.homes.size())
        throw std::runtime_error("Store-to-Nop pass unexpectedly changed instruction inventory");
    u32 assignments{},homes{};
    for(u32 i=0;i<off.census.assignments.size();++i){assignments+=off.census.assignments[i]!=on.census.assignments[i];homes+=off.census.homes[i]!=on.census.homes[i];}
    return {assignments,homes};
}
void Proof(const Program& p,const std::array<std::unique_ptr<Compiled>,route_count>& units) {
    const auto& off=*units[0];const auto& on=*units[1];const auto& a=*off.code;const auto& b=*on.code;
    const auto& old=a.UniformDeadStorePcStats();const auto& now=b.UniformDeadStorePcStats();
    const u32 wanted=p.Positive()?2*pairs-1:0;const auto changes=Changes(off,on);
    if(off.decoded_ir!=on.decoded_ir||a.FlagsResident()!=p.flags||b.FlagsResident()!=p.flags||
       a.CfgPlan().PhysicalRegisters()!=b.CfgPlan().PhysicalRegisters()||a.CfgPlan().Blocks().size()!=b.CfgPlan().Blocks().size())
        throw std::runtime_error("DSE input/budget/flags/CFG topology differs");
    if(old.attempted||old.accepted||old.stores_removed||old.removals_crossing_pc||!now.attempted||!now.accepted||
       now.stores_removed!=wanted||now.removals_crossing_pc!=wanted||now.pure_advance_pc_sites!=2*pairs+2)
    {
        std::ostringstream error;
        error << "DSE actual accepted cross-PC removal proof absent workload=" << p.name
              << " wanted=" << wanted << " removed=" << now.stores_removed
              << " cross_pc=" << now.removals_crossing_pc << " fences=" << now.fences
              << " pure_pc=" << now.pure_advance_pc_sites << " load_kills=" << now.load_kills
              << " accepted=" << now.accepted << " reason=" << int(now.rejection)
              << "\n" << off.decoded_ir;
        throw std::runtime_error(error.str());
    }
    if(off.census.stores!=on.census.stores+wanted||off.census.nops+wanted!=on.census.nops||off.census.pc!=on.census.pc)
        throw std::runtime_error("DSE actual IR stores/Nops/PC markers differ");
    const auto& u=a.Statistics().At(x64::TransferRegion::Uniform);const auto& v=b.Statistics().At(x64::TransferRegion::Uniform);
    if(u.scalar_store_sites!=v.scalar_store_sites+wanted||u.store_bytes!=v.store_bytes+8*wanted||u.vector_store_sites!=v.vector_store_sites||
       u.scalar_load_sites!=v.scalar_load_sites||u.vector_load_sites!=v.vector_load_sites||u.load_bytes!=v.load_bytes)
        throw std::runtime_error("DSE actual emitted Uniform store decrease absent");
    // EndMemory contributes exactly one common epilogue helper; none may occur
    // inside the overwrite region, and the pass cannot delete this boundary.
    if(a.Statistics().helper_call_sites!=1||b.Statistics().helper_call_sites!=1)
        throw std::runtime_error("DSE fixture EndMemory epilogue helper count differs");
    if(wanted ? off.prepared_ir==on.prepared_ir :
       (off.prepared_ir!=on.prepared_ir||changes.first||changes.second||a.getSize()!=b.getSize()||std::memcmp(a.getCode(),b.getCode(),a.getSize())))
        throw std::runtime_error("DSE positive mutation/negative identical code absent");
    std::cerr<<"dse_pc_fixture workload="<<p.name<<" flags="<<p.flags<<" decoded_input_equal=1 budget_equal=1 flags_policy_equal=1 stores_removed="<<wanted
        <<" crossing_pc="<<now.removals_crossing_pc<<" uniform_stores_off="<<u.scalar_store_sites+u.vector_store_sites<<" uniform_stores_on="<<v.scalar_store_sites+v.vector_store_sites
        <<" assignment_changes="<<changes.first<<" home_changes="<<changes.second<<" negative_control="<<!p.Positive()<<" negative_code_equal="<<!p.Positive()
        <<" code_bytes_off="<<a.getSize()<<" code_bytes_on="<<b.getSize()<<'\n';
}
void JsonMachine(std::ostream& out, const char* prefix, Machine& machine) {
    out << ",\"" << prefix << "_pc\":" << machine.state->current_loc.Value()
        << ",\"" << prefix << "_prev_pc\":" << machine.state->prev_loc.Value()
        << ",\"" << prefix << "_packed_flags\":" << machine.state->host_cpu_flags
        << ",\"" << prefix << "_state_hex\":\"";
    const char* digits="0123456789abcdef"; for(auto byte:machine.bytes) out << digits[byte>>4] << digits[byte&15]; out << "\"";
    out << ",\"" << prefix << "_x\":[";
    for (u32 i = 0; i < 29; ++i) { if (i) out << ','; out << machine.Guest().r[i]; }
    out << ',' << machine.Guest().fp << ',' << machine.Guest().lr << "]"
        << ",\"" << prefix << "_sp\":" << machine.Guest().sp << ",\"" << prefix << "_nzcv\":" << (machine.state->host_cpu_flags & nzcv_mask);
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
            if (units[route]->Entry()(machines[route].state) != HaltReason::CallHost) throw std::runtime_error("cross-PC DSE warmup exit");
        }
    }
    for (u32 repeat = 0; repeat < 7; ++repeat) for (u32 order = 0; order < route_count; ++order) {
        const u32 route = (order + repeat) % route_count; auto& machine = machines[route]; const auto entry = units[route]->Entry(); const auto begin = Clock::now();
        for (u32 call = 0; call < calls; ++call) {
            machine.Guest().r[28] = trips; machine.state->halt_reason = HaltReason::None;
            if (entry(machine.state) != HaltReason::CallHost) throw std::runtime_error("cross-PC DSE timing exit");
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
        const bool correctness=validate_only||oracle;
        if(!correctness&&argc>3)throw std::runtime_error("usage: benchmark [calls [trips]], --validate-only, --oracle-corpus NEW_FILE");
        const u32 calls=correctness||argc<2?500:PositiveArgument(argv[1]),trips=correctness||argc<3?64:PositiveArgument(argv[2]);
        std::cerr<<"scope="<<scope<<"; same_decoded_input=1; cfg_on=1; advpc_coalesce=0; terminal_brk_helper=1; flags_liveness_narrowing=0; state_promotion=0; uniform_forwarding=0; native_cf_chains=0; only_cross_pc_dse_policy_differs=1; allocation_homes_may_change=1; oracle=independent_full_context_and_state; transfer_counts=static_sites; application_performance_claim=0; dynamic_spill_claim=0\n";
        std::ostringstream corpus;bool first=true;if(oracle)corpus<<"{\"schema_version\":1,\"scope\":\""<<scope<<"\",\"cases\":[";
        if(!correctness)std::cout<<"workload,flags_residency,uniform_dead_store_cross_pc,guest_words,guest_blocks,cfg_blocks,trips,physical_registers,assigned_values,assignment_changes,home_changes,physical_home_slots,retained_home_slots,home_bytes,frame_bytes,code_bytes,decoded_ir_hash64,prepared_ir_hash64,allocation_hash64,homes_hash64,store_uniform_ir,nop_ir,advance_pc_ir,dse_attempted,dse_accepted,dse_stores_removed,dse_removals_crossing_pc,dse_pure_advance_pc_sites,dse_load_kills,flags_state_load_sites,flags_state_store_sites,scalar_cache_read_sites,scalar_cache_write_sites,observed_ssa_load_sites,observed_ssa_store_sites,ssa_load_bytes,ssa_store_bytes,observed_uniform_load_sites,observed_uniform_store_sites,uniform_load_bytes,uniform_store_bytes,helper_call_sites,preparation_full_allocation_builds,preparation_reused_final_allocation,forward_allocation_rounds,decode_plan_prepare_emit_us,native_ns_per_call,native_ns_per_guest_iteration,guest_instructions_per_second\n";
        for(u32 kind=0;kind<kind_names.size();++kind)for(bool flags:{false,true}) {
            auto program=MakeProgram(Kind(kind),flags);std::array<std::unique_ptr<Compiled>,route_count> units;std::array<std::vector<double>,route_count> preparation;
            if(!correctness)for(u32 repeat=0;repeat<9;++repeat)for(u32 order=0;order<route_count;++order){const u32 route=(order+repeat)%route_count;const auto begin=Clock::now();auto sample=Compile(program,route);preparation[route].push_back(std::chrono::duration<double,std::micro>(Clock::now()-begin).count());}
            for(u32 route=0;route<route_count;++route){units[route]=Compile(program,route);Validate(program,*units[route]);}Proof(program,units);
            if(oracle)OracleCases(corpus,program,units,first);if(correctness)continue;const auto ns=Measure(units,calls,trips);const auto changes=Changes(*units[0],*units[1]);
            for(u32 route=0;route<route_count;++route){const auto& unit=*units[route];const auto& code=*unit.code;const auto& cfg=code.CfgPlan();const auto& frame=code.FramePlan().Statistics();const auto& dse=code.UniformDeadStorePcStats();const auto& s=code.Statistics();const auto& ssa=s.At(x64::TransferRegion::CanonicalSSA);const auto& uniform=s.At(x64::TransferRegion::Uniform);
                const auto assigned=std::count_if(cfg.Allocations().begin(),cfg.Allocations().end(),[](const auto& a){return a.slot.has_value();});
                std::cout<<std::fixed<<std::setprecision(2)<<(program.name)<<','<<(program.flags)<<','<<(route)<<','<<(program.words.size())<<','<<(unit.guest_blocks)<<','<<(cfg.Blocks().size())<<','<<(trips)<<','<<(cfg.PhysicalRegisters().size())<<','<<(assigned)<<','<<(route?changes.first:0)<<','<<(route?changes.second:0)<<','<<(frame.home_count)<<','<<(frame.retained_homes)<<','<<(frame.home_bytes)<<','<<(code.FrameSize())<<','<<(code.getSize())<<','<<(Hash(unit.decoded_ir))<<','<<(Hash(unit.prepared_ir))<<','<<(unit.census.allocation_hash)<<','<<(unit.census.homes_hash)<<','<<(unit.census.stores)<<','<<(unit.census.nops)<<','<<(unit.census.pc)<<','<<(dse.attempted)<<','<<(dse.accepted)<<','<<(dse.stores_removed)<<','<<(dse.removals_crossing_pc)<<','<<(dse.pure_advance_pc_sites)<<','<<(dse.load_kills)<<','<<(s.flags_state_load_sites)<<','<<(s.flags_state_store_sites)<<','<<(s.scalar_cache_read_sites)<<','<<(s.scalar_cache_write_sites)<<','<<(ssa.scalar_load_sites+ssa.vector_load_sites)<<','<<(ssa.scalar_store_sites+ssa.vector_store_sites)<<','<<(ssa.load_bytes)<<','<<(ssa.store_bytes)<<','<<(uniform.scalar_load_sites+uniform.vector_load_sites)<<','<<(uniform.scalar_store_sites+uniform.vector_store_sites)<<','<<(uniform.load_bytes)<<','<<(uniform.store_bytes)<<','<<(s.helper_call_sites)<<','<<(code.PreparationStats().full_allocation_builds)<<','<<(code.PreparationStats().reused_final_allocation)<<','<<(code.UniformForwardStats().allocation_rounds)<<','<<(Median(preparation[route]))<<','<<(ns[route])<<','<<(ns[route]/trips)<<','<<(double(program.Instructions(trips))*1e9/ns[route])<<'\n';
            }
        }
        if(oracle){corpus<<"]}\n";SaveExclusive(argv[2],corpus.str());std::cerr<<"oracle_cases=48; exclusive_corpus=1\n";}
        if(correctness)std::cerr<<"validated_routes=8; seeds_per_route=3; trip_counts=1,2,7,19; actual_cross_pc_removals=1; actual_uniform_stores_decrease=1; negative_code_equal=1; allocation_homes_recorded=1\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"cross-PC DSE benchmark: "<<error.what()<<'\n';return 1;}
}
