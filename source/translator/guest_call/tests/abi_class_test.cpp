//
// Compile-time checks on the SysV classifier.  Every assertion here is a
// static_assert: if the classification is wrong the test binary does not
// build, which is the point of doing the work at compile time.
//
// The values are taken from the psABI text (see the header of abi_sysv.h) and
// cross-checked against what clang actually emits for x86-64 by the
// stub_call_test cases, which call real compiled code with the same types.
//
#include <catch2/catch_test_macros.hpp>

#include "abi_sysv.h"
#include "guest_call.h"

using namespace swift::guest_call;

namespace {

struct TwoInt { int a; int b; };
struct TwoDouble { double x; double y; };
struct DoubleLong { double d; long l; };
struct LongDouble_ { long l; double d; };
struct TwoFloat { float a; float b; };
struct IntFloat { int a; float f; };
struct Chars8 { char a, b, c, d, e, f, g, h; };
struct Big24 { long a, b, c; };
struct Big17 { char c[16]; char d; };
struct Empty {};
struct NestedSse { struct Inner { float x; float y; } in; double d; };
struct NestedInt { int a; struct Inner { short x; short y; } in; };
struct FourFloat { float a, b, c, d; };
struct OneFloatOneLong { float f; long l; };
struct NonTrivial {
    NonTrivial(const NonTrivial&) {}
    NonTrivial() = default;
    int x;
};

// Aggregates with C array members cannot be walked automatically (brace
// elision inflates the deduced arity); this is the documented escape hatch.
struct WithArray { int a; int b[3]; };

}  // namespace

// The escape hatch must be declared before the first use of the type.
template <>
struct swift::guest_call::AbiFields<WithArray> {
    using types = AbiFieldList<int, int, int, int>;  // flattened; the byte map is identical
};

namespace {

// The block of static_asserts below is the primary content of this file.  It
// can be switched off with -DSVM_ABI_STATIC_ASSERTS_OFF, which exists for one
// reason: mutation testing.  A compile error is a legitimate way to kill a
// mutant, but it hides whether the *runtime* tests would have caught it too,
// and "the end-to-end call against real compiled x86-64 notices" is the
// stronger claim.  The mutation harness runs both ways.
#ifndef SVM_ABI_STATIC_ASSERTS_OFF

constexpr bool AllOf(const TypeClassInfo& i, std::initializer_list<Eightbyte> want) {
    if (i.n != want.size()) {
        return false;
    }
    unsigned k = 0;
    for (auto e : want) {
        if (i.eb[k++] != e) {
            return false;
        }
    }
    return true;
}

// --- scalars ---------------------------------------------------------------
static_assert(AbiClass<int>::value == ArgClass::Integer);
static_assert(AbiClass<long>::value == ArgClass::Integer);
static_assert(AbiClass<unsigned long long>::value == ArgClass::Integer);
static_assert(AbiClass<bool>::value == ArgClass::Integer);
static_assert(AbiClass<char>::value == ArgClass::Integer);
static_assert(AbiClass<float>::value == ArgClass::Sse);
static_assert(AbiClass<double>::value == ArgClass::Sse);
static_assert(AbiClass<GuestPtr<char>>::value == ArgClass::Integer);
// psABI: __int128 is "treated as if implemented as struct { long low, high; }"
static_assert(AllOf(ClassifyType<__int128>(), {Eightbyte::Integer, Eightbyte::Integer}));
static_assert(!AbiClass<__int128>::is_memory);

// --- aggregates, one eightbyte ---------------------------------------------
static_assert(AllOf(ClassifyType<TwoInt>(), {Eightbyte::Integer}));
static_assert(AllOf(ClassifyType<TwoFloat>(), {Eightbyte::Sse}));
static_assert(AllOf(ClassifyType<IntFloat>(), {Eightbyte::Integer}));
static_assert(AllOf(ClassifyType<Chars8>(), {Eightbyte::Integer}));

// --- aggregates, two eightbytes --------------------------------------------
static_assert(AllOf(ClassifyType<TwoDouble>(), {Eightbyte::Sse, Eightbyte::Sse}));
static_assert(AllOf(ClassifyType<DoubleLong>(), {Eightbyte::Sse, Eightbyte::Integer}));
static_assert(AllOf(ClassifyType<LongDouble_>(), {Eightbyte::Integer, Eightbyte::Sse}));
static_assert(AllOf(ClassifyType<FourFloat>(), {Eightbyte::Sse, Eightbyte::Sse}));
// float at offset 0 and long at offset 8: the padding does NOT make eightbyte
// 0 INTEGER -- merge rule (b), NO_CLASS merges to the other class.
static_assert(AllOf(ClassifyType<OneFloatOneLong>(), {Eightbyte::Sse, Eightbyte::Integer}));
// Nested aggregates are classified through, and produce the same byte map as
// the flattened form.
static_assert(AllOf(ClassifyType<NestedSse>(), {Eightbyte::Sse, Eightbyte::Sse}));
static_assert(AllOf(ClassifyType<NestedInt>(), {Eightbyte::Integer}));

// --- MEMORY -----------------------------------------------------------------
static_assert(AbiClass<Big24>::value == ArgClass::Memory);   // 24 bytes, rule 5(c)
static_assert(AbiClass<Big17>::value == ArgClass::Memory);   // 17 bytes
static_assert(AbiClass<NonTrivial>::value == ArgClass::Memory);
static_assert(!kIsMarshalable<NonTrivial>);

// --- empty aggregate: consumes nothing --------------------------------------
static_assert(ClassifyType<Empty>().n == 0);
static_assert(AbiClass<Empty>::value == ArgClass::NoClass);

// --- explicit field list escape hatch ---------------------------------------
static_assert(AllOf(ClassifyType<WithArray>(), {Eightbyte::Integer, Eightbyte::Integer}));

// ===========================================================================
// Call plans
// ===========================================================================

// Six INTEGER registers, then the stack.
constexpr auto kInt9 = MakeCallPlan<long, long, long, long, long, long, long, long, long, long>();
static_assert(kInt9.int_used == 6);
static_assert(kInt9.args[0].reg[0] == 0 && kInt9.args[0].kind[0] == SlotKind::IntReg);
static_assert(kInt9.args[5].reg[0] == 5 && kInt9.args[5].kind[0] == SlotKind::IntReg);
static_assert(kInt9.args[6].on_stack && kInt9.args[6].stack_off == 0);
static_assert(kInt9.args[7].on_stack && kInt9.args[7].stack_off == 8);
static_assert(kInt9.args[8].on_stack && kInt9.args[8].stack_off == 16);
static_assert(kInt9.stack_bytes == 24 && kInt9.stack_area == 32);

// Eight vector registers, then the stack.
constexpr auto kDbl10 = MakeCallPlan<double, double, double, double, double, double, double,
                                     double, double, double, double>();
static_assert(kDbl10.sse_used == 8);
static_assert(kDbl10.int_used == 0);
static_assert(kDbl10.args[7].kind[0] == SlotKind::SseReg && kDbl10.args[7].reg[0] == 7);
static_assert(kDbl10.args[8].on_stack && kDbl10.args[8].stack_off == 0);
static_assert(kDbl10.args[9].on_stack && kDbl10.args[9].stack_off == 8);

// THE case that a shared counter would pass by accident only with few
// arguments: interleaved integers and doubles must advance separately.
constexpr auto kMix = MakeCallPlan<double, long, double, long, double, long, double, long,
                                   double>();
static_assert(kMix.int_used == 4 && kMix.sse_used == 4);
static_assert(kMix.args[0].kind[0] == SlotKind::IntReg && kMix.args[0].reg[0] == 0);
static_assert(kMix.args[1].kind[0] == SlotKind::SseReg && kMix.args[1].reg[0] == 0);
static_assert(kMix.args[2].kind[0] == SlotKind::IntReg && kMix.args[2].reg[0] == 1);
static_assert(kMix.args[3].kind[0] == SlotKind::SseReg && kMix.args[3].reg[0] == 1);
static_assert(kMix.args[6].kind[0] == SlotKind::IntReg && kMix.args[6].reg[0] == 3);
static_assert(kMix.args[7].kind[0] == SlotKind::SseReg && kMix.args[7].reg[0] == 3);
static_assert(kMix.stack_bytes == 0);

// MEMORY return: %rdi is the hidden buffer pointer and everything integer
// shifts right by one.  This is the rule docs/aot-design.md §6 calls the
// easiest to miss.
constexpr auto kMemRet = MakeCallPlan<Big24, long, long, long>();
static_assert(kMemRet.ret_memory);
static_assert(kMemRet.ret_size == 24);
static_assert(kMemRet.args[0].reg[0] == 1);  // %rsi, not %rdi
static_assert(kMemRet.args[1].reg[0] == 2);  // %rdx
static_assert(kMemRet.args[2].reg[0] == 3);  // %rcx
static_assert(kMemRet.int_used == 4);
// ... and the shift does NOT touch the SSE sequence.
constexpr auto kMemRetSse = MakeCallPlan<Big24, double, long>();
static_assert(kMemRetSse.args[0].kind[0] == SlotKind::SseReg && kMemRetSse.args[0].reg[0] == 0);
static_assert(kMemRetSse.args[1].kind[0] == SlotKind::IntReg && kMemRetSse.args[1].reg[0] == 1);

// Without a MEMORY return the same arguments start at %rdi.
constexpr auto kNoMemRet = MakeCallPlan<long, long, long, long>();
static_assert(!kNoMemRet.ret_memory);
static_assert(kNoMemRet.args[0].reg[0] == 0);
static_assert(kNoMemRet.int_used == 3);

// A MEMORY argument occupies the stack whatever else is free.
constexpr auto kMemArg = MakeCallPlan<long, Big24, long>();
static_assert(kMemArg.args[0].on_stack && kMemArg.args[0].stack_off == 0);
static_assert(kMemArg.args[1].kind[0] == SlotKind::IntReg && kMemArg.args[1].reg[0] == 0);
static_assert(kMemArg.stack_bytes == 24 && kMemArg.stack_area == 32);

// All-or-nothing spill: {SSE,SSE} needs two vector registers; with only one
// left the WHOLE argument goes to the stack and no half-assignment survives.
constexpr auto kSpill = MakeCallPlan<double, double, double, double, double, double, double,
                                     double, TwoDouble>();
static_assert(kSpill.sse_used == 7);
static_assert(kSpill.args[7].on_stack);
static_assert(kSpill.args[7].stack_off == 0);
// One vector register short is enough to spill: after 7 doubles, xmm7 is the
// only one left and {SSE,SSE} needs two.
constexpr auto kSpill8 = MakeCallPlan<double, double, double, double, double, double, double,
                                      double, double, TwoDouble>();
static_assert(kSpill8.sse_used == 8);
static_assert(kSpill8.args[8].on_stack);

// A later, smaller argument may still take a register after an earlier one
// spilled -- the psABI reverts assignments per argument, it does not close the
// register file.
constexpr auto kLateReg = MakeCallPlan<double, double, double, double, double, double, double,
                                       double, TwoDouble, double>();
static_assert(kLateReg.args[7].on_stack);
static_assert(kLateReg.args[8].kind[0] == SlotKind::SseReg && kLateReg.args[8].reg[0] == 7);

// Return-value classes.
static_assert(MakeCallPlan<TwoDouble>().ret_eb[0] == Eightbyte::Sse);
static_assert(MakeCallPlan<TwoDouble>().ret_eb[1] == Eightbyte::Sse);
static_assert(MakeCallPlan<DoubleLong>().ret_eb[0] == Eightbyte::Sse);
static_assert(MakeCallPlan<DoubleLong>().ret_eb[1] == Eightbyte::Integer);
static_assert(MakeCallPlan<TwoInt>().ret_n_eb == 1);
static_assert(MakeCallPlan<void>().ret_n_eb == 0);
static_assert(!MakeCallPlan<void>().ret_memory);

// Stack alignment: the argument area is always a multiple of 16, so %rsp is
// 16-byte aligned at the call and ≡ 8 (mod 16) inside the callee.
static_assert(kInt9.stack_area % 16 == 0);
static_assert(kMemArg.stack_area % 16 == 0);
static_assert(MakeCallPlan<void, Big17>().stack_area == 32);

// Default argument promotions for a variadic tail.
static_assert(std::is_same_v<VarargPromote<float>, double>);
static_assert(std::is_same_v<VarargPromote<char>, int>);
static_assert(std::is_same_v<VarargPromote<short>, int>);
static_assert(std::is_same_v<VarargPromote<bool>, int>);
static_assert(std::is_same_v<VarargPromote<double>, double>);
static_assert(std::is_same_v<VarargPromote<long>, long>);

#endif  // SVM_ABI_STATIC_ASSERTS_OFF

}  // namespace

TEST_CASE("SysV classification is resolved at compile time") {
#ifndef SVM_ABI_STATIC_ASSERTS_OFF
    // Everything of substance above is a static_assert; this case exists so the
    // translation unit reports as a test and so the runtime-visible plan can be
    // spot-checked.
    STATIC_REQUIRE(kInt9.int_used == 6);
    STATIC_REQUIRE(kMemRet.ret_memory);
    CHECK(kIntArgRegs[0] == 7);   // RDI
    CHECK(kIntArgRegs[1] == 6);   // RSI
    CHECK(kIntArgRegs[2] == 2);   // RDX
    CHECK(kIntArgRegs[3] == 1);   // RCX
    CHECK(kIntArgRegs[4] == 8);   // R8
    CHECK(kIntArgRegs[5] == 9);   // R9
    CHECK(kIntRetRegs[0] == 0);   // RAX
    CHECK(kIntRetRegs[1] == 2);   // RDX
#else
    SUCCEED("compile-time assertions disabled for mutation testing");
#endif
}
