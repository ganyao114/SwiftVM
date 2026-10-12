#include <array>
#include <cstring>
#include <memory>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#if defined(SWIFT_HAS_X64_BACKEND)
#include <xmmintrin.h>
#include "runtime/backend/x64/translator.h"
#endif
using namespace swift;
using namespace swift::runtime;
using namespace vixl::aarch64;
namespace {
using Context=swift::arm64::ThreadContext64;
using Bytes=std::array<u8,128>;
struct Form { u32 opcode,bytes;bool load,simd,sign,dest64; };
const std::array<Form,13> Unprivileged{{
    {LDRB_w,1,true,false,false,false},{LDRH_w,2,true,false,false,false},{LDR_w,4,true,false,false,false},{LDR_x,8,true,false,false,true},
    {LDRSB_w,1,true,false,true,false},{LDRSH_w,2,true,false,true,false},{LDRSB_x,1,true,false,true,true},{LDRSH_x,2,true,false,true,true},{LDRSW_x,4,true,false,true,true},
    {STRB_w,1,false,false,false,false},{STRH_w,2,false,false,false,false},{STR_w,4,false,false,false,false},{STR_x,8,false,false,false,true}}};
const std::array<Form,4> Rcpc{{{LDAPRB,1,true,false,false,false},{LDAPRH,2,true,false,false,false},
    {LDAPR_w,4,true,false,false,false},{LDAPR_x,8,true,false,false,true}}};
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

u32 Encoding(const Form& form,bool rcpc,s32 offset,u32 rn,u32 rt) {
    return (rcpc ? form.opcode|(31u<<16) : 0x38000800u|form.opcode|((u32(offset)&511)<<12))|(rn<<5)|rt;
}
void CheckExtension(const Form& form,bool rcpc,s32 offset,u32 rn,u32 rt,u32 address,bool fault) {
    auto initial=Initial();Gpr(initial,rn)=u64(s64(address)-offset);auto memory=Memory();
    auto expected=initial;auto wanted=memory;
    if(!fault) {
        if(form.load)Load(expected,form,rt,memory.data()+address);else Store(initial,form,rt,wanted.data()+address);
        expected.padding=u32(swift::arm64::InterruptReason::BRK);
    }
    Program{Encoding(form,rcpc,offset,rn,rt)}.Check(initial,memory,expected,wanted,
        fault ? address+form.bytes-1 : memory.size(),fault ? HaltReason::PageFatal : HaltReason::CallHost,fault ? 0x1004 : 0x1008);
}
}

TEST_CASE("ARM64 unprivileged loads stores cover signed immediate boundaries extension SP ZR and aliases") {
    for(const auto form:Unprivileged)for(s32 offset:{-256,-17,-1,0,1,17,255})
    for(const auto regs:{std::array<u32,2>{3,1},std::array<u32,2>{1,1},std::array<u32,2>{31,1},std::array<u32,2>{31,31}})
    for(u32 address:{32u,33u,63u})for(bool fault:{false,true})
        CheckExtension(form,false,offset,regs[0],regs[1],address,fault);
}
TEST_CASE("ARM64 LDAPR reads every width without RMW FP control restrictions or base mutation") {
    for(const auto form:Rcpc)
    for(const auto regs:{std::array<u32,2>{3,1},std::array<u32,2>{1,1},std::array<u32,2>{31,1},std::array<u32,2>{31,31}})
    for(u32 address:{32u,33u,47u,63u})for(bool fault:{false,true})
        CheckExtension(form,true,0,regs[0],regs[1],address,fault);
}
TEST_CASE("ARM64 base memory extension reserved size opc SIMD and LDAPR source modes fail before access") {
    // LDTR SIMD, PRFUM-like opc, signed-load invalid sizes; LDAPR Rs != 31 and release/no-acquire.
    for(u32 word:{0x3C400820u,0xF8800820u,0xB8C00820u,0xF8C00820u,
                  0xB8A0C020u,0xB8BEC020u,0xB8FFC020u,0xB83FC020u}) {
        auto initial=Initial(),expected=initial;auto memory=Memory();
        expected.padding=u32(swift::arm64::InterruptReason::ILL_CODE);
        Program{word}.Check(initial,memory,expected,memory,0,HaltReason::CallHost,0x1004);
    }
}

