enable_language(ASM)
add_subdirectory(externals)
add_subdirectory(frontend)

if ("arm64" IN_LIST ARCHITECTURE)
    set(RUNTIME_ARCH_SOURCE
            backend/arm64/context.S)
else ()
    set(RUNTIME_ARCH_SOURCE backend/dummy_entry.cpp
    )
endif ()

add_library(swift_runtime STATIC
        include/sruntime.h
        include/config.h
        common/types.h
        common/slab_alloc.cc
        common/slab_alloc.h
        common/object_pool.h
        common/variant_util.h
        common/logging.h
        common/logging.cpp
        common/svm_config.cpp
        common/svm_config.h
        common/virtual_vector.cpp
        common/virtual_vector.h
        common/bit_fields.h
        common/function_info.h
        common/hot_coalesce_prof.cpp
        common/hot_coalesce_prof.h
        common/backedge_control.cpp
        common/backedge_control.h
        common/helper_abi.h
        common/cast_utils.h
        common/lru_cache.h
        common/range_mutex.h
        common/range_map.h
        common/ra_shape_prof.cpp
        common/ra_shape_prof.h
        common/spin_lock.cpp
        common/spin_lock.h
        common/spin_lock.h
        common/cityhash.cpp
        common/cityhash.h
        common/swap.h
        common/address_hash_map.h
        frontend/ir_assembler.cpp
        frontend/ir_assembler.h
        frontend/function_abi.h
        ir/args.cpp
        ir/args.h
        ir/ir_types.h
        ir/opcodes.h
        ir/location.cpp
        ir/location.h
        ir/instr.cpp
        ir/instr.h
        ir/block.cpp
        ir/block.h
        ir/terminal.cpp
        ir/terminal.h
        ir/function.cpp
        ir/function.h
        ir/module.cpp
        ir/module.h
        ir/hir_builder.cpp
        ir/hir_builder.h
        ir/ir_meta.cpp
        ir/ir_meta.h
        ir/host_reg.h
        ir/allocator.h
        ir/opts/dataflow_analysis_pass.cpp
        ir/opts/dataflow_analysis_pass.h
        ir/opts/cfg_analysis_pass.cpp
        ir/opts/cfg_analysis_pass.h
        ir/opts/const_folding_pass.cpp
        ir/opts/const_folding_pass.h
        ir/opts/register_alloc_pass.cpp
        ir/opts/register_alloc_pass.h
        ir/opts/reid_instr_pass.cpp
        ir/opts/reid_instr_pass.h
        ir/opts/uniform_elimination_pass.cpp
        ir/opts/uniform_elimination_pass.h
        ir/opts/uniform_store_sink_pass.cpp
        ir/opts/uniform_store_sink_pass.h
        ir/opts/flags_elimination_pass.cpp
        ir/opts/flags_elimination_pass.h
        ir/opts/loop_invariant_hoist_pass.cpp
        ir/opts/loop_invariant_hoist_pass.h
        ir/opts/deadcode_elimination_pass.cpp
        ir/opts/deadcode_elimination_pass.h
        ir/opts/pass_pipeline.cpp
        ir/opts/pass_pipeline.h
        backend/translate_table.h
        backend/asm_interp.cpp
        backend/asm_interp.h
        backend/context.h
        backend/address_space.cpp
        backend/address_space.h
        backend/runtime.cpp
        backend/runtime.h
        backend/signal_handler.cpp
        backend/signal_handler.h
        backend/smc_tracker.cpp
        backend/smc_tracker.h
        backend/entrypoint.h
        backend/code_cache.cpp
        backend/code_cache.h
        backend/link_manager.cpp
        backend/link_manager.h
        backend/code_serial.cpp
        backend/code_serial.h
        backend/jit_cache.cpp
        backend/jit_cache.h
        backend/host_isa.cpp
        backend/host_isa.h
        backend/mem_map.cpp
        backend/mem_map.h
        backend/reg_alloc.cpp
        backend/reg_alloc.h
        backend/jit_code.h
        backend/translator.cpp
        backend/translator.h
        backend/module.cpp
        backend/module.h
        backend/trampolines.cpp
        backend/trampolines.h
        backend/arm64/jit/jit_context.cpp
        backend/arm64/jit/jit_context.h
        backend/arm64/jit/translator.cpp
        backend/arm64/jit/translator_operand.cpp
        backend/arm64/jit/translator_terminal.cpp
        backend/arm64/jit/translator_alu.cpp
        backend/arm64/jit/translator_control.cpp
        backend/arm64/jit/translator_sse42str.cpp
        backend/arm64/jit/translator_x87.cpp
        backend/arm64/jit/translator_flags.cpp
        backend/arm64/jit/translator_mem.cpp
        backend/arm64/jit/translator.h
        backend/arm64/jit/flags.cpp
        backend/arm64/trampolines.cpp
        backend/arm64/trampolines.h
        backend/arm64/region_link_trampoline.cpp
        backend/arm64/region_link_trampoline.h
        backend/riscv64/jit/jit_context.cpp
        backend/riscv64/jit/jit_context.h
        backend/riscv64/trampolines.cpp
        backend/riscv64/trampolines.h
        backend/interp/interpreter.cpp
        backend/interp/interpreter_flags.cpp
        backend/interp/interpreter_memory.cpp
        backend/interp/interpreter.h
        backend/interp/interpreter_internal.h
        backend/cache_clear.cpp
        backend/cache_clear.h
        backend/host_function.h
        ${RUNTIME_ARCH_SOURCE})

# GCC requires the AArch64 crypto extension at compile time before its
# vsha256* intrinsics can be inlined. The x86 SHA frontend already gates
# execution on the runtime host feature; this option only makes the existing
# interpreter implementation buildable on Linux and leaves macOS unchanged.
if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND "arm64" IN_LIST ARCHITECTURE)
    set_source_files_properties(
            backend/interp/interpreter.cpp
            PROPERTIES COMPILE_OPTIONS "-march=armv8-a+crypto")
endif ()

find_package(Boost 1.57)
target_link_libraries(swift_runtime PUBLIC swift::base fmt Boost::boost vixl riscv64_asm dlmalloc)
add_library(swift::runtime ALIAS swift_runtime)
