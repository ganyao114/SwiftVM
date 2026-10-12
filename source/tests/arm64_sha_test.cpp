#include <array>
#include <bit>
#include <cstring>
#include <random>
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
using Words = std::array<u32, 4>;
constexpr std::array<u32, 10> kOps{0x5E000000,0x5E001000,0x5E002000,0x5E003000,
    0x5E004000,0x5E005000,0x5E006000,0x5E280800,0x5E281800,0x5E282800};
u32 Small0(u32 v) { return std::rotr(v,7) ^ std::rotr(v,18) ^ (v >> 3); }
u32 Small1(u32 v) { return std::rotr(v,17) ^ std::rotr(v,19) ^ (v >> 10); }
u32 Big0(u32 v) { return std::rotr(v,2) ^ std::rotr(v,13) ^ std::rotr(v,22); }
u32 Big1(u32 v) { return std::rotr(v,6) ^ std::rotr(v,11) ^ std::rotr(v,25); }
Words Reference(u32 op, Words d, const Words& n, const Words& m) {
    if (op == kOps[7]) return {std::rotl(n[0],30),0,0,0};
    if (op == kOps[3]) return {d[0]^d[2]^m[0],d[1]^d[3]^m[1],d[2]^n[0]^m[2],d[3]^n[1]^m[3]};
    if (op == kOps[8]) {
        Words out{std::rotl(d[0]^n[1],1),std::rotl(d[1]^n[2],1),std::rotl(d[2]^n[3],1),0};
        out[3] = std::rotl(d[3]^out[0],1);
        return out;
    }
    if (op == kOps[9]) return {d[0]+Small0(d[1]),d[1]+Small0(d[2]),d[2]+Small0(d[3]),d[3]+Small0(n[0])};
    if (op == kOps[6]) {
        Words out{d[0]+n[1],d[1]+n[2],d[2]+n[3],d[3]+m[0]};
        out[0]+=Small1(m[2]);out[1]+=Small1(m[3]);out[2]+=Small1(out[0]);out[3]+=Small1(out[1]);
        return out;
    }
    if (op <= kOps[2]) {
        u32 e=n[0];
        for (u32 word:m) {
            const u32 f = op==kOps[0] ? (d[1]&d[2]) | (~d[1]&d[3]) : op==kOps[1] ? d[1]^d[2]^d[3]
                : (d[1]&d[2]) | (d[1]&d[3]) | (d[2]&d[3]);
            const u32 next = std::rotl(d[0],5)+f+e+word;
            e=d[3];d={next,d[0],std::rotl(d[1],30),d[2]};
        }
        return d;
    }
    std::array<u32,8> state;
    for (u32 i=0;i<4;++i) {state[i]=op==kOps[4]?d[i]:n[i];state[i+4]=op==kOps[4]?n[i]:d[i];}
    for (u32 word:m) {
        const u32 t1=state[7]+Big1(state[4])+((state[4]&state[5])^(~state[4]&state[6]))+word;
        const u32 t2=Big0(state[0])+((state[0]&state[1])^(state[0]&state[2])^(state[1]&state[2]));
        state={t1+t2,state[0],state[1],state[2],state[3]+t1,state[4],state[5],state[6]};
    }
    const u32 base=op==kOps[4]?0:4;
    return {state[base],state[base+1],state[base+2],state[base+3]};
}
class InstructionMemory final : public MemoryInterface {
public:
    explicit InstructionMemory(std::vector<u32> input):code(std::move(input)){code.push_back(0xD4200000);}
    bool Read(void* destination,size_t address,size_t size) override {
        if(address<0x1000||address-0x1000+size>code.size()*4)return false;
        std::memcpy(destination,reinterpret_cast<u8*>(code.data())+address-0x1000,size);return true;
    }
    bool Write(void*,size_t,size_t) override{return false;}
    void* GetPointer(void* pointer) override {
        const auto address=reinterpret_cast<size_t>(pointer);
        return address>=0x1000&&address-0x1000<code.size()*4?reinterpret_cast<u8*>(code.data())+address-0x1000:nullptr;
    }
    std::vector<u32> code;
};
struct Machine {
    alignas(backend::State) std::array<u8,sizeof(backend::State)+sizeof(swift::arm64::ThreadContext64)+32> storage{};
    backend::State* state=new(storage.data())backend::State{};
    Machine(){state->host_cpu_flags=0xB0000000;Context().fpsr=0x0800009F;Context().fpcr=0x01400000;
        for(u32 i=0;i<32;++i)Vector(i,{i*13+1,i*29+2,i*43+3,i*79+4});}
    ~Machine(){state->~State();}
    swift::arm64::ThreadContext64& Context(){return *reinterpret_cast<swift::arm64::ThreadContext64*>(state->uniform_buffer_begin);}
    void Vector(u32 reg,const Words& words){std::memcpy(&Context().v[reg],words.data(),16);}
    Words Vector(u32 reg){Words words;std::memcpy(words.data(),&Context().v[reg],16);return words;}
    void Run(std::vector<u32> code,swift::arm64::InterruptReason reason=swift::arm64::InterruptReason::BRK){
        InstructionMemory memory(std::move(code));ir::Block block(0,0x1000);ir::Assembler assembler(&block);
        swift::arm64::A64Decoder decoder(0x1000,&memory,&assembler);decoder.Decode();
#if defined(SWIFT_HAS_X64_BACKEND)
        const auto initial=Context();const auto initial_flags=state->host_cpu_flags;const auto initial_location=state->current_loc;
#endif
        backend::interp::Interpreter interpreter(*state,&block);interpreter.Run();
        REQUIRE(Context().padding==u32(reason));
#if defined(SWIFT_HAS_X64_BACKEND)
        const auto expected=Context();const auto flags=state->host_cpu_flags;const auto location=state->current_loc;
        for(bool baseline:{false,true}){
            Context()=initial;state->host_cpu_flags=initial_flags;state->current_loc=initial_location;state->halt_reason=HaltReason::None;
            backend::x64::Translator translator(&block,baseline?backend::x64::HostFeatures{}:backend::x64::HostFeatures::Detect());
            translator.Translate();translator.getCode<HaltReason(*)(backend::State*)>()(state);
            REQUIRE(std::memcmp(&Context(),&expected,sizeof(expected))==0);REQUIRE(state->host_cpu_flags==flags);
            REQUIRE(state->current_loc.Value()==location.Value());
        }
#endif
    }
};
constexpr std::array<u32,64> kConstants{
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
}

TEST_CASE("ARM64 SHA instructions match independent rounds and schedules under every operand alias"){
    std::mt19937 random(0xF180);
    constexpr std::array<std::array<u32,3>,6> aliases{{{0,1,31},{1,1,31},{31,1,31},{0,31,31},{31,31,31},{0,1,0}}};
    for(u32 op:kOps)for(auto regs:aliases)for(u32 sample=0;sample<40;++sample){
        const auto [rd,rn,rm]=regs;Machine machine;
        for(u32 reg:{rd,rn,rm}){Words value;for(auto& word:value)word=sample==0?0:sample==1?~u32{0}:random();machine.Vector(reg,value);}
        const auto before=machine.Context();const auto expected=Reference(op,machine.Vector(rd),machine.Vector(rn),machine.Vector(rm));
        const bool two=(op&0x00FF0000)==0x00280000;
        INFO("op="<<op<<" rd="<<rd<<" rn="<<rn<<" rm="<<rm<<" sample="<<sample);
        machine.Run({op|(rn<<5)|rd|(two?0:rm<<16)});
        REQUIRE(machine.Vector(rd)==expected);
        for(u32 reg=0;reg<32;++reg)if(reg!=rd)REQUIRE(std::memcmp(&machine.Context().v[reg],&before.v[reg],16)==0);
        REQUIRE(machine.Context().fpsr==before.fpsr);REQUIRE(machine.Context().fpcr==before.fpcr);REQUIRE(machine.state->host_cpu_flags==0xB0000000);
    }
}

TEST_CASE("ARM64 SHA1 and SHA256 rounds produce FIPS180 empty and abc digests"){
    constexpr std::array<u32,8> initial{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    constexpr std::array<u32,5> initial1{0x67452301,0xefcdab89,0x98badcfe,0x10325476,0xc3d2e1f0};
    for(bool abc:{false,true})for(bool sha256:{false,true}){
        std::array<u32,80> schedule{};schedule[0]=abc?0x61626380:0x80000000;schedule[15]=abc?24:0;
        for(u32 i=16;i<(sha256?64u:80u);++i)schedule[i]=sha256?
            Small1(schedule[i-2])+schedule[i-7]+Small0(schedule[i-15])+schedule[i-16]:
            std::rotl(schedule[i-3]^schedule[i-8]^schedule[i-14]^schedule[i-16],1);
        Machine machine;std::vector<u32> code;
        if(sha256){
            machine.Vector(0,{initial[0],initial[1],initial[2],initial[3]});machine.Vector(1,{initial[4],initial[5],initial[6],initial[7]});
            for(u32 group=0;group<16;++group){
                Words key;for(u32 i=0;i<4;++i)key[i]=schedule[group*4+i]+kConstants[group*4+i];machine.Vector(group+3,key);
                code.push_back(u32(vixl::aarch64::NEON_ORR)|(1u<<30)|2u);
                code.push_back(kOps[4]|(1u<<5)|((group+3)<<16));
                code.push_back(kOps[5]|(2u<<5)|((group+3)<<16)|1u);
            }
            machine.Run(code);
            const auto a=machine.Vector(0),e=machine.Vector(1);
            const std::array<u32,8> expected=abc?std::array<u32,8>{0xba7816bf,0x8f01cfea,0x414140de,0x5dae2223,0xb00361a3,0x96177a9c,0xb410ff61,0xf20015ad}:
                std::array<u32,8>{0xe3b0c442,0x98fc1c14,0x9afbf4c8,0x996fb924,0x27ae41e4,0x649b934c,0xa495991b,0x7852b855};
            for(u32 i=0;i<8;++i)REQUIRE((i<4?a[i]:e[i-4])+initial[i]==expected[i]);
        }else{
            machine.Vector(0,{initial1[0],initial1[1],initial1[2],initial1[3]});machine.Vector(1,{initial1[4],0,0,0});
            u32 e=1;
            for(u32 group=0;group<20;++group){
                constexpr std::array<u32,4> constants{0x5a827999,0x6ed9eba1,0x8f1bbcdc,0xca62c1d6};
                Words key;for(u32 i=0;i<4;++i)key[i]=schedule[group*4+i]+constants[group/5];machine.Vector(group+3,key);
                const u32 next_e=3-e;code.push_back(kOps[7]|next_e);
                const u32 op=group<5?kOps[0]:group<10?kOps[1]:group<15?kOps[2]:kOps[1];
                code.push_back(op|(e<<5)|((group+3)<<16));e=next_e;
            }
            machine.Run(code);const auto a=machine.Vector(0);
            const std::array<u32,5> expected=abc?std::array<u32,5>{0xa9993e36,0x4706816a,0xba3e2571,0x7850c26c,0x9cd0d89d}:
                std::array<u32,5>{0xda39a3ee,0x5e6b4b0d,0x3255bfef,0x95601890,0xafd80709};
            for(u32 i=0;i<5;++i)REQUIRE((i<4?a[i]:machine.Vector(e)[0])+initial1[i]==expected[i]);
        }
        REQUIRE(machine.Context().fpsr==0x0800009F);REQUIRE(machine.state->host_cpu_flags==0xB0000000);
    }
}

TEST_CASE("ARM64 SHA rejects reserved opcode and size fields without vector mutation"){
    std::vector<u32> codes;
    for(u32 opcode=3;opcode<16;++opcode)codes.push_back(0x5E280800|(opcode<<12));
    for(u32 opcode=7;opcode<16;++opcode)codes.push_back(0x5E000000|(opcode<<12)|(2u<<16));
    for(u32 op:kOps)for(u32 size=1;size<4;++size)codes.push_back(op|(size<<22)|((op&0x00FF0000)==0x00280000?0:2u<<16));
    for(u32 code:codes){Machine machine;const auto before=machine.Context();
        machine.Run({code|(1u<<5)},swift::arm64::InterruptReason::ILL_CODE);
        for(u32 reg=0;reg<32;++reg)REQUIRE(std::memcmp(&machine.Context().v[reg],&before.v[reg],16)==0);
        REQUIRE(machine.state->current_loc.Value()==0x1000);REQUIRE(machine.Context().fpsr==before.fpsr);REQUIRE(machine.state->host_cpu_flags==0xB0000000);
    }
}
