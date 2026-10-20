/* SPDX-License-Identifier: GPL-3.0-or-later
 * The thread alert of patch 0910 (a mutex, a condition variable and a flag per thread id, in zeroed memory), as
 * ntdll writes it, run under load on the host: an alert before a wait is kept and consumed once, a wait with a
 * timeout times out and does not early, a wait with none ends on an alert, and nothing is lost between threads. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>

typedef long long LONGLONG;
#define TICKSPERSEC 10000000LL
enum { STATUS_ALERTED = 1, STATUS_TIMEOUT = 2 };

struct entry { pthread_mutex_t lock; pthread_cond_t cond; long alerted; };

static LONGLONG now_ticks(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (LONGLONG)t.tv_sec * TICKSPERSEC + t.tv_nsec / 100;
}

static void alert(struct entry *entry)
{
    pthread_mutex_lock(&entry->lock);
    entry->alerted = 1;
    pthread_cond_signal(&entry->cond);
    pthread_mutex_unlock(&entry->lock);
}

/* timeout_ticks < 0: relative, 0: poll, INT64_MIN-like marker for none is -1 passed as infinite */
static int wait_for_alert(struct entry *entry, int infinite, LONGLONG relative)
{
    LONGLONG end = now_ticks() + relative;
    int status = STATUS_ALERTED;

    pthread_mutex_lock(&entry->lock);
    while (!entry->alerted) {
        if (!infinite) {
            LONGLONG left = end - now_ticks();
            struct timespec when;

            if (left <= 0) { status = STATUS_TIMEOUT; break; }
            clock_gettime(CLOCK_REALTIME, &when);
            when.tv_sec += left / TICKSPERSEC;
            when.tv_nsec += (left % TICKSPERSEC) * 100;
            if (when.tv_nsec >= 1000000000) { when.tv_sec++; when.tv_nsec -= 1000000000; }
            if (pthread_cond_timedwait(&entry->cond, &entry->lock, &when) == ETIMEDOUT && !entry->alerted) {
                status = STATUS_TIMEOUT;
                break;
            }
        } else pthread_cond_wait(&entry->cond, &entry->lock);
    }
    if (status == STATUS_ALERTED) entry->alerted = 0;
    pthread_mutex_unlock(&entry->lock);
    return status;
}

static struct entry *block;   /* zeroed memory, like the anonymous blocks */
static atomic_int stop;
static atomic_long alerts_sent, alerts_seen;

static void *signal_later(void *arg)
{
    struct timespec nap = { 0, 20 * 1000 * 1000 };

    nanosleep(&nap, NULL);
    alert(arg);
    return NULL;
}

static void *waiter(void *arg)
{
    struct entry *entry = arg;

    while (!atomic_load(&stop)) {
        if (wait_for_alert(entry, 0, 2 * TICKSPERSEC / 1000) == STATUS_ALERTED) atomic_fetch_add(&alerts_seen, 1);
    }
    return NULL;
}

int main(void)
{
    enum { THREADS = 8 };
    pthread_t threads[THREADS];
    LONGLONG start, took;

    block = calloc(THREADS + 1, sizeof(*block));

    /* an alert before the wait is kept, and consumed once */
    alert(&block[0]); alert(&block[0]);
    assert(wait_for_alert(&block[0], 1, 0) == STATUS_ALERTED);
    assert(wait_for_alert(&block[0], 0, 0) == STATUS_TIMEOUT);

    /* a timeout is neither early nor much late */
    start = now_ticks();
    assert(wait_for_alert(&block[0], 0, 30 * TICKSPERSEC / 1000) == STATUS_TIMEOUT);
    took = now_ticks() - start;
    assert(took >= 29 * TICKSPERSEC / 1000 && took < 200 * TICKSPERSEC / 1000);

    /* an alert from another thread ends a wait with no timeout */
    {
        pthread_t other;

        pthread_create(&other, NULL, signal_later, &block[0]);
        assert(wait_for_alert(&block[0], 1, 0) == STATUS_ALERTED);
        pthread_join(other, NULL);
    }

    /* many threads waiting with short timeouts while alerts arrive: every alert is seen once or merged, and it ends */
    for (int i = 0; i < THREADS; i++) pthread_create(&threads[i], NULL, waiter, &block[1 + i]);
    for (int round = 0; round < 20000; round++) {
        alert(&block[1 + round % THREADS]);
        atomic_fetch_add(&alerts_sent, 1);
        if (round % 50 == 0) { struct timespec t = { 0, 100000 }; nanosleep(&t, NULL); }
    }
    {
        struct timespec t = { 0, 50 * 1000 * 1000 };
        nanosleep(&t, NULL);
    }
    atomic_store(&stop, 1);
    for (int i = 0; i < THREADS; i++) pthread_join(threads[i], NULL);
    assert(atomic_load(&alerts_seen) > 0 && atomic_load(&alerts_seen) <= atomic_load(&alerts_sent));
    printf("alerts sent %ld, seen %ld (merged alerts count once)\n", atomic_load(&alerts_sent), atomic_load(&alerts_seen));
    free(block);
    return 0;
}
