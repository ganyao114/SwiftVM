#pragma once

// C entry points of the preserved Cephes binary128 implementation. The
// arithmetic carrier is SoftFloat's integer-only IEEE binary128 container.
#ifdef __cplusplus
extern "C" {
#endif
#include <SoftFloat-3e/platform.h>
#include <SoftFloat-3e/softfloat.h>
float128_t cephes_f128_atan2l(float128_t y, float128_t x);
float128_t cephes_f128_cosl(float128_t x);
float128_t cephes_f128_exp2l(float128_t x);
float128_t cephes_f128_log2l(float128_t x);
float128_t cephes_f128_sinl(float128_t x);
float128_t cephes_f128_tanl(float128_t x);
#ifdef __cplusplus
}
#endif
