#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static atomic_uint sb_epoch;
static atomic_uint sb_done;
static atomic_int sb_x;
static atomic_int sb_y;
static int sb_r1;
static int sb_r2;

static void *sb_worker0(void *unused) {
    (void)unused;
    unsigned last = 0;
    for (;;) {
        unsigned epoch;
        while ((epoch = atomic_load_explicit(&sb_epoch, memory_order_acquire)) == last) {
            __asm__ volatile("pause");
        }
        if (epoch == UINT32_MAX) return 0;
        atomic_store_explicit(&sb_x, 1, memory_order_relaxed);
        sb_r1 = atomic_load_explicit(&sb_y, memory_order_relaxed);
        atomic_fetch_add_explicit(&sb_done, 1, memory_order_release);
        last = epoch;
    }
}

static void *sb_worker1(void *unused) {
    (void)unused;
    unsigned last = 0;
    for (;;) {
        unsigned epoch;
        while ((epoch = atomic_load_explicit(&sb_epoch, memory_order_acquire)) == last) {
            __asm__ volatile("pause");
        }
        if (epoch == UINT32_MAX) return 0;
        atomic_store_explicit(&sb_y, 1, memory_order_relaxed);
        sb_r2 = atomic_load_explicit(&sb_x, memory_order_relaxed);
        atomic_fetch_add_explicit(&sb_done, 1, memory_order_release);
        last = epoch;
    }
}

static uint64_t run_sb(unsigned iterations) {
    pthread_t a, b;
    uint64_t zero_zero = 0;
    atomic_store(&sb_epoch, 0);
    if (pthread_create(&a, 0, sb_worker0, 0) ||
        pthread_create(&b, 0, sb_worker1, 0)) {
        return UINT64_MAX;
    }
    for (unsigned i = 1; i <= iterations; ++i) {
        atomic_store_explicit(&sb_x, 0, memory_order_relaxed);
        atomic_store_explicit(&sb_y, 0, memory_order_relaxed);
        atomic_store_explicit(&sb_done, 0, memory_order_relaxed);
        atomic_store_explicit(&sb_epoch, i, memory_order_release);
        while (atomic_load_explicit(&sb_done, memory_order_acquire) != 2) {
            __asm__ volatile("pause");
        }
        zero_zero += sb_r1 == 0 && sb_r2 == 0;
    }
    atomic_store_explicit(&sb_epoch, UINT32_MAX, memory_order_release);
    pthread_join(a, 0);
    pthread_join(b, 0);
    return zero_zero;
}

static atomic_uint mp_epoch;
static atomic_uint mp_done;
static atomic_uint mp_data;
static atomic_uint mp_flag;
static atomic_ullong mp_violations;

static void *mp_producer(void *unused) {
    (void)unused;
    unsigned last = 0;
    for (;;) {
        unsigned epoch;
        while ((epoch = atomic_load_explicit(&mp_epoch, memory_order_acquire)) == last) {
            __asm__ volatile("pause");
        }
        if (epoch == UINT32_MAX) return 0;
        atomic_store_explicit(&mp_data, epoch, memory_order_relaxed);
        atomic_store_explicit(&mp_flag, epoch, memory_order_relaxed);
        atomic_fetch_add_explicit(&mp_done, 1, memory_order_release);
        last = epoch;
    }
}

static void *mp_consumer(void *unused) {
    (void)unused;
    unsigned last = 0;
    for (;;) {
        unsigned epoch;
        while ((epoch = atomic_load_explicit(&mp_epoch, memory_order_acquire)) == last) {
            __asm__ volatile("pause");
        }
        if (epoch == UINT32_MAX) return 0;
        while (atomic_load_explicit(&mp_flag, memory_order_relaxed) != epoch) {
            __asm__ volatile("pause");
        }
        if (atomic_load_explicit(&mp_data, memory_order_relaxed) != epoch) {
            atomic_fetch_add_explicit(&mp_violations, 1, memory_order_relaxed);
        }
        atomic_fetch_add_explicit(&mp_done, 1, memory_order_release);
        last = epoch;
    }
}

static uint64_t run_mp(unsigned iterations) {
    pthread_t producer, consumer;
    atomic_store(&mp_epoch, 0);
    atomic_store(&mp_violations, 0);
    if (pthread_create(&producer, 0, mp_producer, 0) ||
        pthread_create(&consumer, 0, mp_consumer, 0)) {
        return UINT64_MAX;
    }
    for (unsigned i = 1; i <= iterations; ++i) {
        atomic_store_explicit(&mp_data, 0, memory_order_relaxed);
        atomic_store_explicit(&mp_flag, 0, memory_order_relaxed);
        atomic_store_explicit(&mp_done, 0, memory_order_relaxed);
        atomic_store_explicit(&mp_epoch, i, memory_order_release);
        while (atomic_load_explicit(&mp_done, memory_order_acquire) != 2) {
            __asm__ volatile("pause");
        }
    }
    atomic_store_explicit(&mp_epoch, UINT32_MAX, memory_order_release);
    pthread_join(producer, 0);
    pthread_join(consumer, 0);
    return atomic_load_explicit(&mp_violations, memory_order_relaxed);
}

int main(int argc, char **argv) {
    unsigned iterations = 1000000;
    if (argc > 1) {
        iterations = (unsigned)strtoul(argv[1], 0, 0);
        if (iterations == 0 || iterations == UINT32_MAX) return 2;
    }
    const uint64_t sb00 = run_sb(iterations);
    const uint64_t mp_bad = run_mp(iterations);
    printf("iterations=%u sb00=%llu mp_violations=%llu\n",
           iterations,
           (unsigned long long)sb00,
           (unsigned long long)mp_bad);
    return sb00 == UINT64_MAX || mp_bad != 0 ? 1 : 0;
}
