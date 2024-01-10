add_library(svm_android SHARED android_entry.cpp
        android_entry.h)
target_link_libraries(svm_android PUBLIC swift_runtime)