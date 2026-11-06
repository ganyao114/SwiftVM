This is the selected ext80, F32/F64 conversion and binary128 dependency subset
copied from the frozen SwiftVM source/runtime/externals/SoftFloat-3e. The
binary128 operations support the Cephes x87 transcendental implementation;
this is not the complete SoftFloat library. Original per-file notices and
the BSD three-clause notice in LICENSE remain. Local changes append a
roundedUp byte to softfloat_state and record the effective magnitude
increment in s_roundPackToExtF80.c and s_roundPackToF64.c. This lets the x87
helper obtain C1 from the actual rounding decision without redoing arithmetic;
the numerical result and exception decisions are unchanged.
The vendored ABI passes a five-byte softfloat_state explicitly to arithmetic
and conversion operations. It has no process/thread-global rounding or
exception state. The public target is svm_softfloat_ext80; consumers include
<SoftFloat-3e/softfloat.h> inside extern "C". No preserve_all ABI is assumed:
the existing native helper bridge owns register/environment preservation.
