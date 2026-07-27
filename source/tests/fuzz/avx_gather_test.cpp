// ===========================================================================
// AVX2 gather (VSIB) against a ROSETTA oracle, on both back ends.
// ===========================================================================
//
// Covers vpgatherdd / vpgatherqd / vpgatherdq / vpgatherqq and their float
// twins vgatherdps / vgatherqps / vgatherdpd / vgatherqpd, at VEX.128 and
// VEX.256, over nine addressing/register shapes and nine index+mask input
// patterns -- 1296 rows, every one of them a value real x86-64 produced.
//
// WHY A HARDWARE ORACLE
// ---------------------
// Unicorn 2.1.4 refuses every VEX.L=1 encoding, and gather's semantics are
// almost entirely PLUMBING rather than arithmetic: which bit of the mask is
// consulted, which register the index comes from, whether a masked-off element
// is preserved or zeroed, how many elements a form actually gathers, and
// whether the mask register is cleared afterwards.  A hand-written model of
// that would only restate the implementation's own assumptions.  Rosetta 2
// executes AVX2 including the full 256-bit register file, so
// avx_gather_rosetta_ref.inc holds literal hardware output.
//
// WHAT THE DATA PINS DOWN, AS THE HARDWARE REPORTED IT
//
//   The mask is the element's MOST SIGNIFICANT BIT and nothing else.  The
//   "msb" and "alloff" pairs carry 0x7FFFFFFF, 0x40000000, 0x00000001 (all
//   off) next to 0x80000000, 0xC0000000, 0xFFFFFFFF (all on), and for 64-bit
//   mask elements the low dword is filled with 0x5A5A5A5A garbage.
//
//   A masked-off element is PRESERVED, not zeroed.  The destination is
//   poisoned per register and per byte before every row, so the "alloff" rows'
//   references carry that poison back verbatim -- an implementation that
//   zeroed instead shows up immediately.
//
//   The mask register is CLEARED.  Every reference row's mask-after field is
//   32 zero bytes, including at VEX.128 where the upper half must go too.  A
//   real gather loop reads the mask to decide whether it is done, so an
//   implementation that skipped this would hang the guest rather than
//   miscompute -- and it is the single easiest thing in the family to forget.
//
//   How much of the destination each form writes.  vpgatherqd VEX.128 gathers
//   two dwords and zeroes the other 24 bytes; VEX.256 gathers four and zeroes
//   16.  Every VEX.128 row's upper half is zero (contract C3).
//
//   Signed indices, and duplicate indices.  The "neg" pair is entirely
//   negative, the "dup" pair points every lane at the same element.
//
// Each block is a SINGLE instruction: the operand registers are written
// straight into ThreadContext64 and the answer read straight back, so a broken
// vmovdqu cannot mask a broken handler.  Every register except the destination
// and the mask is checked to be UNCHANGED afterwards, which also asserts that
// the index register survives.
//
// WHAT THIS FILE DELIBERATELY DOES NOT TEST
// -----------------------------------------
// The restartable-fault behaviour of a gather whose ENABLED lane faults.
// Rosetta aborts the whole process on that ("rosetta error: unexpectedly need
// to EmulateForward on a synchronous exception"), so there is no oracle for
// it, and SwiftVM cannot express it either -- no memory access in this runtime
// is restartable.  decoder_avx_gather.cc's header states the deviation.  The
// property that DOES matter for real code -- a masked-off lane performs no
// access and therefore cannot fault -- is a structural consequence of the
// per-element branch and is asserted directly by the last case in this file.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <sys/mman.h>
#include "runtime/backend/smc_tracker.h"
#include "runtime/frontend/x86/decoder.h"
#include "runtime/frontend/x86/vex_decoder.h"
#include "translator/x86/cpu.h"
#include "translator/x86/translator.h"

using namespace swift::translator::x86;
using namespace swift;

namespace {

struct AvxGatherRef {
    const char* name;
    int width;        // 128 or 256: the VEX.L the generator encoded
    const char* pair; // input-pattern name
    int dst;          // architectural vector register numbers
    int index;
    int mask;
    const char* enc;        // literal instruction bytes, hex
    const char* index_in;   // 32 bytes planted in the index register
    const char* mask_in;    // 32 bytes planted in the mask register
    const char* dst_after;  // 32 bytes hardware left in the destination
    const char* mask_after; // 32 bytes hardware left in the mask register
};
#include "avx_gather_rosetta_ref.inc"

// The instruction table, shared verbatim with the generator.  Only the NAME is
// consumed here -- the encoding comes from each row -- so this exists to pin
// coverage: every mnemonic below must have produced reference rows.
struct Entry {
    const char* name;
    int ebits;  // destination element bits
    int ibits;  // index element bits
};
constexpr Entry kEntries[] = {
#define SVM_GATHER(name, opcode, w, ebits, ibits) {#name, ebits, ibits},
#include "avx_gather_ops.inc"
};

// How many BYTES of the destination a form actually gathers.  Everything above
// that is architecturally zeroed, which is why a "the destination was
// preserved" assertion has to stop there: vpgatherqd VEX.128 preserves eight
// bytes and zeroes twenty-four.
size_t GatheredBytes(const char* mnemonic, int width) {
    for (const auto& e : kEntries) {
        if (std::strcmp(e.name, mnemonic) != 0) continue;
        const int wider = e.ebits > e.ibits ? e.ebits : e.ibits;
        return size_t(width / wider) * size_t(e.ebits) / 8u;
    }
    return 0;
}

using Vec256 = std::array<u8, 32>;

u8 Nib(char ch) { return u8(ch <= '9' ? ch - '0' : (ch | 0x20) - 'a' + 10); }

Vec256 ParseHex32(const char* h) {
    Vec256 v{};
    for (u32 i = 0; i < 32; ++i) {
        v[i] = u8((Nib(h[i * 2]) << 4) | Nib(h[i * 2 + 1]));
    }
    return v;
}

std::vector<u8> ParseHex(const char* h) {
    std::vector<u8> v;
    for (size_t i = 0; h[i] != '\0' && h[i + 1] != '\0'; i += 2) {
        v.push_back(u8((Nib(h[i]) << 4) | Nib(h[i + 1])));
    }
    return v;
}

std::string Hex(const Vec256& v) {
    std::string s;
    for (const u8 x : v) {
        s += fmt::format("{:02x}", x);
    }
    return s;
}

// Must match GATHER_POISON in avx_gather_rosetta_ref.c.
Vec256 Poison(u32 reg) {
    Vec256 v{};
    for (u32 j = 0; j < 32; ++j) {
        v[j] = u8(0xA5 ^ (reg * 32 + j));
    }
    return v;
}

// Must match the generator's layout and TABLE_BYTE formula.
constexpr u64 kOffPoison = 0x100;
constexpr u64 kOffTable = 0x400;
constexpr u64 kTableSize = 4096;
constexpr u64 kTableMid = kOffTable + 0x800;
constexpr u64 kDataSize = kOffTable + kTableSize;
u8 TableByte(u64 b) { return u8(b * 31u + 7u); }

}  // namespace

// ---------------------------------------------------------------------------
// The differential itself.
// ---------------------------------------------------------------------------
TEST_CASE("x86 avx2 gather vs rosetta reference") {
    const char* avx_env = std::getenv("SVM_AVX");
    if (!avx_env || std::strcmp(avx_env, "0") == 0) {
        SUCCEED("SVM_AVX is not set; AVX2 gather Rosetta differential skipped");
        return;
    }

    // ---- properties of the reference data this case depends on ------------
    // Asserted rather than trusted: without these the differential could still
    // pass while having stopped testing what it exists for.
    {
        for (const auto& e : kEntries) {
            size_t rows = 0;
            for (const auto& r : kAvxGatherRefs) {
                if (std::strcmp(r.name, e.name) == 0) ++rows;
            }
            INFO("no reference rows for " << e.name
                                          << " -- the generator did not cover it, or Rosetta "
                                             "refused every encoding of it");
            REQUIRE(rows > 0);
        }
    }
    {
        // THE MASK IS ZEROED.  Hardware said so on every single row, VEX.128
        // included (where the upper 128 bits must go too).
        for (const auto& r : kAvxGatherRefs) {
            const auto m = ParseHex32(r.mask_after);
            INFO(r.name << " " << r.pair << ": the reference mask-after is not all zero");
            REQUIRE(std::all_of(m.begin(), m.end(), [](u8 x) { return x == 0; }));
        }
        REQUIRE(std::size(kAvxGatherRefs) > 1000);
    }
    {
        // Contract C3 as the hardware reported it: every VEX.128 row's
        // destination has a zeroed upper half, against a poisoned register.
        size_t checked = 0;
        for (const auto& r : kAvxGatherRefs) {
            if (r.width != 128) continue;
            const auto v = ParseHex32(r.dst_after);
            INFO(r.name << " " << r.pair << ": VEX.128 reference has a non-zero upper half");
            REQUIRE(std::all_of(v.begin() + 16, v.end(), [](u8 x) { return x == 0; }));
            ++checked;
        }
        REQUIRE(checked > 400);
    }
    {
        // A fully masked-off gather PRESERVES the destination.  The "alloff"
        // rows must come back as the poison the destination was loaded with,
        // in the low half; the high half still follows the form's zeroing.
        size_t seen = 0;
        for (const auto& r : kAvxGatherRefs) {
            if (std::strcmp(r.pair, "alloff") != 0) continue;
            const auto v = ParseHex32(r.dst_after);
            const auto p = Poison(u32(r.dst));
            const size_t used = GatheredBytes(r.name, r.width);
            INFO(r.name << " L" << r.width
                        << " alloff: the reference does not preserve the gathered range");
            REQUIRE(used > 0);
            REQUIRE(std::equal(v.begin(), v.begin() + long(used), p.begin()));
            // ...and zeroes everything above it, at every width.
            REQUIRE(std::all_of(v.begin() + long(used), v.end(), [](u8 x) { return x == 0; }));
            ++seen;
        }
        REQUIRE(seen > 100);
    }
    {
        // The narrow-destination forms zero what they do not gather.  A
        // vpgatherqd/vgatherqps at VEX.128 writes 8 bytes and zeroes 24.
        size_t seen = 0;
        for (const auto& r : kAvxGatherRefs) {
            const bool qd = std::strcmp(r.name, "vpgatherqd") == 0 ||
                            std::strcmp(r.name, "vgatherqps") == 0;
            if (!qd || r.width != 128) continue;
            const auto v = ParseHex32(r.dst_after);
            INFO(r.name << " " << r.pair << ": VEX.128 q-index d-element reference does not "
                                            "zero bytes 8..31");
            REQUIRE(std::all_of(v.begin() + 8, v.end(), [](u8 x) { return x == 0; }));
            ++seen;
        }
        REQUIRE(seen > 100);
    }

    // ---- harness -----------------------------------------------------------
    constexpr size_t kArenaSize = 0x400000;
    swift::runtime::backend::SmcTracker::SetEnabled(false);
    void* arena = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(arena != MAP_FAILED);
    const u64 base = reinterpret_cast<u64>(arena);
    const u64 stack = base + 0x200000;
    const u64 data = base + 0x300000;
    REQUIRE(data + kDataSize < base + kArenaSize);

    // The gathered table, by the generator's formula.
    for (u64 b = 0; b < kTableSize; ++b) {
        reinterpret_cast<u8*>(data + kOffTable)[b] = TableByte(b);
    }
    for (u32 reg = 0; reg < 16; ++reg) {
        const auto p = Poison(reg);
        std::memcpy(reinterpret_cast<void*>(data + kOffPoison + reg * 32), p.data(), 32);
    }

    const char* old_jit = std::getenv("SVM_ENABLE_JIT");
    const bool had_old_jit = old_jit != nullptr;
    const std::string old_jit_value = old_jit ? old_jit : "";
    setenv("SVM_ENABLE_JIT", "1", 1);
    auto* jit_instance = X86Instance::Make();
    setenv("SVM_ENABLE_JIT", "0", 1);
    auto* interp_instance = X86Instance::Make();
    if (had_old_jit) {
        setenv("SVM_ENABLE_JIT", old_jit_value.c_str(), 1);
    } else {
        unsetenv("SVM_ENABLE_JIT");
    }
    auto* jit_core = X86Core::Make(jit_instance);
    auto* interp_core = X86Core::Make(interp_instance);

    struct Out {
        std::array<Vec256, 16> ymm{};
        int exit{};
    };

    size_t code_cursor = 1;
    std::vector<std::string> problems;
    size_t comparisons = 0, bad_exits = 0, divergences = 0, mismatches = 0, bystanders = 0;

    const auto run_on = [&](X86Core* core, const std::vector<u8>& code, const AvxGatherRef& ref,
                            u64 code_addr) {
        std::memcpy(reinterpret_cast<void*>(code_addr), code.data(), code.size());
        auto& ctx = core->GetContext();
        for (u32 i = 0; i < 16; ++i) {
            const auto p = Poison(i);
            std::memcpy(ctx.xmms[i].b, p.data(), 16);
            std::memcpy(ctx.ymm_high[i].b, p.data() + 16, 16);
        }
        const auto index_in = ParseHex32(ref.index_in);
        const auto mask_in = ParseHex32(ref.mask_in);
        std::memcpy(ctx.xmms[ref.index].b, index_in.data(), 16);
        std::memcpy(ctx.ymm_high[ref.index].b, index_in.data() + 16, 16);
        std::memcpy(ctx.xmms[ref.mask].b, mask_in.data(), 16);
        std::memcpy(ctx.ymm_high[ref.mask].b, mask_in.data() + 16, 16);
        // Every GPR the shapes use as a VSIB base points at the middle of the
        // table, exactly as the generator's prologue arranged.
        const u64 table_mid = data + kTableMid;
        ctx.rsi.qword = table_mid;
        ctx.rbp.qword = table_mid;
        ctx.r12.qword = table_mid;
        ctx.r13.qword = table_mid;
        ctx.r14.qword = table_mid;
        ctx.rdi.qword = data;
        ctx.rax.qword = 0xDEADBEEFDEADBEEFull;
        ctx.rsp.qword = stack;
        ctx.rip.qword = code_addr;
        Out o;
        o.exit = int(core->Run());
        for (u32 i = 0; i < 16; ++i) {
            std::memcpy(o.ymm[i].data(), ctx.xmms[i].b, 16);
            std::memcpy(o.ymm[i].data() + 16, ctx.ymm_high[i].b, 16);
        }
        return o;
    };

    for (const auto& ref : kAvxGatherRefs) {
        auto code = ParseHex(ref.enc);
        code.push_back(0xF4);  // hlt
        const u64 code_addr = base + 0x1000 + code_cursor * 0x40;
        ++code_cursor;
        REQUIRE(code.size() < 0x40);
        REQUIRE(code_addr + 0x40 < stack);
        const std::string label =
                fmt::format("{}.L{}/{}/dst{}", ref.name, ref.width, ref.pair, ref.dst);

        const auto jit = run_on(jit_core, code, ref, code_addr);
        const auto itp = run_on(interp_core, code, ref, code_addr);
        ++comparisons;

        if (jit.exit != int(swift::translator::None)) {
            // FALLBACK / ILL_CODE both land here: the encoding was DECLINED
            // rather than mis-executed.  For a guest that is fatal, so it is a
            // failure of this case rather than a gap to note.
            if (bad_exits++ < 15) {
                problems.push_back(fmt::format(
                        "{}: block did not reach HLT (exit={}); encoding {} was not decoded",
                        label, jit.exit, ref.enc));
            }
            continue;
        }
        if (jit.ymm != itp.ymm || jit.exit != itp.exit) {
            if (divergences++ < 15) {
                problems.push_back(fmt::format(
                        "{}: JIT/interpreter divergence (dst {} vs {}, mask {} vs {})", label,
                        Hex(jit.ymm[size_t(ref.dst)]), Hex(itp.ymm[size_t(ref.dst)]),
                        Hex(jit.ymm[size_t(ref.mask)]), Hex(itp.ymm[size_t(ref.mask)])));
            }
        }

        const Vec256 want_dst = ParseHex32(ref.dst_after);
        const Vec256 want_msk = ParseHex32(ref.mask_after);
        for (const auto& [backend, got] : {std::pair<const char*, const Out*>{"jit", &jit},
                                           std::pair<const char*, const Out*>{"interp", &itp}}) {
            if (got->ymm[size_t(ref.dst)] != want_dst && mismatches++ < 15) {
                problems.push_back(fmt::format("{} [{}]: destination {}, Rosetta says {}", label,
                                               backend, Hex(got->ymm[size_t(ref.dst)]),
                                               Hex(want_dst)));
            }
            if (got->ymm[size_t(ref.mask)] != want_msk && mismatches++ < 15) {
                problems.push_back(fmt::format(
                        "{} [{}]: mask register {}, Rosetta says {} (a gather CLEARS its mask)",
                        label, backend, Hex(got->ymm[size_t(ref.mask)]), Hex(want_msk)));
            }
            // Every other vector register must be untouched -- including the
            // INDEX register, which a gather reads and never writes.
            for (u32 i = 0; i < 16; ++i) {
                if (int(i) == ref.dst || int(i) == ref.mask) continue;
                Vec256 expect = Poison(i);
                if (int(i) == ref.index) expect = ParseHex32(ref.index_in);
                if (got->ymm[i] != expect && bystanders++ < 15) {
                    problems.push_back(fmt::format("{} [{}]: ymm{} was modified: {} (want {})",
                                                   label, backend, i, Hex(got->ymm[i]),
                                                   Hex(expect)));
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // TWO GATHERS IN ONE BLOCK, and the mask-clear feedback loop.
    // -----------------------------------------------------------------------
    // Every row above is a single instruction, which leaves two things
    // untested: a block holding several gathers at once (each one emits a
    // branch per element, so a second one doubles the local labels and the
    // values live across them), and the CONSEQUENCE of clearing the mask.
    //
    // Both are covered by replaying `gather; gather'` where gather' is the
    // same encoding with its destination redirected to ymm3.  The first gather
    // zeroes the mask register, so the second one must gather NOTHING and
    // leave ymm3 exactly as it was -- and the first gather's own answer must
    // still equal the hardware reference, which pins that the second one did
    // not disturb it.  This is the shape of a real gather loop, where the mask
    // is what tells the code it is finished.
    size_t combos = 0, combo_bad = 0;
    for (const auto& ref : kAvxGatherRefs) {
        // Only rows whose destination is ymm0 can have ModRM.reg rewritten to
        // 3 without also touching VEX.R, and ymm3 must not already be in use.
        if (ref.dst != 0 || ref.index == 3 || ref.mask == 3) continue;
        auto first = ParseHex(ref.enc);
        auto second = first;
        // ModRM is the byte after the 3-byte VEX prefix and the opcode.
        second[4] = u8((second[4] & 0xC7) | (3 << 3));
        std::vector<u8> code = first;
        code.insert(code.end(), second.begin(), second.end());
        code.push_back(0xF4);  // hlt
        const u64 code_addr = base + 0x1000 + code_cursor * 0x40;
        ++code_cursor;
        REQUIRE(code.size() < 0x40);
        const std::string label = fmt::format("{}.L{}/{}/combo", ref.name, ref.width, ref.pair);

        const auto jit = run_on(jit_core, code, ref, code_addr);
        const auto itp = run_on(interp_core, code, ref, code_addr);
        ++combos;
        const Vec256 want_dst = ParseHex32(ref.dst_after);
        for (const auto& [backend, got] : {std::pair<const char*, const Out*>{"jit", &jit},
                                           std::pair<const char*, const Out*>{"interp", &itp}}) {
            if (got->exit != int(swift::translator::None)) {
                if (combo_bad++ < 10) {
                    problems.push_back(fmt::format("{} [{}]: two-gather block did not reach HLT",
                                                   label, backend));
                }
                continue;
            }
            if (got->ymm[0] != want_dst && combo_bad++ < 10) {
                problems.push_back(fmt::format(
                        "{} [{}]: the FIRST gather's answer changed when a second one followed "
                        "it: {} vs {}",
                        label, backend, Hex(got->ymm[0]), Hex(want_dst)));
            }
            // A fully masked-off gather still ZEROES the destination bits it
            // does not gather (the same rule the "alloff" references show),
            // so ymm3 must be the poison in the gathered range and zero above
            // it -- not untouched.
            Vec256 want3 = Poison(3);
            std::fill(want3.begin() + long(GatheredBytes(ref.name, ref.width)), want3.end(), u8(0));
            if (got->ymm[3] != want3 && combo_bad++ < 10) {
                problems.push_back(fmt::format(
                        "{} [{}]: the second gather wrote ymm3 ({}, want {}) even though the "
                        "first had cleared the mask -- a gather loop would never terminate",
                        label, backend, Hex(got->ymm[3]), Hex(want3)));
            }
        }
    }

    munmap(arena, kArenaSize);
    swift::runtime::backend::SmcTracker::SetEnabled(true);

    for (size_t i = 0; i < problems.size() && i < 30; ++i) {
        UNSCOPED_INFO(problems[i]);
    }
    CHECK(bad_exits == 0);
    CHECK(divergences == 0);
    CHECK(mismatches == 0);
    CHECK(bystanders == 0);
    CHECK(combo_bad == 0);
    // 8 opcodes x 2 widths x 9 shapes x 9 input patterns.  Pinned so a coverage
    // regression cannot pass silently.
    CHECK(comparisons == 8u * 2u * 9u * 9u);
    // Eight of the nine shapes put the destination in ymm0.
    CHECK(combos == 8u * 2u * 8u * 9u);
}

// ---------------------------------------------------------------------------
// A MASKED-OFF LANE PERFORMS NO MEMORY ACCESS.
// ---------------------------------------------------------------------------
// This is the one gather property that cannot be checked by comparing register
// contents, and the one that real code depends on: the tail iteration of a
// vectorized loop routinely leaves garbage in the index lanes it has masked
// off, and hardware never dereferences them.  Verified on hardware first
// (masked-off lanes with indices +2 GiB outside the mapping completed without
// a fault), and asserted here by pointing the masked-off lanes at an address
// with nothing mapped near it: the block must still reach its HLT.
//
// If this case ever regresses, the symptom in a real guest is a spurious
// SIGSEGV in code that is entirely correct.
TEST_CASE("x86 avx2 gather does not access masked-off lanes") {
    const char* avx_env = std::getenv("SVM_AVX");
    if (!avx_env || std::strcmp(avx_env, "0") == 0) {
        SUCCEED("SVM_AVX is not set; gather masked-lane case skipped");
        return;
    }
    constexpr size_t kArenaSize = 0x400000;
    swift::runtime::backend::SmcTracker::SetEnabled(false);
    void* arena = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(arena != MAP_FAILED);
    const u64 base = reinterpret_cast<u64>(arena);
    const u64 stack = base + 0x200000;
    const u64 data = base + 0x300000;

    setenv("SVM_ENABLE_JIT", "1", 1);
    auto* jit_instance = X86Instance::Make();
    setenv("SVM_ENABLE_JIT", "0", 1);
    auto* interp_instance = X86Instance::Make();
    unsetenv("SVM_ENABLE_JIT");
    auto* jit_core = X86Core::Make(jit_instance);
    auto* interp_core = X86Core::Make(interp_instance);

    // vpgatherdd ymm0, [rsi + ymm1*4], ymm2
    const std::vector<u8> enc = {0xC4, 0xE2, 0x6D, 0x90, 0x04, 0x8E};

    for (int all_off = 0; all_off < 2; ++all_off) {
        std::vector<u8> code = enc;
        code.push_back(0xF4);
        const u64 code_addr = base + 0x1000 + u64(all_off) * 0x40;
        std::memcpy(reinterpret_cast<void*>(code_addr), code.data(), code.size());

        // Lane i is enabled only when all_off == 0 and i is even.  The DISABLED
        // lanes carry an index of +0x10000000 elements, i.e. +1 GiB from the
        // base with scale 4 -- far outside anything this process mapped.
        Vec256 index{}, mask{};
        for (u32 i = 0; i < 8; ++i) {
            const bool on = (all_off == 0) && (i % 2 == 0);
            const u32 ix = on ? i : 0x10000000u;
            const u32 mk = on ? 0x80000000u : 0u;
            std::memcpy(index.data() + i * 4, &ix, 4);
            std::memcpy(mask.data() + i * 4, &mk, 4);
        }
        for (u32 i = 0; i < 64; ++i) {
            reinterpret_cast<u8*>(data)[i] = u8(i * 7 + 3);
        }

        for (auto* core : {jit_core, interp_core}) {
            auto& ctx = core->GetContext();
            for (u32 i = 0; i < 16; ++i) {
                const auto p = Poison(i);
                std::memcpy(ctx.xmms[i].b, p.data(), 16);
                std::memcpy(ctx.ymm_high[i].b, p.data() + 16, 16);
            }
            std::memcpy(ctx.xmms[1].b, index.data(), 16);
            std::memcpy(ctx.ymm_high[1].b, index.data() + 16, 16);
            std::memcpy(ctx.xmms[2].b, mask.data(), 16);
            std::memcpy(ctx.ymm_high[2].b, mask.data() + 16, 16);
            ctx.rsi.qword = data;
            ctx.rsp.qword = stack;
            ctx.rip.qword = code_addr;
            const int exit = int(core->Run());
            INFO("all_off=" << all_off
                            << ": a gather touched a lane its mask disabled (or declined the "
                               "encoding); exit="
                            << exit);
            REQUIRE(exit == int(swift::translator::None));
            // The mask register is cleared either way, which is also how a
            // real loop learns the gather ran at all.
            Vec256 after{};
            std::memcpy(after.data(), ctx.xmms[2].b, 16);
            std::memcpy(after.data() + 16, ctx.ymm_high[2].b, 16);
            REQUIRE(std::all_of(after.begin(), after.end(), [](u8 x) { return x == 0; }));
            if (all_off) {
                // Nothing was gathered: the destination keeps its poison.
                Vec256 dst{};
                std::memcpy(dst.data(), ctx.xmms[0].b, 16);
                std::memcpy(dst.data() + 16, ctx.ymm_high[0].b, 16);
                REQUIRE(dst == Poison(0));
            }
        }
    }
    munmap(arena, kArenaSize);
    swift::runtime::backend::SmcTracker::SetEnabled(true);
}

// ---------------------------------------------------------------------------
// #UD SHAPES ARE DECLINED, NOT EXECUTED.
// ---------------------------------------------------------------------------
// The SDM makes a gather #UD when any two of {destination, index, mask} name
// the same register, and when the r/m operand is a register instead of a VSIB
// memory operand.  SwiftVM cannot raise a guest #UD, so the handler declines
// and the block traps as FALLBACK -- fatal, like the #UD would be.  What must
// NOT happen is that one of these encodings quietly executes as if it were
// well-formed, so this case pins the decline.
TEST_CASE("x86 avx2 gather declines #UD operand shapes") {
    const char* avx_env = std::getenv("SVM_AVX");
    if (!avx_env || std::strcmp(avx_env, "0") == 0) {
        SUCCEED("SVM_AVX is not set; gather #UD case skipped");
        return;
    }
    constexpr size_t kArenaSize = 0x400000;
    swift::runtime::backend::SmcTracker::SetEnabled(false);
    void* arena = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(arena != MAP_FAILED);
    const u64 base = reinterpret_cast<u64>(arena);
    const u64 stack = base + 0x200000;
    const u64 data = base + 0x300000;

    setenv("SVM_ENABLE_JIT", "1", 1);
    auto* jit_instance = X86Instance::Make();
    setenv("SVM_ENABLE_JIT", "0", 1);
    auto* interp_instance = X86Instance::Make();
    unsetenv("SVM_ENABLE_JIT");
    auto* jit_core = X86Core::Make(jit_instance);
    auto* interp_core = X86Core::Make(interp_instance);

    struct Bad {
        const char* why;
        std::vector<u8> enc;
    };
    // C4 E2 6D 90 <modrm> <sib>: VEX.256.66.0F38.W0 vpgatherdd.  The vvvv
    // field of byte 2 selects the mask; 0x6D un-inverts to ymm2.
    const std::vector<Bad> bad = {
        // dst == mask (ModRM.reg 2, vvvv 2)
        {"dst == mask", {0xC4, 0xE2, 0x6D, 0x90, 0x14, 0x8E}},
        // dst == index (ModRM.reg 1, SIB index 1)
        {"dst == index", {0xC4, 0xE2, 0x6D, 0x90, 0x0C, 0x8E}},
        // index == mask (SIB index 2, vvvv 2)
        {"index == mask", {0xC4, 0xE2, 0x6D, 0x90, 0x04, 0x96}},
        // mod == 11: a register r/m, which VSIB cannot express
        {"register r/m", {0xC4, 0xE2, 0x6D, 0x90, 0xC1}},
        // no SIB byte: ModRM.rm != 100b
        {"no SIB byte", {0xC4, 0xE2, 0x6D, 0x90, 0x01}},
        // VEX.vvvv == 1111b: no mask operand at all
        {"no vvvv operand", {0xC4, 0xE2, 0x7D, 0x90, 0x04, 0x8E}},
    };
    u64 addr = base + 0x1000;
    for (const auto& b : bad) {
        std::vector<u8> code = b.enc;
        code.push_back(0xF4);
        std::memcpy(reinterpret_cast<void*>(addr), code.data(), code.size());
        for (auto* core : {jit_core, interp_core}) {
            auto& ctx = core->GetContext();
            for (u32 i = 0; i < 16; ++i) {
                const auto p = Poison(i);
                std::memcpy(ctx.xmms[i].b, p.data(), 16);
                std::memcpy(ctx.ymm_high[i].b, p.data() + 16, 16);
            }
            ctx.rsi.qword = data;
            ctx.rsp.qword = stack;
            ctx.rip.qword = addr;
            const int exit = int(core->Run());
            INFO(b.why << ": this encoding is architecturally #UD but the block ran to "
                          "completion, so something executed it as if it were well-formed");
            REQUIRE(exit != int(swift::translator::None));
        }
        addr += 0x40;
    }
    munmap(arena, kArenaSize);
    swift::runtime::backend::SmcTracker::SetEnabled(true);
}

// ---------------------------------------------------------------------------
// VSIB decoding, at the vex_decoder.h level.
// ---------------------------------------------------------------------------
// vex_decoder.cc parses a VSIB ModRM/SIB as an ordinary SIB, which is correct
// for length, base, displacement and scale, and needs exactly ONE
// interpretation at the handler: SIB.index == 100b with VEX.X == 0 means "no
// index register" for a plain SIB and is reported as index_none, but under
// VSIB it names vector register 4.  This case pins both halves of that -- the
// lengths (cross-checked against llvm-objdump when the data was captured) and
// the index_none encoding -- so a future change to the shared decoder cannot
// silently break gather's address computation.
TEST_CASE("x86 avx2 gather VSIB decoding") {
    using namespace swift::x86;
    struct Case {
        const char* text;
        u8 len;
        std::array<u8, 12> bytes;
        u8 n;
        bool base_none;
        u8 base;
        bool index_none;
        u8 index;
        u8 scale;
        s32 disp;
    };
    // Every one of these was produced by the system assembler and confirmed
    // with llvm-objdump to be the mnemonic named.
    const Case cases[] = {
        {"vpgatherdd ymm2,(rax,ymm1,4),ymm0", 6, {0xC4, 0xE2, 0x6D, 0x90, 0x04, 0x88}, 6, false, 0,
         false, 1, 4, 0},
        {"vpgatherdd ymm2,(rbp,ymm1,4),ymm0", 7, {0xC4, 0xE2, 0x6D, 0x90, 0x44, 0x8D, 0x00}, 7,
         false, 5, false, 1, 4, 0},
        {"vpgatherdd ymm2,(r13,ymm1,4),ymm0", 7, {0xC4, 0xC2, 0x6D, 0x90, 0x44, 0x8D, 0x00}, 7,
         false, 13, false, 1, 4, 0},
        {"vpgatherdd ymm2,(r12,ymm1,4),ymm0", 6, {0xC4, 0xC2, 0x6D, 0x90, 0x04, 0x8C}, 6, false,
         12, false, 1, 4, 0},
        {"vpgatherdd ymm2,-0x10(rax,ymm1,4),ymm0", 7, {0xC4, 0xE2, 0x6D, 0x90, 0x44, 0x88, 0xF0},
         7, false, 0, false, 1, 4, -16},
        {"vpgatherdd ymm2,0x12345678(rax,ymm1,4),ymm0", 10,
         {0xC4, 0xE2, 0x6D, 0x90, 0x84, 0x88, 0x78, 0x56, 0x34, 0x12}, 10, false, 0, false, 1, 4,
         0x12345678},
        // No base at all: mod == 00 with SIB.base == 101 is disp32 + index.
        {"vpgatherdd ymm2,0x1000(,ymm1,4),ymm0", 10,
         {0xC4, 0xE2, 0x6D, 0x90, 0x04, 0x8D, 0x00, 0x10, 0x00, 0x00}, 10, true, 0, false, 1, 4,
         0x1000},
        // SIB.index == 100b, VEX.X == 0: index_none for a plain SIB, ymm4 here.
        {"vpgatherdd ymm2,(rax,ymm4,4),ymm0", 6, {0xC4, 0xE2, 0x6D, 0x90, 0x04, 0xA0}, 6, false, 0,
         true, 0, 4, 0},
        {"vpgatherdd ymm2,(rax,ymm12,4),ymm0", 6, {0xC4, 0xA2, 0x6D, 0x90, 0x04, 0xA0}, 6, false,
         0, false, 12, 4, 0},
        {"vpgatherqq ymm2,(rax,ymm1,8),ymm0", 6, {0xC4, 0xE2, 0xED, 0x91, 0x04, 0xC8}, 6, false, 0,
         false, 1, 8, 0},
    };
    for (const auto& c : cases) {
        const auto v = DecodeVexInsn(c.bytes.data(), c.n);
        INFO(c.text);
        REQUIRE(v.valid);
        REQUIRE(v.length == c.len);
        REQUIRE(v.rm_kind == VexRmKind::Memory);
        REQUIRE(v.has_sib);
        REQUIRE_FALSE(v.rip_relative);
        REQUIRE(v.base_none == c.base_none);
        if (!c.base_none) REQUIRE(v.base == c.base);
        REQUIRE(v.index_none == c.index_none);
        if (!c.index_none) REQUIRE(v.index == c.index);
        REQUIRE(v.scale == c.scale);
        REQUIRE(v.displacement == c.disp);
        // The handler's reconstruction: index_none can ONLY arise from
        // index_field == 4 with VEX.X == 0, so it names vector register 4.
        const u32 vsib_index = v.index_none ? 4u : v.index;
        if (c.index_none) REQUIRE(vsib_index == 4u);
    }
}
