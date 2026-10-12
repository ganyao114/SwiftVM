#include <array>
#include <cstring>
#include <memory>
#include <set>
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
class RegressionMemory final : public MemoryInterface {
public:
    explicit RegressionMemory(u32 word) : code{0xD503201F,word,0xD4200000} {}
    bool Read(void* destination,size_t address,size_t size) override {
        if (address<0x1000 || address-0x1000>sizeof(code) || size>sizeof(code)-(address-0x1000)) return false;
        std::memcpy(destination,reinterpret_cast<u8*>(code.data())+address-0x1000,size);return true;
    }
    bool Write(void*,size_t,size_t) override { return false; }
    void* GetPointer(void* pointer) override {
        const auto address=reinterpret_cast<size_t>(pointer);
        return address>=0x1000 && address-0x1000<sizeof(code) ? reinterpret_cast<u8*>(code.data())+address-0x1000 : nullptr;
    }
    std::array<u32,3> code;
};
struct Machine {
    alignas(backend::State) std::array<u8,sizeof(backend::State)+sizeof(swift::arm64::ThreadContext64)+32> storage{};
    backend::State* state=new(storage.data()) backend::State{};
    ~Machine() { state->~State(); }
    swift::arm64::ThreadContext64& Context() { return *reinterpret_cast<swift::arm64::ThreadContext64*>(state->uniform_buffer_begin); }
};
struct Program {
    RegressionMemory memory;
    ir::Block block{0,0x1000};ir::Assembler assembler{&block};
#if defined(SWIFT_HAS_X64_BACKEND)
    std::array<std::unique_ptr<backend::x64::Translator>,2> native;
#endif
    explicit Program(u32 word) : memory(word) {
        swift::arm64::A64Decoder{0x1000,&memory,&assembler}.Decode();
#if defined(SWIFT_HAS_X64_BACKEND)
        for (u32 route=0;route<2;++route) {
            native[route]=std::make_unique<backend::x64::Translator>(&block,route ? backend::x64::HostFeatures{} : backend::x64::HostFeatures::Detect());
            native[route]->Translate();
        }
#endif
    }
    void Check(const swift::arm64::ThreadContext64& initial,const swift::arm64::ThreadContext64& expected,u64 expected_flags,u64 pc) {
        for (u32 route=0;route<
#if defined(SWIFT_HAS_X64_BACKEND)
            3
#else
            1
#endif
            ;++route) {
            Machine machine;machine.Context()=initial;machine.state->host_cpu_flags=0xB0000000;
            machine.state->current_loc=ir::Location{0xDEAD0000};
            HaltReason reason;
            if (!route) reason=backend::interp::Interpreter{*machine.state,&block}.Run();
#if defined(SWIFT_HAS_X64_BACKEND)
            else reason=native[route-1]->getCode<HaltReason(*)(backend::State*)>()(machine.state);
#endif
            INFO("word="<<memory.code[1]<<" route="<<route);
            REQUIRE(reason==HaltReason::CallHost);
            REQUIRE(std::memcmp(&machine.Context(),&expected,sizeof(expected))==0);
            REQUIRE(machine.state->host_cpu_flags==expected_flags);
            REQUIRE(machine.state->current_loc.Value()==pc);
        }
    }
};
swift::arm64::ThreadContext64 Initial() {
    swift::arm64::ThreadContext64 context{};
    for (u32 n=0;n<context.r.size();++n) context.r[n]=0x80123456789ABCDE ^ (u64(n)*0x0101010101010101);
    context.fp=0x1029384756ABCDEF;context.lr=0xFFEEDDCCBBAA0099;context.sp=0xDEADBEEF12345678;
    context.pc=0x777788889999AAAA;context.fpcr=0xFFFFFFFF;context.fpsr=0x0800009F;
    for (u32 n=0;n<32;++n) { const std::array<u64,2> value{0x123456789ABCDEF0ULL+n,0xFEDCBA9876543210ULL-n};std::memcpy(&context.v[n],value.data(),16); }
    return context;
}
u64 WidthMask(u32 bits) { return bits==64 ? ~u64{0} : (u64{1}<<bits)-1; }
// Construct the architectural bitmask from repeated rotated runs of ones,
// independently of VIXL GetImmLogical or the frontend implementation.
u64 LogicalMask(u32 bits,u32 element,u32 length,u32 rotation) {
    const u64 element_mask=WidthMask(element),run=(u64{1}<<length)-1;
    const u64 rotated=rotation ? ((run>>rotation)|(run<<(element-rotation)))&element_mask : run;
    u64 mask=0;for (u32 position=0;position<bits;position+=element) mask|=rotated<<position;
    return mask;
}
}

TEST_CASE("ARM64 logical immediate SP WSP writes and ZR source honor full register modes") {
    for (u32 bits : {32u,64u}) for (u32 element=2;element<=bits;element*=2)
    for (u32 length : std::set<u32>{1,element/2,element-1})
    for (u32 rotation : std::set<u32>{0,1,element-1}) for (u32 operation=0;operation<4;++operation)
    for (const auto regs : {std::array<u32,2>{31,1},std::array<u32,2>{31,31},std::array<u32,2>{1,1}}) {
        const u32 imms=((~(element*2-1))&63)|(length-1);
        const u32 word=0x12000000u|(bits==64 ? 1u<<31 : 0)|(operation<<29)|
            (element==64 ? 1u<<22 : 0)|(rotation<<16)|(imms<<10)|(regs[1]<<5)|regs[0];
        Program program(word);const u64 mask=LogicalMask(bits,element,length,rotation);
        for (u64 input : {u64{0},~u64{0},u64{0x8123456789ABCDEF},u64{0xFEDCBA9876543210}}) {
            auto initial=Initial();initial.r[1]=input;auto expected=initial;
            const u64 a=regs[1]==31 ? 0 : input&WidthMask(bits);
            const u64 result=(operation==0 || operation==3 ? a&mask : operation==1 ? a|mask : a^mask)&WidthMask(bits);
            u64 flags=0xB0000000;
            if (operation==3) flags=((result>>(bits-1))<<31)|(result==0 ? u64{1}<<30 : 0);
            if (regs[0]==31) { if (operation!=3) expected.sp=result; }
            else expected.r[regs[0]]=result;
            expected.padding=u32(swift::arm64::InterruptReason::BRK);
            program.Check(initial,expected,flags,0x1008);
        }
    }
}

TEST_CASE("ARM64 pointer authentication one source encodings fallback without bit operation alias mutation") {
    const std::array<u32,18> operations{PACIA,PACIB,PACDA,PACDB,AUTIA,AUTIB,AUTDA,AUTDB,
        PACIZA,PACIZB,PACDZA,PACDZB,AUTIZA,AUTIZB,AUTDZA,AUTDZB,XPACI,XPACD};
    for (u32 index=0;index<operations.size();++index) for (u32 rd : {0u,1u,31u})
    for (u32 rn : {0u,1u,31u}) {
        if (index>=8 && rn!=31) continue; // Zero-modifier and strip forms reserve Rn=31.
        Program program(operations[index]|(rn<<5)|rd);
        auto initial=Initial(),expected=initial;expected.padding=u32(swift::arm64::InterruptReason::FALLBACK);
        program.Check(initial,expected,0xB0000000,0x1004);
    }
}

TEST_CASE("ARM64 one source decoder rejects reserved control fields before architectural mutation") {
    for (u32 word : {u32(RBIT_x)|(1u<<17),u32(RBIT_x)|(1u<<29),u32(REV_x)&~u32(SixtyFourBits),
                    u32(PACIA)&~u32(SixtyFourBits),u32(RBIT_x)|0x1800u}) {
        Program program(word|(1u<<5));auto initial=Initial(),expected=initial;
        expected.padding=u32(swift::arm64::InterruptReason::ILL_CODE);
        program.Check(initial,expected,0xB0000000,0x1004);
    }
}

TEST_CASE("ARM64 supported one source encodings retain exact bit semantics after full opcode filtering") {
    const std::array<u32,11> operations{RBIT_w,RBIT_x,REV16_w,REV16_x,REV_w,REV32_x,REV_x,CLZ_w,CLZ_x,CLS_w,CLS_x};
    for (u32 word : operations) for (u32 rd : {0u,1u,31u}) for (u32 rn : {1u,31u}) {
        const u32 bits=word&SixtyFourBits ? 64 : 32;Program program(word|(rn<<5)|rd);
        for (u64 input : {u64{0},~u64{0},u64{1},u64{0x8123456789ABCDEF}}) {
            auto initial=Initial();initial.r[1]=input;auto expected=initial;
            const u64 a=rn==31 ? 0 : input&WidthMask(bits);u64 result=0;
            const u32 opcode=(word>>10)&63;
            if (opcode==0) for (u32 bit=0;bit<bits;++bit) result|=((a>>bit)&1)<<(bits-1-bit);
            else if (opcode==4) { result=bits;for (u32 bit=0;bit<bits;++bit) if ((a>>bit)&1) result=bits-1-bit; }
            else if (opcode==5) {
                const u64 sign=(a>>(bits-1))&1;
                for (u32 bit=bits-1;bit>0;--bit) {
                    if (((a>>(bit-1))&1)!=sign) break;
                    ++result;
                }
            }
            else { const u32 group=opcode==1 ? 2 : opcode==2 ? 4 : 8;
                for (u32 byte=0;byte<bits/8;++byte) result|=((a>>(8*byte))&255)<<(8*(byte/group*group+group-1-byte%group)); }
            if (rd!=31) expected.r[rd]=result;expected.padding=u32(swift::arm64::InterruptReason::BRK);
            program.Check(initial,expected,0xB0000000,0x1008);
        }
    }
}

TEST_CASE("ARM64 CLS counts every sign transition and preserves state for aliases and zero registers") {
    for (u32 bits : {32u,64u}) for (u32 rd : {0u,1u,31u}) for (u32 rn : {1u,31u}) {
        Program program(u32(bits==64 ? CLS_x : CLS_w)|(rn<<5)|rd);
        for (u32 transition=0;transition<bits;++transition) for (bool invert : {false,true}) {
            const u64 run=transition ? (u64{1}<<transition)-1 : 0;
            const u64 input=invert ? ~run : run;
            auto initial=Initial();initial.r[1]=input;auto expected=initial;
            const u64 value=rn==31 ? 0 : input&WidthMask(bits);
            const bool sign=(value>>(bits-1))&1;
            u64 count=0;
            for (u32 next=bits-1;next;--next) {
                if (bool((value>>(next-1))&1)!=sign) break;
                ++count;
            }
            if (rd!=31) expected.r[rd]=count;
            expected.padding=u32(swift::arm64::InterruptReason::BRK);
            program.Check(initial,expected,0xB0000000,0x1008);
        }
    }
}
