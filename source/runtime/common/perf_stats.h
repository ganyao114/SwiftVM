//
// Translation-pipeline phase counters.
//
// Off unless SVM_PROF is set in the environment: every probe is guarded by one
// process-constant bool, and every probe sits on a "once per compiled unit"
// path -- never on a per-executed-guest-block path -- so enabling it cannot
// perturb the thing being measured (guest execution time is derived as
// wall - translate).
//
// The counters are far more useful than wall clock for this pipeline: unit
// counts and emitted byte counts are exactly reproducible run to run, and the
// per-phase nanoseconds are only compared *within* one process against each
// other, never across differently loaded machines.
//
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace swift::runtime {

struct PerfStats {
    // Phases of one compiled unit, in pipeline order. Anything the unit spends
    // outside these -- builder setup and teardown, module lookup, the driver's
    // own scans -- shows up as translate_ns minus their sum, which is where the
    // pipeline's fixed per-unit overhead is visible.
    std::atomic<unsigned long long> decode_ns{0};     // frontend decode + HIR build
    std::atomic<unsigned long long> rpo_ns{0};        // ComputeRPO + IdByRPO (both)
    std::atomic<unsigned long long> opt_ns{0};        // PassPipeline
    std::atomic<unsigned long long> regalloc_ns{0};   // RegisterAllocPass
    std::atomic<unsigned long long> codegen_ns{0};    // JitTranslator + Flush
    std::atomic<unsigned long long> publish_ns{0};    // module push, L2 slots, SMC
    std::atomic<unsigned long long> ir_free_ns{0};    // releasing the unit's IR
    std::atomic<unsigned long long> translate_ns{0};  // whole Translate() call

    std::atomic<unsigned long long> func_units{0};
    std::atomic<unsigned long long> block_units{0};
    std::atomic<unsigned long long> decoded_blocks{0};
    std::atomic<unsigned long long> host_bytes{0};
    std::atomic<unsigned long long> ir_insts{0};
    std::atomic<unsigned long long> pool_bytes{0};  // bytes handed to the allocator
    // High-water mark of the ir::Inst arena: chunk bytes malloc'd by
    // Inst::operator new. Chunks are never returned, so this only grows, and it
    // grows only when the free list cannot satisfy an allocation -- i.e. it is
    // exactly "peak IR held live at once", independent of host load and of
    // anything else in the address space. Retaining a compiled unit's IR makes
    // it track total IR ever built instead.
    std::atomic<unsigned long long> ir_arena_bytes{0};
};

struct PerfCounter2 {
    std::atomic<unsigned long long> ns{0};
    std::atomic<unsigned long long> calls{0};
};

enum class PerfLoweringPart2 : unsigned char {
    Address,
    Memory,
    Flags,
    RegValue,
    Count,
};

struct PerfLoweringBucket2 {
    std::atomic<unsigned long long> calls{0};
    std::atomic<unsigned long long> total_ns{0};
    std::atomic<unsigned long long> append_ns{0};
    std::atomic<unsigned long long> dispatch_ns{0};
    std::atomic<unsigned long long> boundaries{0};
    std::array<std::atomic<unsigned long long>,
               static_cast<size_t>(PerfLoweringPart2::Count)>
            part_ns{};
    std::array<std::atomic<unsigned long long>,
               static_cast<size_t>(PerfLoweringPart2::Count)>
            part_append_ns{};
};

// Measure-first W1 probe. This is deliberately separate from PerfStats:
// SVM_PROF keeps its stable output/fingerprint contract, while SVM_PROF2 opts
// into the more intrusive fine-grained clocks below.
struct PerfStats2 {
    PerfCounter2 translate_total;
    PerfCounter2 decode_total;       // X64Decoder::Decode, including IR append
    PerfCounter2 ir_append;          // central HIR/Block instruction append
    PerfCounter2 ir_setup;           // builder/function/block setup
    PerfCounter2 ir_finalize;        // EndFunction / decode-side fixups
    // Default-off W6 attribution probes. SVM_IR_DETAIL=1 enables them; the
    // normal SVM_PROF2 path deliberately does not pay their per-instruction
    // clocks and atomics.
    PerfCounter2 ir_alloc;
    PerfCounter2 ir_args;
    PerfCounter2 ir_link;
    PerfCounter2 ir_value;
    PerfCounter2 ir_use;
    PerfCounter2 ir_finish_blocks;

    PerfCounter2 pass_total;
    PerfCounter2 pass_uniform;
    // UniformElimination split and corpus-shape probes. They are observed only
    // with SVM_PROF2, so the production/default path pays no clocks or atomics.
    PerfCounter2 uniform_forward;
    PerfCounter2 uniform_dse;
    PerfCounter2 pass_flags;
    PerfCounter2 pass_const;
    PerfCounter2 pass_dce;
    PerfCounter2 reid_block;

    PerfCounter2 compute_rpo;
    PerfCounter2 id_rpo_pre;
    PerfCounter2 id_rpo_post;
    PerfCounter2 regalloc_total;
    PerfCounter2 collect_live;
    PerfCounter2 regalloc_live_scan;
    PerfCounter2 regalloc_live_values;
    PerfCounter2 regalloc_sort;
    PerfCounter2 regalloc_assign;
    PerfCounter2 regalloc_verify;

    PerfCounter2 codegen_total;
    PerfCounter2 codegen_prologue;
    PerfCounter2 codegen_body;
    PerfCounter2 codegen_terminal;

    PerfCounter2 publish_total;
    PerfCounter2 publish_prepare;
    PerfCounter2 publish_lookup;
    PerfCounter2 publish_alloc;
    PerfCounter2 publish_flush;
    PerfCounter2 publish_module;
    PerfCounter2 publish_fault;
    PerfCounter2 publish_l2;
    PerfCounter2 publish_smc;
    PerfCounter2 publish_disk;

    PerfCounter2 ir_free;
    PerfCounter2 cache_load;
    PerfCounter2 cache_revive;

    // W7 decode-front-end attribution. SVM_DECODE_PROF=1 enables these in
    // addition to SVM_PROF2. They are translation-only: no generated-code
    // execution path consults or updates them.
    PerfCounter2 decode_fetch;
    PerfCounter2 decode_instruction_total;
    PerfCounter2 decode_predispatch_inclusive;
    PerfCounter2 decode_raw;
    PerfCounter2 decode_distorm;
    PerfCounter2 decode_lowering;
    PerfCounter2 decode_bookkeeping;
    PerfCounter2 decode_raw_ir_append;
    PerfCounter2 decode_lowering_ir_append;
    PerfCounter2 decode_bookkeeping_ir_append;
    PerfCounter2 decode_vex;
    PerfCounter2 decode_vex_core;
    PerfCounter2 decode_vex_ir_append;
    PerfCounter2 decode_empty;
    std::atomic<unsigned long long> decode_empty_wall_ns{0};
    std::atomic<unsigned long long> decode_attempts{0};
    std::atomic<unsigned long long> decode_fetch_getpointer_calls{0};
    std::atomic<unsigned long long> decode_fetch_bounce_calls{0};
    std::atomic<unsigned long long> decode_fetch_short_windows{0};
    std::atomic<unsigned long long> decode_page_validation_calls{0};
    std::atomic<unsigned long long> decode_raw_accepted{0};
    std::atomic<unsigned long long> decode_vex_accepted{0};
    std::atomic<unsigned long long> decode_distorm_instructions{0};
    static constexpr size_t kDecodeOpcodeSlots = 0x3000;
    std::array<std::atomic<unsigned long long>, kDecodeOpcodeSlots> decode_opcode_ns{};
    std::array<std::atomic<unsigned long long>, kDecodeOpcodeSlots> decode_opcode_calls{};

    // W8 lowering attribution. SVM_LOW_PROF=1 additionally records one local
    // (non-atomic) sample per translated instruction, then aggregates here.
    // The four explicitly scoped parts are inclusive of central IR append;
    // part_append_ns records that nested append time so reports can retain
    // W7's "lowering excludes ir_append" accounting exactly.
    static constexpr size_t kLoweringKinds = 2;  // 0 legacy, 1 raw-VEX AVX/BMI
    static constexpr size_t kLoweringGroups = 6;  // op-count 0/1/2+ x no-mem/mem
    std::array<PerfLoweringBucket2, kLoweringKinds> lowering_kind{};
    std::array<PerfLoweringBucket2, kLoweringGroups> lowering_group{};
    std::array<PerfLoweringBucket2, kDecodeOpcodeSlots> lowering_opcode{};
    std::atomic<unsigned long long> lowering_empty_wall_ns{0};
    std::atomic<unsigned long long> lowering_empty_recorded_ns{0};
    std::atomic<unsigned long long> lowering_empty_calls{0};

    PerfCounter2 decode_advance_pc;
    PerfCounter2 decode_end_commit;

    PerfCounter2 translate_single;
    PerfCounter2 translate_multi;
    PerfCounter2 fixed_single;
    PerfCounter2 fixed_multi;
    std::atomic<unsigned long long> single_units{0};
    std::atomic<unsigned long long> multi_units{0};
    std::atomic<unsigned long long> single_blocks{0};
    std::atomic<unsigned long long> multi_blocks{0};

    std::atomic<unsigned long long> coarse_scope_calls{0};
    std::atomic<unsigned long long> translate_probe_calls{0};

    std::atomic<unsigned long long> uniform_blocks{0};
    std::atomic<unsigned long long> uniform_no_ops_blocks{0};
    std::atomic<unsigned long long> uniform_insts{0};
    std::atomic<unsigned long long> uniform_loads{0};
    std::atomic<unsigned long long> uniform_stores{0};
    std::atomic<unsigned long long> uniform_barriers{0};
    std::atomic<unsigned long long> uniform_invalidations{0};
    std::atomic<unsigned long long> uniform_full_invalidations{0};
    std::atomic<unsigned long long> uniform_range_invalidations{0};
    std::atomic<unsigned long long> uniform_preserved_facts{0};
    std::atomic<unsigned long long> uniform_probe_insts{0};
    std::atomic<unsigned long long> uniform_probe_hits{0};
    std::atomic<unsigned long long> uniform_dse_blocks{0};
    std::atomic<unsigned long long> uniform_dse_victims{0};

    static constexpr std::array<const char*, 60> kGetenvNames{{
            "SVM_MEM_IDENTITY",
            "SVM_FUNC_LAZY",
            "SVM_DUMP_IR",
            "SVM_X87_TOPVIRT",
            "SVM_X87_JIT",
            "SVM_FUNC_IR_FREE",
            "SVM_ADVPC_COALESCE",
            "SVM_CONST_CSE",
            "SVM_UNIFORM_DSE",
            "SVM_FLAG_CARRY_ELIM",
            "SVM_RA_1BLK",
            "SVM_UNIFORM_FAST",
            "SVM_IR_UNIFORM_RANGE",
            "SVM_IR_FAST",
            "SVM_FLAG_NARROW",
            "SVM_AVX",
            "SVM_BMI",
            "SVM_SSE4",
            "SVM_SSE42STR",
            "SVM_SSE42_STRING_INLINE",
            "SVM_SSE_SCALAR_INSERT",
            "SVM_XSAVE",
            "SVM_XSAVE_YMM",
            "SVM_X86_64_ABI_BASELINE",
            "SVM_ADX",
            "SVM_PKRU",
            "SVM_FSGSBASE",
            "SVM_X87_JIT_STATS",
            "SVM_TSO_STATS",
            "SVM_SSE_SCALAR_V_OPERANDS",
            "SVM_MEM_NARROW_FUSE",
            "SVM_SHIFT_IMM_FAST",
            "SVM_XMM_SSA_FWD2",
            "SVM_VEC_IMM_SHIFT",
            "SVM_VEC_CONST_CACHE",
            "SVM_VEC_BYTESHIFT_EXT",
            "SVM_VEC_SHUFPS_NEON",
            "SVM_AES_ZERO_REUSE",
            "SVM_SSE_NAN_COLDPATH",
            "SVM_FLAGS_NARROW_ALIGN",
            "SVM_FLAGS_CFINV",
            "SVM_FLAGS_TERMINAL_JCC",
            "SVM_FLAGS_FCMP_FUSE",
            "SVM_FLAGS_FCMP_COMPACT",
            "SVM_FLAGS_BRANCH_ONLY",
            "SVM_ADDRMODE_STRUCT",
            "SVM_JIT_CACHE_EXEC_ID",
            "SVM_SMC_DIRTY_HINT",
            "SVM_JIT_SCRATCH_XPOOL",
            "SVM_X86_HELPER_VALUES",
            "SVM_X86_PIN_EXT",
            "SVM_XMM_FAULT_SINK",
            "SVM_RA_DIAG",
            "SVM_RA_SHAPE_PROF",
            "SVM_RA_HOT_COALESCE",
            "SVM_HELPER_LEAF_ABI",
            "SVM_XMM_POOL_EXT",
            "SVM_BACKEDGE_LATCH",
            "SVM_BACKEDGE_FLAGS",
            "SVM_RA_INTWIDTH_TIE",
    }};
    std::array<std::atomic<unsigned long long>, kGetenvNames.size()> getenv_ns{};
    std::array<std::atomic<unsigned long long>, kGetenvNames.size()> getenv_calls{};
};

inline PerfStats& GetPerfStats() {
    static PerfStats stats{};
    return stats;
}

inline PerfStats2& GetPerfStats2() {
    static PerfStats2 stats{};
    return stats;
}

void PerfDumpAtExit();

inline void PerfRegisterDumpAtExit() {
    static const bool registered = [] {
        std::atexit(PerfDumpAtExit);
        return true;
    }();
    (void)registered;
}

inline bool Perf2Enabled() {
    static const bool enabled = [] {
        const bool on = std::getenv("SVM_PROF2") != nullptr;
        if (on) {
            PerfRegisterDumpAtExit();
        }
        return on;
    }();
    return enabled;
}

inline thread_local bool perf2_translation_active{};
inline thread_local unsigned perf2_unit_blocks{};

enum class PerfDecodePath2 : unsigned char {
    None,
    Raw,
    Vex,
    Lowering,
    Bookkeeping,
};

inline thread_local PerfDecodePath2 perf2_decode_path{};

inline bool PerfEnabled() {
    static const bool enabled = [] {
        if (std::getenv("SVM_PROF") == nullptr && !Perf2Enabled()) {
            return false;
        }
        PerfRegisterDumpAtExit();
        return true;
    }();
    return enabled;
}

inline void PerfDumpAtExit() {
    auto& s = GetPerfStats();
    auto g = [](const std::atomic<unsigned long long>& a) { return a.load(std::memory_order_relaxed); };
    std::fprintf(stderr,
                 "[svm-prof] translate_ns=%llu decode_ns=%llu rpo_ns=%llu "
                 "opt_ns=%llu regalloc_ns=%llu codegen_ns=%llu publish_ns=%llu "
                 "ir_free_ns=%llu\n",
                 g(s.translate_ns), g(s.decode_ns), g(s.rpo_ns), g(s.opt_ns),
                 g(s.regalloc_ns), g(s.codegen_ns), g(s.publish_ns), g(s.ir_free_ns));
    std::fprintf(stderr,
                 "[svm-prof] func_units=%llu block_units=%llu decoded_blocks=%llu "
                 "host_bytes=%llu ir_insts=%llu pool_bytes=%llu\n",
                 g(s.func_units), g(s.block_units), g(s.decoded_blocks), g(s.host_bytes),
                 g(s.ir_insts), g(s.pool_bytes));
    // Separate line on purpose: run_func_fingerprint_tests.sh keys its totals
    // gate on a line that *starts* with "func_units=", so anything appended to
    // the line above would have to be stripped there as well.
    std::fprintf(stderr, "[svm-prof] ir_arena_bytes=%llu\n", g(s.ir_arena_bytes));

    if (!Perf2Enabled()) {
        return;
    }
    auto& d = GetPerfStats2();
#define PERF2_DUMP(name)                                                                            \
    std::fprintf(stderr, "[svm-prof2] " #name "_ns=%llu " #name "_calls=%llu\n",                  \
                 g(d.name.ns), g(d.name.calls))
    PERF2_DUMP(translate_total);
    PERF2_DUMP(decode_total);
    PERF2_DUMP(ir_append);
    PERF2_DUMP(ir_setup);
    PERF2_DUMP(ir_finalize);
    PERF2_DUMP(ir_alloc);
    PERF2_DUMP(ir_args);
    PERF2_DUMP(ir_link);
    PERF2_DUMP(ir_value);
    PERF2_DUMP(ir_use);
    PERF2_DUMP(ir_finish_blocks);
    PERF2_DUMP(pass_total);
    PERF2_DUMP(pass_uniform);
    PERF2_DUMP(uniform_forward);
    PERF2_DUMP(uniform_dse);
    PERF2_DUMP(pass_flags);
    PERF2_DUMP(pass_const);
    PERF2_DUMP(pass_dce);
    PERF2_DUMP(reid_block);
    PERF2_DUMP(compute_rpo);
    PERF2_DUMP(id_rpo_pre);
    PERF2_DUMP(id_rpo_post);
    PERF2_DUMP(regalloc_total);
    PERF2_DUMP(collect_live);
    PERF2_DUMP(regalloc_live_scan);
    PERF2_DUMP(regalloc_live_values);
    PERF2_DUMP(regalloc_sort);
    PERF2_DUMP(regalloc_assign);
    PERF2_DUMP(regalloc_verify);
    PERF2_DUMP(codegen_total);
    PERF2_DUMP(codegen_prologue);
    PERF2_DUMP(codegen_body);
    PERF2_DUMP(codegen_terminal);
    PERF2_DUMP(publish_total);
    PERF2_DUMP(publish_prepare);
    PERF2_DUMP(publish_lookup);
    PERF2_DUMP(publish_alloc);
    PERF2_DUMP(publish_flush);
    PERF2_DUMP(publish_module);
    PERF2_DUMP(publish_fault);
    PERF2_DUMP(publish_l2);
    PERF2_DUMP(publish_smc);
    PERF2_DUMP(publish_disk);
    PERF2_DUMP(ir_free);
    PERF2_DUMP(cache_load);
    PERF2_DUMP(cache_revive);
    PERF2_DUMP(decode_fetch);
    PERF2_DUMP(decode_instruction_total);
    PERF2_DUMP(decode_predispatch_inclusive);
    PERF2_DUMP(decode_raw);
    PERF2_DUMP(decode_distorm);
    PERF2_DUMP(decode_lowering);
    PERF2_DUMP(decode_bookkeeping);
    PERF2_DUMP(decode_raw_ir_append);
    PERF2_DUMP(decode_lowering_ir_append);
    PERF2_DUMP(decode_bookkeeping_ir_append);
    PERF2_DUMP(decode_vex);
    PERF2_DUMP(decode_vex_core);
    PERF2_DUMP(decode_vex_ir_append);
    PERF2_DUMP(decode_empty);
    PERF2_DUMP(decode_advance_pc);
    PERF2_DUMP(decode_end_commit);
    PERF2_DUMP(translate_single);
    PERF2_DUMP(translate_multi);
    PERF2_DUMP(fixed_single);
    PERF2_DUMP(fixed_multi);
#undef PERF2_DUMP
    std::fprintf(stderr,
                 "[svm-prof2] single_units=%llu multi_units=%llu "
                 "single_blocks=%llu multi_blocks=%llu coarse_scope_calls=%llu "
                 "translate_probe_calls=%llu\n",
                 g(d.single_units), g(d.multi_units), g(d.single_blocks), g(d.multi_blocks),
                 g(d.coarse_scope_calls), g(d.translate_probe_calls));
    std::fprintf(stderr,
                 "[svm-decode] attempts=%llu fetch_getpointer_calls=%llu "
                 "fetch_bounce_calls=%llu fetch_short_windows=%llu "
                 "page_validation_calls=%llu raw_accepted=%llu vex_accepted=%llu "
                 "distorm_instructions=%llu empty_wall_ns=%llu\n",
                 g(d.decode_attempts), g(d.decode_fetch_getpointer_calls),
                 g(d.decode_fetch_bounce_calls), g(d.decode_fetch_short_windows),
                 g(d.decode_page_validation_calls), g(d.decode_raw_accepted),
                 g(d.decode_vex_accepted), g(d.decode_distorm_instructions),
                 g(d.decode_empty_wall_ns));
    for (size_t i = 0; i < d.kDecodeOpcodeSlots; ++i) {
        const auto calls = g(d.decode_opcode_calls[i]);
        if (calls) {
            std::fprintf(stderr, "[svm-decode-op] opcode=%zu calls=%llu ns=%llu\n",
                         i, calls, g(d.decode_opcode_ns[i]));
        }
    }
    static constexpr std::array<const char*, 4> kLowerPartNames{
            "address", "memory", "flags", "regvalue"};
    auto dump_lower = [&](const char* tag, size_t id, const PerfLoweringBucket2& b) {
        const auto calls = g(b.calls);
        if (!calls) return;
        std::fprintf(stderr,
                     "[svm-lower-%s] id=%zu calls=%llu total_ns=%llu append_ns=%llu "
                     "dispatch_ns=%llu boundaries=%llu",
                     tag, id, calls, g(b.total_ns), g(b.append_ns), g(b.dispatch_ns),
                     g(b.boundaries));
        for (size_t p = 0; p < kLowerPartNames.size(); ++p) {
            std::fprintf(stderr, " %s_ns=%llu %s_append_ns=%llu",
                         kLowerPartNames[p], g(b.part_ns[p]),
                         kLowerPartNames[p], g(b.part_append_ns[p]));
        }
        std::fputc('\n', stderr);
    };
    for (size_t i = 0; i < d.kLoweringKinds; ++i) {
        dump_lower("kind", i, d.lowering_kind[i]);
    }
    for (size_t i = 0; i < d.kLoweringGroups; ++i) {
        dump_lower("group", i, d.lowering_group[i]);
    }
    for (size_t i = 0; i < d.kDecodeOpcodeSlots; ++i) {
        if (g(d.lowering_opcode[i].calls)) {
            dump_lower("op", i, d.lowering_opcode[i]);
        }
    }
    std::fprintf(stderr,
                 "[svm-lower-empty] calls=%llu wall_ns=%llu recorded_ns=%llu\n",
                 g(d.lowering_empty_calls), g(d.lowering_empty_wall_ns),
                 g(d.lowering_empty_recorded_ns));
    std::fprintf(stderr,
                 "[svm-uniform] blocks=%llu no_ops=%llu insts=%llu loads=%llu "
                 "stores=%llu barriers=%llu invalidations=%llu "
                 "full_invalidations=%llu range_invalidations=%llu "
                 "preserved_facts=%llu probe_insts=%llu probe_hits=%llu dse_blocks=%llu "
                 "dse_victims=%llu\n",
                 g(d.uniform_blocks), g(d.uniform_no_ops_blocks), g(d.uniform_insts),
                 g(d.uniform_loads), g(d.uniform_stores), g(d.uniform_barriers),
                 g(d.uniform_invalidations),
                 g(d.uniform_full_invalidations), g(d.uniform_range_invalidations),
                 g(d.uniform_preserved_facts), g(d.uniform_probe_insts),
                 g(d.uniform_probe_hits),
                 g(d.uniform_dse_blocks),
                 g(d.uniform_dse_victims));
    for (size_t i = 0; i < d.kGetenvNames.size(); ++i) {
        const auto calls = g(d.getenv_calls[i]);
        if (calls) {
            std::fprintf(stderr, "[svm-getenv] name=%s calls=%llu ns=%llu\n",
                         d.kGetenvNames[i], calls, g(d.getenv_ns[i]));
        }
    }
}

inline void PerfAdd(std::atomic<unsigned long long>& counter, unsigned long long v) {
    counter.fetch_add(v, std::memory_order_relaxed);
}

inline bool PerfIRDetailEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SVM_IR_DETAIL");
        return env && std::strcmp(env, "0") != 0;
    }();
    return enabled;
}

inline bool PerfDecodeDetailEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SVM_DECODE_PROF");
        return Perf2Enabled() && env && std::strcmp(env, "0") != 0;
    }();
    return enabled;
}

inline bool PerfLoweringDetailEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("SVM_LOW_PROF");
        return PerfDecodeDetailEnabled() && env && std::strcmp(env, "0") != 0;
    }();
    return enabled;
}

struct PerfLoweringLocal2 {
    bool active{};
    bool avx{};
    bool first_boundary{};
    unsigned opcode{};
    unsigned group{};
    unsigned depth{};
    PerfLoweringPart2 part{PerfLoweringPart2::Address};
    std::chrono::steady_clock::time_point start{};
    std::chrono::steady_clock::time_point part_start{};
    unsigned long long append_ns{};
    unsigned long long dispatch_ns{};
    unsigned long long boundaries{};
    std::array<unsigned long long, static_cast<size_t>(PerfLoweringPart2::Count)> part_ns{};
    std::array<unsigned long long, static_cast<size_t>(PerfLoweringPart2::Count)>
            part_append_ns{};
};

inline thread_local PerfLoweringLocal2 perf2_lowering_local{};

inline unsigned PerfLoweringGroup(unsigned operand_count, bool has_memory) {
    const unsigned count_class = operand_count == 0 ? 0 : operand_count == 1 ? 1 : 2;
    return count_class * 2 + (has_memory ? 1 : 0);
}

inline void PerfLoweringBegin(unsigned opcode, unsigned operand_count, bool has_memory,
                              bool avx = false) {
    if (!PerfLoweringDetailEnabled()) return;
    auto& l = perf2_lowering_local;
    l = {};
    l.active = true;
    l.avx = avx;
    l.opcode = opcode;
    l.group = PerfLoweringGroup(operand_count, has_memory);
    l.start = std::chrono::steady_clock::now();
}

inline void PerfLoweringFirstBoundary(std::chrono::steady_clock::time_point when) {
    auto& l = perf2_lowering_local;
    if (!l.active || l.first_boundary) return;
    l.first_boundary = true;
    l.dispatch_ns = static_cast<unsigned long long>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(when - l.start).count());
}

inline void PerfLoweringHandlerBegin() {
    if (perf2_lowering_local.active && !perf2_lowering_local.first_boundary) {
        PerfLoweringFirstBoundary(std::chrono::steady_clock::now());
    }
}

inline void PerfLoweringRecordAppend(unsigned long long ns) {
    auto& l = perf2_lowering_local;
    if (!l.active) return;
    l.append_ns += ns;
    if (l.depth != 0) {
        l.part_append_ns[static_cast<size_t>(l.part)] += ns;
    }
}

inline void PerfLoweringAccumulate(PerfLoweringBucket2& b,
                                   const PerfLoweringLocal2& l,
                                   unsigned long long total_ns) {
    b.calls.fetch_add(1, std::memory_order_relaxed);
    b.total_ns.fetch_add(total_ns, std::memory_order_relaxed);
    b.append_ns.fetch_add(l.append_ns, std::memory_order_relaxed);
    b.dispatch_ns.fetch_add(l.dispatch_ns, std::memory_order_relaxed);
    b.boundaries.fetch_add(l.boundaries, std::memory_order_relaxed);
    for (size_t p = 0; p < static_cast<size_t>(PerfLoweringPart2::Count); ++p) {
        b.part_ns[p].fetch_add(l.part_ns[p], std::memory_order_relaxed);
        b.part_append_ns[p].fetch_add(l.part_append_ns[p], std::memory_order_relaxed);
    }
}

inline void PerfLoweringFinish(unsigned long long total_ns, bool accepted = true) {
    auto& l = perf2_lowering_local;
    if (!l.active) return;
    if (!l.first_boundary) {
        PerfLoweringFirstBoundary(std::chrono::steady_clock::now());
    }
    if (accepted) {
        auto& s = GetPerfStats2();
        PerfLoweringAccumulate(s.lowering_kind[l.avx ? 1 : 0], l, total_ns);
        if (!l.avx) {
            PerfLoweringAccumulate(s.lowering_group[l.group], l, total_ns);
            if (l.opcode < s.kDecodeOpcodeSlots) {
                PerfLoweringAccumulate(s.lowering_opcode[l.opcode], l, total_ns);
            }
        }
    }
    l.active = false;
}

class PerfLoweringPartScope2 {
public:
    explicit PerfLoweringPartScope2(PerfLoweringPart2 part) {
        auto& l = perf2_lowering_local;
        if (!l.active) return;
        if (l.depth++ != 0) return;
        outer = true;
        l.part = part;
        ++l.boundaries;
        const auto now = std::chrono::steady_clock::now();
        PerfLoweringFirstBoundary(now);
        l.part_start = now;
    }

    ~PerfLoweringPartScope2() {
        auto& l = perf2_lowering_local;
        if (!l.active) return;
        if (!outer) {
            --l.depth;
            return;
        }
        const auto end = std::chrono::steady_clock::now();
        l.part_ns[static_cast<size_t>(l.part)] += static_cast<unsigned long long>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - l.part_start).count());
        --l.depth;
    }

    PerfLoweringPartScope2(const PerfLoweringPartScope2&) = delete;
    PerfLoweringPartScope2& operator=(const PerfLoweringPartScope2&) = delete;

private:
    bool outer{};
};

class PerfDecodePathScope2 {
public:
    explicit PerfDecodePathScope2(PerfDecodePath2 path)
            : prior(perf2_decode_path), active(PerfDecodeDetailEnabled()) {
        if (active) {
            perf2_decode_path = path;
        }
    }
    ~PerfDecodePathScope2() {
        if (active) {
            perf2_decode_path = prior;
        }
    }

    PerfDecodePathScope2(const PerfDecodePathScope2&) = delete;
    PerfDecodePathScope2& operator=(const PerfDecodePathScope2&) = delete;

private:
    PerfDecodePath2 prior{};
    bool active{};
};

inline void PerfIRDetailRecord(PerfCounter2& counter,
                               std::chrono::steady_clock::time_point begin,
                               std::chrono::steady_clock::time_point end) {
    const auto ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
    counter.calls.fetch_add(1, std::memory_order_relaxed);
    counter.ns.fetch_add(static_cast<unsigned long long>(ns), std::memory_order_relaxed);
}

// SVM_PROF=2 additionally prints one line per compiled unit, which is what
// makes a codegen difference attributable: diff the two runs' unit lists and
// the changed guest addresses fall out.
inline bool PerfPerUnit() {
    static const bool on = [] {
        const char* e = std::getenv("SVM_PROF");
        return e && std::atoi(e) >= 2;
    }();
    return on;
}

// Adds the lifetime of the enclosing scope to `counter`. Construction cost is
// one steady_clock::now(); only instantiate on per-unit paths.
class PerfScope {
public:
    explicit PerfScope(std::atomic<unsigned long long>& counter)
            : counter(PerfEnabled() ? &counter : nullptr) {
        if (this->counter) {
            if (Perf2Enabled()) {
                GetPerfStats2().coarse_scope_calls.fetch_add(1, std::memory_order_relaxed);
                if (perf2_translation_active) {
                    GetPerfStats2().translate_probe_calls.fetch_add(1,
                                                                    std::memory_order_relaxed);
                }
            }
            start = std::chrono::steady_clock::now();
        }
    }
    ~PerfScope() { Stop(); }

    void Stop() {
        if (!counter) {
            return;
        }
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();
        counter->fetch_add(static_cast<unsigned long long>(ns), std::memory_order_relaxed);
        counter = nullptr;
    }

    PerfScope(const PerfScope&) = delete;
    PerfScope& operator=(const PerfScope&) = delete;

private:
    std::atomic<unsigned long long>* counter;
    std::chrono::steady_clock::time_point start{};
};

class PerfScope2 {
public:
    explicit PerfScope2(PerfCounter2& counter)
            : counter(Perf2Enabled() ? &counter : nullptr) {
        if (this->counter) {
            this->counter->calls.fetch_add(1, std::memory_order_relaxed);
            if (perf2_translation_active) {
                GetPerfStats2().translate_probe_calls.fetch_add(1, std::memory_order_relaxed);
            }
            start = std::chrono::steady_clock::now();
            if (PerfLoweringDetailEnabled() &&
                this->counter == &GetPerfStats2().ir_append &&
                perf2_lowering_local.active) {
                lowering_append = true;
                PerfLoweringFirstBoundary(start);
            }
        }
    }
    ~PerfScope2() { Stop(); }

    unsigned long long Stop() {
        if (!counter) {
            return 0;
        }
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();
        counter->ns.fetch_add(static_cast<unsigned long long>(ns), std::memory_order_relaxed);
        if (lowering_append) {
            PerfLoweringRecordAppend(static_cast<unsigned long long>(ns));
        }
        if (PerfDecodeDetailEnabled() && counter == &GetPerfStats2().ir_append) {
            auto& s = GetPerfStats2();
            auto add = [ns](PerfCounter2& out) {
                out.calls.fetch_add(1, std::memory_order_relaxed);
                out.ns.fetch_add(static_cast<unsigned long long>(ns),
                                 std::memory_order_relaxed);
            };
            switch (perf2_decode_path) {
                case PerfDecodePath2::Raw:
                    add(s.decode_raw_ir_append);
                    break;
                case PerfDecodePath2::Vex:
                    add(s.decode_raw_ir_append);
                    add(s.decode_vex_ir_append);
                    break;
                case PerfDecodePath2::Lowering:
                    add(s.decode_lowering_ir_append);
                    break;
                case PerfDecodePath2::Bookkeeping:
                    add(s.decode_bookkeeping_ir_append);
                    break;
                case PerfDecodePath2::None:
                    break;
            }
        }
        counter = nullptr;
        return static_cast<unsigned long long>(ns);
    }

    PerfScope2(const PerfScope2&) = delete;
    PerfScope2& operator=(const PerfScope2&) = delete;

private:
    PerfCounter2* counter;
    std::chrono::steady_clock::time_point start{};
    bool lowering_append{};
};

// Detail-only counterpart: unlike PerfScope2, SVM_PROF2 by itself does not
// activate it. The optional path lets central IR-append scopes attribute their
// nested time without changing the append sites.
class PerfDecodeScope2 {
public:
    explicit PerfDecodeScope2(PerfCounter2& counter,
                              PerfDecodePath2 path = PerfDecodePath2::None)
            : counter(PerfDecodeDetailEnabled() ? &counter : nullptr),
              prior(perf2_decode_path) {
        if (this->counter) {
            this->counter->calls.fetch_add(1, std::memory_order_relaxed);
            perf2_decode_path = path;
            start = std::chrono::steady_clock::now();
        }
    }
    ~PerfDecodeScope2() { Stop(); }

    unsigned long long Stop() {
        if (!counter) return 0;
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();
        counter->ns.fetch_add(static_cast<unsigned long long>(ns), std::memory_order_relaxed);
        counter = nullptr;
        perf2_decode_path = prior;
        return static_cast<unsigned long long>(ns);
    }

    PerfDecodeScope2(const PerfDecodeScope2&) = delete;
    PerfDecodeScope2& operator=(const PerfDecodeScope2&) = delete;

private:
    PerfCounter2* counter{};
    PerfDecodePath2 prior{};
    std::chrono::steady_clock::time_point start{};
};

inline void PerfDecodeRecordOpcode(unsigned opcode, unsigned long long ns) {
    if (!PerfDecodeDetailEnabled()) return;
    auto& s = GetPerfStats2();
    s.decode_distorm_instructions.fetch_add(1, std::memory_order_relaxed);
    if (opcode >= s.kDecodeOpcodeSlots) return;
    s.decode_opcode_calls[opcode].fetch_add(1, std::memory_order_relaxed);
    s.decode_opcode_ns[opcode].fetch_add(ns, std::memory_order_relaxed);
}

inline void PerfDecodeRunEmptyMicrobench() {
    if (!PerfDecodeDetailEnabled()) return;
    static const bool ran = [] {
        const char* env = std::getenv("SVM_DECODE_PROF_EMPTY");
        if (!env || std::strcmp(env, "0") == 0) return true;
        unsigned long long iterations = std::strtoull(env, nullptr, 10);
        if (iterations < 1000) iterations = 1000000;
        auto& s = GetPerfStats2();
        const auto begin = std::chrono::steady_clock::now();
        for (unsigned long long i = 0; i < iterations; ++i) {
            PerfScope2 empty{s.decode_empty};
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - begin)
                                     .count();
        s.decode_empty_wall_ns.fetch_add(static_cast<unsigned long long>(elapsed),
                                         std::memory_order_relaxed);
        return true;
    }();
    (void)ran;
}

inline void PerfLoweringRunEmptyMicrobench() {
    if (!PerfLoweringDetailEnabled()) return;
    static const bool ran = [] {
        const char* env = std::getenv("SVM_LOW_PROF_EMPTY");
        if (!env || std::strcmp(env, "0") == 0) return true;
        unsigned long long iterations = std::strtoull(env, nullptr, 10);
        if (iterations < 1000) iterations = 1000000;
        auto& l = perf2_lowering_local;
        l = {};
        l.active = true;
        const auto begin = std::chrono::steady_clock::now();
        for (unsigned long long i = 0; i < iterations; ++i) {
            PerfLoweringPartScope2 empty{PerfLoweringPart2::Address};
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - begin)
                                     .count();
        auto& s = GetPerfStats2();
        s.lowering_empty_calls.fetch_add(iterations, std::memory_order_relaxed);
        s.lowering_empty_wall_ns.fetch_add(static_cast<unsigned long long>(elapsed),
                                           std::memory_order_relaxed);
        s.lowering_empty_recorded_ns.fetch_add(
                l.part_ns[static_cast<size_t>(PerfLoweringPart2::Address)],
                std::memory_order_relaxed);
        l.active = false;
        return true;
    }();
    (void)ran;
}

class PerfTranslationScope2 {
public:
    PerfTranslationScope2()
            : scope(GetPerfStats2().translate_total), prior(perf2_translation_active) {
        if (Perf2Enabled()) {
            perf2_translation_active = true;
            GetPerfStats2().translate_probe_calls.fetch_add(1, std::memory_order_relaxed);
        }
    }
    ~PerfTranslationScope2() {
        if (!Perf2Enabled()) {
            return;
        }
        const auto ns = scope.Stop();
        auto& s = GetPerfStats2();
        if (perf2_unit_blocks == 1) {
            s.translate_single.ns.fetch_add(ns, std::memory_order_relaxed);
            s.translate_single.calls.fetch_add(1, std::memory_order_relaxed);
        } else if (perf2_unit_blocks > 1) {
            s.translate_multi.ns.fetch_add(ns, std::memory_order_relaxed);
            s.translate_multi.calls.fetch_add(1, std::memory_order_relaxed);
        }
        perf2_unit_blocks = 0;
        perf2_translation_active = prior;
    }

    void Classify(unsigned blocks) {
        if (Perf2Enabled()) {
            perf2_unit_blocks = blocks;
            auto& s = GetPerfStats2();
            if (blocks == 1) {
                s.single_units.fetch_add(1, std::memory_order_relaxed);
                s.single_blocks.fetch_add(1, std::memory_order_relaxed);
            } else if (blocks > 1) {
                s.multi_units.fetch_add(1, std::memory_order_relaxed);
                s.multi_blocks.fetch_add(blocks, std::memory_order_relaxed);
            }
        }
    }

private:
    PerfScope2 scope;
    bool prior{};
};

struct PerfFixedSnapshot2 {
    unsigned long long compute{};
    unsigned long long id_pre{};
    unsigned long long id_post{};
    unsigned long long regalloc{};

    PerfFixedSnapshot2() {
        if (!Perf2Enabled()) return;
        auto& s = GetPerfStats2();
        compute = s.compute_rpo.ns.load(std::memory_order_relaxed);
        id_pre = s.id_rpo_pre.ns.load(std::memory_order_relaxed);
        id_post = s.id_rpo_post.ns.load(std::memory_order_relaxed);
        regalloc = s.regalloc_total.ns.load(std::memory_order_relaxed);
    }

    void Record(unsigned blocks) const {
        if (!Perf2Enabled() || blocks == 0) return;
        auto& s = GetPerfStats2();
        const auto ns =
                s.compute_rpo.ns.load(std::memory_order_relaxed) - compute +
                s.id_rpo_pre.ns.load(std::memory_order_relaxed) - id_pre +
                s.id_rpo_post.ns.load(std::memory_order_relaxed) - id_post +
                s.regalloc_total.ns.load(std::memory_order_relaxed) - regalloc;
        auto& out = blocks == 1 ? s.fixed_single : s.fixed_multi;
        out.ns.fetch_add(ns, std::memory_order_relaxed);
        out.calls.fetch_add(1, std::memory_order_relaxed);
    }
};

inline const char* PerfGetenv(const char* name) {
    if (!Perf2Enabled() || !perf2_translation_active) {
        return std::getenv(name);
    }
    auto& s = GetPerfStats2();
    const auto begin = std::chrono::steady_clock::now();
    const char* value = std::getenv(name);
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - begin)
                            .count();
    s.translate_probe_calls.fetch_add(1, std::memory_order_relaxed);
    for (size_t i = 0; i < s.kGetenvNames.size(); ++i) {
        if (std::strcmp(name, s.kGetenvNames[i]) == 0) {
            s.getenv_calls[i].fetch_add(1, std::memory_order_relaxed);
            s.getenv_ns[i].fetch_add(static_cast<unsigned long long>(ns),
                                     std::memory_order_relaxed);
            break;
        }
    }
    return value;
}

}  // namespace swift::runtime
