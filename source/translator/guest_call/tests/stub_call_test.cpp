//
// End-to-end host->guest calls against x86-64 stubs that CLANG compiled.
//
// The stubs (abi_stubs.c) are the independent oracle: clang implemented the
// same psABI when it generated their prologues, so if this layer puts an
// argument in the wrong register the stub reads the wrong value and the
// arithmetic comes out wrong.  kStubDumpArgs goes further and reports the
// whole incoming register file, so placement is asserted directly rather than
// inferred from a result.
//
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstring>

#include "abi_stubs_index.h"
#include "guest_call.h"
#include "test_vm.h"

using namespace swift::guest_call;
using svmtest::StubEntry;
using svmtest::Vm;
using svmtest::VmKind;
using Catch::Approx;

namespace {

struct TwoInt { int a; int b; };
struct TwoDouble { double x; double y; };
struct DoubleLong { double d; long l; };
struct LongDouble_ { long l; double d; };
struct TwoFloat { float a; float b; };
struct IntFloat { int a; float f; };
struct Chars8 { char a, b, c, d, e, f, g, h; };
struct Big24 { long a, b, c; };
struct TwoLong { long a; long b; };

template <typename Sig>
GuestFn<Sig> Stub(int index) {
    return GuestFn<Sig>{&Vm(VmKind::Stub), StubEntry(index)};
}

}  // namespace

// ===========================================================================
// 1. integer arguments, including the ones that spill to the stack
// ===========================================================================
TEST_CASE("guest call: nine integer arguments (six in registers, three spilled)") {
    auto f = Stub<long(long, long, long, long, long, long, long, long, long)>(kStubIntSum9);
    const long a[9] = {2, 5, 11, 17, 23, 31, 41, 53, 67};
    const long expect = a[0] * 1 + a[1] * 3 + a[2] * 7 + a[3] * 11 + a[4] * 13 + a[5] * 17 +
                        a[6] * 19 + a[7] * 23 + a[8] * 29;
    CHECK(f(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]) == expect);
}

TEST_CASE("guest call: integer register placement is asserted directly") {
    svmtest::ClearDumpBuffer();
    auto dump = Stub<void(long, long, long, long, long, long, long, long)>(kStubDumpArgs);
    dump(0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x7777, 0x8888);
    const std::uint64_t* d = svmtest::DumpBuffer();
    REQUIRE(d != nullptr);
    CHECK(d[SVM_DUMP_OFF_RDI / 8] == 0x1111);
    CHECK(d[SVM_DUMP_OFF_RSI / 8] == 0x2222);
    CHECK(d[SVM_DUMP_OFF_RDX / 8] == 0x3333);
    CHECK(d[SVM_DUMP_OFF_RCX / 8] == 0x4444);
    CHECK(d[SVM_DUMP_OFF_R8 / 8] == 0x5555);
    CHECK(d[SVM_DUMP_OFF_R9 / 8] == 0x6666);
    // The seventh and eighth arguments are the first two stack slots.
    CHECK(d[SVM_DUMP_OFF_STACK / 8 + 0] == 0x7777);
    CHECK(d[SVM_DUMP_OFF_STACK / 8 + 1] == 0x8888);
}

// ===========================================================================
// 2. floating-point arguments, including the ones that spill
// ===========================================================================
TEST_CASE("guest call: ten double arguments (eight in xmm, two spilled)") {
    auto f = Stub<double(double, double, double, double, double, double, double, double, double,
                         double)>(kStubDblSum10);
    const double a[10] = {1.5, 2.25, 3.125, 4.0625, 5.5, 6.75, 7.875, 8.5, 9.25, 10.125};
    const double expect = a[0] * 1 + a[1] * 3 + a[2] * 7 + a[3] * 11 + a[4] * 13 + a[5] * 17 +
                          a[6] * 19 + a[7] * 23 + a[8] * 29 + a[9] * 31;
    CHECK(f(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]) == Approx(expect));
}

TEST_CASE("guest call: vector register placement is asserted directly") {
    svmtest::ClearDumpBuffer();
    auto dump = Stub<void(double, double, double, double, double, double, double, double)>(
            kStubDumpArgs);
    dump(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0);
    const std::uint64_t* d = svmtest::DumpBuffer();
    for (int i = 0; i < 8; ++i) {
        double v = 0;
        std::memcpy(&v, &d[SVM_DUMP_OFF_XMM / 8 + i * 2], 8);
        CHECK(v == Approx(static_cast<double>(i + 1)));
    }
    // No integer argument was passed, so %rdi must still hold the poison the
    // layer writes -- proving the doubles did not leak into the INTEGER
    // sequence.
    CHECK((d[SVM_DUMP_OFF_RDI / 8] >> 32) == 0xDEADBEEFu);
}

// ===========================================================================
// 3. mixed integer / floating point: the two counters advance independently
// ===========================================================================
TEST_CASE("guest call: interleaved integer and double arguments") {
    auto f = Stub<double(long, double, long, double, long, double, long, double)>(kStubMix8);
    const double expect = static_cast<double>(2 * 1 + 3 * 3 + 5 * 7 + 7 * 11) + 1.5 * 100 +
                          2.5 * 300 + 3.5 * 700 + 4.5 * 1100;
    CHECK(f(2, 1.5, 3, 2.5, 5, 3.5, 7, 4.5) == Approx(expect));
}

TEST_CASE("guest call: interleaved placement is asserted directly") {
    svmtest::ClearDumpBuffer();
    auto dump = Stub<void(long, double, long, double, long, double)>(kStubDumpArgs);
    dump(0xA1, 1.0, 0xA2, 2.0, 0xA3, 3.0);
    const std::uint64_t* d = svmtest::DumpBuffer();
    CHECK(d[SVM_DUMP_OFF_RDI / 8] == 0xA1);
    CHECK(d[SVM_DUMP_OFF_RSI / 8] == 0xA2);
    CHECK(d[SVM_DUMP_OFF_RDX / 8] == 0xA3);
    for (int i = 0; i < 3; ++i) {
        double v = 0;
        std::memcpy(&v, &d[SVM_DUMP_OFF_XMM / 8 + i * 2], 8);
        CHECK(v == Approx(static_cast<double>(i + 1)));
    }
    // %rcx / xmm3 were never assigned.
    CHECK((d[SVM_DUMP_OFF_RCX / 8] >> 32) == 0xDEADBEEFu);
}

TEST_CASE("guest call: six integers do not exhaust the vector registers") {
    auto f = Stub<long(long, long, long, long, long, long, float)>(kStubIntFloatMix);
    const long expect = 2 + 3 * 3 + 5 * 7 + 7 * 11 + 11 * 13 + 13 * 17 + static_cast<long>(1.5f * 1000);
    CHECK(f(2, 3, 5, 7, 11, 13, 1.5f) == expect);
}

// ===========================================================================
// 4. small structs by value, split into eightbytes
// ===========================================================================
TEST_CASE("guest call: small structs by value") {
    SECTION("{int,int} -> one INTEGER eightbyte") {
        auto f = Stub<long(TwoInt)>(kStubTwoInt);
        CHECK(f(TwoInt{7, 9}) == 7009);
    }
    SECTION("{double,double} -> SSE, SSE in two vector registers") {
        auto f = Stub<double(TwoDouble)>(kStubTwoDouble);
        CHECK(f(TwoDouble{1.5, 2.25}) == Approx(1.5 * 100 + 2.25));
    }
    SECTION("{double,long} -> SSE, INTEGER") {
        auto f = Stub<double(DoubleLong)>(kStubDoubleLong);
        CHECK(f(DoubleLong{1.5, 42}) == Approx(1.5 * 100 + 42));
    }
    SECTION("{long,double} -> INTEGER, SSE") {
        auto f = Stub<double(LongDouble_)>(kStubLongDouble);
        CHECK(f(LongDouble_{42, 1.5}) == Approx(42.0 * 100 + 1.5));
    }
    SECTION("{float,float} -> a single SSE eightbyte (both floats in one xmm)") {
        auto f = Stub<double(TwoFloat)>(kStubTwoFloat);
        CHECK(f(TwoFloat{1.5f, 2.25f}) == Approx(1.5 * 100 + 2.25));
    }
    SECTION("{int,float} -> a single INTEGER eightbyte (the float rides in a GPR)") {
        auto f = Stub<double(IntFloat)>(kStubIntFloat);
        CHECK(f(IntFloat{7, 2.5f}) == Approx(700 + 2.5));
    }
    SECTION("eight chars -> one INTEGER eightbyte") {
        auto f = Stub<long(Chars8)>(kStubChars8);
        Chars8 s{1, 2, 3, 4, 5, 6, 7, 8};
        CHECK(f(s) == 1 + 2 * 2 + 3 * 4 + 4 * 8 + 5 * 16 + 6 * 32 + 7 * 64 + 8 * 128);
    }
}

TEST_CASE("guest call: a struct's eightbytes land in the registers the plan names") {
    svmtest::ClearDumpBuffer();
    // {double,long}: eightbyte 0 -> xmm0, eightbyte 1 -> rdi.
    auto dump = Stub<void(DoubleLong)>(kStubDumpArgs);
    dump(DoubleLong{2.5, 0x1234});
    const std::uint64_t* d = svmtest::DumpBuffer();
    double v = 0;
    std::memcpy(&v, &d[SVM_DUMP_OFF_XMM / 8], 8);
    CHECK(v == Approx(2.5));
    CHECK(d[SVM_DUMP_OFF_RDI / 8] == 0x1234);
}

TEST_CASE("guest call: a struct with no free vector register pair spills whole") {
    auto f = Stub<double(double, double, double, double, double, double, double, double,
                         TwoDouble)>(kStubStructBySpill);
    const double expect = 1 + 2 * 2 + 3 * 3 + 4 * 4 + 5 * 5 + 6 * 6 + 7 * 7 + 8 * 8 +
                          1.5 * 100 + 2.5 * 300;
    CHECK(f(1, 2, 3, 4, 5, 6, 7, 8, TwoDouble{1.5, 2.5}) == Approx(expect));
}

// ===========================================================================
// 5. MEMORY return values: the hidden %rdi pointer and the integer shift
// ===========================================================================
TEST_CASE("guest call: MEMORY return value shifts the integer arguments right") {
    auto f = Stub<Big24(long, long, long)>(kStubMakeBig);
    const Big24 r = f(10, 20, 30);
    CHECK(r.a == 11);
    CHECK(r.b == 22);
    CHECK(r.c == 33);
}

TEST_CASE("guest call: the hidden return pointer really is %rdi") {
    svmtest::ClearDumpBuffer();
    // Declaring a MEMORY return makes %rdi the buffer address, so the three
    // integer arguments must appear in %rsi/%rdx/%rcx.
    auto dump = Stub<Big24(long, long, long)>(kStubDumpArgs);
    // stub_dump_args does not write the return buffer, so the returned bytes
    // are whatever the scratch arena held; only the dump matters here.
    auto result = dump.Try(0xB1, 0xB2, 0xB3);
    REQUIRE(result.ok());
    const std::uint64_t* d = svmtest::DumpBuffer();
    CHECK(d[SVM_DUMP_OFF_RSI / 8] == 0xB1);
    CHECK(d[SVM_DUMP_OFF_RDX / 8] == 0xB2);
    CHECK(d[SVM_DUMP_OFF_RCX / 8] == 0xB3);
    // %rdi holds a scratch-arena address, not the first argument.
    CHECK(d[SVM_DUMP_OFF_RDI / 8] >= 0x60300000);
    CHECK(d[SVM_DUMP_OFF_RDI / 8] < 0x60700000);
}

TEST_CASE("guest call: MEMORY argument and MEMORY return together") {
    auto f = Stub<Big24(Big24, long)>(kStubBigIdent);
    const Big24 r = f(Big24{1, 2, 3}, 10);
    CHECK(r.a == 11);
    CHECK(r.b == 22);
    CHECK(r.c == 33);
}

TEST_CASE("guest call: MEMORY argument with an ordinary return") {
    auto f = Stub<long(Big24, long)>(kStubTakeBig);
    CHECK(f(Big24{2, 3, 5}, 7) == 2 * 1 + 3 * 3 + 5 * 7 + 7 * 11);
}

TEST_CASE("guest call: register-returned structs") {
    SECTION("{double,double} comes back in xmm0/xmm1") {
        auto f = Stub<TwoDouble(double, double)>(kStubRetTwoDouble);
        const TwoDouble r = f(1.5, 2.5);
        CHECK(r.x == Approx(3.0));
        CHECK(r.y == Approx(7.5));
    }
    SECTION("{double,long} comes back in xmm0 and rax") {
        auto f = Stub<DoubleLong(double, long)>(kStubRetDoubleLong);
        const DoubleLong r = f(1.5, 7);
        CHECK(r.d == Approx(3.0));
        CHECK(r.l == 21);
    }
    SECTION("{long,long} comes back in rax and rdx") {
        auto f = Stub<TwoLong(long, long)>(kStubRetTwoLong);
        const TwoLong r = f(11, 13);
        CHECK(r.a == 22);
        CHECK(r.b == 39);
    }
    SECTION("{int,int} comes back packed in rax") {
        auto f = Stub<TwoInt(int, int)>(kStubRetTwoInt);
        const TwoInt r = f(3, 4);
        CHECK(r.a == 6);
        CHECK(r.b == 12);
    }
}

// ===========================================================================
// 6. varargs and %al
// ===========================================================================
TEST_CASE("guest call: variadic integer arguments") {
    auto f = Stub<long(int, ...)>(kStubVarargInts);
    CHECK(f(3, 10L, 20L, 30L) == 10 * 1 + 20 * 3 + 30 * 5);
}

TEST_CASE("guest call: variadic double arguments need %al set correctly") {
    auto f = Stub<double(int, ...)>(kStubVarargDoubles);
    CHECK(f(3, 1.5, 2.5, 3.5) == Approx(1.5 * 1 + 2.5 * 3 + 3.5 * 5));
    CHECK(f(1, 4.25) == Approx(4.25));
}

TEST_CASE("guest call: variadic mixed arguments") {
    auto f = Stub<double(int, ...)>(kStubVarargMixed);
    CHECK(f(2, 3L, 1.5, 4L, 2.5) == Approx(3 * 10 + 1.5 + 4 * 10 + 2.5));
}

TEST_CASE("guest call: %al is the number of vector registers actually used") {
    auto al = Stub<std::uint64_t(...)>(kStubGetAl);
    CHECK(al() == 0);
    CHECK(al(1.0) == 1);
    CHECK(al(1.0, 2.0) == 2);
    CHECK(al(1L, 2.0, 3L, 4.0, 5.0) == 3);
    CHECK(al(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0) == 8);
    // A float in the variadic tail is promoted to double, so it still uses a
    // vector register.
    CHECK(al(1.0f, 2.0f) == 2);
    // Integers alone use none.
    CHECK(al(1L, 2L, 3L) == 0);
}

// ===========================================================================
// 7. stack alignment
// ===========================================================================
TEST_CASE("guest call: %rsp is 16-byte aligned at the call") {
    auto rsp = Stub<std::uint64_t()>(kStubGetRsp);
    // Inside the callee, %rsp points at the return address, so it is
    // 16-aligned minus 8: RSP ≡ 8 (mod 16).
    CHECK(rsp() % 16 == 8);

    // Any number of stack arguments must preserve that.
    auto rsp1 = GuestFn<std::uint64_t(long, long, long, long, long, long, long)>{
            &Vm(VmKind::Stub), StubEntry(kStubGetRsp)};
    CHECK(rsp1(1, 2, 3, 4, 5, 6, 7) % 16 == 8);
    auto rsp2 = GuestFn<std::uint64_t(long, long, long, long, long, long, long, long)>{
            &Vm(VmKind::Stub), StubEntry(kStubGetRsp)};
    CHECK(rsp2(1, 2, 3, 4, 5, 6, 7, 8) % 16 == 8);
    auto rsp3 = GuestFn<std::uint64_t(long, long, long, long, long, long, long, long, long)>{
            &Vm(VmKind::Stub), StubEntry(kStubGetRsp)};
    CHECK(rsp3(1, 2, 3, 4, 5, 6, 7, 8, 9) % 16 == 8);
}

TEST_CASE("guest call: a stack-passed struct is placed at its own alignment") {
    svmtest::ClearDumpBuffer();
    auto dump = Stub<void(double, double, double, double, double, double, double, double,
                          TwoDouble)>(kStubDumpArgs);
    dump(1, 2, 3, 4, 5, 6, 7, 8, TwoDouble{2.5, 3.5});
    const std::uint64_t* d = svmtest::DumpBuffer();
    double x = 0;
    double y = 0;
    std::memcpy(&x, &d[SVM_DUMP_OFF_STACK / 8 + 0], 8);
    std::memcpy(&y, &d[SVM_DUMP_OFF_STACK / 8 + 1], 8);
    CHECK(x == Approx(2.5));
    CHECK(y == Approx(3.5));
}

TEST_CASE("guest call: a stack argument respects its own alignment, not just 8") {
    // Seven longs fill the six INTEGER registers and put one on the stack;
    // __int128 needs two registers, has none left, and lands on the stack at an
    // offset respecting its 16-byte alignment (16, not 8).
    auto f = Stub<long(long, long, long, long, long, long, long, __int128)>(kStubI128AfterStack);
    const __int128 x = (static_cast<__int128>(0x1122334455667788LL) << 64) | 0x99AABBCCDDEEFF00ULL;
    const long lo = static_cast<long>(x);
    const long hi = static_cast<long>(x >> 64);
    const long expect = 2 + 3 * 3 + 5 * 7 + 7 * 11 + 11 * 13 + 13 * 17 + 17 * 19 + lo * 23 +
                        hi * 29;
    CHECK(f(2, 3, 5, 7, 11, 13, 17, x) == expect);
    // The plan itself says 16, and the guest agrees.  (Guarded so mutation
    // testing can show the runtime call catches this on its own -- see
    // abi_class_test.cpp.)
#ifndef SVM_ABI_STATIC_ASSERTS_OFF
    constexpr auto plan =
            MakeCallPlan<long, long, long, long, long, long, long, long, __int128>();
    STATIC_REQUIRE(plan.args[6].stack_off == 0);
    STATIC_REQUIRE(plan.args[7].stack_off == 16);
#endif
}

// ===========================================================================
// 8. failure paths: a broken guest must never look like a return value
// ===========================================================================
TEST_CASE("guest call: a faulting guest is reported, not returned") {
    auto crash = Stub<long()>(kStubCrash);
    auto r = crash.Try();
    CHECK_FALSE(r.ok());
    CHECK(r.status() == GuestCallStatus::GuestFault);
    // value() on a failed call throws rather than handing back the 42 the stub
    // would have returned.
    CHECK_THROWS_AS(r.value(), GuestCallError);
    // The throwing form is the default, so a caller cannot ignore it either.
    CHECK_THROWS_AS(crash(), GuestCallError);
}

TEST_CASE("guest call: an illegal instruction is reported") {
    auto ud2 = Stub<long()>(kStubUd2);
    auto r = ud2.Try();
    CHECK_FALSE(r.ok());
    CHECK(r.status() == GuestCallStatus::GuestFault);
    CHECK_THROWS_AS(ud2(), GuestCallError);
}

TEST_CASE("guest call: exhausting the guest stack is reported, not returned") {
    // The call stack is a bounded mapping; running off its bottom hits
    // unmapped guest memory. Without this the recursion would walk into
    // whatever the host happens to have below it.
    auto runaway = Stub<long()>(kStubRunaway);
    auto r = runaway.Try();
    CHECK_FALSE(r.ok());
    CHECK(r.status() == GuestCallStatus::GuestFault);
}

TEST_CASE("guest call: a guest that unbalances the stack is reported") {
    auto bad = Stub<long()>(kStubUnbalanced);
    auto r = bad.Try();
    CHECK_FALSE(r.ok());
    CHECK(r.status() == GuestCallStatus::StackImbalance);
}

TEST_CASE("guest call: the environment survives a faulted call") {
    auto crash = Stub<long()>(kStubCrash);
    (void)crash.Try();
    // A good call after a bad one still works, and the register file the guest
    // clobbered was restored.
    auto f = Stub<long(long, long, long, long, long, long, long, long, long)>(kStubIntSum9);
    CHECK(f(1, 1, 1, 1, 1, 1, 1, 1, 1) == 1 + 3 + 7 + 11 + 13 + 17 + 19 + 23 + 29);
}

TEST_CASE("guest call: calling a null entry fails instead of running something") {
    GuestFn<long()> nothing{&Vm(VmKind::Stub), 0};
    auto r = nothing.Try();
    CHECK_FALSE(r.ok());
    CHECK(r.status() == GuestCallStatus::SetupFailed);
}

// ===========================================================================
// ScopedGuestBuffer: the explicit copy-in/copy-out helper
// ===========================================================================
TEST_CASE("ScopedGuestBuffer copies host data into the guest window explicitly") {
    auto& env = Vm(VmKind::Stub);
    ScopedGuestBuffer buf{env, 64};
    REQUIRE(buf.valid());
    const char text[] = "hello guest";
    REQUIRE(buf.Write(text, sizeof(text)));
    char back[sizeof(text)] = {};
    REQUIRE(buf.Read(back, sizeof(text)));
    CHECK(std::string(back) == "hello guest");
    // The address is a guest address and typed as one.
    GuestPtr<const char> p = buf.as<const char>();
    CHECK(p.address() == buf.address());
    CHECK(p.address() >= 0x60300000);
}

