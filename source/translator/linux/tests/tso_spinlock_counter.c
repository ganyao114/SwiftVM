#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

enum { THREADS = 4, INCREMENTS = 20000 };

static volatile uint32_t lock_word;
static uint64_t counter;

static uint32_t xchg32(volatile uint32_t *ptr, uint32_t value) {
    __asm__ volatile("xchgl %0, %1"
                     : "+r"(value), "+m"(*ptr)
                     :
                     : "memory");
    return value;
}

static void lock(void) {
    while (xchg32(&lock_word, 1) != 0) {
        while (__atomic_load_n(&lock_word, __ATOMIC_RELAXED) != 0) {
            __asm__ volatile("pause");
        }
    }
}

static void unlock(void) {
    // x86 release is a plain aligned store; AcqRel mode must lower this
    // through StoreMemoryTSO while Relaxed keeps the historical plain store.
    __atomic_store_n(&lock_word, 0, __ATOMIC_RELEASE);
}

static void *worker(void *unused) {
    (void)unused;
    for (int i = 0; i < INCREMENTS; ++i) {
        lock();
        ++counter;
        unlock();
    }
    return 0;
}

int main(void) {
    pthread_t threads[THREADS];
    for (int i = 0; i < THREADS; ++i) {
        if (pthread_create(&threads[i], 0, worker, 0) != 0) return 2;
    }
    for (int i = 0; i < THREADS; ++i) {
        if (pthread_join(threads[i], 0) != 0) return 3;
    }
    const uint64_t expected = (uint64_t)THREADS * INCREMENTS;
    printf("spin-counter=%llu expected=%llu\n",
           (unsigned long long)counter,
           (unsigned long long)expected);
    return counter == expected ? 0 : 1;
}
