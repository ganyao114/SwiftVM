// ===========================================================================
// VEX horizontal / pairwise family against a ROSETTA oracle AND an independent
// SDM model.
// ===========================================================================
//
// Covers vhaddps/pd, vhsubps/pd, vphaddw/d, vphaddsw, vphsubw/d, vphsubsw and
// vpmaddubsw -- every one at both VEX.L values and in both operand shapes,
// which is what source/runtime/frontend/x86/decoder_avx_hadd.cc implements.
//
// WHY A HARDWARE ORACLE
// ---------------------
// Unicorn 2.1.4 refuses every VEX.L=1 encoding (UC_ERR_INSN_INVALID), so the
// 256-bit forms have no emulator oracle at all.  Rosetta 2 executes AVX
// including the full 256-bit register file, so avx_hadd_rosetta_ref.inc holds
// the literal bytes real x86-64 wrote.  Nothing in it is computed here.
//
// WHY ROSETTA ALONE IS NOT ENOUGH, AND WHAT IS DONE ABOUT IT
// ----------------------------------------------------------
// Rosetta is an emulator with its own measured defects -- VPSLLVQ's shift count
// truncated to 32 bits, XSAVE writing bytes past the requested area, vptest's
// PF varying with unrelated details of the surrounding program.  A single
// oracle agreeing with the implementation is therefore not evidence that either
// is right: they can be wrong together.
//
// So this case does not just replay Rosetta.  It re-derives the answer from the
// Intel SDM's per-lane definition, in the ModelOf() below, and asserts THE
// RECORDED BYTES match the model -- before any SwiftVM code runs.  That is a
// check ON THE ORACLE.  It covers all seven integer opcodes exactly (integer
// arithmetic has no rounding to argue about) and the four float opcodes on
// every lane whose operands and result are finite, host `float`/`double`
// arithmetic being IEEE-754 round-to-nearest-even on both sides.  Lanes the
// model declines -- NaN, infinity, and the invalid-operation results -- are
// reported as "unmodelled" and left to the oracle plus the cross-reading below.
//
// WHAT THE SDM DOES NOT SETTLE, AND WHERE THIS FILE FOLLOWS THE MEASUREMENT
// ------------------------------------------------------------------------
// Which element of a horizontal PAIR has NaN priority.  x86 gives operand 1 of
// an add priority, but "operand 1" in the SDM's NaN rules names one of the two
// REGISTER operands, not one of the two elements being summed.  The SDM's own
// pseudocode is not self-consistent on the element order either -- HADDPS is
// written `SRC1[63:32] + SRC1[31:0]` (odd element first) while HADDPD is
// written `SRC1[63:0] + SRC1[127:64]` (even element first) -- so it cannot be
// read as normative here.  Measured with a distinct NaN payload in each element
// of a pair, Rosetta propagates the EVEN element's NaN for hadd and hsub, ps
// and pd alike, and does so even when the odd element is the SIGNALLING one.
// That is also what the obvious hardware implementation produces (two shuffles
// feeding one ADDPS whose first operand is the even stream).  The
// implementation follows the measurement; the f32nan/f64nan rows are what
// would catch a change.
//
// Each block is a SINGLE instruction: the operand registers are written
// straight into ThreadContext64 and the answer read straight back out of it, so
// a broken vmovdqu cannot mask a broken handler.  ymm0 is poisoned before every
// row and all 32 bytes are compared, which is what MEASURES contract C3 (a
// VEX.128 write zeroes bits 255:128) instead of assuming it.  Every register
// except the destination and the two sources is poisoned per register and per
// byte, so a handler that writes the wrong register's upper half is caught too.
//
// WHY THE ROWS CARRY THE ENCODING
// -------------------------------
// The generator and this test could each build the instruction from the shared
// table -- but then a wrong field in the table makes both sides test the same
// wrong instruction and the differential passes vacuously.  Here each row
// carries the LITERAL BYTES the generator executed and this file replays them.
// All 44 distinct encodings were additionally disassembled (clang + otool -tvV)
// and confirmed to be the intended mnemonic, width and operand shape.

#include <algorithm>
#include <array>
#include <cmath>
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

struct AvxHaddInput {
    const char* name;
    const char* a;
    const char* b;
};
struct AvxHaddRef {
    const char* name;
    int width;  // 128 or 256: the VEX.L the generator encoded
    int mem;    // 1 = the r/m operand was [rdi + B], 0 = ymm2
    int pair;
    const char* enc;     // literal instruction bytes, hex
    const char* result;  // ymm0's 32 bytes read back, hex
};
#include "avx_hadd_rosetta_ref.inc"

// The instruction table, shared verbatim with the generator.  The ENCODING is
// never taken from here (each row carries its own); what is used is the name,
// for coverage, and the (lanes, kind) pair, which drives the SDM model.
struct Entry {
    const char* name;
    int lanes;
    int kind;
};
constexpr Entry kEntries[] = {
#define SVM_HADD(name, map, pp, opcode, lanes, kind) {#name, lanes, kind},
#include "avx_hadd_ops.inc"
};

enum : int {
    kFAdd = 0,
    kFSub = 1,
    kIAdd = 2,
    kISub = 3,
    kISatAdd = 4,
    kISatSub = 5,
    kMaddUbs = 6,
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

// Must match HADD_POISON in avx_hadd_rosetta_ref.c for register 0, which is
// what the generator loaded into ymm0; the other registers extend the same
// scheme so a clobber of the wrong register cannot masquerade as the right one.
Vec256 Poison(u32 reg) {
    Vec256 v{};
    for (u32 j = 0; j < 32; ++j) {
        v[j] = u8(0xA5 ^ (reg * 32 + j));
    }
    return v;
}

u64 GetLane(const Vec256& v, size_t byte, u32 bits) {
    u64 x = 0;
    std::memcpy(&x, v.data() + byte, bits / 8);
    return x;
}
void PutLane(Vec256& v, size_t byte, u32 bits, u64 x) { std::memcpy(v.data() + byte, &x, bits / 8); }

s64 SignedLane(u64 raw, u32 bits) {
    const u64 sign = u64(1) << (bits - 1);
    return s64((raw ^ sign) - sign);
}

// ---------------------------------------------------------------------------
// The independent model: the Intel SDM's definition, written out directly.
//
// Every instruction here is defined PER 128-BIT LANE.  Within a lane the
// pairwise results of SRC1 come first and those of SRC2 second -- NOT all of
// SRC1's across the whole register followed by all of SRC2's, which is the
// intuitive-but-wrong reading this model exists to pin down.  vpmaddubsw is the
// exception: it pairs elements ACROSS the two sources rather than within one,
// so it has no such interleave.
//
// `known` marks the bytes the model claims; it is filled PER RESULT ELEMENT so
// that one infinity in a neighbouring lane does not disqualify the rest of the
// row.  Elements whose operands or result are non-finite are left unclaimed --
// those are the ones whose answer turns on NaN propagation, which the SDM does
// not settle for this family (see the file header).  Returns the number of
// claimed elements.
// ---------------------------------------------------------------------------
u32 ModelOf(const Entry& e, int width, const Vec256& a, const Vec256& b, Vec256& out,
            Vec256& known) {
    out.fill(0);
    known.fill(0);
    u32 claimed = 0;
    const auto claim = [&](size_t byte, u32 bits) {
        std::fill(known.begin() + s64(byte), known.begin() + s64(byte) + bits / 8, u8(0xFF));
        ++claimed;
    };
    const u32 w = u32(e.lanes);
    const u32 n = 128 / w;  // elements per 128-bit lane
    const u32 half = n / 2;
    const int lanes = width == 256 ? 2 : 1;

    for (int lane = 0; lane < lanes; ++lane) {
        const size_t base = size_t(lane) * 16;
        if (e.kind == kMaddUbs) {
            for (u32 j = 0; j < 8; ++j) {
                const s32 p0 = s32(u32(a[base + 2 * j])) * s32(s8(b[base + 2 * j]));
                const s32 p1 = s32(u32(a[base + 2 * j + 1])) * s32(s8(b[base + 2 * j + 1]));
                const s32 sum = std::clamp(p0 + p1, -32768, 32767);
                PutLane(out, base + j * 2, 16, u64(u16(sum)));
                claim(base + j * 2, 16);
            }
            continue;
        }
        if (e.kind == kFAdd || e.kind == kFSub) {
            for (u32 j = 0; j < half; ++j) {
                for (int side = 0; side < 2; ++side) {
                    const Vec256& src = side == 0 ? a : b;
                    const size_t so = base + size_t(2 * j) * (w / 8);
                    const size_t dst = base + size_t(side * half + j) * (w / 8);
                    if (w == 32) {
                        float x, y;
                        const u32 xb = u32(GetLane(src, so, 32));
                        const u32 yb = u32(GetLane(src, so + 4, 32));
                        std::memcpy(&x, &xb, 4);
                        std::memcpy(&y, &yb, 4);
                        // EVEN is operand 1 -- the whole point of the ordering.
                        const float r = e.kind == kFAdd ? x + y : x - y;
                        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(r)) {
                            continue;
                        }
                        u32 rb;
                        std::memcpy(&rb, &r, 4);
                        PutLane(out, dst, 32, rb);
                        claim(dst, 32);
                    } else {
                        double x, y;
                        const u64 xb = GetLane(src, so, 64);
                        const u64 yb = GetLane(src, so + 8, 64);
                        std::memcpy(&x, &xb, 8);
                        std::memcpy(&y, &yb, 8);
                        const double r = e.kind == kFAdd ? x + y : x - y;
                        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(r)) {
                            continue;
                        }
                        u64 rb;
                        std::memcpy(&rb, &r, 8);
                        PutLane(out, dst, 64, rb);
                        claim(dst, 64);
                    }
                }
            }
            continue;
        }
        for (u32 j = 0; j < half; ++j) {
            for (int side = 0; side < 2; ++side) {
                const Vec256& src = side == 0 ? a : b;
                const size_t so = base + size_t(2 * j) * (w / 8);
                const size_t dst = base + size_t(side * half + j) * (w / 8);
                const u64 raw_x = GetLane(src, so, w);
                const u64 raw_y = GetLane(src, so + w / 8, w);
                u64 r = 0;
                switch (e.kind) {
                    case kIAdd:
                        r = raw_x + raw_y;  // wraps; there is no 32-bit
                        break;              // saturating horizontal form
                    case kISub:
                        r = raw_x - raw_y;
                        break;
                    case kISatAdd:
                    case kISatSub: {
                        const s64 x = SignedLane(raw_x, w);
                        const s64 y = SignedLane(raw_y, w);
                        const s64 lo = -(s64(1) << (w - 1));
                        const s64 hi = (s64(1) << (w - 1)) - 1;
                        r = u64(std::clamp(e.kind == kISatAdd ? x + y : x - y, lo, hi));
                        break;
                    }
                    default:
                        // Unreachable: kind is one of the four integer ones
                        // here.  Claiming nothing is the fail-safe -- an
                        // unclaimed element is checked by no one rather than
                        // checked against a zero.
                        continue;
                }
                PutLane(out, dst, w, r);
                claim(dst, w);
            }
        }
    }
    return claimed;
}

const Entry* EntryFor(const char* name) {
    for (const auto& e : kEntries) {
        if (std::strcmp(e.name, name) == 0) return &e;
    }
    return nullptr;
}

}  // namespace

TEST_CASE("x86 avx horizontal vs rosetta reference") {
    if (!swift::runtime::GetSvmConfig().avx) {
        SUCCEED("SVM_AVX is not set; VEX horizontal Rosetta differential skipped");
        return;
    }

    std::vector<Vec256> ins_a, ins_b;
    for (const auto& in : kAvxHaddInputs) {
        ins_a.push_back(ParseHex32(in.a));
        ins_b.push_back(ParseHex32(in.b));
    }

    // ---- properties of the reference data, asserted before anything runs ---
    {
        // Every mnemonic in the shared table must have produced rows at BOTH
        // widths and in BOTH operand shapes.  The second wave's fatal bug was
        // exactly an opcode implemented at one width only, so losing a width
        // from the data has to be a failure and not a quiet gap.
        for (const auto& e : kEntries) {
            for (const int width : {128, 256}) {
                for (const int mem : {0, 1}) {
                    size_t rows = 0;
                    for (const auto& r : kAvxHaddRefs) {
                        if (std::strcmp(r.name, e.name) == 0 && r.width == width && r.mem == mem) {
                            ++rows;
                        }
                    }
                    INFO("no reference rows for " << e.name << " at VEX.L=" << width
                                                  << (mem ? " memory" : " register")
                                                  << " -- the generator missed it, or Rosetta "
                                                     "refused every encoding of it");
                    REQUIRE(rows > 0);
                }
            }
        }
    }
    {
        // Contract C3, as the HARDWARE reported it: every VEX.128 row must
        // carry sixteen zero bytes in its upper half, against a ymm0 that was
        // poisoned beforehand.
        size_t checked = 0;
        for (const auto& r : kAvxHaddRefs) {
            if (r.width != 128) continue;
            const auto v = ParseHex32(r.result);
            INFO(r.name << " pair " << r.pair
                        << ": the 128-bit reference does not have a zeroed upper half");
            REQUIRE(std::all_of(v.begin() + 16, v.end(), [](u8 x) { return x == 0; }));
            ++checked;
        }
        INFO("no VEX.128 rows at all");
        REQUIRE(checked > 200);
    }
    {
        // THE ORACLE ITSELF, CHECKED AGAINST THE SDM.  Nothing of SwiftVM is
        // involved here: this compares the bytes Rosetta wrote with the bytes
        // the manual's per-lane definition requires.
        size_t modelled = 0, unmodelled = 0;
        std::vector<std::string> oracle_problems;
        for (const auto& r : kAvxHaddRefs) {
            const Entry* e = EntryFor(r.name);
            REQUIRE(e != nullptr);
            Vec256 model{}, known{};
            const u32 claimed =
                    ModelOf(*e, r.width, ins_a[size_t(r.pair)], ins_b[size_t(r.pair)], model, known);
            const u32 elements = u32(128 / e->lanes) * (r.width == 256 ? 2u : 1u);
            unmodelled += elements - claimed;
            modelled += claimed;
            if (claimed == 0) continue;
            const auto want = ParseHex32(r.result);
            bool differs = false;
            for (u32 i = 0; i < 32; ++i) {
                // A VEX.128 row's upper half is asserted zero above, so only
                // the bytes the model claims are compared here.
                if (known[i] && model[i] != want[i]) differs = true;
            }
            if (differs && oracle_problems.size() < 12) {
                oracle_problems.push_back(fmt::format(
                        "{}.L{}/{}: Rosetta wrote {} but the SDM's per-lane definition gives {} "
                        "(compared bytes {})",
                        r.name, r.width, kAvxHaddInputs[r.pair].name, Hex(want), Hex(model),
                        Hex(known)));
            }
        }
        std::string report;
        for (const auto& p : oracle_problems) {
            report += p;
            report += '\n';
        }
        INFO(report);
        // A disagreement here is an ORACLE defect (or an SDM misreading), not
        // an implementation bug -- so it fails loudly rather than being folded
        // into the differential below.
        REQUIRE(oracle_problems.empty());
        // Pinned so a model that silently stopped claiming elements (a widened
        // bail-out, a renamed mnemonic) cannot pass as agreement.  Counted in
        // RESULT ELEMENTS, not rows: at the time of writing 4380 of 4560 are
        // claimed, the remaining 180 being the float elements whose operands or
        // result are NaN or infinite.
        INFO("modelled elements=" << modelled << " unmodelled=" << unmodelled);
        REQUIRE(modelled > 4000);
        REQUIRE(unmodelled > 0);  // the NaN rows must still be present
    }
    {
        // Pin the anti-intuitive 256-bit layout explicitly, on the input built
        // for it: vhaddps ymm's result is NOT "all of SRC1's sums then all of
        // SRC2's".  ramp32 is A = 2^0..2^7 and B = 2^8..2^15, so the two
        // readings differ in every dword.
        size_t seen = 0;
        for (const auto& r : kAvxHaddRefs) {
            if (std::strcmp(r.name, "vhaddps") != 0 || r.width != 256) continue;
            if (std::strcmp(kAvxHaddInputs[r.pair].name, "ramp32") != 0) continue;
            const auto v = ParseHex32(r.result);
            const float expect[8] = {3.f, 12.f, 768.f, 3072.f, 48.f, 192.f, 12288.f, 49152.f};
            for (u32 i = 0; i < 8; ++i) {
                float got;
                const u32 raw = u32(GetLane(v, i * 4, 32));
                std::memcpy(&got, &raw, 4);
                INFO("vhaddps ymm lane " << i << " is " << got << ", expected " << expect[i]
                                         << " (per-128-bit-lane interleave)");
                REQUIRE(got == expect[i]);
            }
            ++seen;
        }
        REQUIRE(seen > 0);
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
        // ymm1 = A (VEX.vvvv = SRC1), ymm2 = B (r/m = SRC2); ymm0 keeps its
        // poison -- the same state the generator's prologue produced.
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

    for (const auto& ref : kAvxHaddRefs) {
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
                fmt::format("{}.L{}{}/{}", ref.name, ref.width, ref.mem ? ".m" : "",
                            kAvxHaddInputs[ref.pair].name);

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
    // Pinned so a coverage regression -- rows lost in regeneration, or an
    // opcode dropped from avx_hadd_ops.inc -- cannot pass as success.
    CHECK(comparisons == std::size(kAvxHaddRefs));
    CHECK(std::size(kAvxHaddRefs) == 528u);
}
