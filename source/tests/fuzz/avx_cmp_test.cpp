// ===========================================================================
// The floating-point compare-to-mask family against a ROSETTA oracle.
// ===========================================================================
//
// VCMPPS / VCMPPD / VCMPSS / VCMPSD at ALL 32 AVX predicates, at both VEX.L,
// at both r/m shapes -- and the four legacy SSE mnemonics that share
// ir::OpCode::VecFCmpMask with them.
//
// WHY THIS CASE EXISTS
// --------------------
// SwiftVM used to DECLINE imm8 >= 8 on VCMPPS and friends, because
// VecFCmpMask's predicate immediate was the x86 SSE 3-bit encoding and
// accepting 8..31 would have run imm8=13 (GE_OS) as imm8=5 (NLT_US) -- a
// different answer whenever an operand is NaN.  A declined block traps as
// FALLBACK, which kills the guest, so half the AVX compare vocabulary was
// fatal.  Closing that changed what the IR's predicate MEANS: it is now a
// relation set over the four IEEE outcomes {<, ==, >, unordered}, and the x86
// imm8 is translated into it in the front end (runtime/frontend/x86/
// fp_cmp_predicate.h).  Two things therefore need measuring, not asserting:
// that the 24 newly-accepted predicates are right, and that the 8 old ones did
// not move.
//
// WHY NaN IS THE WHOLE TEST
// -------------------------
// Eight of the sixteen relations are indistinguishable from another one unless
// an operand is NaN.  With ordinary floats LT_OS == NGE_US, ORD_Q is all-ones,
// UNORD_Q is all-zeros, NEQ_UQ == NEQ_OQ, EQ_OQ == EQ_UQ; a suite of ordinary
// inputs would run all 32 predicates and separate only eight.  So this file
// does not merely USE NaN inputs -- it ASSERTS, from the reference data
// itself, that for every mnemonic and every VEX.L the 16 relations produce 16
// DISTINCT answers.  If the input pairs ever lost their NaN coverage the
// assertion fails rather than the suite quietly weakening.
//
// WHAT THE ORACLE SETTLED THAT NOTHING ELSE COULD
// -----------------------------------------------
// AVX's 32 predicates are 16 relations x {signalling, quiet}.  Whether the
// signalling variants differ in the RESULT (rather than only in MXCSR) decides
// whether the IR needs a third semantic dimension.  Measured: imm8 `i` and
// `i + 16` gave BIT-IDENTICAL results in all 2112 comparable rows here, while
// a separate probe with a QNaN operand and nothing else showed MXCSR.IE
// following the SDM's _OS/_OQ classification exactly (predicates 1,2,5,6,9,10,
// 13,14 set IE; 0,3,4,7,8,11,12,15 did not; 16..31 inverted that).  So the
// distinction is exception-only, SwiftVM models no FP exception state, and the
// dimension is dropped at the front end.  The first half of that measurement
// is re-asserted below on every run, so the assumption cannot rot silently.
//
// Rosetta is itself an emulator and has been measured wrong before (VPSLLVQ's
// shift count truncated to 32 bits; vptest's PF non-deterministic), so
// agreement with it is evidence and not proof.  Every result shape here was
// cross-read against the Intel SDM's Table 3-1 before the data was accepted,
// and the relation-set model was checked against the SDM predicate by
// predicate.  Nothing in the .inc is edited by hand, ever.
//
// Each block is a single instruction (the legacy rows add the `movaps xmm0,
// xmm1` that CMPPS's destructive form needs) run against a POISONED ymm0 whose
// whole 32 bytes are read back.  That measures two OPPOSITE contracts from one
// reference: a VEX.128 write zeroes bits 255:128, and a legacy SSE write must
// leave them alone.

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
#include "runtime/common/svm_config.h"
#include "runtime/backend/smc_tracker.h"
#include "runtime/frontend/x86/decoder.h"
#include "translator/x86/cpu.h"
#include "translator/x86/translator.h"

using namespace swift::translator::x86;
using namespace swift;

namespace {

struct AvxCmpInput {
    const char* name;
    const char* a;
    const char* b;
};
struct AvxCmpRef {
    const char* name;
    int width;  // 128 or 256: the VEX.L the generator encoded
    int mem;    // 0 = register r/m, 1 = memory r/m
    int pair;
    int imm;             // the x86 imm8 predicate
    const char* enc;     // literal instruction bytes, hex
    const char* result;  // ymm0 afterwards, 32 bytes, hex
};
#include "avx_cmp_rosetta_ref.inc"

// The instruction table, shared verbatim with the generator.  Only the name
// and the predicate count are consumed here -- the encoding comes from each
// row -- so this exists to pin coverage.
struct Entry {
    const char* name;
    int vex;
    int pp;
    int scalar;
    int l256;
};
constexpr Entry kEntries[] = {
#define SVM_CMP(name, vex, pp, scalar, l256) {#name, vex, pp, scalar, l256},
#include "avx_cmp_ops.inc"
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

// Must match CMP_POISON in avx_cmp_rosetta_ref.c for register 0, which is what
// the generator loaded into ymm0; the other registers extend the same scheme so
// a clobber of the wrong register cannot masquerade as the right one.
Vec256 Poison(u32 reg) {
    Vec256 v{};
    for (u32 j = 0; j < 32; ++j) {
        v[j] = u8(0xA5 ^ (reg * 32 + j));
    }
    return v;
}

}  // namespace

TEST_CASE("x86 fp compare predicates vs rosetta reference") {
    if (!swift::runtime::GetSvmConfig().avx) {
        SUCCEED("SVM_AVX is not set; FP compare Rosetta differential skipped");
        return;
    }

    std::vector<Vec256> ins_a, ins_b;
    for (const auto& in : kAvxCmpInputs) {
        ins_a.push_back(ParseHex32(in.a));
        ins_b.push_back(ParseHex32(in.b));
    }

    // ---- properties of the reference data this case depends on ------------
    // Asserted rather than trusted: without these the differential could still
    // pass while having stopped testing what it exists for.
    {
        // Every mnemonic in the shared table must have produced rows, and must
        // have produced them for EVERY predicate it can encode -- 32 for the
        // VEX forms, 8 for the legacy SSE ones.
        for (const auto& e : kEntries) {
            std::set<int> imms;
            for (const auto& r : kAvxCmpRefs) {
                if (std::strcmp(r.name, e.name) == 0) imms.insert(r.imm);
            }
            INFO("wrong predicate coverage for " << e.name);
            REQUIRE(imms.size() == size_t(e.vex ? 32 : 8));
        }
    }
    {
        // THE MEASUREMENT THE IMPLEMENTATION RESTS ON.
        // imm8 bit 4 selects signalling vs quiet, and SwiftVM drops it because
        // it changes only MXCSR.  If that is ever false on some oracle, this
        // fails here -- on the DATA -- rather than turning into a mysterious
        // mismatch downstream.
        size_t pairs = 0;
        for (const auto& r : kAvxCmpRefs) {
            if (r.imm >= 16) continue;
            const AvxCmpRef* twin = nullptr;
            for (const auto& s : kAvxCmpRefs) {
                if (std::strcmp(s.name, r.name) == 0 && s.width == r.width && s.mem == r.mem &&
                    s.pair == r.pair && s.imm == r.imm + 16) {
                    twin = &s;
                    break;
                }
            }
            if (twin == nullptr) continue;  // legacy SSE rows have no twin
            INFO(r.name << " pair " << r.pair << ": imm8 " << r.imm << " and " << r.imm + 16
                        << " disagree on hardware, so the signalling bit is NOT result-neutral "
                           "and dropping it in fp_cmp_predicate.h is wrong");
            REQUIRE(std::strcmp(twin->result, r.result) == 0);
            ++pairs;
        }
        INFO("no imm/imm+16 pairs were compared at all");
        REQUIRE(pairs == 2112u);
    }
    {
        // THE NaN COVERAGE, MEASURED.  For each mnemonic and each VEX.L, the
        // sixteen relations must give sixteen DISTINCT answers once the answers
        // for all input pairs are laid side by side.  Eight of them collapse
        // onto another one the moment NaN leaves the inputs, so this failing
        // means the pairs stopped testing what the file exists for.
        for (const auto& e : kEntries) {
            if (!e.vex) continue;
            for (const int width : {128, 256}) {
                if (width == 256 && !e.l256) continue;
                std::set<std::string> signatures;
                for (int imm = 0; imm < 16; ++imm) {
                    std::map<std::pair<int, int>, std::string> by_pair;
                    for (const auto& r : kAvxCmpRefs) {
                        if (std::strcmp(r.name, e.name) != 0 || r.width != width || r.imm != imm) {
                            continue;
                        }
                        by_pair[{r.pair, r.mem}] = r.result;
                    }
                    std::string joined;
                    for (const auto& [key, value] : by_pair) {
                        joined += value;
                    }
                    REQUIRE(!joined.empty());
                    signatures.insert(joined);
                }
                INFO(e.name << " L" << width << ": only " << signatures.size()
                            << " of the 16 relations are distinguishable in the reference data -- "
                               "the input pairs no longer cover all four IEEE outcomes");
                REQUIRE(signatures.size() == 16u);
            }
        }
    }
    {
        // Two OPPOSITE contracts from the same poisoned destination: a VEX.128
        // write zeroes bits 255:128, a legacy SSE write must not touch them.
        const auto poison = Poison(0);
        size_t vex128 = 0, sse = 0;
        for (const auto& r : kAvxCmpRefs) {
            const auto v = ParseHex32(r.result);
            const bool is_vex = r.name[0] == 'v';
            if (is_vex && r.width == 128) {
                INFO(r.name << " pair " << r.pair << " imm " << r.imm
                            << ": the VEX.128 reference does not have a zeroed upper half");
                REQUIRE(std::all_of(v.begin() + 16, v.end(), [](u8 x) { return x == 0; }));
                ++vex128;
            } else if (!is_vex) {
                INFO(r.name << " pair " << r.pair << " imm " << r.imm
                            << ": the legacy SSE reference did not PRESERVE bits 255:128");
                REQUIRE(std::equal(v.begin() + 16, v.end(), poison.begin() + 16));
                ++sse;
            }
        }
        REQUIRE(vex128 > 1000);
        REQUIRE(sse > 500);
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
    constexpr s32 kOffA = 0x00, kOffB = 0x20;

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

    for (const auto& ref : kAvxCmpRefs) {
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
                fmt::format("{}.L{}{}/imm{}/{}", ref.name, ref.width, ref.mem ? ".m" : ".r",
                            ref.imm, kAvxCmpInputs[ref.pair].name);

        const auto jit = run_on(jit_core, code, a, b, code_addr);
        const auto itp = run_on(interp_core, code, a, b, code_addr);
        ++comparisons;

        if (jit.exit != int(swift::translator::None)) {
            // FALLBACK / ILL_CODE both land here: the encoding was DECLINED
            // rather than mis-executed.  For a guest that is fatal -- it is
            // exactly what predicates 8..31 used to do -- so it is a failure of
            // this case rather than a gap to note.
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
                        fmt::format("{}: JIT/interpreter divergence (ymm0 {} vs {})", label,
                                    Hex(jit.ymm[0]), Hex(itp.ymm[0])));
            }
        }

        for (const auto& [backend, got] : {std::pair<const char*, const Out*>{"jit", &jit},
                                           std::pair<const char*, const Out*>{"interp", &itp}}) {
            if (got->ymm[0] != want && mismatches++ < 15) {
                problems.push_back(fmt::format("{} [{}]: got {}, Rosetta says {} (enc {})", label,
                                               backend, Hex(got->ymm[0]), Hex(want), ref.enc));
            }
            // No register beyond the destination and the two sources may
            // change -- in particular no bystander's UPPER half may be
            // disturbed by the two-halves split of a 256-bit form.
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
    // Pinned so a coverage regression -- rows lost in regeneration, or an
    // opcode dropped from avx_cmp_ops.inc -- cannot pass as success.
    CHECK(comparisons == std::size(kAvxCmpRefs));
    CHECK(std::size(kAvxCmpRefs) == 4928u);
}
