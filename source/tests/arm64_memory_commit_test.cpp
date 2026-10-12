#include <array>
#include <cstring>
#include <memory>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#if defined(SWIFT_HAS_X64_BACKEND)
#include <sys/mman.h>
#include <unistd.h>
#include <xmmintrin.h>
#include "runtime/backend/address_space.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/x64/translator.h"
#endif

using namespace swift;
using namespace swift::runtime;
using namespace vixl::aarch64;
namespace {
using Context=swift::arm64::ThreadContext64;
using Bytes=std::array<u8,128>;
struct Form { u32 opcode,bytes;bool load,simd,sign,dest64; };
const std::array<Form,23> Singles{{
    {LDRB_w,1,true,false,false,false},{LDRH_w,2,true,false,false,false},{LDR_w,4,true,false,false,false},{LDR_x,8,true,false,false,true},
    {LDRSB_w,1,true,false,true,false},{LDRSH_w,2,true,false,true,false},{LDRSB_x,1,true,false,true,true},{LDRSH_x,2,true,false,true,true},{LDRSW_x,4,true,false,true,true},
    {STRB_w,1,false,false,false,false},{STRH_w,2,false,false,false,false},{STR_w,4,false,false,false,false},{STR_x,8,false,false,false,true},
    {LDR_b,1,true,true,false,false},{LDR_h,2,true,true,false,false},{LDR_s,4,true,true,false,false},{LDR_d,8,true,true,false,false},{LDR_q,16,true,true,false,false},
    {STR_b,1,false,true,false,false},{STR_h,2,false,true,false,false},{STR_s,4,false,true,false,false},{STR_d,8,false,true,false,false},{STR_q,16,false,true,false,false}}};
const std::array<Form,11> Pairs{{
    {LDP_w,4,true,false,false,false},{LDP_x,8,true,false,false,true},{LDPSW_x,4,true,false,true,true},
    {STP_w,4,false,false,false,false},{STP_x,8,false,false,false,true},
    {LDP_s,4,true,true,false,false},{LDP_d,8,true,true,false,false},{LDP_q,16,true,true,false,false},
    {STP_s,4,false,true,false,false},{STP_d,8,false,true,false,false},{STP_q,16,false,true,false,false}}};
u64& Gpr(Context& c,u32 reg) { return reg<29 ? c.r[reg] : reg==29 ? c.fp : reg==30 ? c.lr : c.sp; }
u64 ReadGpr(const Context& c,u32 reg) { return reg==31 ? 0 : reg<29 ? c.r[reg] : reg==29 ? c.fp : c.lr; }
Context Initial() {
    Context c{};for(u32 reg=0;reg<31;++reg) Gpr(c,reg)=0x80FFEE1234567890+u64(reg)*0x0101010101010101;
    c.sp=48;c.fpcr=0xFFFFFFFF;c.fpsr=0x0800009F;c.pc=0x8877665544332211;c.padding=0xABCD;
    for(u32 reg=0;reg<32;++reg) { const std::array<u64,2> value{0x987654321001FF80+reg,0xFEDCBA1234567890-reg};std::memcpy(&c.v[reg],value.data(),16); }
    return c;
}
Bytes Memory() { Bytes bytes{};for(u32 n=0;n<bytes.size();++n) bytes[n]=u8(0x80+n*13);return bytes; }
// Exact byte transfers and integer sign extension, with no runtime IR helpers.
void Load(Context& c,const Form& form,u32 reg,const u8* bytes) {
    if(form.simd) { std::memset(&c.v[reg],0,16);std::memcpy(&c.v[reg],bytes,form.bytes);return; }
    u64 value=0;for(u32 byte=0;byte<form.bytes;++byte) value|=u64(bytes[byte])<<(8*byte);
    if(form.sign && (value&(u64{1}<<(form.bytes*8-1)))) value|=~((u64{1}<<(form.bytes*8))-1);
    if(!form.dest64) value=u32(value);if(reg!=31) Gpr(c,reg)=value;
}
void Store(const Context& c,const Form& form,u32 reg,u8* bytes) {
    if(form.simd) { std::memcpy(bytes,&c.v[reg],form.bytes);return; }
    const u64 value=ReadGpr(c,reg);for(u32 byte=0;byte<form.bytes;++byte) bytes[byte]=u8(value>>(8*byte));
}
u32 Encode(const Form& form,bool pair,u32 mode,s32 delta,u32 rn,u32 rt,u32 rt2=2) {
    const u32 base=pair ? (mode==1 ? 0x29800000 : mode==2 ? 0x28800000 : 0x29000000)
                        : (mode==1 ? 0x38000C00 : mode==2 ? 0x38000400 : 0x38000000);
    const u32 immediate=pair ? (u32(delta/s32(form.bytes))&127)<<15 : (u32(delta)&511)<<12;
    return base|form.opcode|immediate|(rn<<5)|rt|(pair ? rt2<<10 : 0);
}
class Code final:public MemoryInterface {
public:
    explicit Code(u32 word):words{0xD503201F,word,0xD4200000}{}
    bool Read(void* dst,size_t address,size_t size)override {
        if(address<0x1000 || address-0x1000>sizeof(words) || size>sizeof(words)-(address-0x1000))return false;
        std::memcpy(dst,reinterpret_cast<u8*>(words.data())+address-0x1000,size);return true;
    }
    bool Write(void*,size_t,size_t)override{return false;}
    void* GetPointer(void* ptr)override{const auto a=reinterpret_cast<size_t>(ptr);return a>=0x1000&&a-0x1000<sizeof(words)?reinterpret_cast<u8*>(words.data())+a-0x1000:nullptr;}
    std::array<u32,3>words;
};
struct Machine {
    alignas(backend::State)std::array<u8,sizeof(backend::State)+sizeof(Context)+32>storage{};
    backend::State*state=new(storage.data())backend::State{};
    ~Machine(){state->~State();}
    Context& Guest(){return *reinterpret_cast<Context*>(state->uniform_buffer_begin);}
};
struct Program {
    Code code;ir::Block block{0,0x1000};ir::Assembler assembler{&block};
#if defined(SWIFT_HAS_X64_BACKEND)
    std::array<std::unique_ptr<backend::x64::Translator>,2>native;
#endif
    explicit Program(u32 word):code(word){swift::arm64::A64Decoder{0x1000,&code,&assembler}.Decode();
#if defined(SWIFT_HAS_X64_BACKEND)
        for(u32 n=0;n<2;++n){native[n]=std::make_unique<backend::x64::Translator>(&block,n?backend::x64::HostFeatures{}:backend::x64::HostFeatures::Detect());native[n]->Translate();}
#endif
    }
    void Check(const Context& initial,const Bytes& memory,const Context& expected,const Bytes& wanted,u64 limit,HaltReason halt,u64 pc){
        for(u32 route=0;route<
#if defined(SWIFT_HAS_X64_BACKEND)
            3
#else
            1
#endif
            ;++route){
            Machine machine;machine.Guest()=initial;Bytes actual=memory;machine.state->pt=actual.data();machine.state->guest_addr_limit=limit;
            machine.state->current_loc=ir::Location{0xDEAD0000};machine.state->host_cpu_flags=0xB0000000;
#if defined(SWIFT_HAS_X64_BACKEND)
            const u32 original=_mm_getcsr(),changed=(original&~0x6000u)|0x6000u;_mm_setcsr(changed);
#endif
            HaltReason reason=HaltReason::None;
            if(!route)reason=backend::interp::Interpreter{*machine.state,&block}.Run();
#if defined(SWIFT_HAS_X64_BACKEND)
            else reason=native[route-1]->getCode<HaltReason(*)(backend::State*)>()(machine.state);
            const u32 restored=_mm_getcsr();_mm_setcsr(original);REQUIRE(restored==changed);
#endif
            INFO("instruction="<<code.words[1]<<" route="<<route<<" limit="<<limit);
            std::string context_difference;
            for(size_t byte=0;byte<sizeof(expected);++byte)
                if(reinterpret_cast<const u8*>(&machine.Guest())[byte]!=reinterpret_cast<const u8*>(&expected)[byte])
                    context_difference+=" byte="+std::to_string(byte)+" actual="+std::to_string(reinterpret_cast<const u8*>(&machine.Guest())[byte])+" expected="+std::to_string(reinterpret_cast<const u8*>(&expected)[byte]);
            INFO(context_difference);
            REQUIRE(reason==halt);REQUIRE(std::memcmp(&machine.Guest(),&expected,sizeof(expected))==0);
            REQUIRE(actual==wanted);REQUIRE(machine.state->host_cpu_flags==0xB0000000);REQUIRE(machine.state->current_loc.Value()==pc);
        }
    }
};
void CheckTransfer(const Form& form,bool pair,u32 mode,u32 rn,u32 rt,u32 rt2,s32 delta,u32 fault,bool unaligned){
    const u64 address=unaligned ? 65 : 64;
    auto initial=Initial();Gpr(initial,rn)=mode==2?address:address-delta;
    const auto memory=Memory();auto expected=initial;auto wanted=memory;
    const u64 limit=fault ? address+form.bytes*(fault-1)+form.bytes-1 : memory.size();
    if(form.load){
        if(!fault){Load(expected,form,rt,memory.data()+address);if(pair)Load(expected,form,rt2,memory.data()+address+form.bytes);}
    }else{
        if(fault!=1)Store(initial,form,rt,wanted.data()+address);
        if(pair&&!fault)Store(initial,form,rt2,wanted.data()+address+form.bytes);
    }
    if(!fault){if(mode)Gpr(expected,rn)=Gpr(initial,rn)+delta;expected.padding=u32(swift::arm64::InterruptReason::BRK);}
    Program{Encode(form,pair,mode,delta,rn,rt,rt2)}.Check(initial,memory,expected,wanted,limit,fault?HaltReason::PageFatal:HaltReason::CallHost,fault?0x1004:0x1008);
}
}

TEST_CASE("ARM64 single memory widths commit writeback only after successful pre post and offset transfers"){
    for(const auto form:Singles)for(u32 mode=0;mode<3;++mode)for(u32 rn:{3u,31u})for(s32 delta:{-16,16})for(bool unaligned:{false,true}){
        CheckTransfer(form,false,mode,rn,1,2,delta,0,unaligned);
        CheckTransfer(form,false,mode,rn,1,2,delta,1,unaligned);
    }
}
TEST_CASE("ARM64 pair loads publish neither destination on second failure while stores retain first transfer"){
    for(const auto form:Pairs)for(u32 mode=0;mode<3;++mode)for(u32 rn:{3u,31u})for(bool unaligned:{false,true})
    for(u32 fault:{0u,1u,2u})CheckTransfer(form,true,mode,rn,1,2,s32(form.bytes*2),fault,unaligned);
}
TEST_CASE("ARM64 memory offset aliases use original base and store snapshots including SP ZR and SIMD names"){
    for(const auto form:Singles){
        CheckTransfer(form,false,0,1,1,2,16,0,false);
        CheckTransfer(form,false,0,31,31,2,16,0,false);
        if(form.simd)for(u32 mode:{1u,2u})CheckTransfer(form,false,mode,1,1,2,16,0,false);
    }
    for(const auto form:Pairs){
        for(u32 rn:{1u,2u,31u})CheckTransfer(form,true,0,rn,1,2,s32(form.bytes*2),0,false);
        if(!form.load)CheckTransfer(form,true,0,3,1,1,s32(form.bytes*2),0,false);
        if(form.simd)for(u32 mode:{1u,2u})CheckTransfer(form,true,mode,1,1,2,s32(form.bytes*2),0,false);
    }
}
TEST_CASE("ARM64 constrained memory overlaps fallback before register or memory mutation"){
    for(const auto form:Singles)if(!form.simd)for(u32 mode:{1u,2u}){
        Program program(Encode(form,false,mode,16,1,1));auto initial=Initial(),expected=initial;const auto memory=Memory();
        expected.padding=u32(swift::arm64::InterruptReason::FALLBACK);program.Check(initial,memory,expected,memory,0,HaltReason::CallHost,0x1004);
    }
    for(const auto form:Pairs)for(u32 mode=0;mode<3;++mode)for(u32 scenario=0;scenario<3;++scenario){
        if(scenario==0&&!form.load)continue;if(scenario&&!mode)continue;if(scenario&&form.simd)continue;
        const u32 rn=scenario==1?1:scenario==2?2:3,rt2=scenario==0?1:2;
        Program program(Encode(form,true,mode,s32(form.bytes*2),rn,1,rt2));auto initial=Initial(),expected=initial;const auto memory=Memory();
        expected.padding=u32(swift::arm64::InterruptReason::FALLBACK);program.Check(initial,memory,expected,memory,0,HaltReason::CallHost,0x1004);
    }
}
TEST_CASE("ARM64 literal faults publish the instruction PC in interpreter and both native routes"){
    for(u32 opcode:{u32(LDR_w_lit),u32(LDR_x_lit),u32(LDRSW_x_lit),u32(LDR_s_lit),u32(LDR_d_lit),u32(LDR_q_lit)}){
        const u32 word=opcode|((u32((s32(64)-0x1004)/4)&0x7FFFF)<<5)|1;
        const auto initial=Initial();const auto memory=Memory();Program{word}.Check(initial,memory,initial,memory,64,HaltReason::PageFatal,0x1004);
    }
}

TEST_CASE("ARM64 register offset extensions snapshot base offset and aliased load destination"){
    for(const auto form:Singles)for(u32 option:{2u,3u,6u,7u})for(bool scaled:{false,true})for(u32 alias=0;alias<4;++alias){
        u32 scale=0;while((1u<<scale)<form.bytes)++scale;
        const u32 rn=alias==3?31:3,rm=alias==2?rn:4,rt=alias==0?1:alias==1?rm:rn;
        auto initial=Initial();Gpr(initial,rn)=alias==2?1:64;
        if(rm!=rn)Gpr(initial,rm)=option==2?0xAABBCCDD00000001ULL:option==6?0x12345678FFFFFFFFULL:1;
        const u64 raw=ReadGpr(initial,rm);
        const s64 offset=option==2?s64(u32(raw)):option==6?s64(s32(u32(raw))):s64(raw);
        const u64 address=Gpr(initial,rn)+offset*s64(scaled?1u<<scale:1u);
        const u32 word=0x38200800|form.opcode|(rm<<16)|(option<<13)|(scaled?1u<<12:0)|(rn<<5)|rt;
        const auto memory=Memory();auto expected=initial;auto wanted=memory;
        if(form.load)Load(expected,form,rt,memory.data()+address);else Store(initial,form,rt,wanted.data()+address);
        expected.padding=u32(swift::arm64::InterruptReason::BRK);
        Program{word}.Check(initial,memory,expected,wanted,memory.size(),HaltReason::CallHost,0x1008);
    }
}

#if defined(SWIFT_HAS_X64_BACKEND)
namespace {
struct ProtectedPages {
    const size_t page=size_t(sysconf(_SC_PAGESIZE));
    u8* data=static_cast<u8*>(mmap(nullptr,2*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    ~ProtectedPages(){if(data!=MAP_FAILED){mprotect(data,2*page,PROT_READ|PROT_WRITE);munmap(data,2*page);}}
};
}
TEST_CASE("ARM64 Runtime real permission faults preserve pair load destinations and writeback but retain first store"){
    for(const auto form:Pairs)for(u32 mode:{1u,2u})for(u32 rn:{3u,31u}){
        Config config{.loc_start=0,.loc_end=0x10000,.enable_jit=true,.backend_isa=kX86_64,
            .uniform_buffer_size=4096,.static_program=true,.global_opts=Optimizations::None,.stack_alignment=16};
        backend::AddressSpace space{config};auto module=space.GetDefaultModule();
        const s32 delta=s32(form.bytes*2);Code code{Encode(form,true,mode,delta,rn,1,2)};
        IntrusivePtr<ir::Block> block{new ir::Block{0,0x1000}};ir::Assembler assembler{block.get()};
        swift::arm64::A64Decoder{0x1000,&code,&assembler}.Decode();
        REQUIRE(module->Push(block.get()));REQUIRE(backend::TranslateIR(module,block)!=nullptr);
        Runtime runtime{&space};runtime.GetState()->pt=nullptr;runtime.GetState()->guest_addr_limit=UINT64_MAX;
        runtime.GetState()->guest_addr_mask=UINT64_MAX;
        for(u32 failed_element:{0u,1u}){
            ProtectedPages mapping;REQUIRE(mapping.data!=MAP_FAILED);
            std::vector<u8> expected_memory(2*mapping.page);
            for(size_t byte=0;byte<expected_memory.size();++byte)expected_memory[byte]=u8(0x80+byte*13);
            std::memcpy(mapping.data,expected_memory.data(),expected_memory.size());
            const u64 address=reinterpret_cast<u64>(mapping.data+mapping.page-failed_element*form.bytes);
            auto initial=Initial();Gpr(initial,rn)=mode==2?address:address-delta;
            if(!form.load&&failed_element)Store(initial,form,1,expected_memory.data()+mapping.page-form.bytes);
            std::memcpy(runtime.GetUniformBuffer().data(),&initial,sizeof(initial));
            runtime.GetState()->host_cpu_flags=0xB0000000;runtime.SetLocation(0x1000);
            // Read faults use PROT_NONE; store faults use a readable but unwritable page.
            REQUIRE(mprotect(mapping.data+mapping.page,mapping.page,form.load?PROT_NONE:PROT_READ)==0);
            const u32 original=_mm_getcsr(),changed=(original&~0x6000u)|0x4000u;_mm_setcsr(changed);
            const auto reason=runtime.Run();const u32 restored=_mm_getcsr();_mm_setcsr(original);
            REQUIRE(mprotect(mapping.data+mapping.page,mapping.page,PROT_READ|PROT_WRITE)==0);
            INFO("opcode="<<form.opcode<<" mode="<<mode<<" rn="<<rn<<" failed_element="<<failed_element);
            REQUIRE(reason==HaltReason::PageFatal);REQUIRE(runtime.GetLocation()==0x1004);
            REQUIRE(std::memcmp(runtime.GetUniformBuffer().data(),&initial,sizeof(initial))==0);
            REQUIRE(std::memcmp(mapping.data,expected_memory.data(),expected_memory.size())==0);
            REQUIRE(runtime.GetState()->host_cpu_flags==0xB0000000);REQUIRE(restored==changed);
        }
    }
}
#endif
