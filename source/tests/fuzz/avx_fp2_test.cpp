// ===========================================================================
// Second-wave VEX family against a ROSETTA oracle.
// ===========================================================================
//
// Covers what avx_fp_test.cpp does not: the scalar moves (vmovss / vmovsd),
// the scalar conversions, the shuffles and unpacks, the 64-bit lane moves,
// vpsrldq / vpslldq, vptest, the element insert/extract family, and the six
// VEX.128 twins whose 256-bit forms already worked while their 128-bit forms
// killed the guest.
//
// WHY A HARDWARE ORACLE
// ---------------------
// Unicorn 2.1.4 refuses every VEX.L=1 encoding (UC_ERR_INSN_INVALID), so the
// 256-bit forms have no emulator oracle.  For this family even the 128-bit
// forms need one, because what goes wrong is not arithmetic but PLUMBING --
// which register a merge takes its untouched lanes from, which half of a
// source a lane move reads, whether an imm8's bit fields were split at the
// right place -- and a hand-written model of that just re-states the
// implementation's own assumption.  Rosetta 2 on macOS 26/27 executes AVX
// including the full 256-bit register file, so avx_fp2_rosetta_ref.inc holds
// the literal bytes real x86-64 wrote.  Nothing in it is computed here.
//
// Rosetta is itself an emulator and has been measured wrong before (VPSLLVQ's
// shift count truncated to 32 bits, among others), so agreement with it is
// evidence and not proof; every result shape below was cross-read against the
// Intel SDM before the data was accepted.
//
// WHY THE ROWS CARRY THE ENCODING
// -------------------------------
// The generator and this test could each build the instruction from the shared
// table, as the first wave did -- but then a wrong field in the table makes
// both sides test the same wrong instruction and the differential passes
// vacuously.  Here each row carries the LITERAL BYTES the generator executed
// and this file replays them, so the two sides cannot diverge onto different
// instructions at all.  Every distinct encoding was additionally disassembled
// and confirmed to be the intended mnemonic before the data was captured.
//
// WHAT THE DATA SETTLES, MEASURED RATHER THAN ASSUMED
//
//   Contract C3 on the scalar moves.  ymm0 is poisoned with 0xA5^index before
//   every row and all 32 bytes are read back, so a VEX.128 row's reference
//   carries sixteen literal zero bytes the HARDWARE wrote.  For `vmovss xmm0,
//   [rdi+4]` it carries twenty-eight of them: the load form has no merge
//   source at all and zeroes everything above the dword.  An implementation
//   that merged instead (the legacy SSE rule) shows the poison.
//
//   Which register a merge reads.  The reg-reg forms use dst=ymm0, src1=ymm1,
//   src2=ymm2 -- three DIFFERENT registers -- so an implementation taking the
//   preserved lanes from the destination rather than from VEX.vvvv is caught.
//
//   vmovss's two encodings.  0x10 /r and 0x11 /r both appear, and 0x11 swaps
//   which of ModRM.reg / ModRM.rm is the destination.  Both rows name the same
//   architectural operation, so a decoder that ignored the swap writes the
//   wrong register and fails one of them.
//
//   vpmovmskb's 256-bit combine is `lo | hi << 16`, and its 32-bit destination
//   ZEROES bits 63:32 -- rax is pre-set to -1 by the recorded bytes, so a
//   64-bit or merging write is visible.
//
// Each block is a SINGLE instruction (plus, where the answer is a GPR or
// EFLAGS, a deterministic seed and the capture): the operand registers are
// written straight into ThreadContext64 and the answer read straight back, so
// a broken vmovdqu cannot mask a broken handler.  Every register except the
// destination and the two sources is poisoned per register and per byte, so a
// handler that writes the wrong register's upper half is caught too.

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
#include "translator/x86/cpu.h"
#include "translator/x86/translator.h"

using namespace swift::translator::x86;
using namespace swift;

namespace {

struct AvxFp2Input {
    const char* name;
    const char* a;
    const char* b;
};
// form: 0 = the answer is ymm0, 1 = the 32-byte capture slot, 2 = rax.
struct AvxFp2Ref {
    const char* name;
    int width;  // 128 or 256: the VEX.L the generator encoded
    int pair;
    int form;
    const char* enc;     // literal instruction bytes, hex
    const char* result;  // 32 bytes read back, hex
};
#include "avx_fp2_rosetta_ref.inc"

// The instruction table, shared verbatim with the generator.  Only the NAME is
// consumed here -- the encoding comes from each row -- so this exists to pin
// coverage: every mnemonic named below must have produced reference rows.
struct Entry {
    const char* name;
};
constexpr Entry kEntries[] = {
#define SVM_FP2(name, shape, map, pp, w, opcode, aux) {#name},
#include "avx_fp2_ops.inc"
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

// Must match FP2_POISON in avx_fp2_rosetta_ref.c for register 0, which is what
// the generator loaded into ymm0; the other registers extend the same scheme so
// a clobber of the wrong register cannot masquerade as the right one.
Vec256 Poison(u32 reg) {
    Vec256 v{};
    for (u32 j = 0; j < 32; ++j) {
        v[j] = u8(0xA5 ^ (reg * 32 + j));
    }
    return v;
}

// Whether a reference row belongs to `entry`.  The S_MOVS / S_MOVLH / S_SI2F
// shapes suffix the mnemonic with the sub-form (".rr10", ".store", ".m"), so
// this is a prefix match at a component boundary rather than equality.
bool NameMatches(const char* row, const char* entry) {
    const size_t n = std::strlen(entry);
    return std::strncmp(row, entry, n) == 0 && (row[n] == '\0' || row[n] == '.');
}

// ---------------------------------------------------------------------------
// THE ONE PLACE THE ROSETTA ORACLE IS NOT BELIEVED
// ---------------------------------------------------------------------------
// Byte 1 of a vptest capture is PF.  The Intel SDM is unambiguous that
// (V)PTEST sets AF <- OF <- PF <- SF <- 0, and this file's implementation does
// that -- but Rosetta was measured returning BOTH answers for byte-identical
// code.  The reference data was captured in a run where PF came back as the
// value the flag seed left (1); five reduced probes replaying the very same
// instruction bytes returned 0, and whether a run gives 0 or 1 turned out to
// depend on unrelated details of the surrounding program (whether signal
// handlers were installed, how many vmovdqu preceded it).  A flag output that
// varies with the surrounding program is not an architectural result, so this
// is a Rosetta defect -- the fifth found over this work, after VPSLLVQ's
// 32-bit-truncated shift count.
//
// The response is to keep the measured bytes exactly as hardware produced them
// (nothing in the .inc is edited by hand, ever) and to compare vptest rows on
// CF, ZF, OF and SF while checking PF against the SDM's constant 0 instead.
// The exclusion is one named byte of one mnemonic, and PF is still ASSERTED --
// just against the manual rather than against the oracle.
constexpr size_t kVptestPfByte = 1;

}  // namespace

TEST_CASE("x86 avx fp2 vs rosetta reference") {
    const char* avx_env = std::getenv("SVM_AVX");
    if (!avx_env || std::strcmp(avx_env, "0") == 0) {
        SUCCEED("SVM_AVX is not set; second-wave VEX Rosetta differential skipped");
        return;
    }

    std::vector<Vec256> ins_a, ins_b;
    for (const auto& in : kAvxFp2Inputs) {
        ins_a.push_back(ParseHex32(in.a));
        ins_b.push_back(ParseHex32(in.b));
    }

    // ---- properties of the reference data this case depends on ------------
    // Asserted rather than trusted: without these the differential could still
    // pass while having stopped testing what it exists for.
    {
        // Every mnemonic in the shared table must have produced rows.  A
        // handler silently dropped from the generator would otherwise vanish
        // from the run with no failure.
        for (const auto& e : kEntries) {
            size_t rows = 0;
            for (const auto& r : kAvxFp2Refs) {
                if (NameMatches(r.name, e.name)) ++rows;
            }
            INFO("no reference rows for " << e.name
                                          << " -- the generator did not cover it, or Rosetta "
                                             "refused every encoding of it");
            REQUIRE(rows > 0);
        }
    }
    {
        // Contract C3, as the HARDWARE reported it: every VEX.128 row whose
        // answer is a register must carry sixteen zero bytes in its upper half,
        // against a destination that was poisoned beforehand.
        size_t checked = 0;
        for (const auto& r : kAvxFp2Refs) {
            if (r.width != 128 || r.form != 0) continue;
            const auto v = ParseHex32(r.result);
            INFO(r.name << " pair " << r.pair
                        << ": the 128-bit reference does not have a zeroed upper half");
            REQUIRE(std::all_of(v.begin() + 16, v.end(), [](u8 x) { return x == 0; }));
            ++checked;
        }
        INFO("no VEX.128 register-destination rows at all");
        REQUIRE(checked > 500);
    }
    {
        // vmovss's LOAD form zeroes bits 255:32, not just 255:128 -- so its
        // reference must be 28 zero bytes after the loaded dword.  This is the
        // single most easily mis-implemented row in the file and the assertion
        // pins that the data can still catch it.
        size_t seen = 0;
        for (const auto& r : kAvxFp2Refs) {
            if (std::strcmp(r.name, "vmovss.load") != 0) continue;
            const auto v = ParseHex32(r.result);
            INFO("vmovss.load reference has non-zero bytes above the loaded dword");
            REQUIRE(std::all_of(v.begin() + 4, v.end(), [](u8 x) { return x == 0; }));
            ++seen;
        }
        REQUIRE(seen > 0);
    }
    {
        // vptest must reach all four (ZF, CF) combinations, or the flag
        // plumbing is only half tested.  Bytes 0..4 of a form-1 vptest row are
        // CF, PF, ZF, OF, SF.
        bool seen[4] = {false, false, false, false};
        for (const auto& r : kAvxFp2Refs) {
            if (std::strcmp(r.name, "vptest") != 0) continue;
            const auto v = ParseHex32(r.result);
            seen[(v[2] ? 2 : 0) | (v[0] ? 1 : 0)] = true;
            // OF and SF are architecturally cleared, and the recorded bytes
            // SEED them to 1 first, so a handler that left them alone fails.
            // (PF is deliberately not asserted here -- see kVptestPfByte.)
            INFO("vptest reference does not clear OF/SF");
            REQUIRE(v[3] == 0);
            REQUIRE(v[4] == 0);
        }
        INFO("the vptest input pairs do not cover all four (ZF, CF) outcomes");
        REQUIRE(seen[0]);
        REQUIRE(seen[1]);
        REQUIRE(seen[2]);
        REQUIRE(seen[3]);
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
        Vec256 mem{};
        u64 rax{};
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
        ctx.rax.qword = 0xDEADBEEFDEADBEEFull;
        ctx.rsi.qword = 0xCAFEBABECAFEBABEull;
        ctx.rdi.qword = data;
        ctx.rsp.qword = stack;
        ctx.rip.qword = code_addr;
        Out o;
        o.exit = int(core->Run());
        for (u32 i = 0; i < 16; ++i) {
            std::memcpy(o.ymm[i].data(), ctx.xmms[i].b, 16);
            std::memcpy(o.ymm[i].data() + 16, ctx.ymm_high[i].b, 16);
        }
        o.rax = ctx.rax.qword;
        std::memcpy(o.mem.data(), reinterpret_cast<void*>(data + u64(kOffOut)), 32);
        return o;
    };

    for (const auto& ref : kAvxFp2Refs) {
        auto code = ParseHex(ref.enc);
        code.push_back(0xF4);  // hlt
        const u64 code_addr = base + 0x1000 + code_cursor * 0x100;
        ++code_cursor;
        REQUIRE(code.size() < 0x100);
        REQUIRE(code_addr + 0x100 < stack);
        const Vec256 want = ParseHex32(ref.result);
        const auto& a = ins_a[size_t(ref.pair)];
        const auto& b = ins_b[size_t(ref.pair)];
        const std::string label = fmt::format("{}.L{}/{}", ref.name, ref.width,
                                              kAvxFp2Inputs[ref.pair].name);
        const bool is_vptest = std::strcmp(ref.name, "vptest") == 0;

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
        if (jit.ymm != itp.ymm || jit.mem != itp.mem || jit.rax != itp.rax ||
            jit.exit != itp.exit) {
            if (divergences++ < 15) {
                problems.push_back(fmt::format(
                        "{}: JIT/interpreter divergence (ymm0 {} vs {}, mem {} vs {}, "
                        "rax {:#x} vs {:#x})",
                        label, Hex(jit.ymm[0]), Hex(itp.ymm[0]), Hex(jit.mem), Hex(itp.mem),
                        jit.rax, itp.rax));
            }
        }

        for (const auto& [backend, got] : {std::pair<const char*, const Out*>{"jit", &jit},
                                           std::pair<const char*, const Out*>{"interp", &itp}}) {
            if (ref.form == 2) {
                // The GPR answer.  The recorded bytes pre-set rax to -1, so a
                // 32-bit destination that failed to zero bits 63:32 shows up as
                // 0xFFFFFFFF in the high half rather than as zeros.
                u64 want_rax = 0;
                for (u32 i = 0; i < 8; ++i) {
                    want_rax |= u64(want[i]) << (i * 8);
                }
                if (got->rax != want_rax && mismatches++ < 15) {
                    problems.push_back(fmt::format("{} [{}]: rax {:#018x}, Rosetta says {:#018x}",
                                                   label, backend, got->rax, want_rax));
                }
            } else {
                const Vec256& g = ref.form == 1 ? got->mem : got->ymm[0];
                Vec256 masked_got = g;
                Vec256 masked_want = want;
                if (is_vptest) {
                    // PF is checked against the SDM instead of against the
                    // oracle; see kVptestPfByte.
                    if (masked_got[kVptestPfByte] != 0 && mismatches++ < 15) {
                        problems.push_back(fmt::format(
                                "{} [{}]: vptest left PF = {}; the SDM requires PF = 0", label,
                                backend, masked_got[kVptestPfByte]));
                    }
                    masked_got[kVptestPfByte] = 0;
                    masked_want[kVptestPfByte] = 0;
                }
                if (masked_got != masked_want && mismatches++ < 15) {
                    problems.push_back(fmt::format("{} [{}]: got {}, Rosetta says {} (enc {})",
                                                   label, backend, Hex(g), Hex(want), ref.enc));
                }
            }
            // No register beyond the destination and the two sources may
            // change -- in particular no bystander's UPPER half may be
            // disturbed by the two-halves split.  ymm0 is excluded because it
            // is the destination of most rows and legitimately poisoned-and-
            // unchanged in the rest.
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
    // opcode dropped from avx_fp2_ops.inc -- cannot pass as success.
    CHECK(comparisons == std::size(kAvxFp2Refs));
    CHECK(std::size(kAvxFp2Refs) == 4080u);
}
