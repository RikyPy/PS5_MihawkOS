/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The flight recorder's process reader (src/pw_flight_probe.h). */
#include "pw_flight_probe.h"
#include <errno.h>
#include <string.h>

static void *flight_worker(void *opaque)
{
    PwFlightProbe *probe = opaque;
    int pids[PW_FLIGHT_PIDS];
    PwFlightRow rows[PW_FLIGHT_PIDS];
    pthread_mutex_lock(&probe->lock);
    for (;;) {
        while (probe->done == probe->asked) pthread_cond_wait(&probe->wake, &probe->lock);
        const unsigned count = probe->count;
        memcpy(pids, probe->pids, sizeof(pids));
        pthread_mutex_unlock(&probe->lock);
        for (unsigned i = 0; i < count; ++i) {
            memset(&rows[i], 0, sizeof(rows[i]));
            rows[i].pid = pids[i];
            __atomic_store_n(&probe->current_ms, probe->now_ms(), __ATOMIC_RELAXED);
            __atomic_store_n(&probe->current, pids[i], __ATOMIC_RELEASE);
            const int error = probe->read(probe->context, pids[i], &rows[i]);
            __atomic_store_n(&probe->current, 0, __ATOMIC_RELEASE);
            if (error) {
                memset(&rows[i], 0, sizeof(rows[i]));
                rows[i].pid = pids[i];
                rows[i].error = error;
            }
        }
        pthread_mutex_lock(&probe->lock);
        memcpy(probe->rows, rows, sizeof(rows));
        probe->row_count = count;
        probe->done = probe->asked;
    }
    return NULL;
}

int pw_flight_probe_start(PwFlightProbe *probe, PwFlightRead read, void *context, uint64_t (*now_ms)(void))
{
    if (!probe || !read || !now_ms) return EINVAL;
    memset(probe, 0, sizeof(*probe));
    pthread_mutex_init(&probe->lock, NULL);
    pthread_cond_init(&probe->wake, NULL);
    probe->read = read;
    probe->context = context;
    probe->now_ms = now_ms;
    pthread_attr_t attributes;
    pthread_t thread;
    int error = pthread_attr_init(&attributes);
    if (error) return error;
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attributes, 256u << 10);
    error = pthread_create(&thread, &attributes, flight_worker, probe);
    pthread_attr_destroy(&attributes);
    return error;
}

unsigned pw_flight_probe_take(PwFlightProbe *probe, PwFlightRow *rows, unsigned capacity, int *busy_pid,
                              uint64_t *busy_ms)
{
    *busy_pid = 0;
    *busy_ms = 0;
    pthread_mutex_lock(&probe->lock);
    if (probe->done != probe->asked) {
        pthread_mutex_unlock(&probe->lock);
        *busy_pid = __atomic_load_n(&probe->current, __ATOMIC_ACQUIRE);
        const uint64_t began = __atomic_load_n(&probe->current_ms, __ATOMIC_RELAXED), now = probe->now_ms();
        *busy_ms = *busy_pid && now > began ? now - began : 0;
        return 0;
    }
    const unsigned taken = probe->row_count < capacity ? probe->row_count : capacity;
    if (taken) memcpy(rows, probe->rows, taken * sizeof(*rows));
    probe->row_count = 0;
    pthread_mutex_unlock(&probe->lock);
    return taken;
}

int pw_flight_probe_ask(PwFlightProbe *probe, const int *pids, unsigned count)
{
    if (!pids || !count) return EINVAL;
    pthread_mutex_lock(&probe->lock);
    if (probe->done != probe->asked) {
        pthread_mutex_unlock(&probe->lock);
        return EBUSY;
    }
    probe->count = count < PW_FLIGHT_PIDS ? count : PW_FLIGHT_PIDS;
    memcpy(probe->pids, pids, probe->count * sizeof(*pids));
    probe->row_count = 0;
    ++probe->asked;
    pthread_cond_signal(&probe->wake);
    pthread_mutex_unlock(&probe->lock);
    return 0;
}
