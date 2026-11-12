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

The selected binary32/binary64 fused-multiply-add algorithms and their two
8086-SSE NaN propagators are pinned by `FMA_UPSTREAM.json`. They come from the
same Berkeley SoftFloat Release 3e, using an existing QEMU subproject cache at
the recorded commit. The local adaptation passes the existing softfloat_state
pointer through every stateful call; it introduces no global/TLS rounding or
exception variables and leaves the other subset interfaces unchanged.

The SSE numerical leaf also uses the matching stateful F32/F64 arithmetic,
signed-integer/precision conversions, and integral-rounding subset. Its additional
upstream file hashes are recorded in `FMA_UPSTREAM.json` at the same pinned
revision. Binary128 is used only on unmasked range-exception boundaries to
separate destination-precision rounding from exponent bounding; ordinary FMA
continues to round once directly in F32/F64.

ARM numerical helpers also require SOFTFLOAT_ROUND_ODD for binary128
intermediates and FCVTXN. The target enables that upstream option and restores
the upstream packReturn label in s_roundPackToF128.c, which the frozen subset
had omitted while compiling that option out. Stateful ABI and rounding modes
remain unchanged.
