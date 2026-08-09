#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/types.h"

namespace swift::runtime {

// 纯单 unit codegen 开关。此表与 docs/svm-config-classification.md 的 B 类
// 一一对应；FeatureSet 在翻译入口解析一次，后续路径不得反向读取全局配置。
#define SVM_FEATURE_FIELDS(X) \
    X(advpc_coalesce, true) \
    X(const_cse, true) \
    X(uniform_dse, true) \
    X(flag_carry_elim, true) \
    X(ra_1blk, true) \
    X(uniform_fast, true) \
    X(ir_uniform_range, false) \
    X(flag_narrow, true) \
    X(sse42_string_inline, true) \
    X(sse_scalar_insert, true) \
    X(sse_scalar_tie, true) \
    X(sse_scalar_v_operands, true) \
    X(mem_narrow_fuse, true) \
    X(addr_ea_tie, true) \
    X(abs_const_mat, false) \
    X(shift_imm_fast, true) \
    X(narrow_rotate_compact, true) \
    X(xmm_ssa_fwd2, true) \
    X(xmm_narrow_fwd, true) \
    X(vec_imm_shift, true) \
    X(vec_const_cache, true) \
    X(vec_byteshift_ext, true) \
    X(vec_shufps_neon, true) \
    X(sse_shufps_imm, false) \
    X(aes_zero_reuse, true) \
    X(ra_aes_chain_tie, true) \
    X(pshufd_4e_ext, false) \
    X(sse_nan_coldpath, true) \
    X(flags_narrow_align, true) \
    X(flags_terminal_jcc, true) \
    X(flags_fcmp_fuse, true) \
    X(flags_fcmp_compact, true) \
    X(flags_branch_only, true) \
    X(flags_region_branch, true) \
    X(flags_loop_lazy, false) \
    X(addrmode_struct, true) \
    X(jit_scratch_xpool, true) \
    X(xmm_fault_sink, true) \
    X(xmm_snapshot_dse, true) \
    X(helper_leaf_abi, false) \
    X(ra_intwidth_tie, true) \
    X(sse_afp_nan, true) \
    X(sse_afp_minmax, false) \
    X(mem_hostbase_fold, true) \
    X(induct_tie, true) \
    X(scratch_precise, true) \
    X(fpr_scratch_precise, true) \
    X(fpr_ipv_reclaim, true) \
    X(ra_spill_evict, true) \
    X(ra_coalesce, true) \
    X(ra_width_chain, false) \
    X(ra_width_chain_long, true) \
    X(const_addr_cache, false) \
    X(indirect_l1, true) \
    X(shadow_lean, true) \
    X(flag_full_elim, false) \
    X(gpr_zext_coalesce, true) \
    X(sse_nan_fast, false) \
    X(uniform_elim, true) \
    X(uniform_path_fwd, true) \
    X(vixl_fast, true) \
    X(x86_gcm_pclmul2, true) \
    X(xmm_uniform_fwd, true) \
    X(loop_gpr_hoist, true) \
    X(loop_const_hoist, true)

enum class FeatureId : std::size_t {
#define SVM_FEATURE_ID(field, default_value) field,
    SVM_FEATURE_FIELDS(SVM_FEATURE_ID)
#undef SVM_FEATURE_ID
    Count,
};

inline constexpr std::size_t kFeatureCount =
        static_cast<std::size_t>(FeatureId::Count);

struct FeatureSet {
#define SVM_FEATURE_MEMBER(field, default_value) bool field{default_value};
    SVM_FEATURE_FIELDS(SVM_FEATURE_MEMBER)
#undef SVM_FEATURE_MEMBER

    [[nodiscard]] bool Get(FeatureId id) const;
    void Set(FeatureId id, bool value);
};

[[nodiscard]] std::string_view FeatureName(FeatureId id);
[[nodiscard]] std::optional<FeatureId> FeatureIdFromName(std::string_view name);
// 稳定的字段序列哈希：按 FeatureId 顺序混入 id 和 bool 值，不依赖结构体
// padding，也不依赖编译器对 bool 的布局。
[[nodiscard]] u64 HashFeatureSet(const FeatureSet& features);

struct FeatureOverrides {
    std::array<std::optional<bool>, kFeatureCount> values{};

    [[nodiscard]] bool Empty() const;
    [[nodiscard]] std::optional<bool> Get(FeatureId id) const;
    void Set(FeatureId id, bool value);
};

// 单一环境配置表。parse 一列命名原读取点的精确判定；source_comment 保留迁移依据。
// 新增开关必须只在此表登记，PerfStats2 注册表和 code-cache 环境哈希均由此生成。
#define SVM_CONFIG_FIELDS(X) \
    X(std::string, mem_identity, "SVM_MEM_IDENTITY", RawString, "", "Linux 内存模型三态；缺省 identity，0/OFF/off 选 bounded bias；原 linux/main.cpp:388") \
    X(u64, func_lazy, "SVM_FUNC_LAZY", FuncLazy, 1, "函数 region decode budget；缺省 1，<=0 回到 eager 1024；原 translator/x86/translator.cpp:603") \
    X(bool, dump_ir, "SVM_DUMP_IR", Presence, false, "IR 诊断打印；变量存在即开（含 =0）；原 runtime/backend/runtime.cpp:813 等") \
    X(bool, x87_jit, "SVM_X87_JIT", NonZero, false, "x87 中层 JIT；非 0 开，缺省 OFF；原 decoder_x87.cc:118") \
    X(bool, func_ir_free, "SVM_FUNC_IR_FREE", DefaultOn, true, "函数 IR 发布后释放；缺省 ON，=0 回退；原 runtime.cpp:771") \
    X(bool, advpc_coalesce, "SVM_ADVPC_COALESCE", DefaultOn, true, "AdvancePC 合并；缺省 ON，=0 回退；原 hir_builder.cpp:597") \
    X(bool, const_cse, "SVM_CONST_CSE", DefaultOn, true, "常量 CSE；缺省 ON，=0 回退；原 const_folding_pass.cpp:104") \
    X(bool, uniform_dse, "SVM_UNIFORM_DSE", DefaultOn, true, "uniform dead-store elimination；缺省 ON，=0 回退；原 uniform_elimination_pass.cpp:387") \
    X(bool, flag_carry_elim, "SVM_FLAG_CARRY_ELIM", DefaultOn, true, "carry write elimination；缺省 ON，=0 回退；原 flags_elimination_pass.cpp:484") \
    X(bool, ra_1blk, "SVM_RA_1BLK", DefaultOn, true, "单块 RA 快路径；缺省 ON，=0 回退；原 register_alloc_pass.cpp:2226") \
    X(bool, uniform_fast, "SVM_UNIFORM_FAST", DefaultOn, true, "uniform 快速实现；缺省 ON，=0 回退；原 uniform_elimination_pass.cpp:838") \
    X(bool, ir_uniform_range, "SVM_IR_UNIFORM_RANGE", NonZero, false, "uniform range effect 分析；非 0 开，缺省 OFF；原 uniform_elimination_pass.cpp:23") \
    X(bool, ir_fast, "SVM_IR_FAST", DefaultOn, true, "IR arena 快路径；缺省 ON，=0 回退；原 instr.cpp:33") \
    X(bool, flag_narrow, "SVM_FLAG_NARROW", DefaultOn, true, "窄 flags 消除；缺省 ON，=0 回退；原 flags_elimination_pass.cpp:537") \
    X(bool, avx, "SVM_AVX", NonZero, false, "AVX/VEX 解码；非 0 开，缺省 OFF；原 decoder.cc:1854") \
    X(bool, bmi, "SVM_BMI", NonZero, false, "BMI1/2 解码；非 0 开，缺省 OFF；原 decoder_bmi.cc:248") \
    X(bool, sse4, "SVM_SSE4", DefaultOn, true, "SSE3/SSSE3/SSE4.1；缺省 ON，=0 回退；原 decoder_sse4.cc:1397") \
    X(bool, sse42str, "SVM_SSE42STR", DefaultOn, true, "SSE4.2 string；缺省 ON，=0 回退；原 decoder_sse42str.cc:1345") \
    X(bool, sse42_string_inline, "SVM_SSE42_STRING_INLINE", DefaultOn, true, "SSE4.2 string inline；缺省 ON，=0 回退；原 decoder_internal.h:37") \
    X(bool, sse_scalar_insert, "SVM_SSE_SCALAR_INSERT", DefaultOn, true, "AFP scalar insert 请求；缺省 ON，=0 回退，仍受 host AFP 限制；原 translator/x86/translator.cpp:332") \
    X(bool, sse_scalar_tie, "SVM_SSE_SCALAR_TIE", DefaultOn, true, "scalar SSE 二元结果与已验证末次使用的固定家 left 合并，依赖有效 scalar insert/AFP 契约；缺省 ON，=0 回退；原 register_alloc_pass.cpp") \
    X(bool, xsave, "SVM_XSAVE", NonZero, false, "XSAVE facility；非 0 开，缺省 OFF；原 xsave.h:66") \
    X(bool, xsave_ymm, "SVM_XSAVE_YMM", InheritAvx, false, "XCR0.YMM override；缺省继承 SVM_AVX，非 0 开；原 xsave.h:91") \
    X(bool, x86_64_abi_baseline, "SVM_X86_64_ABI_BASELINE", NonZero, false, "动态 ELF baseline MMX 标记；非 0 开，缺省 OFF；原 decoder_misc.cc:108") \
    X(bool, adx, "SVM_ADX", NonZero, false, "ADX 解码；非 0 开，缺省 OFF；原 decoder_userland_ext.cc:85") \
    X(bool, fsgsbase, "SVM_FSGSBASE", NonZero, false, "FSGSBASE 解码；非 0 开，缺省 OFF；原 decoder_userland_ext.cc:83") \
    X(bool, x87_jit_stats, "SVM_X87_JIT_STATS", Presence, false, "x87 JIT 统计；变量存在即开；原 x87.cpp:35") \
    X(bool, tso_stats, "SVM_TSO_STATS", Presence, false, "TSO 发码统计；变量存在即开；原 translator_mem.cpp:31") \
    X(bool, sse_scalar_v_operands, "SVM_SSE_SCALAR_V_OPERANDS", DefaultOn, true, "scalar SSE V operand；缺省 ON，=0 回退；原 decoder.cc:203") \
    X(bool, mem_narrow_fuse, "SVM_MEM_NARROW_FUSE", DefaultOn, true, "窄内存融合；缺省 ON，=0 回退；原 translator.cpp:320/register_alloc_pass.cpp:69") \
    X(bool, addr_ea_tie, "SVM_ADDR_EA_TIE", DefaultOn, true, "EA tie/composite 保留；缺省 ON，=0 回退；原 decoder.cc:270 等") \
    X(bool, abs_const_mat, "SVM_ABS_CONST_MAT", NonZero, false, "绝对常量物化优化；非 0 开，缺省 OFF；原 translator.cpp:326") \
    X(bool, shift_imm_fast, "SVM_SHIFT_IMM_FAST", DefaultOn, true, "shift immediate 快路；缺省 ON，=0 回退；原 decoder_alu.cc:789 等") \
    X(bool, narrow_rotate_compact, "SVM_NARROW_ROTATE_COMPACT", DefaultOn, true, "U16 立即数 8 rotate 紧凑 lowering；缺省 ON，=0 回退；原 decoder_alu.cc:1335") \
    X(bool, xmm_ssa_fwd2, "SVM_XMM_SSA_FWD2", DefaultOn, true, "XMM load-load SSA 转发；缺省 ON，=0 回退；原 uniform_elimination_pass.cpp:90") \
    X(bool, xmm_narrow_fwd, "SVM_XMM_NARROW_FWD", DefaultOn, true, "XMM 窄视图转发；缺省 ON，=0 回退；原 uniform_elimination_pass.cpp:102") \
    X(bool, xmm_resident, "SVM_XMM_RESIDENT", DefaultOn, true, "XMM1-7 固定驻留 v17-v23，XMM0 保持 State；缺省 ON，=0 回退；跨 unit ABI，原 translator/x86/translator.cpp") \
    X(bool, xmm_resident_hi, "SVM_XMM_RESIDENT_HI", DefaultOn, true, "XMM8-11 固定驻留 v24-v27；缺省 ON，=0 回退，依赖 SVM_XMM_RESIDENT；无害性依赖 SVM_FPR_SCRATCH_PRECISE+SVM_FPR_IPV_RECLAIM 同开（单独关 B 退化为池 17 有税形态）；跨 unit ABI，原 translator/x86/translator.cpp") \
    X(u32, xmm_fpr_pool_cap, "SVM_XMM_FPR_POOL_CAP", NonNegativeAtoi, 0, "只读供给曲线探针：把 FPR value pool 钳到 13..20；0/缺省惰性，原 trampolines.cpp") \
    X(bool, vec_imm_shift, "SVM_VEC_IMM_SHIFT", DefaultOn, true, "vector immediate shift lowering；缺省 ON，=0 回退；原 decoder_sse.cc/decoder_avx_int.cc") \
    X(bool, vec_const_cache, "SVM_VEC_CONST_CACHE", DefaultOn, true, "vector constant cache；缺省 ON，=0 回退；原 decoder_internal.h:47") \
    X(bool, vec_byteshift_ext, "SVM_VEC_BYTESHIFT_EXT", DefaultOn, true, "vector byte-shift lowering；缺省 ON，=0 回退；原 decoder_sse.cc/decoder_avx_int.cc") \
    X(bool, vec_shufps_neon, "SVM_VEC_SHUFPS_NEON", DefaultOn, true, "SHUFPS NEON lowering；缺省 ON，=0 回退；原 decoder_sse.cc:939") \
    X(bool, sse_shufps_imm, "SVM_SSE_SHUFPS_IMM", NonZero, false, "SHUFPS 高频 imm 的末次使用 destination tie；非 0 开，缺省 OFF；原 register_alloc_pass.cpp") \
    X(bool, aes_zero_reuse, "SVM_AES_ZERO_REUSE", DefaultOn, true, "AES shared-zero reuse；缺省 ON，=0 回退；原 decoder_crypto.cc:89") \
    X(bool, ra_aes_chain_tie, "SVM_RA_AES_CHAIN_TIE", DefaultOn, true, "unit 内连续 AESENC destructive producer 与 resident XMM 家 ownership 合并；缺省 ON，=0 回退；原 register_alloc_pass.cpp") \
    X(bool, pshufd_4e_ext, "SVM_PSHUFD_4E_EXT", NonZero, false, "PSHUFD 0x4e canonical indexed shuffle 改发 EXT #8；非 0 开，缺省 OFF；原 translator_alu.cpp") \
    X(bool, sse_nan_coldpath, "SVM_SSE_NAN_COLDPATH", DefaultOn, true, "SSE NaN cold path；缺省 ON，=0 回退；原 translator.cpp:332") \
    X(bool, flags_narrow_align, "SVM_FLAGS_NARROW_ALIGN", DefaultOn, true, "窄 flags 对齐发码；缺省 ON，=0 回退；原 decoder.cc:1150") \
    X(bool, flags_cfinv, "SVM_FLAGS_CFINV", DefaultOn, true, "FlagM CFINV carry 极性；缺省 ON，=0 回退；原 decoder.cc:1155") \
    X(bool, flags_terminal_jcc, "SVM_FLAGS_TERMINAL_JCC", DefaultOn, true, "terminal Jcc；缺省 ON，=0 回退；原 decoder.cc:1163") \
    X(bool, flags_fcmp_fuse, "SVM_FLAGS_FCMP_FUSE", DefaultOn, true, "FCMP flags fuse；缺省 ON，=0 回退；原 decoder.cc:1234") \
    X(bool, flags_fcmp_compact, "SVM_FLAGS_FCMP_COMPACT", DefaultOn, true, "FCMP compact（需 host FlagM2/AXFlag，无则自动回退默认路径）；缺省 ON，=0 回退；原 decoder.cc:265") \
    X(bool, flags_branch_only, "SVM_FLAGS_BRANCH_ONLY", DefaultOn, true, "branch-only flags；缺省 ON，=0 回退；原 flags_elimination_pass.cpp:47/decoder.cc:1170") \
    X(bool, flags_region_branch, "SVM_FLAGS_REGION_BRANCH", DefaultOn, true, "region 内单边 flags-dead 的 terminal branch 延迟提交；缺省 ON，=0 回退；原 translator.cpp:PlanBackedgeFlags") \
    X(bool, flags_loop_lazy, "SVM_FLAGS_LOOP_LAZY", NonZero, false, "单元内单块自环 NZCV/polarity 延迟到 cold/fault 出口物化；非 0 开，缺省 OFF；原 translator.cpp:PlanBackedgeFlags") \
    X(bool, addrmode_struct, "SVM_ADDRMODE_STRUCT", DefaultOn, true, "结构化寻址；缺省 ON，=0 回退；原 translator_mem.cpp:20/decoder.cc:880") \
    X(bool, jit_cache_exec_id, "SVM_JIT_CACHE_EXEC_ID", NonZeroNonEmpty, false, "JIT cache guest-id 使用 argv[1]；非空非 0 开；原 code_serial.cpp:820") \
    X(bool, smc_dirty_hint, "SVM_SMC_DIRTY_HINT", EqualsOne, false, "SMC dirty hint；仅 =1 开，缺省 OFF；原 smc_tracker.cpp:53") \
    X(bool, jit_scratch_xpool, "SVM_JIT_SCRATCH_XPOOL", DefaultOn, true, "扩展 scratch pool；缺省 ON，=0 回退；原 reg_alloc.cpp:28") \
    X(bool, x86_helper_values, "SVM_X86_HELPER_VALUES", NonZero, false, "helper 显式 guest value ABI；非 0 开，缺省 OFF；原 decoder_internal.h:28") \
    X(u32, x86_pin_ext, "SVM_X86_PIN_EXT", NonNegativeAtoi, 2, "x86 GPR pin level；缺省 2，负数钳为 0；原 reg_alloc.cpp:18/translator.cpp:672") \
    X(bool, xmm_fault_sink, "SVM_XMM_FAULT_SINK", DefaultOn, true, "XMM fault sink；缺省 ON，=0 回退；原 translator.cpp:662") \
    X(bool, xmm_snapshot_dse, "SVM_XMM_SNAPSHOT_DSE", DefaultOn, true, "XMM fault 快照发布最新 SSA 值并删除同边界同值冗余快照；缺省 ON,=0 回退(回退保留已实测的陈旧值错误,仅作应急);原 uniform_store_sink_pass.cpp") \
    X(bool, ra_diag, "SVM_RA_DIAG", NonZero, false, "RA 诊断；非 0 开，缺省 OFF；原 register_alloc_pass.cpp:124") \
    X(std::string, ra_shape_prof, "SVM_RA_SHAPE_PROF", RawString, "", "RA shape profile 输出目标；存在且非 0 启用；原 ra_shape_prof.cpp:132/249") \
    X(std::string, ra_hot_coalesce, "SVM_RA_HOT_COALESCE", RawString, "", "hot coalesce profile 输出目标；存在且非 0 启用；原 hot_coalesce_prof.cpp:171/451") \
    X(bool, helper_leaf_abi, "SVM_HELPER_LEAF_ABI", NonZero, false, "leaf helper ABI；非 0 开且受平台能力限制；原 translator_control.cpp:23") \
    X(bool, backedge_latch, "SVM_BACKEDGE_LATCH", NonZero, false, "self-backedge exit latch；非 0 开，缺省 OFF；原 backedge_control.cpp:18") \
    X(bool, backedge_flags, "SVM_BACKEDGE_FLAGS", NonZero, false, "backedge flags 去物化；非 0 开且依赖 latch；原 backedge_control.cpp:24") \
    X(bool, ra_intwidth_tie, "SVM_RA_INTWIDTH_TIE", DefaultOn, true, "整数宽度链 tie；缺省 ON，=0 回退；原 register_alloc_pass.cpp:90") \
    X(bool, sse_afp_nan, "SVM_SSE_AFP_NAN", DefaultOn, true, "AFP NaN policy 请求；缺省 ON，=0 回退，仍受 host AFP 限制；原 translator/x86/translator.cpp:342") \
    X(bool, sse_afp_minmax, "SVM_SSE_AFP_MINMAX", NonZero, false, "AFP/AH scalar SSE MIN/MAX 单指令 lowering；非 0 开，缺省 OFF，依赖有效 SVM_SSE_AFP_NAN；原 translator_alu.cpp") \
    X(std::string, fpcr_tax_prof, "SVM_FPCR_TAX_PROF", RawString, "", "FPCR tax profile 输出目标；存在且非 0 启用；原 fpcr_tax_prof.cpp:69/358") \
    X(bool, fpcr_tax_skip_switch, "SVM_FPCR_TAX_SKIP_SWITCH", NonZero, false, "FPCR tax 跳过 switch；非 0 开，缺省 OFF；原 fpcr_tax_prof.cpp:384") \
    X(std::string, fpcr_tax_timing, "SVM_FPCR_TAX_TIMING", RawString, "", "FPCR timing 输出目标；存在且非 0 启用；原 fpcr_tax_prof.cpp:227/398") \
    X(bool, mem_hostbase_fold, "SVM_MEM_HOSTBASE_FOLD", DefaultOn, true, "host-base fold；缺省 ON，=0 回退；原 translator/x86/translator.cpp:728") \
    X(bool, induct_tie, "SVM_INDUCT_TIE", DefaultOn, true, "induction tie；缺省 ON，=0 回退；原 register_alloc_pass.cpp:98") \
    X(bool, region_edges, "SVM_REGION_EDGES", DefaultOn, true, "region edge 内部化（bounded16）；缺省 ON，=0 回退；原 translator/x86/translator.cpp:730") \
    X(bool, exec_trace, "SVM_EXEC_TRACE", NonZero, false, "执行 trace 探针；非 0 开，缺省 OFF；原 runtime.cpp:80/jit_context.cpp:78") \
    X(bool, scratch_precise, "SVM_SCRATCH_PRECISE", DefaultOn, true, "Add/Sub 精确 scratch 计费；缺省 ON，=0 回退；原 reg_alloc.cpp:92") \
    X(bool, fpr_scratch_precise, "SVM_FPR_SCRATCH_PRECISE", DefaultOn, true, "FPR 高预算 opcode 按实例精确定价；缺省 ON，=0 回退；原 reg_alloc.cpp") \
    X(bool, fpr_ipv_reclaim, "SVM_FPR_IPV_RECLAIM", DefaultOn, true, "AFP 已覆盖 NaN cold ABI 时归还 v11-v14；缺省 ON，=0 回退；单独关本开关时 SVM_XMM_RESIDENT_HI 退化为有税形态；原 trampolines.cpp") \
    X(bool, ra_spill_evict, "SVM_RA_SPILL_EVICT", DefaultOn, true, "RA farthest-end 驱逐；缺省 ON，=0 回退；原 register_alloc_pass.cpp:106") \
    X(bool, ra_coalesce, "SVM_RA_COALESCE", DefaultOn, true, "guest GPR 发布点定向合并；缺省 ON，=0 回退；原 register_alloc_pass.cpp") \
    X(bool, ra_width_chain, "SVM_RA_WIDTH_CHAIN", NonZero, false, "unit 内多节点整数宽度 identity 链合并；非 0 开，缺省 OFF；原 register_alloc_pass.cpp") \
    X(bool, ra_width_chain_long, "SVM_RA_WIDTH_CHAIN_LONG", DefaultOn, true, "unit 内长 Add/Xor 发布链的低 32 位 ownership 合并；缺省 ON，=0 回退；原 register_alloc_pass.cpp") \
    X(bool, const_addr_cache, "SVM_CONST_ADDR_CACHE", NonZero, false, "unit 内重复绝对地址寄存器缓存；非 0 开，缺省 OFF；原 register_alloc_pass.cpp") \
    X(bool, indirect_l1, "SVM_INDIRECT_L1", DefaultOn, true, "间接出口块内 L1 首槽快查，含逐出口信号 safepoint；缺省 ON，=0 回退；原 jit_context.cpp") \
    X(bool, shadow_lean, "SVM_SHADOW_LEAN", DefaultOn, true, "RSB frame 复用 L2 slot，缩短 push/pop；缺省 ON，=0 回退；原 jit_context.cpp") \
    X(std::string, arm64_lrcpc_override, "SVM_ARM64_LRCPC", RawString, "", "RCpc host 特征 override；仅明确 0/1 生效，缺省或其他值不改探测；原 translator/x86/translator.cpp:315") \
    X(bool, block_link, "SVM_BLOCK_LINK", DefaultOn, true, "direct block link；缺省 ON，=0 回退；原 translator/x86/translator.cpp:703") \
    X(bool, decode_prof, "SVM_DECODE_PROF", NonZero, false, "decode detail profile；非 0 开且依赖 PROF2；原 perf_stats.h:538") \
    X(u64, decode_prof_empty, "SVM_DECODE_PROF_EMPTY", EmptyBenchIterations, 0, "decode 空循环校准次数；0/缺省不跑，小于 1000 时取 1000000；原 perf_stats.h:877") \
    X(bool, density_prof, "SVM_DENSITY_PROF", NonZero, false, "静态密度探针；非 0 开，缺省 OFF；原 jit_context.cpp:88") \
    X(bool, indirect_l1_prof, "SVM_INDIRECT_L1_PROF", NonZero, false, "块内 L1 命中/失败按 guest PC 计数；非 0 开，缺省 OFF；原 hot_coalesce_prof.cpp") \
    X(bool, distorm_fast, "SVM_DISTORM_FAST", DefaultOn, true, "distorm fast decoder；缺省 ON，=0 回退；原 distorm_fast.cc:596") \
    X(bool, distorm_verify, "SVM_DISTORM_VERIFY", NonZero, false, "distorm fast differential verify；非 0 开；原 distorm_fast.cc:604") \
    X(bool, dump_ir_post, "SVM_DUMP_IR_POST", Presence, false, "pass 后 IR 诊断；变量存在即开；原 flags/uniform_elimination_pass.cpp") \
    X(bool, enable_jit, "SVM_ENABLE_JIT", DefaultOn, true, "JIT 总开关；缺省 ON，=0 走解释器；原 translator/x86/arm64 translator.cpp") \
    X(u32, exec_access_pad, "SVM_EXEC_ACCESS_PAD", CappedU32_64, 0, "EXEC profile 访问槽 padding；strtoul 后上限 64；原 jit_context.cpp:81") \
    X(std::string, exec_map, "SVM_EXEC_MAP", RawString, "", "JIT/trampoline 地址图；trampoline 非 0 开，host-map 原站点按变量存在；原 trampolines.cpp:37/jit_context.cpp:913") \
    X(bool, exec_prof, "SVM_EXEC_PROF", NonZero, false, "动态执行计数探针；非 0 开；原 runtime.cpp/jit_context.cpp 等") \
    X(bool, flags_debug, "SVM_FLAGS_DEBUG", Presence, false, "flags pass 诊断；变量存在即开；原 flags_elimination_pass.cpp:39") \
    X(bool, flag_full_elim, "SVM_FLAG_FULL_ELIM", NonZero, false, "完整 flags 消除实验；非 0 开；原 flags_elimination_pass.cpp:541") \
    X(std::string, force_fixed_stack, "SVM_FORCE_FIXED_STACK", RawString, "", "固定 stack/image 三态诊断；=1 强制，=0 强制 fallback，其他值无动作；原 guest_memory.cpp:269/loader.cpp:277") \
    X(bool, func_base, "SVM_FUNC_BASE", DefaultOn, true, "函数级编译；缺省 ON，=0 回退 block；原 translator/x86/arm64 translator.cpp") \
    X(bool, func_lambda, "SVM_FUNC_LAMBDA", DefaultOn, true, "函数 lambda 路径；缺省 ON，=0 回退；原 translator/x86/arm64 translator.cpp") \
    X(bool, func_stats, "SVM_FUNC_STATS", NonZero, false, "函数编译统计；非 0 开；原 translator/function_stats.h:15") \
    X(bool, gpr_zext_coalesce, "SVM_GPR_ZEXT_COALESCE", DefaultOn, true, "GPR zext coalesce；缺省 ON，=0 回退；原 decoder.cc:990") \
    X(std::string, guest_bits, "SVM_GUEST_BITS", RawString, "", "guest window bits 原串；缺省 32，显式值由 launcher/AOT 各自校验；原 linux/main.cpp:386/aot_guest.cpp:52") \
    X(bool, ir_detail, "SVM_IR_DETAIL", NonZero, false, "IR detail profile；非 0 开；原 perf_stats.h:530") \
    X(std::string, jit_cache, "SVM_JIT_CACHE", RawString, "", "JIT disk-cache 目录；非空启用；原 jit_cache.cpp:65/71") \
    X(bool, jit_cache_stats, "SVM_JIT_CACHE_STATS", NonZeroNonEmpty, false, "JIT cache 统计；非空非 0 开；原 jit_cache.cpp:35/75") \
    X(bool, low_prof, "SVM_LOW_PROF", NonZero, false, "lowering detail profile；非 0 开且依赖 decode profile；原 perf_stats.h:546") \
    X(u64, low_prof_empty, "SVM_LOW_PROF_EMPTY", EmptyBenchIterations, 0, "lowering 空循环校准次数；0/缺省不跑，小于 1000 时取 1000000；原 perf_stats.h:899") \
    X(bool, mem_identity_test_collision, "SVM_MEM_IDENTITY_TEST_COLLISION", NonZero, false, "identity 映射碰撞注入测试；非 0 开；原 guest_memory.cpp:228") \
    X(bool, mem_mode_trace, "SVM_MEM_MODE_TRACE", NonZero, false, "内存模式启动诊断；非 0 开；原 linux/main.cpp:480") \
    X(std::string, prof, "SVM_PROF", RawString, "", "总 profile 级别原串；变量存在即开，atoi>=2 打印 per-unit；原 perf_stats.h:344/715") \
    X(bool, prof2, "SVM_PROF2", Presence, false, "细粒度 translate profile；变量存在即开；原 perf_stats.h:320") \
    X(bool, ra_hot_coalesce_all, "SVM_RA_HOT_COALESCE_ALL", NonZero, false, "hot coalesce 全 unit 明细；非 0 开；原 hot_coalesce_prof.cpp:237") \
    X(bool, scratch_precise_peak_audit, "SVM_SCRATCH_PRECISE_PEAK_AUDIT", NonZero, false, "scratch 峰值诊断；非 0 开；原 jit_context.cpp:25") \
    X(bool, signal_delivery, "SVM_SIGNAL_DELIVERY", DefaultOn, true, "guest signal delivery；缺省 ON，=0 回退；原 linux/syscalls.cpp:336") \
    X(bool, signal_private_frame, "SVM_SIGNAL_PRIVATE_FRAME", NonZero, false, "legacy private signal frame；非 0 开；原 linux/syscalls.cpp:348") \
    X(bool, signal_trace, "SVM_SIGNAL_TRACE", NonZero, false, "guest signal trace；非 0 开；原 linux/syscalls.cpp:341") \
    X(bool, smc_mt, "SVM_SMC_MT", DefaultOn, true, "多线程 SMC tracker；缺省 ON，=0 回退；原 translator/x86/translator.cpp:1226") \
    X(bool, sse_nan_fast, "SVM_SSE_NAN_FAST", NonZero, false, "SSE NaN fast path；非 0 开；原 translator.cpp:329") \
    X(bool, static_regs, "SVM_STATIC_REGS", DefaultOn, true, "GPR static residency；缺省 ON，=0 回退；原 translator/x86/translator.cpp:638") \
    X(bool, syscall_mmap_shared_read, "SVM_SYSCALL_MMAP_SHARED_READ", DefaultOn, true, "只读 MAP_SHARED snapshot；缺省 ON，=0 禁用；原 linux/syscalls.cpp:1410") \
    X(bool, syscall_rt_sigaction, "SVM_SYSCALL_RT_SIGACTION", DefaultOn, true, "rt_sigaction syscall；缺省 ON，=0 返回 ENOSYS；原 linux/syscalls.cpp:1736") \
    X(bool, syscall_rt_sigprocmask, "SVM_SYSCALL_RT_SIGPROCMASK", DefaultOn, true, "rt_sigprocmask syscall；缺省 ON，=0 返回 ENOSYS；原 linux/syscalls.cpp:1770") \
    X(std::string, sysroot, "SVM_SYSROOT", RawString, "", "guest 路径 sysroot；非空启用；原 linux/path_utils.h:18") \
    X(bool, trace, "SVM_TRACE", Presence, false, "guest 执行 trace；变量存在即开；原 translator/x86/translator.cpp:1132") \
    X(std::string, tso_mode, "SVM_TSO_MODE", RawString, "relaxed", "TSO 模式 relaxed/acqrel/hardware；未知值回 relaxed；原 translator/x86/translator.cpp:253") \
    X(bool, uniform_elim, "SVM_UNIFORM_ELIM", DefaultOn, true, "uniform elimination；缺省 ON，=0 回退；原 translator/x86/arm64 translator.cpp") \
    X(bool, uniform_path_fwd, "SVM_UNIFORM_PATH_FWD", DefaultOn, true, "uniform path forwarding；缺省 ON，=0 回退；原 uniform_elimination_pass.cpp:65") \
    X(std::string, vixl_host_dump, "SVM_VIXL_HOST_DUMP", RawString, "", "VIXL host 发码诊断非 0 开，host-map 原站点按变量存在；原 jit_context.cpp/trampolines.cpp") \
    X(bool, vixl_prof, "SVM_VIXL_PROF", NonZero, false, "VIXL profile；非 0 开；原 svm-vixl-prof.cc:62") \
    X(bool, vixl_fast, "SVM_VIXL_FAST", DefaultOn, true, "VIXL fast path；缺省 ON，=0 回退；原 svm-vixl-prof.cc:72") \
    X(bool, x86_crypto_ni, "SVM_X86_CRYPTO_NI", DefaultOn, true, "AES/PCLMUL 请求；缺省 ON，=0 回退，仍受 host feature 限制；原 decoder_crypto.cc:58") \
    X(bool, x86_crypto_sha, "SVM_X86_CRYPTO_SHA", DefaultOn, true, "SHA-NI 请求；缺省 ON，=0 回退，仍受 host feature 限制；原 decoder_crypto.cc:69") \
    X(bool, x86_gcm_pclmul2, "SVM_X86_GCM_PCLMUL2", DefaultOn, true, "GCM PCLMUL 双路发码；缺省 ON，=0 回退；原 translator_alu.cpp:15") \
    X(bool, xmm_uniform_fwd, "SVM_XMM_UNIFORM_FWD", DefaultOn, true, "XMM store-load forwarding；缺省 ON，=0 回退；原 uniform_elimination_pass.cpp:78") \
    X(bool, loop_gpr_hoist, "SVM_LOOP_GPR_HOIST", DefaultOn, true, "unit-local 单块自环只读 guest GPR 晋升；缺省 ON，=0 回退；原 loop_invariant_hoist_pass.cpp") \
    X(bool, loop_const_hoist, "SVM_LOOP_CONST_HOIST", DefaultOn, true, "unit-local 单块自环 loop-end 宽常量前缀提升；缺省 ON，=0 回退；原 loop_invariant_hoist_pass.cpp") \
    X(bool, direct_link_stress_long, "SVM_DIRECT_LINK_STRESS_LONG", Presence, false, "direct-link 长压测；变量存在即开；原 region_link_trampoline_test.cpp:426") \
    X(u32, direct_link_stress_iters, "SVM_DIRECT_LINK_STRESS_ITERS", StressIterations, 100000, "direct-link 压测轮数；atoi 后负数钳 0；缺省短测 100000、long 1000000；原 region_link_trampoline_test.cpp:427") \
    X(u32, direct_link_stress_threads, "SVM_DIRECT_LINK_STRESS_THREADS", StressThreads, 0, "direct-link 压测线程数；显式 atoi 后钳 1..8，缺省 hardware_concurrency 钳 2..4；原 region_link_trampoline_test.cpp:381") \
    X(bool, direct_link_w81_child, "SVM_DIRECT_LINK_W81_CHILD", Presence, false, "direct-link 子进程防递归标记；变量存在即开；原 direct_link_production_test.cpp:885") \
    X(bool, sse42str_bench, "SVM_SSE42STR_BENCH", Presence, false, "SSE4.2 fuzz benchmark 输出；变量存在即开；原 sse42str_test.cpp:1278") \
    X(bool, swift_fuzz_debug, "SWIFT_FUZZ_DEBUG", Presence, false, "x86 fuzz 调试子进程标记；变量存在即开；原 x86_fuzz.cpp:953") \
    X(bool, swift_fuzz_dump_ir, "SWIFT_FUZZ_DUMP_IR", Presence, false, "x86 fuzz IR dump；变量存在即开；原 x86_fuzz.cpp:766") \
    X(std::string, swift_fuzz_iters, "SWIFT_FUZZ_ITERS", RawString, "", "x86 fuzz 迭代数原串；显式时各测试 atoi，缺省沿用各调用方 def；原 x86_fuzz.cpp:943") \
    X(std::string, swift_fuzz_seed, "SWIFT_FUZZ_SEED", RawString, "", "x86 fuzz 随机种子原串；显式时 strtoull(base 0)，缺省 random_device；原 x86_fuzz.cpp:547") \
    X(bool, swift_fuzz_trace, "SWIFT_FUZZ_TRACE", Presence, false, "x86 fuzz trace；变量存在即开；原 x86_fuzz.cpp:769")

struct SvmConfig {
#define SVM_DECLARE_FIELD(type, field, name, parse, default_value, source_comment) \
    type field{default_value}; \
    bool field##_is_set{};
    SVM_CONFIG_FIELDS(SVM_DECLARE_FIELD)
#undef SVM_DECLARE_FIELD

    [[nodiscard]] FeatureSet GetFeatureSet() const;
};

struct SvmConfigFieldInfo {
    const char* name;
    const char* type;
    const char* parser;
    const char* source_comment;
};

#define SVM_COUNT_FIELD(...) + 1
inline constexpr std::size_t kSvmConfigFieldCount = 0 SVM_CONFIG_FIELDS(SVM_COUNT_FIELD);
#undef SVM_COUNT_FIELD

inline constexpr std::array<const char*, kSvmConfigFieldCount> kSvmConfigEnvNames{{
#define SVM_FIELD_NAME(type, field, name, parse, default_value, source_comment) name,
        SVM_CONFIG_FIELDS(SVM_FIELD_NAME)
#undef SVM_FIELD_NAME
}};

inline constexpr std::array<SvmConfigFieldInfo, kSvmConfigFieldCount> kSvmConfigFieldInfo{{
#define SVM_FIELD_INFO(type, field, name, parse, default_value, source_comment) \
    SvmConfigFieldInfo{name, #type, #parse, source_comment},
        SVM_CONFIG_FIELDS(SVM_FIELD_INFO)
#undef SVM_FIELD_INFO
}};

// 首次调用通过 call_once 读取整张表；返回引用在进程生命周期内稳定。
const SvmConfig& GetSvmConfig();
void InitSvmConfig();

// 仅供无并发 reader 的测试使用：丢弃快照并在下一次读取时重新解析 environ。
void ReloadSvmConfigForTest();

// 测试内切换进程环境时必须经这些入口，使下一次读取看到同一份新快照。
// raw getter 只用于保存/恢复环境文本；生产决策必须读 GetSvmConfig()。
const char* GetRawSvmConfigEnvForTest(const char* name);
int SetSvmConfigEnvForTest(const char* name, const char* value, int overwrite);
int UnsetSvmConfigEnvForTest(const char* name);

// Linux loader 在解析 PT_INTERP 后发布内部 ABI 标记；必须发生在创建 translator 前。
void EnableSvmX86AbiBaselineForDriver();

bool IsKnownSvmConfigName(std::string_view name);
std::vector<std::string> SerializeSvmConfig(const SvmConfig& config);

}  // namespace swift::runtime
