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
constexpr const char* scope = "decoded_a64_native_cf_chain_pair";
constexpr std::array<const char*, route_count> route_names{"off", "on"};
enum class Kind { AddPositive, SubXorNegative, BarrierNegative, HelperNegative, ObserverNegative };
constexpr std::array<const char*, 5> kind_names{"adds_adcs", "subs_sbcs_xor_barrier", "adds_dmb_adcs", "adds_helper_adcs", "adds_mrs_adcs"};
struct Program final : MemoryInterface {
    std::string name; Kind kind{}; bool wide{}, flags{}; std::vector<u32> words;
    bool Positive() const { return kind == Kind::AddPositive; }
    bool InjectedHelper() const { return kind == Kind::HelperNegative; }
    u32 PerPair() const { return kind == Kind::BarrierNegative || kind == Kind::HelperNegative || kind == Kind::ObserverNegative ? 3 : 2; }
    u64 StopPc() const { return code_base + (words.size()-1)*4; }
    u64 Instructions(u32 trips) const { return u64(trips)*(pairs*PerPair()+2)+2; }
    bool Read(void* out, size_t address, size_t size) override {
        if (address < code_base || address-code_base > words.size()*4 || size > words.size()*4-(address-code_base)) return false;
        std::memcpy(out, reinterpret_cast<const u8*>(words.data())+address-code_base,size); return true;
    }
    bool Write(void*, size_t, size_t) override { return false; }
    void* GetPointer(void* address) override {
        const auto at = reinterpret_cast<uintptr_t>(address);
        if (at < code_base || at-code_base > words.size()*4 || words.size()*4-(at-code_base)<4) return nullptr;
        return reinterpret_cast<u8*>(words.data())+at-code_base;
    }
};
Program MakeProgram(Kind kind, bool wide, bool flags) {
    Program p; p.kind=kind; p.wide=wide; p.flags=flags; p.name=std::string{kind_names[u32(kind)]}+(wide ? "_x" : "_w");
    for (u32 pair=0; pair<pairs; ++pair) {
        // ADDS/SUBS R0,R0,R1 ; ADCS/SBCS R3,R3,R5. The native helper
        // negative case injects a State-neutral helper at the guest NOP;
        // all other inventories are exact unmodified decoder output.
        const bool sub=kind==Kind::SubXorNegative;
        p.words.push_back((wide ? (sub?0xeb010000u:0xab010000u) : (sub?0x6b010000u:0x2b010000u)));
        if (kind==Kind::BarrierNegative) p.words.push_back(0xd5033bbfu); // DMB ISH
        if (kind==Kind::HelperNegative) p.words.push_back(0xd503201fu); // guest NOP, explicit native boundary injection
        if (kind==Kind::ObserverNegative) p.words.push_back(0xd53b420fu); // MRS X15,NZCV
        p.words.push_back(wide ? (sub?0xfa050063u:0xba050063u) : (sub?0x7a050063u:0x3a050063u));
    }
    p.words.push_back(0xd100079cu); // SUB X28,X28,#1, no flags
    p.words.push_back(0xb5000000u | (((0u-u32(p.words.size()))&0x7ffffu)<<5) | 28u); // CBNZ X28,0x1000
    p.words.push_back(0x14000001u); p.words.push_back(0xd4200000u); return p;
}
u64 ClobberHelper(u64,u64,u64) {
    asm volatile("xor %%r8d,%%r8d\n\txor %%r9d,%%r9d\n\txor %%r10d,%%r10d\n\txor %%r11d,%%r11d" ::: "r8","r9","r10","r11","cc"); return 0;
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
struct Arithmetic { u64 value{}, nzcv{}; };
Arithmetic Math(u64 lhs, u64 rhs, u64 carry, bool sub, bool wide) {
    const u32 bits=wide?64:32; const u64 mask=wide?UINT64_MAX:UINT32_MAX; lhs&=mask; rhs&=mask;
    const auto sum=static_cast<unsigned __int128>(lhs)+(sub ? (~rhs&mask):rhs)+carry; const u64 value=u64(sum)&mask;
    const auto sign=[&](u64 a)->__int128{return a&(u64{1}<<(bits-1)) ? __int128(a)-(__int128{1}<<bits):__int128(a);};
    const auto signed_sum=sub ? sign(lhs)-sign(rhs)-(1-carry):sign(lhs)+sign(rhs)+carry;
    const bool overflow=signed_sum < -(__int128{1}<<(bits-1)) || signed_sum >= (__int128{1}<<(bits-1));
    return {value,(u64(bool(value&(u64{1}<<(bits-1))))<<31)|(u64(value==0)<<30)|(u64(bool(sum>>bits))<<29)|(u64(overflow)<<28)};
}
void Model(const Program& p, Machine& machine, u32 trips) {
    auto& regs=machine.Guest().r; auto& flags=machine.state->host_cpu_flags;
    for(u32 trip=0;trip<trips;++trip) {
        for(u32 pair=0;pair<pairs;++pair) {
            const bool sub=p.kind==Kind::SubXorNegative;
            const auto first=Math(regs[0],regs[1],sub?1:0,sub,p.wide); regs[0]=first.value; flags=(flags&~nzcv_mask)|first.nzcv;
            if(p.kind==Kind::ObserverNegative) regs[15]=first.nzcv;
            const auto second=Math(regs[3],regs[5],(first.nzcv>>29)&1,sub,p.wide); regs[3]=second.value; flags=(flags&~nzcv_mask)|second.nzcv;
        }
        --regs[28];
    }
    machine.Guest().padding=u32(swift::arm64::InterruptReason::BRK);
    machine.state->current_loc=ir::Location{p.StopPc()}; machine.state->prev_loc=ir::Location{p.StopPc()-4}; machine.state->halt_reason=HaltReason::CallHost;
}
struct Compiled {
    Config config{.backend_isa=kX86_64,.uniform_buffer_size=sizeof(Context),.stack_alignment=16};
    backend::FunctionPlan plan; std::unique_ptr<x64::Translator> code; u32 route{},guest_blocks{};
    std::string fixed_ir; Fn Entry()const{return code->getCode<Fn>();}
};
std::unique_ptr<Compiled> Compile(Program& p,u32 route) {
    auto unit=std::make_unique<Compiled>(); unit->route=route;
    ir::HIRBuilder builder{1,true}; auto* function=builder.AppendFunction(ir::Location{code_base});
    for(;;) {
        std::vector<u64> pending;
        for(auto& hir:function->GetHIRBlockList()) { auto* body=hir.GetBlock(); if(body->GetInstList().empty()&&!body->HasTerminal()) pending.push_back(body->GetStartLocation().Value()); }
        if(pending.empty())break; if(unit->guest_blocks+pending.size()>8)throw std::runtime_error("carry HIR cap");
        for(auto pc:pending){builder.SetCurBlock(pc);ir::Assembler as{&builder};swift::arm64::A64Decoder{pc,&p,&as}.Decode();++unit->guest_blocks;}
    }
    function->EndFunction();std::vector<ir::HIRBlock*> ordered;
    for(auto* hir:function->GetHIRBlocks())if(hir&&hir->GetBlock()->GetStartLocation()==ir::Location{code_base})ordered.push_back(hir);
    for(auto* hir:function->GetHIRBlocks())if(hir&&hir->GetBlock()->GetStartLocation()!=ir::Location{code_base}&&hir->GetBlock()->GetStartLocation()!=ir::Location::INVALID&&hir->GetBlock()->HasTerminal())ordered.push_back(hir);
    // Carry-liveness narrowing, constant folding, state promotion and
    // forwarding are deliberately fixed OFF so every full NZCV marker stays
    // available and decoded SBCS retains its current real XOR barrier.
    unit->plan=backend::FunctionPlan::Build(function,ordered);
    if(p.InjectedHelper()) {
        std::vector<ir::Inst*> first_markers;u32 publication{};
        for(auto& inst:unit->plan.body->GetInstList())if(inst.GetOp()==ir::OpCode::SaveFlags){if(!(publication++&1))first_markers.push_back(&inst);}
        if(first_markers.size()!=pairs)throw std::runtime_error("helper injection first-publication inventory");
        std::vector<ir::Inst*> original;for(auto& inst:unit->plan.body->GetInstList())original.push_back(&inst);
        for(auto* marker:first_markers){
            auto next=std::find(original.begin(),original.end(),marker);ir::Inst* anchor{};
            for(++next;next!=original.end();++next){if((*next)->GetOp()==ir::OpCode::AdvancePC){anchor=*next;break;}if((*next)->GetOp()==ir::OpCode::SaveFlags)break;}
            if(!anchor)throw std::runtime_error("helper boundary lacks first retirement PC marker");
            auto* helper=new ir::Inst(ir::OpCode::CallLambda);helper->SetArgs(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&ClobberHelper)}});helper->SetReturn(ir::ValueType::U64);unit->plan.body->InsertAfter(helper,anchor);
        }
    }
    unit->fixed_ir=unit->plan.body->ToString();
    unit->code=x64::Translator::PrepareFunction(unit->plan,x64::HostFeatures::Detect(),&unit->config,true,true,p.flags,
        {.uniform_forwarding=false,.state_promotion=false,.scalar_rematerialization=true,.scalar_binary_immediates=true,
         .loop_weighted_costs=true,.profitable_flags_residency=false,.scalar_unary_phases=true,.scalar_unary_coalescing=true,
         .scalar_shift_immediates=true,.scalar_unary_mask_elision=true,.native_cf_chains=bool(route)});
    if(unit->fixed_ir!=unit->plan.body->ToString())throw std::runtime_error("carry preparation changed fixed IR");unit->code->Translate();
    if(!unit->code->CfgPlan().Validate()||!unit->code->FramePlan().Enabled()||unit->guest_blocks!=3||unit->code->NativeCfChainsEnabled()!=bool(route)||
       !unit->code->LoopWeightedCostsEnabled()||!unit->code->ScalarUnaryPhasesEnabled()||!unit->code->ScalarUnaryCoalescingEnabled()||!unit->code->ScalarShiftImmediatesEnabled()||!unit->code->ScalarUnaryMaskElisionEnabled())throw std::runtime_error("carry validated CFG/fixed policy absent");
    return unit;
}
void Validate(const Program& p,const Compiled& unit) {
    for(u64 seed:{u64{0},UINT64_MAX,u64{0x123456789abcdef0}})for(u32 trips:{1u,2u,7u,19u}) {
        Machine actual,expected;actual.Init(seed,trips);expected.Init(seed,trips);Model(p,expected,trips);const auto saved=_mm_getcsr();
        const auto reason=unit.Entry()(actual.state);
        if(reason!=HaltReason::CallHost||_mm_getcsr()!=saved||actual.bytes!=expected.bytes){std::ostringstream out;out<<p.name<<" full State mismatch route="<<unit.route<<" seed="<<seed<<" trips="<<trips;for(u32 i=0;i<actual.bytes.size();++i)if(actual.bytes[i]!=expected.bytes[i])out<<" byte="<<i<<":"<<u32(actual.bytes[i])<<"/"<<u32(expected.bytes[i]);throw std::runtime_error(out.str());}
    }
}
void Proof(const Program& program, std::array<std::unique_ptr<Compiled>, route_count>& units) {
    auto& off = *units[0]; auto& on = *units[1];
    const auto& a = off.code->CfgPlan(); const auto& b = on.code->CfgPlan();
    const auto& pa = off.code->PreparationStats(); const auto& pb = on.code->PreparationStats();
    if (pa.full_allocation_builds != pb.full_allocation_builds || pa.reused_final_allocation != pb.reused_final_allocation ||
        pa.flags_policy_allocation_builds != pb.flags_policy_allocation_builds ||
        pa.flags_policy_allocation_recolors != pb.flags_policy_allocation_recolors)
        throw std::runtime_error("carry emission preparation census differs");
    const auto ordinal = [](const auto& cfg, const ir::Inst* inst) {
        return std::find(cfg.Instructions().begin(), cfg.Instructions().end(), inst) - cfg.Instructions().begin();
    };
    if (off.fixed_ir != on.fixed_ir || off.code->FlagsResident() != program.flags || on.code->FlagsResident() != program.flags ||
        a.PhysicalRegisters() != b.PhysicalRegisters() || a.Blocks().size() != b.Blocks().size() || a.Edges().size() != b.Edges().size() ||
        a.Instructions().size() != b.Instructions().size() || a.Allocations().size() != b.Allocations().size())
        throw std::runtime_error("carry emission pair changed fixed IR/flags/budget/graph");
    for (u32 i = 0; i < a.Blocks().size(); ++i) {
        const auto& x = a.Blocks()[i]; const auto& y = b.Blocks()[i];
        if (x.begin != y.begin || x.end != y.end || x.reachable != y.reachable || x.incoming_edges != y.incoming_edges ||
            x.outgoing_edges != y.outgoing_edges || x.loop_depth != y.loop_depth || x.loop_weight != y.loop_weight ||
            x.live_in.size() != y.live_in.size() || x.live_out.size() != y.live_out.size())
            throw std::runtime_error("carry emission block topology differs");
        for (u32 j = 0; j < x.live_in.size(); ++j)
            if (ordinal(a,x.live_in[j]) != ordinal(b,y.live_in[j])) throw std::runtime_error("carry emission live-in differs");
        for (u32 j = 0; j < x.live_out.size(); ++j)
            if (ordinal(a,x.live_out[j]) != ordinal(b,y.live_out[j])) throw std::runtime_error("carry emission live-out differs");
    }
    for (u32 i = 0; i < a.Edges().size(); ++i) {
        const auto& x = a.Edges()[i]; const auto& y = b.Edges()[i];
        if (x.from != y.from || x.to != y.to || x.taken != y.taken || x.host_entry != y.host_entry ||
            x.copies.size() != y.copies.size() || x.live.size() != y.live.size())
            throw std::runtime_error("carry emission edge topology differs");
        for (u32 j = 0; j < x.live.size(); ++j)
            if (ordinal(a,x.live[j]) != ordinal(b,y.live[j])) throw std::runtime_error("carry emission edge liveness differs");
        for (u32 j = 0; j < x.copies.size(); ++j) {
            const auto& left = x.copies[j]; const auto& right = y.copies[j];
            if (ordinal(a,left.destination) != ordinal(b,right.destination) || left.source.IsValue() != right.source.IsValue() ||
                left.source.IsImm() != right.source.IsImm() ||
                (left.source.IsValue() && (ordinal(a,left.source.value.Def()) != ordinal(b,right.source.value.Def()) || left.source.value.Type() != right.source.value.Type())) ||
                (left.source.IsImm() && left.source.imm.Get() != right.source.imm.Get()))
                throw std::runtime_error("carry emission parallel Phi copy differs");
        }
    }
    for (u32 i = 0; i < a.Allocations().size(); ++i) {
        const auto& x = a.Allocations()[i]; const auto& y = b.Allocations()[i];
        if (ordinal(a,x.producer) != ordinal(b,y.producer) || x.slot != y.slot || x.requires_canonical_home != y.requires_canonical_home ||
            x.uses != y.uses || x.scalar_reads != y.scalar_reads || x.estimated_benefit != y.estimated_benefit ||
            x.requires_full_container != y.requires_full_container || x.weighted_reads != y.weighted_reads || x.weighted_benefit != y.weighted_benefit)
            throw std::runtime_error("carry emission coloring/home obligation differs");
    }
    for (u32 i = 0; i < a.Instructions().size(); ++i) {
        const auto* x = a.Instructions()[i]; const auto* y = b.Instructions()[i];
        if (off.code->FramePlan().Home(x) != on.code->FramePlan().Home(y) ||
            off.code->FramePlan().Decision(x) != on.code->FramePlan().Decision(y) ||
            off.code->FramePlan().RematerializedValue(x) != on.code->FramePlan().RematerializedValue(y))
            throw std::runtime_error("carry emission every-instruction home map differs");
        for (u32 j = 0; j < a.Instructions().size(); ++j)
            if (a.Interferes(x,a.Instructions()[j]) != b.Interferes(y,b.Instructions()[j]) ||
                a.CanReuseUnaryInput(x,a.Instructions()[j]) != b.CanReuseUnaryInput(y,b.Instructions()[j]) ||
                a.CanReuseBinaryInput(x,a.Instructions()[j]) != b.CanReuseBinaryInput(y,b.Instructions()[j]))
                throw std::runtime_error("carry emission interference/reuse proof differs");
    }
    const auto& x=off.code->Statistics();const auto& y=on.code->Statistics();const u64 wanted=program.Positive()?pairs:0;
    if(x.native_cf_forward_sites||x.native_cf_merge_omissions||x.native_cf_state_bt_omissions||x.native_cf_resident_bt_omissions||
       y.native_cf_forward_sites!=wanted||y.native_cf_merge_omissions!=wanted||
       y.native_cf_state_bt_omissions!=(program.flags?0:wanted)||y.native_cf_resident_bt_omissions!=(program.flags?wanted:0)||y.native_cf_polarity_cmc_sites)
        throw std::runtime_error("carry actual forwarding/packed merge/BT evidence differs");
    if(x.flags_state_carry_read_sites!=y.flags_state_carry_read_sites+y.native_cf_state_bt_omissions||
       x.flags_resident_carry_read_sites!=y.flags_resident_carry_read_sites+y.native_cf_resident_bt_omissions||
       x.flags_resident_merge_sites!=y.flags_resident_merge_sites+(program.flags?wanted:0)||
       x.flags_state_load_sites!=y.flags_state_load_sites+(program.flags?0:wanted)||
       x.flags_state_store_sites!=y.flags_state_store_sites+(program.flags?0:wanted))throw std::runtime_error("carry actual packed/BT reductions absent");
    if(off.code->FrameSize()!=on.code->FrameSize()||(wanted ? on.code->getSize()>=off.code->getSize() : (on.code->getSize()!=off.code->getSize()||std::memcmp(on.code->getCode(),off.code->getCode(),off.code->getSize()))))throw std::runtime_error("carry positive shrink/negative identical code absent");
    const auto& old_metadata=x.At(x64::TransferRegion::ProducerMetadata);const auto& new_metadata=y.At(x64::TransferRegion::ProducerMetadata);
    if(old_metadata.scalar_store_sites!=new_metadata.scalar_store_sites||old_metadata.vector_store_sites!=new_metadata.vector_store_sites||old_metadata.store_bytes!=new_metadata.store_bytes||
       old_metadata.scalar_load_sites!=new_metadata.scalar_load_sites+wanted||old_metadata.load_bytes!=new_metadata.load_bytes+8*wanted||old_metadata.vector_load_sites!=new_metadata.vector_load_sites)
        throw std::runtime_error("carry snapshot publication fixed/removed metadata reads differ");
    for(u32 region=0;region<u32(x64::TransferRegion::Count);++region){if(region==u32(x64::TransferRegion::StateFlags)||region==u32(x64::TransferRegion::ProducerMetadata))continue;
        const auto& l=x.transfers[region];const auto& r=y.transfers[region];if(l.scalar_load_sites!=r.scalar_load_sites||l.scalar_store_sites!=r.scalar_store_sites||l.vector_load_sites!=r.vector_load_sites||l.vector_store_sites!=r.vector_store_sites||l.load_bytes!=r.load_bytes||l.store_bytes!=r.store_bytes)throw std::runtime_error("carry fixed memory/home census differs");}
#define SAME(field) if(x.field!=y.field)throw std::runtime_error("carry fixed statistic changed: " #field)
    SAME(helper_call_sites);SAME(flags_snapshot_sites);SAME(flags_materialization_ir_sites);SAME(flags_resident_update_sites);
    SAME(flags_resident_read_sites);SAME(flags_resident_write_sites);
    SAME(phi_value_copy_sites);SAME(phi_immediate_copy_sites);SAME(direct_phi_copy_sites);SAME(scalar_cache_read_sites);SAME(scalar_cache_write_sites);
    SAME(scalar_coalesced_binary_sites);SAME(scalar_coalesced_unary_sites);SAME(scalar_high_word_store_omissions);SAME(scalar_rematerialized_read_sites);SAME(scalar_rematerialized_definition_sites);
    SAME(scalar_binary_immediate_sites);SAME(scalar_binary_rematerialized_immediate_sites);SAME(scalar_shift_immediate_sites);SAME(scalar_shift_zero_sites);SAME(frame_bytes);SAME(unresolved_transfer_width_sites);
#undef SAME
    if(program.InjectedHelper()&&!x.helper_call_sites)throw std::runtime_error("helper negative fixture lacks actual native call");
    std::cerr<<"carry_fixture workload="<<program.name<<" flags="<<program.flags<<" fixed_allocation=1 fixed_homes=1 fixed_cache_mov=1 forward_sites="<<wanted
        <<" merge_omissions="<<y.native_cf_merge_omissions<<" bt_omissions="<<y.native_cf_state_bt_omissions+y.native_cf_resident_bt_omissions
        <<" negative_control="<<!program.Positive()<<" helper_injection="<<program.InjectedHelper()<<" code_bytes_off="<<off.code->getSize()<<" code_bytes_on="<<on.code->getSize()<<'\n';
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
            if (units[route]->Entry()(machines[route].state) != HaltReason::CallHost) throw std::runtime_error("carry chain warmup exit");
        }
    }
    for (u32 repeat = 0; repeat < 7; ++repeat) for (u32 order = 0; order < route_count; ++order) {
        const u32 route = (order + repeat) % route_count; auto& machine = machines[route]; const auto entry = units[route]->Entry(); const auto begin = Clock::now();
        for (u32 call = 0; call < calls; ++call) {
            machine.Guest().r[28] = trips; machine.state->halt_reason = HaltReason::None;
            if (entry(machine.state) != HaltReason::CallHost) throw std::runtime_error("carry chain timing exit");
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
        std::cerr<<"scope="<<scope<<"; fixed_ir_before_policy=1; cfg_on=1; flags_liveness_narrowing=0; state_promotion=0; uniform_forwarding=0; only_native_cf_policy_differs=1; oracle=independent_full_context_and_state; transfer_counts=static_sites; application_performance_claim=0; dynamic_spill_claim=0\n";
        std::ostringstream corpus;bool first=true;if(oracle)corpus<<"{\"schema_version\":1,\"scope\":\""<<scope<<"\",\"cases\":[";
        if(!correctness)std::cout<<"workload,flags_residency,native_cf_chains,guest_words,guest_blocks,cfg_blocks,trips,physical_registers,assigned_values,physical_home_slots,retained_home_slots,home_bytes,frame_bytes,code_bytes,native_cf_forward_sites,native_cf_merge_omissions,native_cf_state_bt_omissions,native_cf_resident_bt_omissions,native_cf_polarity_cmc_sites,flags_state_carry_read_sites,flags_resident_carry_read_sites,flags_state_load_sites,flags_state_store_sites,flags_resident_merge_sites,flags_snapshot_sites,scalar_cache_read_sites,scalar_cache_write_sites,observed_ssa_load_sites,observed_ssa_store_sites,ssa_load_bytes,ssa_store_bytes,observed_uniform_load_sites,observed_uniform_store_sites,helper_call_sites,preparation_full_allocation_builds,preparation_reused_final_allocation,forward_allocation_rounds,decode_plan_prepare_emit_us,native_ns_per_call,native_ns_per_guest_iteration,guest_instructions_per_second\n";
        for(u32 kind=0;kind<kind_names.size();++kind)for(bool wide:{false,true})for(bool flags:{false,true}) {
            auto program=MakeProgram(Kind(kind),wide,flags);std::array<std::unique_ptr<Compiled>,route_count> units;std::array<std::vector<double>,route_count> preparation;
            if(!correctness)for(u32 repeat=0;repeat<9;++repeat)for(u32 order=0;order<route_count;++order){const u32 route=(order+repeat)%route_count;const auto begin=Clock::now();auto sample=Compile(program,route);preparation[route].push_back(std::chrono::duration<double,std::micro>(Clock::now()-begin).count());}
            for(u32 route=0;route<route_count;++route){units[route]=Compile(program,route);Validate(program,*units[route]);}Proof(program,units);
            if(oracle)OracleCases(corpus,program,units,first);if(correctness)continue;const auto ns=Measure(units,calls,trips);
            for(u32 route=0;route<route_count;++route){const auto& unit=*units[route];const auto& code=*unit.code;const auto& cfg=code.CfgPlan();const auto& frame=code.FramePlan().Statistics();const auto& s=code.Statistics();const auto& ssa=s.At(x64::TransferRegion::CanonicalSSA);const auto& uniform=s.At(x64::TransferRegion::Uniform);
                const auto assigned=std::count_if(cfg.Allocations().begin(),cfg.Allocations().end(),[](const auto& a){return a.slot.has_value();});
                std::cout<<std::fixed<<std::setprecision(2)<<program.name<<','<<program.flags<<','<<route<<','<<program.words.size()<<','<<unit.guest_blocks<<','<<cfg.Blocks().size()<<','<<trips<<','<<cfg.PhysicalRegisters().size()<<','<<assigned<<','<<frame.home_count<<','<<frame.retained_homes<<','<<frame.home_bytes<<','<<code.FrameSize()<<','<<code.getSize()
                    <<','<<s.native_cf_forward_sites<<','<<s.native_cf_merge_omissions<<','<<s.native_cf_state_bt_omissions<<','<<s.native_cf_resident_bt_omissions<<','<<s.native_cf_polarity_cmc_sites<<','<<s.flags_state_carry_read_sites<<','<<s.flags_resident_carry_read_sites<<','<<s.flags_state_load_sites<<','<<s.flags_state_store_sites<<','<<s.flags_resident_merge_sites<<','<<s.flags_snapshot_sites<<','<<s.scalar_cache_read_sites<<','<<s.scalar_cache_write_sites
                    <<','<<ssa.scalar_load_sites+ssa.vector_load_sites<<','<<ssa.scalar_store_sites+ssa.vector_store_sites<<','<<ssa.load_bytes<<','<<ssa.store_bytes<<','<<uniform.scalar_load_sites+uniform.vector_load_sites<<','<<uniform.scalar_store_sites+uniform.vector_store_sites<<','<<s.helper_call_sites<<','<<code.PreparationStats().full_allocation_builds<<','<<code.PreparationStats().reused_final_allocation<<','<<code.UniformForwardStats().allocation_rounds
                    <<','<<Median(preparation[route])<<','<<ns[route]<<','<<ns[route]/trips<<','<<double(program.Instructions(trips))*1e9/ns[route]<<'\n';}
        }
        if(oracle){corpus<<"]}\n";SaveExclusive(argv[2],corpus.str());std::cerr<<"oracle_cases=240; exclusive_corpus=1\n";}
        if(correctness)std::cerr<<"validated_routes=40; seeds_per_route=3; trip_counts=1,2,7,19; fixed_allocation_and_homes=1; actual_positive_forward_and_merge_and_bt=1; negative_code_equal=1; helper_boundary_injected=1\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"carry chain benchmark: "<<error.what()<<'\n';return 1;}
}
