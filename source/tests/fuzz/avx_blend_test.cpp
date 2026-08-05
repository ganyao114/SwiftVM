// ===========================================================================
// VEX blend / extract / maskmov family against a ROSETTA oracle.
// ===========================================================================
//
// Covers vblendps, vblendpd, vblendvps, vblendvpd, vextractps, vinsertps and
// vmaskmovps/vmaskmovpd (load and store), both VEX widths -- the ten encodings
// added in source/runtime/frontend/x86/decoder_avx_blend.cc.
//
// WHY A HARDWARE ORACLE
// ---------------------
// Unicorn 2.1.4 refuses every VEX.L=1 encoding (UC_ERR_INSN_INVALID), so the
// 256-bit forms have no emulator oracle at all.  Even the 128-bit forms need
// one here, because what can go wrong in this family is not arithmetic but
// PLUMBING: which imm8 bit drives which lane of which half, whether the /is4
// nibble or VEX.vvvv named the selector, whether vinsertps split its imm8 into
// the right three fields, which register a masked store takes its data from.
// A hand-written model of that only re-states the implementation's own
// assumption.  Rosetta 2 on macOS 26/27 executes AVX including the full
// 256-bit register file, so avx_blend_rosetta_ref.inc holds the literal bytes
// real x86-64 wrote.  Nothing in it is computed here.
//
// Rosetta is itself an emulator and has been measured wrong before over this
// work (VPSLLVQ's shift count truncated to 32 bits, XSAVE writing extra bytes,
// a non-deterministic PF out of vptest), so agreement with it is evidence and
// not proof; every result shape below was cross-read against the Intel SDM
// before the data was accepted, and each of the sixteen distinct encoding
// shapes was disassembled and confirmed to be the intended mnemonic.
//
// WHY THE ROWS CARRY THE ENCODING
// -------------------------------
// The generator and this test could each build the instruction from the shared
// table -- but then a wrong field in the table makes both sides test the same
// wrong instruction and the differential passes vacuously.  Here each row
// carries the LITERAL BYTES the generator executed and this file replays them,
// so the two sides cannot diverge onto different instructions at all.
//
// WHAT THE DATA SETTLES, MEASURED RATHER THAN ASSUMED
//
//   Contract C3.  ymm0 is poisoned with 0xA5^index before every row and all 32
//   bytes are read back, so a VEX.128 row's reference carries sixteen literal
//   zero bytes the HARDWARE wrote.  An implementation that preserved the upper
//   half (the legacy SSE rule) shows the poison.
//
//   Which register supplies which operand.  dst = ymm0, SRC1 = ymm1,
//   SRC2 = ymm2 and the selector/mask = ymm3 are four DIFFERENT registers, so
//   an implementation that took SRC1 from the destination, or the selector
//   from VEX.vvvv instead of the /is4 nibble, is caught.
//
//   vmaskmov's masked-off elements.  The store rows write into a capture slot
//   pre-filled with 0xCC on BOTH sides, so every byte hardware did not write is
//   a literal 0xCC in the reference.  "Masked-off elements are not stored" is
//   therefore a measured property of the data, not an assumption of the test.
//
//   vextractps's 32-bit destination zeroes bits 63:32: the recorded bytes set
//   rax to -1 first, so a 64-bit or merging write shows 0xFFFFFFFF.
//
//   vinsertps ignores imm8[7:6] for a MEMORY source.  The generator emits the
//   same imm8 grid for both operand shapes, so the four memory rows that differ
//   only in imm8[7:6] must agree with each other while the four register rows
//   must not -- asserted below as a property of the reference itself.
//
// Each block is a SINGLE instruction (plus, for vextractps's register form, the
// `mov rax, -1` canary): the operand registers are written straight into
// ThreadContext64 and the answer read straight back, so a broken vmovdqu cannot
// mask a broken handler.  Every register except the destination and the three
// sources is poisoned per register and per byte, so a handler that writes the
// wrong register's upper half is caught too.
//
// THE SECOND TEST CASE
// --------------------
// vmaskmov's other half -- a masked-off element must not FAULT -- cannot be a
// reference row, because a row records a value and this is the absence of a
// signal.  "x86 avx vmaskmov fault suppression" therefore runs the instruction
// straddling a PROT_NONE page with the elements on that page masked off, under
// a SIGSEGV/SIGBUS handler, and asserts that no fault was taken and the answer
// is right.  The Rosetta generator runs the same probe and its four
// `// MASKFAULT` comments in avx_blend_rosetta_ref.inc record that hardware
// completes it too.

#include <algorithm>
#include <array>
#include <csetjmp>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <map>
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

struct AvxBlendInput {
    const char* name;
    const char* a;
    const char* b;
    const char* m;
};
// form: 0 = the answer is ymm0, 1 = the 32-byte capture slot, 2 = rax.
struct AvxBlendRef {
    const char* name;
    int width;  // 128 or 256: the VEX.L the generator encoded
    int pair;
    int form;
    const char* enc;     // literal instruction bytes, hex
    const char* result;  // 32 bytes read back, hex
};
#include "avx_blend_rosetta_ref.inc"

// The instruction table, shared verbatim with the generator.  Only the NAME is
// consumed here -- the encoding comes from each row -- so this exists to pin
// coverage: every mnemonic named below must have produced reference rows.
struct Entry {
    const char* name;
};
constexpr Entry kEntries[] = {
#define SVM_BLEND(name, shape, map, pp, w, opcode, aux) {#name},
#include "avx_blend_ops.inc"
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

// Must match BLEND_POISON in avx_blend_rosetta_ref.c for register 0, which is
// what the generator loaded into ymm0; the other registers extend the same
// scheme so a clobber of the wrong register cannot masquerade as the right one.
Vec256 Poison(u32 reg) {
    Vec256 v{};
    for (u32 j = 0; j < 32; ++j) {
        v[j] = u8(0xA5 ^ (reg * 32 + j));
    }
    return v;
}

// Whether a reference row belongs to `entry`.  The vextractps and vmaskmov
// shapes suffix the mnemonic with the sub-form (".r", ".m", ".ld", ".st"), so
// this is a prefix match at a component boundary rather than equality.
bool NameMatches(const char* row, const char* entry) {
    const size_t n = std::strlen(entry);
    return std::strncmp(row, entry, n) == 0 && (row[n] == '\0' || row[n] == '.');
}

// ---- the guest data block, shared by both test cases ----------------------
// The generator's layout, addressed through rdi.  Replayed encodings carry
// these displacements literally, so it is not a choice here.
constexpr s32 kOffA = 0x00, kOffB = 0x20, kOffM = 0x40, kOffOut = 0x60, kOffPoison = 0x80;

}  // namespace

TEST_CASE("x86 avx blend vs rosetta reference") {
    if (!swift::runtime::GetSvmConfig().avx) {
        SUCCEED("SVM_AVX is not set; VEX blend/extract/maskmov Rosetta differential skipped");
        return;
    }

    std::vector<Vec256> ins_a, ins_b, ins_m;
    for (const auto& in : kAvxBlendInputs) {
        ins_a.push_back(ParseHex32(in.a));
        ins_b.push_back(ParseHex32(in.b));
        ins_m.push_back(ParseHex32(in.m));
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
            for (const auto& r : kAvxBlendRefs) {
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
        for (const auto& r : kAvxBlendRefs) {
            if (r.width != 128 || r.form != 0) continue;
            const auto v = ParseHex32(r.result);
            INFO(r.name << " pair " << r.pair
                        << ": the 128-bit reference does not have a zeroed upper half");
            REQUIRE(std::all_of(v.begin() + 16, v.end(), [](u8 x) { return x == 0; }));
            ++checked;
        }
        INFO("no VEX.128 register-destination rows at all");
        REQUIRE(checked > 400);
    }
    {
        // A masked STORE must leave the un-selected bytes of the capture slot
        // at their pre-fill of 0xCC, and must actually write the selected ones.
        // Both directions have to occur in the data, or the rows could not tell
        // "wrote nothing" from "wrote everything".
        size_t partial = 0, full = 0, empty = 0;
        for (const auto& r : kAvxBlendRefs) {
            if (std::strncmp(r.name, "vmaskmov", 8) != 0 || r.form != 1) continue;
            const auto v = ParseHex32(r.result);
            const size_t bytes = size_t(r.width) / 8;
            const size_t untouched =
                    size_t(std::count(v.begin(), v.begin() + long(bytes), u8(0xCC)));
            if (untouched == bytes) {
                ++empty;
            } else if (untouched == 0) {
                ++full;
            } else {
                ++partial;
            }
            // Nothing beyond the encoded width may ever be written.
            INFO(r.name << " pair " << r.pair << ": the store touched bytes past VEX.L's width");
            REQUIRE(std::all_of(v.begin() + long(bytes), v.end(), [](u8 x) { return x == 0xCC; }));
        }
        INFO("the masked-store references do not cover all three of "
             "no-element / some-elements / every-element stored");
        REQUIRE(empty > 0);
        REQUIRE(partial > 0);
        REQUIRE(full > 0);
    }
    {
        // vextractps into a GPR: rax was -1, so bits 63:32 of the reference
        // being zero is hardware reporting the 32-bit write's zero-extension.
        size_t seen = 0;
        for (const auto& r : kAvxBlendRefs) {
            if (std::strcmp(r.name, "vextractps.r") != 0) continue;
            const auto v = ParseHex32(r.result);
            INFO("vextractps.r reference does not zero bits 63:32 of rax");
            REQUIRE(std::all_of(v.begin() + 4, v.begin() + 8, [](u8 x) { return x == 0; }));
            ++seen;
        }
        REQUIRE(seen > 0);
    }

    // Index the rows so the two "has teeth" properties below can compare
    // specific encodings against each other.
    std::map<std::string, std::string> by_enc;
    for (const auto& r : kAvxBlendRefs) {
        by_enc[std::string(r.name) + "/" + std::to_string(r.pair) + "/" + r.enc] = r.result;
    }
    const auto lookup = [&](const char* name, int pair, const std::string& enc) {
        auto it = by_enc.find(std::string(name) + "/" + std::to_string(pair) + "/" + enc);
        return it == by_enc.end() ? std::string{} : it->second;
    };
    {
        // vinsertps: imm8[7:6] selects the SOURCE LANE for a register operand
        // and is IGNORED for a memory one.  The generator emits the same four
        // imm8 values (0x00 / 0x40 / 0x80 / 0xC0, i.e. COUNT_S = 0..3 with
        // COUNT_D = 0 and ZMASK = 0) for both shapes, so hardware must make the
        // four memory answers identical and the four register answers not.
        // If the register answers were also identical the whole COUNT_S field
        // would be untested and this file would pass without checking it.
        size_t mem_groups = 0, reg_groups = 0;
        for (int pair = 0; pair < int(std::size(kAvxBlendInputs)); ++pair) {
            // c4 e3 71 21 87 20000000 ib = vinsertps xmm0, xmm1, [rdi+0x20], ib
            // c4 e3 71 21 c2       ib = vinsertps xmm0, xmm1, xmm2, ib
            const std::string mem_pre = "c4e371218720000000";
            const std::string reg_pre = "c4e37121c2";
            std::vector<std::string> mem, reg;
            for (const char* imm : {"00", "40", "80", "c0"}) {
                mem.push_back(lookup("vinsertps", pair, mem_pre + imm));
                reg.push_back(lookup("vinsertps", pair, reg_pre + imm));
            }
            if (std::all_of(mem.begin(), mem.end(), [](const std::string& s) { return !s.empty(); })) {
                INFO("vinsertps pair " << pair
                                       << ": the MEMORY rows disagree across imm8[7:6], which is "
                                          "architecturally ignored for a memory source");
                REQUIRE(mem[0] == mem[1]);
                REQUIRE(mem[0] == mem[2]);
                REQUIRE(mem[0] == mem[3]);
                ++mem_groups;
            }
            if (std::all_of(reg.begin(), reg.end(), [](const std::string& s) { return !s.empty(); }) &&
                !(reg[0] == reg[1] && reg[0] == reg[2] && reg[0] == reg[3])) {
                ++reg_groups;
            }
        }
        INFO("no vinsertps memory rows were found to compare");
        REQUIRE(mem_groups > 0);
        INFO("every vinsertps REGISTER row gives the same answer for all four imm8[7:6] values, "
             "so the source-lane field is not actually exercised");
        REQUIRE(reg_groups > 0);
    }
    {
        // vblendv*: the selector is the /is4 byte's HIGH NIBBLE.  The generator
        // emits the same instruction with the selector naming ymm3, ymm1 and
        // ymm2, and at least one input triple must make those three disagree --
        // otherwise a handler reading VEX.vvvv or ModRM.rm as the selector
        // would pass.
        size_t discriminating = 0;
        for (int pair = 0; pair < int(std::size(kAvxBlendInputs)); ++pair) {
            const std::string a = lookup("vblendvps", pair, "c4e3754ac230");
            const std::string b = lookup("vblendvps", pair, "c4e3754ac210");
            const std::string c = lookup("vblendvps", pair, "c4e3754ac220");
            if (!a.empty() && !b.empty() && !c.empty() && a != b && a != c) {
                ++discriminating;
            }
        }
        INFO("no input triple makes vblendvps disagree across the three /is4 selector registers, "
             "so reading the wrong one would go unnoticed");
        REQUIRE(discriminating > 0);
    }
    {
        // The /is4 byte's LOW nibble is architecturally ignored.  Hardware
        // agreeing with itself across it is what licenses the implementation to
        // mask it off.
        size_t compared = 0;
        for (int pair = 0; pair < int(std::size(kAvxBlendInputs)); ++pair) {
            const std::string a = lookup("vblendvps", pair, "c4e3754ac230");
            const std::string b = lookup("vblendvps", pair, "c4e3754ac23b");
            if (a.empty() || b.empty()) continue;
            INFO("vblendvps pair " << pair << ": hardware distinguishes the /is4 LOW nibble");
            REQUIRE(a == b);
            ++compared;
        }
        REQUIRE(compared > 0);
    }

    // ---- harness -----------------------------------------------------------
    constexpr size_t kArenaSize = 0x400000;
    swift::runtime::backend::SmcTracker::SetEnabled(false);
    void* arena = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(arena != MAP_FAILED);
    const u64 base = reinterpret_cast<u64>(arena);
    const u64 stack = base + 0x200000;
    const u64 data = base + 0x300000;

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
        Vec256 mem{};
        u64 rax{};
        int exit{};
    };

    size_t code_cursor = 1;
    std::vector<std::string> problems;
    size_t comparisons = 0, bad_exits = 0, divergences = 0, mismatches = 0, bystanders = 0;

    const auto run_on = [&](X86Core* core, const std::vector<u8>& code, const Vec256& a,
                            const Vec256& b, const Vec256& m, u64 code_addr) {
        std::memcpy(reinterpret_cast<void*>(code_addr), code.data(), code.size());
        std::memcpy(reinterpret_cast<void*>(data + u64(kOffA)), a.data(), 32);
        std::memcpy(reinterpret_cast<void*>(data + u64(kOffB)), b.data(), 32);
        std::memcpy(reinterpret_cast<void*>(data + u64(kOffM)), m.data(), 32);
        std::memset(reinterpret_cast<void*>(data + u64(kOffOut)), 0xCC, 32);
        const auto p0 = Poison(0);
        std::memcpy(reinterpret_cast<void*>(data + u64(kOffPoison)), p0.data(), 32);
        auto& ctx = core->GetContext();
        for (u32 i = 0; i < 16; ++i) {
            const auto p = Poison(i);
            std::memcpy(ctx.xmms[i].b, p.data(), 16);
            std::memcpy(ctx.ymm_high[i].b, p.data() + 16, 16);
        }
        // ymm1 = A, ymm2 = B, ymm3 = M; ymm0 keeps its poison -- the same state
        // the generator's prologue produced.
        std::memcpy(ctx.xmms[1].b, a.data(), 16);
        std::memcpy(ctx.ymm_high[1].b, a.data() + 16, 16);
        std::memcpy(ctx.xmms[2].b, b.data(), 16);
        std::memcpy(ctx.ymm_high[2].b, b.data() + 16, 16);
        std::memcpy(ctx.xmms[3].b, m.data(), 16);
        std::memcpy(ctx.ymm_high[3].b, m.data() + 16, 16);
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

    for (const auto& ref : kAvxBlendRefs) {
        auto code = ParseHex(ref.enc);
        code.push_back(0xF4);  // hlt
        const u64 code_addr = base + 0x1000 + code_cursor * 0x100;
        ++code_cursor;
        REQUIRE(code.size() < 0x100);
        REQUIRE(code_addr + 0x100 < stack);
        const Vec256 want = ParseHex32(ref.result);
        const auto& a = ins_a[size_t(ref.pair)];
        const auto& b = ins_b[size_t(ref.pair)];
        const auto& m = ins_m[size_t(ref.pair)];
        const std::string label =
                fmt::format("{}.L{}/{}", ref.name, ref.width, kAvxBlendInputs[ref.pair].name);

        const auto jit = run_on(jit_core, code, a, b, m, code_addr);
        const auto itp = run_on(interp_core, code, a, b, m, code_addr);
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
                if (g != want && mismatches++ < 15) {
                    problems.push_back(fmt::format("{} [{}]: got {}, Rosetta says {} (enc {})",
                                                   label, backend, Hex(g), Hex(want), ref.enc));
                }
            }
            // No register beyond the destination and the three sources may
            // change -- in particular no bystander's UPPER half may be
            // disturbed by the two-halves split.  ymm0 is excluded because it
            // is the destination of most rows and legitimately poisoned-and-
            // unchanged in the rest.
            for (u32 i = 4; i < 16; ++i) {
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
    // opcode dropped from avx_blend_ops.inc -- cannot pass as success.
    CHECK(comparisons == std::size(kAvxBlendRefs));
    CHECK(std::size(kAvxBlendRefs) == 1120u);
}

// ===========================================================================
// vmaskmov fault suppression.
// ===========================================================================
// The half of vmaskmovps/vmaskmovpd that no reference row can express: an
// element whose mask bit is clear must not touch memory AT ALL, so a masked-off
// element sitting on an unmapped page must not fault.  This is not a corner
// case -- it is what the instruction exists for.  A vectorized loop's tail
// masks off the elements past the end of the array, and the page after the
// array is exactly where the mapping tends to stop.
//
// Method: a three-page arena whose middle page is PROT_NONE, with the access
// placed so its upper half lands on that page and the mask clearing precisely
// those elements.  A SIGSEGV/SIGBUS handler with siglongjmp catches a fault
// instead of killing the test binary, so BOTH outcomes are reportable rather
// than one of them being a crash.
//
// Rosetta runs the same four probes; see the `// MASKFAULT` comments at the end
// of avx_blend_rosetta_ref.inc, which record that hardware completes them.
namespace {

sigjmp_buf g_fault_jmp;
volatile sig_atomic_t g_faulted = 0;

void OnFault(int) {
    g_faulted = 1;
    siglongjmp(g_fault_jmp, 1);
}

}  // namespace

TEST_CASE("x86 avx vmaskmov fault suppression") {
    if (!swift::runtime::GetSvmConfig().avx) {
        SUCCEED("SVM_AVX is not set; vmaskmov fault-suppression case skipped");
        return;
    }

    constexpr size_t kPage = 0x4000;  // >= any page size this runs on
    constexpr size_t kArenaSize = 0x400000;
    swift::runtime::backend::SmcTracker::SetEnabled(false);
    void* arena = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(arena != MAP_FAILED);
    const u64 base = reinterpret_cast<u64>(arena);
    const u64 stack = base + 0x100000;
    // The guard page and the readable page immediately below it.
    const u64 guard = base + 0x300000;
    REQUIRE(mprotect(reinterpret_cast<void*>(guard), kPage, PROT_NONE) == 0);

    swift::runtime::SetSvmConfigEnvForTest("SVM_ENABLE_JIT", "1", 1);
    auto* jit_instance = X86Instance::Make();
    swift::runtime::SetSvmConfigEnvForTest("SVM_ENABLE_JIT", "0", 1);
    auto* interp_instance = X86Instance::Make();
    swift::runtime::UnsetSvmConfigEnvForTest("SVM_ENABLE_JIT");
    auto* jit_core = X86Core::Make(jit_instance);
    auto* interp_core = X86Core::Make(interp_instance);

    // Each probe: the encoding, its access width in bytes, and whether it
    // stores.  All were disassembled and confirmed before use.
    struct Probe {
        const char* name;
        std::vector<u8> code;
        size_t bytes;  // total access width
        bool store;
    };
    const std::vector<Probe> probes = {
            // vmaskmovps ymm0, ymm3, [rdi]
            {"vmaskmovps.ld.256", {0xC4, 0xE2, 0x65, 0x2C, 0x07}, 32, false},
            // vmaskmovpd ymm0, ymm3, [rdi]
            {"vmaskmovpd.ld.256", {0xC4, 0xE2, 0x65, 0x2D, 0x07}, 32, false},
            // vmaskmovps [rdi], ymm3, ymm1
            {"vmaskmovps.st.256", {0xC4, 0xE2, 0x65, 0x2E, 0x0F}, 32, true},
            // vmaskmovpd [rdi], ymm3, ymm1
            {"vmaskmovpd.st.256", {0xC4, 0xE2, 0x65, 0x2F, 0x0F}, 32, true},
            // vmaskmovps xmm0, xmm3, [rdi]
            {"vmaskmovps.ld.128", {0xC4, 0xE2, 0x61, 0x2C, 0x07}, 16, false},
            // vmaskmovps [rdi], xmm3, xmm1
            {"vmaskmovps.st.128", {0xC4, 0xE2, 0x61, 0x2E, 0x0F}, 16, true},
    };

    struct sigaction old_segv{}, old_bus{}, act{};
    act.sa_handler = OnFault;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    REQUIRE(sigaction(SIGSEGV, &act, &old_segv) == 0);
    REQUIRE(sigaction(SIGBUS, &act, &old_bus) == 0);

    std::vector<std::string> problems;
    size_t code_cursor = 1;

    for (const auto& probe : probes) {
        for (const auto& [backend, core] : {std::pair<const char*, X86Core*>{"jit", jit_core},
                                            std::pair<const char*, X86Core*>{"interp",
                                                                             interp_core}}) {
            // Only the LOWER half of the access is on a mapped page; the mask
            // clears every element of the upper half.
            const size_t half = probe.bytes / 2;
            const u64 access = guard - half;
            auto code = probe.code;
            code.push_back(0xF4);  // hlt
            const u64 code_addr = base + 0x1000 + code_cursor * 0x100;
            ++code_cursor;
            std::memcpy(reinterpret_cast<void*>(code_addr), code.data(), code.size());
            std::memset(reinterpret_cast<void*>(access), 0x5A, half);

            auto& ctx = core->GetContext();
            for (u32 i = 0; i < 16; ++i) {
                const auto p = Poison(i);
                std::memcpy(ctx.xmms[i].b, p.data(), 16);
                std::memcpy(ctx.ymm_high[i].b, p.data() + 16, 16);
            }
            // ymm1 = the store's data (distinct per byte); ymm3 = the mask,
            // every lane of the LOWER half selected and every lane of the
            // upper half cleared.
            Vec256 src{}, mask{};
            for (u32 i = 0; i < 32; ++i) {
                src[i] = u8(0x40 + i);
                mask[i] = i < 16 ? u8(0xFF) : u8(0x00);
            }
            std::memcpy(ctx.xmms[1].b, src.data(), 16);
            std::memcpy(ctx.ymm_high[1].b, src.data() + 16, 16);
            std::memcpy(ctx.xmms[3].b, mask.data(), 16);
            std::memcpy(ctx.ymm_high[3].b, mask.data() + 16, 16);
            // A VEX.128 probe puts its second 8 bytes on the guard page, so its
            // mask must clear the UPPER 8 bytes of xmm3 rather than the upper
            // 16 of ymm3.
            if (probe.bytes == 16) {
                std::memset(ctx.xmms[3].b + 8, 0x00, 8);
            }
            ctx.rdi.qword = access;
            ctx.rsp.qword = stack;
            ctx.rip.qword = code_addr;

            g_faulted = 0;
            int exit_code = -1;
            if (sigsetjmp(g_fault_jmp, 1) == 0) {
                exit_code = int(core->Run());
            }

            if (g_faulted) {
                problems.push_back(fmt::format(
                        "{} [{}]: took SIGSEGV/SIGBUS -- a masked-off element touched the "
                        "guard page, so fault suppression is NOT implemented",
                        probe.name, backend));
                continue;
            }
            if (exit_code != int(swift::translator::None)) {
                problems.push_back(fmt::format("{} [{}]: block did not reach HLT (exit={})",
                                               probe.name, backend, exit_code));
                continue;
            }
            if (probe.store) {
                // The mapped half must hold the source's lower half, and only
                // that: the guard page cannot be read back, but a store that
                // reached it would have faulted above.
                for (size_t i = 0; i < half; ++i) {
                    const u8 got = reinterpret_cast<const u8*>(access)[i];
                    if (got != src[i]) {
                        problems.push_back(fmt::format(
                                "{} [{}]: byte {} of the masked store is {:#04x}, expected {:#04x}",
                                probe.name, backend, i, got, src[i]));
                        break;
                    }
                }
            } else {
                Vec256 got{};
                std::memcpy(got.data(), ctx.xmms[0].b, 16);
                std::memcpy(got.data() + 16, ctx.ymm_high[0].b, 16);
                Vec256 want{};
                for (size_t i = 0; i < half; ++i) {
                    want[i] = 0x5A;  // the mapped half, loaded
                }
                // Everything else -- the masked-off elements AND, at VEX.128,
                // bits 255:128 -- must be zero.
                if (got != want) {
                    problems.push_back(fmt::format("{} [{}]: got {}, expected {}", probe.name,
                                                   backend, Hex(got), Hex(want)));
                }
            }
        }
    }

    sigaction(SIGSEGV, &old_segv, nullptr);
    sigaction(SIGBUS, &old_bus, nullptr);

    X86Core::Destroy(jit_core);
    X86Core::Destroy(interp_core);
    X86Instance::Destroy(jit_instance);
    X86Instance::Destroy(interp_instance);
    swift::runtime::backend::SmcTracker::SetEnabled(true);
    munmap(arena, kArenaSize);

    std::string report;
    for (const auto& p : problems) {
        report += p;
        report += '\n';
    }
    INFO(report);
    CHECK(problems.empty());
}
