#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { THREADS = 4, INCREMENTS = 20000 };

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t counter;

static void *worker(void *unused) {
    (void)unused;
    for (int i = 0; i < INCREMENTS; ++i) {
        pthread_mutex_lock(&mutex);
        ++counter;
        pthread_mutex_unlock(&mutex);
    }
    return 0;
}

int main(void) {
    pthread_t threads[THREADS];
    for (int i = 0; i < THREADS; ++i) {
        if (pthread_create(&threads[i], 0, worker, 0) != 0) {
            return 2;
        }
    }
    for (int i = 0; i < THREADS; ++i) {
        if (pthread_join(threads[i], 0) != 0) {
            return 3;
        }
    }
    const uint64_t expected = (uint64_t)THREADS * INCREMENTS;
    printf("mutex-counter=%llu expected=%llu\n",
           (unsigned long long)counter,
           (unsigned long long)expected);
    return counter == expected ? 0 : 1;
}
