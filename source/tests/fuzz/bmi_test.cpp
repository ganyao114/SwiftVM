// Differential test for the BMI1 / BMI2 family
// (source/runtime/frontend/x86/decoder_bmi.cc) against reference values
// produced by executing the same encodings on real x86-64 hardware under
// Rosetta 2, plus a literal transcription of the Intel SDM pseudo-code.
//
// WHY THREE SOURCES OF TRUTH AND NOT ONE
// ---------------------------------------------------------------------------
// Unicorn DOES execute BMI (unlike VEX.L=1, which it refuses outright), and it
// does honour VEX.vvvv on these GPR forms -- both were probed before this test
// was written rather than assumed.  It is nevertheless wrong in four separate
// places, found by diffing it against Rosetta over all 2640 rows and then
// adjudicating each disagreement against the manual (bmi_unicorn_check.c):
//
//   * BLSI's CF is INVERTED.  The SDM says "IF SRC = 0 THEN CF := 0 ELSE
//     CF := 1", the opposite of BLSR/BLSMSK; Unicorn implements the BLSR rule.
//     132 of the 177 disagreements.
//   * BZHI reduces N modulo the operand size and computes CF as
//     N >= width-1.  The SDM does neither: N is the full SRC2[7:0] and CF is
//     N > width-1.  36 rows.
//   * BEXTR reduces LEN modulo the operand size, so LEN = 0xFF at W0 extracts
//     31 bits instead of 32.  3 rows.
//   * PDEP at VEX.W0 computes the 64-bit result, ignoring the operand size.
//     6 rows.  (PEXT at W0 is correct, so this is not one bug but two code
//     paths.)
//
// Rosetta matched the SDM model on all 2640 rows, so the committed reference
// is Rosetta's and Unicorn is not used by this test at all.  That is the whole
// reason for bmi_model.h: with two emulators disagreeing, something outside
// both has to decide, and the test asserts the committed table still matches
// the model so a regenerated table cannot drift silently.
//
// WHAT THIS CASE IS FOR
// ---------------------------------------------------------------------------
// 1. OPERAND ROLES.  BLSI/BLSMSK/BLSR put the DESTINATION in VEX.vvvv, ANDN and
//    PDEP/PEXT take their FIRST source from it, and BEXTR/BZHI/SHLX/SARX/SHRX
//    take their SECOND -- three different meanings for one field.  Every input
//    pair is asymmetric so a swap cannot pass by coincidence, and variant 2
//    puts every operand in a high register so VEX.R/VEX.B folding is exercised.
// 2. THE FLAGS, ONE AT A TIME.  Each of CF/OF/ZF/SF/PF is materialized by its
//    own SETcc into its own byte, so a wrong flag is identified, not just
//    detected.  Only the architecturally DEFINED flags are compared.
// 3. "NO FLAGS AFFECTED" IS A CLAIM THAT NEEDS TESTING.  Half of BMI2 must not
//    touch any flag, which is easy to get wrong in a frontend with a lazy flag
//    window.  Prologue variant 1 sets ZF before the instruction and the test
//    requires it to SURVIVE.
// 4. CARRY POLARITY.  This frontend stores CF with ARM (not-borrow) semantics
//    after a subtract and compensates at consumers.  Prologue variant 1 is a
//    CMP, which leaves the tracker inverted; a handler that writes CF without
//    also resetting the polarity produces a complemented SETC, and only a
//    preceding subtract exposes it.
// 5. WIDTH.  Every row runs at both VEX.W values over the same 64-bit inputs,
//    so a 32-bit form computed at 64 bits (or an SF taken from bit 63) fails.
// 6. JIT VERSUS INTERPRETER.  Both backends run every block; they implement
//    the shifts and the flag machinery independently.
//
// The instruction table (bmi_ops.inc) and the encoder (bmi_shared.h) are shared
// VERBATIM with the generator, so the two sides cannot test different bytes.

#include <cstring>
#include <string.h>

#include <array>
#include <cstdlib>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <sys/mman.h>
#include "runtime/backend/smc_tracker.h"
#include "translator/x86/cpu.h"
#include "translator/x86/translator.h"

namespace bmi_case {
#include "bmi_shared.h"
#include "bmi_model.h"
#include "bmi_rosetta_ref.inc"
}  // namespace bmi_case

using namespace swift;
using namespace swift::translator::x86;
using namespace bmi_case;

namespace {

using u8 = swift::u8;
using u32 = swift::u32;
using u64 = swift::u64;

// The guest block: an optional flag-setting prologue, the instruction under
// test, five SETcc stores and HLT.  Registers are seeded straight into the
// guest context, so unlike the hardware stub there are no MOV loads to get in
// the way of the lazy flag window.
//
// prologue 0: mov edi,1 / test edi,edi  -> CF=OF=ZF=SF=PF=0, carry DIRECT.
//             Identical observable flag state to the generator's POPFQ 0x202.
// prologue 1: cmp edi,edi               -> ZF=1, PF=1, rest 0, carry INVERTED.
void BuildBlock(Buf& c, const Op& o, int width, const Assign& a, int prologue) {
    c.n = 0;
    if (prologue == 0) {
        emit(&c, 0xBF);  // mov edi, 1
        emit(&c, 0x01);
        emit(&c, 0x00);
        emit(&c, 0x00);
        emit(&c, 0x00);
        emit(&c, 0x85);  // test edi, edi
        emit(&c, 0xFF);
    } else {
        emit(&c, 0x39);  // cmp edi, edi
        emit(&c, 0xFF);
    }
    emit_insn(&c, &o, width == 64, &a);
    setcc_mem(&c, 0x92, DATA_FL + 0);
    setcc_mem(&c, 0x90, DATA_FL + 1);
    setcc_mem(&c, 0x94, DATA_FL + 2);
    setcc_mem(&c, 0x98, DATA_FL + 3);
    setcc_mem(&c, 0x9A, DATA_FL + 4);
    emit(&c, 0xF4);  // hlt
}

struct Out {
    std::array<u64, 16> regs{};
    std::array<u8, 5> flags{};
    int exit{};
};

}  // namespace

TEST_CASE("x86 bmi1/bmi2 vs rosetta reference") {
    const char* gate = std::getenv("SVM_BMI");
    if (gate == nullptr || std::strcmp(gate, "0") == 0) {
        SUCCEED("SVM_BMI is not set; BMI1/BMI2 differential skipped");
        return;
    }

    // ---- the reference is only usable if it still matches the manual -------
    // The committed table is hardware output, which is exactly why it needs an
    // independent check: a regeneration on a different machine, or an edit,
    // must not be able to move the goalposts unnoticed.
    {
        size_t checked = 0;
        std::vector<std::string> model_gaps;
        for (const auto& ref : kBmiRefs) {
            const Op* o = nullptr;
            for (const auto& cand : g_ops) {
                if (std::strcmp(cand.name, ref.name) == 0) {
                    o = &cand;
                }
            }
            const Pair* p = nullptr;
            for (const auto& cand : g_pairs) {
                if (std::strcmp(cand.name, ref.pair) == 0) {
                    p = &cand;
                }
            }
            REQUIRE(o != nullptr);
            REQUIRE(p != nullptr);
            const auto mo = bmi_model(o->name, ref.width, unsigned(o->imm), p->a, p->b);
            REQUIRE(mo.has_flags != -1);
            bool ok = ref.dst == mo.dst && ref.dst2 == mo.dst2 && ref.clean == 1;
            if (mo.has_flags == 0) {
                ok = ok && !ref.cf && !ref.of && !ref.zf && !ref.sf;
            } else {
                ok = ok && ref.cf == mo.cf && ref.of == mo.of && ref.zf == mo.zf &&
                     (mo.sf == -1 || ref.sf == mo.sf);
            }
            if (!ok && model_gaps.size() < 8) {
                model_gaps.push_back(fmt::format("{} w{} v{} {}", ref.name, ref.width,
                                                 ref.variant, ref.pair));
            }
            ++checked;
        }
        INFO(fmt::format("reference rows disagreeing with the SDM model: {}",
                         fmt::join(model_gaps, ", ")));
        CHECK(model_gaps.empty());
        REQUIRE(checked > 2000);
    }

    // ---- arena and the two cores ------------------------------------------
    constexpr size_t kArenaSize = 0x400000;
    swift::runtime::backend::SmcTracker::SetEnabled(false);
    void* arena = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(arena != MAP_FAILED);
    const u64 base = reinterpret_cast<u64>(arena);
    const u64 data = base + 0x300000;
    const u64 stack = base + 0x200000;

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

    size_t code_cursor = 1;
    std::vector<std::string> problems;
    size_t comparisons = 0, bad_exits = 0, divergences = 0, mismatches = 0, bystanders = 0,
           flag_bad = 0;

    const auto run_on = [&](X86Core* core, const Buf& code, const Pair& p, u64 code_addr) {
        std::memcpy(reinterpret_cast<void*>(code_addr), code.b, size_t(code.n));
        std::memset(reinterpret_cast<void*>(data), 0, DATA_SIZE);
        std::memcpy(reinterpret_cast<void*>(data + DATA_M), &p.b, 8);
        auto& ctx = core->GetContext();
        for (u32 i = 0; i < 16; ++i) {
            ctx.regs[i].qword = seed_for(int(i), &p);
        }
        ctx.r13.qword = data;
        ctx.rsp.qword = stack;
        ctx.rip.qword = code_addr;
        Out o;
        o.exit = int(core->Run());
        for (u32 i = 0; i < 16; ++i) {
            o.regs[i] = ctx.regs[i].qword;
        }
        std::memcpy(o.flags.data(), reinterpret_cast<void*>(data + DATA_FL), 5);
        return o;
    };

    size_t handled = 0;
    for (const auto& ref : kBmiRefs) {
        const Op* o = nullptr;
        for (const auto& cand : g_ops) {
            if (std::strcmp(cand.name, ref.name) == 0) {
                o = &cand;
            }
        }
        const Pair* p = nullptr;
        for (const auto& cand : g_pairs) {
            if (std::strcmp(cand.name, ref.pair) == 0) {
                p = &cand;
            }
        }
        REQUIRE(o != nullptr);
        REQUIRE(p != nullptr);
        const auto mo = bmi_model(o->name, ref.width, unsigned(o->imm), p->a, p->b);
        const Assign a = assign_for(o->shape, ref.variant);

        for (int prologue = 0; prologue < 2; ++prologue) {
            Buf code;
            BuildBlock(code, *o, ref.width, a, prologue);
            const u64 code_addr = base + code_cursor * 0x100;
            ++code_cursor;
            REQUIRE(code.n < 0x100);
            REQUIRE(code_cursor * 0x100 < 0x200000);

            const auto label = fmt::format("{} w{} v{} {} p{}", ref.name, ref.width, ref.variant,
                                           ref.pair, prologue);
            const auto jit = run_on(jit_core, code, *p, code_addr);
            const auto itp = run_on(interp_core, code, *p, code_addr);
            ++comparisons;
            ++handled;
            if (jit.exit != int(swift::translator::None)) {
                if (bad_exits++ < 12) {
                    problems.push_back(
                            fmt::format("{}: jit exit {} (block not translated)", label, jit.exit));
                }
                continue;
            }
            if (jit.regs != itp.regs || jit.flags != itp.flags || jit.exit != itp.exit) {
                if (divergences++ < 12) {
                    problems.push_back(fmt::format(
                            "{}: jit/interp divergence  jit dst={:016x} fl={}{}{}{}{} | "
                            "interp dst={:016x} fl={}{}{}{}{}",
                            label, jit.regs[u32(a.dst)], jit.flags[0], jit.flags[1], jit.flags[2],
                            jit.flags[3], jit.flags[4], itp.regs[u32(a.dst)], itp.flags[0],
                            itp.flags[1], itp.flags[2], itp.flags[3], itp.flags[4]));
                }
            }

            // Flags the instruction does not define keep whatever the prologue
            // left: 0 for prologue 0, and ZF = 1 for prologue 1.
            const int base_zf = prologue == 1 ? 1 : 0;
            for (const auto& [who, got] : {std::pair{"jit", &jit}, std::pair{"interp", &itp}}) {
                if (got->regs[u32(a.dst)] != ref.dst) {
                    if (mismatches++ < 16) {
                        problems.push_back(fmt::format("{}/{}: dst want {:016x} got {:016x}",
                                                       label, who, ref.dst,
                                                       got->regs[u32(a.dst)]));
                    }
                }
                if (a.dst2 >= 0 && got->regs[u32(a.dst2)] != ref.dst2) {
                    if (mismatches++ < 16) {
                        problems.push_back(fmt::format("{}/{}: dst2 want {:016x} got {:016x}",
                                                       label, who, ref.dst2,
                                                       got->regs[u32(a.dst2)]));
                    }
                }
                for (int i = 0; i < NREG; ++i) {
                    const int rg = REG_ORDER[i];
                    if (rg == a.dst || rg == a.dst2) {
                        continue;
                    }
                    if (got->regs[u32(rg)] != seed_for(rg, p)) {
                        if (bystanders++ < 12) {
                            problems.push_back(fmt::format("{}/{}: r{} clobbered ({:016x})", label,
                                                           who, rg, got->regs[u32(rg)]));
                        }
                    }
                }
                const int want_cf = mo.has_flags ? mo.cf : 0;
                const int want_of = mo.has_flags ? mo.of : 0;
                const int want_zf = mo.has_flags ? mo.zf : base_zf;
                const int want_sf = mo.has_flags ? mo.sf : 0;
                const char* names[4] = {"CF", "OF", "ZF", "SF"};
                const int want[4] = {want_cf, want_of, want_zf, want_sf};
                const u8 have[4] = {got->flags[0], got->flags[1], got->flags[2], got->flags[3]};
                for (int f = 0; f < 4; ++f) {
                    if (want[f] < 0) {
                        continue;  // architecturally undefined
                    }
                    if (int(have[f]) != want[f]) {
                        if (flag_bad++ < 24) {
                            problems.push_back(fmt::format("{}/{}: {} want {} got {}", label, who,
                                                           names[f], want[f], int(have[f])));
                        }
                    }
                }
            }
        }
    }

    munmap(arena, kArenaSize);

    INFO(fmt::format("{} comparisons, {} bad exits, {} jit/interp divergences, {} value "
                     "mismatches, {} bystander writes, {} flag mismatches\n{}",
                     comparisons, bad_exits, divergences, mismatches, bystanders, flag_bad,
                     fmt::join(problems, "\n")));
    CHECK(bad_exits == 0);
    CHECK(divergences == 0);
    CHECK(mismatches == 0);
    CHECK(bystanders == 0);
    CHECK(flag_bad == 0);
    REQUIRE(handled == std::size(kBmiRefs) * 2);
}

// Encodings the reference table cannot express, because the hardware generator
// can only record instructions that RUN.
//
// VEX.LZ means an L=1 encoding is #UD, and 0F38 F3 /0 and /4../7 are not BMI at
// all.  Both must DECLINE -- the block traps -- rather than being executed as
// some neighbouring instruction, which is the failure mode that would silently
// corrupt a guest instead of stopping it.  TZCNT on a zero source is the one
// place TZCNT and BSF genuinely differ, and it is the case that decides whether
// the BSF alias may stay once BMI1 is advertised.
TEST_CASE("x86 bmi VEX.LZ gating and the TZCNT/BSF split") {
    const char* gate = std::getenv("SVM_BMI");
    if (gate == nullptr || std::strcmp(gate, "0") == 0) {
        SUCCEED("SVM_BMI is not set; BMI1/BMI2 encoding gates skipped");
        return;
    }
    struct EdgeCase {
        const char* name;
        std::vector<u8> bytes;
        bool should_run;
        u64 want_rax;  // only when should_run
        int want_cf, want_zf;
    };
    // rsi = 0 for every case here, rax seeded with a poison the handler must
    // overwrite (or, for the trapping cases, must NOT).
    const std::vector<EdgeCase> cases = {
            // blsr eax, esi -- the legal VEX.LZ control.  esi = 0 gives 0, CF=1.
            {"blsr.L0", {0xC4, 0xE2, 0x78, 0xF3, 0xCE, 0xF4}, true, 0, 1, 1},
            // the same with VEX.L = 1
            {"blsr.L1", {0xC4, 0xE2, 0x7C, 0xF3, 0xCE, 0xF4}, false, 0, 0, 0},
            // shrx / andn at VEX.L = 1
            {"shrx.L1", {0xC4, 0xE2, 0x77, 0xF7, 0xC6, 0xF4}, false, 0, 0, 0},
            {"andn.L1", {0xC4, 0xE2, 0x74, 0xF2, 0xC6, 0xF4}, false, 0, 0, 0},
            // 0F38 F3 /0 and /4: inside the BLS group's opcode but not BMI
            {"grpF3./0", {0xC4, 0xE2, 0x78, 0xF3, 0xC6, 0xF4}, false, 0, 0, 0},
            {"grpF3./4", {0xC4, 0xE2, 0x78, 0xF3, 0xE6, 0xF4}, false, 0, 0, 0},
            // tzcnt eax, esi with esi = 0: BSF would leave eax alone and set ZF,
            // TZCNT writes the operand width, clears ZF and sets CF.
            {"tzcnt.src0", {0xF3, 0x0F, 0xBC, 0xC6, 0xF4}, true, 32, 1, 0},
            {"lzcnt.src0", {0xF3, 0x0F, 0xBD, 0xC6, 0xF4}, true, 32, 1, 0},
            // ... and with bit 0 set, where ZF is 1 because the RESULT is zero.
            {"tzcnt.one", {0xF3, 0x0F, 0xBC, 0xC6, 0xF4}, true, 0, 0, 1},
    };

    constexpr size_t kArenaSize = 0x100000;
    swift::runtime::backend::SmcTracker::SetEnabled(false);
    void* arena = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(arena != MAP_FAILED);
    const u64 base = reinterpret_cast<u64>(arena);
    setenv("SVM_ENABLE_JIT", "1", 1);
    auto* core = X86Core::Make(X86Instance::Make());

    u32 slot = 1;
    for (const auto& c : cases) {
        // A fresh guest address per case: translated blocks are cached by guest
        // address and the SMC tracker is off, so reusing one address replays
        // the first translation for every later case (this probe reported all
        // eight cases passing that way before the addresses were separated).
        const u64 addr = base + 0x1000 * slot;
        const u64 flags_at = base + 0x800;
        ++slot;
        std::vector<u8> code = c.bytes;
        if (c.should_run) {
            // Replace the trailing HLT with SETC/SETZ into memory, then HLT.
            code.pop_back();
            Buf tail;
            tail.n = 0;
            setcc_mem(&tail, 0x92, 0);  // setc  [r13+0]
            setcc_mem(&tail, 0x94, 1);  // setz  [r13+1]
            code.insert(code.end(), tail.b, tail.b + tail.n);
            code.push_back(0xF4);
        }
        std::memcpy(reinterpret_cast<void*>(addr), code.data(), code.size());
        std::memset(reinterpret_cast<void*>(flags_at), 0xEE, 8);
        auto& ctx = core->GetContext();
        for (u32 i = 0; i < 16; ++i) {
            ctx.regs[i].qword = 0xAAAA000000000000ull | i;
        }
        ctx.rax.qword = 0x1111111111111111ull;
        ctx.rcx.qword = 4;
        ctx.rsi.qword = std::strcmp(c.name, "tzcnt.one") == 0 ? 1 : 0;
        ctx.r13.qword = flags_at;
        ctx.rsp.qword = base + 0x80000;
        ctx.rip.qword = addr;
        const int exit = int(core->Run());
        INFO(c.name);
        if (!c.should_run) {
            CHECK(exit != int(swift::translator::None));
            // A declined encoding must not have written anything either.
            CHECK(ctx.rax.qword == 0x1111111111111111ull);
            continue;
        }
        REQUIRE(exit == int(swift::translator::None));
        CHECK(ctx.rax.qword == c.want_rax);
        const u8* fl = reinterpret_cast<const u8*>(flags_at);
        CHECK(int(fl[0]) == c.want_cf);
        CHECK(int(fl[1]) == c.want_zf);
    }
    munmap(arena, kArenaSize);
}
