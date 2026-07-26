//
// Compile-time x86-64 System V ABI argument classification.
//
// Everything here is consteval/constexpr: by the time GuestFn::operator()
// runs, "which register does argument 3 go in" is a constant, not a decision.
// That is the explicit requirement in docs/aot-design.md §6 ("ABI 分类必须在
// 编译期完成 ... 通过可变参数模板，静态解析 Caller 符号函数的 ABI").
//
// REFERENCE.  Section numbers below are from the System V Application Binary
// Interface, AMD64 Architecture Processor Supplement (psABI) v1.0, §3.2.3
// "Parameter Passing".  The rules implemented, verbatim in structure:
//
//   Classification of basic types
//     - _Bool, char, short, int, long, long long, pointers  -> INTEGER
//     - float, double                                       -> SSE
//     - __int128    -> "treated as if implemented as struct { long low, high; }"
//                      i.e. INTEGER INTEGER
//     - long double -> X87 + X87UP        (REJECTED here, see LIMITATIONS)
//
//   Classification of aggregates (structs, unions, arrays)
//     1. size > eight eightbytes, or unaligned fields          -> MEMORY
//     2. C++ non-trivial for the purposes of calls -> passed by *invisible
//        reference* (an INTEGER pointer).  NOT the same thing as MEMORY.
//        See LIMITATIONS.
//     3. size > one eightbyte: classify each eightbyte separately, each
//        initialized to NO_CLASS
//     4. classify each field recursively; merge pairwise into the eightbyte
//        it lands in:
//          (a) equal            -> that class
//          (b) one is NO_CLASS  -> the other
//          (c) one is MEMORY    -> MEMORY
//          (d) one is INTEGER   -> INTEGER
//          (e) any X87/X87UP/COMPLEX_X87 -> MEMORY
//          (f) otherwise        -> SSE
//     5. post-merger cleanup:
//          (a) any MEMORY eightbyte -> whole argument in memory
//          (b) X87UP not adjacent to X87 -> memory
//          (c) size > two eightbytes and NOT (first is SSE and all others
//              SSEUP) -> memory.  With no __m128/__m256 support that reduces
//              to: any aggregate above 16 bytes is MEMORY.
//          (d) SSEUP not preceded by SSE/SSEUP -> converted to SSE
//
//   Assignment
//     - INTEGER: %rdi %rsi %rdx %rcx %r8 %r9, then the stack
//     - SSE:     %xmm0..%xmm7, then the stack
//     - "if there are no registers available for ANY eightbyte of an
//       argument, the whole argument is passed on the stack.  If registers
//       have already been assigned for some eightbytes of such an argument,
//       the assignments get reverted."  -> per argument, and a later, smaller
//       argument may still take a register.  Implemented in MakeCallPlan.
//     - MEMORY return value: the caller allocates the buffer and passes its
//       address in %rdi "as if it were the first argument"; every INTEGER
//       argument therefore shifts one register to the right.  On return %rax
//       holds that same address.
//     - stack arguments are laid out left-to-right at increasing offsets from
//       the argument area base (equivalently: "pushed in reversed order"),
//       each at an offset respecting max(8, alignof(T)).
//
// LIMITATIONS (deliberate, and compile-time errors rather than silent guesses)
//   * long double / __float128 / _Decimal* / complex: rejected.  The host is
//     arm64 macOS where `long double` is 8 bytes, so no host type can even
//     represent the x86-64 80-bit value; classifying it would be a lie.
//   * __m128/__m256 (SSEUP): not supported.  No SSEUP class is ever produced,
//     which is why rule 5(c) collapses to "> 16 bytes -> MEMORY".
//   * C++ types that are non-trivial for the purposes of calls: rule 2 says
//     they are passed by invisible reference.  This header reports MEMORY for
//     them (that is the wording used in the task brief) but GuestFn refuses to
//     marshal them at all — see kIsMarshalable — so the discrepancy can never
//     produce a wrong call.
//   * unions and aggregates containing C arrays cannot be walked
//     automatically (see FieldsOf); specialize AbiFields<T> for those.
//   * packed / over-aligned aggregates fail the layout self-check below with
//     a static_assert; specialize AbiOverride<T> to force MEMORY.
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

#include "guest_ptr.h"

namespace swift::guest_call {

// psABI §3.2.3 classes.  SseUp / X87 / X87Up / ComplexX87 are present so the
// merge rules can be written exactly as the document states them, even though
// no supported host type produces them.
enum class Eightbyte : std::uint8_t {
    NoClass = 0,
    Integer,
    Sse,
    SseUp,
    X87,
    X87Up,
    ComplexX87,
    Memory,
};

// The coarse per-argument answer the task asks AbiClass<T> for.
enum class ArgClass : std::uint8_t { NoClass, Integer, Sse, Memory };

inline constexpr unsigned kMaxEightbytes = 8;   // eight eightbytes = 64 bytes
inline constexpr unsigned kMaxIntArgRegs = 6;   // rdi rsi rdx rcx r8 r9
inline constexpr unsigned kMaxSseArgRegs = 8;   // xmm0..xmm7
inline constexpr unsigned kMaxFields = 16;      // auto field-walk limit

// ---------------------------------------------------------------------------
// Escape hatches
// ---------------------------------------------------------------------------

// Specialize with `using types = AbiFieldList<F0, F1, ...>;` to describe an
// aggregate the automatic walker cannot handle.  The list may be FLATTENED
// (nested aggregates spelled out as their members): SysV classification is a
// byte map, and flattening a nested aggregate preserves the byte map exactly.
template <typename... Fs>
struct AbiFieldList {
    static constexpr std::size_t size = sizeof...(Fs);
};
template <typename T>
struct AbiFields;  // primary template intentionally undefined

// Specialize with `static constexpr bool force_memory = true;` for types whose
// layout cannot be modeled (packed structs, over-aligned aggregates).
template <typename T>
struct AbiOverride {
    static constexpr bool force_memory = false;
};

namespace abi_detail {

// --- pairwise merge, psABI §3.2.3 step 4 -----------------------------------
constexpr Eightbyte Merge(Eightbyte a, Eightbyte b) {
    if (a == b) {
        return a;  // (a)
    }
    if (a == Eightbyte::NoClass) {
        return b;  // (b)
    }
    if (b == Eightbyte::NoClass) {
        return a;  // (b)
    }
    if (a == Eightbyte::Memory || b == Eightbyte::Memory) {
        return Eightbyte::Memory;  // (c)
    }
    if (a == Eightbyte::Integer || b == Eightbyte::Integer) {
        return Eightbyte::Integer;  // (d)
    }
    const auto x87ish = [](Eightbyte e) {
        return e == Eightbyte::X87 || e == Eightbyte::X87Up || e == Eightbyte::ComplexX87;
    };
    if (x87ish(a) || x87ish(b)) {
        return Eightbyte::Memory;  // (e)
    }
    return Eightbyte::Sse;  // (f)
}

// --- field enumeration ------------------------------------------------------
//
// Aggregate members are discovered by structured bindings, with the arity
// found by probing how many initializer-clauses T{...} accepts.  The probe
// type converts to anything *except* T itself (otherwise T{x} would be a copy
// construction and every aggregate would report arity 1).
//
// Nested aggregates are handled correctly: because the probe converts to the
// nested struct type directly, the compiler prefers that over brace elision,
// so the arity comes out as the true member count.  C ARRAY members are the
// one case that breaks — a probe cannot initialize `int[3]`, so brace elision
// kicks in and the arity comes out inflated.  The structured binding then
// fails to compile (loudly); AbiFields<T> is the way out.
template <typename T>
struct AnyExcept {
    template <typename U>
        requires(!std::is_same_v<std::remove_cvref_t<U>, T>)
    constexpr operator U() const;
};

template <typename T, std::size_t>
using AnyAt = AnyExcept<T>;

template <typename T, typename Seq>
struct BraceOk : std::false_type {};
template <typename T, std::size_t... I>
struct BraceOk<T, std::index_sequence<I...>>
        : std::bool_constant<requires { T{AnyAt<T, I>{}...}; }> {};

template <typename T, std::size_t N>
constexpr std::size_t FindArity() {
    if constexpr (N == 0) {
        return 0;
    } else if constexpr (BraceOk<T, std::make_index_sequence<N>>::value) {
        return N;
    } else {
        return FindArity<T, N - 1>();
    }
}

template <std::size_t N, typename T>
constexpr auto TieFields(T& t) {
    if constexpr (N == 0) {
        (void)t;
        return std::tuple<>{};
    } else if constexpr (N == 1) { auto& [a] = t; return std::tie(a);
    } else if constexpr (N == 2) { auto& [a, b] = t; return std::tie(a, b);
    } else if constexpr (N == 3) { auto& [a, b, c] = t; return std::tie(a, b, c);
    } else if constexpr (N == 4) { auto& [a, b, c, d] = t; return std::tie(a, b, c, d);
    } else if constexpr (N == 5) { auto& [a, b, c, d, e] = t; return std::tie(a, b, c, d, e);
    } else if constexpr (N == 6) { auto& [a, b, c, d, e, f] = t; return std::tie(a, b, c, d, e, f);
    } else if constexpr (N == 7) { auto& [a, b, c, d, e, f, g] = t;
        return std::tie(a, b, c, d, e, f, g);
    } else if constexpr (N == 8) { auto& [a, b, c, d, e, f, g, h] = t;
        return std::tie(a, b, c, d, e, f, g, h);
    } else if constexpr (N == 9) { auto& [a, b, c, d, e, f, g, h, i] = t;
        return std::tie(a, b, c, d, e, f, g, h, i);
    } else if constexpr (N == 10) { auto& [a, b, c, d, e, f, g, h, i, j] = t;
        return std::tie(a, b, c, d, e, f, g, h, i, j);
    } else if constexpr (N == 11) { auto& [a, b, c, d, e, f, g, h, i, j, k] = t;
        return std::tie(a, b, c, d, e, f, g, h, i, j, k);
    } else if constexpr (N == 12) { auto& [a, b, c, d, e, f, g, h, i, j, k, l] = t;
        return std::tie(a, b, c, d, e, f, g, h, i, j, k, l);
    } else if constexpr (N == 13) { auto& [a, b, c, d, e, f, g, h, i, j, k, l, m] = t;
        return std::tie(a, b, c, d, e, f, g, h, i, j, k, l, m);
    } else if constexpr (N == 14) { auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n] = t;
        return std::tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n);
    } else if constexpr (N == 15) { auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n, o] = t;
        return std::tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o);
    } else {
        static_assert(N <= 16, "aggregate has more fields than the walker supports; "
                               "specialize AbiFields<T>");
        auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p] = t;
        return std::tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p);
    }
}

template <typename Tup>
struct TupleToList;
template <typename... F>
struct TupleToList<std::tuple<F...>> {
    using type = AbiFieldList<std::remove_cvref_t<F>...>;
};

template <typename T>
struct AutoFields {
    static constexpr std::size_t arity = FindArity<T, kMaxFields>();
    using type = typename TupleToList<
            std::remove_cvref_t<decltype(TieFields<arity>(*static_cast<T*>(nullptr)))>>::type;
};

template <typename T>
concept HasExplicitFields = requires { typename AbiFields<T>::types; };

template <typename T, bool Explicit>
struct FieldsSelector {
    using type = typename AutoFields<T>::type;
};
template <typename T>
struct FieldsSelector<T, true> {
    using type = typename AbiFields<T>::types;
};

template <typename T>
using FieldsOf = typename FieldsSelector<T, HasExplicitFields<T>>::type;

// --- layout simulation ------------------------------------------------------
//
// Reproduces the C/Itanium layout algorithm for a standard-layout struct with
// no bitfields, no inheritance and no [[no_unique_address]] overlap, then
// cross-checks the result against the real sizeof/alignof.  A mismatch means
// the modeled field list is not what the compiler actually laid out (packed,
// over-aligned, bitfields, an array member the walker flattened) -- and rather
// than classify from a wrong byte map, that is a hard error.
struct Layout {
    unsigned size{};
    unsigned align{1};
    unsigned count{};
    unsigned offsets[kMaxFields]{};
    bool overflow{};
};

constexpr unsigned AlignUp(unsigned v, unsigned a) { return (v + a - 1) & ~(a - 1); }

template <typename... F>
constexpr Layout ComputeLayout(AbiFieldList<F...>) {
    Layout l{};
    unsigned off = 0;
    unsigned al = 1;
    unsigned i = 0;
    const auto step = [&](unsigned fsize, unsigned falign) {
        if (falign > al) {
            al = falign;
        }
        off = AlignUp(off, falign);
        if (i < kMaxFields) {
            l.offsets[i] = off;
        } else {
            l.overflow = true;
        }
        ++i;
        off += fsize;
    };
    (step(static_cast<unsigned>(sizeof(F)), static_cast<unsigned>(alignof(F))), ...);
    l.count = i;
    l.align = al;
    l.size = AlignUp(off, al);
    return l;
}

// --- scalar predicates ------------------------------------------------------

template <typename T>
inline constexpr bool kIsInt128 =
        std::is_same_v<T, __int128> || std::is_same_v<T, unsigned __int128>;

template <typename T>
inline constexpr bool kIsSseScalar = std::is_same_v<T, float> || std::is_same_v<T, double>;

template <typename T>
inline constexpr bool kIsIntegerScalar =
        (std::is_integral_v<T> && !kIsSseScalar<T>) || std::is_enum_v<T> || kIsGuestPtr<T> ||
        kIsInt128<T>;

template <typename T>
inline constexpr bool kIsScalarAbi = kIsSseScalar<T> || kIsIntegerScalar<T>;

// Types the layer refuses outright: it cannot represent them faithfully on an
// arm64 host, so it must not pretend to.
template <typename T>
inline constexpr bool kIsRejected =
        std::is_same_v<std::remove_cv_t<T>, long double> ||
        std::is_pointer_v<T> || std::is_reference_v<T> || std::is_member_pointer_v<T> ||
        std::is_union_v<T>;

}  // namespace abi_detail

// ---------------------------------------------------------------------------
// The classification result
// ---------------------------------------------------------------------------

struct TypeClassInfo {
    unsigned size{};
    unsigned align{};
    unsigned n{};  // number of eightbytes actually passed (0 = nothing)
    Eightbyte eb[kMaxEightbytes]{};
    bool memory{};
};

namespace abi_detail {

// Merges the class of a scalar leaf occupying [base, base+size) into the
// eightbyte map.  psABI §3.2.3 step 4: "Each field of an object is classified
// recursively so that always two fields are considered."
constexpr void MergeLeaf(Eightbyte cls, unsigned base, unsigned size, Eightbyte* eb) {
    const unsigned first = base / 8;
    const unsigned last = (base + size - 1) / 8;
    for (unsigned i = first; i <= last && i < kMaxEightbytes; ++i) {
        eb[i] = Merge(eb[i], cls);
    }
}

template <typename T>
constexpr void FillClasses(unsigned base, Eightbyte* eb);

template <typename... F, std::size_t... I>
constexpr void FillFields(const Layout& l, unsigned base, Eightbyte* eb,
                          AbiFieldList<F...>, std::index_sequence<I...>) {
    (FillClasses<F>(base + l.offsets[I], eb), ...);
}

template <typename T>
constexpr void FillClasses(unsigned base, Eightbyte* eb) {
    static_assert(!kIsRejected<T>,
                  "x86-64 SysV: unsupported member type (long double, raw pointer, "
                  "reference, or union). Use GuestPtr<T> for pointers; specialize "
                  "AbiFields<T> for unions.");
    if constexpr (std::is_array_v<T>) {
        using E = std::remove_extent_t<T>;
        constexpr unsigned n = static_cast<unsigned>(sizeof(T) / sizeof(E));
        for (unsigned i = 0; i < n; ++i) {
            FillClasses<E>(base + i * static_cast<unsigned>(sizeof(E)), eb);
        }
    } else if constexpr (kIsSseScalar<T>) {
        MergeLeaf(Eightbyte::Sse, base, sizeof(T), eb);
    } else if constexpr (kIsInt128<T>) {
        // "treated as if it were implemented as struct { long low, high; }"
        MergeLeaf(Eightbyte::Integer, base, 8, eb);
        MergeLeaf(Eightbyte::Integer, base + 8, 8, eb);
    } else if constexpr (kIsIntegerScalar<T>) {
        MergeLeaf(Eightbyte::Integer, base, sizeof(T), eb);
    } else {
        using Fields = FieldsOf<T>;
        constexpr Layout l = ComputeLayout(Fields{});
        static_assert(!l.overflow, "aggregate has too many fields; specialize AbiFields<T>");
        // An empty aggregate has sizeof == 1 with no fields: nothing to
        // classify, and the layout cross-check below does not apply (the
        // padding byte is not data).  gcc/clang pass such an argument in no
        // register and no stack slot at all.
        static_assert(Fields::size > 0 || sizeof(T) == 1,
                      "aggregate has no visible fields but is larger than one byte; "
                      "specialize AbiFields<T>");
        static_assert(Fields::size == 0 || (l.size == sizeof(T) && l.align == alignof(T)),
                      "the modeled field layout of this aggregate does not match its real "
                      "sizeof/alignof. It is probably packed, over-aligned, has bitfields, "
                      "or has a C array member. Specialize AbiFields<T> (flattened field "
                      "list) or AbiOverride<T>::force_memory.");
        FillFields(l, base, eb, Fields{}, std::make_index_sequence<Fields::size>{});
    }
}

}  // namespace abi_detail

// The classification metafunction.  consteval so it can never accidentally
// become a runtime decision.
template <typename T>
consteval TypeClassInfo ClassifyType() {
    TypeClassInfo info{};
    if constexpr (std::is_void_v<T>) {
        return info;  // nothing passed / returned
    } else {
        static_assert(!abi_detail::kIsRejected<T>,
                      "x86-64 SysV: unsupported parameter/return type. Raw host pointers are "
                      "rejected on purpose -- use GuestPtr<T>. long double, references, "
                      "member pointers and unions are not modeled.");
        info.size = static_cast<unsigned>(sizeof(T));
        info.align = static_cast<unsigned>(alignof(T));
        info.n = (info.size + 7) / 8;

        // psABI rule 2: a C++ object that is non-trivial for the purposes of
        // calls is passed by invisible reference.  GuestFn rejects these
        // outright (kIsMarshalable), so reporting MEMORY here is only ever
        // observed by AbiClass<T> queries -- never used to build a call.
        //
        // Rule 1 (> eight eightbytes) and rule 5(c) (> two eightbytes without
        // the SSE/SSEUP vector shape, which we never produce) collapse to
        // "> 16 bytes -> MEMORY".  Both branches must be `if constexpr` with an
        // else: a plain early return would still INSTANTIATE the field walk
        // below, and a 200-byte struct would then hit the 16-field limit for
        // no reason.
        if constexpr (!std::is_trivially_copyable_v<T> || AbiOverride<T>::force_memory ||
                      sizeof(T) > 16) {
            info.memory = true;
            for (unsigned i = 0; i < info.n && i < kMaxEightbytes; ++i) {
                info.eb[i] = Eightbyte::Memory;
            }
            return info;
        } else {
            for (unsigned i = 0; i < kMaxEightbytes; ++i) {
                info.eb[i] = Eightbyte::NoClass;
            }
            abi_detail::FillClasses<T>(0, info.eb);
            // Post-merger 5(a).
            for (unsigned i = 0; i < info.n; ++i) {
                if (info.eb[i] == Eightbyte::Memory) {
                    info.memory = true;
                }
            }
            // Post-merger 5(b): X87UP must follow X87.  Unreachable with the
            // supported type set (long double is rejected), kept so the rule
            // set is complete rather than "complete for what we happen to hit".
            for (unsigned i = 0; i < info.n; ++i) {
                if (info.eb[i] == Eightbyte::X87Up &&
                    (i == 0 || info.eb[i - 1] != Eightbyte::X87)) {
                    info.memory = true;
                }
            }
            // Post-merger 5(d).
            for (unsigned i = 0; i < info.n; ++i) {
                if (info.eb[i] == Eightbyte::SseUp &&
                    (i == 0 || (info.eb[i - 1] != Eightbyte::Sse &&
                                info.eb[i - 1] != Eightbyte::SseUp))) {
                    info.eb[i] = Eightbyte::Sse;
                }
            }
            // An empty aggregate (sizeof == 1, no fields) classifies to
            // NO_CLASS and consumes nothing, matching gcc/clang.
            if (info.n == 1 && info.eb[0] == Eightbyte::NoClass) {
                info.n = 0;
            }
            return info;
        }
    }
}

// The metafunction the design doc names: INTEGER / SSE / MEMORY for a type.
template <typename T>
struct AbiClass {
    static constexpr TypeClassInfo info = ClassifyType<T>();
    static constexpr unsigned eightbytes = info.n;
    static constexpr bool is_memory = info.memory;
    static constexpr ArgClass value = [] {
        if (info.memory) {
            return ArgClass::Memory;
        }
        if (info.n == 0) {
            return ArgClass::NoClass;
        }
        for (unsigned i = 0; i < info.n; ++i) {
            if (info.eb[i] == Eightbyte::Integer) {
                return ArgClass::Integer;
            }
        }
        return ArgClass::Sse;
    }();
    static constexpr Eightbyte Class(unsigned i) { return info.eb[i]; }
};

// Types GuestFn will marshal.  A raw host pointer is *not* one of them: that
// is the compile-time half of the GuestPtr ownership decision.
namespace abi_detail {
template <typename T, bool = std::is_void_v<T> || std::is_reference_v<T> ||
                             std::is_function_v<T> || std::is_array_v<T>>
struct Marshalable {  // the "needs sizeof" case
    static constexpr bool value = std::is_trivially_copyable_v<T> && !kIsRejected<T> &&
                                  sizeof(T) <= 512;
};
template <typename T>
struct Marshalable<T, true> {  // void / reference / function / array
    static constexpr bool value = std::is_void_v<T>;
};
}  // namespace abi_detail

template <typename T>
inline constexpr bool kIsMarshalable = abi_detail::Marshalable<T>::value;

// ---------------------------------------------------------------------------
// Call plan: the whole assignment, computed at compile time
// ---------------------------------------------------------------------------

enum class SlotKind : std::uint8_t { IntReg, SseReg, Stack };

inline constexpr unsigned kMaxPlanArgs = 24;

struct ArgPlan {
    unsigned size{};
    unsigned align{};
    unsigned n_eb{};
    bool on_stack{};        // whole argument lives in the stack argument area
    unsigned stack_off{};   // byte offset from the argument area base
    SlotKind kind[kMaxEightbytes]{};
    std::uint8_t reg[kMaxEightbytes]{};  // index into the INTEGER / SSE sequence
};

struct CallPlan {
    unsigned nargs{};
    ArgPlan args[kMaxPlanArgs]{};

    bool ret_memory{};
    unsigned ret_n_eb{};
    unsigned ret_size{};
    unsigned ret_align{};
    Eightbyte ret_eb[2]{};

    unsigned int_used{};    // INTEGER registers consumed (incl. the hidden ptr)
    unsigned sse_used{};    // vector registers consumed -> the value for %al
    unsigned stack_bytes{}; // argument area actually used
    unsigned stack_area{};  // stack_bytes rounded up to 16
};

template <typename Ret, typename... Args>
consteval CallPlan MakeCallPlan() {
    static_assert(sizeof...(Args) <= kMaxPlanArgs, "too many arguments");
    CallPlan p{};
    const TypeClassInfo infos[] = {ClassifyType<Args>()..., TypeClassInfo{}};
    const TypeClassInfo ret = ClassifyType<Ret>();

    p.nargs = sizeof...(Args);
    p.ret_memory = ret.memory;
    p.ret_n_eb = ret.n;
    p.ret_size = ret.size;
    p.ret_align = ret.align;
    p.ret_eb[0] = ret.eb[0];
    p.ret_eb[1] = ret.eb[1];

    // "the caller ... passes the address of this storage in %rdi as if it were
    // the first argument": every INTEGER argument shifts right by one.
    unsigned int_next = ret.memory ? 1u : 0u;
    unsigned sse_next = 0;
    unsigned sp = 0;

    for (unsigned i = 0; i < p.nargs; ++i) {
        const TypeClassInfo& c = infos[i];
        ArgPlan a{};
        a.size = c.size;
        a.align = c.align;
        a.n_eb = c.n;
        if (c.n == 0) {
            // Empty aggregate: consumes neither a register nor a stack slot.
            p.args[i] = a;
            continue;
        }
        bool to_stack = c.memory;
        if (!to_stack) {
            unsigned need_int = 0;
            unsigned need_sse = 0;
            for (unsigned e = 0; e < c.n; ++e) {
                if (c.eb[e] == Eightbyte::Integer) {
                    ++need_int;
                } else if (c.eb[e] == Eightbyte::Sse) {
                    ++need_sse;
                }
                // SseUp shares the previous vector register: no new one.
            }
            // All-or-nothing: "if there are no registers available for any
            // eightbyte of an argument, the whole argument is passed on the
            // stack ... assignments get reverted."
            if (int_next + need_int > kMaxIntArgRegs || sse_next + need_sse > kMaxSseArgRegs) {
                to_stack = true;
            } else {
                for (unsigned e = 0; e < c.n; ++e) {
                    if (c.eb[e] == Eightbyte::Integer) {
                        a.kind[e] = SlotKind::IntReg;
                        a.reg[e] = static_cast<std::uint8_t>(int_next++);
                    } else if (c.eb[e] == Eightbyte::Sse) {
                        a.kind[e] = SlotKind::SseReg;
                        a.reg[e] = static_cast<std::uint8_t>(sse_next++);
                    } else if (c.eb[e] == Eightbyte::SseUp) {
                        a.kind[e] = SlotKind::SseReg;
                        a.reg[e] = static_cast<std::uint8_t>(sse_next - 1);
                    }
                }
            }
        }
        if (to_stack) {
            a.on_stack = true;
            const unsigned slot_align = c.align < 8 ? 8 : c.align;
            sp = abi_detail::AlignUp(sp, slot_align);
            a.stack_off = sp;
            sp += abi_detail::AlignUp(c.size, 8);
        }
        p.args[i] = a;
    }

    p.int_used = int_next;
    p.sse_used = sse_next;
    p.stack_bytes = sp;
    p.stack_area = abi_detail::AlignUp(sp, 16);
    return p;
}

// psABI §3.2.3: the INTEGER argument sequence, as indices into
// x86::ThreadContext64::regs (see translator/x86/cpu.h: RDI=7, RSI=6, RDX=2,
// RCX=1, R8=8, R9=9).
inline constexpr std::uint8_t kIntArgRegs[kMaxIntArgRegs] = {7, 6, 2, 1, 8, 9};
// INTEGER return sequence: %rax, %rdx.
inline constexpr std::uint8_t kIntRetRegs[2] = {0, 2};

// ---------------------------------------------------------------------------
// Default argument promotions for the variadic tail (C11 6.5.2.2p6/p7).
// A guest `printf("%f", x)` must receive a double even if the host wrote 1.5f.
// ---------------------------------------------------------------------------
namespace abi_detail {
template <typename T>
struct VarargPromoteImpl {
    using type = T;
};
template <>
struct VarargPromoteImpl<float> {
    using type = double;
};
template <>
struct VarargPromoteImpl<bool> {
    using type = int;
};
template <>
struct VarargPromoteImpl<char> {
    using type = int;
};
template <>
struct VarargPromoteImpl<signed char> {
    using type = int;
};
template <>
struct VarargPromoteImpl<unsigned char> {
    using type = int;
};
template <>
struct VarargPromoteImpl<short> {
    using type = int;
};
template <>
struct VarargPromoteImpl<unsigned short> {
    using type = int;
};
}  // namespace abi_detail

template <typename T>
using VarargPromote = typename abi_detail::VarargPromoteImpl<std::remove_cvref_t<T>>::type;

}  // namespace swift::guest_call
