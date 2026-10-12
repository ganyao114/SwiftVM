#include <array>
#include <cstring>
#include <vector>
#include <boost/multiprecision/cpp_int.hpp>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#if defined(SWIFT_HAS_X64_BACKEND)
#include "runtime/backend/x64/translator.h"
#endif

using namespace swift;
using namespace swift::runtime;
using namespace vixl::aarch64;
namespace {
using Big = boost::multiprecision::cpp_int;
using ShiftVectorBits = std::array<u64, 2>;
constexpr std::array<u32,11> normal{NEON_SHL_scalar,NEON_SLI_scalar,NEON_SRI_scalar,
    NEON_SSHR_scalar,NEON_USHR_scalar,NEON_SRSHR_scalar,NEON_URSHR_scalar,
    NEON_SSRA_scalar,NEON_USRA_scalar,NEON_SRSRA_scalar,NEON_URSRA_scalar};
constexpr std::array<u32,3> sat_left{NEON_SQSHL_imm_scalar,NEON_UQSHL_imm_scalar,NEON_SQSHLU_scalar};
constexpr std::array<u32,6> sat_narrow{NEON_UQSHRN_scalar,NEON_UQRSHRN_scalar,NEON_SQSHRN_scalar,
    NEON_SQRSHRN_scalar,NEON_SQSHRUN_scalar,NEON_SQRSHRUN_scalar};
u64 Mask(u32 bits){return bits==64?~u64{0}:(u64{1}<<bits)-1;}
Big Signed(u64 value,u32 bits){Big result=value&Mask(bits);if((value>>(bits-1))&1)result-=Big(1)<<bits;return result;}
bool Left(u32 op){return op==NEON_SHL_scalar||op==NEON_SLI_scalar||op==sat_left[0]||op==sat_left[1]||op==sat_left[2];}
struct Expected {u64 value;bool qc;};
Expected Reference(u32 op,u32 bits,u32 shift,u64 input,u64 old){
    bool narrowing=false;for(u32 candidate:sat_narrow)narrowing|=candidate==op;
    bool saturated_left=false;for(u32 candidate:sat_left)saturated_left|=candidate==op;
    const bool signed_source=op==NEON_SSHR_scalar||op==NEON_SRSHR_scalar||op==NEON_SSRA_scalar||op==NEON_SRSRA_scalar||
        op==sat_left[0]||op==sat_left[2]||op==sat_narrow[2]||op==sat_narrow[3]||op==sat_narrow[4]||op==sat_narrow[5];
    const bool rounded=op==NEON_SRSHR_scalar||op==NEON_URSHR_scalar||op==NEON_SRSRA_scalar||op==NEON_URSRA_scalar||
        op==sat_narrow[1]||op==sat_narrow[3]||op==sat_narrow[5];
    const u32 source_bits=narrowing?bits*2:bits;
    Big value=signed_source?Signed(input,source_bits):Big(input&Mask(source_bits));
    if(Left(op))value<<=shift;
    else {if(rounded)value+=Big(1)<<(shift-1);value>>=shift;}
    bool qc=false;
    if(narrowing||saturated_left){
        const bool signed_destination=op==sat_left[0]||op==sat_narrow[2]||op==sat_narrow[3];
        const Big lower=signed_destination?-(Big(1)<<(bits-1)):Big(0);
        const Big upper=signed_destination?(Big(1)<<(bits-1))-1:(Big(1)<<bits)-1;
        if(value<lower){value=lower;qc=true;}else if(value>upper){value=upper;qc=true;}
    }else if(op==NEON_SSRA_scalar||op==NEON_USRA_scalar||op==NEON_SRSRA_scalar||op==NEON_URSRA_scalar)value+=old;
    else if(op==NEON_SLI_scalar){const Big keep=(Big(1)<<shift)-1;value|=Big(old)&keep;}
    else if(op==NEON_SRI_scalar){const Big keep=((Big(1)<<bits)-1)^(((Big(1)<<bits)-1)>>shift);value|=Big(old)&keep;}
    value&=(Big(1)<<bits)-1;return {value.convert_to<u64>(),qc};
}
u32 Encode(u32 op,u32 bits,u32 shift,u32 rn,u32 rd){const u32 encoded=Left(op)?bits+shift:2*bits-shift;return op|(encoded<<16)|(rn<<5)|rd;}
class InstructionMemory final:public MemoryInterface{
public:explicit InstructionMemory(u32 word):code{word,0xD4200000}{}
    bool Read(void* dst,size_t address,size_t size)override{if(address<0x1000||address-0x1000+size>8)return false;
        std::memcpy(dst,reinterpret_cast<u8*>(code.data())+address-0x1000,size);return true;}
    bool Write(void*,size_t,size_t)override{return false;}
    void* GetPointer(void* p)override{auto a=reinterpret_cast<size_t>(p);return a>=0x1000&&a-0x1000<8?reinterpret_cast<u8*>(code.data())+a-0x1000:nullptr;}
    std::array<u32,2>code;
};
struct Machine{
    alignas(backend::State)std::array<u8,sizeof(backend::State)+sizeof(swift::arm64::ThreadContext64)+32>storage{};
    backend::State*state=new(storage.data())backend::State{};
    Machine(){state->host_cpu_flags=0xB0000000;Context().fpcr=0x01400000;}
    ~Machine(){state->~State();}
    swift::arm64::ThreadContext64&Context(){return *reinterpret_cast<swift::arm64::ThreadContext64*>(state->uniform_buffer_begin);}
    void Set(u32 reg,ShiftVectorBits bits){std::memcpy(&Context().v[reg],bits.data(),16);}
    ShiftVectorBits Get(u32 reg){ShiftVectorBits bits;std::memcpy(bits.data(),&Context().v[reg],16);return bits;}
    void Run(u32 word,swift::arm64::InterruptReason reason=swift::arm64::InterruptReason::BRK){
        InstructionMemory memory(word);ir::Block block(0,0x1000);ir::Assembler assembler(&block);swift::arm64::A64Decoder decoder(0x1000,&memory,&assembler);decoder.Decode();
#if defined(SWIFT_HAS_X64_BACKEND)
        auto initial=Context();auto flags=state->host_cpu_flags;auto location=state->current_loc;
#endif
        backend::interp::Interpreter interpreter(*state,&block);interpreter.Run();REQUIRE(Context().padding==u32(reason));
#if defined(SWIFT_HAS_X64_BACKEND)
        auto expected=Context();auto expected_flags=state->host_cpu_flags;auto expected_location=state->current_loc;
        for(bool baseline:{false,true}){Context()=initial;state->host_cpu_flags=flags;state->current_loc=location;state->halt_reason=HaltReason::None;
            backend::x64::Translator translator(&block,baseline?backend::x64::HostFeatures{}:backend::x64::HostFeatures::Detect());translator.Translate();
            translator.getCode<HaltReason(*)(backend::State*)>()(state);REQUIRE(std::memcmp(&Context(),&expected,sizeof(expected))==0);
            REQUIRE(state->host_cpu_flags==expected_flags);REQUIRE(state->current_loc.Value()==expected_location.Value());}
#endif
    }
};
void Check(u32 op,u32 bits,u32 shift,u64 input,bool alias,bool sticky){
    Machine machine;const u32 rd=alias?31:0;machine.Set(0,{0xFEDCBA9876543210,~u64{0}});
    const ShiftVectorBits source{input,0xBADF00D5BADF00D5};machine.Set(31,source);machine.Context().fpsr=0x91|(sticky?1u<<27:0);
    const auto expected=Reference(op,bits,shift,input,machine.Get(rd)[0]);
    INFO("op="<<op<<" bits="<<bits<<" shift="<<shift<<" input="<<input<<" alias="<<alias<<" sticky="<<sticky);
    machine.Run(Encode(op,bits,shift,31,rd));REQUIRE(machine.Get(rd)==ShiftVectorBits{expected.value,0});
    if(!alias)REQUIRE(machine.Get(31)==source);
    REQUIRE(machine.Context().fpsr==(0x91|((sticky||expected.qc)?1u<<27:0)));
    REQUIRE(machine.Context().fpcr==0x01400000);REQUIRE(machine.state->host_cpu_flags==0xB0000000);
}
}

TEST_CASE("ARM64 scalar immediate ordinary shifts cover every D immediate with bigint expectations"){
    for(u32 op:normal){const u32 first=Left(op)?0:1,last=Left(op)?63:64;
        for(u32 shift=first;shift<=last;++shift)for(u64 input:{u64{0},u64{0x8000000000000001},~u64{0}})
            Check(op,64,shift,input,true,false);
        for(u32 shift:{first,last})for(bool alias:{false,true})for(bool sticky:{false,true})
            for(u64 input:{u64{1},u64{0x7FFFFFFFFFFFFFFF},u64{0x8000000000000000}})Check(op,64,shift,input,alias,sticky);
    }
}

TEST_CASE("ARM64 scalar immediate saturating shifts and narrowing use mathematical bigint boundaries"){
    for(u32 op:sat_left)for(u32 bits:{8u,16u,32u,64u}){
        const u64 mask=Mask(bits),sign=u64{1}<<(bits-1);
        for(u32 shift=0;shift<bits;++shift)for(u64 input:{sign,mask})Check(op,bits,shift,input,true,false);
        for(u32 shift:{0u,1u,bits/2,bits-1})for(bool alias:{false,true})for(bool sticky:{false,true})
            for(u64 input:{u64{0},u64{1},sign-1,sign+1,mask-1})Check(op,bits,shift,input|(~mask&0xA5A5A5A5A5A5A5A5),alias,sticky);
    }
    for(u32 op:sat_narrow)for(u32 bits:{8u,16u,32u}){
        const u64 mask=Mask(bits*2),sign=u64{1}<<(bits*2-1);
        for(u32 shift=1;shift<=bits;++shift)for(u64 input:{sign,mask})Check(op,bits,shift,input,true,false);
        for(u32 shift:{1u,bits/2,bits})for(bool alias:{false,true})for(bool sticky:{false,true}){
            const u64 cutoff=Mask(bits)<<shift;
            for(u64 input:{u64{0},u64{1},sign-1,sign+1,cutoff,cutoff+1,mask-1})
                Check(op,bits,shift,input|(~mask&0xA5A5A5A5A5A5A5A5),alias,sticky);
        }
    }
}

TEST_CASE("ARM64 scalar immediate shifts reject reserved widths before vector or FPSR mutation"){
    std::vector<u32>invalid;
    for(u32 op:normal)for(u32 bits:{8u,16u,32u})invalid.push_back(Encode(op,bits,Left(op)?0:1,31,0));
    for(u32 op:sat_narrow)invalid.push_back(Encode(op,64,1,31,0));
    for(u32 op:normal)invalid.push_back(op|(31u<<5));
    for(u32 op:sat_left)invalid.push_back(op|(31u<<5));
    for(u32 op:sat_narrow)invalid.push_back(op|(31u<<5));
    for(u32 word:invalid){Machine machine;machine.Set(0,{0x123456789ABCDEF0,~u64{0}});machine.Set(31,{~u64{0},~u64{0}});
        machine.Context().fpsr=0x08000091;const auto before=machine.Context();machine.Run(word,swift::arm64::InterruptReason::ILL_CODE);
        REQUIRE(std::memcmp(&machine.Context().v,&before.v,sizeof(before.v))==0);REQUIRE(machine.Context().fpsr==before.fpsr);
        REQUIRE(machine.state->current_loc.Value()==0x1000);REQUIRE(machine.state->host_cpu_flags==0xB0000000);}
}
