// ===========================================================================
// vround / vdpps / vdppd / vpermilpd-var against a ROSETTA oracle.
// ===========================================================================
//
// Covers everything decoder_avx_misc.cc claims: VROUNDPS/PD/SS/SD at all
// sixteen imm8 values and all four MXCSR rounding modes, VDPPS/VDPPD over
// sixteen imm8 lane masks, and the VARIABLE form of VPERMILPD -- each at both
// VEX widths (except where the width does not exist) and with both a register
// and a memory r/m operand.
//
// WHY A HARDWARE ORACLE
// ---------------------
// Unicorn 2.1.4 refuses every VEX.L=1 encoding (UC_ERR_INSN_INVALID), so the
// 256-bit forms have no emulator oracle at all.  For this family even the
// 128-bit forms need one: what is being tested is a set of CONVENTIONS -- which
// of two rounding rules applies at an exact tie, which imm8 nibble selects the
// multiply and which the destination, which BIT of a control element is the
// selector -- and a hand-written model of a convention just re-states the
// implementation's own assumption.  Rosetta 2 executes AVX including the full
// 256-bit register file, so avx_misc_rosetta_ref.inc holds the literal bytes
// real x86-64 wrote.  Nothing in it is computed here.
//
// Rosetta is itself an emulator and has been measured wrong before, so
// agreement with it is evidence and not proof; every result shape asserted
// below was cross-read against the Intel SDM before the data was accepted.
//
// WHAT THE DATA SETTLES, MEASURED RATHER THAN ASSUMED
//
//   ROUND-HALF-EVEN, NOT ROUND-HALF-AWAY.  The `mid32` / `mid64` pairs are
//   nothing but exact ties.  Hardware returned 0, 2, 2, 4 for 0.5, 1.5, 2.5,
//   3.5 under imm8 = 0; an implementation built on std::round (half-away)
//   returns 1, 2, 3, 4 and passes every non-tie input in the file.  Asserted
//   directly on the reference data so the distinction cannot be lost in a
//   regeneration.
//
//   IMM8 BIT 3 IS A NO-OP.  Bit 3 suppresses the precision exception, which
//   changes no result bit.  Every imm8 = N row is required to match its
//   imm8 = N|8 twin IN THE REFERENCE DATA, so the 8..15 half of the sweep is
//   pinned as a real duplicate rather than assumed to be one.
//
//   IMM8 BIT 2 REALLY READS MXCSR.  The .mx rows wrap the instruction in
//   `ldmxcsr` / instruction / `ldmxcsr` (all inside the replayed bytes), and
//   the test requires that the four RC values do NOT all produce the same
//   answer.  Without that requirement an implementation that ignored MXCSR
//   entirely would pass every row, because RC = 0 is the default.
//
//   VDPPS MASKS THE PRODUCT, NOT THE OPERANDS.  The `dotnan` pair puts
//   inf * 0 in lane 0.  With imm8 = 0x0F (no lane selected for the multiply)
//   hardware returned +0.0 in all four lanes; with imm8 = 0xF1 it returned the
//   x86 indefinite FFC00000.  Zeroing the OPERANDS and multiplying would give
//   a NaN in both cases, so these two rows separate the two implementations.
//
//   VPERMILPD READS BIT 1 OF THE CONTROL, NOT BIT 0 AND NOT THE ELEMENT.  The
//   `permctrl` pair sets bit 0, the sign bit and the low dword to values that
//   would each produce a different permutation if the wrong field were read.
//
//   CONTRACT C3.  ymm0 is poisoned with 0xA5^index before every row and all 32
//   bytes are read back, so a VEX.128 row's reference carries sixteen literal
//   zero bytes THE HARDWARE wrote.
//
// Each block is a single instruction (plus, for the MXCSR rows, the two
// ldmxcsr that bracket it): the operands are written straight into
// ThreadContext64 and the answer read straight back, so a broken vmovdqu
// cannot mask a broken handler.  Every register except the destination and the
// two sources is poisoned per register and per byte, so a handler that writes
// the wrong register's upper half is caught too.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <sys/mman.h>
#include "runtime/backend/smc_tracker.h"
#include "runtime/frontend/x86/decoder.h"
#include "translator/x86/cpu.h"
#include "translator/x86/translator.h"

using namespace swift::translator::x86;
using namespace swift;

namespace {

struct AvxMiscInput {
    const char* name;
    const char* a;
    const char* b;
};
struct AvxMiscRef {
    const char* name;
    int width;  // 128 or 256: the VEX.L the generator encoded
    int pair;
    int imm;         // the imm8 the generator encoded (0 where there is none)
    int rc;          // MXCSR.RC the recorded ldmxcsr selected, or -1 for none
    const char* enc;     // literal instruction bytes, hex
    const char* result;  // the 32 bytes of ymm0 read back, hex
};
#include "avx_misc_rosetta_ref.inc"

// The mnemonic table, shared verbatim with the generator.  Consumed only to
// pin coverage: every name below must have produced reference rows.
constexpr const char* kEntries[] = {
#define SVM_MISC(name) name,
#include "avx_misc_ops.inc"
};

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

u32 Dword(const Vec256& v, u32 i) {
    return u32(v[i * 4]) | (u32(v[i * 4 + 1]) << 8) | (u32(v[i * 4 + 2]) << 16) |
           (u32(v[i * 4 + 3]) << 24);
}

u64 Qword(const Vec256& v, u32 i) {
    u64 r = 0;
    for (u32 j = 0; j < 8; ++j) {
        r |= u64(v[i * 8 + j]) << (j * 8);
    }
    return r;
}

// Must match MISC_POISON in avx_misc_rosetta_ref.c for register 0, which is
// what the generator loaded into ymm0; the other registers extend the same
// scheme so a clobber of the wrong register cannot masquerade as the right one.
Vec256 Poison(u32 reg) {
    Vec256 v{};
    for (u32 j = 0; j < 32; ++j) {
        v[j] = u8(0xA5 ^ (reg * 32 + j));
    }
    return v;
}

int PairIndex(const char* name) {
    for (int i = 0; i < int(std::size(kAvxMiscInputs)); ++i) {
        if (std::strcmp(kAvxMiscInputs[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

// The first reference row matching a key, or nullptr.
const AvxMiscRef* FindRow(const char* name, int width, int pair, int imm, int rc) {
    for (const auto& r : kAvxMiscRefs) {
        if (std::strcmp(r.name, name) == 0 && r.width == width && r.pair == pair &&
            r.imm == imm && r.rc == rc) {
            return &r;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("x86 avx misc vs rosetta reference") {
    const char* avx_env = std::getenv("SVM_AVX");
    if (!avx_env || std::strcmp(avx_env, "0") == 0) {
        SUCCEED("SVM_AVX is not set; vround/vdpps Rosetta differential skipped");
        return;
    }

    std::vector<Vec256> ins_a, ins_b;
    for (const auto& in : kAvxMiscInputs) {
        ins_a.push_back(ParseHex32(in.a));
        ins_b.push_back(ParseHex32(in.b));
    }

    // ---- properties of the reference data this case depends on ------------
    // Asserted rather than trusted: without these the differential could still
    // pass while having stopped testing what it exists for.
    {
        for (const char* name : kEntries) {
            size_t rows = 0;
            for (const auto& r : kAvxMiscRefs) {
                if (std::strcmp(r.name, name) == 0) ++rows;
            }
            INFO("no reference rows for " << name
                                          << " -- the generator did not cover it, or Rosetta "
                                             "refused every encoding of it");
            REQUIRE(rows > 0);
        }
    }
    {
        // Contract C3, as the HARDWARE reported it.
        size_t checked = 0;
        for (const auto& r : kAvxMiscRefs) {
            if (r.width != 128) continue;
            const auto v = ParseHex32(r.result);
            INFO(r.name << " imm " << r.imm << " pair " << r.pair
                        << ": the 128-bit reference does not have a zeroed upper half");
            REQUIRE(std::all_of(v.begin() + 16, v.end(), [](u8 x) { return x == 0; }));
            ++checked;
        }
        INFO("no VEX.128 rows at all");
        REQUIRE(checked > 1000);
    }
    {
        // ROUND-HALF-EVEN.  mid32 lane 0..3 is 0.5, 1.5, 2.5, 3.5; under
        // imm8 = 0 the SDM's "round to nearest (even)" gives 0, 2, 2, 4.
        // Round-half-away would give 1, 2, 3, 4 and this is the only place in
        // the file the two differ.
        const int mid = PairIndex("mid32");
        REQUIRE(mid >= 0);
        const auto* row = FindRow("vroundps", 128, mid, 0, -1);
        REQUIRE(row != nullptr);
        const auto v = ParseHex32(row->result);
        INFO("the mid32 reference no longer distinguishes half-even from half-away");
        CHECK(Dword(v, 0) == 0x00000000u);  // 0.5 -> 0.0
        CHECK(Dword(v, 1) == 0x40000000u);  // 1.5 -> 2.0
        CHECK(Dword(v, 2) == 0x40000000u);  // 2.5 -> 2.0, NOT 3.0
        CHECK(Dword(v, 3) == 0x40800000u);  // 3.5 -> 4.0
    }
    {
        // imm8 bit 3 changes no result bit.  Every N row must equal its N|8
        // twin in the reference data itself.
        size_t pairs_checked = 0;
        for (const auto& r : kAvxMiscRefs) {
            if (r.imm >= 8 || r.rc >= 0) continue;
            const auto* twin = FindRow(r.name, r.width, r.pair, r.imm | 8, r.rc);
            if (twin == nullptr) continue;  // vdpps' imm8 is a lane mask, not a mode
            if (std::strncmp(r.name, "vround", 6) != 0) continue;
            INFO(r.name << " imm " << r.imm
                        << ": the precision-suppressing twin gives a different result");
            REQUIRE(std::strcmp(r.result, twin->result) == 0);
            ++pairs_checked;
        }
        INFO("no vround imm8 bit-3 twin pairs in the data");
        REQUIRE(pairs_checked > 100);
    }
    {
        // imm8 bit 2 REALLY reads MXCSR.RC: the four RC values must disagree
        // somewhere.  If they never did, an implementation that ignored MXCSR
        // would pass every .mx row.
        size_t discriminating = 0;
        std::set<std::tuple<std::string, int, int, int>> keys;
        for (const auto& r : kAvxMiscRefs) {
            if (r.rc < 0) continue;
            keys.insert({r.name, r.width, r.pair, r.imm});
        }
        for (const auto& [name, width, pair, imm] : keys) {
            std::set<std::string> answers;
            for (int rc = 0; rc < 4; ++rc) {
                const auto* row = FindRow(name.c_str(), width, pair, imm, rc);
                if (row != nullptr) answers.insert(row->result);
            }
            if (answers.size() > 1) ++discriminating;
        }
        INFO("no .mx group has RC-dependent results -- the ldmxcsr rows measure nothing, "
             "which is exactly the defect that made the first cut of this file vacuous "
             "(disp8 0x80 is -128, so MXCSR was loaded from before the data block)");
        REQUIRE(discriminating > 20);
    }
    {
        // VDPPS masks the PRODUCT.  dotnan lane 0 is inf * 0.
        const int dn = PairIndex("dotnan");
        REQUIRE(dn >= 0);
        const auto* off = FindRow("vdpps", 128, dn, 0x0F, -1);
        const auto* on = FindRow("vdpps", 128, dn, 0xF1, -1);
        REQUIRE(off != nullptr);
        REQUIRE(on != nullptr);
        const auto voff = ParseHex32(off->result);
        const auto von = ParseHex32(on->result);
        INFO("a de-selected inf*0 lane is not +0.0 in the reference -- the row that "
             "separates masking the product from masking the operands is gone");
        for (u32 i = 0; i < 4; ++i) {
            CHECK(Dword(voff, i) == 0u);
        }
        INFO("a SELECTED inf*0 lane is not the x86 indefinite in the reference");
        CHECK(Dword(von, 0) == 0xFFC00000u);
    }
    {
        // VPERMILPD's selector is bit 1: permctrl's control qword 1 is exactly
        // 2, which must move A's SECOND qword into the result's second qword,
        // while control qword 0 (bit 1 clear, every other bit set) must leave
        // A's first qword in place.  Reading bit 0, or the whole element, or
        // only the low byte gives a different answer for at least one of them.
        const int pc = PairIndex("permctrl");
        REQUIRE(pc >= 0);
        const auto* row = FindRow("vpermilpd", 256, pc, 0, -1);
        REQUIRE(row != nullptr);
        const auto v = ParseHex32(row->result);
        const auto a = ins_a[size_t(pc)];
        INFO("vpermilpd's reference no longer discriminates which control bit is read");
        CHECK(Qword(v, 0) == Qword(a, 0));
        CHECK(Qword(v, 1) == Qword(a, 1));
        // Upper 128-bit lane: control 0x8000000000000003 has bit 1 SET (so
        // qword 3 of A) and control 0x00000000FFFFFFFD has it CLEAR (qword 2).
        // A lane-crossing implementation would take qwords 0/1 here.
        CHECK(Qword(v, 2) == Qword(a, 3));
        CHECK(Qword(v, 3) == Qword(a, 2));
    }

    // ---- harness -----------------------------------------------------------
    constexpr size_t kArenaSize = 0x400000;
    swift::runtime::backend::SmcTracker::SetEnabled(false);
    void* arena = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(arena != MAP_FAILED);
    const u64 base = reinterpret_cast<u64>(arena);
    const u64 stack = base + 0x200000;
    // The generator's data block: A at +0x00, B at +0x20, capture at +0x40,
    // poison at +0x60, the four MXCSR words at +0x80.  Replayed encodings carry
    // those displacements literally, so this layout is not a choice here.
    const u64 data = base + 0x300000;
    constexpr s32 kOffA = 0x00, kOffB = 0x20, kOffOut = 0x40, kOffMxcsr = 0x80;

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
        u32 mxcsr{};
        int exit{};
    };

    size_t code_cursor = 1;
    std::vector<std::string> problems;
    size_t comparisons = 0, bad_exits = 0, divergences = 0, mismatches = 0, bystanders = 0,
           leaked_mxcsr = 0;

    const auto run_on = [&](X86Core* core, const std::vector<u8>& code, const Vec256& a,
                            const Vec256& b, u64 code_addr) {
        std::memcpy(reinterpret_cast<void*>(code_addr), code.data(), code.size());
        std::memcpy(reinterpret_cast<void*>(data + u64(kOffA)), a.data(), 32);
        std::memcpy(reinterpret_cast<void*>(data + u64(kOffB)), b.data(), 32);
        std::memset(reinterpret_cast<void*>(data + u64(kOffOut)), 0xCC, 32);
        for (u32 rc = 0; rc < 4; ++rc) {
            const u32 word = 0x1F80u | (rc << 13);
            std::memcpy(reinterpret_cast<void*>(data + u64(kOffMxcsr) + rc * 4), &word, 4);
        }
        auto& ctx = core->GetContext();
        for (u32 i = 0; i < 16; ++i) {
            const auto p = Poison(i);
            std::memcpy(ctx.xmms[i].b, p.data(), 16);
            std::memcpy(ctx.ymm_high[i].b, p.data() + 16, 16);
        }
        // ymm1 = A, ymm2 = B; ymm0 keeps its poison -- the same state the
        // generator's prologue produced.
        std::memcpy(ctx.xmms[1].b, a.data(), 16);
        std::memcpy(ctx.ymm_high[1].b, a.data() + 16, 16);
        std::memcpy(ctx.xmms[2].b, b.data(), 16);
        std::memcpy(ctx.ymm_high[2].b, b.data() + 16, 16);
        // Every row starts from the architectural default, so a row that fails
        // to restore RC cannot silently retune the next one.
        ctx.mxcsr = 0x1F80u;
        ctx.rdi.qword = data;
        ctx.rsp.qword = stack;
        ctx.rip.qword = code_addr;
        Out o;
        o.exit = int(core->Run());
        for (u32 i = 0; i < 16; ++i) {
            std::memcpy(o.ymm[i].data(), ctx.xmms[i].b, 16);
            std::memcpy(o.ymm[i].data() + 16, ctx.ymm_high[i].b, 16);
        }
        o.mxcsr = ctx.mxcsr;
        return o;
    };

    for (const auto& ref : kAvxMiscRefs) {
        auto code = ParseHex(ref.enc);
        code.push_back(0xF4);  // hlt
        const u64 code_addr = base + 0x1000 + code_cursor * 0x100;
        ++code_cursor;
        REQUIRE(code.size() < 0x100);
        REQUIRE(code_addr + 0x100 < stack);
        const Vec256 want = ParseHex32(ref.result);
        const auto& a = ins_a[size_t(ref.pair)];
        const auto& b = ins_b[size_t(ref.pair)];
        const std::string label =
                fmt::format("{}.L{}/{}/imm{:02x}/rc{}", ref.name, ref.width,
                            kAvxMiscInputs[ref.pair].name, ref.imm, ref.rc);

        const auto jit = run_on(jit_core, code, a, b, code_addr);
        const auto itp = run_on(interp_core, code, a, b, code_addr);
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
        if (jit.ymm != itp.ymm || jit.exit != itp.exit || jit.mxcsr != itp.mxcsr) {
            if (divergences++ < 15) {
                problems.push_back(fmt::format(
                        "{}: JIT/interpreter divergence (ymm0 {} vs {}, mxcsr {:#x} vs {:#x})",
                        label, Hex(jit.ymm[0]), Hex(itp.ymm[0]), jit.mxcsr, itp.mxcsr));
            }
        }

        for (const auto& [backend, got] : {std::pair<const char*, const Out*>{"jit", &jit},
                                           std::pair<const char*, const Out*>{"interp", &itp}}) {
            if (got->ymm[0] != want && mismatches++ < 15) {
                problems.push_back(fmt::format("{} [{}]: got {}, Rosetta says {} (enc {})", label,
                                               backend, Hex(got->ymm[0]), Hex(want), ref.enc));
            }
            // The .mx rows restore RC themselves; a handler that consumed the
            // ldmxcsr wrongly would leave it changed and poison every later row.
            if (got->mxcsr != 0x1F80u && leaked_mxcsr++ < 15) {
                problems.push_back(fmt::format("{} [{}]: MXCSR left at {:#x}, expected 0x1f80",
                                               label, backend, got->mxcsr));
            }
            // No register beyond the destination and the two sources may
            // change -- in particular no bystander's UPPER half may be
            // disturbed by the two-halves split.
            for (u32 i = 3; i < 16; ++i) {
                if (got->ymm[i] != Poison(i) && bystanders++ < 15) {
                    problems.push_back(fmt::format("{} [{}]: bystander ymm{} clobbered, {} != {}",
                                                   label, backend, i, Hex(got->ymm[i]),
                                                   Hex(Poison(i))));
                }
            }
        }
    }

    X86Core::Destroy(jit_core);
    X86Core::Destroy(interp_core);
    X86Instance::Destroy(jit_instance);
    X86Instance::Destroy(interp_instance);
    swift::runtime::backend::SmcTracker::SetEnabled(true);
    munmap(arena, kArenaSize);

    // One joined INFO rather than a loop of UNSCOPED_INFO: Catch2 clears
    // unscoped messages at the next assertion, so a loop before several CHECKs
    // attaches every message to the FIRST one and the later failures print
    // nothing.
    std::string report;
    for (size_t i = 0; i < problems.size() && i < 40; ++i) {
        report += problems[i];
        report += '\n';
    }
    INFO(report);
    CHECK(bad_exits == 0);
    CHECK(divergences == 0);
    CHECK(mismatches == 0);
    CHECK(bystanders == 0);
    CHECK(leaked_mxcsr == 0);
    // Pinned so a coverage regression -- rows lost in regeneration, or a
    // mnemonic dropped from avx_misc_ops.inc -- cannot pass as success.
    CHECK(comparisons == std::size(kAvxMiscRefs));
    CHECK(std::size(kAvxMiscRefs) == 3720u);
}
