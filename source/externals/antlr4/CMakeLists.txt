add_subdirectory(runtime)
set(ANTLR4_COMPILER ${CMAKE_CURRENT_SOURCE_DIR}/antlr4-compiler.jar CACHE INTERNAL "")

function(antlr4_gen def_path target_path)
    find_program(JAVA java REQUIRED)
    message("-- Java Path: ${JAVA}")
    message("-- Antlr4 Compiler Path: ${ANTLR4_COMPILER}")
    file(GLOB G4_DEFS ${def_path}/*.g4)
    foreach (g4_file ${G4_DEFS})
        get_filename_component(g4_name ${g4_file} NAME_WE)
        execute_process(COMMAND ${JAVA} -jar ${ANTLR4_COMPILER} -Dlanguage=Cpp -visitor -no-listener -o ${target_path} ${g4_file})
        message("-- Antlr4 gen success: ${g4_file}")
    endforeach ()
endfunction()
