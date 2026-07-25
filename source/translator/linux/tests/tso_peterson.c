#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

enum { ITERATIONS = 100000 };

static atomic_uint interested[2];
static volatile uint32_t victim;
static atomic_int in_critical;
static atomic_ullong counter;
static atomic_ullong violations;

static void xchg32(volatile uint32_t *ptr, uint32_t value) {
    __asm__ volatile("xchgl %0, %1"
                     : "+r"(value), "+m"(*ptr)
                     :
                     : "memory");
}

static void *worker(void *argument) {
    const unsigned self = (unsigned)(uintptr_t)argument;
    const unsigned other = self ^ 1;
    for (unsigned i = 0; i < ITERATIONS; ++i) {
        atomic_store_explicit(&interested[self], 1, memory_order_relaxed);
        // The locked exchange is the x86 full fence required between the
        // flag store and the subsequent load in Peterson's algorithm.
        xchg32(&victim, self);
        while (atomic_load_explicit(&interested[other], memory_order_relaxed) &&
               __atomic_load_n(&victim, __ATOMIC_RELAXED) == self) {
            __asm__ volatile("pause");
        }
        if (atomic_fetch_add_explicit(&in_critical, 1, memory_order_relaxed) != 0) {
            atomic_fetch_add_explicit(&violations, 1, memory_order_relaxed);
        }
        atomic_fetch_add_explicit(&counter, 1, memory_order_relaxed);
        atomic_fetch_sub_explicit(&in_critical, 1, memory_order_relaxed);
        atomic_store_explicit(&interested[self], 0, memory_order_release);
    }
    return 0;
}

int main(void) {
    pthread_t a, b;
    if (pthread_create(&a, 0, worker, (void *)(uintptr_t)0) ||
        pthread_create(&b, 0, worker, (void *)(uintptr_t)1)) {
        return 2;
    }
    pthread_join(a, 0);
    pthread_join(b, 0);
    const uint64_t expected = 2ull * ITERATIONS;
    const uint64_t count = atomic_load(&counter);
    const uint64_t bad = atomic_load(&violations);
    printf("peterson-counter=%llu expected=%llu violations=%llu\n",
           (unsigned long long)count,
           (unsigned long long)expected,
           (unsigned long long)bad);
    return count == expected && bad == 0 ? 0 : 1;
}
