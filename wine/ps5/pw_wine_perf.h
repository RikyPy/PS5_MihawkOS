/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_PERF_H
#define PW_WINE_PERF_H
#include <stdint.h>
#include <pthread.h>
#if defined(__PROSPERO__) || defined(__FreeBSD__)
#include <pthread_np.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

/* Low-overhead counters for the runtime's hot paths, read by the title.
 *
 * ntdll links this (like the sink): its server calls, waits and sleeps
 * record here, and win32u's presentation paths find pw_wine_perf_frame
 * with dlsym. Recording is a handful of relaxed atomic adds and one TSC
 * read per event, with no lock and no logging; the title copies the whole
 * block with pw_wine_perf_snapshot from its own thread every few seconds
 * and prints the difference, so nothing is formatted on a game thread.
 *
 * Times are TSC ticks. The title sets the TSC frequency once
 * (sceKernelGetTscFrequency) before Wine runs; until then, and on a host,
 * it is calibrated against CLOCK_MONOTONIC. */

enum {
    PW_PERF_REQUESTS = 512,       /* server request codes (REQ_*) tracked by number */
    PW_PERF_FRAME_BUCKET_US = 250,
    PW_PERF_FRAME_BUCKETS = 256,  /* 0..64 ms in 250 us steps, then one overflow bucket */
    PW_PERF_API_OPENGL = 0,
    PW_PERF_API_VULKAN = 1,
    PW_PERF_APIS = 2,
};

/* Media: what winedmo's demuxers (every MF media source, DirectShow parser and WM reader opens one) did through their
 * byte streams (Wine patch 2065). A Direct3D 12 game's first scene read ~160 MiB with a ~690 ms hitch every ~20 s
 * (2026-10-17; record removed from the tree; in git history); a looping video reopened or read again from its start each loop shows here. */
enum {
    PW_PERF_MEDIA_OPENS,          /* demuxers opened */
    PW_PERF_MEDIA_SEEKS,          /* demuxer seeks by time */
    PW_PERF_MEDIA_SEEKS_TO_START, /* of those, to time 0: a loop's restart */
    PW_PERF_MEDIA_REWINDS,        /* the byte stream repositioned to offset 0 */
    PW_PERF_MEDIA_READS,          /* reads from the byte stream, one buffer each */
    PW_PERF_MEDIA_BYTES,          /* bytes those reads returned */
    PW_PERF_MEDIA_COUNTERS,
};

typedef struct PwWinePerfFrames {
    uint64_t frames;                                  /* presents that reached the display */
    uint64_t interval[PW_PERF_FRAME_BUCKETS + 1];     /* present-to-present, by bucket */
    uint64_t interval_max;                            /* ticks, since the last reset of the field */
    uint64_t call_ticks, call_max;                    /* time inside the present call itself */
    uint64_t last;                                    /* tick of the newest present */
    uint64_t first;                                   /* tick of the first present */
} PwWinePerfFrames;

typedef struct PwWinePerf {
    uint64_t tsc_hz;                     /* filled by the snapshot */
    uint64_t now;                        /* tick at the snapshot */
    /* Every client-server round trip (send_request + wait_reply). */
    uint64_t server_calls, server_ticks;
    uint64_t request_calls[PW_PERF_REQUESTS], request_ticks[PW_PERF_REQUESTS];
    const char *request_names[PW_PERF_REQUESTS];
    /* server_select: waits, sleeps and suspends that go through the server.
     * blocked: the reply said "not yet" and the thread waited for the wakeup. */
    uint64_t selects, selects_blocked, select_ticks, select_block_ticks;
    /* NtWaitFor*Objects, by shape, before any fast path. */
    uint64_t waits, wait_handles, waits_all, waits_polled;
    /* NtDelayExecution: zero timeouts are yields. */
    uint64_t delays, delay_yields, delay_requested_us;
    uint64_t media[PW_PERF_MEDIA_COUNTERS];
    PwWinePerfFrames frames[PW_PERF_APIS];
} PwWinePerf;

/* Title side. */
void pw_wine_perf_set_tsc_hz(uint64_t hz);
void pw_wine_perf_snapshot(PwWinePerf *out);
/* Percentile (0..100) of the intervals recorded between two snapshots,
 * in microseconds (the upper edge of its bucket); 0 with no frames. */
uint32_t pw_wine_perf_frame_percentile(const PwWinePerfFrames *now, const PwWinePerfFrames *before,
                                       unsigned percent);
uint64_t pw_wine_perf_ticks_to_us(uint64_t ticks, uint64_t tsc_hz);

/* Runtime side. */
uint64_t pw_wine_perf_ticks(void);
void pw_wine_perf_request(unsigned code, const char *name, uint64_t ticks);
void pw_wine_perf_select(int blocked, uint64_t ticks, uint64_t block_ticks);
void pw_wine_perf_wait(unsigned handles, int wait_all, int polled);
void pw_wine_perf_delay(int64_t timeout_100ns);
/* One present through api; call_ticks is how long the present call took. */
void pw_wine_perf_frame(unsigned api, uint64_t call_ticks);
/* Adds value to media counter what (PW_PERF_MEDIA_*); an unknown counter is ignored. */
void pw_wine_perf_media(unsigned what, uint64_t value);
/* Microseconds since a tick of pw_wine_perf_ticks; 0 for tick 0, a tick ahead, or before the frequency is known. */
uint64_t pw_wine_perf_age_us(uint64_t since);

/* The calling thread's kernel id, the one the console's kernel lists (a host test's gettid). Needs %fs, as
 * pthread_mutex_lock does. */
static inline uint64_t pw_wine_perf_tid(void)
{
#if defined(__PROSPERO__) || defined(__FreeBSD__)
    return (uint64_t)pthread_getthreadid_np();
#else
    long syscall(long number, ...);   /* unistd.h declares it only with _DEFAULT_SOURCE, not under -std=c11 */
    return (uint64_t)syscall(SYS_gettid);
#endif
}

/* Who holds Wine's virtual mutex. In a 2026-10-15 run (record removed from the tree; in git history) the census waited 160 s on it inside
 * __wine_virtual_image_stats and nothing said who held it, or doing what. Patch 1135 sends unix_private.h's
 * mutex_lock and mutex_unlock for the mutex pw_wine_perf_vlock_mutex names here (virtual_init names virtual_mutex;
 * every other mutex pays one compare). site is the return address of the function mutex_lock is inlined into: for
 * server_enter_uninterrupted_section, that function's caller.
 *
 * One writer: only the holder writes owner, since, site, its tag and the longest hold, at its first acquisition and
 * its last release (the mutex is recursive); waiters is an atomic count of the threads blocked on it, and one ended
 * while it blocks (SIGQUIT at a process's exit, TerminateThread) stays counted. Readers use atomic loads only and never
 * wait: pw_wine_perf_vlock_read retries a sample whose owner changed while it read, and a hold that ended and began
 * again in between reads as the newer, shorter one. A signal handler on the holder may take
 * the mutex again in the middle of an update (virtual_handle_fault does, inside SIGSEGV): its lock and unlock are
 * balanced, so at worst it stamps a short hold of its own over the outer one's start and adds an acquisition; it never
 * leaves an owner recorded after the last release. Once process_exiting is set Wine's helpers stop calling here, so a
 * hold the exit cut short stays recorded.
 * Cost: one trylock, two TSC reads, a thread id and a few relaxed stores per first acquisition, about 13,000 a second
 * at that run's rates (an estimate, not measured on the console). */
typedef struct PwWinePerfVlock {
    uint64_t owner;              /* kernel thread id of the holder, 0 when free */
    uint64_t held_us;            /* since the holder took it */
    uint64_t site;
    const char *doing;           /* the holder's tag (pw_wine_perf_vlock_doing), NULL for none */
    uint64_t doing_us;           /* since that tag */
    uint64_t waiters;            /* threads blocked on it now */
    uint64_t acquires;           /* first acquisitions, since the start */
    uint64_t contended;          /* of those, ones that waited */
    uint64_t changes;            /* of those, ones by another thread than the last */
    uint64_t longest_us, longest_owner, longest_site;
    const char *longest_doing;   /* the tag the longest hold ended with */
} PwWinePerfVlock;
extern pthread_mutex_t *pw_wine_perf_vlock_mutex;   /* the watched mutex, NULL until virtual_init */
/* PwWinePerfVlock.waiters itself (atomic): the 4 KiB MMU's runs read it (PwWineMmu4kOps.waiters, patch 1136) and hand
 * the mutex over when it is not 0; a count left stale by an ended waiter only makes them hand over more often. */
extern uint64_t pw_wine_perf_vlock_waiters;
void pw_wine_perf_vlock_lock(pthread_mutex_t *mutex, const void *site);
void pw_wine_perf_vlock_unlock(pthread_mutex_t *mutex);
/* The holder says what it is doing while it holds the mutex (a static string: a kernel call, a wait for the title),
 * and gets the previous tag back to restore after. From any other thread: nothing written, NULL. A tag stays until
 * the next tag or the last release. */
const char *pw_wine_perf_vlock_doing(const char *tag);
void pw_wine_perf_vlock_read(PwWinePerfVlock *out);

#endif
