#include <math.h>
#include <stdio.h>
#include <stdint.h>
int main(void) {
    printf("sqrt2=%a sin1=%a cos1=%a exp1=%a log2=%a\n",
           sqrt(2.0), sin(1.0), cos(1.0), exp(1.0), log(2.0));
    double neg = -1.0, zero = 0.0;
    printf("nan_sqrt=%d inf_div=%d negzero=%d\n",
           isnan(sqrt(neg)), isinf(1.0 / zero), signbit(-zero) != 0);
    float f = 1.5f;
    for (int i = 0; i < 20; ++i) f = f * 1.25f + 0.5f;
    printf("chain=%a\n", (double)f);
    double s = 0;
    for (int i = 0; i < 100000; ++i) s += sin(i * 0.001) * cos(i * 0.002);
    printf("loop=%.12a\n", s);
    return 0;
}
