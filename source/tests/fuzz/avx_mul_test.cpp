// ===========================================================================
// Widening 32x32 -> 64 multiply family against a ROSETTA oracle.
// ===========================================================================
//
// vpmuludq / vpmuldq at both VEX widths and both VEX.W values, plus the legacy
// SSE pmuludq / pmuldq.  These four are the reason IR opcode VecMulWiden
// exists: nothing in the previous Vec* set computed a 64-bit product, so the
// front end had declined them and a guest that executed one took a FALLBACK
// and then IllegalCode -- fatal, with no interpreter to fall back to.
//
// WHY A HARDWARE ORACLE, AND WHY IT IS NOT THE ONLY ONE
// -----------------------------------------------------
// Rosetta 2 executes AVX including the full 256-bit register file, so
// avx_mul_rosetta_ref.inc holds the literal bytes real x86-64 wrote; nothing in
// it is computed here.  Rosetta is itself an emulator and has been measured
// wrong before (VPSLLVQ's shift count truncated to 32 bits, PTEST's PF varying
// with the surrounding program), so agreement with it is evidence, not proof.
//
// avx_mul_unicorn_check.c replays the same recorded bytes under a completely
// independent emulator.  Its result, in full:
//
//   * the 20 legacy SSE rows agree byte for byte, upper half included, so
//     pmuludq / pmuldq rest on two independent oracles;
//   * the 30 VEX.L=1 rows are refused outright (UC_ERR_INSN_INVALID), the
//     already-known Unicorn limitation;
//   * the 30 VEX.L=0 rows DISAGREE, and it is Unicorn that is wrong: it runs a
//     VEX.128 encoding with legacy SSE semantics, taking src1 from the
//     DESTINATION rather than from VEX.vvvv and leaving bits 255:128 alone.
//     Reduced probes show the same for vpxor and vpaddd, so it is a general
//     VEX.128 defect and not something about this family.
//
// That leaves the VEX rows on ONE emulator, which is not enough.  So the
// property a broken oracle could hide -- WHICH source lanes are multiplied --
// is additionally asserted straight against the Intel SDM below, lane by lane,
// on every recorded row, before any comparison against the implementation runs.
//
// WHY THE ROWS CARRY THE ENCODING
// -------------------------------
// The generator and this test could each build the instruction from the shared
// table, but then a wrong field in the table makes both sides test the same
// wrong instruction and the differential passes vacuously.  Each row carries
// the LITERAL BYTES the generator executed and this file replays them.  All
// sixteen distinct encodings were disassembled and confirmed to be the intended
// mnemonic before the data was captured.
//
// WHAT THE DATA SETTLES, MEASURED RATHER THAN ASSUMED
//
//   Which lanes.  The SDM says DEST[127:64] comes from SRC1[95:64] * SRC2[95:64]
//   -- source dword 2, not dword 1.  AArch64's UMULL widens dwords 0 and 1, so
//   the obvious lowering is wrong in exactly one lane.  Every input pair has
//   a[1]*b[1] != a[2]*b[2], and kLaneCheck below reads the oracle's second
//   result lane and requires it to be a[2]*b[2].
//
//   Signedness.  0x80000000 * 0x80000000 is 0x4000000000000000 under BOTH
//   interpretations, so an "extremes" pair alone cannot see a flipped sign
//   flag.  The `signdisc` pair exists for that: 0xFFFFFFFF * 2 is 0x1FFFFFFFE
//   unsigned and 0xFFFFFFFFFFFFFFFE signed.
//
//   Contract C3, and its INVERSE.  ymm0 is poisoned before every row and all
//   32 bytes are read back, so a VEX.128 row's reference carries sixteen
//   literal zero bytes the HARDWARE wrote.  The legacy SSE rows are the
//   opposite case: their destination is ymm1, whose upper half holds A's upper
//   half, and hardware leaves it ALONE -- an implementation that zeroed it (the
//   VEX rule applied to an SSE encoding) fails those rows.
//
// Each block is a SINGLE instruction: the operand registers are written
// straight into ThreadContext64 and the answer read straight back, so a broken
// vmovdqu cannot mask a broken handler.  Every register except the destination
// and the two sources is poisoned per register and per byte, so a handler that
// writes the wrong register's upper half is caught too.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <sys/mman.h>
#include "runtime/common/svm_config.h"
#include "runtime/backend/smc_tracker.h"
#include "runtime/frontend/x86/decoder.h"
#include "translator/x86/cpu.h"
#include "translator/x86/translator.h"

using namespace swift::translator::x86;
using namespace swift;

namespace {

struct AvxMulInput {
    const char* name;
    const char* a;
    const char* b;
};
// form: which YMM holds the answer -- 0 = ymm0 (the VEX rows, poisoned
// destination), 1 = ymm1 (the legacy SSE rows, whose upper half must survive).
struct AvxMulRef {
    const char* name;
    int width;  // 128 or 256: the VEX.L the generator encoded (128 for SSE)
    int pair;
    int form;
    const char* enc;     // literal instruction bytes, hex
    const char* result;  // 32 bytes read back, hex
};
#include "avx_mul_rosetta_ref.inc"

// The instruction table, shared verbatim with the generator.  Only the NAME is
// consumed here -- the encoding comes from each row -- so this exists to pin
// coverage: every mnemonic named below must have produced reference rows.
struct Entry {
    const char* name;
};
constexpr Entry kEntries[] = {
#define SVM_MUL(name, shape, map, pp, opcode) {#name},
#include "avx_mul_ops.inc"
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

u32 Dword(const Vec256& v, u32 index) {
    u32 out = 0;
    for (u32 i = 0; i < 4; ++i) {
        out |= u32(v[index * 4 + i]) << (i * 8);
    }
    return out;
}

u64 Qword(const Vec256& v, u32 index) {
    u64 out = 0;
    for (u32 i = 0; i < 8; ++i) {
        out |= u64(v[index * 8 + i]) << (i * 8);
    }
    return out;
}

// Must match MUL_POISON in avx_mul_rosetta_ref.c for register 0, which is what
// the generator loaded into ymm0; the other registers extend the same scheme so
// a clobber of the wrong register cannot masquerade as the right one.
Vec256 Poison(u32 reg) {
    Vec256 v{};
    for (u32 j = 0; j < 32; ++j) {
        v[j] = u8(0xA5 ^ (reg * 32 + j));
    }
    return v;
}

// Whether a reference row belongs to `entry`.  Rows are suffixed with the
// sub-form (".m", ".w1"), so this is a prefix match at a component boundary.
bool NameMatches(const char* row, const char* entry) {
    const size_t n = std::strlen(entry);
    return std::strncmp(row, entry, n) == 0 && (row[n] == '\0' || row[n] == '.');
}

bool IsSignedMnemonic(const char* name) {
    return std::strncmp(name, "vpmuldq", 7) == 0 || std::strncmp(name, "pmuldq", 6) == 0;
}

u64 WidenMul(u32 x, u32 y, bool is_signed) {
    return is_signed ? u64(s64(s32(x)) * s64(s32(y))) : u64(x) * u64(y);
}

}  // namespace

TEST_CASE("x86 widening multiply vs rosetta reference") {
    const bool avx_on = swift::runtime::GetSvmConfig().avx;

    std::vector<Vec256> ins_a, ins_b;
    for (const auto& in : kAvxMulInputs) {
        ins_a.push_back(ParseHex32(in.a));
        ins_b.push_back(ParseHex32(in.b));
    }

    // ---- properties of the reference data this case depends on ------------
    // Asserted rather than trusted: without these the differential could still
    // pass while having stopped testing what it exists for.
    {
        // Every mnemonic in the shared table must have produced rows.
        for (const auto& e : kEntries) {
            size_t rows = 0;
            for (const auto& r : kAvxMulRefs) {
                if (NameMatches(r.name, e.name)) ++rows;
            }
            INFO("no reference rows for " << e.name
                                          << " -- the generator did not cover it, or Rosetta "
                                             "refused every encoding of it");
            REQUIRE(rows > 0);
        }
    }
    {
        // The inputs must be able to SEE a wrong lane selection: if
        // a[1]*b[1] == a[2]*b[2] for a pair, then widening dwords 0 and 1
        // (AArch64 UMULL) and widening dwords 0 and 2 (what x86 specifies)
        // produce identical results and that pair proves nothing.
        for (size_t p = 0; p < ins_a.size(); ++p) {
            for (const bool is_signed : {false, true}) {
                INFO(kAvxMulInputs[p].name
                     << ": lanes 1 and 2 give the same product, so this pair cannot "
                        "distinguish UMULL from the even-lane selection");
                REQUIRE(WidenMul(Dword(ins_a[p], 1), Dword(ins_b[p], 1), is_signed) !=
                        WidenMul(Dword(ins_a[p], 2), Dword(ins_b[p], 2), is_signed));
            }
        }
        // A flipped is_signed must be visible.  This is deliberately NOT a
        // per-pair requirement -- `lanes` is all small positives on purpose, so
        // that a failure there reads by eye -- but enough pairs must
        // discriminate that the sign flag is genuinely exercised, and it must
        // be visible in the FIRST 128-bit lane too (dwords 0 and 2), not only
        // in the upper lane that a VEX.128 row never computes.
        size_t sign_pairs = 0, sign_pairs_low_lane = 0;
        for (size_t p = 0; p < ins_a.size(); ++p) {
            bool any = false, low = false;
            for (u32 lane = 0; lane < 8; lane += 2) {
                const bool differs =
                        WidenMul(Dword(ins_a[p], lane), Dword(ins_b[p], lane), false) !=
                        WidenMul(Dword(ins_a[p], lane), Dword(ins_b[p], lane), true);
                any |= differs;
                low |= differs && lane < 4;
            }
            sign_pairs += any ? 1 : 0;
            sign_pairs_low_lane += low ? 1 : 0;
        }
        INFO("too few input pairs distinguish the signed product from the unsigned one");
        REQUIRE(sign_pairs >= 3);
        REQUIRE(sign_pairs_low_lane >= 3);
    }
    {
        // THE ONE PLACE THE ORACLE IS CROSS-READ AGAINST THE SDM.
        // SDM VPMULUDQ/VPMULDQ: DEST[63:0] <- SRC1[31:0] * SRC2[31:0] and
        // DEST[127:64] <- SRC1[95:64] * SRC2[95:64].  Every recorded result is
        // required to match that, lane by lane, so if Rosetta had a lane-
        // selection defect the DATA is rejected rather than a wrong
        // implementation being blessed by it.
        size_t checked = 0;
        for (const auto& r : kAvxMulRefs) {
            const auto got = ParseHex32(r.result);
            const auto& a = ins_a[size_t(r.pair)];
            const auto& b = ins_b[size_t(r.pair)];
            const bool is_signed = IsSignedMnemonic(r.name);
            const u32 lanes = r.width == 256 ? 4u : 2u;
            for (u32 lane = 0; lane < lanes; ++lane) {
                const u64 want = WidenMul(Dword(a, lane * 2), Dword(b, lane * 2), is_signed);
                INFO(r.name << " L" << r.width << " pair " << r.pair << " lane " << lane
                            << ": the recorded product is not SRC1[dword " << lane * 2
                            << "] * SRC2[dword " << lane * 2 << "] as the SDM specifies");
                REQUIRE(Qword(got, lane) == want);
                ++checked;
            }
        }
        INFO("no rows were cross-read against the SDM");
        REQUIRE(checked > 150);
    }
    {
        // Contract C3, as the HARDWARE reported it: every VEX.128 row must
        // carry sixteen zero bytes in its upper half, against a destination
        // that was poisoned beforehand.
        size_t checked = 0;
        for (const auto& r : kAvxMulRefs) {
            if (r.form != 0 || r.width != 128) continue;
            const auto v = ParseHex32(r.result);
            INFO(r.name << " pair " << r.pair
                        << ": the VEX.128 reference does not have a zeroed upper half");
            REQUIRE(std::all_of(v.begin() + 16, v.end(), [](u8 x) { return x == 0; }));
            ++checked;
        }
        INFO("no VEX.128 rows at all");
        REQUIRE(checked > 0);
    }
    {
        // The INVERSE of C3: a legacy SSE write must leave bits 255:128 alone.
        // The destination is ymm1, whose upper half is A's upper half.
        size_t checked = 0;
        for (const auto& r : kAvxMulRefs) {
            if (r.form != 1) continue;
            const auto v = ParseHex32(r.result);
            const auto& a = ins_a[size_t(r.pair)];
            INFO(r.name << " pair " << r.pair
                        << ": the legacy SSE reference did NOT preserve bits 255:128");
            REQUIRE(std::equal(v.begin() + 16, v.end(), a.begin() + 16));
            ++checked;
        }
        INFO("no legacy SSE rows at all -- pmuludq/pmuldq lost their coverage");
        REQUIRE(checked > 0);
    }

    // The SSE rows need no AVX; the VEX rows do.  Running the SSE half even
    // when SVM_AVX is off keeps the legacy handlers covered in the default
    // configuration rather than only in the AVX one.
    if (!avx_on) {
        WARN("SVM_AVX is not set; only the legacy SSE rows of the widening-multiply "
             "differential are executed");
    }

    // ---- harness -----------------------------------------------------------
    constexpr size_t kArenaSize = 0x400000;
    swift::runtime::backend::SmcTracker::SetEnabled(false);
    void* arena = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(arena != MAP_FAILED);
    const u64 base = reinterpret_cast<u64>(arena);
    const u64 stack = base + 0x200000;
    // The generator's data block: A at +0x00, B at +0x20, capture at +0x40,
    // poison at +0x60, addressed through rdi.  Replayed encodings carry those
    // displacements literally, so this layout is not a choice here.
    const u64 data = base + 0x300000;
    constexpr s32 kOffA = 0x00, kOffB = 0x20, kOffOut = 0x40;

    const char* old_jit = swift::runtime::GetRawSvmConfigEnvForTest("SVM_ENABLE_JIT");
    const bool had_old_jit = old_jit != nullptr;
    const std::string old_jit_value = old_jit ? old_jit : "";
    swift::runtime::SetSvmConfigEnvForTest("SVM_ENABLE_JIT", "1", 1);
    auto* jit_instance = X86Instance::Make();
    swift::runtime::SetSvmConfigEnvForTest("SVM_ENABLE_JIT", "0", 1);
    auto* interp_instance = X86Instance::Make();
    if (had_old_jit) {
        swift::runtime::SetSvmConfigEnvForTest("SVM_ENABLE_JIT", old_jit_value.c_str(), 1);
    } else {
        swift::runtime::UnsetSvmConfigEnvForTest("SVM_ENABLE_JIT");
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

    const auto run_on = [&](X86Core* core, const std::vector<u8>& code, const Vec256& a,
                            const Vec256& b, u64 code_addr) {
        std::memcpy(reinterpret_cast<void*>(code_addr), code.data(), code.size());
        std::memcpy(reinterpret_cast<void*>(data + u64(kOffA)), a.data(), 32);
        std::memcpy(reinterpret_cast<void*>(data + u64(kOffB)), b.data(), 32);
        std::memset(reinterpret_cast<void*>(data + u64(kOffOut)), 0xCC, 32);
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
        ctx.rdi.qword = data;
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

    for (const auto& ref : kAvxMulRefs) {
        // form 0 is always a VEX encoding, form 1 always a legacy SSE one.
        if (ref.form == 0 && !avx_on) continue;

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
                fmt::format("{}.L{}/{}", ref.name, ref.width, kAvxMulInputs[ref.pair].name);

        const auto jit = run_on(jit_core, code, a, b, code_addr);
        const auto itp = run_on(interp_core, code, a, b, code_addr);
        ++comparisons;

        if (jit.exit != int(swift::translator::None)) {
            // FALLBACK / ILL_CODE both land here: the encoding was DECLINED
            // rather than mis-executed.  For a guest that is fatal, so it is a
            // failure of this case rather than a gap to note.
            if (bad_exits++ < 15) {
                problems.push_back(fmt::format(
                        "{}: block did not reach HLT (exit={}); encoding {} was not decoded", label,
                        jit.exit, ref.enc));
            }
            continue;
        }
        if (jit.ymm != itp.ymm || jit.exit != itp.exit) {
            if (divergences++ < 15) {
                problems.push_back(
                        fmt::format("{}: JIT/interpreter divergence (answer {} vs {})", label,
                                    Hex(jit.ymm[size_t(ref.form)]), Hex(itp.ymm[size_t(ref.form)])));
            }
        }

        for (const auto& [backend, got] : {std::pair<const char*, const Out*>{"jit", &jit},
                                           std::pair<const char*, const Out*>{"interp", &itp}}) {
            const Vec256& g = got->ymm[size_t(ref.form)];
            if (g != want && mismatches++ < 15) {
                problems.push_back(fmt::format("{} [{}]: got {}, Rosetta says {} (enc {})", label,
                                               backend, Hex(g), Hex(want), ref.enc));
            }
            // No register beyond the destination and the two sources may
            // change -- in particular no bystander's UPPER half may be
            // disturbed by the two-halves split.  ymm0 and ymm1 are excluded:
            // one or the other is the destination of every row.
            for (u32 i = 3; i < 16; ++i) {
                if (got->ymm[i] != Poison(i) && bystanders++ < 15) {
                    problems.push_back(fmt::format("{} [{}]: bystander ymm{} clobbered, {} != {}",
                                                   label, backend, i, Hex(got->ymm[i]),
                                                   Hex(Poison(i))));
                }
            }
            // ymm2 holds B and is never a destination here.
            if (got->ymm[2] != b && bystanders++ < 15) {
                problems.push_back(fmt::format("{} [{}]: source ymm2 was modified, {} != {}", label,
                                               backend, Hex(got->ymm[2]), Hex(b)));
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
    // Pinned so a coverage regression -- rows lost in regeneration, or an
    // opcode dropped from avx_mul_ops.inc -- cannot pass as success.
    CHECK(comparisons == (avx_on ? std::size(kAvxMulRefs) : 20u));
    CHECK(std::size(kAvxMulRefs) == 80u);
}
