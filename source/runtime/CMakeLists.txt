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
        include/address_space.h
        include/config.h
        common/types.h
        common/slab_alloc.cc
        common/slab_alloc.h
        common/object_pool.h
        common/variant_util.h
        common/logging.h
        common/logging.cpp
        common/virtual_vector.cpp
        common/virtual_vector.h
        common/bit_fields.h
        common/function_info.h
        common/cast_utils.h
        common/lru_cache.h
        common/range_mutex.h
        common/range_map.h
        common/spin_lock.cpp
        common/spin_lock.h
        frontend/ir_assembler.cpp
        frontend/ir_assembler.h
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
        ir/host_reg.h
        ir/opts/dataflow_analysis_pass.cpp
        ir/opts/dataflow_analysis_pass.h
        ir/opts/cfg_analysis_pass.cpp
        ir/opts/cfg_analysis_pass.h
        ir/opts/local_elimination_pass.cpp
        ir/opts/local_elimination_pass.h
        ir/opts/const_folding_pass.cpp
        ir/opts/const_folding_pass.h
        ir/opts/register_alloc_pass.cpp
        ir/opts/register_alloc_pass.h
        ir/opts/reid_instr_pass.cpp
        ir/opts/reid_instr_pass.h
        backend/translate_table.h
        backend/asm_interp.cpp
        backend/asm_interp.h
        backend/context.h
        backend/address_space.cpp
        backend/address_space.h
        backend/interface.cpp
        backend/entrypoint.h
        backend/code_cache.cpp
        backend/code_cache.h
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
        backend/arm64/jit/jit_context.cpp
        backend/arm64/jit/jit_context.h
        backend/arm64/jit/translator.cpp
        backend/arm64/jit/translator.h
        backend/riscv64/jit/jit_context.cpp
        backend/riscv64/jit/jit_context.h
        backend/interp/interpreter.cpp
        backend/interp/interpreter.h
        ${RUNTIME_ARCH_SOURCE})

find_package(Boost 1.57)
target_link_libraries(swift_runtime PUBLIC swift::base fmt Boost::boost vixl riscv64_asm dlmalloc)
add_library(swift::runtime ALIAS swift_runtime)