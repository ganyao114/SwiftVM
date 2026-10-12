#include <array>
#include <cstring>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#if defined(SWIFT_HAS_X64_BACKEND)
#include "runtime/backend/x64/translator.h"
#endif

using namespace swift;
using namespace swift::runtime;
namespace {
template<typename Context> auto& Gpr(Context& ctx,u32 reg){return reg<29?ctx.r[reg]:reg==29?ctx.fp:ctx.lr;}
u64 Read(const swift::arm64::ThreadContext64& ctx,u32 reg){return reg==31?0:Gpr(ctx,reg);}
class InstructionMemory final:public MemoryInterface{
public:explicit InstructionMemory(std::vector<u32> input):code(std::move(input)){code.push_back(0xD4200000);}
    bool Read(void*dst,size_t addr,size_t size)override{if(addr<0x1000||addr-0x1000+size>code.size()*4)return false;std::memcpy(dst,reinterpret_cast<u8*>(code.data())+addr-0x1000,size);return true;}
    bool Write(void*,size_t,size_t)override{return false;}
    void* GetPointer(void*p)override{auto addr=reinterpret_cast<size_t>(p);return addr>=0x1000&&addr-0x1000<code.size()*4?reinterpret_cast<u8*>(code.data())+addr-0x1000:nullptr;}
    std::vector<u32>code;
};
struct Machine{
    alignas(backend::State)std::array<u8,sizeof(backend::State)+sizeof(swift::arm64::ThreadContext64)+32>storage{};
    backend::State*state=new(storage.data())backend::State{};
    alignas(16)std::array<u8,64>memory{};
    Machine(){state->host_cpu_flags=0xB0000000;state->pt=memory.data();state->guest_addr_limit=memory.size();Context().fpsr=0x08000091;Context().fpcr=0x01400000;
        for(u32 reg=0;reg<31;++reg)Gpr(Context(),reg)=0x1020304050607080+u64(reg)*0x0101010101010101;Context().sp=16;}
    ~Machine(){state->~State();}
    swift::arm64::ThreadContext64&Context(){return *reinterpret_cast<swift::arm64::ThreadContext64*>(state->uniform_buffer_begin);}
    void Run(std::vector<u32>code,HaltReason halt=HaltReason::CallHost){InstructionMemory instructions(std::move(code));ir::Block block(0,0x1000);ir::Assembler assembler(&block);
        swift::arm64::A64Decoder decoder(0x1000,&instructions,&assembler);decoder.Decode();
#if defined(SWIFT_HAS_X64_BACKEND)
        const auto initial=Context();const auto initial_memory=memory;const auto initial_flags=state->host_cpu_flags;const auto initial_location=state->current_loc;
#endif
        backend::interp::Interpreter interpreter(*state,&block);REQUIRE(interpreter.Run()==halt);
#if defined(SWIFT_HAS_X64_BACKEND)
        const auto expected=Context();const auto expected_memory=memory;const auto expected_flags=state->host_cpu_flags;const auto expected_location=state->current_loc;
        for(bool baseline:{false,true}){Context()=initial;memory=initial_memory;state->host_cpu_flags=initial_flags;state->current_loc=initial_location;state->halt_reason=HaltReason::None;
            backend::x64::Translator translator(&block,baseline?backend::x64::HostFeatures{}:backend::x64::HostFeatures::Detect());translator.Translate();
            REQUIRE(translator.getCode<HaltReason(*)(backend::State*)>()(state)==halt);REQUIRE(std::memcmp(&Context(),&expected,sizeof(expected))==0);
            REQUIRE(memory==expected_memory);REQUIRE(state->host_cpu_flags==expected_flags);REQUIRE(state->current_loc.Value()==expected_location.Value());}
#endif
    }
};
u32 Encoding(bool wide,u32 order,u32 rs,u32 rt,u32 rn){return 0x08207C00|(wide?1u<<30:0)|(order&1?1u<<22:0)|(order&2?1u<<15:0)|(rs<<16)|(rn<<5)|rt;}
}

TEST_CASE("ARM64 CASP X pairs atomically compare both halves for all order and register aliases"){
    constexpr std::array<std::array<u32,3>,10> combinations{{{0,2,4},{0,0,4},{0,2,0},{0,2,1},{0,2,2},{0,2,3},
        {30,2,4},{0,30,4},{30,30,31},{0,2,31}}};
    for(u32 order=0;order<4;++order)for(auto regs:combinations)for(u32 mismatch=0;mismatch<4;++mismatch){
        auto[rs,rt,rn]=regs;Machine machine;if(rn==31)machine.Context().sp=16;else Gpr(machine.Context(),rn)=16;
        const auto initial=machine.Context();const std::array<u64,2>expected{Read(initial,rs),Read(initial,rs+1)},desired{Read(initial,rt),Read(initial,rt+1)};
        const std::array<u64,2>observed{expected[0]^(mismatch&1?1u:0u),expected[1]^(mismatch&2?u64{1}<<63:0)};
        std::memcpy(machine.memory.data()+16,observed.data(),16);auto expected_memory=machine.memory;
        if(!mismatch)std::memcpy(expected_memory.data()+16,desired.data(),16);
        INFO("order="<<order<<" rs="<<rs<<" rt="<<rt<<" rn="<<rn<<" mismatch="<<mismatch);
        machine.Run({Encoding(true,order,rs,rt,rn)});
        for(u32 reg=0;reg<31;++reg){const u64 value=reg==rs?observed[0]:reg==rs+1?observed[1]:Read(initial,reg);REQUIRE(Read(machine.Context(),reg)==value);}
        REQUIRE(machine.Context().sp==initial.sp);REQUIRE(machine.memory==expected_memory);REQUIRE(machine.Context().fpsr==initial.fpsr);
        REQUIRE(machine.Context().fpcr==initial.fpcr);REQUIRE(machine.state->host_cpu_flags==0xB0000000);
    }
}

TEST_CASE("ARM64 CASP W pairs retain zero extended observations and pair atomicity"){
    for(u32 order=0;order<4;++order)for(bool match:{false,true}){Machine machine;machine.Context().r[4]=16;
        machine.Context().r[0]=0xDEADBEEF01234567;machine.Context().r[1]=0xBADF00D589ABCDEF;
        machine.Context().r[2]=0x11223344FEDCBA98;machine.Context().r[3]=0x5566778876543210;
        const u64 observed=0x89ABCDEF01234567^(match?0:u64{1}<<32);std::memcpy(machine.memory.data()+16,&observed,8);
        machine.Run({Encoding(false,order,0,2,4)});u64 actual;std::memcpy(&actual,machine.memory.data()+16,8);
        REQUIRE(machine.Context().r[0]==u32(observed));REQUIRE(machine.Context().r[1]==u32(observed>>32));
        REQUIRE(actual==(match?0x76543210FEDCBA98:observed));REQUIRE(machine.state->host_cpu_flags==0xB0000000);
    }
}

TEST_CASE("ARM64 CASP faults publish the exact later instruction PC without memory or register mutation"){
    for(bool alignment:{false,true})for(u32 address:alignment?std::array<u32,3>{1,8,15}:std::array<u32,3>{48,64,80}){
        // address48 would fit normally; shorten the range so only half fits.
        Machine machine;machine.Context().r[4]=address;if(!alignment)machine.state->guest_addr_limit=address==48?56:64;
        const auto initial=machine.Context();const auto initial_memory=machine.memory;
        machine.Run({0xD503201F,Encoding(true,3,0,2,4)},HaltReason::PageFatal);
        REQUIRE(std::memcmp(&machine.Context(),&initial,sizeof(initial))==0);REQUIRE(machine.memory==initial_memory);
        REQUIRE(machine.state->current_loc.Value()==0x1004);REQUIRE(machine.state->host_cpu_flags==0xB0000000);
    }
}

TEST_CASE("ARM64 CASP rejects odd pair starts before accessing memory"){
    for(bool wide:{false,true})for(u32 rs:{0u,1u,31u})for(u32 rt:{0u,1u,31u}){
        if(!(rs&1)&&!(rt&1))continue;Machine machine;machine.Context().r[4]=1;const auto initial=machine.Context();const auto memory=machine.memory;
        machine.Run({Encoding(wide,0,rs,rt,4)});REQUIRE(machine.Context().padding==u32(swift::arm64::InterruptReason::ILL_CODE));
        for(u32 reg=0;reg<31;++reg)REQUIRE(Read(machine.Context(),reg)==Read(initial,reg));REQUIRE(machine.memory==memory);
        REQUIRE(machine.state->current_loc.Value()==0x1000);REQUIRE(machine.state->host_cpu_flags==0xB0000000);
    }
}
