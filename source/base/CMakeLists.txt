add_library(swift_base STATIC file.cpp
        file.h
        logging.cpp
        logging.h
        common_funcs.h
        scope_exit.h)

target_link_libraries(swift_base PUBLIC fmt)
add_library(swift::base ALIAS swift_base)