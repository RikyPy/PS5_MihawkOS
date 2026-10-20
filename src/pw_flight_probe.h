/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_FLIGHT_PROBE_H
#define PW_FLIGHT_PROBE_H
/* The flight recorder's process reader (native/proton_flight_recorder.cpp): one worker, started once and never ended,
 * asks the kernel about a few processes (sysctl kern.proc on the console), and the recorder's thread only takes what
 * the last round found. A read can block in exactly the hangs the recorder is there for: the memory helper's census,
 * itself a kern.proc walk, froze in a 2026-10-14 run (record removed from the tree). Then only this worker stops, and the recorder says
 * which pid it is in and for how long. One worker for good, never one per sample: a blocked read would leave a thread
 * behind every second.
 *
 * Locking: the worker holds the mutex only to take a request and to hand its rows over, never while read runs.
 * pw_flight_probe_take never waits on the worker. One process row per pid: per-thread records (KERN_PROC_INC_THREAD)
 * came back empty on the console (tools/console-processes.c). */
#include <pthread.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PW_FLIGHT_PIDS 8u

typedef struct {
    int32_t pid, error;         /* error: 0, or the errno the read gave (no other field is then set) */
    int32_t state, cpu;         /* p_stat (2 running, 3 asleep, 7 on a lock), the CPU it last ran on */
    uint8_t pri_class, pri_level, pri_native, pri_user;
    uint32_t threads;
    uint64_t runtime_us;        /* CPU time used so far */
    char name[20], wait[16];    /* its command name, and what its first thread waits on */
} PwFlightRow;

/* Fill row (row->pid is set) for one pid: 0, or an errno. Called on the worker only. */
typedef int (*PwFlightRead)(void *context, int pid, PwFlightRow *row);

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    unsigned asked, done, count, row_count;
    int pids[PW_FLIGHT_PIDS];
    PwFlightRow rows[PW_FLIGHT_PIDS];
    int current;                /* the pid being read now, 0 between reads (atomic) */
    uint64_t current_ms;        /* when that read began (atomic) */
    PwFlightRead read;
    void *context;
    uint64_t (*now_ms)(void);
} PwFlightProbe;

/* Starts the worker (detached, 256 KiB stack). 0, or EINVAL for a NULL argument, or pthread's errno. */
int pw_flight_probe_start(PwFlightProbe *probe, PwFlightRead read, void *context, uint64_t (*now_ms)(void));
/* When the worker is idle: copies out the rows of the round it finished last, one per pid asked, up to capacity (each
 * round's rows are taken once), and returns how many, with *busy_pid 0. While it reads: copies nothing, returns 0, and
 * says in *busy_pid and *busy_ms which pid it is in and for how long (*busy_pid 0 between two reads). */
unsigned pw_flight_probe_take(PwFlightProbe *probe, PwFlightRow *rows, unsigned capacity, int *busy_pid,
                              uint64_t *busy_ms);
/* Ask the worker to read pids (up to PW_FLIGHT_PIDS; more are left out): 0, EBUSY while it still reads the last round
 * (nothing asked), or EINVAL for no pid. A round not taken before the next is asked is dropped. */
int pw_flight_probe_ask(PwFlightProbe *probe, const int *pids, unsigned count);

#ifdef __cplusplus
}
#endif
#endif
